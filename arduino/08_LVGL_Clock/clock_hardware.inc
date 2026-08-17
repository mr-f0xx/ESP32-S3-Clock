// Display, touch, RTC and orientation primitives.

// Keep orientation transition state beside its implementation. Arduino IDE can
// retain an open .ino buffer while re-reading included .inc files from disk;
// defining these only in the sketch made that mixed state fail to compile.
volatile bool orientationTransitionBlanked = false;
volatile uint32_t orientationTransitionDeadlineMs = 0;
volatile bool orientationVideoReady = false;
volatile bool orientationUiReady = false;
volatile bool orientationFadeOutActive = false;
volatile bool orientationFadeInActive = false;
volatile uint8_t orientationFadeStep = 0;
volatile uint32_t orientationFadeStepDueAt = 0;
lv_disp_rot_t orientationPendingRotation = LV_DISP_ROT_NONE;
// Included once by 08_LVGL_Clock.ino inside its private namespace.

String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '\\' || c == '"') {
      escaped += '\\';
      escaped += c;
    } else if (c == '\n') {
      escaped += F("\\n");
    } else if (static_cast<uint8_t>(c) >= 0x20) {
      escaped += c;
    }
  }
  return escaped;
}

void lvglTick(void *) {
  lv_tick_inc(LVGL_TICK_MS);
}

void IRAM_ATTR onTouchInterrupt() {
  touchPending = true;
}

bool takeTouchInterrupt() {
  noInterrupts();
  const bool pending = touchPending;
  touchPending = false;
  interrupts();
  return pending;
}

bool recoverSharedI2cBus(const char *reason) {
  // A soft restart does not power-cycle the external devices. If a controller
  // was interrupted mid-transaction it can keep SDA low across ESP.restart().
  // Clock it out and issue a STOP before starting the Arduino Wire driver.
  Wire.end();
  pinMode(IIC_SDA, INPUT_PULLUP);
  pinMode(IIC_SCL, INPUT_PULLUP);
  delayMicroseconds(10);
  const bool wasStuck = digitalRead(IIC_SDA) == LOW ||
                        digitalRead(IIC_SCL) == LOW;
  if (wasStuck) {
    // CST9217 is the device most likely to retain an unfinished transaction
    // across a software reset. Reset it before attempting generic clock-pulse
    // recovery so an already-locked board can recover without a power cycle.
    pinMode(TP_RST, OUTPUT);
    digitalWrite(TP_RST, LOW);
    delay(10);
    digitalWrite(TP_RST, HIGH);
    delay(35);
    pinMode(IIC_SCL, OUTPUT_OPEN_DRAIN);
    digitalWrite(IIC_SCL, HIGH);
    for (uint8_t pulse = 0; pulse < 18 && digitalRead(IIC_SDA) == LOW; ++pulse) {
      digitalWrite(IIC_SCL, LOW);
      delayMicroseconds(5);
      digitalWrite(IIC_SCL, HIGH);
      delayMicroseconds(5);
    }
    pinMode(IIC_SDA, OUTPUT_OPEN_DRAIN);
    digitalWrite(IIC_SDA, LOW);
    delayMicroseconds(5);
    digitalWrite(IIC_SCL, HIGH);
    delayMicroseconds(5);
    digitalWrite(IIC_SDA, HIGH);
    delayMicroseconds(5);
    pinMode(IIC_SDA, INPUT_PULLUP);
    pinMode(IIC_SCL, INPUT_PULLUP);
  }
  const bool released = digitalRead(IIC_SDA) == HIGH &&
                        digitalRead(IIC_SCL) == HIGH;
  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setTimeOut(50);
  if (wasStuck || !released)
    Serial.printf("I2C bus recovery (%s): %s\n", reason,
                  released ? "released" : "still held low");
  return released;
}

bool isValidRtcTime(const RTC_DateTime &value) {
  return value.getYear() >= 2024 && value.getYear() <= 2099 &&
         value.getMonth() >= 1 && value.getMonth() <= 12 &&
         value.getDay() >= 1 && value.getDay() <= 31 &&
         value.getHour() <= 23 && value.getMinute() <= 59 &&
         value.getSecond() <= 59;
}

