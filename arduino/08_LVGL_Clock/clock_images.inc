// Five-image LittleFS history and the board's active-low +/- keys.
// Included once by 08_LVGL_Clock.ino inside its private namespace.

struct ImageKeyState {
  uint8_t pin;
  bool rawHigh;
  bool stableHigh;
  uint32_t changedAt;
};

ImageKeyState previousImageKey{IMAGE_PREVIOUS_KEY_PIN, true, true, 0};
ImageKeyState nextImageKey{IMAGE_NEXT_KEY_PIN, true, true, 0};
volatile bool nextImageKeyInterruptPending = false;
bool nextImageKeyInterruptAttached = false;
uint32_t lastNextImageKeyAcceptedAt = 0;
bool albumAutoSwitching = false;

void IRAM_ATTR onNextImageKeyInterrupt() {
  nextImageKeyInterruptPending = true;
}

bool takeNextImageKeyInterrupt() {
  noInterrupts();
  const bool pending = nextImageKeyInterruptPending;
  nextImageKeyInterruptPending = false;
  interrupts();
  return pending;
}

bool isValidImageFile(const char *path) {
  if (!path || !LittleFS.exists(path)) return false;
  File file = LittleFS.open(path, FILE_READ);
  const bool valid = file && file.size() == IMAGE_BYTES;
  if (file) file.close();
  return valid;
}

void saveImageHistory() {
  preferences.putUChar("img_count", imageCount);
  preferences.putUChar("img_head", imageHeadSlot);
  preferences.putUChar("img_current", imageCurrentSlot);
}

const char *currentImagePath() {
  if (!imageCount || imageCurrentSlot >= IMAGE_SLOT_COUNT) return nullptr;
  return IMAGE_SLOT_PATHS[imageCurrentSlot];
}

void initializeImageHistory() {
  imageCount = min(preferences.getUChar("img_count", 0), IMAGE_SLOT_COUNT);
  imageHeadSlot = preferences.getUChar("img_head", 0) % IMAGE_SLOT_COUNT;
  imageCurrentSlot = preferences.getUChar("img_current", imageHeadSlot) % IMAGE_SLOT_COUNT;

  // Preserve the single-image format used by earlier firmware versions.
  if (!imageCount && isValidImageFile(LEGACY_IMAGE_PATH)) {
    LittleFS.remove(IMAGE_SLOT_PATHS[0]);
    if (LittleFS.rename(LEGACY_IMAGE_PATH, IMAGE_SLOT_PATHS[0])) {
      imageCount = 1;
      imageHeadSlot = 0;
      imageCurrentSlot = 0;
      Serial.println("Migrated legacy background into image history");
    }
  }

  uint8_t validCount = 0;
  int8_t firstValidSlot = -1;
  for (uint8_t slot = 0; slot < IMAGE_SLOT_COUNT; ++slot) {
    if (isValidImageFile(IMAGE_SLOT_PATHS[slot])) {
      ++validCount;
      if (firstValidSlot < 0) firstValidSlot = slot;
    }
  }

  imageCount = validCount;
  if (!imageCount) {
    imageHeadSlot = 0;
    imageCurrentSlot = 0;
  } else {
    if (!isValidImageFile(IMAGE_SLOT_PATHS[imageHeadSlot])) {
      imageHeadSlot = static_cast<uint8_t>(firstValidSlot);
    }
    if (!isValidImageFile(IMAGE_SLOT_PATHS[imageCurrentSlot])) {
      imageCurrentSlot = imageHeadSlot;
    }
  }
  if (albumImageLimit < imageCount) {
    albumImageLimit = imageCount;
    preferences.putUChar("album_limit", albumImageLimit);
  }
  saveImageHistory();
  Serial.printf("Image history ready: %u/%u (physical %u), slot %u\n",
                imageCount, albumImageLimit, IMAGE_SLOT_COUNT,
                imageCurrentSlot);
}

