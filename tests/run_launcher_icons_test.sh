#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 - <<'PY'
from pathlib import Path
import re

menu = Path("arduino/08_LVGL_Clock/clock_app_menu.inc").read_text()
header = Path("arduino/08_LVGL_Clock/tabler_control_icons.h").read_text()

# The launcher regression came from runtime zoom on Alpha-8 images. Keep the
# artwork native-size and forbid zoom in the launcher creation path.
assert not re.search(r"^\s*lv_img_set_zoom\s*\(", menu, re.M), "launcher must not zoom Alpha-8 icons"
assert "constexpr lv_coord_t APP_MENU_ICON_SIZE = 120;" in menu

image_icons = ["clock", "device_tv", "adjustments", "cloud", "calendar", "alarm", "music", "power"]
for name in image_icons:
    pattern = rf"static const lv_img_dsc_t tabler_{name} = \{{.*?\.w = (\d+),\s*\.h = (\d+)"
    match = re.search(pattern, header, re.S)
    assert match, f"missing tabler_{name}"
    assert match.groups() == ("120", "120"), f"tabler_{name} must be native 120x120"
    assert f"createTablerAppIcon(" in menu and f"&tabler_{name}" in menu

# All 16 tiles still use the shared larger canvas: eight images plus eight vectors.
assert menu.count("createTablerAppIcon(") == 9  # definition + eight call sites
vector_calls = re.findall(r"createTablerIcon\([^;]*?APP_MENU_ICON_SIZE", menu, re.S)
assert len(vector_calls) == 6, f"expected six vector call sites covering eight icons, got {len(vector_calls)}"
assert "createTablerAppIcon(powerButton, &tabler_power);" in menu
assert "TABLER_ICON_POWER" not in menu, "power must use the smooth native asset, not the faceted polyline"
assert "TABLER_ICON_CALCULATOR" in menu and "TABLER_ICON_NOTES" in menu and "TABLER_ICON_STOPWATCH" in menu

# Icon Glow uses a transparent tint of the icon color; Amber uses CRT amber.
glow = menu.split("} else if (themeIsGlow()) {", 1)[1].split("} else if (themeIsAmber()) {", 1)[0]
assert "lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN);" in glow
assert "lv_obj_set_style_bg_opa(button, LV_OPA_20, LV_PART_MAIN);" in glow
amber = menu.split("} else if (themeIsAmber()) {", 1)[1].split("} else if (themeIsNeon()) {", 1)[0]
assert "lv_color_hex(CRT_AMBER)" in amber
assert "lv_obj_set_style_bg_opa(button, LV_OPA_20, LV_PART_MAIN);" in amber
assert "lv_obj_set_style_shadow_width(icon, 0, 0);" in menu
print("launcher icons: native 120 px, smooth power asset, no runtime zoom, themed translucent tiles")
PY
