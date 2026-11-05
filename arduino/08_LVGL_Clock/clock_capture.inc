// ES7210 stereo capture, shared by voice memos and the sampler. The playback
// TX channel remains allocated; RX joins it in full duplex on the same clocks.
#include "third_party/es7210/es7210.h"
#include <driver/i2s_std.h>

constexpr uint32_t CAPTURE_RATE = 16000;
using CaptureSink = bool (*)(const int16_t *, uint16_t);
i2s_chan_handle_t captureRx = nullptr;
es7210_dev_handle_t captureCodec = nullptr;
CaptureSink captureSink = nullptr;
bool captureRunning = false;
uint32_t captureFrames = 0, capturePeak = 0, captureErrors = 0;

bool beginMicrophoneCapture(CaptureSink sink) {
  if (captureRunning || !sink) return false;
  stopExtensionAudio();
  stopMusicPlayback();
  stopAudiobookPlayback();
  stopRadioPlayback();
  if (videoAudioActive) stopVideoAudioSourceKeepingOutput();
  if (!initializeMusicCodec() || !musicOutput) return false;
  // Both directions of one I2S controller must use identical clock/slot config.
  musicOutput->stop();
  if (!musicOutput->SetRate(CAPTURE_RATE) || !musicOutput->SetChannels(2) ||
      !musicOutput->begin()) return false;
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  channel.dma_desc_num = 6;
  channel.dma_frame_num = 256;
  if (i2s_new_channel(&channel, nullptr, &captureRx) != ESP_OK || !captureRx)
    return false;
  i2s_std_config_t config = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CAPTURE_RATE),
    .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = static_cast<gpio_num_t>(I2S_MCLK_PIN),
      .bclk = static_cast<gpio_num_t>(I2S_BCLK_PIN),
      .ws = static_cast<gpio_num_t>(I2S_WS_PIN),
      .dout = static_cast<gpio_num_t>(I2S_DOUT_PIN),
      .din = static_cast<gpio_num_t>(PIN_ES7210_DIN),
      .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
    },
  };
#if defined(SOC_CLK_APLL_SUPPORTED) && SOC_CLK_APLL_SUPPORTED
  config.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
#endif
  config.slot_cfg.bit_shift = true;
  bool configured = i2s_channel_init_std_mode(captureRx, &config) == ESP_OK;
  if (configured) configured = i2s_channel_enable(captureRx) == ESP_OK;
  if (!configured) {
    i2s_del_channel(captureRx);
    captureRx = nullptr;
    return false;
  }
  const es7210_i2c_config_t i2cConfig = {.i2c_port = I2C_NUM_0, .i2c_addr = 0x40};
  esp_err_t result = es7210_new_codec(&i2cConfig, &captureCodec);
  if (result == ESP_OK) {
    const es7210_codec_config_t codecConfig = {
      .sample_rate_hz = CAPTURE_RATE, .mclk_ratio = 256,
      .i2s_format = ES7210_I2S_FMT_I2S, .bit_width = ES7210_I2S_BITS_16B,
      .mic_bias = ES7210_MIC_BIAS_2V87, .mic_gain = ES7210_MIC_GAIN_24DB,
      .flags = {.tdm_enable = 0},
    };
    result = es7210_config_codec(captureCodec, &codecConfig);
    if (result == ESP_OK) result = es7210_config_volume(captureCodec, 0);
  }
  if (result != ESP_OK) {
    Serial.printf("Capture: ES7210 unavailable at 0x40: %s\n", esp_err_to_name(result));
    i2s_channel_disable(captureRx);
    i2s_del_channel(captureRx);
    captureRx = nullptr;
    if (captureCodec) es7210_del_codec(captureCodec);
    captureCodec = nullptr;
    return false;
  }
  captureSink = sink;
  captureFrames = capturePeak = captureErrors = 0;
  captureRunning = true;
  return true;
}
void serviceMicrophoneCapture() {
  if (!captureRunning || !captureRx) return;
  int16_t samples[512]; // 256 stereo frames; bounded stack and SD write.
  size_t received = 0;
  if (i2s_channel_read(captureRx, samples, sizeof(samples), &received, 0) != ESP_OK)
    return;
  const uint16_t count = received / sizeof(int16_t);
  uint32_t peak = 0;
  for (uint16_t i = 0; i < count; ++i)
    peak = max(peak, static_cast<uint32_t>(abs(static_cast<int32_t>(samples[i]))));
  capturePeak = peak;
  captureFrames += count / 2;
  if (captureSink && !captureSink(samples, count / 2)) ++captureErrors;
}
void stopMicrophoneCapture() {
  captureRunning = false;
  captureSink = nullptr;
  if (captureRx) {
    i2s_channel_disable(captureRx);
    i2s_del_channel(captureRx);
    captureRx = nullptr;
  }
  if (captureCodec) {
    es7210_del_codec(captureCodec);
    captureCodec = nullptr;
  }
  if (musicOutput) musicOutput->stop();
}