void initializeImageKeys() {
  // gpio_wakeup_disable() removes the wake source but does not guarantee that
  // the low-level wake interrupt and pad routing are back at their power-on
  // state. Reset both pads before installing the normal polling/edge paths.
  if (nextImageKeyInterruptAttached) {
    detachInterrupt(digitalPinToInterrupt(IMAGE_NEXT_KEY_PIN));
    nextImageKeyInterruptAttached = false;
  }
  gpio_intr_disable(static_cast<gpio_num_t>(IMAGE_PREVIOUS_KEY_PIN));
  gpio_intr_disable(static_cast<gpio_num_t>(IMAGE_NEXT_KEY_PIN));
  gpio_reset_pin(static_cast<gpio_num_t>(IMAGE_PREVIOUS_KEY_PIN));
  gpio_reset_pin(static_cast<gpio_num_t>(IMAGE_NEXT_KEY_PIN));
  pinMode(IMAGE_PREVIOUS_KEY_PIN, INPUT_PULLUP);
  pinMode(IMAGE_NEXT_KEY_PIN, INPUT_PULLUP);
  const bool previousHigh = digitalRead(IMAGE_PREVIOUS_KEY_PIN) == HIGH;
  const bool nextHigh = digitalRead(IMAGE_NEXT_KEY_PIN) == HIGH;
  previousImageKey = {IMAGE_PREVIOUS_KEY_PIN, previousHigh, previousHigh, millis()};
  nextImageKey = {IMAGE_NEXT_KEY_PIN, nextHigh, nextHigh, millis()};
  nextImageKeyInterruptPending = false;
  lastNextImageKeyAcceptedAt = 0;
  attachInterrupt(digitalPinToInterrupt(IMAGE_NEXT_KEY_PIN),
                  onNextImageKeyInterrupt, FALLING);
  nextImageKeyInterruptAttached = true;
  Serial.printf("Image keys ready: GPIO0 = previous (-), GPIO18 = next (+), levels %u/%u\n",
                previousHigh ? 1U : 0U, nextHigh ? 1U : 0U);
}

void suspendImageKeys() {
  if (nextImageKeyInterruptAttached) {
    detachInterrupt(digitalPinToInterrupt(IMAGE_NEXT_KEY_PIN));
    nextImageKeyInterruptAttached = false;
  }
  nextImageKeyInterruptPending = false;
}

bool showCurrentImage(bool fade) {
  if (fade) beginScreenFade(activeBrightness);
  const bool loaded = loadBackgroundImage();
  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(nullptr);
  if (fade) endScreenFade(activeBrightness);
  return loaded;
}

void selectAdjacentImage(int8_t direction) {
  if (!direction || albumImageStepRequested) return;
  if (!albumAutoSwitching) markUserActivity();
  nextAlbumAutoSwitchAt = albumAutoSwitchEnabled
    ? millis() + static_cast<uint32_t>(albumSwitchMinutes) * 60000UL : 0;
  if (imageCount < 2) {
    Serial.printf("Image key ignored: only %u cached image(s)\n", imageCount);
    return;
  }
  if (controlPanelVisible || appMenuVisible || calendarPageVisible ||
      alarmPageVisible || musicPageVisible || weatherPageVisible) {
    Serial.println("Image key ignored: an overlay is open");
    return;
  }
  if (lowPowerMode) {
    Serial.println("Image key ignored: standby is active");
    return;
  }
  if (screenFadeTransitionActive) {
    Serial.println("Image key ignored: a screen transition is active");
    return;
  }

  albumImageStepRequested = direction > 0 ? 1 : -1;
}

void processAlbumImageStep() {
  if (!albumImageStepRequested) return;
  if (lowPowerMode || controlPanelVisible || appMenuVisible ||
      calendarPageVisible || alarmPageVisible || musicPageVisible ||
      weatherPageVisible) {
    albumImageStepRequested = 0;
    return;
  }
  if (screenFadeTransitionActive) return;
  const int8_t direction = albumImageStepRequested;
  albumImageStepRequested = 0;

  uint8_t slot = imageCurrentSlot;
  for (uint8_t checked = 0; checked < IMAGE_SLOT_COUNT; ++checked) {
    slot = static_cast<uint8_t>((slot + IMAGE_SLOT_COUNT + direction) % IMAGE_SLOT_COUNT);
    if (isValidImageFile(IMAGE_SLOT_PATHS[slot])) {
      imageCurrentSlot = slot;
      saveImageHistory();
      showCurrentImage(true);
      Serial.printf("Image selected: slot %u (%u cached)\n", slot, imageCount);
      return;
    }
  }
}

void serviceAlbumAutoSwitch() {
  if (!albumAutoSwitchEnabled || imageCount < 2 || lowPowerMode ||
      controlPanelVisible || appMenuVisible || calendarPageVisible ||
      alarmPageVisible || musicPageVisible || weatherPageVisible ||
      screenFadeTransitionActive) {
    if (!albumAutoSwitchEnabled) nextAlbumAutoSwitchAt = 0;
    return;
  }
  const uint32_t now = millis();
  const uint32_t intervalMs = static_cast<uint32_t>(albumSwitchMinutes) * 60000UL;
  if (!nextAlbumAutoSwitchAt) {
    nextAlbumAutoSwitchAt = now + intervalMs;
    return;
  }
  if (static_cast<int32_t>(now - nextAlbumAutoSwitchAt) < 0) return;
  nextAlbumAutoSwitchAt = now + intervalMs;
  albumAutoSwitching = true;
  selectAdjacentImage(1);
  albumAutoSwitching = false;
}

