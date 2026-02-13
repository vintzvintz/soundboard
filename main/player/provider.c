/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "soundboard.h"
#include "provider.h"

#ifdef CONFIG_SOUNDBOARD_IO_STATS_ENABLE
    #include "benchmark.h"
#endif

static const char *TAG_PROVIDER = "audio_provider";
static const char *TAG_CACHE = "audio_cache";

#define CACHE_ENTRY_COUNT 64
#define WAV_CHUNK_SIZE 4096  // WAV read chunk size

// Maximum cacheable file size: files larger than half of total PSRAM are not cached
// (they would cause excessive eviction and fragmentation)
#define CACHE_ITEM_MAXSIZE (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 2)

// Preload task configuration
#define PRELOAD_TASK_PRIORITY    1   // Lower than player (2)
#define PRELOAD_TASK_STACK_SIZE  4096
#define PRELOAD_QUEUE_LENGTH     16


/*
 * Audio provider module provides an abstraction layer of a PCM audio data source to the player module
 *
 * Two possible underlying sources:
 * 1. WAV file decoder:
 *    - Parse WAV headers to get audio settings (rate, channels, etc.) and file size
 *    - stream_read() fetches data chunks from SD card (FAT) or SPIFFS and copies to caller's buffer
 *
 * 2. PSRAM cache:
 *    - stream_read() copies data chunks from cache entry to caller's buffer
 *    - ~100-500x faster than file streaming
 */

/**
 * @brief Stream type discriminator
 */
typedef enum {
    STREAM_TYPE_WAV_FILE,    // Streaming from filesystem via decoder
    STREAM_TYPE_CACHE        // Streaming from PSRAM cache buffer
} stream_type_t;


/**
 * @brief Cache entry structure
 *
 * Stores pre-loaded PCM data in PSRAM for fast playback.
 * Data size in bytes = frame_count * info.channels * (info.bit_depth / 8)
 */
typedef struct cache_entry_s {
    char *filename;                  // Heap-allocated filename string
    size_t frame_count;              // Number of audio frames
    audio_info_t info;               // Audio format parameters
    size_t buf_size;                 // Total buffer size in bytes
    size_t total_samples;            // Pre-calculated: frame_count * channels (optimization)
    int16_t *buffer;                 // PSRAM-allocated PCM buffer (16-bit only)

    // Reference counting for safe multi-stream support
    uint8_t ref_count;               // Number of active streams using this entry

    // LRU tracking
    uint32_t last_access_tick;       // FreeRTOS tick count of last access

    // Thread safety
    SemaphoreHandle_t mutex;         // Protects ref_count and last_access_tick
} cache_entry_t;

/**
 * @brief Preload queue item
 */
typedef struct {
    char filename[SOUNDBOARD_MAX_PATH_LEN];
} preload_item_t;

/**
 * @brief Audio provider internal state
 */
typedef struct audio_provider_s {
    // Cache management
    cache_entry_t cache[CACHE_ENTRY_COUNT];
    size_t max_cache_bytes;                   // Total cache size limit (from config)
    size_t used_cache_bytes;                  // Current cache usage

    // Thread safety
    SemaphoreHandle_t cache_mutex;            // Protects cache array operations

    // Preload task
    TaskHandle_t preload_task_handle;         // Background preload task
    QueueHandle_t preload_queue;              // Queue for preload requests
    bool preload_task_running;                // Flag to signal task shutdown

    // Preload pause control (pause during active playback to avoid SD card contention)
    volatile int active_stream_count;         // Number of open WAV file streams (atomically updated)

    // Configuration
    bool initialized;
} audio_provider_state_t;

/**
 * @brief Audio stream handle (concrete implementation)
 */
struct audio_stream_s {
    // Stream metadata
    char filename[SOUNDBOARD_MAX_PATH_LEN];  // Copied on open for safety
    audio_info_t info;                   // Audio format parameters
    stream_type_t type;                  // WAV_FILE or CACHE

    // State flags
    bool eof_reached;                    // End of stream indicator
    bool error_state;                    // Error flag

    // Parent provider reference (for cache updates)
    audio_provider_handle_t provider;

    // Type-specific state (union for memory efficiency)
    union {
        // WAV file stream state
        struct {
            FILE *fp;                    // File handle
            uint32_t data_offset;        // Offset to PCM data in file
            uint32_t data_size;          // Size of PCM data in bytes
            uint32_t bytes_read;         // Bytes read so far
        } wav;

        // Cache stream state
        struct {
            cache_entry_t *entry;        // Pointer to cache entry (not owned)
            size_t position;             // Current read position (int16_t samples)
        } cache;
    };
};

// ============================================================================
// WAV Format Parsing
// ============================================================================

