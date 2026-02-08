/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file console.h
 * @brief ESP-IDF Console command handler
 *
 * The console registers all commands at init time. Commands that depend on
 * optional modules (player, mapper, etc.) check for NULL handles and return
 * an error message when the module is unavailable.
 */

#pragma once

#include "esp_err.h"

// Forward declaration - defined in app_state.h
struct app_state_s;

/**
 * @brief Initialize and start the ESP console with all commands
 *
 * All commands are registered unconditionally. Commands that require optional
 * modules (player, mapper, MSC, etc.) gracefully handle NULL handles by
 * printing an error message and returning.
 *
 * @param[in] app_state Pointer to application state (kept as reference)
 * @return
 *    - ESP_OK: Success
 *    - Others: Fail
 */
esp_err_t console_init(const struct app_state_s *app_state);

/**
 * @brief Deinitialize the console handler
 *
 * @return
 *    - ESP_OK: Success
 *    - Others: Fail
 */
esp_err_t console_deinit(void);
