// Full-screen app menu opened by swiping upward from the clock's bottom edge.
// App icons are native-size LVGL Alpha-8 assets generated from Tabler's
// official 24x24, 2 px round-stroke outline artwork.
// Sources: https://tabler.io/icons/icon/clock
//          https://tabler.io/icons/icon/calendar
//          https://tabler.io/icons/icon/alarm
//          https://tabler.io/icons/icon/adjustments
//          https://tabler.io/icons/icon/music
// Included once by 08_LVGL_Clock.ino inside its private namespace.

constexpr int16_t APP_MENU_SIZE = 480;
constexpr int16_t APP_MENU_TILE_SIZE = 176;
constexpr int16_t APP_MENU_TILE_LEFT = 52;
constexpr int16_t APP_MENU_TILE_TOP = 52;
constexpr int16_t APP_MENU_TILE_GAP_X = 24;
constexpr int16_t APP_MENU_TILE_GAP_Y = 24;
constexpr uint8_t APP_MENU_ITEMS_PER_PAGE = 4;
lv_obj_t *clockModeSelector = nullptr;
bool clockModeChoiceBusy = false;
uint32_t clockModeChoiceSuppressedUntil = 0;

enum AppMenuItem : uint8_t {
  APP_MENU_CLOCK = 0,
  APP_MENU_ALARM,
  APP_MENU_SETTINGS,
  APP_MENU_MUSIC,
  APP_MENU_CALENDAR,
  APP_MENU_VIDEO,
  APP_MENU_WEATHER
};

void cancelPendingAppLaunches() {
  // A touch sample must never leave two page constructors armed. Hide requests
  // are not touched here because they belong to an already-visible app.
  calendarPageShowRequested = false;
  alarmPageShowRequested = false;
  musicPageShowRequested = false;
  weatherPageShowRequested = false;
  weatherInfoPageShowRequested = false;
  appMenuHideRequested = false;
}

void consumeCurrentTouchRelease() {
  lv_indev_t *input = lv_indev_get_act();
  if (input) lv_indev_wait_release(input);
}

