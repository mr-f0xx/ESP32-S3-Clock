// SD-card MP3/WAV player for the full-screen Music app.
// Included once by 08_LVGL_Clock.ino inside its private namespace.

constexpr uint8_t MUSIC_TRACK_LIMIT = 64;
constexpr uint32_t MUSIC_SD_FREQUENCY = 20000000;
constexpr uint32_t MUSIC_DEFAULT_SAMPLE_RATE = 44100;
// Reserve I2S before the video DMA arena fragments internal SRAM. Keep the
// v1.1.0 queue (10 x 1024 B = 2560 stereo frames, ~58 ms at 44.1 kHz): growing
// it to 32 KB starved the shared internal DMA pool that the weather TLS window
// and the video arena also need, and destabilized the device. The radio stutter
// is instead fixed at its source -- the radio page no longer forces whole-screen
// LVGL recomposition, the main loop services audio every 1 ms while streaming,
// and Wi-Fi modem sleep stays off -- so this modest queue stays full.
constexpr uint8_t MUSIC_I2S_DMA_BUFFER_COUNT = 10;
constexpr uint16_t MUSIC_I2S_DMA_BUFFER_BYTES = 1024;

lv_obj_t *musicProgressBar = nullptr;
uint32_t musicProgressLastUpdateAt = 0;

// The AMOLED QSPI driver owns SPI2_HOST for the lifetime of the UI. On the
// ESP32-S3, FSPI maps to that same host, so using it for SD corrupts later LCD
// transactions (visible as a whole-screen color shift). Keep SD on SPI3_HOST.
SPIClass musicSdSpi(HSPI);
String musicTracks[MUSIC_TRACK_LIMIT];
String musicSuspendedTrackPath;
uint8_t musicTrackCount = 0;
uint8_t musicTrackIndex = 0;
bool musicSdReady = false;
bool musicCodecReady = false;
bool musicPaused = false;
es8311_handle_t musicCodec = nullptr;
AudioFileSourceSD *musicSource = nullptr;
AudioGenerator *musicGenerator = nullptr;
bool videoAudioActive = false;
bool videoAudioRestartRequested = false;
String videoAudioPath;
uint64_t videoAudioStartedAtUs = 0;
uint32_t videoAudioStartPositionMs = 0;

enum MusicPlaybackMode : uint8_t {
  MUSIC_MODE_REPEAT_ALL = 0,
  MUSIC_MODE_REPEAT_ONE,
  MUSIC_MODE_SHUFFLE,
  MUSIC_MODE_COUNT
};

uint8_t musicPlaybackMode = MUSIC_MODE_REPEAT_ALL;
lv_obj_t *musicModeIcons[MUSIC_MODE_COUNT] = {};

class ClockMusicOutput : public AudioOutputI2S {
 public:
  bool SetRate(int hz) override {
    if (musicCodecReady && musicCodec &&
        es8311_sample_frequency_config(musicCodec, hz * 256, hz) != ESP_OK) {
      Serial.printf("ES8311 does not support %d Hz with 256x MCLK\n", hz);
      return false;
    }
    return AudioOutputI2S::SetRate(hz);
  }

  // AudioGeneratorWAV normally submits one sample per I2S driver call. At
  // 24 kHz that needlessly occupies the Arduino/LVGL task and makes touch
  // input feel sticky. The video sidecar is mono PCM, so submit it in small
  // stereo blocks while preserving the normal gain path.
  uint16_t consumeMonoSamples(const int16_t *samples, uint16_t count) {
    if (!i2sOn || !samples || !count) return 0;
    uint16_t consumed = 0;
    uint32_t stereo[256];
    while (consumed < count) {
      const uint16_t batch =
        min<uint16_t>(count - consumed,
                      static_cast<uint16_t>(sizeof(stereo) / sizeof(stereo[0])));
      for (uint16_t i = 0; i < batch; ++i) {
        const uint16_t sample =
          static_cast<uint16_t>(Amplify(samples[consumed + i]));
        stereo[i] = (static_cast<uint32_t>(sample) << 16U) | sample;
      }
      size_t writtenBytes = 0;
      i2s_channel_write(_tx_handle, stereo, batch * sizeof(stereo[0]),
                        &writtenBytes, 0);
      const uint16_t written =
        static_cast<uint16_t>(writtenBytes / sizeof(stereo[0]));
      consumed += written;
      if (written < batch) break;
    }
    return consumed;
  }

  uint32_t dmaCapacityFrames() const {
    return static_cast<uint32_t>(_buffers * _bufferWords);
  }
};

class ClockVideoWavGenerator : public AudioGeneratorWAV {
 public:
  bool begin(AudioFileSource *source, AudioOutput *audioOutput) override {
    if (!AudioGeneratorWAV::begin(source, audioOutput)) return false;
    dataOffset = file->getPos();
    dataBytes = availBytes;
    fastMonoPcm = channels == 1 && bitsPerSample == 16;
    return true;
  }

  // AudioGeneratorWAV::stop() deletes the I2S channel. During video seek or
  // source changes that channel may be impossible to allocate again after the
  // two display DMA strips exist. Tear down only the WAV source/generator and
  // keep the already-reserved I2S channel alive and muted.
  bool stopPreservingOutput() {
    if (!running) return true;
    running = false;
    free(buff);
    buff = nullptr;
    pendingOffset = 0;
    pendingSamples = 0;
    return !file || file->close();
  }

  bool loop() override {
    if (!fastMonoPcm) return AudioGeneratorWAV::loop();
    if (!running) return false;
    ClockMusicOutput *clockOutput =
      static_cast<ClockMusicOutput *>(output);
    while (running) {
      if (pendingSamples) {
        const uint16_t written = clockOutput->consumeMonoSamples(
          pcmBuffer + pendingOffset, pendingSamples);
        submittedSamples += written;
        pendingOffset += written;
        pendingSamples -= written;
        if (pendingSamples) break;
      }
      if (availBytes < sizeof(int16_t)) {
        // Keep I2S alive until the video reports its own loop boundary.
        // Stopping as soon as the last bytes are queued discards the DMA tail
        // (up to a few hundred milliseconds) and restarts audio too early.
        break;
      }
      const uint32_t wanted =
        min<uint32_t>(availBytes, sizeof(pcmBuffer)) & ~1U;
      const uint64_t readStartedAtUs = esp_timer_get_time();
      const uint32_t received = file->read(pcmBuffer, wanted);
      sdReadUs += esp_timer_get_time() - readStartedAtUs;
      ++sdReadCalls;
      sdReadBytes += received;
      if (received < sizeof(int16_t)) {
        stop();
        break;
      }
      const uint32_t aligned = received & ~1U;
      availBytes -= min(availBytes, aligned);
      pendingOffset = 0;
      pendingSamples = static_cast<uint16_t>(
        aligned / sizeof(int16_t));
    }
    file->loop();
    output->loop();
    return running;
  }

  bool seekToMs(uint32_t positionMs) {
    if (!running || !file || !dataBytes || !sampleRate || !channels ||
        !bitsPerSample) return false;
    const uint32_t blockBytes = channels * (bitsPerSample / 8U);
    const uint64_t bytesPerSecond =
      static_cast<uint64_t>(sampleRate) * blockBytes;
    uint64_t offset =
      (static_cast<uint64_t>(positionMs) * bytesPerSecond) / 1000U;
    offset -= offset % blockBytes;
    if (offset >= dataBytes)
      offset = dataBytes > blockBytes ? dataBytes - blockBytes : 0;
    if (!file->seek(static_cast<int32_t>(dataOffset + offset), SEEK_SET))
      return false;
    availBytes = dataBytes - static_cast<uint32_t>(offset);
    buffPtr = 0;
    buffLen = 0;
    pendingOffset = 0;
    pendingSamples = 0;
    submittedSamples = 0;
    lastSample[0] = 0;
    lastSample[1] = 0;
    return true;
  }

