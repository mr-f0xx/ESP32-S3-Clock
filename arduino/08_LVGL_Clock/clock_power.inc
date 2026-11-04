// Power-key handling and low-power clock mode.
// Included once by 08_LVGL_Clock.ino inside its private namespace.

constexpr uint16_t LOW_POWER_PARTIAL_LONG_START = 40;
constexpr uint16_t LOW_POWER_PARTIAL_LONG_END = 439;
constexpr uint16_t LOW_POWER_PARTIAL_SHORT_START = 144;
constexpr uint16_t LOW_POWER_PARTIAL_SHORT_END = 335;

bool lowPowerPanelModeActive = false;
bool lowPowerPanelSleeping = false;

bool anyAlarmEnabled() {
  for (bool enabled : alarmEnabled) {
    if (enabled) return true;
  }
  return false;
}

void writeLowPowerPanelBrightness(uint8_t brightness) {
  bus->beginWrite();
  bus->writeC8D8(CO5300_W_WDBRIGHTNESSVALAOD, brightness);
  bus->endWrite();
}

void setLowPowerPartialWindow() {
  // Quarter turns still transpose the framebuffer in software; 180 degrees is
  // supplied by the CO5300 axis-flip bits. The centered partial window is
  // invariant under that 180-degree flip, but must swap axes for 90/270.
  const bool quarterTurn =
    displayRotation == LV_DISP_ROT_90 || displayRotation == LV_DISP_ROT_270;
  const uint16_t xStart = quarterTurn ? LOW_POWER_PARTIAL_SHORT_START
                                      : LOW_POWER_PARTIAL_LONG_START;
  const uint16_t xEnd = quarterTurn ? LOW_POWER_PARTIAL_SHORT_END
                                    : LOW_POWER_PARTIAL_LONG_END;
  const uint16_t yStart = quarterTurn ? LOW_POWER_PARTIAL_LONG_START
                                      : LOW_POWER_PARTIAL_SHORT_START;
  const uint16_t yEnd = quarterTurn ? LOW_POWER_PARTIAL_LONG_END
                                    : LOW_POWER_PARTIAL_SHORT_END;

  // With this module's MADCTL setting, PTLAR receives the visible X range and
  // PTLAC receives the visible Y range (confirmed by the standalone test).
  bus->beginWrite();
  bus->writeC8D16D16(CO5300_W_PTLAR, xStart, xEnd);
  bus->writeC8D16D16(CO5300_W_PTLAC, yStart, yEnd);
  bus->endWrite();
}

void enableLowPowerPanelMode(uint8_t brightness) {
  setLowPowerPartialWindow();
  bus->sendCommand(CO5300_C_PTLON);
  writeLowPowerPanelBrightness(brightness);
  bus->sendCommand(CO5300_C_AODMON);
  lowPowerPanelModeActive = true;
}

void disableLowPowerPanelMode() {
  if (!lowPowerPanelModeActive) return;
  bus->sendCommand(CO5300_C_AODMOFF);
  delay(5);
  bus->sendCommand(CO5300_C_NORON);
  delay(5);
  lowPowerPanelModeActive = false;
}

bool lowPowerWakeKeyPressed() {
  return digitalRead(IMAGE_PREVIOUS_KEY_PIN) == LOW ||
         digitalRead(IMAGE_NEXT_KEY_PIN) == LOW;
}

constexpr uint64_t LOW_POWER_KEY_WAKE_MASK =
  (1ULL << IMAGE_PREVIOUS_KEY_PIN) | (1ULL << IMAGE_NEXT_KEY_PIN);

