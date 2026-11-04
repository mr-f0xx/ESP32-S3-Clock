// Internet radio player for the full-screen Radio app.
// Included once by 08_LVGL_Clock.ino inside its private namespace, after the
// audiobook module so it can share the ES8311/I2S output and audio helpers.
//
// Streams HTTP/HTTPS internet radio (MP3, or AAC when the URL contains ".aac")
// through ESP8266Audio: an ICY source -> a PSRAM ring buffer -> the shared codec
// output. ICY "StreamTitle" metadata is shown as the now-playing line. Playback
// is serviced from the main loop like music/audiobook; the large buffer keeps
// network reads off the UI path except during the initial connect/buffer fill.
// Live streams cannot be paused in place, so pause/stop both disconnect and play
// reconnects to the live point.

struct RadioStation {
  const char *name;
  const char *url;
};

// Editable preset list. Add your own stations here; a URL containing ".aac"
// selects the AAC decoder, anything else is treated as MP3.
static const RadioStation RADIO_STATIONS[] = {
  // User-requested chillhop / lo-fi defaults.
  {"FluxFM Chillhop",     "http://channels.fluxfm.de/chillhop/stream.mp3"},
  {"I Love Chillhop",     "https://streams.ilovemusic.de/iloveradio17.mp3"},
  {"ChillHop",            "http://stream01.iloveradio.de/iloveradio17.mp3"},
  {"FluxFM Chillhop 320", "https://streams.fluxfm.de/Chillhop/mp3-320/streams.fluxfm.de/"},
  {"Le Mellotron",        "https://listen.radioking.com/radio/477719/stream/534044"},
  {"Lo-Fi Hip-Hop Radio", "https://stream.zeno.fm/0r0xa792kwzuv"},
  // Original presets kept after the requested defaults.
  {"Radio Paradise",      "http://stream-uk1.radioparadise.com/mp3-128"},
  {"Groove Salad",        "https://ice5.somafm.com/groovesalad-128-mp3"},
  {"Drone Zone",          "https://ice5.somafm.com/dronezone-128-mp3"},
  {"FIP",                 "http://icecast.radiofrance.fr/fip-midfi.mp3"},
  {"France Inter",        "http://icecast.radiofrance.fr/franceinter-midfi.mp3"},
  {"France Culture",      "http://icecast.radiofrance.fr/franceculture-midfi.mp3"},
};
constexpr uint8_t RADIO_STATION_COUNT =
  sizeof(RADIO_STATIONS) / sizeof(RADIO_STATIONS[0]);
// A larger PSRAM ring buffer absorbs Wi-Fi/TLS scheduling jitter and keeps the
// decoder fed while LVGL, weather or the web server briefly owns the main loop.
constexpr uint32_t RADIO_BUFFER_BYTES = 320U * 1024U;
constexpr uint32_t RADIO_RECONNECT_DELAY_MS = 3000;
constexpr uint32_t RADIO_TITLE_WAIT_MS = 20000;

uint8_t radioStationIndex = 0;
bool radioPaused = false;
bool radioStreamActive = false;
bool radioReconnectPending = false;
UiStr radioStatusId = STR_RADIO_CHOOSE_STATION;
AudioFileSourceICYStream *radioSource = nullptr;
AudioFileSourceBuffer *radioBuffer = nullptr;
AudioGenerator *radioGenerator = nullptr;
uint8_t *radioBufferMemory = nullptr;
char radioStreamTitle[128] = "";
uint32_t radioStreamStartedAt = 0;
uint32_t radioLastReconnectAt = 0;
uint32_t radioLastTitleRefreshAt = 0;

void updateRadioPageText();
void updateRadioPlayPauseIcon();
void updateRadioTitle();
void setRadioStatus(UiStr id);
void hideRadioStationList();
void radioStationRowEvent(lv_event_t *event);

// ---- Audio callbacks (run in the main loop with the decoder) ---------------
void radioMetadataCallback(void *cbData, const char *type, bool isUnicode,
                           const char *str) {
  (void)cbData;
  (void)isUnicode;
  if (!type || !str) return;
  if (strcmp(type, "StreamTitle") == 0) {
    strlcpy(radioStreamTitle, str, sizeof(radioStreamTitle));
    updateRadioTitle();
  }
}