  uint32_t sampleRateHz() const { return sampleRate; }
  uint16_t channelCount() const { return channels; }
  uint16_t sampleBits() const { return bitsPerSample; }
  uint32_t pcmDataBytes() const { return dataBytes; }
  uint32_t pcmDataOffset() const { return dataOffset; }
  uint32_t durationMs() const {
    const uint32_t blockBytes = channels * (bitsPerSample / 8U);
    if (!sampleRate || !blockBytes) return 0;
    return static_cast<uint32_t>(
      (static_cast<uint64_t>(dataBytes) * 1000ULL) /
      (static_cast<uint64_t>(sampleRate) * blockBytes));
  }
  uint32_t readCalls() const { return sdReadCalls; }
  uint64_t bytesRead() const { return sdReadBytes; }
  uint64_t readTimeUs() const { return sdReadUs; }
  uint64_t submittedSampleFrames() const { return submittedSamples; }
  bool usingFastMonoPcm() const { return fastMonoPcm; }

 private:
  // Keep each SD transaction short enough to fit inside the I2S headroom even
  // when the higher-priority JPEG worker preempts the Arduino loop on core 1.
  // 2 KiB contains 42.7 ms of 24 kHz mono PCM and also frees the SRAM consumed
  // by the two additional I2S descriptors above.
  int16_t pcmBuffer[1024];
  uint16_t pendingOffset = 0;
  uint16_t pendingSamples = 0;
  uint32_t dataOffset = 0;
  uint32_t dataBytes = 0;
  bool fastMonoPcm = false;
  uint32_t sdReadCalls = 0;
  uint64_t sdReadBytes = 0;
  uint64_t sdReadUs = 0;
  uint64_t submittedSamples = 0;
};

ClockMusicOutput *musicOutput = nullptr;
ClockVideoWavGenerator *videoWavGenerator = nullptr;
bool alarmToneActive = false;
uint16_t alarmTonePhase = 0;
uint16_t alarmToneOvertonePhase = 0;
uint8_t alarmToneNoteIndex = 0;
uint32_t alarmToneNoteSample = 0;

struct AlarmChimeNote {
  uint16_t frequency;
  uint16_t durationMs;
  uint16_t soundingMs;
};

// A ready-to-use ascending chime preset. The short articulated notes and soft
// harmonic sound more animated than the old alternating two-tone buzzer.
constexpr AlarmChimeNote ALARM_CHIME[] = {
  {659, 170, 135}, {784, 170, 135}, {1047, 270, 225}, {0, 90, 0},
  {784, 170, 135}, {988, 170, 135}, {1319, 360, 310}, {0, 520, 0}
};
constexpr uint16_t ALARM_SAMPLE_RATE = 16000;
constexpr uint16_t ALARM_SAMPLES_PER_MS = ALARM_SAMPLE_RATE / 1000U;
constexpr uint16_t ALARM_FEED_BLOCK_FRAMES = 256;
constexpr uint8_t ALARM_FEED_MAX_BLOCKS = 8;

static const int16_t ALARM_SINE_TABLE[32] = {
  0, 1756, 3444, 5000, 6364, 7483, 8315, 8827,
  9000, 8827, 8315, 7483, 6364, 5000, 3444, 1756,
  0, -1756, -3444, -5000, -6364, -7483, -8315, -8827,
  -9000, -8827, -8315, -7483, -6364, -5000, -3444, -1756
};

bool startMusicTrack(uint8_t index);
void musicTrackRowEvent(lv_event_t *event);
void setMusicWholeScreenRefresh(bool enabled);

void updateMusicProgress(bool force = false) {
  if (!musicProgressBar || (!musicPageVisible && !force)) return;
  const uint32_t now = millis();
  if (!force && now - musicProgressLastUpdateAt < 500) return;
  musicProgressLastUpdateAt = now;

  uint8_t percent = 0;
  if (musicSource && musicSource->isOpen()) {
    const uint32_t size = musicSource->getSize();
    if (size) {
      percent = static_cast<uint8_t>(min<uint64_t>(
        100, (static_cast<uint64_t>(musicSource->getPos()) * 100ULL) / size));
    }
  }
  lv_bar_set_value(musicProgressBar, percent, LV_ANIM_OFF);
}

