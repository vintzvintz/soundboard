/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "sdkconfig.h"

#include "esp_err.h"
#include "driver/gpio.h"   // IWYU pragma: keep
#include "soundboard.h"

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Opaque handle to oled display instance
 */
typedef struct display_state_s* display_handle_t;

/**
 * @brief Display module configuration structure
 */
typedef struct {
    gpio_num_t sda_gpio;          /**< I2C SDA GPIO pin */
    gpio_num_t scl_gpio;          /**< I2C SCL GPIO pin */
    uint8_t i2c_address;          /**< I2C device address (default: 0x3C for SSD1306) */
    uint32_t i2c_freq_hz;         /**< I2C frequency in Hz (default: 400000 = 400kHz fast mode) */
} display_config_t;

/**
 * @brief Default display configuration initializer
 *
 * Uses values from Kconfig (idf.py menuconfig -> Soundboard Application)
 */
#define DISPLAY_CONFIG_DEFAULT() {                              \
    .sda_gpio = (gpio_num_t)CONFIG_SOUNDBOARD_DISPLAY_SDA_GPIO, \
    .scl_gpio = (gpio_num_t)CONFIG_SOUNDBOARD_DISPLAY_SCL_GPIO, \
    .i2c_address = CONFIG_SOUNDBOARD_DISPLAY_I2C_ADDR,          \
    .i2c_freq_hz = CONFIG_SOUNDBOARD_DISPLAY_I2C_FREQ,          \
}

/**
 * @brief Initialize display module
 *
 * Initializes I2C bus and u8g2 library for SSD1306 128x64 OLED display.
 * Displays initial "Ready" screen.
 *
 * @param config Pointer to display configuration structure (NULL uses defaults)
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_ARG if GPIO pins are invalid
 *     - ESP_ERR_NO_MEM if memory allocation fails
 *     - Other ESP_ERR_* codes from I2C driver initialization
 */
esp_err_t display_init(const display_config_t *config, display_handle_t *display_handle);


/**
 * @brief Deinitialize display module
 *
 * Powers off display and cleans up I2C resources.
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if display module not initialized
 */
esp_err_t display_deinit(display_handle_t oled);


/**
 * @brief Show startup layout
 *
 * Displays "Wait USB device".
 * Call this after display_init() and before USB device is detected.
 */
void display_show_startup(display_handle_t oled);

/**
 * @brief Show idle layout
 *
 * Displays page number, volume, SD card status, and config source.
 * Call this when soundboard is ready but not playing.
 */
void display_show_idle(display_handle_t oled);

/**
 * @brief Event: Volume changed
 *
 * Updates internal state and refreshes display if not playing.
 *
 * @param volume_index New volume level index
 */
void display_on_volume_changed(display_handle_t oled, int volume_index);

/**
 * @brief Event: Playback started or stopped
 *
 * Updates internal state and shows playing screen with filename or idle screen.
 *
 * @param filename Full path to audio file (e.g., "/sdcard/sound1.wav") or NULL/empty if playback stopped
 * @param progress Playback progress (0 = start, UINT16_MAX = end)
 */
void display_on_playing(display_handle_t oled, const char *filename, uint16_t progress);

/**
 * @brief Event: Page changed
 *
 * Updates internal state and refreshes display if not playing.
 *
 * @param page_id Page string identifier (e.g., "Base", "FX")
 */
void display_on_page_changed(display_handle_t oled, const char *page_id);

/**
 * @brief Event: Encoder mode changed
 *
 * Shows page select layout when entering PAGE mode, returns to
 * appropriate layout (idle or playing) when leaving PAGE mode.
 *
 * @param is_page_mode true if entering PAGE mode, false if leaving (VOLUME mode)
 */
void display_on_encoder_mode_changed(display_handle_t oled, bool is_page_mode);

/**
 * @brief Show reboot layout
 *
 * Shows "Rebooting..." message on display. Display will be reinitialized after reboot.
 */
void display_show_reboot(display_handle_t oled);

/**
 * @brief Event: Error occurred
 *
 * Displays error message.
 *
 * @param message Error message to display (will be truncated to fit screen)
 */
void display_on_error(display_handle_t oled, const char *message);

/**
 * @brief Show MSC analysis layout
 *
 * Displays "Checking data" with status message during MSC device validation.
 *
 * @param status_msg Status message to display (e.g., "Reading mappings...")
 */
void display_on_msc_analysis(display_handle_t oled, const char *status_msg);

/**
 * @brief Show MSC interactive menu layout
 *
 * Displays "USB Update" title with 3 menu items and a selection indicator.
 * Menu items: "Full update", "Incremental update", "Clear SD card".
 *
 * @param selected_index Currently selected menu item (0=Full, 1=Incremental, 2=Clear SD)
 */
void display_on_msc_menu(display_handle_t oled, int selected_index);

/**
 * @brief Show MSC SD card clear confirmation layout
 *
 * Displays warning screen asking user to press a bottom-row button (10-12)
 * to confirm destructive SD card erase, or any other button to cancel.
 */
void display_on_msc_sd_clear_confirm(display_handle_t oled);

/**
 * @brief Show MSC progress layout
 *
 * Displays "Updating..." with filename being copied.
 *
 * @param filename Name of file currently being copied
 * @param progress Progress value (0 = start, UINT16_MAX = complete)
 */
void display_on_msc_progress(display_handle_t oled, const char *filename, uint16_t progress);

/**
 * @brief Print display module status information to console
 *
 * @param handle Display handle (NULL-safe)
 * @param output_type Output verbosity level (COMPACT, NORMAL, VERBOSE)
 */
void display_print_status(display_handle_t handle, status_output_type_t output_type);


#ifdef __cplusplus
}
#endif