void radioStatusCallback(void *cbData, int code, const char *str) {
  (void)cbData;
  (void)str;
  if (code == AudioFileSourceBuffer::STATUS_FILLING ||
      code == AudioFileSourceBuffer::STATUS_UNDERFLOW) {
    if (radioStreamActive && !radioReconnectPending && !radioPaused)
      setRadioStatus(STR_RADIO_BUFFERING);
  }
}

// ---- Status / text ---------------------------------------------------------
void setRadioStatus(UiStr id) {
  radioStatusId = id;
  if (radioStatusLabel) lv_label_set_text(radioStatusLabel, tr(id));
}

void updateRadioTitle() {
  if (!radioTitleLabel) return;
  if (radioStreamTitle[0]) {
    lv_label_set_text(radioTitleLabel, radioStreamTitle);
  } else if (!radioStreamActive) {
    lv_label_set_text(radioTitleLabel, tr(STR_RADIO_CHOOSE_STATION));
  } else if (millis() - radioStreamStartedAt < RADIO_TITLE_WAIT_MS) {
    lv_label_set_text(radioTitleLabel, tr(STR_RADIO_WAITING_TITLE));
  } else {
    lv_label_set_text(radioTitleLabel, tr(STR_RADIO_NO_TITLE));
  }
}

void updateRadioPlayPauseIcon() {
  const bool playing = radioStreamActive && !radioPaused;
  if (radioPlayIcon) {
    if (playing) lv_obj_add_flag(radioPlayIcon, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(radioPlayIcon, LV_OBJ_FLAG_HIDDEN);
  }
  if (radioPauseIcon) {
    if (playing) lv_obj_clear_flag(radioPauseIcon, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(radioPauseIcon, LV_OBJ_FLAG_HIDDEN);
  }
}

void refreshRadioStationListStyles() {
  if (!radioStationListList) return;
  const uint32_t childCount = lv_obj_get_child_cnt(radioStationListList);
  for (uint32_t i = 0; i < childCount; ++i) {
    lv_obj_t *row = lv_obj_get_child(radioStationListList, i);
    const bool selected = i == radioStationIndex;
    if (themeIsAmber()) {
      lv_obj_set_style_bg_color(row,
        selected ? lv_color_hex(CRT_AMBER) : lv_color_black(), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(row, lv_color_hex(CRT_AMBER_FAINT), LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_color(row,
        lv_color_hex(selected ? UI_ACCENT_BLUE : 0x0D0D12), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_80, LV_PART_MAIN);
      lv_obj_set_style_border_color(row,
        lv_color_hex(selected ? 0x60A5FA : 0x282C37), LV_PART_MAIN);
    }
    if (lv_obj_get_child_cnt(row)) {
      lv_obj_set_style_text_color(lv_obj_get_child(row, 0),
        themeIsAmber() ? (selected ? lv_color_black() : lv_color_hex(CRT_AMBER))
                       : lv_color_white(), 0);
    }
  }
}

void updateRadioPageText() {
  if (radioStationLabel)
    lv_label_set_text(radioStationLabel, RADIO_STATIONS[radioStationIndex].name);
  if (radioStatusLabel) lv_label_set_text(radioStatusLabel, tr(radioStatusId));
  updateRadioTitle();
  updateRadioPlayPauseIcon();
  refreshRadioStationListStyles();
}

void rebuildRadioStationList() {
  if (!radioStationListList) return;
  lv_obj_clean(radioStationListList);
  for (uint8_t i = 0; i < RADIO_STATION_COUNT; ++i) {
    lv_obj_t *row = lv_btn_create(radioStationListList);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 58);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(row, radioStationRowEvent, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    char title[96];
    snprintf(title, sizeof(title), "%02u   %s", i + 1, RADIO_STATIONS[i].name);
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, title);
    lv_obj_set_width(label, 310);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, &lv_font_ui_18, 0);
    lv_obj_set_style_text_color(label, themeText(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
  }
  refreshRadioStationListStyles();
}

// ---- Playback --------------------------------------------------------------
void teardownRadioPipeline() {
  if (musicCodecReady) digitalWrite(AUDIO_PA_PIN, LOW);
  if (radioGenerator) {
    if (radioGenerator->isRunning()) radioGenerator->stop();
    delete radioGenerator;
    radioGenerator = nullptr;
  }
  if (radioBuffer) {
    delete radioBuffer;
    radioBuffer = nullptr;
  }
  if (radioSource) {
    radioSource->close();
    delete radioSource;
    radioSource = nullptr;
  }
}

void stopRadioPlayback() {
  // No-op when radio is not the active source so a cross-stop from music /
  // video / audiobook / alarm never pulls the shared PA pin low on them.
  if (!radioGenerator && !radioSource && !radioStreamActive) return;
  radioStreamActive = false;
  radioPaused = false;
  radioReconnectPending = false;
  teardownRadioPipeline();
  radioStreamTitle[0] = '\0';
  setRadioStatus(STR_RADIO_STOPPED);
  updateRadioTitle();
  updateRadioPlayPauseIcon();
}

bool startRadioStation(uint8_t index) {
  if (index >= RADIO_STATION_COUNT) return false;
  if (WiFi.status() != WL_CONNECTED) {
    setRadioStatus(STR_RADIO_NO_WIFI);
    return false;
  }
  // Modem sleep is a common cause of periodic MP3 underflow on ESP32-S3.
  WiFi.setSleep(false);
  // Mutual exclusion with every other audio producer.
  stopMusicPlayback();
  stopAudiobookPlayback();
  radioReconnectPending = false;
  teardownRadioPipeline();

  radioStationIndex = index;
  radioStreamActive = true;
  radioPaused = false;
  radioStreamTitle[0] = '\0';
  radioStreamStartedAt = millis();
  setRadioStatus(STR_RADIO_CONNECTING);
  updateRadioPageText();
  // Show "Connecting..." before the blocking HTTP request below.
  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(nullptr);

  if (!radioBufferMemory) {
    radioBufferMemory = static_cast<uint8_t *>(heap_caps_malloc(
      RADIO_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!radioBufferMemory)
      radioBufferMemory = static_cast<uint8_t *>(malloc(RADIO_BUFFER_BYTES));
  }
  if (!radioBufferMemory) {
    radioStreamActive = false;
    setRadioStatus(STR_RADIO_STREAM_ERROR);
    return false;
  }

  radioSource = new AudioFileSourceICYStream();
  radioSource->RegisterMetadataCB(radioMetadataCallback, nullptr);
  radioSource->RegisterStatusCB(radioStatusCallback, nullptr);
  // Reconnect is handled by serviceRadioPlayback() so a dead station never
  // blocks the UI inside the library's delay-based retry loop.
  radioSource->SetReconnect(0, 0);
  if (!radioSource->open(RADIO_STATIONS[index].url)) {
    teardownRadioPipeline();
    radioStreamActive = false;
    setRadioStatus(STR_RADIO_STREAM_ERROR);
    updateRadioPlayPauseIcon();
    return false;
  }
  radioBuffer = new AudioFileSourceBuffer(radioSource, radioBufferMemory,
                                          RADIO_BUFFER_BYTES);
  radioBuffer->RegisterStatusCB(radioStatusCallback, nullptr);
  if (!initializeMusicCodec()) {
    teardownRadioPipeline();
    radioStreamActive = false;
    updateRadioPlayPauseIcon();
    return false;
  }

  String url = RADIO_STATIONS[index].url;
  url.toLowerCase();
  if (url.indexOf(".aac") >= 0) radioGenerator = new AudioGeneratorAAC();
  else radioGenerator = new AudioGeneratorMP3();
  if (!radioGenerator || !radioGenerator->begin(radioBuffer, musicOutput)) {
    teardownRadioPipeline();
    radioStreamActive = false;
    setRadioStatus(STR_RADIO_STREAM_ERROR);
    updateRadioPlayPauseIcon();
    return false;
  }

  digitalWrite(AUDIO_PA_PIN, HIGH);
  applyAudioVolume();
  setRadioStatus(STR_RADIO_BUFFERING);
  updateRadioPlayPauseIcon();
  preferences.putUChar("radio_station", radioStationIndex);
  Serial.printf("Radio: streaming %s\n", RADIO_STATIONS[index].url);
  return true;
}

void serviceRadioPlayback() {
  if (lowPowerMode) return;
  if (radioReconnectPending) {
    if (millis() - radioLastReconnectAt >= RADIO_RECONNECT_DELAY_MS) {
      radioReconnectPending = false;
      if (radioStreamActive && !radioPaused) startRadioStation(radioStationIndex);
    }
    return;
  }
  if (!radioGenerator || !radioGenerator->isRunning()) return;
  if (radioGenerator->loop()) {
    if (radioStatusId != STR_PLAYING) setRadioStatus(STR_PLAYING);
    if (millis() - radioLastTitleRefreshAt >= 1000) {
      radioLastTitleRefreshAt = millis();
      updateRadioTitle();
    }
    return;
  }
  // A live stream that stops producing data has dropped: reconnect shortly.
  teardownRadioPipeline();
  if (radioStreamActive && !radioPaused) {
    radioReconnectPending = true;
    radioLastReconnectAt = millis();
    setRadioStatus(STR_RADIO_RECONNECTING);
  }
  updateRadioPlayPauseIcon();
}

void selectAdjacentRadioStation(int8_t direction) {
  if (!direction || lowPowerMode) return;
  markUserActivity();
  const int next = (static_cast<int>(radioStationIndex) + RADIO_STATION_COUNT +
                    (direction > 0 ? 1 : -1)) % RADIO_STATION_COUNT;
  startRadioStation(static_cast<uint8_t>(next));
}

void toggleRadioPlayPause() {
  markUserActivity();
  if (radioStreamActive && !radioPaused) {
    // Pause a live stream by disconnecting; play reconnects to the live point.
    radioPaused = true;
    radioReconnectPending = false;
    teardownRadioPipeline();
    setRadioStatus(STR_PAUSED);
    updateRadioPlayPauseIcon();
    return;
  }
  startRadioStation(radioStationIndex);
}

void stopRadioFromButton() {
  markUserActivity();
  stopRadioPlayback();
}

bool pauseRadioForControlPanel() {
  // Keep the stream playing while the quick settings panel is open so the
  // volume slider can be adjusted by ear. Nothing to suspend.
  return false;
}

void resumeRadioAfterControlPanel(bool wasPlaying) {
  (void)wasPlaying;
}

void suspendRadioForLowPower() {
  hideRadioStationList();
  stopRadioPlayback();
  if (radioBufferMemory) {
    free(radioBufferMemory);
    radioBufferMemory = nullptr;
  }
}

// ---- Station list overlay --------------------------------------------------
void hideRadioStationList() {
  radioStationListVisible = false;
  if (radioStationListPanel)
    lv_obj_add_flag(radioStationListPanel, LV_OBJ_FLAG_HIDDEN);
}

void showRadioStationList() {
  if (!radioStationListPanel || !radioStationListList) return;
  radioStationListVisible = true;
  refreshRadioStationListStyles();
  lv_obj_clear_flag(radioStationListPanel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(radioStationListPanel);
  if (radioStationIndex < lv_obj_get_child_cnt(radioStationListList))
    lv_obj_scroll_to_view(
      lv_obj_get_child(radioStationListList, radioStationIndex), LV_ANIM_OFF);
}

void toggleRadioStationList() {
  markUserActivity();
  if (radioStationListVisible) hideRadioStationList();
  else showRadioStationList();
}

// ---- Page show/hide --------------------------------------------------------
void showRadioPage(bool fade) {
  if (!radioPage || lowPowerMode || radioPageVisible || radioPageShowRequested)
    return;
  if (fade) {
    radioPageHideRequested = false;
    radioPageShowRequested = true;
    return;
  }
  radioPageVisible = true;
  lv_obj_clear_flag(radioPage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(radioPage);
  updateRadioPageText();
}

void hideRadioPage(bool fade, bool returnToMenu) {
  if (!radioPage) return;
  if (!radioPageVisible && lv_obj_has_flag(radioPage, LV_OBJ_FLAG_HIDDEN) &&
      !radioPageShowRequested && !radioPageHideRequested) return;
  if (fade) {
    radioPageShowRequested = false;
    radioPageHideRequested = true;
    return;
  }
  // The stream keeps playing in the background, like music and audiobooks.
  radioPageVisible = false;
  hideRadioStationList();
  radioPageShowRequested = false;
  radioPageHideRequested = false;
  lv_obj_add_flag(radioPage, LV_OBJ_FLAG_HIDDEN);
  if (returnToMenu && !lowPowerMode) showAppMenu(false);
}

void commitRadioFullScreen() {
  lv_obj_invalidate(lv_scr_act());
  serviceRadioPlayback();
  lv_refr_now(nullptr);
  serviceRadioPlayback();
}

void processRadioPageTransition() {
  if (lowPowerMode || screenFadeTransitionActive) return;
  if (radioPageShowRequested) {
    radioPageShowRequested = false;
    beginScreenFade(activeBrightness);
    hideAppMenu(false);
    showRadioPage(false);
    setMusicWholeScreenRefresh(true);
    commitRadioFullScreen();
    endScreenFade(activeBrightness);
  } else if (radioPageHideRequested) {
    radioPageHideRequested = false;
    beginScreenFade(activeBrightness);
    hideRadioPage(false, true);
    commitRadioFullScreen();
    setMusicWholeScreenRefresh(false);
    endScreenFade(activeBrightness);
  }
}

// ---- Events ----------------------------------------------------------------
void radioPlayPauseEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) toggleRadioPlayPause();
}

void radioPreviousEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) selectAdjacentRadioStation(-1);
}

void radioNextEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) selectAdjacentRadioStation(1);
}

void radioStopEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) stopRadioFromButton();
}

void radioStationListEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) toggleRadioStationList();
}

void radioStationRowEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  const uint8_t index = static_cast<uint8_t>(
    reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  if (index < RADIO_STATION_COUNT && startRadioStation(index))
    hideRadioStationList();
}

// ---- UI construction -------------------------------------------------------
lv_obj_t *createRadioControlButton(lv_obj_t *parent, lv_coord_t size,
                                   lv_coord_t x, lv_coord_t y,
                                   lv_event_cb_t callback, TablerIconKind iconKind,
                                   lv_coord_t iconSize) {
  lv_obj_t *button = lv_btn_create(parent);
  lv_obj_set_size(button, size, size);
  lv_obj_align(button, LV_ALIGN_TOP_MID, x, y);
  lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(themeSurface(0x0A0A0A)), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(button, lv_color_hex(themeBorder(0x343846)), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(themeAccent(UI_ACCENT_BLUE)),
                            LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
  createTablerIcon(button, iconKind, iconSize, themeIcon());
  return button;
}

void createRadioStationListPanel() {
  radioStationListPanel = lv_obj_create(radioPage);
  lv_obj_remove_style_all(radioStationListPanel);
  lv_obj_set_size(radioStationListPanel, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_pos(radioStationListPanel, 0, 0);
  lv_obj_set_style_bg_color(radioStationListPanel, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(radioStationListPanel, LV_OPA_COVER, 0);
  lv_obj_add_flag(radioStationListPanel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(radioStationListPanel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(radioStationListPanel, appPageGestureEvent,
                      LV_EVENT_GESTURE, nullptr);

  lv_obj_t *title = lv_label_create(radioStationListPanel);
  i18nRegister(title, STR_RADIO_STATIONS);
  lv_obj_set_style_text_font(title, &lv_font_ui_24, 0);
  lv_obj_set_style_text_letter_space(title, 3, 0);
  lv_obj_set_style_text_color(title, themeText(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);

  radioStationListList = lv_obj_create(radioStationListPanel);
  lv_obj_set_pos(radioStationListList, 24, 75);
  lv_obj_set_size(radioStationListList, SCREEN_WIDTH - 48, SCREEN_HEIGHT - 94);
  lv_obj_set_flex_flow(radioStationListList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(radioStationListList, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(radioStationListList, 0, 0);
  lv_obj_set_style_pad_row(radioStationListList, 10, 0);
  lv_obj_set_style_bg_opa(radioStationListList, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(radioStationListList, 0, 0);
  lv_obj_set_style_width(radioStationListList, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(radioStationListList,
                            lv_color_hex(themeAccent(UI_ACCENT_BLUE)),
                            LV_PART_SCROLLBAR);
  lv_obj_add_flag(radioStationListList, LV_OBJ_FLAG_GESTURE_BUBBLE);

  rebuildRadioStationList();
  lv_obj_add_flag(radioStationListPanel, LV_OBJ_FLAG_HIDDEN);
}

void createRadioPage(lv_obj_t *screen) {
  radioPage = lv_obj_create(screen);
  lv_obj_remove_style_all(radioPage);
  lv_obj_set_pos(radioPage, 0, 0);
  lv_obj_set_size(radioPage, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(radioPage, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(radioPage, LV_OPA_COVER, 0);
  lv_obj_clear_flag(radioPage, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(radioPage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(radioPage, appPageGestureEvent, LV_EVENT_GESTURE, nullptr);

  radioStationLabel = lv_label_create(radioPage);
  lv_obj_set_size(radioStationLabel, SCREEN_WIDTH - 64, 44);
  lv_label_set_long_mode(radioStationLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(radioStationLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(radioStationLabel, &lv_font_ui_32, 0);
  lv_obj_set_style_text_color(radioStationLabel, themeText(), 0);
  lv_obj_align(radioStationLabel, LV_ALIGN_TOP_MID, 0, 34);

  radioNowPlayingLabel = lv_label_create(radioPage);
  i18nRegister(radioNowPlayingLabel, STR_RADIO_NOW_PLAYING);
  lv_obj_set_style_text_font(radioNowPlayingLabel, &lv_font_ui_16, 0);
  lv_obj_set_style_text_letter_space(radioNowPlayingLabel, 3, 0);
  lv_obj_set_style_text_color(radioNowPlayingLabel, themeTextDim(0x777D8D), 0);
  lv_obj_align(radioNowPlayingLabel, LV_ALIGN_TOP_MID, 0, 96);

  radioTitleLabel = lv_label_create(radioPage);
  lv_obj_set_size(radioTitleLabel, SCREEN_WIDTH - 64, 34);
  lv_label_set_long_mode(radioTitleLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_align(radioTitleLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(radioTitleLabel, &lv_font_ui_24, 0);
  lv_obj_set_style_text_color(radioTitleLabel,
                              lv_color_hex(themeAccent(UI_ACCENT_BLUE)), 0);
  lv_obj_align(radioTitleLabel, LV_ALIGN_TOP_MID, 0, 128);

  radioStatusLabel = lv_label_create(radioPage);
  lv_obj_set_size(radioStatusLabel, SCREEN_WIDTH - 80, 30);
  lv_label_set_long_mode(radioStatusLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(radioStatusLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(radioStatusLabel, &lv_font_ui_18, 0);
  lv_obj_set_style_text_color(radioStatusLabel, themeTextDim(0xB8C0CC), 0);
  lv_obj_align(radioStatusLabel, LV_ALIGN_TOP_MID, 0, 182);

  createRadioControlButton(radioPage, 58, -176, 300, radioStationListEvent,
                           TABLER_ICON_PLAYLIST, 28);
  createRadioControlButton(radioPage, 58, -88, 300, radioPreviousEvent,
                           TABLER_ICON_SKIP_BACK, 28);
  lv_obj_t *playButton = createRadioControlButton(
    radioPage, 74, 0, 292, radioPlayPauseEvent, TABLER_ICON_PLAY, 32);
  lv_obj_set_style_bg_color(playButton, lv_color_hex(themeAccent(UI_ACCENT_BLUE)),
                            LV_PART_MAIN);
  lv_obj_set_style_border_width(playButton, themeIsAmber() ? 2 : 0, LV_PART_MAIN);
  lv_obj_set_style_border_color(playButton, lv_color_hex(CRT_AMBER), LV_PART_MAIN);
  radioPlayIcon = lv_obj_get_child(playButton, lv_obj_get_child_cnt(playButton) - 1);
  radioPauseIcon = createTablerIcon(playButton, TABLER_ICON_PAUSE, 32,
    themeIsAmber() ? lv_color_black() : lv_color_white());
  if (radioPlayIcon)
    setTablerIconColor(radioPlayIcon,
      themeIsAmber() ? lv_color_black() : lv_color_white());
  lv_obj_add_flag(radioPauseIcon, LV_OBJ_FLAG_HIDDEN);
  createRadioControlButton(radioPage, 58, 88, 300, radioNextEvent,
                           TABLER_ICON_SKIP_FORWARD, 28);
  createRadioControlButton(radioPage, 58, 176, 300, radioStopEvent,
                           TABLER_ICON_STOP, 26);

  radioHintLabel = lv_label_create(radioPage);
  i18nRegister(radioHintLabel, STR_RADIO);
  lv_obj_set_style_text_font(radioHintLabel, &lv_font_ui_16, 0);
  lv_obj_set_style_text_color(radioHintLabel, themeTextDim(0x777D8D), 0);
  lv_obj_align(radioHintLabel, LV_ALIGN_BOTTOM_MID, 0, -30);

  setRadioStatus(STR_RADIO_CHOOSE_STATION);
  createRadioStationListPanel();
  updateRadioPageText();
  lv_obj_add_flag(radioPage, LV_OBJ_FLAG_HIDDEN);
}

// Re-apply the active theme to the radio player and its station list.
void refreshRadioTheme() {
  if (!radioPage) return;
  lv_obj_set_style_bg_color(radioPage, themePageBackground(), 0);
  if (radioStationLabel)
    lv_obj_set_style_text_color(radioStationLabel, themeText(), 0);
  if (radioNowPlayingLabel)
    lv_obj_set_style_text_color(radioNowPlayingLabel, themeTextDim(0x777D8D), 0);
  if (radioTitleLabel)
    lv_obj_set_style_text_color(radioTitleLabel,
      lv_color_hex(themeAccent(UI_ACCENT_BLUE)), 0);
  if (radioStatusLabel)
    lv_obj_set_style_text_color(radioStatusLabel, themeTextDim(0xB8C0CC), 0);
  if (radioHintLabel)
    lv_obj_set_style_text_color(radioHintLabel, themeTextDim(0x777D8D), 0);

  const uint32_t childCount = lv_obj_get_child_cnt(radioPage);
  for (uint32_t i = 0; i < childCount; ++i) {
    lv_obj_t *child = lv_obj_get_child(radioPage, i);
    if (!lv_obj_check_type(child, &lv_btn_class)) continue;
    const bool isPlayButton = lv_obj_get_width(child) == 74 &&
                              lv_obj_get_height(child) == 74;
    if (isPlayButton) {
      lv_obj_set_style_bg_color(child, lv_color_hex(themeAccent(UI_ACCENT_BLUE)),
                                LV_PART_MAIN);
      lv_obj_set_style_border_width(child, themeIsAmber() ? 2 : 0, LV_PART_MAIN);
      lv_obj_set_style_border_color(child, lv_color_hex(CRT_AMBER), LV_PART_MAIN);
      for (uint32_t j = 0; j < lv_obj_get_child_cnt(child); ++j)
        setTablerIconColor(lv_obj_get_child(child, j),
          themeIsAmber() ? lv_color_black() : lv_color_white());
    } else {
      lv_obj_set_style_bg_color(child, lv_color_hex(themeSurface(0x0A0A0A)), LV_PART_MAIN);
      lv_obj_set_style_border_color(child, lv_color_hex(themeBorder(0x343846)), LV_PART_MAIN);
      lv_obj_set_style_bg_color(child, lv_color_hex(themeAccent(UI_ACCENT_BLUE)),
                                LV_PART_MAIN | LV_STATE_PRESSED);
      for (uint32_t j = 0; j < lv_obj_get_child_cnt(child); ++j)
        setTablerIconColor(lv_obj_get_child(child, j), themeIcon());
    }
  }
  if (radioStationListPanel) {
    lv_obj_set_style_bg_color(radioStationListPanel, themePageBackground(), 0);
    if (lv_obj_get_child_cnt(radioStationListPanel))
      lv_obj_set_style_text_color(lv_obj_get_child(radioStationListPanel, 0),
                                  themeText(), 0);
    if (radioStationListList)
      lv_obj_set_style_bg_color(radioStationListList,
        lv_color_hex(themeAccent(UI_ACCENT_BLUE)), LV_PART_SCROLLBAR);
  }
  refreshRadioStationListStyles();
}
