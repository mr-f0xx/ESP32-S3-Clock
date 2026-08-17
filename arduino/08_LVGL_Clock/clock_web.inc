// Wi-Fi portal, web API, uploads and preferences.
// Included once by 08_LVGL_Clock.ino inside its private namespace.

void startConfigPortal() {
  if (!wifiUserEnabled) return;
  if (portalActive) {
    if (!webServerStarted) startWebServer();
    updateWifiSetupPage();
    return;
  }
  Serial.println("Starting configuration AP...");
  WiFi.mode(WIFI_AP_STA);
  const uint64_t chipId = ESP.getEfuseMac() & 0xFFFFFFULL;
  snprintf(apSsid, sizeof(apSsid), "AMOLED-Clock-%06llX", chipId);
  // Configuration mode is intentionally an open, temporary network. The AP
  // is stopped as soon as the user leaves the Settings app.
  if (!WiFi.softAP(apSsid)) {
    Serial.println("Unable to start configuration AP");
    return;
  }
  delay(100);
  dnsServer.start(53, "*", WiFi.softAPIP());
  portalActive = true;
  if (!webServerStarted) startWebServer();
  updateNetworkLabels();
  updateWifiSetupPage();
  Serial.printf("Configuration AP: %s, http://%s\n", apSsid, WiFi.softAPIP().toString().c_str());
}

void stopConfigPortal() {
  portalRequested = false;
  if (!portalActive) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  portalActive = false;
  if (wifiUserEnabled && (!savedSsid.isEmpty() || WiFi.status() == WL_CONNECTED)) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
  } else {
    WiFi.mode(WIFI_OFF);
  }
  updateNetworkLabels();
  updateWifiSetupPage();
  Serial.println("Configuration AP stopped after leaving Settings");
}

bool connectSavedWifi(bool ntpOnly) {
  if (!wifiUserEnabled && !ntpOnly) return false;
  if (savedSsid.isEmpty()) {
    Serial.println("No saved Wi-Fi credentials");
    return false;
  }
  Serial.printf("Connecting to saved Wi-Fi: %s\n", savedSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_CONNECT_TIMEOUT_MS) {
    lv_timer_handler();
    delay(10);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Wi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
    startNtp();
    return true;
  }
  Serial.println("Saved Wi-Fi connection timed out");
  return false;
}

void stopTemporaryNtpWifi() {
  if (!temporaryNtpWifi) return;
  if (esp_sntp_enabled()) esp_sntp_stop();
  temporaryNtpWifi = false;
  temporaryNtpWifiStartedAt = 0;
  ntpStarted = rtcCalibratedThisBoot;
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  updateNetworkLabels();
  Serial.println("Temporary NTP Wi-Fi stopped");
}

void startTemporaryNtpWifi() {
  if (wifiUserEnabled || temporaryNtpWifi || rtcCalibratedThisBoot || savedSsid.isEmpty()) return;

  temporaryNtpWifi = true;
  temporaryNtpWifiStartedAt = millis();
  Serial.println("Wi-Fi is disabled; starting temporary NTP connection");
  if (!connectSavedWifi(true)) {
    Serial.println("Temporary NTP Wi-Fi could not connect");
    stopTemporaryNtpWifi();
  }
}

void redirectToPortal() {
  server.sendHeader("Location", portalActive ? "http://192.168.4.1/" : "/", true);
  server.send(302, "text/plain", "");
}

