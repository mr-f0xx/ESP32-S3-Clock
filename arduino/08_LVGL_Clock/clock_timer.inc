// Countdown timer app. The timer keeps running in standby and wakes the device
// when it reaches zero, reusing the alarm melody for the finished alert.
// Included once by 08_LVGL_Clock.ino inside its private namespace.

lv_obj_t *timerPage = nullptr;
lv_obj_t *timerTimeLabel = nullptr;
lv_obj_t *timerStatusLabel = nullptr;
lv_obj_t *timerStartLabel = nullptr;
lv_obj_t *timerAlertOverlay = nullptr;
uint8_t timerSetMinutes = 5;
uint8_t timerSetSeconds = 0;
uint32_t timerLastTickAt = 0;

void updateTimerPageText();
void refreshTimerTheme();

uint32_t timerConfiguredMs() {
  return (static_cast<uint32_t>(timerSetMinutes) * 60U + timerSetSeconds) * 1000U;
}

uint32_t timerCurrentRemainingMs() {
  if (timerRunning) {
    const int32_t remaining = static_cast<int32_t>(timerEndsAt - millis());
    return remaining > 0 ? static_cast<uint32_t>(remaining) : 0;
  }
  return timerRemainingMs;
}

void formatTimerText(char *buffer, size_t length, uint32_t remainingMs) {
  const uint32_t totalSeconds = (remainingMs + 999U) / 1000U;
  snprintf(buffer, length, "%02u:%02u",
           static_cast<unsigned>(totalSeconds / 60U),
           static_cast<unsigned>(totalSeconds % 60U));
}

void updateTimerPageText() {
  if (!timerPage) return;
  char text[16];
  formatTimerText(text, sizeof(text), timerCurrentRemainingMs());
  if (timerTimeLabel) lv_label_set_text(timerTimeLabel, text);
  if (timerStatusLabel)
    lv_label_set_text(timerStatusLabel,
      timerRunning ? tr(STR_TIMER_RUNNING)
                   : (timerCurrentRemainingMs() ? tr(STR_TIMER_PAUSED) : tr(STR_TIMER)));
  if (timerStartLabel)
    lv_label_set_text(timerStartLabel, timerRunning ? tr(STR_TIMER_PAUSE) : tr(STR_TIMER_START));
}

void saveTimerDuration() {
  preferences.putUChar("timer_min", timerSetMinutes);
  preferences.putUChar("timer_sec", timerSetSeconds);
}

void setTimerDuration(uint8_t minutes, uint8_t seconds) {
  timerSetMinutes = min<uint8_t>(minutes, 99);
  timerSetSeconds = min<uint8_t>(seconds, 59);
  timerRemainingMs = timerConfiguredMs();
  timerRunning = false;
  saveTimerDuration();
  updateTimerPageText();
}

void adjustTimerMinutes(int8_t direction) {
  if (!direction || lowPowerMode || timerRunning) return;
  markUserActivity();
  const int next = static_cast<int>(timerSetMinutes) + direction;
  setTimerDuration(static_cast<uint8_t>(constrain(next, 0, 99)), timerSetSeconds);
}

void startTimer() {
  if (timerRunning) return;
  if (!timerRemainingMs) timerRemainingMs = timerConfiguredMs();
  if (!timerRemainingMs) return;
  markUserActivity();
  timerEndsAt = millis() + timerRemainingMs;
  timerRunning = true;
  timerLastTickAt = millis();
  updateTimerPageText();
}

void pauseTimer() {
  if (!timerRunning) return;
  timerRemainingMs = timerCurrentRemainingMs();
  timerRunning = false;
  updateTimerPageText();
}

void resetTimer() {
  timerRunning = false;
  timerRemainingMs = timerConfiguredMs();
  updateTimerPageText();
}

void dismissTimerAlert() {
  timerAlertVisible = false;
  stopAlarmSound();
  if (timerAlertOverlay) lv_obj_add_flag(timerAlertOverlay, LV_OBJ_FLAG_HIDDEN);
  timerRemainingMs = timerConfiguredMs();
  updateTimerPageText();
}

void finishTimer() {
  if (timerAlertVisible) return;
  timerRunning = false;
  timerRemainingMs = 0;
  timerAlertVisible = true;
  markUserActivity();
  if (!timerPageVisible) showTimerPage(false);
  if (timerAlertOverlay) {
    lv_obj_clear_flag(timerAlertOverlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(timerAlertOverlay);
  }
  startAlarmSound();
  updateTimerPageText();
}

void serviceTimer() {
  if (lowPowerMode || !timerRunning) return;
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - timerEndsAt) >= 0) {
    finishTimer();
    return;
  }
  if (now - timerLastTickAt < 250) return;
  timerLastTickAt = now;
  if (timerPageVisible) updateTimerPageText();
}

