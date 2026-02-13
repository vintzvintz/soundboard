/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file console.c
 * @brief ESP-IDF Console command handler
 */

#include "sdkconfig.h"
#include <sys/stat.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"  // IWYU pragma: keep
#include "esp_console.h"
#include "esp_log.h"
#include "argtable3/argtable3.h"

#include "app_state.h"
#include "console.h"
#include "sd_card.h"
#include "persistent_volume.h"
#include "soundboard.h"

#ifdef CONFIG_SOUNDBOARD_IO_STATS_ENABLE
    #include "benchmark.h"
#endif

static const char *TAG = "console";

// =============================================================================
// Module State
// =============================================================================

static const app_state_t *s_app = NULL;
static bool s_initialized = false;

// =============================================================================
// Helper Functions
// =============================================================================

/**
 * @brief Recursively list files in a directory
 */
static void list_directory_recursive(const char *path, int depth)
{
    DIR *dir = opendir(path);
    if (!dir) {
        return;
    }

    struct dirent *entry;
    struct stat entry_stat;
    char full_path[SOUNDBOARD_MAX_PATH_LEN];

    while ((entry = readdir(dir)) != NULL) {
        // Skip "." and ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        int written = snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        if (written >= (int)sizeof(full_path)) {
            continue;
        }

        if (stat(full_path, &entry_stat) == 0) {
            // Print indentation
            for (int i = 0; i < depth; i++) {
                printf("  ");
            }

            if (S_ISDIR(entry_stat.st_mode)) {
                printf("[DIR]  %s\n", entry->d_name);
                // Recurse into subdirectory
                list_directory_recursive(full_path, depth + 1);
            } else {
                printf("%10ld  %s\n", entry_stat.st_size, entry->d_name);
            }
        }
    }

    closedir(dir);
}

/**
 * @brief Print contents of a file
 */
static void cat_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("  [Not found: %s]\n", path);
        return;
    }

    printf("  Contents of %s:\n", path);
    printf("  ----------------------------------------\n");

    char line[256];
    while (fgets(line, sizeof(line), f) != NULL) {
        printf("  %s", line);
    }

    printf("  ----------------------------------------\n");
    fclose(f);
}

// =============================================================================
// Core Commands (Always Available)
// =============================================================================

/**
 * @brief 'ls' command - Recursive VFS tree walk
 */
static int cmd_ls(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("\n=== VFS File System Tree ===\n\n");

    // List SPIFFS
    printf("SPIFFS (%s):\n", SPIFFS_MOUNT_POINT);
    list_directory_recursive(SPIFFS_MOUNT_POINT, 1);
    printf("\n");

    // List SD card
    printf("SD Card (%s):\n", SDCARD_MOUNT_POINT);
    list_directory_recursive(SDCARD_MOUNT_POINT, 1);
    printf("\n");

    // List MSC device (only visible during USB sync)
    printf("MSC Device (%s):\n", MSC_MOUNT_POINT);
    list_directory_recursive(MSC_MOUNT_POINT, 1);
    printf("\n");

    return 0;
}

/**
 * @brief 'mapping' command - Show or cat button-to-sound mappings
 *
 * Usage: mapping [show|cat]
 *   - show (default): print loaded mappings from mapper
 *   - cat: print raw mappings.csv files from storage
 */
static int cmd_mapping(int argc, char **argv)
{
    const char *subcmd = "show";
    if (argc >= 2) {
        subcmd = argv[1];
    }

    if (strcmp(subcmd, "show") == 0) {
        if (!s_app->mapper) {
            printf("Mapper not available\n");
            return 1;
        }
        mapper_print_mappings(s_app->mapper);
    } else if (strcmp(subcmd, "cat") == 0) {
        printf("\n=== Button-to-Sound Mappings ===\n\n");
        printf("Internal mappings (SPIFFS):\n");
        cat_file(SPIFFS_MAPPINGS_PATH);
        printf("\n");
        printf("External mappings (SD Card):\n");
        cat_file(SDCARD_MAPPINGS_PATH);
        printf("\n");
    } else {
        printf("Unknown subcommand: %s\n", subcmd);
        printf("Usage: mapping [show|cat]\n");
        return 1;
    }

    return 0;
}

/**
 * @brief Print system-level status (memory, tasks)
 */
