/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file persistent_volume.h
 * @brief NVS-backed persistent volume storage
 *
 * Stores volume index in NVS for persistence across reboots.
 * Uses deferred saves with a 10-second timer to minimize NVS wear and avoid
 * latency during rapid volume adjustments.
 */

#pragma once

#include "esp_err.h"
#include "soundboard.h"

/**
 * @brief Default volume index (used when no saved value exists)
 */
#define PERSISTENT_VOLUME_DEFAULT_INDEX 16   // 50% volume (mid-range of 0-31)

/**
 * @brief Delay before saving volume to NVS (milliseconds)
 *
 * After each volume change, the save is deferred by this amount.
 * Rapid changes reset the timer, resulting in a single NVS write
 * after the volume settles.
 */
#define PERSISTENT_VOLUME_SAVE_DELAY_MS 10000  // 10 seconds

/**
 * @brief Initialize persistent volume module
 *
 * Creates the deferred save timer. Must be called before load/save functions.
 * Safe to call multiple times (subsequent calls are no-ops).
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_NO_MEM if timer creation fails
 */
esp_err_t persistent_volume_init(void);

/**
 * @brief Load volume index from NVS
 *
 * Attempts to load previously saved volume index. If no saved value
 * exists (first boot or NVS erased), returns default without error.
 *
 * @param[out] index Volume index (0 to VOLUME_LEVELS-1)
 * @return
 *     - ESP_OK on success (either loaded or default used)
 *     - ESP_ERR_INVALID_ARG if index is NULL
 *     - ESP_FAIL on NVS access error
 */
esp_err_t persistent_volume_load(uint16_t *index);

/**
 * @brief Queue deferred volume save to NVS
 *
 * Schedules a volume save after PERSISTENT_VOLUME_SAVE_DELAY_MS.
 * If called again before the timer fires, the timer is reset and
 * the new value is used. This coalesces rapid changes into a
 * single NVS write.
 *
 * @param index Volume index (0 to VOLUME_LEVELS-1)
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_STATE if module not initialized
 */
esp_err_t persistent_volume_save_deferred(uint16_t index);

/**
 * @brief Print persistent volume status information to console
 *
 * @param output_type Output verbosity level (COMPACT, NORMAL, VERBOSE)
 */
void persistent_volume_print_status(status_output_type_t output_type);
