/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */


#include "freertos/FreeRTOS.h"  // IWYU pragma: keep
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "player.h"
#include "provider.h"
#include "persistent_volume.h"

static const char *TAG = "player";

// hardcoded to avoid including soundboard.h just for SPIFFS_MOUNT_POINT
// DISABLED do not delete
//static const char *STARTUP_SOUND = "/spiffs/startup.wav";

// Player task configuration (hardcoded)
#define PLAYER_TASK_PRIORITY        2       // Run between input scanner (3) and idle (0)
#define PLAYER_TASK_STACK_SIZE      8192    // Sufficient for file I/O, decoding, I2S transfers
#define PLAYER_TASK_CORE_ID         1       // Pin to core 1 (APP_CPU) for real-time audio

// I2S DMA buffer architecture (ESP-IDF I2S_CHANNEL_DEFAULT_CONFIG):
//   dma_desc_num  = 6    -- number of DMA descriptors (ring buffer of 6 slots)
//   dma_frame_num = 240  -- audio frames per descriptor
//   One frame = one sample per channel. For mono 16-bit: 240 frames = 480 bytes.
//   Total DMA ring: 6 * 480 = 2880 bytes (~30 ms at 48 kHz mono).
//
// i2s_channel_write() copies our PCM buffer into the DMA ring and blocks until
// space is available. Sizing the PCM buffer to a multiple of dma_frame_num
// avoids partial descriptor fills and keeps writes aligned.
//
// PCM_BUFFER_SIZE = 480 samples = 2 * dma_frame_num (mono 16-bit)
//   -> each i2s_channel_write() fills exactly 2 DMA descriptors (~10 ms at 48 kHz)
//   -> remaining 4 descriptors provide ~20 ms of runway before underrun
//   -> stop/play commands are serviced within ~10 ms (good responsiveness)
//   -> 2-descriptor batching halves the per-chunk overhead vs single-descriptor
#define PCM_BUFFER_SIZE 480

#define I2S_WRITE_TIMEOUT_MS 100


#define INITIAL_BIT_RESOLUTION 16
#define INITIAL_SAMPLE_FREQ    48000
#define INITIAL_CHANNELS       1


// I2S GPIO configuration from Kconfig
#define I2S_LRC_GPIO   CONFIG_SOUNDBOARD_I2S_LRC_GPIO
#define I2S_BCLK_GPIO  CONFIG_SOUNDBOARD_I2S_BCLK_GPIO
#define I2S_DIN_GPIO   CONFIG_SOUNDBOARD_I2S_DIN_GPIO
#define I2S_SD_GPIO    CONFIG_SOUNDBOARD_I2S_SD_GPIO

// Software volume control (MAX98357A has no hardware volume)
#define VOLUME_LEVELS 32
#define VOLUME_FACTOR_UNITY 65536  // 65536 = 0dB (unity gain), 16-bit fixed-point

/**
 * @brief Logarithmic volume lookup table (32 entries)
 *
 * Maps volume index (0-31) to a fixed-point scaling factor (0 to 65536).
 * Index 0 = mute (0), Index 31 = full volume (65536 = unity).
 * ~1.94 dB per step, covering ~60dB dynamic range.
 *
 * factor[i] = round(65536 * 10^(-60*(31-i)/(31*20))), factor[0] = 0
 */
static const uint32_t volume_table[VOLUME_LEVELS] = {
        0,    82,   102,   128,   160,   200,   250,   312,  //  0- 7
      390,   487,   608,   760,   950,  1187,  1484,  1854,  //  8-15
     2317,  2895,  3618,  4521,  5649,  7059,  8821, 11023,  // 16-23
    13774, 17212, 21508, 26877, 33586, 41969, 52445, 65536,  // 24-31
};

/**
 * @brief Player commands
 */
typedef enum {
    PLAYER_CMD_PLAY,
    PLAYER_CMD_STOP,
} player_cmd_type_t;

/**
 * @brief Player command message 
 */
typedef struct {
    player_cmd_type_t type;
    union {
        struct {
            char filename[256];
        } play;
        struct {
            bool interrupt_now;  /* true=stop playing as fast as possible. false= stop loop timer and finish playing current sample*/
        } stop;
    };
} player_cmd_t;

/**
 * @brief Player task state
 */
