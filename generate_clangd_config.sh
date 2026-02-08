#!/bin/bash
# Generate .clangd configuration with dynamic toolchain paths
# Run this after building or when toolchain is updated

set -e

# Get compiler from compile_commands.json or environment
if [ -f build/compile_commands.json ]; then
    COMPILER=$(jq -r '.[0].command' build/compile_commands.json | awk '{print $1}')
else
    COMPILER=$(which xtensa-esp32s3-elf-gcc 2>/dev/null || echo "")
fi

if [ -z "$COMPILER" ]; then
    echo "Error: Cannot find xtensa compiler. Make sure you've built the project or sourced ESP-IDF environment."
    exit 1
fi

# Get sysroot from compiler (normalize the path)
SYSROOT=$(realpath "$($COMPILER -print-sysroot)")

# Derive paths
INCLUDE_DIR="${SYSROOT}/include"
SYS_INCLUDE_DIR="${SYSROOT}/sys-include"
# picolibc headers are a sibling of the sysroot directory
PICOLIBC_INCLUDE_DIR="$(dirname "${SYSROOT}")/picolibc/include"

echo "Detected toolchain:"
echo "  Compiler: $COMPILER"
echo "  Sysroot: $SYSROOT"
echo "  Picolibc: $PICOLIBC_INCLUDE_DIR"

if [ ! -d "$PICOLIBC_INCLUDE_DIR" ]; then
    echo "Warning: picolibc include directory not found at $PICOLIBC_INCLUDE_DIR"
fi

# Generate .clangd config
cat > .clangd <<EOF
CompileFlags:
  Remove:
    - "-mlongcalls"
    - "-fno-shrink-wrap"
    - "-fstrict-volatile-bitfields"
    - "-fno-tree-switch-conversion"
    - "-fzero-init-padding-bits=all"
    - "-fno-malloc-dce"
    - "-mdisable-hardware-atomics"
    - "-specs=picolibc.specs"
  Add:
    - "-D__XTENSA__"
    - "--sysroot=${SYSROOT}"
    - "-isystem${PICOLIBC_INCLUDE_DIR}"
    - "-isystem${SYS_INCLUDE_DIR}"
    - "-isystem${INCLUDE_DIR}"
  CompilationDatabase: build/

Diagnostics:
  Suppress:
    - implicit-int-conversion
    - implicit-int-float-conversion
  ClangTidy:
    Remove:
      - readability-implicit-bool-conversion
      - misc-use-anonymous-namespace
EOF

echo "Generated .clangd configuration"
echo "  Restart clangd language server to apply changes"
