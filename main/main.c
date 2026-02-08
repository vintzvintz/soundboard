/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file main.c
 * @brief Main application entry point and common initialization
 */

#include <inttypes.h>
#include <sys/stat.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"   // IWYU pragma: keep
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"

// Application modules
#include "app_state.h"
#include "sd_card.h"
#include "persistent_volume.h"
#include "console.h"
#include "esp_timer.h"


static const char *TAG = "soundboard";

// SPIFFS uses hardcoded filename
// not a kconfig option because it must match filename in spiffs/ project directory
#define SPIFFS_MAPPINGS_FILE "mappings.csv"
// SD card uses Kconfig-configurable filename (shared with MSC sync)
#define SDCARD_MAPPINGS_FILE CONFIG_SOUNDBOARD_MAPPINGS_FILENAME

// Full paths to mappings files (declared in soundboard.h)
const char *const SPIFFS_MAPPINGS_PATH = SPIFFS_MOUNT_POINT "/" SPIFFS_MAPPINGS_FILE;
const char *const SDCARD_MAPPINGS_PATH = SDCARD_MOUNT_POINT "/" SDCARD_MAPPINGS_FILE;

static app_state_t s_app_state = {0};


/* ============================================================================
 * Log Level Configuration
 * ============================================================================ */

static void set_loglevels(void)
{
    //esp_log_level_set(TAG, ESP_LOG_DEBUG);
    //esp_log_level_set("display", ESP_LOG_DEBUG);
    //esp_log_level_set("mapper", ESP_LOG_DEBUG);
    //esp_log_level_set("player", ESP_LOG_DEBUG);
    //esp_log_level_set("audio_cache", ESP_LOG_DEBUG);
    //esp_log_level_set("audio_provider", ESP_LOG_DEBUG);
    //esp_log_level_set("msc", ESP_LOG_DEBUG);
    //esp_log_level_set("input_scanner", ESP_LOG_DEBUG);
}


/* ============================================================================
 * Fine-grained State Accessors
 * ============================================================================ */

static void app_set_mode(application_mode_t mode)
{
    if(mode == s_app_state.mode) {
        return;
    }
    switch(mode) {
        case APP_MODE_MSC:
            ESP_LOGI(TAG, "Switching to USB update mode");
            break;
        case APP_MODE_PLAYER:
            ESP_LOGI(TAG, "Switching to normal soundboard mode");
            break;
        case APP_MODE_NONE:
            ESP_LOGW(TAG, "Switching to NONE mode");
            break;
        default:
            ESP_LOGW(TAG, "Unknown mode %d", mode);
            return;
    }
    s_app_state.mode = mode;
}


/* ============================================================================
 * Event routing callbacks
 * ============================================================================ */

/**
 * @brief MSC event callback to update display
 *
 * Called from MSC FSM task context when state changes occur.
 * Routes events to display module.
 */
static void msc_event_callback(const msc_event_data_t *event_data, void *user_ctx)
{
    (void)user_ctx;

    display_handle_t oled = s_app_state.oled;

    switch (event_data->type) {
        case MSC_EVENT_READY:
            display_on_msc_analysis(oled, "Ready");
            break;
        case MSC_EVENT_MENU_FULL_SELECTED:
            display_on_msc_menu(oled, 0);
            break;
        case MSC_EVENT_MENU_INCREMENTAL_SELECTED:
            display_on_msc_menu(oled, 1);
            break;
        case MSC_EVENT_MENU_SD_CLEAR_SELECTED:
            display_on_msc_menu(oled, 2);
            break;
        case MSC_EVENT_MENU_SD_CLEAR_CONFIRM:
            display_on_msc_sd_clear_confirm(oled);
            break;
        case MSC_EVENT_UPDATING:
            display_on_msc_progress(oled, event_data->progress.filename,
                                    event_data->progress.progress);
            break;
        case MSC_EVENT_UPDATE_DONE:
            display_on_msc_progress(oled, "Done", UINT16_MAX);
            break;
        case MSC_EVENT_UPDATE_FAILED:
            display_on_error(oled, event_data->error.message);
            break;
    }
}

/**
 * @brief Unified mapper event callback
 *
 * Called when mapper state changes or actions are executed.
 * Routes events to display and other observers.
 */