static void print_system_status(status_output_type_t output_type)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    if (output_type == STATUS_OUTPUT_COMPACT) {
        printf("[system] internal_free=%zuKB", internal_free / 1024);
#if CONFIG_SPIRAM_SUPPORT
        printf(", psram_free=%zuKB", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
#endif
        printf(", tasks=%u\n", (unsigned int)uxTaskGetNumberOfTasks());
        return;
    }

    printf("System Status:\n");
    printf("  Internal RAM:\n");
    printf("    Free:     %6zu KB\n", internal_free / 1024);
    printf("    Largest:  %6zu KB\n", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
    printf("    Minimum:  %6zu KB\n", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);

#if CONFIG_SPIRAM_SUPPORT
    printf("  PSRAM:\n");
    printf("    Free:     %6zu KB\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    printf("    Largest:  %6zu KB\n", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024);
    printf("    Minimum:  %6zu KB\n", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024);
#endif

    if (output_type == STATUS_OUTPUT_VERBOSE) {
        printf("  FreeRTOS Tasks:\n");
        printf("    %-20s %8s %8s\n", "Name", "State", "Priority");
        printf("    ------------------------------------------------\n");

        UBaseType_t num_tasks = uxTaskGetNumberOfTasks();
        TaskStatus_t *task_status_array = pvPortMalloc(num_tasks * sizeof(TaskStatus_t));

        if (task_status_array != NULL) {
            uint32_t total_runtime;
            num_tasks = uxTaskGetSystemState(task_status_array, num_tasks, &total_runtime);

            for (UBaseType_t i = 0; i < num_tasks; i++) {
                const char *state_str;
                switch (task_status_array[i].eCurrentState) {
                    case eRunning:   state_str = "Running";  break;
                    case eReady:     state_str = "Ready";    break;
                    case eBlocked:   state_str = "Blocked";  break;
                    case eSuspended: state_str = "Suspend";  break;
                    case eDeleted:   state_str = "Deleted";  break;
                    default:         state_str = "Unknown";  break;
                }

                printf("    %-20s %8s %8u\n",
                       task_status_array[i].pcTaskName,
                       state_str,
                       (unsigned int)task_status_array[i].uxCurrentPriority);
            }

            vPortFree(task_status_array);
        }
    }
}


/**
 * @brief 'erase_sdcard' command - Recursively erase all files on SD card
 */
static int cmd_erase_sdcard(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("\n=== Erasing SD Card ===\n\n");

    if (!s_app->sdcard) {
        printf("SD card not mounted\n");
        return 1;
    }

    printf("Deleting all files and directories on %s...\n\n", SDCARD_MOUNT_POINT);

    esp_err_t ret = sd_card_erase_all(SDCARD_MOUNT_POINT);
    if (ret != ESP_OK) {
        printf("Failed to erase SD card: %s\n", esp_err_to_name(ret));
        return 1;
    }

    return 0;
}

// =============================================================================
// Status Command
// =============================================================================

/**
 * @brief Parse output type string
 */
static status_output_type_t parse_output_type(const char *str)
{
    if (str == NULL) {
        return STATUS_OUTPUT_NORMAL;
    }
    if (strcmp(str, "compact") == 0) {
        return STATUS_OUTPUT_COMPACT;
    }
    if (strcmp(str, "verbose") == 0) {
        return STATUS_OUTPUT_VERBOSE;
    }
    return STATUS_OUTPUT_NORMAL;
}

/**
 * @brief Print status for all modules
 */
static void print_all_status(status_output_type_t output_type)
{
    app_print_status(output_type);
    print_system_status(output_type);
    sd_card_print_status(output_type);
    persistent_volume_print_status(output_type);
    display_print_status(s_app->oled, output_type);
    input_scanner_print_status(s_app->input_scanner, output_type);
    mapper_print_status(s_app->mapper, output_type);
    player_print_status(s_app->player, output_type);
    msc_print_status(s_app->msc, output_type);
#ifdef IO_STATS_ENABLE
    benchmark_print_status(output_type);
#endif
}

/**
 * @brief 'status' command - Hierarchical status printing
 *
 * Usage: status <module|all|help> [compact|normal|verbose]
 */
static int cmd_status(int argc, char **argv)
{
    // Parse arguments
    if (argc < 2) {
        printf("Usage: status <module|all|help> [compact|normal|verbose]\n");
        printf("Type 'status help' for available modules.\n");
        return 1;
    }

    const char *module = argv[1];
    status_output_type_t output_type = STATUS_OUTPUT_NORMAL;
    if (argc >= 3) {
        output_type = parse_output_type(argv[2]);
    }

    // Handle 'help' command
    if (strcmp(module, "help") == 0) {
        printf("Usage: status <module|all|help> [compact|normal|verbose]\n\n");
        printf("Available modules:\n");
        printf("  app      - Application state\n");
        printf("  system   - Memory and FreeRTOS tasks\n");
        printf("  mapper   - Button-to-action mapping\n");
        printf("  sdcard   - SD card storage\n");
        printf("  msc      - USB MSC host\n");
        printf("  input    - Input scanner (keypad + encoder)\n");
        printf("  display  - OLED display\n");
        printf("  volume   - Persistent volume\n");
        printf("  player   - Audio player and cache\n");
        printf("  all      - Print all modules\n\n");
        printf("Output types:\n");
        printf("  compact  - Single-line summary\n");
        printf("  normal   - Standard output (default)\n");
        printf("  verbose  - Detailed output\n");
        return 0;
    }

    // Handle 'all' command
    if (strcmp(module, "all") == 0) {
        print_all_status(output_type);
        return 0;
    }

    // Handle individual modules
    if (strcmp(module, "app") == 0) {
        app_print_status(output_type);
    } else if (strcmp(module, "system") == 0) {
        print_system_status(output_type);
    } else if (strcmp(module, "mapper") == 0) {
        mapper_print_status(s_app->mapper, output_type);
    } else if (strcmp(module, "sdcard") == 0) {
        sd_card_print_status(output_type);
    } else if (strcmp(module, "msc") == 0) {
        msc_print_status(s_app->msc, output_type);
    } else if (strcmp(module, "input") == 0) {
        input_scanner_print_status(s_app->input_scanner, output_type);
    } else if (strcmp(module, "display") == 0) {
        display_print_status(s_app->oled, output_type);
    } else if (strcmp(module, "volume") == 0) {
        persistent_volume_print_status(output_type);
    } else if (strcmp(module, "player") == 0) {
        player_print_status(s_app->player, output_type);
#ifdef IO_STATS_ENABLE
    } else if (strcmp(module, "benchmark") == 0) {
        benchmark_print_status(output_type);
#endif
    } else {
        printf("Unknown module: %s\n", module);
        printf("Type 'status help' for available modules.\n");
        return 1;
    }

    return 0;
}

/**
 * @brief 'cat' command - Dump file contents to console
 *
 * Usage: cat <path>
 * Outputs up to 4096 bytes of the file to the console.
 */
static int cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: cat <path>\n");
        return 1;
    }

    const char *path = argv[1];
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("Cannot open: %s\n", path);
        return 1;
    }

    static const size_t MAX_BYTES = 4096;
    char buf[256];
    size_t total = 0;

    while (total < MAX_BYTES) {
        size_t to_read = sizeof(buf);
        if (total + to_read > MAX_BYTES) {
            to_read = MAX_BYTES - total;
        }
        size_t n = fread(buf, 1, to_read, f);
        if (n == 0) {
            break;
        }
        fwrite(buf, 1, n, stdout);
        total += n;
    }

    fclose(f);

    if (total >= MAX_BYTES) {
        printf("\n[truncated at %zu bytes]\n", MAX_BYTES);
    }

    return 0;
}