void enableLowPowerKeyWakeup() {
  pinMode(IMAGE_PREVIOUS_KEY_PIN, INPUT_PULLUP);
  pinMode(IMAGE_NEXT_KEY_PIN, INPUT_PULLUP);
  // GPIO0 and GPIO18 are both RTC IOs on ESP32-S3. EXT1 remains available
  // when the digital GPIO peripheral is powered down in light sleep, whereas
  // esp_sleep_enable_gpio_wakeup() does not. The latter produced
  // "Incorrect wakeup source (7)" and the keys never reached the restore path.
  gpio_wakeup_disable(static_cast<gpio_num_t>(IMAGE_PREVIOUS_KEY_PIN));
  gpio_wakeup_disable(static_cast<gpio_num_t>(IMAGE_NEXT_KEY_PIN));
  esp_sleep_disable_ext1_wakeup_io(0);
  const esp_err_t wakeResult = esp_sleep_enable_ext1_wakeup_io(
    LOW_POWER_KEY_WAKE_MASK, ESP_EXT1_WAKEUP_ANY_LOW);
  if (wakeResult != ESP_OK)
    Serial.printf("Standby EXT1 key wake setup failed: %s\n",
                  esp_err_to_name(wakeResult));
  else
    Serial.printf("Standby EXT1 key wake ready: mask 0x%llx\n",
                  static_cast<unsigned long long>(LOW_POWER_KEY_WAKE_MASK));
}

void disableLowPowerKeyWakeup() {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  esp_sleep_disable_ext1_wakeup_io(LOW_POWER_KEY_WAKE_MASK);
  gpio_wakeup_disable(static_cast<gpio_num_t>(IMAGE_PREVIOUS_KEY_PIN));
  gpio_wakeup_disable(static_cast<gpio_num_t>(IMAGE_NEXT_KEY_PIN));
}

enum class LowPowerRestoreApp : uint8_t {
  Album, AppMenu, Calendar, Alarm, Music, Audiobook, Radio, Weather, WeatherInfo
};

LowPowerRestoreApp lowPowerRestoreApp = LowPowerRestoreApp::Album;
bool lowPowerRestoreControlPanel = false;

void resetLowPowerInputState() {
  // A light-sleep transition can interrupt LVGL between PRESSED and RELEASED.
  // Reset both our gesture latches and LVGL's active object so the first touch
  // after wake cannot finish a stale swipe or click a tile on menu page two.
  touchCurrentlyPressed = false;
  gestureStartX = -1;
  gestureStartY = -1;
  gestureLastX = -1;
  gestureLastY = -1;
  weatherTouchGestureDirection = LV_DIR_NONE;
  weatherTouchGestureStartY = -1;
  appMenuPageStepRequested = 0;
  appMenuClickSuppressedUntil = 0;
  for (lv_indev_t *input = lv_indev_get_next(nullptr); input;
       input = lv_indev_get_next(input))
    lv_indev_reset(input, nullptr);
}

void captureLowPowerRestoreLocation() {
  lowPowerRestoreControlPanel = controlPanelVisible;
  if (weatherInfoPageVisible) lowPowerRestoreApp = LowPowerRestoreApp::WeatherInfo;
  else if (weatherPageVisible) lowPowerRestoreApp = LowPowerRestoreApp::Weather;
  else if (audiobookPageVisible) lowPowerRestoreApp = LowPowerRestoreApp::Audiobook;
  else if (radioPageVisible) lowPowerRestoreApp = LowPowerRestoreApp::Radio;
  else if (musicPageVisible) lowPowerRestoreApp = LowPowerRestoreApp::Music;
  else if (alarmPageVisible) lowPowerRestoreApp = LowPowerRestoreApp::Alarm;
  else if (calendarPageVisible) lowPowerRestoreApp = LowPowerRestoreApp::Calendar;
  else if (appMenuVisible) lowPowerRestoreApp = LowPowerRestoreApp::AppMenu;
  else lowPowerRestoreApp = LowPowerRestoreApp::Album;
}

