#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <mbedtls/ssl.h>
#include "esp_sntp.h"
#include <AudioFileSourceSD.h>
#include <AudioFileSourceHTTPStream.h>
#include <AudioFileSourceICYStream.h>
#include <AudioFileSourceBuffer.h>
#include <AudioGenerator.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorAAC.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESP_New_JPEG_Library.h>
#include <miniz.h>
#include "es8311.h"

#include <lvgl.h>
#include "lv_conf.h"
#include "src/clock_fonts.h"
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include "TouchDrvCSTXXX.hpp"
#include "SensorQMI8658.hpp"
#include "SensorPCF85063.hpp"
#include "XPowersLib.h"
#include "web_page.h"
#include "tabler_control_icons.h"
#include "weather_tabler_icons.h"
#include "clock_weather_config.h"

namespace {

enum ClockFontId : uint8_t {
  CLOCK_FONT_MONTSERRAT = 0,
  CLOCK_FONT_KAMERON,
  CLOCK_FONT_SILKSCREEN,
  CLOCK_FONT_DOTGOTHIC16,
  CLOCK_FONT_ORBITRON,
  CLOCK_FONT_AUDIOWIDE,
  CLOCK_FONT_CINZEL,
  CLOCK_FONT_BEBASNEUE,
  CLOCK_FONT_COUNT
};

constexpr uint16_t SCREEN_WIDTH = LCD_WIDTH;
constexpr uint16_t SCREEN_HEIGHT = LCD_HEIGHT;
constexpr uint16_t WEATHER_VIDEO_HEIGHT = 360;
constexpr size_t IMAGE_BYTES = static_cast<size_t>(SCREEN_WIDTH) * SCREEN_HEIGHT * 2;
// Keep the CO5300 QSPI clock identical to the standalone 09 player. Calling
// gfx->begin() without an argument falls back to the library's 40 MHz default.
constexpr uint32_t DISPLAY_BUS_FREQUENCY = 80000000UL;
constexpr uint32_t LVGL_TICK_MS = 2;
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint8_t DEFAULT_ACTIVE_BRIGHTNESS = 100;
constexpr uint8_t DEFAULT_AUDIO_VOLUME = 25;
constexpr uint8_t MIN_ACTIVE_BRIGHTNESS = 30;
constexpr uint8_t NIGHT_SLEEP_BRIGHTNESS = 25;
constexpr uint8_t DAY_SLEEP_BRIGHTNESS = 42;
constexpr uint16_t DEFAULT_NIGHT_START_MINUTE = 0;
constexpr uint16_t DEFAULT_NIGHT_END_MINUTE = 8 * 60;
constexpr uint16_t DEFAULT_IDLE_SLEEP_MINUTES = 15;
constexpr uint16_t MIN_IDLE_SLEEP_MINUTES = 1;
constexpr uint16_t MAX_IDLE_SLEEP_MINUTES = 24 * 60;
constexpr uint16_t MIN_BACKGROUND_SWITCH_MINUTES = 1;
constexpr uint16_t MAX_BACKGROUND_SWITCH_MINUTES = 24 * 60;
constexpr uint16_t DEFAULT_ALBUM_SWITCH_MINUTES = 30;
constexpr uint16_t DEFAULT_WEATHER_SWITCH_MINUTES = 5;
constexpr uint8_t DEFAULT_ALBUM_IMAGE_LIMIT = 20;
// One restrained accent family is shared by tiles, controls and weather icons.
// Neutral black/white/grey surfaces intentionally remain unchanged.
constexpr uint32_t UI_ACCENT_BLUE = 0x2563EB;
constexpr uint32_t UI_ACCENT_MINT = 0x0F9F82;
constexpr uint32_t UI_ACCENT_AMBER = 0xF59E0B;
constexpr uint32_t UI_ACCENT_ROSE = 0xDC3F67;
constexpr uint32_t UI_ACCENT_VIOLET = 0x7C3AED;
constexpr uint32_t UI_ACCENT_BILIBILI = 0xFB7299;
constexpr uint32_t LOW_POWER_CLOCK_POLL_SECONDS = 60;
constexpr uint8_t ORIENTATION_SAMPLE_HZ = 3;
constexpr uint32_t ORIENTATION_CHECK_MS = 1000 / ORIENTATION_SAMPLE_HZ;
// Hardware-brightness fades hide full-frame replacement without asking LVGL
// to blend two 480x480 buffers. Rotation uses these steps asynchronously;
// synchronous program/image transitions continue servicing audio meanwhile.
constexpr uint8_t ROTATION_FADE_FRAMES = 5;
constexpr uint32_t ROTATION_FADE_FRAME_MS = 10;
constexpr uint32_t ROTATION_BLACK_HOLD_MS = 6;
constexpr float ORIENTATION_MIN_GRAVITY = 0.55f;
constexpr float ORIENTATION_AXIS_MARGIN = 0.07f;
constexpr uint32_t NTP_RETRY_MS = 8000;
constexpr uint32_t NTP_TEMPORARY_WIFI_TIMEOUT_MS = 45000;
constexpr int16_t TIME_PANEL_MIN_WIDTH = 132;
constexpr int16_t TIME_PANEL_MAX_WIDTH = 240;
constexpr int16_t TIME_PANEL_HEIGHT = 84;
constexpr int16_t TIME_PANEL_RADIUS = 36;
constexpr int16_t TIME_PANEL_HORIZONTAL_PADDING = 20;
constexpr float TIME_PANEL_FEATHER = 28.0f;
constexpr size_t TIME_PANEL_BUFFER_BYTES =
  LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(TIME_PANEL_MAX_WIDTH, TIME_PANEL_HEIGHT);
// The 0xBF0000 LittleFS partition can safely hold twenty 480x480 RGB565
// frames plus the upload/rollback files used while replacing an image.
constexpr uint8_t IMAGE_SLOT_COUNT = 20;
constexpr uint8_t IMAGE_PREVIOUS_KEY_PIN = 0;
constexpr uint8_t IMAGE_NEXT_KEY_PIN = 18;
constexpr uint32_t IMAGE_KEY_DEBOUNCE_MS = 35;
constexpr char LEGACY_IMAGE_PATH[] = "/clock.rgb565";
constexpr char IMAGE_TEMP_PATH[] = "/upload.tmp";
constexpr char IMAGE_BACKUP_PATH[] = "/image.prev";
constexpr const char *IMAGE_SLOT_PATHS[IMAGE_SLOT_COUNT] = {
  "/clock0.rgb565", "/clock1.rgb565", "/clock2.rgb565",
  "/clock3.rgb565", "/clock4.rgb565", "/clock5.rgb565",
  "/clock6.rgb565", "/clock7.rgb565", "/clock8.rgb565",
  "/clock9.rgb565", "/clock10.rgb565", "/clock11.rgb565",
  "/clock12.rgb565", "/clock13.rgb565", "/clock14.rgb565",
  "/clock15.rgb565", "/clock16.rgb565", "/clock17.rgb565",
  "/clock18.rgb565", "/clock19.rgb565"
};
constexpr uint8_t ES8311_I2C_ADDRESS = 0x18;
constexpr uint8_t ES8311_DAC_VOLUME_REGISTER = 0x32;
// ES8311 DAC volume is 0.5 dB/step: 0xBF is 0 dB and values above it
// add digital gain. Keep the UI range below 0 dB to prevent clipping.
constexpr uint8_t ES8311_DAC_MIN_AUDIBLE_REGISTER = 0x5B;  // -50 dB
constexpr uint8_t ES8311_DAC_ZERO_DB_REGISTER = 0xBF;
// The tiny onboard speaker/power-amplifier path distorts on full-scale peaks
// before the codec itself reaches digital clipping. Keep 5 dB of headroom;
// the UI can still show 100%, but 100% now means the board's clean maximum.
constexpr uint8_t ES8311_DAC_SAFE_MAX_REGISTER = 0xB5;  // -5 dB
constexpr uint8_t SD_MOSI_PIN = 1;
constexpr uint8_t SD_SCLK_PIN = 2;
constexpr uint8_t SD_MISO_PIN = 3;
constexpr uint8_t SD_CS_PIN = 41;
constexpr uint8_t I2S_MCLK_PIN = 42;
constexpr uint8_t I2S_BCLK_PIN = 9;
constexpr uint8_t I2S_WS_PIN = 45;
constexpr uint8_t I2S_DOUT_PIN = 8;
constexpr uint8_t AUDIO_PA_PIN = 46;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *gfx = new Arduino_CO5300(
  bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);

TouchDrvCST92xx touch;
SensorQMI8658 qmi;
SensorPCF85063 rtc;
XPowersPMU power;
WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

lv_disp_draw_buf_t drawBuffer;
lv_color_t *drawBufferMemory = nullptr;
lv_color_t *rotationBufferMemory = nullptr;
bool manualRotationBufferReady = false;
lv_obj_t *backgroundImage = nullptr;
lv_obj_t *shadeObject = nullptr;
lv_obj_t *timePanel = nullptr;
lv_obj_t *timeBackground = nullptr;
lv_obj_t *timeLabel = nullptr;
lv_obj_t *dateLabel = nullptr;
lv_obj_t *statusLabel = nullptr;
lv_obj_t *addressLabel = nullptr;
lv_obj_t *settingsButton = nullptr;
lv_obj_t *controlPanel = nullptr;
lv_obj_t *appMenu = nullptr;
constexpr uint8_t APP_MENU_PAGE_COUNT = 4;
lv_obj_t *appMenuPages[APP_MENU_PAGE_COUNT] = {};
lv_obj_t *appMenuPageDots[APP_MENU_PAGE_COUNT] = {};
lv_obj_t *wifiSetupPage = nullptr;
lv_obj_t *wifiSetupSsidLabel = nullptr;
lv_obj_t *wifiSetupPasswordLabel = nullptr;
lv_obj_t *calendarPage = nullptr;
lv_obj_t *calendarWidget = nullptr;
lv_obj_t *weatherInfoPage = nullptr;
lv_obj_t *alarmPage = nullptr;
lv_obj_t *alarmHourLabel = nullptr;
lv_obj_t *alarmMinuteLabel = nullptr;
lv_obj_t *alarmToggleButton = nullptr;
lv_obj_t *alarmSlotButtons[3] = {};
lv_obj_t *alarmAlertOverlay = nullptr;
lv_obj_t *alarmAlertTimeLabel = nullptr;
lv_obj_t *alarmAlertSlotLabel = nullptr;
lv_obj_t *musicPage = nullptr;
lv_obj_t *musicTrackLabel = nullptr;
lv_obj_t *musicStatusLabel = nullptr;
lv_obj_t *musicCountLabel = nullptr;
lv_obj_t *musicPlayIcon = nullptr;
lv_obj_t *musicPauseIcon = nullptr;
lv_obj_t *musicPlaylistPanel = nullptr;
lv_obj_t *musicPlaylistList = nullptr;
lv_obj_t *audiobookPage = nullptr;
lv_obj_t *audiobookTitleLabel = nullptr;
lv_obj_t *audiobookStatusLabel = nullptr;
lv_obj_t *audiobookCountLabel = nullptr;
lv_obj_t *audiobookTimeLabel = nullptr;
lv_obj_t *audiobookPlayIcon = nullptr;
lv_obj_t *audiobookPauseIcon = nullptr;
lv_obj_t *audiobookBooklistPanel = nullptr;
lv_obj_t *audiobookBooklistList = nullptr;
lv_obj_t *audiobookProgressBar = nullptr;
lv_obj_t *radioPage = nullptr;
lv_obj_t *radioStationLabel = nullptr;
lv_obj_t *radioNowPlayingLabel = nullptr;
lv_obj_t *radioTitleLabel = nullptr;
lv_obj_t *radioStatusLabel = nullptr;
lv_obj_t *radioPlayIcon = nullptr;
lv_obj_t *radioPauseIcon = nullptr;
lv_obj_t *radioStationListPanel = nullptr;
lv_obj_t *radioStationListList = nullptr;
lv_obj_t *radioHintLabel = nullptr;
lv_obj_t *languageControlButton = nullptr;
lv_obj_t *languageControlLabel = nullptr;
lv_obj_t *themeControlButton = nullptr;
lv_obj_t *themeControlLabel = nullptr;
lv_obj_t *colorControlButton = nullptr;
lv_obj_t *colorControlLabel = nullptr;
lv_obj_t *wifiControlButton = nullptr;
lv_obj_t *bluetoothControlButton = nullptr;
lv_obj_t *wifiControlStatus = nullptr;
lv_obj_t *wifiControlIp = nullptr;
lv_obj_t *bluetoothControlStatus = nullptr;
lv_obj_t *wifiControlIcon = nullptr;
lv_obj_t *bluetoothControlIcon = nullptr;
lv_obj_t *brightnessSlider = nullptr;
lv_obj_t *volumeSlider = nullptr;
lv_obj_t *batteryStatusIcon = nullptr;
lv_obj_t *batteryStatusLabel = nullptr;

lv_img_dsc_t backgroundDescriptor{};
uint8_t *backgroundPixels = nullptr;
uint8_t *timePanelPixels = nullptr;
int16_t timePanelWidth = TIME_PANEL_MIN_WIDTH;

String savedSsid;
String savedPassword;
String timezoneRule = "CST-8";
String weatherApiHost = CLOCK_QWEATHER_API_HOST;
String weatherApiKey = CLOCK_QWEATHER_API_KEY;
String weatherLocation = CLOCK_QWEATHER_LOCATION;
bool weatherAutoLocationEnabled = true;
char apSsid[32] = {};

bool webServerStarted = false;
bool portalActive = false;
bool ntpStarted = false;
bool temporaryNtpWifi = false;
bool timeSynced = false;
bool rtcReady = false;
bool rtcTimeValid = false;
bool rtcCalibratedThisBoot = false;
uint8_t rtcReadFailureCount = 0;
char displayedTimeText[6] = {};
volatile bool ntpSyncPending = false;
uint32_t ntpStartedAt = 0;
uint8_t ntpAttempt = 0;
uint32_t temporaryNtpWifiStartedAt = 0;
bool pmuReady = false;
bool lowPowerMode = false;
// Set once the Power tile has been confirmed. Blocks re-entry into the UI,
// standby and the main loop while the AXP2101 shutdown sequence runs.
bool deviceShuttingDown = false;
bool imageReloadRequested = false;
bool imageClearRequested = false;
uint8_t imageCount = 0;
uint8_t imageHeadSlot = 0;
uint8_t imageCurrentSlot = 0;
bool portalRequested = false;
bool uploadSucceeded = false;
bool touchReady = false;
bool touchPresent = false;
bool qmiReady = false;
bool orientationLocked = false;
volatile bool touchPending = false;
bool touchCurrentlyPressed = false;
// After light-sleep wake the CST9217 can finish resetting without producing a
// new falling edge. Poll only until a real interrupt proves the edge path is
// healthy again; normal operation remains interrupt-driven.
volatile bool touchWakePollingFallback = false;
int16_t gestureStartY = -1;
volatile uint8_t weatherTouchGestureDirection = LV_DIR_NONE;
volatile int16_t weatherTouchGestureStartY = -1;
volatile bool controlPanelVisible = false;
volatile bool controlPanelShowRequested = false;
bool controlPanelHideRequested = false;
bool controlPanelContentRefreshRequested = false;
bool wifiControlToggleRequested = false;
bool brightnessSaveRequested = false;
bool volumeSaveRequested = false;
bool brightnessApplyRequested = false;
bool volumeApplyRequested = false;
bool wifiControlTargetEnabled = false;
bool wifiControlConnectPending = false;
bool appMenuVisible = false;
bool appMenuShowRequested = false;
bool appMenuHideRequested = false;
uint8_t appMenuPageIndex = 0;
int8_t appMenuPageStepRequested = 0;
bool calendarPageVisible = false;
bool calendarPageShowRequested = false;
bool calendarPageHideRequested = false;
bool weatherInfoPageVisible = false;
bool weatherInfoPageShowRequested = false;
bool weatherInfoPageHideRequested = false;
bool alarmPageVisible = false;
bool alarmPageShowRequested = false;
bool alarmPageHideRequested = false;
bool alarmAlertVisible = false;
bool musicPageVisible = false;
bool musicPageShowRequested = false;
bool musicPageHideRequested = false;
bool musicPlaylistVisible = false;
bool audiobookPageVisible = false;
bool audiobookPageShowRequested = false;
bool audiobookPageHideRequested = false;
bool audiobookBooklistVisible = false;
bool radioPageVisible = false;
bool worldClockPageVisible = false;
bool photoFramePageVisible = false;
bool timerPageVisible = false;
bool timerRunning = false;
bool timerAlertVisible = false;
uint32_t timerEndsAt = 0;
uint32_t timerRemainingMs = 0;
bool radioPageShowRequested = false;
bool radioPageHideRequested = false;
bool radioStationListVisible = false;
bool albumAutoSwitchEnabled = false;
bool weatherAutoSwitchEnabled = true;
bool weatherWallpaperFollowEnabled = true;
bool weatherPageVisible = false;
volatile bool weatherVideoFrameVisible = false;
bool weatherPageShowRequested = false;
bool weatherPageHideRequested = false;
int8_t calendarMonthStepRequested = 0;
bool screenFadeTransitionActive = false;
bool fullScreenDrawBufferReady = false;
bool musicWholeScreenRefreshEnabled = false;
bool wifiUserEnabled = true;
bool audioCodecReachable = false;
uint8_t activeBrightness = DEFAULT_ACTIVE_BRIGHTNESS;
uint8_t audioVolume = DEFAULT_AUDIO_VOLUME;
uint8_t currentSleepBrightness = DAY_SLEEP_BRIGHTNESS;
uint8_t alarmHours[3] = {7, 8, 9};
uint8_t alarmMinutes[3] = {0, 0, 0};
bool alarmEnabled[3] = {false, false, false};
uint8_t selectedAlarmSlot = 0;
uint16_t nightStartMinute = DEFAULT_NIGHT_START_MINUTE;
uint16_t nightEndMinute = DEFAULT_NIGHT_END_MINUTE;
uint16_t idleSleepMinutes = DEFAULT_IDLE_SLEEP_MINUTES;
bool idleSleepEnabled = true;
bool sleepTimeDisplayEnabled = true;
uint16_t albumSwitchMinutes = DEFAULT_ALBUM_SWITCH_MINUTES;
uint8_t albumImageLimit = DEFAULT_ALBUM_IMAGE_LIMIT;
uint16_t weatherSwitchMinutes = DEFAULT_WEATHER_SWITCH_MINUTES;
uint8_t clockFontIndex = CLOCK_FONT_MONTSERRAT;
uint8_t pendingClockFontIndex = CLOCK_FONT_MONTSERRAT;
bool clockFontChangeRequested = false;
// Clock digit color preset. 0 follows the active OS theme; the other entries
// are fixed colors the user can pick from the web UI or the control panel.
uint8_t clockColorIndex = 0;
bool clockColorChangeRequested = false;
// UI language (0 = English, 1 = French) and theme (0 = AMOLED black,
// 1 = CRT amber). Named constants live in clock_i18n.inc / clock_theme.inc.
uint8_t uiLanguage = 0;
uint8_t uiTheme = 0;
bool uiAppearanceChangeRequested = false;
String uploadError;
File uploadFile;
size_t uploadBytes = 0;
uint32_t rebootAt = 0;
uint32_t lastClockUpdate = 0;
uint32_t lastWifiCheck = 0;
uint32_t lastPowerKeyCheck = 0;
uint32_t lastOrientationCheck = 0;
uint32_t lowPowerLastTick = 0;
uint32_t lowPowerNextClockPollAt = 0;
uint32_t lastUserActivityAt = 0;
uint32_t lastAlarmCheck = 0;
uint32_t nextAlbumAutoSwitchAt = 0;
int8_t albumImageStepRequested = 0;
uint32_t controlPanelActionDueAt = 0;
uint32_t wifiControlConnectStartedAt = 0;
uint32_t alarmSaveDueAt = 0;
uint8_t alarmSavePendingMask = 0;
uint32_t lastAlarmTriggerStamp[3] = {0xFFFFFFFFUL, 0xFFFFFFFFUL, 0xFFFFFFFFUL};
uint8_t lowPowerDisplayedMinute = 0xFF;
lv_disp_rot_t displayRotation = LV_DISP_ROT_NONE;
// The CO5300 can flip both axes in hardware but cannot transpose X/Y. Keep
// the requested physical orientation separate from the only software
// transform still needed (90 degrees).
lv_disp_rot_t displaySoftwareRotation = LV_DISP_ROT_NONE;
esp_timer_handle_t lvglTimer = nullptr;
SemaphoreHandle_t displayBusMutex = nullptr;

bool connectSavedWifi(bool ntpOnly = false);
void startTemporaryNtpWifi();
void stopTemporaryNtpWifi();
void startConfigPortal();
void stopConfigPortal();
void startWebServer();
void loadSettings();
void createControlPanel();
void hideControlPanel(bool animated = true);
void updateControlPanelStatus();
// Declared here because clock_theme.inc (included before clock_ui.inc) calls it
// from applyUiAppearance().
void updateNetworkLabels();
void syncControlPanelSettings();
void processControlPanelTransition();
void processControlPanelActions();
void createAppMenu();
void showAppMenu(bool fade = true);
void hideAppMenu(bool fade = true);
void updateWifiSetupPage();
void updateCalendarToday();
void processAppMenuTransition();
void createAlarmPage(lv_obj_t *screen);
void showAlarmPage(bool fade = true);
void hideAlarmPage(bool fade = true, bool returnToMenu = true);
void processAlarmPageTransition();
void syncAlarmUi();
void selectAlarmSlotRelative(int8_t direction);
void serviceAlarms();
void serviceAlarmSettingsSave(bool force = false);
int8_t matchingAlarmIndex(const RTC_DateTime &current);
void showAlarmAlert(uint8_t index, const RTC_DateTime &current);
void hideAlarmAlert();
void startAlarmSound();
void stopAlarmSound();
void serviceAlarmSound();
void showCalendarPage(bool fade = true);
void hideCalendarPage(bool fade = true, bool returnToMenu = true);
void requestCalendarMonthStep(int8_t direction);
void processCalendarPageActions();
void createWeatherInfoPage(lv_obj_t *screen);
void showWeatherInfoPage(bool fade = true);
void hideWeatherInfoPage(bool fade = true, bool returnToMenu = true);
void processWeatherInfoPageTransition();
void serviceWeatherInfoPage();
bool handleWeatherInfoHorizontalGesture(lv_dir_t direction);
void createMusicPage(lv_obj_t *screen);
void showMusicPage(bool fade = true);
void hideMusicPage(bool fade = true, bool returnToMenu = true);
void processMusicPageTransition();
void serviceMusicPlayback();
void selectAdjacentMusicTrack(int8_t direction);
void hideMusicPlaylist();
void suspendMusicForLowPower(bool sdSafeToUnmount = true);
bool pauseMusicForControlPanel();
void resumeMusicAfterControlPanel(bool wasPlaying);
void createAudiobookPage(lv_obj_t *screen);
void showAudiobookPage(bool fade = true);
void hideAudiobookPage(bool fade = true, bool returnToMenu = true);
void processAudiobookPageTransition();
void serviceAudiobookPlayback();
void selectAdjacentAudiobook(int8_t direction);
void hideAudiobookBooklist();
void suspendAudiobookForLowPower();
bool pauseAudiobookForControlPanel();
void resumeAudiobookAfterControlPanel(bool wasPlaying);
void stopAudiobookPlayback();
void updateAudiobookPageText();
void createRadioPage(lv_obj_t *screen);
void showRadioPage(bool fade = true);
void hideRadioPage(bool fade = true, bool returnToMenu = true);
void processRadioPageTransition();
void serviceRadioPlayback();
void selectAdjacentRadioStation(int8_t direction);
void hideRadioStationList();
void suspendRadioForLowPower();
bool pauseRadioForControlPanel();
void resumeRadioAfterControlPanel(bool wasPlaying);
void stopRadioPlayback();
void updateRadioPageText();
void createWorldClockPage(lv_obj_t *screen);
void showWorldClockPage(bool fade = true);
void hideWorldClockPage(bool fade = true, bool returnToMenu = true);
void updateWorldClockPage();
void serviceWorldClock();
void refreshWorldClockTheme();
void selectAdjacentWorldCity(int8_t direction);
void hideWorldCityList();
void createPhotoFramePage(lv_obj_t *screen);
void showPhotoFramePage(bool fade = true);
void hidePhotoFramePage(bool fade = true, bool returnToMenu = true);
void servicePhotoFrame();
void refreshPhotoFrameTheme();
void suspendPhotoFrameForLowPower();
void selectAdjacentPhoto(int8_t direction);
void createTimerPage(lv_obj_t *screen);
void showTimerPage(bool fade = true);
void hideTimerPage(bool fade = true, bool returnToMenu = true);
void serviceTimer();
void updateTimerPageText();
void refreshTimerTheme();
void suspendTimerForLowPower();
void adjustTimerMinutes(int8_t direction);
void finishTimer();
void dismissTimerAlert();
void updateMusicPageText();
void initUiFonts();
void applyUiAppearance();
void requestUiAppearanceChange();
void setUiLanguage(uint8_t language, bool persist);
void setUiTheme(uint8_t theme, bool persist);
void createWeatherWallpaperPage(lv_obj_t *screen);
void showWeatherWallpaperPage(bool fade = true);
void showVideoPlayerPage(bool fade = true);
void hideWeatherWallpaperPage(bool fade = true, bool returnToMenu = true);
void processWeatherWallpaperTransition();
void serviceWeatherWallpaper();
void serviceWeatherSerialCommands();
void startWeatherNetworkService();
void selectAdjacentWeatherWallpaper(int8_t direction);
bool suspendWeatherWallpaperForLowPower();
void pauseWeatherWallpaperForControlPanel();
void resumeWeatherWallpaperAfterControlPanel();
bool weatherWallpaperSdReadyForOtherUsers();
bool videoPlayerModeActive();
bool videoPlayerSeekGestureSuppressed();
void refreshWeatherConfiguration();
void notifyWeatherWallpaperOrientationChanged();
void invalidateWeatherWallpaperStatus();
void handleGlobalNavigationStep(int8_t direction);
void processWeatherTouchGesture();
bool handleSerialGesture(lv_dir_t direction);
void markUserActivity();
void checkAutoSleep();
uint8_t sleepBrightnessForCurrentTime();
void enterLowPowerClockMode();
bool requestDevicePowerOff();
const lv_font_t *currentClockFont();
void applyClockFont();
void applyClockColor();
void setClockColor(uint8_t color, bool persist);
void beginScreenFade(uint8_t fromBrightness);
void endScreenFade(uint8_t toBrightness);
const char *currentImagePath();
void initializeImageHistory();
void initializeImageKeys();
void suspendImageKeys();
void pollImageKeys();
void clearImageHistory();
bool showCurrentImage(bool fade);
void selectAdjacentImage(int8_t direction);
void processAlbumImageStep();
void serviceAlbumAutoSwitch();


#include "clock_tabler_icons.inc"
#include "clock_i18n.inc"
#include "clock_theme.inc"
#include "clock_hardware.inc"
#include "clock_ui.inc"
#include "clock_time.inc"
#include "clock_control_panel.inc"
#include "clock_app_menu.inc"
#include "clock_alarm.inc"
#include "clock_music.inc"
#include "clock_audiobook.inc"
#include "clock_radio.inc"
#include "clock_world.inc"
#include "clock_photo.inc"
#include "clock_timer.inc"
#include "clock_weather.inc"
#include "clock_images.inc"
#include "clock_power.inc"
#include "clock_web.inc"

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("AMOLED LVGL Clock starting");

