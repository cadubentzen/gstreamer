#!/usr/bin/env bash
# Build GStreamer for WebAssembly (wasm32) using Emscripten
#
# Prerequisites:
#   - Emscripten SDK installed and activated (emcc in PATH)
#     OR set EMSDK to point to the emsdk root directory
#
# Usage:
#   ./scripts/build-wasm.sh [builddir]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${1:-${SOURCE_DIR}/builddir-wasm}"
CROSS_FILE="${SOURCE_DIR}/cross-files/wasm32-emscripten.txt"

# Activate emsdk if not already in PATH
if ! command -v emcc &>/dev/null; then
    if [ -n "$EMSDK" ]; then
        source "$EMSDK/emsdk_env.sh"
    else
        echo "ERROR: emcc not found. Please activate emsdk or set EMSDK env variable."
        exit 1
    fi
fi

echo "Using emcc: $(which emcc)"
emcc --version | head -1

if [ ! -d "$BUILD_DIR" ]; then
    echo "=== Configuring build in $BUILD_DIR ==="
    meson setup "$BUILD_DIR" "$SOURCE_DIR" --cross-file "$CROSS_FILE"
else
    echo "=== Build directory exists, reconfiguring ==="
    meson setup --reconfigure "$BUILD_DIR" "$SOURCE_DIR" --cross-file "$CROSS_FILE"
fi

echo ""
echo "=== Building ==="
ninja -C "$BUILD_DIR"

echo ""
echo "=== Build complete ==="
echo "Output: $BUILD_DIR"
