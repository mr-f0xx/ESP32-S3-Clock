// Full-screen app menu opened by swiping upward from the clock's bottom edge.
// App icons use LVGL Alpha-8 assets generated from Tabler's official
// 24x24, 2 px round-stroke artwork, scaled uniformly for the launcher.
// Sources: https://tabler.io/icons/icon/clock
//          https://tabler.io/icons/icon/calendar
//          https://tabler.io/icons/icon/alarm
//          https://tabler.io/icons/icon/adjustments
//          https://tabler.io/icons/icon/music
// Included once by 08_LVGL_Clock.ino inside its private namespace.

constexpr int16_t APP_MENU_SIZE = 480;
constexpr int16_t APP_MENU_TILE_SIZE = 176;
// Larger glyphs retain 28 px of clearance on every side of a tile.
constexpr lv_coord_t APP_MENU_ICON_SIZE = 120;
constexpr int16_t APP_MENU_TILE_LEFT = 52;
constexpr int16_t APP_MENU_TILE_TOP = 52;
constexpr int16_t APP_MENU_TILE_GAP_X = 24;
constexpr int16_t APP_MENU_TILE_GAP_Y = 24;
constexpr uint8_t APP_MENU_ITEMS_PER_PAGE = 4;
lv_obj_t *clockModeSelector = nullptr;
bool clockModeChoiceBusy = false;
uint32_t clockModeChoiceSuppressedUntil = 0;
// Power-off confirmation modal. Kept separate from the clock-mode selector so
// a destructive action can never be triggered by the same touch release that
// opened the launcher tile.
lv_obj_t *powerOffSelector = nullptr;
lv_obj_t *powerOffMessageLabel = nullptr;
lv_obj_t *powerOffConfirmButton = nullptr;
bool powerOffChoiceBusy = false;
uint32_t powerOffChoiceSuppressedUntil = 0;
// Track the launcher tiles so a theme change can recolor every key without
// rebuilding the menu. The base color is the per-app accent used in the black
// theme; the amber theme turns each tile into a black, amber-outlined key.
constexpr uint8_t APP_MENU_TILE_CAPACITY = 16;
lv_obj_t *appMenuTileButtons[APP_MENU_TILE_CAPACITY] = {};
uint32_t appMenuTileColors[APP_MENU_TILE_CAPACITY] = {};
// Indexed by launcher tile creation order, including the three utility apps.
// Each tile gets its own trace and halo instead of one theme-wide accent.
constexpr uint32_t SPECTRUM_TILE_COLORS[APP_MENU_TILE_CAPACITY] = {
  0xFFB020, 0xFF5C7A, 0xA8B6FF, 0x46A8FF,
  0x3FE0D0, 0xFF558F, 0xB57AFF, 0x53F0AA,
  0x43E8FF, 0xFF6551, 0x6B9CFF, 0xE96AFF,
  0xFFE05C, 0x8CE85A, 0xFF9656, 0xE8A4FF
};
uint8_t appMenuTileCount = 0;

enum AppMenuItem : uint8_t {
  APP_MENU_CLOCK = 0,
  APP_MENU_ALARM,
  APP_MENU_SETTINGS,
  APP_MENU_MUSIC,
  APP_MENU_CALENDAR,
  APP_MENU_VIDEO,
  APP_MENU_WEATHER,
  APP_MENU_AUDIOBOOK,
  APP_MENU_RADIO,
  APP_MENU_POWER,
  APP_MENU_WORLD_CLOCK,
  APP_MENU_PHOTO_FRAME,
  APP_MENU_TIMER,
  APP_MENU_CALCULATOR,
  APP_MENU_NOTES,
  APP_MENU_STOPWATCH
};

void cancelPendingAppLaunches() {
  // A touch sample must never leave two page constructors armed. Hide requests
  // are not touched here because they belong to an already-visible app.
  calendarPageShowRequested = false;
  alarmPageShowRequested = false;
  musicPageShowRequested = false;
  audiobookPageShowRequested = false;
  weatherPageShowRequested = false;
  weatherInfoPageShowRequested = false;
  appMenuHideRequested = false;
}

void consumeCurrentTouchRelease() {
  lv_indev_t *input = lv_indev_get_act();
  if (input) lv_indev_wait_release(input);
}

bool powerOffModalVisible() {
  return powerOffSelector &&
         !lv_obj_has_flag(powerOffSelector, LV_OBJ_FLAG_HIDDEN);
}

bool clockModeModalVisible() {
  return clockModeSelector &&
         !lv_obj_has_flag(clockModeSelector, LV_OBJ_FLAG_HIDDEN);
}

// The confirmation copy is dynamic because it depends on the live USB/VBUS
// state and on whether the AXP2101 was detected at boot.
void updatePowerOffMessage() {
  if (!powerOffMessageLabel || !powerOffConfirmButton) return;
  UiStr message = STR_POWER_OFF_NOTE;
  if (!pmuReady) message = STR_POWER_OFF_UNAVAILABLE;
  else if (power.isVbusIn()) message = STR_POWER_OFF_USB_NOTE;
  lv_label_set_text(powerOffMessageLabel, tr(message));
  if (pmuReady) lv_obj_clear_state(powerOffConfirmButton, LV_STATE_DISABLED);
  else lv_obj_add_state(powerOffConfirmButton, LV_STATE_DISABLED);
}

void showPowerOffSelector() {
  if (!powerOffSelector || deviceShuttingDown) return;
  powerOffChoiceBusy = false;
  powerOffChoiceSuppressedUntil = millis() + 250;
  updatePowerOffMessage();
  lv_obj_clear_flag(powerOffSelector, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(powerOffSelector);
}

void hidePowerOffSelector() {
  if (!powerOffSelector) return;
  lv_obj_add_flag(powerOffSelector, LV_OBJ_FLAG_HIDDEN);
  powerOffChoiceBusy = false;
}

void powerOffChoiceEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  const uint32_t now = millis();
  if (!powerOffModalVisible() || powerOffChoiceBusy || deviceShuttingDown ||
      static_cast<int32_t>(now - powerOffChoiceSuppressedUntil) < 0) return;
  const uintptr_t choice = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  if (choice > 2) return;
  powerOffChoiceBusy = true;
  appMenuClickSuppressedUntil = now + 500;
  consumeCurrentTouchRelease();
  cancelPendingAppLaunches();
  markUserActivity();
  if (choice != 1) {
    hidePowerOffSelector();
    return;
  }
  if (!pmuReady) {
    powerOffChoiceBusy = false;
    updatePowerOffMessage();
    return;
  }
  // requestDevicePowerOff() only returns when the shutdown sequence could not
  // complete (for example the weather SD worker did not reach a safe idle).
  if (!requestDevicePowerOff()) {
    powerOffChoiceBusy = false;
    showAppMenu(false);
    showPowerOffSelector();
    if (powerOffMessageLabel)
      lv_label_set_text(powerOffMessageLabel, tr(STR_POWER_OFF_BUSY));
  }
}

// Launcher gestures must not page or dismiss the menu while a modal is open.
void appMenuGestureEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_GESTURE) return;
  if (powerOffModalVisible() || clockModeModalVisible()) {
    gestureStartX = -1;
    gestureStartY = -1;
    return;
  }
  mainUiGestureEvent(event);
}