/**
 * @brief Parse WAV file header (robust chunk-based parser)
 *
 * Handles arbitrary chunk ordering and metadata chunks.
 */
static esp_err_t parse_wav_header(FILE *fp, audio_info_t *info, uint32_t *data_offset, uint32_t *data_size)
{
    char chunk_id[4];
    uint32_t chunk_size;

    // Read RIFF header
    if (fread(chunk_id, 1, 4, fp) != 4) {
        ESP_LOGE(TAG_PROVIDER, "Failed to read RIFF header");
        return ESP_FAIL;
    }

    if (memcmp(chunk_id, "RIFF", 4) != 0) {
        ESP_LOGE(TAG_PROVIDER, "Not a RIFF file");
        return ESP_FAIL;
    }

    // Skip file size (4 bytes)
    fseek(fp, 4, SEEK_CUR);

    // Read WAVE identifier
    if (fread(chunk_id, 1, 4, fp) != 4) {
        ESP_LOGE(TAG_PROVIDER, "Failed to read WAVE identifier");
        return ESP_FAIL;
    }

    if (memcmp(chunk_id, "WAVE", 4) != 0) {
        ESP_LOGE(TAG_PROVIDER, "Not a WAVE file");
        return ESP_FAIL;
    }

    // Parse chunks until we find both fmt and data
    bool found_fmt = false;
    bool found_data = false;

    while (!feof(fp) && (!found_fmt || !found_data)) {
        // Read chunk ID and size
        if (fread(chunk_id, 1, 4, fp) != 4) {
            break;
        }
        if (fread(&chunk_size, 4, 1, fp) != 1) {
            break;
        }

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            // Format chunk
            uint16_t audio_format;
            uint16_t num_channels;
            uint32_t frame_rate;
            uint16_t bits_per_sample;

            if (fread(&audio_format, 2, 1, fp) != 1) {
                break;
            }

            if (audio_format != 1) {
                ESP_LOGE(TAG_PROVIDER, "Only PCM format supported (format=%u)", audio_format);
                return ESP_ERR_NOT_SUPPORTED;
            }

            fread(&num_channels, 2, 1, fp);
            fread(&frame_rate, 4, 1, fp);
            fseek(fp, 6, SEEK_CUR);  // Skip ByteRate and BlockAlign
            fread(&bits_per_sample, 2, 1, fp);

            info->frame_rate = frame_rate;
            info->channels = num_channels;
            info->bit_depth = bits_per_sample;

            // Skip any extra format bytes
            uint32_t bytes_read = 16;
            if (chunk_size > bytes_read) {
                fseek(fp, chunk_size - bytes_read, SEEK_CUR);
            }

            found_fmt = true;
            ESP_LOGD(TAG_PROVIDER, "WAV format: %u Hz, %u channels, %u bits",
                     frame_rate, num_channels, bits_per_sample);

        } else if (memcmp(chunk_id, "data", 4) == 0) {
            // Data chunk
            *data_size = chunk_size;
            *data_offset = ftell(fp);
            found_data = true;

            ESP_LOGD(TAG_PROVIDER, "WAV data: offset=%lu, size=%lu", *data_offset, *data_size);
            break;  // Found data, done parsing

        } else {
            // Unknown chunk - skip it
            ESP_LOGD(TAG_PROVIDER, "Skipping unknown chunk: %.4s (size=%lu)", chunk_id, chunk_size);
            fseek(fp, chunk_size, SEEK_CUR);
        }

        // Handle word alignment padding
        if (chunk_size & 1) {
            fseek(fp, 1, SEEK_CUR);
        }
    }

    if (!found_fmt || !found_data) {
        ESP_LOGE(TAG_PROVIDER, "Invalid WAV file (fmt=%d, data=%d)", found_fmt, found_data);
        return ESP_FAIL;
    }

    // Calculate total samples
    uint32_t bytes_per_sample = (info->bit_depth / 8) * info->channels;
    info->total_frames = *data_size / bytes_per_sample;

    return ESP_OK;
}



// ============================================================================
// Internal Cache Implementation
// ============================================================================

/**
 * @brief Find cache entry by filename
 */
static cache_entry_t* cache_lookup(audio_provider_state_t *provider, const char *filename)
{
    for (int i = 0; i < CACHE_ENTRY_COUNT; i++) {
        cache_entry_t *entry = &provider->cache[i];
        if (entry->filename != NULL && strcmp(entry->filename, filename) == 0) {
            return entry;
        }
    }
    return NULL;
}

/**
 * @brief Find LRU victim for eviction
 *
 * @return Index of oldest unused entry, or -1 if all entries have ref_count > 0
 */
