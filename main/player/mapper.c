/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

#include <ctype.h>    // for isspace()
#include <sys/stat.h>
#include "mapper.h"
#include "player.h"
#include "esp_log.h"

static const char *TAG = "mapper";


/* ============================================================================
 * Linked List Data Structures for Mappings
 * ============================================================================ */

/**
 * @brief Single mapping node in a linked list
 *
 * Contains button number, event type, and action. Linked within a page.
 */
typedef struct mapping_node_s {
    uint8_t button_number;              /**< Button number (1-12 for 4x3 matrix) */
    input_event_type_t event;           /**< Event type to trigger on */
    action_t action;                    /**< Action to execute */
    struct mapping_node_s *next;        /**< Next mapping in this page */
} mapping_node_t;

/**
 * @brief Page node in a circular doubly-linked list of pages
 *
 * Contains page string identifier and linked list of mappings for this page.
 * The pages form a circular doubly-linked list for efficient prev/next navigation.
 */
typedef struct page_node_s {
    char page_id[PAGE_ID_MAX_LEN];      /**< Page string identifier (e.g., "default", "fx") */
    uint8_t page_number;                /**< 1-based page number (for direct selection via buttons) */
    mapping_node_t *mappings;           /**< Linked list of mappings for this page */
    struct page_node_s *prev;           /**< Previous page (circular) */
    struct page_node_s *next;           /**< Next page (circular) */
} page_node_t;


/**
 * @brief Per-button FSM states
 *
 * Tracks the lifecycle of a button press from activation through release.
 * Only one button can be active at a time (tracked by current_button).
 */
typedef enum {
    BTN_STATE_INITIAL,          /**< No active playback from this button */
    BTN_STATE_PLAY_ONCE,        /**< play: let player finish on release */
    BTN_STATE_PLAY_CUT,         /**< play_cut: stop on release */
    BTN_STATE_PLAY_LOCK_PENDING,/**< play_lock: stop on release unless long-pressed */
    BTN_STATE_PLAY_LOCKED,      /**< play_lock: long-pressed, let player finish on release */
} button_fsm_state_t;

/**
 * @brief Mapper internal structure
 */
struct mapper_s {
    player_handle_t player;

    // Current page pointer (entry point into circular doubly-linked list)
    page_node_t *current_page;

    // First page pointer (page_number=1, for direct page selection)
    page_node_t *first_page;

    // Page count for display purposes
    uint8_t page_count;

    // Unified event callback
    mapper_event_cb_t event_cb;
    void *event_cb_ctx;

    // Encoder mode state (toggle between volume and page)
    encoder_mode_t encoder_mode;

    // Button FSM: single active button tracking
    button_fsm_state_t button_fsm_state;
    uint8_t current_button;                     // 0 = none, 1-12 = matrix button
    char current_filename[CONFIG_MAX_PATH_LEN]; // filename of active playback
};

/* ============================================================================
 * Linked List Management Functions
 * ============================================================================ */

/**
 * @brief Find or create a page node in circular doubly-linked list
 *
 * @param mapper Mapper handle
 * @param page_id Page string identifier
 * @return Page node pointer, or NULL on allocation failure
 */
static page_node_t *find_or_create_page(mapper_handle_t mapper, const char *page_id)
{
    // If no pages exist yet, create first page
    if (mapper->current_page == NULL) {
        page_node_t *new_page = calloc(1, sizeof(page_node_t));
        if (new_page == NULL) {
            ESP_LOGE(TAG, "Failed to allocate page node for page '%s'", page_id);
            return NULL;
        }

        strncpy(new_page->page_id, page_id, PAGE_ID_MAX_LEN - 1);
        new_page->page_id[PAGE_ID_MAX_LEN - 1] = '\0';
        new_page->page_number = 1;  // First page gets number 1
        new_page->mappings = NULL;
        new_page->prev = new_page;  // Points to self (circular)
        new_page->next = new_page;  // Points to self (circular)

        mapper->current_page = new_page;
        mapper->first_page = new_page;  // Track first page for direct selection
        mapper->page_count = 1;
        return new_page;
    }

    // Search existing pages (traverse until back to start)
    page_node_t *start = mapper->current_page;
    page_node_t *page = start;
    do {
        if (strcmp(page->page_id, page_id) == 0) {
            return page;  // Found existing
        }
        page = page->next;
    } while (page != start);

    // Create new page and insert after current
    page_node_t *new_page = calloc(1, sizeof(page_node_t));
    if (new_page == NULL) {
        ESP_LOGE(TAG, "Failed to allocate page node for page '%s'", page_id);
        return NULL;
    }

    strncpy(new_page->page_id, page_id, PAGE_ID_MAX_LEN - 1);
    new_page->page_id[PAGE_ID_MAX_LEN - 1] = '\0';
    new_page->page_number = mapper->page_count + 1;  // Assign next page number
    new_page->mappings = NULL;

    // Insert after current page (maintains load order for first page)
    new_page->next = mapper->current_page->next;
    new_page->prev = mapper->current_page;
    mapper->current_page->next->prev = new_page;
    mapper->current_page->next = new_page;

    mapper->page_count++;
    return new_page;
}

