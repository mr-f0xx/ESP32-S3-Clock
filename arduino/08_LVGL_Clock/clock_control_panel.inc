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

constexpr int16_t CONTROL_PANEL_SIZE = 480;
constexpr int16_t CONTROL_PANEL_BUTTON_X = 43;
constexpr int16_t CONTROL_PANEL_BUTTON_WIDTH = 130;
constexpr int16_t CONTROL_PANEL_BUTTON_HEIGHT = 130;
constexpr int16_t CONTROL_PANEL_BUTTON_RADIUS = LV_RADIUS_CIRCLE;
constexpr int16_t CONTROL_PANEL_WIFI_Y = 80;
constexpr int16_t CONTROL_PANEL_BLUETOOTH_Y = 288;
constexpr int16_t CONTROL_PANEL_SLIDER_Y = 36;
constexpr int16_t CONTROL_PANEL_SLIDER_WIDTH = 30;
constexpr int16_t CONTROL_PANEL_SLIDER_HEIGHT = 350;
constexpr int16_t CONTROL_PANEL_BRIGHTNESS_X = 240;
constexpr int16_t CONTROL_PANEL_VOLUME_X = 371;
constexpr int16_t CONTROL_PANEL_GESTURE_EDGE = 72;
bool controlPanelSuspendedWeather = false;
bool controlPanelSuspendedMusic = false;
bool controlPanelMusicWasPlaying = false;
bool controlPanelWeatherRevealPending = false;
uint32_t controlPanelWeatherRevealBaseline = 0;
uint32_t controlPanelWeatherRevealDeadline = 0;

void suspendControlPanelUnderlyingApp() {
  controlPanelSuspendedWeather = weatherPageVisible;
  controlPanelSuspendedMusic = musicPageVisible;
  controlPanelMusicWasPlaying = false;
  if (controlPanelSuspendedWeather) pauseWeatherWallpaperForControlPanel();
  if (controlPanelSuspendedMusic)
    controlPanelMusicWasPlaying = pauseMusicForControlPanel();
}

void resumeControlPanelUnderlyingApp() {
  if (controlPanelSuspendedWeather && weatherPageVisible)
    resumeWeatherWallpaperAfterControlPanel();
  if (controlPanelSuspendedMusic && musicPageVisible)
    resumeMusicAfterControlPanel(controlPanelMusicWasPlaying);
  controlPanelSuspendedWeather = false;
  controlPanelSuspendedMusic = false;
  controlPanelMusicWasPlaying = false;
}

void showControlPanel(bool fade = true) {
  if (!controlPanel || lowPowerMode || controlPanelVisible || appMenuVisible ||
      appMenuShowRequested) return;
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
  if (lv_event_get_code(event) != LV_EVENT_GESTURE || lowPowerMode) return;
  lv_indev_t *input = lv_indev_get_act();
  if (!input) return;

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
      !musicPageVisible && !weatherPageVisible && !weatherInfoPageVisible &&
      direction == LV_DIR_LEFT) {
    gestureStartY = -1;
    handleGlobalNavigationStep(1);
  } else if (!controlPanelVisible && !appMenuVisible && !calendarPageVisible &&
             !alarmPageVisible &&
             !musicPageVisible && !weatherPageVisible && !weatherInfoPageVisible &&
             direction == LV_DIR_RIGHT) {
    gestureStartY = -1;
    handleGlobalNavigationStep(-1);
  } else if (!controlPanelVisible && !appMenuVisible && !calendarPageVisible &&
             !alarmPageVisible &&
             !musicPageVisible && !weatherPageVisible && !weatherInfoPageVisible &&
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
             !musicPageVisible && !weatherPageVisible && !weatherInfoPageVisible &&
             direction == LV_DIR_TOP &&
             gestureStartY >= SCREEN_HEIGHT - CONTROL_PANEL_GESTURE_EDGE) {
    gestureStartY = -1;
    showAppMenu(true);
  } else if (!controlPanelVisible && !appMenuVisible && !calendarPageVisible &&
             !alarmPageVisible && !musicPageVisible && !weatherPageVisible &&
             !weatherInfoPageVisible &&
             direction == LV_DIR_BOTTOM) {
    gestureStartX = -1;
    gestureStartY = -1;
    handleGlobalNavigationStep(-1);
  } else if (!controlPanelVisible && !appMenuVisible && !calendarPageVisible &&
             !alarmPageVisible && !musicPageVisible && !weatherPageVisible &&
             !weatherInfoPageVisible &&
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
    hideAppMenu(true);
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
    percentText = "NO BAT";
    colorValue = 0x9299A3;
  } else {
    percentText = String(percent) + "%";
    colorValue = charging ? 0x45D483 : (percent <= 10 ? 0xFF5B62 : 0xFFFFFF);
  }

  static String displayedPercentText;
  static uint32_t displayedColorValue = 0xFFFFFFFF;
  static const lv_img_dsc_t *displayedImage = nullptr;
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
    if (webServerStarted) server.stop();
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
    if (webServerStarted) server.begin();
    else startWebServer();
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
  } else if (code == LV_EVENT_RELEASED) {
    volumeApplyRequested = true;
    volumeSaveRequested = true;
    controlPanelActionDueAt = millis() + 20;
  }
}

