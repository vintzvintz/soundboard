/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"  // IWYU pragma: keep
#include "lcdgfx.h"             // IWYU pragma: keep
#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "display.h"

static const char *TAG = "display";

#define UPDATE_OLED(dest, src) do { \
    strncpy((dest), (src), sizeof(dest) - 1); \
    (dest)[sizeof(dest) - 1] = '\0'; \
} while (0)

// Display update message types
typedef enum : uint8_t {
    DISPLAY_MSG_STARTUP,            // Show startup layout
    DISPLAY_MSG_IDLE,               // Show idle layout
    DISPLAY_MSG_PLAYING,            // Playback started/stopped
    DISPLAY_MSG_VOLUME,             // Volume changed
    DISPLAY_MSG_PAGE_CHANGED,       // Page changed
    DISPLAY_MSG_ENCODER_MODE,       // Encoder mode changed (page/volume)
    DISPLAY_MSG_REBOOT,             // Show reboot layout
    DISPLAY_MSG_ERROR,              // Show error layout
    DISPLAY_MSG_MSC_ANALYSIS,       // Show MSC analysis layout
    DISPLAY_MSG_MSC_PROGRESS,       // Show MSC progress layout
    DISPLAY_MSG_MSC_MENU,           // Show MSC interactive menu
    DISPLAY_MSG_MSC_SD_CLEAR_CONFIRM, // Show MSC SD clear confirmation
} display_msg_type_t;

// Display update message
typedef struct {
    display_msg_type_t type;
    union {
        struct {
            char filename[64];  // Empty string means stop
            uint16_t progress;  // 0 = start, UINT16_MAX = end
        } playback;
        struct {
            int volume_index;
        } volume;
        struct {
            char page_id[32];
        } page;
        struct {
            bool is_page_mode;
        } encoder_mode;
        struct {
            char message[64];
        } error;
        struct {
            char status_msg[64];
        } msc_analysis;
        struct {
            char filename[64];
            uint16_t progress;  // 0 = start, UINT16_MAX = end
        } msc_progress;
        struct {
            int selected_index;
        } msc_menu;
    } data;
} display_msg_t;

// Current layout being displayed
typedef enum : uint8_t {
    LAYOUT_STARTUP,
    LAYOUT_IDLE,
    LAYOUT_PAGE_SELECT,
    LAYOUT_PLAYING,
    LAYOUT_MSC_ANALYSIS,
    LAYOUT_MSC_PROGRESS,
    LAYOUT_MSC_MENU,
    LAYOUT_MSC_SD_CLEAR_CONFIRM,
    LAYOUT_REBOOT,
    LAYOUT_ERROR,
} display_layout_t;

// display width used for progress bars
#define DISPLAY_WIDTH 128  // pixels

/**
 * @brief Display module state
 */
struct display_state_s {
    DisplaySSD1306_128x64_I2C *display;        // lcdgfx display object
    display_config_t config;                   // Display configuration

    // Async task infrastructure
    QueueHandle_t msg_queue;                   // Message queue for display updates
    TaskHandle_t task_handle;                  // Display task handle

    // Current layout
    display_layout_t current_layout;           // Which layout is currently displayed

    char full_filename[64];                    // Current playback filename
    uint16_t player_progress;                   // current player progression
    char msc_status_msg[64];                   // MSC analysis status message
    int volume_index;                          // Current volume index
    char current_page[32];                     // Current page string identifier
    char error_message[64];                    // Error message to display
    uint16_t msc_progress;                     // MSC sync progress (fullscale uint16)
    int msc_menu_selected;                     // MSC menu selected item index (0-2)
};

// helpers for progress bars
static inline uint16_t progress_to_pixels(uint16_t width, uint16_t progress) {
    return ((uint32_t)progress * width) / UINT16_MAX;
}

static inline uint16_t progress_to_pct(uint16_t progress) {
    return ((uint32_t)progress * 100) / UINT16_MAX;
}

/**
 * @brief Draw a progress bar fill at the given y position
 *
 * Draws inside a frame drawn with drawRect(0, y, 127, y+7).
 * The fill area is (2, y+2) to (125, y+5) with 2px inset.
 *
 * @param oled     Display handle
 * @param y        Top y coordinate of the progress bar frame
 * @param progress Progress value (0 to UINT16_MAX)
 */
static void draw_progress_bar_fill(display_handle_t oled, int y, uint16_t progress)
{
    uint16_t fill_width = progress_to_pixels(DISPLAY_WIDTH - 4, progress);
    if (fill_width > 0) {
        oled->display->setColor(0xFF);
        oled->display->fillRect(2, y + 2, 2 + fill_width - 1, y + 5);
    }
    if (fill_width < DISPLAY_WIDTH - 4) {
        oled->display->setColor(0x00);
        oled->display->fillRect(2 + fill_width, y + 2, 125, y + 5);
    }
    oled->display->setColor(0xFF);
}

