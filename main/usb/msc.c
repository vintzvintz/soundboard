/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file msc.c
 * @brief Interactive MSC update module with FSM-driven task
 */

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"  // IWYU pragma: keep
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include <sys/stat.h>
#include <dirent.h>

#include "esp_heap_caps.h"
#include "soundboard.h"
#include "sd_card.h"
#include "mapper.h"
#include "msc.h"

static const char *TAG = "msc";

// Mount points and paths
#define MSC_SOUNDBOARD_DIR   MSC_MOUNT_POINT "/" CONFIG_SOUNDBOARD_MSC_ROOT_DIR
#define MAPPINGS_FILENAME    CONFIG_SOUNDBOARD_MAPPINGS_FILENAME
#define MSC_MAPPINGS_PATH    MSC_SOUNDBOARD_DIR "/" MAPPINGS_FILENAME
#define MSC_MAX_PATH_LEN     256

// Minimum time between progress updates (milliseconds)
#define PROGRESS_UPDATE_MIN_INTERVAL_MS 100

// Internal event queue depth
#define MSC_EVENT_QUEUE_DEPTH 8

/* ============================================================================
 * FSM State Enum
 * ============================================================================ */

typedef enum {
    MSC_STATE_WAIT_MSC,
    MSC_STATE_INIT,
    MSC_STATE_MENU_UPDATE_FULL,
    MSC_STATE_MENU_UPDATE_INCREMENTAL,
    MSC_STATE_MENU_SD_CLEAR,
    MSC_STATE_MENU_SD_CLEAR_CONFIRM,
    MSC_STATE_UPDATING_FULL,
    MSC_STATE_UPDATING_INCREMENTAL,
    MSC_STATE_UPDATING_SD_CLEAR,
    MSC_STATE_UPDATE_DONE,
    MSC_STATE_UPDATE_FAILED,
} msc_fsm_state_t;

/* ============================================================================
 * Internal Event Types (posted to FSM queue)
 * ============================================================================ */

typedef enum {
    MSC_INTERNAL_USB_CONNECTED,
    MSC_INTERNAL_USB_DISCONNECTED,
    MSC_INTERNAL_INPUT_EVENT,
} msc_internal_event_type_t;

typedef struct {
    msc_internal_event_type_t type;
    union {
        uint8_t device_address;
        struct {
            uint8_t btn_num;
            input_event_type_t event;
        } input;
    };
} msc_internal_event_t;

/* ============================================================================
 * MSC State Structure
 * ============================================================================ */

struct msc_s {
    // USB/MSC device state
    msc_host_device_handle_t device;
    msc_host_vfs_handle_t vfs_handle;
    uint8_t device_address;

    // Sync progress tracking
    size_t total_bytes;
    size_t done_bytes;
    int total_files;
    int done_files;
    char current_filename[64];
    TickType_t last_progress_update;

    // FSM
    msc_fsm_state_t state;
    QueueHandle_t event_queue;
    TaskHandle_t fsm_task;
    TaskHandle_t usb_lib_task;

    // Notification
    TaskHandle_t main_task;
    msc_event_cb_t event_cb;
    void *event_cb_ctx;
};

/* ============================================================================
 * Event Notification Helpers
 * ============================================================================ */

static void notify_event_simple(msc_handle_t h, msc_event_type_t type)
{
    if (h == NULL || h->event_cb == NULL) {
        return;
    }
    msc_event_data_t data = { .type = type };
    h->event_cb(&data, h->event_cb_ctx);
}

static void notify_progress(msc_handle_t h)
{
    if (h == NULL || h->event_cb == NULL) {
        return;
    }
    uint16_t progress = 0;
    if (h->total_bytes > 0) {
        progress = (uint16_t)((uint64_t)h->done_bytes * UINT16_MAX / h->total_bytes);
    }
    msc_event_data_t data = {
        .type = MSC_EVENT_UPDATING,
        .progress = {
            .filename = h->current_filename,
            .progress = progress,
        },
    };
    h->event_cb(&data, h->event_cb_ctx);
}

static void notify_error(msc_handle_t h, const char *message)
{
    if (h == NULL || h->event_cb == NULL) {
        return;
    }
    msc_event_data_t data = {
        .type = MSC_EVENT_UPDATE_FAILED,
        .error = { .message = message },
    };
    h->event_cb(&data, h->event_cb_ctx);
}

