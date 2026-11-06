// MPC-style 16-pad sampler. Banks live only in PSRAM while this app is open.
// Capacitive touch has no pressure sensor: velocity is mapped from hold time.
#include "src/wav_pcm.h"
constexpr uint8_t SAMPLER_PADS = 16, SAMPLER_VOICES = 16;
struct SamplerBank { int16_t *samples = nullptr; uint32_t count = 0, capacity = 0; };
struct SamplerVoice { uint8_t pad = 255; uint32_t position = 0, velocity = 256; int32_t filter = 0; };
SamplerBank samplerBanks[SAMPLER_PADS];
SamplerVoice samplerVoices[SAMPLER_VOICES];
lv_obj_t *samplerPads[SAMPLER_PADS] = {}, *samplerStatus = nullptr, *samplerEffects = nullptr;
uint8_t samplerSelected = 0;
bool samplerArmed = false, samplerRecording = false, samplerOutputRunning = false;
uint32_t samplerPressedAt[SAMPLER_PADS] = {}, samplerRingUntil[SAMPLER_PADS] = {};
uint32_t samplerPitch = 65536, samplerCutoff = 32768, samplerBudget = 0;

bool samplerBusy() {
  if (samplerRecording) return true;
  for (const SamplerVoice &voice : samplerVoices) if (voice.pad != 255) return true;
  return false;
}
void samplerUpdateEffects() {
  if (samplerEffects) lv_label_set_text_fmt(samplerEffects, "Pitch %+d st   Filter %lu%%   Pad %02u",
    static_cast<int>((samplerPitch >> 12) - 16), samplerCutoff * 100 / 32768, samplerSelected + 1);
}
bool samplerAllocate() {
  if (samplerBanks[0].samples) return true;
  const size_t available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (available < 3UL * 1024UL * 1024UL) return false;
  samplerBudget = min<size_t>(6UL * 1024UL * 1024UL, available - 1536UL * 1024UL);
  samplerBudget &= ~65535UL;
  const uint32_t capacity = samplerBudget / SAMPLER_PADS / sizeof(int16_t);
  for (SamplerBank &bank : samplerBanks) {
    bank.samples = static_cast<int16_t *>(heap_caps_calloc(capacity, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!bank.samples) {
      for (SamplerBank &allocated : samplerBanks) {
        if (allocated.samples) free(allocated.samples);
        allocated = SamplerBank();
      }
      samplerBudget = 0;
      return false;
    }
    bank.capacity = capacity;
  }
  return true;
}
bool loadSamplerWav(uint8_t pad, const char *path) {
  if (pad >= SAMPLER_PADS || !samplerBanks[pad].samples || !extSdReady()) return false;
  File file = storageFs().open(path, FILE_READ);
  if (!file || file.size() < 44 || file.size() > 64UL * 1024UL * 1024UL) { if (file) file.close(); return false; }
  uint8_t header[12];
  if (file.read(header, 12) != 12 || memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) { file.close(); return false; }
  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t rate = 0, dataOffset = 0, dataBytes = 0;
  while (file.position() + 8 <= file.size()) {
    const uint32_t position = file.position();
    uint8_t chunk[8]; if (file.read(chunk, 8) != 8) break;
    const uint32_t size = WavPcm::u32(chunk + 4);
    if (size > file.size() - position - 8) break;
    if (!memcmp(chunk, "fmt ", 4) && size >= 16) {
      uint8_t fmt[40]; const uint32_t n = min<uint32_t>(size, sizeof(fmt));
      if (file.read(fmt, n) != n) break;
      format = WavPcm::u16(fmt); channels = WavPcm::u16(fmt + 2);
      rate = WavPcm::u32(fmt + 4); bits = WavPcm::u16(fmt + 14);
    } else if (!memcmp(chunk, "data", 4)) { dataOffset = position + 8; dataBytes = size; }
    file.seek(position + 8 + size + (size & 1));
  }
  if (!WavPcm::supported(format, channels, bits) || rate < 4000 || rate > 192000 || !dataBytes) { file.close(); return false; }
  SamplerBank &bank = samplerBanks[pad];
  bank.count = 0;
  file.seek(dataOffset);
  const uint16_t frameBytes = channels * bits / 8;
  const uint64_t sourceFrames = dataBytes / frameBytes;
  // Zero-order resampling to the common 16 kHz engine. Bound both source reads and
  // destination writes; a long TF file is truncated to this pad's RAM quota.
  uint8_t bytes[8];
  uint64_t lastSource = 0;
  int16_t previous = 0;
  for (uint32_t out = 0; out < bank.capacity; ++out) {
    const uint64_t source = static_cast<uint64_t>(out) * rate / CAPTURE_RATE;
    if (source >= sourceFrames) break;
    while (lastSource <= source) {
      if (file.read(bytes, frameBytes) != frameBytes) { file.close(); return bank.count > 0; }
      int32_t sum = 0;
      for (uint8_t channel = 0; channel < channels; ++channel)
        sum += WavPcm::decodeSample(bytes + channel * bits / 8, bits, format);
      previous = static_cast<int16_t>(sum / channels);
      ++lastSource;
    }
    bank.samples[bank.count++] = previous;
  }
  file.close();
  return bank.count > 0;
}
void samplerLoadAll() {
  if (!samplerAllocate()) {
    if (samplerStatus) lv_label_set_text(samplerStatus, "Need at least 3 MB free PSRAM; close other apps / restart");
    return;
  }
  uint8_t loaded = 0;
  for (uint8_t i = 0; i < SAMPLER_PADS; ++i) {
    char path[40]; snprintf(path, sizeof(path), "/samples/%02u.wav", i + 1);
    if (storageFs().exists(path) && loadSamplerWav(i, path)) ++loaded;
    lv_timer_handler();
  }
  if (samplerStatus) lv_label_set_text_fmt(samplerStatus, "%u / 16 pads loaded - /samples/01.wav ... 16.wav", loaded);
}
void stopSamplerPlayback() {
  if (samplerRecording) {
    samplerRecording = false;
    stopMicrophoneCapture();
  }
  for (SamplerVoice &voice : samplerVoices) voice.pad = 255;
  samplerOutputRunning = false;
  if (musicOutput) musicOutput->stop();
}
void samplerTrigger(uint8_t pad, uint32_t velocity) {
  if (pad >= SAMPLER_PADS || !samplerBanks[pad].count || samplerRecording) return;
  if (!samplerOutputRunning) {
    if (!initializeMusicCodec() || !musicOutput->SetRate(CAPTURE_RATE) ||
        !musicOutput->SetChannels(2) || !musicOutput->begin()) return;
    samplerOutputRunning = true; digitalWrite(AUDIO_PA_PIN, HIGH); applyAudioVolume();
  }
  uint8_t slot = SAMPLER_VOICES;
  for (uint8_t i = 0; i < SAMPLER_VOICES; ++i) {
    if (samplerVoices[i].pad == 255) { slot = i; break; }
    if (samplerVoices[i].pad == pad) { slot = i; break; }
  }
  if (slot == SAMPLER_VOICES) slot = 0; // bounded polyphony; steal oldest slot
  samplerVoices[slot] = {pad, 0, velocity, 0};
  samplerRingUntil[pad] = millis() + 220 + velocity / 2;
}
bool samplerCaptureSink(const int16_t *stereo, uint16_t frames) {
  if (!samplerRecording) return false;
  SamplerBank &bank = samplerBanks[samplerSelected];
  for (uint16_t i = 0; i < frames && bank.count < bank.capacity; ++i)
    bank.samples[bank.count++] = static_cast<int16_t>(
      (static_cast<int32_t>(stereo[i * 2]) + stereo[i * 2 + 1]) / 2);
  return bank.count < bank.capacity;
}
void samplerPadEvent(lv_event_t *event) {
  const uint8_t pad = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  if (pad >= SAMPLER_PADS) return;
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_PRESSED) {
    samplerSelected = pad; samplerPressedAt[pad] = millis(); samplerUpdateEffects();
    if (samplerPads[pad]) lv_obj_set_style_border_width(samplerPads[pad], 4, 0);
  } else if (code == LV_EVENT_RELEASED) {
    const uint32_t held = min<uint32_t>(millis() - samplerPressedAt[pad], 600);
    samplerTrigger(pad, 96 + held * 160 / 600);
    if (samplerPads[pad]) lv_obj_set_style_border_width(samplerPads[pad], 3, 0);
  } else if (code == LV_EVENT_PRESS_LOST) {
    if (samplerPads[pad]) lv_obj_set_style_border_width(samplerPads[pad], 1, 0);
  }
}
void samplerActionEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  const uintptr_t action = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  if (action == 0) { stopSamplerPlayback(); samplerLoadAll(); }
  else if (action == 1) {
    if (samplerRecording) {
      stopSamplerPlayback();
      if (samplerStatus) lv_label_set_text(samplerStatus, "Sample captured in RAM - tap the pad to play");
    } else {
      stopExtensionAudio(); stopMusicPlayback(); stopAudiobookPlayback(); stopRadioPlayback();
      if (!samplerAllocate()) return;
      samplerBanks[samplerSelected].count = 0;
      if (beginMicrophoneCapture(samplerCaptureSink)) {
        samplerRecording = true;
        if (samplerStatus) lv_label_set_text(samplerStatus, "Live sampling dual mics - press Sample again to stop");
      } else if (samplerStatus) lv_label_set_text(samplerStatus, "ES7210 capture unavailable");
    }
  } else if (action == 2) { stopSamplerPlayback(); if (samplerStatus) lv_label_set_text(samplerStatus, "All voices stopped"); }
  else if (action == 3) {
    samplerPitch = 65536; samplerCutoff = 32768; samplerUpdateEffects();
  } else if (action == 4 && !samplerRecording && extSdReady() && samplerBanks[samplerSelected].count) {
    if (!storageFs().exists("/samples")) storageFs().mkdir("/samples");
    char path[40]; snprintf(path, sizeof(path), "/samples/%02u.wav", samplerSelected + 1);
    File file = storageFs().open(path, FILE_WRITE);
    if (file) {
      uint8_t header[44]; recorderWavHeader(header, samplerBanks[samplerSelected].count * 2);
      // Sampler recordings are mono: rewrite channel count and byte rate.
      header[22] = 1; header[32] = 2; header[34] = 16;
      header[24] = 0x80; header[25] = 0x3e; header[28] = 0; header[29] = 0x7d;
      file.write(header, 44);
      file.write(reinterpret_cast<const uint8_t *>(samplerBanks[samplerSelected].samples), samplerBanks[samplerSelected].count * 2);
      file.close();
      if (samplerStatus) lv_label_set_text(samplerStatus, "Pad saved to TF (mono WAV)");
    }
  }
}
void samplerGestureEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_GESTURE || lowPowerMode) return;
  lv_indev_t *input = lv_indev_get_act();
  if (!input) return;
  lv_point_t point; lv_indev_get_point(input, &point);
  lv_dir_t direction = lv_indev_get_gesture_dir(input);
  // Edge lanes are reserved for expression; central swipes do not change pads.
  if (point.x < 48) samplerPitch = constrain(samplerPitch + (direction == LV_DIR_TOP ? 4096 : -4096), 32768U, 131072U);
  else if (point.x > SCREEN_WIDTH - 48) samplerCutoff = constrain(samplerCutoff + (direction == LV_DIR_TOP ? 4096 : -4096), 2048U, 32768U);
  else return;
  lv_indev_reset(input, nullptr); samplerUpdateEffects();
}
void createSamplerPage() {
  lv_obj_t *page = extPage(EXT_SAMPLER, "Sample studio", 0xFFBD66);
  lv_obj_add_event_cb(page, samplerGestureEvent, LV_EVENT_GESTURE, nullptr);
  samplerStatus = extLabel(page, "16 TF pads - PSRAM voices - hold for velocity", 24, 86, 432, 16);
  lv_obj_set_height(samplerStatus, 38);
  samplerEffects = extLabel(page, "", 24, 124, 432, 18, true);
  for (uint8_t i = 0; i < SAMPLER_PADS; ++i) {
    char text[4]; snprintf(text, sizeof(text), "%02u", i + 1);
    samplerPads[i] = extButton(page, text, 50 + (i % 4) * 98, 158 + (i / 4) * 58,
                               86, 48, samplerPadEvent, i, i == 0);
    lv_obj_set_style_border_color(samplerPads[i], lv_color_hex(extensionAccent), 0);
    lv_obj_set_style_border_width(samplerPads[i], 1, 0);
    lv_obj_set_style_border_opa(samplerPads[i], LV_OPA_60, 0);
    lv_obj_add_event_cb(samplerPads[i], samplerPadEvent, LV_EVENT_PRESSED, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    lv_obj_add_event_cb(samplerPads[i], samplerPadEvent, LV_EVENT_RELEASED, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    lv_obj_add_event_cb(samplerPads[i], samplerPadEvent, LV_EVENT_PRESS_LOST, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
  }
  extButton(page, "Load TF", 24, 408, 100, 48, samplerActionEvent, 0);
  extButton(page, "Sample", 132, 408, 100, 48, samplerActionEvent, 1, true);
  extButton(page, "Save", 240, 408, 80, 48, samplerActionEvent, 4);
  extButton(page, "Stop", 328, 408, 76, 48, samplerActionEvent, 2);
  extButton(page, "Reset", 412, 408, 60, 48, samplerActionEvent, 3);
  samplerUpdateEffects();
}
void serviceSampler() {
  if (samplerRecording) {
    serviceMicrophoneCapture();
    if (captureErrors) {
      stopSamplerPlayback();
      if (samplerStatus) lv_label_set_text(samplerStatus, "Pad RAM full - release / play the sample");
    }
    if (samplerStatus && samplerRecording) lv_label_set_text_fmt(samplerStatus,
      "Sampling pad %02u: %.1f / %.1f s", samplerSelected + 1,
      samplerBanks[samplerSelected].count / 16000.0, samplerBanks[samplerSelected].capacity / 16000.0);
  }
  const uint32_t now = millis();
  for (uint8_t i = 0; i < SAMPLER_PADS; ++i) {
    if (!samplerPads[i] || !samplerRingUntil[i]) continue;
    const uint32_t remaining = static_cast<int32_t>(samplerRingUntil[i] - now);
    if (static_cast<int32_t>(remaining) <= 0) {
      lv_obj_set_style_border_width(samplerPads[i], 1, 0); samplerRingUntil[i] = 0;
    } else lv_obj_set_style_border_opa(samplerPads[i], min<uint32_t>(255, remaining), 0);
  }
  if (!samplerOutputRunning || !musicOutput) return;
  int16_t mix[128] = {};
  bool active = false;
  for (SamplerVoice &voice : samplerVoices) {
    if (voice.pad == 255) continue;
    const SamplerBank &bank = samplerBanks[voice.pad];
    for (uint8_t i = 0; i < 128 && voice.position >> 16 < bank.count; ++i) {
      const uint32_t index = voice.position >> 16;
      const int32_t dry = bank.samples[index];
      voice.filter += ((dry - voice.filter) * static_cast<int32_t>(samplerCutoff)) >> 15;
      mix[i] = static_cast<int16_t>(constrain(static_cast<int32_t>(mix[i]) +
        ((voice.filter * static_cast<int32_t>(voice.velocity)) >> 10), -32768, 32767));
      voice.position += samplerPitch;
    }
    if (voice.position >> 16 >= bank.count) voice.pad = 255;
    else active = true;
  }
  musicOutput->consumeMonoSamples(mix, 128);
  if (!active) {
    // Let the DMA tail drain naturally on the next service call before mute.
    static uint8_t idleBlocks = 0;
    if (++idleBlocks >= 6) { digitalWrite(AUDIO_PA_PIN, LOW); idleBlocks = 0; }
  }
}
void cleanupSampler() {
  stopSamplerPlayback();
  for (SamplerBank &bank : samplerBanks) {
    if (bank.samples) free(bank.samples);
    bank = SamplerBank();
  }
  samplerStatus = samplerEffects = nullptr;
  for (lv_obj_t *&pad : samplerPads) pad = nullptr;
  for (uint32_t &until : samplerRingUntil) until = 0;
}
