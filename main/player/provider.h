/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esp_err.h"
#include "soundboard.h"

/**
 * @brief Audio provider interface
 *
 * Abstraction over different audio sources (WAV decoder, PSRAM cache)
 * Provides uniform interface for playback engine with transparent source selection.
 *
 * Thread-safety: All public APIs are thread-safe.
 * Memory: Cache buffers allocated in PSRAM, metadata in internal RAM.
 */


/**
 * @brief Audio stream information
 *
 * Contains format parameters for PCM audio data.
 */
typedef struct {
    uint32_t frame_rate;      /**< Frame rate in Hz (e.g., 44100, 48000) */
    uint16_t channels;        /**< Number of channels (1=mono, 2=stereo) */
    uint16_t bit_depth;       /**< Bits per sample (16 only supported) */
    uint32_t total_frames;    /**< Total sample frames (1 frame = 1 value per channel) */
} audio_info_t;

/**
 * @brief Audio provider handle (opaque)
 *
 * Manages cache entries and provides audio stream access.
 */
typedef struct audio_provider_s* audio_provider_handle_t;

/**
 * @brief Stream handle (opaque)
 *
 * Represents an open audio stream (WAV file or cache).
 */
typedef struct audio_stream_s* audio_stream_handle_t;

/**
 * @brief Audio provider configuration
 */
typedef struct {
    size_t cache_size_kb;     /**< Maximum cache size in KB (requires PSRAM) */
} audio_provider_config_t;

/**
 * @brief Create audio provider from configuration
 *
 * @param config Provider configuration
 * @param[out] provider Provider handle (on success)
 * @return esp_err_t
 */
esp_err_t audio_provider_init(audio_provider_config_t *config, audio_provider_handle_t *provider);

/**
 * @brief Destroy audio provider and free resources
 *
 * @param provider Provider handle (NULL-safe)
 */
void audio_provider_deinit(audio_provider_handle_t provider);


/**
 * @brief Open an audio stream from a WAV file
 *
 * Creates a stream backed by PSRAM cache (on hit) or direct file I/O (on miss).
 * The caller owns the returned stream handle and must close it with
 * audio_provider_close_stream() after use.
 *
 * @param provider Provider handle
 * @param filename Path to WAV file (e.g., "/sdcard/laser.wav")
 * @param[out] stream Stream handle (on success)
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_ARG if any parameter is NULL
 *     - ESP_ERR_NOT_FOUND if file cannot be opened
 *     - ESP_ERR_NO_MEM if stream allocation fails
 *     - ESP_FAIL if WAV header parsing fails
 */
esp_err_t audio_provider_open_stream(audio_provider_handle_t provider,
                              const char *filename,
                              audio_stream_handle_t *stream);


/**
 * @brief Read PCM samples from a stream
 *
 * Fills the buffer with PCM data from cache or file. On EOF, returns ESP_OK
 * with *samples_read set to 0.
 *
 * @param stream Stream handle
 * @param[out] buffer Output buffer for 16-bit PCM samples
 * @param buffer_samples Maximum number of samples to read
 * @param[out] samples_read Actual number of samples read (0 at EOF)
 * @return
 *     - ESP_OK on success (including EOF)
 *     - ESP_ERR_INVALID_ARG if any parameter is NULL
 *     - ESP_ERR_INVALID_STATE if the stream encountered a previous error
 *     - ESP_FAIL on file read error
 */
esp_err_t audio_provider_read_stream(audio_stream_handle_t stream,
                               int16_t *buffer,
                               size_t buffer_samples,
                               size_t *samples_read);

/**
 * @brief Close an audio stream and release its resources
 *
 * For cache-backed streams, decrements the cache entry reference count.
 * For file-backed streams, closes the file handle and resumes the preload
 * task if no other file streams remain active.
 *
 * @param stream Stream handle (NULL-safe, returns ESP_OK)
 * @return ESP_OK always
 */
esp_err_t audio_provider_close_stream(audio_stream_handle_t stream);


/**
 * @brief Get audio info from stream
 *
 * @param stream Stream handle
 * @return Pointer to audio_info_t or NULL if invalid (valid for stream lifetime)
 */
const audio_info_t* audio_provider_get_stream_info(audio_stream_handle_t stream);

/**
 * @brief Get stream playback progress
 *
 * Returns the current playback position as a uint16_t (0 = start, UINT16_MAX = end).
 * Works for both cache-backed and file-backed streams.
 *
 * @param stream Stream handle (NULL-safe, returns 0)
 * @return Progress value (0 to UINT16_MAX)
 */
uint16_t audio_provider_get_stream_progress(audio_stream_handle_t stream);

/**
 * @brief Queue audio file for background preloading
 *
 * Queues the file to be loaded into PSRAM cache by a background task.
 * Non-blocking - returns immediately after queueing.
 * Evicts LRU entries if cache is full.
 *
 * @param provider Provider handle
 * @param filename Path to WAV file
 * @return
 *     - ESP_OK if queued successfully
 *     - ESP_ERR_INVALID_ARG if provider or filename is NULL
 *     - ESP_ERR_INVALID_STATE if preload queue not initialized
 *     - ESP_ERR_NO_MEM if preload queue is full (request dropped)
 */
esp_err_t audio_provider_preload(audio_provider_handle_t provider, const char *filename);

/**
 * @brief Flush the preload queue
 *
 * Discards all pending preload requests. Does not affect files already cached
 * or currently being loaded. Useful when switching pages to avoid loading
 * files from the previous page.
 *
 * @param provider Provider handle
 */
void audio_provider_flush_preload_queue(audio_provider_handle_t provider);

/**
 * @brief Print audio provider status information to console
 *
 * @param provider Provider handle (NULL-safe)
 * @param output_type Output verbosity level (COMPACT, NORMAL, VERBOSE)
 */
void audio_provider_print_status(audio_provider_handle_t provider, status_output_type_t output_type);