bool readRtcTime(RTC_DateTime &value) {
  if (!rtcReady) return false;

  value = rtc.getDateTime();
  if (isValidRtcTime(value)) {
    rtcReadFailureCount = 0;
    rtcTimeValid = true;
    timeSynced = true;
    return true;
  }

  if (++rtcReadFailureCount < 3) return false;
  rtcReadFailureCount = 0;
  Serial.println("PCF85063 read failed repeatedly; resetting I2C bus");
  recoverSharedI2cBus("RTC read");
  delay(3);

  value = rtc.getDateTime();
  if (!isValidRtcTime(value)) return false;
  rtcTimeValid = true;
  timeSynced = true;
  Serial.println("PCF85063 read recovered");
  return true;
}

void onNtpTimeSynchronized(struct timeval *) {
  ntpSyncPending = true;
}

void rounderCallback(lv_disp_drv_t *, lv_area_t *area) {
  // LVGL cannot combine driver.full_refresh with software rotation: its
  // refresh path returns before flush_ready(), leaving the draw buffer locked.
  // Keep the driver in normal mode and expand Music invalidations instead.
  if (musicWholeScreenRefreshEnabled && fullScreenDrawBufferReady) {
    area->x1 = 0;
    area->y1 = 0;
    area->x2 = lv_disp_get_hor_res(nullptr) - 1;
    area->y2 = lv_disp_get_ver_res(nullptr) - 1;
    return;
  }
  if (area->x1 & 1) --area->x1;
  if (area->y1 & 1) --area->y1;
  if (!(area->x2 & 1)) ++area->x2;
  if (!(area->y2 & 1)) ++area->y2;
}

void lockDisplayBus() {
  if (displayBusMutex) xSemaphoreTake(displayBusMutex, portMAX_DELAY);
}

void unlockDisplayBus() {
  if (displayBusMutex) xSemaphoreGive(displayBusMutex);
}

void restoreOrientationBrightness() {
  if (!orientationTransitionBlanked) return;
  orientationTransitionBlanked = false;
  orientationTransitionDeadlineMs = 0;
  orientationVideoReady = false;
  orientationUiReady = false;
  orientationFadeStep = 0;
  orientationFadeStepDueAt = millis();
  orientationFadeInActive = true;
}

void completeWeatherOrientationIfReady() {
  if (orientationTransitionBlanked && weatherPageVisible &&
      orientationVideoReady && orientationUiReady)
    restoreOrientationBrightness();
}

void drawDisplayRegion(int16_t x, int16_t y, uint32_t width, uint32_t height,
                       lv_color_t *pixels) {
#if LV_COLOR_16_SWAP
  gfx->draw16bitBeRGBBitmap(x, y, reinterpret_cast<uint16_t *>(&pixels->full),
                            width, height);
#else
  gfx->draw16bitRGBBitmap(x, y, reinterpret_cast<uint16_t *>(&pixels->full),
                          width, height);
#endif
}