  if (!initDisplayAndLvgl()) {
    while (true) delay(1000);
  }
  Serial.println("[4/5] LittleFS mounting");
  if (!LittleFS.begin(true)) {
    lv_label_set_text(statusLabel, "LittleFS mount failed");
    while (true) {
      lv_timer_handler();
      delay(10);
    }
  }
  Serial.println("LittleFS ready");
  loadSettings();
  initializeImageHistory();
  initializeImageKeys();
  loadBackgroundImage();
  Serial.println("[5/5] Network services");
  Serial.println("BLE disabled to preserve weather TLS memory");

  if (wifiUserEnabled) {
    if (!connectSavedWifi()) startConfigPortal();
    // ESP32 Arduino 3.x requires the network interface to exist before
    // NetworkServer/WebServer creates its internal synchronization objects.
    startWebServer();
  } else {
    startTemporaryNtpWifi();
  }
  // Start QWeather as part of boot, before the wallpaper player allocates its
  // JPEG/DMA resources. The first successful connection triggers an immediate
  // request, so opening weather mode does not have to start from stale data.
  startWeatherNetworkService();
  updateNetworkLabels();
  updateControlPanelStatus();
  updateClock();
  // Cold boot opens the launcher instead of the clock/album page. Always
  // start on page one even if a later session navigated to page two.
  appMenuPageIndex = 0;
  updateAppMenuPageVisibility();
  showAppMenu(false);
  markUserActivity();
  Serial.printf("Setup complete; free internal heap: %u bytes\n",
                static_cast<unsigned int>(
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
}

void loop() {
  if (deviceShuttingDown) {
    delay(100);
    return;
  }
  pollPowerKey();
  serviceWeatherSerialCommands();
  if (lowPowerMode) {
    runLowPowerClock();
    return;
  }

  // Keep the alarm DMA queue full before wake restoration, LVGL or network
  // work can occupy the main loop.
  serviceAlarmSound();

  processControlPanelTransition();
  processAppMenuTransition();
  processAlarmPageTransition();
  processMusicPageTransition();
  processAudiobookPageTransition();
  processRadioPageTransition();
  processWeatherWallpaperTransition();
  processWeatherInfoPageTransition();
  processCalendarPageActions();
  updateScreenOrientation();
  pollImageKeys();
  serviceAlbumAutoSwitch();
  processAlbumImageStep();
  serviceMusicPlayback();
  serviceAudiobookPlayback();
  serviceRadioPlayback();
  serviceWeatherWallpaper();
  serviceWeatherInfoPage();
  serviceAlarms();
  serviceWorldClock();
  servicePhotoFrame();
  serviceTimer();

  if (webServerStarted && wifiUserEnabled) server.handleClient();
  if (portalActive) dnsServer.processNextRequest();
  updateRtcFromNtp();

  if (portalRequested) {
    portalRequested = false;
    startConfigPortal();
  }
  if (imageReloadRequested && !screenFadeTransitionActive) {
    imageReloadRequested = false;
    showCurrentImage(true);
  }
  if (imageClearRequested && !screenFadeTransitionActive) {
    imageClearRequested = false;
    beginScreenFade(activeBrightness);
    clearImageHistory();
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
  }
  if (clockFontChangeRequested && !screenFadeTransitionActive) {
    clockFontChangeRequested = false;
    if (pendingClockFontIndex < CLOCK_FONT_COUNT &&
        pendingClockFontIndex != clockFontIndex) {
      beginScreenFade(activeBrightness);
      clockFontIndex = pendingClockFontIndex;
      preferences.putUChar("clock_font", clockFontIndex);
      applyClockFont();
      lv_obj_invalidate(timePanel);
      lv_refr_now(nullptr);
      endScreenFade(activeBrightness);
    }
  }
  if (uiAppearanceChangeRequested && !screenFadeTransitionActive) {
    uiAppearanceChangeRequested = false;
    beginScreenFade(activeBrightness);
    applyUiAppearance();
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
    endScreenFade(activeBrightness);
  }

  if (millis() - lastClockUpdate >= 1000) {
    lastClockUpdate = millis();
    updateClock();
  }
  if (millis() - lastWifiCheck >= 5000) {
    lastWifiCheck = millis();
    if (wifiUserEnabled && WiFi.status() == WL_CONNECTED) startNtp();
    updateNetworkLabels();
    updateControlPanelStatus();
  }
  if (rebootAt && static_cast<int32_t>(millis() - rebootAt) >= 0) {
    ESP.restart();
  }

  processControlPanelTransition();
  processAppMenuTransition();
  processAlarmPageTransition();
  processMusicPageTransition();
  processAudiobookPageTransition();
  processRadioPageTransition();
  processWeatherWallpaperTransition();
  processWeatherInfoPageTransition();
  serviceMusicPlayback();
  serviceAudiobookPlayback();
  serviceRadioPlayback();
  // The standalone 09 player does not run LVGL for every decoded frame. Keep
  // its 10 ms UI cadence in weather mode so the main loop can immediately
  // begin the next JPEG; all other applications retain the original cadence.
  static uint32_t lastWeatherLvglServiceMs = 0;
  const uint32_t lvglNowMs = millis();
  if (!weatherPageVisible ||
      lvglNowMs - lastWeatherLvglServiceMs >= 10) {
    lastWeatherLvglServiceMs = lvglNowMs;
    lv_timer_handler();
  }
  processWeatherTouchGesture();
  processControlPanelActions();
  serviceAlarmSettingsSave();
  serviceMusicPlayback();
  serviceAudiobookPlayback();
  serviceRadioPlayback();
  checkAutoSleep();
  if (weatherPageVisible) taskYIELD();
  else delay(5);
}
