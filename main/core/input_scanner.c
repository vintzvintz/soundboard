/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

#include "input_scanner.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"
#include <inttypes.h>

static const char *TAG = "input_scanner";

/**
 * @brief Encoder steps per detent (mechanical click)
 *
 * Standard quadrature encoders generate 4 Gray code transitions per detent.
 * Each detent produces a complete cycle through states: 00 → 01 → 11 → 10 → 00
 */
#define ENCODER_STEPS_PER_DETENT 4

/**
 * @brief Button state machine states
 */
typedef enum {
    BUTTON_STATE_IDLE,              // Button released, stable
    BUTTON_STATE_DEBOUNCE_PRESS,    // Press detected, debouncing
    BUTTON_STATE_PRESSED,           // Stable press confirmed
    BUTTON_STATE_DEBOUNCE_RELEASE   // Release detected, debouncing
} button_state_t;

// Forward declaration
struct input_scanner_s;

/**
 * @brief Per-button state tracking
 *
 * Used for both matrix buttons and encoder switch.
 * Contains FSM state, timing information, debounce configuration, and button identification.
 */
typedef struct {
    button_state_t state;           // Current FSM state
    int64_t state_change_time_us;   // When state last changed (for debouncing)
    int64_t press_start_time_us;    // When button was pressed (for long press)
    bool long_press_triggered;      // Flag to prevent double long-press events

    // Debounce/timing configuration (microseconds)
    int64_t debounce_press_us;      // Press debounce threshold
    int64_t debounce_release_us;    // Release debounce threshold
    int64_t long_press_us;          // Long press threshold

    // Button identification and callback
    uint8_t btn_num;                // Button number (0 for encoder, col + row * MATRIX_COLS + 1 for matrix)
    input_scanner_callback_t callback;  // User callback function
    void *user_ctx;                 // User context for callback
} button_state_info_t;

/**
 * @brief Encoder quadrature state
 *
 * Self-contained encoder state similar to button_state_info_t.
 * Stores all necessary configuration for polling without needing scanner handle.
 */
typedef struct {
    // Quadrature state
    uint8_t last_clk;               // Last CLK pin level
    uint8_t last_dt;                // Last DT pin level
    int8_t step_counter;            // Accumulated steps (-4 to +4, ±4 = one detent)
    int64_t last_transition_us;     // Last valid transition time (for debouncing)

    // GPIO configuration
    gpio_num_t clk_gpio;            // CLK pin (A phase)
    gpio_num_t dt_gpio;             // DT pin (B phase)
    gpio_num_t sw_gpio;             // Switch pin (or GPIO_NUM_NC)

    // Timing configuration
    int64_t debounce_us;            // Rotation debounce threshold in microseconds

    // Callback
    input_scanner_callback_t callback;  // User callback function
    void *user_ctx;                     // User context for callback

    // Switch button state (uses button_state_info_t)
    button_state_info_t sw_state;
} encoder_state_info_t;

/**
 * @brief Input scanner handle structure
 */
struct input_scanner_s {
    input_scanner_config_t config;
    TaskHandle_t task_handle;
    bool initialized;
    bool running;

    // Matrix state
    button_state_info_t *button_states;  // Size: MATRIX_ROWS * MATRIX_COLS

    // Encoder state
    encoder_state_info_t encoder_state;
};

// ===== Helper Functions =====

/**
 * @brief Get button state by row/col indices
 */
static inline button_state_info_t *get_button_state(input_scanner_handle_t handle, uint8_t row, uint8_t col)
{
    if (row >= MATRIX_ROWS || col >= MATRIX_COLS) {
        return NULL;
    }
    return &handle->button_states[(row * MATRIX_COLS) + col];
}

/**
 * @brief Initialize a button state structure
 *
 * @param btn Button state to initialize
 * @param btn_num Button number (0 for encoder, col + row * MATRIX_COLS + 1 for matrix)
 * @param debounce_press_us Press debounce time in microseconds
 * @param debounce_release_us Release debounce time in microseconds
 * @param long_press_us Long press threshold in microseconds
 * @param callback User callback function
 * @param user_ctx User context for callback
 */