static int cache_find_lru_victim(audio_provider_state_t *provider)
{
    int victim_idx = -1;
    uint32_t oldest_tick = UINT32_MAX;

    for (int i = 0; i < CACHE_ENTRY_COUNT; i++) {
        cache_entry_t *entry = &provider->cache[i];

        // Skip empty slots
        if (entry->filename == NULL) continue;

        // Skip entries currently in use
        if (entry->ref_count > 0) continue;

        // Find oldest accessed entry
        if (entry->last_access_tick < oldest_tick) {
            oldest_tick = entry->last_access_tick;
            victim_idx = i;
        }
    }

    return victim_idx;
}

/**
 * @brief Free cache entry resources
 */
static void free_cache_entry(audio_provider_state_t *provider, cache_entry_t *entry)
{
    if (entry == NULL || entry->filename == NULL) {
        return;
    }

    // Assert ref_count == 0 (defensive programming)
    assert(entry->ref_count == 0 && "Cannot free entry with active streams");

    // Free PSRAM buffer
    if (entry->buffer != NULL) {
        heap_caps_free(entry->buffer);
        entry->buffer = NULL;
    }

    // Update cache size
    provider->used_cache_bytes -= entry->buf_size;

    // Free filename
    free(entry->filename);
    entry->filename = NULL;

    // Note: entry->mutex is NOT deleted here - it's a persistent resource
    // of the cache slot, created once in audio_provider_init() and reused
    // across different cached files

    ESP_LOGD(TAG_CACHE, "Freed cache entry (used: %zu/%zu KB)",
             provider->used_cache_bytes / 1024, provider->max_cache_bytes / 1024);
}

/**
 * @brief Parse WAV file header to determine buffer size needed
 *
 * Opens and closes the file — caller will reopen for PCM data read.
 */
static esp_err_t cache_parse_wav_file(const char *filename, audio_info_t *info,
                                       uint32_t *data_offset, size_t *total_bytes)
{
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        ESP_LOGE(TAG_CACHE, "Failed to open file: %s", filename);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t data_size;
    esp_err_t ret = parse_wav_header(fp, info, data_offset, &data_size);
    fclose(fp);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG_CACHE, "Failed to parse WAV header: %s", filename);
        return ret;
    }

    *total_bytes = (size_t)info->total_frames * info->channels * sizeof(int16_t);
    return ESP_OK;
}

/**
 * @brief Reserve a cache slot and allocate PSRAM buffer, evicting LRU entries until malloc succeeds
 *
 * Combines slot reservation and PSRAM allocation in a single eviction loop.
 * This handles heap fragmentation: even if used_cache_bytes says there's enough
 * room, the actual malloc may fail due to non-contiguous free blocks. By evicting
 * one entry at a time and retrying malloc, we consolidate free memory.
 *
 * Caller must hold NO mutex. This function acquires cache_mutex internally.
 */
static esp_err_t cache_reserve_and_alloc(audio_provider_state_t *provider, size_t total_bytes,
                                          int *out_slot, int16_t **out_buffer)
{
    xSemaphoreTake(provider->cache_mutex, portMAX_DELAY);

    int slot = -1;
    int16_t *buffer = NULL;

    while (true) {
        // Find an empty slot
        if (slot < 0) {
            for (int i = 0; i < CACHE_ENTRY_COUNT; i++) {
                if (provider->cache[i].filename == NULL) {
                    slot = i;
                    break;
                }
            }
        }

        // Check memory budget first
        if (slot >= 0 && provider->used_cache_bytes + total_bytes <= provider->max_cache_bytes) {
            // Budget OK — try actual allocation (may fail due to fragmentation)
            buffer = heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM);
            if (buffer != NULL) {
                break;  // Success: have slot, budget, and buffer
            }
            ESP_LOGD(TAG_CACHE, "PSRAM fragmented, evicting to defragment (%zu KB requested)", total_bytes / 1024);
        }

        // Need to evict: no slot, budget exceeded, or malloc failed despite budget
        int victim = cache_find_lru_victim(provider);
        if (victim < 0) {
            xSemaphoreGive(provider->cache_mutex);
            ESP_LOGW(TAG_CACHE, "Cannot allocate %zu KB: no evictable entries", total_bytes / 1024);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG_CACHE, "Evicting LRU entry: %s (%zu KB)",
                 provider->cache[victim].filename, provider->cache[victim].buf_size / 1024);
        free_cache_entry(provider, &provider->cache[victim]);

        if (slot < 0) {
            slot = victim;
        }
    }

    // Reserve memory budget
    provider->used_cache_bytes += total_bytes;

    xSemaphoreGive(provider->cache_mutex);

    *out_slot = slot;
    *out_buffer = buffer;
    return ESP_OK;
}

