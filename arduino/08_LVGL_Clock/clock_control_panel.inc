// EEZ-derived pull-down control panel and hardware controls.
// Included once by 08_LVGL_Clock.ino inside its private namespace.

// Keep these declarations next to their first use. Arduino's sketch
// preprocessor does not reliably preserve hand-written prototypes that refer
// to functions implemented later in separately included .inc files.
bool pauseMusicForControlPanel();
void resumeMusicAfterControlPanel(bool wasPlaying);
void pauseWeatherWallpaperForControlPanel();
void resumeWeatherWallpaperAfterControlPanel();
uint32_t weatherWallpaperDisplayedSequence();
bool weatherWallpaperPlaybackReady();

// All rectangles come from ClockLayout::control(SCREEN_WIDTH, SCREEN_HEIGHT).
// Settings, connectivity and horizontal sliders occupy separate bands; slider
// knob overhang is included in the native layout tests.
bool controlPanelSuspendedWeather = false;
bool controlPanelSuspendedMusic = false;
bool controlPanelMusicWasPlaying = false;
bool controlPanelSuspendedAudiobook = false;
bool controlPanelAudiobookWasPlaying = false;
bool controlPanelWeatherRevealPending = false;
uint32_t controlPanelWeatherRevealBaseline = 0;
uint32_t controlPanelWeatherRevealDeadline = 0;

void suspendControlPanelUnderlyingApp() {
  controlPanelSuspendedWeather = weatherPageVisible;
  controlPanelSuspendedMusic = musicPageVisible;
  controlPanelSuspendedAudiobook = audiobookPageVisible;
  controlPanelMusicWasPlaying = false;
  controlPanelAudiobookWasPlaying = false;
  if (controlPanelSuspendedWeather) pauseWeatherWallpaperForControlPanel();
  if (controlPanelSuspendedMusic)
    controlPanelMusicWasPlaying = pauseMusicForControlPanel();
  if (controlPanelSuspendedAudiobook)
    controlPanelAudiobookWasPlaying = pauseAudiobookForControlPanel();
  if (extensionAppVisible()) {
    // Capture/streaming cannot safely continue behind the overlay; the vault
    // locks immediately rather than remaining unlocked under another surface.
    stopExtensionAudio();
    vaultLock();
  }
}

void resumeControlPanelUnderlyingApp() {
  if (controlPanelSuspendedWeather && weatherPageVisible)
    resumeWeatherWallpaperAfterControlPanel();
  if (controlPanelSuspendedMusic && musicPageVisible)
    resumeMusicAfterControlPanel(controlPanelMusicWasPlaying);
  if (controlPanelSuspendedAudiobook && audiobookPageVisible)
    resumeAudiobookAfterControlPanel(controlPanelAudiobookWasPlaying);
  controlPanelSuspendedWeather = false;
  controlPanelSuspendedMusic = false;
  controlPanelMusicWasPlaying = false;
  controlPanelSuspendedAudiobook = false;
  controlPanelAudiobookWasPlaying = false;
}