/**
 * @brief Extract filename without path and extension
 *
 * @param full_path Full path like "/sdcard/sound1.wav"
 * @param out_buffer Output buffer for filename
 * @param out_size Size of output buffer
 */
static void extract_filename(const char *full_path, char *out_buffer, size_t out_size)
{
    if (!full_path || !out_buffer || out_size == 0) {
        return;
    }

    // Find last '/' to extract filename
    const char *filename = strrchr(full_path, '/');
    if (filename) {
        filename++;  // Skip the '/'
    } else {
        filename = full_path;  // No path separator found
    }

    // Copy filename to buffer
    strncpy(out_buffer, filename, out_size - 1);
    out_buffer[out_size - 1] = '\0';

    // Remove extension by finding last '.'
    char *dot = strrchr(out_buffer, '.');
    if (dot) {
        *dot = '\0';
    }
}


// =============================================================================
// Layout rendering functions
// =============================================================================

/**
 * @brief Render startup layout
 *
 * Line 1 (8x16): "Wait USB device"
 */
static void layout_startup(display_handle_t oled)
{
    if (oled == NULL) return;

    oled->display->clear();
    oled->display->setFixedFont(ssd1306xled_font8x16);
    oled->display->printFixed(0, 8, "Starting...", STYLE_NORMAL);

    oled->current_layout = LAYOUT_STARTUP;
    ESP_LOGD(TAG, "Layout: startup");
}

/**
 * @brief Render UAC idle layout with selective refresh
 *
 * Line 1 (8x16, y=0-15): Page name
 * Line 2 (8x16, y=24-39): "Volume [N]"
 *
 * Compares new values against stored state. Only redraws areas that changed.
 *
 * @param page      Page name to display
 * @param volume_index Volume level to display
 * @param force     Force full redraw regardless of changes
 */
static void layout_idle(display_handle_t oled, const char *page, int volume_index, bool force)
{
    if (oled == NULL) return;

    bool full_refresh = force || (oled->current_layout != LAYOUT_IDLE);

    bool page_changed = (strcmp(page, oled->current_page) != 0);
    bool volume_changed = (oled->volume_index != volume_index);

    if (full_refresh) {
        oled->display->clear();

        // Line 1: Page (large font)
        oled->display->setFixedFont(ssd1306xled_font8x16);
        oled->display->printFixed(0, 0, page, STYLE_NORMAL);

        // Line 2: Volume (large font)
        char vol_str[20];
        snprintf(vol_str, sizeof(vol_str), "Volume %d", volume_index);
        oled->display->printFixed(0, 24, vol_str, STYLE_NORMAL);
    } else {
        // Selective refresh: only areas where values changed

        // Page name (y=0-15, 8x16 font)
        if (page_changed) {
            oled->display->setColor(0x00);
            oled->display->fillRect(0, 0, 127, 15);
            oled->display->setColor(0xFF);
            oled->display->setFixedFont(ssd1306xled_font8x16);
            oled->display->printFixed(0, 0, page, STYLE_NORMAL);
        }

        // Volume (y=24-39, 8x16 font)
        if (volume_changed) {
            oled->display->setColor(0x00);
            oled->display->fillRect(0, 24, 127, 39);
            oled->display->setColor(0xFF);
            oled->display->setFixedFont(ssd1306xled_font8x16);
            char vol_str[20];
            snprintf(vol_str, sizeof(vol_str), "Volume %d", volume_index);
            oled->display->printFixed(0, 24, vol_str, STYLE_NORMAL);
        }
    }

    UPDATE_OLED(oled->current_page, page);
    oled->volume_index = volume_index;
    oled->current_layout = LAYOUT_IDLE;
    ESP_LOGD(TAG, "Layout: UAC idle (Page=%s, Vol=%d, full=%d)",
             page, volume_index, full_refresh);
}

/**
 * @brief Render page select layout
 *
 * Line 1 (8x16, y=0): "Page Select"
 * Line 2 (8x16, y=24): current page name
 */
static void layout_page_select(display_handle_t oled, const char *page, bool force)
{
    if (oled == NULL) return;

    bool full_refresh = force || (oled->current_layout != LAYOUT_PAGE_SELECT);
    bool page_changed = (strcmp(page, oled->current_page) != 0);

    if (full_refresh) {
        oled->display->clear();

        // Line 1: "Page Select" in inverse video (white bg, black text)
        // lcdgfx 1-bit text rendering XORs font data with m_bgColor,
        // so setBackground(0xFF) inverts text pixels to black-on-white
        oled->display->setFixedFont(ssd1306xled_font8x16);
        oled->display->setColor(0xFF);
        oled->display->fillRect(0, 0, 127, 15);
        oled->display->setBackground(0xFF);
        oled->display->printFixed(0, 0, "Page Select", STYLE_NORMAL);
        oled->display->setBackground(0x00);

        // Line 2: current page name (large font)
        oled->display->printFixed(0, 24, page, STYLE_NORMAL);
    } else if (page_changed) {
        // Selective refresh: only the page name area (y=24-39)
        oled->display->setColor(0x00);
        oled->display->fillRect(0, 24, 127, 39);
        oled->display->setColor(0xFF);
        oled->display->setFixedFont(ssd1306xled_font8x16);
        oled->display->printFixed(0, 24, page, STYLE_NORMAL);
    }

    UPDATE_OLED(oled->current_page, page);
    oled->current_layout = LAYOUT_PAGE_SELECT;
    ESP_LOGD(TAG, "Layout: page select (page=%s)", page);
}

