// World clock app: a selectable city list with POSIX time-zone rules.
// Included once by 08_LVGL_Clock.ino inside its private namespace, after the
// radio module so it can reuse the shared round control-button helper.

struct WorldCity {
  const char *name;
  const char *tz;
};

static const WorldCity WORLD_CITIES[] = {
  {"Local",       nullptr}, // uses the device's configured POSIX TZ
  {"UTC",         "UTC0"},
  {"Paris",       "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"London",      "GMT0BST,M3.5.0/1,M10.5.0"},
  {"New York",    "EST5EDT,M3.2.0,M11.1.0"},
  {"Los Angeles", "PST8PDT,M3.2.0,M11.1.0"},
  {"Tokyo",       "JST-9"},
  {"Shanghai",    "CST-8"},
  {"Singapore",   "SGT-8"},
  {"Dubai",       "GST-4"},
  {"Sydney",      "AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"São Paulo",   "BRT3"},
  {"Moscow",      "MSK-3"},
};
constexpr uint8_t WORLD_CITY_COUNT = sizeof(WORLD_CITIES) / sizeof(WORLD_CITIES[0]);

uint8_t worldCityIndex = 0;
lv_obj_t *worldClockPage = nullptr;
lv_obj_t *worldTimeLabel = nullptr;
lv_obj_t *worldSecondsLabel = nullptr;
lv_obj_t *worldCityLabel = nullptr;
lv_obj_t *worldDateLabel = nullptr;
lv_obj_t *worldStatusLabel = nullptr;
lv_obj_t *worldCityListPanel = nullptr;
lv_obj_t *worldCityListList = nullptr;
bool worldCityListVisible = false;
uint32_t worldLastUpdateAt = 0;

void updateWorldClockPage();
void refreshWorldCityListStyles();

const char *worldCityName(uint8_t index) {
  if (index == 0) return tr(STR_WORLD_CLOCK_LOCAL);
  return WORLD_CITIES[index < WORLD_CITY_COUNT ? index : 0].name;
}

// Returns a UTC epoch from either the ESP32 system clock (set by NTP) or the
// battery-backed RTC. The RTC stores local time, so it is interpreted through
// the device's configured time zone before converting to UTC.
bool worldClockUtcEpoch(time_t &utc) {
  const time_t now = time(nullptr);
  if (now > 1700000000) {
    utc = now;
    return true;
  }
  if (!rtcReady || !rtcTimeValid) return false;
  RTC_DateTime rtcTime;
  if (!readRtcTime(rtcTime)) return false;
  struct tm localTime {};
  localTime.tm_year = rtcTime.getYear() - 1900;
  localTime.tm_mon = rtcTime.getMonth() - 1;
  localTime.tm_mday = rtcTime.getDay();
  localTime.tm_hour = rtcTime.getHour();
  localTime.tm_min = rtcTime.getMinute();
  localTime.tm_sec = rtcTime.getSecond();
  localTime.tm_isdst = -1;
  const char *previousTz = getenv("TZ");
  char savedTz[96];
  if (previousTz) strlcpy(savedTz, previousTz, sizeof(savedTz));
  else savedTz[0] = '\0';
  setenv("TZ", timezoneRule.c_str(), 1);
  tzset();
  utc = mktime(&localTime);
  if (savedTz[0]) setenv("TZ", savedTz, 1);
  else unsetenv("TZ");
  tzset();
  return utc > 0;
}

bool worldClockCityTime(uint8_t index, struct tm &out) {
  time_t utc;
  if (!worldClockUtcEpoch(utc)) return false;
  const char *tz = index == 0 ? timezoneRule.c_str()
                              : WORLD_CITIES[index < WORLD_CITY_COUNT ? index : 0].tz;
  const char *previousTz = getenv("TZ");
  char savedTz[96];
  if (previousTz) strlcpy(savedTz, previousTz, sizeof(savedTz));
  else savedTz[0] = '\0';
  setenv("TZ", tz ? tz : "UTC0", 1);
  tzset();
  localtime_r(&utc, &out);
  if (savedTz[0]) setenv("TZ", savedTz, 1);
  else unsetenv("TZ");
  tzset();
  return true;
}

void updateWorldClockPage() {
  if (!worldClockPage) return;
  if (worldCityLabel) lv_label_set_text(worldCityLabel, worldCityName(worldCityIndex));
  struct tm cityTime {};
  if (!worldClockCityTime(worldCityIndex, cityTime)) {
    if (worldTimeLabel) lv_label_set_text(worldTimeLabel, "--:--");
    if (worldSecondsLabel) lv_label_set_text(worldSecondsLabel, "--");
    if (worldDateLabel) lv_label_set_text(worldDateLabel, "");
    if (worldStatusLabel) lv_label_set_text(worldStatusLabel, tr(STR_WORLD_CLOCK_NO_TIME));
    return;
  }
  char text[16];
  snprintf(text, sizeof(text), "%02d:%02d", cityTime.tm_hour, cityTime.tm_min);
  if (worldTimeLabel) lv_label_set_text(worldTimeLabel, text);
  snprintf(text, sizeof(text), ":%02d", cityTime.tm_sec);
  if (worldSecondsLabel) lv_label_set_text(worldSecondsLabel, text);
  snprintf(text, sizeof(text), "%02u %s %04u",
           static_cast<unsigned>(cityTime.tm_mday),
           calendarMonthName(static_cast<uint8_t>(cityTime.tm_mon)),
           static_cast<unsigned>(cityTime.tm_year + 1900));
  if (worldDateLabel) lv_label_set_text(worldDateLabel, text);
  if (worldStatusLabel) lv_label_set_text(worldStatusLabel, "");
}

void serviceWorldClock() {
  if (!worldClockPageVisible || lowPowerMode) return;
  if (millis() - worldLastUpdateAt < 1000) return;
  worldLastUpdateAt = millis();
  updateWorldClockPage();
}

void selectAdjacentWorldCity(int8_t direction) {
  if (!direction || lowPowerMode) return;
  markUserActivity();
  const int next = (static_cast<int>(worldCityIndex) + WORLD_CITY_COUNT +
                    (direction > 0 ? 1 : -1)) % WORLD_CITY_COUNT;
  worldCityIndex = static_cast<uint8_t>(next);
  preferences.putUChar("world_city", worldCityIndex);
  updateWorldClockPage();
  refreshWorldCityListStyles();
}

void refreshWorldCityListStyles() {
  if (!worldCityListList) return;
  const uint32_t count = lv_obj_get_child_cnt(worldCityListList);
  for (uint32_t i = 0; i < count; ++i) {
    lv_obj_t *row = lv_obj_get_child(worldCityListList, i);
    const bool selected = i == worldCityIndex;
    lv_obj_set_style_bg_color(row,
      lv_color_hex(selected ? themeAccent(UI_ACCENT_BLUE) : themeSurface(0x0D0D12)),
      LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_color(row,
      lv_color_hex(selected ? themeAccent(UI_ACCENT_BLUE) : themeBorder(0x282C37)),
      LV_PART_MAIN);
    if (lv_obj_get_child_cnt(row))
      lv_obj_set_style_text_color(lv_obj_get_child(row, 0),
        selected ? lv_color_white() : themeText(), 0);
  }
}

void rebuildWorldCityList() {
  if (!worldCityListList) return;
  lv_obj_clean(worldCityListList);
  for (uint8_t i = 0; i < WORLD_CITY_COUNT; ++i) {
    lv_obj_t *row = lv_obj_create(worldCityListList);
    lv_obj_set_size(row, SCREEN_WIDTH - 72, 58);
    lv_obj_set_style_radius(row, 18, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_pad_all(row, 14, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(row, [](lv_event_t *event) {
      if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
      markUserActivity();
      const uint8_t index = static_cast<uint8_t>(
        reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
      if (index < WORLD_CITY_COUNT) {
        worldCityIndex = index;
        preferences.putUChar("world_city", worldCityIndex);
        updateWorldClockPage();
        refreshWorldCityListStyles();
        worldCityListVisible = false;
        lv_obj_add_flag(worldCityListPanel, LV_OBJ_FLAG_HIDDEN);
      }
    }, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, worldCityName(i));
    lv_obj_set_width(label, 300);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, &lv_font_ui_20, 0);
    lv_obj_set_style_text_color(label, themeText(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
  }
  refreshWorldCityListStyles();
}

void hideWorldCityList() {
  worldCityListVisible = false;
  if (worldCityListPanel) lv_obj_add_flag(worldCityListPanel, LV_OBJ_FLAG_HIDDEN);
}

void worldCityListGestureEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_GESTURE) return;
  lv_indev_t *input = lv_indev_get_act();
  if (!input) return;
  if (logicalGestureDirection(input) == LV_DIR_TOP) {
    gestureStartX = -1;
    gestureStartY = -1;
    worldCityListVisible = false;
    lv_obj_add_flag(worldCityListPanel, LV_OBJ_FLAG_HIDDEN);
  }
}

void worldCityListEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  worldCityListVisible = !worldCityListVisible;
  if (worldCityListVisible) {
    refreshWorldCityListStyles();
    lv_obj_clear_flag(worldCityListPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(worldCityListPanel);
  } else {
    lv_obj_add_flag(worldCityListPanel, LV_OBJ_FLAG_HIDDEN);
  }
}

void worldPreviousEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) selectAdjacentWorldCity(-1);
}
void worldNextEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) selectAdjacentWorldCity(1);
}
void worldBackEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) hideWorldClockPage(true, true);
}

