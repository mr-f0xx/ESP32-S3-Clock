// Subsonic API 1.16.1 client (Navidrome / Gonic / compatible servers).
#include "src/subsonic_protocol.h"

constexpr uint8_t SUBSONIC_TRACK_LIMIT = 60;
struct SubsonicTrack { String id, title, artist; uint16_t seconds; };
SubsonicTrack subsonicTracks[SUBSONIC_TRACK_LIMIT];
uint8_t subsonicCount = 0, subsonicIndex = 0, subsonicListPage = 0;
bool subsonicActive = false, subsonicPaused = false, subsonicConfigured = false;
String subsonicServer, subsonicUser, subsonicPassword, subsonicQuery;
lv_obj_t *subsonicStatus = nullptr, *subsonicTitle = nullptr;
lv_obj_t *subsonicRows[6] = {}, *subsonicServerField = nullptr,
          *subsonicUserField = nullptr, *subsonicPasswordField = nullptr,
          *subsonicSearchField = nullptr, *subsonicConfigPanel = nullptr,
          *subsonicKeyboard = nullptr;
AudioFileSource *subsonicSource = nullptr;
AudioFileSourceBuffer *subsonicBuffer = nullptr;
void *subsonicBufferMemory = nullptr;
AudioGenerator *subsonicGenerator = nullptr;
void loadSubsonTracksSafe();
bool loadSubsonicTracks(bool search = false);

class SubsonicHttpSource : public AudioFileSource {
 public:
  ~SubsonicHttpSource() override { close(); }
  bool open(const char *url) override {
    close();
    savedUrl = url;
    const bool tls = savedUrl.startsWith("https://");
    if (tls) {
      if (!extSdReady()) return false;
      File ca = SD.open("/subsonic-ca.pem", FILE_READ);
      if (!ca || ca.size() > 16384) { if (ca) ca.close(); return false; }
      rootCa = static_cast<char *>(heap_caps_malloc(ca.size() + 1, MALLOC_CAP_SPIRAM));
      if (!rootCa) { ca.close(); return false; }
      const size_t n = ca.read(reinterpret_cast<uint8_t *>(rootCa), ca.size());
      ca.close(); rootCa[n] = 0;
      secure.setCACert(rootCa);
      secure.setHandshakeTimeout(8000);
    }
    http.setTimeout(5000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(tls ? static_cast<NetworkClient &>(secure) : plain, savedUrl)) return false;
    http.setReuse(false);
    const int code = http.GET();
    if (code != 200) { http.end(); return false; }
    length = http.getSize();
    position = 0;
    opened = true;
    return true;
  }
  uint32_t read(void *data, uint32_t length) override { return receive(data, length, true); }
  uint32_t readNonBlock(void *data, uint32_t length) override { return receive(data, length, false); }
  bool seek(int32_t, int) override { return false; }
  bool close() override {
    http.end(); secure.stop(); plain.stop(); opened = false;
    if (rootCa) { free(rootCa); rootCa = nullptr; }
    return true;
  }
  bool isOpen() override { return opened; }
  uint32_t getSize() override { return length > 0 ? length : 0; }
  uint32_t getPos() override { return position; }
  int status() { return http.connected() ? 200 : 0; }
 private:
  uint32_t receive(void *data, uint32_t wanted, bool blocking) {
    if (!opened || !data) return 0;
    WiFiClient *stream = http.getStreamPtr();
    const uint32_t started = millis();
    while (opened && !stream->available() && http.connected() &&
           (length < 0 || position < static_cast<uint32_t>(length))) {
      if (!blocking || millis() - started > 5000) return 0;
      delay(1);
    }
    const int available = stream->available();
    if (available <= 0) return 0;
    const int n = stream->readBytes(static_cast<uint8_t *>(data), min<uint32_t>(wanted, available));
    if (n > 0) position += n;
    return n > 0 ? n : 0;
  }
  HTTPClient http;
  NetworkClient plain;
  NetworkClientSecure secure;
  String savedUrl;
  char *rootCa = nullptr;
  int length = -1;
  uint32_t position = 0;
  bool opened = false;
};

