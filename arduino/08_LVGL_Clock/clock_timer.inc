// Countdown timer app. The timer keeps running in standby and wakes the device
// when it reaches zero, reusing the alarm melody for the finished alert.
// Included once by 08_LVGL_Clock.ino inside its private namespace.

lv_obj_t *timerPage = nullptr;
lv_obj_t *timerTimeLabel = nullptr;
lv_obj_t *timerStatusLabel = nullptr;
lv_obj_t *timerStartLabel = nullptr;
lv_obj_t *timerAlertOverlay = nullptr;
lv_obj_t *timerPresetButtons[4] = {};
lv_obj_t *timerPrimaryButton = nullptr;
lv_obj_t *timerProgressBar = nullptr;
lv_obj_t *timerAlertTimeLabel = nullptr;
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
  if (timerProgressBar) {
    const uint32_t configured = timerConfiguredMs();
    lv_bar_set_value(timerProgressBar, configured ?
      static_cast<int32_t>((timerCurrentRemainingMs() * 100ULL) / configured) : 0, LV_ANIM_OFF);
  }
  if (timerAlertTimeLabel) lv_label_set_text(timerAlertTimeLabel, text);
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
  refreshTimerTheme();
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
  refreshTimerTheme();
}

void pauseTimer() {
  if (!timerRunning) return;
  timerRemainingMs = timerCurrentRemainingMs();
  timerRunning = false;
  updateTimerPageText();
  refreshTimerTheme();
}

void resetTimer() {
  timerRunning = false;
  timerRemainingMs = timerConfiguredMs();
  updateTimerPageText();
  refreshTimerTheme();
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

void setTimerRect(lv_obj_t *object, const ClockLayout::Rect &r) {
  lv_obj_set_pos(object, r.x, r.y);
  lv_obj_set_size(object, r.w, r.h);
}

lv_obj_t *createTimerTextButton(lv_obj_t *parent, const ClockLayout::Rect &r,
                                const char *text, lv_event_cb_t callback, uintptr_t data) {
  lv_obj_t *button = lv_btn_create(parent);
  setTimerRect(button, r);
  lv_obj_set_style_radius(button, themeControlRadius(ClockLayout::scale(18, SCREEN_WIDTH)), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, reinterpret_cast<void *>(data));
  lv_obj_t *label = lv_label_create(button);
  lv_obj_set_width(label, r.w - 12);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, &lv_font_ui_20, 0);
  lv_obj_center(label);
  return button;
}

lv_obj_t *createTimerLabel(const ClockLayout::Rect &r, const char *text,
                           const lv_font_t *font) {
  lv_obj_t *label = lv_label_create(timerPage);
  setTimerRect(label, r);
  lv_label_set_text(label, text);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(label, font, 0);
  return label;
}

