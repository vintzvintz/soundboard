/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sdmmc_cmd.h"  // IWYU pragma: keep
#include "sdkconfig.h"
#include "soundboard.h"

/**
 * @brief SD card SPI pin configuration
 */
typedef struct {
    const char *mount_point; /*!< VFS mount point (e.g. "/sdcard") */
    int mosi_io_num;    /*!< GPIO number for MOSI signal */
    int miso_io_num;    /*!< GPIO number for MISO signal */
    int sclk_io_num;    /*!< GPIO number for SCLK signal */
    int cs_io_num;      /*!< GPIO number for CS signal */
} sd_card_spi_config_t;

/**
 * @brief Default SD card SPI configuration macro
 *
 * GPIO pins are sourced from Kconfig. Only mount_point needs to be set
 * by the caller.
 *
 * Usage:
 * @code
 * sd_card_spi_config_t config = SD_CARD_SPI_DEFAULT_CONFIG();
 * config.mount_point = "/sdcard";
 * @endcode
 */
#define SD_CARD_SPI_DEFAULT_CONFIG() { \
    .mount_point = NULL, \
    .mosi_io_num = CONFIG_SOUNDBOARD_SD_MOSI_GPIO, \
    .miso_io_num = CONFIG_SOUNDBOARD_SD_MISO_GPIO, \
    .sclk_io_num = CONFIG_SOUNDBOARD_SD_CLK_GPIO, \
    .cs_io_num = CONFIG_SOUNDBOARD_SD_CS_GPIO, \
}


/**
 * @brief Initialize SD card with SPI interface
 *
 * This function initializes the SPI bus, mounts the FAT filesystem on the SD card,
 * and makes it accessible via VFS at the configured mount point.
 *
 * @param[in] config SPI pin configuration for SD card
 * @param[out] out_card Pointer to store SD card information (must not be NULL)
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_FAIL if mount failed
 *      - ESP_ERR_INVALID_ARG if config or out_card is NULL
 *      - Other error codes from underlying drivers
 */
esp_err_t sd_card_init(const sd_card_spi_config_t *config, sdmmc_card_t **out_card);

/**
 * @brief Unmount SD card and deinitialize SPI bus
 *
 * @param[in] card SD card handle obtained from sd_card_init()
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if card is NULL
 *      - Error codes from underlying drivers
 */
esp_err_t sd_card_deinit(sdmmc_card_t *card);

/**
 * @brief Erase all files and directories on a mounted filesystem
 *
 * Recursively deletes all files and subdirectories under the given mount point.
 * Refuses to erase SPIFFS paths as a safety measure.
 *
 * @param mount_point VFS mount point to erase (e.g., "/sdcard")
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if mount_point is NULL or points to SPIFFS
 *      - ESP_FAIL if directory cannot be opened
 */
esp_err_t sd_card_erase_all(const char *mount_point);

/**
 * @brief Print SD card status information to console
 *
 * @param output_type Output verbosity level (COMPACT, NORMAL, VERBOSE)
 */
void sd_card_print_status(status_output_type_t output_type);