/**
 * @brief Read PCM data from file into a pre-allocated buffer
 *
 * Pause-aware: blocks during active playback to yield SD bandwidth.
 */
static esp_err_t cache_read_pcm_data(audio_provider_state_t *provider, const char *filename,
                                      uint32_t data_offset, size_t total_bytes, int16_t *buffer)
{
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        ESP_LOGE(TAG_CACHE, "Failed to reopen file: %s", filename);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(fp, data_offset, SEEK_SET);
    size_t total_read = 0;
    while (total_read < total_bytes) {
        // Block if playback is active (yield SD card bandwidth to player task)
        while (provider->active_stream_count > 0) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }

        size_t to_read = total_bytes - total_read;
        if (to_read > WAV_CHUNK_SIZE) {
            to_read = WAV_CHUNK_SIZE;
        }

#ifdef IO_STATS_ENABLE
        int64_t t0 = benchmark_start();
#endif
        size_t n = fread((uint8_t*)buffer + total_read, 1, to_read, fp);
#ifdef IO_STATS_ENABLE
        benchmark_record(BENCH_CACHE_LOAD, t0, n);
#endif
        if (n == 0) {
            break;
        }
        total_read += n;
    }

    fclose(fp);
#ifdef IO_STATS_ENABLE
    // Log benchmark data
    benchmark_log_and_reset(BENCH_CACHE_LOAD, filename);