static inline void init_button_state(button_state_info_t *btn,
                                     uint8_t btn_num,
                                     int64_t debounce_press_us,
                                     int64_t debounce_release_us,
                                     int64_t long_press_us,
                                     input_scanner_callback_t callback,
                                     void *user_ctx)
{
    btn->state = BUTTON_STATE_IDLE;
    btn->state_change_time_us = 0;
    btn->press_start_time_us = 0;
    btn->long_press_triggered = false;
    btn->debounce_press_us = debounce_press_us;
    btn->debounce_release_us = debounce_release_us;
    btn->long_press_us = long_press_us;
    btn->btn_num = btn_num;
    btn->callback = callback;
    btn->user_ctx = user_ctx;
}

/**
 * @brief Unified button state machine
 *
 * Handles debouncing, long-press detection for any button (matrix or encoder switch).
 * Invokes callbacks on press, long-press, and release events.
 * All parameters (timing, identification, callback) are read from the button state structure.
 *
 * @param btn Button state structure (includes timing, identification, and callback)
 * @param pressed Current physical button state (true = pressed)
 * @param now Current time in microseconds
 */
static void button_fsm_update(button_state_info_t *btn, bool pressed, int64_t now)
{
    switch (btn->state) {
        case BUTTON_STATE_IDLE:
            if (pressed) {
                btn->state = BUTTON_STATE_DEBOUNCE_PRESS;
                btn->state_change_time_us = now;
            }
            break;

        case BUTTON_STATE_DEBOUNCE_PRESS:
            if (!pressed) {
                // Released before debounce completed (glitch)
                btn->state = BUTTON_STATE_IDLE;
            } else if ((now - btn->state_change_time_us) >= btn->debounce_press_us) {
                // Debounce time elapsed, confirm press
                btn->state = BUTTON_STATE_PRESSED;
                btn->press_start_time_us = now;
                btn->long_press_triggered = false;

                // Invoke user callback
                if (btn->callback != NULL) {
                    btn->callback(btn->btn_num, INPUT_EVENT_BUTTON_PRESS, btn->user_ctx);
                }
            }
            break;

        case BUTTON_STATE_PRESSED:
            if (!pressed) {
                btn->state = BUTTON_STATE_DEBOUNCE_RELEASE;
                btn->state_change_time_us = now;
            } else if (!btn->long_press_triggered && (now - btn->press_start_time_us) >= btn->long_press_us) {
                // Long press threshold reached
                btn->long_press_triggered = true;

                // Invoke user callback
                if (btn->callback != NULL) {
                    btn->callback(btn->btn_num, INPUT_EVENT_BUTTON_LONG_PRESS, btn->user_ctx);
                }
            }
            break;

        case BUTTON_STATE_DEBOUNCE_RELEASE:
            if (pressed) {
                // Pressed again before debounce completed (glitch)
                btn->state = BUTTON_STATE_PRESSED;
            } else if ((now - btn->state_change_time_us) >= btn->debounce_release_us) {
                // Debounce time elapsed, confirm release
                btn->state = BUTTON_STATE_IDLE;

                // Invoke user callback
                if (btn->callback != NULL) {
                    btn->callback(btn->btn_num, INPUT_EVENT_BUTTON_RELEASE, btn->user_ctx);
                }
            }
            break;
    }
}

// ===== Encoder Gray Code Decoding =====

/**
 * @brief Decode encoder direction from state transition
 *
 * Gray code quadrature encoding:
 * CW:  00 → 01 → 11 → 10 → 00 (steps: +1, +1, +1, +1)
 * CCW: 00 → 10 → 11 → 01 → 00 (steps: -1, -1, -1, -1)
 *
 * Returns +1 for CW, -1 for CCW, 0 for invalid/no change
 */
