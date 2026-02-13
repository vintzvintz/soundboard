/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "esp_err.h"
#include "sdkconfig.h"
#include "soundboard.h"

// Player configuration from Kconfig
#define CACHE_SIZE_KB CONFIG_SOUNDBOARD_PLAYER_CACHE_SIZE_KB

// Forward declarations
typedef struct player_s* player_handle_t;

/**
 * @brief Player event types for callback
 */
typedef enum {
    PLAYER_EVENT_READY,             /**< Player initialized, carries initial volume_index */
    PLAYER_EVENT_STARTED,           /**< Playback started */
    PLAYER_EVENT_STOPPED,           /**< Playback stopped (finished or interrupted) */
    PLAYER_EVENT_PROGRESS,          /**< Playback progress update, carries filename + progress */
    PLAYER_EVENT_VOLUME_CHANGED,    /**< Volume level changed */
    PLAYER_EVENT_ERROR,             /**< Playback error occurred */
} player_event_name_t;


typedef struct {
    player_event_name_t name;
    union {
        const char *filename;   // For PLAYER_EVENT_STARTED, PLAYER_EVENT_STOPPED
        struct {
            const char *filename;   // Current file being played
            uint16_t progress;      // 0 = start, UINT16_MAX = end
        } playback;             // For PLAYER_EVENT_PROGRESS
        int volume_index;       // For PLAYER_EVENT_READY, PLAYER_EVENT_VOLUME_CHANGED
        esp_err_t error_code;   // For PLAYER_EVENT_ERROR
    };
} player_event_data_t;

/**
 * @brief Player event callback function type
 *
 * Called when player state changes or events occur.
 *
 * @param event_data Event type with data payload
 * @param user_ctx User context pointer passed during registration
 */
typedef void (*player_event_callback_t)(const player_event_data_t *event_data, void *user_ctx);

/**
 * @brief Player module configuration structure
 */
typedef struct {
    size_t cache_size_kb;                /**< Cache size in KB (0 = disabled, >0 = enabled, requires PSRAM) */
    player_event_callback_t event_cb;    /**< to notify player state changes to other modules (e.g. display)*/
    void *event_cb_ctx;                  /**< User context for player events callback */
} player_config_t;

/**
 * @brief Default player configuration initializer
 */
#define PLAYER_CONFIG_DEFAULT() {       \
    .cache_size_kb = CACHE_SIZE_KB,     \
    .event_cb = NULL,                   \
    .event_cb_ctx = NULL,               \
}

/**
 * @brief Initialize player module
 *
 * Initializes the I2S audio output, audio provider subsystem,
 * creates internal command queue, and launches the player task.
 *
 * @param config Pointer to player configuration structure (must not be NULL)
 * @param[out] player Output handle to initialized player instance (must not be NULL)
 * @return
 *     - ESP_OK: Player initialized successfully
 *     - ESP_ERR_INVALID_ARG: config or player is NULL
 *     - ESP_ERR_NO_MEM: Failed to allocate memory for:
 *         - Player state structure
 *         - PCM buffer
 *         - Command queue
 *         - Player task
 *     - ESP_FAIL: I2S or audio_provider init errors
 */
esp_err_t player_init(const player_config_t *config, player_handle_t *player);

/**
 * @brief Deinitialize player module
 *
 * Stops player task and cleans up resources. Frees the player handle.
 * Note: Does not free the cache - cache must be deinitialized separately.
 *
 * @param player Player handle returned from player_init()
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_ARG if player is NULL
 */
esp_err_t player_deinit(player_handle_t player);

/**
 * @brief Play audio file (async, queued request)
 *
 * Queues a play request to the player task. Plays the file once until EOF.
 * If already playing, stops the current stream and starts the new one.
 *
 * @param player Player handle returned from player_init()
 * @param filename Path to audio file (e.g., "/sdcard/sound.wav")
 * @return
 *     - ESP_OK if request queued successfully
 *     - ESP_ERR_INVALID_STATE if player not initialized
 *     - ESP_ERR_INVALID_ARG if filename is NULL or player is NULL
 *     - ESP_FAIL if queue is full
 */
esp_err_t player_play(player_handle_t player, const char *filename);

/**
 * @brief Stop audio playback (async, queued request)
 *
 * Queues a stop request to the player task.
 *
 * @param player Player handle returned from player_init()
 * @param interrupt_now If true, stop immediately. If false, finish current sample then stop.
 * @return
 *     - ESP_OK if request queued successfully
 *     - ESP_ERR_INVALID_STATE if player not initialized
 *     - ESP_ERR_INVALID_ARG if player is NULL
 *     - ESP_FAIL if queue is full
 */
esp_err_t player_stop(player_handle_t player, bool interrupt_now);


/**
 * @brief Get current volume index
 *
 * Returns the current software volume index immediately without queueing.
 * Safe to call from any context.
 *
 * @param player Player handle returned from player_init()
 * @param[out] volume_index Pointer to store current volume index (0 to 31)
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_ARG if player or volume_index is NULL
 */
esp_err_t player_volume_get(player_handle_t player, int *volume_index);

/**
 * @brief Set volume level
 *
 * Sets the software volume immediately without queueing.
 * Fires PLAYER_EVENT_VOLUME_CHANGED callback.
 *
 * @param player Player handle returned from player_init()
 * @param index Volume index (0=mute, 31=max)
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_ARG if player is NULL
 */
esp_err_t player_volume_set(player_handle_t player, int8_t index);

/**
 * @brief Adjust volume by step
 *
 * Adjusts the software volume immediately without queueing.
 * Fires PLAYER_EVENT_VOLUME_CHANGED callback.
 *
 * @param player Player handle returned from player_init()
 * @param step Number of steps to adjust (positive=louder, negative=quieter)
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_ARG if player is NULL
 */
esp_err_t player_volume_adjust(player_handle_t player, int8_t step);

/**
 * @brief Get the number of volume levels
 *
 * @return Number of discrete volume levels (32)
 */
int player_volume_get_max_index(void);

/**
 * @brief Queue audio file for background preloading
 *
 * Queues the file to be loaded into PSRAM cache by a background task.
 * Non-blocking - returns immediately after queueing.
 * Call this when switching pages to preload files for fast playback.
 *
 * @param player Player handle returned from player_init()
 * @param filename Path to audio file (e.g., "/sdcard/sound.wav")
 * @return
 *     - ESP_OK if queued successfully
 *     - ESP_ERR_INVALID_ARG if player or filename is NULL
 *     - ESP_ERR_NO_MEM if preload queue is full (request dropped)
 */
esp_err_t player_preload(player_handle_t player, const char *filename);

/**
 * @brief Flush the preload queue
 *
 * Discards all pending preload requests without affecting cached files.
 * Call before preloading a new page to avoid stale requests.
 *
 * @param player Player handle returned from player_init()
 */
void player_flush_preload(player_handle_t player);

/**
 * @brief Print player status information to console
 *
 * Outputs player state and audio provider/cache status.
 * Internally calls audio_provider_print_status().
 *
 * @param player Player handle (NULL-safe)
 * @param output_type Output verbosity level (COMPACT, NORMAL, VERBOSE)
 */
void player_print_status(player_handle_t player, status_output_type_t output_type);