static void mapper_event_callback(const mapper_event_t *event, void *user_ctx)
{
    (void)user_ctx;

    switch (event->type) {

        case MAPPER_EVENT_LOADED:
            ESP_LOGI(TAG, "Mapper loaded: %d pages, initial page '%s'",
                     event->loaded.page_count, event->loaded.initial_page_id);
            display_on_page_changed(s_app_state.oled, event->loaded.initial_page_id);
            break;

        case MAPPER_EVENT_ACTION_EXECUTED:
            if (s_app_state.mode != APP_MODE_PLAYER) {
                ESP_LOGW(TAG, "Mapper event ignored (not in PLAYER mode)");
                break;
            }
            ESP_LOGD(TAG, "Mapper action: btn=%d, action=%d",
                     event->action_executed.button_number,
                     event->action_executed.action->type);
            break;

        case MAPPER_EVENT_ENCODER_MODE_CHANGED:
            if (s_app_state.mode != APP_MODE_PLAYER) {
                ESP_LOGW(TAG, "Mapper event ignored (not in PLAYER mode)");
                break;
            }
            ESP_LOGI(TAG, "Encoder mode changed to %s",
                     event->encoder_mode_changed.mode == ENCODER_MODE_VOLUME ? "VOLUME" : "PAGE");
            display_on_encoder_mode_changed(s_app_state.oled,
                                             event->encoder_mode_changed.mode == ENCODER_MODE_PAGE);
            break;

        case MAPPER_EVENT_PAGE_CHANGED:
            if (s_app_state.mode != APP_MODE_PLAYER) {
                ESP_LOGW(TAG, "Mapper event ignored (not in PLAYER mode)");
                break;
            }
            ESP_LOGI(TAG, "Page changed to '%s' (%d pages)",
                     event->page_changed.page_id, event->page_changed.page_count);
            display_on_page_changed(s_app_state.oled, event->page_changed.page_id);
            break;

        default:
            ESP_LOGW(TAG, "Unknown mapper event: %d", event->type);
            break;
    }
}

/**
 * @brief Player event callback to update display
 */
static void player_event_callback(const player_event_data_t *event, void *user_ctx)
{
    (void)user_ctx;

    display_handle_t oled = s_app_state.oled;

    // PLAYER_EVENT_READY fires during init (before APP_MODE_PLAYER is set)
    // and must not be blocked by the mode guard
    if (event->name == PLAYER_EVENT_READY) {
        ESP_LOGD(TAG, "Player ready, initial volume: %d", event->volume_index);
        display_on_volume_changed(oled, event->volume_index);
        return;
    }

    if (s_app_state.mode != APP_MODE_PLAYER) {
        ESP_LOGW(TAG, "Player event ignored (not in PLAYER mode)");
        return;
    }

    switch (event->name) {
        case PLAYER_EVENT_STARTED:
            ESP_LOGD(TAG, "Playback started: %s", event->filename ? event->filename : "(unknown)");
            if (event->filename != NULL) {
                display_on_playing(oled, event->filename, 0);
            }
            break;

        case PLAYER_EVENT_STOPPED:
            ESP_LOGD(TAG, "Playback stopped");
            display_on_playing(oled, NULL, 0);
            break;

        case PLAYER_EVENT_PROGRESS:
            display_on_playing(oled, event->playback.filename, event->playback.progress);
            break;

        case PLAYER_EVENT_VOLUME_CHANGED:
            ESP_LOGD(TAG, "Volume changed to index %d", event->volume_index);
            display_on_volume_changed(oled, event->volume_index);
            break;

        case PLAYER_EVENT_ERROR:
            ESP_LOGE(TAG, "Player error occurred: %s", esp_err_to_name(event->error_code));
            display_on_error(oled, esp_err_to_name(event->error_code));
            break;

        default:
            ESP_LOGW(TAG, "Unknown player event: %d", event->name);
            break;
    }
}

/**
 * @brief Routes input events to mapper or msc - depending on application mode
 *
 * Adapts the input_scanner callback signature to call mapper_handle_event.
 *
 * @param btn_num Button number from input scanner
 * @param event Input event type
 * @param user_ctx User context (mapper_handle_t)
 */
