/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esp_err.h"
#include "input_scanner.h"
#include "player.h"
#include "soundboard.h"

/**
 * @brief Maximum page identifier length (including NUL terminator)
 */
#define PAGE_ID_MAX_LEN         32

/**
 * @brief Encoder mode for short press toggle
 */
typedef enum {
    ENCODER_MODE_VOLUME,        /**< Encoder controls volume (default) */
    ENCODER_MODE_PAGE,          /**< Encoder controls page selection */
} encoder_mode_t;

/**
 * @brief Action types that can be triggered by input events
 */
typedef enum {
    ACTION_TYPE_STOP,           /**< Stop playback immediately */
    ACTION_TYPE_PLAY,           /**< Play once until EOF */
    ACTION_TYPE_PLAY_CUT,       /**< Play once, stop on RELEASE */
    ACTION_TYPE_PLAY_LOCK,      /**< Press=play_cut, long_press=lock, second press=stop */
} action_type_t;

/**
 * @brief Action definition with type and parameters
 */
typedef struct {
    action_type_t type;
    union {
        struct {
            char filename[SOUNDBOARD_MAX_PATH_LEN];
        } play;
    } params;
} action_t;

/* ============================================================================
 * Mapper Event Callback System
 * ============================================================================ */

/**
 * @brief Mapper event types for unified callback
 */
typedef enum {

    MAPPER_EVENT_LOADED,                /**< Mappings loaded (fired once after init) */
    MAPPER_EVENT_ACTION_EXECUTED,       /**< An action was executed */
    MAPPER_EVENT_ENCODER_MODE_CHANGED,  /**< Encoder mode toggled (volume/page) */
    MAPPER_EVENT_PAGE_CHANGED,          /**< Current page changed */
} mapper_event_type_t;

/**
 * @brief Mapper event data structure
 */
typedef struct {
    mapper_event_type_t type;           /**< Event type */
    union {

        struct {
            uint8_t page_count;         /**< Total number of pages */
            const char *initial_page_id; /**< Initial page identifier */
        } loaded;

        struct {
            uint8_t button_number;      /**< Button that triggered action (1-12 matrix, 0 encoder) */
            input_event_type_t event;   /**< Input event that triggered action */
            const action_t *action;     /**< Action that was executed */
        } action_executed;

        struct {
            encoder_mode_t mode;        /**< New encoder mode */
        } encoder_mode_changed;

        struct {
            const char *page_id;        /**< New page string identifier */
            uint8_t page_number;        /**< 1-based page number */
            uint8_t page_count;         /**< Total number of pages (for display) */
        } page_changed;
    };
} mapper_event_t;

/**
 * @brief Unified mapper event callback
 *
 * Called when mapper state changes or actions are executed.
 * Replaces the separate status_cb and encoder_mode_cb callbacks.
 *
 * @param event Event data with type and payload
 * @param user_ctx User context provided during init
 */
typedef void (*mapper_event_cb_t)(const mapper_event_t *event, void *user_ctx);

/**
 * @brief Opaque mapper handle
 */
typedef struct mapper_s *mapper_handle_t;

/**
 * @brief Mapper configuration structure
 */
typedef struct {
    const char *spiffs_root;            /**< SPIFFS mount point (e.g., "/spiffs"), NULL to skip */
    const char *spiffs_mappings_file;   /**< Mappings filename in SPIFFS (e.g., "mappings.csv"), NULL to skip */
    const char *sdcard_root;            /**< SD card mount point (e.g., "/sdcard"), NULL to skip */
    const char *sdcard_mappings_file;   /**< Mappings filename on SD card (e.g., "mappings.csv"), NULL to skip */
    player_handle_t player;             /**< Player handle for executing playback actions */
    mapper_event_cb_t event_cb;         /**< Optional unified event callback (can be NULL) */
    void *event_cb_ctx;                 /**< User context passed to event callback */
} mapper_config_t;