/**
 * @brief Render UAC playing layout
 *
 * Line 1 (8x16, y=0): "Playing"
 * Line 2-3 (6x8, y=24,32): filename (wraps to line 3 if too long)
 */
static void layout_playing(display_handle_t oled, const char *filename, uint16_t progress, bool force)
{
    if (oled == NULL) return;

    bool full_refresh = force || (oled->current_layout != LAYOUT_PLAYING);

    // Extract filename without path and extension
    char short_name[48];
    extract_filename(filename, short_name, sizeof(short_name));

    // Detect progress bar pixel-level change
    uint16_t cur_bar = progress_to_pixels(DISPLAY_WIDTH - 4, oled->player_progress);
    uint16_t new_bar = progress_to_pixels(DISPLAY_WIDTH - 4, progress);
    bool progress_changed = (cur_bar != new_bar);

    if (full_refresh) {
        oled->display->clear();

        // Line 1: "Playing" (large font, y=0-15)
        oled->display->setFixedFont(ssd1306xled_font8x16);
        oled->display->printFixed(0, 0, "Playing", STYLE_NORMAL);

        // Progress bar frame (y=16-23)
        oled->display->drawRect(0, 16, 127, 23);

        // Filename (small font, y=32 onwards)
        oled->display->setFixedFont(ssd1306xled_font6x8);
        size_t name_len = strlen(short_name);
        const size_t max_chars_per_line = 21;
        if (name_len <= max_chars_per_line) {
            oled->display->printFixed(0, 32, short_name, STYLE_NORMAL);
        } else {
            char line1[24];
            char line2[24];
            strncpy(line1, short_name, max_chars_per_line);
            line1[max_chars_per_line] = '\0';
            strncpy(line2, short_name + max_chars_per_line, max_chars_per_line);
            line2[max_chars_per_line] = '\0';
            oled->display->printFixed(0, 32, line1, STYLE_NORMAL);
            oled->display->printFixed(0, 40, line2, STYLE_NORMAL);
        }

        // Force progress bar fill on first draw
        progress_changed = true;
    }

    // Update progress bar fill (inside frame at y=16)
    if (progress_changed) {
        draw_progress_bar_fill(oled, 16, progress);
    }

    UPDATE_OLED(oled->full_filename, filename);
    oled->player_progress = progress;
    oled->current_layout = LAYOUT_PLAYING;
    ESP_LOGD(TAG, "Layout: UAC playing (%s, %d%%)", short_name, progress_to_pct(progress));
}

/**
 * @brief Render MSC analysis layout
 *
 * Line 1 (8x16): "Checking data"
 * Line 2 (6x8):  status message
 */
static void layout_msc_analysis(display_handle_t oled, const char *status_msg, bool force)
{
    if (oled == NULL) return;

    bool full_refresh = force || (oled->current_layout != LAYOUT_MSC_ANALYSIS);
    bool msg_changed = (strcmp(status_msg, oled->msc_status_msg) != 0);

    if (full_refresh) {
        oled->display->clear();
        oled->display->setFixedFont(ssd1306xled_font8x16);
        oled->display->printFixed(0, 0, "Checking data", STYLE_NORMAL);
        oled->display->setFixedFont(ssd1306xled_font6x8);
        oled->display->printFixed(0, 24, status_msg, STYLE_NORMAL);
    } else if (msg_changed) {
        oled->display->setColor(0x00);
        oled->display->fillRect(0, 24, 127, 31);
        oled->display->setColor(0xFF);
        oled->display->setFixedFont(ssd1306xled_font6x8);
        oled->display->printFixed(0, 24, status_msg, STYLE_NORMAL);
    }

    UPDATE_OLED(oled->msc_status_msg, status_msg);
    oled->current_layout = LAYOUT_MSC_ANALYSIS;
    ESP_LOGD(TAG, "Layout: MSC analysis (%s)", status_msg);
}

/**
 * @brief Render MSC progress layout with selective refresh to avoid flicker
 *
 * Line 1 (8x16): "Updating..." (static - only drawn on layout change)
 * Line 2 (y=24-31): progress bar (selective refresh when pixel changes)
 * Line 3 (y=48, 6x8): filename (selective refresh when changed)
 *
 * @param oled Display handle
 * @param filename Current filename being copied
 * @param progress Progress value (0 to UINT16_MAX)
 */