void displayFlush(lv_disp_drv_t *display, const lv_area_t *area, lv_color_t *pixels) {
  lv_area_t renderArea = *area;
  lv_color_t *renderPixels = pixels;
  const uint32_t originalWidth = area->x2 - area->x1 + 1;

  // Weather video bypasses LVGL and is already present in the LCD framebuffer.
  // Never let an LVGL invalidation repaint its black placeholder over a decoded
  // block. A full-page dirty area is cropped to the 120-row status strip.
  // The direct-LCD wallpaper owns the upper 360 rows only while it is the
  // visible foreground. Once the control panel starts opening, the video
  // workers stop and LVGL must be allowed to repaint the complete overlay.
  // Keeping this crop active for the panel left the old video pixels in place,
  // which looked as if the wallpaper had drawn over the controls.
  const bool weatherVideoOwnsTop =
    weatherPageVisible && weatherVideoFrameVisible &&
    !controlPanelVisible && !controlPanelShowRequested;
  if (weatherVideoOwnsTop && renderArea.y1 < WEATHER_VIDEO_HEIGHT) {
    if (renderArea.y2 < WEATHER_VIDEO_HEIGHT) {
      lv_disp_flush_ready(display);
      return;
    }
    const uint32_t skippedRows = WEATHER_VIDEO_HEIGHT - renderArea.y1;
    renderPixels += skippedRows * originalWidth;
    renderArea.y1 = WEATHER_VIDEO_HEIGHT;
  }

  const uint32_t width = renderArea.x2 - renderArea.x1 + 1;
  const uint32_t height = renderArea.y2 - renderArea.y1 + 1;

  lockDisplayBus();
  // LVGL 8 rotates 90/270-degree dirty regions in small synchronous chunks.
  // A button press produces several consecutive invalidations and can leave
  // its single draw buffer waiting between those chunks. With a square panel,
  // rotate the complete dirty rectangle into PSRAM and submit it once instead.
  if (manualRotationBufferReady && displaySoftwareRotation != LV_DISP_ROT_NONE) {
    int16_t outputX = renderArea.x1;
    int16_t outputY = renderArea.y1;
    uint32_t outputWidth = width;
    uint32_t outputHeight = height;

    if (displaySoftwareRotation == LV_DISP_ROT_90) {
      outputX = renderArea.y1;
      outputY = SCREEN_HEIGHT - renderArea.x2 - 1;
      outputWidth = height;
      outputHeight = width;
      for (uint32_t sourceY = 0; sourceY < height; ++sourceY) {
        for (uint32_t sourceX = 0; sourceX < width; ++sourceX) {
          rotationBufferMemory[(width - sourceX - 1U) * height + sourceY] =
            renderPixels[sourceY * width + sourceX];
        }
      }
    }
    drawDisplayRegion(outputX, outputY, outputWidth, outputHeight,
                      rotationBufferMemory);
  } else {
    drawDisplayRegion(renderArea.x1, renderArea.y1, width, height, renderPixels);
  }
  unlockDisplayBus();
  const bool lastFlush = lv_disp_flush_is_last(display);
  const bool orientationFrameComplete =
    orientationTransitionBlanked && !weatherPageVisible && lastFlush;
  if (orientationTransitionBlanked && weatherPageVisible && lastFlush)
    orientationUiReady = true;
  lv_disp_flush_ready(display);
  if (orientationFrameComplete) restoreOrientationBrightness();
  else completeWeatherOrientationIfReady();
}

// Kept with the touch module so gesture-capable .inc files do not depend on a
// matching revision of the main sketch for this newly added coordinate.
int16_t gestureStartX = -1;
int16_t gestureLastX = -1;
int16_t gestureLastY = -1;

lv_dir_t logicalGestureDirection(lv_indev_t *input) {
  if (!input) return LV_DIR_NONE;
  // touchRead() now feeds the hardware half-turn into LVGL's actual pointer
  // coordinates, so the gesture vector and hit-test coordinates share one
  // transform and no direction-only correction is required here.
  return lv_indev_get_gesture_dir(input);
}

void touchRead(lv_indev_drv_t *, lv_indev_data_t *data) {
  if (!touchReady) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }
  // Runtime orientation changes are brief and own the logical coordinate
  // transform. Ignore a new press during that window so no LVGL object keeps
  // coordinates from two orientations in the same gesture.
  if (orientationFadeOutActive || orientationTransitionBlanked ||
      orientationFadeInActive) {
    takeTouchInterrupt();
    touchCurrentlyPressed = false;
    data->state = LV_INDEV_STATE_REL;
    return;
  }
  // Start a read only after the CST92xx interrupt, but once a press has begun
  // keep polling until an explicit zero-point sample is seen. Some release
  // interrupts are lost while LVGL is rotating and flushing a dirty region.
  // Returning without updating our press latch made rollers/sliders repeatedly
  // re-enter their pressed state and continuously redraw at 90/270 degrees.
  const bool interrupted = takeTouchInterrupt();
  if (interrupted) touchWakePollingFallback = false;
  if (!interrupted && !touchCurrentlyPressed && !touchWakePollingFallback) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }

  int16_t x[2] = {};
  int16_t y[2] = {};
  const uint8_t count = touch.getPoint(x, y, touch.getSupportTouchPoint());
  if (count > 0) {
    markUserActivity();
    data->state = LV_INDEV_STATE_PR;
    data->point.x = constrain(x[0], 0, SCREEN_WIDTH - 1);
    data->point.y = constrain(y[0], 0, SCREEN_HEIGHT - 1);
    // LVGL knows about displaySoftwareRotation (0/90) and will apply that part
    // itself. Feed the CO5300 MADCTL 180-degree half-turn into the real pointer
    // point first so hit testing, sliders and gesture vectors all match at
    // physical 180/270 degrees.
    if (displayRotation == LV_DISP_ROT_180 || displayRotation == LV_DISP_ROT_270) {
      data->point.x = SCREEN_WIDTH - data->point.x - 1;
      data->point.y = SCREEN_HEIGHT - data->point.y - 1;
    }
    int16_t logicalX = data->point.x;
    int16_t logicalY = data->point.y;
    if (displaySoftwareRotation == LV_DISP_ROT_90) {
      const int16_t previousY = logicalY;
      logicalY = logicalX;
      logicalX = SCREEN_HEIGHT - previousY - 1;
    }
    if (!touchCurrentlyPressed) {
      gestureStartX = logicalX;
      gestureStartY = logicalY;
      touchCurrentlyPressed = true;
    }
    gestureLastX = logicalX;
    gestureLastY = logicalY;
  } else {
    data->state = LV_INDEV_STATE_REL;
    if (touchCurrentlyPressed &&
        (weatherPageVisible || weatherInfoPageVisible) && !controlPanelVisible &&
        !controlPanelShowRequested && gestureStartX >= 0 && gestureStartY >= 0 &&
        gestureLastX >= 0 && gestureLastY >= 0) {
      const int16_t deltaX = gestureLastX - gestureStartX;
      const int16_t deltaY = gestureLastY - gestureStartY;
      if (max(abs(deltaX), abs(deltaY)) >= 30) {
        weatherTouchGestureDirection = abs(deltaX) > abs(deltaY)
          ? (deltaX < 0 ? LV_DIR_LEFT : LV_DIR_RIGHT)
          : (deltaY < 0 ? LV_DIR_TOP : LV_DIR_BOTTOM);
        weatherTouchGestureStartY = gestureStartY;
      }
    }
    touchCurrentlyPressed = false;
  }
}