void updateMusicPlayPauseIcon() {
  const bool playing = musicGenerator && musicGenerator->isRunning() && !musicPaused;
  if (musicPlayIcon) {
    if (playing) lv_obj_add_flag(musicPlayIcon, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(musicPlayIcon, LV_OBJ_FLAG_HIDDEN);
  }
  if (musicPauseIcon) {
    if (playing) lv_obj_clear_flag(musicPauseIcon, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(musicPauseIcon, LV_OBJ_FLAG_HIDDEN);
  }
}

void updateMusicPlaybackModeIcon() {
  for (uint8_t i = 0; i < MUSIC_MODE_COUNT; ++i) {
    if (!musicModeIcons[i]) continue;
    if (i == musicPlaybackMode) lv_obj_clear_flag(musicModeIcons[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(musicModeIcons[i], LV_OBJ_FLAG_HIDDEN);
  }
}

void refreshMusicPlaylistStyles() {
  if (!musicPlaylistList) return;
  const uint32_t childCount = lv_obj_get_child_cnt(musicPlaylistList);
  for (uint32_t i = 0; i < childCount; ++i) {
    lv_obj_t *row = lv_obj_get_child(musicPlaylistList, i);
    const bool selected = i == musicTrackIndex;
    if (themeIsAmber()) {
      // Inverted amber key for the active track, black keys for the rest.
      lv_obj_set_style_bg_color(row,
        selected ? lv_color_hex(CRT_AMBER) : lv_color_black(), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_color(row, lv_color_hex(CRT_AMBER_FAINT), LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_color(row,
        lv_color_hex(selected ? UI_ACCENT_VIOLET : 0x0D0D12), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_80, LV_PART_MAIN);
      lv_obj_set_style_border_color(row,
        lv_color_hex(selected ? 0xA78BFA : 0x282C37), LV_PART_MAIN);
    }
    if (lv_obj_get_child_cnt(row)) {
      lv_obj_set_style_text_color(lv_obj_get_child(row, 0),
        themeIsAmber() ? (selected ? lv_color_black() : lv_color_hex(CRT_AMBER))
                       : lv_color_white(), 0);
    }
  }
}

String musicDisplayName(const String &path) {
  const int slash = path.lastIndexOf('/');
  return slash >= 0 ? path.substring(slash + 1) : path;
}

// The current status line is stored as a string id so a language switch can
// re-render it without losing the live playback state.
UiStr musicStatusId = STR_INSERT_SD;

void updateMusicPageText() {
  if (musicTrackLabel) {
    lv_label_set_text(musicTrackLabel,
                      musicTrackCount ? musicDisplayName(musicTracks[musicTrackIndex]).c_str()
                                      : tr(STR_NO_MUSIC_FOUND));
  }
  if (musicCountLabel) {
    if (musicTrackCount) {
      char count[20];
      snprintf(count, sizeof(count), "%u / %u", musicTrackIndex + 1, musicTrackCount);
      lv_label_set_text(musicCountLabel, count);
    } else {
      lv_label_set_text(musicCountLabel, tr(STR_MP3_WAV));
    }
  }
  if (musicStatusLabel) lv_label_set_text(musicStatusLabel, tr(musicStatusId));
  updateMusicPlayPauseIcon();
  updateMusicPlaybackModeIcon();
  refreshMusicPlaylistStyles();
}

void setMusicStatus(UiStr id) {
  musicStatusId = id;
  updateMusicPageText();
}

bool isSupportedMusicFile(const String &name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".mp3") || lower.endsWith(".wav");
}

void rebuildMusicPlaylist() {
  if (!musicPlaylistList) return;
  lv_obj_clean(musicPlaylistList);
  for (uint8_t i = 0; i < musicTrackCount; ++i) {
    lv_obj_t *row = lv_btn_create(musicPlaylistList);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, 58);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 16, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(row, musicTrackRowEvent, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(i)));

    char title[160];
    snprintf(title, sizeof(title), "%02u   %s", i + 1,
             musicDisplayName(musicTracks[i]).c_str());
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, title);
    lv_obj_set_width(label, 310);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, &lv_font_ui_18, 0);
    lv_obj_set_style_text_color(label, themeText(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
  }
  refreshMusicPlaylistStyles();
}

bool mountMusicSd() {
  if (musicSdReady) return true;
  musicSdSpi.begin(SD_SCLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN, musicSdSpi, MUSIC_SD_FREQUENCY)) {
    musicSdSpi.end();
    Serial.println("Music: SD mount failed");
    setMusicStatus(STR_SD_UNAVAILABLE);
    return false;
  }
  musicSdReady = true;
  Serial.println("Music: SD mounted");
  return true;
}

void scanMusicTracks() {
  const String preferredTrack = musicSuspendedTrackPath;
  musicTrackCount = 0;
  musicTrackIndex = 0;
  rebuildMusicPlaylist();
  if (!weatherWallpaperSdReadyForOtherUsers()) {
    setMusicStatus(STR_SD_BUSY);
    Serial.println("Music: waiting for weather SD worker to stop");
    return;
  }
  if (!mountMusicSd()) return;

  const char *scanPath = SD.exists("/music") ? "/music" : "/";
  File directory = SD.open(scanPath);
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    setMusicStatus(STR_CANNOT_OPEN_MUSIC_FOLDER);
    return;
  }

  File entry;
  while (musicTrackCount < MUSIC_TRACK_LIMIT &&
         (entry = directory.openNextFile())) {
    if (!entry.isDirectory()) {
      const char *entryPath = entry.path();
      String path = entryPath ? entryPath : entry.name();
      if (!path.startsWith("/")) {
        if (strcmp(scanPath, "/") == 0) path = "/" + path;
        else path = String(scanPath) + "/" + path;
      }
      if (isSupportedMusicFile(path)) musicTracks[musicTrackCount++] = path;
    }
    entry.close();
  }
  directory.close();

  for (uint8_t i = 0; i < musicTrackCount; ++i) {
    for (uint8_t j = i + 1; j < musicTrackCount; ++j) {
      if (musicTracks[j].compareTo(musicTracks[i]) < 0) {
        String temporary = musicTracks[i];
        musicTracks[i] = musicTracks[j];
        musicTracks[j] = temporary;
      }
    }
  }
  if (!preferredTrack.isEmpty()) {
    for (uint8_t i = 0; i < musicTrackCount; ++i) {
      if (musicTracks[i] == preferredTrack) {
        musicTrackIndex = i;
        break;
      }
    }
  }
  Serial.printf("Music: found %u track(s) in %s\n", musicTrackCount, scanPath);
  rebuildMusicPlaylist();
  setMusicStatus(musicTrackCount ? STR_READY : STR_NO_MP3_WAV_SD);
}

bool initializeMusicCodec() {
  if (musicCodecReady && musicCodec && musicOutput) return true;
  pinMode(AUDIO_PA_PIN, OUTPUT);
  digitalWrite(AUDIO_PA_PIN, LOW);

  // A failed or interrupted wake attempt must not leave stale handles that
  // make the next Play press look initialized while I2S is actually gone.
  if (musicOutput) {
    musicOutput->stop();
    delete musicOutput;
    musicOutput = nullptr;
  }
  if (musicCodec) {
    es8311_delete(musicCodec);
    musicCodec = nullptr;
  }
  musicCodecReady = false;

  musicCodec = es8311_create(0, ES8311_I2C_ADDRESS);
  if (!musicCodec) {
    setMusicStatus(STR_CODEC_UNAVAILABLE);
    return false;
  }
  const es8311_clock_config_t clockConfig = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = static_cast<int>(MUSIC_DEFAULT_SAMPLE_RATE * 256),
    .sample_frequency = static_cast<int>(MUSIC_DEFAULT_SAMPLE_RATE)
  };
  if (es8311_init(musicCodec, &clockConfig, ES8311_RESOLUTION_16,
                  ES8311_RESOLUTION_16) != ESP_OK) {
    es8311_delete(musicCodec);
    musicCodec = nullptr;
    setMusicStatus(STR_CODEC_INIT_FAILED);
    return false;
  }
  const esp_err_t wakeResult = es8311_power_up(musicCodec);
  if (wakeResult != ESP_OK) {
    Serial.printf("Music: ES8311 wake failed: %s\n",
                  esp_err_to_name(wakeResult));
    es8311_delete(musicCodec);
    musicCodec = nullptr;
    audioCodecReachable = false;
    setMusicStatus(STR_AUDIO_WAKE_FAILED);
    return false;
  }
  audioCodecReachable = true;

  musicOutput = new ClockMusicOutput();
  if (!musicOutput ||
      !musicOutput->SetBuffers(MUSIC_I2S_DMA_BUFFER_COUNT,
                               MUSIC_I2S_DMA_BUFFER_BYTES) ||
      !musicOutput->SetPinout(I2S_BCLK_PIN, I2S_WS_PIN, I2S_DOUT_PIN,
                             I2S_MCLK_PIN)) {
    delete musicOutput;
    musicOutput = nullptr;
    es8311_delete(musicCodec);
    musicCodec = nullptr;
    setMusicStatus(STR_I2S_SETUP_FAILED);
    return false;
  }
  // ES8311 is mono. Averaging L/R here preserves information from both
  // channels instead of relying on whichever slot the codec consumes.
  musicOutput->SetOutputModeMono(true);
#if defined(SOC_CLK_APLL_SUPPORTED) && SOC_CLK_APLL_SUPPORTED
  // Use the audio PLL for a more accurate I2S/MCLK source when available.
  if (!musicOutput->SetUseAPLL()) {
    Serial.println("Music: APLL setup unavailable, using default I2S clock");
  }
#endif
  musicOutput->SetGain(1.0f);
  musicCodecReady = true;
  applyAudioVolume();
  return true;
}