static void layout_msc_progress(display_handle_t oled, const char *filename, uint16_t progress, bool force)
{
    if (oled == NULL) return;

    bool full_refresh = force || (oled->current_layout != LAYOUT_MSC_PROGRESS);

    // Detect what changed
    bool progress_changed = (progress_to_pixels(DISPLAY_WIDTH - 4, progress) !=
                             progress_to_pixels(DISPLAY_WIDTH - 4, oled->msc_progress));
    bool filename_changed = (strcmp(filename, oled->full_filename) != 0);

    // Update stored state
    UPDATE_OLED(oled->full_filename, filename);
    oled->msc_progress = progress;

    // Extract short filename for display
    char short_name[32];
    extract_filename(oled->full_filename, short_name, sizeof(short_name));

    if (full_refresh) {
        oled->display->clear();
        oled->display->setFixedFont(ssd1306xled_font8x16);
        oled->display->printFixed(0, 0, "Updating...", STYLE_NORMAL);

        // Draw progress bar frame (static)
        oled->display->drawRect(0, 24, 127, 31);

        // Force both dynamic areas on first draw
        progress_changed = true;
        filename_changed = true;
    }

    // Selective refresh: progress bar (inside frame at y=24)
    if (progress_changed) {
        draw_progress_bar_fill(oled, 24, progress);
    }

    // Selective refresh: filename
    if (filename_changed) {
        oled->display->setColor(0x00);
        oled->display->fillRect(0, 48, 127, 63);
        oled->display->setColor(0xFF);

        oled->display->setFixedFont(ssd1306xled_font6x8);
        oled->display->printFixed(0, 48, short_name, STYLE_NORMAL);
    }

    oled->current_layout = LAYOUT_MSC_PROGRESS;
    ESP_LOGD(TAG, "Layout: MSC progress (%d%%, %s, full=%d)",
             progress_to_pct(progress), short_name, full_refresh);
}

/**
 * @brief Render MSC interactive menu layout
 *
 * Line 1 (8x16, y=0):  "USB Update"
 * Line 2 (6x8,  y=24): " Full update"        or "> Full update"
 * Line 3 (6x8,  y=32): " Incremental"         or "> Incremental"
 * Line 4 (6x8,  y=40): " Clear SD card"       or "> Clear SD card"
 *
 * Selected item is prefixed with ">" indicator.
 */
static void layout_msc_menu(display_handle_t oled, int selected, bool force)
{
    if (oled == NULL) return;

    static const char *menu_items[] = {
        "Full update",
        "Incremental",
        "Clear SD card",
    };
    static const int menu_count = 3;

    bool full_refresh = force || (oled->current_layout != LAYOUT_MSC_MENU);
    bool selection_changed = (oled->msc_menu_selected != selected);

    if (full_refresh) {
        oled->display->clear();

        // Title
        oled->display->setFixedFont(ssd1306xled_font8x16);
        oled->display->printFixed(0, 0, "USB Update", STYLE_NORMAL);

        // Menu items (6x8 font, 8px spacing, starting below 8x16 title)
        oled->display->setFixedFont(ssd1306xled_font6x8);
        for (int i = 0; i < menu_count; i++) {
            char line[24];
            snprintf(line, sizeof(line), "%c %s",
                     (i == selected) ? '>' : ' ',
                     menu_items[i]);
            oled->display->printFixed(0, 24 + (i * 8), line, STYLE_NORMAL);
        }
    } else if (selection_changed) {
        // Selective refresh: only redraw the two affected menu items
        oled->display->setFixedFont(ssd1306xled_font6x8);

        // Clear and redraw old selected item (remove '>')
        int old_y = 24 + (oled->msc_menu_selected * 8);
        oled->display->setColor(0x00);
        oled->display->fillRect(0, old_y, 127, old_y + 7);
        oled->display->setColor(0xFF);
        char line_old[24];
        snprintf(line_old, sizeof(line_old), "  %s", menu_items[oled->msc_menu_selected]);
        oled->display->printFixed(0, old_y, line_old, STYLE_NORMAL);

        // Clear and redraw new selected item (add '>')
        int new_y = 24 + (selected * 8);
        oled->display->setColor(0x00);
        oled->display->fillRect(0, new_y, 127, new_y + 7);
        oled->display->setColor(0xFF);
        char line_new[24];
        snprintf(line_new, sizeof(line_new), "> %s", menu_items[selected]);
        oled->display->printFixed(0, new_y, line_new, STYLE_NORMAL);
    }

    oled->msc_menu_selected = selected;
    oled->current_layout = LAYOUT_MSC_MENU;
    ESP_LOGD(TAG, "Layout: MSC menu (selected=%d)", selected);
}