void suspendTimerForLowPower() {
  // Keep the countdown armed; runLowPowerClock() includes the timer in its
  // wake schedule and calls finishTimer() when the deadline is reached.
  hideTimerPage(false, false);
}

void timerStartEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  if (timerRunning) pauseTimer();
  else startTimer();
}
void timerResetEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) resetTimer();
}
void timerMinusEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) adjustTimerMinutes(-1);
}
void timerPlusEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) adjustTimerMinutes(1);
}
void timerBackEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) hideTimerPage(true, true);
}
void timerDismissEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) dismissTimerAlert();
}
void timerPresetEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  const uint8_t minutes = static_cast<uint8_t>(
    reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  setTimerDuration(minutes, 0);
}

void showTimerPage(bool fade) {
  if (lowPowerMode || timerPageVisible) return;
  if (fade) {
    beginScreenFade(activeBrightness);
    hideAppMenu(false);
    showTimerPage(false);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
    return;
  }
  if (!timerPage) createTimerPage(lv_scr_act());
  if (!timerPage) return;
  timerPageVisible = true;
  lv_obj_clear_flag(timerPage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(timerPage);
  updateTimerPageText();
  if (timerAlertVisible && timerAlertOverlay) {
    lv_obj_clear_flag(timerAlertOverlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(timerAlertOverlay);
  }
}

void hideTimerPage(bool fade, bool returnToMenu) {
  if (!timerPage) return;
  if (!timerPageVisible && lv_obj_has_flag(timerPage, LV_OBJ_FLAG_HIDDEN)) return;
  if (fade) {
    beginScreenFade(activeBrightness);
    hideTimerPage(false, returnToMenu);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
    return;
  }
  timerPageVisible = false;
  lv_obj_add_flag(timerPage, LV_OBJ_FLAG_HIDDEN);
  if (returnToMenu && !lowPowerMode) showAppMenu(false);
}

lv_obj_t *createTimerTextButton(lv_obj_t *parent, int16_t x, int16_t y,
                                int16_t width, const char *text,
                                lv_event_cb_t callback, uintptr_t data) {
  lv_obj_t *button = lv_btn_create(parent);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, width, 56);
  lv_obj_set_style_radius(button, 20, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(themeSurface(0x0A0A0A)), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, lv_color_hex(themeBorder(0x343846)), 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(themeAccent(UI_ACCENT_AMBER)),
                            LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED,
                      reinterpret_cast<void *>(data));
  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, &lv_font_ui_20, 0);
  lv_obj_set_style_text_color(label, themeText(), 0);
  lv_obj_center(label);
  return button;
}