void createTimerPage(lv_obj_t *screen) {
  timerPage = lv_obj_create(screen);
  lv_obj_remove_style_all(timerPage);
  lv_obj_set_pos(timerPage, 0, 0);
  lv_obj_set_size(timerPage, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_opa(timerPage, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(timerPage, 0, 0);
  lv_obj_clear_flag(timerPage, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(timerPage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(timerPage, appPageGestureEvent, LV_EVENT_GESTURE, nullptr);
  const ClockLayout::Timer l = ClockLayout::timer(SCREEN_WIDTH, SCREEN_HEIGHT);

  createAppBackButton(timerPage, timerBackEvent, UI_ACCENT_AMBER);
  lv_obj_t *title = createTimerLabel(l.title, tr(STR_TIMER), &lv_font_ui_24);
  i18nRegister(title, STR_TIMER);
  // No card behind the digits: mostly-off pixels and one restrained accent line.
  // A fixed native face keeps timer geometry independent of clock-font choice.
  timerTimeLabel = createTimerLabel(l.time, "05:00", &lv_font_montserrat_time_96);
  timerStatusLabel = createTimerLabel(l.status, tr(STR_TIMER), &lv_font_ui_18);
  timerProgressBar = lv_bar_create(timerPage);
  setTimerRect(timerProgressBar, l.progress);
  lv_bar_set_range(timerProgressBar, 0, 100);
  lv_obj_set_style_radius(timerProgressBar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_radius(timerProgressBar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_anim_time(timerProgressBar, 0, 0);

  const uint8_t presets[] = {1, 5, 10, 25};
  for (uint8_t i = 0; i < 4; ++i) {
    char label[8];
    snprintf(label, sizeof(label), "%um", presets[i]);
    timerPresetButtons[i] = createTimerTextButton(timerPage, l.presets[i], label,
                                                  timerPresetEvent, presets[i]);
  }
  createTimerTextButton(timerPage, l.minus, "-1m", timerMinusEvent, 0);
  timerPrimaryButton = createTimerTextButton(timerPage, l.start, tr(STR_TIMER_START), timerStartEvent, 0);
  timerStartLabel = lv_obj_get_child(timerPrimaryButton, 0);
  createTimerTextButton(timerPage, l.plus, "+1m", timerPlusEvent, 0);
  createTimerTextButton(timerPage, l.reset, tr(STR_TIMER_RESET), timerResetEvent, 0);
  lv_obj_t *hint = createTimerLabel(l.hint, tr(STR_TIMER_HINT), &lv_font_ui_14);
  i18nRegister(hint, STR_TIMER_HINT);

  timerAlertOverlay = lv_obj_create(timerPage);
  lv_obj_remove_style_all(timerAlertOverlay);
  lv_obj_set_size(timerAlertOverlay, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_pos(timerAlertOverlay, 0, 0);
  lv_obj_set_style_bg_color(timerAlertOverlay, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(timerAlertOverlay, LV_OPA_COVER, 0);
  lv_obj_add_flag(timerAlertOverlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(timerAlertOverlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *alertTitle = lv_label_create(timerAlertOverlay);
  setTimerRect(alertTitle, l.alertTitle);
  i18nRegister(alertTitle, STR_TIMER_DONE);
  lv_obj_set_style_text_font(alertTitle, &lv_font_ui_32, 0);
  lv_obj_set_style_text_align(alertTitle, LV_TEXT_ALIGN_CENTER, 0);
  timerAlertTimeLabel = lv_label_create(timerAlertOverlay);
  setTimerRect(timerAlertTimeLabel, l.alertTime);
  lv_label_set_text(timerAlertTimeLabel, "00:00");
  lv_obj_set_style_text_font(timerAlertTimeLabel, &lv_font_montserrat_time_72, 0);
  lv_obj_set_style_text_align(timerAlertTimeLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_t *dismiss = createTimerTextButton(timerAlertOverlay, l.dismiss,
                                            tr(STR_TIMER_DISMISS), timerDismissEvent, 0);
  lv_obj_set_style_bg_opa(dismiss, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(dismiss, 0, 0);
  lv_obj_t *dismissLabel = lv_obj_get_child(dismiss, 0);
  i18nRegister(dismissLabel, STR_TIMER_DISMISS);
  lv_obj_set_style_text_font(dismissLabel, &lv_font_ui_22, 0);
  lv_obj_add_flag(timerAlertOverlay, LV_OBJ_FLAG_HIDDEN);

  timerSetMinutes = min<uint8_t>(preferences.getUChar("timer_min", 5), 99);
  timerSetSeconds = min<uint8_t>(preferences.getUChar("timer_sec", 0), 59);
  timerRemainingMs = timerConfiguredMs();
  refreshTimerTheme();
  lv_obj_add_flag(timerPage, LV_OBJ_FLAG_HIDDEN);
}

void refreshTimerTheme() {
  if (!timerPage) return;
  // Icon Glow's timer is true black; other themes retain their own surfaces.
  lv_obj_set_style_bg_color(timerPage, themeIsGlow() ? lv_color_black() : themePageBackground(), 0);
  const lv_color_t accent = lv_color_hex(themeAccent(UI_ACCENT_AMBER));
  const lv_color_t surface = themeControlSurface(UI_ACCENT_AMBER);
  const lv_color_t border = themeControlBorder(UI_ACCENT_AMBER);
  if (timerTimeLabel) lv_obj_set_style_text_color(timerTimeLabel, accent, 0);
  if (timerStatusLabel) lv_obj_set_style_text_color(timerStatusLabel, themeTextDim(0xB8C0CC), 0);
  if (timerProgressBar) {
    lv_obj_set_style_bg_color(timerProgressBar, surface, LV_PART_MAIN);
    lv_obj_set_style_bg_color(timerProgressBar, accent, LV_PART_INDICATOR);
  }
  const uint32_t count = lv_obj_get_child_cnt(timerPage);
  for (uint32_t i = 0; i < count; ++i) {
    lv_obj_t *child = lv_obj_get_child(timerPage, i);
    if (lv_obj_check_type(child, &lv_label_class) && child != timerTimeLabel && child != timerStatusLabel)
      lv_obj_set_style_text_color(child, themeTextDim(0x9AA3B2), 0);
    if (!lv_obj_check_type(child, &lv_btn_class)) continue;
    lv_obj_set_style_radius(child, themeControlRadius(ClockLayout::scale(18, SCREEN_WIDTH)), 0);
    lv_obj_set_style_bg_color(child, surface, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(child, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(child, border, LV_PART_MAIN);
    lv_obj_set_style_bg_color(child, themeTint(UI_ACCENT_AMBER, LV_OPA_40), LV_PART_MAIN | LV_STATE_PRESSED);
    if (lv_obj_get_child_cnt(child))
      lv_obj_set_style_text_color(lv_obj_get_child(child, 0), themeText(), 0);
  }
  const uint8_t presets[] = {1, 5, 10, 25};
  for (uint8_t i = 0; i < 4; ++i) {
    if (!timerPresetButtons[i]) continue;
    const bool selected = !timerRunning && timerSetSeconds == 0 && timerSetMinutes == presets[i];
    lv_obj_set_style_bg_color(timerPresetButtons[i],
      selected ? themeTint(UI_ACCENT_AMBER, LV_OPA_30) : surface, LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_obj_get_child(timerPresetButtons[i], 0),
      selected ? accent : themeText(), 0);
  }
  if (timerPrimaryButton) {
    lv_obj_set_style_bg_color(timerPrimaryButton, timerRunning ? surface : accent, LV_PART_MAIN);
    lv_obj_set_style_text_color(timerStartLabel,
      timerRunning ? themeText() : themeTextOnAccent(), 0);
  }
  if (timerAlertOverlay) {
    const lv_color_t done = lv_color_hex(themeAccent(UI_ACCENT_ROSE));
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(timerAlertOverlay); ++i) {
      lv_obj_t *child = lv_obj_get_child(timerAlertOverlay, i);
      if (lv_obj_check_type(child, &lv_btn_class)) {
        lv_obj_set_style_bg_color(child, done, LV_PART_MAIN);
        lv_obj_set_style_text_color(lv_obj_get_child(child, 0), themeTextOnAccent(), 0);
      } else lv_obj_set_style_text_color(child, done, 0);
    }
  }
  updateTimerPageText();
}