/**
 * @brief Render MSC SD card clear confirmation layout
 *
 * Line 1 (8x16, y=0):  "Erase SD card?"
 * Line 3 (6x8,  y=24): "Bottom row btn: confirm"
 * Line 4 (6x8,  y=40): "Other: cancel"
 */
static void layout_msc_sd_clear_confirm(display_handle_t oled)
{
    if (oled == NULL) return;

    oled->display->clear();

    oled->display->setFixedFont(ssd1306xled_font8x16);
    oled->display->printFixed(0, 0, "Erase SDcard ?", STYLE_NORMAL);

    oled->display->setFixedFont(ssd1306xled_font6x8);
    oled->display->printFixed(0, 24, "Red buttons: YES", STYLE_NORMAL);
    oled->display->printFixed(0, 40, "Other:    CANCEL", STYLE_NORMAL);

    oled->current_layout = LAYOUT_MSC_SD_CLEAR_CONFIRM;
    ESP_LOGD(TAG, "Layout: MSC SD clear confirm");
}

/**
 * @brief Render reboot layout
 *
 * Centered (8x16): "Rebooting..."
 */
static void layout_reboot(display_handle_t oled)
{
    if (oled == NULL) return;

    oled->display->clear();
    oled->display->setFixedFont(ssd1306xled_font8x16);
    // Centered horizontally: (128 - 12*8) / 2 = 16
    oled->display->printFixed(16, 24, "Rebooting...", STYLE_NORMAL);

    oled->current_layout = LAYOUT_REBOOT;
    ESP_LOGD(TAG, "Layout: reboot");
}

/**
 * @brief Render error layout
 *
 * Line 1 (8x16): "Error"
 * Line 2 (6x8):  error message
 */
static void layout_error(display_handle_t oled, const char *message)
{
    if (oled == NULL) return;

    oled->display->clear();
    oled->display->setFixedFont(ssd1306xled_font8x16);
    oled->display->printFixed(0, 0, "Error", STYLE_NORMAL);
    oled->display->setFixedFont(ssd1306xled_font6x8);
    oled->display->printFixed(0, 24, message, STYLE_NORMAL);

    UPDATE_OLED(oled->error_message, message);
    oled->current_layout = LAYOUT_ERROR;
    ESP_LOGD(TAG, "Layout: error (%s)", message);
}

/**
 * @brief Display task - processes update messages from queue
 *
 * Low priority task that blocks on message queue and updates display asynchronously
 */
static void display_task(void *arg)
{
    display_handle_t oled = (display_handle_t)arg;
    display_msg_t msg;

    ESP_LOGI(TAG, "Display task started");

    while (1) {
        // Block waiting for messages
        if (xQueueReceive(oled->msg_queue, &msg, portMAX_DELAY) == pdTRUE) {
            // Process message and render appropriate layout
            switch (msg.type) {
                case DISPLAY_MSG_STARTUP:
                    layout_startup(oled);
                    break;

                case DISPLAY_MSG_IDLE:
                    layout_idle(oled, oled->current_page, oled->volume_index, true);
                    break;

                case DISPLAY_MSG_PLAYING: {
                    bool is_stop = (msg.data.playback.filename[0] == '\0');
                    if (is_stop) {
                        oled->full_filename[0] = '\0';
                    }
                    if (oled->current_layout != LAYOUT_PAGE_SELECT) {
                        if (is_stop) {
                            layout_idle(oled, oled->current_page, oled->volume_index, true);
                        } else {
                            layout_playing(oled, msg.data.playback.filename,
                                           msg.data.playback.progress, false);
                        }
                    }
                    break;
                }

                case DISPLAY_MSG_VOLUME:
                    if (oled->current_layout == LAYOUT_IDLE) {
                        layout_idle(oled, oled->current_page, msg.data.volume.volume_index, false);
                    } else {
                        oled->volume_index = msg.data.volume.volume_index;
                    }
                    break;

                case DISPLAY_MSG_PAGE_CHANGED:
                    if (oled->current_layout == LAYOUT_IDLE) {
                        layout_idle(oled, msg.data.page.page_id, oled->volume_index, false);
                    } else if (oled->current_layout == LAYOUT_PAGE_SELECT) {
                        layout_page_select(oled, msg.data.page.page_id, false);
                    } else {
                        UPDATE_OLED(oled->current_page, msg.data.page.page_id);
                    }
                    break;

                case DISPLAY_MSG_ENCODER_MODE:
                    if (msg.data.encoder_mode.is_page_mode) {
                        layout_page_select(oled, oled->current_page, true);
                    } else {
                        if (oled->full_filename[0] != '\0') {
                            layout_playing(oled, oled->full_filename, oled->player_progress, true);
                        } else {
                            layout_idle(oled, oled->current_page, oled->volume_index, true);
                        }
                    }
                    break;

                case DISPLAY_MSG_REBOOT:
                    layout_reboot(oled);
                    break;

                case DISPLAY_MSG_ERROR:
                    layout_error(oled, msg.data.error.message);
                    break;

                case DISPLAY_MSG_MSC_ANALYSIS:
                    layout_msc_analysis(oled, msg.data.msc_analysis.status_msg, false);
                    break;

                case DISPLAY_MSG_MSC_PROGRESS:
                    layout_msc_progress(oled, msg.data.msc_progress.filename,
                                        msg.data.msc_progress.progress, false);
                    break;

                case DISPLAY_MSG_MSC_MENU:
                    layout_msc_menu(oled, msg.data.msc_menu.selected_index, false);
                    break;

                case DISPLAY_MSG_MSC_SD_CLEAR_CONFIRM:
                    layout_msc_sd_clear_confirm(oled);
                    break;

                default:
                    ESP_LOGW(TAG, "Unknown message type: %d", msg.type);
                    break;
            }
        }
    }
}