void handleStatus() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  String json = F("{\"connected\":");
  json += connected ? F("true") : F("false");
  json += F(",\"ssid\":\"");
  json += jsonEscape(connected ? WiFi.SSID() : savedSsid);
  json += F("\",\"ip\":\"");
  json += connected ? WiFi.localIP().toString() : String();
  json += F("\",\"portal\":");
  json += portalActive ? F("true") : F("false");
  json += F(",\"ap_ssid\":\"");
  json += jsonEscape(apSsid);
  json += F("\",\"time_synced\":");
  json += timeSynced ? F("true") : F("false");
  json += F(",\"has_image\":");
  json += imageCount ? F("true") : F("false");
  json += F(",\"image_count\":");
  json += String(imageCount);
  json += F(",\"font\":");
  json += String(clockFontIndex);
  json += F(",\"night_start\":");
  json += String(nightStartMinute);
  json += F(",\"night_end\":");
  json += String(nightEndMinute);
  json += F(",\"idle_minutes\":");
  json += String(idleSleepMinutes);
  json += F(",\"idle_enabled\":");
  json += idleSleepEnabled ? F("true") : F("false");
  json += F(",\"sleep_show_time\":");
  json += sleepTimeDisplayEnabled ? F("true") : F("false");
  json += F(",\"weather_host\":\"");
  json += jsonEscape(weatherApiHost);
  json += F("\",\"weather_key\":\"");
  json += jsonEscape(weatherApiKey);
  json += F("\",\"weather_location\":\"");
  json += jsonEscape(weatherLocation);
  json += F("\",\"weather_auto_location\":");
  json += weatherAutoLocationEnabled ? F("true") : F("false");
  char resolvedCity[sizeof(WeatherWallpaper::resolvedWeatherCity)]{};
  char resolvedTimezone[sizeof(WeatherWallpaper::resolvedWeatherTimezone)]{};
  portENTER_CRITICAL(&WeatherWallpaper::stateMux);
  strlcpy(resolvedCity, WeatherWallpaper::resolvedWeatherCity,
          sizeof(resolvedCity));
  strlcpy(resolvedTimezone, WeatherWallpaper::resolvedWeatherTimezone,
          sizeof(resolvedTimezone));
  portEXIT_CRITICAL(&WeatherWallpaper::stateMux);
  json += F(",\"weather_city\":\"");
  json += jsonEscape(resolvedCity);
  json += F("\",\"weather_timezone\":\"");
  json += jsonEscape(resolvedTimezone);
  json += F("\",\"album_auto\":");
  json += albumAutoSwitchEnabled ? F("true") : F("false");
  json += F(",\"album_minutes\":");
  json += String(albumSwitchMinutes);
  json += F(",\"album_limit\":");
  json += String(albumImageLimit);
  json += F(",\"album_capacity\":");
  json += String(IMAGE_SLOT_COUNT);
  json += F(",\"weather_auto\":");
  json += weatherAutoSwitchEnabled ? F("true") : F("false");
  json += F(",\"weather_follow\":");
  json += weatherWallpaperFollowEnabled ? F("true") : F("false");
  json += F(",\"weather_minutes\":");
  json += String(weatherSwitchMinutes);
  json += '}';
  server.send(200, "application/json; charset=utf-8", json);
}

bool parseClockMinute(const String &value, uint16_t &minuteOfDay) {
  if (value.length() != 5 || value[2] != ':' ||
      !isDigit(value[0]) || !isDigit(value[1]) ||
      !isDigit(value[3]) || !isDigit(value[4])) return false;
  const uint8_t hour = (value[0] - '0') * 10 + value[1] - '0';
  const uint8_t minute = (value[3] - '0') * 10 + value[4] - '0';
  if (hour > 23 || minute > 59) return false;
  minuteOfDay = hour * 60U + minute;
  return true;
}

void handleSleepSettings() {
  uint16_t startMinute;
  uint16_t endMinute;
  const bool enabled = server.arg("idle_enabled") == "1";
  const bool showTime = server.hasArg("show_time")
                          ? server.arg("show_time") == "1"
                          : sleepTimeDisplayEnabled;
  const long idleMinutes = server.arg("idle_minutes").toInt();
  if (!parseClockMinute(server.arg("night_start"), startMinute) ||
      !parseClockMinute(server.arg("night_end"), endMinute) ||
      startMinute == endMinute || idleMinutes < MIN_IDLE_SLEEP_MINUTES ||
      idleMinutes > MAX_IDLE_SLEEP_MINUTES) {
    server.send(400, "text/plain; charset=utf-8", "休眠时长或夜间时段无效");
    return;
  }
  nightStartMinute = startMinute;
  nightEndMinute = endMinute;
  idleSleepEnabled = enabled;
  sleepTimeDisplayEnabled = showTime;
  idleSleepMinutes = static_cast<uint16_t>(idleMinutes);
  preferences.putUShort("night_start", nightStartMinute);
  preferences.putUShort("night_end", nightEndMinute);
  preferences.putUShort("idle_minutes", idleSleepMinutes);
  preferences.putBool("idle_on", idleSleepEnabled);
  preferences.putBool("sleep_clock", sleepTimeDisplayEnabled);
  markUserActivity();
  server.send(200, "text/plain; charset=utf-8",
              idleSleepEnabled ? "无操作休眠设置已保存" : "无操作休眠已关闭");
}