void showControlPanel(bool fade = true) {
  if (!controlPanel || lowPowerMode || controlPanelVisible || appMenuShowRequested) return;
  if (fade) {
    if (controlPanelShowRequested) return;
    controlPanelHideRequested = false;
    controlPanelShowRequested = true;
    return;
  }

  // Publish the overlay before waiting for the weather/audio workers. Direct
  // video rendering checks this flag and must stop writing immediately, not
  // after pausePipeline() has finished draining an in-flight frame.
  controlPanelVisible = true;
  suspendControlPanelUnderlyingApp();
  controlPanelShowRequested = false;
  updateControlPanelStatus();
  lv_obj_clear_flag(controlPanel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(controlPanel);
}

void hideControlPanel(bool fade) {
  if (!controlPanel) return;
  if (!controlPanelVisible && lv_obj_has_flag(controlPanel, LV_OBJ_FLAG_HIDDEN) &&
      !controlPanelShowRequested && !controlPanelHideRequested) return;
  if (fade) {
    controlPanelShowRequested = false;
    controlPanelHideRequested = true;
    return;
  }

  controlPanelVisible = false;
  controlPanelHideRequested = false;
  controlPanelShowRequested = false;
  lv_obj_add_flag(controlPanel, LV_OBJ_FLAG_HIDDEN);
}

void processControlPanelTransition() {
  if (!controlPanel || lowPowerMode) return;
  if (controlPanelWeatherRevealPending) {
    const uint32_t now = millis();
    if (weatherWallpaperDisplayedSequence() == controlPanelWeatherRevealBaseline &&
        static_cast<int32_t>(now - controlPanelWeatherRevealDeadline) < 0)
      return;
    controlPanelWeatherRevealPending = false;
    endScreenFade(activeBrightness);
    return;
  }
  if (screenFadeTransitionActive) return;
  if (controlPanelShowRequested) {
    // Keep showRequested asserted through fade-out so the weather display
    // task cannot draw one more frame before controlPanelVisible is published.
    beginScreenFade(activeBrightness);
    showControlPanel(false);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
  } else if (controlPanelHideRequested) {
    controlPanelHideRequested = false;
    beginScreenFade(activeBrightness);
    const bool waitForWeatherFrame =
      controlPanelSuspendedWeather && weatherPageVisible &&
      weatherWallpaperPlaybackReady();
    const uint32_t weatherBaseline = weatherWallpaperDisplayedSequence();
    hideControlPanel(false);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    resumeControlPanelUnderlyingApp();
    if (waitForWeatherFrame) {
      controlPanelWeatherRevealBaseline = weatherBaseline;
      controlPanelWeatherRevealDeadline = millis() + 1500;
      controlPanelWeatherRevealPending = true;
      return;
    }
    endScreenFade(activeBrightness);
  } else if (controlPanelContentRefreshRequested) {
    controlPanelContentRefreshRequested = false;
    // Do not force a synchronous refresh from a control action. In rotated
    // modes this can re-enter the full-screen flush path and leave LVGL's draw
    // buffer locked. The regular lv_timer_handler() safely merges the dirty
    // icon/label regions later in the same loop.
  }
}

uint32_t appMenuClickSuppressedUntil = 0;

void mainUiGestureEvent(lv_event_t *event) {
  // Extension pages handle their own gestures. The overlay must still close.
  if (extensionAppVisible() && !controlPanelVisible) return;
  if (lv_event_get_code(event) != LV_EVENT_GESTURE || lowPowerMode) return;
  lv_indev_t *input = lv_indev_get_act();
  if (!input || timerAlertVisible) return;

  const lv_dir_t direction = logicalGestureDirection(input);
  if (!controlPanelVisible && alarmPageVisible &&
      direction == LV_DIR_BOTTOM && gestureStartY >= 0 &&
      gestureStartY <= CONTROL_PANEL_GESTURE_EDGE) {
    gestureStartX = -1;
    gestureStartY = -1;
    showControlPanel(true);
  } else if (!controlPanelVisible && alarmPageVisible &&
             direction == LV_DIR_TOP &&
             gestureStartY >= SCREEN_HEIGHT - CONTROL_PANEL_GESTURE_EDGE) {
    gestureStartX = -1;
    gestureStartY = -1;
    hideAlarmPage(true, true);
  } else if (!controlPanelVisible && !appMenuVisible && !calendarPageVisible &&
      !alarmPageVisible &&
      !musicPageVisible && !audiobookPageVisible && !radioPageVisible && !weatherPageVisible && !weatherInfoPageVisible && !worldClockPageVisible && !photoFramePageVisible && !timerPageVisible &&
      direction == LV_DIR_LEFT) {
    gestureStartY = -1;
    handleGlobalNavigationStep(1);
  } else if (!controlPanelVisible && !appMenuVisible && !calendarPageVisible &&
             !alarmPageVisible &&
             !musicPageVisible && !audiobookPageVisible && !radioPageVisible && !weatherPageVisible && !weatherInfoPageVisible && !worldClockPageVisible && !photoFramePageVisible && !timerPageVisible &&
             direction == LV_DIR_RIGHT) {
    gestureStartY = -1;
    handleGlobalNavigationStep(-1);
  } else if (!controlPanelVisible && !appMenuVisible && !calendarPageVisible &&
             !alarmPageVisible &&
             !musicPageVisible && !audiobookPageVisible && !radioPageVisible && !weatherPageVisible && !weatherInfoPageVisible && !worldClockPageVisible && !photoFramePageVisible && !timerPageVisible &&
             direction == LV_DIR_BOTTOM &&
      gestureStartY >= 0 && gestureStartY <= CONTROL_PANEL_GESTURE_EDGE) {
    gestureStartY = -1;
    showControlPanel(true);
  } else if (controlPanelVisible && direction == LV_DIR_TOP &&
             gestureStartY >= SCREEN_HEIGHT - CONTROL_PANEL_GESTURE_EDGE) {
    gestureStartY = -1;
    hideControlPanel(true);
  } else if (!controlPanelVisible && !appMenuVisible && !calendarPageVisible &&
             !alarmPageVisible &&
             !musicPageVisible && !audiobookPageVisible && !radioPageVisible && !weatherPageVisible && !weatherInfoPageVisible && !worldClockPageVisible && !photoFramePageVisible && !timerPageVisible &&
             direction == LV_DIR_TOP &&
             gestureStartY >= SCREEN_HEIGHT - CONTROL_PANEL_GESTURE_EDGE) {
    gestureStartY = -1;
    showAppMenu(true);
  } else if (!controlPanelVisible && !appMenuVisible && !calendarPageVisible &&
             !alarmPageVisible && !musicPageVisible && !audiobookPageVisible && !radioPageVisible && !weatherPageVisible &&
             !weatherInfoPageVisible && !worldClockPageVisible && !photoFramePageVisible && !timerPageVisible &&
             direction == LV_DIR_BOTTOM) {
    gestureStartX = -1;
    gestureStartY = -1;
    handleGlobalNavigationStep(-1);
  } else if (!controlPanelVisible && !appMenuVisible && !calendarPageVisible &&
             !alarmPageVisible && !musicPageVisible && !audiobookPageVisible && !radioPageVisible && !weatherPageVisible &&
             !weatherInfoPageVisible && !worldClockPageVisible && !photoFramePageVisible && !timerPageVisible &&
             direction == LV_DIR_TOP) {
    gestureStartX = -1;
    gestureStartY = -1;
    handleGlobalNavigationStep(1);
  } else if (appMenuVisible && direction == LV_DIR_LEFT) {
    appMenuClickSuppressedUntil = millis() + 350;
    gestureStartX = -1;
    gestureStartY = -1;
    appMenuPageStepRequested = 1;
  } else if (appMenuVisible && direction == LV_DIR_RIGHT) {
    appMenuClickSuppressedUntil = millis() + 350;
    gestureStartX = -1;
    gestureStartY = -1;
    appMenuPageStepRequested = -1;
  } else if (appMenuVisible && direction == LV_DIR_BOTTOM &&
             gestureStartY >= 0 && gestureStartY <= CONTROL_PANEL_GESTURE_EDGE) {
    gestureStartY = -1;
    appMenuClickSuppressedUntil = millis() + 350;
    showControlPanel(true);
  }
}

uint8_t brightnessPercentToHardware(int32_t percent) {
  percent = constrain(percent, 0, 100);
  return static_cast<uint8_t>(
    MIN_ACTIVE_BRIGHTNESS +
    (percent * (255 - MIN_ACTIVE_BRIGHTNESS) + 50) / 100);
}

int32_t brightnessHardwareToPercent(uint8_t level) {
  level = max(level, MIN_ACTIVE_BRIGHTNESS);
  return constrain(
    ((static_cast<int32_t>(level) - MIN_ACTIVE_BRIGHTNESS) * 100 +
     (255 - MIN_ACTIVE_BRIGHTNESS) / 2) /
    (255 - MIN_ACTIVE_BRIGHTNESS), 0, 100);
}

int estimateLipoPercent(uint16_t millivolts) {
  if (millivolts >= 4200) return 100;
  if (millivolts >= 4000) return map(millivolts, 4000, 4200, 75, 100);
  if (millivolts >= 3800) return map(millivolts, 3800, 4000, 40, 75);
  if (millivolts >= 3700) return map(millivolts, 3700, 3800, 20, 40);
  if (millivolts >= 3500) return map(millivolts, 3500, 3700, 5, 20);
  if (millivolts >= 3200) return map(millivolts, 3200, 3500, 0, 5);
  return 0;
}

lv_obj_t *createOfficialTablerIcon(lv_obj_t *parent,
                                   const lv_img_dsc_t *image,
                                   lv_color_t color = lv_color_white()) {
  lv_obj_t *icon = lv_img_create(parent);
  lv_img_set_src(icon, image);
  lv_obj_set_style_img_recolor(icon, color, 0);
  lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
  lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_center(icon);
  return icon;
}

const lv_img_dsc_t *batteryTablerImage(bool connected, bool charging,
                                       int percent) {
  if (!connected) return &tabler_battery_off;
  if (charging) return &tabler_battery_charging;
  if (percent >= 88) return &tabler_battery_4;
  if (percent >= 63) return &tabler_battery_3;
  if (percent >= 38) return &tabler_battery_2;
  if (percent >= 13) return &tabler_battery_1;
  return &tabler_battery;
}

bool updateBatteryPanelStatus() {
  if (!controlPanelVisible || !batteryStatusIcon || !batteryStatusLabel) return false;

  const bool connected = pmuReady && power.isBatteryConnect();
  const bool charging = connected && power.isCharging();
  int percent = -1;
  uint16_t millivolts = 0;
  if (connected) {
    millivolts = power.getBattVoltage();
    percent = power.getBatteryPercent();
    if (percent < 0 || percent > 100 || (percent == 0 && millivolts > 3500)) {
      percent = estimateLipoPercent(millivolts);
    }
    percent = constrain(percent, 0, 100);
  }

  String percentText;
  uint32_t colorValue;
  if (!connected) {
    percentText = tr(STR_NO_BAT);
    colorValue = themeIsAmber() ? CRT_AMBER_DIM : 0x9AA3B2;
  } else {
    percentText = String(percent) + "%";
    if (themeIsAmber()) {
      colorValue = percent <= 10 ? CRT_AMBER_DIM : CRT_AMBER;
    } else {
      colorValue = charging ? themeAccent(UI_ACCENT_MINT)
                            : (percent <= 10 ? themeAccent(UI_ACCENT_ROSE) : 0xFFFFFF);
    }
  }

  static String displayedPercentText;
  static uint32_t displayedColorValue = 0xFFFFFFFF;
  static const lv_img_dsc_t *displayedImage = nullptr;
  static uint8_t displayedBatteryTheme = 0xFF;
  static uint8_t displayedBatteryLanguage = 0xFF;
  if (displayedBatteryTheme != uiTheme || displayedBatteryLanguage != uiLanguage) {
    displayedBatteryTheme = uiTheme;
    displayedBatteryLanguage = uiLanguage;
    displayedColorValue = 0xFFFFFFFF;
    displayedPercentText = "";
  }
  bool changed = false;
  const lv_img_dsc_t *image = batteryTablerImage(connected, charging, percent);
  if (displayedImage != image) {
    displayedImage = image;
    lv_img_set_src(batteryStatusIcon, displayedImage);
    changed = true;
  }
  if (displayedPercentText != percentText) {
    displayedPercentText = percentText;
    lv_label_set_text(batteryStatusLabel, displayedPercentText.c_str());
    changed = true;
  }
  if (displayedColorValue != colorValue) {
    displayedColorValue = colorValue;
    const lv_color_t color = lv_color_hex(colorValue);
    lv_obj_set_style_img_recolor(batteryStatusIcon, color, 0);
    lv_obj_set_style_img_recolor_opa(batteryStatusIcon, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(batteryStatusLabel, color, 0);
    changed = true;
  }
  return changed;
}

bool applyAudioVolumePercent(uint8_t percent) {
  percent = min<uint8_t>(percent, 100);
  uint8_t registerValue = 0;  // -95.5 dB, effectively muted.
  if (percent > 0) {
    // Map 1..100% linearly in dB from -50 dB to the board's clean maximum.
    // The codec can reach 0 dB, but full-scale programme peaks overload this
    // board's small speaker/PA above roughly the old 90% position.
    constexpr uint16_t registerSpan =
      ES8311_DAC_SAFE_MAX_REGISTER - ES8311_DAC_MIN_AUDIBLE_REGISTER;
    const uint16_t roundedStep =
      (static_cast<uint16_t>(percent - 1U) * registerSpan + 49U) / 99U;
    registerValue = static_cast<uint8_t>(
      ES8311_DAC_MIN_AUDIBLE_REGISTER + roundedStep);
  }
  Wire.beginTransmission(ES8311_I2C_ADDRESS);
  Wire.write(ES8311_DAC_VOLUME_REGISTER);
  Wire.write(registerValue);
  audioCodecReachable = Wire.endTransmission() == 0;
  return audioCodecReachable;
}

bool applyAudioVolume() {
  return applyAudioVolumePercent(audioVolume);
}

void setWifiUserEnabled(bool enabled, bool persist) {
  if (wifiUserEnabled == enabled &&
      (enabled || WiFi.getMode() == WIFI_MODE_NULL)) return;

  wifiUserEnabled = enabled;
  if (persist) preferences.putBool("wifi_on", wifiUserEnabled);

  if (!enabled) {
    wifiControlConnectPending = false;
    if (esp_sntp_enabled()) esp_sntp_stop();
    if (!rtcCalibratedThisBoot) ntpStarted = false;
    dnsServer.stop();
    if (webServerListening) server.stop();
    webServerListening = false;
    portalActive = false;
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    Serial.println("Wi-Fi disabled from control panel");
  } else {
    temporaryNtpWifi = false;
    temporaryNtpWifiStartedAt = 0;
    Serial.println("Wi-Fi enabled from control panel");
    if (savedSsid.isEmpty()) {
      startConfigPortal();
    } else {
      if (WiFi.status() != WL_CONNECTED) {
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
      }
      wifiControlConnectPending = true;
      wifiControlConnectStartedAt = millis();
    }
  }
  updateNetworkLabels();
  updateControlPanelStatus();
}

void wifiControlEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  wifiControlTargetEnabled = !wifiUserEnabled;
  wifiControlToggleRequested = true;
  controlPanelActionDueAt = millis() + 50;
}

void bluetoothControlEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  orientationLocked = !orientationLocked;
  preferences.putBool("orient_lock", orientationLocked);
  if (!orientationLocked) lastOrientationCheck = 0;
  Serial.printf("Screen direction: %s\n",
                orientationLocked ? "locked" : "automatic");
  updateControlPanelStatus();
}