/* ============================================================================
 * MSC Mount/Unmount Functions
 * ============================================================================ */

static esp_err_t mount_device(uint8_t address, msc_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = msc_host_install_device(address, &handle->device);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install MSC device: %s", esp_err_to_name(ret));
        return ret;
    }

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 4096,
    };

    ret = msc_host_vfs_register(handle->device, MSC_MOUNT_POINT,
                                &mount_config, &handle->vfs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount MSC device at %s: %s", MSC_MOUNT_POINT, esp_err_to_name(ret));
        msc_host_uninstall_device(handle->device);
        handle->device = NULL;
        return ret;
    }

    handle->device_address = address;
    ESP_LOGI(TAG, "MSC device mounted at %s", MSC_MOUNT_POINT);
    return ESP_OK;
}

static esp_err_t unmount_vfs(msc_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    if (handle->vfs_handle != NULL) {
        ret = msc_host_vfs_unregister(handle->vfs_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to unregister VFS: %s", esp_err_to_name(ret));
        }
        handle->vfs_handle = NULL;
    }

    ESP_LOGI(TAG, "MSC VFS unmounted (device still installed)");
    return ret;
}

static esp_err_t uninstall_device(msc_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    if (handle->device != NULL) {
        ret = msc_host_uninstall_device(handle->device);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to uninstall MSC device: %s", esp_err_to_name(ret));
        }
        handle->device = NULL;
    }

    handle->device_address = 0;
    ESP_LOGI(TAG, "MSC device uninstalled");
    return ret;
}

/* ============================================================================
 * File Copy Functions (unchanged from previous implementation)
 * ============================================================================ */

static void update_copy_progress(msc_handle_t handle, size_t bytes_copied)
{
    handle->done_bytes += bytes_copied;

    TickType_t now = xTaskGetTickCount();
    TickType_t min_interval = pdMS_TO_TICKS(PROGRESS_UPDATE_MIN_INTERVAL_MS);

    if ((now - handle->last_progress_update) >= min_interval) {
        handle->last_progress_update = now;
        notify_progress(handle);
    }
}

static esp_err_t copy_file(const char *src, const char *dst, msc_handle_t handle)
{
    FILE *src_file = fopen(src, "rb");
    if (src_file == NULL) {
        ESP_LOGE(TAG, "Failed to open source file: %s", src);
        return ESP_ERR_NOT_FOUND;
    }

    FILE *dst_file = fopen(dst, "wb");
    if (dst_file == NULL) {
        ESP_LOGE(TAG, "Failed to create destination file: %s", dst);
        fclose(src_file);
        return ESP_FAIL;
    }

    // 8192 = 16 SD sectors, fits ~2 GDMA descriptors (4092 bytes each)
    // 4-byte alignment ensures DMA descriptor compatibility
    static const size_t copy_buf_size = 8192;
    char *buffer = heap_caps_aligned_alloc(4, copy_buf_size, MALLOC_CAP_DMA);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate copy buffer");
        fclose(src_file);
        fclose(dst_file);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    size_t bytes_read;
    size_t file_bytes = 0;

    while ((bytes_read = fread(buffer, 1, copy_buf_size, src_file)) > 0) {
        size_t bytes_written = fwrite(buffer, 1, bytes_read, dst_file);
        if (bytes_written != bytes_read) {
            ESP_LOGE(TAG, "Write error copying %s to %s", src, dst);
            ret = ESP_FAIL;
            break;
        }
        file_bytes += bytes_written;
        update_copy_progress(handle, bytes_written);
    }

    free(buffer);
    fclose(src_file);
    fclose(dst_file);

    if (ret == ESP_OK) {
        handle->done_files++;
        ESP_LOGI(TAG, "Copied %s -> %s (%zu bytes)", src, dst, file_bytes);
    }

    return ret;
}

static bool is_wav_file(const char *name)
{
    size_t name_len = strlen(name);
    return (name_len > 4 &&
            (strcasecmp(name + name_len - 4, ".wav") == 0));
}

static bool is_sync_file(const char *name)
{
    return is_wav_file(name) || (strcasecmp(name, MAPPINGS_FILENAME) == 0);
}