bool validCoordinate(const String &text, double minimum, double maximum) {
  if (text.isEmpty()) return false;
  char *end = nullptr;
  const double value = strtod(text.c_str(), &end);
  return end && *end == '\0' && value >= minimum && value <= maximum;
}

void handleWeatherSettings() {
  String host = server.arg("host");
  String key = server.arg("key");
  String latitude = server.arg("latitude");
  String longitude = server.arg("longitude");
  const bool automatic = server.arg("auto_location") == "1";
  host.trim();
  key.trim();
  latitude.trim();
  longitude.trim();
  host.replace("https://", "");
  host.replace("http://", "");
  while (host.endsWith("/")) host.remove(host.length() - 1);
  const bool haveManualCoordinates = !latitude.isEmpty() || !longitude.isEmpty();
  if (host.isEmpty() || host.length() > 96 || key.isEmpty() ||
      key.length() > 128 || (!automatic && !haveManualCoordinates) ||
      (haveManualCoordinates &&
       (!validCoordinate(latitude, -90.0, 90.0) ||
        !validCoordinate(longitude, -180.0, 180.0)))) {
    server.send(400, "text/plain; charset=utf-8", "天气 API 参数无效");
    return;
  }
  weatherApiHost = host;
  weatherApiKey = key;
  weatherAutoLocationEnabled = automatic;
  if (haveManualCoordinates)
    weatherLocation = longitude + ',' + latitude;
  preferences.putString("weather_host", weatherApiHost);
  preferences.putString("weather_key", weatherApiKey);
  preferences.putString("weather_loc", weatherLocation);
  preferences.putBool("weather_geo", weatherAutoLocationEnabled);
  refreshWeatherConfiguration();
  markUserActivity();
  server.send(200, "text/plain; charset=utf-8", "天气配置已保存，正在刷新");
}

void handleBackgroundSwitchSettings() {
  const long albumMinutes = server.arg("album_minutes").toInt();
  const long albumLimit = server.hasArg("album_limit")
                            ? server.arg("album_limit").toInt()
                            : albumImageLimit;
  const long weatherMinutes = server.arg("weather_minutes").toInt();
  if (albumMinutes < MIN_BACKGROUND_SWITCH_MINUTES ||
      albumMinutes > MAX_BACKGROUND_SWITCH_MINUTES ||
      albumLimit < 1 || albumLimit > IMAGE_SLOT_COUNT ||
      albumLimit < imageCount ||
      weatherMinutes < MIN_BACKGROUND_SWITCH_MINUTES ||
      weatherMinutes > MAX_BACKGROUND_SWITCH_MINUTES) {
    server.send(400, "text/plain; charset=utf-8",
                albumLimit < imageCount
                  ? "相册上限不能小于当前图片数；请先清空相册"
                  : "自动切换间隔或相册上限无效");
    return;
  }
  albumAutoSwitchEnabled = server.arg("album_auto") == "1";
  weatherAutoSwitchEnabled = server.arg("weather_auto") == "1";
  weatherWallpaperFollowEnabled = server.hasArg("weather_follow")
                                    ? server.arg("weather_follow") == "1"
                                    : weatherWallpaperFollowEnabled;
  albumSwitchMinutes = static_cast<uint16_t>(albumMinutes);
  albumImageLimit = static_cast<uint8_t>(albumLimit);
  weatherSwitchMinutes = static_cast<uint16_t>(weatherMinutes);
  nextAlbumAutoSwitchAt = 0;
  WeatherWallpaper::nextRandomizeAtMs = 0;
  preferences.putBool("album_auto", albumAutoSwitchEnabled);
  preferences.putUShort("album_mins", albumSwitchMinutes);
  preferences.putUChar("album_limit", albumImageLimit);
  preferences.putBool("weather_auto", weatherAutoSwitchEnabled);
  preferences.putBool("weather_follow", weatherWallpaperFollowEnabled);
  preferences.putUShort("weather_mins", weatherSwitchMinutes);
  WeatherWallpaper::applyWallpaperFollowSetting();
  markUserActivity();
  server.send(200, "text/plain; charset=utf-8", "背景自动切换设置已保存");
}

