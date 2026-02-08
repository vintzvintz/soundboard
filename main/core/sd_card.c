/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project) Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: MIT
 */


#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"   // IWYU pragma: keep
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

#include "sd_card.h"
#include "soundboard.h"

#define SD_CARD_STABILISATION_DELAY 250
#define SD_CARD_MAX_FREQ_KHZ 10000

static const char *TAG = "sd_card";
static const char *s_mount_point = NULL;
static sdmmc_card_t *s_card = NULL;

esp_err_t sd_card_init(const sd_card_spi_config_t *config, sdmmc_card_t **out_card)
{
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    if (config == NULL) {
        ESP_LOGE(TAG, "config is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (config->mount_point == NULL) {
        ESP_LOGE(TAG, "mount_point is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (out_card == NULL) {
        ESP_LOGE(TAG, "out_card is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;

    // Options for mounting the filesystem
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, .max_files = 10, .allocation_unit_size = (size_t)(16 * 1024)};

    ESP_LOGI(TAG, "Initializing SD card via SPI");
    ESP_LOGI(TAG, "Using SPI pins - MOSI:%d MISO:%d CLK:%d CS:%d", config->mosi_io_num, config->miso_io_num,
        config->sclk_io_num, config->cs_io_num);


    // Wait for SD card to power up and stabilize
    // SD cards require 1ms minimum, but some need up to 74 clock cycles at 400kHz
    ESP_LOGI(TAG, "Waiting for SD card to stabilize (250ms)");
    vTaskDelay(pdMS_TO_TICKS(SD_CARD_STABILISATION_DELAY));

    // Initialize SPI bus
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = SD_CARD_MAX_FREQ_KHZ;
    ESP_LOGI(TAG, "SPI frequency set to %d kHz", host.max_freq_khz);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = config->mosi_io_num,
        .miso_io_num = config->miso_io_num,
        .sclk_io_num = config->sclk_io_num,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 0,  // default buffer size with DMA
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };

    ESP_LOGD(TAG, "Initializing SPI bus on slot %d with DMA channel %d", host.slot, SDSPI_DEFAULT_DMA);
    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s (0x%x)", esp_err_to_name(ret), ret);
        return ret;
    }
    ESP_LOGD(TAG, "SPI bus initialized successfully");

    // Initialize SD card slot
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = config->cs_io_num;
    slot_config.host_id = host.slot;

    ESP_LOGI(TAG, "Mounting filesystem at %s", config->mount_point);
    ret = esp_vfs_fat_sdspi_mount(config->mount_point, &host, &slot_config, &mount_config, out_card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                          "Make sure the SD card is formatted with FAT filesystem.");
        } else if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "SD card communication timeout (0x%x).", ret);
        } else if (ret == ESP_ERR_INVALID_RESPONSE) {
            ESP_LOGE(TAG, "SD card invalid response (0x%x).", ret);
        } else {
            ESP_LOGE(TAG, "Failed to initialize SD card: %s (0x%x).", esp_err_to_name(ret), ret);
        }
        ESP_LOGI(TAG, "Cleaning up SPI bus after failure");
        spi_bus_free(host.slot);
        return ret;
    }

    ESP_LOGI(TAG, "Filesystem mounted successfully");
    s_mount_point = config->mount_point;
    s_card = *out_card;

    // Print card info
    sdmmc_card_print_info(stdout, *out_card);

    return ESP_OK;
}