void showWorldClockPage(bool fade) {
  if (lowPowerMode || worldClockPageVisible) return;
  if (fade) {
    beginScreenFade(activeBrightness);
    hideAppMenu(false);
    showWorldClockPage(false);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
    return;
  }
  if (!worldClockPage) createWorldClockPage(lv_scr_act());
  if (!worldClockPage) return;
  worldClockPageVisible = true;
  worldLastUpdateAt = 0;
  lv_obj_clear_flag(worldClockPage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(worldClockPage);
  updateWorldClockPage();
}

void hideWorldClockPage(bool fade, bool returnToMenu) {
  if (!worldClockPage) return;
  if (!worldClockPageVisible && lv_obj_has_flag(worldClockPage, LV_OBJ_FLAG_HIDDEN)) return;
  if (fade) {
    beginScreenFade(activeBrightness);
    hideWorldClockPage(false, returnToMenu);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
    return;
  }
  worldClockPageVisible = false;
  worldCityListVisible = false;
  if (worldCityListPanel) lv_obj_add_flag(worldCityListPanel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(worldClockPage, LV_OBJ_FLAG_HIDDEN);
  if (returnToMenu && !lowPowerMode) showAppMenu(false);
}

void createWorldClockPage(lv_obj_t *screen) {
  worldClockPage = lv_obj_create(screen);
  lv_obj_remove_style_all(worldClockPage);
  lv_obj_set_pos(worldClockPage, 0, 0);
  lv_obj_set_size(worldClockPage, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(worldClockPage, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(worldClockPage, LV_OPA_COVER, 0);
  lv_obj_clear_flag(worldClockPage, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(worldClockPage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(worldClockPage, appPageGestureEvent, LV_EVENT_GESTURE, nullptr);

  lv_obj_t *back = createRadioControlButton(worldClockPage, 52, -190, 26,
                                            worldBackEvent, TABLER_ICON_CHEVRON_LEFT, 26);
  (void)back;
  lv_obj_t *title = lv_label_create(worldClockPage);
  i18nRegister(title, STR_WORLD_CLOCK);
  lv_obj_set_style_text_font(title, &lv_font_ui_24, 0);
  lv_obj_set_style_text_letter_space(title, 2, 0);
  lv_obj_set_style_text_color(title, themeText(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

  worldCityLabel = lv_label_create(worldClockPage);
  lv_obj_set_size(worldCityLabel, SCREEN_WIDTH - 80, 40);
  lv_label_set_long_mode(worldCityLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(worldCityLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(worldCityLabel, &lv_font_ui_30, 0);
  lv_obj_set_style_text_color(worldCityLabel, lv_color_hex(themeAccent(UI_ACCENT_BLUE)), 0);
  lv_obj_align(worldCityLabel, LV_ALIGN_TOP_MID, 0, 86);

  worldTimeLabel = lv_label_create(worldClockPage);
  lv_label_set_text(worldTimeLabel, "--:--");
  lv_obj_set_style_text_font(worldTimeLabel, currentStandbyClockFont(), 0);
  lv_obj_set_style_text_color(worldTimeLabel, themeText(), 0);
  lv_obj_align(worldTimeLabel, LV_ALIGN_TOP_MID, -24, 150);

  worldSecondsLabel = lv_label_create(worldClockPage);
  lv_label_set_text(worldSecondsLabel, ":--");
  lv_obj_set_style_text_font(worldSecondsLabel, &lv_font_ui_32, 0);
  lv_obj_set_style_text_color(worldSecondsLabel, themeTextDim(0x9299A3), 0);
  lv_obj_align(worldSecondsLabel, LV_ALIGN_TOP_MID, 168, 214);

  worldDateLabel = lv_label_create(worldClockPage);
  lv_obj_set_style_text_font(worldDateLabel, &lv_font_ui_22, 0);
  lv_obj_set_style_text_color(worldDateLabel, themeTextDim(0xDCEBFF), 0);
  lv_obj_align(worldDateLabel, LV_ALIGN_TOP_MID, 0, 268);

  worldStatusLabel = lv_label_create(worldClockPage);
  lv_obj_set_size(worldStatusLabel, SCREEN_WIDTH - 80, 30);
  lv_label_set_long_mode(worldStatusLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(worldStatusLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(worldStatusLabel, &lv_font_ui_18, 0);
  lv_obj_set_style_text_color(worldStatusLabel, themeTextDim(0xB8C0CC), 0);
  lv_obj_align(worldStatusLabel, LV_ALIGN_TOP_MID, 0, 306);

  createRadioControlButton(worldClockPage, 58, -110, 360, worldPreviousEvent,
                           TABLER_ICON_CHEVRON_LEFT, 28);
  createRadioControlButton(worldClockPage, 74, 0, 352, worldCityListEvent,
                           TABLER_ICON_PLAYLIST, 30);
  createRadioControlButton(worldClockPage, 58, 110, 360, worldNextEvent,
                           TABLER_ICON_CHEVRON_RIGHT, 28);

  lv_obj_t *hint = lv_label_create(worldClockPage);
  i18nRegister(hint, STR_WORLD_CLOCK_HINT);
  lv_obj_set_style_text_font(hint, &lv_font_ui_16, 0);
  lv_obj_set_style_text_color(hint, themeTextDim(0x777D8D), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);

  worldCityListPanel = lv_obj_create(worldClockPage);
  lv_obj_remove_style_all(worldCityListPanel);
  lv_obj_set_size(worldCityListPanel, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_pos(worldCityListPanel, 0, 0);
  lv_obj_set_style_bg_color(worldCityListPanel, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(worldCityListPanel, LV_OPA_COVER, 0);
  lv_obj_add_flag(worldCityListPanel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(worldCityListPanel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(worldCityListPanel, worldCityListGestureEvent, LV_EVENT_GESTURE, nullptr);

  lv_obj_t *listTitle = lv_label_create(worldCityListPanel);
  i18nRegister(listTitle, STR_WORLD_CLOCK_CITIES);
  lv_obj_set_style_text_font(listTitle, &lv_font_ui_24, 0);
  lv_obj_set_style_text_letter_space(listTitle, 3, 0);
  lv_obj_set_style_text_color(listTitle, themeText(), 0);
  lv_obj_align(listTitle, LV_ALIGN_TOP_MID, 0, 25);

  worldCityListList = lv_obj_create(worldCityListPanel);
  lv_obj_set_pos(worldCityListList, 24, 75);
  lv_obj_set_size(worldCityListList, SCREEN_WIDTH - 48, SCREEN_HEIGHT - 94);
  lv_obj_set_flex_flow(worldCityListList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(worldCityListList, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(worldCityListList, 0, 0);
  lv_obj_set_style_pad_row(worldCityListList, 10, 0);
  lv_obj_set_style_bg_opa(worldCityListList, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(worldCityListList, 0, 0);
  lv_obj_set_style_width(worldCityListList, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(worldCityListList,
                            lv_color_hex(themeAccent(UI_ACCENT_BLUE)), LV_PART_SCROLLBAR);
  lv_obj_add_flag(worldCityListList, LV_OBJ_FLAG_GESTURE_BUBBLE);
  rebuildWorldCityList();
  lv_obj_add_flag(worldCityListPanel, LV_OBJ_FLAG_HIDDEN);

  worldCityIndex = min<uint8_t>(preferences.getUChar("world_city", 0),
                                WORLD_CITY_COUNT - 1);
  updateWorldClockPage();
  lv_obj_add_flag(worldClockPage, LV_OBJ_FLAG_HIDDEN);
}

void refreshWorldClockTheme() {
  if (!worldClockPage) return;
  lv_obj_set_style_bg_color(worldClockPage, themePageBackground(), 0);
  if (worldCityLabel)
    lv_obj_set_style_text_color(worldCityLabel, lv_color_hex(themeAccent(UI_ACCENT_BLUE)), 0);
  if (worldTimeLabel) lv_obj_set_style_text_color(worldTimeLabel, themeText(), 0);
  if (worldSecondsLabel) lv_obj_set_style_text_color(worldSecondsLabel, themeTextDim(0x9299A3), 0);
  if (worldDateLabel) lv_obj_set_style_text_color(worldDateLabel, themeTextDim(0xDCEBFF), 0);
  if (worldStatusLabel) lv_obj_set_style_text_color(worldStatusLabel, themeTextDim(0xB8C0CC), 0);
  const uint32_t count = lv_obj_get_child_cnt(worldClockPage);
  for (uint32_t i = 0; i < count; ++i) {
    lv_obj_t *child = lv_obj_get_child(worldClockPage, i);
    if (!lv_obj_check_type(child, &lv_btn_class)) continue;
    lv_obj_set_style_bg_color(child, lv_color_hex(themeSurface(0x0A0A0A)), LV_PART_MAIN);
    lv_obj_set_style_border_color(child, lv_color_hex(themeBorder(0x343846)), LV_PART_MAIN);
    lv_obj_set_style_bg_color(child, lv_color_hex(themeAccent(UI_ACCENT_BLUE)),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    for (uint32_t j = 0; j < lv_obj_get_child_cnt(child); ++j)
      setTablerIconColor(lv_obj_get_child(child, j), themeIcon());
  }
  if (worldCityListPanel) {
    lv_obj_set_style_bg_color(worldCityListPanel, themePageBackground(), 0);
    if (lv_obj_get_child_cnt(worldCityListPanel))
      lv_obj_set_style_text_color(lv_obj_get_child(worldCityListPanel, 0), themeText(), 0);
    if (worldCityListList)
      lv_obj_set_style_bg_color(worldCityListList,
        lv_color_hex(themeAccent(UI_ACCENT_BLUE)), LV_PART_SCROLLBAR);
  }
  refreshWorldCityListStyles();
  updateWorldClockPage();
}
