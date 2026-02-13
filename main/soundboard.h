/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file soundboard.h
 * @brief Application-level state and type definitions
 */

#pragma once

#include "esp_vfs.h"  // IWYU pragma: keep (ESP_VFS_PATH_MAX)
#include "ff.h"       // IWYU pragma: keep (FF_LFN_BUF)

// Storage mountpoint defines (used in main.c, console.c, msc.c)
#define SPIFFS_MOUNT_POINT "/spiffs"
#define SDCARD_MOUNT_POINT "/sdcard"
#define MSC_MOUNT_POINT    "/msc"

// Maximum full path length: mount point + "/" + longest LFN + NUL
#define SOUNDBOARD_MAX_PATH_LEN (ESP_VFS_PATH_MAX + 1 + FF_LFN_BUF + 1)

/**
 * @brief Application mode enumeration
 */
typedef enum {
    APP_MODE_NONE = 0,
    APP_MODE_PLAYER,
    APP_MODE_MSC,
} application_mode_t;

/**
 * @brief Output type for status printing functions
 */
typedef enum {
    STATUS_OUTPUT_COMPACT,   /**< Single-line summary, suitable for startup */
    STATUS_OUTPUT_NORMAL,    /**< Standard multi-line output */
    STATUS_OUTPUT_VERBOSE    /**< Detailed output with all available metrics */
} status_output_type_t;


// Full paths to mappings files (defined in main.c, depends on sdkconfig)
extern const char *const SPIFFS_MAPPINGS_PATH;
extern const char *const SDCARD_MAPPINGS_PATH;