/**
 * @brief Insert a mapping into a page, overwriting if conflict exists
 *
 * Takes ownership of the mapping data (copies into new node).
 *
 * @param page Page node to insert into
 * @param button_number Button number (1-12)
 * @param event Event type
 * @param action Action to execute
 * @param source_name Source file name for logging conflicts
 * @return ESP_OK on success, ESP_ERR_NO_MEM on allocation failure
 */
static esp_err_t insert_mapping(page_node_t *page, uint8_t button_number,
                                input_event_type_t event, const action_t *action,
                                const char *source_name)
{
    // Search for existing mapping with same button+event
    mapping_node_t *mapping = page->mappings;

    while (mapping != NULL) {
        if (mapping->button_number == button_number && mapping->event == event) {
            // Overwrite existing mapping
            ESP_LOGD(TAG, "%s: overwriting mapping (page='%s', btn=%d, event=%d)",
                     source_name, page->page_id, button_number, event);
            mapping->action = *action;
            return ESP_OK;
        }
        mapping = mapping->next;
    }

    // Create new mapping node (insert at head for simplicity)
    mapping_node_t *new_mapping = calloc(1, sizeof(mapping_node_t));
    if (new_mapping == NULL) {
        ESP_LOGE(TAG, "Failed to allocate mapping node");
        return ESP_ERR_NO_MEM;
    }

    new_mapping->button_number = button_number;
    new_mapping->event = event;
    new_mapping->action = *action;
    new_mapping->next = page->mappings;
    page->mappings = new_mapping;

    return ESP_OK;
}

/**
 * @brief Free all mappings in a page
 */
static void free_page_mappings(page_node_t *page)
{
    mapping_node_t *mapping = page->mappings;
    while (mapping != NULL) {
        mapping_node_t *next = mapping->next;
        free(mapping);
        mapping = next;
    }
    page->mappings = NULL;
}

/**
 * @brief Free all pages and their mappings (circular list)
 */
static void free_all_pages(mapper_handle_t mapper)
{
    if (mapper->current_page == NULL) {
        return;
    }

    // Break the circle first to enable linear traversal
    page_node_t *start = mapper->current_page;
    page_node_t *last = start->prev;
    last->next = NULL;  // Break the circular link

    // Now free linearly from start
    page_node_t *page = start;
    while (page != NULL) {
        page_node_t *next = page->next;
        free_page_mappings(page);
        free(page);
        page = next;
    }

    mapper->current_page = NULL;
    mapper->first_page = NULL;
    mapper->page_count = 0;
}

/**
 * @brief Find page by its 1-based page number
 *
 * @param mapper Mapper handle
 * @param page_number 1-based page number (1-12)
 * @return Page node pointer, or NULL if page doesn't exist
 */
static page_node_t *find_page_by_number(mapper_handle_t mapper, uint8_t page_number)
{
    if (mapper->first_page == NULL || page_number == 0 || page_number > mapper->page_count) {
        return NULL;
    }

    // Start from first page and traverse
    page_node_t *page = mapper->first_page;
    do {
        if (page->page_number == page_number) {
            return page;
        }
        page = page->next;
    } while (page != mapper->first_page);

    return NULL;  // Should not reach here if page_count is accurate
}

/**
 * @brief Find mapping in current page
 */
static const mapping_node_t *find_mapping(mapper_handle_t mapper,
                                          uint8_t button_number,
                                          input_event_type_t event)
{
    if (mapper->current_page == NULL) {
        return NULL;
    }

    mapping_node_t *mapping = mapper->current_page->mappings;
    while (mapping != NULL) {
        if (mapping->button_number == button_number && mapping->event == event) {
            return mapping;
        }
        mapping = mapping->next;
    }

    return NULL;
}

/* ============================================================================
 * CSV Parsing Functions
 * ============================================================================ */

/**
 * @brief Check if action type uses a sound file
 */
static bool action_has_file(action_type_t type)
{
    switch (type) {
        case ACTION_TYPE_PLAY:
        case ACTION_TYPE_PLAY_CUT:
        case ACTION_TYPE_PLAY_LOCK:
            return true;
        default:
            return false;
    }
}

/**
 * @brief Parsed mapping fields (validation result, no linked-list insertion)
 */
typedef struct {
    char page_id[PAGE_ID_MAX_LEN];
    uint8_t button_number;
    input_event_type_t event;
    action_t action;
} parsed_mapping_t;

/**
 * @brief Trim leading and trailing whitespace from a string (in-place)
 */
