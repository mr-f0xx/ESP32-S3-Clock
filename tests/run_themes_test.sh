#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 - <<'PY'
from pathlib import Path
import re

root = Path("arduino/08_LVGL_Clock")
theme = (root / "clock_theme.inc").read_text()
layout = (root / "clock_layout.h").read_text()
menu = (root / "clock_app_menu.inc").read_text()
control = (root / "clock_control_panel.inc").read_text()
ext = (root / "clock_extensions.inc").read_text()
gb = (root / "clock_gameboy.inc").read_text()
web = (root / "web_page.h").read_text()

ids = ["GLOW", "AMBER", "RETRO", "CYBERPUNK", "AURORA", "OCEAN", "SPECTRUM",
       "MATRIX", "TOKYO_NIGHT", "DRACULA"]
enum = theme.split("enum UiTheme : uint8_t {", 1)[1].split("};", 1)[0]
found = re.findall(r"UI_THEME_([A-Z_]+)(?:\s*=\s*0)?", enum)
assert found == ids + ["COUNT"], found
assert "constexpr uint8_t THEME_COUNT = 10;" in layout
assert "migrateTheme(uint8_t stored, bool version2)" in layout

# Canonical palette anchors. Secondary Dracula text is intentionally lifted for
# small-screen contrast; the background/surface/border/accent remain classic.
expected = {
    "MATRIX_GREEN": 0x00FF41, "MATRIX_GREEN_DIM": 0x00B32C,
    "MATRIX_BORDER": 0x008F11, "MATRIX_SURFACE": 0x031006,
    "TOKYO_BG": 0x1A1B26, "TOKYO_SURFACE": 0x24283B,
    "TOKYO_BORDER": 0x414868, "TOKYO_TEXT": 0xC0CAF5,
    "TOKYO_DIM": 0xA9B1D6, "TOKYO_BLUE": 0x7AA2F7,
    "DRACULA_BG": 0x282A36, "DRACULA_SURFACE": 0x44475A,
    "DRACULA_BORDER": 0x6272A4, "DRACULA_TEXT": 0xF8F8F2,
    "DRACULA_DIM": 0xC3CBE4, "DRACULA_PURPLE": 0xBD93F9,
}
constants = {name: int(value, 16) for name, value in
             re.findall(r"\b([A-Z][A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+)", theme)}
for name, value in expected.items():
    assert constants.get(name) == value, (name, constants.get(name))

def luminance(rgb):
    def channel(v):
        v /= 255.0
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
    r, g, b = (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255
    return 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b)

def contrast(a, b):
    hi, lo = sorted((luminance(a), luminance(b)), reverse=True)
    return (hi + 0.05) / (lo + 0.05)

for bg, surface, text, dim, accent in [
    (0x000000, constants["MATRIX_SURFACE"], constants["MATRIX_GREEN"], constants["MATRIX_GREEN_DIM"], constants["MATRIX_GREEN"]),
    (constants["TOKYO_BG"], constants["TOKYO_SURFACE"], constants["TOKYO_TEXT"], constants["TOKYO_DIM"], constants["TOKYO_BLUE"]),
    (constants["DRACULA_BG"], constants["DRACULA_SURFACE"], constants["DRACULA_TEXT"], constants["DRACULA_DIM"], constants["DRACULA_PURPLE"]),
]:
    assert contrast(text, bg) >= 7.0, (hex(text), hex(bg), contrast(text, bg))
    assert contrast(dim, surface) >= 4.5, (hex(dim), hex(surface), contrast(dim, surface))
    # Small accent labels sit on the control surface. Dracula deliberately uses
    # the darker page background there; current-line remains the panel surface.
    control_surface = 0x282A36 if surface == constants["DRACULA_SURFACE"] else surface
    assert contrast(accent, control_surface) >= 4.5, (hex(accent), hex(control_surface), contrast(accent, control_surface))

for short in ["MTX", "TKY", "DRC"]:
    assert f'return "{short}";' in theme
for option, key, name in [(7, "theme_matrix", "Matrix Terminal"),
                          (8, "theme_tokyo", "Tokyo Night"),
                          (9, "theme_dracula", "Dracula")]:
    assert f'<option value="{option}" data-i18n="{key}">{name}</option>' in web
    assert web.count(f'{key}:') >= 2  # English and French dictionaries

# Matrix/editor themes are flat and do not fall into the older neon-halo branch.
assert "themeIsFlatDark() { return themeIsMatrix() || themeIsEditor(); }" in theme
assert "!themeIsFlatDark()" in theme.split("inline bool themeIsNeon()", 1)[1].split("}", 1)[0]
flat = menu.split("} else if (themeIsFlatDark()) {", 1)[1].split("} else {", 1)[0]
assert "lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);" in flat
assert "themeIsMatrix() ? 4 : themeIsEditor() ? 16 : 38" in menu
assert 'themeLabelPrefix() { return themeIsMatrix() ? "> " : ""; }' in theme

# Wi-Fi card: fixed single-line rows, not wrapping text over the IP address.
assert "l.wifiStatus = r(92, 198, 128, 26);" in layout
assert "l.wifiIp = r(92, 232, 128, 22);" in layout
assert "wifiControlStatus = createControlLabel(l.wifiStatus" in control
assert "false, LV_LABEL_LONG_DOT);" in control
assert "lv_label_set_long_mode(label, longMode);" in control

# Game Boy: the full-screen ROM picker cannot bury the shared escape key.
assert "extensionBackButtons[app] = createAppBackButton(page, extBackEvent, accent);" in ext
assert gb.count("lv_obj_move_foreground(extensionBackButtons[EXT_GAMEBOY])") >= 2
assert "lv_obj_move_foreground(gbList);" in gb
assert "layout.title.x, layout.title.y" in gb
assert "l.back = appBack(width, height);" in layout
print("themes: Matrix/Tokyo Night/Dracula palettes, stable IDs, web options, flat finish, Wi-Fi rows and Game Boy back button OK")
PY