void handleSave() {
  markUserActivity();
  const String ssid = server.arg("ssid");
  const String password = server.arg("password");
  const String tz = server.arg("tz");
  if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 64 || tz.isEmpty() || tz.length() > 63) {
    server.send(400, "text/plain; charset=utf-8", "参数无效");
    return;
  }

  preferences.putString("ssid", ssid);
  preferences.putString("pass", password);
  preferences.putString("tz", tz);
  server.send(200, "text/html; charset=utf-8",
              "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'><h2>设置已保存</h2><p>设备正在重启并连接 Wi-Fi。</p>");
  rebootAt = millis() + 1200;
}

bool commitUploadedImage() {
  return commitUploadedImageToHistory();
}

void handleImageUpload() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    markUserActivity();
    uploadSucceeded = false;
    uploadError = String();
    uploadBytes = 0;
    LittleFS.remove(IMAGE_TEMP_PATH);
    uploadFile = LittleFS.open(IMAGE_TEMP_PATH, FILE_WRITE);
    if (!uploadFile) uploadError = "无法创建临时文件";
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile && uploadError.isEmpty()) {
      const size_t written = uploadFile.write(upload.buf, upload.currentSize);
      uploadBytes += written;
      if (written != upload.currentSize || uploadBytes > IMAGE_BYTES) uploadError = "写入图片失败";
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    if (uploadError.isEmpty() && uploadBytes != IMAGE_BYTES) {
      uploadError = "图片尺寸不正确，必须为 480x480 RGB565";
    }
    if (uploadError.isEmpty()) {
      uploadSucceeded = commitUploadedImage();
      if (uploadSucceeded) imageReloadRequested = true;
    }
    if (!uploadSucceeded) LittleFS.remove(IMAGE_TEMP_PATH);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
    LittleFS.remove(IMAGE_TEMP_PATH);
    uploadError = "上传已中止";
  }
}

void finishImageUpload() {
  if (uploadSucceeded) {
    server.send(200, "text/plain; charset=utf-8", "OK");
  } else {
    server.send(400, "text/plain; charset=utf-8", uploadError.isEmpty() ? "上传失败" : uploadError);
  }
}

void handleClearImage() {
  markUserActivity();
  imageClearRequested = true;
  server.send(200, "text/plain; charset=utf-8", "相册清空请求已提交");
}

void handleClockFont() {
  markUserActivity();
  const String value = server.arg("font");
  if (value.length() != 1 || value[0] < '0' || value[0] >= '0' + CLOCK_FONT_COUNT) {
    server.send(400, "text/plain; charset=utf-8", "字体参数无效");
    return;
  }

  pendingClockFontIndex = static_cast<uint8_t>(value[0] - '0');
  clockFontChangeRequested = true;
  server.send(200, "text/plain; charset=utf-8", "字体已保存");
}

void startWebServer() {
  if (!wifiUserEnabled || webServerStarted) return;
  Serial.println("Starting web server...");
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", CLOCK_WEB_PAGE); });
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/upload", HTTP_POST, finishImageUpload, handleImageUpload);
  server.on("/clear-image", HTTP_POST, handleClearImage);
  server.on("/font", HTTP_POST, handleClockFont);
  server.on("/sleep-settings", HTTP_POST, handleSleepSettings);
  server.on("/weather-settings", HTTP_POST, handleWeatherSettings);
  server.on("/background-switch", HTTP_POST, handleBackgroundSwitchSettings);
  server.on("/generate_204", HTTP_ANY, redirectToPortal);
  server.on("/hotspot-detect.html", HTTP_ANY, redirectToPortal);
  server.on("/fwlink", HTTP_ANY, redirectToPortal);
  server.onNotFound([] {
    if (portalActive) redirectToPortal();
    else server.send(404, "text/plain", "Not found");
  });
  server.begin();
  webServerStarted = true;
  Serial.println("Web server ready");
}

