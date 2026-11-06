// SD-card audiobook player for the full-screen Audiobooks app.
// Included once by 08_LVGL_Clock.ino inside its private namespace, after the
// music module so it can share the ES8311/I2S output, SD mount and WAV decoder.
//
// Audiobooks are MP3/WAV files in /audiobooks on the SD card (one file per
// book or chapter). Each file keeps its own resume position in NVS so playback
// continues where it left off, even across standby and reboot. Files play in
// order and auto-advance, so a chapter-split book resumes and flows naturally.

constexpr uint8_t AUDIOBOOK_LIMIT = 64;
constexpr uint32_t AUDIOBOOK_SEEK_STEP_MS = 30000;
constexpr uint32_t AUDIOBOOK_RESUME_SAVE_INTERVAL_MS = 10000;

String audiobookTracks[AUDIOBOOK_LIMIT];
uint8_t audiobookTrackCount = 0;
uint8_t audiobookTrackIndex = 0;
bool audiobookPaused = false;
bool audiobookIsMp3 = false;
ClockSdFileSource *audiobookSource = nullptr;
AudioGenerator *audiobookGenerator = nullptr;
uint32_t audiobookLastSaveAt = 0;
uint32_t audiobookProgressLastUpdateAt = 0;
UiStr audiobookStatusId = STR_INSERT_SD;
String audiobookSuspendedPath;

// MP3 decoder subclass that exposes the frame bitrate so the player can
// estimate duration and seek by byte offset (libmad re-syncs on the next frame).
class ClockAudiobookMp3 : public AudioGeneratorMP3 {
 public:
  uint32_t bitrate() const {
    return madInitted && frame ? frame->header.bitrate : 0;
  }
  void forceResync() { desync(); }
};
ClockAudiobookMp3 *audiobookMp3 = nullptr;
ClockVideoWavGenerator *audiobookWav = nullptr;

void updateAudiobookProgress(bool force = false);
void updateAudiobookPlayPauseIcon();
void audiobookBookRowEvent(lv_event_t *event);

// ---- Resume persistence ----------------------------------------------------
uint32_t audiobookPathHash(const String &path) {
  uint32_t hash = 2166136261UL;
  for (size_t i = 0; i < path.length(); ++i) {
    hash ^= static_cast<uint8_t>(path[i]);
    hash *= 16777619UL;
  }
  return hash;
}

void audiobookResumeKey(const String &path, char *key, size_t keySize) {
  snprintf(key, keySize, "ab%08lx",
           static_cast<unsigned long>(audiobookPathHash(path)));
}

uint32_t audiobookLoadResume(const String &path) {
  char key[12];
  audiobookResumeKey(path, key, sizeof(key));
  return preferences.getUInt(key, 0);
}

void audiobookSaveResume(uint32_t bytes) {
  if (!audiobookTrackCount || audiobookTrackIndex >= audiobookTrackCount) return;
  char key[12];
  audiobookResumeKey(audiobookTracks[audiobookTrackIndex], key, sizeof(key));
  preferences.putUInt(key, bytes);
}

// ---- Position / duration ---------------------------------------------------
uint32_t audiobookPositionMs() {
  if (!audiobookSource || !audiobookSource->isOpen()) return 0;
  if (audiobookIsMp3) {
    if (!audiobookMp3) return 0;
    const uint32_t bitrate = audiobookMp3->bitrate();
    if (!bitrate) return 0;
    return static_cast<uint32_t>(
      (static_cast<uint64_t>(audiobookSource->getPos()) * 8000ULL) / bitrate);
  }
  if (!audiobookWav) return 0;
  const uint32_t rate = audiobookWav->sampleRateHz();
  const uint32_t blockBytes =
    audiobookWav->channelCount() * (audiobookWav->sampleBits() / 8U);
  const uint64_t bytesPerSecond = static_cast<uint64_t>(rate) * blockBytes;
  if (!bytesPerSecond) return 0;
  const uint32_t dataOffset = audiobookWav->pcmDataOffset();
  const uint32_t pos = audiobookSource->getPos();
  const uint32_t consumed = pos > dataOffset ? pos - dataOffset : 0;
  return static_cast<uint32_t>((static_cast<uint64_t>(consumed) * 1000ULL) /
                               bytesPerSecond);
}

uint32_t audiobookDurationMs() {
  if (!audiobookSource || !audiobookSource->isOpen()) return 0;
  if (audiobookIsMp3) {
    if (!audiobookMp3) return 0;
    const uint32_t bitrate = audiobookMp3->bitrate();
    if (!bitrate) return 0;
    return static_cast<uint32_t>(
      (static_cast<uint64_t>(audiobookSource->getSize()) * 8000ULL) / bitrate);
  }
  return audiobookWav ? audiobookWav->durationMs() : 0;
}