String subsonicUrl(const char *endpoint, const String &extra = String()) {
  char salt[17];
  for (uint8_t i = 0; i < 16; ++i) {
    const uint8_t nibble = esp_random() & 15;
    salt[i] = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
  }
  salt[16] = 0;
  uint8_t digest[16];
  mbedtls_md5_context context;
  mbedtls_md5_init(&context);
  mbedtls_md5_starts(&context);
  mbedtls_md5_update(&context, reinterpret_cast<const uint8_t *>(subsonicPassword.c_str()), subsonicPassword.length());
  mbedtls_md5_update(&context, reinterpret_cast<const uint8_t *>(salt), 16);
  mbedtls_md5_finish(&context, digest);
  mbedtls_md5_free(&context);
  char token[33];
  for (uint8_t i = 0; i < 16; ++i) snprintf(token + i * 2, 3, "%02x", digest[i]);
  char user[256];
  SubsonicProtocol::urlEncode(subsonicUser.c_str(), user, sizeof(user));
  String base = subsonicServer;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  String url = base + "/rest/" + endpoint + "?u=" + user + "&t=" + token +
    "&s=" + salt + "&v=1.16.1&c=AMOLEMClock&f=json";
  if (extra.length()) url += "&" + extra;
  return url;
}
void subsonicSetStatus(const char *text) {
  if (subsonicStatus) lv_label_set_text(subsonicStatus, text);
}
void stopSubsonicPlayback() {
  subsonicActive = subsonicPaused = false;
  if (subsonicGenerator) { subsonicGenerator->stop(); delete subsonicGenerator; subsonicGenerator = nullptr; }
  if (subsonicBuffer) { delete subsonicBuffer; subsonicBuffer = nullptr; }
  if (subsonicSource) { delete subsonicSource; subsonicSource = nullptr; }
  if (subsonicBufferMemory) { free(subsonicBufferMemory); subsonicBufferMemory = nullptr; }
  if (musicOutput) musicOutput->stop();
}
bool subsonicRequest(JsonDocument &document, const char *endpoint, const String &extra) {
  if (WiFi.status() != WL_CONNECTED || !subsonicConfigured) {
    subsonicSetStatus("Connect Wi-Fi and save server settings first"); return false;
  }
  SubsonicHttpSource source;
  if (!source.open(subsonicUrl(endpoint, extra).c_str())) {
    subsonicSetStatus("HTTP/TLS failed. HTTPS needs /subsonic-ca.pem on TF"); return false;
  }
  // Bounded response; never deserialize an unbounded server-controlled stream.
  String body;
  body.reserve(16384);
  uint8_t bytes[512];
  const uint32_t started = millis();
  while (source.isOpen() && body.length() < 196608 && millis() - started < 15000) {
    const uint32_t n = source.read(bytes, sizeof(bytes));
    if (n) body.concat(reinterpret_cast<const char *>(bytes), n);
    else if (!source.status()) break;
    lv_timer_handler();
  }
  source.close();
  DeserializationError error = deserializeJson(document, body);
  if (error || document["subsonic-response"]["status"].as<String>() != "ok") {
    const int code = document["subsonic-response"]["error"]["code"] | -1;
    char message[80]; snprintf(message, sizeof(message), "Subsonic error %d (check credentials / API)", code);
    subsonicSetStatus(message); return false;
  }
  return true;
}
void updateSubsonicRows() {
  for (uint8_t i = 0; i < 6; ++i) {
    if (!subsonicRows[i]) continue;
    lv_obj_t *label = lv_obj_get_child(subsonicRows[i], 0);
    const uint8_t index = subsonicListPage * 6 + i;
    if (index < subsonicCount) {
      String text = subsonicTracks[index].title + "\n" + subsonicTracks[index].artist;
      lv_label_set_text(label, text.c_str());
      lv_obj_clear_state(subsonicRows[i], LV_STATE_DISABLED);
    } else {
      lv_label_set_text(label, "-");
      lv_obj_add_state(subsonicRows[i], LV_STATE_DISABLED);
    }
  }
  if (subsonicTitle) {
    lv_label_set_text(subsonicTitle, subsonicCount ? subsonicTracks[subsonicIndex].title.c_str() : "No tracks loaded");
  }
}
bool loadSubsonicTracks(bool search) {
  JsonDocument document;
  String extra = "size=60";
  if (search && subsonicQuery.length()) {
    char query[256]; SubsonicProtocol::urlEncode(subsonicQuery.c_str(), query, sizeof(query));
    extra = "query=" + String(query) + "&songCount=60&albumCount=0&artistCount=0";
  }
  if (!subsonicRequest(document, search && subsonicQuery.length() ? "search3" : "getRandomSongs", extra)) return false;
  JsonArray songs = search && subsonicQuery.length()
    ? document["subsonic-response"]["searchResult3"]["song"].as<JsonArray>()
    : document["subsonic-response"]["randomSongs"]["song"].as<JsonArray>();
  subsonicCount = subsonicIndex = subsonicListPage = 0;
  for (JsonObject song : songs) {
    if (subsonicCount >= SUBSONIC_TRACK_LIMIT) break;
    SubsonicTrack &track = subsonicTracks[subsonicCount++];
    track.id = song["id"].as<String>();
    track.title = song["title"] | "Untitled";
    track.artist = song["artist"] | "Unknown artist";
    track.seconds = song["duration"] | 0;
  }
  updateSubsonicRows();
  subsonicSetStatus(subsonicCount ? "Ready - tap a track to stream" : "Server returned no songs");
  return subsonicCount != 0;
}
bool startSubsonicTrack(uint8_t index) {
  if (index >= subsonicCount) return false;
  stopExtensionAudio(); stopMusicPlayback(); stopAudiobookPlayback(); stopRadioPlayback();
  if (!initializeMusicCodec()) return false;
  subsonicBufferMemory = heap_caps_malloc(262144, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!subsonicBufferMemory) { subsonicSetStatus("Not enough PSRAM for stream buffer"); return false; }
  subsonicSource = new SubsonicHttpSource();
  // Ask compatible servers for MP3; no unauthenticated HTTPS or password in URL.
  if (!subsonicSource->open(subsonicUrl("stream", "id=" + subsonicTracks[index].id + "&maxBitRate=192&format=mp3").c_str())) {
    stopSubsonicPlayback(); subsonicSetStatus("Stream failed (TLS CA / server / network)"); return false;
  }
  subsonicBuffer = new AudioFileSourceBuffer(subsonicSource, subsonicBufferMemory, 262144);
  subsonicGenerator = new AudioGeneratorMP3();
  if (!subsonicGenerator->begin(subsonicBuffer, musicOutput)) {
    stopSubsonicPlayback(); subsonicSetStatus("MP3 decoder could not start"); return false;
  }
  subsonicIndex = index; subsonicActive = true; subsonicPaused = false;
  digitalWrite(AUDIO_PA_PIN, HIGH); applyAudioVolume(); updateSubsonicRows();
  subsonicSetStatus("Streaming from your server");
  return true;
}
void subsonicHideKeyboard() {
  if (subsonicKeyboard) { lv_obj_del(subsonicKeyboard); subsonicKeyboard = nullptr; }
}
void subsonicFieldEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  subsonicHideKeyboard();
  subsonicKeyboard = lv_keyboard_create(lv_scr_act());
  lv_obj_set_size(subsonicKeyboard, SCREEN_WIDTH, 190);
  lv_obj_align(subsonicKeyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  styleUtilityKeyboard(subsonicKeyboard, lv_color_hex(themeAccent(0x69D2FF)));
  lv_keyboard_set_textarea(subsonicKeyboard, lv_event_get_target(event));
}
void subsonicKeyboardEvent(lv_event_t *event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) subsonicHideKeyboard();
}
void subsonicConfigEvent(lv_event_t *) {
  if (!subsonicConfigPanel) return;
  subsonicHideKeyboard();
  if (lv_obj_has_flag(subsonicConfigPanel, LV_OBJ_FLAG_HIDDEN))
    lv_obj_clear_flag(subsonicConfigPanel, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(subsonicConfigPanel, LV_OBJ_FLAG_HIDDEN);
}
void subsonicSaveEvent(lv_event_t *) {
  if (!subsonicServerField || !subsonicUserField || !subsonicPasswordField) return;
  const char *server = lv_textarea_get_text(subsonicServerField);
  if (!SubsonicProtocol::validBaseUrl(server) || !lv_textarea_get_text(subsonicUserField)[0]) {
    subsonicSetStatus("Use http(s)://host[:port], no query; username required"); return;
  }
  subsonicServer = server; subsonicUser = lv_textarea_get_text(subsonicUserField);
  String password = lv_textarea_get_text(subsonicPasswordField);
  if (password.length()) subsonicPassword = password;
  preferences.putString("sub_url", subsonicServer);
  preferences.putString("sub_user", subsonicUser);
  if (password.length()) preferences.putString("sub_pass", subsonicPassword);
  subsonicConfigured = true;
  lv_textarea_set_text(subsonicPasswordField, "");
  password = String();
  subsonicHideKeyboard(); lv_obj_add_flag(subsonicConfigPanel, LV_OBJ_FLAG_HIDDEN);
  subsonicSetStatus("Saved. Server password is in device NVS (not the encrypted vault)");
}
void subsonicRowEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  const uint8_t index = subsonicListPage * 6 + reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  startSubsonicTrack(index);
}
void subsonicControlEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  const uintptr_t action = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  if (action == 0) { if (subsonicCount) startSubsonicTrack((subsonicIndex + subsonicCount - 1) % subsonicCount); }
  else if (action == 1) {
    if (!subsonicActive && subsonicCount) startSubsonicTrack(subsonicIndex);
    else if (musicOutput) {
      subsonicPaused = !subsonicPaused;
      digitalWrite(AUDIO_PA_PIN, subsonicPaused ? LOW : HIGH);
      subsonicSetStatus(subsonicPaused ? "Paused (network stream stopped)" : "Streaming");
      if (subsonicPaused) stopSubsonicPlayback();
    }
  } else if (action == 2) { if (subsonicCount) startSubsonicTrack((subsonicIndex + 1) % subsonicCount); }
  else if (action == 3) { stopSubsonicPlayback(); subsonicSetStatus("Stopped"); }
  else if (action == 4) { subsonicListPage = max(0, static_cast<int>(subsonicListPage) - 1); updateSubsonicRows(); }
  else if (action == 5) { if ((subsonicListPage + 1) * 6 < subsonicCount) ++subsonicListPage; updateSubsonicRows(); }
  else if (action == 6) loadSubsonTracksSafe();
}
void loadSubsonTracksSafe() { loadSubsonicTracks(); }
void subsonicSearchEvent(lv_event_t *) {
  if (subsonicSearchField) subsonicQuery = lv_textarea_get_text(subsonicSearchField);
  subsonicHideKeyboard(); loadSubsonicTracks(true);
}
void createSubsonicPage() {
  lv_obj_t *page = extPage(EXT_SUBSONIC, "Subsonic / Gonic", 0x69D2FF);
  subsonicTitle = extLabel(page, "Your music, your server", 24, 88, 432, 24, true);
  lv_label_set_long_mode(subsonicTitle, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_height(subsonicTitle, 32);
  subsonicStatus = extLabel(page, "Load random songs or search your library", 24, 124, 320, 16);
  lv_obj_set_height(subsonicStatus, 42);
  for (uint8_t i = 0; i < 6; ++i) {
    subsonicRows[i] = extButton(page, "-", 24, 170 + i * 38, 432, 34, subsonicRowEvent, i);
    lv_obj_t *label = lv_obj_get_child(subsonicRows[i], 0);
    lv_obj_set_width(label, 404); lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, &lv_font_ui_16, 0);
  }
  extButton(page, "|<", 24, 410, 62, 48, subsonicControlEvent, 0);
  extButton(page, "Play", 94, 410, 82, 48, subsonicControlEvent, 1, true);
  extButton(page, ">|", 184, 410, 62, 48, subsonicControlEvent, 2);
  extButton(page, "Stop", 254, 410, 82, 48, subsonicControlEvent, 3);
  extButton(page, "Setup", 344, 410, 112, 48, subsonicConfigEvent);
  subsonicConfigPanel = lv_obj_create(page);
  lv_obj_remove_style_all(subsonicConfigPanel);
  lv_obj_set_size(subsonicConfigPanel, 480, 480);
  lv_obj_set_style_bg_color(subsonicConfigPanel, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(subsonicConfigPanel, LV_OPA_COVER, 0);
  lv_obj_clear_flag(subsonicConfigPanel, LV_OBJ_FLAG_SCROLLABLE);
  extLabel(subsonicConfigPanel, "Server connection", 24, 18, 350, 24, true);
  extButton(subsonicConfigPanel, "X", 408, 12, 56, 48, subsonicConfigEvent);
  subsonicServerField = extField(subsonicConfigPanel, "https://music.example.com", 24, 64, 432);
  subsonicUserField = extField(subsonicConfigPanel, "Username", 24, 126, 432);
  subsonicPasswordField = extField(subsonicConfigPanel, "Password (blank keeps saved)", 24, 188, 432, true);
  lv_textarea_set_text(subsonicServerField, subsonicServer.c_str());
  lv_textarea_set_text(subsonicUserField, subsonicUser.c_str());
  lv_obj_t *fields[] = {subsonicServerField, subsonicUserField, subsonicPasswordField};
  for (lv_obj_t *field : fields) lv_obj_add_event_cb(field, subsonicFieldEvent, LV_EVENT_CLICKED, nullptr);
  extButton(subsonicConfigPanel, "Save", 24, 252, 130, 48, subsonicSaveEvent, 0, true);
  extButton(subsonicConfigPanel, "Random songs", 166, 252, 290, 48, subsonicControlEvent, 6);
  subsonicSearchField = extField(subsonicConfigPanel, "Search songs", 24, 312, 302);
  lv_obj_add_event_cb(subsonicSearchField, subsonicFieldEvent, LV_EVENT_CLICKED, nullptr);
  extButton(subsonicConfigPanel, "Find", 338, 312, 118, 48, subsonicSearchEvent);
  extLabel(subsonicConfigPanel, "HTTPS: /subsonic-ca.pem on TF. HTTP sends credentials on your LAN. Password stays in NVS, not the vault.", 24, 378, 432, 16);
  lv_obj_add_flag(subsonicConfigPanel, LV_OBJ_FLAG_HIDDEN);
  // Paging sits on the list header, keeping all six track targets 34 px high.
  extButton(page, "<", 360, 126, 44, 36, subsonicControlEvent, 4);
  extButton(page, ">", 412, 126, 44, 36, subsonicControlEvent, 5);
  lv_obj_move_foreground(subsonicConfigPanel);
  updateSubsonicRows();
}
void serviceSubsonic() {
  if (!subsonicActive || subsonicPaused || !subsonicGenerator) return;
  if (!subsonicGenerator->loop()) {
    stopSubsonicPlayback(); subsonicSetStatus("Stream ended / connection lost");
  }
}
void cleanupSubsonic() {
  stopSubsonicPlayback(); subsonicHideKeyboard();
  subsonicStatus = subsonicTitle = subsonicServerField = subsonicUserField =
    subsonicPasswordField = subsonicSearchField = subsonicConfigPanel = nullptr;
  for (lv_obj_t *&row : subsonicRows) row = nullptr;
}
void loadSubsonicSettings() {
  subsonicServer = preferences.getString("sub_url", "");
  subsonicUser = preferences.getString("sub_user", "");
  subsonicPassword = preferences.getString("sub_pass", "");
  subsonicConfigured = SubsonicProtocol::validBaseUrl(subsonicServer.c_str()) && subsonicUser.length();
}
