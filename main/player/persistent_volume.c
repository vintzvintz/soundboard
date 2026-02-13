/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file persistent_volume.c
 * @brief NVS-backed persistent volume storage implementation
 */

#include "persistent_volume.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "persistent_volume";

// NVS configuration
#define NVS_NAMESPACE   "soundboard"
#define NVS_VOLUME_KEY  "volume"

// Module state
static esp_timer_handle_t s_save_timer = NULL;
static uint16_t s_pending_index = 0;
static bool s_initialized = false;

/**
 * @brief Timer callback - performs actual NVS write
 *
 * Called 10 seconds after the last volume change.
 */
static void save_timer_callback(void *arg)
{
    (void)arg;

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(ret));
        return;
    }

    ret = nvs_set_u16(handle, NVS_VOLUME_KEY, s_pending_index);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write volume: %s", esp_err_to_name(ret));
        nvs_close(handle);
        return;
    }

    ret = nvs_commit(handle);
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Saved volume: index=%u", s_pending_index);
}

esp_err_t persistent_volume_init(void)
{
    if (s_initialized) {
        return ESP_OK;  // Already initialized
    }

    // Create one-shot timer for deferred saves
    const esp_timer_create_args_t timer_args = {
        .callback = save_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "vol_save",
    };

    esp_err_t ret = esp_timer_create(&timer_args, &s_save_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create save timer: %s", esp_err_to_name(ret));
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGD(TAG, "Persistent volume module initialized");
    return ESP_OK;
}

esp_err_t persistent_volume_load(uint16_t *index)
{
    if (index == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        // Namespace doesn't exist yet (first boot) - use defaults
        ESP_LOGI(TAG, "No saved volume (first boot), using default index=%d",
                 PERSISTENT_VOLUME_DEFAULT_INDEX);
        *index = PERSISTENT_VOLUME_DEFAULT_INDEX;
        s_pending_index = *index;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    uint16_t saved = 0;
    ret = nvs_get_u16(handle, NVS_VOLUME_KEY, &saved);
    nvs_close(handle);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        // Key doesn't exist - use defaults
        ESP_LOGI(TAG, "No saved volume, using default index=%d",
                 PERSISTENT_VOLUME_DEFAULT_INDEX);
        *index = PERSISTENT_VOLUME_DEFAULT_INDEX;
        s_pending_index = *index;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read volume: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    *index = saved;
    s_pending_index = *index;

    ESP_LOGI(TAG, "Loaded volume: index=%u", *index);
    return ESP_OK;
}

esp_err_t persistent_volume_save_deferred(uint16_t index)
{
    if (!s_initialized || s_save_timer == NULL) {
        ESP_LOGW(TAG, "Module not initialized, cannot save");
        return ESP_ERR_INVALID_STATE;
    }

    // Store pending value
    s_pending_index = index;

    // Stop timer if running (reset for new delay)
    esp_timer_stop(s_save_timer);

    // Start timer for deferred save
    esp_err_t ret = esp_timer_start_once(s_save_timer, (uint64_t)PERSISTENT_VOLUME_SAVE_DELAY_MS * 1000);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start save timer: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Deferred save scheduled: index=%u", index);
    return ESP_OK;
}

void persistent_volume_print_status(status_output_type_t output_type)
{
    bool pending = false;
    if (s_save_timer != NULL) {
        pending = esp_timer_is_active(s_save_timer);
    }

    if (output_type == STATUS_OUTPUT_COMPACT) {
        printf("[volume] level=%u/31, %s\n",
               s_pending_index,
               pending ? "save pending" : "saved");
    } else {
        printf("Persistent Volume Status:\n");
        printf("  Current level: %u / 31\n", s_pending_index);
        printf("  Save status: %s\n", pending ? "Save pending" : "Saved to NVS");
        printf("  Deferred save: %s\n", pending ? "Timer active" : "Not pending");

        if (output_type == STATUS_OUTPUT_VERBOSE) {
            printf("  Module initialized: %s\n", s_initialized ? "Yes" : "No");
            printf("  Save delay: %d ms\n", PERSISTENT_VOLUME_SAVE_DELAY_MS);
            printf("  NVS namespace: %s\n", NVS_NAMESPACE);
        }
    }
}
