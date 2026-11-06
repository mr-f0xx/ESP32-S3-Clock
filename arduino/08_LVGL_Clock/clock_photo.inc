// Photo frame app: displays JPG pictures from the SD card (/photos or root).
// Included once by 08_LVGL_Clock.ino inside its private namespace, after the
// world-clock module. Decoding uses the vendored ESP_New_JPEG hardware decoder.

constexpr uint8_t PHOTO_TRACK_LIMIT = 80;
constexpr uint32_t PHOTO_MAX_JPEG_BYTES = 4UL * 1024UL * 1024UL;
constexpr uint32_t PHOTO_SLIDESHOW_INTERVAL_MS = 15000;

String photoPaths[PHOTO_TRACK_LIMIT];
uint8_t photoCount = 0;
uint8_t photoIndex = 0;
bool photoSlideshowEnabled = true;
bool photoFrameScanned = false;
uint32_t photoLastAdvanceAt = 0;
uint32_t photoLastScanAt = 0;
lv_obj_t *photoFramePage = nullptr;
lv_obj_t *photoImage = nullptr;
lv_obj_t *photoStatusLabel = nullptr;
lv_obj_t *photoCountLabel = nullptr;
lv_obj_t *photoPlayIcon = nullptr;
lv_obj_t *photoPauseIcon = nullptr;
uint8_t *photoPixels = nullptr;
lv_img_dsc_t photoDescriptor;

void refreshPhotoFrameTheme();
void updatePhotoFrameStatus();

bool photoSdReady() {
  return weatherWallpaperSdReadyForOtherUsers() && mountMusicSd();
}

bool isPhotoFile(const String &path) {
  String lower = path;
  lower.toLowerCase();
  return lower.endsWith(".jpg") || lower.endsWith(".jpeg");
}

void scanPhotoFiles() {
  photoCount = 0;
  photoFrameScanned = false;
  photoLastScanAt = millis();
  if (!photoSdReady()) return;
  const char *scanPath = storageFs().exists("/photos") ? "/photos" : "/";
  File directory = storageFs().open(scanPath);
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return;
  }
  File entry;
  while (photoCount < PHOTO_TRACK_LIMIT && (entry = directory.openNextFile())) {
    if (!entry.isDirectory()) {
      const char *entryPath = entry.path();
      String path = entryPath ? entryPath : entry.name();
      if (!path.startsWith("/")) {
        if (strcmp(scanPath, "/") == 0) path = "/" + path;
        else path = String(scanPath) + "/" + path;
      }
      if (isPhotoFile(path)) photoPaths[photoCount++] = path;
    }
    entry.close();
  }
  directory.close();
  for (uint8_t i = 0; i < photoCount; ++i)
    for (uint8_t j = i + 1; j < photoCount; ++j)
      if (photoPaths[j].compareTo(photoPaths[i]) < 0) {
        String temporary = photoPaths[i];
        photoPaths[i] = photoPaths[j];
        photoPaths[j] = temporary;
      }
  photoFrameScanned = true;
  if (photoIndex >= photoCount) photoIndex = 0;
  Serial.printf("Photo frame: found %u picture(s) in %s\n", photoCount, scanPath);
}

