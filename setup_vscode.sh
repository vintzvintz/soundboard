#!/usr/bin/env bash
#
# Generate .vscode/settings.json with locally-resolved paths.
# The other .vscode files (c_cpp_properties.json, tasks.json) contain no
# local paths and are committed directly.
#
set -euo pipefail

# --- Resolve ESP-IDF path ---
if [[ -n "${IDF_PATH:-}" ]]; then
    ESP_IDF_PATH="$IDF_PATH"
else
    # Fallback: look for release-v6.0 next to this project
    candidate="$(cd "$(dirname "$0")" && cd ../esp-idf/release-v6.0 2>/dev/null && pwd)"
    if [[ -d "$candidate" ]]; then
        ESP_IDF_PATH="$candidate"
    else
        echo "Error: IDF_PATH not set and could not find esp-idf/release-v6.0" >&2
        echo "Source the ESP-IDF export.sh first, or set IDF_PATH." >&2
        exit 1
    fi
fi

# --- Resolve tools path ---
ESP_TOOLS_PATH="${IDF_TOOLS_PATH:-$HOME/.espressif}"
if [[ ! -d "$ESP_TOOLS_PATH" ]]; then
    echo "Error: ESP-IDF tools directory not found at $ESP_TOOLS_PATH" >&2
    exit 1
fi

# --- Find python in the IDF virtual env ---
PYTHON_PATH="$(find "$ESP_TOOLS_PATH/python_env" -maxdepth 3 -name python \( -type f -o -type l \) 2>/dev/null | head -1)"
if [[ -z "$PYTHON_PATH" ]]; then
    echo "Error: Could not find python in $ESP_TOOLS_PATH/python_env/" >&2
    exit 1
fi

# --- Find xtensa-esp32s3-elf-gcc ---
GCC_PATH="$(find "$ESP_TOOLS_PATH/tools/xtensa-esp-elf" -name xtensa-esp32s3-elf-gcc -type f 2>/dev/null | sort -V | tail -1)"
if [[ -z "$GCC_PATH" ]]; then
    echo "Error: Could not find xtensa-esp32s3-elf-gcc in $ESP_TOOLS_PATH/tools/" >&2
    exit 1
fi

# --- Generate settings.json ---
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$SCRIPT_DIR/.vscode"

cat > "$SCRIPT_DIR/.vscode/settings.json" <<EOF
{
    "idf.espIdfPath": "${ESP_IDF_PATH}",
    "idf.toolsPath": "${ESP_TOOLS_PATH}",
    "idf.pythonInstallPath": "${PYTHON_PATH}",
    "idf.flashType": "UART",
    "idf.flashArgs": "app",
    "C_Cpp.errorSquiggles": "enabled",
    "C_Cpp.intelliSenseEngine": "default",
    "C_Cpp.default.compilerPath": "${GCC_PATH}",
    "clangd.arguments": [
        "--compile-commands-dir=\${workspaceFolder}/build",
        "--background-index",
        "--clang-tidy=false",
        "--completion-style=detailed",
        "--header-insertion=never",
        "--log=error"
    ],
    "clangd.fallbackFlags": [
        "-Wno-implicit-int-conversion",
        "-Wno-sign-conversion",
        "-Wno-conversion",
        "-Wno-implicit-int-float-conversion"
    ],
    "idf.currentSetup": "${ESP_IDF_PATH}",
    "cmake.configureArgs": [
        "-Wno-dev"
    ]
}
EOF

echo "Generated $SCRIPT_DIR/.vscode/settings.json"
echo "  IDF path:  $ESP_IDF_PATH"
echo "  Tools:     $ESP_TOOLS_PATH"
echo "  Python:    $PYTHON_PATH"
echo "  GCC:       $GCC_PATH"