void processControlPanelActions() {
  const uint32_t now = millis();
  if ((wifiControlToggleRequested ||
       brightnessSaveRequested || volumeSaveRequested ||
       brightnessApplyRequested || volumeApplyRequested) &&
      static_cast<int32_t>(now - controlPanelActionDueAt) >= 0) {
    if (wifiControlToggleRequested) {
      wifiControlToggleRequested = false;
      setWifiUserEnabled(wifiControlTargetEnabled, true);
    }
    if (brightnessSaveRequested) {
      brightnessSaveRequested = false;
      preferences.putUChar("brightness", activeBrightness);
    }
    if (volumeSaveRequested) {
      volumeSaveRequested = false;
      preferences.putUChar("volume", audioVolume);
    }
    if (brightnessApplyRequested) {
      brightnessApplyRequested = false;
      if (!lowPowerMode) gfx->setBrightness(activeBrightness);
    }
    if (volumeApplyRequested) {
      volumeApplyRequested = false;
      applyAudioVolume();
    }
  }

  if (!wifiControlConnectPending) return;
  if (!wifiUserEnabled) {
    wifiControlConnectPending = false;
    return;
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiControlConnectPending = false;
    Serial.printf("Wi-Fi connected from control panel: %s\n",
                  WiFi.localIP().toString().c_str());
    startNtp();
    startWebServer();
    updateNetworkLabels();
    updateControlPanelStatus();
  } else if (now - wifiControlConnectStartedAt >= WIFI_CONNECT_TIMEOUT_MS) {
    wifiControlConnectPending = false;
    Serial.println("Control-panel Wi-Fi connection timed out");
    startConfigPortal();
  }
}

