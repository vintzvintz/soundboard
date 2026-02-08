/**
 * @file input_scanner.h
 * @brief Unified polling-based input scanner for matrix keypad and rotary encoder
 *
 * This module combines matrix keypad scanning and rotary encoder polling into a single
 * FreeRTOS task with consistent architecture and callback contexts.
 */

#pragma once

#include "freertos/FreeRTOS.h"   // IWYU pragma: keep
#include "driver/gpio.h"         // IWYU pragma: keep
#include "sdkconfig.h"
#include "soundboard.h"

/**
 * @brief Opaque handle for input scanner instance
 */
typedef struct input_scanner_s *input_scanner_handle_t;

/**
 * @brief Unified input event types for all input sources
 *
 * This enum represents all possible events from both matrix keypad and rotary encoder.
 * It serves as the canonical input event type used throughout the application including:
 * - Input scanner callbacks
 * - Mapper event handling
 * - Configuration file parsing (mappings.csv)
 * - Debug/display modules
 *
 * The unified type eliminates the need for translation between hardware and application layers.
 */
typedef enum {
    // Button events (matrix buttons and encoder switch)
    INPUT_EVENT_BUTTON_PRESS,           ///< Button pressed (after debounce)
    INPUT_EVENT_BUTTON_LONG_PRESS,      ///< Button held for long press duration
    INPUT_EVENT_BUTTON_RELEASE,         ///< Button released (after debounce)

    // Encoder rotation events
    INPUT_EVENT_ENCODER_ROTATE_CW,      ///< Encoder rotated clockwise (one detent)
    INPUT_EVENT_ENCODER_ROTATE_CCW,     ///< Encoder rotated counter-clockwise (one detent)
} input_event_type_t;

/**
 * @brief Input scanner event callback
 *
 * Called from input scanner task context (NOT ISR) when any input event occurs.
 * Handles both matrix button events and encoder events with a unified interface.
 *
 * Button numbering:
 *   - 0: Encoder switch
 *   - 1-12: Matrix buttons (col + row * MATRIX_COLS + 1)
 *     Example for 4x3 matrix: buttons 1-3 (row 0), 4-6 (row 1), 7-9 (row 2), 10-12 (row 3)
 *
 * For button events (BUTTON_PRESS/LONG_PRESS/RELEASE):
 *   - btn_num identifies which button (0 for encoder, 1-12 for matrix)
 *
 * For encoder rotation events (ENCODER_ROTATE_CW/CCW):
 *   - btn_num is not used (set to 0)
 *
 * @param btn_num Button number (0 for encoder, col + row * MATRIX_COLS + 1 for matrix buttons)
 * @param event Event type from input_event_type_t enum
 * @param user_ctx User context pointer passed during initialization
 */
typedef void (*input_scanner_callback_t)(
    uint8_t btn_num,
    input_event_type_t event,
    void *user_ctx
);

/**
 * @brief Matrix keypad dimensions (fixed at compile time)
 */
#define MATRIX_ROWS 4
#define MATRIX_COLS 3

/**
 * @brief Signal settling delay for matrix row scanning (microseconds)
 *
 * After driving a row HIGH, a small delay is needed for signal settling
 * due to diode forward voltage drop and wire/PCB capacitance.
 * Typical range: 5-20us depending on hardware layout.
 */
#define MATRIX_SCAN_SETTLE_DELAY_US 10

/**
 * @brief Input scanner configuration
 */