void formatAudiobookClock(char *text, size_t textSize, uint32_t totalSeconds) {
  const uint32_t hours = totalSeconds / 3600U;
  const uint32_t minutes = (totalSeconds / 60U) % 60U;
  const uint32_t seconds = totalSeconds % 60U;
  if (hours) {
    snprintf(text, textSize, "%lu:%02lu:%02lu",
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes),
             static_cast<unsigned long>(seconds));
  } else {
    snprintf(text, textSize, "%lu:%02lu",
             static_cast<unsigned long>(minutes),
             static_cast<unsigned long>(seconds));
  }
}

void updateAudiobookProgress(bool force) {
  if (!audiobookProgressBar || (!audiobookPageVisible && !force)) return;
  const uint32_t now = millis();
  if (!force && now - audiobookProgressLastUpdateAt < 500) return;
  audiobookProgressLastUpdateAt = now;

  const uint32_t duration = audiobookDurationMs();
  const uint32_t position = audiobookPositionMs();
  const uint8_t percent = duration
    ? static_cast<uint8_t>(min<uint64_t>(100,
        (static_cast<uint64_t>(position) * 100ULL) / duration))
    : 0;
  lv_bar_set_value(audiobookProgressBar, percent, LV_ANIM_OFF);

  if (audiobookTimeLabel) {
    char elapsed[16];
    char total[16];
    char text[40];
    formatAudiobookClock(elapsed, sizeof(elapsed), position / 1000U);
    formatAudiobookClock(total, sizeof(total), duration / 1000U);
    snprintf(text, sizeof(text), "%s / %s", elapsed, total);
    lv_label_set_text(audiobookTimeLabel, text);
  }
}