static int8_t encoder_decode_direction(uint8_t prev_state, uint8_t new_state)
{
    // Gray code state transition table
    static const int8_t transition_table[16] = {
        0,  // 00 → 00: no change
        1,  // 00 → 01: CW
        -1, // 00 → 10: CCW
        0,  // 00 → 11: invalid
        -1, // 01 → 00: CCW
        0,  // 01 → 01: no change
        0,  // 01 → 10: invalid
        1,  // 01 → 11: CW
        1,  // 10 → 00: CW
        0,  // 10 → 01: invalid
        0,  // 10 → 10: no change
        -1, // 10 → 11: CCW
        0,  // 11 → 00: invalid
        -1, // 11 → 01: CCW
        1,  // 11 → 10: CW
        0,  // 11 → 11: no change
    };

    return transition_table[(prev_state << 2) | new_state];
}

// ===== Matrix Scanning =====

/**
 * @brief Scan one row and update button states
 *
 * Uses unified button FSM for all buttons in the row.
 */
static void scan_row(input_scanner_handle_t handle, uint8_t row)
{
    int64_t now = esp_timer_get_time();

    // Drive this row HIGH (active) - allows current to flow through diode when button pressed
    gpio_set_level(handle->config.row_gpios[row], 1);

    // Small delay for signal settling (diodes + wire capacitance)
    ets_delay_us(MATRIX_SCAN_SETTLE_DELAY_US);

    // Read all columns
    for (uint8_t col = 0; col < MATRIX_COLS; col++) {
        int level = gpio_get_level(handle->config.col_gpios[col]);
        bool pressed = (level == 1);  // HIGH = pressed (pulled up through diode + switch from active row)

        button_state_info_t *btn = get_button_state(handle, row, col);
        if (btn == NULL) continue;

        // Update button FSM (all configuration is in btn structure)
        button_fsm_update(btn, pressed, now);
    }

    // Drive row LOW (inactive)
    gpio_set_level(handle->config.row_gpios[row], 0);
}

// ===== Encoder Polling =====

/**
 * @brief Poll encoder quadrature pins for rotation
 *
 * Includes debouncing to filter noise and prevent spurious rotation events.
 * All configuration (GPIOs, timing, callback) is stored in the encoder state structure.
 *
 * @param enc Encoder state structure (self-contained)
 */
static void poll_encoder_quadrature(encoder_state_info_t *enc)
{
    int64_t now = esp_timer_get_time();

    uint8_t clk = gpio_get_level(enc->clk_gpio);
    uint8_t dt = gpio_get_level(enc->dt_gpio);

    // Detect ANY pin change (CLK or DT) — both edges matter for quadrature decoding
    if (clk != enc->last_clk || dt != enc->last_dt) {
        uint8_t prev_state = (enc->last_clk << 1) | enc->last_dt;
        uint8_t new_state = (clk << 1) | dt;

        int8_t direction = encoder_decode_direction(prev_state, new_state);
        if (direction != 0) {
            // Apply debouncing: ignore transitions that occur too quickly
            if ((now - enc->last_transition_us) >= enc->debounce_us) {
                enc->step_counter = (int8_t)(enc->step_counter + direction);
                enc->last_transition_us = now;

                // Check for detent completion (ENCODER_STEPS_PER_DETENT steps in same direction)
                if (enc->step_counter >= ENCODER_STEPS_PER_DETENT) {
                    if (enc->callback != NULL) {
                        enc->callback(enc->sw_state.btn_num, INPUT_EVENT_ENCODER_ROTATE_CCW, enc->user_ctx);
                    }
                    enc->step_counter = 0;
                } else if (enc->step_counter <= -ENCODER_STEPS_PER_DETENT) {
                    if (enc->callback != NULL) {
                        enc->callback(enc->sw_state.btn_num, INPUT_EVENT_ENCODER_ROTATE_CW, enc->user_ctx);
                    }
                    enc->step_counter = 0;
                }
            }
        }
    }

    enc->last_clk = clk;
    enc->last_dt = dt;
}

/**
 * @brief Poll encoder switch button
 *
 * Uses unified button FSM. All configuration (GPIO, timing, callback) is stored
 * in the encoder state structure.
 *
 * @param enc Encoder state structure (self-contained)
 */