static esp_err_t scan_files_for_sync(const char *msc_root, msc_handle_t handle)
{
    DIR *dir = opendir(msc_root);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory: %s", msc_root);
        return ESP_ERR_NOT_FOUND;
    }

    handle->total_files = 0;
    handle->total_bytes = 0;
    handle->done_files = 0;
    handle->done_bytes = 0;

    struct dirent *entry;
    struct stat st;
    char path[MSC_MAX_PATH_LEN + 64];

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR || !is_sync_file(entry->d_name)) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", msc_root, entry->d_name);
        if (stat(path, &st) == 0) {
            handle->total_files++;
            handle->total_bytes += st.st_size;
            ESP_LOGD(TAG, "Found: %s (%ld bytes)", entry->d_name, (long)st.st_size);
        }
    }

    closedir(dir);
    ESP_LOGI(TAG, "Scan complete: %d files, %zu bytes total",
             handle->total_files, handle->total_bytes);
    return ESP_OK;
}

static esp_err_t copy_files_to_sdcard(const char *msc_root, const char *sdcard_root,
                                       msc_handle_t handle)
{
    if (msc_root == NULL || sdcard_root == NULL || handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = scan_files_for_sync(msc_root, handle);
    if (ret != ESP_OK) {
        return ret;
    }

    if (handle->total_files == 0) {
        ESP_LOGW(TAG, "No files to copy");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Copying %d files from %s to %s...",
             handle->total_files, msc_root, sdcard_root);

    DIR *dir = opendir(msc_root);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory: %s", msc_root);
        return ESP_ERR_NOT_FOUND;
    }

    ret = ESP_OK;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR || !is_sync_file(entry->d_name)) {
            continue;
        }

        strncpy(handle->current_filename, entry->d_name,
                sizeof(handle->current_filename) - 1);
        handle->current_filename[sizeof(handle->current_filename) - 1] = '\0';

        char src_path[MSC_MAX_PATH_LEN + 64];
        char dst_path[MSC_MAX_PATH_LEN + 64];
        snprintf(src_path, sizeof(src_path), "%s/%s", msc_root, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", sdcard_root, entry->d_name);

        esp_err_t copy_ret = copy_file(src_path, dst_path, handle);
        if (copy_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to copy %s: %s", entry->d_name, esp_err_to_name(copy_ret));
            ret = copy_ret;
        }
    }

    closedir(dir);
    ESP_LOGI(TAG, "File copy complete: %d/%d files copied",
             handle->done_files, handle->total_files);
    return ret;
}

/* ============================================================================
 * Full Update Workflow
 * ============================================================================ */

static esp_err_t run_full_update(msc_handle_t handle)
{
    ESP_LOGI(TAG, "Running full update...");

    // Check SD card is mounted
    struct stat st;
    if (stat(SDCARD_MOUNT_POINT, &st) != 0) {
        ESP_LOGE(TAG, "SD card not mounted at %s", SDCARD_MOUNT_POINT);
        return ESP_ERR_INVALID_STATE;
    }

    // Copy files
    esp_err_t ret = copy_files_to_sdcard(MSC_SOUNDBOARD_DIR, SDCARD_MOUNT_POINT, handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Full update failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Full update complete");
    return ESP_OK;
}

/* ============================================================================
 * Incremental Update Workflow
 * ============================================================================ */

/**
 * @brief Check if a WAV file on SD card matches the USB source by size
 *
 * @return true if file exists on SD card with the same size as msc_size
 */
static bool sdcard_file_matches(const char *filename, off_t msc_size)
{
    char path[MSC_MAX_PATH_LEN + 64];
    snprintf(path, sizeof(path), "%s/%s", SDCARD_MOUNT_POINT, filename);
    struct stat sd_st;
    return (stat(path, &sd_st) == 0 && sd_st.st_size == msc_size);
}

/**
 * @brief Scan USB files for incremental sync.
 *
 * Counts total bytes to copy: always includes mappings.csv,
 * skips WAV files that already exist on SD card with the same size.
 */
static esp_err_t scan_files_for_incremental(const char *msc_root, msc_handle_t handle)
{
    DIR *dir = opendir(msc_root);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory: %s", msc_root);
        return ESP_ERR_NOT_FOUND;
    }

    handle->total_files = 0;
    handle->total_bytes = 0;
    handle->done_files = 0;
    handle->done_bytes = 0;

    struct dirent *entry;
    struct stat st;
    char path[MSC_MAX_PATH_LEN + 64];
    int skipped = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR || !is_sync_file(entry->d_name)) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", msc_root, entry->d_name);
        if (stat(path, &st) != 0) {
            continue;
        }

        // Skip WAV files that exist on SD card with matching size
        if (is_wav_file(entry->d_name) && sdcard_file_matches(entry->d_name, st.st_size)) {
            ESP_LOGI(TAG, "Skip (same size): %s (%ld bytes)", entry->d_name, (long)st.st_size);
            skipped++;
            continue;
        }

        handle->total_files++;
        handle->total_bytes += st.st_size;
        ESP_LOGD(TAG, "Will copy: %s (%ld bytes)", entry->d_name, (long)st.st_size);
    }

    closedir(dir);
    ESP_LOGI(TAG, "Incremental scan: %d files to copy, %d skipped, %zu bytes total",
             handle->total_files, skipped, handle->total_bytes);
    return ESP_OK;
}