typedef struct player_s {
    // Task handle
    TaskHandle_t player_task_handle;

    // Command queue (public API → player task)
    QueueHandle_t cmd_queue;

    // i2s channel handle (from I2S driver)
    i2s_chan_handle_t i2s_channel;
    
    // streaming permanent ressources
    audio_provider_handle_t provider;
    int16_t *pcm_buf;       // buffer between audio_provider and uac_host_write

    // temporary ressources (NULL when not playing)
    audio_stream_handle_t stream;

    // callback to notify player state changes event to other modules (e.g. display)
    player_event_callback_t event_cb;
    void *event_cb_ctx;

    // Stream optimization state (avoid useless reconfigurations to reduce latency)
    uint32_t last_frame_rate;
    uint8_t last_bit_depth;
    uint8_t last_channels;

    // Software volume control (logarithmic scaling for MAX98357A)
    SemaphoreHandle_t volume_mutex;  // Protects sw_volume_factor and vol_current
    uint32_t sw_volume_factor;  // 0=mute, 65536=unity (from volume_table[])
    uint8_t vol_current;        // Current volume index (0 to VOLUME_LEVELS-1)

    // Progress reporting (time-throttled to avoid display queue flood)
    TickType_t last_progress_tick;    // Last tick when progress was sent
    char current_filename[256];       // Filename of current stream (for progress events)

} player_state_t;


/**
 * @brief Apply software volume scaling to PCM samples in-place
 *
 * Scales 16-bit PCM samples using fixed-point multiplication.
 * Factor 65536 = unity (0dB), factor 0 = mute. Logarithmic curve
 * via lookup table provides perceptually uniform volume steps.
 *
 * @param buffer PCM sample buffer (modified in-place)
 * @param sample_count Number of samples in buffer
 * @param factor Volume scaling factor (0-65536)
 */
static void apply_software_volume(int16_t *buffer, size_t sample_count, uint32_t factor)
{
    // Skip if full volume (no scaling needed)
    if (factor >= VOLUME_FACTOR_UNITY) {
        return;
    }

    // Mute case - zero all samples
    if (factor == 0) {
        memset(buffer, 0, sample_count * sizeof(int16_t));
        return;
    }

    // Apply scaling: sample = (sample * factor) >> 16
    for (size_t i = 0; i < sample_count; i++) {
        int32_t scaled = ((int32_t)buffer[i] * (int32_t)factor) >> 16;
        buffer[i] = (int16_t)scaled;
    }
}

/**
 * @brief Transfer PCM data chunk from provider to I2S device
 *
 * Reads a chunk of samples from the audio stream, applies software volume,
 * and writes them to the I2S device. Called repeatedly for continuous playback.
 *
 * @param[in] player Player state containing provider and I2S channel handles
 * @param[out] bytes_written Number of bytes successfully written to I2S (0 = end of stream)
 * @return ESP_OK on success or end of stream, error code otherwise
 */
static esp_err_t send_chunk(player_state_t *player, size_t *bytes_written)
{
    *bytes_written = 0;

    // Read samples from provider
    size_t samples_read = 0;
    esp_err_t ret = audio_provider_read_stream(player->stream, player->pcm_buf, PCM_BUFFER_SIZE, &samples_read);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Provider read error: %s", esp_err_to_name(ret));
        return ret;
    }

    // End of stream - no more samples available
    if (samples_read == 0) {
        ESP_LOGD(TAG, "End of stream reached");
        return ESP_OK;
    }

    // Apply software volume control (read factor under mutex)
    xSemaphoreTake(player->volume_mutex, portMAX_DELAY);
    uint32_t volume_factor = player->sw_volume_factor;
    xSemaphoreGive(player->volume_mutex);
    apply_software_volume(player->pcm_buf, samples_read, volume_factor);

    // Note: MAX98357A with SD pin pulled to 1MOhm outputs Left channel only.
    // If stereo mixing is needed, implement L+R mix here before I2S write.
    // Currently deferred until hardware testing confirms unmixed audio issue.

    // Write samples to I2S device
    size_t bytes_to_write = samples_read * sizeof(int16_t);
    ret = i2s_channel_write(player->i2s_channel, player->pcm_buf, bytes_to_write, bytes_written, I2S_WRITE_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2S write error: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}