static void poll_encoder_switch(encoder_state_info_t *enc)
{
    int64_t now = esp_timer_get_time();
    int sw_level = gpio_get_level(enc->sw_gpio);
    bool pressed = (sw_level == 0);  // Active low

    // Update button FSM (all configuration is in sw_state structure)
    button_fsm_update(&enc->sw_state, pressed, now);
}

// ===== Main Task =====

/**
 * @brief Input scanner task - periodically polls matrix and encoder
 */
static void input_scanner_task(void *arg)
{
    input_scanner_handle_t handle = (input_scanner_handle_t)arg;
    if (handle == NULL) {
        ESP_LOGE(TAG, "Scanner task received NULL handle");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Input scanner task started (%dx%d matrix + encoder, scan interval %lu ms)",
             MATRIX_ROWS, MATRIX_COLS, handle->config.scan_interval_ms);

    TickType_t scan_interval_ticks = pdMS_TO_TICKS(handle->config.scan_interval_ms);

    while (handle->running) {
        TickType_t start_tick = xTaskGetTickCount();

        // Scan matrix
        for (uint8_t row = 0; row < MATRIX_ROWS; row++) {
            scan_row(handle, row);
        }

        // Poll encoder quadrature
        poll_encoder_quadrature(&handle->encoder_state);

        // Poll encoder switch (if present)
        if (handle->config.encoder_sw_gpio != GPIO_NUM_NC) {
            poll_encoder_switch(&handle->encoder_state);
        }

        // Wait for next scan interval (deterministic timing)
        vTaskDelayUntil(&start_tick, scan_interval_ticks);
    }

    ESP_LOGI(TAG, "Input scanner task stopped");
    vTaskDelete(NULL);
}

// ===== Public API =====