void audioFriendlyDelay(uint32_t durationMs) {
  const uint32_t startedAt = millis();
  do {
    serviceMusicPlayback();
    delay(1);
  } while (millis() - startedAt < durationMs);
}

void fadeDisplayBrightness(uint8_t from, uint8_t to) {
  lockDisplayBus();
  gfx->setBrightness(from);
  unlockDisplayBus();
  audioFriendlyDelay(ROTATION_FADE_FRAME_MS);
  for (uint8_t frame = 1; frame <= ROTATION_FADE_FRAMES; ++frame) {
    const float progress = static_cast<float>(frame) / ROTATION_FADE_FRAMES;
    const float eased = progress * progress * (3.0f - 2.0f * progress);
    const int16_t level = static_cast<int16_t>(from) +
                          static_cast<int16_t>((static_cast<int16_t>(to) - from) * eased);
    lockDisplayBus();
    gfx->setBrightness(static_cast<uint8_t>(constrain(level, 0, 255)));
    unlockDisplayBus();
    audioFriendlyDelay(ROTATION_FADE_FRAME_MS);
  }
}

void beginScreenFade(uint8_t fromBrightness) {
  screenFadeTransitionActive = true;
  fadeDisplayBrightness(fromBrightness, 0);
}

void endScreenFade(uint8_t toBrightness) {
  audioFriendlyDelay(ROTATION_BLACK_HOLD_MS);
  fadeDisplayBrightness(0, toBrightness);
  screenFadeTransitionActive = false;
}

void setOrientationFadeBrightness(uint8_t level) {
  lockDisplayBus();
  gfx->setBrightness(level);
  unlockDisplayBus();
}

void applyScreenOrientationNow(lv_disp_rot_t nextRotation,
                               bool externallyFaded) {
  if (!externallyFaded) {
    orientationTransitionBlanked = true;
    orientationTransitionDeadlineMs = millis() + 1000;
    orientationVideoReady = !weatherPageVisible;
    orientationUiReady = !weatherPageVisible;
  }

  displayRotation = nextRotation;
  displaySoftwareRotation =
    (nextRotation == LV_DISP_ROT_90 || nextRotation == LV_DISP_ROT_270)
      ? LV_DISP_ROT_90 : LV_DISP_ROT_NONE;

  // This module's working address mode is 0xA0 (MY | MV). A 180-degree turn
  // must preserve MV and toggle the standard MY/MX scan directions, producing
  // 0x60 (MX | MV). Combining it with the one software transpose covers all
  // four directions without a 180/270 per-pixel copy.
  const bool hardwareFlip180 =
    nextRotation == LV_DISP_ROT_180 || nextRotation == LV_DISP_ROT_270;
  notifyWeatherWallpaperOrientationChanged();
  lockDisplayBus();
  bus->writeC8D8(0x36, hardwareFlip180 ? 0x60 : 0xA0);
  unlockDisplayBus();

  lv_disp_set_rotation(nullptr, displaySoftwareRotation);
  if (weatherPageVisible) invalidateWeatherWallpaperStatus();
  else lv_obj_invalidate(lv_scr_act());
  Serial.printf("Screen rotation: %u degrees\n",
                static_cast<unsigned>(displayRotation) * 90U);
}