// =============================================================================
// Player Commands
// =============================================================================

/**
 * @brief Arguments for 'play' command
 */
static struct {
    struct arg_str *filename;
    struct arg_end *end;
} play_args;

/**
 * @brief 'play' command handler
 */
static int cmd_play(int argc, char **argv)
{
    if (!s_app->player) {
        printf("Player not available\n");
        return 1;
    }

    int nerrors = arg_parse(argc, argv, (void **)&play_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, play_args.end, argv[0]);
        return 1;
    }

    const char *filename = play_args.filename->sval[0];

    esp_err_t ret = player_play(s_app->player, filename);
    if (ret == ESP_OK) {
        printf("Play request sent: %s\n", filename);
    } else {
        printf("Failed to request play %s: %s\n", filename, esp_err_to_name(ret));
        return 1;
    }

    return 0;
}

/**
 * @brief 'stop' command handler
 */
static int cmd_stop(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (!s_app->player) {
        printf("Player not available\n");
        return 1;
    }

    esp_err_t ret = player_stop(s_app->player, true);
    if (ret == ESP_OK) {
        printf("Stop request sent\n");
    } else {
        printf("Failed to request stop: %s\n", esp_err_to_name(ret));
        return 1;
    }

    return 0;
}

/**
 * @brief 'volume' command handler (I2S software volume - synchronous)
 *
 * Usage: volume [up|down|<index>]
 *   - No argument: display current volume index
 *   - up/down: adjust by one step
 *   - index: set to specific level (0-31)
 */
