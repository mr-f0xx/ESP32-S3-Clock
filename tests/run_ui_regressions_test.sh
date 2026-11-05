#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 - <<'PY'
from pathlib import Path
import re

root = Path("arduino/08_LVGL_Clock")
control = (root / "clock_control_panel.inc").read_text()
ui = (root / "clock_ui.inc").read_text()
menu = (root / "clock_app_menu.inc").read_text()
layout = (root / "clock_layout.h").read_text()
power = (root / "clock_power.inc").read_text()
i18n = (root / "clock_i18n.inc").read_text()

# LVGL keeps alignment styles after center/align; absolute placement must reset
# them. This was the root cause of the missing control-centre icons and the
# timer back button landing on top of the title.
assert re.search(r"void setControlRect\([^)]*\) \{\s*(?://[^\n]*\n\s*)*lv_obj_set_align\(object, LV_ALIGN_TOP_LEFT\);", control)
assert re.search(r"void placeControlIcon\([^)]*\) \{\s*lv_obj_update_layout\(icon\);\s*lv_obj_set_align\(icon, LV_ALIGN_TOP_LEFT\);", control)
assert "lv_obj_set_align(*icon, LV_ALIGN_TOP_LEFT);" in control
# Single-line control text must be centered by font line height, not painted at
# the top of a taller fixed label box (the high pills / 94% / AUTO report).
assert "inline Rect centeredLineRect(" in layout
assert "ClockLayout::centeredLineRect(" in control
assert "lv_obj_set_height(label, lv_font_get_line_height(&lv_font_ui_16));" in control
assert "lv_label_long_mode_t longMode = LV_LABEL_LONG_DOT" in control
# Active connectivity cards must not paint accent text on an accent fill.
assert "themeTint(UI_ACCENT_BLUE, LV_OPA_30)" in control
assert "wifiUserEnabled ? activeTextColor : themeIcon()" in control

# Shared borderless back button in the safe area, used by every app that had a
# clipped back key. The timer must no longer reposition an aligned radio button.
assert "lv_obj_t *createAppBackButton(" in ui
assert "lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);" in ui
for name in ["clock_utilities.inc", "clock_timer.inc", "clock_world.inc", "clock_photo.inc"]:
    assert "createAppBackButton(" in (root / name).read_text(), name
assert "createAppBackButton(calendarPage, calendarBackEvent" in menu
assert "createAppBackButton(wifiSetupPage, wifiSetupBackEvent" in menu
assert not re.search(r"createRadioControlButton\(timerPage[^;]*CHEVRON_LEFT", menu + (root / "clock_timer.inc").read_text())
assert "inline Rect appBack(" in layout

# Clock-mode selector uses the launcher Glow treatment, not stock solid tiles.
assert "applyAppTileTheme(button, color);" in menu
assert "applyAppTileTheme(child, i == 1 ? UI_ACCENT_BLUE : UI_ACCENT_MINT);" in menu
# Audiobook launcher icon is native smooth artwork, not the old polyline book.
assert "createTablerAppIcon(audiobookButton, &tabler_headphones);" in menu

# Large clock is 120 px and the colon blinks without changing digit advances.
assert "return normalizeClockSize(size) == 0 ? 48 : (normalizeClockSize(size) == 1 ? 72 : 120);" in layout
assert "timeHourLabel" in ui and "timeColonLabel" in ui and "timeMinuteLabel" in ui
assert "setColonVisible(lowPowerMode || (rtcTime.getSecond() % 2) == 0);" in ui
assert "bool serviceBurnInShift()" in ui
assert "now - lastBurnInShiftAt < 60000" in ui
assert "constrain(static_cast<int16_t>(burnInOffsetX), -maxX, maxX)" in ui
assert "LOW_POWER_PARTIAL_LONG_START = 0" in power
assert "LOW_POWER_PARTIAL_LONG_END = 479" in power

# Calendar: Sunday-first names match lv_conf, rounded cells and a today action.
assert '{"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"}' in i18n
assert "calendarDrawPartEvent" in menu
assert "dsc->rect_dsc->radius = 16;" in menu
assert "calendarTodayEvent" in menu
print("ui regressions: vertically centered control text, alignment-safe control centre, shared back button, Glow mode tiles, 120 px clock, colon blink, burn-in shift, calendar polish OK")
PY