lv_obj_t *createControlButton(lv_obj_t *parent, int16_t y,
                               const lv_img_dsc_t *iconImage,
                               lv_event_cb_t callback, lv_obj_t **icon,
                               lv_obj_t **status) {
  lv_obj_t *button = lv_btn_create(parent);
  lv_obj_set_pos(button, CONTROL_PANEL_BUTTON_X, y);
  lv_obj_set_size(button, CONTROL_PANEL_BUTTON_WIDTH, CONTROL_PANEL_BUTTON_HEIGHT);
  lv_obj_set_style_radius(button, CONTROL_PANEL_BUTTON_RADIUS, 0);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);

  // These alpha maps are generated directly from Tabler's official SVGs.
  // Keep their native 56 px size: no hand-drawn approximation and no image
  // transform path that can disappear on the rotated display.
  *icon = createOfficialTablerIcon(button, iconImage);

  *status = lv_label_create(parent);
  lv_obj_set_size(*status, 185, 28);
  lv_label_set_long_mode(*status, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(*status, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_align(*status, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_color(*status, lv_color_hex(0x101114), 0);
  lv_obj_set_style_bg_opa(*status, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(*status, 0, 0);
  lv_obj_align_to(*status, button, LV_ALIGN_OUT_BOTTOM_MID, 0, 7);
  return button;
}

void createControlPanel() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_add_event_cb(screen, mainUiGestureEvent, LV_EVENT_GESTURE, nullptr);

  controlPanel = lv_obj_create(screen);
  lv_obj_remove_style_all(controlPanel);
  lv_obj_set_pos(controlPanel, 0, 0);
  lv_obj_set_size(controlPanel, CONTROL_PANEL_SIZE, CONTROL_PANEL_SIZE);
  lv_obj_set_style_bg_color(controlPanel, lv_color_hex(0x101114), 0);
  lv_obj_set_style_bg_opa(controlPanel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(controlPanel, 0, 0);
  lv_obj_set_style_pad_all(controlPanel, 0, 0);
  lv_obj_clear_flag(controlPanel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(controlPanel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(controlPanel, mainUiGestureEvent, LV_EVENT_GESTURE, nullptr);

  batteryStatusIcon = createOfficialTablerIcon(
    controlPanel, &tabler_battery, lv_color_white());
  lv_obj_align(batteryStatusIcon, LV_ALIGN_TOP_LEFT, 24, 8);

  batteryStatusLabel = lv_label_create(controlPanel);
  lv_label_set_text(batteryStatusLabel, "--%");
  lv_obj_set_size(batteryStatusLabel, 92, 42);
  lv_obj_set_style_text_font(batteryStatusLabel, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_align(batteryStatusLabel, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_set_style_bg_color(batteryStatusLabel, lv_color_hex(0x101114), 0);
  lv_obj_set_style_bg_opa(batteryStatusLabel, LV_OPA_COVER, 0);
  lv_obj_align(batteryStatusLabel, LV_ALIGN_TOP_LEFT, 86, 25);

  wifiControlButton = createControlButton(
    controlPanel, CONTROL_PANEL_WIFI_Y, &tabler_wifi,
    wifiControlEvent, &wifiControlIcon, &wifiControlStatus);
  wifiControlIp = lv_label_create(controlPanel);
  lv_label_set_text(wifiControlIp, "--");
  lv_obj_set_size(wifiControlIp, 185, 24);
  lv_label_set_long_mode(wifiControlIp, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_font(wifiControlIp, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_align(wifiControlIp, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_bg_color(wifiControlIp, lv_color_hex(0x101114), 0);
  lv_obj_set_style_bg_opa(wifiControlIp, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(wifiControlIp, 0, 0);
  lv_obj_align_to(wifiControlIp, wifiControlStatus, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
  bluetoothControlButton = createControlButton(
    controlPanel, CONTROL_PANEL_BLUETOOTH_Y, &tabler_lock,
    bluetoothControlEvent, &bluetoothControlIcon, &bluetoothControlStatus);

  brightnessSlider = lv_slider_create(controlPanel);
  lv_obj_set_pos(brightnessSlider, CONTROL_PANEL_BRIGHTNESS_X, CONTROL_PANEL_SLIDER_Y);
  lv_obj_set_size(brightnessSlider, CONTROL_PANEL_SLIDER_WIDTH, CONTROL_PANEL_SLIDER_HEIGHT);
  lv_slider_set_range(brightnessSlider, 0, 100);
  lv_slider_set_value(brightnessSlider, 25, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(brightnessSlider, lv_palette_main(LV_PALETTE_BLUE),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(brightnessSlider, lv_palette_main(LV_PALETTE_BLUE),
                            LV_PART_KNOB);
  lv_obj_clear_flag(brightnessSlider, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(brightnessSlider, brightnessControlEvent, LV_EVENT_ALL, nullptr);

  volumeSlider = lv_slider_create(controlPanel);
  lv_obj_set_pos(volumeSlider, CONTROL_PANEL_VOLUME_X, CONTROL_PANEL_SLIDER_Y);
  lv_obj_set_size(volumeSlider, CONTROL_PANEL_SLIDER_WIDTH, CONTROL_PANEL_SLIDER_HEIGHT);
  lv_slider_set_range(volumeSlider, 0, 100);
  lv_slider_set_value(volumeSlider, DEFAULT_AUDIO_VOLUME, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(volumeSlider, lv_palette_main(LV_PALETTE_BLUE),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(volumeSlider, lv_palette_main(LV_PALETTE_BLUE),
                            LV_PART_KNOB);
  lv_obj_clear_flag(volumeSlider, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(volumeSlider, volumeControlEvent, LV_EVENT_ALL, nullptr);

  lv_obj_t *volumeIcon = createOfficialTablerIcon(
    controlPanel, &tabler_volume, lv_color_white());
  lv_obj_align_to(volumeIcon, volumeSlider, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);

  lv_obj_t *brightnessIcon = createOfficialTablerIcon(
    controlPanel, &tabler_sun, lv_color_white());
  lv_obj_align_to(brightnessIcon, brightnessSlider, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);

  updateControlPanelStatus();
  lv_obj_add_flag(controlPanel, LV_OBJ_FLAG_HIDDEN);
}

void syncControlPanelSettings() {
  if (brightnessSlider) {
    lv_slider_set_value(brightnessSlider,
                        brightnessHardwareToPercent(activeBrightness), LV_ANIM_OFF);
  }
  if (volumeSlider) lv_slider_set_value(volumeSlider, audioVolume, LV_ANIM_OFF);
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
    wifiText = "OFF";
  } else if (WiFi.status() == WL_CONNECTED) {
    wifiText = WiFi.SSID();
    wifiIpText = WiFi.localIP().toString();
  } else if (portalActive) {
    wifiText = apSsid[0] ? apSsid : "AP";
    wifiIpText = WiFi.softAPIP().toString();
  } else {
    wifiText = "CONNECTING";
  }

  const char *bluetoothText = orientationLocked ? "LOCKED" : "AUTO";

  static String displayedWifiText;
  static String displayedWifiIpText;
  static String displayedBluetoothText;
  static int8_t displayedWifiEnabled = -1;
  static int8_t displayedOrientationLocked = -1;
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

  const lv_color_t activeColor = lv_palette_main(LV_PALETTE_BLUE);
  const lv_color_t inactiveColor = lv_color_hex(0x555B65);
  const lv_color_t activeTextColor = lv_palette_main(LV_PALETTE_BLUE);
  const lv_color_t inactiveTextColor = lv_color_hex(0x9299A3);
  if (displayedWifiEnabled != static_cast<int8_t>(wifiUserEnabled)) {
    displayedWifiEnabled = wifiUserEnabled;
    lv_obj_set_style_bg_color(wifiControlButton,
                              wifiUserEnabled ? activeColor : inactiveColor, LV_PART_MAIN);
    lv_obj_set_style_text_color(wifiControlStatus,
                                wifiUserEnabled ? activeTextColor : inactiveTextColor, 0);
    lv_obj_set_style_text_color(wifiControlIp,
                                wifiUserEnabled ? activeTextColor : inactiveTextColor, 0);
    changed = true;
  }
  if (displayedOrientationLocked != static_cast<int8_t>(orientationLocked)) {
    displayedOrientationLocked = orientationLocked;
    const lv_color_t lockColor = orientationLocked ? activeColor : inactiveColor;
    const lv_color_t lockTextColor = orientationLocked
                                       ? activeTextColor : inactiveTextColor;
    lv_obj_set_style_bg_color(bluetoothControlButton, lockColor, LV_PART_MAIN);
    lv_obj_set_style_text_color(bluetoothControlStatus, lockTextColor, 0);
    setTablerIconColor(bluetoothControlIcon, lv_color_white());
    changed = true;
  }
  changed = updateBatteryPanelStatus() || changed;
  if (changed && controlPanelVisible &&
      !lv_obj_has_flag(controlPanel, LV_OBJ_FLAG_HIDDEN) &&
      (displayRotation == LV_DISP_ROT_90 || displayRotation == LV_DISP_ROT_270)) {
    controlPanelContentRefreshRequested = true;
  }
}
