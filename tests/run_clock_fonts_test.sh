#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="$(mktemp)"
trap 'rm -f "$out"' EXIT
cc -std=c11 -ffunction-sections -fdata-sections -DLV_CONF_INCLUDE_SIMPLE \
  -I arduino/libraries -I arduino/libraries/lvgl \
  tests/clock_fonts_test.c arduino/libraries/lvgl/src/font/lv_font.c \
  arduino/libraries/lvgl/src/font/lv_font_fmt_txt.c arduino/libraries/lvgl/src/misc/lv_utils.c \
  arduino/08_LVGL_Clock/src/fonts/lv_font_{digitalnumbers,rajdhani,robotomono,neonlines}_{48,96}.c \
  -Wl,--gc-sections -o "$out"
"$out"