void updateAppMenuPageVisibility() {
  for (uint8_t i = 0; i < APP_MENU_PAGE_COUNT; ++i) {
    if (appMenuPages[i]) {
      if (i == appMenuPageIndex) lv_obj_clear_flag(appMenuPages[i], LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(appMenuPages[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (appMenuPageDots[i]) {
      lv_obj_set_size(appMenuPageDots[i], i == appMenuPageIndex ? 9 : 6,
                      i == appMenuPageIndex ? 9 : 6);
      lv_obj_set_style_bg_opa(appMenuPageDots[i],
                              i == appMenuPageIndex ? LV_OPA_COVER : LV_OPA_30, 0);
      lv_obj_align(appMenuPageDots[i], LV_ALIGN_BOTTOM_MID,
                   (static_cast<int16_t>(i) * 2 - (APP_MENU_PAGE_COUNT - 1)) * 9, -13);
    }
  }
}

void updateCalendarToday() {
  if (!calendarWidget || !rtcReady || !rtcTimeValid) return;
  RTC_DateTime current;
  if (!readRtcTime(current)) return;
  lv_calendar_set_today_date(calendarWidget, current.getYear(),
                             current.getMonth(), current.getDay());
  lv_calendar_set_showed_date(calendarWidget, current.getYear(), current.getMonth());
}

// Apply the selected language to the calendar's month and weekday names.
// Re-setting the showed date makes the arrow header rebuild its month label.
void applyCalendarLocalization() {
  if (!calendarWidget) return;
  const uint8_t lang = uiLanguage < UI_LANG_COUNT ? uiLanguage : UI_LANG_EN;
  lv_calendar_header_arrow_set_month_names(
    const_cast<const char **>(CALENDAR_MONTHS[lang]));
  lv_calendar_set_day_names(calendarWidget,
                            const_cast<const char **>(CALENDAR_DAYS[lang]));
  const lv_calendar_date_t *shown = lv_calendar_get_showed_date(calendarWidget);
  if (shown)
    lv_calendar_set_showed_date(calendarWidget, shown->year, shown->month);
}

// Re-apply the active theme to the full-screen calendar.
void refreshCalendarTheme() {
  if (!calendarPage) return;
  lv_obj_set_style_bg_color(calendarPage, themePageBackground(), 0);
  if (!calendarWidget) return;
  lv_obj_set_style_bg_color(calendarWidget, themePageBackground(), 0);
  lv_obj_set_style_text_color(calendarWidget, themeText(), 0);
  lv_obj_t *header = lv_obj_get_child(calendarWidget, 0);
  if (header) {
    lv_obj_t *monthLabel = lv_obj_get_child(header, 1);
    if (monthLabel) lv_obj_set_style_text_color(monthLabel, themeText(), 0);
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(header); ++i) {
      lv_obj_t *child = lv_obj_get_child(header, i);
      if (lv_obj_get_child_cnt(child))
        setTablerIconColor(lv_obj_get_child(child, 0), themeIcon());
    }
  }
  applyCalendarLocalization();
}

void showAppMenu(bool fade) {
  if (!appMenu || lowPowerMode || appMenuVisible || appMenuShowRequested ||
      controlPanelVisible || controlPanelShowRequested || calendarPageVisible ||
      calendarPageShowRequested || alarmPageVisible || alarmPageShowRequested ||
      musicPageVisible || musicPageShowRequested || audiobookPageVisible ||
      audiobookPageShowRequested || weatherPageVisible ||
      weatherPageShowRequested || weatherInfoPageVisible ||
      weatherInfoPageShowRequested || worldClockPageVisible ||
      photoFramePageVisible || timerPageVisible) return;
  if (fade) {
    appMenuHideRequested = false;
    appMenuShowRequested = true;
    return;
  }

  appMenuVisible = true;
  lv_obj_clear_flag(appMenu, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(appMenu);
}

void hideAppMenu(bool fade) {
  if (!appMenu) return;
  if (!appMenuVisible && lv_obj_has_flag(appMenu, LV_OBJ_FLAG_HIDDEN) &&
      !appMenuShowRequested && !appMenuHideRequested) return;
  if (fade) {
    appMenuShowRequested = false;
    appMenuHideRequested = true;
    return;
  }

  appMenuVisible = false;
  appMenuHideRequested = false;
  appMenuShowRequested = false;
  if (clockModeSelector) lv_obj_add_flag(clockModeSelector, LV_OBJ_FLAG_HIDDEN);
  hidePowerOffSelector();
  lv_obj_add_flag(appMenu, LV_OBJ_FLAG_HIDDEN);
}

void showCalendarPage(bool fade) {
  if (!calendarPage || lowPowerMode || calendarPageVisible ||
      calendarPageShowRequested) return;
  if (fade) {
    calendarPageHideRequested = false;
    calendarPageShowRequested = true;
    return;
  }

  updateCalendarToday();
  calendarPageVisible = true;
  lv_obj_clear_flag(calendarPage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(calendarPage);
}

void hideCalendarPage(bool fade, bool returnToMenu) {
  if (!calendarPage) return;
  if (!calendarPageVisible && lv_obj_has_flag(calendarPage, LV_OBJ_FLAG_HIDDEN) &&
      !calendarPageShowRequested && !calendarPageHideRequested) return;
  if (fade) {
    calendarPageShowRequested = false;
    calendarPageHideRequested = true;
    return;
  }

  calendarPageVisible = false;
  calendarPageHideRequested = false;
  calendarPageShowRequested = false;
  lv_obj_add_flag(calendarPage, LV_OBJ_FLAG_HIDDEN);
  if (returnToMenu && !lowPowerMode) showAppMenu(false);
}

void requestCalendarMonthStep(int8_t direction) {
  if (!calendarPageVisible || !calendarWidget || !direction || lowPowerMode) return;
  calendarMonthStepRequested = direction > 0 ? 1 : -1;
}

void processCalendarPageActions() {
  if (!calendarMonthStepRequested || !calendarPageVisible || !calendarWidget ||
      lowPowerMode) return;
  const int8_t direction = calendarMonthStepRequested;
  calendarMonthStepRequested = 0;
  const lv_calendar_date_t *shown = lv_calendar_get_showed_date(calendarWidget);
  if (!shown) return;
  int32_t year = static_cast<int32_t>(shown->year);
  int32_t month = static_cast<int32_t>(shown->month) + direction;
  if (month > 12) {
    month = 1;
    ++year;
  } else if (month < 1) {
    month = 12;
    --year;
  }
  year = constrain(year, 1900, 2099);
  const bool rotated = displayRotation == LV_DISP_ROT_90 ||
                       displayRotation == LV_DISP_ROT_270;
  if (rotated) beginScreenFade(activeBrightness);
  lv_calendar_set_showed_date(calendarWidget, static_cast<uint32_t>(year),
                              static_cast<uint32_t>(month));
  lv_obj_invalidate(calendarPage);
  if (rotated) {
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
  }
}

void handleGlobalNavigationStep(int8_t direction) {
  if (!direction || lowPowerMode || controlPanelVisible || controlPanelShowRequested ||
      appMenuVisible || appMenuShowRequested || timerAlertVisible) return;
  markUserActivity();
  if (calendarPageVisible) {
    requestCalendarMonthStep(direction);
  } else if (alarmPageVisible) {
    selectAlarmSlotRelative(direction);
  } else if (musicPageVisible) {
    selectAdjacentMusicTrack(direction);
  } else if (audiobookPageVisible) {
    selectAdjacentAudiobook(direction);
  } else if (radioPageVisible) {
    selectAdjacentRadioStation(direction);
  } else if (worldClockPageVisible) {
    selectAdjacentWorldCity(direction);
  } else if (photoFramePageVisible) {
    selectAdjacentPhoto(direction);
  } else if (timerPageVisible) {
    adjustTimerMinutes(direction);
  } else if (weatherPageVisible) {
    // Physical +/- and weather swipes share the same ordered scene playlist.
    selectAdjacentWeatherWallpaper(direction);
  } else if (weatherInfoPageVisible) {
    return;
  } else {
    selectAdjacentImage(direction);
  }
}

void processWeatherTouchGesture() {
  const lv_dir_t direction =
    static_cast<lv_dir_t>(weatherTouchGestureDirection);
  if (direction == LV_DIR_NONE) return;
  const int16_t startY = weatherTouchGestureStartY;
  weatherTouchGestureDirection = LV_DIR_NONE;
  weatherTouchGestureStartY = -1;

  static uint32_t suppressedUntil = 0;
  const uint32_t now = millis();
  if ((!weatherPageVisible && !weatherInfoPageVisible) || lowPowerMode ||
      controlPanelVisible ||
      controlPanelShowRequested || screenFadeTransitionActive ||
      orientationTransitionBlanked ||
      (weatherPageVisible && videoPlayerSeekGestureSuppressed()) ||
      static_cast<int32_t>(now - suppressedUntil) < 0) return;
  suppressedUntil = now + 300;
  markUserActivity();

  if (weatherInfoPageVisible) {
    if ((direction == LV_DIR_LEFT || direction == LV_DIR_RIGHT) &&
        handleWeatherInfoHorizontalGesture(direction)) return;
    if (direction == LV_DIR_TOP) hideWeatherInfoPage(true, true);
    else if (direction == LV_DIR_BOTTOM) showControlPanel(true);
    return;
  }

  if (direction == LV_DIR_LEFT) {
    handleGlobalNavigationStep(1);
  } else if (direction == LV_DIR_RIGHT) {
    handleGlobalNavigationStep(-1);
  } else if (direction == LV_DIR_BOTTOM && startY >= 0 &&
             startY <= CONTROL_PANEL_GESTURE_EDGE) {
    showControlPanel(true);
  } else if (direction == LV_DIR_TOP && startY >=
             SCREEN_HEIGHT - CONTROL_PANEL_GESTURE_EDGE) {
    hideWeatherWallpaperPage(true, true);
  } else if (direction == LV_DIR_BOTTOM) {
    handleGlobalNavigationStep(-1);
  } else if (direction == LV_DIR_TOP) {
    handleGlobalNavigationStep(1);
  }
}

void processAppMenuTransition() {
  if (lowPowerMode || screenFadeTransitionActive) return;
  if (appMenuPageStepRequested && appMenuVisible) {
    const int8_t step = appMenuPageStepRequested;
    appMenuPageStepRequested = 0;
    const uint8_t nextPage = constrain(
      static_cast<int16_t>(appMenuPageIndex) + step, 0, APP_MENU_PAGE_COUNT - 1);
    if (nextPage != appMenuPageIndex) {
      beginScreenFade(activeBrightness);
      appMenuPageIndex = nextPage;
      updateAppMenuPageVisibility();
      lv_obj_invalidate(appMenu);
      lv_refr_now(nullptr);
      endScreenFade(activeBrightness);
    }
  } else if (appMenuShowRequested) {
    appMenuShowRequested = false;
    beginScreenFade(activeBrightness);
    showAppMenu(false);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
  } else if (appMenuHideRequested) {
    appMenuHideRequested = false;
    beginScreenFade(activeBrightness);
    hideAppMenu(false);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
  } else if (calendarPageShowRequested) {
    calendarPageShowRequested = false;
    beginScreenFade(activeBrightness);
    hideAppMenu(false);
    showCalendarPage(false);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
  } else if (calendarPageHideRequested) {
    calendarPageHideRequested = false;
    beginScreenFade(activeBrightness);
    hideCalendarPage(false, true);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
  }
}

void appMenuItemEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  // LVGL can emit CLICKED for the tile that originally received a horizontal
  // swipe. The menu gesture handler arms this short release guard so paging
  // and launching an app remain two distinct actions.
  const uint32_t now = millis();
  if (!appMenuVisible || deviceShuttingDown || powerOffModalVisible() ||
      clockModeModalVisible() ||
      static_cast<int32_t>(now - appMenuClickSuppressedUntil) < 0) return;
  const AppMenuItem item = static_cast<AppMenuItem>(
    reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  if (item > APP_MENU_STOPWATCH) return;
  // Guard every normal click too, not only a preceding swipe. CST9217 can
  // occasionally report another press after release, especially near a
  // rotation; without this lock the second sample could launch another tile.
  appMenuClickSuppressedUntil = now + 450;
  consumeCurrentTouchRelease();
  cancelPendingAppLaunches();
  if (item == APP_MENU_CLOCK) {
    if (clockModeSelector) {
      clockModeChoiceBusy = false;
      // Do not let the release which opened the modal immediately select the
      // mode button under the same physical finger position.
      clockModeChoiceSuppressedUntil = now + 250;
      lv_obj_clear_flag(clockModeSelector, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(clockModeSelector);
    }
  } else if (item == APP_MENU_CALENDAR) {
    showCalendarPage(true);
  } else if (item == APP_MENU_VIDEO) {
    showVideoPlayerPage(true);
  } else if (item == APP_MENU_WEATHER) {
    showWeatherInfoPage(true);
  } else if (item == APP_MENU_ALARM) {
    showAlarmPage(true);
  } else if (item == APP_MENU_MUSIC) {
    showMusicPage(true);
  } else if (item == APP_MENU_AUDIOBOOK) {
    showAudiobookPage(true);
  } else if (item == APP_MENU_RADIO) {
    showRadioPage(true);
  } else if (item == APP_MENU_POWER) {
    showPowerOffSelector();
  } else if (item == APP_MENU_WORLD_CLOCK) {
    showWorldClockPage(true);
  } else if (item == APP_MENU_PHOTO_FRAME) {
    showPhotoFramePage(true);
  } else if (item == APP_MENU_TIMER) {
    showTimerPage(true);
  } else if (item >= APP_MENU_CALCULATOR && item <= APP_MENU_STOPWATCH) {
    openUtilityApp(static_cast<uint8_t>(item - APP_MENU_CALCULATOR));
  } else {
    // The Settings tile exposes the same configuration page served over the
    // normal Wi-Fi connection, but through a predictable local hotspot.
    if (!wifiUserEnabled) setWifiUserEnabled(true, true);
    portalRequested = true;
    updateWifiSetupPage();
    lv_obj_clear_flag(wifiSetupPage, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(wifiSetupPage);
  }
}

void wifiSetupBackEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED || !wifiSetupPage) return;
  markUserActivity();
  consumeCurrentTouchRelease();
  appMenuClickSuppressedUntil = millis() + 300;
  stopConfigPortal();
  lv_obj_add_flag(wifiSetupPage, LV_OBJ_FLAG_HIDDEN);
}

void updateWifiSetupPage() {
  if (!wifiSetupPage) return;
  if (!apSsid[0]) {
    const uint64_t chipId = ESP.getEfuseMac() & 0xFFFFFFULL;
    snprintf(apSsid, sizeof(apSsid), "AMOLED-Clock-%06llX", chipId);
  }
  if (wifiSetupSsidLabel)
    lv_label_set_text_fmt(wifiSetupSsidLabel, "%s", apSsid);
  if (wifiSetupPasswordLabel)
    lv_label_set_text(wifiSetupPasswordLabel, tr(STR_OPEN_NETWORK_NO_PASS));
}

void createWifiSetupPage() {
  wifiSetupPage = lv_obj_create(appMenu);
  lv_obj_remove_style_all(wifiSetupPage);
  lv_obj_set_pos(wifiSetupPage, 0, 0);
  lv_obj_set_size(wifiSetupPage, APP_MENU_SIZE, APP_MENU_SIZE);
  lv_obj_set_style_bg_color(wifiSetupPage, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(wifiSetupPage, LV_OPA_COVER, 0);
  lv_obj_clear_flag(wifiSetupPage,
                    LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(wifiSetupPage, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *backButton = lv_btn_create(wifiSetupPage);
  lv_obj_set_size(backButton, 56, 56);
  lv_obj_align(backButton, LV_ALIGN_TOP_LEFT, 22, 22);
  lv_obj_set_style_radius(backButton, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(backButton, lv_color_hex(themeSurface(0x0A0A0A)), 0);
  lv_obj_set_style_bg_opa(backButton, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(backButton, 1, 0);
  lv_obj_set_style_border_color(backButton, lv_color_hex(themeBorder(0x2A2A2A)), 0);
  lv_obj_set_style_shadow_width(backButton, 0, 0);
  lv_obj_clear_flag(backButton,
                    LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(backButton, wifiSetupBackEvent, LV_EVENT_CLICKED, nullptr);
  createTablerIcon(backButton, TABLER_ICON_CHEVRON_LEFT, 30, themeIcon());

  lv_obj_t *title = lv_label_create(wifiSetupPage);
  i18nRegister(title, STR_WIFI_SETUP);
  lv_obj_set_style_text_font(title, &lv_font_ui_30, 0);
  lv_obj_set_style_text_color(title, themeText(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 33);

  lv_obj_t *firstStep = lv_label_create(wifiSetupPage);
  i18nRegister(firstStep, STR_SETUP_STEP1);
  lv_obj_set_width(firstStep, 420);
  lv_obj_set_style_text_align(firstStep, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(firstStep, &lv_font_ui_22, 0);
  lv_obj_set_style_text_color(firstStep, themeTextDim(0xCBD5E1), 0);
  lv_obj_align(firstStep, LV_ALIGN_TOP_MID, 0, 104);

  wifiSetupSsidLabel = lv_label_create(wifiSetupPage);
  lv_obj_set_width(wifiSetupSsidLabel, 430);
  lv_obj_set_style_text_align(wifiSetupSsidLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(wifiSetupSsidLabel, &lv_font_ui_28, 0);
  lv_obj_set_style_text_color(wifiSetupSsidLabel,
                              lv_color_hex(themeAccent(UI_ACCENT_BLUE)), 0);
  lv_obj_align(wifiSetupSsidLabel, LV_ALIGN_TOP_MID, 0, 150);

  wifiSetupPasswordLabel = lv_label_create(wifiSetupPage);
  lv_obj_set_width(wifiSetupPasswordLabel, 420);
  lv_obj_set_style_text_align(wifiSetupPasswordLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(wifiSetupPasswordLabel, &lv_font_ui_22, 0);
  lv_obj_set_style_text_color(wifiSetupPasswordLabel, themeTextDim(0xE2E8F0), 0);
  lv_obj_align(wifiSetupPasswordLabel, LV_ALIGN_TOP_MID, 0, 202);

  lv_obj_t *secondStep = lv_label_create(wifiSetupPage);
  i18nRegister(secondStep, STR_SETUP_STEP2);
  lv_obj_set_width(secondStep, 420);
  lv_obj_set_style_text_align(secondStep, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(secondStep, &lv_font_ui_22, 0);
  lv_obj_set_style_text_color(secondStep, themeTextDim(0xCBD5E1), 0);
  lv_obj_align(secondStep, LV_ALIGN_TOP_MID, 0, 270);

  lv_obj_t *address = lv_label_create(wifiSetupPage);
  lv_label_set_text(address, "http://192.168.4.1");
  lv_obj_set_style_text_font(address, &lv_font_ui_28, 0);
  lv_obj_set_style_text_color(address, lv_color_hex(themeAccent(UI_ACCENT_MINT)), 0);
  lv_obj_align(address, LV_ALIGN_TOP_MID, 0, 316);

  lv_obj_t *note = lv_label_create(wifiSetupPage);
  i18nRegister(note, STR_SETUP_NOTE);
  lv_obj_set_width(note, 400);
  lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(note, &lv_font_ui_16, 0);
  lv_obj_set_style_text_color(note, themeTextDim(0x94A3B8), 0);
  lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -50);

  updateWifiSetupPage();
  lv_obj_add_flag(wifiSetupPage, LV_OBJ_FLAG_HIDDEN);
}

// Shared by full-screen apps. Both weather screens finalize gestures from the
// physical touch release so their child objects cannot swallow vertical swipes.
void appPageGestureEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_GESTURE || lowPowerMode) return;
  lv_indev_t *input = lv_indev_get_act();
  if (!input || controlPanelVisible || controlPanelShowRequested) return;
  markUserActivity();
  if (timerAlertVisible) return;
  lv_dir_t direction = logicalGestureDirection(input);
  if (weatherPageVisible || weatherInfoPageVisible) {
    // This object callback is intentionally inert so child labels, direct-LCD
    // video and LVGL gesture bubbling cannot duplicate the physical gesture.
    return;
  }
  if (direction == LV_DIR_BOTTOM && gestureStartY >= 0 &&
      gestureStartY <= CONTROL_PANEL_GESTURE_EDGE) {
    gestureStartX = -1;
    gestureStartY = -1;
    showControlPanel(true);
    return;
  }
  if (musicPlaylistVisible) {
    gestureStartX = -1;
    gestureStartY = -1;
    if (direction == LV_DIR_TOP) hideMusicPlaylist();
    return;
  }
  if (audiobookBooklistVisible) {
    gestureStartX = -1;
    gestureStartY = -1;
    if (direction == LV_DIR_TOP) hideAudiobookBooklist();
    return;
  }
  if (radioStationListVisible) {
    gestureStartX = -1;
    gestureStartY = -1;
    if (direction == LV_DIR_TOP) hideRadioStationList();
    return;
  }
  if (worldCityListVisible) {
    gestureStartX = -1;
    gestureStartY = -1;
    if (direction == LV_DIR_TOP) hideWorldCityList();
    return;
  }
  if (direction == LV_DIR_LEFT) {
    gestureStartX = -1;
    gestureStartY = -1;
    handleGlobalNavigationStep(1);
  } else if (direction == LV_DIR_RIGHT) {
    gestureStartX = -1;
    gestureStartY = -1;
    handleGlobalNavigationStep(-1);
  } else if (!alarmPageVisible && direction == LV_DIR_BOTTOM &&
             gestureStartY >= 0 &&
             gestureStartY <= CONTROL_PANEL_GESTURE_EDGE) {
    gestureStartX = -1;
    gestureStartY = -1;
    showControlPanel(true);
  } else if (direction == LV_DIR_TOP) {
    gestureStartX = -1;
    gestureStartY = -1;
    if (calendarPageVisible) hideCalendarPage(true, true);
    else if (musicPageVisible) hideMusicPage(true, true);
    else if (audiobookPageVisible) hideAudiobookPage(true, true);
    else if (radioPageVisible) hideRadioPage(true, true);
    else if (worldClockPageVisible) hideWorldClockPage(true, true);
    else if (photoFramePageVisible) hidePhotoFramePage(true, true);
    else if (timerPageVisible) hideTimerPage(true, true);
    else if (weatherInfoPageVisible) hideWeatherInfoPage(true, true);
  }
}

void clockModeChoiceEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  const uint32_t now = millis();
  if (!clockModeSelector ||
      lv_obj_has_flag(clockModeSelector, LV_OBJ_FLAG_HIDDEN) ||
      clockModeChoiceBusy ||
      static_cast<int32_t>(now - clockModeChoiceSuppressedUntil) < 0) return;
  const uintptr_t choice = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  if (choice > 2) return;
  clockModeChoiceBusy = true;
  // Suppress both another modal callback and a click-through into the app menu
  // after the modal becomes hidden. Only the next explicit opening unlocks it.
  appMenuClickSuppressedUntil = now + 500;
  consumeCurrentTouchRelease();
  cancelPendingAppLaunches();
  lv_obj_add_flag(clockModeSelector, LV_OBJ_FLAG_HIDDEN);
  markUserActivity();
  if (choice == 0) hideAppMenu(true);
  else if (choice == 1) showWeatherWallpaperPage(true);
}

lv_obj_t *createClockModeButton(lv_obj_t *parent, int16_t x,
                                const lv_img_dsc_t *icon,
                                uint32_t color, uintptr_t choice) {
  lv_obj_t *button = lv_btn_create(parent);
  lv_obj_set_pos(button, x, 94);
  lv_obj_set_size(button, 156, 156);
  lv_obj_set_style_radius(button, 30, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(themeAccent(color)), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(button, themeIsAmber() ? 2 : 1, 0);
  lv_obj_set_style_border_color(button,
    themeIsAmber() ? lv_color_hex(CRT_AMBER)
                 : lv_color_hex(themeBorder(0xFFFFFF)), 0);
  lv_obj_set_style_border_opa(button, themeIsAmber() ? LV_OPA_COVER : LV_OPA_20, 0);
  lv_obj_add_event_cb(button, clockModeChoiceEvent, LV_EVENT_CLICKED,
                      reinterpret_cast<void *>(choice));
  createOfficialTablerIcon(button, icon,
    themeIsAmber() ? lv_color_hex(CRT_AMBER) : themeTextOnAccent());
  return button;
}

void createClockModeSelector() {
  clockModeSelector = lv_obj_create(appMenu);
  lv_obj_remove_style_all(clockModeSelector);
  lv_obj_set_size(clockModeSelector, APP_MENU_SIZE, APP_MENU_SIZE);
  lv_obj_set_pos(clockModeSelector, 0, 0);
  lv_obj_set_style_bg_color(clockModeSelector, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(clockModeSelector, LV_OPA_90, 0);
  lv_obj_clear_flag(clockModeSelector, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(clockModeSelector, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *panel = lv_obj_create(clockModeSelector);
  lv_obj_set_size(panel, 420, 310);
  lv_obj_center(panel);
  lv_obj_set_style_radius(panel, 38, 0);
  lv_obj_set_style_bg_color(panel, lv_color_hex(themeSurface(0x0D0D0D)), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(themeBorder(0xFFFFFF)), 0);
  lv_obj_set_style_border_opa(panel, themeIsAmber() ? LV_OPA_COVER : LV_OPA_20, 0);
  lv_obj_set_style_pad_all(panel, 0, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(panel);
  i18nRegister(title, STR_CHOOSE_CLOCK_MODE);
  lv_obj_set_style_text_font(title, &lv_font_ui_28, 0);
  lv_obj_set_style_text_color(title, themeText(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

  // Symmetric 40 px side margins and a 28 px center gap. The buttons are
  // deliberately icon-only; artwork is generated from official Tabler PNGs.
  createClockModeButton(panel, 40, &tabler_photo, UI_ACCENT_BLUE, 0);
  createClockModeButton(panel, 224, &tabler_cloud, UI_ACCENT_MINT, 1);

  lv_obj_t *close = lv_btn_create(panel);
  lv_obj_set_size(close, 42, 42);
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -12, 10);
  lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
  lv_obj_add_event_cb(close, clockModeChoiceEvent, LV_EVENT_CLICKED,
                      reinterpret_cast<void *>(2));
  createOfficialTablerIcon(close, &tabler_x, themeIcon());

  lv_obj_add_flag(clockModeSelector, LV_OBJ_FLAG_HIDDEN);
}

void createPowerOffSelector() {
  powerOffSelector = lv_obj_create(appMenu);
  lv_obj_remove_style_all(powerOffSelector);
  lv_obj_set_size(powerOffSelector, APP_MENU_SIZE, APP_MENU_SIZE);
  lv_obj_set_pos(powerOffSelector, 0, 0);
  lv_obj_set_style_bg_color(powerOffSelector, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(powerOffSelector, LV_OPA_90, 0);
  lv_obj_clear_flag(powerOffSelector, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(powerOffSelector, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *panel = lv_obj_create(powerOffSelector);
  lv_obj_set_size(panel, 420, 360);
  lv_obj_center(panel);
  lv_obj_set_style_radius(panel, 38, 0);
  lv_obj_set_style_bg_color(panel, lv_color_hex(themeSurface(0x0D0D0D)), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_border_color(panel, lv_color_hex(themeBorder(0xFFFFFF)), 0);
  lv_obj_set_style_border_opa(panel, themeIsAmber() ? LV_OPA_COVER : LV_OPA_20, 0);
  lv_obj_set_style_pad_all(panel, 0, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(panel);
  i18nRegister(title, STR_POWER_OFF_QUESTION);
  lv_obj_set_style_text_font(title, &lv_font_ui_28, 0);
  lv_obj_set_style_text_color(title, themeText(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

  powerOffMessageLabel = lv_label_create(panel);
  lv_obj_set_width(powerOffMessageLabel, 380);
  lv_label_set_long_mode(powerOffMessageLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(powerOffMessageLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(powerOffMessageLabel, &lv_font_ui_18, 0);
  lv_obj_set_style_text_color(powerOffMessageLabel, themeTextDim(0xCBD5E1), 0);
  lv_obj_align(powerOffMessageLabel, LV_ALIGN_TOP_MID, 0, 82);

  lv_obj_t *cancel = lv_btn_create(panel);
  lv_obj_set_pos(cancel, 40, 254);
  lv_obj_set_size(cancel, 160, 64);
  lv_obj_set_style_radius(cancel, 24, 0);
  lv_obj_set_style_bg_color(cancel, lv_color_hex(themeSurface(0x1F2937)), 0);
  lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(cancel, 1, 0);
  lv_obj_set_style_border_color(cancel, lv_color_hex(themeBorder(0x374151)), 0);
  lv_obj_set_style_shadow_width(cancel, 0, 0);
  lv_obj_add_event_cb(cancel, powerOffChoiceEvent, LV_EVENT_CLICKED,
                      reinterpret_cast<void *>(0));
  lv_obj_t *cancelLabel = lv_label_create(cancel);
  i18nRegister(cancelLabel, STR_CANCEL);
  lv_obj_set_style_text_font(cancelLabel, &lv_font_ui_22, 0);
  lv_obj_set_style_text_color(cancelLabel, themeText(), 0);
  lv_obj_center(cancelLabel);

  powerOffConfirmButton = lv_btn_create(panel);
  lv_obj_set_pos(powerOffConfirmButton, 220, 254);
  lv_obj_set_size(powerOffConfirmButton, 160, 64);
  lv_obj_set_style_radius(powerOffConfirmButton, 24, 0);
  lv_obj_set_style_bg_color(powerOffConfirmButton,
    lv_color_hex(themeAccent(UI_ACCENT_ROSE)), 0);
  lv_obj_set_style_bg_opa(powerOffConfirmButton, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(powerOffConfirmButton, 1, 0);
  lv_obj_set_style_border_color(powerOffConfirmButton,
    themeIsAmber() ? lv_color_hex(CRT_AMBER)
                 : lv_color_hex(themeBorder(0xFFFFFF)), 0);
  lv_obj_set_style_border_opa(powerOffConfirmButton,
    themeIsAmber() ? LV_OPA_COVER : LV_OPA_20, 0);
  lv_obj_set_style_shadow_width(powerOffConfirmButton, 0, 0);
  lv_obj_set_style_bg_opa(powerOffConfirmButton, LV_OPA_40, LV_STATE_DISABLED);
  lv_obj_set_style_text_opa(powerOffConfirmButton, LV_OPA_40, LV_STATE_DISABLED);
  lv_obj_add_event_cb(powerOffConfirmButton, powerOffChoiceEvent, LV_EVENT_CLICKED,
                      reinterpret_cast<void *>(1));
  lv_obj_t *confirmLabel = lv_label_create(powerOffConfirmButton);
  i18nRegister(confirmLabel, STR_POWER_OFF);
  lv_obj_set_style_text_font(confirmLabel, &lv_font_ui_22, 0);
  lv_obj_set_style_text_color(confirmLabel, themeTextOnAccent(), 0);
  lv_obj_center(confirmLabel);

  lv_obj_t *close = lv_btn_create(panel);
  lv_obj_set_size(close, 42, 42);
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -12, 10);
  lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
  lv_obj_add_event_cb(close, powerOffChoiceEvent, LV_EVENT_CLICKED,
                      reinterpret_cast<void *>(2));
  createOfficialTablerIcon(close, &tabler_x, themeIcon());

  updatePowerOffMessage();
  lv_obj_add_flag(powerOffSelector, LV_OBJ_FLAG_HIDDEN);
}

// Icon color for a launcher tile in the active theme. Neon palettes collapse
// every accent to one hue; amber uses the phosphor trace; black keeps white.
lv_color_t appTileIconColor(uint32_t accent) {
  if (themeIsAmber()) return lv_color_hex(CRT_AMBER);
  if (themePerTileColors()) return lv_color_hex(accent);
  if (themeIsNeon()) return lv_color_hex(themeAccent(accent));
  return lv_color_white();
}

void applyAppTileTheme(lv_obj_t *button, uint32_t color) {
  if (themeIsSpectrum()) {
    lv_obj_set_style_bg_color(button, lv_color_hex(0x10101B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_border_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_spread(button, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(button, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(button, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_30, LV_PART_MAIN | LV_STATE_PRESSED);
  } else if (themeIsGlow()) {
    // Keep the full tile touch target, but let the true-black launcher show
    // through: no gray card, outline or halo behind the icon. The icon keeps
    // its neon color; pressing the tile still gives colored touch feedback.
    lv_obj_set_style_bg_color(button, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_30, LV_PART_MAIN | LV_STATE_PRESSED);
  } else if (themeIsAmber()) {
    lv_obj_set_style_bg_color(button, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(CRT_AMBER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(CRT_AMBER),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_30, LV_PART_MAIN | LV_STATE_PRESSED);
  } else if (themeIsNeon()) {
    // Retro / cyberpunk / aurora / ocean: dark tinted tile, neon trace border
    // and a soft accent glow so the launcher visibly adopts the palette.
    lv_obj_set_style_bg_color(button, lv_color_hex(themeSurface(0x0D0D12)),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(themeAccent(color)),
                                  LV_PART_MAIN);
    lv_obj_set_style_border_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    // Only the palettes that define a glow color get a halo; aurora / ocean
    // report no glow, and a width-16 shadow with color 0 would just be a
    // faint black smudge on the AMOLED background.
    lv_obj_set_style_shadow_width(button, themeHasGlow() ? 16 : 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(button, lv_color_hex(themeGlowColor()),
                                  LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(button, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(themeAccent(color)),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_30, LV_PART_MAIN | LV_STATE_PRESSED);
  } else {
    lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(button, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(button, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(button, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_30, LV_PART_MAIN | LV_STATE_PRESSED);
  }
  if (lv_obj_get_child_cnt(button)) {
    lv_obj_t *icon = lv_obj_get_child(button, 0);
    const lv_color_t iconColor = appTileIconColor(color);
    // Image tiles use img_recolor; the vector audiobook tile uses line/border
    // colors. Applying both is safe for either representation.
    lv_obj_set_style_img_recolor(icon, iconColor, 0);
    setTablerIconColor(icon, iconColor);
    // Never shadow the icon container: its box becomes a glowing circle rather
    // than following the transparent glyph. Keep the colored strokes unhaloed.
    lv_obj_set_style_shadow_width(icon, 0, 0);
  }
}

lv_obj_t *createAppTile(lv_obj_t *parent, AppMenuItem item, uint8_t slot,
                        uint32_t color) {
  const int16_t column = slot % 2;
  const int16_t row = slot / 2;
  lv_obj_t *button = lv_btn_create(parent);
  lv_obj_set_pos(button,
                 APP_MENU_TILE_LEFT + column * (APP_MENU_TILE_SIZE + APP_MENU_TILE_GAP_X),
                 APP_MENU_TILE_TOP + row * (APP_MENU_TILE_SIZE + APP_MENU_TILE_GAP_Y));
  lv_obj_set_size(button, APP_MENU_TILE_SIZE, APP_MENU_TILE_SIZE);
  lv_obj_set_style_radius(button, 38, LV_PART_MAIN);
  applyAppTileTheme(button, themePerTileColors() && appMenuTileCount < APP_MENU_TILE_CAPACITY
                            ? SPECTRUM_TILE_COLORS[appMenuTileCount] : color);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(button, appMenuItemEvent, LV_EVENT_CLICKED,
                      reinterpret_cast<void *>(static_cast<uintptr_t>(item)));
  if (appMenuTileCount < APP_MENU_TILE_CAPACITY) {
    appMenuTileButtons[appMenuTileCount] = button;
    appMenuTileColors[appMenuTileCount] = color;
    ++appMenuTileCount;
  }
  return button;
}

void createTablerAppIcon(lv_obj_t *button, const lv_img_dsc_t *icon) {
  lv_obj_t *image = lv_img_create(button);
  lv_img_set_src(image, icon);
  lv_obj_center(image);
  // lv_img_set_src() sets the pivot to the source center. Scaling around that
  // pivot keeps the enlarged glyph centered without allocating a larger asset.
  const lv_coord_t sourceSize = max(icon->header.w, icon->header.h);
  lv_img_set_antialias(image, true);
  lv_img_set_zoom(image, LV_IMG_ZOOM_NONE * APP_MENU_ICON_SIZE / sourceSize);
  // Neon palettes collapse every accent to one hue, so the tile's original
  // color does not matter; pass 0 as the accent seed.
  lv_obj_set_style_img_recolor(image, appTileIconColor(0), 0);
  lv_obj_set_style_img_recolor_opa(image, LV_OPA_COVER, 0);
  lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

void createCalendarPage(lv_obj_t *screen) {
  calendarPage = lv_obj_create(screen);
  lv_obj_remove_style_all(calendarPage);
  lv_obj_set_pos(calendarPage, 0, 0);
  lv_obj_set_size(calendarPage, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(calendarPage, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(calendarPage, LV_OPA_COVER, 0);
  lv_obj_clear_flag(calendarPage,
                    LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(calendarPage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(calendarPage, appPageGestureEvent, LV_EVENT_GESTURE, nullptr);

  calendarWidget = lv_calendar_create(calendarPage);
  lv_obj_set_pos(calendarWidget, 0, 0);
  lv_obj_set_size(calendarWidget, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_radius(calendarWidget, 0, 0);
  lv_obj_set_style_bg_color(calendarWidget, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(calendarWidget, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(calendarWidget, 0, 0);
  lv_obj_set_style_pad_all(calendarWidget, 18, 0);
  lv_obj_set_style_text_color(calendarWidget, themeText(), 0);
  lv_obj_clear_flag(calendarWidget,
                    LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *header = lv_calendar_header_arrow_create(calendarWidget);
  lv_obj_set_height(header, 64);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *previousMonthButton = lv_obj_get_child(header, 0);
  lv_obj_t *monthLabel = lv_obj_get_child(header, 1);
  lv_obj_t *nextMonthButton = lv_obj_get_child(header, 2);
  lv_obj_set_size(previousMonthButton, 64, 64);
  lv_obj_set_size(nextMonthButton, 64, 64);
  lv_obj_set_style_text_font(monthLabel, &lv_font_ui_30, 0);
  lv_obj_set_style_text_font(previousMonthButton, &lv_font_ui_30, 0);
  lv_obj_set_style_text_font(nextMonthButton, &lv_font_ui_30, 0);
  lv_obj_set_style_text_color(monthLabel, themeText(), 0);
  applyCalendarLocalization();
  lv_obj_set_style_bg_opa(previousMonthButton, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(previousMonthButton, LV_OPA_TRANSP,
                          LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(nextMonthButton, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(nextMonthButton, LV_OPA_TRANSP,
                          LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(previousMonthButton, 0, 0);
  lv_obj_set_style_shadow_width(nextMonthButton, 0, 0);
  lv_obj_set_style_bg_img_src(previousMonthButton, nullptr, 0);
  lv_obj_set_style_bg_img_src(nextMonthButton, nullptr, 0);
  createTablerIcon(previousMonthButton, TABLER_ICON_CHEVRON_LEFT, 30, themeIcon());
  createTablerIcon(nextMonthButton, TABLER_ICON_CHEVRON_RIGHT, 30, themeIcon());

  lv_obj_t *dayMatrix = lv_calendar_get_btnmatrix(calendarWidget);
  lv_obj_set_style_text_font(dayMatrix, &lv_font_ui_28, LV_PART_ITEMS);
  // The calendar is a viewer, so dates do not need pressed/selected states.
  // Removing their touch redraws prevents many small rotated flushes while an
  // upward gesture crosses cells, which is visible as tearing at 90/270 deg.
  lv_obj_clear_flag(dayMatrix,
                    LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE |
                    LV_OBJ_FLAG_PRESS_LOCK | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(dayMatrix, LV_OBJ_FLAG_GESTURE_BUBBLE);
  const uint32_t calendarChildCount = lv_obj_get_child_cnt(calendarWidget);
  for (uint32_t i = 0; i < calendarChildCount; ++i) {
    lv_obj_t *child = lv_obj_get_child(calendarWidget, i);
    lv_obj_clear_flag(child, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(child, LV_OBJ_FLAG_GESTURE_BUBBLE);
    const uint32_t grandchildCount = lv_obj_get_child_cnt(child);
    for (uint32_t j = 0; j < grandchildCount; ++j) {
      lv_obj_t *grandchild = lv_obj_get_child(child, j);
      lv_obj_clear_flag(grandchild, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(grandchild, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
  }
  updateCalendarToday();

  lv_obj_add_flag(calendarPage, LV_OBJ_FLAG_HIDDEN);
}

void createAppMenu() {
  lv_obj_t *screen = lv_scr_act();
  appMenu = lv_obj_create(screen);
  lv_obj_remove_style_all(appMenu);
  lv_obj_set_pos(appMenu, 0, 0);
  lv_obj_set_size(appMenu, APP_MENU_SIZE, APP_MENU_SIZE);
  lv_obj_set_style_bg_color(appMenu, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(appMenu, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(appMenu, 0, 0);
  lv_obj_set_style_pad_all(appMenu, 0, 0);
  lv_obj_clear_flag(appMenu, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(appMenu, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(appMenu, appMenuGestureEvent, LV_EVENT_GESTURE, nullptr);

  for (uint8_t i = 0; i < APP_MENU_PAGE_COUNT; ++i) {
    appMenuPages[i] = lv_obj_create(appMenu);
    lv_obj_remove_style_all(appMenuPages[i]);
    lv_obj_set_size(appMenuPages[i], APP_MENU_SIZE, APP_MENU_SIZE);
    lv_obj_set_pos(appMenuPages[i], 0, 0);
    lv_obj_clear_flag(appMenuPages[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(appMenuPages[i],
                    LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  }

  lv_obj_t *clockButton = createAppTile(
    appMenuPages[0], APP_MENU_CLOCK, 0, UI_ACCENT_AMBER);
  createTablerAppIcon(clockButton, &tabler_clock);
  lv_obj_t *videoButton = createAppTile(
    appMenuPages[0], APP_MENU_VIDEO, 1, UI_ACCENT_BILIBILI);
  createTablerAppIcon(videoButton, &tabler_device_tv);
  lv_obj_t *settingsTile = createAppTile(
    appMenuPages[0], APP_MENU_SETTINGS, 2, 0x6B7280);
  createTablerAppIcon(settingsTile, &tabler_adjustments);
  lv_obj_t *weatherButton = createAppTile(
    appMenuPages[0], APP_MENU_WEATHER, 3, UI_ACCENT_BLUE);
  createTablerAppIcon(weatherButton, &tabler_cloud);

  lv_obj_t *calendarButton = createAppTile(
    appMenuPages[1], APP_MENU_CALENDAR, 0, UI_ACCENT_BLUE);
  createTablerAppIcon(calendarButton, &tabler_calendar);
  lv_obj_t *alarmButton = createAppTile(
    appMenuPages[1], APP_MENU_ALARM, 1, UI_ACCENT_ROSE);
  createTablerAppIcon(alarmButton, &tabler_alarm);
  lv_obj_t *musicButton = createAppTile(
    appMenuPages[1], APP_MENU_MUSIC, 2, UI_ACCENT_VIOLET);
  createTablerAppIcon(musicButton, &tabler_music);
  lv_obj_t *audiobookButton = createAppTile(
    appMenuPages[1], APP_MENU_AUDIOBOOK, 3, UI_ACCENT_MINT);
  createTablerIcon(audiobookButton, TABLER_ICON_AUDIOBOOK, APP_MENU_ICON_SIZE,
    appTileIconColor(UI_ACCENT_MINT));

  lv_obj_t *radioButton = createAppTile(
    appMenuPages[2], APP_MENU_RADIO, 0, UI_ACCENT_MINT);
  createTablerIcon(radioButton, TABLER_ICON_RADIO, APP_MENU_ICON_SIZE,
    appTileIconColor(UI_ACCENT_MINT));
  lv_obj_t *powerButton = createAppTile(
    appMenuPages[2], APP_MENU_POWER, 1, UI_ACCENT_ROSE);
  createTablerIcon(powerButton, TABLER_ICON_POWER, APP_MENU_ICON_SIZE,
    appTileIconColor(UI_ACCENT_ROSE));
  lv_obj_t *worldButton = createAppTile(
    appMenuPages[2], APP_MENU_WORLD_CLOCK, 2, UI_ACCENT_BLUE);
  createTablerIcon(worldButton, TABLER_ICON_WORLD, APP_MENU_ICON_SIZE,
    appTileIconColor(UI_ACCENT_BLUE));
  lv_obj_t *photoButton = createAppTile(
    appMenuPages[2], APP_MENU_PHOTO_FRAME, 3, UI_ACCENT_VIOLET);
  createTablerIcon(photoButton, TABLER_ICON_PHOTO, APP_MENU_ICON_SIZE,
    appTileIconColor(UI_ACCENT_VIOLET));
  lv_obj_t *timerButton = createAppTile(
    appMenuPages[3], APP_MENU_TIMER, 0, UI_ACCENT_AMBER);
  createTablerIcon(timerButton, TABLER_ICON_TIMER, APP_MENU_ICON_SIZE,
    appTileIconColor(UI_ACCENT_AMBER));

  const AppMenuItem utilityItems[] = {APP_MENU_CALCULATOR, APP_MENU_NOTES, APP_MENU_STOPWATCH};
  const uint32_t utilityColors[] = {UI_ACCENT_BLUE, UI_ACCENT_MINT, UI_ACCENT_ROSE};
  const TablerIconKind utilityIcons[] = {TABLER_ICON_CALCULATOR, TABLER_ICON_NOTES,
                                         TABLER_ICON_STOPWATCH};
  for (uint8_t i = 0; i < 3; ++i) {
    lv_obj_t *tile = createAppTile(appMenuPages[3], utilityItems[i], i + 1, utilityColors[i]);
    // Icon-only tile (no title), matching the other launcher tiles. The icon is
    // child 0 so applyAppTileTheme() recolors it on theme change.
    createTablerIcon(tile, utilityIcons[i], APP_MENU_ICON_SIZE,
      appTileIconColor(utilityColors[i]));
  }

  for (uint8_t i = 0; i < APP_MENU_PAGE_COUNT; ++i) {
    appMenuPageDots[i] = lv_obj_create(appMenu);
    lv_obj_remove_style_all(appMenuPageDots[i]);
    lv_obj_set_style_radius(appMenuPageDots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(appMenuPageDots[i], themeText(), 0);
    lv_obj_align(appMenuPageDots[i], LV_ALIGN_BOTTOM_MID,
                 (static_cast<int16_t>(i) * 2 - (APP_MENU_PAGE_COUNT - 1)) * 9, -13);
    lv_obj_clear_flag(appMenuPageDots[i],
                      LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  }
  updateAppMenuPageVisibility();
  createClockModeSelector();
  createPowerOffSelector();
  createWifiSetupPage();

  lv_obj_add_flag(appMenu, LV_OBJ_FLAG_HIDDEN);
  createCalendarPage(screen);
  createMusicPage(screen);
  createAudiobookPage(screen);
  createRadioPage(screen);
  // The newer app pages are constructed lazily on first open. Building every
  // page eagerly at boot exhausted LVGL's fixed object pool on the device and
  // left the panel black before the launcher could be shown.
  createWeatherWallpaperPage(screen);
  createWeatherInfoPage(screen);
}

// Re-apply the active theme to the launcher, its tiles and the modal pages.
void refreshAppMenuTheme() {
  if (!appMenu) return;
  lv_obj_set_style_bg_color(appMenu, themePageBackground(), 0);
  for (uint8_t i = 0; i < appMenuTileCount; ++i)
    applyAppTileTheme(appMenuTileButtons[i],
                      themePerTileColors() ? SPECTRUM_TILE_COLORS[i] : appMenuTileColors[i]);
  for (uint8_t i = 0; i < APP_MENU_PAGE_COUNT; ++i) {
    if (appMenuPageDots[i])
      lv_obj_set_style_bg_color(appMenuPageDots[i], themeText(), 0);
  }
  if (clockModeSelector) {
    lv_obj_set_style_bg_color(clockModeSelector, lv_color_black(), 0);
    // Rebuild the two mode keys' colors in place.
    lv_obj_t *panel = lv_obj_get_child(clockModeSelector, 0);
    if (panel) {
      lv_obj_set_style_bg_color(panel, lv_color_hex(themeSurface(0x0D0D0D)), 0);
      lv_obj_set_style_border_color(panel, lv_color_hex(themeBorder(0xFFFFFF)), 0);
      lv_obj_set_style_border_opa(panel,
        themeIsAmber() ? LV_OPA_COVER : LV_OPA_20, 0);
      const uint32_t childCount = lv_obj_get_child_cnt(panel);
      for (uint32_t i = 0; i < childCount; ++i) {
        lv_obj_t *child = lv_obj_get_child(panel, i);
        // Mode buttons are 156x156; the close key is 42x42 and the title is a
        // label. Recolor the two large buttons and the close icon.
        if (lv_obj_get_width(child) == 156) {
          const bool amber = themeIsAmber();
          const uint32_t keyAccent =
            themeAccent(i == 1 ? UI_ACCENT_BLUE : UI_ACCENT_MINT);
          lv_obj_set_style_bg_color(child,
            amber ? lv_color_black() : lv_color_hex(keyAccent), 0);
          lv_obj_set_style_border_width(child, amber ? 2 : 1, 0);
          lv_obj_set_style_border_color(child,
            amber ? lv_color_hex(CRT_AMBER)
                  : lv_color_hex(themeBorder(0xFFFFFF)), 0);
          lv_obj_set_style_border_opa(child, amber ? LV_OPA_COVER : LV_OPA_20, 0);
          if (lv_obj_get_child_cnt(child))
            lv_obj_set_style_img_recolor(lv_obj_get_child(child, 0),
              amber ? lv_color_hex(CRT_AMBER) : themeTextOnAccent(), 0);
        }
      }
    }
  }
  if (powerOffSelector) {
    lv_obj_set_style_bg_color(powerOffSelector, lv_color_black(), 0);
    lv_obj_t *panel = lv_obj_get_child(powerOffSelector, 0);
    if (panel) {
      lv_obj_set_style_bg_color(panel, lv_color_hex(themeSurface(0x0D0D0D)), 0);
      lv_obj_set_style_border_color(panel, lv_color_hex(themeBorder(0xFFFFFF)), 0);
      lv_obj_set_style_border_opa(panel,
        themeIsAmber() ? LV_OPA_COVER : LV_OPA_20, 0);
      const uint32_t childCount = lv_obj_get_child_cnt(panel);
      for (uint32_t i = 0; i < childCount; ++i) {
        lv_obj_t *child = lv_obj_get_child(panel, i);
        if (child == powerOffMessageLabel) {
          lv_obj_set_style_text_color(child, themeTextDim(0xCBD5E1), 0);
        } else if (child == powerOffConfirmButton) {
          lv_obj_set_style_bg_color(child,
            lv_color_hex(themeAccent(UI_ACCENT_ROSE)), 0);
          lv_obj_set_style_border_color(child,
            themeIsAmber() ? lv_color_hex(CRT_AMBER)
                         : lv_color_hex(themeBorder(0xFFFFFF)), 0);
          lv_obj_set_style_border_opa(child,
            themeIsAmber() ? LV_OPA_COVER : LV_OPA_20, 0);
          if (lv_obj_get_child_cnt(child))
            lv_obj_set_style_text_color(lv_obj_get_child(child, 0),
              themeTextOnAccent(), 0);
        } else if (lv_obj_get_width(child) == 160) {
          lv_obj_set_style_bg_color(child, lv_color_hex(themeSurface(0x1F2937)), 0);
          lv_obj_set_style_border_color(child, lv_color_hex(themeBorder(0x374151)), 0);
          if (lv_obj_get_child_cnt(child))
            lv_obj_set_style_text_color(lv_obj_get_child(child, 0), themeText(), 0);
        } else if (lv_obj_get_width(child) == 42 && lv_obj_get_child_cnt(child)) {
          lv_obj_set_style_img_recolor(lv_obj_get_child(child, 0), themeIcon(), 0);
        } else if (lv_obj_get_child_cnt(child) == 0) {
          lv_obj_set_style_text_color(child, themeText(), 0);
        }
      }
    }
    updatePowerOffMessage();
  }
  if (wifiSetupPage) {
    lv_obj_set_style_bg_color(wifiSetupPage, themePageBackground(), 0);
    lv_obj_set_style_text_color(wifiSetupSsidLabel,
      lv_color_hex(themeAccent(UI_ACCENT_BLUE)), 0);
  }
}