void createTimerPage(lv_obj_t *screen) {
  timerPage = lv_obj_create(screen);
  lv_obj_remove_style_all(timerPage);
  lv_obj_set_pos(timerPage, 0, 0);
  lv_obj_set_size(timerPage, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(timerPage, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(timerPage, LV_OPA_COVER, 0);
  lv_obj_clear_flag(timerPage, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(timerPage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(timerPage, appPageGestureEvent, LV_EVENT_GESTURE, nullptr);

  createRadioControlButton(timerPage, 52, -190, 26, timerBackEvent,
                           TABLER_ICON_CHEVRON_LEFT, 26);
  lv_obj_t *title = lv_label_create(timerPage);
  i18nRegister(title, STR_TIMER);
  lv_obj_set_style_text_font(title, &lv_font_ui_24, 0);
  lv_obj_set_style_text_letter_space(title, 2, 0);
  lv_obj_set_style_text_color(title, themeText(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

  timerTimeLabel = lv_label_create(timerPage);
  lv_label_set_text(timerTimeLabel, "05:00");
  lv_obj_set_style_text_font(timerTimeLabel, currentStandbyClockFont(), 0);
  lv_obj_set_style_text_color(timerTimeLabel, themeText(), 0);
  lv_obj_align(timerTimeLabel, LV_ALIGN_TOP_MID, 0, 110);

  timerStatusLabel = lv_label_create(timerPage);
  lv_obj_set_style_text_font(timerStatusLabel, &lv_font_ui_20, 0);
  lv_obj_set_style_text_color(timerStatusLabel, themeTextDim(0xB8C0CC), 0);
  lv_obj_align(timerStatusLabel, LV_ALIGN_TOP_MID, 0, 238);

  // Preset row.
  const uint8_t presets[] = {1, 5, 10, 25};
  for (uint8_t i = 0; i < 4; ++i) {
    char label[8];
    snprintf(label, sizeof(label), "%um", presets[i]);
    lv_obj_t *button = createTimerTextButton(timerPage, 42 + i * 102, 260, 92,
                                             label, timerPresetEvent, presets[i]);
    (void)button;
  }

  createTimerTextButton(timerPage, 40, 326, 100, "-1m", timerMinusEvent, 0);
  lv_obj_t *startButton = createTimerTextButton(timerPage, 156, 326, 168,
                                                tr(STR_TIMER_START), timerStartEvent, 0);
  timerStartLabel = lv_obj_get_child(startButton, 0);
  createTimerTextButton(timerPage, 340, 326, 100, "+1m", timerPlusEvent, 0);
  createTimerTextButton(timerPage, 156, 385, 168, tr(STR_TIMER_RESET), timerResetEvent, 0);

  lv_obj_t *hint = lv_label_create(timerPage);
  i18nRegister(hint, STR_TIMER_HINT);
  lv_obj_set_width(hint, 460);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(hint, &lv_font_ui_16, 0);
  lv_obj_set_style_text_color(hint, themeTextDim(0x777D8D), 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);

  timerAlertOverlay = lv_obj_create(timerPage);
  lv_obj_remove_style_all(timerAlertOverlay);
  lv_obj_set_size(timerAlertOverlay, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_pos(timerAlertOverlay, 0, 0);
  lv_obj_set_style_bg_color(timerAlertOverlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(timerAlertOverlay, LV_OPA_90, 0);
  lv_obj_add_flag(timerAlertOverlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(timerAlertOverlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *alertTitle = lv_label_create(timerAlertOverlay);
  i18nRegister(alertTitle, STR_TIMER_DONE);
  lv_obj_set_style_text_font(alertTitle, &lv_font_ui_32, 0);
  lv_obj_set_style_text_color(alertTitle, lv_color_hex(themeAccent(UI_ACCENT_ROSE)), 0);
  lv_obj_align(alertTitle, LV_ALIGN_TOP_MID, 0, 150);
  lv_obj_t *dismiss = lv_btn_create(timerAlertOverlay);
  lv_obj_set_size(dismiss, 220, 64);
  lv_obj_align(dismiss, LV_ALIGN_TOP_MID, 0, 240);
  lv_obj_set_style_radius(dismiss, 24, 0);
  lv_obj_set_style_bg_color(dismiss, lv_color_hex(themeAccent(UI_ACCENT_ROSE)), 0);
  lv_obj_set_style_bg_opa(dismiss, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(dismiss, 0, 0);
  lv_obj_add_event_cb(dismiss, timerDismissEvent, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *dismissLabel = lv_label_create(dismiss);
  i18nRegister(dismissLabel, STR_TIMER_DISMISS);
  lv_obj_set_style_text_font(dismissLabel, &lv_font_ui_22, 0);
  lv_obj_set_style_text_color(dismissLabel, lv_color_white(), 0);
  lv_obj_center(dismissLabel);
  lv_obj_add_flag(timerAlertOverlay, LV_OBJ_FLAG_HIDDEN);

  timerSetMinutes = min<uint8_t>(preferences.getUChar("timer_min", 5), 99);
  timerSetSeconds = min<uint8_t>(preferences.getUChar("timer_sec", 0), 59);
  timerRemainingMs = timerConfiguredMs();
  updateTimerPageText();
  lv_obj_add_flag(timerPage, LV_OBJ_FLAG_HIDDEN);
}

void refreshTimerTheme() {
  if (!timerPage) return;
  lv_obj_set_style_bg_color(timerPage, themePageBackground(), 0);
  if (timerTimeLabel) lv_obj_set_style_text_color(timerTimeLabel, themeText(), 0);
  if (timerStatusLabel) lv_obj_set_style_text_color(timerStatusLabel, themeTextDim(0xB8C0CC), 0);
  const uint32_t count = lv_obj_get_child_cnt(timerPage);
  for (uint32_t i = 0; i < count; ++i) {
    lv_obj_t *child = lv_obj_get_child(timerPage, i);
    if (!lv_obj_check_type(child, &lv_btn_class)) continue;
    lv_obj_set_style_bg_color(child, lv_color_hex(themeSurface(0x0A0A0A)), LV_PART_MAIN);
    lv_obj_set_style_border_color(child, lv_color_hex(themeBorder(0x343846)), LV_PART_MAIN);
    lv_obj_set_style_bg_color(child, lv_color_hex(themeAccent(UI_ACCENT_AMBER)),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    if (lv_obj_get_child_cnt(child))
      lv_obj_set_style_text_color(lv_obj_get_child(child, 0), themeText(), 0);
  }
  updateTimerPageText();
}
