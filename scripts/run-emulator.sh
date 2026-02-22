#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
EMU_DIR="$PROJECT_DIR/cyd-emulator"
BUILD_DIR="$EMU_DIR/build"
PAYLOAD="$PROJECT_DIR/esp32/payload.bin"
SDCARD="$BUILD_DIR/sd.img"

# Parse arguments (pass through to emulator)
EXTRA_ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --payload) PAYLOAD="$2"; shift 2 ;;
        --sdcard)  SDCARD="$2"; shift 2 ;;
        *)         EXTRA_ARGS+=("$1"); shift ;;
    esac
done

# Build if needed
if [ ! -f "$BUILD_DIR/cyd-emulator" ]; then
    echo "Building emulator..."
    mkdir -p "$BUILD_DIR"
    cmake -S "$EMU_DIR" -B "$BUILD_DIR" \
        -DAPP_SOURCE_DIR="$PROJECT_DIR/esp32/main"
    make -C "$BUILD_DIR" -j"$(nproc)"
fi

# Rebuild if sources changed
make -C "$BUILD_DIR" -j"$(nproc)" --quiet 2>/dev/null || \
    make -C "$BUILD_DIR" -j"$(nproc)"

# Check payload exists
if [ ! -f "$PAYLOAD" ]; then
    echo "Error: payload not found at $PAYLOAD"
    echo "Run esp32/scripts/pack_payload.py first, or pass --payload <path>"
    exit 1
fi

exec "$BUILD_DIR/cyd-emulator" \
    --payload "$PAYLOAD" \
    --sdcard "$SDCARD" \
    "${EXTRA_ARGS[@]}"