typedef struct {
    // Matrix keypad configuration
    gpio_num_t row_gpios[MATRIX_ROWS];  ///< Array of row GPIO numbers (4x3 matrix)
    gpio_num_t col_gpios[MATRIX_COLS];  ///< Array of column GPIO numbers

    // Rotary encoder configuration
    gpio_num_t encoder_clk_gpio;        ///< Encoder CLK pin (A phase)
    gpio_num_t encoder_dt_gpio;         ///< Encoder DT pin (B phase)
    gpio_num_t encoder_sw_gpio;         ///< Encoder switch pin (GPIO_NUM_NC if unused)

    // Timing parameters
    uint32_t scan_interval_ms;          ///< Polling interval in milliseconds (default: 5ms)

    // Button debounce parameters (matrix + encoder switch)
    uint32_t button_debounce_press_ms;  ///< Press debounce time in milliseconds (default: 10ms)
    uint32_t button_debounce_release_ms;///< Release debounce time in milliseconds (default: 50ms)
    uint32_t long_press_ms;             ///< Long press threshold in milliseconds (default: 1000ms)

    // Encoder rotation parameters
    uint32_t encoder_debounce_us;       ///< Encoder rotation debounce in microseconds (default: 1000us)

    // Callback
    input_scanner_callback_t callback;  ///< Unified input event callback (required)
    void *user_ctx;                     ///< User context passed to callback

    // Task configuration
    uint8_t task_priority;              ///< FreeRTOS task priority (default: 3)
    uint32_t task_stack_size;           ///< Task stack size in bytes (default: 4096)
    BaseType_t task_core_id;            ///< CPU core ID to pin task to (default: 0, -1 for no affinity)
} input_scanner_config_t;

/**
 * @brief Default input scanner configuration macro
 *
 * All hardware parameters (GPIOs, timings) are sourced from Kconfig.
 * Only callback, user_ctx, and task_core_id need to be set by the caller.
 *
 * Usage:
 * @code
 * input_scanner_config_t config = INPUT_SCANNER_DEFAULT_CONFIG();
 * config.callback = my_callback;
 * config.user_ctx = my_context;
 * config.task_core_id = 1;
 * @endcode
 */
#define INPUT_SCANNER_DEFAULT_CONFIG() { \
    .row_gpios = { \
        CONFIG_SOUNDBOARD_MATRIX_ROW_GPIO_0, \
        CONFIG_SOUNDBOARD_MATRIX_ROW_GPIO_1, \
        CONFIG_SOUNDBOARD_MATRIX_ROW_GPIO_2, \
        CONFIG_SOUNDBOARD_MATRIX_ROW_GPIO_3, \
    }, \
    .col_gpios = { \
        CONFIG_SOUNDBOARD_MATRIX_COL_GPIO_0, \
        CONFIG_SOUNDBOARD_MATRIX_COL_GPIO_1, \
        CONFIG_SOUNDBOARD_MATRIX_COL_GPIO_2, \
    }, \
    .encoder_clk_gpio = CONFIG_SOUNDBOARD_ENCODER_CLK_GPIO, \
    .encoder_dt_gpio = CONFIG_SOUNDBOARD_ENCODER_DT_GPIO, \
    .encoder_sw_gpio = CONFIG_SOUNDBOARD_ENCODER_SW_GPIO, \
    .scan_interval_ms = CONFIG_SOUNDBOARD_MATRIX_SCAN_INTERVAL_MS, \
    .button_debounce_press_ms = CONFIG_SOUNDBOARD_MATRIX_DEBOUNCE_PRESS_MS, \
    .button_debounce_release_ms = CONFIG_SOUNDBOARD_MATRIX_DEBOUNCE_RELEASE_MS, \
    .long_press_ms = CONFIG_SOUNDBOARD_MATRIX_LONG_PRESS_MS, \
    .encoder_debounce_us = CONFIG_SOUNDBOARD_ENCODER_DEBOUNCE_MS * 1000, \
    .callback = NULL, \
    .user_ctx = NULL, \
    .task_priority = 3, \
    .task_stack_size = 4096, \
    .task_core_id = 0, \
}

/**
 * @brief Initialize unified input scanner
 *
 * Creates a FreeRTOS task that polls the matrix keypad and rotary encoder at the
 * configured scan interval. All callbacks are invoked from task context (not ISR).
 *
 * @param config Configuration structure
 * @param out_handle Output handle on success
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t input_scanner_init(const input_scanner_config_t *config,
                              input_scanner_handle_t *out_handle);

/**
 * @brief Deinitialize input scanner
 *
 * Stops the scanner task, releases GPIOs, and frees all resources.
 *
 * @param handle Scanner handle to deinitialize
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t input_scanner_deinit(input_scanner_handle_t handle);

/**
 * @brief Print input scanner status information to console
 *
 * @param handle Scanner handle (NULL-safe)
 * @param output_type Output verbosity level (COMPACT, NORMAL, VERBOSE)
 */
void input_scanner_print_status(input_scanner_handle_t handle, status_output_type_t output_type);
