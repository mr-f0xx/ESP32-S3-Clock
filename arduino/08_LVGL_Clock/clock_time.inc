// NTP synchronization and RTC calibration.
// Included once by 08_LVGL_Clock.ino inside its private namespace.

void startNtp(bool forceRestart = false) {
  if (WiFi.status() != WL_CONNECTED || (ntpStarted && !forceRestart)) return;

  static const char *const serverSets[][3] = {
    {"ntp.cloud.aliyuncs.com", "time.cloudflare.com", "pool.ntp.org"},
    {"time.cloudflare.com", "ntp.cloud.aliyuncs.com", "time.nist.gov"},
    {"cn.pool.ntp.org", "ntp.cloud.aliyuncs.com", "pool.ntp.org"}
  };
  if (forceRestart && esp_sntp_enabled()) esp_sntp_stop();
  const uint8_t serverSet = ntpAttempt % 3;
  esp_sntp_set_time_sync_notification_cb(onNtpTimeSynchronized);
  configTzTime(timezoneRule.c_str(),
               serverSets[serverSet][0],
               serverSets[serverSet][1],
               serverSets[serverSet][2]);
  ntpStarted = true;
  ntpStartedAt = millis();
  ++ntpAttempt;
  Serial.printf("NTP synchronization attempt %u started with %s\n",
                ntpAttempt, serverSets[serverSet][0]);
}

void updateRtcFromNtp() {
  if (rtcCalibratedThisBoot) return;
  if (!ntpSyncPending) {
    if (temporaryNtpWifi &&
        millis() - temporaryNtpWifiStartedAt >= NTP_TEMPORARY_WIFI_TIMEOUT_MS) {
      Serial.println("Temporary NTP Wi-Fi timed out");
      stopTemporaryNtpWifi();
      return;
    }
    if (ntpStarted && WiFi.status() == WL_CONNECTED &&
        millis() - ntpStartedAt >= NTP_RETRY_MS) {
      startNtp(true);
    }
    return;
  }
  ntpSyncPending = false;

  const time_t now = time(nullptr);
  if (now <= 1700000000) return;

  struct tm localTime {};
  if (!localtime_r(&now, &localTime)) return;

  if (!rtcReady) {
    Serial.println("NTP synchronized, but PCF85063 is unavailable");
    stopTemporaryNtpWifi();
    return;
  }

  rtc.setDateTime(localTime);
  RTC_DateTime writtenTime;
  if (!readRtcTime(writtenTime)) {
    Serial.println("PCF85063 verification failed after NTP write");
    return;
  }

  rtcTimeValid = true;
  timeSynced = true;
  rtcCalibratedThisBoot = true;
  updateClock();
  Serial.printf("NTP written to PCF85063: %04u-%02u-%02u %02u:%02u:%02u\n",
                writtenTime.getYear(), writtenTime.getMonth(), writtenTime.getDay(),
                writtenTime.getHour(), writtenTime.getMinute(), writtenTime.getSecond());
  if (esp_sntp_enabled()) esp_sntp_stop();
  ntpStartedAt = 0;
  Serial.println("SNTP stopped; clock source is now PCF85063");
  stopTemporaryNtpWifi();
}