bool prepareVideoAudioI2S() {
  if (!initializeMusicCodec() || !musicOutput) return false;
  if (!musicOutput->SetRate(24000) || !musicOutput->SetChannels(1) ||
      !musicOutput->begin()) {
    Serial.println("Video audio: unable to reserve I2S before video buffers");
    musicOutput->stop();
    delete musicOutput;
    musicOutput = nullptr;
    if (musicCodec) {
      es8311_delete(musicCodec);
      musicCodec = nullptr;
    }
    musicCodecReady = false;
    return false;
  }
  digitalWrite(AUDIO_PA_PIN, LOW);
  Serial.printf(
    "Video audio: I2S reserved first (%lu frames); SRAM free/largest %lu/%lu KiB\n",
    static_cast<unsigned long>(musicOutput->dmaCapacityFrames()),
    static_cast<unsigned long>(heap_caps_get_free_size(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U),
    static_cast<unsigned long>(heap_caps_get_largest_free_block(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U));
  return true;
}

void stopMusicPlayback() {
  if (musicCodecReady) digitalWrite(AUDIO_PA_PIN, LOW);
  musicPaused = false;
  if (musicGenerator) {
    if (musicGenerator->isRunning()) musicGenerator->stop();
    delete musicGenerator;
    musicGenerator = nullptr;
    videoWavGenerator = nullptr;
  }
  if (musicSource) {
    musicSource->close();
    delete musicSource;
    musicSource = nullptr;
  }
  videoAudioActive = false;
  videoAudioPath = "";
  videoAudioStartedAtUs = 0;
  videoAudioStartPositionMs = 0;
  updateMusicProgress(true);
}

void stopVideoAudioSourceKeepingOutput() {
  if (musicCodecReady) digitalWrite(AUDIO_PA_PIN, LOW);
  musicPaused = false;
  if (musicGenerator) {
    if (videoWavGenerator && videoWavGenerator->isRunning())
      videoWavGenerator->stopPreservingOutput();
    else if (musicGenerator->isRunning())
      musicGenerator->stop();
    delete musicGenerator;
    musicGenerator = nullptr;
    videoWavGenerator = nullptr;
  }
  if (musicSource) {
    musicSource->close();
    delete musicSource;
    musicSource = nullptr;
  }
  videoAudioActive = false;
  videoAudioPath = "";
  videoAudioStartedAtUs = 0;
  videoAudioStartPositionMs = 0;
}

void holdVideoAudioForSeek() {
  if (videoAudioActive) stopVideoAudioSourceKeepingOutput();
}

bool startVideoAudioPlayback(const String &path, uint32_t positionMs) {
  stopAudiobookPlayback();
  stopRadioPlayback();
  const uint64_t requestStartedAtUs = esp_timer_get_time();
  if (videoAudioActive && videoWavGenerator && musicOutput &&
      videoAudioPath == path) {
    const uint32_t sampleRate = videoWavGenerator->sampleRateHz();
    const uint32_t dmaLatencyMs = sampleRate
      ? static_cast<uint32_t>(
          (static_cast<uint64_t>(musicOutput->dmaCapacityFrames()) * 1000ULL) /
          sampleRate)
      : 0;
    const uint32_t adjustedPositionMs = positionMs + dmaLatencyMs;
    if (videoWavGenerator->seekToMs(adjustedPositionMs)) {
      videoAudioRestartRequested = false;
      videoAudioStartPositionMs = adjustedPositionMs;
      videoAudioStartedAtUs = esp_timer_get_time();
      digitalWrite(AUDIO_PA_PIN, HIGH);
      Serial.printf("Video audio: repositioned in place to %.3f s (DMA %lu ms)\n",
                    adjustedPositionMs / 1000.0f,
                    static_cast<unsigned long>(dmaLatencyMs));
      return true;
    }
  }
  if (videoAudioActive && videoWavGenerator && musicOutput)
    stopVideoAudioSourceKeepingOutput();
  else
    stopMusicPlayback();
  videoAudioRestartRequested = false;
  if (!path.length() || !mountMusicSd() || !SD.exists(path.c_str())) {
    Serial.printf("Video audio: no sidecar %s\n", path.c_str());
    return false;
  }
  musicSource = new AudioFileSourceSD(path.c_str());
  if (!musicSource || !musicSource->isOpen()) {
    Serial.printf("Video audio: cannot open %s\n", path.c_str());
    stopMusicPlayback();
    return false;
  }
  if (!initializeMusicCodec()) {
    stopMusicPlayback();
    return false;
  }
  videoWavGenerator = new ClockVideoWavGenerator();
  musicGenerator = videoWavGenerator;
  if (!videoWavGenerator ||
      !videoWavGenerator->begin(musicSource, musicOutput)) {
    Serial.printf("Video audio: WAV/I2S start failed %s\n", path.c_str());
    if (musicOutput) musicOutput->stop();
    stopMusicPlayback();
    return false;
  }
  // Opening a large FAT file and waking the codec may take long enough to be
  // visible as lip-sync error. Seek past that elapsed setup time because the
  // video display task continued running while audio was being prepared.
  // AudioOutputI2S also preloads every DMA descriptor with silence before it
  // enables the channel. Skip that known latency in the source so the first
  // audible PCM sample corresponds to the video position at that later time.
  const uint32_t setupMs = static_cast<uint32_t>(
    (esp_timer_get_time() - requestStartedAtUs) / 1000ULL);
  const uint32_t dmaFrames =
    musicOutput ? musicOutput->dmaCapacityFrames() : 0;
  const uint32_t dmaLatencyMs =
    videoWavGenerator->sampleRateHz()
      ? static_cast<uint32_t>(
          (static_cast<uint64_t>(dmaFrames) * 1000ULL) /
          videoWavGenerator->sampleRateHz())
      : 0;
  const uint32_t adjustedPositionMs =
    positionMs + setupMs + dmaLatencyMs;
  if (!videoWavGenerator->seekToMs(adjustedPositionMs)) {
    Serial.printf("Video audio: seek failed %s\n", path.c_str());
    if (musicOutput) musicOutput->stop();
    stopMusicPlayback();
    return false;
  }
  videoAudioActive = true;
  videoAudioPath = path;
  videoAudioStartPositionMs = adjustedPositionMs;
  videoAudioStartedAtUs = esp_timer_get_time();
  musicPaused = false;
  digitalWrite(AUDIO_PA_PIN, HIGH);
  Serial.printf(
    "Video audio: playing %s from %.3f s "
    "(setup %lu ms + DMA silence %lu ms)\n",
                path.c_str(), adjustedPositionMs / 1000.0f,
                static_cast<unsigned long>(setupMs),
                static_cast<unsigned long>(dmaLatencyMs));
  Serial.printf(
    "Video audio WAV: %lu Hz, %u-bit, %u channel(s), "
    "%lu data bytes, %.3f s, feed=%s, DMA %lu frames\n",
    static_cast<unsigned long>(videoWavGenerator->sampleRateHz()),
    static_cast<unsigned>(videoWavGenerator->sampleBits()),
    static_cast<unsigned>(videoWavGenerator->channelCount()),
    static_cast<unsigned long>(videoWavGenerator->pcmDataBytes()),
    videoWavGenerator->durationMs() / 1000.0f,
    videoWavGenerator->usingFastMonoPcm() ? "2 KiB block" : "legacy",
    static_cast<unsigned long>(dmaFrames));
  return true;
}

void stopVideoAudioPlayback() {
  if (videoAudioActive) stopMusicPlayback();
  else if (musicOutput) musicOutput->stop();
}

bool videoAudioPlaybackActive() {
  return videoAudioActive && musicGenerator && musicGenerator->isRunning();
}

bool videoAudioPlaybackRestartPending() {
  return videoAudioRestartRequested;
}

uint32_t videoAudioPlaybackPositionMs(uint64_t atUs) {
  if (!videoAudioPlaybackActive() || !videoAudioStartedAtUs)
    return videoAudioStartPositionMs;
  if (!videoWavGenerator || !musicOutput ||
      !videoWavGenerator->sampleRateHz()) return videoAudioStartPositionMs;

  // The generator always fills I2S until a non-blocking write is rejected.
  // Therefore submitted-capacity is the amount of real PCM already consumed.
  // Unlike wall-clock time, this stops advancing when the main loop failed to
  // feed I2S and the peripheral had to output silence, exposing accumulating
  // audio lag to syncVideoAudio().
  const uint64_t submitted =
    videoWavGenerator->submittedSampleFrames();
  const uint64_t capacity = musicOutput->dmaCapacityFrames();
  uint64_t consumed = submitted > capacity ? submitted - capacity : 0;
  const uint64_t nowUs = esp_timer_get_time();
  if (nowUs > atUs) {
    const uint64_t rewind =
      ((nowUs - atUs) * videoWavGenerator->sampleRateHz()) / 1000000ULL;
    consumed = consumed > rewind ? consumed - rewind : 0;
  }
  return videoAudioStartPositionMs +
         static_cast<uint32_t>(
           (consumed * 1000ULL) / videoWavGenerator->sampleRateHz());
}

void printVideoAudioDiagnostics(uint32_t videoPositionMs,
                                uint32_t audioPositionMs,
                                int32_t driftMs, uint8_t driftStrikes) {
  if (!videoAudioPlaybackActive() || !videoWavGenerator) {
    Serial.printf("Video audio sync: inactive, restart=%s\n",
                  videoAudioRestartRequested ? "yes" : "no");
    return;
  }
  const uint32_t reads = videoWavGenerator->readCalls();
  const uint64_t bytes = videoWavGenerator->bytesRead();
  const uint64_t readUs = videoWavGenerator->readTimeUs();
  const uint64_t submitted = videoWavGenerator->submittedSampleFrames();
  const double throughput =
    readUs ? bytes * 1000000.0 / readUs / 1024.0 : 0.0;
  const double averageReadMs =
    reads ? readUs / 1000.0 / reads : 0.0;
  Serial.printf(
    "Video audio sync: video %.3f s, audio %.3f s, drift %+ld ms, "
    "strike %u/2; PCM submitted %.3f s; "
    "WAV SD %lu reads, %.2f MiB, %.3f ms/read, %.1f KiB/s\n",
    videoPositionMs / 1000.0f, audioPositionMs / 1000.0f,
    static_cast<long>(driftMs), static_cast<unsigned>(driftStrikes),
    submitted / static_cast<double>(videoWavGenerator->sampleRateHz()),
    static_cast<unsigned long>(reads), bytes / 1048576.0,
    averageReadMs, throughput);
}

bool startMusicTrack(uint8_t index) {
  if (!musicTrackCount || index >= musicTrackCount ||
      !weatherWallpaperSdReadyForOtherUsers() || !mountMusicSd()) return false;
  stopAudiobookPlayback();
  stopMusicPlayback();
  stopRadioPlayback();
  musicTrackIndex = index;
  musicSource = new AudioFileSourceSD(musicTracks[musicTrackIndex].c_str());
  if (!musicSource || !musicSource->isOpen()) {
    Serial.printf("Music: cannot open %s\n",
                  musicTracks[musicTrackIndex].c_str());
    stopMusicPlayback();
    setMusicStatus(STR_CANNOT_OPEN_TRACK);
    return false;
  }
  if (!initializeMusicCodec()) {
    stopMusicPlayback();
    return false;
  }

  String lower = musicTracks[musicTrackIndex];
  lower.toLowerCase();
  if (lower.endsWith(".mp3")) musicGenerator = new AudioGeneratorMP3();
  else musicGenerator = new AudioGeneratorWAV();
  if (!musicGenerator ||
      !musicGenerator->begin(musicSource, musicOutput)) {
    if (musicOutput) musicOutput->stop();
    stopMusicPlayback();
    setMusicStatus(STR_UNSUPPORTED_AUDIO);
    return false;
  }
  musicPaused = false;
  musicSuspendedTrackPath = musicTracks[musicTrackIndex];
  digitalWrite(AUDIO_PA_PIN, HIGH);
  setMusicStatus(STR_PLAYING);
  updateMusicProgress(true);
  Serial.printf("Music: playing %s\n", musicTracks[musicTrackIndex].c_str());
  return true;
}

void generateAlarmSamples(int16_t *samples, uint16_t count) {
  constexpr uint8_t noteCount = sizeof(ALARM_CHIME) / sizeof(ALARM_CHIME[0]);
  for (uint16_t i = 0; i < count; ++i) {
    const AlarmChimeNote &note = ALARM_CHIME[alarmToneNoteIndex];
    const uint32_t soundingSamples =
      static_cast<uint32_t>(note.soundingMs) * ALARM_SAMPLES_PER_MS;
    uint16_t phaseStep = 0;
    uint8_t envelope = 0;
    if (note.frequency && alarmToneNoteSample < soundingSamples) {
      phaseStep = static_cast<uint16_t>(
        (static_cast<uint32_t>(note.frequency) * 65536UL) /
        ALARM_SAMPLE_RATE);
      constexpr uint32_t attackSamples = 24U * ALARM_SAMPLES_PER_MS;
      constexpr uint32_t releaseSamples = 55U * ALARM_SAMPLES_PER_MS;
      const uint8_t attack = static_cast<uint8_t>(min<uint32_t>(
        255U, alarmToneNoteSample * 255U / attackSamples));
      const uint32_t remaining = soundingSamples - alarmToneNoteSample;
      const uint8_t release = static_cast<uint8_t>(min<uint32_t>(
        255U, remaining * 255U / releaseSamples));
      envelope = min(attack, release);
    }
    int16_t value = 0;
    if (phaseStep && envelope) {
      const int32_t fundamental =
        static_cast<int32_t>(ALARM_SINE_TABLE[alarmTonePhase >> 11]) * envelope / 255;
      const int32_t overtone =
        static_cast<int32_t>(ALARM_SINE_TABLE[alarmToneOvertonePhase >> 11]) *
        envelope / 1275;
      value = static_cast<int16_t>(fundamental + overtone);
    }
    samples[i] = value;
    alarmTonePhase = static_cast<uint16_t>(alarmTonePhase + phaseStep);
    alarmToneOvertonePhase = static_cast<uint16_t>(
      alarmToneOvertonePhase + static_cast<uint16_t>(phaseStep * 2U));
    ++alarmToneNoteSample;
    const uint32_t durationSamples =
      static_cast<uint32_t>(note.durationMs) * ALARM_SAMPLES_PER_MS;
    if (alarmToneNoteSample >= durationSamples) {
      alarmToneNoteSample = 0;
      alarmToneNoteIndex = static_cast<uint8_t>(
        (alarmToneNoteIndex + 1U) % noteCount);
    }
  }
}

void serviceAlarmSound() {
  if (!alarmToneActive || (!alarmAlertVisible && !timerAlertVisible) || !musicOutput) return;
  int16_t samples[ALARM_FEED_BLOCK_FRAMES];
  for (uint8_t block = 0; block < ALARM_FEED_MAX_BLOCKS; ++block) {
    // Save the synthesizer cursor because the zero-timeout I2S write can
    // accept only part of the final block when its DMA queue becomes full.
    const uint16_t savedPhase = alarmTonePhase;
    const uint16_t savedOvertonePhase = alarmToneOvertonePhase;
    const uint8_t savedNoteIndex = alarmToneNoteIndex;
    const uint32_t savedNoteSample = alarmToneNoteSample;
    generateAlarmSamples(samples, ALARM_FEED_BLOCK_FRAMES);
    const uint16_t written = musicOutput->consumeMonoSamples(
      samples, ALARM_FEED_BLOCK_FRAMES);
    if (written < ALARM_FEED_BLOCK_FRAMES) {
      // Commit exactly the frames accepted by I2S; otherwise each full-DMA
      // poll would silently skip part of the melody and make timing unstable.
      alarmTonePhase = savedPhase;
      alarmToneOvertonePhase = savedOvertonePhase;
      alarmToneNoteIndex = savedNoteIndex;
      alarmToneNoteSample = savedNoteSample;
      if (written) generateAlarmSamples(samples, written);
      break;
    }
  }
}

void startAlarmSound() {
  const bool interruptedVideoAudio = videoAudioActive;
  stopAudiobookPlayback();
  stopMusicPlayback();
  stopRadioPlayback();
  videoAudioRestartRequested = interruptedVideoAudio;
  if (!initializeMusicCodec() || !musicOutput) {
    Serial.println("Alarm: audio codec unavailable");
    return;
  }
  musicOutput->stop();
  if (!musicOutput->SetRate(ALARM_SAMPLE_RATE) ||
      !musicOutput->SetChannels(2) ||
      !musicOutput->begin()) {
    Serial.println("Alarm: unable to start I2S output");
    return;
  }
  // Alarm loudness is intentionally independent of the music volume slider.
  // The user's music volume is restored when the notification is dismissed.
  applyAudioVolumePercent(85);
  alarmTonePhase = 0;
  alarmToneOvertonePhase = 0;
  alarmToneNoteIndex = 0;
  alarmToneNoteSample = 0;
  alarmToneActive = true;
  serviceAlarmSound();
  digitalWrite(AUDIO_PA_PIN, HIGH);
  Serial.println("Alarm: sound started");
}

void stopAlarmSound() {
  if (!alarmToneActive) return;
  alarmToneActive = false;
  digitalWrite(AUDIO_PA_PIN, LOW);
  if (musicOutput) musicOutput->stop();
  applyAudioVolume();
  Serial.println("Alarm: sound stopped");
}

void selectAdjacentMusicTrack(int8_t direction) {
  if (!direction || lowPowerMode) return;
  markUserActivity();
  if (!musicTrackCount) {
    scanMusicTracks();
    if (!musicTrackCount) return;
  }
  const int next = (static_cast<int>(musicTrackIndex) + musicTrackCount +
                    (direction > 0 ? 1 : -1)) % musicTrackCount;
  startMusicTrack(static_cast<uint8_t>(next));
}

void serviceMusicPlayback() {
  if (lowPowerMode) return;
  if (musicPaused || !musicGenerator || !musicGenerator->isRunning()) {
    if (videoAudioActive && !musicPaused && videoPlayerModeActive()) {
      // Let the video result choose the exact restart position. Restarting
      // here from zero races the video loop/seek and causes large A/V drift.
      videoAudioRestartRequested = true;
      stopVideoAudioSourceKeepingOutput();
      return;
    }
    updateMusicProgress();
    return;
  }
  if (musicGenerator->loop()) {
    updateMusicProgress();
    return;
  }
  if (videoAudioActive) {
    videoAudioRestartRequested = true;
    stopVideoAudioSourceKeepingOutput();
    return;
  }
  updateMusicProgress(true);
  musicGenerator->stop();
  if (musicPlaybackMode == MUSIC_MODE_REPEAT_ONE) {
    startMusicTrack(musicTrackIndex);
  } else if (musicTrackCount > 1) {
    uint8_t next = (musicTrackIndex + 1) % musicTrackCount;
    if (musicPlaybackMode == MUSIC_MODE_SHUFFLE) {
      next = static_cast<uint8_t>(random(musicTrackCount - 1));
      if (next >= musicTrackIndex) ++next;
    }
    startMusicTrack(next);
  } else {
    setMusicStatus(STR_FINISHED);
    digitalWrite(AUDIO_PA_PIN, LOW);
  }
  updateMusicPlayPauseIcon();
}

bool pauseMusicForControlPanel() {
  if (videoAudioActive) return false;
  const bool wasPlaying = musicGenerator && musicGenerator->isRunning() && !musicPaused;
  if (!wasPlaying) return false;
  musicPaused = true;
  if (musicCodecReady) digitalWrite(AUDIO_PA_PIN, LOW);
  setMusicStatus(STR_PAUSED_BY_CONTROLS);
  updateMusicPlayPauseIcon();
  Serial.println("Music: paused while Controls is open");
  return true;
}

void resumeMusicAfterControlPanel(bool wasPlaying) {
  if (!wasPlaying || !musicGenerator || !musicGenerator->isRunning()) return;
  musicPaused = false;
  // Refill I2S before reconnecting the power amplifier.
  serviceMusicPlayback();
  if (musicCodecReady) digitalWrite(AUDIO_PA_PIN, HIGH);
  setMusicStatus(STR_PLAYING);
  updateMusicPlayPauseIcon();
  Serial.println("Music: resumed after Controls closed");
}

void toggleMusicPause() {
  markUserActivity();
  if (!musicGenerator || !musicGenerator->isRunning()) {
    if (!musicTrackCount) scanMusicTracks();
    if (musicTrackCount) startMusicTrack(musicTrackIndex);
    return;
  }

  musicPaused = !musicPaused;
  if (musicPaused) {
    digitalWrite(AUDIO_PA_PIN, LOW);
    setMusicStatus(STR_PAUSED);
  } else {
    // Refill I2S while the power amplifier is still muted, then reconnect it.
    serviceMusicPlayback();
    digitalWrite(AUDIO_PA_PIN, HIGH);
    setMusicStatus(STR_PLAYING);
  }
}

void hideMusicPlaylist() {
  musicPlaylistVisible = false;
  if (musicPlaylistPanel) lv_obj_add_flag(musicPlaylistPanel, LV_OBJ_FLAG_HIDDEN);
}

void showMusicPlaylist() {
  if (!musicPlaylistPanel || !musicPlaylistList) return;
  musicPlaylistVisible = true;
  refreshMusicPlaylistStyles();
  lv_obj_clear_flag(musicPlaylistPanel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(musicPlaylistPanel);
  if (musicTrackIndex < lv_obj_get_child_cnt(musicPlaylistList)) {
    lv_obj_scroll_to_view(lv_obj_get_child(musicPlaylistList, musicTrackIndex),
                          LV_ANIM_OFF);
  }
}

void toggleMusicPlaylist() {
  markUserActivity();
  if (musicPlaylistVisible) hideMusicPlaylist();
  else showMusicPlaylist();
}

void musicPreviousEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) selectAdjacentMusicTrack(-1);
}

void musicNextEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) selectAdjacentMusicTrack(1);
}

void musicPlayPauseEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) toggleMusicPause();
}

void musicPlaylistEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) toggleMusicPlaylist();
}

void musicPlaybackModeEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  musicPlaybackMode = static_cast<uint8_t>(
    (musicPlaybackMode + 1U) % MUSIC_MODE_COUNT);
  preferences.putUChar("music_mode", musicPlaybackMode);
  updateMusicPlaybackModeIcon();
  const UiStr status = musicPlaybackMode == MUSIC_MODE_REPEAT_ONE
                         ? STR_REPEAT_ONE
                         : (musicPlaybackMode == MUSIC_MODE_SHUFFLE
                              ? STR_SHUFFLE : STR_REPEAT_ALL);
  setMusicStatus(status);
}

void musicTrackRowEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  const uint8_t index = static_cast<uint8_t>(
    reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  if (index < musicTrackCount && startMusicTrack(index)) hideMusicPlaylist();
}

void suspendMusicForLowPower(bool sdSafeToUnmount) {
  setMusicWholeScreenRefresh(false);
  hideMusicPlaylist();
  if (musicTrackCount && musicTrackIndex < musicTrackCount) {
    musicSuspendedTrackPath = musicTracks[musicTrackIndex];
  }
  stopMusicPlayback();
  if (musicCodec) {
    const esp_err_t powerDownResult = es8311_power_down(musicCodec);
    if (powerDownResult != ESP_OK) {
      Serial.printf("Music: ES8311 power-down failed: %s\n",
                    esp_err_to_name(powerDownResult));
    }
  }
  if (musicOutput) {
    musicOutput->stop();
    delete musicOutput;
    musicOutput = nullptr;
  }
  if (musicCodec) {
    es8311_delete(musicCodec);
    musicCodec = nullptr;
  }
  musicCodecReady = false;
  audioCodecReachable = false;
  if (musicSdReady && sdSafeToUnmount) {
    SD.end();
    musicSdSpi.end();
    musicSdReady = false;
  } else if (musicSdReady) {
    // Never tear down the FAT/SPI objects while the wallpaper reader could
    // still be returning from a card transaction. Retaining the mounted card
    // for one standby cycle costs little power and avoids a wake-time deadlock.
    Serial.println("Music: keeping SD mounted because wallpaper I/O is still stopping");
  }
  musicTrackCount = 0;
  musicProgressLastUpdateAt = 0;
}