static bool i2s_sd_gpio_initialized = false;

/**
 * @brief Enable or disable the MAX98357A amplifier via SD pin
 *
 * The SD pin is active-high: HIGH enables the amplifier, LOW puts it in shutdown mode.
 *
 * @param enable true to enable amplifier, false to shutdown
 * @return ESP_OK on success
 */
static esp_err_t set_i2s_sd_gpio(bool enable)
{
    // One-time GPIO initialization
    if (!i2s_sd_gpio_initialized) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << I2S_SD_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t ret = gpio_config(&io_conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure I2S SD GPIO: %s", esp_err_to_name(ret));
            return ret;
        }
        i2s_sd_gpio_initialized = true;
        ESP_LOGD(TAG, "I2S SD GPIO %d configured", I2S_SD_GPIO);
    }

    // Set GPIO level (HIGH=enable, LOW=shutdown)
    gpio_set_level(I2S_SD_GPIO, enable ? 1 : 0);
    ESP_LOGD(TAG, "I2S amplifier %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}

/**
 * @brief Initialize I2S channel for audio output
 *
 * Creates and configures the I2S channel with standard Philips mode.
 * Uses GPIOs defined in Kconfig for LRC, BCLK, and DIN.
 *
 * @param player Player state to store channel handle
 * @return ESP_OK on success
 */
static esp_err_t init_i2s_channel(player_state_t *player)
{
    // Channel configuration
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;  // Clear DMA buffer on underflow

    esp_err_t ret = i2s_new_channel(&chan_cfg, &player->i2s_channel, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Standard I2S mode configuration (Philips format)
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(INITIAL_SAMPLE_FREQ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                    (INITIAL_CHANNELS == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws = I2S_LRC_GPIO,
            .dout = I2S_DIN_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(player->i2s_channel, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init I2S standard mode: %s", esp_err_to_name(ret));
        i2s_del_channel(player->i2s_channel);
        player->i2s_channel = NULL;
        return ret;
    }

    ret = i2s_channel_enable(player->i2s_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
        i2s_del_channel(player->i2s_channel);
        player->i2s_channel = NULL;
        return ret;
    }

    // Initialize format tracking
    player->last_frame_rate = INITIAL_SAMPLE_FREQ;
    player->last_bit_depth = INITIAL_BIT_RESOLUTION;
    player->last_channels = INITIAL_CHANNELS;

    ESP_LOGI(TAG, "I2S channel initialized (LRC=%d, BCLK=%d, DIN=%d)",
             I2S_LRC_GPIO, I2S_BCLK_GPIO, I2S_DIN_GPIO);
    return ESP_OK;
}

/**
 * @brief Configure I2S stream for audio format
 *
 * Reconfigures I2S clock and slot settings when audio format changes.
 * Skips reconfiguration if format hasn't changed (optimization).
 *
 * @param player Player state with I2S channel handle
 * @param frame_rate Sample rate in Hz (e.g., 44100, 48000)
 * @param channels Number of channels (1=mono, 2=stereo)
 * @param bit_depth Bits per sample (typically 16)
 * @return ESP_OK on success
 */
static esp_err_t configure_stream(player_state_t *player, uint32_t frame_rate, uint16_t channels, uint16_t bit_depth)
{
    // Check if format changed
    if (frame_rate == player->last_frame_rate &&
        bit_depth == player->last_bit_depth &&
        channels == player->last_channels) {
        set_i2s_sd_gpio(true);
        return ESP_OK;
    }

    ESP_LOGD(TAG, "Reconfiguring I2S: %lu Hz, %d ch, %d bit",
             frame_rate, channels, bit_depth);

    // Disable channel before reconfiguration
    esp_err_t ret = i2s_channel_disable(player->i2s_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disable I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Reconfigure clock if sample rate changed
    if (frame_rate != player->last_frame_rate) {
        i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(frame_rate);
        ret = i2s_channel_reconfig_std_clock(player->i2s_channel, &clk_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reconfigure I2S clock: %s", esp_err_to_name(ret));
            i2s_channel_enable(player->i2s_channel);
            return ret;
        }
    }

    // Reconfigure slot if channels or bit depth changed
    if (channels != player->last_channels || bit_depth != player->last_bit_depth) {
        i2s_data_bit_width_t bit_width = I2S_DATA_BIT_WIDTH_16BIT;
        if (bit_depth == 24) {
            bit_width = I2S_DATA_BIT_WIDTH_24BIT;
        } else if (bit_depth == 32) {
            bit_width = I2S_DATA_BIT_WIDTH_32BIT;
        }
        i2s_slot_mode_t slot_mode = (channels == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;

        i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width, slot_mode);
        ret = i2s_channel_reconfig_std_slot(player->i2s_channel, &slot_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to reconfigure I2S slot: %s", esp_err_to_name(ret));
            i2s_channel_enable(player->i2s_channel);
            return ret;
        }
    }

    // Re-enable channel
    ret = i2s_channel_enable(player->i2s_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to re-enable I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Update tracked format
    player->last_frame_rate = frame_rate;
    player->last_bit_depth = bit_depth;
    player->last_channels = channels;

    // Enable amplifier
    set_i2s_sd_gpio(true);

    return ESP_OK;
}


static void close_stream(player_state_t *player, player_event_name_t event, esp_err_t error_code, bool enable_amp)
{
    if (player->stream != NULL) {
        esp_err_t ret = audio_provider_close_stream(player->stream);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Error closing stream: %s", esp_err_to_name(ret));
        }
        player->stream = NULL;
    }


    // Shutdown amplifier if requested
    set_i2s_sd_gpio(enable_amp);

    // fire a callback event
    if (player->event_cb != NULL) {
        player_event_data_t event_data = {
            .name = event,
        };
        if (event == PLAYER_EVENT_ERROR) {
            event_data.error_code = error_code;
        } else {
            event_data.filename = NULL;
        }
        player->event_cb(&event_data, player->event_cb_ctx);
    }
}

/**
 * @brief Command handler for play command
 *
 * @param player Player state
 * @param filename Path to audio file
 */
static void cmd_play(player_state_t *player, const char filename[256])
{
    if(player->stream != NULL) {
        // stop current stream first
        close_stream(player, PLAYER_EVENT_STOPPED, ESP_OK, true);
    }

    esp_err_t err = audio_provider_open_stream(player->provider, filename, &player->stream);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open stream '%s': %s", filename, esp_err_to_name(err));
        close_stream(player, PLAYER_EVENT_ERROR, err, false);
        player->stream = NULL;
        return;
    }

    // Get audio format from stream
    const audio_info_t *stream_info = audio_provider_get_stream_info(player->stream);
    if (stream_info == NULL) {
        ESP_LOGE(TAG, "Failed to get stream info");
        close_stream(player, PLAYER_EVENT_ERROR, ESP_FAIL, false);
        return;
    }

    // Reconfigure output channel if format changed (no-op if format is unchanged)
    err = configure_stream(player, stream_info->frame_rate, stream_info->channels, stream_info->bit_depth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reconfigure stream: %s", esp_err_to_name(err));
        close_stream(player, PLAYER_EVENT_ERROR, err, false);
        return;
    }

    ESP_LOGD(TAG, "Started playback: %s", filename);

    // Store filename and reset progress tick for periodic reporting
    strncpy(player->current_filename, filename, sizeof(player->current_filename) - 1);
    player->current_filename[sizeof(player->current_filename) - 1] = '\0';
    player->last_progress_tick = 0;

    // Fire callback event to notify playback started
    if (player->event_cb != NULL) {
        player_event_data_t event_data = {
            .name = PLAYER_EVENT_STARTED,
            .filename = filename,
        };
        player->event_cb(&event_data, player->event_cb_ctx);
    }

    // player main loop will start sending chunks from player->stream to I2S channel
}

static void cmd_stop(player_state_t *player, bool immediate)
{
    if (immediate) {
        if (player->stream != NULL) {
            close_stream(player, PLAYER_EVENT_STOPPED, ESP_OK, false);
        }
    } else {
        ESP_LOGD(TAG, "Non-immediate stop: will stop after current playback completes");
    }
}


/**
 * @brief Player task main loop
 *
 * Waits for commands (PLAY/STOP) and handles audio streaming
 * - When idle: Blocks waiting for commands
 * - When playing: Continuously sends chunks and poll queue for commands
 */
static void player_task(void *arg)
{
    player_state_t *player = (player_state_t *)arg;
    ESP_LOGI(TAG, "Player task started");

    player_cmd_t cmd;
    while (1) {
        // when idle (stream==NULL): Block indefinitely
        // when playing (stream!=NULL): Non-blocking check for STOP while continuing to stream
        TickType_t wait_time = (player->stream == NULL) ? portMAX_DELAY : 0;

        if (xQueueReceive(player->cmd_queue, &cmd, wait_time) == pdTRUE) {
            // process command
            switch (cmd.type) {
            case PLAYER_CMD_PLAY:
                cmd_play(player, cmd.play.filename);
                break;

            case PLAYER_CMD_STOP:
                cmd_stop(player, cmd.stop.interrupt_now);
                break;

            default:
                ESP_LOGW(TAG, "Unknown command type: %d", cmd.type);
                break;
            }
        }

        // If we have an active stream, send the next chunk
        if (player->stream == NULL) {
            continue;  // No stream, wait for commands
        }

        // Send next chunk
        size_t written = 0;
        esp_err_t ret = send_chunk(player, &written);

        if (ret != ESP_OK) {
            // Transfer error
            ESP_LOGE(TAG, "Chunk send error: %s, stopping stream", esp_err_to_name(ret));
            close_stream(player, PLAYER_EVENT_ERROR, ret, false);
            continue;
        }

        if (written == 0) {
            // End of stream reached
            close_stream(player, PLAYER_EVENT_STOPPED, ESP_OK, false);
            continue;
        }

        // Report progress (time-throttled: max ~20 updates/sec to avoid display queue flood)
        if (player->event_cb != NULL) {
            TickType_t now = xTaskGetTickCount();
            if ((now - player->last_progress_tick) >= pdMS_TO_TICKS(50)) {
                player->last_progress_tick = now;
                uint16_t progress = audio_provider_get_stream_progress(player->stream);
                player_event_data_t event_data = {
                    .name = PLAYER_EVENT_PROGRESS,
                    .playback = {
                        .filename = player->current_filename,
                        .progress = progress,
                    },
                };
                player->event_cb(&event_data, player->event_cb_ctx);
            }
        }
    }
}


//=========================================================
// public API implementation
//=========================================================

/**
 * @brief Cleanup helper for partial initialization failure
 *
 * Releases resources in reverse order of allocation.
 * Safe to call with NULL handles (skips cleanup for uninitialized resources).
 */
static void cleanup_player_state(player_state_t *state)
{
    set_i2s_sd_gpio(false);
    if (state == NULL) {
        return;
    }
    if (state->player_task_handle != NULL) {
        vTaskDelete(state->player_task_handle);
    }
    if (state->cmd_queue != NULL) {
        vQueueDelete(state->cmd_queue);
    }
    if (state->volume_mutex != NULL) {
        vSemaphoreDelete(state->volume_mutex);
    }
    if (state->pcm_buf != NULL) {
        free(state->pcm_buf);
    }
    if (state->provider != NULL) {
        audio_provider_deinit(state->provider);
    }
    if (state->i2s_channel != NULL) {
        i2s_channel_disable(state->i2s_channel);
        i2s_del_channel(state->i2s_channel);
    }
    free(state);
}

esp_err_t player_init(const player_config_t *config, player_handle_t *player)
{
    if (config == NULL || player == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Allocate player state
    player_state_t *state = (player_state_t *)calloc(1, sizeof(player_state_t));
    if (state == NULL) {
        ESP_LOGE(TAG, "Failed to allocate player state");
        return ESP_ERR_NO_MEM;
    }

    // Store configuration
    state->event_cb = config->event_cb;
    state->event_cb_ctx = config->event_cb_ctx;

    // Create volume mutex for thread-safe volume access
    state->volume_mutex = xSemaphoreCreateMutex();
    if (state->volume_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create volume mutex");
        cleanup_player_state(state);
        return ESP_ERR_NO_MEM;
    }

    // Initialize I2S channel for audio output
    esp_err_t ret = init_i2s_channel(state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S channel: %s", esp_err_to_name(ret));
        cleanup_player_state(state);
        return ret;
    }

    // Initialize persistent volume module (creates deferred save timer)
    persistent_volume_init();

    // Load volume from NVS (or use defaults if not saved)
    uint16_t saved_curve = 0;
    uint16_t saved_index = 0;
    esp_err_t vol_ret = persistent_volume_load(&saved_curve, &saved_index);
    if (vol_ret != ESP_OK) {
        // Fallback to default if NVS fails
        saved_index = VOLUME_LEVELS / 2;
        ESP_LOGW(TAG, "Failed to load volume from NVS, using default");
    }

    // Clamp index to valid range (in case of corrupted NVS data)
    if (saved_index >= VOLUME_LEVELS) {
        saved_index = VOLUME_LEVELS - 1;
    }

    state->vol_current = (uint8_t)saved_index;
    state->sw_volume_factor = volume_table[state->vol_current];
    ESP_LOGI(TAG, "Initial volume: index %d (factor %lu/65536)", state->vol_current, (unsigned long)state->sw_volume_factor);

    // Create audio provider
    audio_provider_config_t provider_config = {
        .cache_size_kb = config->cache_size_kb,
    };
    ret = audio_provider_init(&provider_config, &state->provider);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create audio provider: %s", esp_err_to_name(ret));
        cleanup_player_state(state);
        return ret;
    }

    // Allocate PCM buffer - pass data chunks from audio_provider to i2s_channel_write()
    state->pcm_buf = (int16_t *)malloc(PCM_BUFFER_SIZE * sizeof(int16_t));
    if (state->pcm_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer");
        cleanup_player_state(state);
        return ESP_ERR_NO_MEM;
    }

    // Create command queue
    state->cmd_queue = xQueueCreate(10, sizeof(player_cmd_t));
    if (state->cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create command queue");
        cleanup_player_state(state);
        return ESP_ERR_NO_MEM;
    }

    // Create player task
    BaseType_t task_created = xTaskCreatePinnedToCore(
        player_task,
        "player_task",
        PLAYER_TASK_STACK_SIZE,
        (void *)state,
        PLAYER_TASK_PRIORITY,
        &state->player_task_handle,
        PLAYER_TASK_CORE_ID
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create player task");
        cleanup_player_state(state);
        return ESP_ERR_NO_MEM;
    }

    *player = state;
    ESP_LOGI(TAG, "Player initialized successfully");

    // Fire PLAYER_EVENT_READY callback with initial volume
    if (state->event_cb != NULL) {
        player_event_data_t event_data = {
            .name = PLAYER_EVENT_READY,
            .volume_index = state->vol_current,
        };
        state->event_cb(&event_data, state->event_cb_ctx);
    }

    // Queue a command to play a short sound to confirm init completion
    // DISABLED but DO NOT DELETE
    // ret = player_play(state, STARTUP_SOUND, 0, 1);
    // ESP_LOGD(TAG, "called player_play() with startup sound");
    // if (ret != ESP_OK) {
    //     ESP_LOGW(TAG, "Failed to send command to play startup sound: %s", esp_err_to_name(ret));
    //     // Continue anyway - non-critical
    // }

    return ESP_OK;
}

esp_err_t send_cmd(player_handle_t player, player_cmd_t *cmd)
{
    if (player == NULL || cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    player_state_t *state = (player_state_t *)player;
    if (xQueueSend(state->cmd_queue, cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to queue player command (queue full)");
        return ESP_FAIL;
    }

    return ESP_OK;
}


esp_err_t player_play(player_handle_t player, const char *filename)
{
    if (player == NULL || filename == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    player_cmd_t cmd = {
        .type = PLAYER_CMD_PLAY,
    };

    // Copy filename (safely truncate if too long)
    strncpy(cmd.play.filename, filename, sizeof(cmd.play.filename) - 1);
    cmd.play.filename[sizeof(cmd.play.filename) - 1] = '\0';

    return send_cmd(player, &cmd);
}


esp_err_t player_stop(player_handle_t player, bool interrupt_now)
{
    if (player == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    player_cmd_t cmd = {
        .type = PLAYER_CMD_STOP,
        .stop = {
            .interrupt_now = interrupt_now
        }
    };

    return send_cmd(player, &cmd);
}


esp_err_t player_preload(player_handle_t player, const char *filename)
{
    if (player == NULL || filename == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    player_state_t *state = (player_state_t *)player;
    return audio_provider_preload(state->provider, filename);
}

void player_flush_preload(player_handle_t player)
{
    if (player == NULL) {
        return;
    }
    player_state_t *state = (player_state_t *)player;
    audio_provider_flush_preload_queue(state->provider);
}

esp_err_t player_deinit(player_handle_t player)
{
    if (player == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    player_state_t *state = (player_state_t *)player;

    // Stop playback if active
    if (state->stream != NULL) {
        close_stream(player, PLAYER_EVENT_STOPPED, ESP_OK, false);
    }

    // Cleanup all resources
    cleanup_player_state(state);
    ESP_LOGI(TAG, "Player deinitialized");
    return ESP_OK;
}

/* =============================================================================
 * Synchronous Volume API (for software volume - no USB queries needed)
 * ============================================================================= */

int player_volume_get_max_index(void)
{
    return VOLUME_LEVELS - 1;
}

esp_err_t player_volume_get(player_handle_t player, int *volume_index)
{
    if (player == NULL || volume_index == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    player_state_t *state = (player_state_t *)player;
    xSemaphoreTake(state->volume_mutex, portMAX_DELAY);
    *volume_index = (int)state->vol_current;
    xSemaphoreGive(state->volume_mutex);
    return ESP_OK;
}

esp_err_t player_volume_set(player_handle_t player, int8_t index)
{
    if (player == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    player_state_t *state = (player_state_t *)player;

    // Clamp index to valid range
    if (index < 0) {
        index = 0;
    }
    if (index >= VOLUME_LEVELS) {
        index = VOLUME_LEVELS - 1;
    }

    // Update volume fields under mutex protection
    xSemaphoreTake(state->volume_mutex, portMAX_DELAY);
    state->sw_volume_factor = volume_table[index];
    state->vol_current = (uint8_t)index;
    uint32_t factor = state->sw_volume_factor;
    xSemaphoreGive(state->volume_mutex);

    ESP_LOGI(TAG, "Set volume (sync): index %d (factor %lu/65536)", index, (unsigned long)factor);

    // Schedule deferred save to NVS (coalesces rapid changes)
    persistent_volume_save_deferred(0, (uint16_t)index);

    // Fire event callback to notify volume change
    if (state->event_cb != NULL) {
        player_event_data_t event_data = {
            .name = PLAYER_EVENT_VOLUME_CHANGED,
            .volume_index = index,
        };
        state->event_cb(&event_data, state->event_cb_ctx);
    }

    return ESP_OK;
}

esp_err_t player_volume_adjust(player_handle_t player, int8_t step)
{
    if (player == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (step == 0) {
        return ESP_OK;
    }

    player_state_t *state = (player_state_t *)player;

    // Read current index under mutex, then call set (which also takes mutex)
    xSemaphoreTake(state->volume_mutex, portMAX_DELAY);
    int32_t new_index = (int32_t)state->vol_current + (int32_t)step;
    xSemaphoreGive(state->volume_mutex);

    return player_volume_set(player, (int8_t)new_index);
}

void player_print_status(player_handle_t player, status_output_type_t output_type)
{
    if (player == NULL) {
        if (output_type == STATUS_OUTPUT_COMPACT) {
            printf("[player] not initialized\n");
        } else {
            printf("Player Status:\n");
            printf("  State: Not initialized\n");
        }
        return;
    }

    player_state_t *state = (player_state_t *)player;

    // Get current state
    xSemaphoreTake(state->volume_mutex, portMAX_DELAY);
    int vol_index = state->vol_current;
    xSemaphoreGive(state->volume_mutex);

    bool is_playing = (state->stream != NULL);

    if (output_type == STATUS_OUTPUT_COMPACT) {
        printf("[player] %s, vol=%d/%d\n",
               is_playing ? "playing" : "idle",
               vol_index, VOLUME_LEVELS - 1);
    } else {
        printf("Player Status:\n");
        printf("  State: %s\n", is_playing ? "Playing" : "Idle");
        printf("  Volume: %d / %d\n", vol_index, VOLUME_LEVELS - 1);
        printf("  I2S: GPIO LRC=%d, BCLK=%d, DIN=%d, SD=%d\n",
               I2S_LRC_GPIO, I2S_BCLK_GPIO, I2S_DIN_GPIO, I2S_SD_GPIO);
    }

    // Delegate to audio provider for cache status
    audio_provider_print_status(state->provider, output_type);
}