void pollImageKey(ImageKeyState &key, int8_t direction) {
  const uint32_t now = millis();
  const bool rawHigh = digitalRead(key.pin) == HIGH;
  if (rawHigh != key.rawHigh) {
    key.rawHigh = rawHigh;
    key.changedAt = now;
    Serial.printf("Image key GPIO%u level: %s\n", key.pin,
                  rawHigh ? "released" : "pressed");
  }
  if (rawHigh == key.stableHigh || now - key.changedAt < IMAGE_KEY_DEBOUNCE_MS) return;

  key.stableHigh = rawHigh;
  if (!key.stableHigh) handleGlobalNavigationStep(direction);
}

void pollImageKeys() {
  if (lowPowerMode) return;
  pollImageKey(previousImageKey, -1);

  // GPIO18/Key3 has an external 10K pull-up and is active low. Capture its
  // falling edge in hardware so a short '+' press cannot be lost while the
  // main loop is servicing Wi-Fi, NTP, LVGL or a file operation. The regular
  // polling path below remains as a fallback and tracks the release state.
  if (takeNextImageKeyInterrupt()) {
    const uint32_t now = millis();
    if (lastNextImageKeyAcceptedAt == 0 ||
        now - lastNextImageKeyAcceptedAt >= IMAGE_KEY_DEBOUNCE_MS) {
      lastNextImageKeyAcceptedAt = now;
      nextImageKey.rawHigh = false;
      nextImageKey.stableHigh = false;
      nextImageKey.changedAt = now;
      Serial.println("Image key GPIO18 interrupt: next (+)");
      handleGlobalNavigationStep(1);
    }
  }
  pollImageKey(nextImageKey, 1);
}

int8_t oldestImageSlot() {
  if (!imageCount) return -1;
  for (uint8_t offset = 1; offset <= IMAGE_SLOT_COUNT; ++offset) {
    const uint8_t slot = (imageHeadSlot + offset) % IMAGE_SLOT_COUNT;
    if (isValidImageFile(IMAGE_SLOT_PATHS[slot])) return slot;
  }
  return -1;
}

uint8_t nextUploadImageSlot() {
  if (!imageCount) return 0;
  const uint8_t first = (imageHeadSlot + 1) % IMAGE_SLOT_COUNT;
  if (imageCount < albumImageLimit) {
    for (uint8_t checked = 0; checked < IMAGE_SLOT_COUNT; ++checked) {
      const uint8_t candidate = (first + checked) % IMAGE_SLOT_COUNT;
      if (!isValidImageFile(IMAGE_SLOT_PATHS[candidate])) return candidate;
    }
  }
  // At the configured limit, the slot after the newest entry is the oldest
  // entry in this circular history and is replaced transactionally below.
  const int8_t oldest = oldestImageSlot();
  return oldest >= 0 ? static_cast<uint8_t>(oldest) : imageHeadSlot;
}

bool commitUploadedImageToHistory() {
  const uint8_t destinationSlot = nextUploadImageSlot();
  const char *destinationPath = IMAGE_SLOT_PATHS[destinationSlot];
  const bool replacedExisting = LittleFS.exists(destinationPath);

  LittleFS.remove(IMAGE_BACKUP_PATH);
  if (replacedExisting && !LittleFS.rename(destinationPath, IMAGE_BACKUP_PATH)) {
    uploadError = "无法备份最旧图片";
    return false;
  }
  if (!LittleFS.rename(IMAGE_TEMP_PATH, destinationPath)) {
    if (replacedExisting) LittleFS.rename(IMAGE_BACKUP_PATH, destinationPath);
    uploadError = "无法保存新图片";
    return false;
  }
  LittleFS.remove(IMAGE_BACKUP_PATH);

  if (!replacedExisting) {
    imageCount = min(static_cast<uint8_t>(imageCount + 1), albumImageLimit);
  }
  imageHeadSlot = destinationSlot;
  imageCurrentSlot = destinationSlot;
  saveImageHistory();
  Serial.printf("Image cached in slot %u (%u/%u, physical %u)\n",
                destinationSlot, imageCount, albumImageLimit,
                IMAGE_SLOT_COUNT);
  return true;
}

void clearImageHistory() {
  for (uint8_t slot = 0; slot < IMAGE_SLOT_COUNT; ++slot) {
    LittleFS.remove(IMAGE_SLOT_PATHS[slot]);
  }
  LittleFS.remove(LEGACY_IMAGE_PATH);
  LittleFS.remove(IMAGE_TEMP_PATH);
  LittleFS.remove(IMAGE_BACKUP_PATH);
  imageCount = 0;
  imageHeadSlot = 0;
  imageCurrentSlot = 0;
  saveImageHistory();
  lv_obj_add_flag(backgroundImage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_invalidate(backgroundImage);
  Serial.println("Image history cleared");
}