void brightnessControlEvent(lv_event_t *event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_VALUE_CHANGED) {
    activeBrightness = brightnessPercentToHardware(
      lv_slider_get_value(static_cast<lv_obj_t *>(lv_event_get_target(event))));
    // CO5300 brightness is a short display command, not an I2C transaction;
    // apply it immediately so the slider remains visually and physically live.
    if (!lowPowerMode) gfx->setBrightness(activeBrightness);
    if (controlBrightnessValue) lv_label_set_text_fmt(controlBrightnessValue, "%u%%", brightnessHardwareToPercent(activeBrightness));
  } else if (code == LV_EVENT_RELEASED) {
    brightnessSaveRequested = true;
    controlPanelActionDueAt = millis() + 20;
  }
}

void volumeControlEvent(lv_event_t *event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_VALUE_CHANGED) {
    audioVolume = static_cast<uint8_t>(constrain(
      lv_slider_get_value(static_cast<lv_obj_t *>(lv_event_get_target(event))), 0, 100));
    volumeApplyRequested = true;
    // ES8311 and touch share the I2C bus. Defer codec writes until the slider
    // pauses instead of writing on every movement sample.
    controlPanelActionDueAt = millis() + 80;
    if (controlVolumeValue) lv_label_set_text_fmt(controlVolumeValue, "%u%%", audioVolume);
  } else if (code == LV_EVENT_RELEASED) {
    volumeApplyRequested = true;
    volumeSaveRequested = true;
    controlPanelActionDueAt = millis() + 20;
  }
}