static void trim(char *str)
{
    if (str == NULL) return;

    // Trim leading whitespace
    char *start = str;
    while (isspace((unsigned char)*start)) start++;

    // Trim trailing whitespace
    char *end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';

    // Move trimmed string to beginning if needed
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

/**
 * @brief Parse event type from string
 */
static esp_err_t parse_event_type(const char *str, input_event_type_t *out_event)
{
    if (strcmp(str, "press") == 0) {
        *out_event = INPUT_EVENT_BUTTON_PRESS;
    } else if (strcmp(str, "long_press") == 0) {
        *out_event = INPUT_EVENT_BUTTON_LONG_PRESS;
    } else if (strcmp(str, "release") == 0) {
        *out_event = INPUT_EVENT_BUTTON_RELEASE;
    } else {
        ESP_LOGE(TAG, "Unknown event type: %s (valid: press, long_press, release)", str);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/**
 * @brief Action specification structure
 */
typedef struct {
    action_type_t type;
    const char* name;
    int min_params;
    int max_params;
} action_spec_t;

static const action_spec_t action_specs[] = {
    {ACTION_TYPE_STOP,         "stop",         0, 0},
    {ACTION_TYPE_PLAY,         "play",         1, 1},
    {ACTION_TYPE_PLAY_CUT,     "play_cut",     1, 1},
    {ACTION_TYPE_PLAY_LOCK,    "play_lock",    1, 1},
};

#define NUM_ACTION_SPECS (sizeof(action_specs) / sizeof(action_specs[0]))

static const action_spec_t* find_action_spec(const char* name)
{
    for (size_t i = 0; i < NUM_ACTION_SPECS; i++) {
        if (strcmp(name, action_specs[i].name) == 0) {
            return &action_specs[i];
        }
    }
    return NULL;
}

/**
 * @brief Build absolute path from root and relative filename
 *
 * @param dest Destination buffer (must be at least CONFIG_MAX_PATH_LEN)
 * @param root Root mount point (e.g., "/sdcard")
 * @param filename Relative filename from CSV (e.g., "laser.wav" or "sounds/laser.wav")
 */
static void build_absolute_path(char *dest, const char *root, const char *filename)
{
    // Skip leading slash in filename if present (normalize to relative)
    const char *rel_filename = filename;
    while (*rel_filename == '/') {
        rel_filename++;
    }

    // Build path: root + "/" + filename
    snprintf(dest, CONFIG_MAX_PATH_LEN, "%s/%s", root, rel_filename);
}

/**
 * @brief Parse action with optional parameters
 *
 * @param tokens Array of token strings (action type followed by parameters)
 * @param num_tokens Number of tokens in array
 * @param action Output action structure
 * @param root Root path to prepend to filenames (e.g., "/sdcard")
 */
static esp_err_t parse_action(char** tokens, int num_tokens, action_t* action, const char *root)
{
    if (num_tokens < 1) {
        ESP_LOGE(TAG, "No action type provided");
        return ESP_ERR_INVALID_ARG;
    }

    const char* action_name = tokens[0];
    trim((char*)action_name);

    const action_spec_t* spec = find_action_spec(action_name);
    if (spec == NULL) {
        ESP_LOGE(TAG, "Unknown action type: %s", action_name);
        return ESP_ERR_INVALID_ARG;
    }

    int provided_params = num_tokens - 1;

    if (provided_params < spec->min_params) {
        ESP_LOGE(TAG, "Action '%s' requires at least %d params, got %d",
                 action_name, spec->min_params, provided_params);
        return ESP_ERR_INVALID_ARG;
    }
    if (provided_params > spec->max_params) {
        ESP_LOGW(TAG, "Action '%s' has %d extra params, ignoring extras",
                 action_name, provided_params - spec->max_params);
    }

    action->type = spec->type;

    switch (spec->type) {
        case ACTION_TYPE_STOP:
            break;

        case ACTION_TYPE_PLAY:
        case ACTION_TYPE_PLAY_CUT:
        case ACTION_TYPE_PLAY_LOCK:
            trim(tokens[1]);
            build_absolute_path(action->params.play.filename, root, tokens[1]);
            break;

        default:
            ESP_LOGE(TAG, "Unhandled action type: %d", spec->type);
            return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

/**
 * @brief Parse and validate a single CSV mapping line (no insertion)
 *
 * Tokenizes the line, validates all fields (page_id, button, event, action
 * with parameters), and populates the output struct. Does NOT modify any
 * mapper state or linked-list structures.
 *
 * @param line CSV line (modified in-place by strtok_r)
 * @param root Root path for building absolute file paths (e.g., "/sdcard")
 * @param out  Parsed mapping fields on success
 * @return ESP_OK if valid, ESP_ERR_INVALID_ARG on parse error
 */
static esp_err_t validate_line(char *line, const char *root, parsed_mapping_t *out)
{
    char *tokens[16];
    int num_tokens = 0;
    char *saveptr = NULL;

    char *token = strtok_r(line, ",", &saveptr);
    while (token != NULL && num_tokens < 16) {
        tokens[num_tokens++] = token;
        token = strtok_r(NULL, ",", &saveptr);
    }

    if (num_tokens < 4) {
        ESP_LOGE(TAG, "Line has only %d fields (need at least 4: page_id,button,event,action)", num_tokens);
        return ESP_ERR_INVALID_ARG;
    }

    // Parse page_id (string identifier)
    trim(tokens[0]);
    const char *page_id = tokens[0];
    if (strlen(page_id) == 0) {
        ESP_LOGE(TAG, "Empty page_id");
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(page_id) >= PAGE_ID_MAX_LEN) {
        ESP_LOGE(TAG, "Page ID '%s' too long (max %d chars)", page_id, PAGE_ID_MAX_LEN - 1);
        return ESP_ERR_INVALID_ARG;
    }

    // Parse button_number
    trim(tokens[1]);
    int button = atoi(tokens[1]);
    if (button < 1 || button > 12) {
        ESP_LOGE(TAG, "Invalid button number: %d (must be 1-12)", button);
        return ESP_ERR_INVALID_ARG;
    }

    // Parse event_type
    trim(tokens[2]);
    input_event_type_t event;
    if (parse_event_type(tokens[2], &event) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    // Parse action (pass root for building absolute paths)
    action_t action;
    int action_token_count = num_tokens - 3;
    if (parse_action(&tokens[3], action_token_count, &action, root) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    // Populate output
    strncpy(out->page_id, page_id, PAGE_ID_MAX_LEN - 1);
    out->page_id[PAGE_ID_MAX_LEN - 1] = '\0';
    out->button_number = (uint8_t)button;
    out->event = event;
    out->action = action;

    return ESP_OK;
}

/**
 * @brief Parse mapping line and insert into mapper
 *
 * Validates the line via validate_line(), then inserts into the mapper's
 * linked-list structures.
 *
 * @param mapper Mapper handle
 * @param line CSV line to parse (modified in-place by strtok_r)
 * @param root Root path to prepend to filenames (e.g., "/sdcard")
 * @param source_name Human-readable source name for logging
 */
static esp_err_t parse_and_insert_mapping(mapper_handle_t mapper, char *line,
                                          const char *root, const char *source_name)
{
    parsed_mapping_t parsed;
    esp_err_t ret = validate_line(line, root, &parsed);
    if (ret != ESP_OK) {
        return ret;
    }

    page_node_t *page_node = find_or_create_page(mapper, parsed.page_id);
    if (page_node == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = insert_mapping(page_node, parsed.button_number, parsed.event,
                         &parsed.action, source_name);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "Inserted mapping: page='%s', btn=%d, event=%d -> action=%d",
                 parsed.page_id, parsed.button_number, parsed.event, parsed.action.type);
    }

    return ret;
}

/**
 * @brief Load mappings from a single file into mapper
 *
 * Storage-agnostic loading function. Builds the full path from root and filename,
 * and uses root as the base path for resolving relative filenames in mappings.
 *
 * @param mapper Mapper handle
 * @param root Mount point (e.g., "/sdcard" or "/spiffs")
 * @param mappings_filename Mappings filename (e.g., "mappings.csv")
 * @param source_name Human-readable source name for logging
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if file not found
 */
static esp_err_t load_mappings_from_file(mapper_handle_t mapper, const char *root,
                                         const char *mappings_filename, const char *source_name)
{
    if (root == NULL || mappings_filename == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    // Build full path to mappings file
    char path[CONFIG_MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", root, mappings_filename);

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGD(TAG, "%s: file not found: %s", source_name, path);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Loading mappings from %s: %s", source_name, path);

    char line[512];
    int line_num = 0;
    int loaded_count = 0;
    esp_err_t ret = ESP_OK;

    while (fgets(line, sizeof(line), f) != NULL) {
        line_num++;

        line[strcspn(line, "\r\n")] = '\0';
        trim(line);

        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        ret = parse_and_insert_mapping(mapper, line, root, source_name);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "%s line %d: Failed to parse mapping", source_name, line_num);
            break;
        }
        loaded_count++;
    }

    fclose(f);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "%s: loaded %d mappings", source_name, loaded_count);
    }


    return ret;
}

/**
 * @brief Load and merge mappings from SPIFFS and SD card
 *
 * Loading order:
 * 1. SPIFFS file first (firmware defaults)
 * 2. SD card file second (user overrides, overwrites on conflict)
 *
 * Note: current_page is set to the first page loaded by find_or_create_page().
 * Page count is updated incrementally during loading.
 *
 * @param mapper Mapper handle
 * @param spiffs_root SPIFFS mount point (e.g., "/spiffs"), NULL to skip
 * @param spiffs_mappings_file Mappings filename in SPIFFS, NULL to skip
 * @param sdcard_root SD card mount point (e.g., "/sdcard"), NULL to skip
 * @param sdcard_mappings_file Mappings filename on SD card, NULL to skip
 */
static esp_err_t load_all_mappings(mapper_handle_t mapper,
                                   const char *spiffs_root,
                                   const char *spiffs_mappings_file,
                                   const char *sdcard_root,
                                   const char *sdcard_mappings_file)
{
    bool any_loaded = false;
    esp_err_t ret;

    // Load SPIFFS mappings first (all pages allowed)
    ret = load_mappings_from_file(mapper, spiffs_root, spiffs_mappings_file, "SPIFFS");
    if (ret == ESP_OK) {
        any_loaded = true;
    } else if (ret != ESP_ERR_NOT_FOUND) {
        return ret;  // Error other than not found
    }

    // Load SD card mappings second (overwrites on conflict)
    ret = load_mappings_from_file(mapper, sdcard_root, sdcard_mappings_file, "SD card");
    if (ret == ESP_OK) {
        any_loaded = true;
    } else if (ret != ESP_ERR_NOT_FOUND) {
        return ret;  // Error other than not found
    }

    if (!any_loaded) {
        ESP_LOGE(TAG, "No mappings loaded from either source");
        return ESP_ERR_NOT_FOUND;
    }

    // current_page is already set by find_or_create_page() to the first page loaded
    // page_count is already updated incrementally during loading

    ESP_LOGI(TAG, "Mappings loaded: %d pages, current='%s'",
             mapper->page_count,
             mapper->current_page ? mapper->current_page->page_id : "(none)");

    return ESP_OK;
}

/* ============================================================================
 * Public Validation API
 * ============================================================================ */

esp_err_t mapper_validate_file(const char *filepath, const char *root, bool check_files)
{
    FILE *f = fopen(filepath, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Validation: file not found: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }

    char line[512];
    int line_num = 0;
    int valid_count = 0;
    int error_count = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        line_num++;

        line[strcspn(line, "\r\n")] = '\0';
        trim(line);

        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        parsed_mapping_t parsed;
        esp_err_t ret = validate_line(line, root, &parsed);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Validation error at line %d", line_num);
            error_count++;
            continue;
        }

        if (check_files && action_has_file(parsed.action.type)) {
            struct stat st;
            if (stat(parsed.action.params.play.filename, &st) != 0) {
                ESP_LOGE(TAG, "Line %d: audio file not found: %s",
                         line_num, parsed.action.params.play.filename);
                error_count++;
                continue;
            }
        }

        valid_count++;
    }

    fclose(f);

    if (error_count > 0) {
        ESP_LOGE(TAG, "Validation failed: %d error(s) in %d lines", error_count, line_num);
        return ESP_ERR_INVALID_STATE;
    }

    if (valid_count == 0) {
        ESP_LOGW(TAG, "Validation: no mappings found in %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Validation passed: %d valid mappings", valid_count);
    return ESP_OK;
}

/* ============================================================================
 * Event Notification Helpers
 * ============================================================================ */

static void notify_loaded(mapper_handle_t mapper)
{
    if (mapper->event_cb == NULL) return;

    mapper_event_t evt = {
        .type = MAPPER_EVENT_LOADED,
        .loaded = {
            .page_count = mapper->page_count,
            .initial_page_id = mapper->current_page ? mapper->current_page->page_id : "",
        }
    };
    mapper->event_cb(&evt, mapper->event_cb_ctx);
}

static void notify_action_executed(mapper_handle_t mapper, uint8_t button_number,
                                   input_event_type_t event, const action_t *action)
{
    if (mapper->event_cb == NULL) return;

    mapper_event_t evt = {
        .type = MAPPER_EVENT_ACTION_EXECUTED,
        .action_executed = {
            .button_number = button_number,
            .event = event,
            .action = action,
        }
    };
    mapper->event_cb(&evt, mapper->event_cb_ctx);
}

static void notify_encoder_mode_changed(mapper_handle_t mapper, encoder_mode_t mode)
{
    if (mapper->event_cb == NULL) return;

    mapper_event_t evt = {
        .type = MAPPER_EVENT_ENCODER_MODE_CHANGED,
        .encoder_mode_changed = {
            .mode = mode,
        }
    };
    mapper->event_cb(&evt, mapper->event_cb_ctx);
}

static void notify_page_changed(mapper_handle_t mapper)
{
    if (mapper->event_cb == NULL) return;

    mapper_event_t evt = {
        .type = MAPPER_EVENT_PAGE_CHANGED,
        .page_changed = {
            .page_id = mapper->current_page ? mapper->current_page->page_id : "",
            .page_number = mapper->current_page ? mapper->current_page->page_number : 0,
            .page_count = mapper->page_count,
        }
    };
    mapper->event_cb(&evt, mapper->event_cb_ctx);
}

/**
 * @brief Preload files for current page into cache
 *
 * Collects filenames from current page's mappings and queues them for
 * background preloading. Files are queued in descending button order
 * (12, 11, ... 1) so that button 1's file loads first (FIFO queue).
 */
static void preload_current_page_files(mapper_handle_t mapper)
{
    if (mapper == NULL || mapper->current_page == NULL || mapper->player == NULL) {
        return;
    }

    page_node_t *page = mapper->current_page;

    // Collect unique filenames with their button numbers
    // Max 12 buttons, but same file might be on multiple buttons
    typedef struct {
        const char *filename;
        uint8_t button_number;
    } file_entry_t;

    file_entry_t entries[12];
    int entry_count = 0;

    // Iterate through mappings and collect unique filenames
    for (mapping_node_t *m = page->mappings; m != NULL; m = m->next) {
        if (!action_has_file(m->action.type)) {
            continue;
        }

        const char *filename = m->action.params.play.filename;

        // Check if already in list (avoid duplicates)
        bool found = false;
        for (int i = 0; i < entry_count; i++) {
            if (strcmp(entries[i].filename, filename) == 0) {
                found = true;
                // Use highest button number for this file
                if (m->button_number > entries[i].button_number) {
                    entries[i].button_number = m->button_number;
                }
                break;
            }
        }

        if (!found && entry_count < 12) {
            entries[entry_count].filename = filename;
            entries[entry_count].button_number = m->button_number;
            entry_count++;
        }
    }

    if (entry_count == 0) {
        return;
    }

    // Sort by button number descending (bubble sort, small array)
    for (int i = 0; i < entry_count - 1; i++) {
        for (int j = 0; j < entry_count - 1 - i; j++) {
            if (entries[j].button_number < entries[j + 1].button_number) {
                file_entry_t tmp = entries[j];
                entries[j] = entries[j + 1];
                entries[j + 1] = tmp;
            }
        }
    }

    // Flush stale preload requests from previous page
    player_flush_preload(mapper->player);

    // Queue files for preloading (button 12 first, button 1 last)
    // Since queue is FIFO, button 1's file will load first
    ESP_LOGI(TAG, "Preloading %d files for page '%s'", entry_count, page->page_id);
    for (int i = 0; i < entry_count; i++) {
        ESP_LOGD(TAG, "  Queue preload: btn %d -> %s",
                 entries[i].button_number, entries[i].filename);
        player_preload(mapper->player, entries[i].filename);
    }
}

/* ============================================================================
 * Action Execution
 * ============================================================================ */

static void execute_action(mapper_handle_t mapper, uint8_t button_number,
                          input_event_type_t event, const action_t *action)
{
    switch (action->type) {
        case ACTION_TYPE_STOP:
            ESP_LOGI(TAG, "Action: Stop playback");
            player_stop(mapper->player, true);
            mapper->button_fsm_state = BTN_STATE_INITIAL;
            mapper->current_button = 0;
            break;

        case ACTION_TYPE_PLAY:
            ESP_LOGI(TAG, "Action: Play '%s'", action->params.play.filename);
            player_play(mapper->player, action->params.play.filename);
            mapper->button_fsm_state = BTN_STATE_PLAY_ONCE;
            mapper->current_button = button_number;
            strncpy(mapper->current_filename, action->params.play.filename, CONFIG_MAX_PATH_LEN - 1);
            mapper->current_filename[CONFIG_MAX_PATH_LEN - 1] = '\0';
            break;

        case ACTION_TYPE_PLAY_CUT:
            ESP_LOGI(TAG, "Action: Play '%s' (cut on release)", action->params.play.filename);
            player_play(mapper->player, action->params.play.filename);
            mapper->button_fsm_state = BTN_STATE_PLAY_CUT;
            mapper->current_button = button_number;
            strncpy(mapper->current_filename, action->params.play.filename, CONFIG_MAX_PATH_LEN - 1);
            mapper->current_filename[CONFIG_MAX_PATH_LEN - 1] = '\0';
            break;

        case ACTION_TYPE_PLAY_LOCK:
            ESP_LOGI(TAG, "Action: Play_lock '%s' (start)", action->params.play.filename);
            player_play(mapper->player, action->params.play.filename);
            mapper->button_fsm_state = BTN_STATE_PLAY_LOCK_PENDING;
            mapper->current_button = button_number;
            strncpy(mapper->current_filename, action->params.play.filename, CONFIG_MAX_PATH_LEN - 1);
            mapper->current_filename[CONFIG_MAX_PATH_LEN - 1] = '\0';
            break;

        default:
            ESP_LOGW(TAG, "Unknown action type: %d", action->type);
            break;
    }

    // Notify via unified callback
    notify_action_executed(mapper, button_number, event, action);
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

esp_err_t mapper_init(const mapper_config_t *config, mapper_handle_t *out_handle)
{
    if (config == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (config->player == NULL) {
        ESP_LOGE(TAG, "Player handle is required");
        return ESP_ERR_INVALID_ARG;
    }

    // Check that at least one source is configured (both root and filename required)
    bool has_spiffs = (config->spiffs_root != NULL && config->spiffs_mappings_file != NULL);
    bool has_sdcard = (config->sdcard_root != NULL && config->sdcard_mappings_file != NULL);

    if (!has_spiffs && !has_sdcard) {
        ESP_LOGE(TAG, "At least one mappings source (root + file) is required");
        return ESP_ERR_INVALID_ARG;
    }

    mapper_handle_t mapper = calloc(1, sizeof(struct mapper_s));
    if (mapper == NULL) {
        ESP_LOGE(TAG, "Failed to allocate mapper");
        return ESP_ERR_NO_MEM;
    }

    mapper->player = config->player;
    mapper->event_cb = config->event_cb;
    mapper->event_cb_ctx = config->event_cb_ctx;

    mapper->encoder_mode = ENCODER_MODE_VOLUME;
    mapper->current_page = NULL;
    mapper->first_page = NULL;
    mapper->page_count = 0;

    // Load mappings using linked list structure
    // current_page and page_count are set during loading
    esp_err_t ret = load_all_mappings(mapper,
                                       config->spiffs_root,
                                       config->spiffs_mappings_file,
                                       config->sdcard_root,
                                       config->sdcard_mappings_file);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load mappings: %s", esp_err_to_name(ret));
        free(mapper);
        return ret;
    }

    *out_handle = mapper;

    ESP_LOGI(TAG, "Mapper initialized: %d pages, current='%s'",
             mapper->page_count,
             mapper->current_page ? mapper->current_page->page_id : "(none)");

    // Print loaded mappings for debugging 
    if (esp_log_level_get(TAG) >= ESP_LOG_DEBUG) {
        mapper_print_mappings(mapper);
    }

    // notify observers of loaded state (includes initial page info)
    notify_loaded(mapper);

    // preload files for initial page
    preload_current_page_files(mapper);

    return ESP_OK;
}

void mapper_handle_event(mapper_handle_t handle,
                        uint8_t button_number,
                        input_event_type_t event)
{
    if (handle == NULL) {
        return;
    }

    // Encoder rotation handling - behavior depends on current mode
    if (event == INPUT_EVENT_ENCODER_ROTATE_CW) {
        if (handle->encoder_mode == ENCODER_MODE_VOLUME) {
            ESP_LOGD(TAG, "Encoder: volume up");
            player_volume_adjust(handle->player, 1);
        } else {
            // PAGE mode: next page (circular navigation)
            if (handle->current_page != NULL && handle->page_count > 1) {
                handle->current_page = handle->current_page->next;
                ESP_LOGI(TAG, "Encoder: page changed to '%s'", handle->current_page->page_id);
                notify_page_changed(handle);
                preload_current_page_files(handle);
            }
        }
        return;
    }
    if (event == INPUT_EVENT_ENCODER_ROTATE_CCW) {
        if (handle->encoder_mode == ENCODER_MODE_VOLUME) {
            ESP_LOGD(TAG, "Encoder: volume down");
            player_volume_adjust(handle->player, -1);
        } else {
            // PAGE mode: previous page (circular navigation)
            if (handle->current_page != NULL && handle->page_count > 1) {
                handle->current_page = handle->current_page->prev;
                ESP_LOGI(TAG, "Encoder: page changed to '%s'", handle->current_page->page_id);
                notify_page_changed(handle);
                preload_current_page_files(handle);
            }
        }
        return;
    }

    // Encoder switch uses button_number=0 with button events
    if (button_number == 0) {
        if (event == INPUT_EVENT_BUTTON_PRESS) {
            // Toggle encoder mode
            handle->encoder_mode = (handle->encoder_mode == ENCODER_MODE_VOLUME)
                                   ? ENCODER_MODE_PAGE
                                   : ENCODER_MODE_VOLUME;
            ESP_LOGI(TAG, "Encoder mode changed to %s",
                     handle->encoder_mode == ENCODER_MODE_VOLUME ? "VOLUME" : "PAGE");

            notify_encoder_mode_changed(handle, handle->encoder_mode);
            return;
        }
        if (event == INPUT_EVENT_BUTTON_LONG_PRESS) {
            ESP_LOGD(TAG, "Encoder switch long press: reserved");
            return;
        }
        return;
    }

    // Handle RELEASE events via button FSM
    if (event == INPUT_EVENT_BUTTON_RELEASE && button_number >= 1 && button_number <= 12) {
        if (handle->current_button == button_number) {
            switch (handle->button_fsm_state) {
                case BTN_STATE_PLAY_CUT:
                case BTN_STATE_PLAY_LOCK_PENDING:
                    ESP_LOGI(TAG, "Button %d released: stopping playback", button_number);
                    player_stop(handle->player, true);
                    handle->button_fsm_state = BTN_STATE_INITIAL;
                    break;
                case BTN_STATE_PLAY_ONCE:
                case BTN_STATE_PLAY_LOCKED:
                    handle->button_fsm_state = BTN_STATE_INITIAL;
                    break;
                case BTN_STATE_INITIAL:
                    break;
            }
        }
        return;
    }

    // Direct page selection in PAGE mode
    if (handle->encoder_mode == ENCODER_MODE_PAGE &&
        event == INPUT_EVENT_BUTTON_PRESS &&
        button_number >= 1 && button_number <= 12) {

        page_node_t *target_page = find_page_by_number(handle, button_number);
        if (target_page != NULL) {
            // Select the page
            handle->current_page = target_page;
            ESP_LOGI(TAG, "Direct page select: button %d -> page '%s'",
                     button_number, target_page->page_id);

            // Switch back to VOLUME mode
            handle->encoder_mode = ENCODER_MODE_VOLUME;
            notify_encoder_mode_changed(handle, ENCODER_MODE_VOLUME);

            // Notify page change and preload files
            notify_page_changed(handle);
            preload_current_page_files(handle);
        } else {
            ESP_LOGD(TAG, "Page %d does not exist (only %d pages loaded)",
                     button_number, handle->page_count);
        }
        return;  // Don't process as regular mapping
    }

    // Handle LONG_PRESS for play_lock: transition PENDING -> LOCKED
    if (event == INPUT_EVENT_BUTTON_LONG_PRESS && button_number >= 1 && button_number <= 12) {
        if (handle->current_button == button_number &&
            handle->button_fsm_state == BTN_STATE_PLAY_LOCK_PENDING) {
            handle->button_fsm_state = BTN_STATE_PLAY_LOCKED;
            ESP_LOGI(TAG, "Play_lock button %d: locked (playback continues after release)", button_number);
            return;  // Consume event
        }
    }

    // For matrix button events, look up mapping (using current page)
    const mapping_node_t *mapping = find_mapping(handle, button_number, event);
    if (mapping != NULL) {
        execute_action(handle, button_number, event, &mapping->action);
    } else {
        ESP_LOGD(TAG, "No mapping found for page='%s', button=%d, event=%d",
                 handle->current_page ? handle->current_page->page_id : "(none)",
                 button_number, event);
    }
}

esp_err_t mapper_deinit(mapper_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    free_all_pages(handle);
    free(handle);

    ESP_LOGI(TAG, "Mapper deinitialized");

    return ESP_OK;
}

/**
 * @brief Get action type name as string
 */
static const char *action_type_to_str(action_type_t type)
{
    switch (type) {
        case ACTION_TYPE_STOP:         return "stop";
        case ACTION_TYPE_PLAY:         return "play";
        case ACTION_TYPE_PLAY_CUT:     return "play_cut";
        case ACTION_TYPE_PLAY_LOCK:    return "play_lock";
        default:                       return "unknown";
    }
}

/**
 * @brief Get event type name as string
 */
static const char *event_type_to_str(input_event_type_t event)
{
    switch (event) {
        case INPUT_EVENT_BUTTON_PRESS:      return "press";
        case INPUT_EVENT_BUTTON_LONG_PRESS: return "long_press";
        case INPUT_EVENT_BUTTON_RELEASE:    return "release";
        default:                            return "unknown";
    }
}

void mapper_print_mappings(mapper_handle_t handle)
{
    if (handle == NULL) {
        printf("Mapper not initialized\n");
        return;
    }

    if (handle->current_page == NULL) {
        printf("No mappings loaded\n");
        return;
    }

    printf("=== Loaded Mappings (%d pages) ===\n", handle->page_count);

    // Traverse circular page list
    page_node_t *start = handle->current_page;
    page_node_t *page = start;

    do {
        printf("\nPage %d '%s'%s:\n", page->page_number, page->page_id,
               (page == handle->current_page) ? " (current)" : "");

        // Count and print mappings for this page
        int mapping_count = 0;
        mapping_node_t *mapping = page->mappings;

        while (mapping != NULL) {
            mapping_count++;

            const char *event_str = event_type_to_str(mapping->event);
            const char *action_str = action_type_to_str(mapping->action.type);

            // Print based on action type
            switch (mapping->action.type) {
                case ACTION_TYPE_STOP:
                    printf("  btn=%2d %-10s -> %s\n",
                           mapping->button_number, event_str, action_str);
                    break;

                case ACTION_TYPE_PLAY:
                case ACTION_TYPE_PLAY_CUT:
                case ACTION_TYPE_PLAY_LOCK:
                    printf("  btn=%2d %-10s -> %-12s %s\n",
                           mapping->button_number, event_str, action_str,
                           mapping->action.params.play.filename);
                    break;

                default:
                    printf("  btn=%2d %-10s -> %s\n",
                           mapping->button_number, event_str, action_str);
                    break;
            }

            mapping = mapping->next;
        }

        if (mapping_count == 0) {
            printf("  (no mappings)\n");
        }

        page = page->next;
    } while (page != start);

    printf("\n");
}

void mapper_print_status(mapper_handle_t handle, status_output_type_t output_type)
{
    if (handle == NULL) {
        if (output_type == STATUS_OUTPUT_COMPACT) {
            printf("[mapper] not initialized\n");
        } else {
            printf("Mapper Status:\n");
            printf("  State: Not initialized\n");
        }
        return;
    }

    const char *page_id = handle->current_page ? handle->current_page->page_id : "none";
    uint8_t page_num = handle->current_page ? handle->current_page->page_number : 0;
    uint8_t page_count = handle->page_count;
    const char *mode = (handle->encoder_mode == ENCODER_MODE_VOLUME) ? "VOLUME" : "PAGE";

    // Count total mappings
    int total_mappings = 0;
    if (handle->current_page != NULL) {
        page_node_t *start = handle->current_page;
        page_node_t *page = start;
        do {
            mapping_node_t *mapping = page->mappings;
            while (mapping != NULL) {
                total_mappings++;
                mapping = mapping->next;
            }
            page = page->next;
        } while (page != start);
    }

    if (output_type == STATUS_OUTPUT_COMPACT) {
        printf("[mapper] page=\"%s\" (%d/%d), mode=%s, %d mappings\n",
               page_id, page_num, page_count, mode, total_mappings);
    } else {
        printf("Mapper Status:\n");
        printf("  Current page: \"%s\" (%d of %d)\n", page_id, page_num, page_count);
        printf("  Encoder mode: %s\n", mode);
        printf("  Total mappings: %d\n", total_mappings);

        if (output_type == STATUS_OUTPUT_VERBOSE) {
            // Per-page mapping counts
            printf("  Pages:\n");
            if (handle->current_page != NULL) {
                page_node_t *start = handle->current_page;
                page_node_t *page = start;
                do {
                    int count = 0;
                    mapping_node_t *mapping = page->mappings;
                    while (mapping != NULL) {
                        count++;
                        mapping = mapping->next;
                    }
                    printf("    %s: %d mappings%s\n",
                           page->page_id, count,
                           (page == handle->current_page) ? " (current)" : "");
                    page = page->next;
                } while (page != start);
            }

            // Button FSM state
            const char *fsm_state = "INITIAL";
            switch (handle->button_fsm_state) {
                case BTN_STATE_PLAY_ONCE: fsm_state = "PLAY_ONCE"; break;
                case BTN_STATE_PLAY_CUT: fsm_state = "PLAY_CUT"; break;
                case BTN_STATE_PLAY_LOCK_PENDING: fsm_state = "PLAY_LOCK_PENDING"; break;
                case BTN_STATE_PLAY_LOCKED: fsm_state = "PLAY_LOCKED"; break;
                default: break;
            }
            printf("  Button FSM: %s (btn=%d)\n", fsm_state, handle->current_button);
        }
    }
}
