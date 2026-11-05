#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# The firmware places LVGL's fixed pool in ESP32 PSRAM. Provide a host-only
# allocator shim so the same lv_conf.h can be used without an ESP32 toolchain.
cat > "$work/esp_heap_caps.h" <<'EOF'
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
static inline void *heap_caps_malloc(size_t size, int caps) { (void)caps; return malloc(size); }
EOF
mapfile -t sources < <(find arduino/libraries/lvgl/src -name '*.c' | sort)
gcc -O0 -w -fPIC -shared -DLV_CONF_INCLUDE_SIMPLE \
  -I"$work" -Iarduino/libraries -Iarduino/libraries/lvgl \
  "${sources[@]}" -o "$work/liblvgl.so" -lm
g++ -std=c++17 -O0 -Wall -Wextra -Werror -DLV_CONF_INCLUDE_SIMPLE \
  -I"$work" -Iarduino/libraries -Iarduino/libraries/lvgl \
  tests/lvgl_ui_test.cpp -L"$work" -llvgl -Wl,-rpath,"$work" -o "$work/lvgl_ui_test"
"$work/lvgl_ui_test"