#endif

    if (total_read != total_bytes) {
        ESP_LOGE(TAG_CACHE, "Failed to read entire file: read %zu/%zu bytes", total_read, total_bytes);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief Store entry metadata in a previously reserved cache slot
 *
 * Acquires cache_mutex internally. Slot must already be reserved with budget accounted for.
 */
static void cache_store_entry(audio_provider_state_t *provider, int slot, const char *filename,
                               const audio_info_t *info, int16_t *buffer, size_t total_bytes)
{
    xSemaphoreTake(provider->cache_mutex, portMAX_DELAY);

    cache_entry_t *entry = &provider->cache[slot];
    entry->filename = strdup(filename);
    entry->frame_count = info->total_frames;
    memcpy(&entry->info, info, sizeof(audio_info_t));
    entry->buf_size = total_bytes;
    entry->total_samples = (size_t)info->total_frames * info->channels;
    entry->buffer = buffer;
    entry->ref_count = 0;
    entry->last_access_tick = xTaskGetTickCount();

    ESP_LOGI(TAG_CACHE, "Cached file: %s (%zu KB, %u Hz, %u ch) - cache usage: %zu/%zu KB",
             filename, total_bytes / 1024, info->frame_rate, info->channels,
             provider->used_cache_bytes / 1024, provider->max_cache_bytes / 1024);

    xSemaphoreGive(provider->cache_mutex);
}

/**
 * @brief Cache a file into PSRAM
 *
 * Only called from preload task (single writer). Evicts LRU entries before
 * allocating the buffer to minimize peak PSRAM usage.
 */
static esp_err_t cache_file_internal(audio_provider_state_t *provider, const char *filename)
{
    if (!provider || !filename) {
        return ESP_ERR_INVALID_ARG;
    }

    // Check if already cached
    xSemaphoreTake(provider->cache_mutex, portMAX_DELAY);
    if (cache_lookup(provider, filename) != NULL) {
        ESP_LOGD(TAG_CACHE, "File already cached: %s", filename);
        xSemaphoreGive(provider->cache_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(provider->cache_mutex);

    // Parse WAV header to determine buffer size needed
    audio_info_t info;
    uint32_t data_offset;
    size_t total_bytes;
    esp_err_t ret = cache_parse_wav_file(filename, &info, &data_offset, &total_bytes);
    if (ret != ESP_OK) {
        return ret;
    }

    // Reject files too large to cache (avoids excessive eviction and fragmentation)
    if (total_bytes > CACHE_ITEM_MAXSIZE) {
        ESP_LOGW(TAG_CACHE, "File too large to cache: %s (%zu KB, max %zu KB)",
                 filename, total_bytes / 1024, (size_t)CACHE_ITEM_MAXSIZE / 1024);
        return ESP_ERR_NO_MEM;
    }

    // Reserve slot + allocate PSRAM buffer (evicts LRU entries until malloc succeeds)
    int slot;
    int16_t *buffer;
    ret = cache_reserve_and_alloc(provider, total_bytes, &slot, &buffer);
    if (ret != ESP_OK) {
        return ret;
    }

    // Read PCM data into allocated buffer (no mutex — slow I/O)
    ret = cache_read_pcm_data(provider, filename, data_offset, total_bytes, buffer);
    if (ret != ESP_OK) {
        // Unreserve: give back the memory budget and free buffer
        heap_caps_free(buffer);
        xSemaphoreTake(provider->cache_mutex, portMAX_DELAY);
        provider->used_cache_bytes -= total_bytes;
        xSemaphoreGive(provider->cache_mutex);
        return ret;
    }

    // Store entry in reserved slot
    cache_store_entry(provider, slot, filename, &info, buffer, total_bytes);
    return ESP_OK;
}

// ============================================================================
// Preload Task
// ============================================================================

/**
 * @brief Background preload task
 *
 * Processes filenames from preload queue and caches them.
 * Runs at low priority to avoid interfering with playback.
 */
static void cache_task(void *arg)
{
    audio_provider_state_t *provider = (audio_provider_state_t *)arg;
    preload_item_t item;

    ESP_LOGI(TAG_CACHE, "Preload task started");

    while (provider->preload_task_running) {
        // Block waiting for items (100ms timeout to check shutdown flag)
        if (xQueueReceive(provider->preload_queue, &item, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Empty filename is sentinel for shutdown
            if (item.filename[0] == '\0') {
                break;
            }

            ESP_LOGD(TAG_CACHE, "Preloading: %s", item.filename);
            esp_err_t ret = cache_file_internal(provider, item.filename);
            if (ret != ESP_OK && ret != ESP_ERR_NO_MEM) {
                ESP_LOGW(TAG_CACHE, "Failed to preload %s: %s", item.filename, esp_err_to_name(ret));
            }
        }
    }

    ESP_LOGI(TAG_CACHE, "Preload task exiting");
    vTaskDelete(NULL);
}

esp_err_t audio_provider_preload(audio_provider_handle_t provider, const char *filename)
{
    if (!provider || !filename) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!provider->preload_queue) {
        ESP_LOGW(TAG_CACHE, "Preload queue not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Prepare queue item
    preload_item_t item;
    strncpy(item.filename, filename, SOUNDBOARD_MAX_PATH_LEN - 1);
    item.filename[SOUNDBOARD_MAX_PATH_LEN - 1] = '\0';

    // Non-blocking queue send (drop if full)
    if (xQueueSend(provider->preload_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG_CACHE, "Preload queue full, dropping: %s", filename);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGD(TAG_CACHE, "Queued for preload: %s", filename);
    return ESP_OK;
}

void audio_provider_flush_preload_queue(audio_provider_handle_t provider)
{
    if (!provider || !provider->preload_queue) {
        return;
    }

    preload_item_t item;
    int flushed = 0;
    while (xQueueReceive(provider->preload_queue, &item, 0) == pdTRUE) {
        flushed++;
    }

    if (flushed > 0) {
        ESP_LOGI(TAG_CACHE, "Flushed %d items from preload queue", flushed);
    }
}



// ============================================================================
// Public API Implementation
// ============================================================================
esp_err_t audio_provider_open_stream(audio_provider_handle_t provider,
                                      const char *filename,
                                      audio_stream_handle_t *stream)
{
    if (!provider || !filename || !stream) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(provider->cache_mutex, portMAX_DELAY);

    // Check cache first
    cache_entry_t *entry = cache_lookup(provider, filename);

    if (entry != NULL) {
        // CACHE HIT PATH
        ESP_LOGD(TAG_CACHE, "Cache hit: %s", filename);

        // Increment ref_count (thread-safe)
        xSemaphoreTake(entry->mutex, portMAX_DELAY);
        entry->ref_count++;
        entry->last_access_tick = xTaskGetTickCount();
        xSemaphoreGive(entry->mutex);

        xSemaphoreGive(provider->cache_mutex);

        // Allocate stream structure
        audio_stream_handle_t s = heap_caps_calloc(1, sizeof(struct audio_stream_s), MALLOC_CAP_8BIT);
        if (!s) {
            // Rollback ref_count
            xSemaphoreTake(entry->mutex, portMAX_DELAY);
            entry->ref_count--;
            xSemaphoreGive(entry->mutex);
            return ESP_ERR_NO_MEM;
        }

        // Initialize cache stream
        strncpy(s->filename, filename, SOUNDBOARD_MAX_PATH_LEN - 1);
        s->filename[SOUNDBOARD_MAX_PATH_LEN - 1] = '\0';
        memcpy(&s->info, &entry->info, sizeof(audio_info_t));
        s->type = STREAM_TYPE_CACHE;
        s->provider = provider;
        s->cache.entry = entry;
        s->cache.position = 0;
        s->eof_reached = false;
        s->error_state = false;

        *stream = s;
        return ESP_OK;
    }

    // CACHE MISS PATH - Create WAV file stream
    ESP_LOGD(TAG_CACHE, "Cache miss: %s", filename);

    xSemaphoreGive(provider->cache_mutex);

    // Open file
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        return ESP_ERR_NOT_FOUND;
    }

    // Parse WAV header
    audio_info_t info;
    uint32_t data_offset;
    uint32_t data_size;
    esp_err_t ret = parse_wav_header(fp, &info, &data_offset, &data_size);
    if (ret != ESP_OK) {
        fclose(fp);
        return ret;
    }

    // Allocate stream structure
    audio_stream_handle_t s = heap_caps_calloc(1, sizeof(struct audio_stream_s), MALLOC_CAP_8BIT);
    if (!s) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    // Initialize WAV stream
    strncpy(s->filename, filename, SOUNDBOARD_MAX_PATH_LEN - 1);
    s->filename[SOUNDBOARD_MAX_PATH_LEN - 1] = '\0';
    memcpy(&s->info, &info, sizeof(audio_info_t));
    s->type = STREAM_TYPE_WAV_FILE;
    s->provider = provider;
    s->wav.fp = fp;
    s->wav.data_offset = data_offset;
    s->wav.data_size = data_size;
    s->wav.bytes_read = 0;
    s->eof_reached = false;
    s->error_state = false;

    // Seek to data start
    fseek(fp, data_offset, SEEK_SET);

    // Pause preload task while this WAV file stream is active
    // (cache streams don't need SD card, so they don't pause preload)
    // Atomic increment - preload task polls this flag
    __atomic_add_fetch(&provider->active_stream_count, 1, __ATOMIC_SEQ_CST);
    ESP_LOGD(TAG_CACHE, "Preload paused (active streams: %d)", provider->active_stream_count);

    *stream = s;
    return ESP_OK;
}

esp_err_t audio_provider_read_stream(audio_stream_handle_t stream,
                                      int16_t *buffer,
                                      size_t buffer_samples,
                                      size_t *samples_read)
{
    if (!stream || !buffer || !samples_read) {
        return ESP_ERR_INVALID_ARG;
    }

    if (stream->error_state) {
        return ESP_ERR_INVALID_STATE;
    }

    *samples_read = 0;

    if (stream->eof_reached) {
        return ESP_OK;  // Already at end
    }

    if (stream->type == STREAM_TYPE_CACHE) {
        // CACHE READ PATH

        cache_entry_t *entry = stream->cache.entry;
        size_t remaining = entry->total_samples - stream->cache.position;

        if (remaining == 0) {
            stream->eof_reached = true;
            return ESP_OK;
        }

        size_t to_read = (buffer_samples < remaining) ? buffer_samples : remaining;

        // Copy from PSRAM buffer
        size_t bytes_to_read = to_read * sizeof(int16_t);
#ifdef IO_STATS_ENABLE        
        int64_t t0 = benchmark_start();
#endif
        memcpy(buffer, entry->buffer + stream->cache.position, bytes_to_read );
#ifdef IO_STATS_ENABLE
        benchmark_record(BENCH_CACHE_HIT, t0, bytes_to_read);
#endif
        stream->cache.position += to_read;
        *samples_read = to_read;

        // Update LRU timestamp
        xSemaphoreTake(entry->mutex, portMAX_DELAY);
        entry->last_access_tick = xTaskGetTickCount();
        xSemaphoreGive(entry->mutex);

        return ESP_OK;
    }

    // WAV FILE READ PATH

    uint32_t bytes_remaining = stream->wav.data_size - stream->wav.bytes_read;
    if (bytes_remaining == 0) {
        stream->eof_reached = true;
        return ESP_OK;
    }

    size_t bytes_to_read = buffer_samples * sizeof(int16_t);
    if (bytes_to_read > bytes_remaining) {
        bytes_to_read = bytes_remaining;
    }
    if (bytes_to_read > WAV_CHUNK_SIZE) {
        bytes_to_read = WAV_CHUNK_SIZE;
    }

#ifdef IO_STATS_ENABLE
    int64_t t0 = benchmark_start();
#endif
    size_t bytes_read = fread(buffer, 1, bytes_to_read, stream->wav.fp);
#ifdef IO_STATS_ENABLE
    benchmark_record(BENCH_SD_READ, t0, bytes_read);
#endif
    if (bytes_read == 0) {
        if (feof(stream->wav.fp)) {
            stream->eof_reached = true;
            return ESP_OK;
        }
        stream->error_state = true;
        return ESP_FAIL;
    }

    stream->wav.bytes_read += bytes_read;
    *samples_read = bytes_read / sizeof(int16_t);

    return ESP_OK;
}

esp_err_t audio_provider_close_stream(audio_stream_handle_t stream)
{
    if (stream == NULL) {
        return ESP_OK;  // NULL-safe
    }

    if (stream->type == STREAM_TYPE_CACHE) {
        // Decrement cache entry ref_count
        cache_entry_t *entry = stream->cache.entry;

        xSemaphoreTake(entry->mutex, portMAX_DELAY);
        if (entry->ref_count > 0) {
            entry->ref_count--;
        }
        xSemaphoreGive(entry->mutex);

#ifdef IO_STATS_ENABLE
        // Log benchmark data
        benchmark_log_and_reset(BENCH_CACHE_HIT, stream->filename);
#endif
    } else if (stream->type == STREAM_TYPE_WAV_FILE) {
        // Close file handle
        if (stream->wav.fp != NULL) {
            fclose(stream->wav.fp);
            stream->wav.fp = NULL;
        }

#ifdef IO_STATS_ENABLE
        // Log benchmark data
        benchmark_log_and_reset(BENCH_SD_READ, stream->filename);
#endif
        // Resume preload task if this was the last active WAV file stream
        audio_provider_state_t *provider = stream->provider;
        int new_count = __atomic_sub_fetch(&provider->active_stream_count, 1, __ATOMIC_SEQ_CST);
        if (new_count == 0 && provider->preload_task_handle != NULL) {
            // Last stream closed: notify preload task to resume
            xTaskNotifyGive(provider->preload_task_handle);
            ESP_LOGD(TAG_CACHE, "Preload resumed (no active streams)");
        }
    }

    // Free stream structure
    heap_caps_free(stream);

    return ESP_OK;
}

const audio_info_t* audio_provider_get_stream_info(audio_stream_handle_t stream)
{
    if (stream == NULL) {
        return NULL;
    }
    return &stream->info;
}

uint16_t audio_provider_get_stream_progress(audio_stream_handle_t stream)
{
    if (stream == NULL) {
        return 0;
    }

    if (stream->type == STREAM_TYPE_CACHE) {
        size_t total = stream->cache.entry->total_samples;
        if (total == 0) {
            return 0;
        }
        return (uint16_t)((uint64_t)stream->cache.position * UINT16_MAX / total);
    }

    uint32_t total = stream->wav.data_size;
    if (total == 0) {
        return 0;
    }
    return (uint16_t)((uint64_t)stream->wav.bytes_read * UINT16_MAX / total);
}

esp_err_t audio_provider_init(audio_provider_config_t *config, audio_provider_handle_t *provider)
{
    if (!config || !provider) {
        return ESP_ERR_INVALID_ARG;
    }

    // Check PSRAM availability
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_size == 0) {
        ESP_LOGE(TAG_CACHE, "PSRAM not available - cache requires PSRAM");
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Allocate provider state in internal RAM
    audio_provider_state_t *p = heap_caps_calloc(1, sizeof(audio_provider_state_t), MALLOC_CAP_8BIT);
    if (!p) {
        return ESP_ERR_NO_MEM;
    }

    // Initialize cache mutex
    p->cache_mutex = xSemaphoreCreateMutex();
    if (!p->cache_mutex) {
        heap_caps_free(p);
        return ESP_ERR_NO_MEM;
    }

    // Initialize configuration
    p->max_cache_bytes = config->cache_size_kb * 1024;
    p->used_cache_bytes = 0;
    p->initialized = true;

    // Initialize all cache entries to empty
    for (int i = 0; i < CACHE_ENTRY_COUNT; i++) {
        p->cache[i].filename = NULL;
        p->cache[i].buffer = NULL;
        p->cache[i].ref_count = 0;
        p->cache[i].mutex = xSemaphoreCreateMutex();
        if (!p->cache[i].mutex) {
            // Cleanup on failure
            for (int j = 0; j < i; j++) {
                vSemaphoreDelete(p->cache[j].mutex);
            }
            vSemaphoreDelete(p->cache_mutex);
            heap_caps_free(p);
            return ESP_ERR_NO_MEM;
        }
    }

    // Create preload queue
    p->preload_queue = xQueueCreate(PRELOAD_QUEUE_LENGTH, sizeof(preload_item_t));
    if (!p->preload_queue) {
        for (int i = 0; i < CACHE_ENTRY_COUNT; i++) {
            vSemaphoreDelete(p->cache[i].mutex);
        }
        vSemaphoreDelete(p->cache_mutex);
        heap_caps_free(p);
        return ESP_ERR_NO_MEM;
    }

    // Initialize preload pause control
    p->active_stream_count = 0;

    // Create preload task (run on core 0 to avoid contention with player on core 1)
    p->preload_task_running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        cache_task,
        "cache",
        PRELOAD_TASK_STACK_SIZE,
        p,
        PRELOAD_TASK_PRIORITY,
        &p->preload_task_handle,
        0  // Core 0
    );

    if (ret != pdPASS) {
        vQueueDelete(p->preload_queue);
        for (int i = 0; i < CACHE_ENTRY_COUNT; i++) {
            vSemaphoreDelete(p->cache[i].mutex);
        }
        vSemaphoreDelete(p->cache_mutex);
        heap_caps_free(p);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG_PROVIDER, "Audio provider initialized (cache: %zu KB, PSRAM: %zu KB available)",
             p->max_cache_bytes / 1024, psram_size / 1024);

    *provider = p;
    return ESP_OK;
}

void audio_provider_deinit(audio_provider_handle_t provider)
{
    if (!provider) {
        return;
    }

    // Stop preload task
    if (provider->preload_task_running) {
        provider->preload_task_running = false;

        // Send empty filename as sentinel to wake up task
        preload_item_t sentinel = { .filename = "" };
        xQueueSend(provider->preload_queue, &sentinel, pdMS_TO_TICKS(100));

        // Wait for task to exit (max 500ms)
        for (int i = 0; i < 50 && provider->preload_task_handle != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // Delete preload queue
    if (provider->preload_queue) {
        vQueueDelete(provider->preload_queue);
    }

    // Free all cache entries
    for (int i = 0; i < CACHE_ENTRY_COUNT; i++) {
        if (provider->cache[i].filename != NULL) {
            // Warn if entry still in use
            if (provider->cache[i].ref_count > 0) {
                ESP_LOGW(TAG_CACHE, "Freeing cache entry with active streams: %s",
                         provider->cache[i].filename);
            }
            free_cache_entry(provider, &provider->cache[i]);
        }
        vSemaphoreDelete(provider->cache[i].mutex);
    }

    vSemaphoreDelete(provider->cache_mutex);
    heap_caps_free(provider);

    ESP_LOGI(TAG_PROVIDER, "Audio provider deinitialized");
}


void audio_provider_print_status(audio_provider_handle_t provider, status_output_type_t output_type)
{
    if (provider == NULL) {
        if (output_type == STATUS_OUTPUT_COMPACT) {
            printf("[provider] not initialized\n");
        } else {
            printf("Audio Provider Status:\n");
            printf("  State: Not initialized\n");
        }
        return;
    }

    // Count used slots and calculate stats
    int slots_used = 0;
    size_t total_cached_bytes = 0;

    xSemaphoreTake(provider->cache_mutex, portMAX_DELAY);
    for (int i = 0; i < CACHE_ENTRY_COUNT; i++) {
        if (provider->cache[i].filename != NULL) {
            slots_used++;
            total_cached_bytes += provider->cache[i].buf_size;
        }
    }
    size_t max_cache = provider->max_cache_bytes;
    int active_streams = provider->active_stream_count;
    bool preload_running = provider->preload_task_running;
    xSemaphoreGive(provider->cache_mutex);

    if (output_type == STATUS_OUTPUT_COMPACT) {
        printf("[provider] cache: %d/%d slots, %.1fMB/%.1fMB\n",
               slots_used, CACHE_ENTRY_COUNT,
               (double)total_cached_bytes / (1024 * 1024),
               (double)max_cache / (1024 * 1024));
    } else {
        printf("Audio Provider Status:\n");
        printf("  PSRAM Cache: Enabled\n");
        printf("  Slots: %d / %d used\n", slots_used, CACHE_ENTRY_COUNT);
        printf("  Memory: %.1f MB / %.1f MB (%d%%)\n",
               (double)total_cached_bytes / (1024 * 1024),
               (double)max_cache / (1024 * 1024),
               max_cache > 0 ? (int)((total_cached_bytes * 100) / max_cache) : 0);
        const char *preload_state = "Stopped";
        if (preload_running) {
            preload_state = active_streams > 0 ? "Paused" : "Idle";
        }
        printf("  Preload task: %s\n", preload_state);
        printf("  Active streams: %d\n", active_streams);

        if (output_type == STATUS_OUTPUT_VERBOSE) {
            printf("  Cached files:\n");
            xSemaphoreTake(provider->cache_mutex, portMAX_DELAY);
            for (int i = 0; i < CACHE_ENTRY_COUNT; i++) {
                if (provider->cache[i].filename != NULL) {
                    printf("    - %s (%zu KB, refs=%d)\n",
                           provider->cache[i].filename,
                           provider->cache[i].buf_size / 1024,
                           provider->cache[i].ref_count);
                }
            }
            xSemaphoreGive(provider->cache_mutex);
        }
    }
}
