// Three-slot RTC alarm editor and full-screen alarm notification.
// Included once by 08_LVGL_Clock.ino inside its private namespace.

constexpr uint8_t ALARM_SLOT_COUNT = 3;
lv_obj_t *alarmToggleStatusLabel = nullptr;

void adjustAlarmTime(int8_t delta, bool adjustMinute);

uint32_t alarmMinuteStamp(const RTC_DateTime &current) {
  uint32_t stamp = current.getYear();
  stamp = stamp * 13U + current.getMonth();
  stamp = stamp * 32U + current.getDay();
  stamp = stamp * 24U + current.getHour();
  return stamp * 60U + current.getMinute();
}

void persistAlarm(uint8_t index) {
  if (index >= ALARM_SLOT_COUNT) return;
  char key[8];
  snprintf(key, sizeof(key), "a%u_h", index);
  preferences.putUChar(key, alarmHours[index]);
  snprintf(key, sizeof(key), "a%u_m", index);
  preferences.putUChar(key, alarmMinutes[index]);
  snprintf(key, sizeof(key), "a%u_on", index);
  preferences.putBool(key, alarmEnabled[index]);
}

void scheduleAlarmSave(uint8_t index) {
  if (index >= ALARM_SLOT_COUNT) return;
  alarmSavePendingMask |= static_cast<uint8_t>(1U << index);
  alarmSaveDueAt = millis() + 600;
}

void serviceAlarmSettingsSave(bool force) {
  if (!alarmSavePendingMask) return;
  if (!force && static_cast<int32_t>(millis() - alarmSaveDueAt) < 0) return;
  const uint8_t pendingMask = alarmSavePendingMask;
  alarmSavePendingMask = 0;
  for (uint8_t i = 0; i < ALARM_SLOT_COUNT; ++i) {
    if (pendingMask & static_cast<uint8_t>(1U << i)) persistAlarm(i);
  }
}

void updateAlarmSlotStyles() {
  for (uint8_t i = 0; i < ALARM_SLOT_COUNT; ++i) {
    if (!alarmSlotButtons[i]) continue;
    const bool selected = i == selectedAlarmSlot;
    uint32_t background;
    if (themeIsAmber()) {
      background = selected ? 0x2A1D00 : 0x000000;
    } else {
      background = selected ? 0x7A2945 : (alarmEnabled[i] ? 0x293047 : 0x101014);
    }
    lv_obj_set_style_bg_color(alarmSlotButtons[i], lv_color_hex(background),
                              LV_PART_MAIN);
    lv_obj_set_style_border_color(
      alarmSlotButtons[i],
      lv_color_hex(selected ? themeAccent(UI_ACCENT_ROSE) : themeBorder(0x343846)),
      LV_PART_MAIN);
    if (lv_obj_get_child_cnt(alarmSlotButtons[i]))
      lv_obj_set_style_text_color(lv_obj_get_child(alarmSlotButtons[i], 0),
                                  themeText(), LV_PART_MAIN);
  }
}

void syncAlarmUi() {
  selectedAlarmSlot = min<uint8_t>(selectedAlarmSlot, ALARM_SLOT_COUNT - 1);
  if (alarmHourLabel) {
    lv_label_set_text_fmt(alarmHourLabel, "%02u", alarmHours[selectedAlarmSlot]);
  }
  if (alarmMinuteLabel) {
    lv_label_set_text_fmt(alarmMinuteLabel, "%02u", alarmMinutes[selectedAlarmSlot]);
  }
  if (alarmToggleButton) {
    if (alarmEnabled[selectedAlarmSlot]) {
      lv_obj_add_state(alarmToggleButton, LV_STATE_CHECKED);
    } else {
      lv_obj_clear_state(alarmToggleButton, LV_STATE_CHECKED);
    }
  }
  if (alarmToggleStatusLabel) {
    const bool enabled = alarmEnabled[selectedAlarmSlot];
    lv_label_set_text(alarmToggleStatusLabel,
                      enabled ? tr(STR_ALARM_ON) : tr(STR_ALARM_OFF));
    lv_obj_set_style_text_color(
      alarmToggleStatusLabel,
      lv_color_hex(enabled ? themeAccent(UI_ACCENT_ROSE) : themeSurface(0x73798A)),
      LV_PART_MAIN);
  }
  updateAlarmSlotStyles();
}

