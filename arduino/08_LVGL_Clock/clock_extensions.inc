// Lazily constructed applications. No recorder buffers, ROMs, sample banks,
// network queues, or decrypted vault records are allocated at boot.
// Included once inside the sketch's private namespace, after the audio owner.

constexpr uint8_t EXT_SUBSONIC = 0, EXT_RECORDER = 1, EXT_GAMEBOY = 2,
                  EXT_WIFI_HUD = 3, EXT_SAMPLER = 4, EXT_VAULT = 5;
lv_obj_t *extensionPages[EXTENSION_APP_COUNT] = {};
bool extensionVisible[EXTENSION_APP_COUNT] = {};
bool extensionShowRequested[EXTENSION_APP_COUNT] = {};
bool extensionHideRequested[EXTENSION_APP_COUNT] = {};
uint32_t extensionAccent = UI_ACCENT_BLUE;

bool extensionAppVisible() {
  for (bool visible : extensionVisible) if (visible) return true;
  return false;
}
uint8_t activeExtensionApp() {
  for (uint8_t i = 0; i < EXTENSION_APP_COUNT; ++i)
    if (extensionVisible[i]) return i;
  return 255;
}
void cancelExtensionLaunches() {
  for (bool &requested : extensionShowRequested) requested = false;
}
void requestExtensionApp(uint8_t app) {
  if (app >= EXTENSION_APP_COUNT || lowPowerMode) return;
  cancelPendingAppLaunches();
  for (uint8_t i = 0; i < EXTENSION_APP_COUNT; ++i)
    extensionShowRequested[i] = i == app;
}
lv_obj_t *extLabel(lv_obj_t *parent, const char *text, int x, int y, int w,
                   int size = 20, bool accent = false) {
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_width(label, w);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, size >= 30 ? &lv_font_ui_30 :
    size >= 24 ? &lv_font_ui_24 : size >= 20 ? &lv_font_ui_20 :
    size >= 18 ? &lv_font_ui_18 : &lv_font_ui_16, 0);
  lv_obj_set_style_text_color(label,
    accent ? lv_color_hex(extensionAccent) : themeText(), 0);
  return label;
}
lv_obj_t *extButton(lv_obj_t *parent, const char *text, int x, int y,
                    int w, int h, lv_event_cb_t callback, uintptr_t data = 0,
                    bool primary = false) {
  const lv_color_t color = lv_color_hex(extensionAccent);
  lv_obj_t *button = lv_btn_create(parent);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, w, h);
  lv_obj_set_style_radius(button, 16, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_bg_color(button, primary ? color : themeControlSurface(extensionAccent), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(button, 0, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_70, LV_STATE_PRESSED);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, reinterpret_cast<void *>(data));
  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, &lv_font_ui_20, 0);
  lv_obj_set_style_text_color(label, primary ? themeTextOnAccent() : color, 0);
  lv_obj_center(label);
  return button;
}
lv_obj_t *extField(lv_obj_t *parent, const char *placeholder, int x, int y,
                    int w, bool password = false) {
  lv_obj_t *field = lv_textarea_create(parent);
  lv_textarea_set_one_line(field, true);
  lv_textarea_set_max_length(field, 255);
  lv_textarea_set_placeholder_text(field, placeholder);
  lv_textarea_set_password_mode(field, password);
  lv_obj_set_pos(field, x, y);
  lv_obj_set_size(field, w, 52);
  styleUtilityNote(field, lv_color_hex(extensionAccent));
  return field;
}
void extensionPageGestureEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_GESTURE || lowPowerMode) return;
  const uint8_t app = activeExtensionApp();
  if (app == EXT_SAMPLER || app == EXT_GAMEBOY) return; // expression lanes / controls
  lv_indev_t *input = lv_indev_get_act();
  if (!input) return;
  const lv_dir_t direction = logicalGestureDirection(input);
  if (!controlPanelVisible && direction == LV_DIR_BOTTOM &&
      gestureStartY >= 0 && gestureStartY <= CONTROL_PANEL_GESTURE_EDGE) {
    markUserActivity(); showControlPanel(true);
  } else if (controlPanelVisible && direction == LV_DIR_TOP &&
             gestureStartY >= SCREEN_HEIGHT - CONTROL_PANEL_GESTURE_EDGE) {
    hideControlPanel(true);
  }
  gestureStartX = gestureStartY = -1;
}
void extBackEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  const uint8_t app = activeExtensionApp();
  if (app == 255) return;
  markUserActivity();
  consumeCurrentTouchRelease();
  extensionHideRequested[app] = true;
}
lv_obj_t *extPage(uint8_t app, const char *title, uint32_t accent) {
  extensionAccent = themeAccent(accent);
  lv_obj_t *page = lv_obj_create(lv_scr_act());
  lv_obj_remove_style_all(page);
  lv_obj_set_size(page, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_pos(page, 0, 0);
  lv_obj_set_style_bg_color(page, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(page, 0, 0);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(page, extensionPageGestureEvent, LV_EVENT_GESTURE, nullptr);
  createAppBackButton(page, extBackEvent, accent);
  lv_obj_t *label = extLabel(page, title, 96, 34, 360, 28);
  lv_obj_set_height(label, 40);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  extensionPages[app] = page;
  return page;
}
bool extSdReady() {
  return weatherWallpaperSdReadyForOtherUsers() && mountMusicSd();
}
void extDeletePage(uint8_t app) {
  if (extensionPages[app]) lv_obj_del(extensionPages[app]);
  extensionPages[app] = nullptr;
  extensionVisible[app] = false;
}

#include "clock_capture.inc"
#include "clock_subsonic.inc"
#include "clock_recorder.inc"
#include "clock_gameboy.inc"
#include "clock_wifi_hud.inc"
#include "clock_sampler.inc"
#include "clock_vault.inc"

void extensionCreate(uint8_t app) {
  switch (app) {
    case EXT_SUBSONIC: createSubsonicPage(); break;
    case EXT_RECORDER: createRecorderPage(); break;
    case EXT_GAMEBOY: createGameboyPage(); break;
    case EXT_WIFI_HUD: createWifiHudPage(); break;
    case EXT_SAMPLER: createSamplerPage(); break;
    case EXT_VAULT: createVaultPage(); break;
  }
}
void extensionCleanup(uint8_t app) {
  switch (app) {
    case EXT_SUBSONIC: cleanupSubsonic(); break;
    case EXT_RECORDER: cleanupRecorder(); break;
    case EXT_GAMEBOY: cleanupGameboy(); break;
    case EXT_WIFI_HUD: cleanupWifiHud(); break;
    case EXT_SAMPLER: cleanupSampler(); break;
    case EXT_VAULT: cleanupVault(); break;
  }
}
void extensionEnter(uint8_t app) {
  if (app == EXT_SUBSONIC) loadSubsonicSettings();
  if (app == EXT_SAMPLER) samplerLoadAll();
}

void stopExtensionAudio() {
  stopSubsonicPlayback();
  stopRecorderActivity();
  stopSamplerPlayback();
}
bool extensionAudioBusy() {
  return subsonicActive || recorderBusy() || samplerBusy() || vaultTypingActive();
}
void closeOtherExtensionApps(uint8_t keep) {
  for (uint8_t i = 0; i < EXTENSION_APP_COUNT; ++i) {
    if (i == keep || !extensionPages[i]) continue;
    extensionCleanup(i);
    extDeletePage(i);
    extensionHideRequested[i] = extensionShowRequested[i] = false;
  }
}
void processExtensionTransitions() {
  if (lowPowerMode || screenFadeTransitionActive || controlPanelVisible) return;
  for (uint8_t i = 0; i < EXTENSION_APP_COUNT; ++i) {
    if (extensionHideRequested[i]) {
      extensionHideRequested[i] = extensionShowRequested[i] = false;
      beginScreenFade(activeBrightness);
      extensionCleanup(i);
      extDeletePage(i);
      lv_refr_now(nullptr);
      endScreenFade(activeBrightness);
      showAppMenu(false);
      return;
    }
    if (!extensionShowRequested[i]) continue;
    extensionShowRequested[i] = false;
    if (utilityPage) {
      if (utilityActive == 1) saveUtilityNote();
      lv_obj_del(utilityPage);
      utilityPage = utilityDisplay = utilityNote = utilityStatus = nullptr;
      utilityActive = 255;
    }
    stopExtensionAudio();
    stopMusicPlayback();
    stopAudiobookPlayback();
    stopRadioPlayback();
    if (videoAudioActive) stopVideoAudioSourceKeepingOutput();
    closeOtherExtensionApps(i);
    beginScreenFade(activeBrightness);
    hideAppMenu(false);
    hideCalendarPage(false, false);
    hideAlarmPage(false, false);
    hideMusicPage(false, false);
    hideAudiobookPage(false, false);
    hideRadioPage(false, false);
    hideWorldClockPage(false, false);
    hidePhotoFramePage(false, false);
    hideTimerPage(false, false);
    hideWeatherInfoPage(false, false);
    hideWeatherWallpaperPage(false, false);
    extensionCreate(i);
    if (!extensionPages[i]) {
      endScreenFade(activeBrightness);
      showAppMenu(false);
      return;
    }
    extensionVisible[i] = true;
    lv_obj_move_foreground(extensionPages[i]);
    extensionEnter(i);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
    return;
  }
}
void serviceExtensionApps() {
  if (lowPowerMode) return;
  serviceSubsonic();
  serviceRecorder();
  serviceGameboy();
  serviceWifiHud();
  serviceSampler();
  serviceVault();
}
void refreshExtensionThemes() {
  const uint32_t accents[] = {0x69D2FF, 0xFF7B9C, 0x9DE879, 0x53E0D0, 0xFFBD66, 0xC5A3FF};
  for (uint8_t i = 0; i < EXTENSION_APP_COUNT; ++i) {
    lv_obj_t *page = extensionPages[i];
    if (!page) continue;
    lv_obj_set_style_bg_color(page, themePageBackground(), 0);
    // Pages are also rebuilt on entry. Recolor existing top-level controls so
    // a web-initiated theme change cannot leave the active app unthemed.
    for (uint32_t c = 0; c < lv_obj_get_child_cnt(page); ++c) {
      lv_obj_t *child = lv_obj_get_child(page, c);
      if (lv_obj_check_type(child, &lv_label_class))
        lv_obj_set_style_text_color(child, themeText(), 0);
      else if (lv_obj_check_type(child, &lv_btn_class)) {
        lv_obj_set_style_bg_color(child, themeControlSurface(accents[i]), 0);
        if (lv_obj_get_child_cnt(child))
          lv_obj_set_style_text_color(lv_obj_get_child(child, 0), themeText(), 0);
      }
    }
  }
}
void suspendExtensionAppsForLowPower() {
  stopExtensionAudio();
  vaultLock();
  for (uint8_t i = 0; i < EXTENSION_APP_COUNT; ++i) {
    extensionCleanup(i);
    extDeletePage(i);
    extensionShowRequested[i] = extensionHideRequested[i] = false;
  }
}