/**
 * @brief Copy files from USB to SD card incrementally.
 *
 * Always overwrites mappings.csv. Only copies WAV files that are new
 * or have a different size than the SD card copy.
 */
static esp_err_t copy_files_incremental(const char *msc_root, const char *sdcard_root,
                                         msc_handle_t handle)
{
    if (msc_root == NULL || sdcard_root == NULL || handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = scan_files_for_incremental(msc_root, handle);
    if (ret != ESP_OK) {
        return ret;
    }

    if (handle->total_files == 0) {
        ESP_LOGI(TAG, "No new files to copy");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Incremental copy: %d files from %s to %s...",
             handle->total_files, msc_root, sdcard_root);

    DIR *dir = opendir(msc_root);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory: %s", msc_root);
        return ESP_ERR_NOT_FOUND;
    }

    ret = ESP_OK;
    struct dirent *entry;
    struct stat st;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_DIR || !is_sync_file(entry->d_name)) {
            continue;
        }

        char src_path[MSC_MAX_PATH_LEN + 64];
        snprintf(src_path, sizeof(src_path), "%s/%s", msc_root, entry->d_name);
        if (stat(src_path, &st) != 0) {
            continue;
        }

        // Skip WAV files that exist on SD card with matching size
        if (is_wav_file(entry->d_name) && sdcard_file_matches(entry->d_name, st.st_size)) {
            continue;
        }

        strncpy(handle->current_filename, entry->d_name,
                sizeof(handle->current_filename) - 1);
        handle->current_filename[sizeof(handle->current_filename) - 1] = '\0';

        char dst_path[MSC_MAX_PATH_LEN + 64];
        snprintf(dst_path, sizeof(dst_path), "%s/%s", sdcard_root, entry->d_name);

        esp_err_t copy_ret = copy_file(src_path, dst_path, handle);
        if (copy_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to copy %s: %s", entry->d_name, esp_err_to_name(copy_ret));
            ret = copy_ret;
        }
    }

    closedir(dir);
    ESP_LOGI(TAG, "Incremental copy complete: %d/%d files copied",
             handle->done_files, handle->total_files);
    return ret;
}