void restoreLowPowerLocation() {
  controlPanelSuspendedWeather = false;
  controlPanelSuspendedMusic = false;
  controlPanelMusicWasPlaying = false;
  controlPanelSuspendedAudiobook = false;
  controlPanelAudiobookWasPlaying = false;
  switch (lowPowerRestoreApp) {
    case LowPowerRestoreApp::AppMenu:
      updateAppMenuPageVisibility();
      showAppMenu(false);
      break;
    case LowPowerRestoreApp::Calendar: showCalendarPage(false); break;
    case LowPowerRestoreApp::Alarm: showAlarmPage(false); break;
    case LowPowerRestoreApp::Music: showMusicPage(false); break;
    case LowPowerRestoreApp::Audiobook: showAudiobookPage(false); break;
    case LowPowerRestoreApp::Radio: showRadioPage(false); break;
    case LowPowerRestoreApp::Weather:
      if (videoPlayerModeActive()) showVideoPlayerPage(false);
      else showWeatherWallpaperPage(false);
      break;
    case LowPowerRestoreApp::WeatherInfo: showWeatherInfoPage(false); break;
    default: break;
  }
  if (lowPowerRestoreControlPanel &&
      lowPowerRestoreApp != LowPowerRestoreApp::AppMenu)
    showControlPanel(false);
}

void markUserActivity() {
  lastUserActivityAt = millis();
}

void checkAutoSleep() {
  if (lowPowerMode || screenFadeTransitionActive || !lastUserActivityAt) return;
  // Keep the full UI awake while USB/VBUS power is present. Refreshing the
  // activity timestamp also prevents an immediate sleep on unplug.
  if (pmuReady && power.isVbusIn()) {
    lastUserActivityAt = millis();
    return;
  }
  const uint32_t timeoutMs = static_cast<uint32_t>(idleSleepMinutes) * 60UL * 1000UL;
  if (!idleSleepEnabled || millis() - lastUserActivityAt < timeoutMs) return;
  Serial.printf("%u minutes idle: entering low-power clock mode\n", idleSleepMinutes);
  enterLowPowerClockMode();
}

bool isNightMinute(uint16_t minuteOfDay) {
  if (nightStartMinute == nightEndMinute) return false;
  if (nightStartMinute < nightEndMinute) {
    return minuteOfDay >= nightStartMinute && minuteOfDay < nightEndMinute;
  }
  return minuteOfDay >= nightStartMinute || minuteOfDay < nightEndMinute;
}

uint8_t sleepBrightnessForCurrentTime() {
  if (!rtcReady || !rtcTimeValid) return DAY_SLEEP_BRIGHTNESS;
  RTC_DateTime current;
  if (!readRtcTime(current)) return DAY_SLEEP_BRIGHTNESS;
  const uint16_t minuteOfDay = current.getHour() * 60U + current.getMinute();
  return isNightMinute(minuteOfDay) ? NIGHT_SLEEP_BRIGHTNESS : DAY_SLEEP_BRIGHTNESS;
}

