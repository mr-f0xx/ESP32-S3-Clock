// Small offline tools. Construct only the current tool to conserve LVGL memory.
lv_obj_t *utilityPage = nullptr;
lv_obj_t *utilityDisplay = nullptr;
lv_obj_t *utilityNote = nullptr;
lv_obj_t *utilityStatus = nullptr;
uint8_t utilityActive = 255;
UtilityCalculator utilityCalculator;
String utilitySavedNote;
bool stopwatchRunning = false;
uint32_t stopwatchAccumulated = 0;
uint32_t stopwatchStartedAt = 0;

lv_obj_t *utilityButton(lv_obj_t *parent, const char *text, int x, int y,
                        int w, int h, lv_event_cb_t cb, uintptr_t data = 0) {
  lv_obj_t *button = lv_btn_create(parent);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, w, h);
  lv_obj_set_style_bg_color(button, lv_color_hex(themeSurface(0x242936)), 0);
  lv_obj_set_style_border_color(button, lv_color_hex(themeBorder(0x586070)), 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_radius(button, 12, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(data));
  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, &lv_font_ui_20, 0);
  lv_obj_set_style_text_color(label, themeText(), 0);
  lv_obj_center(label);
  return button;
}

bool saveUtilityNote() {
  if (!utilityNote) return true;
  const String text = lv_textarea_get_text(utilityNote);
  if (text == utilitySavedNote) return true;
  if (preferences.putString("utility_note", text) == 0) return false;
  utilitySavedNote = text;
  return true;
}

void utilityBackEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED || !utilityPage) return;
  markUserActivity();
  if (utilityActive == 1 && !saveUtilityNote()) {
    lv_label_set_text(utilityStatus, tr(STR_NOTE_SAVE_FAILED));
    return;
  }
  consumeCurrentTouchRelease();
  appMenuClickSuppressedUntil = millis() + 350;
  lv_obj_del_async(utilityPage);
  utilityPage = utilityDisplay = utilityNote = utilityStatus = nullptr;
  utilityActive = 255;
}

void utilitySaveEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED || !utilityNote) return;
  markUserActivity();
  lv_label_set_text(utilityStatus, tr(saveUtilityNote() ? STR_NOTE_SAVED : STR_NOTE_SAVE_FAILED));
}

void utilityNoteChanged(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED && utilityStatus)
    lv_label_set_text(utilityStatus, "");
}

void utilityCalculatorEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED || !utilityDisplay) return;
  markUserActivity();
  utilityCalculator.key(static_cast<char>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
  lv_label_set_text(utilityDisplay, utilityCalculator.display);
}

void utilityStopwatchEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  const uintptr_t action = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  const uint32_t now = millis();
  if (action == 0) {
    if (stopwatchRunning) stopwatchAccumulated += now - stopwatchStartedAt;
    else stopwatchStartedAt = now;
    stopwatchRunning = !stopwatchRunning;
    lv_obj_t *label = lv_obj_get_child(static_cast<lv_obj_t *>(lv_event_get_target(event)), 0);
    lv_label_set_text(label, tr(stopwatchRunning ? STR_TIMER_PAUSE : STR_TIMER_START));
  } else {
    stopwatchAccumulated = 0;
    if (stopwatchRunning) stopwatchStartedAt = now;
  }
  serviceUtilityApps();
}

void serviceUtilityApps() {
  if (utilityActive != 2 || !utilityDisplay || !appMenuVisible) return;
  const uint32_t elapsed = stopwatchAccumulated + (stopwatchRunning ? millis() - stopwatchStartedAt : 0);
  const uint32_t seconds = elapsed / 1000;
  char text[24];
  snprintf(text, sizeof(text), "%02lu:%02lu:%02lu.%01lu",
           static_cast<unsigned long>(seconds / 3600),
           static_cast<unsigned long>((seconds / 60) % 60),
           static_cast<unsigned long>(seconds % 60),
           static_cast<unsigned long>((elapsed / 100) % 10));
  if (strcmp(lv_label_get_text(utilityDisplay), text)) lv_label_set_text(utilityDisplay, text);
}

void openUtilityApp(uint8_t app) {
  if (app > 2 || utilityPage || !appMenu) return;
  utilityActive = app;
  utilityPage = lv_obj_create(appMenu);
  lv_obj_remove_style_all(utilityPage);
  lv_obj_set_size(utilityPage, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(utilityPage, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(utilityPage, LV_OPA_COVER, 0);
  lv_obj_clear_flag(utilityPage, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(utilityPage, LV_OBJ_FLAG_CLICKABLE);
  utilityButton(utilityPage, "<", 15, 14, 54, 48, utilityBackEvent);
  lv_obj_t *title = lv_label_create(utilityPage);
  lv_label_set_text(title, tr(app == 0 ? STR_CALCULATOR : app == 1 ? STR_NOTES : STR_STOPWATCH));
  lv_obj_set_style_text_font(title, &lv_font_ui_24, 0);
  lv_obj_set_style_text_color(title, themeText(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

  if (app == 0) {
    utilityDisplay = lv_label_create(utilityPage);
    lv_obj_set_width(utilityDisplay, 420);
    lv_obj_set_style_text_align(utilityDisplay, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(utilityDisplay, &lv_font_ui_32, 0);
    lv_obj_set_style_text_color(utilityDisplay, themeText(), 0);
    lv_label_set_text(utilityDisplay, utilityCalculator.display);
    lv_obj_set_pos(utilityDisplay, 30, 78);
    const char *keys[] = {"7", "8", "9", "/", "4", "5", "6", "*", "1", "2", "3", "-", "C", "0", ".", "+", "="};
    for (uint8_t i = 0; i < 17; ++i) {
      const int row = i / 4, col = i % 4;
      utilityButton(utilityPage, keys[i], 30 + col * 106, 130 + row * 66,
                    i == 16 ? 420 : 96, 56, utilityCalculatorEvent, keys[i][0]);
    }
  } else if (app == 1) {
    utilitySavedNote = preferences.getString("utility_note", "");
    utilityNote = lv_textarea_create(utilityPage);
    lv_obj_set_pos(utilityNote, 20, 72);
    lv_obj_set_size(utilityNote, 440, 108);
    lv_textarea_set_max_length(utilityNote, 512);
    lv_textarea_set_text(utilityNote, utilitySavedNote.c_str());
    lv_obj_add_event_cb(utilityNote, utilityNoteChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    utilityStatus = lv_label_create(utilityPage);
    lv_obj_set_style_text_color(utilityStatus, themeText(), 0);
    lv_obj_set_pos(utilityStatus, 185, 195);
    utilityButton(utilityPage, tr(STR_SAVE_NOTE), 310, 187, 150, 45, utilitySaveEvent);
    lv_obj_t *keyboard = lv_keyboard_create(utilityPage);
    lv_obj_set_size(keyboard, 480, 238);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, utilityNote);
  } else {
    utilityDisplay = lv_label_create(utilityPage);
    lv_obj_set_style_text_font(utilityDisplay, &lv_font_ui_32, 0);
    lv_obj_set_style_text_color(utilityDisplay, themeText(), 0);
    lv_obj_align(utilityDisplay, LV_ALIGN_TOP_MID, 0, 150);
    utilityButton(utilityPage, tr(stopwatchRunning ? STR_TIMER_PAUSE : STR_TIMER_START),
                  55, 260, 170, 65, utilityStopwatchEvent, 0);
    utilityButton(utilityPage, tr(STR_TIMER_RESET), 255, 260, 170, 65, utilityStopwatchEvent, 1);
    serviceUtilityApps();
  }
  lv_obj_move_foreground(utilityPage);
}