bool serviceOrientationFade() {
  const uint32_t now = millis();
  if (orientationFadeOutActive) {
    if (static_cast<int32_t>(now - orientationFadeStepDueAt) < 0) return true;
    if (orientationFadeStep < ROTATION_FADE_FRAMES) ++orientationFadeStep;
    const uint8_t level = static_cast<uint8_t>(
      (static_cast<uint16_t>(activeBrightness) *
       (ROTATION_FADE_FRAMES - orientationFadeStep)) /
      ROTATION_FADE_FRAMES);
    setOrientationFadeBrightness(level);
    orientationFadeStepDueAt = now + ROTATION_FADE_FRAME_MS;
    if (orientationFadeStep >= ROTATION_FADE_FRAMES) {
      orientationFadeOutActive = false;
      applyScreenOrientationNow(orientationPendingRotation, false);
    }
    return true;
  }
  if (orientationFadeInActive) {
    if (static_cast<int32_t>(now - orientationFadeStepDueAt) < 0) return true;
    if (orientationFadeStep < ROTATION_FADE_FRAMES) ++orientationFadeStep;
    const uint8_t level = static_cast<uint8_t>(
      (static_cast<uint16_t>(activeBrightness) * orientationFadeStep +
       ROTATION_FADE_FRAMES / 2U) / ROTATION_FADE_FRAMES);
    setOrientationFadeBrightness(level);
    orientationFadeStepDueAt = now + ROTATION_FADE_FRAME_MS;
    if (orientationFadeStep >= ROTATION_FADE_FRAMES) {
      orientationFadeInActive = false;
      screenFadeTransitionActive = false;
    }
    return true;
  }
  return false;
}

void updateScreenOrientation(bool force = false) {
  if (!qmiReady || lowPowerMode) return;

  if (serviceOrientationFade()) return;

  // The first completed LVGL flush (or direct weather frame) restores the
  // backlight. Keep a timeout so a failed render can never leave the panel dark.
  if (orientationTransitionBlanked) {
    if (static_cast<int32_t>(millis() - orientationTransitionDeadlineMs) >= 0)
      restoreOrientationBrightness();
    else
      return;
  }

  // Direction lock freezes the current physical orientation while still
  // allowing forced setup/wake synchronization to restore a valid panel mode.
  if (orientationLocked && !force) return;

  // Never change LVGL's coordinate system in the middle of a pointer gesture.
  // This is especially important for rollers and sliders, which retain their
  // active object until LVGL receives the release sample.
  if (touchCurrentlyPressed) return;
  const uint32_t now = millis();
  if (!force && now - lastOrientationCheck < ORIENTATION_CHECK_MS) return;
  lastOrientationCheck = now;
  if (!qmi.getDataReady()) return;

  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  if (!qmi.getAccelerometer(x, y, z)) return;

  const float absX = fabsf(x);
  const float absY = fabsf(y);
  if (max(absX, absY) < ORIENTATION_MIN_GRAVITY ||
      fabsf(absX - absY) < ORIENTATION_AXIS_MARGIN) {
    return;
  }

  lv_disp_rot_t nextRotation;
  if (absX > absY) {
    nextRotation = x > 0.0f ? LV_DISP_ROT_270 : LV_DISP_ROT_90;
  } else {
    nextRotation = y < 0.0f ? LV_DISP_ROT_180 : LV_DISP_ROT_NONE;
  }

  if (nextRotation == displayRotation) return;
  // Setup and standby restoration already own a black-screen transition, so
  // they apply the orientation inside that existing fade. Runtime rotations
  // use this non-blocking state machine and never stall LVGL or JPEG decoding.
  if (force) {
    applyScreenOrientationNow(nextRotation, true);
    return;
  }
  if (screenFadeTransitionActive) return;
  screenFadeTransitionActive = true;
  orientationPendingRotation = nextRotation;
  orientationFadeStep = 0;
  orientationFadeStepDueAt = now;
  orientationFadeOutActive = true;
}