void enterLowPowerClockMode() {
  if (lowPowerMode) return;
  captureLowPowerRestoreLocation();
  controlPanelWeatherRevealPending = false;
  beginScreenFade(activeBrightness);
  currentSleepBrightness = sleepBrightnessForCurrentTime();
  lowPowerMode = true;
  Serial.println("Entering low-power clock mode");

  resetLowPowerInputState();
  hideControlPanel(false);
  hideAppMenu(false);
  hideCalendarPage(false, false);
  hideAlarmPage(false, false);
  hideAlarmAlert();
  hideWeatherInfoPage(false, false);
  const bool weatherSdIdle = suspendWeatherWallpaperForLowPower();
  if (!weatherSdIdle) {
    // Sleeping or tearing down SPI while a FAT read is still inside the SD
    // driver is the failure mode that used to produce intermittent wake
    // freezes. Leave the device awake and recoverable instead.
    lowPowerMode = false;
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
    Serial.println("Standby canceled: weather SD worker did not stop safely");
    return;
  }
  hideMusicPage(false, false);
  hideAudiobookPage(false, false);
  hideRadioPage(false, false);
  suspendAudiobookForLowPower();
  suspendRadioForLowPower();
  suspendMusicForLowPower(weatherSdIdle);
  suspendImageKeys();
  enableLowPowerKeyWakeup();

  if (esp_sntp_enabled()) esp_sntp_stop();
  dnsServer.stop();
  server.stop();
  portalActive = false;
  if (uploadFile) uploadFile.close();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);

  if (touchReady) {
    detachInterrupt(digitalPinToInterrupt(TP_INT));
    // Keep CST9217 powered and awake. Its sleep command changes the INT/RST
    // pad modes and has proven able to leave the shared I2C bus held after a
    // light-sleep wake or software restart.
    touchReady = false;
  }

  if (qmiReady) qmi.powerDown();

  if (pmuReady) {
    power.disableTemperatureMeasure();
    power.disableBattDetection();
    power.disableVbusVoltageMeasure();
    power.disableBattVoltageMeasure();
    power.disableSystemVoltageMeasure();
    power.setChargingLedMode(XPOWERS_CHG_LED_OFF);
    power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    power.clearIrqStatus();
    // Leave ALDO1 enabled. On this board revision it cannot be treated as an
    // isolated audio-only rail; cycling it made wake restoration unreliable.
  }

  LittleFS.end();
  serviceAlarmSettingsSave(true);
  preferences.end();
  if (lvglTimer) esp_timer_stop(lvglTimer);

  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
  lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_NONE, 0);
  if (backgroundImage) lv_obj_add_flag(backgroundImage, LV_OBJ_FLAG_HIDDEN);
  if (timeBackground) lv_obj_add_flag(timeBackground, LV_OBJ_FLAG_HIDDEN);
  if (shadeObject) lv_obj_add_flag(shadeObject, LV_OBJ_FLAG_HIDDEN);
  if (dateLabel) lv_obj_add_flag(dateLabel, LV_OBJ_FLAG_HIDDEN);
  if (statusLabel) lv_obj_add_flag(statusLabel, LV_OBJ_FLAG_HIDDEN);
  if (addressLabel) lv_obj_add_flag(addressLabel, LV_OBJ_FLAG_HIDDEN);
  if (settingsButton) lv_obj_add_flag(settingsButton, LV_OBJ_FLAG_HIDDEN);

  if (sleepTimeDisplayEnabled) {
    lv_obj_set_style_bg_opa(timePanel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_dir(timePanel, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_width(timePanel, 0, 0);
    lv_obj_set_style_radius(timePanel, 0, 0);
    lv_obj_set_style_shadow_width(timePanel, 0, 0);
    // Use a real 96 px font in standby. Unlike transform_zoom, its layout and
    // clipping bounds match the rendered glyphs.
    applyClockFont();
    lv_obj_align(timePanel, LV_ALIGN_CENTER, 0, -36);
    lv_obj_center(timeLabel);
    lv_obj_clear_flag(timeLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(timeLabel, lv_color_white(), 0);
    lv_obj_set_style_text_opa(timeLabel, LV_OPA_COVER, 0);
    lv_obj_clear_flag(timePanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(timePanel);
    updateClock();
    lv_obj_update_layout(screen);
    lv_obj_invalidate(screen);
    lv_refr_now(nullptr);
    gfx->setBrightness(currentSleepBrightness);
    enableLowPowerPanelMode(currentSleepBrightness);
    lowPowerPanelSleeping = false;
  } else {
    lv_obj_add_flag(timePanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(screen);
    lv_refr_now(nullptr);
    gfx->setBrightness(0);
    gfx->displayOff();
    lowPowerPanelSleeping = true;
  }
  orientationFadeOutActive = false;
  orientationFadeInActive = false;
  orientationTransitionBlanked = false;
  orientationTransitionDeadlineMs = 0;
  orientationVideoReady = false;
  orientationUiReady = false;
  screenFadeTransitionActive = false;

  const uint32_t nowMs = millis();
  lowPowerNextClockPollAt = nowMs + LOW_POWER_CLOCK_POLL_SECONDS * 1000UL;
  if ((sleepTimeDisplayEnabled || anyAlarmEnabled()) &&
      rtcReady && rtcTimeValid) {
    RTC_DateTime current;
    if (readRtcTime(current)) {
      lowPowerDisplayedMinute = current.getMinute();
      lowPowerNextClockPollAt =
        nowMs + static_cast<uint32_t>(60U - current.getSecond()) * 1000UL;
    }
  }
  lowPowerLastTick = nowMs;
  Serial.println(sleepTimeDisplayEnabled
                   ? "Low-power AOD clock active; press - or + to restore normal mode"
                   : "Low-power screen-off mode active; press - or + to restore normal mode");
  Serial.flush();
}

void exitLowPowerClockMode() {
  if (!lowPowerMode) return;
  disableLowPowerKeyWakeup();
  uint8_t fadeFromBrightness = currentSleepBrightness;
  if (lowPowerPanelSleeping) {
    fadeFromBrightness = 0;
    gfx->setBrightness(0);
    gfx->displayOn();
    lowPowerPanelSleeping = false;
  } else {
    disableLowPowerPanelMode();
    gfx->setBrightness(currentSleepBrightness);
  }

  beginScreenFade(fadeFromBrightness);
  lowPowerMode = false;
  markUserActivity();
  Serial.println("Standby wake: restoring normal mode");

  if (lvglTimer) esp_timer_start_periodic(lvglTimer, LVGL_TICK_MS * 1000);

  const bool i2cRecovered = recoverSharedI2cBus("standby wake");

  if (qmiReady && i2cRecovered) {
    qmi.powerOn();
    qmi.enableAccelerometer();
    lastOrientationCheck = 0;
  }

  if (touchPresent) {
    touchReady = false;
    resetLowPowerInputState();
    noInterrupts();
    touchPending = false;
    touchWakePollingFallback = true;
    interrupts();
    if (i2cRecovered) {
      pinMode(TP_INT, INPUT_PULLUP);
      attachInterrupt(digitalPinToInterrupt(TP_INT), onTouchInterrupt, FALLING);
      touchReady = true;
      Serial.println("Standby wake: touch interrupt restored");
    } else {
      touchWakePollingFallback = false;
      Serial.println("Standby wake: touch disabled because I2C is held low");
    }
  }

  if (pmuReady) {
    power.enableBattDetection();
    power.enableBattVoltageMeasure();
    power.setChargingLedMode(XPOWERS_CHG_LED_CTRL_CHG);
    power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    power.clearIrqStatus();
    power.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
    power.enableIRQ(XPOWERS_AXP2101_PKEY_LONG_IRQ);
  }

  const bool fsReady = LittleFS.begin(true);
  if (!fsReady) Serial.println("LittleFS remount failed after low-power mode");
  loadSettings();
  initializeImageKeys();
  Serial.printf("Standby wake: inputs touch=%u fallback=%u, INT=%u, keys -/+=%u/%u\n",
                touchReady ? 1U : 0U,
                touchWakePollingFallback ? 1U : 0U,
                digitalRead(TP_INT) == HIGH ? 1U : 0U,
                digitalRead(IMAGE_PREVIOUS_KEY_PIN) == HIGH ? 1U : 0U,
                digitalRead(IMAGE_NEXT_KEY_PIN) == HIGH ? 1U : 0U);

  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, themePageBackground(), 0);
  lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_NONE, 0);
  if (shadeObject) lv_obj_add_flag(shadeObject, LV_OBJ_FLAG_HIDDEN);
  if (statusLabel) lv_obj_add_flag(statusLabel, LV_OBJ_FLAG_HIDDEN);
  if (addressLabel) lv_obj_add_flag(addressLabel, LV_OBJ_FLAG_HIDDEN);
  if (settingsButton) lv_obj_add_flag(settingsButton, LV_OBJ_FLAG_HIDDEN);

  // loadSettings() has already reapplied the selected 48 px main-page font
  // and restored the normal time panel dimensions.
  lv_obj_set_style_bg_opa(timePanel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_grad_dir(timePanel, LV_GRAD_DIR_NONE, 0);
  lv_obj_set_style_border_width(timePanel, 0, 0);
  lv_obj_set_style_shadow_width(timePanel, 0, 0);
  if (timeBackground) {
    lv_obj_clear_flag(timeBackground, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_set_style_bg_color(timePanel, themePageBackground(), 0);
    lv_obj_set_style_bg_opa(timePanel, LV_OPA_20, 0);
    lv_obj_set_style_radius(timePanel, TIME_PANEL_RADIUS, 0);
  }
  lv_obj_align(timePanel, LV_ALIGN_TOP_LEFT, 24, 24);
  lv_obj_center(timeLabel);
  lv_obj_set_style_text_color(timeLabel, lv_color_white(), 0);
  // Screen-off standby hides both objects. Restoring their normal styles does
  // not clear LVGL's hidden flags, which previously left the album clock
  // missing until reboot after waking with the AOD time option disabled.
  lv_obj_set_style_text_opa(timeLabel, LV_OPA_COVER, 0);
  lv_obj_clear_flag(timeLabel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(timePanel, LV_OBJ_FLAG_HIDDEN);
  if (fsReady) loadBackgroundImage();

  portalActive = false;
  ntpStarted = rtcCalibratedThisBoot;
  ntpSyncPending = false;
  updateNetworkLabels();
  updateControlPanelStatus();
  updateClock();
  updateScreenOrientation(true);
  restoreLowPowerLocation();
  // showWeatherWallpaperPage() commits its LVGL layer before restarting the
  // direct-LCD worker. A second full-screen refresh here could repaint the
  // black video placeholder over its first decoded frame.
  if (lowPowerRestoreApp == LowPowerRestoreApp::Weather &&
      !lowPowerRestoreControlPanel)
    invalidateWeatherWallpaperStatus();
  else
    lv_obj_invalidate(screen);
  lv_timer_handler();
  endScreenFade(activeBrightness);

  // Radio restoration must not hold the UI (or delay a standby alarm) for the
  // 15-second connection timeout. Wi-Fi.begin is asynchronous; completion is
  // handled by the existing control-panel connection state machine.
  if (wifiUserEnabled) {
    if (savedSsid.isEmpty()) {
      portalRequested = true;
    } else {
      WiFi.mode(WIFI_STA);
      WiFi.setSleep(false);
      WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
      wifiControlConnectPending = true;
      wifiControlConnectStartedAt = millis();
    }
  }
  const bool restoredLiveWeather =
    lowPowerRestoreApp == LowPowerRestoreApp::Weather &&
    !videoPlayerModeActive();
  if (wifiUserEnabled && restoredLiveWeather) {
    startWeatherNetworkService();
    queueWeatherRefreshKeepingCache();
    Serial.println("Standby wake: live weather refresh queued");
  }
  updateNetworkLabels();
  updateControlPanelStatus();
  Serial.println("Normal mode restored");
}

void pollPowerKey() {
  if (!pmuReady || lowPowerMode || millis() - lastPowerKeyCheck < 100) return;
  lastPowerKeyCheck = millis();
  power.getIrqStatus();
  const bool longPressed = power.isPekeyLongPressIrq();
  const bool shortPressed = power.isPekeyShortPressIrq();
  power.clearIrqStatus();
  if (longPressed) {
    Serial.println("PWR long press: restarting");
    Serial.flush();
    delay(50);
    ESP.restart();
  }
  if (shortPressed) {
    // In the Video app the three physical keys are previous, play/pause and
    // next. Outside Video, the PMU key keeps its normal standby function.
    if (!toggleVideoPlayerPlayback()) enterLowPowerClockMode();
  }
}

void runLowPowerClock() {
  const uint32_t nowMs = millis();
  const uint32_t elapsed = nowMs - lowPowerLastTick;
  lowPowerLastTick = nowMs;
  if (elapsed) lv_tick_inc(elapsed);

  // Also sample before sleeping so a key pressed while the minute refresh was
  // running cannot be missed. During sleep the same pins wake the ESP32-S3 in
  // hardware, so there is no periodic key-poll timer.
  if (lowPowerWakeKeyPressed()) {
    delay(15);
    if (lowPowerWakeKeyPressed()) {
      exitLowPowerClockMode();
      return;
    }
  }

  if (rtcReady && rtcTimeValid &&
      static_cast<int32_t>(nowMs - lowPowerNextClockPollAt) >= 0) {
    lowPowerNextClockPollAt =
      nowMs + LOW_POWER_CLOCK_POLL_SECONDS * 1000UL;
    RTC_DateTime current;
    if (readRtcTime(current)) {
      const int8_t alarmIndex = matchingAlarmIndex(current);
      if (alarmIndex >= 0) {
        exitLowPowerClockMode();
        showAlarmAlert(static_cast<uint8_t>(alarmIndex), current);
        return;
      }
      lowPowerNextClockPollAt =
        nowMs + static_cast<uint32_t>(60U - current.getSecond()) * 1000UL;
      if (sleepTimeDisplayEnabled) {
        if (current.getMinute() != lowPowerDisplayedMinute) {
          lowPowerDisplayedMinute = current.getMinute();
          disableLowPowerPanelMode();
          updateClock();
          // Finish the complete GRAM update before limiting the CO5300 scan
          // area again. This keeps the once-per-minute update tear-free.
          lv_refr_now(nullptr);
        }
        const uint16_t minuteOfDay =
          current.getHour() * 60U + current.getMinute();
        const uint8_t desiredBrightness =
          isNightMinute(minuteOfDay) ? NIGHT_SLEEP_BRIGHTNESS
                                     : DAY_SLEEP_BRIGHTNESS;
        if (desiredBrightness != currentSleepBrightness)
          currentSleepBrightness = desiredBrightness;
        if (!lowPowerPanelModeActive) {
          enableLowPowerPanelMode(currentSleepBrightness);
        } else {
          writeLowPowerPanelBrightness(currentSleepBrightness);
        }
      }
    }
  }

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
  if (sleepTimeDisplayEnabled || anyAlarmEnabled()) {
    uint32_t nextWakeMs = LOW_POWER_CLOCK_POLL_SECONDS * 1000UL;
    if (rtcReady && rtcTimeValid) {
      const int32_t clockRemaining =
        static_cast<int32_t>(lowPowerNextClockPollAt - nowMs);
      nextWakeMs = min(nextWakeMs,
                       clockRemaining > 0
                         ? static_cast<uint32_t>(clockRemaining) : 1U);
    }
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(nextWakeMs) * 1000ULL);
  }
  esp_light_sleep_start();
  const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  if (wakeCause == ESP_SLEEP_WAKEUP_EXT1 ||
      wakeCause == ESP_SLEEP_WAKEUP_GPIO) {
    // Treat the hardware wake itself as the key event so a short press is not
    // lost if it is released before execution resumes here. GPIO is retained
    // only for compatibility with a previously armed source.
    const uint64_t ext1Status = wakeCause == ESP_SLEEP_WAKEUP_EXT1
                                  ? esp_sleep_get_ext1_wakeup_status() : 0;
    Serial.printf("Standby key wake: cause %u, EXT1 status 0x%llx\n",
                  static_cast<unsigned>(wakeCause),
                  static_cast<unsigned long long>(ext1Status));
    exitLowPowerClockMode();
  }
}