static esp_err_t run_incremental_update(msc_handle_t handle)
{
    ESP_LOGI(TAG, "Running incremental update...");

    // Check SD card is mounted
    struct stat st;
    if (stat(SDCARD_MOUNT_POINT, &st) != 0) {
        ESP_LOGE(TAG, "SD card not mounted at %s", SDCARD_MOUNT_POINT);
        return ESP_ERR_INVALID_STATE;
    }

    // Validate mappings.csv on USB (syntax + referenced audio files)
    esp_err_t ret = mapper_validate_file(MSC_MAPPINGS_PATH, MSC_SOUNDBOARD_DIR, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Incremental update: mappings validation failed");
        return ret;
    }

    // Copy: always overwrite mappings.csv, skip existing WAV files
    ret = copy_files_incremental(MSC_SOUNDBOARD_DIR, SDCARD_MOUNT_POINT, handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Incremental update failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Incremental update complete");
    return ESP_OK;
}

/* ============================================================================
 * Queue Drain Helper
 * ============================================================================ */

static void drain_queue(msc_handle_t handle)
{
    msc_internal_event_t evt;
    while (xQueueReceive(handle->event_queue, &evt, 0) == pdTRUE) {
        // If we get a disconnect while draining, forward it immediately
        if (evt.type == MSC_INTERNAL_USB_DISCONNECTED) {
            xTaskNotify(handle->main_task, MSC_NOTIFY_DISCONNECTED, eSetBits);
        }
    }
}

/* ============================================================================
 * USB Content Validation (stub)
 * ============================================================================ */

static esp_err_t validate_usb_content(void)
{
    // Check /msc/soundboard directory exists
    struct stat dir_st;
    if (stat(MSC_SOUNDBOARD_DIR, &dir_st) != 0 || !S_ISDIR(dir_st.st_mode)) {
        ESP_LOGE(TAG, "Directory not found: %s", MSC_SOUNDBOARD_DIR);
        return ESP_ERR_NOT_FOUND;
    }

    // Validate mappings CSV syntax and check referenced audio files exist on USB
    esp_err_t ret = mapper_validate_file(MSC_MAPPINGS_PATH, MSC_SOUNDBOARD_DIR, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mappings validation failed");
        return ret;
    }

    ESP_LOGI(TAG, "USB content validation passed");
    return ESP_OK;
}

/* ============================================================================
 * FSM State Handlers
 * ============================================================================ */

static void fsm_handle_init(msc_handle_t h, uint8_t device_address)
{
    ESP_LOGI(TAG, "FSM: INIT (device_address=%d)", device_address);

    esp_err_t ret = mount_device(device_address, h);
    if (ret != ESP_OK) {
        notify_error(h, "Mount failed");
        h->state = MSC_STATE_UPDATE_FAILED;
        return;
    }

    ret = validate_usb_content();
    if (ret != ESP_OK) {
        notify_error(h, "Invalid USB content");
        unmount_vfs(h);
        h->state = MSC_STATE_UPDATE_FAILED;
        return;
    }

    notify_event_simple(h, MSC_EVENT_READY);

    // Enter first menu item
    h->state = MSC_STATE_MENU_UPDATE_FULL;
    notify_event_simple(h, MSC_EVENT_MENU_FULL_SELECTED);
}

static void fsm_handle_menu_full(msc_handle_t h, const msc_internal_event_t *evt)
{
    if (evt->type != MSC_INTERNAL_INPUT_EVENT) {
        return;
    }

    if (evt->input.event == INPUT_EVENT_ENCODER_ROTATE_CW) {
        h->state = MSC_STATE_MENU_UPDATE_INCREMENTAL;
        notify_event_simple(h, MSC_EVENT_MENU_INCREMENTAL_SELECTED);
    } else if (evt->input.event == INPUT_EVENT_ENCODER_ROTATE_CCW) {
        h->state = MSC_STATE_MENU_SD_CLEAR;
        notify_event_simple(h, MSC_EVENT_MENU_SD_CLEAR_SELECTED);
    } else if (evt->input.event == INPUT_EVENT_BUTTON_PRESS && evt->input.btn_num == 0) {
        // Encoder switch press → launch full update
        h->state = MSC_STATE_UPDATING_FULL;
        strncpy(h->current_filename, "Scanning...", sizeof(h->current_filename) - 1);
        notify_progress(h);

        esp_err_t ret = run_full_update(h);
        drain_queue(h);

        if (ret == ESP_OK) {
            h->state = MSC_STATE_UPDATE_DONE;
            unmount_vfs(h);
            notify_event_simple(h, MSC_EVENT_UPDATE_DONE);
        } else {
            h->state = MSC_STATE_UPDATE_FAILED;
            unmount_vfs(h);
            notify_error(h, "Full update failed");
        }
    }
}

static void fsm_handle_menu_incremental(msc_handle_t h, const msc_internal_event_t *evt)
{
    if (evt->type != MSC_INTERNAL_INPUT_EVENT) {
        return;
    }

    if (evt->input.event == INPUT_EVENT_ENCODER_ROTATE_CW) {
        h->state = MSC_STATE_MENU_SD_CLEAR;
        notify_event_simple(h, MSC_EVENT_MENU_SD_CLEAR_SELECTED);
    } else if (evt->input.event == INPUT_EVENT_ENCODER_ROTATE_CCW) {
        h->state = MSC_STATE_MENU_UPDATE_FULL;
        notify_event_simple(h, MSC_EVENT_MENU_FULL_SELECTED);
    } else if (evt->input.event == INPUT_EVENT_BUTTON_PRESS && evt->input.btn_num == 0) {
        // Encoder switch press → launch incremental update
        h->state = MSC_STATE_UPDATING_INCREMENTAL;
        strncpy(h->current_filename, "Scanning...", sizeof(h->current_filename) - 1);
        notify_progress(h);

        esp_err_t ret = run_incremental_update(h);
        drain_queue(h);

        if (ret == ESP_OK) {
            h->state = MSC_STATE_UPDATE_DONE;
            unmount_vfs(h);
            notify_event_simple(h, MSC_EVENT_UPDATE_DONE);
        } else {
            h->state = MSC_STATE_UPDATE_FAILED;
            unmount_vfs(h);
            notify_error(h, "Incremental update failed");
        }
    }
}

static void fsm_handle_menu_sd_clear(msc_handle_t h, const msc_internal_event_t *evt)
{
    if (evt->type != MSC_INTERNAL_INPUT_EVENT) {
        return;
    }

    if (evt->input.event == INPUT_EVENT_ENCODER_ROTATE_CW) {
        h->state = MSC_STATE_MENU_UPDATE_FULL;
        notify_event_simple(h, MSC_EVENT_MENU_FULL_SELECTED);
    } else if (evt->input.event == INPUT_EVENT_ENCODER_ROTATE_CCW) {
        h->state = MSC_STATE_MENU_UPDATE_INCREMENTAL;
        notify_event_simple(h, MSC_EVENT_MENU_INCREMENTAL_SELECTED);
    } else if (evt->input.event == INPUT_EVENT_BUTTON_PRESS && evt->input.btn_num == 0) {
        // Encoder switch press → ask for confirmation
        h->state = MSC_STATE_MENU_SD_CLEAR_CONFIRM;
        notify_event_simple(h, MSC_EVENT_MENU_SD_CLEAR_CONFIRM);
    }
}

static void fsm_handle_sd_clear_confirm(msc_handle_t h, const msc_internal_event_t *evt)
{
    if (evt->type != MSC_INTERNAL_INPUT_EVENT) {
        return;
    }

    // ignore long press and release events, just stay in confirmation state
    if (evt->input.event != INPUT_EVENT_BUTTON_PRESS) {
        return;
    }

    // "Red buttons" are buttons 7/8/9 (3rd row)
    if( evt->input.btn_num >= 7 && evt->input.btn_num <= 9) {
        h->state = MSC_STATE_UPDATING_SD_CLEAR;
        strncpy(h->current_filename, "Erasing...", sizeof(h->current_filename) - 1);
        notify_progress(h);

        esp_err_t ret = sd_card_erase_all(SDCARD_MOUNT_POINT);
        drain_queue(h);

        if (ret == ESP_OK) {
            h->state = MSC_STATE_UPDATE_DONE;
            notify_event_simple(h, MSC_EVENT_UPDATE_DONE);
        } else {
            h->state = MSC_STATE_UPDATE_FAILED;
            notify_error(h, "SD erase failed");
        }
    } else {
        // Any other input → back to SD clear menu
        h->state = MSC_STATE_MENU_SD_CLEAR;
        notify_event_simple(h, MSC_EVENT_MENU_SD_CLEAR_SELECTED);
    }
}

/* ============================================================================
 * FSM Task
 * ============================================================================ */

static void msc_fsm_task(void *arg)
{
    msc_handle_t h = (msc_handle_t)arg;
    h->state = MSC_STATE_WAIT_MSC;
    msc_internal_event_t evt;

    ESP_LOGI(TAG, "FSM task started, waiting for MSC device...");

    while (1) {
        if (xQueueReceive(h->event_queue, &evt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // Handle USB disconnect in any state
        if (evt.type == MSC_INTERNAL_USB_DISCONNECTED) {
            ESP_LOGW(TAG, "FSM: USB disconnected in state %d", h->state);
            xTaskNotify(h->main_task, MSC_NOTIFY_DISCONNECTED, eSetBits);
            continue;
        }

        switch (h->state) {
            case MSC_STATE_WAIT_MSC:
                if (evt.type == MSC_INTERNAL_USB_CONNECTED) {
                    xTaskNotify(h->main_task, MSC_NOTIFY_CONNECTED, eSetBits);
                    fsm_handle_init(h, evt.device_address);
                }
                break;

            case MSC_STATE_MENU_UPDATE_FULL:
                fsm_handle_menu_full(h, &evt);
                break;

            case MSC_STATE_MENU_UPDATE_INCREMENTAL:
                fsm_handle_menu_incremental(h, &evt);
                break;

            case MSC_STATE_MENU_SD_CLEAR:
                fsm_handle_menu_sd_clear(h, &evt);
                break;

            case MSC_STATE_MENU_SD_CLEAR_CONFIRM:
                fsm_handle_sd_clear_confirm(h, &evt);
                break;

            case MSC_STATE_UPDATE_DONE:
            case MSC_STATE_UPDATE_FAILED:
                // Terminal states: ignore all events (main handles reboot on disconnect)
                break;

            case MSC_STATE_INIT:
            case MSC_STATE_UPDATING_FULL:
            case MSC_STATE_UPDATING_INCREMENTAL:
            case MSC_STATE_UPDATING_SD_CLEAR:
                // These states run synchronously within handlers above,
                // so we should not receive events in these states.
                ESP_LOGW(TAG, "FSM: unexpected event in state %d", h->state);
                break;
        }
    }
}

/* ============================================================================
 * USB Host Library Task
 * ============================================================================ */

static void usb_lib_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "USB Host Library task started");

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGD(TAG, "USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS");
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGI(TAG, "USB_HOST_LIB_EVENT_FLAGS_ALL_FREE");
        }
    }
}