void updateAppMenuPageVisibility() {
  for (uint8_t i = 0; i < 2; ++i) {
    if (appMenuPages[i]) {
      if (i == appMenuPageIndex) lv_obj_clear_flag(appMenuPages[i], LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(appMenuPages[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (appMenuPageDots[i]) {
      lv_obj_set_size(appMenuPageDots[i], i == appMenuPageIndex ? 9 : 6,
                      i == appMenuPageIndex ? 9 : 6);
      lv_obj_set_style_bg_opa(appMenuPageDots[i],
                              i == appMenuPageIndex ? LV_OPA_COVER : LV_OPA_30, 0);
      lv_obj_align(appMenuPageDots[i], LV_ALIGN_BOTTOM_MID, i ? 9 : -9, -13);
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

void showAppMenu(bool fade) {
  if (!appMenu || lowPowerMode || appMenuVisible || appMenuShowRequested ||
      controlPanelVisible || controlPanelShowRequested || calendarPageVisible ||
      calendarPageShowRequested || alarmPageVisible || alarmPageShowRequested ||
      musicPageVisible || musicPageShowRequested || weatherPageVisible ||
      weatherPageShowRequested || weatherInfoPageVisible ||
      weatherInfoPageShowRequested) return;
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
      appMenuVisible || appMenuShowRequested) return;
  markUserActivity();
  if (calendarPageVisible) {
    requestCalendarMonthStep(direction);
  } else if (alarmPageVisible) {
    selectAlarmSlotRelative(direction);
  } else if (musicPageVisible) {
    selectAdjacentMusicTrack(direction);
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
      static_cast<int16_t>(appMenuPageIndex) + step, 0, 1);
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
  if (!appMenuVisible ||
      (clockModeSelector &&
       !lv_obj_has_flag(clockModeSelector, LV_OBJ_FLAG_HIDDEN)) ||
      static_cast<int32_t>(now - appMenuClickSuppressedUntil) < 0) return;
  const AppMenuItem item = static_cast<AppMenuItem>(
    reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  if (item > APP_MENU_WEATHER) return;
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
    lv_label_set_text(wifiSetupPasswordLabel, "Open network - no password");
}

void createWifiSetupPage() {
  wifiSetupPage = lv_obj_create(appMenu);
  lv_obj_remove_style_all(wifiSetupPage);
  lv_obj_set_pos(wifiSetupPage, 0, 0);
  lv_obj_set_size(wifiSetupPage, APP_MENU_SIZE, APP_MENU_SIZE);
  lv_obj_set_style_bg_color(wifiSetupPage, lv_color_hex(0x0D0F14), 0);
  lv_obj_set_style_bg_opa(wifiSetupPage, LV_OPA_COVER, 0);
  lv_obj_clear_flag(wifiSetupPage,
                    LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(wifiSetupPage, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *backButton = lv_btn_create(wifiSetupPage);
  lv_obj_set_size(backButton, 56, 56);
  lv_obj_align(backButton, LV_ALIGN_TOP_LEFT, 22, 22);
  lv_obj_set_style_radius(backButton, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(backButton, lv_color_hex(0x20242D), 0);
  lv_obj_set_style_bg_opa(backButton, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(backButton, 1, 0);
  lv_obj_set_style_border_color(backButton, lv_color_hex(0x3D4654), 0);
  lv_obj_set_style_shadow_width(backButton, 0, 0);
  lv_obj_clear_flag(backButton,
                    LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(backButton, wifiSetupBackEvent, LV_EVENT_CLICKED, nullptr);
  createTablerIcon(backButton, TABLER_ICON_CHEVRON_LEFT, 30);

  lv_obj_t *title = lv_label_create(wifiSetupPage);
  lv_label_set_text(title, "Wi-Fi Setup");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_30, 0);
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 33);

  lv_obj_t *firstStep = lv_label_create(wifiSetupPage);
  lv_label_set_text(firstStep, "1. Connect your phone to this Wi-Fi");
  lv_obj_set_width(firstStep, 420);
  lv_obj_set_style_text_align(firstStep, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(firstStep, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(firstStep, lv_color_hex(0xCBD5E1), 0);
  lv_obj_align(firstStep, LV_ALIGN_TOP_MID, 0, 104);

  wifiSetupSsidLabel = lv_label_create(wifiSetupPage);
  lv_obj_set_width(wifiSetupSsidLabel, 430);
  lv_obj_set_style_text_align(wifiSetupSsidLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(wifiSetupSsidLabel, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(wifiSetupSsidLabel, lv_color_hex(UI_ACCENT_BLUE), 0);
  lv_obj_align(wifiSetupSsidLabel, LV_ALIGN_TOP_MID, 0, 150);

  wifiSetupPasswordLabel = lv_label_create(wifiSetupPage);
  lv_obj_set_width(wifiSetupPasswordLabel, 420);
  lv_obj_set_style_text_align(wifiSetupPasswordLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(wifiSetupPasswordLabel, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(wifiSetupPasswordLabel, lv_color_hex(0xE2E8F0), 0);
  lv_obj_align(wifiSetupPasswordLabel, LV_ALIGN_TOP_MID, 0, 202);

  lv_obj_t *secondStep = lv_label_create(wifiSetupPage);
  lv_label_set_text(secondStep, "2. Open this address in your browser");
  lv_obj_set_width(secondStep, 420);
  lv_obj_set_style_text_align(secondStep, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(secondStep, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(secondStep, lv_color_hex(0xCBD5E1), 0);
  lv_obj_align(secondStep, LV_ALIGN_TOP_MID, 0, 270);

  lv_obj_t *address = lv_label_create(wifiSetupPage);
  lv_label_set_text(address, "http://192.168.4.1");
  lv_obj_set_style_text_font(address, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(address, lv_color_hex(UI_ACCENT_MINT), 0);
  lv_obj_align(address, LV_ALIGN_TOP_MID, 0, 316);

  lv_obj_t *note = lv_label_create(wifiSetupPage);
  lv_label_set_text(note,
                    "Use the same configuration page as the Wi-Fi web UI.");
  lv_obj_set_width(note, 400);
  lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(note, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(note, lv_color_hex(0x94A3B8), 0);
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
  lv_dir_t direction = logicalGestureDirection(input);
  if (weatherPageVisible || weatherInfoPageVisible) {
    // This object callback is intentionally inert so child labels, direct-LCD
    // video and LVGL gesture bubbling cannot duplicate the physical gesture.
    return;
  }
  if (musicPlaylistVisible) {
    gestureStartX = -1;
    gestureStartY = -1;
    if (direction == LV_DIR_TOP) hideMusicPlaylist();
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
  lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, lv_color_white(), 0);
  lv_obj_set_style_border_opa(button, LV_OPA_20, 0);
  lv_obj_add_event_cb(button, clockModeChoiceEvent, LV_EVENT_CLICKED,
                      reinterpret_cast<void *>(choice));
  createOfficialTablerIcon(button, icon);
  return button;
}

void createClockModeSelector() {
  clockModeSelector = lv_obj_create(appMenu);
  lv_obj_remove_style_all(clockModeSelector);
  lv_obj_set_size(clockModeSelector, APP_MENU_SIZE, APP_MENU_SIZE);
  lv_obj_set_pos(clockModeSelector, 0, 0);
  lv_obj_set_style_bg_color(clockModeSelector, lv_color_hex(0x080A0F), 0);
  lv_obj_set_style_bg_opa(clockModeSelector, LV_OPA_90, 0);
  lv_obj_clear_flag(clockModeSelector, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(clockModeSelector, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *panel = lv_obj_create(clockModeSelector);
  lv_obj_set_size(panel, 420, 310);
  lv_obj_center(panel);
  lv_obj_set_style_radius(panel, 38, 0);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0x171A22), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(panel, 1, 0);
  lv_obj_set_style_border_color(panel, lv_color_white(), 0);
  lv_obj_set_style_border_opa(panel, LV_OPA_20, 0);
  lv_obj_set_style_pad_all(panel, 0, 0);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(panel);
  lv_label_set_text(title, "Choose Clock Mode");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
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
  createOfficialTablerIcon(close, &tabler_x);

  lv_obj_add_flag(clockModeSelector, LV_OBJ_FLAG_HIDDEN);
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
  lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_border_opa(button, LV_OPA_20, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 18, LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(button, LV_OPA_20, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF),
                            LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(button, LV_OPA_30, LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(button, appMenuItemEvent, LV_EVENT_CLICKED,
                      reinterpret_cast<void *>(static_cast<uintptr_t>(item)));
  return button;
}

void createTablerAppIcon(lv_obj_t *button, const lv_img_dsc_t *icon) {
  lv_obj_t *image = lv_img_create(button);
  lv_img_set_src(image, icon);
  lv_obj_center(image);
  lv_obj_set_style_img_recolor(image, lv_color_white(), 0);
  lv_obj_set_style_img_recolor_opa(image, LV_OPA_COVER, 0);
  lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

void createCalendarPage(lv_obj_t *screen) {
  calendarPage = lv_obj_create(screen);
  lv_obj_remove_style_all(calendarPage);
  lv_obj_set_pos(calendarPage, 0, 0);
  lv_obj_set_size(calendarPage, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(calendarPage, lv_color_hex(0x101114), 0);
  lv_obj_set_style_bg_opa(calendarPage, LV_OPA_COVER, 0);
  lv_obj_clear_flag(calendarPage,
                    LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(calendarPage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(calendarPage, appPageGestureEvent, LV_EVENT_GESTURE, nullptr);

  calendarWidget = lv_calendar_create(calendarPage);
  lv_obj_set_pos(calendarWidget, 0, 0);
  lv_obj_set_size(calendarWidget, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_radius(calendarWidget, 0, 0);
  lv_obj_set_style_bg_color(calendarWidget, lv_color_hex(0x181C24), 0);
  lv_obj_set_style_bg_opa(calendarWidget, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(calendarWidget, 0, 0);
  lv_obj_set_style_pad_all(calendarWidget, 18, 0);
  lv_obj_set_style_text_color(calendarWidget, lv_color_white(), 0);
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
  lv_obj_set_style_text_font(monthLabel, &lv_font_montserrat_30, 0);
  lv_obj_set_style_text_font(previousMonthButton, &lv_font_montserrat_30, 0);
  lv_obj_set_style_text_font(nextMonthButton, &lv_font_montserrat_30, 0);
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
  createTablerIcon(previousMonthButton, TABLER_ICON_CHEVRON_LEFT, 30);
  createTablerIcon(nextMonthButton, TABLER_ICON_CHEVRON_RIGHT, 30);

  lv_obj_t *dayMatrix = lv_calendar_get_btnmatrix(calendarWidget);
  lv_obj_set_style_text_font(dayMatrix, &lv_font_montserrat_28, LV_PART_ITEMS);
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
  lv_obj_set_style_bg_color(appMenu, lv_color_hex(0x101114), 0);
  lv_obj_set_style_bg_opa(appMenu, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(appMenu, 0, 0);
  lv_obj_set_style_pad_all(appMenu, 0, 0);
  lv_obj_clear_flag(appMenu, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(appMenu, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(appMenu, mainUiGestureEvent, LV_EVENT_GESTURE, nullptr);

  for (uint8_t i = 0; i < 2; ++i) {
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
  lv_obj_set_x(musicButton, (APP_MENU_SIZE - APP_MENU_TILE_SIZE) / 2);
  createTablerAppIcon(musicButton, &tabler_music);

  for (uint8_t i = 0; i < 2; ++i) {
    appMenuPageDots[i] = lv_obj_create(appMenu);
    lv_obj_remove_style_all(appMenuPageDots[i]);
    lv_obj_set_style_radius(appMenuPageDots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(appMenuPageDots[i], lv_color_white(), 0);
    lv_obj_align(appMenuPageDots[i], LV_ALIGN_BOTTOM_MID, i ? 9 : -9, -13);
    lv_obj_clear_flag(appMenuPageDots[i],
                      LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  }
  updateAppMenuPageVisibility();
  createClockModeSelector();
  createWifiSetupPage();

  lv_obj_add_flag(appMenu, LV_OBJ_FLAG_HIDDEN);
  createCalendarPage(screen);
  createMusicPage(screen);
  createWeatherWallpaperPage(screen);
  createWeatherInfoPage(screen);
}