esp_err_t input_scanner_init(const input_scanner_config_t *config, input_scanner_handle_t *out_handle)
{
    if (config == NULL || out_handle == NULL) {
        ESP_LOGE(TAG, "Config or out_handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // Validate encoder GPIOs
    if (!GPIO_IS_VALID_GPIO(config->encoder_clk_gpio) || !GPIO_IS_VALID_GPIO(config->encoder_dt_gpio)) {
        ESP_LOGE(TAG, "Invalid encoder GPIO(s)");
        return ESP_ERR_INVALID_ARG;
    }

    // Validate callback
    if (config->callback == NULL) {
        ESP_LOGE(TAG, "Callback is required");
        return ESP_ERR_INVALID_ARG;
    }

    // Validate all matrix GPIOs
    for (uint8_t i = 0; i < MATRIX_ROWS; i++) {
        if (!GPIO_IS_VALID_OUTPUT_GPIO(config->row_gpios[i])) {
            ESP_LOGE(TAG, "Invalid row GPIO: %d", config->row_gpios[i]);
            return ESP_ERR_INVALID_ARG;
        }
    }
    for (uint8_t i = 0; i < MATRIX_COLS; i++) {
        if (!GPIO_IS_VALID_GPIO(config->col_gpios[i])) {
            ESP_LOGE(TAG, "Invalid column GPIO: %d", config->col_gpios[i]);
            return ESP_ERR_INVALID_ARG;
        }
    }

    // Allocate handle
    input_scanner_handle_t handle = (input_scanner_handle_t)heap_caps_calloc(1, sizeof(struct input_scanner_s), MALLOC_CAP_INTERNAL);
    if (handle == NULL) {
        ESP_LOGE(TAG, "Failed to allocate scanner handle");
        return ESP_ERR_NO_MEM;
    }

    // Copy configuration (includes fixed-size GPIO arrays)
    memcpy(&handle->config, config, sizeof(input_scanner_config_t));

    // Allocate button state array
    size_t num_buttons = (size_t)MATRIX_ROWS * MATRIX_COLS;
    handle->button_states = (button_state_info_t *)heap_caps_calloc(num_buttons, sizeof(button_state_info_t), MALLOC_CAP_INTERNAL);
    if (handle->button_states == NULL) {
        ESP_LOGE(TAG, "Failed to allocate button state array");
        heap_caps_free(handle);
        return ESP_ERR_NO_MEM;
    }

    // Convert timing configuration to microseconds
    int64_t debounce_press_us = (int64_t)config->button_debounce_press_ms * 1000;
    int64_t debounce_release_us = (int64_t)config->button_debounce_release_ms * 1000;
    int64_t long_press_us = (int64_t)config->long_press_ms * 1000;

    // Initialize button states
    for (size_t i = 0; i < num_buttons; i++) {
        uint8_t row = i / MATRIX_COLS;
        uint8_t col = i % MATRIX_COLS;
        // Calculate button number: col + row * MATRIX_COLS + 1 (1-based for matrix buttons)
        uint8_t btn_num = col + (row * MATRIX_COLS) + 1;
        init_button_state(&handle->button_states[i], btn_num,
                         debounce_press_us, debounce_release_us, long_press_us,
                         config->callback, config->user_ctx);
    }

    // Initialize encoder state (self-contained)
    handle->encoder_state.last_clk = gpio_get_level(config->encoder_clk_gpio);
    handle->encoder_state.last_dt = gpio_get_level(config->encoder_dt_gpio);
    handle->encoder_state.step_counter = 0;
    handle->encoder_state.last_transition_us = 0;
    handle->encoder_state.clk_gpio = config->encoder_clk_gpio;
    handle->encoder_state.dt_gpio = config->encoder_dt_gpio;
    handle->encoder_state.sw_gpio = config->encoder_sw_gpio;
    handle->encoder_state.debounce_us = (int64_t)config->encoder_debounce_us;
    handle->encoder_state.callback = config->callback;
    handle->encoder_state.user_ctx = config->user_ctx;

    // Initialize encoder switch state (button number 0)
    init_button_state(&handle->encoder_state.sw_state, 0,
                     debounce_press_us, debounce_release_us, long_press_us,
                     config->callback, config->user_ctx);

    esp_err_t ret;

    // Configure row GPIOs (outputs, default LOW = inactive)
    for (uint8_t i = 0; i < MATRIX_ROWS; i++) {
        gpio_config_t row_conf = {
            .pin_bit_mask = (1ULL << config->row_gpios[i]),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        ret = gpio_config(&row_conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure row GPIO %d", config->row_gpios[i]);
            goto cleanup;
        }
        gpio_set_level(config->row_gpios[i], 0);  // Set LOW (inactive)
    }

    // Configure column GPIOs (inputs with pull-downs)
    for (uint8_t i = 0; i < MATRIX_COLS; i++) {
        gpio_config_t col_conf = {
            .pin_bit_mask = (1ULL << config->col_gpios[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        ret = gpio_config(&col_conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure column GPIO %d", config->col_gpios[i]);
            goto cleanup;
        }
    }

    // Configure encoder CLK and DT GPIOs (inputs with pull-ups)
    gpio_config_t enc_conf = {
        .pin_bit_mask = (1ULL << config->encoder_clk_gpio) | (1ULL << config->encoder_dt_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE  // No interrupts for polling
    };
    ret = gpio_config(&enc_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure encoder quadrature GPIOs");
        goto cleanup;
    }

    // Configure encoder switch GPIO (if present)
    if (config->encoder_sw_gpio != GPIO_NUM_NC) {
        gpio_config_t sw_conf = {
            .pin_bit_mask = (1ULL << config->encoder_sw_gpio),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE  // No interrupts for polling
        };
        ret = gpio_config(&sw_conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure encoder switch GPIO %d", config->encoder_sw_gpio);
            goto cleanup;
        }
    }

    // Create scanner task
    handle->running = true;
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        input_scanner_task,
        "input_scanner",
        handle->config.task_stack_size,
        handle,
        handle->config.task_priority,
        &handle->task_handle,
        handle->config.task_core_id
    );

    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create scanner task");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    handle->initialized = true;
    *out_handle = handle;

    ESP_LOGI(TAG, "Input scanner initialized: %dx%d matrix (%zu buttons) + encoder",
             MATRIX_ROWS, MATRIX_COLS, num_buttons);

    return ESP_OK;

cleanup:
    heap_caps_free(handle->button_states);
    heap_caps_free(handle);
    return ret;
}

esp_err_t input_scanner_deinit(input_scanner_handle_t handle)
{
    if (handle == NULL) {
        ESP_LOGE(TAG, "Handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!handle->initialized) {
        ESP_LOGW(TAG, "Scanner not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Stop task
    handle->running = false;

    // Wait for task to finish (give it 100ms)
    vTaskDelay(pdMS_TO_TICKS(100));

    // Reset all row GPIOs to LOW (inactive)
    for (uint8_t i = 0; i < MATRIX_ROWS; i++) {
        gpio_set_level(handle->config.row_gpios[i], 0);
    }

    // Free resources
    heap_caps_free(handle->button_states);

    handle->initialized = false;
    heap_caps_free(handle);

    ESP_LOGI(TAG, "Input scanner deinitialized");

    return ESP_OK;
}

void input_scanner_print_status(input_scanner_handle_t handle, status_output_type_t output_type)
{
    if (handle == NULL) {
        if (output_type == STATUS_OUTPUT_COMPACT) {
            printf("[input] not initialized\n");
        } else {
            printf("Input Scanner Status:\n");
            printf("  State: Not initialized\n");
        }
        return;
    }

    bool running = handle->running;
    uint32_t scan_interval = handle->config.scan_interval_ms;

    // Count currently pressed buttons
    int pressed_count = 0;
    for (int i = 0; i < MATRIX_ROWS * MATRIX_COLS; i++) {
        if (handle->button_states[i].state == BUTTON_STATE_PRESSED ||
            handle->button_states[i].state == BUTTON_STATE_DEBOUNCE_RELEASE) {
            pressed_count++;
        }
    }

    // Check encoder switch state
    bool encoder_pressed = (handle->encoder_state.sw_state.state == BUTTON_STATE_PRESSED ||
                           handle->encoder_state.sw_state.state == BUTTON_STATE_DEBOUNCE_RELEASE);

    if (output_type == STATUS_OUTPUT_COMPACT) {
        printf("[input] %s, %" PRIu32 "ms scan, %d button%s pressed\n",
               running ? "running" : "stopped",
               scan_interval,
               pressed_count,
               pressed_count == 1 ? "" : "s");
    } else {
        printf("Input Scanner Status:\n");
        printf("  Task: %s\n", running ? "Running" : "Stopped");
        printf("  Scan interval: %" PRIu32 " ms\n", scan_interval);
        printf("  Matrix: %d button%s pressed\n", pressed_count, pressed_count == 1 ? "" : "s");
        printf("  Encoder switch: %s\n", encoder_pressed ? "Pressed" : "Released");

        if (output_type == STATUS_OUTPUT_VERBOSE) {
            printf("  Matrix GPIOs:\n");
            printf("    Rows: %d, %d, %d, %d\n",
                   handle->config.row_gpios[0], handle->config.row_gpios[1],
                   handle->config.row_gpios[2], handle->config.row_gpios[3]);
            printf("    Cols: %d, %d, %d\n",
                   handle->config.col_gpios[0], handle->config.col_gpios[1],
                   handle->config.col_gpios[2]);
            printf("  Encoder GPIOs: CLK=%d, DT=%d, SW=%d\n",
                   handle->config.encoder_clk_gpio,
                   handle->config.encoder_dt_gpio,
                   handle->config.encoder_sw_gpio);
            printf("  Debounce: press=%" PRIu32 "ms, release=%" PRIu32 "ms, long_press=%" PRIu32 "ms\n",
                   handle->config.button_debounce_press_ms,
                   handle->config.button_debounce_release_ms,
                   handle->config.long_press_ms);
        }
    }
}