static void input_scanner_callback(uint8_t btn_num, input_event_type_t event, void *user_ctx)
{
    (void)user_ctx;

    switch(s_app_state.mode){
        case APP_MODE_PLAYER:
            mapper_handle_event(s_app_state.mapper, btn_num, event);
            break;
        case APP_MODE_MSC:
            msc_handle_input_event(s_app_state.msc, btn_num, event);
            break;
        case APP_MODE_NONE:
        default:
            ESP_LOGI(TAG, "input event (btn_num=%d event=%d) ignored", btn_num, event);
            break;
    }
}


/* ============================================================================
 * Modules initialisation helpers
 * ============================================================================ */

/**
 * @brief Initialize SPIFFS filesystem
 * @return ESP_OK on success, error code otherwise
 * @note FATAL - application requires SPIFFS for fallback config
 */
static esp_err_t init_spiffs(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS...");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_MOUNT_POINT,
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = false
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS: partition not found");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("spiffs", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS initialized (used %zu/%zu KB)", used / 1024, total / 1024);
    } else {
        ESP_LOGI(TAG, "SPIFFS initialized");
    }

    return ESP_OK;
}

/**
 * @brief Initialize NVS (Non-Volatile Storage)
 * @return ESP_OK on success, error code otherwise
 * @note FATAL - application requires NVS for volume persistence
 */
