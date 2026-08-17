// Dynamic weather wallpaper mode adapted from
// 09_LVGL_Dynamic_Wallpaper_480x360_Playlist.
// Compressed frames are prefetched from TF, decoded into RGB565 DMA blocks,
// and submitted to the display independently from LVGL's UI flushes. Portrait
// playback uses pre-rotated JWP assets instead of a run-time pixel transform.

namespace WeatherWallpaper {

constexpr uint16_t VIDEO_WIDTH = 480;
constexpr uint16_t VIDEO_HEIGHT = 360;
constexpr uint16_t VIDEO_CORNER_RADIUS = 72;
constexpr uint32_t FRAME_BYTES =
  static_cast<uint32_t>(VIDEO_WIDTH) * VIDEO_HEIGHT * sizeof(uint16_t);
constexpr uint32_t MAX_JPEG_BYTES = 512UL * 1024UL;
constexpr uint32_t DISPLAY_DMA_BLOCK_BYTES =
  static_cast<uint32_t>(VIDEO_WIDTH) * 16U * sizeof(uint16_t);
// Two strips let JPEG decode and LCD transfer overlap. I2S reserves its much
// smaller DMA queue before this arena is allocated, avoiding fragmentation
// without reducing 24 fps video throughput.
constexpr uint8_t PIPELINE_BUFFER_COUNT = 2;
constexpr size_t PLAYBACK_DMA_ARENA_BYTES =
  PIPELINE_BUFFER_COUNT * DISPLAY_DMA_BLOCK_BYTES;
// Weather mode keeps the DMA strips and a sacrificial TLS reserve in one
// allocation. Releasing it before HTTPS gives mbedTLS a larger contiguous
// internal block. Video mode uses only the strips; I2S is reserved first.
constexpr size_t TLS_CONTIGUOUS_RESERVE_BYTES = 16U * 1024U;
constexpr size_t PLAYBACK_INTERNAL_ARENA_BYTES =
  PLAYBACK_DMA_ARENA_BYTES + TLS_CONTIGUOUS_RESERVE_BYTES;
// Keep small HTTP/String allocations out of the block that mbedTLS will use.
// The guard is allocated after URL/DNS preparation and released immediately
// before GET(), leaving two 16 KiB TLS record buffers plus working headroom in
// one contiguous internal-SRAM window.
// Runtime recovery after video consistently yields a 35 KiB largest block.
// Two 16 KiB TLS record buffers need just over 32 KiB including allocator
// metadata, so 34 KiB preserves useful safety margin without rejecting that
// valid post-video memory state.
constexpr size_t TLS_HANDSHAKE_WINDOW_BYTES = 34U * 1024U;
// Since the LVGL heap moved to PSRAM, a healthy weather-wallpaper session has
// enough contiguous SRAM to let low-priority HTTPS run beside JPEG playback.
// Keep additional headroom for the active DMA arena and task stacks; if this
// window is unavailable, defer the refresh instead of freezing the wallpaper.
constexpr size_t WEATHER_LIVE_TLS_WINDOW_BYTES =
  TLS_HANDSHAKE_WINDOW_BYTES + 32U * 1024U;
constexpr uint32_t WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL;
constexpr uint32_t WEATHER_RETRY_MS = 60UL * 1000UL;
constexpr uint32_t WEATHER_MEMORY_RETRY_MS = 10UL * 1000UL;
constexpr uint32_t WEATHER_BOOT_SETTLE_MS = 5UL * 1000UL;
// Two missed normal refreshes are enough to stop presenting cached weather as
// current. The network task keeps retrying while the UI falls back to unknown.
constexpr uint32_t WEATHER_STALE_MS = 20UL * 60UL * 1000UL;
// Air Quality API v1 returns richer payloads than weather v7. Keep its
// temporary inflate target in PSRAM so the larger response does not consume
// the contiguous internal SRAM reserved for TLS and I2S DMA.
constexpr size_t MAX_WEATHER_JSON_BYTES = 16 * 1024;
// The bundled TF layout can contain more than 90 landscape/portrait wallpaper
// pairs before any videos are counted. Keep enough registry space for those
// pairs plus a useful video playlist without consuming significant SRAM.
constexpr uint8_t MAX_WALLPAPER_FILES = 128;
constexpr uint32_t PIPELINE_QUEUE_WAIT_MS = 60;
constexpr uint32_t PIPELINE_STOP_TIMEOUT_MS = 1200;
constexpr uint32_t VIDEO_RESUME_SAVE_MS = 15000;
constexpr uint32_t VIDEO_AUDIO_RETRY_INITIAL_MS = 1000;
constexpr uint32_t VIDEO_AUDIO_RETRY_MAX_MS = 30000;
constexpr size_t SD_INTERRUPTIBLE_READ_BYTES = 4096;
constexpr uint8_t ASSET_FADE_STEPS = 5;
constexpr uint32_t ASSET_FADE_STEP_MS = 12;
constexpr uint32_t ASSET_FADE_FRAME_TIMEOUT_MS = 1500;

uint32_t pacedQueueWaitMs(uint16_t fpsTimes100) {
  uint32_t waitMs = PIPELINE_QUEUE_WAIT_MS;
  if (fpsTimes100 > 0) {
    const uint32_t framePeriodMs =
      (100000UL + fpsTimes100 - 1UL) / fpsTimes100;
    const uint32_t pacedWaitMs = framePeriodMs + 30UL;
    if (pacedWaitMs > waitMs) waitMs = pacedWaitMs;
  }
  const uint32_t maximumWaitMs =
    PIPELINE_STOP_TIMEOUT_MS > 50UL ? PIPELINE_STOP_TIMEOUT_MS - 50UL
                                    : PIPELINE_STOP_TIMEOUT_MS;
  if (waitMs > maximumWaitMs) waitMs = maximumWaitMs;
  return waitMs;
}

enum class WeatherScene : uint8_t {
  Clear, Cloudy, Overcast, Rain, Thunder, Snow, Fog, Haze, Sand, Night
};

struct WeatherWallpaperAsset {
  String key;
  String path;
  String rotatedPath;
  WeatherScene scene;
  bool video;
};

WeatherWallpaperAsset weatherWallpapers[MAX_WALLPAPER_FILES];
uint8_t weatherWallpaperCount = 0;
uint16_t mediaScanCapacityDrops = 0;

#pragma pack(push, 1)
struct WallpaperHeader {
  char magic[4];
  uint16_t version;
  uint16_t headerSize;
  uint16_t width;
  uint16_t height;
  uint16_t fpsTimes100;
  uint16_t pixelFormat;
  uint32_t frameCount;
  uint32_t maxFrameBytes;
  uint32_t payloadCrc32;
  uint32_t reserved;
};

struct MjpegIndexHeader {
  char magic[4];
  uint32_t frameCount;
};
#pragma pack(pop)

static_assert(sizeof(WallpaperHeader) == 32, "JWP1 header size changed");
static_assert(sizeof(MjpegIndexHeader) == 8, "MJP1 index header size changed");

struct CompressedFrame {
  uint8_t slot;
  uint8_t wallpaperIndex;
  uint32_t generation;
  uint32_t bytes;
  uint32_t index;
  uint32_t frameCount;
  uint16_t fpsTimes100;
  uint16_t width;
  uint16_t height;
  bool rotatedAsset;
  uint32_t fileOffset;
  uint64_t startedAtUs;
  uint64_t readUs;
};

struct ReadyVideoFrame {
  uint8_t slot;
  uint8_t wallpaperIndex;
  lv_disp_rot_t rotation;
  bool first;
  bool last;
  uint32_t generation;
  uint32_t index;
  uint32_t frameCount;
  uint16_t fpsTimes100;
  uint32_t fileOffset;
  uint16_t blockY;
  uint16_t rows;
  uint16_t sourceWidth;
  int16_t x;
  int16_t y;
  uint16_t width;
  uint16_t height;
  uint64_t startedAtUs;
  uint64_t readUs;
  uint64_t decodeUs;
  uint64_t decoderSetupUs;
  uint64_t headerUs;
  uint64_t processUs;
  uint64_t bufferWaitUs;
  uint64_t assembleUs;
};

struct VideoDisplayResult {
  uint8_t wallpaperIndex;
  lv_disp_rot_t rotation;
  uint32_t generation;
  uint32_t index;
  uint32_t frameCount;
  uint16_t fpsTimes100;
  uint32_t fileOffset;
  uint32_t sequence;
  uint64_t displayedAtUs;
  uint64_t pipelineUs;
  uint64_t readUs;
  uint64_t decodeUs;
  uint64_t decoderSetupUs;
  uint64_t headerUs;
  uint64_t processUs;
  uint64_t bufferWaitUs;
  uint64_t assembleUs;
  uint64_t lcdUs;
};

lv_obj_t *page = nullptr;
lv_obj_t *videoImage = nullptr;
lv_obj_t *statusPanel = nullptr;
lv_obj_t *clockLabel = nullptr;
lv_obj_t *temperatureLabel = nullptr;
lv_obj_t *weatherIconHolder = nullptr;
lv_obj_t *videoProgressBar = nullptr;
lv_obj_t *videoTimeLabel = nullptr;
lv_obj_t *videoNameLabel = nullptr;
lv_obj_t *messageLabel = nullptr;

WallpaperHeader wallpaperHeader{};
volatile bool playerVideoMode = false;
bool videoPlaybackUserPaused = false;
uint8_t *jpegBuffers[PIPELINE_BUFFER_COUNT]{};
lv_color_t *videoFrameBuffers[PIPELINE_BUFFER_COUNT]{};
uint8_t *playbackInternalArena = nullptr;
uint8_t bottomCornerInsets[VIDEO_CORNER_RADIUS]{};
jpeg_dec_handle_t jpegDecoder = nullptr;
jpeg_dec_io_t jpegIo{};
jpeg_dec_header_info_t jpegInfo{};
uint32_t frameIndex = 0;
uint32_t nextRandomizeAtMs = 0;
volatile uint8_t activeIndex = 0;
volatile uint8_t requestedReaderIndex = 0;
volatile bool requestedReaderRotated = false;
volatile uint32_t requestedReaderFrame = 0;
volatile uint32_t requestedReaderFileOffset = 0;
volatile uint32_t pipelineGeneration = 1;
volatile uint32_t displayedGeneration = 0;
volatile uint32_t displayedFrameSequence = 0;
volatile uint8_t displayedWallpaperIndex = UINT8_MAX;
volatile bool readerIdle = true;
volatile bool decoderIdle = true;
volatile bool displayIdle = true;
volatile bool pipelinePauseRequested = false;
volatile bool networkResourceHold = false;
volatile bool assetFadeNetworkCancelRequested = false;
int16_t requestedIndex = -1;
enum class AssetFadeState : uint8_t { Idle, FadeOut, WaitForFrame, FadeIn };
AssetFadeState assetFadeState = AssetFadeState::Idle;
int16_t assetFadeTargetIndex = -1;
int16_t assetFadeSourceIndex = -1;
uint32_t assetFadeStepDueAt = 0;
uint32_t assetFadeWaitStartedAt = 0;
uint8_t assetFadeStep = 0;
bool assetFadeFallbackAttempted = false;
bool initialized = false;
bool initializationFailed = false;
QueueHandle_t freeCompressedQueue = nullptr;
QueueHandle_t readyCompressedQueue = nullptr;
QueueHandle_t freeVideoQueue = nullptr;
QueueHandle_t readyVideoQueue = nullptr;
QueueHandle_t videoDisplayResultQueue = nullptr;
TaskHandle_t wallpaperReaderTaskHandle = nullptr;
TaskHandle_t wallpaperDisplayTaskHandle = nullptr;
TaskHandle_t wallpaperDecodeTaskHandle = nullptr;
volatile bool playbackErrorPending = false;
enum class PlaybackErrorCode : uint8_t {
  None,
  OpenFile,
  ReadHeader,
  InvalidHeader,
  Seek,
  FrameLength,
  FrameData,
  JpegDecode,
};
volatile PlaybackErrorCode playbackErrorCode = PlaybackErrorCode::None;
uint32_t lastMediaErrorLogAtMs = 0;
PlaybackErrorCode lastMediaErrorLogCode = PlaybackErrorCode::None;
bool videoSeekDragging = false;
uint32_t lastVideoSeekInteractionAtMs = 0;
uint8_t currentVideoPositionIndex = UINT8_MAX;
uint32_t currentVideoPositionFrame = 0;
uint32_t currentVideoPositionFileOffset = 0;
uint32_t currentVideoFrameCount = 0;
uint16_t currentVideoFpsTimes100 = 0;
uint32_t lastVideoResumeSaveAtMs = 0;
uint32_t videoNameHideAtMs = 0;
uint8_t displayedVideoNameIndex = UINT8_MAX;
uint8_t lastSavedVideoIndex = UINT8_MAX;
uint32_t lastSavedVideoFrame = UINT32_MAX;
uint8_t lastPersistedWeatherIndex = UINT8_MAX;
bool videoResumeSaveRequested = false;
uint32_t videoAudioGeneration = 0;
uint8_t videoAudioWallpaperIndex = UINT8_MAX;
uint32_t videoAudioLastFrame = 0;
bool videoAudioAttempted = false;
uint32_t videoAudioRetryAtMs = 0;
uint32_t videoAudioRetryDelayMs = VIDEO_AUDIO_RETRY_INITIAL_MS;
uint32_t videoAudioLastDriftCheckMs = 0;
uint8_t videoAudioDriftStrikes = 0;
uint32_t videoAudioLastVideoPositionMs = 0;
uint32_t videoAudioLastAudioPositionMs = 0;
int32_t videoAudioLastMeasuredDriftMs = 0;

portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool weatherPending = false;
volatile int16_t temperatureC = 0;
volatile uint16_t weatherCode = 999;
volatile uint32_t weatherLastSuccessAtMs = 0;
struct ForecastDay {
  char date[11];
  char text[28];
  int16_t minimumC;
  int16_t maximumC;
  uint16_t code;
  bool valid;
};
ForecastDay forecastDays[3]{};
constexpr uint8_t WEATHER_HOURLY_MAX_POINTS = 24;
constexpr uint8_t WEATHER_MINUTELY_MAX_POINTS = 24;
struct HourlyForecastPoint {
  char time[6];
  int16_t temperatureC;
};
HourlyForecastPoint hourlyForecast[WEATHER_HOURLY_MAX_POINTS]{};
uint8_t hourlyForecastCount = 0;
int16_t minutelyPrecipHundredths[WEATHER_MINUTELY_MAX_POINTS]{};
uint8_t minutelyPrecipCount = 0;
char minutelyFirstTime[6]{};
char minutelyLastTime[6]{};
char minutelySummary[96] = "Minute forecast unavailable";
char currentConditionText[32] = "Unknown";
char currentWindText[40] = "--";
uint8_t currentHumidity = 0;
uint16_t currentAqi = 0;
bool currentAqiValid = false;
int16_t currentFeelsLikeC = 0;
char resolvedWeatherLocation[40]{};
char resolvedWeatherCity[48]{};
char resolvedWeatherTimezone[48]{};
volatile bool weatherInfoPending = true;
volatile bool weatherTrendPending = true;
uint8_t weatherInfoSubpageIndex = 0;
lv_obj_t *weatherInfoOverviewPage = nullptr;
lv_obj_t *weatherInfoTrendPage = nullptr;
lv_obj_t *weatherInfoLocationLabel = nullptr;
lv_obj_t *weatherInfoUpdatedLabel = nullptr;
lv_obj_t *weatherInfoCurrentIcon = nullptr;
lv_obj_t *weatherInfoTemperatureLabel = nullptr;
lv_obj_t *weatherInfoConditionLabel = nullptr;
lv_obj_t *weatherInfoFeelsLabel = nullptr;
lv_obj_t *weatherInfoHumidityLabel = nullptr;
lv_obj_t *weatherInfoWindLabel = nullptr;
lv_obj_t *weatherInfoAqiLabel = nullptr;
lv_obj_t *weatherInfoForecastDateLabels[3]{};
lv_obj_t *weatherInfoForecastIcons[3]{};
lv_obj_t *weatherInfoForecastTemperatureLabels[3]{};
lv_obj_t *weatherInfoForecastConditionLabels[3]{};
lv_obj_t *weatherTrendLocationLabel = nullptr;
lv_obj_t *weatherHourlyStatusLabel = nullptr;
lv_obj_t *weatherHourlyChart = nullptr;
lv_chart_series_t *weatherHourlySeries = nullptr;
lv_obj_t *weatherHourlyYLabels[3]{};
lv_obj_t *weatherHourlyXLabels[3]{};
lv_obj_t *weatherMinutelyStatusLabel = nullptr;
lv_obj_t *weatherMinutelyChart = nullptr;
lv_chart_series_t *weatherMinutelySeries = nullptr;
lv_obj_t *weatherMinutelyYLabels[2]{};
lv_obj_t *weatherMinutelyXLabels[3]{};
struct SmoothWeatherChartData {
  int16_t values[WEATHER_HOURLY_MAX_POINTS]{};
  uint8_t count = 0;
  int16_t minimum = 0;
  int16_t maximum = 1;
  lv_color_t color{};
  uint8_t pointRadius = 2;
};
SmoothWeatherChartData weatherHourlySmooth{};
SmoothWeatherChartData weatherMinutelySmooth{};
// -1 accepts live QWeather data; non-negative means a serial scene override.
volatile int16_t serialOverrideCode = -1;
// Direct scene selection keeps `weather clear` distinct from `weather night`
// regardless of the device's local time.
volatile int8_t serialOverrideScene = -1;
TaskHandle_t networkTaskHandle = nullptr;
bool networkServiceStarted = false;
volatile bool weatherRefreshRequested = false;
bool tlsMemoryPressureObserved = false;
WiFiClientSecure *qweatherSessionClient = nullptr;
HTTPClient *qweatherSessionHttp = nullptr;
bool qweatherSessionBegun = false;
uint32_t lastClockMinute = UINT32_MAX;
// QWeather sunrise/sunset for the configured location and local calendar day.
// Minutes are copied under stateMux because the network and UI tasks live on
// different cores. A zero date means that the 06:00/18:00 fallback is active.
volatile uint32_t solarDate = 0;
volatile uint16_t sunriseMinute = 6U * 60U;
volatile uint16_t sunsetMinute = 18U * 60U;
bool preferDailySunEndpoint = false;
int8_t lastLocalNight = -1;
const lv_img_dsc_t *displayedWeatherIcon = nullptr;
uint32_t playbackStatsStartedAtMs = 0;
uint32_t playbackStatsFrames = 0;
uint32_t playbackStatsIntervals = 0;
uint32_t playbackStatsGeneration = 0;
lv_disp_rot_t playbackStatsRotation = static_cast<lv_disp_rot_t>(0xFF);
uint64_t playbackStatsLastDisplayedAtUs = 0;
uint64_t playbackStatsIntervalUs = 0;
uint32_t playbackStatsLastSequence = 0;
uint64_t playbackStatsReadUs = 0;
uint64_t playbackStatsDecodeUs = 0;
uint64_t playbackStatsDecoderSetupUs = 0;
uint64_t playbackStatsHeaderUs = 0;
uint64_t playbackStatsProcessUs = 0;
uint64_t playbackStatsBufferWaitUs = 0;
uint64_t playbackStatsAssembleUs = 0;
uint64_t playbackStatsLcdUs = 0;
uint64_t playbackStatsPipelineUs = 0;
uint64_t playbackStatsPipelineMinUs = UINT64_MAX;
uint64_t playbackStatsPipelineMaxUs = 0;
uint64_t playbackStatsFrameMinUs = UINT64_MAX;
uint64_t playbackStatsFrameMaxUs = 0;
volatile uint32_t playbackStatsResultDrops = 0;
uint32_t playbackStatsResultDropsBaseline = 0;

void requestVideoSeek(uint32_t targetFrame);
void cancelAssetFade(bool restoreBrightness);
void resetPlaybackStats();

const char *sceneName(WeatherScene scene) {
  switch (scene) {
    case WeatherScene::Clear: return "clear";
    case WeatherScene::Cloudy: return "cloudy";
    case WeatherScene::Overcast: return "overcast";
    case WeatherScene::Rain: return "rain";
    case WeatherScene::Thunder: return "thunder";
    case WeatherScene::Snow: return "snow";
    case WeatherScene::Fog: return "fog";
    case WeatherScene::Haze: return "haze";
    case WeatherScene::Sand: return "sand";
    case WeatherScene::Night: return "night";
  }
  return "unknown";
}

struct LocalClockFields {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
};

bool readLocalClock(LocalClockFields &current) {
  if (rtcReady && rtcTimeValid) {
    RTC_DateTime rtc;
    if (readRtcTime(rtc)) {
      current = {static_cast<uint16_t>(rtc.getYear()),
                 static_cast<uint8_t>(rtc.getMonth()),
                 static_cast<uint8_t>(rtc.getDay()),
                 static_cast<uint8_t>(rtc.getHour()),
                 static_cast<uint8_t>(rtc.getMinute())};
      return true;
    }
  }
  // The weather service starts before NTP finishes and the RTC may be absent
  // after an I2C recovery. Once SNTP has set libc time, it is an equally valid
  // local calendar source and avoids suppressing the astronomy request.
  const time_t epoch = time(nullptr);
  if (epoch < 1704067200) return false;  // 2024-01-01 UTC
  struct tm local{};
  localtime_r(&epoch, &local);
  current = {static_cast<uint16_t>(local.tm_year + 1900),
             static_cast<uint8_t>(local.tm_mon + 1),
             static_cast<uint8_t>(local.tm_mday),
             static_cast<uint8_t>(local.tm_hour),
             static_cast<uint8_t>(local.tm_min)};
  return true;
}

uint32_t localDateKey(const LocalClockFields &current) {
  return current.year * 10000UL + current.month * 100UL + current.day;
}

bool localNight() {
  LocalClockFields current{};
  if (!readLocalClock(current)) return false;

  uint32_t date = 0;
  uint16_t sunrise = 6U * 60U;
  uint16_t sunset = 18U * 60U;
  portENTER_CRITICAL(&stateMux);
  date = solarDate;
  sunrise = sunriseMinute;
  sunset = sunsetMinute;
  portEXIT_CRITICAL(&stateMux);
  const uint16_t minute = current.hour * 60U + current.minute;
  if (date == localDateKey(current) && sunrise < sunset)
    return minute < sunrise || minute >= sunset;
  // RTC can become valid before the astronomy request completes. Retain the
  // old behavior only as a temporary/error fallback, never as the normal rule.
  return minute < 6U * 60U || minute >= 18U * 60U;
}

WeatherScene sceneForCode(uint16_t code) {
  WeatherScene scene = WeatherScene::Overcast;
  if (code == 100 || code == 900 || code == 150) scene = WeatherScene::Clear;
  else if ((code >= 101 && code <= 104) || (code >= 151 && code <= 153))
    scene = code == 104 ? WeatherScene::Overcast : WeatherScene::Cloudy;
  else if (code >= 302 && code <= 304) scene = WeatherScene::Thunder;
  else if ((code >= 300 && code <= 318) || code == 350 || code == 351 || code == 399)
    scene = WeatherScene::Rain;
  else if ((code >= 400 && code <= 410) || code == 456 || code == 457 ||
           code == 499 || code == 901) scene = WeatherScene::Snow;
  else if (code == 500 || code == 501 || code == 509 || code == 510 ||
           code == 514 || code == 515) scene = WeatherScene::Fog;
  else if (code == 502 || (code >= 511 && code <= 513)) scene = WeatherScene::Haze;
  else if (code == 503 || code == 504 || code == 507 || code == 508)
    scene = WeatherScene::Sand;
  if ((scene == WeatherScene::Clear || scene == WeatherScene::Cloudy ||
       scene == WeatherScene::Overcast) && localNight()) {
    return WeatherScene::Night;
  }
  return scene;
}

const lv_img_dsc_t *iconForCode(uint16_t code) {
  if (code == 100) return &tabler_weather_sun;
  if (code == 900) return &tabler_weather_temperature_sun;
  if (code == 901) return &tabler_weather_temperature_snow;
  if (code == 150) return &tabler_weather_moon;
  if (code >= 101 && code <= 103) return &tabler_weather_cloudy_composite;
  if (code == 104 || (code >= 151 && code <= 153))
    return &tabler_weather_cloud;
  if (code >= 302 && code <= 304) return &tabler_weather_cloud_storm;
  if ((code >= 300 && code <= 318) || code == 350 || code == 351 || code == 399)
    return &tabler_weather_cloud_rain;
  if ((code >= 404 && code <= 406) || code == 456)
    return &tabler_weather_cloud_snow;
  if ((code >= 400 && code <= 403) || (code >= 407 && code <= 410) ||
      code == 457 || code == 499) return &tabler_weather_snowflake;
  if (code >= 500 && code <= 515) return &tabler_weather_mist;
  return &tabler_weather_cloud_question;
}

lv_color_t weatherAccentForCode(uint16_t code) {
  (void)code;
  return lv_color_white();
}

void setWeatherIconInHolder(lv_obj_t *holder, uint16_t code) {
  if (!holder) return;
  lv_obj_clean(holder);
  lv_obj_t *image = lv_img_create(holder);
  lv_img_set_src(image, iconForCode(code));
  lv_obj_center(image);
  lv_obj_set_style_img_recolor(image, weatherAccentForCode(code), 0);
  lv_obj_set_style_img_recolor_opa(image, LV_OPA_COVER, 0);
  lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

void updateWeatherIcon(uint16_t code) {
  if (!weatherIconHolder) return;
  const lv_img_dsc_t *icon = iconForCode(code);
  if (lv_obj_get_child_cnt(weatherIconHolder) && icon == displayedWeatherIcon) return;
  lv_obj_clean(weatherIconHolder);
  lv_obj_t *image = lv_img_create(weatherIconHolder);
  lv_img_set_src(image, icon);
  lv_obj_center(image);
  lv_obj_set_style_img_recolor(image, weatherAccentForCode(code), 0);
  lv_obj_set_style_img_recolor_opa(image, LV_OPA_COVER, 0);
  lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  displayedWeatherIcon = icon;
}

void setMessage(const char *text, lv_color_t color = lv_color_hex(0xCBD5E1)) {
  if (!messageLabel) return;
  lv_label_set_text(messageLabel, text ? text : "");
  lv_obj_set_style_text_color(messageLabel, color, 0);
  if (text && text[0]) lv_obj_clear_flag(messageLabel, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(messageLabel, LV_OBJ_FLAG_HIDDEN);
}

void formatVideoTime(char *text, size_t textSize, uint32_t seconds) {
  const uint32_t hours = seconds / 3600U;
  const uint32_t minutes = (seconds / 60U) % 60U;
  const uint32_t remainingSeconds = seconds % 60U;
  if (hours) {
    snprintf(text, textSize, "%lu:%02lu:%02lu",
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes),
             static_cast<unsigned long>(remainingSeconds));
  } else {
    snprintf(text, textSize, "%02lu:%02lu",
             static_cast<unsigned long>(minutes),
             static_cast<unsigned long>(remainingSeconds));
  }
}

void updateVideoTimeLabel(uint32_t frame, uint32_t frameCount,
                          uint16_t fpsTimes100) {
  if (!videoTimeLabel) return;
  if (!frameCount || !fpsTimes100) {
    lv_label_set_text(videoTimeLabel, "--:-- / --:--");
    return;
  }
  if (frame >= frameCount) frame = frameCount - 1U;
  const uint32_t elapsedSeconds = static_cast<uint32_t>(
    (static_cast<uint64_t>(frame + 1U) * 100U) / fpsTimes100);
  const uint32_t totalSeconds = static_cast<uint32_t>(
    (static_cast<uint64_t>(frameCount) * 100U + fpsTimes100 - 1U) /
    fpsTimes100);
  char elapsed[16]{};
  char total[16]{};
  char text[36]{};
  formatVideoTime(elapsed, sizeof(elapsed), elapsedSeconds);
  formatVideoTime(total, sizeof(total), totalSeconds);
  snprintf(text, sizeof(text), "%s / %s", elapsed, total);
  lv_label_set_text(videoTimeLabel, text);
}

String videoDisplayName(uint8_t index) {
  if (index >= weatherWallpaperCount) return "Video";
  String name = weatherWallpapers[index].path;
  const int slash = max(name.lastIndexOf('/'), name.lastIndexOf('\\'));
  if (slash >= 0) name.remove(0, slash + 1);
  const int extension = name.lastIndexOf('.');
  if (extension > 0) name.remove(extension);
  return name.length() ? name : String("Video");
}

void showVideoName(uint8_t index) {
  if (!playerVideoMode || !videoNameLabel || !videoTimeLabel) return;
  const String name = videoDisplayName(index);
  lv_label_set_text(videoNameLabel, name.c_str());
  lv_obj_add_flag(videoTimeLabel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(videoNameLabel, LV_OBJ_FLAG_HIDDEN);
  videoNameHideAtMs = millis() + 1800U;
}

void serviceVideoNameOverlay() {
  if (!videoNameHideAtMs || !videoNameLabel || !videoTimeLabel) return;
  if (static_cast<int32_t>(millis() - videoNameHideAtMs) < 0) return;
  videoNameHideAtMs = 0;
  lv_obj_add_flag(videoNameLabel, LV_OBJ_FLAG_HIDDEN);
  if (playerVideoMode) lv_obj_clear_flag(videoTimeLabel, LV_OBJ_FLAG_HIDDEN);
}

void persistLastVideoSelection(uint8_t index) {
  if (index >= weatherWallpaperCount || !weatherWallpapers[index].video) return;
  const String &key = weatherWallpapers[index].key;
  if (key.isEmpty()) return;
  static String persistedKey;
  static bool loaded = false;
  if (!loaded) {
    persistedKey = preferences.getString("last_video", "");
    loaded = true;
  }
  if (persistedKey == key) return;
  if (preferences.putString("last_video", key)) {
    persistedKey = key;
    Serial.printf("Video selection saved: %s\n", key.c_str());
  }
}

void persistLastWeatherSelection(uint8_t index) {
  if (index >= weatherWallpaperCount || weatherWallpapers[index].video) return;
  const String &key = weatherWallpapers[index].key;
  if (key.isEmpty()) return;
  static String persistedKey = preferences.getString("last_wall", "");
  if (persistedKey == key) return;
  if (preferences.putString("last_wall", key)) {
    persistedKey = key;
    Serial.printf("Weather wallpaper selection saved: %s\n", key.c_str());
  }
}

void videoProgressEvent(lv_event_t *event) {
  if (!playerVideoMode || !videoProgressBar) return;
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_PRESSED) {
    videoSeekDragging = true;
    lastVideoSeekInteractionAtMs = millis();
    markUserActivity();
    return;
  }
  if (code == LV_EVENT_VALUE_CHANGED && videoSeekDragging &&
      currentVideoFrameCount && currentVideoFpsTimes100) {
    const uint32_t value = static_cast<uint32_t>(
      lv_slider_get_value(videoProgressBar));
    const uint32_t target = static_cast<uint32_t>(
      (static_cast<uint64_t>(currentVideoFrameCount - 1U) * value) / 1000U);
    updateVideoTimeLabel(target, currentVideoFrameCount,
                         currentVideoFpsTimes100);
    lastVideoSeekInteractionAtMs = millis();
    return;
  }
  if (code == LV_EVENT_RELEASED && videoSeekDragging) {
    videoSeekDragging = false;
    lastVideoSeekInteractionAtMs = millis();
    if (currentVideoFrameCount) {
      const uint32_t value = static_cast<uint32_t>(
        lv_slider_get_value(videoProgressBar));
      const uint32_t target = static_cast<uint32_t>(
        (static_cast<uint64_t>(currentVideoFrameCount - 1U) * value) / 1000U);
      requestVideoSeek(target);
    }
  } else if (code == LV_EVENT_PRESS_LOST) {
    videoSeekDragging = false;
    lastVideoSeekInteractionAtMs = millis();
    updateVideoTimeLabel(currentVideoPositionFrame, currentVideoFrameCount,
                         currentVideoFpsTimes100);
  }
}

void createPage(lv_obj_t *screen) {
  page = lv_obj_create(screen);
  lv_obj_remove_style_all(page);
  lv_obj_set_size(page, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_pos(page, 0, 0);
  lv_obj_set_style_bg_color(page, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(page, appPageGestureEvent, LV_EVENT_GESTURE, nullptr);

  videoImage = lv_obj_create(page);
  lv_obj_remove_style_all(videoImage);
  lv_obj_set_pos(videoImage, 0, 0);
  lv_obj_set_size(videoImage, VIDEO_WIDTH, VIDEO_HEIGHT);
  lv_obj_set_style_bg_color(videoImage, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(videoImage, LV_OPA_COVER, 0);
  lv_obj_clear_flag(videoImage, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(videoImage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(videoImage, appPageGestureEvent, LV_EVENT_GESTURE, nullptr);

  statusPanel = lv_obj_create(page);
  lv_obj_remove_style_all(statusPanel);
  lv_obj_set_pos(statusPanel, 0, VIDEO_HEIGHT);
  lv_obj_set_size(statusPanel, SCREEN_WIDTH, SCREEN_HEIGHT - VIDEO_HEIGHT);
  lv_obj_set_style_bg_color(statusPanel, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(statusPanel, LV_OPA_COVER, 0);
  lv_obj_clear_flag(statusPanel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(statusPanel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(statusPanel, appPageGestureEvent, LV_EVENT_GESTURE, nullptr);

  clockLabel = lv_label_create(statusPanel);
  lv_label_set_text(clockLabel, "--:--");
  lv_obj_set_pos(clockLabel, 24, 38);
  lv_obj_set_style_text_font(clockLabel, &lv_font_inter_weather_52, 0);
  lv_obj_set_style_text_color(clockLabel, lv_color_hex(0xF7F8FA), 0);

  weatherIconHolder = lv_obj_create(statusPanel);
  lv_obj_remove_style_all(weatherIconHolder);
  lv_obj_set_pos(weatherIconHolder, 288, 32);
  lv_obj_set_size(weatherIconHolder, 56, 56);
  lv_obj_clear_flag(weatherIconHolder, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  updateWeatherIcon(999);

  temperatureLabel = lv_label_create(statusPanel);
  lv_label_set_text(temperatureLabel, "--\xC2\xB0");
  lv_obj_set_pos(temperatureLabel, 346, 38);
  lv_obj_set_width(temperatureLabel, 112);
  lv_obj_set_style_text_font(temperatureLabel, &lv_font_inter_weather_52, 0);
  lv_obj_set_style_text_align(temperatureLabel, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_text_color(temperatureLabel, lv_color_hex(0xF7F8FA), 0);

  videoTimeLabel = lv_label_create(statusPanel);
  lv_label_set_text(videoTimeLabel, "--:-- / --:--");
  lv_obj_set_pos(videoTimeLabel, 24, 26);
  lv_obj_set_width(videoTimeLabel, 432);
  lv_obj_set_style_text_align(videoTimeLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(videoTimeLabel, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(videoTimeLabel, lv_color_hex(0xF7F8FA), 0);
  lv_obj_add_flag(videoTimeLabel, LV_OBJ_FLAG_HIDDEN);

  videoNameLabel = lv_label_create(statusPanel);
  lv_label_set_text(videoNameLabel, "");
  lv_obj_set_pos(videoNameLabel, 24, 26);
  lv_obj_set_width(videoNameLabel, 432);
  lv_label_set_long_mode(videoNameLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(videoNameLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(videoNameLabel, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(videoNameLabel, lv_color_hex(0xD6D9DF), 0);
  lv_obj_add_flag(videoNameLabel, LV_OBJ_FLAG_HIDDEN);

  videoProgressBar = lv_slider_create(statusPanel);
  lv_obj_set_pos(videoProgressBar, 24, 70);
  lv_obj_set_size(videoProgressBar, 432, 12);
  lv_slider_set_range(videoProgressBar, 0, 1000);
  lv_slider_set_value(videoProgressBar, 0, LV_ANIM_OFF);
  lv_obj_set_ext_click_area(videoProgressBar, 12);
  lv_obj_set_style_bg_color(videoProgressBar, lv_color_hex(0x273244),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(videoProgressBar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(videoProgressBar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(videoProgressBar, lv_color_hex(0xAEB4BE),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(videoProgressBar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(videoProgressBar, LV_RADIUS_CIRCLE,
                          LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(videoProgressBar, lv_color_hex(0xAEB4BE),
                            LV_PART_KNOB);
  lv_obj_set_style_bg_opa(videoProgressBar, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_pad_all(videoProgressBar, 5, LV_PART_KNOB);
  lv_obj_add_event_cb(videoProgressBar, videoProgressEvent, LV_EVENT_ALL,
                      nullptr);
  lv_obj_add_flag(videoProgressBar, LV_OBJ_FLAG_HIDDEN);

  messageLabel = lv_label_create(page);
  lv_obj_set_width(messageLabel, 410);
  lv_label_set_long_mode(messageLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(messageLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(messageLabel, &lv_font_montserrat_18, 0);
  lv_obj_align(messageLabel, LV_ALIGN_CENTER, 0, -60);
  setMessage("");
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *createWeatherInfoCard(lv_obj_t *parent, int16_t x, int16_t y,
                                int16_t width, int16_t height) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, height);
  lv_obj_set_style_radius(card, 24, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x202020), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0x3A3A3A), 0);
  lv_obj_set_style_pad_all(card, 0, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  // Cards cover most of the screen. Bubble their gestures to the page so
  // swiping from the current/forecast areas still opens controls or exits.
  lv_obj_add_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
  return card;
}

void drawSmoothWeatherChart(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_DRAW_PART_BEGIN) return;
  auto *draw = static_cast<lv_obj_draw_part_dsc_t *>(lv_event_get_param(event));
  if (!draw || draw->type != LV_CHART_DRAW_PART_LINE_AND_POINT) return;

  // Suppress LVGL's native straight segment and its per-segment point. The
  // first draw callback below renders the complete smoothed series once.
  if (draw->line_dsc) draw->line_dsc->opa = LV_OPA_TRANSP;
  if (draw->rect_dsc) {
    draw->rect_dsc->bg_opa = LV_OPA_TRANSP;
    draw->rect_dsc->border_opa = LV_OPA_TRANSP;
    draw->rect_dsc->shadow_opa = LV_OPA_TRANSP;
  }
  auto *data = static_cast<SmoothWeatherChartData *>(
    lv_event_get_user_data(event));
  if (!data || data->count < 2 || data->maximum <= data->minimum ||
      draw->id != 0 || !draw->draw_ctx) return;

  lv_obj_t *chart = lv_event_get_target(event);
  lv_area_t plot{};
  lv_obj_get_content_coords(chart, &plot);
  const int32_t width = lv_obj_get_content_width(chart);
  const int32_t height = lv_obj_get_content_height(chart);
  const float valueRange = data->maximum - data->minimum;
  auto pointFor = [&](float sample, float value) {
    lv_point_t point;
    point.x = plot.x1 + static_cast<lv_coord_t>(
      roundf(sample * width / (data->count - 1)));
    point.y = plot.y1 + height - static_cast<lv_coord_t>(
      roundf((value - data->minimum) * height / valueRange));
    return point;
  };

  lv_draw_line_dsc_t line{};
  lv_draw_line_dsc_init(&line);
  line.color = data->color;
  line.width = 3;
  line.opa = LV_OPA_COVER;
  line.round_start = 1;
  line.round_end = 1;

  // Soften each interior sample before spline interpolation. This trades a
  // small amount of point accuracy for visibly rounder peaks and valleys.
  auto softenedValue = [&](int16_t index) {
    if (index <= 0 || index >= data->count - 1)
      return static_cast<float>(data->values[index]);
    return (data->values[index - 1] + 2.0f * data->values[index] +
            data->values[index + 1]) * 0.25f;
  };

  constexpr uint8_t STEPS_PER_SAMPLE = 12;
  lv_point_t previous = pointFor(0.0f, softenedValue(0));
  for (uint8_t i = 0; i + 1 < data->count; ++i) {
    const float p0 = softenedValue(i ? i - 1 : i);
    const float p1 = softenedValue(i);
    const float p2 = softenedValue(i + 1);
    const float p3 = softenedValue(i + 2 < data->count ? i + 2 : i + 1);
    for (uint8_t step = 1; step <= STEPS_PER_SAMPLE; ++step) {
      const float t = static_cast<float>(step) / STEPS_PER_SAMPLE;
      const float t2 = t * t;
      const float t3 = t2 * t;
      float value = 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                    (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                    (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
      // Permit mild inter-sample overshoot for a rounder curve, but never let
      // it escape the chart's displayed numeric range.
      if (value < data->minimum) value = data->minimum;
      if (value > data->maximum) value = data->maximum;
      const lv_point_t current = pointFor(i + t, value);
      lv_draw_line(draw->draw_ctx, &line, &previous, &current);
      previous = current;
    }
  }

  lv_draw_rect_dsc_t marker{};
  lv_draw_rect_dsc_init(&marker);
  marker.radius = LV_RADIUS_CIRCLE;
  marker.bg_color = data->color;
  marker.bg_opa = LV_OPA_COVER;
  for (uint8_t i = 0; i < data->count; ++i) {
    const lv_point_t point = pointFor(i, softenedValue(i));
    lv_area_t area{
      static_cast<lv_coord_t>(point.x - data->pointRadius),
      static_cast<lv_coord_t>(point.y - data->pointRadius),
      static_cast<lv_coord_t>(point.x + data->pointRadius),
      static_cast<lv_coord_t>(point.y + data->pointRadius)
    };
    lv_draw_rect(draw->draw_ctx, &marker, &area);
  }
}

lv_obj_t *createWeatherInfoLabel(lv_obj_t *parent, const char *text,
                                 const lv_font_t *font, lv_color_t color) {
  lv_obj_t *label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, color, 0);
  return label;
}

void createWeatherInfoPageDots(lv_obj_t *parent, uint8_t activeIndex) {
  for (uint8_t i = 0; i < 2; ++i) {
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_pos(dot, 218 + i * 25, 465);
    lv_obj_set_size(dot, 18, 4);
    lv_obj_set_style_radius(dot, 2, 0);
    lv_obj_set_style_bg_color(
      dot, i == activeIndex ? lv_color_white() : lv_color_hex(0x454545), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  }
}

void createInfoPage(lv_obj_t *screen) {
  weatherInfoPage = lv_obj_create(screen);
  lv_obj_remove_style_all(weatherInfoPage);
  lv_obj_set_pos(weatherInfoPage, 0, 0);
  lv_obj_set_size(weatherInfoPage, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(weatherInfoPage, lv_color_hex(0x141414), 0);
  lv_obj_set_style_bg_opa(weatherInfoPage, LV_OPA_COVER, 0);
  lv_obj_clear_flag(weatherInfoPage, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(weatherInfoPage, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(weatherInfoPage, appPageGestureEvent, LV_EVENT_GESTURE,
                      nullptr);

  weatherInfoOverviewPage = lv_obj_create(weatherInfoPage);
  lv_obj_remove_style_all(weatherInfoOverviewPage);
  lv_obj_set_pos(weatherInfoOverviewPage, 0, 0);
  lv_obj_set_size(weatherInfoOverviewPage, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_clear_flag(weatherInfoOverviewPage, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(weatherInfoOverviewPage, LV_OBJ_FLAG_GESTURE_BUBBLE);

  weatherInfoLocationLabel = createWeatherInfoLabel(
    weatherInfoOverviewPage, "--", &lv_font_montserrat_32, lv_color_white());
  lv_obj_set_pos(weatherInfoLocationLabel, 37, 7);
  lv_obj_set_width(weatherInfoLocationLabel, 267);
  lv_obj_set_style_text_align(weatherInfoLocationLabel, LV_TEXT_ALIGN_LEFT, 0);
  lv_label_set_long_mode(weatherInfoLocationLabel, LV_LABEL_LONG_DOT);

  weatherInfoUpdatedLabel = createWeatherInfoLabel(
    weatherInfoOverviewPage, "Waiting for update", &lv_font_montserrat_22,
    lv_color_hex(0x9299A3));
  lv_obj_set_pos(weatherInfoUpdatedLabel, 314, 14);
  lv_obj_set_width(weatherInfoUpdatedLabel, 136);
  lv_label_set_long_mode(weatherInfoUpdatedLabel, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(weatherInfoUpdatedLabel, LV_TEXT_ALIGN_RIGHT, 0);

  lv_obj_t *currentCard = createWeatherInfoCard(
    weatherInfoOverviewPage, 24, 50, 432, 176);
  weatherInfoCurrentIcon = lv_obj_create(currentCard);
  lv_obj_remove_style_all(weatherInfoCurrentIcon);
  lv_obj_set_pos(weatherInfoCurrentIcon, 25, 23);
  lv_obj_set_size(weatherInfoCurrentIcon, 72, 72);
  lv_obj_clear_flag(weatherInfoCurrentIcon,
                    LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  setWeatherIconInHolder(weatherInfoCurrentIcon, 999);

  weatherInfoTemperatureLabel = createWeatherInfoLabel(
    currentCard, "--\xC2\xB0", &lv_font_inter_weather_52, lv_color_white());
  lv_obj_set_pos(weatherInfoTemperatureLabel, 108, 39);
  lv_obj_set_width(weatherInfoTemperatureLabel, 132);

  weatherInfoConditionLabel = createWeatherInfoLabel(
    currentCard, "Unknown", &lv_font_montserrat_28,
    lv_color_white());
  lv_obj_set_pos(weatherInfoConditionLabel, 14, 110);
  lv_obj_set_size(weatherInfoConditionLabel, 204, 34);
  lv_obj_set_style_text_align(weatherInfoConditionLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(weatherInfoConditionLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);

  lv_obj_t *currentDivider = lv_obj_create(currentCard);
  lv_obj_remove_style_all(currentDivider);
  lv_obj_set_pos(currentDivider, 228, 24);
  lv_obj_set_size(currentDivider, 1, 128);
  lv_obj_set_style_bg_color(currentDivider, lv_color_hex(0x4A4A4A), 0);
  lv_obj_set_style_bg_opa(currentDivider, LV_OPA_COVER, 0);
  lv_obj_clear_flag(currentDivider,
                    LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

  weatherInfoFeelsLabel = createWeatherInfoLabel(
    currentCard, "Feels --", &lv_font_montserrat_24, lv_color_hex(0xCBD5E1));
  weatherInfoHumidityLabel = createWeatherInfoLabel(
    currentCard, "Humidity --", &lv_font_montserrat_24, lv_color_hex(0xCBD5E1));
  weatherInfoWindLabel = createWeatherInfoLabel(
    currentCard, "Wind --", &lv_font_montserrat_24, lv_color_hex(0xCBD5E1));
  weatherInfoAqiLabel = createWeatherInfoLabel(
    currentCard, "AQI --", &lv_font_montserrat_24, lv_color_hex(0xCBD5E1));
  lv_obj_t *detailLabels[] = {
    weatherInfoFeelsLabel, weatherInfoHumidityLabel,
    weatherInfoWindLabel, weatherInfoAqiLabel
  };
  for (uint8_t i = 0; i < 4; ++i) {
    lv_obj_set_pos(detailLabels[i], 244, 16 + i * 38);
    lv_obj_set_size(detailLabels[i], 172, 28);
    lv_label_set_long_mode(detailLabels[i], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(detailLabels[i], LV_TEXT_ALIGN_LEFT, 0);
  }

  lv_obj_t *forecastTitle = createWeatherInfoLabel(
    weatherInfoOverviewPage, "3-DAY FORECAST", &lv_font_montserrat_20,
    lv_color_hex(0x9299A3));
  lv_obj_set_pos(forecastTitle, 24, 238);

  for (uint8_t i = 0; i < 3; ++i) {
    lv_obj_t *card = createWeatherInfoCard(
      weatherInfoOverviewPage, 24 + i * 146, 264, 140, 184);
    weatherInfoForecastDateLabels[i] = createWeatherInfoLabel(
      card, "--", &lv_font_montserrat_24, lv_color_hex(0xCBD5E1));
    lv_obj_set_width(weatherInfoForecastDateLabels[i], 140);
    lv_obj_set_style_text_align(weatherInfoForecastDateLabels[i],
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(weatherInfoForecastDateLabels[i], 0, 10);
    weatherInfoForecastIcons[i] = lv_obj_create(card);
    lv_obj_remove_style_all(weatherInfoForecastIcons[i]);
    lv_obj_set_pos(weatherInfoForecastIcons[i], 42, 42);
    lv_obj_set_size(weatherInfoForecastIcons[i], 56, 56);
    setWeatherIconInHolder(weatherInfoForecastIcons[i], 999);
    weatherInfoForecastTemperatureLabels[i] = createWeatherInfoLabel(
      card, "-- / --", &lv_font_montserrat_24, lv_color_white());
    lv_obj_set_width(weatherInfoForecastTemperatureLabels[i], 140);
    lv_obj_set_style_text_align(weatherInfoForecastTemperatureLabels[i],
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(weatherInfoForecastTemperatureLabels[i], 0, 105);
    weatherInfoForecastConditionLabels[i] = createWeatherInfoLabel(
      card, "Unknown", &lv_font_montserrat_24, lv_color_hex(0x9299A3));
    lv_obj_set_size(weatherInfoForecastConditionLabels[i], 124, 28);
    lv_label_set_long_mode(weatherInfoForecastConditionLabels[i],
                           LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(weatherInfoForecastConditionLabels[i],
                                LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(weatherInfoForecastConditionLabels[i], 8, 140);
  }
  createWeatherInfoPageDots(weatherInfoOverviewPage, 0);

  weatherInfoTrendPage = lv_obj_create(weatherInfoPage);
  lv_obj_remove_style_all(weatherInfoTrendPage);
  lv_obj_set_pos(weatherInfoTrendPage, 0, 0);
  lv_obj_set_size(weatherInfoTrendPage, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_clear_flag(weatherInfoTrendPage, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(weatherInfoTrendPage, LV_OBJ_FLAG_GESTURE_BUBBLE);

  weatherTrendLocationLabel = createWeatherInfoLabel(
    weatherInfoTrendPage, "--", &lv_font_montserrat_28, lv_color_white());
  lv_obj_set_pos(weatherTrendLocationLabel, 32, 7);
  lv_obj_set_width(weatherTrendLocationLabel, 432);
  lv_label_set_long_mode(weatherTrendLocationLabel, LV_LABEL_LONG_DOT);

  lv_obj_t *hourlyCard = createWeatherInfoCard(
    weatherInfoTrendPage, 24, 43, 432, 200);
  lv_obj_t *hourlyTitle = createWeatherInfoLabel(
    hourlyCard, "TEMPERATURE \xE2\x80\xA2 24H", &lv_font_montserrat_24,
    lv_color_hex(0x9299A3));
  lv_obj_set_pos(hourlyTitle, 12, 6);
  lv_obj_set_height(hourlyTitle, 25);
  weatherHourlyStatusLabel = createWeatherInfoLabel(
    hourlyCard, "Waiting for forecast", &lv_font_montserrat_20,
    lv_color_white());
  lv_obj_set_pos(weatherHourlyStatusLabel, 251, 9);
  lv_obj_set_width(weatherHourlyStatusLabel, 166);
  lv_obj_set_style_text_align(weatherHourlyStatusLabel, LV_TEXT_ALIGN_RIGHT, 0);
  lv_label_set_long_mode(weatherHourlyStatusLabel, LV_LABEL_LONG_DOT);

  const int16_t hourlyYPositions[3] = {38, 95, 145};
  const int16_t hourlyYWidths[3] = {51, 51, 52};
  for (uint8_t i = 0; i < 3; ++i) {
    weatherHourlyYLabels[i] = createWeatherInfoLabel(
      hourlyCard, "--", &lv_font_montserrat_20, lv_color_hex(0x9299A3));
    lv_obj_set_pos(weatherHourlyYLabels[i], -1, hourlyYPositions[i]);
    lv_obj_set_size(weatherHourlyYLabels[i], hourlyYWidths[i], 22);
    lv_obj_set_style_text_align(weatherHourlyYLabels[i], LV_TEXT_ALIGN_RIGHT, 0);
  }
  weatherHourlyChart = lv_chart_create(hourlyCard);
  lv_obj_set_pos(weatherHourlyChart, 55, 41);
  lv_obj_set_size(weatherHourlyChart, 360, 126);
  lv_chart_set_type(weatherHourlyChart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(weatherHourlyChart, WEATHER_HOURLY_MAX_POINTS);
  lv_chart_set_range(weatherHourlyChart, LV_CHART_AXIS_PRIMARY_Y, -10, 40);
  lv_chart_set_div_line_count(weatherHourlyChart, 3, 5);
  lv_obj_set_style_bg_opa(weatherHourlyChart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(weatherHourlyChart, 0, 0);
  lv_obj_set_style_pad_all(weatherHourlyChart, 5, 0);
  lv_obj_set_style_line_color(weatherHourlyChart, lv_color_hex(0x3A3A3A),
                              LV_PART_MAIN);
  lv_obj_set_style_line_opa(weatherHourlyChart, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_line_width(weatherHourlyChart, 3, LV_PART_ITEMS);
  lv_obj_set_style_size(weatherHourlyChart, 5, LV_PART_INDICATOR);
  lv_obj_clear_flag(weatherHourlyChart,
                    LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(weatherHourlyChart, LV_OBJ_FLAG_GESTURE_BUBBLE);
  weatherHourlySeries = lv_chart_add_series(
    weatherHourlyChart, lv_color_hex(0x38BDF8), LV_CHART_AXIS_PRIMARY_Y);
  weatherHourlySmooth.color = lv_color_hex(0x38BDF8);
  weatherHourlySmooth.pointRadius = 2;
  lv_obj_add_event_cb(weatherHourlyChart, drawSmoothWeatherChart,
                      LV_EVENT_DRAW_PART_BEGIN, &weatherHourlySmooth);
  lv_chart_set_all_value(weatherHourlyChart, weatherHourlySeries,
                         LV_CHART_POINT_NONE);
  const int16_t hourlyXPositions[3] = {60, 178, 346};
  const int16_t hourlyXWidths[3] = {60, 110, 66};
  const lv_text_align_t hourlyXAlignments[3] = {
    LV_TEXT_ALIGN_LEFT, LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_RIGHT
  };
  const char *hourlyXText[3] = {"0h", "+12h", "+24h"};
  for (uint8_t i = 0; i < 3; ++i) {
    weatherHourlyXLabels[i] = createWeatherInfoLabel(
      hourlyCard, hourlyXText[i], &lv_font_montserrat_20,
      lv_color_hex(0x9299A3));
    lv_obj_set_pos(weatherHourlyXLabels[i], hourlyXPositions[i], 172);
    lv_obj_set_size(weatherHourlyXLabels[i], hourlyXWidths[i], 22);
    lv_obj_set_style_text_align(weatherHourlyXLabels[i], hourlyXAlignments[i], 0);
  }

  lv_obj_t *minutelyCard = createWeatherInfoCard(
    weatherInfoTrendPage, 24, 254, 432, 200);
  lv_obj_t *minutelyTitle = createWeatherInfoLabel(
    minutelyCard, "PRECIPITATION \xE2\x80\xA2 2H", &lv_font_montserrat_25,
    lv_color_hex(0x9299A3));
  lv_obj_set_pos(minutelyTitle, 12, 6);
  lv_obj_set_height(minutelyTitle, 25);
  weatherMinutelyStatusLabel = createWeatherInfoLabel(
    minutelyCard, "Minute forecast unavailable", &lv_font_montserrat_18,
    lv_color_hex(0xCBD5E1));
  lv_obj_set_pos(weatherMinutelyStatusLabel, 14, 34);
  lv_obj_set_size(weatherMinutelyStatusLabel, 396, 24);
  lv_label_set_long_mode(weatherMinutelyStatusLabel,
                         LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_align(weatherMinutelyStatusLabel, LV_TEXT_ALIGN_LEFT, 0);
  for (uint8_t i = 0; i < 2; ++i) {
    weatherMinutelyYLabels[i] = createWeatherInfoLabel(
      minutelyCard, i ? "0.0mm" : "--", &lv_font_montserrat_16,
      lv_color_hex(0x9299A3));
    lv_obj_set_pos(weatherMinutelyYLabels[i], 0, i ? 148 : 59);
    lv_obj_set_size(weatherMinutelyYLabels[i], 62, 22);
    lv_obj_set_style_text_align(weatherMinutelyYLabels[i], LV_TEXT_ALIGN_RIGHT, 0);
  }
  weatherMinutelyChart = lv_chart_create(minutelyCard);
  lv_obj_set_pos(weatherMinutelyChart, 65, 61);
  lv_obj_set_size(weatherMinutelyChart, 350, 110);
  lv_chart_set_type(weatherMinutelyChart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(weatherMinutelyChart,
                           WEATHER_MINUTELY_MAX_POINTS);
  lv_chart_set_range(weatherMinutelyChart, LV_CHART_AXIS_PRIMARY_Y, 0, 10);
  lv_chart_set_div_line_count(weatherMinutelyChart, 3, 5);
  lv_obj_set_style_bg_opa(weatherMinutelyChart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(weatherMinutelyChart, 0, 0);
  lv_obj_set_style_pad_all(weatherMinutelyChart, 5, 0);
  lv_obj_set_style_line_color(weatherMinutelyChart, lv_color_hex(0x3A3A3A),
                              LV_PART_MAIN);
  lv_obj_set_style_line_opa(weatherMinutelyChart, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_line_width(weatherMinutelyChart, 3, LV_PART_ITEMS);
  lv_obj_set_style_size(weatherMinutelyChart, 4, LV_PART_INDICATOR);
  lv_obj_clear_flag(weatherMinutelyChart,
                    LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(weatherMinutelyChart, LV_OBJ_FLAG_GESTURE_BUBBLE);
  weatherMinutelySeries = lv_chart_add_series(
    weatherMinutelyChart, lv_color_hex(0x60A5FA), LV_CHART_AXIS_PRIMARY_Y);
  weatherMinutelySmooth.color = lv_color_hex(0x60A5FA);
  weatherMinutelySmooth.pointRadius = 2;
  lv_obj_add_event_cb(weatherMinutelyChart, drawSmoothWeatherChart,
                      LV_EVENT_DRAW_PART_BEGIN, &weatherMinutelySmooth);
  lv_chart_set_all_value(weatherMinutelyChart, weatherMinutelySeries,
                         LV_CHART_POINT_NONE);
  const int16_t minutelyXPositions[3] = {58, 180, 342};
  const int16_t minutelyXWidths[3] = {60, 110, 72};
  const lv_text_align_t minutelyXAlignments[3] = {
    LV_TEXT_ALIGN_LEFT, LV_TEXT_ALIGN_CENTER, LV_TEXT_ALIGN_RIGHT
  };
  const char *minutelyXText[3] = {"0m", "+60m", "+120m"};
  for (uint8_t i = 0; i < 3; ++i) {
    weatherMinutelyXLabels[i] = createWeatherInfoLabel(
      minutelyCard, minutelyXText[i], &lv_font_montserrat_20,
      lv_color_hex(0x9299A3));
    lv_obj_set_pos(weatherMinutelyXLabels[i], minutelyXPositions[i], 175);
    lv_obj_set_size(weatherMinutelyXLabels[i], minutelyXWidths[i], 22);
    lv_obj_set_style_text_align(weatherMinutelyXLabels[i],
                                minutelyXAlignments[i], 0);
  }
  createWeatherInfoPageDots(weatherInfoTrendPage, 1);

  lv_obj_add_flag(weatherInfoTrendPage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(weatherInfoPage, LV_OBJ_FLAG_HIDDEN);
}

bool readExact(File &file, uint8_t *destination, size_t bytes) {
  size_t read = 0;
  while (read < bytes) {
    const int count = file.read(destination + read, bytes - read);
    if (count <= 0) return false;
    read += static_cast<size_t>(count);
  }
  return true;
}

bool readExactInterruptible(File &file, uint8_t *destination, size_t bytes) {
  size_t read = 0;
  while (read < bytes) {
    if (pipelinePauseRequested || !weatherPageVisible || lowPowerMode ||
        controlPanelVisible || controlPanelShowRequested)
      return false;
    const size_t requested = min(bytes - read, SD_INTERRUPTIBLE_READ_BYTES);
    const int count = file.read(destination + read, requested);
    if (count <= 0) return false;
    read += static_cast<size_t>(count);
  }
  return true;
}

bool validHeader(const WallpaperHeader &header, bool rotatedAsset) {
  const uint16_t expectedWidth = rotatedAsset ? VIDEO_HEIGHT : VIDEO_WIDTH;
  const uint16_t expectedHeight = rotatedAsset ? VIDEO_WIDTH : VIDEO_HEIGHT;
  const bool supportedMagic = memcmp(header.magic, "JWP1", 4) == 0 ||
                              memcmp(header.magic, "MJP1", 4) == 0;
  return supportedMagic && header.version == 1 &&
         header.headerSize == sizeof(WallpaperHeader) &&
         header.width == expectedWidth && header.height == expectedHeight &&
         header.pixelFormat == 1 && header.frameCount > 0 &&
         header.fpsTimes100 > 0 && header.maxFrameBytes <= MAX_JPEG_BYTES;
}

void reportPlaybackError(PlaybackErrorCode code, const String &path,
                         const char *detail) {
  playbackErrorCode = code;
  playbackErrorPending = true;
  const uint32_t now = millis();
  if (code == lastMediaErrorLogCode &&
      now - lastMediaErrorLogAtMs < 5000) return;
  lastMediaErrorLogCode = code;
  lastMediaErrorLogAtMs = now;
  Serial.printf("MJPEG error: %s: %s\n", path.c_str(),
                detail ? detail : "unknown error");
}

bool assetTypeFromPath(const String &path, WeatherScene &scene, bool &video) {
  String lower = path;
  lower.toLowerCase();
  if (lower.startsWith("/video/") && lower.endsWith(".mjpeg")) {
    scene = WeatherScene::Overcast;
    video = true;
    return true;
  }
  video = false;
  if (lower.indexOf("/clear/") >= 0) scene = WeatherScene::Clear;
  else if (lower.indexOf("/cloudy/") >= 0) scene = WeatherScene::Cloudy;
  else if (lower.indexOf("/overcast/") >= 0) scene = WeatherScene::Overcast;
  else if (lower.indexOf("/rain/") >= 0) scene = WeatherScene::Rain;
  else if (lower.indexOf("/thunder/") >= 0) scene = WeatherScene::Thunder;
  else if (lower.indexOf("/snow/") >= 0) scene = WeatherScene::Snow;
  else if (lower.indexOf("/fog/") >= 0) scene = WeatherScene::Fog;
  else if (lower.indexOf("/haze/") >= 0) scene = WeatherScene::Haze;
  else if (lower.indexOf("/sand/") >= 0) scene = WeatherScene::Sand;
  else if (lower.indexOf("/night/") >= 0) scene = WeatherScene::Night;
  else return false;
  return lower.endsWith(".jwp");
}

void scanWallpaperDirectory(File &directory, uint8_t depth = 0) {
  if (!directory || !directory.isDirectory() || depth > 4) return;
  File entry;
  while ((entry = directory.openNextFile())) {
    if (entry.isDirectory()) {
      scanWallpaperDirectory(entry, depth + 1);
    } else {
      String path = entry.path() ? entry.path() : entry.name();
      if (!path.startsWith("/")) path = "/" + path;
      WeatherScene scene;
      bool video = false;
      if (assetTypeFromPath(path, scene, video)) {
        String lower = path;
        lower.toLowerCase();
        const int rotatedMarker = lower.indexOf("/rotated/");
        String pairKey = path;
        if (rotatedMarker >= 0) pairKey.remove(rotatedMarker, 8);

        int16_t pairIndex = -1;
        for (uint8_t index = 0; index < weatherWallpaperCount; ++index) {
          if (weatherWallpapers[index].key.equalsIgnoreCase(pairKey)) {
            pairIndex = index;
            break;
          }
        }
        if (pairIndex < 0) {
          if (weatherWallpaperCount < MAX_WALLPAPER_FILES) {
            pairIndex = weatherWallpaperCount++;
            weatherWallpapers[pairIndex].key = pairKey;
            weatherWallpapers[pairIndex].path = "";
            weatherWallpapers[pairIndex].rotatedPath = "";
            weatherWallpapers[pairIndex].scene = scene;
            weatherWallpapers[pairIndex].video = video;
          } else {
            ++mediaScanCapacityDrops;
          }
        }
        if (pairIndex >= 0) {
          if (rotatedMarker >= 0)
            weatherWallpapers[pairIndex].rotatedPath = path;
          else
            weatherWallpapers[pairIndex].path = path;
        }
      }
    }
    entry.close();
  }
}

bool scanWeatherWallpapers() {
  weatherWallpaperCount = 0;
  mediaScanCapacityDrops = 0;
  // Videos are user content and must not disappear merely because a large
  // wallpaper library filled the fixed registry first.
  File root = SD.open("/video");
  if (root && root.isDirectory()) {
    scanWallpaperDirectory(root);
  }
  if (root) root.close();
  root = SD.open("/wallpaper");
  if (root && root.isDirectory()) {
    scanWallpaperDirectory(root);
  }
  if (root) root.close();

  // A rotated-only orphan is unusable because the landscape member is the
  // canonical playlist entry. Compact those out and sort the pairs so +/- and
  // manual swipes have stable, filename-based ordering.
  uint8_t kept = 0;
  for (uint8_t index = 0; index < weatherWallpaperCount; ++index) {
    if (!weatherWallpapers[index].path.length()) continue;
    if (kept != index) weatherWallpapers[kept] = weatherWallpapers[index];
    ++kept;
  }
  weatherWallpaperCount = kept;
  for (uint8_t left = 0; left < weatherWallpaperCount; ++left) {
    for (uint8_t right = left + 1; right < weatherWallpaperCount; ++right) {
      if (weatherWallpapers[right].path.compareTo(weatherWallpapers[left].path) < 0) {
        WeatherWallpaperAsset temporary = weatherWallpapers[left];
        weatherWallpapers[left] = weatherWallpapers[right];
        weatherWallpapers[right] = temporary;
      }
    }
  }
  uint8_t rotatedCount = 0;
  uint8_t videoCount = 0;
  for (uint8_t index = 0; index < weatherWallpaperCount; ++index) {
    if (weatherWallpapers[index].rotatedPath.length()) ++rotatedCount;
    if (weatherWallpapers[index].video) ++videoCount;
  }
  Serial.printf("Media scan: %u pair(s), %u video(s), %u rotated file(s)\n",
                weatherWallpaperCount, videoCount, rotatedCount);
  if (mediaScanCapacityDrops)
    Serial.printf("Media scan warning: registry full, ignored %u file(s)\n",
                  mediaScanCapacityDrops);
  return weatherWallpaperCount > 0;
}

const String &assetPath(uint8_t index, bool rotatedAsset) {
  return rotatedAsset ? weatherWallpapers[index].rotatedPath
                      : weatherWallpapers[index].path;
}

void videoPositionKey(uint8_t index, char *key, size_t keySize) {
  uint32_t hash = 2166136261UL;
  if (index < weatherWallpaperCount) {
    const String &path = weatherWallpapers[index].path;
    for (size_t offset = 0; offset < path.length(); ++offset) {
      hash ^= static_cast<uint8_t>(path[offset]);
      hash *= 16777619UL;
    }
  }
  snprintf(key, keySize, "vp%08lx", static_cast<unsigned long>(hash));
}

void loadVideoPosition(uint8_t index, uint32_t &frame, uint32_t &fileOffset) {
  frame = 0;
  fileOffset = 0;
  if (!playerVideoMode || index >= weatherWallpaperCount ||
      !weatherWallpapers[index].video) return;
  char key[12]{};
  videoPositionKey(index, key, sizeof(key));
  const uint64_t packed = preferences.getULong64(key, 0);
  if (!packed) return;
  frame = static_cast<uint32_t>(packed >> 32U);
  fileOffset = static_cast<uint32_t>(packed);
}

void persistVideoPosition(bool force = false) {
  if (!playerVideoMode || currentVideoPositionIndex >= weatherWallpaperCount ||
      !weatherWallpapers[currentVideoPositionIndex].video ||
      !currentVideoPositionFileOffset) return;
  const uint32_t now = millis();
  if (!force && now - lastVideoResumeSaveAtMs < VIDEO_RESUME_SAVE_MS) return;
  if (lastSavedVideoIndex == currentVideoPositionIndex &&
      lastSavedVideoFrame == currentVideoPositionFrame) return;
  char key[12]{};
  videoPositionKey(currentVideoPositionIndex, key, sizeof(key));
  const uint64_t packed =
    (static_cast<uint64_t>(currentVideoPositionFrame) << 32U) |
    currentVideoPositionFileOffset;
  preferences.putULong64(key, packed);
  lastVideoResumeSaveAtMs = now;
  lastSavedVideoIndex = currentVideoPositionIndex;
  lastSavedVideoFrame = currentVideoPositionFrame;
}

String videoAudioSidecarPath(uint8_t index) {
  if (index >= weatherWallpaperCount) return "";
  String path = weatherWallpapers[index].path;
  const int extension = path.lastIndexOf('.');
  if (extension >= 0) path.remove(extension);
  path += ".wav";
  return path;
}

void syncVideoAudio(const VideoDisplayResult &result) {
  if (!playerVideoMode || !result.fpsTimes100 ||
      result.wallpaperIndex >= weatherWallpaperCount || alarmToneActive) return;
  const bool sourceChanged =
    videoAudioGeneration != result.generation ||
    videoAudioWallpaperIndex != result.wallpaperIndex;
  const uint32_t oneSecondFrames =
    (result.fpsTimes100 + 99U) / 100U;
  const bool looped =
    !sourceChanged &&
    result.index + oneSecondFrames < videoAudioLastFrame;
  const bool interrupted = videoAudioPlaybackRestartPending();
  const uint32_t nowMs = millis();
  const bool retryDue =
    videoAudioAttempted && !videoAudioPlaybackActive() &&
    videoAudioRetryAtMs &&
    static_cast<int32_t>(nowMs - videoAudioRetryAtMs) >= 0;
  bool shouldStart =
    sourceChanged || looped || interrupted ||
    (!videoAudioAttempted && !videoAudioPlaybackActive()) || retryDue;
  if (!shouldStart && videoAudioPlaybackActive() &&
      nowMs - videoAudioLastDriftCheckMs >= 1000U) {
    videoAudioLastDriftCheckMs = nowMs;
    const int64_t videoPositionMs = static_cast<int64_t>(
      (static_cast<uint64_t>(result.index) * 100000ULL) /
      result.fpsTimes100);
    const int64_t audioPositionMs =
      videoAudioPlaybackPositionMs(result.displayedAtUs);
    const int32_t driftMs = static_cast<int32_t>(
      videoPositionMs - audioPositionMs);
    videoAudioLastVideoPositionMs = static_cast<uint32_t>(videoPositionMs);
    videoAudioLastAudioPositionMs = static_cast<uint32_t>(audioPositionMs);
    videoAudioLastMeasuredDriftMs = driftMs;
    if (abs(driftMs) >= 200) {
      if (++videoAudioDriftStrikes >= 2) {
        Serial.printf("Video audio: drift %+ld ms, resyncing at frame %lu\n",
                      static_cast<long>(driftMs),
                      static_cast<unsigned long>(result.index + 1U));
        shouldStart = true;
      }
    } else {
      videoAudioDriftStrikes = 0;
    }
  }
  if (shouldStart) {
    const String path = videoAudioSidecarPath(result.wallpaperIndex);
    uint32_t positionMs = static_cast<uint32_t>(
      (static_cast<uint64_t>(result.index) * 100000ULL) /
      result.fpsTimes100);
    const uint64_t nowUs = esp_timer_get_time();
    if (nowUs > result.displayedAtUs)
      positionMs += static_cast<uint32_t>(
        (nowUs - result.displayedAtUs) / 1000ULL);
    videoAudioAttempted = true;
    const bool started = startVideoAudioPlayback(path, positionMs);
    if (started) {
      videoAudioRetryAtMs = 0;
      videoAudioRetryDelayMs = VIDEO_AUDIO_RETRY_INITIAL_MS;
    } else {
      videoAudioRetryAtMs = nowMs + videoAudioRetryDelayMs;
      Serial.printf("Video audio: retry in %lu ms\n",
                    static_cast<unsigned long>(videoAudioRetryDelayMs));
      videoAudioRetryDelayMs =
        min<uint32_t>(videoAudioRetryDelayMs * 2U,
                      VIDEO_AUDIO_RETRY_MAX_MS);
    }
    videoAudioGeneration = result.generation;
    videoAudioWallpaperIndex = result.wallpaperIndex;
    videoAudioLastDriftCheckMs = nowMs;
    videoAudioDriftStrikes = 0;
    videoAudioLastVideoPositionMs = positionMs;
    videoAudioLastAudioPositionMs = positionMs;
    videoAudioLastMeasuredDriftMs = 0;
  }
  videoAudioLastFrame = result.index;
}

void resetVideoAudioSync(bool stopPlayback) {
  if (stopPlayback) stopVideoAudioPlayback();
  videoAudioGeneration = 0;
  videoAudioWallpaperIndex = UINT8_MAX;
  videoAudioLastFrame = 0;
  videoAudioAttempted = false;
  videoAudioRetryAtMs = 0;
  videoAudioRetryDelayMs = VIDEO_AUDIO_RETRY_INITIAL_MS;
  videoAudioLastDriftCheckMs = 0;
  videoAudioDriftStrikes = 0;
  videoAudioLastVideoPositionMs = 0;
  videoAudioLastAudioPositionMs = 0;
  videoAudioLastMeasuredDriftMs = 0;
}

void setReaderRequest(uint8_t index, bool rotatedAsset, uint32_t frame,
                      uint32_t fileOffset, bool advanceGeneration) {
  portENTER_CRITICAL(&stateMux);
  requestedReaderIndex = index;
  requestedReaderRotated = rotatedAsset;
  requestedReaderFrame = frame;
  requestedReaderFileOffset = fileOffset;
  if (advanceGeneration) ++pipelineGeneration;
  portEXIT_CRITICAL(&stateMux);
  if (advanceGeneration) displayedGeneration = 0;
}

void setReaderAsset(uint8_t index, bool rotatedAsset, bool restoreVideoPosition,
                    bool advanceGeneration) {
  uint32_t frame = 0;
  uint32_t fileOffset = 0;
  if (restoreVideoPosition) loadVideoPosition(index, frame, fileOffset);
  setReaderRequest(index, rotatedAsset, frame, fileOffset, advanceGeneration);
}

bool readerRequestStillCurrent(uint32_t generation, uint8_t index,
                               bool rotatedAsset) {
  bool current = false;
  portENTER_CRITICAL(&stateMux);
  current = generation == pipelineGeneration &&
            index == requestedReaderIndex &&
            rotatedAsset == requestedReaderRotated;
  portEXIT_CRITICAL(&stateMux);
  return current && !pipelinePauseRequested;
}

bool seekReaderFrame(File &file, const WallpaperHeader &header,
                     uint32_t &targetFrame, uint32_t requestedFileOffset,
                     uint32_t generation, uint8_t index, bool rotatedAsset) {
  if (targetFrame >= header.frameCount) targetFrame = 0;
  if (!targetFrame) return file.seek(header.headerSize);

  const uint64_t fileBytes = file.size();

  // New converter output appends IDX1 + one uint32 file offset per frame and
  // stores the index position in the formerly-reserved header field. Byte-ratio
  // seeking can land on a valid JPEG that belongs to a different timestamp
  // when scene complexity changes, permanently desynchronizing audio after a
  // slider seek. The embedded table makes target frame and audio time exact.
  const uint64_t indexBytes =
    sizeof(MjpegIndexHeader) +
    static_cast<uint64_t>(header.frameCount) * sizeof(uint32_t);
  if (header.reserved >= header.headerSize &&
      static_cast<uint64_t>(header.reserved) + indexBytes <= fileBytes &&
      file.seek(header.reserved)) {
    MjpegIndexHeader indexHeader{};
    uint32_t indexedOffset = 0;
    const uint64_t offsetPosition =
      static_cast<uint64_t>(header.reserved) + sizeof(indexHeader) +
      static_cast<uint64_t>(targetFrame) * sizeof(indexedOffset);
    const bool indexValid =
      readExact(file, reinterpret_cast<uint8_t *>(&indexHeader),
                sizeof(indexHeader)) &&
      memcmp(indexHeader.magic, "IDX1", 4) == 0 &&
      indexHeader.frameCount == header.frameCount &&
      offsetPosition + sizeof(indexedOffset) <= fileBytes &&
      file.seek(static_cast<uint32_t>(offsetPosition)) &&
      readExact(file, reinterpret_cast<uint8_t *>(&indexedOffset),
                sizeof(indexedOffset)) &&
      indexedOffset >= header.headerSize && indexedOffset < header.reserved;
    if (indexValid && file.seek(indexedOffset)) {
      uint32_t byteCount = 0;
      uint8_t soi[2]{};
      const bool frameValid =
        readExact(file, reinterpret_cast<uint8_t *>(&byteCount),
                  sizeof(byteCount)) &&
        byteCount > 0 && byteCount <= header.maxFrameBytes &&
        byteCount <= MAX_JPEG_BYTES &&
        static_cast<uint64_t>(indexedOffset) + sizeof(uint32_t) +
          byteCount <= header.reserved &&
        readExact(file, soi, sizeof(soi)) &&
        soi[0] == 0xFF && soi[1] == 0xD8 &&
        file.seek(indexedOffset);
      if (frameValid) {
        Serial.printf(
          "Video seek index: exact frame %lu/%lu at byte %lu\n",
          static_cast<unsigned long>(targetFrame + 1U),
          static_cast<unsigned long>(header.frameCount),
          static_cast<unsigned long>(indexedOffset));
        return true;
      }
    }
    Serial.println("Video seek index invalid; using compatibility search");
  }

  // A persisted resume position stores the displayed frame and its exact file
  // offset together. For an unchanged legacy file this is already an exact,
  // instant seek. If the file was replaced or regenerated, do not scan many
  // thousands of frames while the UI remains on Loading: discard the stale
  // resume point and begin at frame zero. Explicit slider seeks use offset 0
  // and continue into the compatibility search below.
  if (requestedFileOffset) {
    const uint64_t payloadEnd =
      header.reserved >= header.headerSize && header.reserved <= fileBytes
        ? header.reserved : fileBytes;
    bool resumeValid =
      requestedFileOffset >= header.headerSize &&
      static_cast<uint64_t>(requestedFileOffset) + sizeof(uint32_t) + 2U <
        payloadEnd &&
      file.seek(requestedFileOffset);
    uint32_t byteCount = 0;
    uint8_t soi[2]{};
    resumeValid = resumeValid &&
      readExact(file, reinterpret_cast<uint8_t *>(&byteCount),
                sizeof(byteCount)) &&
      byteCount > 0 && byteCount <= header.maxFrameBytes &&
      byteCount <= MAX_JPEG_BYTES &&
      static_cast<uint64_t>(requestedFileOffset) + sizeof(uint32_t) +
        byteCount <= payloadEnd &&
      readExact(file, soi, sizeof(soi)) &&
      soi[0] == 0xFF && soi[1] == 0xD8 &&
      file.seek(requestedFileOffset);
    if (resumeValid) {
      Serial.printf("Video resume: exact frame %lu/%lu at byte %lu\n",
                    static_cast<unsigned long>(targetFrame + 1U),
                    static_cast<unsigned long>(header.frameCount),
                    static_cast<unsigned long>(requestedFileOffset));
      return true;
    }

    Serial.println("Video resume offset is stale; restarting from frame 1");
    targetFrame = 0;
    return file.seek(header.headerSize);
  }

  // Legacy MJP1 files have no frame index. Exact walking can require thousands
  // of FAT seeks and freeze the Loading page. Estimate the byte position and
  // validate a nearby complete JPEG boundary instead. Current converter output
  // includes IDX1 and always takes the exact, instant path above.
  const uint32_t startedAtMs = millis();
  const uint64_t payloadEnd =
    header.reserved >= header.headerSize && header.reserved <= fileBytes
      ? header.reserved : fileBytes;
  const uint64_t payloadBytes = payloadEnd - header.headerSize;
  const uint64_t estimatedOffset =
    header.headerSize +
    (payloadBytes * targetFrame) / header.frameCount;
  const uint64_t searchRadius =
    static_cast<uint64_t>(header.maxFrameBytes) + sizeof(uint32_t) + 8U;
  const uint64_t searchStart =
    estimatedOffset > searchRadius
      ? estimatedOffset - searchRadius : header.headerSize;
  const uint64_t searchEnd =
    min<uint64_t>(payloadEnd, estimatedOffset + searchRadius);
  if (!file.seek(static_cast<uint32_t>(searchStart))) return false;

  uint8_t recent[6]{};
  uint8_t scanBuffer[256]{};
  size_t recentBytes = 0;
  while (file.position() < searchEnd) {
    if (!readerRequestStillCurrent(generation, index, rotatedAsset))
      return false;
    const uint32_t chunkStart = static_cast<uint32_t>(file.position());
    const size_t requested = static_cast<size_t>(
      min<uint64_t>(sizeof(scanBuffer), searchEnd - chunkStart));
    const int count = file.read(scanBuffer, requested);
    if (count <= 0) break;
    const uint32_t nextReadOffset = static_cast<uint32_t>(file.position());
    for (int offset = 0; offset < count; ++offset) {
      if (recentBytes < sizeof(recent)) {
        recent[recentBytes++] = scanBuffer[offset];
      } else {
        memmove(recent, recent + 1, sizeof(recent) - 1U);
        recent[sizeof(recent) - 1U] = scanBuffer[offset];
      }
      if (recentBytes < sizeof(recent) ||
          recent[4] != 0xFF || recent[5] != 0xD8) continue;

      const uint32_t byteCount =
        static_cast<uint32_t>(recent[0]) |
        (static_cast<uint32_t>(recent[1]) << 8U) |
        (static_cast<uint32_t>(recent[2]) << 16U) |
        (static_cast<uint32_t>(recent[3]) << 24U);
      const uint32_t candidateOffset =
        chunkStart + static_cast<uint32_t>(offset + 1) - sizeof(recent);
      const uint64_t frameEnd =
        static_cast<uint64_t>(candidateOffset) + sizeof(uint32_t) + byteCount;
      if (!byteCount || byteCount > header.maxFrameBytes ||
          byteCount > MAX_JPEG_BYTES || frameEnd > payloadEnd) continue;

      uint8_t eoi[2]{};
      const bool validBoundary =
        file.seek(static_cast<uint32_t>(frameEnd - sizeof(eoi))) &&
        readExact(file, eoi, sizeof(eoi)) &&
        eoi[0] == 0xFF && eoi[1] == 0xD9;
      if (validBoundary && file.seek(candidateOffset)) {
        Serial.printf(
          "Video seek legacy: approximate frame %lu/%lu near byte %lu in %lu ms; "
          "reconvert for exact IDX1 seek\n",
          static_cast<unsigned long>(targetFrame + 1U),
          static_cast<unsigned long>(header.frameCount),
          static_cast<unsigned long>(candidateOffset),
          static_cast<unsigned long>(millis() - startedAtMs));
        return true;
      }
      if (!file.seek(nextReadOffset)) return false;
    }
    if (file.position() != nextReadOffset && !file.seek(nextReadOffset))
      return false;
  }
  return false;
}

bool openReaderAsset(File &file, WallpaperHeader &header, uint8_t index,
                     bool rotatedAsset) {
  if (index >= weatherWallpaperCount) return false;
  if (file) file.close();
  const String &path = assetPath(index, rotatedAsset);
  if (!path.length()) {
    Serial.printf("Weather wallpaper missing %s asset for %s\n",
                  rotatedAsset ? "rotated" : "landscape",
                  weatherWallpapers[index].path.c_str());
    return false;
  }
  file = SD.open(path.c_str(), FILE_READ);
  if (!file) {
    reportPlaybackError(PlaybackErrorCode::OpenFile, path,
                        "cannot open file from TF card");
    return false;
  }
  if (!readExact(file, reinterpret_cast<uint8_t *>(&header), sizeof(header))) {
    reportPlaybackError(PlaybackErrorCode::ReadHeader, path,
                        "file is shorter than the 32-byte MJP1 header");
    file.close();
    return false;
  }
  const bool correctVideoMagic =
    !weatherWallpapers[index].video || memcmp(header.magic, "MJP1", 4) == 0;
  if (!validHeader(header, rotatedAsset) || !correctVideoMagic) {
    char detail[192]{};
    snprintf(detail, sizeof(detail),
             "invalid header magic=%.4s version=%u header=%u size=%ux%u "
             "fps=%.2f frames=%lu maxJPEG=%lu pixel=%u",
             header.magic, header.version, header.headerSize, header.width,
             header.height, header.fpsTimes100 / 100.0f,
             static_cast<unsigned long>(header.frameCount),
             static_cast<unsigned long>(header.maxFrameBytes),
             header.pixelFormat);
    reportPlaybackError(PlaybackErrorCode::InvalidHeader, path, detail);
    file.close();
    return false;
  }
  if (file.size() < header.headerSize + sizeof(uint32_t)) {
    reportPlaybackError(PlaybackErrorCode::ReadHeader, path,
                        "header is valid but the first frame is missing");
    file.close();
    return false;
  }
  activeIndex = index;
  playbackErrorCode = PlaybackErrorCode::None;
  Serial.printf("Media opened: %s (%s, %.2f fps, %ux%u, %lu frames, "
                "max JPEG %lu bytes, file %llu bytes)\n",
                 path.c_str(),
                 sceneName(weatherWallpapers[index].scene),
                 header.fpsTimes100 / 100.0f, header.width, header.height,
                 static_cast<unsigned long>(header.frameCount),
                 static_cast<unsigned long>(header.maxFrameBytes),
                 static_cast<unsigned long long>(file.size()));
  return true;
}

bool rotatedAssetRequired() {
  return displayRotation == LV_DISP_ROT_90 || displayRotation == LV_DISP_ROT_270;
}

bool assetAvailable(uint8_t index, bool rotatedAsset) {
  if (index >= weatherWallpaperCount) return false;
  return rotatedAsset ? weatherWallpapers[index].rotatedPath.length() > 0
                      : weatherWallpapers[index].path.length() > 0;
}

bool assetInCurrentMode(uint8_t index) {
  return index < weatherWallpaperCount &&
         weatherWallpapers[index].video == playerVideoMode;
}

int16_t findAsset(WeatherScene scene, uint8_t base, int8_t direction = 1) {
  if (!weatherWallpaperCount) return -1;
  const bool rotatedAsset = rotatedAssetRequired();
  const int step = direction < 0 ? -1 : 1;
  for (uint8_t offset = 1; offset <= weatherWallpaperCount; ++offset) {
    const int candidate = (static_cast<int>(base) + step * offset +
                           weatherWallpaperCount) % weatherWallpaperCount;
    if (assetInCurrentMode(candidate) &&
        weatherWallpapers[candidate].scene == scene &&
        assetAvailable(candidate, rotatedAsset)) return candidate;
  }
  return -1;
}

int16_t findAnyWeatherAsset(uint8_t base, int8_t direction = 1) {
  if (!weatherWallpaperCount) return -1;
  const bool rotatedAsset = rotatedAssetRequired();
  const int step = direction < 0 ? -1 : 1;
  for (uint8_t offset = 1; offset <= weatherWallpaperCount; ++offset) {
    const int candidate = (static_cast<int>(base) + step * offset +
                           weatherWallpaperCount) % weatherWallpaperCount;
    if (!weatherWallpapers[candidate].video &&
        assetAvailable(candidate, rotatedAsset)) return candidate;
  }
  return -1;
}

int16_t randomAsset(WeatherScene scene, int16_t exclude = -1) {
  if (!weatherWallpaperCount) return -1;
  const bool rotatedAsset = rotatedAssetRequired();
  uint8_t matches = 0;
  for (uint8_t index = 0; index < weatherWallpaperCount; ++index) {
    if (assetInCurrentMode(index) &&
        weatherWallpapers[index].scene == scene &&
        assetAvailable(index, rotatedAsset) && index != exclude) ++matches;
  }
  // A one-file scene should continue playing instead of failing merely
  // because the current entry was excluded.
  if (!matches && exclude >= 0 && exclude < weatherWallpaperCount &&
      assetInCurrentMode(static_cast<uint8_t>(exclude)) &&
      weatherWallpapers[exclude].scene == scene &&
      assetAvailable(static_cast<uint8_t>(exclude), rotatedAsset)) return exclude;
  if (!matches) return -1;
  uint8_t selected = static_cast<uint8_t>(esp_random() % matches);
  for (uint8_t index = 0; index < weatherWallpaperCount; ++index) {
    if (!assetInCurrentMode(index) ||
        weatherWallpapers[index].scene != scene || index == exclude ||
        !assetAvailable(index, rotatedAsset)) continue;
    if (!selected--) return index;
  }
  return -1;
}

int16_t firstAvailableAsset() {
  if (playerVideoMode) {
    const bool rotatedAsset = rotatedAssetRequired();
    const String lastVideo = preferences.getString("last_video", "");
    if (!lastVideo.isEmpty()) {
      for (uint8_t index = 0; index < weatherWallpaperCount; ++index) {
        if (assetInCurrentMode(index) && assetAvailable(index, rotatedAsset) &&
            weatherWallpapers[index].key.equalsIgnoreCase(lastVideo)) {
          Serial.printf("Video selection restored: %s\n", lastVideo.c_str());
          return index;
        }
      }
    }
    for (uint8_t index = 0; index < weatherWallpaperCount; ++index) {
      if (assetInCurrentMode(index) && assetAvailable(index, rotatedAsset))
        return index;
    }
    return -1;
  }
  if (!weatherWallpaperFollowEnabled) {
    const bool rotatedAsset = rotatedAssetRequired();
    const String lastWallpaper = preferences.getString("last_wall", "");
    if (!lastWallpaper.isEmpty()) {
      for (uint8_t index = 0; index < weatherWallpaperCount; ++index) {
        if (!weatherWallpapers[index].video &&
            assetAvailable(index, rotatedAsset) &&
            weatherWallpapers[index].key.equalsIgnoreCase(lastWallpaper)) {
          Serial.printf("Weather wallpaper selection restored: %s\n",
                        lastWallpaper.c_str());
          return index;
        }
      }
    }
    for (uint8_t index = 0; index < weatherWallpaperCount; ++index) {
      if (!weatherWallpapers[index].video && assetAvailable(index, rotatedAsset))
        return index;
    }
    return -1;
  }
  const WeatherScene desired = sceneForCode(weatherCode);
  int16_t found = randomAsset(desired);
  if (found < 0) found = randomAsset(WeatherScene::Overcast);
  if (found < 0) found = randomAsset(WeatherScene::Clear);
  if (found < 0) {
    const bool rotatedAsset = rotatedAssetRequired();
    for (uint8_t index = 0; index < weatherWallpaperCount; ++index) {
      if (assetInCurrentMode(index) && assetAvailable(index, rotatedAsset))
        return index;
    }
  }
  return found;
}

void wallpaperReaderTask(void *);
void wallpaperDisplayTask(void *);
void wallpaperDecodeTask(void *);
bool decodeReadyFrame();
bool pausePipeline(bool announce = true);
bool resumePipeline();

bool ensureBlockDecoder() {
  if (jpegDecoder) return true;
  // The standalone 09 player opens and drives the hardware JPEG decoder from
  // core1. Keep that ownership invariant after TLS temporarily releases it;
  // opening it from the core0 network task measurably increased process time.
  if (xPortGetCoreID() != 1) {
    Serial.printf("Weather JPEG decoder creation deferred from core %d\n",
                  xPortGetCoreID());
    return false;
  }
  jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
  config.output_type = JPEG_PIXEL_FORMAT_RGB565_BE;
  config.rotate = JPEG_ROTATE_0D;
  config.block_enable = true;
  if (jpeg_dec_open(&config, &jpegDecoder) != JPEG_ERR_OK || !jpegDecoder)
    return false;
  Serial.printf("Weather JPEG decoder opened on core %d\n", xPortGetCoreID());
  return true;
}

bool ensurePlaybackResources() {
  bool buffersReady = true;
  for (uint8_t slot = 0; slot < PIPELINE_BUFFER_COUNT; ++slot)
    buffersReady = buffersReady && videoFrameBuffers[slot];

  if (!buffersReady) {
    // Weather mode keeps a sacrificial TLS reserve. Video mode pauses network
    // access and allocates only the DMA arena so I2S can use the remaining
    // internal DMA SRAM; the 8 MB PSRAM cannot host I2S DMA descriptors.
    const size_t preferredArenaBytes =
      playerVideoMode ? PLAYBACK_DMA_ARENA_BYTES
                      : PLAYBACK_INTERNAL_ARENA_BYTES;
    playbackInternalArena = static_cast<uint8_t *>(heap_caps_aligned_alloc(
      16, preferredArenaBytes,
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA));
    if (!playbackInternalArena &&
        preferredArenaBytes != PLAYBACK_DMA_ARENA_BYTES) {
      playbackInternalArena = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        16, PLAYBACK_DMA_ARENA_BYTES,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA));
      if (playbackInternalArena) {
        Serial.printf(
          "Weather playback arena: TLS reserve unavailable; using %lu KiB DMA arena "
          "(largest block %lu KiB)\n",
          static_cast<unsigned long>(PLAYBACK_DMA_ARENA_BYTES / 1024U),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U));
      }
    }

    if (playbackInternalArena && playerVideoMode) {
      Serial.printf(
        "Video playback arena: %lu KiB DMA arena; SRAM free/largest %lu/%lu KiB\n",
        static_cast<unsigned long>(PLAYBACK_DMA_ARENA_BYTES / 1024U),
        static_cast<unsigned long>(heap_caps_get_free_size(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U),
        static_cast<unsigned long>(heap_caps_get_largest_free_block(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U));
    }

    if (playbackInternalArena) {
      for (uint8_t slot = 0; slot < PIPELINE_BUFFER_COUNT; ++slot) {
        videoFrameBuffers[slot] = reinterpret_cast<lv_color_t *>(
          playbackInternalArena +
          static_cast<size_t>(slot) * DISPLAY_DMA_BLOCK_BYTES);
      }
    } else {
      // Last resort for a heavily fragmented heap: allocate each required DMA
      // strip independently.
      for (uint8_t slot = 0; slot < PIPELINE_BUFFER_COUNT; ++slot) {
        videoFrameBuffers[slot] = static_cast<lv_color_t *>(
          heap_caps_aligned_alloc(
            16, DISPLAY_DMA_BLOCK_BYTES,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA));
        if (!videoFrameBuffers[slot]) {
          for (uint8_t allocated = 0; allocated <= slot; ++allocated) {
            if (videoFrameBuffers[allocated])
              heap_caps_free(videoFrameBuffers[allocated]);
            videoFrameBuffers[allocated] = nullptr;
          }
          return false;
        }
      }
      Serial.printf(
        "Weather playback arena: fragmented fallback, %u x %lu KiB DMA strips\n",
        static_cast<unsigned>(PIPELINE_BUFFER_COUNT),
        static_cast<unsigned long>(DISPLAY_DMA_BLOCK_BYTES / 1024U));
    }
  }
  return ensureBlockDecoder();
}

bool ensureDecodeTask() {
  if (wallpaperDecodeTaskHandle) return true;
  return xTaskCreatePinnedToCore(wallpaperDecodeTask, "clock_jpeg_dec", 4096,
                                 nullptr, 2, &wallpaperDecodeTaskHandle, 1) == pdPASS;
}

void deletePipelineQueue(QueueHandle_t &queue) {
  if (!queue) return;
  vQueueDelete(queue);
  queue = nullptr;
}

void releasePipelineWorkers() {
  if (wallpaperReaderTaskHandle) {
    vTaskDelete(wallpaperReaderTaskHandle);
    wallpaperReaderTaskHandle = nullptr;
  }
  if (wallpaperDisplayTaskHandle) {
    vTaskDelete(wallpaperDisplayTaskHandle);
    wallpaperDisplayTaskHandle = nullptr;
  }
  if (wallpaperDecodeTaskHandle) {
    vTaskDelete(wallpaperDecodeTaskHandle);
    wallpaperDecodeTaskHandle = nullptr;
  }
  deletePipelineQueue(freeCompressedQueue);
  deletePipelineQueue(readyCompressedQueue);
  deletePipelineQueue(freeVideoQueue);
  deletePipelineQueue(readyVideoQueue);
  deletePipelineQueue(videoDisplayResultQueue);
  readerIdle = true;
  decoderIdle = true;
  displayIdle = true;
}

bool ensurePipelineQueues() {
  if (freeCompressedQueue && readyCompressedQueue && freeVideoQueue &&
      readyVideoQueue && videoDisplayResultQueue) return true;
  deletePipelineQueue(freeCompressedQueue);
  deletePipelineQueue(readyCompressedQueue);
  deletePipelineQueue(freeVideoQueue);
  deletePipelineQueue(readyVideoQueue);
  deletePipelineQueue(videoDisplayResultQueue);
  freeCompressedQueue = xQueueCreate(PIPELINE_BUFFER_COUNT, sizeof(uint8_t));
  readyCompressedQueue =
    xQueueCreate(PIPELINE_BUFFER_COUNT, sizeof(CompressedFrame));
  freeVideoQueue = xQueueCreate(PIPELINE_BUFFER_COUNT, sizeof(uint8_t));
  readyVideoQueue = xQueueCreate(PIPELINE_BUFFER_COUNT, sizeof(ReadyVideoFrame));
  videoDisplayResultQueue = xQueueCreate(8, sizeof(VideoDisplayResult));
  if (!freeCompressedQueue || !readyCompressedQueue || !freeVideoQueue ||
      !readyVideoQueue || !videoDisplayResultQueue) {
    deletePipelineQueue(freeCompressedQueue);
    deletePipelineQueue(readyCompressedQueue);
    deletePipelineQueue(freeVideoQueue);
    deletePipelineQueue(readyVideoQueue);
    deletePipelineQueue(videoDisplayResultQueue);
    return false;
  }
  for (uint8_t slot = 0; slot < PIPELINE_BUFFER_COUNT; ++slot) {
    xQueueSend(freeCompressedQueue, &slot, 0);
    xQueueSend(freeVideoQueue, &slot, 0);
  }
  return true;
}

bool ensurePipelineTasks() {
  if (!wallpaperReaderTaskHandle &&
      xTaskCreatePinnedToCore(wallpaperReaderTask, "clock_jpeg_read", 6144,
                              nullptr, 2, &wallpaperReaderTaskHandle, 0) != pdPASS)
    goto failed;
  if (!wallpaperDisplayTaskHandle &&
      xTaskCreatePinnedToCore(wallpaperDisplayTask, "clock_wall_lcd", 4096,
                              nullptr, 3, &wallpaperDisplayTaskHandle, 0) != pdPASS)
    goto failed;
  if (!ensureDecodeTask()) goto failed;
  return true;

failed:
  releasePipelineWorkers();
  return false;
}

void releasePlaybackResources() {
  // This function is called only after pausePipeline() observed decoderIdle.
  // Tear down every media worker and queue, not only the decoder. Video mode's
  // 30 KiB DMA arena alone is too small for a later 32 KiB TLS record pair;
  // reclaiming the adjacent task stacks/queues restores the larger boot-time
  // allocation window after leaving video playback.
  releasePipelineWorkers();
  if (jpegDecoder) {
    jpeg_dec_close(jpegDecoder);
    jpegDecoder = nullptr;
  }
  if (playbackInternalArena) {
    heap_caps_free(playbackInternalArena);
    playbackInternalArena = nullptr;
  } else {
    for (uint8_t slot = 0; slot < PIPELINE_BUFFER_COUNT; ++slot) {
      if (videoFrameBuffers[slot]) heap_caps_free(videoFrameBuffers[slot]);
    }
  }
  for (uint8_t slot = 0; slot < PIPELINE_BUFFER_COUNT; ++slot)
    videoFrameBuffers[slot] = nullptr;
  // Give the idle task cleanup a scheduling point before mbedTLS measures and
  // allocates its handshake working set.
  vTaskDelay(2);
  Serial.printf("Weather TLS window: SRAM free/largest %lu/%lu KiB\n",
                static_cast<unsigned long>(heap_caps_get_free_size(
                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
                static_cast<unsigned long>(heap_caps_get_largest_free_block(
                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
}

bool initializePlayer() {
  if (initialized) return mountMusicSd();
  if (initializationFailed) return false;
  if (networkResourceHold) {
    setMessage(playerVideoMode ? "Preparing video..."
                               : "Waiting for weather data...");
    return false;
  }
  setMessage(playerVideoMode ? "Loading video..."
                             : "Loading weather wallpaper...");
  Serial.printf("Media initialization: mode=%s rotation=%u\n",
                playerVideoMode ? "video" : "weather",
                static_cast<unsigned>(displayRotation) * 90U);
  stopMusicPlayback();
  if (!mountMusicSd()) {
    setMessage(playerVideoMode ? "TF card unavailable\nCopy files to /video"
                               : "TF card unavailable\nCopy the wallpaper folder",
               lv_color_hex(0xFCA5A5));
    initializationFailed = true;
    return false;
  }
  if (!scanWeatherWallpapers()) {
    setMessage("No MJPEG video or weather wallpaper found",
               lv_color_hex(0xFCA5A5));
    initializationFailed = true;
    return false;
  }

  // I2S DMA cannot live in PSRAM. Reserve it while internal SRAM is still
  // contiguous, then allocate the larger double-buffered video DMA arena.
  if (playerVideoMode && !prepareVideoAudioI2S())
    Serial.println("Video audio: early I2S reservation failed; retrying after first frame");

  for (uint8_t slot = 0; slot < PIPELINE_BUFFER_COUNT; ++slot) {
    if (!jpegBuffers[slot]) jpegBuffers[slot] = static_cast<uint8_t *>(
      heap_caps_aligned_alloc(16, MAX_JPEG_BYTES,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  bool buffersReady = ensurePlaybackResources();
  for (uint8_t slot = 0; slot < PIPELINE_BUFFER_COUNT; ++slot) {
    buffersReady = buffersReady && jpegBuffers[slot];
  }
  if (!buffersReady) {
    setMessage("Wallpaper buffer allocation failed", lv_color_hex(0xFCA5A5));
    initializationFailed = true;
    return false;
  }

  for (uint16_t row = 0; row < VIDEO_CORNER_RADIUS; ++row) {
    // row 0 is the top of the lower-corner band and must be almost uncut;
    // the last row touches the physical bottom edge and needs the largest
    // inset. The previous reversed Y distance produced upside-down corners,
    // which was especially visible on bright-edged clips such as night_03.
    const float dy = static_cast<float>(row) + 0.5f;
    bottomCornerInsets[row] = static_cast<uint8_t>(ceilf(
      VIDEO_CORNER_RADIUS - sqrtf(max(0.0f,
        VIDEO_CORNER_RADIUS * VIDEO_CORNER_RADIUS - dy * dy))));
  }

  if (!ensureBlockDecoder()) {
    setMessage("JPEG decoder initialization failed", lv_color_hex(0xFCA5A5));
    initializationFailed = true;
    return false;
  }

  const int16_t initial = firstAvailableAsset();
  if (initial < 0) {
    Serial.printf("Media initialization failed: no %s %s asset found\n",
                  playerVideoMode ? "MJPEG video" : "weather wallpaper",
                  rotatedAssetRequired() ? "360x480 rotated" : "480x360");
    setMessage(playerVideoMode
                 ? "No MJPEG video found\nCopy .mjpeg files to /video"
                 : "No weather wallpaper found\nCopy /wallpaper to the TF card",
               lv_color_hex(0xFCA5A5));
    initializationFailed = true;
    return false;
  }

  if (!ensurePipelineQueues()) {
    setMessage("Wallpaper queue allocation failed", lv_color_hex(0xFCA5A5));
    initializationFailed = true;
    return false;
  }

  setReaderAsset(static_cast<uint8_t>(initial), rotatedAssetRequired(),
                 playerVideoMode, false);
  pipelinePauseRequested = false;
  readerIdle = true;
  decoderIdle = true;
  displayIdle = true;
  activeIndex = requestedReaderIndex;
  if (!ensurePipelineTasks()) {
    setMessage("Wallpaper task startup failed", lv_color_hex(0xFCA5A5));
    initializationFailed = true;
    return false;
  }

  initialized = true;
  // Keep the loading message until the display task reports a complete first
  // frame. Clearing it here made every startup failure look like a featureless
  // black page and also hid the short TF-card warm-up interval.
  Serial.println("Weather wallpaper pipeline ready (JPEG core1 / SD+LCD core0)");
  Serial.printf("Weather memory: SRAM free/largest %lu/%lu KiB; PSRAM free %lu KiB\n",
                static_cast<unsigned long>(heap_caps_get_free_size(
                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
                static_cast<unsigned long>(heap_caps_get_largest_free_block(
                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
                static_cast<unsigned long>(heap_caps_get_free_size(
                  MALLOC_CAP_SPIRAM) / 1024));
  return true;
}

void maskDecodedBlockCorners(lv_color_t *blockPixels, uint16_t blockY,
                             uint16_t blockRows, bool rotatedAsset) {
  uint16_t *pixels = reinterpret_cast<uint16_t *>(blockPixels);
  if (rotatedAsset) {
    // The portrait file is the landscape composition rotated CCW. Map only
    // its right-side 72x72 corner strips back to the original landscape
    // coordinates so the existing rounded lower corners remain identical.
    for (uint16_t localY = 0; localY < blockRows; ++localY) {
      const uint16_t portraitY = blockY + localY;
      if (portraitY >= VIDEO_CORNER_RADIUS &&
          portraitY < VIDEO_WIDTH - VIDEO_CORNER_RADIUS) continue;
      const uint16_t originalX = VIDEO_WIDTH - portraitY - 1U;
      uint16_t *row = pixels + localY * VIDEO_HEIGHT;
      for (uint16_t portraitX = VIDEO_HEIGHT - VIDEO_CORNER_RADIUS;
           portraitX < VIDEO_HEIGHT; ++portraitX) {
        const uint16_t cornerRow =
          portraitX - (VIDEO_HEIGHT - VIDEO_CORNER_RADIUS);
        const uint16_t inset = bottomCornerInsets[cornerRow];
        if (originalX < inset || originalX >= VIDEO_WIDTH - inset)
          row[portraitX] = 0;
      }
    }
    return;
  }
  for (uint16_t localY = 0; localY < blockRows; ++localY) {
    const uint16_t globalY = blockY + localY;
    if (globalY < VIDEO_HEIGHT - VIDEO_CORNER_RADIUS) continue;
    const uint16_t cornerRow = globalY - (VIDEO_HEIGHT - VIDEO_CORNER_RADIUS);
    const uint16_t inset = bottomCornerInsets[cornerRow];
    memset(pixels + localY * VIDEO_WIDTH, 0, inset * sizeof(uint16_t));
    memset(pixels + localY * VIDEO_WIDTH + VIDEO_WIDTH - inset, 0,
           inset * sizeof(uint16_t));
  }
}

void setBlockRegion(ReadyVideoFrame &frame) {
  frame.x = 0;
  frame.y = frame.blockY;
  frame.width = frame.sourceWidth;
  frame.height = frame.rows;
}

void drawBigEndianVideoBlock(const ReadyVideoFrame &frame, lv_color_t *pixels) {
  gfx->draw16bitBeRGBBitmap(
    frame.x, frame.y, reinterpret_cast<uint16_t *>(pixels),
    frame.width, frame.height);
}

bool interactiveUiNeedsCpu() {
  // Normal playback keeps JPEG on its dedicated high-priority core1 task.
  // Only lend short time slices to Arduino/LVGL while the user is touching,
  // pressing a navigation key, or a fade state machine is active. This keeps
  // idle playback at 30 fps without making 12 ms fade steps advance at the
  // decoder's previous ~33 ms main-loop cadence.
  return screenFadeTransitionActive || touchPending || touchCurrentlyPressed ||
         controlPanelShowRequested || controlPanelHideRequested ||
         weatherPageHideRequested ||
         gpio_get_level(static_cast<gpio_num_t>(IMAGE_PREVIOUS_KEY_PIN)) == 0 ||
         gpio_get_level(static_cast<gpio_num_t>(IMAGE_NEXT_KEY_PIN)) == 0;
}

void wallpaperReaderTask(void *) {
  File file;
  WallpaperHeader header{};
  uint8_t openedIndex = UINT8_MAX;
  bool openedRotated = false;
  uint32_t openedGeneration = 0;
  uint32_t localFrameIndex = 0;

  while (true) {
    readerIdle = false;
    if (!initialized || pipelinePauseRequested || !weatherPageVisible || lowPowerMode ||
        controlPanelVisible || controlPanelShowRequested) {
      if (file) file.close();
      openedIndex = UINT8_MAX;
      readerIdle = true;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    uint32_t generation = 0;
    uint8_t desiredIndex = 0;
    bool desiredRotated = false;
    uint32_t desiredFrame = 0;
    uint32_t desiredFileOffset = 0;
    portENTER_CRITICAL(&stateMux);
    generation = pipelineGeneration;
    desiredIndex = requestedReaderIndex;
    desiredRotated = requestedReaderRotated;
    desiredFrame = requestedReaderFrame;
    desiredFileOffset = requestedReaderFileOffset;
    portEXIT_CRITICAL(&stateMux);
    if (!file || openedIndex != desiredIndex || openedRotated != desiredRotated ||
        openedGeneration != generation) {
      if (!openReaderAsset(file, header, desiredIndex, desiredRotated)) {
        readerIdle = true;
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
      uint32_t targetFrame = desiredFrame;
      if (!seekReaderFrame(file, header, targetFrame, desiredFileOffset,
                           generation, desiredIndex, desiredRotated)) {
        file.close();
        openedIndex = UINT8_MAX;
        if (readerRequestStillCurrent(generation, desiredIndex,
                                      desiredRotated)) {
          reportPlaybackError(PlaybackErrorCode::Seek,
                              assetPath(desiredIndex, desiredRotated),
                              "cannot locate requested MJPEG frame");
        }
        readerIdle = true;
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
      if (!readerRequestStillCurrent(generation, desiredIndex,
                                     desiredRotated)) {
        file.close();
        openedIndex = UINT8_MAX;
        continue;
      }
      openedIndex = desiredIndex;
      openedRotated = desiredRotated;
      openedGeneration = generation;
      localFrameIndex = targetFrame;
    }

    uint8_t slot = 0;
    if (xQueueReceive(freeCompressedQueue, &slot, pdMS_TO_TICKS(20)) != pdTRUE)
      continue;
    if (pipelinePauseRequested || !weatherPageVisible || lowPowerMode ||
        controlPanelVisible || controlPanelShowRequested ||
        generation != pipelineGeneration ||
        desiredIndex != requestedReaderIndex ||
        desiredRotated != requestedReaderRotated) {
      xQueueSend(freeCompressedQueue, &slot,
                 pdMS_TO_TICKS(PIPELINE_QUEUE_WAIT_MS));
      continue;
    }
    if (localFrameIndex >= header.frameCount) {
      localFrameIndex = 0;
      if (!file.seek(header.headerSize)) {
        xQueueSend(freeCompressedQueue, &slot,
                   pdMS_TO_TICKS(PIPELINE_QUEUE_WAIT_MS));
        reportPlaybackError(PlaybackErrorCode::Seek,
                            assetPath(desiredIndex, desiredRotated),
                            "cannot seek back to the first frame");
        file.close();
        continue;
      }
    }

    const uint32_t frameFileOffset = static_cast<uint32_t>(file.position());
    const uint64_t startedAtUs = esp_timer_get_time();
    uint32_t byteCount = 0;
    const bool lengthRead =
      readExact(file, reinterpret_cast<uint8_t *>(&byteCount), sizeof(byteCount));
    const bool lengthValid = lengthRead && byteCount > 0 &&
                             byteCount <= header.maxFrameBytes &&
                             byteCount <= MAX_JPEG_BYTES;
    const bool frameRead =
      lengthValid && readExactInterruptible(file, jpegBuffers[slot], byteCount);
    const bool readOk = lengthRead && lengthValid && frameRead;
    const uint64_t readUs = esp_timer_get_time() - startedAtUs;
    if (!readOk) {
      xQueueSend(freeCompressedQueue, &slot,
                 pdMS_TO_TICKS(PIPELINE_QUEUE_WAIT_MS));
      if (!lengthRead && playerVideoMode &&
          file.position() >= file.size() && file.seek(header.headerSize)) {
        localFrameIndex = 0;
        Serial.println("Video reached file end after seek; looping from frame 1");
        continue;
      }
      file.close();
      openedIndex = UINT8_MAX;
      if (!pipelinePauseRequested && weatherPageVisible && !lowPowerMode &&
          !controlPanelVisible && !controlPanelShowRequested) {
        char detail[160]{};
        if (!lengthRead) {
          snprintf(detail, sizeof(detail), "cannot read frame %lu/%lu length",
                   static_cast<unsigned long>(localFrameIndex + 1),
                   static_cast<unsigned long>(header.frameCount));
          reportPlaybackError(PlaybackErrorCode::FrameLength,
                              assetPath(desiredIndex, desiredRotated), detail);
        } else if (!lengthValid) {
          snprintf(detail, sizeof(detail),
                   "frame %lu/%lu length %lu is outside 1..%lu bytes",
                   static_cast<unsigned long>(localFrameIndex + 1),
                   static_cast<unsigned long>(header.frameCount),
                   static_cast<unsigned long>(byteCount),
                   static_cast<unsigned long>(
                     min(header.maxFrameBytes, MAX_JPEG_BYTES)));
          reportPlaybackError(PlaybackErrorCode::FrameLength,
                              assetPath(desiredIndex, desiredRotated), detail);
        } else {
          snprintf(detail, sizeof(detail),
                   "cannot read frame %lu/%lu payload (%lu bytes)",
                   static_cast<unsigned long>(localFrameIndex + 1),
                   static_cast<unsigned long>(header.frameCount),
                   static_cast<unsigned long>(byteCount));
          reportPlaybackError(PlaybackErrorCode::FrameData,
                              assetPath(desiredIndex, desiredRotated), detail);
        }
      }
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    CompressedFrame frame{};
    frame.slot = slot;
    frame.wallpaperIndex = desiredIndex;
    frame.generation = generation;
    frame.bytes = byteCount;
    frame.index = localFrameIndex++;
    frame.frameCount = header.frameCount;
    frame.fpsTimes100 = header.fpsTimes100;
    frame.width = header.width;
    frame.height = header.height;
    frame.rotatedAsset = desiredRotated;
    frame.fileOffset = frameFileOffset;
    frame.startedAtUs = startedAtUs;
    frame.readUs = readUs;
    if (xQueueSend(readyCompressedQueue, &frame,
                   pdMS_TO_TICKS(pacedQueueWaitMs(frame.fpsTimes100))) != pdTRUE)
      xQueueSend(freeCompressedQueue, &slot,
                 pdMS_TO_TICKS(PIPELINE_QUEUE_WAIT_MS));
  }
}

void wallpaperDisplayTask(void *) {
  uint32_t scheduledGeneration = 0;
  uint64_t nextDisplayAtUs = 0;
  uint64_t frameLcdUs = 0;
  bool frameActive = false;
  uint32_t activeFrameIndex = 0;
  while (true) {
    displayIdle = false;
    if (!initialized || pipelinePauseRequested || !weatherPageVisible || lowPowerMode ||
        controlPanelVisible || controlPanelShowRequested) {
      displayIdle = true;
      frameActive = false;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    ReadyVideoFrame frame{};
    if (xQueueReceive(readyVideoQueue, &frame, pdMS_TO_TICKS(20)) != pdTRUE)
      continue;
    if (pipelinePauseRequested || !weatherPageVisible || lowPowerMode ||
        controlPanelVisible || controlPanelShowRequested ||
        frame.generation != pipelineGeneration || frame.rotation != displayRotation) {
      if (frame.last) frameActive = false;
      xQueueSend(freeVideoQueue, &frame.slot,
                 pdMS_TO_TICKS(PIPELINE_QUEUE_WAIT_MS));
      continue;
    }

    const uint64_t periodUs = max<uint64_t>(1, 100000000ULL / frame.fpsTimes100);
    if (frame.first) {
      frameActive = true;
      activeFrameIndex = frame.index;
      frameLcdUs = 0;
      if (scheduledGeneration != frame.generation) {
        scheduledGeneration = frame.generation;
        nextDisplayAtUs = esp_timer_get_time();
      }
      uint64_t nowUs = esp_timer_get_time();
      while (nowUs < nextDisplayAtUs) {
        const uint64_t remainingUs = nextDisplayAtUs - nowUs;
        if (remainingUs > 2000) vTaskDelay(pdMS_TO_TICKS(1));
        else taskYIELD();
        nowUs = esp_timer_get_time();
      }
    } else if (!frameActive || frame.index != activeFrameIndex) {
      xQueueSend(freeVideoQueue, &frame.slot,
                 pdMS_TO_TICKS(PIPELINE_QUEUE_WAIT_MS));
      continue;
    }

    lv_color_t *displayPixels = videoFrameBuffers[frame.slot];
    setBlockRegion(frame);
    uint64_t lcdUs = 0;
    if (!pipelinePauseRequested && weatherPageVisible && !lowPowerMode &&
        !controlPanelVisible && !controlPanelShowRequested &&
        frame.generation == pipelineGeneration && frame.rotation == displayRotation) {
      lockDisplayBus();
      if (!pipelinePauseRequested && weatherPageVisible && !lowPowerMode &&
        !controlPanelVisible && !controlPanelShowRequested &&
        frame.generation == pipelineGeneration && frame.rotation == displayRotation) {
        const uint64_t startedAtUs = esp_timer_get_time();
        drawBigEndianVideoBlock(frame, displayPixels);
        lcdUs = esp_timer_get_time() - startedAtUs;
      }
      unlockDisplayBus();
    }

    frameLcdUs += lcdUs;
    if (frame.last && lcdUs) {
      frameActive = false;
      displayedGeneration = frame.generation;
      displayedWallpaperIndex = frame.wallpaperIndex;
      // Until this point LVGL owns the full screen and shows the loading/error
      // page. Hand the upper 360 rows to direct video only after one complete
      // frame, never after the first partial 16-row block.
      weatherVideoFrameVisible = true;
      const uint64_t completedAtUs = esp_timer_get_time();
      VideoDisplayResult result{};
      result.wallpaperIndex = frame.wallpaperIndex;
      result.rotation = frame.rotation;
      result.generation = frame.generation;
      result.index = frame.index;
      result.frameCount = frame.frameCount;
      result.fpsTimes100 = frame.fpsTimes100;
      result.fileOffset = frame.fileOffset;
      result.sequence = ++displayedFrameSequence;
      result.displayedAtUs = completedAtUs;
      result.pipelineUs = completedAtUs > frame.startedAtUs
                            ? completedAtUs - frame.startedAtUs : 0;
      result.readUs = frame.readUs;
      result.decodeUs = frame.decodeUs;
      result.decoderSetupUs = frame.decoderSetupUs;
      result.headerUs = frame.headerUs;
      result.processUs = frame.processUs;
      result.bufferWaitUs = frame.bufferWaitUs;
      result.assembleUs = frame.assembleUs;
      result.lcdUs = frameLcdUs;
      if (xQueueSend(videoDisplayResultQueue, &result, 0) != pdTRUE)
        ++playbackStatsResultDrops;
      // updateScreenOrientation() blanked the panel without blocking the main
      // loop. Reveal it only after a complete valid frame in the requested
      // orientation has reached the LCD.
      if (orientationTransitionBlanked) {
        orientationVideoReady = true;
        completeWeatherOrientationIfReady();
      }
      nextDisplayAtUs += periodUs;
      if (completedAtUs > nextDisplayAtUs + periodUs) nextDisplayAtUs = completedAtUs;
    }
    xQueueSend(freeVideoQueue, &frame.slot,
               pdMS_TO_TICKS(PIPELINE_QUEUE_WAIT_MS));
  }
}

bool decodeReadyFrame() {
  if (!initialized) return false;
  CompressedFrame compressed{};
  if (xQueueReceive(readyCompressedQueue, &compressed, 0) != pdTRUE) return false;
  if (pipelinePauseRequested || !weatherPageVisible || lowPowerMode ||
      controlPanelVisible || controlPanelShowRequested ||
      compressed.generation != pipelineGeneration) {
    xQueueSend(freeCompressedQueue, &compressed.slot,
               pdMS_TO_TICKS(PIPELINE_QUEUE_WAIT_MS));
    return true;
  }

  const lv_disp_rot_t rotation = displayRotation;
  uint64_t decoderSetupUs = 0;
  uint64_t headerUs = 0;
  uint64_t processUs = 0;
  uint64_t bufferWaitUs = 0;
  uint64_t maskUs = 0;
  uint64_t operationStartedAtUs = esp_timer_get_time();
  const uint32_t pacedWaitMs = pacedQueueWaitMs(compressed.fpsTimes100);
  const char *failureStage = "decoder-open";
  bool ok = ensureBlockDecoder();
  decoderSetupUs = esp_timer_get_time() - operationStartedAtUs;
  memset(&jpegIo, 0, sizeof(jpegIo));
  memset(&jpegInfo, 0, sizeof(jpegInfo));
  jpegIo.inbuf = jpegBuffers[compressed.slot];
  jpegIo.inbuf_len = compressed.bytes;
  if (ok) {
    failureStage = "jpeg-header";
    operationStartedAtUs = esp_timer_get_time();
    const jpeg_error_t result = jpeg_dec_parse_header(jpegDecoder, &jpegIo, &jpegInfo);
    headerUs = esp_timer_get_time() - operationStartedAtUs;
    ok = result == JPEG_ERR_OK && jpegInfo.width == compressed.width &&
         jpegInfo.height == compressed.height;
  }
  int blockBytes = 0;
  int processCount = 0;
  if (ok) {
    failureStage = "decoder-layout";
    ok = jpeg_dec_get_outbuf_len(jpegDecoder, &blockBytes) == JPEG_ERR_OK &&
         jpeg_dec_get_process_count(jpegDecoder, &processCount) == JPEG_ERR_OK &&
         blockBytes > 0 &&
         blockBytes <= static_cast<int>(DISPLAY_DMA_BLOCK_BYTES) &&
         processCount > 0;
  }

  uint32_t offset = 0;
  const uint32_t rowBytes = compressed.width * sizeof(uint16_t);
  bool queuedLastBlock = false;
  for (int block = 0; ok && block < processCount; ++block) {
    if (pipelinePauseRequested || !weatherPageVisible || lowPowerMode ||
        controlPanelVisible || controlPanelShowRequested ||
        compressed.generation != pipelineGeneration || rotation != displayRotation) {
      failureStage = "interrupted";
      ok = false;
      break;
    }
    uint8_t videoSlot = 0;
    operationStartedAtUs = esp_timer_get_time();
    if (xQueueReceive(freeVideoQueue, &videoSlot,
                      pdMS_TO_TICKS(pacedWaitMs)) != pdTRUE) {
      failureStage = "video-buffer-timeout";
      ok = false;
      break;
    }
    bufferWaitUs += esp_timer_get_time() - operationStartedAtUs;
    jpegIo.outbuf = reinterpret_cast<uint8_t *>(videoFrameBuffers[videoSlot]);
    operationStartedAtUs = esp_timer_get_time();
    const jpeg_error_t processResult = jpeg_dec_process(jpegDecoder, &jpegIo);
    processUs += esp_timer_get_time() - operationStartedAtUs;
    failureStage = "jpeg-process";
    ok = processResult == JPEG_ERR_OK && jpegIo.out_size > 0 &&
         jpegIo.out_size <= blockBytes &&
         (jpegIo.out_size % rowBytes) == 0 &&
         offset + static_cast<uint32_t>(jpegIo.out_size) <= FRAME_BYTES;
    if (!ok) {
      xQueueSend(freeVideoQueue, &videoSlot,
                 pdMS_TO_TICKS(PIPELINE_QUEUE_WAIT_MS));
      break;
    }
    const uint16_t blockY = offset / rowBytes;
    const uint16_t blockRows = jpegIo.out_size / rowBytes;
    offset += static_cast<uint32_t>(jpegIo.out_size);
    const bool lastBlock = block == processCount - 1;
    if (lastBlock && offset != FRAME_BYTES) {
      failureStage = "frame-size";
      xQueueSend(freeVideoQueue, &videoSlot,
                 pdMS_TO_TICKS(PIPELINE_QUEUE_WAIT_MS));
      ok = false;
      break;
    }
    operationStartedAtUs = esp_timer_get_time();
    maskDecodedBlockCorners(videoFrameBuffers[videoSlot], blockY, blockRows,
                            compressed.rotatedAsset);
    maskUs += esp_timer_get_time() - operationStartedAtUs;

    ReadyVideoFrame ready{};
    ready.slot = videoSlot;
    ready.wallpaperIndex = compressed.wallpaperIndex;
    ready.rotation = rotation;
    ready.first = block == 0;
    ready.last = lastBlock;
    ready.generation = compressed.generation;
    ready.index = compressed.index;
    ready.frameCount = compressed.frameCount;
    ready.fpsTimes100 = compressed.fpsTimes100;
    ready.fileOffset = compressed.fileOffset;
    ready.blockY = blockY;
    ready.rows = blockRows;
    ready.sourceWidth = compressed.width;
    if (lastBlock) {
      ready.startedAtUs = compressed.startedAtUs;
      ready.readUs = compressed.readUs;
      ready.decoderSetupUs = decoderSetupUs;
      ready.headerUs = headerUs;
      ready.processUs = processUs;
      ready.bufferWaitUs = bufferWaitUs;
      ready.decodeUs = decoderSetupUs + headerUs + processUs + bufferWaitUs;
      ready.assembleUs = maskUs;
    }
    if (xQueueSend(readyVideoQueue, &ready,
                   pdMS_TO_TICKS(pacedWaitMs)) != pdTRUE) {
      failureStage = "display-queue-timeout";
      xQueueSend(freeVideoQueue, &videoSlot,
                 pdMS_TO_TICKS(PIPELINE_QUEUE_WAIT_MS));
      ok = false;
      break;
    }
    queuedLastBlock = lastBlock;
    failureStage = "incomplete-frame";
    // A 16-row block is a natural preemption boundary. Yield once per four
    // blocks only during interaction/transition; yielding every block would
    // make a touched video visibly lose frame rate, while never yielding made
    // controls and brightness fades wait for a complete ~31 ms JPEG.
    if ((block & 3) == 3 && interactiveUiNeedsCpu()) vTaskDelay(1);
  }
  ok = ok && queuedLastBlock && offset == FRAME_BYTES &&
       compressed.generation == pipelineGeneration && rotation == displayRotation &&
       !pipelinePauseRequested && weatherPageVisible && !lowPowerMode &&
       !controlPanelVisible && !controlPanelShowRequested;

  if (ok) {
    wallpaperHeader.fpsTimes100 = compressed.fpsTimes100;
    wallpaperHeader.frameCount = compressed.frameCount;
    frameIndex = compressed.index;
  } else {
    if (!pipelinePauseRequested &&
        compressed.generation == pipelineGeneration && rotation == displayRotation &&
        weatherPageVisible && !lowPowerMode && !controlPanelVisible &&
        !controlPanelShowRequested) {
      Serial.printf(
        "Weather wallpaper decode failed: %s frame %lu/%lu, stage=%s, "
        "offset=%lu/%lu, blocks=%d, queue wait=%lu ms\n",
                    assetPath(compressed.wallpaperIndex,
                              compressed.rotatedAsset).c_str(),
                    static_cast<unsigned long>(compressed.index),
                    static_cast<unsigned long>(compressed.frameCount),
                    failureStage,
                    static_cast<unsigned long>(offset),
                    static_cast<unsigned long>(FRAME_BYTES),
                    processCount,
                    static_cast<unsigned long>(pacedWaitMs));
      playbackErrorCode = PlaybackErrorCode::JpegDecode;
      playbackErrorPending = true;
    }
    // A generation/orientation switch can interrupt block-mode decoding in
    // the middle of a JPEG. The decoder handle is then no longer at a frame
    // boundary; reusing it made the first frame of the new JWP fail even when
    // that JPEG was valid. Recreate it after every incomplete/error path.
    if (jpegDecoder) {
      jpeg_dec_close(jpegDecoder);
      jpegDecoder = nullptr;
    }
  }
  xQueueSend(freeCompressedQueue, &compressed.slot,
             pdMS_TO_TICKS(PIPELINE_QUEUE_WAIT_MS));
  return true;
}

void wallpaperDecodeTask(void *) {
  while (true) {
    bool decodeAllowed = false;
    // Pair this acquisition with pausePipeline()'s stateMux update. If a
    // pause wins first, the worker cannot enter JPEG code; if decoding wins,
    // pausePipeline() observes decoderIdle=false and waits for this frame.
    portENTER_CRITICAL(&stateMux);
    if (initialized && !pipelinePauseRequested && !networkResourceHold &&
        weatherPageVisible && !lowPowerMode && !controlPanelVisible &&
        !controlPanelShowRequested) {
      decoderIdle = false;
      decodeAllowed = true;
    } else {
      decoderIdle = true;
    }
    portEXIT_CRITICAL(&stateMux);

    if (!decodeAllowed) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    // Priority 2 on core1 is above Arduino's loop task. Always block for one
    // scheduler tick after a frame, rather than relying only on queue back-
    // pressure: at 80 MHz the LCD can recycle both block buffers so quickly
    // that JPEG otherwise starves orientation, buttons and LVGL for seconds.
    // One tick is enough for the core1 Arduino/LVGL task to service input, and
    // unlike the previous two-tick delay still leaves a 29-32 ms decode inside
    // the 33.33 ms frame budget used by the standalone 09 implementation.
    decodeReadyFrame();
    vTaskDelay(1);
  }
}

void requestScene(WeatherScene scene) {
  if (!initialized) return;
  const int16_t found = randomAsset(scene, activeIndex);
  if (found >= 0 && found != activeIndex) requestedIndex = found;
}

void applyWallpaperFollowSetting() {
  nextRandomizeAtMs = 0;
  if (!initialized || playerVideoMode) return;
  if (!weatherWallpaperFollowEnabled) {
    persistLastWeatherSelection(activeIndex);
    return;
  }
  uint16_t code = 999;
  int8_t overrideScene = -1;
  portENTER_CRITICAL(&stateMux);
  code = weatherCode;
  overrideScene = serialOverrideScene;
  portEXIT_CRITICAL(&stateMux);
  requestScene(overrideScene >= 0
                 ? static_cast<WeatherScene>(overrideScene)
                 : sceneForCode(code));
}

void requestStep(int8_t direction) {
  if (!initialized || !direction || requestedIndex >= 0 ||
      assetFadeState != AssetFadeState::Idle) return;
  const int16_t found = !playerVideoMode && !weatherWallpaperFollowEnabled
                          ? findAnyWeatherAsset(activeIndex, direction)
                          : findAsset(weatherWallpapers[activeIndex].scene,
                                      activeIndex, direction);
  if (found >= 0 && found != activeIndex) {
    requestedIndex = found;
    nextRandomizeAtMs = weatherAutoSwitchEnabled
      ? millis() + static_cast<uint32_t>(weatherSwitchMinutes) * 60000UL : 0;
  }
}

void requestVideoSeek(uint32_t targetFrame) {
  if (!initialized || !playerVideoMode || !currentVideoFrameCount ||
      activeIndex >= weatherWallpaperCount) return;
  if (targetFrame >= currentVideoFrameCount)
    targetFrame = currentVideoFrameCount - 1U;
  cancelAssetFade(true);
  requestedIndex = -1;
  holdVideoAudioForSeek();
  resetVideoAudioSync(false);
  setReaderRequest(activeIndex, rotatedAssetRequired(), targetFrame, 0, true);
  resetPlaybackStats();
  videoResumeSaveRequested = true;
  Serial.printf("Video seek requested: frame %lu/%lu (%.1f%%)\n",
                static_cast<unsigned long>(targetFrame + 1U),
                static_cast<unsigned long>(currentVideoFrameCount),
                currentVideoFrameCount > 1
                  ? targetFrame * 100.0f / (currentVideoFrameCount - 1U) : 0.0f);
}

bool seekGestureSuppressed() {
  return playerVideoMode &&
         (videoSeekDragging ||
          millis() - lastVideoSeekInteractionAtMs < 400U);
}

void resetPlaybackStats() {
  playbackStatsStartedAtMs = 0;
  playbackStatsFrames = 0;
  playbackStatsIntervals = 0;
  playbackStatsGeneration = 0;
  playbackStatsRotation = static_cast<lv_disp_rot_t>(0xFF);
  playbackStatsLastDisplayedAtUs = 0;
  playbackStatsIntervalUs = 0;
  playbackStatsLastSequence = 0;
  playbackStatsReadUs = 0;
  playbackStatsDecodeUs = 0;
  playbackStatsDecoderSetupUs = 0;
  playbackStatsHeaderUs = 0;
  playbackStatsProcessUs = 0;
  playbackStatsBufferWaitUs = 0;
  playbackStatsAssembleUs = 0;
  playbackStatsLcdUs = 0;
  playbackStatsPipelineUs = 0;
  playbackStatsPipelineMinUs = UINT64_MAX;
  playbackStatsPipelineMaxUs = 0;
  playbackStatsFrameMinUs = UINT64_MAX;
  playbackStatsFrameMaxUs = 0;
  playbackStatsResultDropsBaseline = playbackStatsResultDrops;
}

void orientationChanged() {
  if (!initialized) return;
  const bool rotatedAsset = rotatedAssetRequired();
  if (rotatedAsset == requestedReaderRotated) return;

  int16_t target = activeIndex;
  if (!assetAvailable(static_cast<uint8_t>(target), rotatedAsset)) {
    target = randomAsset(weatherWallpapers[activeIndex].scene);
  }
  if (target < 0) {
    Serial.printf("Weather wallpaper: no %s asset for %s scene\n",
                  rotatedAsset ? "rotated" : "landscape",
                  sceneName(weatherWallpapers[activeIndex].scene));
    playbackErrorPending = true;
    return;
  }

  if (playerVideoMode &&
      currentVideoPositionIndex == static_cast<uint8_t>(target)) {
    setReaderRequest(static_cast<uint8_t>(target), rotatedAsset,
                     currentVideoPositionFrame, 0, true);
  } else {
    setReaderAsset(static_cast<uint8_t>(target), rotatedAsset,
                   playerVideoMode, true);
  }
  resetPlaybackStats();
}

void setAssetFadeBrightness(uint8_t level) {
  lockDisplayBus();
  gfx->setBrightness(level);
  unlockDisplayBus();
}

void cancelAssetFade(bool restoreBrightness) {
  if (assetFadeState == AssetFadeState::Idle) return;
  assetFadeState = AssetFadeState::Idle;
  assetFadeTargetIndex = -1;
  assetFadeSourceIndex = -1;
  assetFadeStep = 0;
  assetFadeStepDueAt = 0;
  assetFadeWaitStartedAt = 0;
  assetFadeFallbackAttempted = false;
  if (restoreBrightness) {
    setAssetFadeBrightness(activeBrightness);
    if (!orientationTransitionBlanked) screenFadeTransitionActive = false;
  }
}

void beginRequestedAssetFade() {
  if (requestedIndex < 0 || requestedIndex == activeIndex ||
      assetFadeState != AssetFadeState::Idle || screenFadeTransitionActive ||
      orientationTransitionBlanked) {
    if (requestedIndex == activeIndex) requestedIndex = -1;
    return;
  }
  if (requestedIndex >= weatherWallpaperCount ||
      !assetAvailable(static_cast<uint8_t>(requestedIndex),
                      rotatedAssetRequired())) {
    requestedIndex = -1;
    return;
  }
  if (playerVideoMode) persistVideoPosition(true);
  assetFadeTargetIndex = requestedIndex;
  assetFadeSourceIndex = activeIndex;
  requestedIndex = -1;
  assetFadeState = AssetFadeState::FadeOut;
  assetFadeStep = 0;
  assetFadeStepDueAt = millis();
  screenFadeTransitionActive = true;
}

void serviceAssetFade() {
  if (assetFadeState == AssetFadeState::Idle) return;
  if (!weatherPageVisible || lowPowerMode) {
    cancelAssetFade(false);
    return;
  }

  const uint32_t now = millis();
  if (assetFadeState == AssetFadeState::FadeOut) {
    if (static_cast<int32_t>(now - assetFadeStepDueAt) < 0) return;
    if (assetFadeStep < ASSET_FADE_STEPS) ++assetFadeStep;
    const uint8_t level = static_cast<uint8_t>(
      (static_cast<uint16_t>(activeBrightness) *
       (ASSET_FADE_STEPS - assetFadeStep)) / ASSET_FADE_STEPS);
    setAssetFadeBrightness(level);
    assetFadeStepDueAt = now + ASSET_FADE_STEP_MS;
    if (assetFadeStep < ASSET_FADE_STEPS) return;

    const uint8_t target = static_cast<uint8_t>(assetFadeTargetIndex);
    if (playerVideoMode) {
      holdVideoAudioForSeek();
      resetVideoAudioSync(false);
    }
    setReaderAsset(target, rotatedAssetRequired(), playerVideoMode, false);
    Serial.printf("Weather wallpaper fade switch: %s\n",
                  assetPath(target, requestedReaderRotated).c_str());
    assetFadeState = AssetFadeState::WaitForFrame;
    assetFadeWaitStartedAt = now;
    return;
  }

  if (assetFadeState == AssetFadeState::WaitForFrame) {
    // Keep the panel black while already-prefetched old frames drain. No queue
    // or JPEG generation is invalidated; fade-in starts only after a complete
    // frame from the requested file is physically present on the display.
    if (displayedWallpaperIndex != static_cast<uint8_t>(assetFadeTargetIndex) ||
        activeIndex != static_cast<uint8_t>(assetFadeTargetIndex)) {
      if (now - assetFadeWaitStartedAt < ASSET_FADE_FRAME_TIMEOUT_MS) return;
      if (!assetFadeFallbackAttempted && assetFadeSourceIndex >= 0 &&
          assetFadeSourceIndex != assetFadeTargetIndex) {
        assetFadeFallbackAttempted = true;
        assetFadeTargetIndex = assetFadeSourceIndex;
        setReaderAsset(static_cast<uint8_t>(assetFadeSourceIndex),
                       rotatedAssetRequired(), playerVideoMode, false);
        assetFadeWaitStartedAt = now;
        Serial.println("Weather wallpaper fade: target timeout, restoring previous asset");
        if (displayedWallpaperIndex !=
            static_cast<uint8_t>(assetFadeSourceIndex)) return;
      } else {
        // Never leave the user on a permanently black screen even when both
        // the requested file and its fallback fail to produce a complete frame.
        Serial.println("Weather wallpaper fade: frame timeout, revealing display");
      }
    }
    assetFadeState = AssetFadeState::FadeIn;
    assetFadeStep = 0;
    assetFadeStepDueAt = now;
  }

  if (assetFadeState == AssetFadeState::FadeIn) {
    if (static_cast<int32_t>(now - assetFadeStepDueAt) < 0) return;
    if (assetFadeStep < ASSET_FADE_STEPS) ++assetFadeStep;
    const uint8_t level = static_cast<uint8_t>(
      (static_cast<uint16_t>(activeBrightness) * assetFadeStep +
       ASSET_FADE_STEPS / 2U) / ASSET_FADE_STEPS);
    setAssetFadeBrightness(level);
    assetFadeStepDueAt = now + ASSET_FADE_STEP_MS;
    if (assetFadeStep < ASSET_FADE_STEPS) return;
    assetFadeState = AssetFadeState::Idle;
    assetFadeTargetIndex = -1;
    assetFadeSourceIndex = -1;
    assetFadeStepDueAt = 0;
    assetFadeWaitStartedAt = 0;
    assetFadeFallbackAttempted = false;
    screenFadeTransitionActive = false;
  }
}

void printPlaybackStats() {
  VideoDisplayResult result{};
  while (xQueueReceive(videoDisplayResultQueue, &result, 0) == pdTRUE) {
    if (result.generation != pipelineGeneration ||
        result.rotation != displayRotation) continue;
    if (playbackStatsGeneration != result.generation ||
        playbackStatsRotation != result.rotation) {
      resetPlaybackStats();
      playbackStatsGeneration = result.generation;
      playbackStatsRotation = result.rotation;
      playbackStatsStartedAtMs = millis();
      playbackStatsLastDisplayedAtUs = result.displayedAtUs;
      playbackStatsLastSequence = result.sequence;
    } else if (playbackStatsLastDisplayedAtUs &&
               result.displayedAtUs > playbackStatsLastDisplayedAtUs) {
      const uint64_t intervalUs =
        result.displayedAtUs - playbackStatsLastDisplayedAtUs;
      const uint32_t intervalCount =
        result.sequence - playbackStatsLastSequence;
      playbackStatsIntervalUs += intervalUs;
      playbackStatsIntervals += intervalCount;
      // Min/max is exact only for adjacent observed frames. If the result
      // queue dropped a sample, the sequence delta still keeps average FPS
      // exact, but the individual intervals inside that span are unknowable.
      if (intervalCount == 1) {
        playbackStatsFrameMinUs = min(playbackStatsFrameMinUs, intervalUs);
        playbackStatsFrameMaxUs = max(playbackStatsFrameMaxUs, intervalUs);
      }
      playbackStatsLastDisplayedAtUs = result.displayedAtUs;
      playbackStatsLastSequence = result.sequence;
    }
    ++playbackStatsFrames;
    playbackStatsReadUs += result.readUs;
    playbackStatsDecodeUs += result.decodeUs;
    playbackStatsDecoderSetupUs += result.decoderSetupUs;
    playbackStatsHeaderUs += result.headerUs;
    playbackStatsProcessUs += result.processUs;
    playbackStatsBufferWaitUs += result.bufferWaitUs;
    playbackStatsAssembleUs += result.assembleUs;
    playbackStatsLcdUs += result.lcdUs;
    playbackStatsPipelineUs += result.pipelineUs;
    if (playerVideoMode && videoProgressBar && result.frameCount) {
      // Trigger from the first frame that actually reached the LCD. This
      // avoids the fade state hiding or shortening the filename notification.
      if (displayedVideoNameIndex != result.wallpaperIndex) {
        displayedVideoNameIndex = result.wallpaperIndex;
        showVideoName(result.wallpaperIndex);
        persistLastVideoSelection(result.wallpaperIndex);
      }
      currentVideoPositionIndex = result.wallpaperIndex;
      currentVideoPositionFrame = result.index;
      currentVideoPositionFileOffset = result.fileOffset;
      currentVideoFrameCount = result.frameCount;
      currentVideoFpsTimes100 = result.fpsTimes100;
      if (!videoSeekDragging) {
        const int32_t progress = result.frameCount > 1U
          ? static_cast<int32_t>(
              (static_cast<uint64_t>(result.index) * 1000U) /
              (result.frameCount - 1U))
          : 0;
        lv_slider_set_value(videoProgressBar,
                            progress > 1000 ? 1000 : progress, LV_ANIM_OFF);
        updateVideoTimeLabel(result.index, result.frameCount,
                             result.fpsTimes100);
      }
      if (videoResumeSaveRequested) {
        persistVideoPosition(true);
        videoResumeSaveRequested = false;
      }
      syncVideoAudio(result);
    } else if (!playerVideoMode && !weatherWallpaperFollowEnabled &&
               lastPersistedWeatherIndex != result.wallpaperIndex) {
      lastPersistedWeatherIndex = result.wallpaperIndex;
      persistLastWeatherSelection(result.wallpaperIndex);
    }
    if (result.pipelineUs) {
      playbackStatsPipelineMinUs =
        min(playbackStatsPipelineMinUs, result.pipelineUs);
      playbackStatsPipelineMaxUs =
        max(playbackStatsPipelineMaxUs, result.pipelineUs);
    }
    setMessage("");
  }
  const uint32_t nowMs = millis();
  if (playerVideoMode) persistVideoPosition(false);
  if (!playbackStatsStartedAtMs) return;
  const uint32_t elapsedMs = nowMs - playbackStatsStartedAtMs;
  if (elapsedMs < 5000 || !playbackStatsFrames || !playbackStatsIntervals ||
      !playbackStatsIntervalUs) return;
  const double fps = playbackStatsIntervals * 1000000.0 /
                     playbackStatsIntervalUs;
  const double frameMs = playbackStatsIntervalUs / 1000.0 /
                         playbackStatsIntervals;
  const double frameMinMs = playbackStatsFrameMinUs == UINT64_MAX
                              ? 0.0 : playbackStatsFrameMinUs / 1000.0;
  const double frameMaxMs = playbackStatsFrameMaxUs / 1000.0;
  const double readMs = playbackStatsReadUs / 1000.0 / playbackStatsFrames;
  const double decodeMs = playbackStatsDecodeUs / 1000.0 / playbackStatsFrames;
  const double decoderSetupMs = playbackStatsDecoderSetupUs / 1000.0 /
                                playbackStatsFrames;
  const double headerMs = playbackStatsHeaderUs / 1000.0 / playbackStatsFrames;
  const double processMs = playbackStatsProcessUs / 1000.0 / playbackStatsFrames;
  const double bufferWaitMs = playbackStatsBufferWaitUs / 1000.0 /
                              playbackStatsFrames;
  const double assembleMs = playbackStatsAssembleUs / 1000.0 /
                            playbackStatsFrames;
  const double lcdMs = playbackStatsLcdUs / 1000.0 / playbackStatsFrames;
  const double pipelineMs = playbackStatsPipelineUs / 1000.0 /
                            playbackStatsFrames;
  const double pipelineMinMs = playbackStatsPipelineMinUs == UINT64_MAX
                                 ? 0.0 : playbackStatsPipelineMinUs / 1000.0;
  const double pipelineMaxMs = playbackStatsPipelineMaxUs / 1000.0;
  const uint32_t droppedResults =
    playbackStatsResultDrops - playbackStatsResultDropsBaseline;
  Serial.printf(
    "Weather playback: actual %.3f fps, frame %.3f ms "
    "(adjacent min/max %.3f/%.3f), target %.2f; "
    "pipeline %.2f ms (min/max %.2f/%.2f)\n",
    fps, frameMs, frameMinMs, frameMaxMs,
    wallpaperHeader.fpsTimes100 / 100.0,
    pipelineMs, pipelineMinMs, pipelineMaxMs);
  Serial.printf(
    "Weather timing: SD %.3f ms; JPEG %.3f ms "
    "(setup %.3f + header %.3f + core %.3f + wait %.3f); "
    "mask %.3f; LCD %.3f; samples %lu, dropped %lu, rotation %u deg\n",
    readMs, decodeMs, decoderSetupMs, headerMs, processMs, bufferWaitMs,
    assembleMs, lcdMs,
    static_cast<unsigned long>(playbackStatsFrames),
    static_cast<unsigned long>(droppedResults),
    static_cast<unsigned>(playbackStatsRotation) * 90U);
  if (playerVideoMode) {
    printVideoAudioDiagnostics(
      videoAudioLastVideoPositionMs, videoAudioLastAudioPositionMs,
      videoAudioLastMeasuredDriftMs, videoAudioDriftStrikes);
  }
  playbackStatsStartedAtMs = nowMs;
  playbackStatsFrames = 0;
  playbackStatsIntervals = 0;
  playbackStatsIntervalUs = 0;
  playbackStatsReadUs = 0;
  playbackStatsDecodeUs = 0;
  playbackStatsDecoderSetupUs = 0;
  playbackStatsHeaderUs = 0;
  playbackStatsProcessUs = 0;
  playbackStatsBufferWaitUs = 0;
  playbackStatsAssembleUs = 0;
  playbackStatsLcdUs = 0;
  playbackStatsPipelineUs = 0;
  playbackStatsPipelineMinUs = UINT64_MAX;
  playbackStatsPipelineMaxUs = 0;
  playbackStatsFrameMinUs = UINT64_MAX;
  playbackStatsFrameMaxUs = 0;
  playbackStatsResultDropsBaseline = playbackStatsResultDrops;
}

bool jsonField(const String &json, const char *field,
               char *destination, size_t capacity) {
  String marker = String('"') + field + "\":\"";
  const int at = json.indexOf(marker);
  if (at < 0) return false;
  const int start = at + marker.length();
  const int end = json.indexOf('"', start);
  if (end < start || static_cast<size_t>(end - start) >= capacity) return false;
  memcpy(destination, json.c_str() + start, end - start);
  destination[end - start] = '\0';
  return true;
}

bool jsonScalarField(const String &json, const char *field,
                     char *destination, size_t capacity,
                     int searchFrom = 0, int *fieldAt = nullptr) {
  if (!destination || capacity < 2) return false;
  const String marker = String('"') + field + '"';
  const int at = json.indexOf(marker, searchFrom);
  if (at < 0) return false;
  int valueStart = json.indexOf(':', at + marker.length());
  if (valueStart < 0) return false;
  ++valueStart;
  while (valueStart < static_cast<int>(json.length()) &&
         isspace(static_cast<unsigned char>(json[valueStart]))) ++valueStart;
  const bool quoted = valueStart < static_cast<int>(json.length()) &&
                      json[valueStart] == '"';
  if (quoted) ++valueStart;
  int valueEnd = valueStart;
  while (valueEnd < static_cast<int>(json.length())) {
    const char c = json[valueEnd];
    if ((quoted && c == '"') || (!quoted && (c == ',' || c == '}' || isspace(
          static_cast<unsigned char>(c))))) break;
    ++valueEnd;
  }
  if (valueEnd <= valueStart ||
      static_cast<size_t>(valueEnd - valueStart) >= capacity) return false;
  memcpy(destination, json.c_str() + valueStart, valueEnd - valueStart);
  destination[valueEnd - valueStart] = '\0';
  if (fieldAt) *fieldAt = at;
  return true;
}

bool inflateWeatherPayload(const String &wire, String &json) {
  const uint8_t *input = reinterpret_cast<const uint8_t *>(wire.c_str());
  const size_t length = wire.length();
  if (length < 2 || input[0] != 0x1F || input[1] != 0x8B) {
    json = wire;
    return true;
  }
  if (length < 18 || input[2] != 8) return false;
  size_t offset = 10;
  const uint8_t flags = input[3];
  if (flags & 0x04) {
    if (offset + 2 > length) return false;
    const size_t extra = input[offset] | (input[offset + 1] << 8);
    offset += 2 + extra;
  }
  const auto skipString = [&]() {
    while (offset < length && input[offset]) ++offset;
    return offset++ < length;
  };
  if ((flags & 0x08) && !skipString()) return false;
  if ((flags & 0x10) && !skipString()) return false;
  if (flags & 0x02) offset += 2;
  if (offset + 8 > length) return false;
  char *decoded = static_cast<char *>(heap_caps_malloc(
    MAX_WEATHER_JSON_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!decoded)
    decoded = static_cast<char *>(heap_caps_malloc(
      MAX_WEATHER_JSON_BYTES, MALLOC_CAP_8BIT));
  if (!decoded) return false;
  const size_t decodedBytes = tinfl_decompress_mem_to_mem(
    decoded, MAX_WEATHER_JSON_BYTES - 1, input + offset, length - offset - 8,
    TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  if (decodedBytes == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
    free(decoded);
    return false;
  }
  decoded[decodedBytes] = '\0';
  json = decoded;
  free(decoded);
  return true;
}

void *reserveTlsHandshakeWindow(const char *label) {
  void *guard = heap_caps_aligned_alloc(
    16, TLS_HANDSHAKE_WINDOW_BYTES,
    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!guard) {
    tlsMemoryPressureObserved = true;
    Serial.printf(
      "%s: HTTPS deferred; TLS window needs %lu KiB, SRAM free/largest "
      "%lu/%lu KiB\n",
      label, static_cast<unsigned long>(TLS_HANDSHAKE_WINDOW_BYTES / 1024U),
      static_cast<unsigned long>(heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U),
      static_cast<unsigned long>(heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024U));
  }
  return guard;
}

bool fetchQWeatherJson(const String &resource, const char *label, String &json) {
  if (playerVideoMode && weatherPageVisible) return false;
  if (!qweatherSessionClient || !qweatherSessionHttp) {
    Serial.printf("%s: shared HTTPS session unavailable\n", label);
    return false;
  }
  String host = weatherApiHost;
  const String key = weatherApiKey;
  host.trim();
  host.replace("https://", "");
  host.replace("http://", "");
  const int slash = host.indexOf('/');
  if (slash >= 0) host.remove(slash);
  if (host.isEmpty() || key.isEmpty() || resource.isEmpty()) return false;
  const String url = String("https://") + host + resource;

  IPAddress resolved;
  if (!WiFi.hostByName(host.c_str(), resolved)) {
    Serial.printf("%s: DNS failed for %s, RSSI %d dBm\n", label,
                  host.c_str(), WiFi.RSSI());
    return false;
  }
  Serial.printf("%s: connecting %s (%s), RSSI %d dBm, "
                "heap free/largest %lu/%lu KiB\n",
                label, host.c_str(), resolved.toString().c_str(), WiFi.RSSI(),
                static_cast<unsigned long>(heap_caps_get_free_size(
                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
                static_cast<unsigned long>(heap_caps_get_largest_free_block(
                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));

  String wire;
  int status = HTTPC_ERROR_CONNECTION_REFUSED;
  bool tlsAllocationFailed = false;
  for (uint8_t attempt = 1; attempt <= 2; ++attempt) {
    if (playerVideoMode && weatherPageVisible) {
      Serial.printf("%s: cancelled for video playback\n", label);
      return false;
    }
    // The first request creates TLS; later QWeather endpoints reuse that same
    // socket. If the server closed keep-alive, reserve a fresh contiguous
    // window before GET reconnects.
    void *tlsGuard = nullptr;
    if (!qweatherSessionClient->connected()) {
      tlsGuard = reserveTlsHandshakeWindow(label);
      if (!tlsGuard) {
        tlsAllocationFailed = true;
        break;
      }
    }
    if (!qweatherSessionBegun) {
      if (!qweatherSessionHttp->begin(*qweatherSessionClient, url)) {
        if (tlsGuard) heap_caps_free(tlsGuard);
        Serial.printf("%s: HTTPS begin failed\n", label);
        return false;
      }
      qweatherSessionHttp->setReuse(true);
      qweatherSessionBegun = true;
    } else if (!qweatherSessionHttp->setURL(resource)) {
      if (tlsGuard) heap_caps_free(tlsGuard);
      Serial.printf("%s: HTTPS path update failed\n", label);
      return false;
    }
    qweatherSessionHttp->setConnectTimeout(12000);
    qweatherSessionHttp->setTimeout(12000);
    const char *headerKeys[] = {"Content-Encoding", "Content-Type"};
    qweatherSessionHttp->collectHeaders(headerKeys, 2);
    qweatherSessionHttp->addHeader("X-QW-Api-Key", key);
    qweatherSessionHttp->addHeader("Accept-Encoding", "gzip");
    if (tlsGuard) heap_caps_free(tlsGuard);
    status = qweatherSessionHttp->GET();
    if (status == HTTP_CODE_OK) wire = qweatherSessionHttp->getString();
    if (status <= 0) {
      char tlsError[128]{};
      const int tlsCode = qweatherSessionClient->lastError(
        tlsError, sizeof(tlsError));
      tlsAllocationFailed = tlsCode == MBEDTLS_ERR_SSL_ALLOC_FAILED;
      if (tlsAllocationFailed) tlsMemoryPressureObserved = true;
      Serial.printf("%s: attempt %u failed: HTTP %d (%s), "
                    "TLS %d (%s)\n", label, attempt, status,
                    HTTPClient::errorToString(status).c_str(), tlsCode,
                    tlsError[0] ? tlsError : "no TLS detail");
    }
    if (status == HTTP_CODE_OK || status > 0) break;
    if (tlsAllocationFailed) {
      Serial.printf(
        "%s: TLS allocation failed; skipping immediate retry, "
        "SRAM free/largest %lu/%lu KiB\n",
        label,
        static_cast<unsigned long>(heap_caps_get_free_size(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
        static_cast<unsigned long>(heap_caps_get_largest_free_block(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(350));
  }
  if (status != HTTP_CODE_OK) {
    Serial.printf("%s: HTTP %d (%s)\n", label, status,
                  status < 0 ? HTTPClient::errorToString(status).c_str()
                             : "server response");
    return false;
  }
  if (!inflateWeatherPayload(wire, json)) {
    Serial.printf("%s: gzip/JSON decode failed\n", label);
    return false;
  }
  return true;
}

bool fetchPublicJson(const char *host, const char *resource,
                     const char *label, String &json) {
  if (!host || !resource || (playerVideoMode && weatherPageVisible)) return false;
  IPAddress resolved;
  if (!WiFi.hostByName(host, resolved)) {
    Serial.printf("%s: DNS failed for %s\n", label, host);
    return false;
  }
  const String url = String("https://") + host + resource;
  void *tlsGuard = reserveTlsHandshakeWindow(label);
  if (!tlsGuard) return false;
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(15);
  client.setTimeout(12000);
  HTTPClient http;
  http.setConnectTimeout(12000);
  http.setTimeout(12000);
  http.setReuse(false);
  if (!http.begin(client, url)) {
    heap_caps_free(tlsGuard);
    return false;
  }
  http.addHeader("User-Agent", "ESP32-AMOLED-Clock/1.0");
  heap_caps_free(tlsGuard);
  tlsGuard = nullptr;
  const int status = http.GET();
  if (status == HTTP_CODE_OK) json = http.getString();
  char tlsError[128]{};
  const int tlsCode = status <= 0
                        ? client.lastError(tlsError, sizeof(tlsError)) : 0;
  if (tlsCode == MBEDTLS_ERR_SSL_ALLOC_FAILED)
    tlsMemoryPressureObserved = true;
  http.end();
  if (status != HTTP_CODE_OK || json.isEmpty() ||
      json.length() >= MAX_WEATHER_JSON_BYTES) {
    Serial.printf("%s: HTTP %d (%s), TLS %d (%s)\n", label, status,
                  status < 0 ? HTTPClient::errorToString(status).c_str()
                             : "server response",
                  tlsCode, tlsError[0] ? tlsError : "no TLS detail");
    return false;
  }
  return true;
}

bool fetchPlainHttpJson(const char *host, const char *resource,
                        const char *label, String &json) {
  if (!host || !resource || (playerVideoMode && weatherPageVisible))
    return false;
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.setReuse(false);
  const String url = String("http://") + host + resource;
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", "ESP32-AMOLED-Clock/1.0");
  const int status = http.GET();
  if (status == HTTP_CODE_OK) json = http.getString();
  http.end();
  if (status != HTTP_CODE_OK || json.isEmpty() ||
      json.length() >= MAX_WEATHER_JSON_BYTES) {
    Serial.printf("%s: HTTP %d (%s)\n", label, status,
                  status < 0 ? HTTPClient::errorToString(status).c_str()
                             : "server response");
    return false;
  }
  return true;
}

String effectiveWeatherLocation() {
  char automatic[sizeof(resolvedWeatherLocation)]{};
  portENTER_CRITICAL(&stateMux);
  strlcpy(automatic, resolvedWeatherLocation, sizeof(automatic));
  portEXIT_CRITICAL(&stateMux);
  if (weatherAutoLocationEnabled && automatic[0]) return String(automatic);
  return weatherLocation;
}

bool fetchAutomaticWeatherLocation() {
  if (!weatherAutoLocationEnabled) return true;
  String json;
  char latitude[20]{};
  char longitude[20]{};
  char city[sizeof(resolvedWeatherCity)]{};
  char timezone[sizeof(resolvedWeatherTimezone)]{};
  bool parsed = false;

  // ipapi.co currently returns 403 for this device/network and consumed the
  // clean boot-time TLS window before QWeather could use it. Prefer ipwho.is;
  // the non-TLS ip-api.com endpoint remains the low-memory fallback.
  if (fetchPublicJson("ipwho.is", "/", "IP location (ipwho.is)", json)) {
    parsed = jsonScalarField(json, "latitude", latitude, sizeof(latitude)) &&
             jsonScalarField(json, "longitude", longitude, sizeof(longitude));
    jsonScalarField(json, "city", city, sizeof(city));
    const int timezoneAt = json.indexOf("\"timezone\"");
    if (timezoneAt >= 0)
      jsonScalarField(json, "id", timezone, sizeof(timezone), timezoneAt);
  }
  if (!parsed) {
    json = "";
    memset(latitude, 0, sizeof(latitude));
    memset(longitude, 0, sizeof(longitude));
    memset(city, 0, sizeof(city));
    memset(timezone, 0, sizeof(timezone));
    if (fetchPlainHttpJson(
          "ip-api.com",
          "/json/?fields=status,message,city,lat,lon,timezone",
          "IP location (ip-api.com)", json)) {
      parsed = jsonScalarField(json, "lat", latitude, sizeof(latitude)) &&
               jsonScalarField(json, "lon", longitude, sizeof(longitude));
      jsonScalarField(json, "city", city, sizeof(city));
      jsonScalarField(json, "timezone", timezone, sizeof(timezone));
    }
  }
  if (!parsed) {
    Serial.println("IP location: all providers failed");
    return false;
  }
  const double lat = atof(latitude);
  const double lon = atof(longitude);
  if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
    Serial.println("IP location: coordinates out of range");
    return false;
  }
  char location[sizeof(resolvedWeatherLocation)]{};
  snprintf(location, sizeof(location), "%.5f,%.5f", lon, lat);
  portENTER_CRITICAL(&stateMux);
  strlcpy(resolvedWeatherLocation, location, sizeof(resolvedWeatherLocation));
  strlcpy(resolvedWeatherCity, city[0] ? city : "Auto location",
          sizeof(resolvedWeatherCity));
  strlcpy(resolvedWeatherTimezone, timezone, sizeof(resolvedWeatherTimezone));
  weatherInfoPending = true;
  weatherTrendPending = true;
  portEXIT_CRITICAL(&stateMux);
  Serial.printf("IP location: %s, %s (%s)\n",
                resolvedWeatherCity, resolvedWeatherLocation,
                resolvedWeatherTimezone[0] ? resolvedWeatherTimezone : "timezone unknown");
  return true;
}

bool fetchWeatherLocationName() {
  String location = effectiveWeatherLocation();
  location.trim();
  if (location.isEmpty()) return false;
  location.replace(",", "%2C");
  String json;
  if (!fetchQWeatherJson(String("/geo/v2/city/lookup?location=") + location +
                         "&lang=en&number=1", "QWeather city", json))
    return false;
  char apiStatus[8]{};
  char city[sizeof(resolvedWeatherCity)]{};
  char timezone[sizeof(resolvedWeatherTimezone)]{};
  if (!jsonField(json, "code", apiStatus, sizeof(apiStatus)) ||
      strcmp(apiStatus, "200") != 0 ||
      !jsonField(json, "name", city, sizeof(city))) return false;
  jsonField(json, "tz", timezone, sizeof(timezone));
  portENTER_CRITICAL(&stateMux);
  strlcpy(resolvedWeatherCity, city, sizeof(resolvedWeatherCity));
  if (timezone[0])
    strlcpy(resolvedWeatherTimezone, timezone,
            sizeof(resolvedWeatherTimezone));
  weatherInfoPending = true;
  weatherTrendPending = true;
  portEXIT_CRITICAL(&stateMux);
  Serial.printf("QWeather city: %s%s%s\n", city,
                timezone[0] ? ", " : "", timezone);
  return true;
}

bool fetchWeather() {
  String location = effectiveWeatherLocation();
  location.trim();
  if (location.isEmpty()) return false;
  location.replace(",", "%2C");
  String json;
  if (!fetchQWeatherJson(String("/v7/weather/now?location=") + location +
                         "&lang=en&unit=m", "QWeather", json)) return false;
  char apiStatus[8]{};
  char temperature[12]{};
  char icon[12]{};
  char condition[sizeof(currentConditionText)]{};
  char feelsLike[12]{};
  char humidity[12]{};
  char windDirection[20]{};
  char windScale[12]{};
  if (!jsonField(json, "code", apiStatus, sizeof(apiStatus)) ||
      strcmp(apiStatus, "200") != 0 ||
      !jsonField(json, "temp", temperature, sizeof(temperature)) ||
      !jsonField(json, "icon", icon, sizeof(icon))) return false;
  const int16_t parsedTemperature = static_cast<int16_t>(atoi(temperature));
  const uint16_t parsedCode = static_cast<uint16_t>(atoi(icon));
  jsonField(json, "text", condition, sizeof(condition));
  jsonField(json, "feelsLike", feelsLike, sizeof(feelsLike));
  jsonField(json, "humidity", humidity, sizeof(humidity));
  jsonField(json, "windDir", windDirection, sizeof(windDirection));
  jsonField(json, "windScale", windScale, sizeof(windScale));
  portENTER_CRITICAL(&stateMux);
  const bool overridden = serialOverrideCode >= 0;
  if (!overridden) {
    temperatureC = parsedTemperature;
    weatherCode = parsedCode;
    weatherLastSuccessAtMs = millis();
    weatherPending = true;
    strlcpy(currentConditionText, condition[0] ? condition : "Unknown",
            sizeof(currentConditionText));
    snprintf(currentWindText, sizeof(currentWindText), "%s%s%s",
             windDirection[0] ? windDirection : "--",
             windScale[0] ? " " : "", windScale);
    currentFeelsLikeC = static_cast<int16_t>(atoi(feelsLike));
    currentHumidity = static_cast<uint8_t>(constrain(atoi(humidity), 0, 100));
    weatherInfoPending = true;
  }
  portEXIT_CRITICAL(&stateMux);
  if (overridden)
    Serial.printf("QWeather: live %s held back by serial test mode\n",
                  sceneName(sceneForCode(parsedCode)));
  else
    Serial.printf("QWeather: %d C, %s\n", parsedTemperature,
                  sceneName(sceneForCode(parsedCode)));
  return true;
}

bool parseQWeatherIsoMinute(const char *value, uint16_t &minute) {
  if (!value || !*value) return false;
  const char *time = strchr(value, 'T');
  time = time ? time + 1 : value;
  if (time[0] < '0' || time[0] > '9' ||
      time[1] < '0' || time[1] > '9' || time[2] != ':' ||
      time[3] < '0' || time[3] > '9' ||
      time[4] < '0' || time[4] > '9') return false;
  const uint8_t hour = (time[0] - '0') * 10U + time[1] - '0';
  const uint8_t minutes = (time[3] - '0') * 10U + time[4] - '0';
  if (hour > 23 || minutes > 59) return false;
  minute = hour * 60U + minutes;
  return true;
}

void formatQWeatherTime(const char *value, char destination[6]) {
  uint16_t minute = 0;
  if (!parseQWeatherIsoMinute(value, minute)) {
    strlcpy(destination, "--:--", 6);
    return;
  }
  snprintf(destination, 6, "%02u:%02u", minute / 60U, minute % 60U);
}

bool fetchHourlyForecast() {
  String location = effectiveWeatherLocation();
  location.trim();
  if (location.isEmpty()) return false;
  location.replace(",", "%2C");
  String json;
  if (!fetchQWeatherJson(String("/v7/weather/24h?location=") + location +
                         "&lang=en&unit=m", "QWeather hourly", json))
    return false;
  char apiStatus[8]{};
  if (!jsonField(json, "code", apiStatus, sizeof(apiStatus)) ||
      strcmp(apiStatus, "200") != 0) return false;

  HourlyForecastPoint parsed[WEATHER_HOURLY_MAX_POINTS]{};
  uint8_t count = 0;
  int cursor = 0;
  while (count < WEATHER_HOURLY_MAX_POINTS) {
    char timeText[40]{};
    char temperature[12]{};
    int timeAt = -1;
    if (!jsonScalarField(json, "fxTime", timeText, sizeof(timeText), cursor,
                         &timeAt) ||
        !jsonScalarField(json, "temp", temperature, sizeof(temperature),
                         timeAt)) break;
    formatQWeatherTime(timeText, parsed[count].time);
    parsed[count].temperatureC = static_cast<int16_t>(atoi(temperature));
    ++count;
    cursor = timeAt + 8;
  }
  if (count < 2) {
    Serial.println("QWeather hourly: no usable forecast entries");
    return false;
  }
  portENTER_CRITICAL(&stateMux);
  memcpy(hourlyForecast, parsed, sizeof(hourlyForecast));
  hourlyForecastCount = count;
  weatherTrendPending = true;
  portEXIT_CRITICAL(&stateMux);
  Serial.printf("QWeather hourly: %u point(s), %s to %s\n", count,
                parsed[0].time, parsed[count - 1].time);
  return true;
}

bool fetchMinutelyPrecipitation() {
  String location = effectiveWeatherLocation();
  location.trim();
  if (location.isEmpty()) return false;
  location.replace(",", "%2C");
  String json;
  if (!fetchQWeatherJson(String("/v7/minutely/5m?location=") + location +
                         "&lang=en", "QWeather minutely", json))
    return false;
  char apiStatus[8]{};
  if (!jsonField(json, "code", apiStatus, sizeof(apiStatus)) ||
      strcmp(apiStatus, "200") != 0) return false;

  int16_t parsed[WEATHER_MINUTELY_MAX_POINTS]{};
  char firstTime[6]{};
  char lastTime[6]{};
  char summary[sizeof(minutelySummary)]{};
  jsonField(json, "summary", summary, sizeof(summary));
  uint8_t count = 0;
  int cursor = 0;
  while (count < WEATHER_MINUTELY_MAX_POINTS) {
    char timeText[40]{};
    char precip[16]{};
    int timeAt = -1;
    if (!jsonScalarField(json, "fxTime", timeText, sizeof(timeText), cursor,
                         &timeAt) ||
        !jsonScalarField(json, "precip", precip, sizeof(precip), timeAt))
      break;
    char formatted[6]{};
    formatQWeatherTime(timeText, formatted);
    if (count == 0) strlcpy(firstTime, formatted, sizeof(firstTime));
    strlcpy(lastTime, formatted, sizeof(lastTime));
    const double millimetres = max(0.0, atof(precip));
    parsed[count] = static_cast<int16_t>(constrain(
      static_cast<int>(millimetres * 100.0 + 0.5), 0, 32700));
    ++count;
    cursor = timeAt + 8;
  }
  if (count < 2) {
    Serial.println("QWeather minutely: no usable precipitation entries");
    return false;
  }
  portENTER_CRITICAL(&stateMux);
  memcpy(minutelyPrecipHundredths, parsed, sizeof(minutelyPrecipHundredths));
  minutelyPrecipCount = count;
  strlcpy(minutelyFirstTime, firstTime, sizeof(minutelyFirstTime));
  strlcpy(minutelyLastTime, lastTime, sizeof(minutelyLastTime));
  strlcpy(minutelySummary,
          summary[0] ? summary : "Two-hour precipitation forecast",
          sizeof(minutelySummary));
  weatherTrendPending = true;
  portEXIT_CRITICAL(&stateMux);
  Serial.printf("QWeather minutely: %u point(s), %s to %s\n", count,
                firstTime, lastTime);
  return true;
}

bool parseQWeatherSunPayload(const String &json, const char *source,
                             uint16_t &parsedSunrise,
                             uint16_t &parsedSunset) {
  char apiStatus[8]{};
  char sunriseText[40]{};
  char sunsetText[40]{};
  const bool valid = jsonField(json, "code", apiStatus, sizeof(apiStatus)) &&
                     strcmp(apiStatus, "200") == 0 &&
                     jsonField(json, "sunrise", sunriseText,
                               sizeof(sunriseText)) &&
                     jsonField(json, "sunset", sunsetText,
                               sizeof(sunsetText)) &&
                     parseQWeatherIsoMinute(sunriseText, parsedSunrise) &&
                     parseQWeatherIsoMinute(sunsetText, parsedSunset) &&
                     parsedSunrise < parsedSunset;
  if (!valid)
    Serial.printf("QWeather sun: unusable %s response (code=%s, rise=%s, set=%s)\n",
                  source, apiStatus[0] ? apiStatus : "missing",
                  sunriseText[0] ? sunriseText : "missing",
                  sunsetText[0] ? sunsetText : "missing");
  return valid;
}

bool fetchSunriseSunset() {
  String location = effectiveWeatherLocation();
  location.trim();
  if (location.isEmpty()) return false;
  location.replace(",", "%2C");
  String dailyJson;
  if (!fetchQWeatherJson(String("/v7/weather/3d?location=") + location +
                         "&lang=en&unit=m", "QWeather daily", dailyJson))
    return false;
  char apiStatus[8]{};
  if (!jsonField(dailyJson, "code", apiStatus, sizeof(apiStatus)) ||
      strcmp(apiStatus, "200") != 0) return false;

  ForecastDay parsedDays[3]{};
  int cursor = 0;
  uint8_t parsedCount = 0;
  for (uint8_t i = 0; i < 3; ++i) {
    char date[sizeof(parsedDays[i].date)]{};
    char icon[12]{};
    char minimum[12]{};
    char maximum[12]{};
    char text[sizeof(parsedDays[i].text)]{};
    int dateAt = -1;
    if (!jsonScalarField(dailyJson, "fxDate", date, sizeof(date), cursor,
                         &dateAt) ||
        !jsonScalarField(dailyJson, "tempMin", minimum, sizeof(minimum),
                         dateAt) ||
        !jsonScalarField(dailyJson, "tempMax", maximum, sizeof(maximum),
                         dateAt) ||
        !jsonScalarField(dailyJson, "iconDay", icon, sizeof(icon), dateAt) ||
        !jsonScalarField(dailyJson, "textDay", text, sizeof(text), dateAt))
      break;
    strlcpy(parsedDays[i].date, date, sizeof(parsedDays[i].date));
    strlcpy(parsedDays[i].text, text, sizeof(parsedDays[i].text));
    parsedDays[i].minimumC = static_cast<int16_t>(atoi(minimum));
    parsedDays[i].maximumC = static_cast<int16_t>(atoi(maximum));
    parsedDays[i].code = static_cast<uint16_t>(atoi(icon));
    parsedDays[i].valid = true;
    ++parsedCount;
    cursor = dateAt + 8;
  }
  if (!parsedCount) {
    Serial.println("QWeather daily: no usable forecast entries");
    return false;
  }

  LocalClockFields current{};
  const bool haveDate = readLocalClock(current);
  uint16_t parsedSunrise = 0;
  uint16_t parsedSunset = 0;
  const bool solarValid = parseQWeatherSunPayload(
    dailyJson, "3-day forecast", parsedSunrise, parsedSunset);

  portENTER_CRITICAL(&stateMux);
  memcpy(forecastDays, parsedDays, sizeof(forecastDays));
  if (haveDate && solarValid) {
    solarDate = localDateKey(current);
    sunriseMinute = parsedSunrise;
    sunsetMinute = parsedSunset;
  }
  // Re-evaluate the active clear/cloudy scene immediately using the newly
  // downloaded boundary, even when the temperature itself did not change.
  weatherPending = true;
  weatherInfoPending = true;
  portEXIT_CRITICAL(&stateMux);
  Serial.printf("QWeather daily: %u day(s), sunrise %02u:%02u, sunset %02u:%02u\n",
                parsedCount, parsedSunrise / 60U, parsedSunrise % 60U,
                parsedSunset / 60U, parsedSunset % 60U);
  return true;
}

bool fetchAirQuality() {
  String location = effectiveWeatherLocation();
  location.trim();
  if (location.isEmpty()) return false;
  const int comma = location.indexOf(',');
  if (comma <= 0 || comma >= static_cast<int>(location.length()) - 1) {
    Serial.println("QWeather AQI: coordinates unavailable");
    return false;
  }
  String longitude = location.substring(0, comma);
  String latitude = location.substring(comma + 1);
  longitude.trim();
  latitude.trim();
  String json;
  bool valid = false;
  uint16_t parsedAqi = 0;
  String encodedLocation = longitude + "%2C" + latitude;
  if (fetchQWeatherJson(String("/v7/air/now?location=") + encodedLocation +
                        "&lang=en", "QWeather AQI v7", json)) {
    char aqi[12]{};
    valid = jsonField(json, "aqi", aqi, sizeof(aqi));
    if (valid) parsedAqi = static_cast<uint16_t>(constrain(atoi(aqi), 0, 999));
  }
  // Newer projects use Air Quality API v1, while many existing API-key
  // projects only authorize Web API v7. Supporting both avoids a permanent
  // 403 when the weather endpoints and the credential generation differ.
  if (!valid && !tlsMemoryPressureObserved &&
      !(playerVideoMode && weatherPageVisible)) {
    json = "";
    if (fetchQWeatherJson(String("/airquality/v1/current/") + latitude + "/" +
                          longitude + "?lang=en", "QWeather AQI v1", json)) {
      char aqi[12]{};
      valid = jsonScalarField(json, "aqi", aqi, sizeof(aqi));
      if (valid)
        parsedAqi = static_cast<uint16_t>(constrain(atoi(aqi), 0, 999));
    }
  }
  portENTER_CRITICAL(&stateMux);
  currentAqi = parsedAqi;
  currentAqiValid = valid;
  weatherInfoPending = true;
  portEXIT_CRITICAL(&stateMux);
  if (valid) Serial.printf("QWeather AQI: %u\n", parsedAqi);
  else Serial.println("QWeather AQI: unavailable");
  return valid;
}

void networkTask(void *) {
  // Let SNTP, the web server and Wi-Fi event callbacks finish their startup
  // allocations before the first TLS handshake.
  uint32_t nextAttempt = millis() + WEATHER_BOOT_SETTLE_MS;
  uint8_t consecutiveSolarFailures = 0;
  while (true) {
    const uint32_t now = millis();
    if (weatherRefreshRequested) {
      weatherRefreshRequested = false;
      nextAttempt = 0;
      consecutiveSolarFailures = 0;
    }
    // Video playback owns the TF/JPEG pipeline continuously. Do not pause it
    // for periodic weather HTTPS requests; refresh immediately after leaving
    // the video application instead.
    if (playerVideoMode && weatherPageVisible) {
      nextAttempt = 0;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (WiFi.status() == WL_CONNECTED &&
        static_cast<int32_t>(now - nextAttempt) >= 0) {
      const bool weatherWallpaperVisible =
        weatherPageVisible && !playerVideoMode;
      const size_t liveLargestBlock = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      const bool queryBesideWallpaper =
        weatherWallpaperVisible && initialized &&
        liveLargestBlock >= WEATHER_LIVE_TLS_WINDOW_BYTES;

      // Never freeze a visible weather wallpaper merely to make room for an
      // API refresh. With sufficient post-LVGL-migration SRAM, the network
      // task runs at lower priority beside the media workers. If memory is
      // tight, keep the animation running and retry immediately after leaving
      // the wallpaper page.
      if (weatherWallpaperVisible && !queryBesideWallpaper) {
        nextAttempt = 0;
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }

      if (queryBesideWallpaper) {
        Serial.printf(
          "QWeather: refreshing beside live wallpaper, TLS window %lu KiB\n",
          static_cast<unsigned long>(liveLargestBlock / 1024U));
      } else {
        // When no wallpaper is visible, retain the conservative hand-off:
        // park workers and lend their contiguous internal allocations to TLS.
        assetFadeNetworkCancelRequested = true;
        const uint32_t fadeCancelStartedAt = millis();
        while (assetFadeNetworkCancelRequested &&
               millis() - fadeCancelStartedAt < 250) {
          vTaskDelay(pdMS_TO_TICKS(1));
        }
        networkResourceHold = true;
        const bool pipelineIdle = !initialized || pausePipeline();
        if (!pipelineIdle) {
          networkResourceHold = false;
          nextAttempt = millis() + WEATHER_MEMORY_RETRY_MS;
          Serial.println("QWeather: media pipeline still busy; HTTPS deferred");
          vTaskDelay(pdMS_TO_TICKS(100));
          continue;
        }
        if (initialized) releasePlaybackResources();
        if (playerVideoMode && weatherPageVisible) {
          networkResourceHold = false;
          nextAttempt = 0;
          vTaskDelay(pdMS_TO_TICKS(100));
          continue;
        }
        const size_t largestTlsBlock = heap_caps_get_largest_free_block(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (largestTlsBlock < TLS_HANDSHAKE_WINDOW_BYTES) {
          Serial.printf(
            "QWeather: HTTPS deferred; contiguous SRAM %lu KiB, need %lu KiB\n",
            static_cast<unsigned long>(largestTlsBlock / 1024U),
            static_cast<unsigned long>(TLS_HANDSHAKE_WINDOW_BYTES / 1024U));
          networkResourceHold = false;
          nextAttempt = millis() + WEATHER_MEMORY_RETRY_MS;
          vTaskDelay(pdMS_TO_TICKS(100));
          continue;
        }
      }
      tlsMemoryPressureObserved = false;
      bool needAutomaticLocation = false;
      portENTER_CRITICAL(&stateMux);
      needAutomaticLocation = weatherAutoLocationEnabled &&
                              resolvedWeatherLocation[0] == '\0';
      portEXIT_CRITICAL(&stateMux);
      const bool locationOk = !needAutomaticLocation ||
                              fetchAutomaticWeatherLocation();
      if (!locationOk && !weatherLocation.isEmpty())
        Serial.println("IP location unavailable; using manual fallback");
      bool needCityName = false;
      portENTER_CRITICAL(&stateMux);
      needCityName = resolvedWeatherCity[0] == '\0';
      portEXIT_CRITICAL(&stateMux);
      bool weatherOk = false;
      bool solarOk = false;
      bool aqiOk = false;
      bool hourlyOk = false;
      bool minutelyOk = false;
      if (!tlsMemoryPressureObserved) {
        // Current conditions, forecast and optional city lookup share one
        // keep-alive TLS connection. Put essential weather first so an
        // optional geocoding failure can never suppress the current reading.
        WiFiClientSecure qweatherClient;
        qweatherClient.setInsecure();
        qweatherClient.setHandshakeTimeout(15);
        qweatherClient.setTimeout(12000);
        HTTPClient qweatherHttp;
        qweatherSessionClient = &qweatherClient;
        qweatherSessionHttp = &qweatherHttp;
        qweatherSessionBegun = false;
        weatherOk = fetchWeather();
        if (!tlsMemoryPressureObserved &&
            !(playerVideoMode && weatherPageVisible))
          solarOk = fetchSunriseSunset();
        if (!tlsMemoryPressureObserved &&
            !(playerVideoMode && weatherPageVisible))
          hourlyOk = fetchHourlyForecast();
        if (!tlsMemoryPressureObserved &&
            !(playerVideoMode && weatherPageVisible))
          minutelyOk = fetchMinutelyPrecipitation();
        if (!tlsMemoryPressureObserved &&
            !(playerVideoMode && weatherPageVisible))
          aqiOk = fetchAirQuality();
        if (needCityName && !tlsMemoryPressureObserved &&
            !(playerVideoMode && weatherPageVisible))
          fetchWeatherLocationName();
        qweatherHttp.end();
        qweatherClient.stop();
        qweatherSessionClient = nullptr;
        qweatherSessionHttp = nullptr;
        qweatherSessionBegun = false;
      }
      (void)aqiOk;
      (void)hourlyOk;
      (void)minutelyOk;
      if (!tlsMemoryPressureObserved) {
        if (solarOk) consecutiveSolarFailures = 0;
        else if (consecutiveSolarFailures < UINT8_MAX)
          ++consecutiveSolarFailures;
      }
      networkResourceHold = false;
      // If a hidden media pipeline was lent to TLS, do not rebuild its
      // hardware decoder here: this task is pinned to core0. service() notices
      // pipelinePauseRequested on core1 and performs the clean resume there.
      // A missing RTC at boot gets one quick astronomy retry after NTP has
      // settled. Persistent astronomy/account errors must not pause playback
      // every minute forever; after that first retry use the normal cadence.
      const bool quickRetry = !weatherOk ||
                               (!solarOk && consecutiveSolarFailures == 1);
      nextAttempt = (playerVideoMode && weatherPageVisible)
                      ? 0
                      : now + (tlsMemoryPressureObserved
                                 ? WEATHER_MEMORY_RETRY_MS
                                 : (quickRetry ? WEATHER_RETRY_MS
                                               : WEATHER_REFRESH_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void startNetworkService() {
  if (networkServiceStarted) return;
  networkServiceStarted = true;
  BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
    networkTask, "clock_weather", 16 * 1024, nullptr, 1,
    &networkTaskHandle, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (created == pdPASS) {
    Serial.println("QWeather task stack: 16 KiB PSRAM");
    return;
  }
  Serial.println("QWeather task: PSRAM stack unavailable, using internal SRAM");
  created = xTaskCreatePinnedToCore(
    networkTask, "clock_weather", 16 * 1024,
    nullptr, 1, &networkTaskHandle, 0);
  if (created != pdPASS) {
    networkTaskHandle = nullptr;
    networkServiceStarted = false;
    Serial.println("Unable to start QWeather task");
  }
}

void updatePlayerModeUi() {
  if (!clockLabel || !weatherIconHolder || !temperatureLabel ||
      !videoProgressBar || !videoTimeLabel || !videoNameLabel) return;
  if (playerVideoMode) {
    lv_obj_add_flag(clockLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(weatherIconHolder, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(temperatureLabel, LV_OBJ_FLAG_HIDDEN);
    if (!videoNameHideAtMs)
      lv_obj_clear_flag(videoTimeLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(videoProgressBar, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(clockLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(weatherIconHolder, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(temperatureLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(videoTimeLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(videoNameLabel, LV_OBJ_FLAG_HIDDEN);
    videoNameHideAtMs = 0;
    lv_obj_add_flag(videoProgressBar, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_invalidate(statusPanel);
}

void setPlayerMode(bool video) {
  const bool changed = playerVideoMode != video;
  if (changed && playerVideoMode) persistVideoPosition(true);
  if (changed) resetVideoAudioSync(true);
  playerVideoMode = video;
  if (changed || !video) videoPlaybackUserPaused = false;
  if (changed) {
    videoNameHideAtMs = 0;
    displayedVideoNameIndex = UINT8_MAX;
    if (videoNameLabel) lv_obj_add_flag(videoNameLabel, LV_OBJ_FLAG_HIDDEN);
    videoSeekDragging = false;
    currentVideoPositionIndex = UINT8_MAX;
    currentVideoPositionFrame = 0;
    currentVideoPositionFileOffset = 0;
    currentVideoFrameCount = 0;
    currentVideoFpsTimes100 = 0;
  }
  if (!initialized) initializationFailed = false;
  if (video && videoProgressBar) {
    lv_slider_set_value(videoProgressBar, 0, LV_ANIM_OFF);
    updateVideoTimeLabel(0, 0, 0);
  }
  Serial.printf("Media mode selected: %s; weather network %s\n",
                video ? "video" : "weather", video ? "paused" : "enabled");
  if (changed) {
    requestedIndex = -1;
    nextRandomizeAtMs = 0;
    if (initialized) {
      const int16_t initial = firstAvailableAsset();
      if (initial >= 0) {
        activeIndex = static_cast<uint8_t>(initial);
        setReaderAsset(activeIndex, rotatedAssetRequired(), video, true);
      } else {
        setMessage(video
                     ? "No MJPEG video found\nCopy .mjpeg files to /video"
                     : "No weather wallpaper found\nCopy /wallpaper to the TF card",
                   lv_color_hex(0xFCA5A5));
      }
    }
  }
  updatePlayerModeUi();
}

void serviceStatus() {
  if (playerVideoMode && weatherPageVisible) return;
  bool weatherExpired = false;
  const uint32_t now = millis();
  portENTER_CRITICAL(&stateMux);
  if (serialOverrideCode < 0 && weatherLastSuccessAtMs != 0 &&
      now - weatherLastSuccessAtMs >= WEATHER_STALE_MS) {
    weatherLastSuccessAtMs = 0;
    temperatureC = 0;
    weatherCode = 999;
    strlcpy(currentConditionText, "Unknown", sizeof(currentConditionText));
    strlcpy(currentWindText, "--", sizeof(currentWindText));
    currentHumidity = 0;
    currentAqi = 0;
    currentAqiValid = false;
    currentFeelsLikeC = 0;
    memset(forecastDays, 0, sizeof(forecastDays));
    memset(hourlyForecast, 0, sizeof(hourlyForecast));
    hourlyForecastCount = 0;
    memset(minutelyPrecipHundredths, 0, sizeof(minutelyPrecipHundredths));
    minutelyPrecipCount = 0;
    minutelyFirstTime[0] = '\0';
    minutelyLastTime[0] = '\0';
    strlcpy(minutelySummary, "Minute forecast unavailable",
            sizeof(minutelySummary));
    weatherPending = true;
    weatherInfoPending = true;
    weatherTrendPending = true;
    weatherExpired = true;
  }
  portEXIT_CRITICAL(&stateMux);
  if (weatherExpired)
    Serial.println("QWeather: cached data expired; showing unknown until refresh");
  if (weatherPending) {
    int16_t value = 0;
    uint16_t code = 999;
    int8_t overrideScene = -1;
    portENTER_CRITICAL(&stateMux);
    value = temperatureC;
    code = weatherCode;
    overrideScene = serialOverrideScene;
    weatherPending = false;
    portEXIT_CRITICAL(&stateMux);
    if (code == 999) lv_label_set_text(temperatureLabel, "--\xC2\xB0");
    else lv_label_set_text_fmt(temperatureLabel, "%d\xC2\xB0", value);
    updateWeatherIcon(code);
    if (weatherWallpaperFollowEnabled)
      requestScene(overrideScene >= 0
                     ? static_cast<WeatherScene>(overrideScene)
                     : sceneForCode(code));
  }

  LocalClockFields current{};
  if (!readLocalClock(current)) return;
  const uint32_t minute = current.hour * 60U + current.minute;
  if (minute == lastClockMinute) return;
  lastClockMinute = minute;
  lv_label_set_text_fmt(clockLabel, "%02u:%02u", current.hour, current.minute);

  // QWeather's current-condition icon can lag a sunrise/sunset transition.
  // Re-evaluate the wallpaper at the exact downloaded boundary instead of
  // waiting for the next 15-minute weather refresh.
  const bool night = localNight();
  if (lastLocalNight >= 0 && night != (lastLocalNight != 0)) {
    uint16_t code = 999;
    int16_t overrideCode = -1;
    portENTER_CRITICAL(&stateMux);
    code = weatherCode;
    overrideCode = serialOverrideCode;
    portEXIT_CRITICAL(&stateMux);
    const WeatherScene desired = sceneForCode(code);
    if (weatherWallpaperFollowEnabled && overrideCode < 0 &&
        (desired == WeatherScene::Clear || desired == WeatherScene::Cloudy ||
         desired == WeatherScene::Overcast || desired == WeatherScene::Night))
      requestScene(desired);
  }
  lastLocalNight = night ? 1 : 0;
}

void setInfoSubpage(uint8_t index) {
  if (!weatherInfoOverviewPage || !weatherInfoTrendPage) return;
  index = index ? 1 : 0;
  weatherInfoSubpageIndex = index;
  if (index == 0) {
    lv_obj_clear_flag(weatherInfoOverviewPage, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(weatherInfoTrendPage, LV_OBJ_FLAG_HIDDEN);
  } else {
    weatherTrendPending = true;
    lv_obj_add_flag(weatherInfoOverviewPage, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(weatherInfoTrendPage, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_invalidate(weatherInfoPage);
}

bool handleInfoHorizontalGesture(lv_dir_t direction) {
  if (direction == LV_DIR_LEFT && weatherInfoSubpageIndex == 0) {
    setInfoSubpage(1);
    return true;
  }
  if (direction == LV_DIR_RIGHT && weatherInfoSubpageIndex == 1) {
    setInfoSubpage(0);
    return true;
  }
  return direction == LV_DIR_LEFT || direction == LV_DIR_RIGHT;
}

void serviceTrendPage() {
  if (!weatherTrendPending || !weatherInfoTrendPage) return;
  HourlyForecastPoint hourly[WEATHER_HOURLY_MAX_POINTS]{};
  int16_t minutely[WEATHER_MINUTELY_MAX_POINTS]{};
  uint8_t hourlyCount = 0;
  uint8_t minutelyCount = 0;
  char summary[sizeof(minutelySummary)]{};
  char city[sizeof(resolvedWeatherCity)]{};
  portENTER_CRITICAL(&stateMux);
  hourlyCount = hourlyForecastCount;
  minutelyCount = minutelyPrecipCount;
  memcpy(hourly, hourlyForecast, sizeof(hourly));
  memcpy(minutely, minutelyPrecipHundredths, sizeof(minutely));
  strlcpy(summary, minutelySummary, sizeof(summary));
  strlcpy(city, resolvedWeatherCity, sizeof(city));
  weatherTrendPending = false;
  portEXIT_CRITICAL(&stateMux);

  const String displayedLocation = city[0] ? String(city)
                                           : effectiveWeatherLocation();
  lv_label_set_text(weatherTrendLocationLabel,
                    displayedLocation.isEmpty() ? "Unknown location"
                                                : displayedLocation.c_str());

  lv_chart_set_all_value(weatherHourlyChart, weatherHourlySeries,
                         LV_CHART_POINT_NONE);
  weatherHourlySmooth.count = 0;
  if (hourlyCount >= 2) {
    int16_t minimum = hourly[0].temperatureC;
    int16_t maximum = hourly[0].temperatureC;
    for (uint8_t i = 0; i < hourlyCount; ++i) {
      if (hourly[i].temperatureC < minimum) minimum = hourly[i].temperatureC;
      if (hourly[i].temperatureC > maximum) maximum = hourly[i].temperatureC;
      lv_chart_set_value_by_id(weatherHourlyChart, weatherHourlySeries, i,
                               hourly[i].temperatureC);
      weatherHourlySmooth.values[i] = hourly[i].temperatureC;
    }
    const int16_t chartMinimum = minimum == maximum ? minimum - 1 : minimum;
    const int16_t chartMaximum = minimum == maximum ? maximum + 1 : maximum;
    lv_chart_set_range(weatherHourlyChart, LV_CHART_AXIS_PRIMARY_Y,
                       chartMinimum, chartMaximum);
    weatherHourlySmooth.count = hourlyCount;
    weatherHourlySmooth.minimum = chartMinimum;
    weatherHourlySmooth.maximum = chartMaximum;
    lv_label_set_text_fmt(weatherHourlyStatusLabel,
                          "%d\xC2\xB0" "C - %d\xC2\xB0" "C",
                          minimum, maximum);
    lv_label_set_text_fmt(weatherHourlyYLabels[0], "%d\xC2\xB0" "C", maximum);
    lv_label_set_text_fmt(weatherHourlyYLabels[1], "%d\xC2\xB0" "C",
                          (minimum + maximum) / 2);
    lv_label_set_text_fmt(weatherHourlyYLabels[2], "%d\xC2\xB0" "C", minimum);
  } else {
    lv_chart_set_range(weatherHourlyChart, LV_CHART_AXIS_PRIMARY_Y, -10, 40);
    lv_label_set_text(weatherHourlyStatusLabel, "Forecast unavailable");
    for (lv_obj_t *label : weatherHourlyYLabels) lv_label_set_text(label, "--");
  }
  lv_chart_refresh(weatherHourlyChart);

  lv_chart_set_all_value(weatherMinutelyChart, weatherMinutelySeries,
                         LV_CHART_POINT_NONE);
  weatherMinutelySmooth.count = 0;
  if (minutelyCount >= 2) {
    int16_t maximum = 0;
    for (uint8_t i = 0; i < minutelyCount; ++i) {
      if (minutely[i] > maximum) maximum = minutely[i];
      lv_chart_set_value_by_id(weatherMinutelyChart, weatherMinutelySeries, i,
                               minutely[i]);
      weatherMinutelySmooth.values[i] = minutely[i];
    }
    const int16_t upperRange = maximum < 10
                                 ? 10 : maximum + maximum / 4 + 1;
    lv_chart_set_range(weatherMinutelyChart, LV_CHART_AXIS_PRIMARY_Y,
                       0, upperRange);
    weatherMinutelySmooth.count = minutelyCount;
    weatherMinutelySmooth.minimum = 0;
    weatherMinutelySmooth.maximum = upperRange;
    lv_label_set_text(weatherMinutelyStatusLabel,
                      summary[0] ? summary : "Two-hour forecast");
    // LVGL's compact formatter is built without floating-point support in
    // this project; using %.1f therefore renders literally as "fmm". Convert
    // hundredths of a millimetre to rounded tenths using integers instead.
    const int16_t upperTenths = static_cast<int16_t>((upperRange + 5) / 10);
    lv_label_set_text_fmt(weatherMinutelyYLabels[0], "%d.%dmm",
                          upperTenths / 10, upperTenths % 10);
    lv_label_set_text(weatherMinutelyYLabels[1], "0.0mm");
  } else {
    lv_chart_set_range(weatherMinutelyChart, LV_CHART_AXIS_PRIMARY_Y, 0, 10);
    lv_label_set_text(weatherMinutelyStatusLabel,
                      "Minute forecast unavailable");
    lv_label_set_text(weatherMinutelyYLabels[0], "--");
    lv_label_set_text(weatherMinutelyYLabels[1], "0.0mm");
  }
  lv_chart_refresh(weatherMinutelyChart);
  lv_obj_invalidate(weatherInfoTrendPage);
}

void serviceInfoPage() {
  if (!weatherInfoPageVisible || !weatherInfoPage) return;
  // Reuse the same expiry path as the wallpaper status so the dedicated app
  // can never keep showing data that the compact clock view has rejected.
  serviceStatus();
  static uint32_t displayedAgeMinute = UINT32_MAX;
  const uint32_t ageMinute = millis() / 60000UL;
  if (ageMinute != displayedAgeMinute) {
    displayedAgeMinute = ageMinute;
    weatherInfoPending = true;
  }
  serviceTrendPage();
  if (!weatherInfoPending) return;

  int16_t value = 0;
  int16_t feels = 0;
  uint16_t code = 999;
  uint8_t humidity = 0;
  uint16_t aqi = 0;
  bool aqiValid = false;
  uint32_t lastSuccess = 0;
  ForecastDay days[3]{};
  char condition[sizeof(currentConditionText)]{};
  char wind[sizeof(currentWindText)]{};
  char city[sizeof(resolvedWeatherCity)]{};
  portENTER_CRITICAL(&stateMux);
  value = temperatureC;
  feels = currentFeelsLikeC;
  code = weatherCode;
  humidity = currentHumidity;
  aqi = currentAqi;
  aqiValid = currentAqiValid;
  lastSuccess = weatherLastSuccessAtMs;
  memcpy(days, forecastDays, sizeof(days));
  strlcpy(condition, currentConditionText, sizeof(condition));
  strlcpy(wind, currentWindText, sizeof(wind));
  strlcpy(city, resolvedWeatherCity, sizeof(city));
  weatherInfoPending = false;
  portEXIT_CRITICAL(&stateMux);

  const String displayedLocation = city[0] ? String(city)
                                           : effectiveWeatherLocation();
  lv_label_set_text(weatherInfoLocationLabel,
                    displayedLocation.isEmpty() ? "Unknown location"
                                                : displayedLocation.c_str());
  if (lastSuccess) {
    const uint32_t ageMinutes = (millis() - lastSuccess) / 60000UL;
    lv_label_set_text_fmt(weatherInfoUpdatedLabel, "%lum ago",
                          static_cast<unsigned long>(ageMinutes));
  } else {
    lv_label_set_text(weatherInfoUpdatedLabel, "Waiting for update");
  }
  if (code == 999) {
    lv_label_set_text(weatherInfoTemperatureLabel, "--\xC2\xB0");
    lv_label_set_text(weatherInfoConditionLabel, "Unknown");
    lv_label_set_text(weatherInfoFeelsLabel, "Feels --");
    lv_label_set_text(weatherInfoHumidityLabel, "Humidity --");
    lv_label_set_text(weatherInfoWindLabel, "Wind --");
    lv_label_set_text(weatherInfoAqiLabel, "AQI --");
  } else {
    lv_label_set_text_fmt(weatherInfoTemperatureLabel, "%d\xC2\xB0", value);
    lv_label_set_text(weatherInfoConditionLabel,
                      condition[0] ? condition : "Unknown");
    lv_label_set_text_fmt(weatherInfoFeelsLabel, "Feels %d\xC2\xB0", feels);
    lv_label_set_text_fmt(weatherInfoHumidityLabel, "Humidity %u%%", humidity);
    lv_label_set_text_fmt(weatherInfoWindLabel, "Wind %s", wind[0] ? wind : "--");
    if (aqiValid) lv_label_set_text_fmt(weatherInfoAqiLabel, "AQI %u", aqi);
    else lv_label_set_text(weatherInfoAqiLabel, "AQI --");
  }
  lv_obj_set_style_text_color(weatherInfoConditionLabel,
                              weatherAccentForCode(code), 0);
  setWeatherIconInHolder(weatherInfoCurrentIcon, code);
  for (uint8_t i = 0; i < 3; ++i) {
    if (!days[i].valid || code == 999) {
      lv_label_set_text(weatherInfoForecastDateLabels[i], "--");
      lv_label_set_text(weatherInfoForecastTemperatureLabels[i], "-- / --");
      lv_label_set_text(weatherInfoForecastConditionLabels[i], "Unknown");
      setWeatherIconInHolder(weatherInfoForecastIcons[i], 999);
      continue;
    }
    const char *shortDate = strlen(days[i].date) >= 10
                              ? days[i].date + 5 : days[i].date;
    lv_label_set_text(weatherInfoForecastDateLabels[i], shortDate);
    lv_label_set_text_fmt(weatherInfoForecastTemperatureLabels[i],
                          "%d\xC2\xB0 / %d\xC2\xB0",
                          days[i].minimumC, days[i].maximumC);
    lv_label_set_text(weatherInfoForecastConditionLabels[i], days[i].text);
    setWeatherIconInHolder(weatherInfoForecastIcons[i], days[i].code);
  }
  lv_obj_invalidate(weatherInfoPage);
}

void service(bool forceFrame = false) {
  if (assetFadeNetworkCancelRequested) {
    cancelAssetFade(true);
    assetFadeNetworkCancelRequested = false;
  }
  // Run the UI timeout before every early return (paused video, control panel,
  // memory hand-off) so the filename can never remain visible indefinitely.
  serviceVideoNameOverlay();
  if (!weatherPageVisible || lowPowerMode || controlPanelVisible ||
      controlPanelShowRequested) return;
  (void)forceFrame;
  serviceStatus();
  if (!initializePlayer()) return;
  if (playerVideoMode && videoPlaybackUserPaused) return;
  if (pipelinePauseRequested &&
      (networkResourceHold || !readerIdle || !decoderIdle || !displayIdle ||
       !resumePipeline())) return;
  serviceAssetFade();
  if (assetFadeState != AssetFadeState::Idle) {
    printPlaybackStats();
    return;
  }
  if (requestedIndex >= 0) {
    beginRequestedAssetFade();
    return;
  }
  if (playbackErrorPending) {
    playbackErrorPending = false;
    const char *message = "Media playback failed\nSee Serial log";
    switch (playbackErrorCode) {
      case PlaybackErrorCode::OpenFile:
        message = "Cannot open video on TF card\nSee Serial log";
        break;
      case PlaybackErrorCode::ReadHeader:
        message = "Incomplete MJP1 file\nSee Serial log";
        break;
      case PlaybackErrorCode::InvalidHeader:
        message = "Unsupported MJP1 header\nSee Serial log";
        break;
      case PlaybackErrorCode::Seek:
        message = "TF card seek failed\nSee Serial log";
        break;
      case PlaybackErrorCode::FrameLength:
        message = "Invalid MJPEG frame length\nSee Serial log";
        break;
      case PlaybackErrorCode::FrameData:
        message = "TF card frame read failed\nSee Serial log";
        break;
      case PlaybackErrorCode::JpegDecode:
        message = "JPEG frame decode failed\nSee Serial log";
        break;
      default:
        message = playerVideoMode ? "MJPEG video playback failed"
                                  : "Weather wallpaper playback failed";
        break;
    }
    setMessage(message, lv_color_hex(0xFCA5A5));
  }
  printPlaybackStats();

  const uint32_t now = millis();
  if (playerVideoMode || !weatherAutoSwitchEnabled) {
    nextRandomizeAtMs = 0;
  } else {
    const uint32_t intervalMs = static_cast<uint32_t>(weatherSwitchMinutes) * 60000UL;
    if (!nextRandomizeAtMs) nextRandomizeAtMs = now + intervalMs;
    if (static_cast<int32_t>(now - nextRandomizeAtMs) >= 0) {
      requestScene(weatherWallpapers[activeIndex].scene);
      nextRandomizeAtMs = now + intervalMs;
    }
  }

}

bool waitForPipelineIdle(uint32_t timeoutMs = PIPELINE_STOP_TIMEOUT_MS) {
  const uint32_t startedAtMs = millis();
  while ((!readerIdle || !decoderIdle || !displayIdle) &&
         millis() - startedAtMs < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return readerIdle && decoderIdle && displayIdle;
}

void resetPipelineQueues() {
  if (!freeCompressedQueue || !readyCompressedQueue || !freeVideoQueue ||
      !readyVideoQueue || !videoDisplayResultQueue) return;
  xQueueReset(freeCompressedQueue);
  xQueueReset(readyCompressedQueue);
  xQueueReset(freeVideoQueue);
  xQueueReset(readyVideoQueue);
  xQueueReset(videoDisplayResultQueue);
  for (uint8_t slot = 0; slot < PIPELINE_BUFFER_COUNT; ++slot) {
    xQueueSend(freeCompressedQueue, &slot, 0);
    xQueueSend(freeVideoQueue, &slot, 0);
  }
}

bool pausePipeline(bool announce) {
  if (!initialized) return true;
  // A network hand-off, control-panel overlay, or user pause may already have
  // parked every worker. Treat that state as idempotent instead of bumping the
  // generation and resetting the queues a second time.
  if (pipelinePauseRequested && readerIdle && decoderIdle && displayIdle)
    return true;
  const uint32_t startedAtMs = millis();
  cancelAssetFade(false);
  requestedIndex = -1;
  if (playerVideoMode) resetVideoAudioSync(true);
  if (playerVideoMode) persistVideoPosition(true);
  portENTER_CRITICAL(&stateMux);
  if (playerVideoMode && currentVideoPositionIndex == activeIndex) {
    requestedReaderIndex = activeIndex;
    requestedReaderRotated = rotatedAssetRequired();
    requestedReaderFrame = currentVideoPositionFrame;
    requestedReaderFileOffset = currentVideoPositionFileOffset;
  }
  pipelinePauseRequested = true;
  ++pipelineGeneration;
  portEXIT_CRITICAL(&stateMux);
  displayedGeneration = 0;
  const bool idle = waitForPipelineIdle();
  if (idle) {
    resetPipelineQueues();
    resetPlaybackStats();
    if (announce)
      Serial.printf("Weather pipeline paused safely in %lu ms\n",
                    static_cast<unsigned long>(millis() - startedAtMs));
  } else {
    Serial.printf("Weather pipeline stop timeout: reader/decoder/display %u/%u/%u\n",
                  readerIdle ? 1U : 0U, decoderIdle ? 1U : 0U,
                  displayIdle ? 1U : 0U);
  }
  return idle;
}

bool resumePipeline() {
  if (!initialized || networkResourceHold) return false;
  pipelinePauseRequested = true;
  if (!waitForPipelineIdle()) {
    Serial.println("Weather pipeline resume deferred: worker still busy");
    return false;
  }
  if (playerVideoMode && !prepareVideoAudioI2S())
    Serial.println("Video audio: I2S reservation failed during pipeline resume");
  if (!ensurePlaybackResources() || !ensurePipelineQueues()) {
    Serial.println("Weather pipeline resume failed: playback memory unavailable");
    return false;
  }
  resetPipelineQueues();
  if (!ensurePipelineTasks()) {
    Serial.println("Weather pipeline resume failed: worker startup unavailable");
    return false;
  }
  const bool rotatedAsset = rotatedAssetRequired();
  const uint32_t resumeOffset =
    rotatedAsset == requestedReaderRotated ? requestedReaderFileOffset : 0;
  setReaderRequest(activeIndex, rotatedAsset, requestedReaderFrame,
                   resumeOffset, true);
  resetPlaybackStats();
  pipelinePauseRequested = false;
  Serial.println("Weather pipeline resumed with clean queues");
  return true;
}

bool closeForLowPower() {
  const bool idle = pausePipeline();
  // Buffers and decoder are intentionally retained for a quick wake; the SD
  // card itself is shut down by suspendMusicForLowPower().
  return idle;
}

void requestWeatherRefresh() {
  portENTER_CRITICAL(&stateMux);
  solarDate = 0;
  weatherLastSuccessAtMs = 0;
  weatherCode = 999;
  temperatureC = 0;
  strlcpy(currentConditionText, "Unknown", sizeof(currentConditionText));
  memset(forecastDays, 0, sizeof(forecastDays));
  memset(hourlyForecast, 0, sizeof(hourlyForecast));
  hourlyForecastCount = 0;
  memset(minutelyPrecipHundredths, 0, sizeof(minutelyPrecipHundredths));
  minutelyPrecipCount = 0;
  minutelyFirstTime[0] = '\0';
  minutelyLastTime[0] = '\0';
  strlcpy(minutelySummary, "Minute forecast unavailable",
          sizeof(minutelySummary));
  resolvedWeatherLocation[0] = '\0';
  resolvedWeatherCity[0] = '\0';
  resolvedWeatherTimezone[0] = '\0';
  weatherPending = true;
  weatherInfoPending = true;
  weatherTrendPending = true;
  portEXIT_CRITICAL(&stateMux);
  weatherRefreshRequested = true;
}

bool serialCondition(const String &name, WeatherScene &scene, uint16_t &code) {
  if (name == "clear") {
    scene = WeatherScene::Clear; code = 100;
  } else if (name == "cloudy") {
    scene = WeatherScene::Cloudy; code = 101;
  } else if (name == "overcast") {
    scene = WeatherScene::Overcast; code = 104;
  } else if (name == "rain") {
    scene = WeatherScene::Rain; code = 305;
  } else if (name == "thunder") {
    scene = WeatherScene::Thunder; code = 302;
  } else if (name == "snow") {
    scene = WeatherScene::Snow; code = 400;
  } else if (name == "fog") {
    scene = WeatherScene::Fog; code = 500;
  } else if (name == "haze") {
    scene = WeatherScene::Haze; code = 502;
  } else if (name == "sand") {
    scene = WeatherScene::Sand; code = 503;
  } else if (name == "night") {
    scene = WeatherScene::Night; code = 150;
  } else {
    return false;
  }
  return true;
}

void applySerialOverride(WeatherScene scene, uint16_t code, bool hasTemperature,
                         int16_t temperature) {
  portENTER_CRITICAL(&stateMux);
  serialOverrideCode = static_cast<int16_t>(code);
  serialOverrideScene = static_cast<int8_t>(scene);
  weatherCode = code;
  if (hasTemperature) temperatureC = temperature;
  weatherPending = true;
  weatherInfoPending = true;
  portEXIT_CRITICAL(&stateMux);
  nextRandomizeAtMs = 0;
  Serial.printf("Weather test: forced %s%s\n", sceneName(scene),
                hasTemperature ? " with test temperature" : "");
}

void printSerialHelp() {
  Serial.println("Serial commands:");
  Serial.println("  W/A/S/D                 swipe up/left/down/right");
  Serial.println("  weather <name> [temp]  force scene; optional Celsius temp");
  Serial.println("    clear cloudy overcast rain thunder snow fog haze sand night");
  Serial.println("  weather auto           resume live QWeather and refresh now");
  Serial.println("  weather status         print current condition, temperature and mode");
}

void processSerialLine(char *line) {
  String command = line;
  command.trim();
  command.toLowerCase();
  if (command.isEmpty()) return;
  if (command.length() == 1) {
    lv_dir_t direction = LV_DIR_NONE;
    const char key = command[0];
    if (key == 'w') direction = LV_DIR_TOP;
    else if (key == 'a') direction = LV_DIR_LEFT;
    else if (key == 's') direction = LV_DIR_BOTTOM;
    else if (key == 'd') direction = LV_DIR_RIGHT;
    if (direction != LV_DIR_NONE) {
      const bool handled = handleSerialGesture(direction);
      Serial.printf("Serial gesture %c: %s%s\n", key - 'a' + 'A',
                    key == 'w' ? "up" : key == 'a' ? "left" :
                    key == 's' ? "down" : "right",
                    handled ? "" : " (ignored in current state)");
      return;
    }
  }
  if (command == "weather help" || command == "weather ?") {
    printSerialHelp();
    return;
  }
  if (command == "weather auto") {
    portENTER_CRITICAL(&stateMux);
    serialOverrideCode = -1;
    serialOverrideScene = -1;
    weatherLastSuccessAtMs = 0;
    temperatureC = 0;
    weatherCode = 999;
    weatherPending = true;
    weatherInfoPending = true;
    weatherRefreshRequested = true;
    portEXIT_CRITICAL(&stateMux);
    nextRandomizeAtMs = 0;
    Serial.println("Weather test: live QWeather restored; refreshing now");
    return;
  }
  if (command == "weather status") {
    int16_t value = 0;
    uint16_t code = 999;
    int16_t overrideCode = -1;
    int8_t overrideScene = -1;
    uint32_t sunDate = 0;
    uint16_t sunrise = 0;
    uint16_t sunset = 0;
    portENTER_CRITICAL(&stateMux);
    value = temperatureC;
    code = weatherCode;
    overrideCode = serialOverrideCode;
    overrideScene = serialOverrideScene;
    sunDate = solarDate;
    sunrise = sunriseMinute;
    sunset = sunsetMinute;
    portEXIT_CRITICAL(&stateMux);
    const WeatherScene scene = overrideScene >= 0
                                 ? static_cast<WeatherScene>(overrideScene)
                                 : sceneForCode(code);
    Serial.printf("Weather status: %s, %d C, %s\n", sceneName(scene), value,
                  overrideCode >= 0 ? "serial test" : "live QWeather");
    if (sunDate)
      Serial.printf("Weather sun: %08lu, sunrise %02u:%02u, sunset %02u:%02u\n",
                    static_cast<unsigned long>(sunDate), sunrise / 60U,
                    sunrise % 60U, sunset / 60U, sunset % 60U);
    else {
      weatherRefreshRequested = true;
      Serial.println("Weather sun: unavailable; refresh queued, temporarily using 06:00/18:00");
    }
    return;
  }
  if (!command.startsWith("weather ")) {
    Serial.println("Unknown serial command. Type: weather help");
    return;
  }

  String arguments = command.substring(strlen("weather "));
  arguments.trim();
  const int separator = arguments.indexOf(' ');
  const String conditionText = separator < 0
                            ? arguments : arguments.substring(0, separator);
  WeatherScene scene = WeatherScene::Overcast;
  uint16_t code = 999;
  if (!serialCondition(conditionText, scene, code)) {
    Serial.println("Weather test: unknown name. Type: weather help");
    return;
  }
  bool hasTemperature = false;
  int16_t value = 0;
  if (separator >= 0) {
    String temperatureText = arguments.substring(separator + 1);
    temperatureText.trim();
    if (!temperatureText.isEmpty()) {
      hasTemperature = true;
      value = static_cast<int16_t>(temperatureText.toInt());
    }
  }
  applySerialOverride(scene, code, hasTemperature, value);
}

void serviceSerialCommands() {
  static char line[64]{};
  static size_t length = 0;
  static uint32_t lastByteAt = 0;
  while (Serial.available() > 0) {
    const char received = static_cast<char>(Serial.read());
    if (received == '\r') continue;
    if (received == '\n') {
      line[length] = '\0';
      processSerialLine(line);
      length = 0;
      continue;
    }
    if (length + 1 < sizeof(line)) {
      line[length++] = received;
      lastByteAt = millis();
    } else {
      length = 0;
      Serial.println("Weather serial command too long; discarded");
    }
  }
  // A terminal can send a bare WASD keystroke without a line ending. Give a
  // possible multi-byte command (notably "weather ...") a short assembly
  // window, then execute a lone key without requiring Enter.
  if (length == 1 && millis() - lastByteAt >= 30) {
    const char key = line[0];
    const bool gestureKey = key == 'w' || key == 'W' || key == 'a' ||
                            key == 'A' || key == 's' || key == 'S' ||
                            key == 'd' || key == 'D';
    if (gestureKey) {
      line[1] = '\0';
      processSerialLine(line);
      length = 0;
    }
  }
}

}  // namespace WeatherWallpaper

bool handleSerialGesture(lv_dir_t direction) {
  if (direction == LV_DIR_NONE || lowPowerMode || screenFadeTransitionActive ||
      orientationTransitionBlanked || controlPanelShowRequested)
    return false;

  markUserActivity();

  // An alarm overlay owns the whole screen; its real gesture callback accepts
  // only an upward dismissal, so serial control follows the same rule.
  if (alarmAlertVisible) {
    if (direction != LV_DIR_TOP) return false;
    hideAlarmAlert();
    return true;
  }

  // Weather pages finalize gestures outside LVGL because direct-LCD video and
  // child labels can swallow gesture events. Feed the same pending direction
  // used by physical touch so page switching, edge actions and suppression are
  // shared rather than reimplemented for Serial.
  if (weatherPageVisible || weatherInfoPageVisible) {
    weatherTouchGestureDirection = direction;
    weatherTouchGestureStartY = direction == LV_DIR_BOTTOM
                                  ? 0
                                  : direction == LV_DIR_TOP
                                      ? SCREEN_HEIGHT - 1
                                      : SCREEN_HEIGHT / 2;
    processWeatherTouchGesture();
    return true;
  }

  if (controlPanelVisible) {
    if (direction != LV_DIR_TOP) return false;
    hideControlPanel(true);
    return true;
  }

  if (appMenuVisible) {
    if ((clockModeSelector &&
         !lv_obj_has_flag(clockModeSelector, LV_OBJ_FLAG_HIDDEN)) ||
        (wifiSetupPage &&
         !lv_obj_has_flag(wifiSetupPage, LV_OBJ_FLAG_HIDDEN)))
      return false;
    if (direction == LV_DIR_LEFT || direction == LV_DIR_RIGHT) {
      appMenuClickSuppressedUntil = millis() + 350;
      appMenuPageStepRequested = direction == LV_DIR_LEFT ? 1 : -1;
      return true;
    }
    if (direction == LV_DIR_BOTTOM) {
      hideAppMenu(true);
      return true;
    }
    return false;
  }

  if (musicPlaylistVisible) {
    if (direction != LV_DIR_TOP) return false;
    hideMusicPlaylist();
    return true;
  }

  if (calendarPageVisible || alarmPageVisible || musicPageVisible) {
    if (direction == LV_DIR_LEFT) {
      handleGlobalNavigationStep(1);
      return true;
    }
    if (direction == LV_DIR_RIGHT) {
      handleGlobalNavigationStep(-1);
      return true;
    }
    if (direction == LV_DIR_BOTTOM) {
      showControlPanel(true);
      return true;
    }
    if (direction == LV_DIR_TOP) {
      if (calendarPageVisible) hideCalendarPage(true, true);
      else if (alarmPageVisible) hideAlarmPage(true, true);
      else hideMusicPage(true, true);
      return true;
    }
    return false;
  }

  // Album/main screen: use edge-equivalent vertical swipes. W rises from the
  // lower edge to open apps; S descends from the upper edge to open controls.
  if (direction == LV_DIR_TOP) {
    showAppMenu(true);
    return true;
  }
  if (direction == LV_DIR_BOTTOM) {
    showControlPanel(true);
    return true;
  }
  if (direction == LV_DIR_LEFT) {
    handleGlobalNavigationStep(1);
    return true;
  }
  if (direction == LV_DIR_RIGHT) {
    handleGlobalNavigationStep(-1);
    return true;
  }
  return false;
}

void invalidateWeatherWallpaperStatus() {
  if (WeatherWallpaper::statusPanel)
    lv_obj_invalidate(WeatherWallpaper::statusPanel);
}

void createWeatherWallpaperPage(lv_obj_t *screen) {
  WeatherWallpaper::createPage(screen);
}

void createWeatherInfoPage(lv_obj_t *screen) {
  WeatherWallpaper::createInfoPage(screen);
}

bool handleWeatherInfoHorizontalGesture(lv_dir_t direction) {
  return WeatherWallpaper::handleInfoHorizontalGesture(direction);
}

void showWeatherInfoPage(bool fade) {
  if (!weatherInfoPage || lowPowerMode || weatherInfoPageVisible ||
      weatherInfoPageShowRequested) return;
  if (fade) {
    weatherInfoPageHideRequested = false;
    weatherInfoPageShowRequested = true;
    return;
  }
  weatherInfoPageVisible = true;
  WeatherWallpaper::weatherInfoPending = true;
  WeatherWallpaper::weatherTrendPending = true;
  WeatherWallpaper::setInfoSubpage(0);
  lv_obj_clear_flag(weatherInfoPage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(weatherInfoPage);
  // Keep the cached values visible while every app entry forces a fresh
  // network cycle. The worker consumes this flag within at most one second.
  WeatherWallpaper::weatherRefreshRequested = true;
}

void hideWeatherInfoPage(bool fade, bool returnToMenu) {
  if (!weatherInfoPage) return;
  if (fade) {
    weatherInfoPageShowRequested = false;
    weatherInfoPageHideRequested = true;
    return;
  }
  weatherInfoPageVisible = false;
  weatherInfoPageShowRequested = false;
  weatherInfoPageHideRequested = false;
  lv_obj_add_flag(weatherInfoPage, LV_OBJ_FLAG_HIDDEN);
  if (returnToMenu && !lowPowerMode) showAppMenu(false);
}

void processWeatherInfoPageTransition() {
  if (lowPowerMode || screenFadeTransitionActive) return;
  if (weatherInfoPageShowRequested) {
    weatherInfoPageShowRequested = false;
    beginScreenFade(activeBrightness);
    hideAppMenu(false);
    showWeatherInfoPage(false);
    lv_obj_invalidate(weatherInfoPage);
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
  } else if (weatherInfoPageHideRequested) {
    weatherInfoPageHideRequested = false;
    beginScreenFade(activeBrightness);
    hideWeatherInfoPage(false, true);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
  }
}

void serviceWeatherInfoPage() {
  WeatherWallpaper::serviceInfoPage();
}

bool mediaPageRequestedVideo = false;

void showMediaPlayerPage(bool fade) {
  if (!WeatherWallpaper::page || lowPowerMode || weatherPageVisible ||
      weatherPageShowRequested || weatherInfoPageVisible ||
      weatherInfoPageShowRequested) return;
  if (fade) {
    weatherPageHideRequested = false;
    weatherPageShowRequested = true;
    return;
  }
  WeatherWallpaper::setPlayerMode(mediaPageRequestedVideo);
  weatherVideoFrameVisible = false;
  weatherPageVisible = true;
  lv_obj_clear_flag(WeatherWallpaper::page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(WeatherWallpaper::page);
  WeatherWallpaper::setMessage(
    mediaPageRequestedVideo ? "Loading video..."
                            : "Loading weather wallpaper...");
  // Clear the previous application, then let LVGL draw the full loading page.
  // weatherVideoFrameVisible remains false until a complete decoded frame has
  // reached the LCD, so the top 360 rows are not prematurely reserved.
  lockDisplayBus();
  gfx->fillScreen(RGB565_BLACK);
  unlockDisplayBus();
  lv_obj_invalidate(WeatherWallpaper::page);
  lv_refr_now(nullptr);
  // Do not scan the TF card or allocate the decoder here: this function runs
  // between fade-out and fade-in. WeatherWallpaper::service() initializes or
  // resumes the pipeline on the next loop, after Loading is already visible.
  WeatherWallpaper::startNetworkService();
}

void showWeatherWallpaperPage(bool fade) {
  mediaPageRequestedVideo = false;
  showMediaPlayerPage(fade);
}

void showVideoPlayerPage(bool fade) {
  mediaPageRequestedVideo = true;
  showMediaPlayerPage(fade);
}

void hideWeatherWallpaperPage(bool fade, bool returnToMenu) {
  if (!WeatherWallpaper::page) return;
  if (fade) {
    weatherPageShowRequested = false;
    weatherPageHideRequested = true;
    return;
  }
  weatherPageVisible = false;
  weatherVideoFrameVisible = false;
  weatherPageShowRequested = false;
  weatherPageHideRequested = false;
  WeatherWallpaper::videoPlaybackUserPaused = false;
  // Stopping the direct-LCD workers is required before revealing another app,
  // but a normal app exit does not need a routine timing message in Serial.
  WeatherWallpaper::pausePipeline(false);
  lv_obj_add_flag(WeatherWallpaper::page, LV_OBJ_FLAG_HIDDEN);
  if (returnToMenu && !lowPowerMode) showAppMenu(false);
}

void processWeatherWallpaperTransition() {
  if (lowPowerMode || screenFadeTransitionActive) return;
  if (weatherPageShowRequested) {
    weatherPageShowRequested = false;
    beginScreenFade(activeBrightness);
    hideAppMenu(false);
    showMediaPlayerPage(false);
    endScreenFade(activeBrightness);
  } else if (weatherPageHideRequested) {
    weatherPageHideRequested = false;
    beginScreenFade(activeBrightness);
    hideWeatherWallpaperPage(false, true);
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
  }
}

void serviceWeatherWallpaper() {
  WeatherWallpaper::service();
}

void serviceWeatherSerialCommands() {
  WeatherWallpaper::serviceSerialCommands();
}

void startWeatherNetworkService() {
  WeatherWallpaper::startNetworkService();
}

void selectAdjacentWeatherWallpaper(int8_t direction) {
  if (WeatherWallpaper::playerVideoMode &&
      WeatherWallpaper::videoPlaybackUserPaused) {
    WeatherWallpaper::videoPlaybackUserPaused = false;
    if (!WeatherWallpaper::resumePipeline()) {
      WeatherWallpaper::videoPlaybackUserPaused = true;
      Serial.println("Video key: cannot resume before changing video");
      return;
    }
  }
  WeatherWallpaper::requestStep(direction);
}

bool toggleVideoPlayerPlayback() {
  if (!weatherPageVisible || !WeatherWallpaper::initialized ||
      !WeatherWallpaper::playerVideoMode || lowPowerMode) return false;
  markUserActivity();
  if (WeatherWallpaper::videoPlaybackUserPaused) {
    if (!WeatherWallpaper::resumePipeline()) {
      Serial.println("Video key: play failed");
      return true;
    }
    WeatherWallpaper::videoPlaybackUserPaused = false;
    WeatherWallpaper::setMessage("");
    Serial.printf("Video key: play from frame %lu\n",
                  static_cast<unsigned long>(
                    WeatherWallpaper::requestedReaderFrame + 1U));
  } else {
    WeatherWallpaper::videoPlaybackUserPaused = true;
    const bool paused = WeatherWallpaper::pausePipeline();
    WeatherWallpaper::setMessage("Paused", lv_color_hex(0xF7F8FA));
    Serial.printf("Video key: paused at frame %lu (%s)\n",
                  static_cast<unsigned long>(
                    WeatherWallpaper::requestedReaderFrame + 1U),
                  paused ? "clean" : "worker timeout");
  }
  return true;
}

bool suspendWeatherWallpaperForLowPower() {
  hideWeatherWallpaperPage(false, false);
  return WeatherWallpaper::closeForLowPower();
}

void pauseWeatherWallpaperForControlPanel() {
  if (!weatherPageVisible || !WeatherWallpaper::initialized) return;
  WeatherWallpaper::pausePipeline();
}

void resumeWeatherWallpaperAfterControlPanel() {
  if (!weatherPageVisible || !WeatherWallpaper::initialized || lowPowerMode) return;
  if (WeatherWallpaper::playerVideoMode &&
      WeatherWallpaper::videoPlaybackUserPaused) return;
  WeatherWallpaper::resumePipeline();
}

uint32_t weatherWallpaperDisplayedSequence() {
  return WeatherWallpaper::displayedFrameSequence;
}

bool weatherWallpaperPlaybackReady() {
  return WeatherWallpaper::initialized;
}

bool weatherWallpaperSdReadyForOtherUsers() {
  return !WeatherWallpaper::initialized ||
         (WeatherWallpaper::pipelinePauseRequested &&
          WeatherWallpaper::readerIdle && WeatherWallpaper::decoderIdle &&
          WeatherWallpaper::displayIdle);
}

bool videoPlayerModeActive() {
  return WeatherWallpaper::playerVideoMode;
}

bool videoPlayerSeekGestureSuppressed() {
  return WeatherWallpaper::seekGestureSuppressed();
}

void refreshWeatherConfiguration() {
  WeatherWallpaper::requestWeatherRefresh();
}

void queueWeatherRefreshKeepingCache() {
  // Wake refreshes should not blank a still-valid temperature and forecast
  // while Wi-Fi is reconnecting. The network task resets its schedule when it
  // consumes this flag, then atomically replaces the cached values on success.
  WeatherWallpaper::weatherRefreshRequested = true;
}

void notifyWeatherWallpaperOrientationChanged() {
  WeatherWallpaper::orientationChanged();
}