void loadSettings() {
  preferences.begin("clock", false);
  savedSsid = preferences.getString("ssid", "");
  savedPassword = preferences.getString("pass", "");
  timezoneRule = preferences.getString("tz", "CST-8");
  weatherApiHost = preferences.getString("weather_host", CLOCK_QWEATHER_API_HOST);
  weatherApiKey = preferences.getString("weather_key", CLOCK_QWEATHER_API_KEY);
  weatherLocation = preferences.getString("weather_loc", CLOCK_QWEATHER_LOCATION);
  weatherAutoLocationEnabled = preferences.getBool("weather_geo", true);
  wifiUserEnabled = preferences.getBool("wifi_on", true);
  orientationLocked = preferences.getBool("orient_lock", false);
  // BLE is intentionally unavailable in this build. Clear the previous
  // preference so it cannot silently return after a future restart.
  if (preferences.getBool("bt_on", false)) preferences.putBool("bt_on", false);
  activeBrightness = max(
    preferences.getUChar("brightness", DEFAULT_ACTIVE_BRIGHTNESS),
    MIN_ACTIVE_BRIGHTNESS);
  nightStartMinute = min(preferences.getUShort(
    "night_start", DEFAULT_NIGHT_START_MINUTE), static_cast<uint16_t>(1439));
  nightEndMinute = min(preferences.getUShort(
    "night_end", DEFAULT_NIGHT_END_MINUTE), static_cast<uint16_t>(1439));
  if (nightStartMinute == nightEndMinute) {
    nightStartMinute = DEFAULT_NIGHT_START_MINUTE;
    nightEndMinute = DEFAULT_NIGHT_END_MINUTE;
  }
  idleSleepMinutes = constrain(preferences.getUShort(
    "idle_minutes", DEFAULT_IDLE_SLEEP_MINUTES),
    MIN_IDLE_SLEEP_MINUTES, MAX_IDLE_SLEEP_MINUTES);
  idleSleepEnabled = preferences.getBool("idle_on", true);
  sleepTimeDisplayEnabled = preferences.getBool("sleep_clock", true);
  albumAutoSwitchEnabled = preferences.getBool("album_auto", false);
  weatherAutoSwitchEnabled = preferences.getBool("weather_auto", true);
  weatherWallpaperFollowEnabled = preferences.getBool("weather_follow", true);
  albumSwitchMinutes = constrain(preferences.getUShort(
    "album_mins", DEFAULT_ALBUM_SWITCH_MINUTES),
    MIN_BACKGROUND_SWITCH_MINUTES, MAX_BACKGROUND_SWITCH_MINUTES);
  albumImageLimit = constrain(preferences.getUChar(
    "album_limit", DEFAULT_ALBUM_IMAGE_LIMIT), static_cast<uint8_t>(1),
    IMAGE_SLOT_COUNT);
  weatherSwitchMinutes = constrain(preferences.getUShort(
    "weather_mins", DEFAULT_WEATHER_SWITCH_MINUTES),
    MIN_BACKGROUND_SWITCH_MINUTES, MAX_BACKGROUND_SWITCH_MINUTES);
  audioVolume = constrain(preferences.getUChar("volume", DEFAULT_AUDIO_VOLUME), 0, 100);
  musicPlaybackMode = min<uint8_t>(
    preferences.getUChar("music_mode", MUSIC_MODE_REPEAT_ALL),
    MUSIC_MODE_COUNT - 1);
  for (uint8_t i = 0; i < 3; ++i) {
    char key[8];
    snprintf(key, sizeof(key), "a%u_h", i);
    alarmHours[i] = min<uint8_t>(preferences.getUChar(key, alarmHours[i]), 23);
    snprintf(key, sizeof(key), "a%u_m", i);
    alarmMinutes[i] = min<uint8_t>(preferences.getUChar(key, alarmMinutes[i]), 59);
    snprintf(key, sizeof(key), "a%u_on", i);
    alarmEnabled[i] = preferences.getBool(key, alarmEnabled[i]);
  }
  clockFontIndex = min(preferences.getUChar("clock_font", CLOCK_FONT_MONTSERRAT),
                       static_cast<uint8_t>(CLOCK_FONT_COUNT - 1));
  pendingClockFontIndex = clockFontIndex;
  applyClockFont();
  syncControlPanelSettings();
  syncAlarmUi();
  updateMusicPlaybackModeIcon();
}