/**
 * @brief Initialize synchronous mapper
 *
 * Loads event-to-action mappings from CSV files and stores player handle
 * for executing actions. Does NOT create any tasks or queues - all event
 * processing is synchronous (called directly from input_scanner callbacks).
 *
 * This function:
 * 1. Loads SPIFFS mappings first (any page, firmware defaults)
 * 2. Loads SD card mappings (overwrites existing on same page/button/event, warns on conflict)
 * 3. Stores player handle for action execution
 * 4. Returns mapper handle for event processing
 *
 * Mappings loading order:
 * - SPIFFS mappings loaded first (firmware defaults)
 * - SD card mappings loaded second (user overrides)
 * - SD card mappings overwrite SPIFFS mappings on conflict (same page/button/event)
 *
 * Supported CSV format (mappings.csv) with optional fields:
 * - Lines starting with # are comments
 * - Format: <page_id>,<button>,<event>,<action>[,param1,param2,...]
 * - page_id is a string identifier (e.g., "default", "fx", "music")
 * - Trailing columns are optional (no trailing commas needed)
 * - Parameters depend on action type:
 *
 *   Action types and parameters:
 *   - stop: No parameters
 *   - play: file (play once until EOF)
 *   - play_cut: file (play once, stop on RELEASE)
 *   - play_lock: file (press=play_cut, long_press=lock, second press=stop)
 *
 * Encoder behavior:
 * - Short press on encoder switch toggles between VOLUME and PAGE modes
 * - In VOLUME mode: CW/CCW rotation adjusts volume
 * - In PAGE mode: CW/CCW rotation changes current page
 * - event_cb is called with MAPPER_EVENT_ENCODER_MODE_CHANGED when mode changes
 * - event_cb is called with MAPPER_EVENT_PAGE_CHANGED when page changes
 *
 * @param config Pointer to mapper configuration structure
 * @param[out] out_handle Pointer to store mapper handle
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_ARG if config or out_handle is NULL, or if player is NULL
 *     - ESP_ERR_NOT_FOUND if both mappings files not found
 *     - ESP_ERR_NO_MEM if allocation fails
 *     - ESP_ERR_INVALID_STATE if parse error occurs
 */
esp_err_t mapper_init(const mapper_config_t *config, mapper_handle_t *out_handle);

/**
 * @brief Handle input event synchronously
 *
 * Processes input event immediately in caller's context (no queue/task).
 * This function is designed to be called directly from input_scanner callbacks.
 *
 * Processing flow:
 * 1. Encoder switch short press: Toggle between VOLUME and PAGE modes
 * 2. Encoder rotation:
 *    - In VOLUME mode: CW/CCW adjusts volume
 *    - In PAGE mode: CW/CCW changes current page
 * 3. Matrix button events:
 *    - Lookup mapping rule for (current_page, button_number, event)
 *    - If found, execute action (play, volume, etc.)
 *
 * @param handle Mapper handle
 * @param button_number Button number (1-12 for matrix, 0 for encoder)
 * @param event Input event type
 *
 * @note Called from input_scanner task context (not ISR)
 * @note Thread safety: Relies on input_scanner serialization
 */
void mapper_on_input_event(mapper_handle_t handle,
                        uint8_t button_number,
                        input_event_type_t event);

/**
 * @brief Deinitialize mapper
 *
 * Frees all allocated memory including the linked list data structures.
 *
 * @param handle Mapper handle
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_ARG if handle is NULL
 */
esp_err_t mapper_deinit(mapper_handle_t handle);

/**
 * @brief Print all loaded mappings to stdout
 *
 * Iterates over all pages and mappings, printing each mapping on one line.
 * Useful for debugging and verifying loaded configuration.
 *
 * @param handle Mapper handle
 */
void mapper_print_mappings(mapper_handle_t handle);

/**
 * @brief Validate a mappings CSV file without loading into mapper
 *
 * Parses each line and validates syntax (page_id, button range, event type,
 * action type, parameter counts). Optionally checks that referenced audio
 * files exist on disk via stat().
 *
 * This function does NOT require a mapper handle or player handle.
 * Intended for pre-validation of USB content before copying to SD card.
 *
 * @param filepath     Full path to the mappings CSV file
 * @param root         Root path for resolving relative filenames (e.g., "/msc/soundboard")
 * @param check_files  If true, verify each referenced audio file exists
 * @return
 *     - ESP_OK: All lines valid (and all files found, if check_files)
 *     - ESP_ERR_NOT_FOUND: File not found, or no valid mappings in file
 *     - ESP_ERR_INVALID_STATE: Parse error on one or more lines
 */
esp_err_t mapper_validate_file(const char *filepath, const char *root, bool check_files);

/**
 * @brief Print mapper status information to console
 *
 * @param handle Mapper handle (NULL-safe)
 * @param output_type Output verbosity level (COMPACT, NORMAL, VERBOSE)
 */
void mapper_print_status(mapper_handle_t handle, status_output_type_t output_type);