static int cmd_volume(int argc, char **argv)
{
    if (!s_app->player) {
        printf("Player not available\n");
        return 1;
    }

    esp_err_t ret;
    int current_vol = 0;

    // If no argument provided, display current volume
    if (argc < 2) {
        ret = player_volume_get(s_app->player, &current_vol);
        if (ret == ESP_OK) {
            printf("Current volume: %d/%d\n", current_vol, player_volume_get_max_index());
        } else {
            printf("Failed to get volume: %s\n", esp_err_to_name(ret));
        }
        return (ret == ESP_OK) ? 0 : 1;
    }

    const char *val_str = argv[1];

    if (strcmp(val_str, "up") == 0) {
        ret = player_volume_adjust(s_app->player, 1);
    } else if (strcmp(val_str, "down") == 0) {
        ret = player_volume_adjust(s_app->player, -1);
    } else {
        int index = atoi(val_str);
        ret = player_volume_set(s_app->player, (int8_t)index);
    }

    if (ret != ESP_OK) {
        printf("Failed to set volume: %s\n", esp_err_to_name(ret));
        return 1;
    }

    // Show the new volume level
    ret = player_volume_get(s_app->player, &current_vol);
    if (ret == ESP_OK) {
        printf("Volume: %d/%d\n", current_vol, player_volume_get_max_index());
    }

    return 0;
}

// =============================================================================
// Command Registration
// =============================================================================

/**
 * @brief Register all commands unconditionally
 */
static void register_all_commands(void)
{
    // --- Core commands (always functional) ---

    const esp_console_cmd_t ls_cmd = {
        .command = "ls",
        .help = "List all files in VFS (recursive)",
        .hint = NULL,
        .func = &cmd_ls,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ls_cmd));

    const esp_console_cmd_t mapping_cmd = {
        .command = "mapping",
        .help = "Show loaded mappings or cat raw CSV files",
        .hint = "[show|cat]",
        .func = &cmd_mapping,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&mapping_cmd));

    const esp_console_cmd_t cat_cmd = {
        .command = "cat",
        .help = "Dump file contents (max 4096 bytes)",
        .hint = "<path>",
        .func = &cmd_cat,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cat_cmd));

    const esp_console_cmd_t erase_sdcard_cmd = {
        .command = "erase_sdcard",
        .help = "Erase all files and directories on SD card",
        .hint = NULL,
        .func = &cmd_erase_sdcard,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&erase_sdcard_cmd));

    const esp_console_cmd_t status_cmd = {
        .command = "status",
        .help = "Show module status (status help for usage)",
        .hint = "<module|all|help> [compact|normal|verbose]",
        .func = &cmd_status,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&status_cmd));

    // --- Player commands (guard with NULL checks at runtime) ---

    play_args.filename = arg_str1(NULL, NULL, "<path>", "Path to audio file");
    play_args.end = arg_end(2);

    const esp_console_cmd_t play_cmd = {
        .command = "play",
        .help = "Play audio file",
        .hint = NULL,
        .func = &cmd_play,
        .argtable = &play_args,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&play_cmd));

    const esp_console_cmd_t stop_cmd = {
        .command = "stop",
        .help = "Stop playback",
        .hint = NULL,
        .func = &cmd_stop,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&stop_cmd));

    const esp_console_cmd_t volume_cmd = {
        .command = "volume",
        .help = "Query or set volume (index, up, down)",
        .hint = "[<index>|up|down]",
        .func = &cmd_volume,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&volume_cmd));

}

// =============================================================================
// Public API
// =============================================================================

esp_err_t console_init(const app_state_t *app_state)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Console already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (app_state == NULL) {
        ESP_LOGE(TAG, "app_state is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    s_app = app_state;

    // Initialize console REPL
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "soundboard> ";
    repl_config.max_cmdline_length = 256;

    // Register help command
    esp_console_register_help_command();

    // Register all commands unconditionally
    register_all_commands();

    // Initialize console REPL environment
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));

    // Start console REPL
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    s_initialized = true;

    ESP_LOGI(TAG, "Console initialized. Type 'help' to see available commands.");

    return ESP_OK;
}

esp_err_t console_deinit(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Console REPL cleanup is handled by the framework
    s_app = NULL;
    s_initialized = false;

    return ESP_OK;
}