static esp_err_t init_nvs(void)
{
    ESP_LOGI(TAG, "Initializing NVS...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition corrupt, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "NVS initialized");
    return ESP_OK;
}

/**
 * @brief Initialize SD card
 * @param[out] card Output SD card handle (NULL if failed)
 * @return ESP_OK on success, error code otherwise
 * @note NON-FATAL - configuration will fallback to SPIFFS
 */
static esp_err_t init_sd_card(sdmmc_card_t **card)
{
    ESP_LOGI(TAG, "Initializing SD card...");

    sd_card_spi_config_t config = SD_CARD_SPI_DEFAULT_CONFIG();
    config.mount_point = SDCARD_MOUNT_POINT;

    esp_err_t ret = sd_card_init(&config, card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize SD card: %s (using SPIFFS fallback)", esp_err_to_name(ret));
        *card = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "SD card initialized");
    return ESP_OK;
}


/**
 * @brief Initialize display module
 * @param[out] oled Output display handle (NULL if failed)
 * @return ESP_OK on success, error code otherwise
 * @note NON-FATAL - soundboard can operate without display
 */
static esp_err_t init_display(display_handle_t *oled)
{
    ESP_LOGI(TAG, "Initializing display...");

    display_config_t config = DISPLAY_CONFIG_DEFAULT();

    esp_err_t ret = display_init(&config, oled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize display: %s (continuing headless)", esp_err_to_name(ret));
        *oled = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "Display initialized");
    return ESP_OK;
}

/**
 * @brief Initialize mapper module
 * @param player Player handle for playback control
 * @param[out] mapper Output mapper handle
 * @param[out] config_source Output configuration source indicator
 * @return ESP_OK on success, error code otherwise
 * @note FATAL - application cannot function without button mappings
 */
static esp_err_t init_mapper(player_handle_t player, mapper_handle_t *mapper, config_source_t *config_source)
{
    ESP_LOGI(TAG, "Initializing mapper...");

    struct stat st;
    bool has_spiffs = (stat(SPIFFS_MAPPINGS_PATH, &st) == 0);
    bool has_sdcard = (stat(SDCARD_MAPPINGS_PATH, &st) == 0);

    // Determine configuration source (SD card takes precedence)
    if (has_sdcard) {
        *config_source = CONFIG_SOURCE_SDCARD;
        ESP_LOGI(TAG, "  Config source: SD card (%s)", SDCARD_MAPPINGS_PATH);
    } else if (has_spiffs) {
        *config_source = CONFIG_SOURCE_FIRMWARE;
        ESP_LOGI(TAG, "  Config source: SPIFFS (%s)", SPIFFS_MAPPINGS_PATH);
    } else {
        *config_source = CONFIG_SOURCE_NONE;
        ESP_LOGE(TAG, "No mappings.csv found (tried %s, %s)", SDCARD_MAPPINGS_PATH, SPIFFS_MAPPINGS_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    mapper_config_t config = {
        .spiffs_root = has_spiffs ? SPIFFS_MOUNT_POINT : NULL,
        .spiffs_mappings_file = has_spiffs ? SPIFFS_MAPPINGS_FILE : NULL,
        .sdcard_root = has_sdcard ? SDCARD_MOUNT_POINT : NULL,
        .sdcard_mappings_file = has_sdcard ? SDCARD_MAPPINGS_FILE : NULL,
        .player = player,
        .event_cb = mapper_event_callback,
        .event_cb_ctx = NULL,
    };

    esp_err_t ret = mapper_init(&config, mapper);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize mapper: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Mapper initialized");
    return ESP_OK;
}


/**
 * @brief Initialize unified input scanner (matrix keypad + rotary encoder)
 * @param mapper Action mapper handle (for callbacks)
 * @param[out] scanner Output handle for input scanner
 * @return ESP_OK on success, error code otherwise
 * @note FATAL - application cannot function without input
 */
static esp_err_t init_input_scanner(mapper_handle_t mapper, input_scanner_handle_t *scanner)
{
    ESP_LOGI(TAG, "Initializing input scanner...");

    input_scanner_config_t config = INPUT_SCANNER_DEFAULT_CONFIG();
    config.callback = input_scanner_callback;
    // Pin to core 1 (APP_CPU) for real-time input responsiveness
    config.task_core_id = 1;

    esp_err_t ret = input_scanner_init(&config, scanner);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize input scanner: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Input scanner initialized");
    return ESP_OK;
}

/**
 * @brief Initialize player module
 * @param[out] player Output handle for player
 * @return ESP_OK on success, error code otherwise
 * @note FATAL - application cannot function without player
 */
static esp_err_t init_player(player_handle_t *player)
{
    ESP_LOGI(TAG, "Initializing player...");

    player_config_t config = PLAYER_CONFIG_DEFAULT();
    config.event_cb = player_event_callback;

    esp_err_t ret = player_init(&config, player);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize player: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Player initialized");
    return ESP_OK;
}

/**
 * @brief Initialize MSC (Mass Storage Class) module
 * @param[out] msc Output handle for MSC
 * @return ESP_OK on success, error code otherwise
 * @note NON-FATAL - USB sync feature won't work but soundboard can still operate
 */
static esp_err_t init_msc(msc_handle_t *msc)
{
    ESP_LOGI(TAG, "Initializing MSC module...");

    msc_config_t config = {
        .main_task = xTaskGetCurrentTaskHandle(),
        .event_cb = msc_event_callback,
        .event_cb_ctx = NULL,
    };

    esp_err_t ret = msc_init(&config, msc);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize MSC module: %s (USB sync disabled)", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "MSC module initialized");
    return ESP_OK;
}

/**
 * @brief Initialize console (UART CLI)
 * @param app_state Pointer to application state
 * @note NON-FATAL - debug console won't work but soundboard can still operate
 */
static void init_console(const app_state_t *app_state)
{
    ESP_LOGI(TAG, "Initializing console...");
    console_init(app_state);
    ESP_LOGI(TAG, "Console initialized");
}


/* ============================================================================
 * console utility
 * ============================================================================ */

/**
 * @brief Print application-level status information
 */
void app_print_status(status_output_type_t output_type)
{
    const char *mode_str = "UNKNOWN";
    switch (s_app_state.mode) {
        case APP_MODE_NONE: mode_str = "NONE"; break;
        case APP_MODE_PLAYER: mode_str = "PLAYER"; break;
        case APP_MODE_MSC: mode_str = "MSC"; break;
    }

    if (output_type == STATUS_OUTPUT_COMPACT) {
        printf("[app] mode=%s\n", mode_str);
    } else {
        printf("Application Status:\n");
        printf("  Mode: %s\n", mode_str);
        printf("  Free heap: %lu bytes\n", (unsigned long)esp_get_free_heap_size());

        if (output_type == STATUS_OUTPUT_VERBOSE) {
            printf("  Min free heap: %lu bytes\n", (unsigned long)esp_get_minimum_free_heap_size());
            printf("  Uptime: %" PRId64 " s\n", esp_timer_get_time() / 1000000);

            // Configuration source
            const char *config_src = "NONE";
            switch (s_app_state.config_source) {
                case CONFIG_SOURCE_FIRMWARE: config_src = "SPIFFS"; break;
                case CONFIG_SOURCE_SDCARD: config_src = "SD Card"; break;
                default: break;
            }
            printf("  Config source: %s\n", config_src);
        }
    }
}

/* ============================================================================
 * Application Entry Point
 * ============================================================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "=== Soundboard Starting ===");
    set_loglevels();

    // -------------------------------------------------------------------------
    // Phase 1: Display (early init for startup screen)
    // -------------------------------------------------------------------------
    init_display(&s_app_state.oled);  // NON-FATAL
    if (s_app_state.oled != NULL) {
        display_show_startup(s_app_state.oled);
    }

    // -------------------------------------------------------------------------
    // Phase 2: Storage subsystems
    // -------------------------------------------------------------------------
    if (init_spiffs() != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: SPIFFS required for fallback config");
        return;
    }

    if (init_nvs() != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: NVS required for volume persistence");
        return;
    }

    init_sd_card(&s_app_state.sdcard);  // NON-FATAL

    // -------------------------------------------------------------------------
    // Phase 3: USB subsystem
    // -------------------------------------------------------------------------
    init_msc(&s_app_state.msc);  // NON-FATAL

    // -------------------------------------------------------------------------
    // Phase 4: Audio subsystem
    // -------------------------------------------------------------------------
    if (init_player(&s_app_state.player) != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: Player required for audio playback");
        return;
    }

    if (init_mapper(s_app_state.player, &s_app_state.mapper, &s_app_state.config_source) != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: Mapper required for button mappings");
        return;
    }

    // -------------------------------------------------------------------------
    // Phase 5: Input subsystem
    // -------------------------------------------------------------------------
    if (init_input_scanner(s_app_state.mapper, &s_app_state.input_scanner) != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: Input scanner required for user interaction");
        return;
    }

    // -------------------------------------------------------------------------
    // Phase 6: Debug/utility subsystems
    // -------------------------------------------------------------------------
    init_console(&s_app_state);  // NON-FATAL

    // -------------------------------------------------------------------------
    // Initialization complete - enter player mode
    // -------------------------------------------------------------------------
    display_show_idle(s_app_state.oled);
    app_set_mode(APP_MODE_PLAYER);
    ESP_LOGI(TAG, "=== Soundboard Ready ===");

    // Print compact status of all modules after init
    ESP_LOGI(TAG, "=== System Status ===");
    app_print_status(STATUS_OUTPUT_COMPACT);
    sd_card_print_status(STATUS_OUTPUT_COMPACT);
    persistent_volume_print_status(STATUS_OUTPUT_COMPACT);
    display_print_status(s_app_state.oled, STATUS_OUTPUT_COMPACT);
    input_scanner_print_status(s_app_state.input_scanner, STATUS_OUTPUT_COMPACT);
    mapper_print_status(s_app_state.mapper, STATUS_OUTPUT_COMPACT);
    player_print_status(s_app_state.player, STATUS_OUTPUT_COMPACT);
    msc_print_status(s_app_state.msc, STATUS_OUTPUT_COMPACT);
    ESP_LOGI(TAG, "=====================");

    // -------------------------------------------------------------------------
    // Main event loop - wait for MSC notifications
    // -------------------------------------------------------------------------
    while (1) {
        uint32_t notify_value = 0;
        xTaskNotifyWait(0, 0xFFFFFFFF, &notify_value, pdMS_TO_TICKS(1000));

        if (notify_value & MSC_NOTIFY_DISCONNECTED) {
            ESP_LOGW(TAG, "MSC device disconnected - rebooting...");
            display_show_reboot(s_app_state.oled);
            esp_restart();
        }

        if (notify_value & MSC_NOTIFY_CONNECTED) {
            if (s_app_state.mode == APP_MODE_MSC) {
                ESP_LOGW(TAG, "Already in MSC mode - ignoring connection event");
                continue;
            }
            ESP_LOGI(TAG, "MSC device connected");
            app_set_mode(APP_MODE_MSC);
        }
    }
}
