/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file msc.h
 * @brief Interactive MSC (Mass Storage Class) update module with FSM-driven task
 *
 * This module manages the full MSC update lifecycle:
 * - USB host library and MSC driver setup
 * - FSM task for interactive menu navigation via rotary encoder
 * - Three update modes: full update, incremental update, SD card clear
 * - Progress reporting via event callback
 *
 * The module is self-contained: it owns USB host setup, MSC driver,
 * and its own FreeRTOS task. It does NOT depend on display.h.
 */

#pragma once

#include "freertos/FreeRTOS.h"  // IWYU pragma: keep
#include "input_scanner.h"

/**
 * @brief Opaque MSC handle
 */
typedef struct msc_s *msc_handle_t;

/**
 * @brief MSC event types (sent to main app via callback)
 */
typedef enum {
    MSC_EVENT_ANALYSIS,                  /**< Show analysis screen (with status message) */
    MSC_EVENT_MENU_UPDATE_SELECTED,      /**< Menu: update option highlighted (see menu.incremental) */
    MSC_EVENT_MENU_SD_CLEAR_SELECTED,    /**< Menu: "Clear SD card" highlighted */
    MSC_EVENT_CONFIRM,                   /**< Awaiting user confirmation (with action + message) */
    MSC_EVENT_UPDATING,                  /**< Update in progress (with progress data) */
    MSC_EVENT_UPDATE_DONE,               /**< Update completed successfully */
    MSC_EVENT_UPDATE_FAILED,             /**< Update failed (with error message) */
} msc_event_type_t;

/**
 * @brief MSC event data passed to notification callback
 */
typedef struct {
    msc_event_type_t type;
    union {
        struct {
            const char *status_msg;      /**< Analysis status message */
        } analysis;
        struct {
            bool incremental;            /**< true=incremental, false=full */
        } menu;
        struct {
            const char *action;          /**< Confirmation title (e.g., "ERASE SDCARD") */
            const char *line1;           /**< Message line 1 */
            const char *line2;           /**< Message line 2 (can be NULL) */
        } confirm;
        struct {
            const char *filename;        /**< Current file being copied */
            uint16_t progress;           /**< 0 = start, UINT16_MAX = complete */
        } progress;
        struct {
            const char *message;         /**< Error description */
        } error;
    };
} msc_event_data_t;

/**
 * @brief MSC event notification callback type
 *
 * Called from the MSC FSM task context when state changes occur.
 * The callback should be lightweight (e.g., update display, log).
 *
 * @param event Event data with type and payload
 * @param user_ctx User context pointer passed during init
 */
typedef void (*msc_event_cb_t)(const msc_event_data_t *event, void *user_ctx);

/**
 * @brief Task notification bits sent to main_task
 */
#define MSC_NOTIFY_CONNECTED     BIT0
#define MSC_NOTIFY_DISCONNECTED  BIT1

/**
 * @brief MSC configuration structure
 */
typedef struct {
    TaskHandle_t main_task;              /**< Main task handle for xTaskNotify (connect/disconnect) */
    msc_event_cb_t event_cb;             /**< State change notification callback (NULL = none) */
    void *event_cb_ctx;                  /**< User context for event_cb */
} msc_config_t;

/**
 * @brief Initialize MSC module
 *
 * Installs USB host library, creates USB lib task, installs MSC class driver,
 * and creates the FSM task. The FSM starts in WAIT_MSC state.
 *
 * @param config Pointer to MSC configuration (must not be NULL)
 * @param[out] handle Output handle to initialized MSC instance
 * @return
 *     - ESP_OK: MSC initialized successfully
 *     - ESP_ERR_INVALID_ARG: config or handle is NULL
 *     - ESP_ERR_NO_MEM: Failed to allocate memory
 */
esp_err_t msc_init(const msc_config_t *config, msc_handle_t *handle);

/**
 * @brief Deinitialize MSC module
 *
 * Stops tasks, uninstalls drivers, and frees resources.
 *
 * @param handle MSC handle returned from msc_init()
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_ARG if handle is NULL
 */
esp_err_t msc_deinit(msc_handle_t handle);

/**
 * @brief Forward an input event to the MSC FSM task
 *
 * Posts the event to the FSM's internal queue. Non-blocking; drops event if queue is full.
 * Call this from input_scanner callback context when app is in MSC mode.
 *
 * @param handle MSC handle returned from msc_init()
 * @param btn_num Button number (0=encoder switch, 1-12=matrix buttons)
 * @param event Input event type
 */
void msc_on_input_event(msc_handle_t handle, uint8_t btn_num, input_event_type_t event);

/**
 * @brief Print MSC module status information to console
 *
 * @param handle MSC handle (NULL-safe)
 * @param output_type Output verbosity level (COMPACT, NORMAL, VERBOSE)
 */
void msc_print_status(msc_handle_t handle, status_output_type_t output_type);
