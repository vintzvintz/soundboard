/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file app_state.h
 * @brief Private application state definition shared between main.c and console.c
 *
 * This header is NOT part of the public module API. Only main.c and console.c
 * should include it.
 */

#pragma once

#include "soundboard.h"
#include "sdmmc_cmd.h" // IWYU pragma: keep
#include "display.h"
#include "player.h"
#include "mapper.h"
#include "input_scanner.h"
#include "msc.h"

/**
 * @brief Configuration source tracking
 */
typedef enum {
    CONFIG_SOURCE_NONE = 0,      /**< No configuration loaded */
    CONFIG_SOURCE_FIRMWARE,      /**< Configuration from internal flash (SPIFFS) */
    CONFIG_SOURCE_SDCARD,        /**< Configuration from SD card */
} config_source_t;

/**
 * @brief Application-level global state
 */
typedef struct app_state_s {
    application_mode_t mode;
    config_source_t config_source;
    sdmmc_card_t *sdcard;
    display_handle_t oled;
    player_handle_t player;
    mapper_handle_t mapper;
    input_scanner_handle_t input_scanner;
    msc_handle_t msc;
} app_state_t;

/**
 * @brief Print application-level status information
 *
 * @param output_type Output verbosity level
 */
void app_print_status(status_output_type_t output_type);