void updateAudiobookPlayPauseIcon() {
  const bool playing =
    audiobookGenerator && audiobookGenerator->isRunning() && !audiobookPaused;
  if (audiobookPlayIcon) {
    if (playing) lv_obj_add_flag(audiobookPlayIcon, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(audiobookPlayIcon, LV_OBJ_FLAG_HIDDEN);
  }
  if (audiobookPauseIcon) {
    if (playing) lv_obj_clear_flag(audiobookPauseIcon, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(audiobookPauseIcon, LV_OBJ_FLAG_HIDDEN);
  }
}

String audiobookDisplayName(const String &path) {
  const int slash = path.lastIndexOf('/');
  String name = slash >= 0 ? path.substring(slash + 1) : path;
  const int dot = name.lastIndexOf('.');
  if (dot > 0) name.remove(dot);
  return name;
}

void refreshAudiobookBooklistStyles() {
  if (!audiobookBooklistList) return;
  const uint32_t childCount = lv_obj_get_child_cnt(audiobookBooklistList);
  for (uint32_t i = 0; i < childCount; ++i) {
    lv_obj_t *row = lv_obj_get_child(audiobookBooklistList, i);
    const bool selected = i == audiobookTrackIndex;
    lv_obj_set_style_bg_color(row,
      selected ? lv_color_hex(themeAccent(UI_ACCENT_MINT))
               : themeControlSurface(UI_ACCENT_MINT), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_color(row,
      selected ? lv_color_hex(themeAccent(UI_ACCENT_MINT))
               : themeControlBorder(UI_ACCENT_MINT), LV_PART_MAIN);
    if (lv_obj_get_child_cnt(row)) {
      lv_obj_set_style_text_color(lv_obj_get_child(row, 0),
        selected ? themeTextOnAccent() : themeText(), 0);
    }
  }
}

void updateAudiobookPageText() {
  if (audiobookTitleLabel) {
    lv_label_set_text(audiobookTitleLabel,
      audiobookTrackCount
        ? audiobookDisplayName(audiobookTracks[audiobookTrackIndex]).c_str()
        : tr(STR_NO_AUDIOBOOKS));
  }
  if (audiobookCountLabel) {
    if (audiobookTrackCount) {
      char count[20];
      snprintf(count, sizeof(count), tr(STR_BOOK_COUNT_FMT),
               audiobookTrackIndex + 1, audiobookTrackCount);
      lv_label_set_text(audiobookCountLabel, count);
    } else {
      lv_label_set_text(audiobookCountLabel, tr(STR_MP3_WAV));
    }
  }
  if (audiobookStatusLabel)
    lv_label_set_text(audiobookStatusLabel, tr(audiobookStatusId));
  updateAudiobookPlayPauseIcon();
  refreshAudiobookBooklistStyles();
}

void setAudiobookStatus(UiStr id) {
  audiobookStatusId = id;
  updateAudiobookPageText();
}

void rebuildAudiobookBooklist() {
  if (!audiobookBooklistList) return;
  lv_obj_clean(audiobookBooklistList);
  for (uint8_t i = 0; i < audiobookTrackCount; ++i) {
    lv_obj_t *row = lv_btn_create(audiobookBooklistList);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 58);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(row, audiobookBookRowEvent, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    char title[160];
    snprintf(title, sizeof(title), "%02u   %s", i + 1,
             audiobookDisplayName(audiobookTracks[i]).c_str());
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, title);
    lv_obj_set_width(label, 310);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, &lv_font_ui_18, 0);
    lv_obj_set_style_text_color(label, themeText(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
  }
  refreshAudiobookBooklistStyles();
}

bool isSupportedAudiobookFile(const String &name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".mp3") || lower.endsWith(".wav");
}

void scanAudiobooks() {
  const String preferredBook = audiobookSuspendedPath;
  audiobookTrackCount = 0;
  audiobookTrackIndex = 0;
  rebuildAudiobookBooklist();
  if (!weatherWallpaperSdReadyForOtherUsers()) {
    setAudiobookStatus(STR_SD_BUSY);
    return;
  }
  if (!mountMusicSd()) {
    setAudiobookStatus(STR_SD_UNAVAILABLE);
    return;
  }

  if (!storageFs().exists("/audiobooks")) {
    setAudiobookStatus(STR_AUDIOBOOK_FOLDER_HINT);
    return;
  }
  File directory = storageFs().open("/audiobooks");
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    setAudiobookStatus(STR_CANNOT_OPEN_BOOK_FOLDER);
    return;
  }
  File entry;
  while (audiobookTrackCount < AUDIOBOOK_LIMIT &&
         (entry = directory.openNextFile())) {
    if (!entry.isDirectory()) {
      const char *entryPath = entry.path();
      String path = entryPath ? entryPath : entry.name();
      if (!path.startsWith("/")) path = String("/audiobooks/") + path;
      if (isSupportedAudiobookFile(path)) audiobookTracks[audiobookTrackCount++] = path;
    }
    entry.close();
  }
  directory.close();

  for (uint8_t i = 0; i < audiobookTrackCount; ++i) {
    for (uint8_t j = i + 1; j < audiobookTrackCount; ++j) {
      if (audiobookTracks[j].compareTo(audiobookTracks[i]) < 0) {
        String temporary = audiobookTracks[i];
        audiobookTracks[i] = audiobookTracks[j];
        audiobookTracks[j] = temporary;
      }
    }
  }
  if (!preferredBook.isEmpty()) {
    for (uint8_t i = 0; i < audiobookTrackCount; ++i) {
      if (audiobookTracks[i] == preferredBook) {
        audiobookTrackIndex = i;
        break;
      }
    }
  }
  Serial.printf("Audiobooks: found %u file(s)\n", audiobookTrackCount);
  rebuildAudiobookBooklist();
  setAudiobookStatus(audiobookTrackCount ? STR_READY : STR_NO_AUDIOBOOKS_SD);
}

// ---- Playback --------------------------------------------------------------
void stopAudiobookPlayback() {
  if (!audiobookGenerator && !audiobookSource) return;
  if (audiobookSource && audiobookSource->isOpen() && audiobookTrackCount)
    audiobookSaveResume(audiobookSource->getPos());
  if (musicCodecReady) digitalWrite(AUDIO_PA_PIN, LOW);
  audiobookPaused = false;
  if (audiobookGenerator) {
    if (audiobookGenerator->isRunning()) audiobookGenerator->stop();
    delete audiobookGenerator;
    audiobookGenerator = nullptr;
    audiobookMp3 = nullptr;
    audiobookWav = nullptr;
  }
  if (audiobookSource) {
    audiobookSource->close();
    delete audiobookSource;
    audiobookSource = nullptr;
  }
  updateAudiobookPlayPauseIcon();
}

bool startAudiobook(uint8_t index, bool resume) {
  if (!audiobookTrackCount || index >= audiobookTrackCount) return false;
  if (!weatherWallpaperSdReadyForOtherUsers()) {
    setAudiobookStatus(STR_SD_BUSY);
    return false;
  }
  if (!mountMusicSd()) {
    setAudiobookStatus(STR_SD_UNAVAILABLE);
    return false;
  }
  stopMusicPlayback();
  stopAudiobookPlayback();
  stopRadioPlayback();
  audiobookTrackIndex = index;
  const String path = audiobookTracks[index];
  String lower = path;
  lower.toLowerCase();
  audiobookIsMp3 = lower.endsWith(".mp3");
  const uint32_t resumeBytes = resume ? audiobookLoadResume(path) : 0;

  audiobookSource = new ClockSdFileSource(path.c_str());
  if (!audiobookSource || !audiobookSource->isOpen()) {
    Serial.printf("Audiobooks: cannot open %s\n", path.c_str());
    stopAudiobookPlayback();
    setAudiobookStatus(STR_CANNOT_OPEN_TRACK);
    return false;
  }
  if (!initializeMusicCodec()) {
    stopAudiobookPlayback();
    return false;
  }

  if (audiobookIsMp3) {
    // Seek before begin: libmad re-syncs on the next frame header, so an
    // arbitrary byte offset is a safe resume point for CBR/VBR MP3 alike.
    const uint32_t size = audiobookSource->getSize();
    if (resumeBytes > 0 && resumeBytes < size)
      audiobookSource->seek(static_cast<int32_t>(resumeBytes), SEEK_SET);
    audiobookMp3 = new ClockAudiobookMp3();
    audiobookGenerator = audiobookMp3;
    audiobookWav = nullptr;
  } else {
    audiobookWav = new ClockVideoWavGenerator();
    audiobookGenerator = audiobookWav;
    audiobookMp3 = nullptr;
  }
  if (!audiobookGenerator ||
      !audiobookGenerator->begin(audiobookSource, musicOutput)) {
    Serial.printf("Audiobooks: decoder start failed %s\n", path.c_str());
    if (musicOutput) musicOutput->stop();
    stopAudiobookPlayback();
    setAudiobookStatus(STR_UNSUPPORTED_AUDIO);
    return false;
  }
  if (!audiobookIsMp3 && audiobookWav && resumeBytes > 0) {
    const uint32_t blockBytes =
      audiobookWav->channelCount() * (audiobookWav->sampleBits() / 8U);
    const uint64_t bytesPerSecond =
      static_cast<uint64_t>(audiobookWav->sampleRateHz()) * blockBytes;
    const uint32_t dataOffset = audiobookWav->pcmDataOffset();
    const uint32_t consumed = resumeBytes > dataOffset ? resumeBytes - dataOffset : 0;
    const uint32_t resumeMs = bytesPerSecond
      ? static_cast<uint32_t>((static_cast<uint64_t>(consumed) * 1000ULL) /
                              bytesPerSecond)
      : 0;
    audiobookWav->seekToMs(resumeMs);
  }

  audiobookPaused = false;
  audiobookSuspendedPath = path;
  digitalWrite(AUDIO_PA_PIN, HIGH);
  setAudiobookStatus(resume && resumeBytes > 0 ? STR_RESUMED : STR_PLAYING);
  updateAudiobookProgress(true);
  Serial.printf("Audiobooks: playing %s from byte %lu\n", path.c_str(),
                static_cast<unsigned long>(resumeBytes));
  return true;
}

void audiobookSeekByMs(int32_t deltaMs) {
  if (!audiobookGenerator || !audiobookGenerator->isRunning() || !audiobookSource)
    return;
  markUserActivity();
  if (audiobookIsMp3 && audiobookMp3) {
    const uint32_t bitrate = audiobookMp3->bitrate();
    if (!bitrate) return;
    const uint32_t size = audiobookSource->getSize();
    const int64_t deltaBytes =
      (static_cast<int64_t>(deltaMs) * static_cast<int64_t>(bitrate)) / 8000;
    int64_t next = static_cast<int64_t>(audiobookSource->getPos()) + deltaBytes;
    if (next < 0) next = 0;
    if (next >= static_cast<int64_t>(size))
      next = size > 4096 ? static_cast<int64_t>(size) - 4096 : 0;
    audiobookSource->seek(static_cast<int32_t>(next), SEEK_SET);
    audiobookMp3->forceResync();
  } else if (audiobookWav) {
    int64_t next = static_cast<int64_t>(audiobookPositionMs()) + deltaMs;
    if (next < 0) next = 0;
    audiobookWav->seekToMs(static_cast<uint32_t>(next));
  }
  audiobookSaveResume(audiobookSource->getPos());
  updateAudiobookProgress(true);
}

void serviceAudiobookPlayback() {
  if (lowPowerMode) return;
  if (audiobookPaused || !audiobookGenerator || !audiobookGenerator->isRunning()) {
    updateAudiobookProgress();
    return;
  }
  if (audiobookGenerator->loop()) {
    updateAudiobookProgress();
    if (millis() - audiobookLastSaveAt >= AUDIOBOOK_RESUME_SAVE_INTERVAL_MS) {
      audiobookLastSaveAt = millis();
      if (audiobookSource && audiobookSource->isOpen())
        audiobookSaveResume(audiobookSource->getPos());
    }
    return;
  }

  // Current file finished: clear its resume point and auto-advance so a
  // chapter-split book keeps flowing. The last file leaves the player idle.
  updateAudiobookProgress(true);
  audiobookGenerator->stop();
  delete audiobookGenerator;
  audiobookGenerator = nullptr;
  audiobookMp3 = nullptr;
  audiobookWav = nullptr;
  if (audiobookSource) {
    audiobookSource->close();
    delete audiobookSource;
    audiobookSource = nullptr;
  }
  audiobookSaveResume(0);
  digitalWrite(AUDIO_PA_PIN, LOW);
  if (audiobookTrackIndex + 1 < audiobookTrackCount) {
    startAudiobook(audiobookTrackIndex + 1, true);
  } else {
    setAudiobookStatus(STR_FINISHED);
  }
  updateAudiobookPlayPauseIcon();
}

void selectAdjacentAudiobook(int8_t direction) {
  if (!direction || lowPowerMode) return;
  markUserActivity();
  if (!audiobookTrackCount) {
    scanAudiobooks();
    if (!audiobookTrackCount) return;
  }
  const int next = (static_cast<int>(audiobookTrackIndex) + audiobookTrackCount +
                    (direction > 0 ? 1 : -1)) % audiobookTrackCount;
  startAudiobook(static_cast<uint8_t>(next), true);
}

void toggleAudiobookPause() {
  markUserActivity();
  if (!audiobookGenerator || !audiobookGenerator->isRunning()) {
    if (!audiobookTrackCount) scanAudiobooks();
    if (audiobookTrackCount) startAudiobook(audiobookTrackIndex, true);
    return;
  }
  audiobookPaused = !audiobookPaused;
  if (audiobookPaused) {
    if (audiobookSource && audiobookSource->isOpen())
      audiobookSaveResume(audiobookSource->getPos());
    digitalWrite(AUDIO_PA_PIN, LOW);
    setAudiobookStatus(STR_PAUSED);
  } else {
    serviceAudiobookPlayback();
    digitalWrite(AUDIO_PA_PIN, HIGH);
    setAudiobookStatus(STR_PLAYING);
  }
}

bool pauseAudiobookForControlPanel() {
  const bool wasPlaying =
    audiobookGenerator && audiobookGenerator->isRunning() && !audiobookPaused;
  if (!wasPlaying) return false;
  audiobookPaused = true;
  if (audiobookSource && audiobookSource->isOpen())
    audiobookSaveResume(audiobookSource->getPos());
  if (musicCodecReady) digitalWrite(AUDIO_PA_PIN, LOW);
  setAudiobookStatus(STR_PAUSED_BY_CONTROLS);
  return true;
}

void resumeAudiobookAfterControlPanel(bool wasPlaying) {
  if (!wasPlaying || !audiobookGenerator || !audiobookGenerator->isRunning()) return;
  audiobookPaused = false;
  serviceAudiobookPlayback();
  if (musicCodecReady) digitalWrite(AUDIO_PA_PIN, HIGH);
  setAudiobookStatus(STR_PLAYING);
}

void suspendAudiobookForLowPower() {
  hideAudiobookBooklist();
  if (audiobookTrackCount && audiobookTrackIndex < audiobookTrackCount)
    audiobookSuspendedPath = audiobookTracks[audiobookTrackIndex];
  stopAudiobookPlayback();
  audiobookTrackCount = 0;
  audiobookProgressLastUpdateAt = 0;
}

// ---- Book list overlay -----------------------------------------------------
void hideAudiobookBooklist() {
  audiobookBooklistVisible = false;
  if (audiobookBooklistPanel)
    lv_obj_add_flag(audiobookBooklistPanel, LV_OBJ_FLAG_HIDDEN);
}

void showAudiobookBooklist() {
  if (!audiobookBooklistPanel || !audiobookBooklistList) return;
  audiobookBooklistVisible = true;
  refreshAudiobookBooklistStyles();
  lv_obj_clear_flag(audiobookBooklistPanel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(audiobookBooklistPanel);
  if (audiobookTrackIndex < lv_obj_get_child_cnt(audiobookBooklistList)) {
    lv_obj_scroll_to_view(
      lv_obj_get_child(audiobookBooklistList, audiobookTrackIndex), LV_ANIM_OFF);
  }
}

void toggleAudiobookBooklist() {
  markUserActivity();
  if (audiobookBooklistVisible) hideAudiobookBooklist();
  else showAudiobookBooklist();
}

// ---- Page show/hide --------------------------------------------------------
void showAudiobookPage(bool fade) {
  if (!audiobookPage || lowPowerMode || audiobookPageVisible ||
      audiobookPageShowRequested) return;
  if (fade) {
    audiobookPageHideRequested = false;
    audiobookPageShowRequested = true;
    return;
  }
  audiobookPageVisible = true;
  lv_obj_clear_flag(audiobookPage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(audiobookPage);
  if (!audiobookTrackCount) scanAudiobooks();
  if (audiobookTrackCount &&
      (!audiobookGenerator || !audiobookGenerator->isRunning())) {
    startAudiobook(audiobookTrackIndex, true);
  }
  updateAudiobookProgress(true);
}

void hideAudiobookPage(bool fade, bool returnToMenu) {
  if (!audiobookPage) return;
  if (!audiobookPageVisible &&
      lv_obj_has_flag(audiobookPage, LV_OBJ_FLAG_HIDDEN) &&
      !audiobookPageShowRequested && !audiobookPageHideRequested) return;
  if (fade) {
    audiobookPageShowRequested = false;
    audiobookPageHideRequested = true;
    return;
  }
  // Persist the position before leaving so the book resumes on return.
  if (audiobookSource && audiobookSource->isOpen())
    audiobookSaveResume(audiobookSource->getPos());
  audiobookPageVisible = false;
  hideAudiobookBooklist();
  audiobookPageShowRequested = false;
  audiobookPageHideRequested = false;
  lv_obj_add_flag(audiobookPage, LV_OBJ_FLAG_HIDDEN);
  if (returnToMenu && !lowPowerMode) showAppMenu(false);
}

void commitAudiobookFullScreen() {
  lv_obj_invalidate(lv_scr_act());
  serviceAudiobookPlayback();
  lv_refr_now(nullptr);
  serviceAudiobookPlayback();
}

void processAudiobookPageTransition() {
  if (lowPowerMode || screenFadeTransitionActive) return;
  if (audiobookPageShowRequested) {
    audiobookPageShowRequested = false;
    beginScreenFade(activeBrightness);
    hideAppMenu(false);
    showAudiobookPage(false);
    setMusicWholeScreenRefresh(true);
    commitAudiobookFullScreen();
    endScreenFade(activeBrightness);
  } else if (audiobookPageHideRequested) {
    audiobookPageHideRequested = false;
    beginScreenFade(activeBrightness);
    hideAudiobookPage(false, true);
    commitAudiobookFullScreen();
    setMusicWholeScreenRefresh(false);
    endScreenFade(activeBrightness);
  }
}

// ---- Events ----------------------------------------------------------------
void audiobookPlayPauseEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) toggleAudiobookPause();
}

void audiobookPreviousEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) selectAdjacentAudiobook(-1);
}

void audiobookNextEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) selectAdjacentAudiobook(1);
}

void audiobookSeekBackEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED)
    audiobookSeekByMs(-static_cast<int32_t>(AUDIOBOOK_SEEK_STEP_MS));
}

void audiobookSeekForwardEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED)
    audiobookSeekByMs(static_cast<int32_t>(AUDIOBOOK_SEEK_STEP_MS));
}

void audiobookRestartEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  if (!audiobookTrackCount) return;
  audiobookSaveResume(0);
  startAudiobook(audiobookTrackIndex, false);
}

void audiobookBooklistEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) toggleAudiobookBooklist();
}

void audiobookBookRowEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  const uint8_t index = static_cast<uint8_t>(
    reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  if (index < audiobookTrackCount && startAudiobook(index, true))
    hideAudiobookBooklist();
}

// ---- UI construction -------------------------------------------------------
lv_obj_t *createAudiobookControlButton(lv_obj_t *parent, lv_coord_t size,
                                       lv_coord_t x, lv_coord_t y,
                                       lv_event_cb_t callback,
                                       TablerIconKind iconKind,
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
  lv_obj_set_style_bg_color(button, lv_color_hex(themeAccent(UI_ACCENT_MINT)),
                            LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
  createTablerIcon(button, iconKind, iconSize, themeIcon());
  return button;
}

lv_obj_t *createAudiobookSeekButton(lv_obj_t *parent, lv_coord_t x,
                                    lv_event_cb_t callback, const char *text) {
  lv_obj_t *button = lv_btn_create(parent);
  lv_obj_set_size(button, 92, 60);
  lv_obj_align(button, LV_ALIGN_TOP_MID, x, 238);
  lv_obj_set_style_radius(button, 30, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(themeSurface(0x0A0A0A)), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_border_color(button, lv_color_hex(themeBorder(0x343846)), 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(themeAccent(UI_ACCENT_MINT)),
                            LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, &lv_font_ui_20, 0);
  lv_obj_set_style_text_color(label, themeText(), 0);
  lv_obj_center(label);
  return button;
}

void createAudiobookBooklistPanel() {
  audiobookBooklistPanel = lv_obj_create(audiobookPage);
  lv_obj_remove_style_all(audiobookBooklistPanel);
  lv_obj_set_size(audiobookBooklistPanel, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_pos(audiobookBooklistPanel, 0, 0);
  lv_obj_set_style_bg_color(audiobookBooklistPanel, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(audiobookBooklistPanel, LV_OPA_COVER, 0);
  lv_obj_add_flag(audiobookBooklistPanel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(audiobookBooklistPanel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(audiobookBooklistPanel, appPageGestureEvent,
                      LV_EVENT_GESTURE, nullptr);

  lv_obj_t *title = lv_label_create(audiobookBooklistPanel);
  i18nRegister(title, STR_BOOKLIST);
  lv_obj_set_style_text_font(title, &lv_font_ui_24, 0);
  lv_obj_set_style_text_letter_space(title, 3, 0);
  lv_obj_set_style_text_color(title, themeText(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);

  audiobookBooklistList = lv_obj_create(audiobookBooklistPanel);
  lv_obj_set_pos(audiobookBooklistList, 24, 75);
  lv_obj_set_size(audiobookBooklistList, SCREEN_WIDTH - 48, SCREEN_HEIGHT - 94);
  lv_obj_set_flex_flow(audiobookBooklistList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(audiobookBooklistList, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(audiobookBooklistList, 0, 0);
  lv_obj_set_style_pad_row(audiobookBooklistList, 10, 0);
  lv_obj_set_style_bg_opa(audiobookBooklistList, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(audiobookBooklistList, 0, 0);
  lv_obj_set_style_width(audiobookBooklistList, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(audiobookBooklistList,
                            lv_color_hex(themeAccent(UI_ACCENT_MINT)),
                            LV_PART_SCROLLBAR);
  lv_obj_add_flag(audiobookBooklistList, LV_OBJ_FLAG_GESTURE_BUBBLE);

  lv_obj_add_flag(audiobookBooklistPanel, LV_OBJ_FLAG_HIDDEN);
}

void createAudiobookPage(lv_obj_t *screen) {
  audiobookPage = lv_obj_create(screen);
  lv_obj_remove_style_all(audiobookPage);
  lv_obj_set_pos(audiobookPage, 0, 0);
  lv_obj_set_size(audiobookPage, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(audiobookPage, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(audiobookPage, LV_OPA_COVER, 0);
  lv_obj_clear_flag(audiobookPage,
                    LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(audiobookPage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(audiobookPage, appPageGestureEvent, LV_EVENT_GESTURE, nullptr);

  audiobookTitleLabel = lv_label_create(audiobookPage);
  lv_obj_set_size(audiobookTitleLabel, SCREEN_WIDTH - 64, 64);
  lv_label_set_long_mode(audiobookTitleLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(audiobookTitleLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(audiobookTitleLabel, &lv_font_ui_24, 0);
  lv_obj_set_style_text_color(audiobookTitleLabel, themeText(), 0);
  lv_obj_align(audiobookTitleLabel, LV_ALIGN_TOP_MID, 0, 30);

  audiobookStatusLabel = lv_label_create(audiobookPage);
  lv_obj_set_size(audiobookStatusLabel, 230, 30);
  lv_obj_set_style_text_font(audiobookStatusLabel, &lv_font_ui_18, 0);
  lv_obj_set_style_text_color(audiobookStatusLabel,
                              lv_color_hex(themeAccent(UI_ACCENT_MINT)), 0);
  lv_obj_set_style_text_align(audiobookStatusLabel, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_align(audiobookStatusLabel, LV_ALIGN_TOP_LEFT, 40, 116);

  audiobookCountLabel = lv_label_create(audiobookPage);
  lv_obj_set_size(audiobookCountLabel, 150, 30);
  lv_obj_set_style_text_font(audiobookCountLabel, &lv_font_ui_16, 0);
  lv_obj_set_style_text_color(audiobookCountLabel, themeTextDim(0x777D8D), 0);
  lv_obj_set_style_text_align(audiobookCountLabel, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(audiobookCountLabel, LV_ALIGN_TOP_RIGHT, -40, 118);

  audiobookTimeLabel = lv_label_create(audiobookPage);
  lv_obj_set_size(audiobookTimeLabel, SCREEN_WIDTH - 80, 30);
  lv_obj_set_style_text_font(audiobookTimeLabel, &lv_font_ui_20, 0);
  lv_obj_set_style_text_color(audiobookTimeLabel, themeTextDim(0xB8C0CC), 0);
  lv_obj_set_style_text_align(audiobookTimeLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(audiobookTimeLabel, LV_ALIGN_TOP_MID, 0, 158);
  lv_label_set_text(audiobookTimeLabel, "0:00 / 0:00");

  audiobookProgressBar = lv_bar_create(audiobookPage);
  lv_obj_set_pos(audiobookProgressBar, 40, 196);
  lv_obj_set_size(audiobookProgressBar, SCREEN_WIDTH - 80, 12);
  lv_bar_set_range(audiobookProgressBar, 0, 100);
  lv_bar_set_value(audiobookProgressBar, 0, LV_ANIM_OFF);
  lv_obj_set_style_radius(audiobookProgressBar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(audiobookProgressBar, lv_color_hex(themeSurface(0x1A1A1A)),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(audiobookProgressBar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(audiobookProgressBar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(audiobookProgressBar, lv_color_hex(themeAccent(UI_ACCENT_MINT)),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(audiobookProgressBar, LV_OPA_COVER, LV_PART_INDICATOR);

  createAudiobookSeekButton(audiobookPage, -120, audiobookSeekBackEvent,
                            tr(STR_SEEK_BACK));
  createAudiobookSeekButton(audiobookPage, 120, audiobookSeekForwardEvent,
                            tr(STR_SEEK_FORWARD));

  createAudiobookControlButton(audiobookPage, 58, -176, 350, audiobookBooklistEvent,
                               TABLER_ICON_PLAYLIST, 28);
  createAudiobookControlButton(audiobookPage, 58, -88, 350, audiobookPreviousEvent,
                               TABLER_ICON_SKIP_BACK, 28);
  lv_obj_t *playButton = createAudiobookControlButton(
    audiobookPage, 74, 0, 342, audiobookPlayPauseEvent, TABLER_ICON_PLAY, 32);
  lv_obj_set_style_bg_color(playButton, lv_color_hex(themeAccent(UI_ACCENT_MINT)),
                            LV_PART_MAIN);
  lv_obj_set_style_border_width(playButton, themeIsAmber() ? 2 : 0, LV_PART_MAIN);
  lv_obj_set_style_border_color(playButton, lv_color_hex(CRT_AMBER), LV_PART_MAIN);
  audiobookPlayIcon = lv_obj_get_child(playButton, lv_obj_get_child_cnt(playButton) - 1);
  audiobookPauseIcon = createTablerIcon(playButton, TABLER_ICON_PAUSE, 32,
    themeTextOnAccent());
  if (audiobookPlayIcon)
    setTablerIconColor(audiobookPlayIcon,
      themeTextOnAccent());
  lv_obj_add_flag(audiobookPauseIcon, LV_OBJ_FLAG_HIDDEN);
  createAudiobookControlButton(audiobookPage, 58, 88, 350, audiobookNextEvent,
                               TABLER_ICON_SKIP_FORWARD, 28);
  createAudiobookControlButton(audiobookPage, 58, 176, 350, audiobookRestartEvent,
                               TABLER_ICON_ROTATE_LEFT, 28);

  setAudiobookStatus(STR_INSERT_SD);
  createAudiobookBooklistPanel();
  lv_obj_add_flag(audiobookPage, LV_OBJ_FLAG_HIDDEN);
}

// Re-apply the active theme to the audiobook player and its book list.
void refreshAudiobookTheme() {
  if (!audiobookPage) return;
  lv_obj_set_style_bg_color(audiobookPage, themePageBackground(), 0);
  if (audiobookTitleLabel)
    lv_obj_set_style_text_color(audiobookTitleLabel, themeText(), 0);
  if (audiobookStatusLabel)
    lv_obj_set_style_text_color(audiobookStatusLabel,
      lv_color_hex(themeAccent(UI_ACCENT_MINT)), 0);
  if (audiobookCountLabel)
    lv_obj_set_style_text_color(audiobookCountLabel, themeTextDim(0x777D8D), 0);
  if (audiobookTimeLabel)
    lv_obj_set_style_text_color(audiobookTimeLabel, themeTextDim(0xB8C0CC), 0);
  if (audiobookProgressBar) {
    lv_obj_set_style_bg_color(audiobookProgressBar, lv_color_hex(themeSurface(0x1A1A1A)),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(audiobookProgressBar,
      lv_color_hex(themeAccent(UI_ACCENT_MINT)), LV_PART_INDICATOR);
  }
  const uint32_t childCount = lv_obj_get_child_cnt(audiobookPage);
  for (uint32_t i = 0; i < childCount; ++i) {
    lv_obj_t *child = lv_obj_get_child(audiobookPage, i);
    if (!lv_obj_check_type(child, &lv_btn_class)) continue;
    const bool isPlayButton = lv_obj_get_width(child) == 74 &&
                              lv_obj_get_height(child) == 74;
    if (isPlayButton) {
      lv_obj_set_style_bg_color(child, lv_color_hex(themeAccent(UI_ACCENT_MINT)),
                                LV_PART_MAIN);
      lv_obj_set_style_border_width(child, themeIsAmber() ? 2 : 0, LV_PART_MAIN);
      lv_obj_set_style_border_color(child, lv_color_hex(CRT_AMBER), LV_PART_MAIN);
      for (uint32_t j = 0; j < lv_obj_get_child_cnt(child); ++j)
        setTablerIconColor(lv_obj_get_child(child, j),
          themeTextOnAccent());
    } else {
      lv_obj_set_style_bg_color(child, lv_color_hex(themeSurface(0x0A0A0A)), LV_PART_MAIN);
      lv_obj_set_style_border_color(child, lv_color_hex(themeBorder(0x343846)), LV_PART_MAIN);
      lv_obj_set_style_bg_color(child, lv_color_hex(themeAccent(UI_ACCENT_MINT)),
                                LV_PART_MAIN | LV_STATE_PRESSED);
      // Icon buttons carry a vector layer; seek buttons carry a text label.
      for (uint32_t j = 0; j < lv_obj_get_child_cnt(child); ++j) {
        lv_obj_t *grandchild = lv_obj_get_child(child, j);
        if (lv_obj_check_type(grandchild, &lv_label_class))
          lv_obj_set_style_text_color(grandchild, themeText(), 0);
        else
          setTablerIconColor(grandchild, themeIcon());
      }
    }
  }
  if (audiobookBooklistPanel) {
    lv_obj_set_style_bg_color(audiobookBooklistPanel, themePageBackground(), 0);
    if (lv_obj_get_child_cnt(audiobookBooklistPanel))
      lv_obj_set_style_text_color(lv_obj_get_child(audiobookBooklistPanel, 0),
                                  themeText(), 0);
    if (audiobookBooklistList)
      lv_obj_set_style_bg_color(audiobookBooklistList,
        lv_color_hex(themeAccent(UI_ACCENT_MINT)), LV_PART_SCROLLBAR);
  }
  refreshAudiobookBooklistStyles();
}