void setControlRect(lv_obj_t *object, const ClockLayout::Rect &r) {
  // LVGL keeps an alignment style after lv_obj_align()/lv_obj_center(). Reset
  // it before writing absolute coordinates, otherwise a previously centered
  // icon treats these values as center offsets and lands in the wrong place.
  lv_obj_set_align(object, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(object, r.x, r.y);
  lv_obj_set_size(object, r.w, r.h);
}

void placeControlIcon(lv_obj_t *icon, const ClockLayout::Rect &r) {
  lv_obj_update_layout(icon);
  lv_obj_set_align(icon, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(icon, r.x + (r.w - lv_obj_get_width(icon)) / 2,
               r.y + (r.h - lv_obj_get_height(icon)) / 2);
}

lv_obj_t *createControlButton(const ClockLayout::Rect &r,
                               const lv_img_dsc_t *iconImage,
                               lv_event_cb_t callback, lv_obj_t **icon) {
  lv_obj_t *button = lv_btn_create(controlPanel);
  setControlRect(button, r);
  lv_obj_set_style_radius(button, themeControlRadius(ClockLayout::scale(22, SCREEN_WIDTH)), 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_pad_all(button, 0, 0);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
  // Native Alpha-8 artwork; never use the image transform path.
  *icon = createOfficialTablerIcon(button, iconImage);
  lv_obj_set_align(*icon, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(*icon, (r.w - iconImage->header.w) / 2,
               (r.h - iconImage->header.h) / 2);
  return button;
}

// Two-row settings grid cycles appearance without leaving the control centre.
void updateControlPillLabels();

void languageControlEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  setUiLanguage(uiLanguage == UI_LANG_FR ? UI_LANG_EN : UI_LANG_FR, true);
}

void colorControlEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  setClockColor(static_cast<uint8_t>((clockColorIndex + 1) % CLOCK_COLOR_COUNT), true);
  updateControlPillLabels();
}

void themeControlEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  setUiTheme(static_cast<uint8_t>((uiTheme + 1) % UI_THEME_COUNT), true);
}

void fontControlEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  const uint8_t base = clockFontChangeRequested ? pendingClockFontIndex : clockFontIndex;
  pendingClockFontIndex = static_cast<uint8_t>((base + 1) % CLOCK_FONT_COUNT);
  clockFontChangeRequested = true;
  updateControlPillLabels();
}

void sizeControlEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  const uint8_t base = clockSizeChangeRequested ? pendingClockSizeIndex : clockSizeIndex;
  pendingClockSizeIndex = static_cast<uint8_t>((base + 1) % ClockLayout::CLOCK_SIZE_COUNT);
  clockSizeChangeRequested = true;
  updateControlPillLabels();
}

void closeControlEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) hideControlPanel(true);
}