//
// Event-driven API functions
//

extern "C" void display_show_startup(display_handle_t oled)
{
    if (oled == NULL || oled->msg_queue == NULL) {
        return;
    }

    display_msg_t msg = {
        .type = DISPLAY_MSG_STARTUP,
        .data = {}
    };

    if (xQueueSend(oled->msg_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Display queue full, dropped startup message");
    }
}

extern "C" void display_show_idle(display_handle_t oled)
{
    if (oled == NULL || oled->msg_queue == NULL) {
        return;
    }

    display_msg_t msg = {
        .type = DISPLAY_MSG_IDLE,
        .data = {}
    };

    if (xQueueSend(oled->msg_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Display queue full, dropped UAC idle message");
    }
}

extern "C" void display_on_volume_changed(display_handle_t oled, int volume_index)
{
    if (oled == NULL || oled->msg_queue == NULL) {
        return;
    }

    display_msg_t msg = {
        .type = DISPLAY_MSG_VOLUME,
        .data = {
            .volume = {
                .volume_index = volume_index
            }
        }
    };

    if (xQueueSend(oled->msg_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Display queue full, dropped volume update");
    }
}

extern "C" void display_on_playing(display_handle_t oled, const char *filename, uint16_t progress)
{
    if (oled == NULL || oled->msg_queue == NULL) {
        return;
    }

    display_msg_t msg = {
        .type = DISPLAY_MSG_PLAYING,
        .data = {
            .playback = {
                .filename = {0},
                .progress = progress
            }
        }
    };

    if (filename != NULL) {
        strncpy(msg.data.playback.filename, filename,
                sizeof(msg.data.playback.filename) - 1);
        msg.data.playback.filename[sizeof(msg.data.playback.filename) - 1] = '\0';
    }

    if (xQueueSend(oled->msg_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Display queue full, dropped playback update");
    }
}

extern "C" void display_on_page_changed(display_handle_t oled, const char *page_id)
{
    if (oled == NULL || oled->msg_queue == NULL) {
        return;
    }

    display_msg_t msg = {
        .type = DISPLAY_MSG_PAGE_CHANGED,
        .data = {
            .page = {
                .page_id = {0}
            }
        }
    };

    if (page_id != NULL) {
        strncpy(msg.data.page.page_id, page_id,
                sizeof(msg.data.page.page_id) - 1);
        msg.data.page.page_id[sizeof(msg.data.page.page_id) - 1] = '\0';
    }

    if (xQueueSend(oled->msg_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Display queue full, dropped page change update");
    }
}

extern "C" void display_on_encoder_mode_changed(display_handle_t oled, bool is_page_mode)
{
    if (oled == NULL || oled->msg_queue == NULL) {
        return;
    }

    display_msg_t msg = {
        .type = DISPLAY_MSG_ENCODER_MODE,
        .data = {
            .encoder_mode = {
                .is_page_mode = is_page_mode
            }
        }
    };

    if (xQueueSend(oled->msg_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Display queue full, dropped encoder mode message");
    }
}

extern "C" void display_show_reboot(display_handle_t oled)
{
    if (oled == NULL || oled->msg_queue == NULL) {
        return;
    }

    display_msg_t msg = {
        .type = DISPLAY_MSG_REBOOT,
        .data = {}  // No payload for reboot
    };

    if (xQueueSend(oled->msg_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Display queue full, dropped reboot message");
    }
}

extern "C" void display_on_error(display_handle_t oled, const char *message)
{
    if (oled == NULL || oled->msg_queue == NULL) {
        return;
    }

    display_msg_t msg = {
        .type = DISPLAY_MSG_ERROR,
        .data = {
            .error = {
                .message = {0}
            }
        }
    };

    if (message != NULL) {
        strncpy(msg.data.error.message, message, sizeof(msg.data.error.message) - 1);
        msg.data.error.message[sizeof(msg.data.error.message) - 1] = '\0';
    }

    if (xQueueSend(oled->msg_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Display queue full, dropped error message");
    }
}

extern "C" void display_on_msc_analysis(display_handle_t oled, const char *status_msg)
{
    if (oled == NULL || oled->msg_queue == NULL) {
        return;
    }

    display_msg_t msg = {
        .type = DISPLAY_MSG_MSC_ANALYSIS,
        .data = {
            .msc_analysis = {
                .status_msg = {0}
            }
        }
    };

    if (status_msg != NULL) {
        strncpy(msg.data.msc_analysis.status_msg, status_msg,
                sizeof(msg.data.msc_analysis.status_msg) - 1);
        msg.data.msc_analysis.status_msg[sizeof(msg.data.msc_analysis.status_msg) - 1] = '\0';
    }

    if (xQueueSend(oled->msg_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Display queue full, dropped MSC analysis message");
    }
}


extern "C" void display_on_msc_menu(display_handle_t oled, int selected_index)
{
    if (oled == NULL || oled->msg_queue == NULL) {
        return;
    }

    display_msg_t msg = {
        .type = DISPLAY_MSG_MSC_MENU,
        .data = {
            .msc_menu = {
                .selected_index = selected_index
            }
        }
    };

    if (xQueueSend(oled->msg_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Display queue full, dropped MSC menu message");
    }
}

extern "C" void display_on_msc_sd_clear_confirm(display_handle_t oled)
{
    if (oled == NULL || oled->msg_queue == NULL) {
        return;
    }

    display_msg_t msg = {
        .type = DISPLAY_MSG_MSC_SD_CLEAR_CONFIRM,
        .data = {}
    };

    if (xQueueSend(oled->msg_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Display queue full, dropped MSC SD clear confirm message");
    }
}

extern "C" void display_on_msc_progress(display_handle_t oled, const char *filename, uint16_t progress)
{
    if (oled == NULL || oled->msg_queue == NULL) {
        return;
    }

    display_msg_t msg = {
        .type = DISPLAY_MSG_MSC_PROGRESS,
        .data = {
            .msc_progress = {
                .filename = {0},
                .progress = progress
            }
        }
    };

    if (filename != NULL) {
        strncpy(msg.data.msc_progress.filename, filename,
                sizeof(msg.data.msc_progress.filename) - 1);
        msg.data.msc_progress.filename[sizeof(msg.data.msc_progress.filename) - 1] = '\0';
    }

    if (xQueueSend(oled->msg_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Display queue full, dropped MSC progress message");
    }
}

extern "C" esp_err_t display_init(const display_config_t *config, display_handle_t *display_handle)
{
    // Validate output parameter
    if (display_handle == NULL) {
        ESP_LOGE(TAG, "display_handle parameter cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // allocate internal state struct
    display_handle_t oled = (display_handle_t)heap_caps_calloc(1, sizeof(struct display_state_s), MALLOC_CAP_8BIT);
    if(oled == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate OLED display");
        return ESP_ERR_NO_MEM;
    }

    // Use default config if none provided
    if (config == NULL) {
        ESP_LOGE(TAG, "display_config parameter cannot be NULL");
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(&oled->config, config, sizeof(display_config_t));

    // Validate GPIO pins
    // NOLINTNEXTLINE(readability-simplify-boolean-expr) -- macro expansion
    if (!(GPIO_IS_VALID_GPIO(oled->config.sda_gpio) &&
          GPIO_IS_VALID_GPIO(oled->config.scl_gpio))) {
        ESP_LOGE(TAG, "Invalid GPIO pins: SDA=%d, SCL=%d",
                 oled->config.sda_gpio,
                 oled->config.scl_gpio);
        free(oled);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initializing I2C display on SDA=%d, SCL=%d, addr=0x%02X, freq=%lu Hz",
             oled->config.sda_gpio,
             oled->config.scl_gpio,
             oled->config.i2c_address,
             oled->config.i2c_freq_hz);

    // Create display object with I2C parameters
    // Parameters: RST pin, {busId, addr, scl, sda, frequency}
    oled->display = new DisplaySSD1306_128x64_I2C(
        -1,  // RST pin not used
        {
            -1,  // busId (-1 defaults to I2C_NUM_1 on ESP32)
            oled->config.i2c_address,
            static_cast<int8_t>(oled->config.scl_gpio),
            static_cast<int8_t>(oled->config.sda_gpio),
            oled->config.i2c_freq_hz
        }
    );

    if (oled->display == NULL) {
        ESP_LOGE(TAG, "Failed to create display object");
        free(oled);
        return ESP_ERR_NO_MEM;
    }

    // Initialize display
    oled->display->begin();
    oled->display->clear();
    oled->display->getInterface().flipHorizontal(1);
    oled->display->getInterface().flipVertical(1);

    // Initialize state
    oled->current_layout = LAYOUT_STARTUP;
    oled->volume_index = 0;
    memset(oled->current_page, 0, sizeof(oled->current_page));
    memset(oled->full_filename, 0, sizeof(oled->full_filename));
    memset(oled->msc_status_msg, 0, sizeof(oled->msc_status_msg));
    memset(oled->error_message, 0, sizeof(oled->error_message));
    oled->msc_progress = 0;
    oled->player_progress = 0;
    oled->msc_menu_selected = 0;

    // Draw initial startup screen
    oled->display->setFixedFont(ssd1306xled_font8x16);
    oled->display->printFixed(0, 8, "Wait USB device", STYLE_NORMAL);

    // Create message queue (depth=10, large enough for burst updates)
    oled->msg_queue = xQueueCreate(10, sizeof(display_msg_t));
    if (oled->msg_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create display message queue");
        // Note: Cannot delete display object due to non-virtual destructor in lcdgfx
        // delete oled->display;
        oled->display = NULL;
        free(oled);
        return ESP_ERR_NO_MEM;
    }

    // Create display task (low priority = 1, stack size = 3KB)
    // Pin to core 0 (PRO_CPU) with USB tasks - low priority UI doesn't need real-time core
    BaseType_t task_created = xTaskCreatePinnedToCore(
        display_task,
        "display_task",
        3072,  // Stack size in bytes
        oled,  // Pass display handle as parameter
        1,     // Low priority (tskIDLE_PRIORITY + 1)
        &oled->task_handle,
        0      // Core 0
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create display task");
        vQueueDelete(oled->msg_queue);
        // Note: Cannot delete display object due to non-virtual destructor in lcdgfx
        // delete oled->display;
        oled->display = NULL;
        free(oled);
        return ESP_ERR_NO_MEM;
    }

    *display_handle = oled;
    ESP_LOGI(TAG, "Display initialized successfully (async mode)");
    return ESP_OK;
}

extern "C" esp_err_t display_deinit(display_handle_t oled)
{
    if (oled == NULL) {
        ESP_LOGW(TAG, "Display already deinitialized or not initialized");
        return ESP_ERR_INVALID_ARG;
    }

    // Delete display task
    if (oled->task_handle != NULL) {
        vTaskDelete(oled->task_handle);
        oled->task_handle = NULL;
    }

    // Delete message queue
    if (oled->msg_queue != NULL) {
        vQueueDelete(oled->msg_queue);
        oled->msg_queue = NULL;
    }

    // Clear and delete display object
    if (oled->display != NULL) {
        oled->display->clear();
        // deleting object of polymorphic class type 'DisplaySSD1306_128x64_I2C' which has
        // non-virtual destructor might cause undefined behavior [-Werror=delete-non-virtual-dtor]
        // delete oled->display;
        oled->display = NULL;
    }

    // Free the handle itself
    free(oled);

    ESP_LOGI(TAG, "Display deinitialized");

    return ESP_OK;
}

extern "C" void display_print_status(display_handle_t handle, status_output_type_t output_type)
{
    if (handle == NULL) {
        if (output_type == STATUS_OUTPUT_COMPACT) {
            printf("[display] not initialized\n");
        } else {
            printf("Display Status:\n");
            printf("  State: Not initialized\n");
        }
        return;
    }

    // Get layout name
    const char *layout_name = "unknown";
    switch (handle->current_layout) {
        case LAYOUT_STARTUP: layout_name = "startup"; break;
        case LAYOUT_IDLE: layout_name = "idle"; break;
        case LAYOUT_PAGE_SELECT: layout_name = "page_select"; break;
        case LAYOUT_PLAYING: layout_name = "playing"; break;
        case LAYOUT_MSC_ANALYSIS: layout_name = "msc_analysis"; break;
        case LAYOUT_MSC_PROGRESS: layout_name = "msc_progress"; break;
        case LAYOUT_MSC_MENU: layout_name = "msc_menu"; break;
        case LAYOUT_MSC_SD_CLEAR_CONFIRM: layout_name = "msc_sd_clear_confirm"; break;
        case LAYOUT_REBOOT: layout_name = "reboot"; break;
        case LAYOUT_ERROR: layout_name = "error"; break;
    }

    if (output_type == STATUS_OUTPUT_COMPACT) {
        printf("[display] initialized, layout=%s, 128x64 OLED\n", layout_name);
    } else {
        printf("Display Status:\n");
        printf("  Current layout: %s\n", layout_name);
        printf("  I2C: SDA=GPIO%d, SCL=GPIO%d\n", handle->config.sda_gpio, handle->config.scl_gpio);

        if (output_type == STATUS_OUTPUT_VERBOSE) {
            printf("  I2C address: 0x%02X\n", handle->config.i2c_address);
            printf("  I2C frequency: %" PRIu32 " Hz\n", handle->config.i2c_freq_hz);
            printf("  Playing filename: %s\n", handle->full_filename );
            printf("  Volume index: %d\n", handle->volume_index);
            printf("  Page: %s\n", handle->current_page);
        }
    }
}
