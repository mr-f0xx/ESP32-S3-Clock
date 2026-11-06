// Pocket stereo recorder. 16 kHz / 16-bit PCM WAV, streamed to TF rather than
// accumulating an unbounded recording in RAM. ES7210 MIC1/MIC2 are L/R.
constexpr uint8_t RECORDER_FILE_LIMIT = 40;
String recorderFiles[RECORDER_FILE_LIMIT];
uint8_t recorderCount = 0, recorderIndex = 0, recorderListPage = 0;
bool recorderRecording = false, recorderPlaying = false;
File recorderFile;
ClockSdFileSource *recorderSource = nullptr;
AudioGeneratorWAV *recorderGenerator = nullptr;
uint32_t recorderDataBytes = 0, recorderStartedAt = 0, recorderLastUiAt = 0;
lv_obj_t *recorderStatus = nullptr, *recorderLevel = nullptr,
          *recorderRows[5] = {}, *recorderRecordButton = nullptr;

bool recorderBusy() { return recorderRecording || recorderPlaying || captureRunning; }
void recorderWavHeader(uint8_t header[44], uint32_t dataBytes) {
  memcpy(header, "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0"
    "\x80\x3e\0\0\0\xfa\0\0\x04\0\x10\0data\0\0\0\0", 44);
  auto le32 = [&](uint8_t *p, uint32_t v) { for (uint8_t i = 0; i < 4; ++i) p[i] = v >> (8 * i); };
  le32(header + 4, dataBytes + 36); le32(header + 40, dataBytes);
}
bool recorderWriteSink(const int16_t *samples, uint16_t frames) {
  if (!recorderRecording || !recorderFile) return false;
  const size_t wanted = frames * 2 * sizeof(int16_t);
  const size_t written = recorderFile.write(reinterpret_cast<const uint8_t *>(samples), wanted);
  recorderDataBytes += written;
  // Keep each memo below FAT's 4 GiB limit and leave room for other apps.
  return written == wanted && recorderDataBytes < 512UL * 1024UL * 1024UL;
}
void recorderScan() {
  recorderCount = 0;
  if (!extSdReady()) return;
  if (!storageFs().exists("/memos")) storageFs().mkdir("/memos");
  File directory = storageFs().open("/memos");
  File entry;
  while (directory && recorderCount < RECORDER_FILE_LIMIT && (entry = directory.openNextFile())) {
    String name = entry.name();
    if (!entry.isDirectory() && name.endsWith(".wav")) {
      if (!name.startsWith("/")) name = String("/memos/") + name;
      recorderFiles[recorderCount++] = name;
    }
    entry.close();
  }
  if (directory) directory.close();
  for (uint8_t i = 0; i < recorderCount; ++i)
    for (uint8_t j = i + 1; j < recorderCount; ++j)
      if (recorderFiles[j] > recorderFiles[i]) std::swap(recorderFiles[i], recorderFiles[j]);
  if (recorderIndex >= recorderCount) recorderIndex = 0;
}
void recorderUpdateRows() {
  for (uint8_t i = 0; i < 5; ++i) {
    if (!recorderRows[i]) continue;
    const uint8_t index = recorderListPage * 5 + i;
    String text = index < recorderCount ? recorderFiles[index].substring(7) : "-";
    lv_label_set_text(lv_obj_get_child(recorderRows[i], 0), text.c_str());
    if (index < recorderCount) lv_obj_clear_state(recorderRows[i], LV_STATE_DISABLED);
    else lv_obj_add_state(recorderRows[i], LV_STATE_DISABLED);
  }
}
void stopRecorderActivity() {
  if (recorderRecording) {
    recorderRecording = false;
    stopMicrophoneCapture();
    uint8_t header[44]; recorderWavHeader(header, recorderDataBytes);
    if (recorderFile) {
      recorderFile.seek(0); recorderFile.write(header, 44);
      recorderFile.flush(); recorderFile.close();
    }
  }
  if (captureRunning && captureSink == recorderWriteSink) stopMicrophoneCapture();
  if (recorderGenerator) { recorderGenerator->stop(); delete recorderGenerator; recorderGenerator = nullptr; }
  if (recorderSource) { delete recorderSource; recorderSource = nullptr; }
  recorderPlaying = false;
  if (musicOutput) musicOutput->stop();
}
bool recorderStart() {
  stopRecorderActivity();
  if (!extSdReady()) {
    if (recorderStatus) lv_label_set_text(recorderStatus, "Insert / unlock the TF card first");
    return false;
  }
  if (!storageFs().exists("/memos")) storageFs().mkdir("/memos");
  RTC_DateTime now;
  char path[64];
  if (rtcReady && readRtcTime(now))
    snprintf(path, sizeof(path), "/memos/%04u%02u%02u-%02u%02u%02u-%lu.wav",
      now.getYear(), now.getMonth(), now.getDay(), now.getHour(), now.getMinute(), now.getSecond(), millis());
  else snprintf(path, sizeof(path), "/memos/memo-%lu.wav", millis());
  recorderFile = storageFs().open(path, FILE_WRITE);
  if (!recorderFile) return false;
  uint8_t header[44]; recorderWavHeader(header, 0);
  recorderFile.write(header, 44);
  recorderDataBytes = 0;
  if (!beginMicrophoneCapture(recorderWriteSink)) {
    recorderFile.close(); storageFs().remove(path);
    if (recorderStatus) lv_label_set_text(recorderStatus, "ES7210 microphone unavailable (I2C 0x40 / I2S RX)");
    return false;
  }
  recorderRecording = true; recorderStartedAt = millis();
  if (recorderRecordButton) lv_label_set_text(lv_obj_get_child(recorderRecordButton, 0), "Stop recording");
  if (recorderStatus) lv_label_set_text(recorderStatus, "Recording stereo - keep this app open");
  return true;
}
bool recorderPlay(uint8_t index) {
  stopExtensionAudio(); stopMusicPlayback(); stopAudiobookPlayback(); stopRadioPlayback();
  if (index >= recorderCount || !extSdReady()) return false;
  recorderSource = new ClockSdFileSource(recorderFiles[index].c_str());
  if (!recorderSource->isOpen() || !initializeMusicCodec()) { stopRecorderActivity(); return false; }
  recorderGenerator = new AudioGeneratorWAV();
  if (!recorderGenerator->begin(recorderSource, musicOutput)) { stopRecorderActivity(); return false; }
  recorderIndex = index; recorderPlaying = true;
  digitalWrite(AUDIO_PA_PIN, HIGH); applyAudioVolume();
  if (recorderStatus) lv_label_set_text(recorderStatus, "Playing voice memo");
  return true;
}
void recorderRecordEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  if (recorderRecording) { stopRecorderActivity(); recorderScan(); recorderUpdateRows(); }
  else recorderStart();
  if (!recorderRecording) {
    if (recorderRecordButton) lv_label_set_text(lv_obj_get_child(recorderRecordButton, 0), "Record memo");
    if (recorderStatus) lv_label_set_text(recorderStatus, "Saved to /memos on TF");
  }
}
void recorderRowEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  recorderPlay(recorderListPage * 5 + reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
}
void recorderActionEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  const uintptr_t action = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  if (action == 0) { if (!recorderRecording) recorderPlay(recorderIndex); }
  else if (action == 1) { stopRecorderActivity(); if (recorderStatus) lv_label_set_text(recorderStatus, "Stopped"); }
  else if (action == 2) { recorderListPage = max(0, static_cast<int>(recorderListPage) - 1); recorderUpdateRows(); }
  else if (action == 3) { if ((recorderListPage + 1) * 5 < recorderCount) ++recorderListPage; recorderUpdateRows(); }
  else if (action == 4 && !recorderBusy() && recorderIndex < recorderCount && extSdReady()) {
    storageFs().remove(recorderFiles[recorderIndex].c_str()); recorderScan(); recorderUpdateRows();
  }
}
void createRecorderPage() {
  lv_obj_t *page = extPage(EXT_RECORDER, "Voice memos", 0xFF7B9C);
  recorderStatus = extLabel(page, "Dual microphone - stereo WAV on TF", 24, 90, 432, 18);
  lv_obj_set_height(recorderStatus, 44);
  recorderLevel = lv_bar_create(page);
  lv_obj_set_pos(recorderLevel, 24, 140); lv_obj_set_size(recorderLevel, 432, 12);
  lv_bar_set_range(recorderLevel, 0, 32768);
  lv_obj_set_style_bg_color(recorderLevel, themeControlSurface(0xFF7B9C), LV_PART_MAIN);
  lv_obj_set_style_bg_color(recorderLevel, lv_color_hex(extensionAccent), LV_PART_INDICATOR);
  recorderRecordButton = extButton(page, "Record memo", 24, 168, 272, 56, recorderRecordEvent, 0, true);
  extButton(page, "Stop", 312, 168, 144, 56, recorderActionEvent, 1);
  for (uint8_t i = 0; i < 5; ++i) {
    recorderRows[i] = extButton(page, "-", 24, 240 + i * 36, 432, 32, recorderRowEvent, i);
    lv_obj_t *label = lv_obj_get_child(recorderRows[i], 0);
    lv_obj_set_width(label, 404); lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, &lv_font_ui_16, 0);
  }
  extButton(page, "Play", 24, 426, 100, 44, recorderActionEvent, 0);
  extButton(page, "Delete", 136, 426, 112, 44, recorderActionEvent, 4);
  extButton(page, "<", 344, 426, 52, 44, recorderActionEvent, 2);
  extButton(page, ">", 404, 426, 52, 44, recorderActionEvent, 3);
  recorderScan(); recorderUpdateRows();
}
void serviceRecorder() {
  if (recorderRecording) {
    serviceMicrophoneCapture();
    if (captureErrors || millis() - recorderStartedAt > 30UL * 60UL * 1000UL) {
      stopRecorderActivity(); recorderScan(); recorderUpdateRows();
      if (recorderRecordButton) lv_label_set_text(lv_obj_get_child(recorderRecordButton, 0), "Record memo");
      if (recorderStatus) lv_label_set_text(recorderStatus, captureErrors ? "Recording stopped: TF write error" : "30-minute safety limit reached; memo saved");
    }
    if (recorderLevel) lv_bar_set_value(recorderLevel, min<uint32_t>(capturePeak, 32768), LV_ANIM_OFF);
    if (recorderStatus && recorderRecording && millis() - recorderLastUiAt >= 500) {
      recorderLastUiAt = millis();
      const uint32_t seconds = captureFrames / CAPTURE_RATE;
      lv_label_set_text_fmt(recorderStatus, "REC  %02lu:%02lu  -  stereo 16 kHz", seconds / 60, seconds % 60);
    }
  }
  if (recorderPlaying && recorderGenerator && !recorderGenerator->loop()) {
    stopRecorderActivity();
    if (recorderStatus) lv_label_set_text(recorderStatus, "Memo finished");
  }
}
void cleanupRecorder() {
  stopRecorderActivity();
  recorderStatus = recorderLevel = recorderRecordButton = nullptr;
  for (lv_obj_t *&row : recorderRows) row = nullptr;
}