void selectAlarmSlot(uint8_t index) {
  if (index >= ALARM_SLOT_COUNT) return;
  selectedAlarmSlot = index;
  syncAlarmUi();
}

void selectAlarmSlotRelative(int8_t direction) {
  if (!direction || !alarmPageVisible) return;
  const int next = (static_cast<int>(selectedAlarmSlot) + ALARM_SLOT_COUNT +
                    (direction > 0 ? 1 : -1)) % ALARM_SLOT_COUNT;
  selectAlarmSlot(static_cast<uint8_t>(next));
}

void alarmSlotEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  selectAlarmSlot(static_cast<uint8_t>(
    reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
}

void alarmAdjustEvent(lv_event_t *event) {
  const lv_event_code_t eventCode = lv_event_get_code(event);
  if (eventCode != LV_EVENT_CLICKED &&
      eventCode != LV_EVENT_LONG_PRESSED_REPEAT) return;
  markUserActivity();
  const uint8_t action = static_cast<uint8_t>(
    reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  const int8_t delta = (action & 1U) ? 1 : -1;
  adjustAlarmTime(delta, action >= 2);
}

void adjustAlarmTime(int8_t delta, bool adjustMinute) {
  if (!delta || !alarmPageVisible) return;
  markUserActivity();
  if (!adjustMinute) {
    alarmHours[selectedAlarmSlot] = static_cast<uint8_t>(
      (alarmHours[selectedAlarmSlot] + 24 + delta) % 24);
  } else {
    alarmMinutes[selectedAlarmSlot] = static_cast<uint8_t>(
      (alarmMinutes[selectedAlarmSlot] + 60 + delta) % 60);
  }
  scheduleAlarmSave(selectedAlarmSlot);
  syncAlarmUi();
}

void alarmToggleEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
  markUserActivity();
  alarmEnabled[selectedAlarmSlot] = lv_obj_has_state(
    lv_event_get_target(event), LV_STATE_CHECKED);
  scheduleAlarmSave(selectedAlarmSlot);
  syncAlarmUi();
}

void showAlarmPage(bool fade) {
  if (lowPowerMode || alarmPageVisible || alarmPageShowRequested) return;
  if (fade) {
    alarmPageHideRequested = false;
    alarmPageShowRequested = true;
    return;
  }
  if (!alarmPage) createAlarmPage(lv_scr_act());
  if (!alarmPage) return;
  syncAlarmUi();
  alarmPageVisible = true;
  lv_obj_clear_flag(alarmPage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(alarmPage);
}

void hideAlarmPage(bool fade, bool returnToMenu) {
  if (!alarmPage) return;
  if (!alarmPageVisible && lv_obj_has_flag(alarmPage, LV_OBJ_FLAG_HIDDEN) &&
      !alarmPageShowRequested && !alarmPageHideRequested) return;
  if (fade) {
    alarmPageShowRequested = false;
    alarmPageHideRequested = true;
    return;
  }
  alarmPageVisible = false;
  alarmPageShowRequested = false;
  alarmPageHideRequested = false;
  lv_obj_add_flag(alarmPage, LV_OBJ_FLAG_HIDDEN);
  if (returnToMenu && !lowPowerMode) showAppMenu(false);
}

void processAlarmPageTransition() {
  if (lowPowerMode || screenFadeTransitionActive) return;
  if (alarmPageShowRequested) {
    alarmPageShowRequested = false;
    // Build before switching. If allocation fails or LVGL needs extra time,
    // the current page remains visible instead of leaving a black screen.
    if (!alarmPage) createAlarmPage(lv_scr_act());
    if (!alarmPage) {
      Serial.println("Alarm: unable to create page");
      return;
    }
    beginScreenFade(activeBrightness);
    hideAppMenu(false);
    showAlarmPage(false);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
  } else if (alarmPageHideRequested) {
    alarmPageHideRequested = false;
    beginScreenFade(activeBrightness);
    hideAlarmPage(false, true);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
  }
}

int8_t matchingAlarmIndex(const RTC_DateTime &current) {
  const uint32_t stamp = alarmMinuteStamp(current);
  for (uint8_t i = 0; i < ALARM_SLOT_COUNT; ++i) {
    if (!alarmEnabled[i] || alarmHours[i] != current.getHour() ||
        alarmMinutes[i] != current.getMinute() ||
        lastAlarmTriggerStamp[i] == stamp) continue;
    lastAlarmTriggerStamp[i] = stamp;
    return static_cast<int8_t>(i);
  }
  return -1;
}

void hideAlarmAlert() {
  if (!alarmAlertOverlay || !alarmAlertVisible) return;
  alarmAlertVisible = false;
  stopAlarmSound();
  lv_obj_add_flag(alarmAlertOverlay, LV_OBJ_FLAG_HIDDEN);
  markUserActivity();
}

void alarmAlertDismissEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) hideAlarmAlert();
}

void alarmAlertGestureEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_GESTURE) return;
  lv_indev_t *input = lv_indev_get_act();
  if (input && logicalGestureDirection(input) == LV_DIR_TOP) hideAlarmAlert();
}

void showAlarmAlert(uint8_t index, const RTC_DateTime &current) {
  if (!alarmAlertOverlay) createAlarmPage(lv_scr_act());
  if (!alarmAlertOverlay || index >= ALARM_SLOT_COUNT) return;
  char timeText[6];
  snprintf(timeText, sizeof(timeText), "%02u:%02u",
           current.getHour(), current.getMinute());
  char slotText[2] = {static_cast<char>('1' + index), '\0'};
  lv_label_set_text(alarmAlertTimeLabel, timeText);
  lv_label_set_text(alarmAlertSlotLabel, slotText);
  alarmAlertVisible = true;
  gfx->setBrightness(activeBrightness);
  lv_obj_clear_flag(alarmAlertOverlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(alarmAlertOverlay);
  lv_obj_invalidate(alarmAlertOverlay);
  startAlarmSound();
  markUserActivity();
}

void serviceAlarms() {
  if (lowPowerMode || alarmAlertVisible || !rtcReady || !rtcTimeValid ||
      millis() - lastAlarmCheck < 1000) return;
  lastAlarmCheck = millis();
  RTC_DateTime current;
  if (!readRtcTime(current)) return;
  const int8_t index = matchingAlarmIndex(current);
  if (index >= 0) showAlarmAlert(static_cast<uint8_t>(index), current);
}

lv_obj_t *createAlarmSlotButton(uint8_t index, int16_t x) {
  lv_obj_t *button = lv_btn_create(alarmPage);
  lv_obj_set_size(button, 60, 60);
  lv_obj_align(button, LV_ALIGN_TOP_MID, x, 20);
  lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(button, 2, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(button, alarmSlotEvent, LV_EVENT_CLICKED,
                      reinterpret_cast<void *>(static_cast<uintptr_t>(index)));
  lv_obj_t *label = lv_label_create(button);
  char number[2] = {static_cast<char>('1' + index), '\0'};
  lv_label_set_text(label, number);
  lv_obj_set_style_text_font(label, &lv_font_ui_22, 0);
  lv_obj_center(label);
  return button;
}

lv_obj_t *createAlarmAdjustButton(uint8_t action, int16_t x, int16_t y,
                                  const char *symbol) {
  lv_obj_t *button = lv_btn_create(alarmPage);
  lv_obj_set_size(button, 76, 76);
  lv_obj_align(button, LV_ALIGN_TOP_MID, x, y);
  lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(themeSurface(0x0A0A0A)), 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, lv_color_hex(themeBorder(0x454B5D)), 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(button, alarmAdjustEvent, LV_EVENT_ALL,
                      reinterpret_cast<void *>(static_cast<uintptr_t>(action)));
  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, symbol);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(label, themeText(), 0);
  lv_obj_align(label, LV_ALIGN_CENTER, 0, strcmp(symbol, "-") == 0 ? -3 : 0);
  return button;
}

void createAlarmPage(lv_obj_t *screen) {
  alarmPage = lv_obj_create(screen);
  lv_obj_remove_style_all(alarmPage);
  lv_obj_set_size(alarmPage, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_pos(alarmPage, 0, 0);
  lv_obj_set_style_bg_color(alarmPage, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(alarmPage, LV_OPA_COVER, 0);
  lv_obj_clear_flag(alarmPage, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(alarmPage, LV_OBJ_FLAG_CLICKABLE);
  // Use the same edge-gesture callback as the clock screen. The shared app
  // callback below remains responsible only for horizontal navigation here.
  lv_obj_add_event_cb(alarmPage, mainUiGestureEvent, LV_EVENT_GESTURE, nullptr);
  lv_obj_add_event_cb(alarmPage, appPageGestureEvent, LV_EVENT_GESTURE, nullptr);

  createTablerIcon(alarmPage, TABLER_ICON_ALARM, 42,
                   lv_color_hex(themeAccent(UI_ACCENT_ROSE)));
  lv_obj_t *headerIcon = lv_obj_get_child(
    alarmPage, lv_obj_get_child_cnt(alarmPage) - 1);
  lv_obj_align(headerIcon, LV_ALIGN_TOP_LEFT, 25, 29);

  alarmSlotButtons[0] = createAlarmSlotButton(0, -76);
  alarmSlotButtons[1] = createAlarmSlotButton(1, 4);
  alarmSlotButtons[2] = createAlarmSlotButton(2, 84);

  // Tap controls replace the continuously redrawn roller. A 90-degree LVGL
  // software rotation can now update only the changed two-digit label.
  createAlarmAdjustButton(1, -106, 105, "+");
  createAlarmAdjustButton(3, 106, 105, "+");
  createAlarmAdjustButton(0, -106, 303, "-");
  createAlarmAdjustButton(2, 106, 303, "-");

  alarmHourLabel = lv_label_create(alarmPage);
  lv_label_set_text(alarmHourLabel, "00");
  lv_obj_set_style_text_font(alarmHourLabel, &lv_font_montserrat_alarm_bold_96, 0);
  lv_obj_set_style_text_color(alarmHourLabel, themeText(), 0);
  lv_obj_align(alarmHourLabel, LV_ALIGN_TOP_MID, -106, 199);

  alarmMinuteLabel = lv_label_create(alarmPage);
  lv_label_set_text(alarmMinuteLabel, "00");
  lv_obj_set_style_text_font(alarmMinuteLabel, &lv_font_montserrat_alarm_bold_96, 0);
  lv_obj_set_style_text_color(alarmMinuteLabel, themeText(), 0);
  lv_obj_align(alarmMinuteLabel, LV_ALIGN_TOP_MID, 106, 199);

  lv_obj_t *colon = lv_label_create(alarmPage);
  lv_label_set_text(colon, ":");
  lv_obj_set_style_text_font(colon, &lv_font_montserrat_alarm_bold_96, 0);
  lv_obj_set_style_text_color(colon, themeText(), 0);
  lv_obj_align(colon, LV_ALIGN_TOP_MID, 0, 199);

  alarmToggleButton = lv_switch_create(alarmPage);
  lv_obj_set_size(alarmToggleButton, 106, 48);
  lv_obj_align(alarmToggleButton, LV_ALIGN_BOTTOM_MID, -28, -22);
  lv_obj_set_style_radius(alarmToggleButton, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(alarmToggleButton, lv_color_hex(themeSurface(0x0A0A0A)),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_color(alarmToggleButton, lv_color_hex(themeAccent(UI_ACCENT_ROSE)),
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_opa(alarmToggleButton, LV_OPA_COVER,
                          LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(alarmToggleButton, themeText(), LV_PART_KNOB);
  lv_obj_set_style_pad_all(alarmToggleButton, 5, LV_PART_KNOB);
  lv_obj_set_style_border_width(alarmToggleButton, 1, 0);
  lv_obj_set_style_border_color(alarmToggleButton, lv_color_hex(themeBorder(0x454B5D)), 0);
  lv_obj_clear_flag(alarmToggleButton, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(alarmToggleButton, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(alarmToggleButton, alarmToggleEvent,
                      LV_EVENT_VALUE_CHANGED, nullptr);

  alarmToggleStatusLabel = lv_label_create(alarmPage);
  lv_label_set_text(alarmToggleStatusLabel, tr(STR_ALARM_OFF));
  lv_obj_set_width(alarmToggleStatusLabel, 90);
  lv_obj_set_style_text_font(alarmToggleStatusLabel, &lv_font_ui_22, 0);
  lv_obj_set_style_text_letter_space(alarmToggleStatusLabel, 2, 0);
  lv_obj_set_style_text_align(alarmToggleStatusLabel, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_align_to(alarmToggleStatusLabel, alarmToggleButton,
                  LV_ALIGN_OUT_RIGHT_MID, 18, 0);

  syncAlarmUi();
  lv_obj_add_flag(alarmPage, LV_OBJ_FLAG_HIDDEN);

  alarmAlertOverlay = lv_obj_create(screen);
  lv_obj_remove_style_all(alarmAlertOverlay);
  lv_obj_set_size(alarmAlertOverlay, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_pos(alarmAlertOverlay, 0, 0);
  lv_obj_set_style_bg_color(alarmAlertOverlay, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(alarmAlertOverlay, LV_OPA_COVER, 0);
  lv_obj_clear_flag(alarmAlertOverlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(alarmAlertOverlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(alarmAlertOverlay, alarmAlertGestureEvent,
                      LV_EVENT_GESTURE, nullptr);
  lv_obj_t *alertIcon = createTablerIcon(
    alarmAlertOverlay, TABLER_ICON_ALARM, 92, lv_color_hex(themeAccent(UI_ACCENT_ROSE)));
  lv_obj_align(alertIcon, LV_ALIGN_TOP_MID, 0, 58);
  alarmAlertTimeLabel = lv_label_create(alarmAlertOverlay);
  lv_obj_set_style_text_font(alarmAlertTimeLabel, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(alarmAlertTimeLabel, themeText(), 0);
  lv_obj_align(alarmAlertTimeLabel, LV_ALIGN_CENTER, 0, 18);
  alarmAlertSlotLabel = lv_label_create(alarmAlertOverlay);
  lv_obj_set_style_text_font(alarmAlertSlotLabel, &lv_font_ui_22, 0);
  lv_obj_set_style_text_color(alarmAlertSlotLabel, themeTextDim(0x8B92A3), 0);
  lv_obj_align(alarmAlertSlotLabel, LV_ALIGN_CENTER, 0, 72);
  lv_obj_t *dismissButton = lv_btn_create(alarmAlertOverlay);
  lv_obj_set_size(dismissButton, 72, 72);
  lv_obj_align(dismissButton, LV_ALIGN_BOTTOM_MID, 0, -44);
  lv_obj_set_style_radius(dismissButton, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dismissButton, lv_color_hex(themeSurface(0x0A0A0A)), 0);
  lv_obj_set_style_border_width(dismissButton, 1, 0);
  lv_obj_set_style_border_color(dismissButton, lv_color_hex(themeBorder(0x454B5D)), 0);
  lv_obj_set_style_shadow_width(dismissButton, 0, 0);
  lv_obj_add_event_cb(dismissButton, alarmAlertDismissEvent, LV_EVENT_CLICKED, nullptr);
  createTablerIcon(dismissButton, TABLER_ICON_X, 34, themeIcon());
  lv_obj_add_flag(alarmAlertOverlay, LV_OBJ_FLAG_HIDDEN);
}

// Re-apply the active theme to the alarm editor and its full-screen alert.
void refreshAlarmTheme() {
  if (!alarmPage) return;
  lv_obj_set_style_bg_color(alarmPage, themePageBackground(), 0);
  const uint32_t childCount = lv_obj_get_child_cnt(alarmPage);
  for (uint32_t i = 0; i < childCount; ++i) {
    lv_obj_t *child = lv_obj_get_child(alarmPage, i);
    if (lv_obj_check_type(child, &lv_label_class)) {
      if (child != alarmToggleStatusLabel)
        lv_obj_set_style_text_color(child, themeText(), 0);
    } else if (lv_obj_check_type(child, &lv_switch_class)) {
      lv_obj_set_style_bg_color(child, lv_color_hex(themeSurface(0x0A0A0A)),
                                LV_PART_MAIN);
      lv_obj_set_style_bg_color(child, lv_color_hex(themeAccent(UI_ACCENT_ROSE)),
                                LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_set_style_bg_color(child, themeText(), LV_PART_KNOB);
      lv_obj_set_style_border_color(child, lv_color_hex(themeBorder(0x454B5D)), 0);
    } else if (lv_obj_check_type(child, &lv_btn_class)) {
      bool isSlot = false;
      for (uint8_t s = 0; s < ALARM_SLOT_COUNT; ++s)
        if (alarmSlotButtons[s] == child) isSlot = true;
      if (!isSlot) {
        lv_obj_set_style_bg_color(child, lv_color_hex(themeSurface(0x0A0A0A)), 0);
        lv_obj_set_style_border_color(child, lv_color_hex(themeBorder(0x454B5D)), 0);
        if (lv_obj_get_child_cnt(child))
          lv_obj_set_style_text_color(lv_obj_get_child(child, 0), themeText(), 0);
      }
    } else {
      // Header alarm icon (vector layer).
      setTablerIconColor(child, lv_color_hex(themeAccent(UI_ACCENT_ROSE)));
    }
  }
  syncAlarmUi();

  if (alarmAlertOverlay) {
    lv_obj_set_style_bg_color(alarmAlertOverlay, themePageBackground(), 0);
    if (alarmAlertTimeLabel)
      lv_obj_set_style_text_color(alarmAlertTimeLabel, themeText(), 0);
    if (alarmAlertSlotLabel)
      lv_obj_set_style_text_color(alarmAlertSlotLabel, themeTextDim(0x8B92A3), 0);
    const uint32_t alertChildren = lv_obj_get_child_cnt(alarmAlertOverlay);
    for (uint32_t i = 0; i < alertChildren; ++i) {
      lv_obj_t *child = lv_obj_get_child(alarmAlertOverlay, i);
      if (lv_obj_check_type(child, &lv_btn_class)) {
        lv_obj_set_style_bg_color(child, lv_color_hex(themeSurface(0x0A0A0A)), 0);
        lv_obj_set_style_border_color(child, lv_color_hex(themeBorder(0x454B5D)), 0);
        if (lv_obj_get_child_cnt(child))
          setTablerIconColor(lv_obj_get_child(child, 0), themeIcon());
      } else if (!lv_obj_check_type(child, &lv_label_class)) {
        setTablerIconColor(child, lv_color_hex(themeAccent(UI_ACCENT_ROSE)));
      }
    }
  }
}