bool ensurePhotoBuffer() {
  if (photoPixels) return true;
  photoPixels = static_cast<uint8_t *>(heap_caps_malloc(
    IMAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!photoPixels) photoPixels = static_cast<uint8_t *>(malloc(IMAGE_BYTES));
  if (!photoPixels) return false;
  photoDescriptor.header.always_zero = 0;
  photoDescriptor.header.w = SCREEN_WIDTH;
  photoDescriptor.header.h = SCREEN_HEIGHT;
  photoDescriptor.header.cf = LV_IMG_CF_TRUE_COLOR;
  photoDescriptor.data_size = IMAGE_BYTES;
  photoDescriptor.data = photoPixels;
  return true;
}

// Decode a JPEG into the 480x480 frame buffer using a center square crop.
bool loadPhotoToFrame(const String &path) {
  if (!ensurePhotoBuffer()) return false;
  File file = storageFs().open(path, FILE_READ);
  if (!file) return false;
  const size_t fileSize = file.size();
  if (!fileSize || fileSize > PHOTO_MAX_JPEG_BYTES) {
    file.close();
    return false;
  }
  uint8_t *input = static_cast<uint8_t *>(heap_caps_malloc(
    fileSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!input) input = static_cast<uint8_t *>(malloc(fileSize));
  if (!input) {
    file.close();
    return false;
  }
  const size_t readBytes = file.read(input, fileSize);
  file.close();
  if (readBytes != fileSize) {
    free(input);
    return false;
  }

  jpeg_dec_handle_t decoder = nullptr;
  jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
  config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
  config.rotate = JPEG_ROTATE_0D;
  config.block_enable = false;
  if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK || !decoder) {
    free(input);
    return false;
  }
  jpeg_dec_io_t io {};
  io.inbuf = input;
  io.inbuf_len = static_cast<int>(fileSize);
  jpeg_dec_header_info_t info {};
  if (jpeg_dec_parse_header(decoder, &io, &info) != JPEG_ERR_OK ||
      !info.width || !info.height) {
    jpeg_dec_close(decoder);
    free(input);
    return false;
  }

  // Pick a hardware downscale so the decoded image stays within a reasonable
  // PSRAM footprint, then crop/resize the center square in software.
  int divisor = 1;
  const int maxSide = max(info.width, info.height);
  if (maxSide > 3840) divisor = 8;
  else if (maxSide > 1920) divisor = 4;
  else if (maxSide > 960) divisor = 2;
  int scaledWidth = info.width;
  int scaledHeight = info.height;
  if (divisor > 1) {
    scaledWidth = max(8, (info.width / divisor) & ~7);
    scaledHeight = max(8, (info.height / divisor) & ~7);
    jpeg_dec_close(decoder);
    decoder = nullptr;
    config.scale.width = scaledWidth;
    config.scale.height = scaledHeight;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK || !decoder) {
      free(input);
      return false;
    }
    io.inbuf = input;
    io.inbuf_len = static_cast<int>(fileSize);
    if (jpeg_dec_parse_header(decoder, &io, &info) != JPEG_ERR_OK) {
      jpeg_dec_close(decoder);
      free(input);
      return false;
    }
  }

  int outLen = 0;
  if (jpeg_dec_get_outbuf_len(decoder, &outLen) != JPEG_ERR_OK || outLen <= 0) {
    jpeg_dec_close(decoder);
    free(input);
    return false;
  }
  uint8_t *output = static_cast<uint8_t *>(heap_caps_aligned_alloc(
    16, outLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!output) output = static_cast<uint8_t *>(heap_caps_aligned_alloc(
    16, outLen, MALLOC_CAP_8BIT));
  if (!output) {
    jpeg_dec_close(decoder);
    free(input);
    return false;
  }
  io.outbuf = output;
  io.out_size = outLen;
  const bool decoded = jpeg_dec_process(decoder, &io) == JPEG_ERR_OK;
  jpeg_dec_close(decoder);
  if (!decoded) {
    free(output);
    free(input);
    return false;
  }

  const uint16_t *src = reinterpret_cast<const uint16_t *>(output);
  uint16_t *dst = reinterpret_cast<uint16_t *>(photoPixels);
  const int crop = min(scaledWidth, scaledHeight);
  const int offsetX = (scaledWidth - crop) / 2;
  const int offsetY = (scaledHeight - crop) / 2;
  for (int y = 0; y < SCREEN_HEIGHT; ++y) {
    const int srcY = offsetY + (static_cast<int>(y) * crop) / SCREEN_HEIGHT;
    for (int x = 0; x < SCREEN_WIDTH; ++x) {
      const int srcX = offsetX + (static_cast<int>(x) * crop) / SCREEN_WIDTH;
      dst[y * SCREEN_WIDTH + x] = src[srcY * scaledWidth + srcX];
    }
  }
  free(output);
  free(input);
  lv_img_cache_invalidate_src(&photoDescriptor);
  lv_img_set_src(photoImage, &photoDescriptor);
  lv_obj_clear_flag(photoImage, LV_OBJ_FLAG_HIDDEN);
  return true;
}

void updatePhotoFrameStatus() {
  if (photoCountLabel) {
    char text[24];
    snprintf(text, sizeof(text), "%u / %u", photoCount ? photoIndex + 1 : 0, photoCount);
    lv_label_set_text(photoCountLabel, text);
  }
  if (photoPlayIcon && photoPauseIcon) {
    if (photoSlideshowEnabled) {
      lv_obj_add_flag(photoPlayIcon, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(photoPauseIcon, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(photoPlayIcon, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(photoPauseIcon, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

bool showPhotoIndex(uint8_t index) {
  if (!photoCount) return false;
  photoIndex = index % photoCount;
  if (photoStatusLabel) lv_label_set_text(photoStatusLabel, tr(STR_PHOTO_FRAME_LOADING));
  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(nullptr);
  const bool ok = loadPhotoToFrame(photoPaths[photoIndex]);
  if (photoStatusLabel)
    lv_label_set_text(photoStatusLabel, ok ? "" : tr(STR_PHOTO_FRAME_NO_PHOTOS));
  updatePhotoFrameStatus();
  photoLastAdvanceAt = millis();
  return ok;
}

void selectAdjacentPhoto(int8_t direction) {
  if (!direction || lowPowerMode) return;
  markUserActivity();
  if (!photoFrameScanned) scanPhotoFiles();
  if (!photoCount) {
    updatePhotoFrameStatus();
    return;
  }
  const int next = (static_cast<int>(photoIndex) + photoCount +
                    (direction > 0 ? 1 : -1)) % photoCount;
  showPhotoIndex(static_cast<uint8_t>(next));
}

void servicePhotoFrame() {
  if (!photoFramePageVisible || lowPowerMode) return;
  if (!photoFrameScanned) {
    if (millis() - photoLastScanAt < 2000) return;
    scanPhotoFiles();
    if (photoCount) showPhotoIndex(photoIndex);
    else if (photoStatusLabel)
      lv_label_set_text(photoStatusLabel,
        photoSdReady() ? tr(STR_PHOTO_FRAME_NO_PHOTOS) : tr(STR_PHOTO_FRAME_NO_SD));
    updatePhotoFrameStatus();
    return;
  }
  if (photoSlideshowEnabled && photoCount > 1 &&
      millis() - photoLastAdvanceAt >= PHOTO_SLIDESHOW_INTERVAL_MS) {
    selectAdjacentPhoto(1);
  }
}

void photoPreviousEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) selectAdjacentPhoto(-1);
}
void photoNextEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) selectAdjacentPhoto(1);
}
void photoSlideshowEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  photoSlideshowEnabled = !photoSlideshowEnabled;
  photoLastAdvanceAt = millis();
  updatePhotoFrameStatus();
}
void photoBackEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) hidePhotoFramePage(true, true);
}

void showPhotoFramePage(bool fade) {
  if (lowPowerMode || photoFramePageVisible) return;
  if (fade) {
    beginScreenFade(activeBrightness);
    hideAppMenu(false);
    showPhotoFramePage(false);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
    return;
  }
  if (!photoFramePage) createPhotoFramePage(lv_scr_act());
  if (!photoFramePage) return;
  photoFramePageVisible = true;
  lv_obj_clear_flag(photoFramePage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(photoFramePage);
  if (!photoFrameScanned) scanPhotoFiles();
  if (photoCount) showPhotoIndex(photoIndex);
  else if (photoStatusLabel)
    lv_label_set_text(photoStatusLabel,
      photoSdReady() ? tr(STR_PHOTO_FRAME_NO_PHOTOS) : tr(STR_PHOTO_FRAME_NO_SD));
  updatePhotoFrameStatus();
}

void hidePhotoFramePage(bool fade, bool returnToMenu) {
  if (!photoFramePage) return;
  if (!photoFramePageVisible && lv_obj_has_flag(photoFramePage, LV_OBJ_FLAG_HIDDEN)) return;
  if (fade) {
    beginScreenFade(activeBrightness);
    hidePhotoFramePage(false, returnToMenu);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
    return;
  }
  photoFramePageVisible = false;
  lv_obj_add_flag(photoFramePage, LV_OBJ_FLAG_HIDDEN);
  if (returnToMenu && !lowPowerMode) showAppMenu(false);
}

void suspendPhotoFrameForLowPower() {
  hidePhotoFramePage(false, false);
  photoSlideshowEnabled = false;
}

void createPhotoFramePage(lv_obj_t *screen) {
  photoFramePage = lv_obj_create(screen);
  lv_obj_remove_style_all(photoFramePage);
  lv_obj_set_pos(photoFramePage, 0, 0);
  lv_obj_set_size(photoFramePage, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(photoFramePage, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(photoFramePage, LV_OPA_COVER, 0);
  lv_obj_clear_flag(photoFramePage, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(photoFramePage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(photoFramePage, appPageGestureEvent, LV_EVENT_GESTURE, nullptr);

  photoImage = lv_img_create(photoFramePage);
  lv_obj_center(photoImage);
  lv_obj_add_flag(photoImage, LV_OBJ_FLAG_HIDDEN);

  photoStatusLabel = lv_label_create(photoFramePage);
  lv_obj_set_size(photoStatusLabel, SCREEN_WIDTH - 80, 34);
  lv_label_set_long_mode(photoStatusLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(photoStatusLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(photoStatusLabel, &lv_font_ui_20, 0);
  lv_obj_set_style_text_color(photoStatusLabel, themeText(), 0);
  lv_obj_set_style_bg_color(photoStatusLabel, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(photoStatusLabel, LV_OPA_60, 0);
  lv_obj_set_style_radius(photoStatusLabel, 12, 0);
  lv_obj_align(photoStatusLabel, LV_ALIGN_TOP_MID, 0, 70);

  photoCountLabel = lv_label_create(photoFramePage);
  lv_obj_set_style_text_font(photoCountLabel, &lv_font_ui_18, 0);
  lv_obj_set_style_text_color(photoCountLabel, themeText(), 0);
  lv_obj_set_style_bg_color(photoCountLabel, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(photoCountLabel, LV_OPA_60, 0);
  lv_obj_set_style_radius(photoCountLabel, 10, 0);
  lv_obj_align(photoCountLabel, LV_ALIGN_TOP_RIGHT, -20, 24);

  createAppBackButton(photoFramePage, photoBackEvent, UI_ACCENT_VIOLET);
  createRadioControlButton(photoFramePage, 58, -110, 392, photoPreviousEvent,
                           TABLER_ICON_SKIP_BACK, 28);
  lv_obj_t *playButton = createRadioControlButton(photoFramePage, 74, 0, 384,
                                                  photoSlideshowEvent, TABLER_ICON_PLAY, 32);
  photoPlayIcon = lv_obj_get_child(playButton, lv_obj_get_child_cnt(playButton) - 1);
  photoPauseIcon = createTablerIcon(playButton, TABLER_ICON_PAUSE, 32, lv_color_white());
  lv_obj_add_flag(photoPauseIcon, LV_OBJ_FLAG_HIDDEN);
  createRadioControlButton(photoFramePage, 58, 110, 392, photoNextEvent,
                           TABLER_ICON_SKIP_FORWARD, 28);

  lv_obj_t *hint = lv_label_create(photoFramePage);
  i18nRegister(hint, STR_PHOTO_FRAME_HINT);
  lv_obj_set_style_text_font(hint, &lv_font_ui_16, 0);
  lv_obj_set_style_text_color(hint, themeText(), 0);
  lv_obj_set_style_bg_color(hint, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(hint, LV_OPA_60, 0);
  lv_obj_set_style_radius(hint, 10, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);

  updatePhotoFrameStatus();
  lv_obj_add_flag(photoFramePage, LV_OBJ_FLAG_HIDDEN);
}

void refreshPhotoFrameTheme() {
  if (!photoFramePage) return;
  // Media keeps a neutral black backdrop for image contrast; overlay text still
  // follows the selected theme (phosphor amber / neon-tinted white).
  lv_obj_t *overlayLabels[] = {photoStatusLabel, photoCountLabel};
  for (lv_obj_t *label : overlayLabels)
    if (label) lv_obj_set_style_text_color(label, themeText(), 0);
  const uint32_t count = lv_obj_get_child_cnt(photoFramePage);
  for (uint32_t i = 0; i < count; ++i) {
    lv_obj_t *child = lv_obj_get_child(photoFramePage, i);
    if (!lv_obj_check_type(child, &lv_btn_class)) continue;
    lv_obj_set_style_bg_color(child, lv_color_hex(themeSurface(0x0A0A0A)), LV_PART_MAIN);
    lv_obj_set_style_border_color(child, lv_color_hex(themeBorder(0x343846)), LV_PART_MAIN);
    lv_obj_set_style_bg_color(child, lv_color_hex(themeAccent(UI_ACCENT_VIOLET)),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    for (uint32_t j = 0; j < lv_obj_get_child_cnt(child); ++j)
      setTablerIconColor(lv_obj_get_child(child, j), themeIcon());
  }
  updatePhotoFrameStatus();
}