esp_err_t sd_card_deinit(sdmmc_card_t *card)
{
    if (card == NULL) {
        ESP_LOGE(TAG, "card is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Unmount filesystem
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(s_mount_point, card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount SD card: %s", esp_err_to_name(ret));
        return ret;
    }

    // Free SPI bus
    spi_bus_free(SDSPI_DEFAULT_HOST);

    ESP_LOGI(TAG, "SD card unmounted");

    return ESP_OK;
}

/* ============================================================================
 * SD Card Erase
 * ============================================================================ */

#define ERASE_PATH_MAX 512

static int erase_directory_contents(const char *path)
{
    if (strncmp(path, SPIFFS_MOUNT_POINT, strlen(SPIFFS_MOUNT_POINT)) == 0) {
        ESP_LOGE(TAG, "Refusing to erase SPIFFS path: %s", path);
        return -1;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }

    int deleted_count = 0;
    struct dirent *entry;
    struct stat entry_stat;

    char *full_path = malloc(ERASE_PATH_MAX);
    if (!full_path) {
        closedir(dir);
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        int written = snprintf(full_path, ERASE_PATH_MAX, "%s/%s", path, entry->d_name);
        if (written >= ERASE_PATH_MAX) {
            ESP_LOGW(TAG, "Path too long, skipping: %s/%.20s...", path, entry->d_name);
            continue;
        }

        if (stat(full_path, &entry_stat) == 0) {
            if (S_ISDIR(entry_stat.st_mode)) {
                int sub_count = erase_directory_contents(full_path);
                if (sub_count >= 0) {
                    deleted_count += sub_count;
                }
                if (rmdir(full_path) == 0) {
                    ESP_LOGI(TAG, "Removed dir:  %s", full_path);
                    deleted_count++;
                } else {
                    ESP_LOGW(TAG, "Failed to remove dir: %s", full_path);
                }
            } else {
                if (unlink(full_path) == 0) {
                    ESP_LOGI(TAG, "Deleted file: %s", full_path);
                    deleted_count++;
                } else {
                    ESP_LOGW(TAG, "Failed to delete: %s", full_path);
                }
            }
        }
    }

    free(full_path);
    closedir(dir);
    return deleted_count;
}

esp_err_t sd_card_erase_all(const char *mount_point)
{
    if (mount_point == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strncmp(mount_point, SPIFFS_MOUNT_POINT, strlen(SPIFFS_MOUNT_POINT)) == 0) {
        ESP_LOGE(TAG, "Refusing to erase SPIFFS path: %s", mount_point);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Erasing all files on %s...", mount_point);

    int deleted = erase_directory_contents(mount_point);
    if (deleted < 0) {
        ESP_LOGE(TAG, "Failed to open directory: %s", mount_point);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Erase complete. %d items deleted.", deleted);
    return ESP_OK;
}

/* ============================================================================
 * SD Card Status
 * ============================================================================ */

void sd_card_print_status(status_output_type_t output_type)
{
    bool mounted = (s_card != NULL && s_mount_point != NULL);

    // Get capacity and free space if mounted
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    int free_percent = 0;

    if (mounted) {
        // Calculate total capacity from card info
        total_bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;

        // Get free space using FATFS
        FATFS *fs;
        DWORD free_clusters;
        // Drive number 0 for SD card (mounted as "0:")
        if (f_getfree("0:", &free_clusters, &fs) == FR_OK) {
            free_bytes = (uint64_t)free_clusters * fs->csize * s_card->csd.sector_size;
            if (total_bytes > 0) {
                free_percent = (int)((free_bytes * 100) / total_bytes);
            }
        }
    }

    if (output_type == STATUS_OUTPUT_COMPACT) {
        if (mounted) {
            printf("[sdcard] mounted at %s, %.1fGB free / %.1fGB\n",
                   s_mount_point,
                   (double)free_bytes / (1024 * 1024 * 1024),
                   (double)total_bytes / (1024 * 1024 * 1024));
        } else {
            printf("[sdcard] not mounted\n");
        }
    } else {
        printf("SD Card Status:\n");
        if (mounted) {
            printf("  State: Mounted\n");
            printf("  Mount point: %s\n", s_mount_point);
            printf("  Capacity: %.1f GB\n", (double)total_bytes / (1024 * 1024 * 1024));
            printf("  Free space: %.1f GB (%d%%)\n",
                   (double)free_bytes / (1024 * 1024 * 1024), free_percent);
            printf("  Filesystem: FAT\n");

            if (output_type == STATUS_OUTPUT_VERBOSE) {
                printf("  Card name: %s\n", s_card->cid.name);
                printf("  Sector size: %d bytes\n", s_card->csd.sector_size);
                printf("  SPI frequency: %d kHz\n", SD_CARD_MAX_FREQ_KHZ);
                printf("  SPI pins: MOSI=%d, MISO=%d, CLK=%d, CS=%d\n",
                       CONFIG_SOUNDBOARD_SD_MOSI_GPIO,
                       CONFIG_SOUNDBOARD_SD_MISO_GPIO,
                       CONFIG_SOUNDBOARD_SD_CLK_GPIO,
                       CONFIG_SOUNDBOARD_SD_CS_GPIO);
            }
        } else {
            printf("  State: Not mounted\n");
        }
    }
}