lv_obj_t *createControlPillButton(const ClockLayout::Rect &r, lv_event_cb_t callback,
                                  lv_obj_t **labelOut) {
  lv_obj_t *button = lv_btn_create(controlPanel);
  setControlRect(button, r);
  lv_obj_set_style_radius(button, themeControlRadius(ClockLayout::scale(16, SCREEN_WIDTH)), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_pad_all(button, 2, 0);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *label = lv_label_create(button);
  lv_obj_set_width(label, r.w - 8);
  // A fixed r.h-8 label box paints its glyphs at the top of that box, which is
  // what made the five pills look high in their tiles. Use one font line and
  // center the line box vertically in the button.
  lv_obj_set_height(label, lv_font_get_line_height(&lv_font_ui_16));
  lv_obj_set_style_text_font(label, &lv_font_ui_16, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_center(label);
  *labelOut = label;
  return button;
}

void updateControlPillLabels() {
  char text[48];
  const char *prefix = themeLabelPrefix();
  if (languageControlLabel) {
    snprintf(text, sizeof(text), "%s%s", prefix, uiLanguage == UI_LANG_FR ? "Langue FR" : "Lang EN");
    lv_label_set_text(languageControlLabel, text);
  }
  if (themeControlLabel) {
    snprintf(text, sizeof(text), "%s%s %s", prefix, tr(STR_THEME), themeShortName());
    lv_label_set_text(themeControlLabel, text);
  }
  if (colorControlLabel) {
    static const char *const colorNames[CLOCK_COLOR_COUNT] = {"THM", "WHT", "AMB", "CYN", "PNK", "MNT", "GLD", "ICE", "RED"};
    snprintf(text, sizeof(text), "%s%s %s", prefix, tr(STR_CLOCK_COLOR),
             colorNames[clockColorIndex < CLOCK_COLOR_COUNT ? clockColorIndex : 0]);
    lv_label_set_text(colorControlLabel, text);
  }
  if (fontControlLabel) {
    snprintf(text, sizeof(text), "%s%s %s", prefix, tr(STR_CLOCK_FONT),
             clockFontShortName(clockFontChangeRequested ? pendingClockFontIndex : clockFontIndex));
    lv_label_set_text(fontControlLabel, text);
  }
  if (sizeControlLabel) {
    const uint8_t size = clockSizeChangeRequested ? pendingClockSizeIndex : clockSizeIndex;
    snprintf(text, sizeof(text), "%s%s %c", prefix, tr(STR_CLOCK_SIZE), clockSizeShortName(size)[0]);
    lv_label_set_text(sizeControlLabel, text);
  }
}

lv_obj_t *createControlSlider(const ClockLayout::Rect &r, int32_t value,
                              lv_event_cb_t callback) {
  lv_obj_t *slider = lv_slider_create(controlPanel);
  setControlRect(slider, r);
  lv_slider_set_range(slider, 0, 100);
  lv_slider_set_value(slider, value, LV_ANIM_OFF);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(slider, ClockLayout::scale(8, SCREEN_WIDTH), LV_PART_KNOB);
  lv_obj_set_ext_click_area(slider, ClockLayout::scale(12, SCREEN_WIDTH));
  lv_obj_clear_flag(slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(slider, callback, LV_EVENT_ALL, nullptr);
  return slider;
}

lv_obj_t *createControlLabel(const ClockLayout::Rect &r, const char *text,
                             const lv_font_t *font, bool centered = false,
                             lv_label_long_mode_t longMode = LV_LABEL_LONG_DOT) {
  lv_obj_t *label = lv_label_create(controlPanel);
  // Center the font's line box inside the layout row. This keeps the battery
  // percentage on the icon centerline and AUTO/LOCKED beside the lock icon,
  // instead of painting the first line at the top of a taller fixed label.
  const ClockLayout::Rect line = ClockLayout::centeredLineRect(
    r, static_cast<int16_t>(lv_font_get_line_height(font)));
  lv_obj_set_align(label, LV_ALIGN_TOP_LEFT);
  lv_obj_set_pos(label, line.x, line.y);
  lv_obj_set_size(label, line.w, line.h);
  lv_label_set_text(label, text);
  // DOT keeps both the width and height fixed; WRAP can grow past a short row.
  lv_label_set_long_mode(label, longMode);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_align(label, centered ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(label, 0, 0);
  lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  return label;
}

void createControlPanel() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_add_event_cb(screen, mainUiGestureEvent, LV_EVENT_GESTURE, nullptr);
  controlPanel = lv_obj_create(screen);
  lv_obj_remove_style_all(controlPanel);
  lv_obj_set_pos(controlPanel, 0, 0);
  lv_obj_set_size(controlPanel, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(controlPanel, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(controlPanel, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(controlPanel, 0, 0);
  lv_obj_clear_flag(controlPanel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(controlPanel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(controlPanel, mainUiGestureEvent, LV_EVENT_GESTURE, nullptr);
  const ClockLayout::Control l = ClockLayout::control(SCREEN_WIDTH, SCREEN_HEIGHT);

  batteryStatusIcon = createOfficialTablerIcon(controlPanel, &tabler_battery, themeIcon());
  placeControlIcon(batteryStatusIcon, l.batteryIcon);
  batteryStatusLabel = createControlLabel(l.battery, "--%", &lv_font_ui_22);
  controlCloseButton = lv_btn_create(controlPanel);
  setControlRect(controlCloseButton, l.close);
  lv_obj_set_style_radius(controlCloseButton, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_shadow_width(controlCloseButton, 0, 0);
  lv_obj_add_event_cb(controlCloseButton, closeControlEvent, LV_EVENT_CLICKED, nullptr);
  createOfficialTablerIcon(controlCloseButton, &tabler_x, themeIcon());

  themeControlButton = createControlPillButton(l.settings[0], themeControlEvent, &themeControlLabel);
  fontControlButton = createControlPillButton(l.settings[1], fontControlEvent, &fontControlLabel);
  sizeControlButton = createControlPillButton(l.settings[2], sizeControlEvent, &sizeControlLabel);
  colorControlButton = createControlPillButton(l.settings[3], colorControlEvent, &colorControlLabel);
  languageControlButton = createControlPillButton(l.settings[4], languageControlEvent, &languageControlLabel);

  wifiControlButton = createControlButton(l.wifi, &tabler_wifi, wifiControlEvent, &wifiControlIcon);
  // These status labels sit beside, not below, the native 40 px icon.
  lv_obj_set_pos(wifiControlIcon, l.wifiIcon.x - l.wifi.x, l.wifiIcon.y - l.wifi.y);
  wifiControlStatus = createControlLabel(l.wifiStatus, "--", &lv_font_ui_18,
                                         false, LV_LABEL_LONG_DOT);
  wifiControlIp = createControlLabel(l.wifiIp, "--", &lv_font_ui_16,
                                     false, LV_LABEL_LONG_DOT);
  bluetoothControlButton = createControlButton(l.rotation, &tabler_lock,
                                                bluetoothControlEvent, &bluetoothControlIcon);
  lv_obj_set_pos(bluetoothControlIcon, l.rotationIcon.x - l.rotation.x,
                 l.rotationIcon.y - l.rotation.y);
  bluetoothControlStatus = createControlLabel(l.rotationStatus, "--", &lv_font_ui_18,
                                              false, LV_LABEL_LONG_DOT);

  brightnessSlider = createControlSlider(l.brightnessTrack, 25, brightnessControlEvent);
  volumeSlider = createControlSlider(l.volumeTrack, DEFAULT_AUDIO_VOLUME, volumeControlEvent);
  controlBrightnessIcon = createOfficialTablerIcon(controlPanel, &tabler_sun, themeIcon());
  controlVolumeIcon = createOfficialTablerIcon(controlPanel, &tabler_volume, themeIcon());
  placeControlIcon(controlBrightnessIcon, l.brightnessIcon);
  placeControlIcon(controlVolumeIcon, l.volumeIcon);
  controlBrightnessTitle = createControlLabel(l.brightnessTitle, tr(STR_BRIGHTNESS), &lv_font_ui_16);
  controlVolumeTitle = createControlLabel(l.volumeTitle, tr(STR_VOLUME), &lv_font_ui_16);
  controlBrightnessValue = createControlLabel(l.brightnessValue, "25%", &lv_font_ui_20, true);
  controlVolumeValue = createControlLabel(l.volumeValue, "25%", &lv_font_ui_20, true);
  i18nRegister(controlBrightnessTitle, STR_BRIGHTNESS);
  i18nRegister(controlVolumeTitle, STR_VOLUME);
  controlHintLabel = createControlLabel(l.hint, tr(STR_CONTROL_HINT), &lv_font_ui_14, true);
  i18nRegister(controlHintLabel, STR_CONTROL_HINT);
  refreshControlPanelTheme();
  lv_obj_add_flag(controlPanel, LV_OBJ_FLAG_HIDDEN);
}

void refreshControlPanelTheme() {
  if (!controlPanel) return;
  lv_obj_set_style_bg_color(controlPanel, themePageBackground(), 0);
  const lv_color_t accent = lv_color_hex(themeAccent(UI_ACCENT_BLUE));
  lv_obj_t *sliders[] = {brightnessSlider, volumeSlider};
  for (lv_obj_t *slider : sliders) {
    if (!slider) continue;
    lv_obj_set_style_bg_color(slider, themeControlSurface(UI_ACCENT_BLUE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, accent, LV_PART_KNOB);
  }
  lv_obj_t *icons[] = {batteryStatusIcon, controlBrightnessIcon, controlVolumeIcon};
  for (lv_obj_t *icon : icons)
    if (icon) lv_obj_set_style_img_recolor(icon, themeIcon(), 0);
  if (batteryStatusLabel) lv_obj_set_style_text_color(batteryStatusLabel, themeText(), 0);
  lv_obj_t *dimLabels[] = {controlBrightnessTitle, controlVolumeTitle, controlHintLabel};
  for (lv_obj_t *label : dimLabels)
    if (label) lv_obj_set_style_text_color(label, themeTextDim(0x9AA3B2), 0);
  lv_obj_t *valueLabels[] = {controlBrightnessValue, controlVolumeValue};
  for (lv_obj_t *label : valueLabels)
    if (label) lv_obj_set_style_text_color(label, accent, 0);
  lv_obj_t *buttons[] = {languageControlButton, colorControlButton, fontControlButton,
                         themeControlButton, sizeControlButton, controlCloseButton};
  for (lv_obj_t *button : buttons) {
    if (!button) continue;
    lv_obj_set_style_radius(button,
      button == controlCloseButton ? LV_RADIUS_CIRCLE
                                   : themeControlRadius(ClockLayout::scale(16, SCREEN_WIDTH)), 0);
    lv_obj_set_style_bg_color(button, themeControlSurface(UI_ACCENT_BLUE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, themeControlBorder(UI_ACCENT_BLUE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, themeTint(UI_ACCENT_BLUE, LV_OPA_40), LV_PART_MAIN | LV_STATE_PRESSED);
    if (lv_obj_get_child_cnt(button)) {
      lv_obj_t *child = lv_obj_get_child(button, 0);
      lv_obj_set_style_text_color(child, themeText(), 0);
      lv_obj_set_style_img_recolor(child, themeIcon(), 0);
    }
  }
  updateControlPillLabels();
  updateControlPanelStatus();
}

void syncControlPanelSettings() {
  if (brightnessSlider) {
    lv_slider_set_value(brightnessSlider,
                        brightnessHardwareToPercent(activeBrightness), LV_ANIM_OFF);
  }
  if (volumeSlider) lv_slider_set_value(volumeSlider, audioVolume, LV_ANIM_OFF);
  if (controlBrightnessValue) lv_label_set_text_fmt(controlBrightnessValue, "%u%%", brightnessHardwareToPercent(activeBrightness));
  if (controlVolumeValue) lv_label_set_text_fmt(controlVolumeValue, "%u%%", audioVolume);
  if (!lowPowerMode && !screenFadeTransitionActive) gfx->setBrightness(activeBrightness);
  applyAudioVolume();
  updateControlPanelStatus();
}

void updateControlPanelStatus() {
  if (!wifiControlStatus || !wifiControlIp || !bluetoothControlStatus) return;
  bool changed = false;

  String wifiText;
  String wifiIpText = "--";
  if (!wifiUserEnabled) {
    wifiText = tr(STR_WIFI_OFF);
  } else if (WiFi.status() == WL_CONNECTED) {
    wifiText = WiFi.SSID();
    wifiIpText = WiFi.localIP().toString();
  } else if (portalActive) {
    wifiText = apSsid[0] ? apSsid : "AP";
    wifiIpText = WiFi.softAPIP().toString();
  } else {
    wifiText = tr(STR_WIFI_CONNECTING);
  }

  const char *bluetoothText = orientationLocked ? tr(STR_ORIENT_LOCKED)
                                                : tr(STR_ORIENT_AUTO);

  static String displayedWifiText;
  static String displayedWifiIpText;
  static String displayedBluetoothText;
  static int8_t displayedWifiEnabled = -1;
  static int8_t displayedOrientationLocked = -1;
  static uint8_t displayedTheme = 0xFF;
  static uint8_t displayedLanguage = 0xFF;
  // A theme or language change must re-render colors and text even when the
  // underlying wifi/orientation state has not changed.
  if (displayedTheme != uiTheme || displayedLanguage != uiLanguage) {
    displayedTheme = uiTheme;
    displayedLanguage = uiLanguage;
    displayedWifiEnabled = -1;
    displayedOrientationLocked = -1;
    displayedWifiText = "";
    displayedBluetoothText = "";
  }
  if (displayedWifiText != wifiText) {
    displayedWifiText = wifiText;
    lv_label_set_text(wifiControlStatus, displayedWifiText.c_str());
    changed = true;
  }
  if (displayedWifiIpText != wifiIpText) {
    displayedWifiIpText = wifiIpText;
    lv_label_set_text(wifiControlIp, displayedWifiIpText.c_str());
    changed = true;
  }
  if (displayedBluetoothText != bluetoothText) {
    displayedBluetoothText = bluetoothText;
    lv_label_set_text(bluetoothControlStatus, displayedBluetoothText.c_str());
    changed = true;
  }

  // Active cards use a translucent accent tint, not a solid accent block:
  // accent text on an accent fill was invisible on the device.
  const lv_color_t activeColor = themeTint(UI_ACCENT_BLUE, LV_OPA_30);
  const lv_color_t activeBorder = lv_color_hex(themeAccent(UI_ACCENT_BLUE));
  const lv_color_t inactiveColor = themeControlSurface(UI_ACCENT_BLUE);
  const lv_color_t inactiveBorder = themeControlBorder(UI_ACCENT_BLUE);
  const lv_color_t activeTextColor = lv_color_hex(themeAccent(UI_ACCENT_BLUE));
  const lv_color_t inactiveTextColor = themeTextDim(0x9299A3);
  lv_obj_set_style_radius(wifiControlButton,
    themeControlRadius(ClockLayout::scale(22, SCREEN_WIDTH)), 0);
  lv_obj_set_style_radius(bluetoothControlButton,
    themeControlRadius(ClockLayout::scale(22, SCREEN_WIDTH)), 0);
  if (displayedWifiEnabled != static_cast<int8_t>(wifiUserEnabled)) {
    displayedWifiEnabled = wifiUserEnabled;
    lv_obj_set_style_bg_color(wifiControlButton,
                              wifiUserEnabled ? activeColor : inactiveColor, LV_PART_MAIN);
    lv_obj_set_style_border_color(wifiControlButton,
                                  wifiUserEnabled ? activeBorder : inactiveBorder, LV_PART_MAIN);
    lv_obj_set_style_text_color(wifiControlStatus,
                                wifiUserEnabled ? activeTextColor : inactiveTextColor, 0);
    lv_obj_set_style_text_color(wifiControlIp,
                                wifiUserEnabled ? activeTextColor : inactiveTextColor, 0);
    if (wifiControlIcon) {
      lv_obj_set_style_img_recolor(wifiControlIcon,
        wifiUserEnabled ? activeTextColor : themeIcon(), 0);
    }
    changed = true;
  }
  if (displayedOrientationLocked != static_cast<int8_t>(orientationLocked)) {
    displayedOrientationLocked = orientationLocked;
    const lv_color_t lockColor = orientationLocked ? activeColor : inactiveColor;
    const lv_color_t lockTextColor = orientationLocked
                                       ? activeTextColor : inactiveTextColor;
    lv_obj_set_style_bg_color(bluetoothControlButton, lockColor, LV_PART_MAIN);
    lv_obj_set_style_border_color(bluetoothControlButton,
                                  orientationLocked ? activeBorder : inactiveBorder, LV_PART_MAIN);
    lv_obj_set_style_text_color(bluetoothControlStatus, lockTextColor, 0);
    if (bluetoothControlIcon) {
      lv_obj_set_style_img_recolor(bluetoothControlIcon,
        orientationLocked ? activeTextColor : themeIcon(), 0);
    }
    changed = true;
  }
  changed = updateBatteryPanelStatus() || changed;
  if (changed && controlPanelVisible &&
      !lv_obj_has_flag(controlPanel, LV_OBJ_FLAG_HIDDEN) &&
      (displayRotation == LV_DISP_ROT_90 || displayRotation == LV_DISP_ROT_270)) {
    controlPanelContentRefreshRequested = true;
  }
}