/* ============================================================================
 * MSC Driver Event Callback (internal, static)
 * ============================================================================ */

static void msc_driver_event_cb(const msc_host_event_t *event, void *arg)
{
    msc_handle_t h = (msc_handle_t)arg;
    if (h == NULL) {
        ESP_LOGE(TAG, "MSC driver callback: NULL handle");
        return;
    }

    msc_internal_event_t evt = {0};

    switch (event->event) {
        case MSC_DEVICE_CONNECTED:
            ESP_LOGI(TAG, "MSC device connected (address=%d)", event->device.address);
            evt.type = MSC_INTERNAL_USB_CONNECTED;
            evt.device_address = event->device.address;
            xQueueSend(h->event_queue, &evt, 0);
            break;

        case MSC_DEVICE_DISCONNECTED:
            ESP_LOGW(TAG, "MSC device disconnected");
            evt.type = MSC_INTERNAL_USB_DISCONNECTED;
            xQueueSend(h->event_queue, &evt, 0);
            break;

        default:
            ESP_LOGW(TAG, "Unknown MSC event: %d", event->event);
            break;
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

esp_err_t msc_init(const msc_config_t *config, msc_handle_t *handle)
{
    if (config == NULL || handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    msc_handle_t h = calloc(1, sizeof(struct msc_s));
    if (h == NULL) {
        ESP_LOGE(TAG, "Failed to allocate MSC handle");
        return ESP_ERR_NO_MEM;
    }

    h->main_task = config->main_task;
    h->event_cb = config->event_cb;
    h->event_cb_ctx = config->event_cb_ctx;

    // Create internal event queue
    h->event_queue = xQueueCreate(MSC_EVENT_QUEUE_DEPTH, sizeof(msc_internal_event_t));
    if (h->event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
        free(h);
        return ESP_ERR_NO_MEM;
    }

    // Install USB host library
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t ret = usb_host_install(&host_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB host: %s", esp_err_to_name(ret));
        vQueueDelete(h->event_queue);
        free(h);
        return ret;
    }
    ESP_LOGI(TAG, "USB Host Library installed");

    // Create USB host lib task (priority 5, core 0)
    BaseType_t task_ret = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096,
                                                   NULL, 5, &h->usb_lib_task, 0);
    if (task_ret != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create USB lib task");
        usb_host_uninstall();
        vQueueDelete(h->event_queue);
        free(h);
        return ESP_ERR_NO_MEM;
    }

    // Install MSC class driver with internal callback
    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,  // Note: typo in esp-usb API
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = msc_driver_event_cb,
        .callback_arg = h,
    };
    ret = msc_host_install(&msc_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install MSC host: %s", esp_err_to_name(ret));
        vTaskDelete(h->usb_lib_task);
        usb_host_uninstall();
        vQueueDelete(h->event_queue);
        free(h);
        return ret;
    }
    ESP_LOGI(TAG, "MSC host driver installed");

    // Create FSM task (priority 2, core 0)
    task_ret = xTaskCreatePinnedToCore(msc_fsm_task, "msc_fsm", 6144,
                                        h, 2, &h->fsm_task, 0);
    if (task_ret != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create FSM task");
        msc_host_uninstall();
        vTaskDelete(h->usb_lib_task);
        usb_host_uninstall();
        vQueueDelete(h->event_queue);
        free(h);
        return ESP_ERR_NO_MEM;
    }

    *handle = h;
    ESP_LOGI(TAG, "MSC module initialized (FSM task running)");
    return ESP_OK;
}

esp_err_t msc_deinit(msc_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (handle->vfs_handle != NULL) {
        unmount_vfs(handle);
    }
    if (handle->device != NULL) {
        uninstall_device(handle);
    }

    if (handle->fsm_task != NULL) {
        vTaskDelete(handle->fsm_task);
    }
    if (handle->usb_lib_task != NULL) {
        vTaskDelete(handle->usb_lib_task);
    }
    if (handle->event_queue != NULL) {
        vQueueDelete(handle->event_queue);
    }

    free(handle);
    ESP_LOGI(TAG, "MSC module deinitialized");
    return ESP_OK;
}

void msc_handle_input_event(msc_handle_t handle, uint8_t btn_num, input_event_type_t event)
{
    if (handle == NULL || handle->event_queue == NULL) {
        return;
    }

    msc_internal_event_t evt = {
        .type = MSC_INTERNAL_INPUT_EVENT,
        .input = {
            .btn_num = btn_num,
            .event = event,
        },
    };
    xQueueSend(handle->event_queue, &evt, 0);  // Non-blocking, drop if full
}

void msc_print_status(msc_handle_t handle, status_output_type_t output_type)
{
    if (handle == NULL) {
        if (output_type == STATUS_OUTPUT_COMPACT) {
            printf("[msc] not initialized\n");
        } else {
            printf("MSC Status:\n");
            printf("  State: Not initialized\n");
        }
        return;
    }

    // Get state name
    const char *state_name = "unknown";
    switch (handle->state) {
        case MSC_STATE_WAIT_MSC: state_name = "WAIT_MSC"; break;
        case MSC_STATE_INIT: state_name = "INIT"; break;
        case MSC_STATE_MENU_UPDATE_FULL: state_name = "MENU_UPDATE_FULL"; break;
        case MSC_STATE_MENU_UPDATE_INCREMENTAL: state_name = "MENU_UPDATE_INCREMENTAL"; break;
        case MSC_STATE_MENU_SD_CLEAR: state_name = "MENU_SD_CLEAR"; break;
        case MSC_STATE_MENU_SD_CLEAR_CONFIRM: state_name = "MENU_SD_CLEAR_CONFIRM"; break;
        case MSC_STATE_UPDATING_FULL: state_name = "UPDATING_FULL"; break;
        case MSC_STATE_UPDATING_INCREMENTAL: state_name = "UPDATING_INCREMENTAL"; break;
        case MSC_STATE_UPDATING_SD_CLEAR: state_name = "UPDATING_SD_CLEAR"; break;
        case MSC_STATE_UPDATE_DONE: state_name = "UPDATE_DONE"; break;
        case MSC_STATE_UPDATE_FAILED: state_name = "UPDATE_FAILED"; break;
    }

    bool device_connected = (handle->device != NULL);

    if (output_type == STATUS_OUTPUT_COMPACT) {
        printf("[msc] state=%s, %s\n",
               state_name,
               device_connected ? "device connected" : "no device");
    } else {
        printf("MSC Status:\n");
        printf("  FSM state: %s\n", state_name);
        printf("  Device: %s\n", device_connected ? "Connected" : "Not connected");
        printf("  USB Host: Running\n");

        if (output_type == STATUS_OUTPUT_VERBOSE) {
            if (device_connected) {
                printf("  Device address: %d\n", handle->device_address);
            }
            if (handle->state >= MSC_STATE_UPDATING_FULL &&
                handle->state <= MSC_STATE_UPDATING_SD_CLEAR) {
                printf("  Progress: %d/%d files\n", handle->done_files, handle->total_files);
                if (handle->current_filename[0] != '\0') {
                    printf("  Current file: %s\n", handle->current_filename);
                }
            }
        }
    }
}
