// Local RF diagnostics: network names/RSSI from normal scans, plus aggregate
// 802.11 frame counts. Promiscuous payloads, MAC addresses and credentials are
// never recorded, displayed, uploaded, or written to storage.
#include <esp_wifi.h>
#include <esp_wifi_types.h>
constexpr uint8_t HUD_NETWORK_LIMIT = 8;
volatile uint32_t hudManagement = 0, hudData = 0, hudControl = 0, hudOther = 0;
volatile uint32_t hudWeak = 0, hudMedium = 0, hudStrong = 0;
bool hudRunning = false, hudPaused = false, hudWifiWasOff = false, hudConnected = false;
uint8_t hudChannel = 1;
uint32_t hudLastTick = 0, hudLastHop = 0, hudLastPackets = 0;
float hudRate = 0;
lv_obj_t *hudStats = nullptr, *hudStatus = nullptr, *hudNetworks = nullptr, *hudBars[12] = {};
uint32_t hudHistory[12] = {};

void IRAM_ATTR hudPromiscuousCallback(void *buffer, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t *packet = static_cast<const wifi_promiscuous_pkt_t *>(buffer);
  if (!packet || packet->rx_ctrl.sig_len < 2) return;
  // Only these two header bytes are inspected. Nothing from the frame is kept.
  const uint8_t frameType = (packet->payload[0] >> 2) & 3;
  if (frameType == 0) ++hudManagement;
  else if (frameType == 2) ++hudData;
  else if (frameType == 1) ++hudControl;
  else ++hudOther;
  const int8_t rssi = packet->rx_ctrl.rssi;
  if (rssi < -75) ++hudWeak;
  else if (rssi < -55) ++hudMedium;
  else ++hudStrong;
}
void hudClearCounts() {
  hudManagement = hudData = hudControl = hudOther = hudWeak = hudMedium = hudStrong = 0;
  hudLastPackets = 0; hudRate = 0;
  for (uint32_t &value : hudHistory) value = 0;
}
bool hudStart() {
  if (hudRunning) return true;
  hudWifiWasOff = WiFi.getMode() == WIFI_OFF;
  if (hudWifiWasOff) { WiFi.mode(WIFI_STA); delay(30); }
  hudConnected = WiFi.status() == WL_CONNECTED;
  if (hudConnected) {
    wifi_second_chan_t secondary;
    esp_wifi_get_channel(&hudChannel, &secondary);
  }
  hudClearCounts();
  if (esp_wifi_set_promiscuous(false) != ESP_OK ||
      esp_wifi_set_promiscuous_rx_cb(hudPromiscuousCallback) != ESP_OK ||
      esp_wifi_set_promiscuous(true) != ESP_OK) return false;
  hudRunning = true; hudPaused = false; hudLastTick = hudLastHop = millis();
  WiFi.scanNetworks(true, false, true, 120);
  return true;
}
void hudStop() {
  if (hudRunning) {
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
  }
  hudRunning = hudPaused = false;
  WiFi.scanDelete();
  if (hudWifiWasOff && !wifiUserEnabled) WiFi.mode(WIFI_OFF);
}
void hudUpdateNetworks() {
  if (!hudNetworks) return;
  const int16_t count = WiFi.scanComplete();
  if (count < 0) return;
  String text;
  for (uint8_t i = 0; i < min<int16_t>(count, HUD_NETWORK_LIMIT); ++i) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) ssid = "(hidden)";
    if (ssid.length() > 24) ssid = ssid.substring(0, 24);
    text += ssid + "  " + String(WiFi.RSSI(i)) + " dBm  ch" + String(WiFi.channel(i)) + "\n";
  }
  if (!count) text = "No networks found";
  lv_label_set_text(hudNetworks, text.c_str());
  WiFi.scanDelete();
}
void hudActionEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  const uintptr_t action = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  if (action == 0) { WiFi.scanDelete(); WiFi.scanNetworks(true, false, true, 120); }
  else if (action == 1 && hudRunning) {
    hudPaused = !hudPaused; esp_wifi_set_promiscuous(!hudPaused);
  } else if (action == 2) hudClearCounts();
  else if ((action == 3 || action == 4) && hudRunning && !hudConnected) {
    hudChannel = action == 3 ? (hudChannel == 1 ? 13 : hudChannel - 1) : (hudChannel % 13 + 1);
    esp_wifi_set_channel(hudChannel, WIFI_SECOND_CHAN_NONE);
  }
}
void createWifiHudPage() {
  lv_obj_t *page = extPage(EXT_WIFI_HUD, "WiFi / RF HUD", 0x53E0D0);
  hudStatus = extLabel(page, "Local diagnostics only - no packet contents captured", 24, 88, 432, 16);
  lv_obj_set_height(hudStatus, 38);
  hudStats = extLabel(page, "Starting radio...", 24, 130, 432, 20, true);
  lv_obj_set_height(hudStats, 70);
  for (uint8_t i = 0; i < 12; ++i) {
    hudBars[i] = lv_obj_create(page);
    lv_obj_remove_style_all(hudBars[i]);
    lv_obj_set_pos(hudBars[i], 28 + i * 36, 230);
    lv_obj_set_size(hudBars[i], 24, 2);
    lv_obj_set_style_bg_color(hudBars[i], lv_color_hex(extensionAccent), 0);
    lv_obj_set_style_bg_opa(hudBars[i], LV_OPA_COVER, 0);
    lv_obj_set_style_radius(hudBars[i], 4, 0);
  }
  hudNetworks = extLabel(page, "Scanning nearby networks...", 24, 248, 432, 16);
  lv_obj_set_height(hudNetworks, 166);
  extButton(page, "Scan", 24, 426, 94, 44, hudActionEvent, 0, true);
  extButton(page, "Pause", 126, 426, 94, 44, hudActionEvent, 1);
  extButton(page, "Clear", 228, 426, 94, 44, hudActionEvent, 2);
  extButton(page, "Ch-", 330, 426, 60, 44, hudActionEvent, 3);
  extButton(page, "Ch+", 398, 426, 60, 44, hudActionEvent, 4);
  if (!hudStart() && hudStatus) lv_label_set_text(hudStatus, "Radio monitor could not start; Wi-Fi may be busy");
}
void serviceWifiHud() {
  if (!hudRunning || !extensionVisible[EXT_WIFI_HUD]) return;
  hudUpdateNetworks();
  const uint32_t now = millis();
  if (!hudConnected && !hudPaused && now - hudLastHop >= 1500) {
    hudLastHop = now; hudChannel = hudChannel % 13 + 1;
    esp_wifi_set_channel(hudChannel, WIFI_SECOND_CHAN_NONE);
  }
  if (now - hudLastTick < 500) return;
  const uint32_t total = hudManagement + hudData + hudControl + hudOther;
  hudRate = (total - hudLastPackets) * 1000.0f / (now - hudLastTick);
  hudLastPackets = total; hudLastTick = now;
  for (uint8_t i = 1; i < 12; ++i) hudHistory[i - 1] = hudHistory[i];
  hudHistory[11] = hudRate;
  uint32_t peak = 10;
  for (uint32_t value : hudHistory) peak = max(peak, value);
  for (uint8_t i = 0; i < 12; ++i) {
    if (hudBars[i]) {
      const uint32_t height = hudHistory[i] * 34 / peak;
      lv_obj_set_height(hudBars[i], height < 2 ? 2 : height);
    }
  }
  if (hudStats) lv_label_set_text_fmt(hudStats, "CH %u   %.0f frames/s   %lu total\nMgmt %lu   Data %lu   Ctrl %lu\nRSSI: weak %lu / mid %lu / strong %lu",
    hudChannel, hudRate, total, hudManagement, hudData, hudControl, hudWeak, hudMedium, hudStrong);
  if (hudStatus) lv_label_set_text(hudStatus, hudPaused ? "Monitor paused" :
    hudConnected ? "Connected: locked to current AP channel" : "Hopping channels 1-13 - aggregate counts only");
}
void cleanupWifiHud() {
  hudStop(); hudStats = hudStatus = hudNetworks = nullptr;
  for (lv_obj_t *&bar : hudBars) bar = nullptr;
}