void showMusicPage(bool fade) {
  if (!musicPage || lowPowerMode || musicPageVisible || musicPageShowRequested) return;
  if (fade) {
    musicPageHideRequested = false;
    musicPageShowRequested = true;
    return;
  }
  musicPageVisible = true;
  lv_obj_clear_flag(musicPage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(musicPage);
  if (!musicTrackCount) scanMusicTracks();
  if (musicTrackCount && (!musicGenerator || !musicGenerator->isRunning())) {
    startMusicTrack(musicTrackIndex);
  }
  updateMusicProgress(true);
}

void hideMusicPage(bool fade, bool returnToMenu) {
  if (!musicPage) return;
  if (!musicPageVisible && lv_obj_has_flag(musicPage, LV_OBJ_FLAG_HIDDEN) &&
      !musicPageShowRequested && !musicPageHideRequested) return;
  if (fade) {
    musicPageShowRequested = false;
    musicPageHideRequested = true;
    return;
  }
  musicPageVisible = false;
  hideMusicPlaylist();
  musicPageShowRequested = false;
  musicPageHideRequested = false;
  lv_obj_add_flag(musicPage, LV_OBJ_FLAG_HIDDEN);
  if (returnToMenu && !lowPowerMode) showAppMenu(false);
}

void setMusicWholeScreenRefresh(bool enabled) {
  musicWholeScreenRefreshEnabled = enabled && fullScreenDrawBufferReady;
  lv_disp_t *display = lv_disp_get_default();
  if (display && display->driver) {
    // full_refresh deadlocks LVGL when sw_rotate is enabled. The rounder
    // callback provides whole-screen composition without entering that path.
    display->driver->full_refresh = 0;
  }
}

void commitMusicFullScreen() {
  lv_obj_invalidate(lv_scr_act());
  serviceMusicPlayback();
  lv_refr_now(nullptr);
  serviceMusicPlayback();
}

void processMusicPageTransition() {
  if (lowPowerMode || screenFadeTransitionActive) return;
  if (musicPageShowRequested) {
    musicPageShowRequested = false;
    beginScreenFade(activeBrightness);
    hideAppMenu(false);
    showMusicPage(false);
    // The player expands invalidated areas to the whole screen while visible.
    // LVGL composes the frame in PSRAM using normal refresh mode, which remains
    // compatible with software rotation and avoids separated partial redraws.
    setMusicWholeScreenRefresh(true);
    commitMusicFullScreen();
    endScreenFade(activeBrightness);
  } else if (musicPageHideRequested) {
    musicPageHideRequested = false;
    beginScreenFade(activeBrightness);
    hideMusicPage(false, true);
    commitMusicFullScreen();
    setMusicWholeScreenRefresh(false);
    endScreenFade(activeBrightness);
  }
}

lv_obj_t *createMusicControlButton(lv_obj_t *parent, lv_coord_t size,
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
  lv_obj_set_style_bg_color(button, lv_color_hex(themeAccent(UI_ACCENT_VIOLET)),
                            LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
  createTablerIcon(button, iconKind, iconSize, themeIcon());
  return button;
}

void createMusicPlaylistPanel() {
  musicPlaylistPanel = lv_obj_create(musicPage);
  lv_obj_remove_style_all(musicPlaylistPanel);
  lv_obj_set_size(musicPlaylistPanel, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_pos(musicPlaylistPanel, 0, 0);
  lv_obj_set_style_bg_color(musicPlaylistPanel, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(musicPlaylistPanel, LV_OPA_COVER, 0);
  lv_obj_add_flag(musicPlaylistPanel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(musicPlaylistPanel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(musicPlaylistPanel, appPageGestureEvent,
                      LV_EVENT_GESTURE, nullptr);

  lv_obj_t *title = lv_label_create(musicPlaylistPanel);
  i18nRegister(title, STR_PLAYLIST);
  lv_obj_set_style_text_font(title, &lv_font_ui_24, 0);
  lv_obj_set_style_text_letter_space(title, 3, 0);
  lv_obj_set_style_text_color(title, themeText(), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 25);

  musicPlaylistList = lv_obj_create(musicPlaylistPanel);
  lv_obj_set_pos(musicPlaylistList, 24, 75);
  lv_obj_set_size(musicPlaylistList, SCREEN_WIDTH - 48, SCREEN_HEIGHT - 94);
  lv_obj_set_flex_flow(musicPlaylistList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(musicPlaylistList, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_all(musicPlaylistList, 0, 0);
  lv_obj_set_style_pad_row(musicPlaylistList, 10, 0);
  lv_obj_set_style_bg_opa(musicPlaylistList, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(musicPlaylistList, 0, 0);
  lv_obj_set_style_width(musicPlaylistList, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(musicPlaylistList, lv_color_hex(themeAccent(UI_ACCENT_VIOLET)),
                            LV_PART_SCROLLBAR);
  lv_obj_add_flag(musicPlaylistList, LV_OBJ_FLAG_GESTURE_BUBBLE);

  lv_obj_add_flag(musicPlaylistPanel, LV_OBJ_FLAG_HIDDEN);
}

void createMusicPage(lv_obj_t *screen) {
  musicPage = lv_obj_create(screen);
  lv_obj_remove_style_all(musicPage);
  lv_obj_set_pos(musicPage, 0, 0);
  lv_obj_set_size(musicPage, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(musicPage, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(musicPage, LV_OPA_COVER, 0);
  lv_obj_clear_flag(musicPage,
                    LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(musicPage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(musicPage, appPageGestureEvent, LV_EVENT_GESTURE, nullptr);

  musicTrackLabel = lv_label_create(musicPage);
  lv_obj_set_size(musicTrackLabel, SCREEN_WIDTH - 64, 96);
  // A static ellipsis avoids continuous full-width redraws while decoding.
  lv_label_set_long_mode(musicTrackLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(musicTrackLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(musicTrackLabel, &lv_font_ui_32, 0);
  lv_obj_set_style_text_color(musicTrackLabel, themeText(), 0);
  lv_obj_align(musicTrackLabel, LV_ALIGN_TOP_MID, 0, 48);

  musicStatusLabel = lv_label_create(musicPage);
  lv_obj_set_size(musicStatusLabel, 230, 30);
  lv_obj_set_style_text_font(musicStatusLabel, &lv_font_ui_18, 0);
  lv_obj_set_style_text_color(musicStatusLabel,
                              lv_color_hex(themeAccent(UI_ACCENT_VIOLET)), 0);
  lv_obj_set_style_text_align(musicStatusLabel, LV_TEXT_ALIGN_LEFT, 0);
  lv_obj_align(musicStatusLabel, LV_ALIGN_TOP_LEFT, 40, 178);

  musicCountLabel = lv_label_create(musicPage);
  lv_obj_set_size(musicCountLabel, 150, 30);
  lv_obj_set_style_text_font(musicCountLabel, &lv_font_ui_16, 0);
  lv_obj_set_style_text_color(musicCountLabel, themeTextDim(0x777D8D), 0);
  lv_obj_set_style_text_align(musicCountLabel, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(musicCountLabel, LV_ALIGN_TOP_RIGHT, -40, 180);

  musicProgressBar = lv_bar_create(musicPage);
  lv_obj_set_pos(musicProgressBar, 40, 242);
  lv_obj_set_size(musicProgressBar, SCREEN_WIDTH - 80, 12);
  lv_bar_set_range(musicProgressBar, 0, 100);
  lv_bar_set_value(musicProgressBar, 0, LV_ANIM_OFF);
  lv_obj_set_style_radius(musicProgressBar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(musicProgressBar, lv_color_hex(themeSurface(0x1A1A1A)),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(musicProgressBar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(musicProgressBar, LV_RADIUS_CIRCLE,
                          LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(musicProgressBar, lv_color_hex(themeAccent(UI_ACCENT_VIOLET)),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(musicProgressBar, LV_OPA_COVER,
                          LV_PART_INDICATOR);

  lv_obj_t *modeButton = createMusicControlButton(
    musicPage, 58, -176, 352, musicPlaybackModeEvent, TABLER_ICON_REPEAT, 28);
  musicModeIcons[MUSIC_MODE_REPEAT_ALL] = lv_obj_get_child(
    modeButton, lv_obj_get_child_cnt(modeButton) - 1);
  musicModeIcons[MUSIC_MODE_REPEAT_ONE] = createTablerIcon(
    modeButton, TABLER_ICON_REPEAT_ONCE, 28, themeIcon());
  musicModeIcons[MUSIC_MODE_SHUFFLE] = createTablerIcon(
    modeButton, TABLER_ICON_SHUFFLE, 28, themeIcon());
  createMusicControlButton(musicPage, 58, -88, 352, musicPreviousEvent,
                           TABLER_ICON_SKIP_BACK, 28);
  lv_obj_t *playButton = createMusicControlButton(
    musicPage, 74, 0, 344, musicPlayPauseEvent, TABLER_ICON_PLAY, 32);
  lv_obj_set_style_bg_color(playButton, lv_color_hex(themeAccent(UI_ACCENT_VIOLET)),
                            LV_PART_MAIN);
  lv_obj_set_style_border_width(playButton, themeIsAmber() ? 2 : 0, LV_PART_MAIN);
  lv_obj_set_style_border_color(playButton, lv_color_hex(CRT_AMBER), LV_PART_MAIN);
  musicPlayIcon = lv_obj_get_child(playButton, lv_obj_get_child_cnt(playButton) - 1);
  musicPauseIcon = createTablerIcon(playButton, TABLER_ICON_PAUSE, 32,
    themeTextOnAccent());
  if (musicPlayIcon)
    setTablerIconColor(musicPlayIcon,
      themeTextOnAccent());
  lv_obj_add_flag(musicPauseIcon, LV_OBJ_FLAG_HIDDEN);
  createMusicControlButton(musicPage, 58, 88, 352, musicNextEvent,
                           TABLER_ICON_SKIP_FORWARD, 28);
  createMusicControlButton(musicPage, 58, 176, 352, musicPlaylistEvent,
                           TABLER_ICON_PLAYLIST, 28);
  updateMusicPlaybackModeIcon();

  setMusicStatus(STR_INSERT_SD);
  createMusicPlaylistPanel();

  lv_obj_add_flag(musicPage, LV_OBJ_FLAG_HIDDEN);
}

// Re-apply the active theme to the music player and its playlist overlay.
void refreshMusicTheme() {
  if (!musicPage) return;
  lv_obj_set_style_bg_color(musicPage, themePageBackground(), 0);
  if (musicTrackLabel) lv_obj_set_style_text_color(musicTrackLabel, themeText(), 0);
  if (musicStatusLabel)
    lv_obj_set_style_text_color(musicStatusLabel,
      lv_color_hex(themeAccent(UI_ACCENT_VIOLET)), 0);
  if (musicCountLabel)
    lv_obj_set_style_text_color(musicCountLabel, themeTextDim(0x777D8D), 0);
  if (musicProgressBar) {
    lv_obj_set_style_bg_color(musicProgressBar, lv_color_hex(themeSurface(0x1A1A1A)),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(musicProgressBar,
      lv_color_hex(themeAccent(UI_ACCENT_VIOLET)), LV_PART_INDICATOR);
  }
  const uint32_t childCount = lv_obj_get_child_cnt(musicPage);
  for (uint32_t i = 0; i < childCount; ++i) {
    lv_obj_t *child = lv_obj_get_child(musicPage, i);
    if (!lv_obj_check_type(child, &lv_btn_class)) continue;
    const bool isPlayButton = lv_obj_get_width(child) == 74;
    if (isPlayButton) {
      lv_obj_set_style_bg_color(child, lv_color_hex(themeAccent(UI_ACCENT_VIOLET)),
                                LV_PART_MAIN);
      lv_obj_set_style_border_width(child, themeIsAmber() ? 2 : 0, LV_PART_MAIN);
      lv_obj_set_style_border_color(child, lv_color_hex(CRT_AMBER), LV_PART_MAIN);
      for (uint32_t j = 0; j < lv_obj_get_child_cnt(child); ++j)
        setTablerIconColor(lv_obj_get_child(child, j),
          themeTextOnAccent());
    } else {
      lv_obj_set_style_bg_color(child, lv_color_hex(themeSurface(0x0A0A0A)),
                                LV_PART_MAIN);
      lv_obj_set_style_border_color(child, lv_color_hex(themeBorder(0x343846)),
                                    LV_PART_MAIN);
      lv_obj_set_style_bg_color(child, lv_color_hex(themeAccent(UI_ACCENT_VIOLET)),
                                LV_PART_MAIN | LV_STATE_PRESSED);
      for (uint32_t j = 0; j < lv_obj_get_child_cnt(child); ++j)
        setTablerIconColor(lv_obj_get_child(child, j), themeIcon());
    }
  }
  if (musicPlaylistPanel) {
    lv_obj_set_style_bg_color(musicPlaylistPanel, themePageBackground(), 0);
    if (lv_obj_get_child_cnt(musicPlaylistPanel))
      lv_obj_set_style_text_color(lv_obj_get_child(musicPlaylistPanel, 0),
                                  themeText(), 0);
    if (musicPlaylistList)
      lv_obj_set_style_bg_color(musicPlaylistList,
        lv_color_hex(themeAccent(UI_ACCENT_VIOLET)), LV_PART_SCROLLBAR);
  }
  refreshMusicPlaylistStyles();
}
