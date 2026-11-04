#!/usr/bin/env bash
# Build the ESP32-S3 AMOLED clock firmware on Linux/macOS.
# Mirrors scripts/build_firmware.ps1 and .github/workflows/build-firmware.yml.
#
# Prerequisites:
#   arduino-cli >= 1.0 with the ESP32 core installed:
#     arduino-cli config init --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
#     arduino-cli core update-index
#     arduino-cli core install esp32:esp32@3.3.10
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
SKETCH_DIR="$PROJECT_ROOT/arduino/08_LVGL_Clock"
LIBRARIES_DIR="$PROJECT_ROOT/arduino/libraries"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
FQBN="${FQBN:-esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=custom,PSRAM=opi,CDCOnBoot=cdc}"

if ! command -v arduino-cli >/dev/null 2>&1; then
    echo "error: arduino-cli not found in PATH" >&2
    echo "Install it from https://arduino.github.io/arduino-cli/latest/installation/" >&2
    exit 1
fi

if ! arduino-cli core list | awk '{print $1}' | grep -qx "esp32:esp32"; then
    echo "error: the esp32:esp32 core is not installed" >&2
    echo "  arduino-cli core install esp32:esp32@3.3.10 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json" >&2
    exit 1
fi

# clock_weather_config.h is git-ignored because it normally holds a private
# QWeather key. Local builds fall back to the public template.
if [ ! -f "$SKETCH_DIR/clock_weather_config.h" ]; then
    cp "$SKETCH_DIR/clock_weather_config.example.h" "$SKETCH_DIR/clock_weather_config.h"
    echo "Created clock_weather_config.h from clock_weather_config.example.h"
fi

arduino-cli compile \
    --fqbn "$FQBN" \
    --libraries "$LIBRARIES_DIR" \
    --build-path "$BUILD_DIR" \
    --warnings none \
    "$SKETCH_DIR"

MERGED_BIN="$(find "$BUILD_DIR" -maxdepth 1 -name '*.merged.bin' -print -quit)"
if [ -z "$MERGED_BIN" ]; then
    echo "warning: merged firmware not found in $BUILD_DIR" >&2
    echo "Build output: $BUILD_DIR" >&2
    exit 1
fi

echo
echo "Build complete"
echo "  merged firmware: $MERGED_BIN"
echo "  size:            $(stat -c '%s' "$MERGED_BIN" 2>/dev/null || stat -f '%z' "$MERGED_BIN") bytes"
(cd "$(dirname "$MERGED_BIN")" && shasum -a 256 "$(basename "$MERGED_BIN")" 2>/dev/null || sha256sum "$(basename "$MERGED_BIN")")
