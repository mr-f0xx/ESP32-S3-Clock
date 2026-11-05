// AES-256-GCM password vault + explicit, host-focused USB keyboard typing.
#include "src/vault_crypto.h"
#include <bootloader_random.h>
#if !defined(CONFIG_TINYUSB_HID_ENABLED) || !CONFIG_TINYUSB_HID_ENABLED
#error "Password vault USB typing requires CONFIG_TINYUSB_HID_ENABLED=y in the ESP32 Arduino core"
#endif
#if defined(CONFIG_TINYUSB_HID_ENABLED) && CONFIG_TINYUSB_HID_ENABLED
#include <USBHIDKeyboard.h>
#include "keyboardLayout/KeyboardLayout.h"
// Global constructor registers the HID interface before Arduino main() starts
// TinyUSB. CDC remains enabled for serial/upload; reports are never sent here.
USBHIDKeyboard vaultUsbKeyboard;
#endif

constexpr uint8_t VAULT_RECORD_LIMIT = 24;
struct VaultRecord {
  char title[49] = {}, username[65] = {}, password[129] = {}, note[97] = {};
};
VaultRecord vaultRecords[VAULT_RECORD_LIMIT];
uint8_t vaultCount = 0, vaultSelected = 0, vaultListPage = 0, vaultEditIndex = 255;
bool vaultUnlocked = false, vaultExists = false, vaultFrenchLayout = false, vaultKeyboardReady = false;
void vaultUpdateRows();
uint8_t vaultKey[VaultCrypto::VAULT_KEY_BYTES] = {}, vaultSalt[VaultCrypto::VAULT_SALT_BYTES] = {};
uint32_t vaultLockUntil = 0;
lv_obj_t *vaultLockPanel = nullptr, *vaultListPanel = nullptr, *vaultDetailPanel = nullptr,
          *vaultEditPanel = nullptr, *vaultConfirmPanel = nullptr, *vaultKeyboard = nullptr,
          *vaultStatus = nullptr, *vaultPinField = nullptr, *vaultRows[6] = {},
          *vaultTitleField = nullptr, *vaultUserField = nullptr, *vaultPassField = nullptr,
          *vaultNoteField = nullptr, *vaultDetailTitle = nullptr, *vaultDetailUser = nullptr;
bool vaultTyping = false, vaultTypePressed = false;
uint16_t vaultTypeIndex = 0;
uint32_t vaultTypeNextAt = 0;

bool vaultTypingActive() { return vaultTyping; }
void vaultStatusText(const char *text) { if (vaultStatus) lv_label_set_text(vaultStatus, text); }
void vaultClearField(lv_obj_t *field) {
  if (!field) return;
  char *text = const_cast<char *>(lv_textarea_get_text(field));
  if (text) VaultCrypto::wipe(text, strlen(text));
  lv_textarea_set_text(field, "");
}
void vaultHideKeyboard() {
  if (vaultKeyboard) { lv_obj_del(vaultKeyboard); vaultKeyboard = nullptr; }
}
void vaultStopTyping() {
  vaultTyping = vaultTypePressed = false; vaultTypeIndex = 0;
#if defined(CONFIG_TINYUSB_HID_ENABLED) && CONFIG_TINYUSB_HID_ENABLED
  if (vaultKeyboardReady) vaultUsbKeyboard.releaseAll();
#endif
}
void vaultLock() {
  vaultStopTyping(); vaultHideKeyboard();
  VaultCrypto::wipe(vaultKey, sizeof(vaultKey));
  VaultCrypto::wipe(vaultRecords, sizeof(vaultRecords));
  vaultCount = 0; vaultUnlocked = false; vaultEditIndex = 255;
  vaultClearField(vaultPinField);
  vaultClearField(vaultPassField);
  if (vaultListPanel) lv_obj_add_flag(vaultListPanel, LV_OBJ_FLAG_HIDDEN);
  if (vaultDetailPanel) lv_obj_add_flag(vaultDetailPanel, LV_OBJ_FLAG_HIDDEN);
  if (vaultEditPanel) lv_obj_add_flag(vaultEditPanel, LV_OBJ_FLAG_HIDDEN);
  if (vaultConfirmPanel) lv_obj_add_flag(vaultConfirmPanel, LV_OBJ_FLAG_HIDDEN);
  if (vaultLockPanel) { lv_obj_clear_flag(vaultLockPanel, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(vaultLockPanel); }
  vaultStatusText("Locked - keys and plaintext wiped from RAM");
}
bool vaultValidPin(const char *pin) {
  const size_t length = strlen(pin);
  if (length < 10 || length > 32) return false;
  for (size_t i = 0; i < length; ++i) if (pin[i] < '0' || pin[i] > '9') return false;
  return true;
}
void vaultRandom(uint8_t *bytes, size_t length) {
  // True hardware entropy, including when Wi-Fi/Bluetooth are disabled.
  bootloader_fill_random(bytes, length);
}
bool vaultWriteEncrypted(const uint8_t *plain, size_t length) {
  if (!vaultUnlocked || !extSdReady()) return false;
  if (!SD.exists("/vault")) SD.mkdir("/vault");
  uint8_t nonce[VaultCrypto::VAULT_NONCE_BYTES]; vaultRandom(nonce, sizeof(nonce));
  uint8_t *container = static_cast<uint8_t *>(heap_caps_malloc(VaultCrypto::VAULT_MAX_FILE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!container) return false;
  size_t containerLength = 0;
  const bool sealed = VaultCrypto::seal(vaultKey, vaultSalt, nonce, plain, length,
                                        container, VaultCrypto::VAULT_MAX_FILE, containerLength);
  VaultCrypto::wipe(nonce, sizeof(nonce));
  bool written = false;
  if (sealed) {
    File file = SD.open("/vault/passwords.new", FILE_WRITE);
    if (file) {
      written = file.write(container, containerLength) == containerLength;
      file.flush(); file.close();
    }
    if (written) {
      if (SD.exists("/vault/passwords.bak")) SD.remove("/vault/passwords.bak");
      if (SD.exists("/vault/passwords.bin")) SD.rename("/vault/passwords.bin", "/vault/passwords.bak");
      written = SD.rename("/vault/passwords.new", "/vault/passwords.bin");
      if (written) vaultExists = true;
    }
    if (!written) SD.remove("/vault/passwords.new");
  }
  VaultCrypto::wipe(container, VaultCrypto::VAULT_MAX_FILE); free(container);
  return written;
}
bool vaultSaveRecords() {
  JsonDocument document;
  document["version"] = 1;
  JsonArray records = document["records"].to<JsonArray>();
  for (uint8_t i = 0; i < vaultCount; ++i) {
    JsonObject record = records.add<JsonObject>();
    record["title"] = vaultRecords[i].title; record["username"] = vaultRecords[i].username;
    record["password"] = vaultRecords[i].password; record["note"] = vaultRecords[i].note;
  }
  uint8_t *plain = static_cast<uint8_t *>(heap_caps_malloc(VaultCrypto::VAULT_MAX_PLAINTEXT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!plain) return false;
  const size_t length = serializeJson(document, plain, VaultCrypto::VAULT_MAX_PLAINTEXT);
  const bool saved = length > 0 && length < VaultCrypto::VAULT_MAX_PLAINTEXT && vaultWriteEncrypted(plain, length);
  VaultCrypto::wipe(plain, VaultCrypto::VAULT_MAX_PLAINTEXT); free(plain);
  return saved;
}
bool vaultUnlockOrCreate(bool create) {
  if (!vaultPinField || vaultUnlocked) return false;
  const char *pin = lv_textarea_get_text(vaultPinField);
  if (!vaultValidPin(pin)) { vaultStatusText("Use a 10-32 digit master PIN; 10 digits is the minimum"); return false; }
  const uint8_t failures = preferences.getUChar("vault_fail", 0);
  if (failures >= 5 && static_cast<int32_t>(millis() - vaultLockUntil) < 0) {
    vaultStatusText("Too many attempts - wait for the cooldown"); return false;
  }
  if (!extSdReady()) { vaultStatusText("Insert a TF card; vault is never stored in internal flash"); return false; }
  vaultStatusText(create ? "Creating authenticated vault..." : "Deriving key and verifying vault...");
  lv_refr_now(nullptr);
  uint8_t *plain = static_cast<uint8_t *>(heap_caps_calloc(VaultCrypto::VAULT_MAX_PLAINTEXT + 1, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!plain) { vaultStatusText("Not enough PSRAM"); return false; }
  bool success = false;
  if (create) {
    if (SD.exists("/vault/passwords.bin")) { free(plain); vaultExists = true; vaultStatusText("Vault already exists - unlock it instead"); return false; }
    vaultRandom(vaultSalt, sizeof(vaultSalt));
    success = VaultCrypto::deriveKey(pin, strlen(pin), vaultSalt, VaultCrypto::VAULT_KDF_ITERATIONS, vaultKey);
    if (success) {
      const char empty[] = "{\"version\":1,\"records\":[]}";
      memcpy(plain, empty, sizeof(empty) - 1);
      vaultUnlocked = true; vaultCount = 0;
      success = vaultWriteEncrypted(plain, sizeof(empty) - 1);
      if (!success) vaultUnlocked = false;
    }
  } else {
    File file = SD.open("/vault/passwords.bin", FILE_READ);
    if (!file) { if (SD.exists("/vault/passwords.bak")) file = SD.open("/vault/passwords.bak", FILE_READ); }
    if (file && file.size() <= VaultCrypto::VAULT_MAX_FILE) {
      uint8_t *container = static_cast<uint8_t *>(heap_caps_malloc(file.size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (container && file.read(container, file.size()) == file.size()) {
        size_t plainLength = 0;
        success = VaultCrypto::open(pin, strlen(pin), container, file.size(), plain,
          VaultCrypto::VAULT_MAX_PLAINTEXT, plainLength, vaultKey, vaultSalt);
        if (success) {
          plain[plainLength] = 0;
          JsonDocument document;
          success = !deserializeJson(document, plain) && document["version"] == 1;
          if (success) {
            vaultCount = 0;
            for (JsonObject record : document["records"].as<JsonArray>()) {
              if (vaultCount >= VAULT_RECORD_LIMIT) break;
              VaultRecord &entry = vaultRecords[vaultCount++];
              strlcpy(entry.title, record["title"] | "", sizeof(entry.title));
              strlcpy(entry.username, record["username"] | "", sizeof(entry.username));
              strlcpy(entry.password, record["password"] | "", sizeof(entry.password));
              strlcpy(entry.note, record["note"] | "", sizeof(entry.note));
            }
          }
        }
      }
      if (container) { VaultCrypto::wipe(container, file.size()); free(container); }
    }
    if (file) file.close();
  }
  VaultCrypto::wipe(plain, VaultCrypto::VAULT_MAX_PLAINTEXT + 1); free(plain);
  vaultClearField(vaultPinField); vaultHideKeyboard();
  if (!success) {
    VaultCrypto::wipe(vaultKey, sizeof(vaultKey));
    const uint8_t next = min<uint8_t>(failures + 1, 20);
    preferences.putUChar("vault_fail", next);
    if (next >= 5) vaultLockUntil = millis() + min<uint32_t>(3600000, 30000UL << min<uint8_t>(next - 5, 7));
    vaultStatusText(create ? "Could not create vault (TF / memory)" : "Wrong PIN or damaged/auth-failed vault; cooldown after 5 failures");
    return false;
  }
  preferences.putUChar("vault_fail", 0); vaultUnlocked = true; markUserActivity();
  lv_obj_add_flag(vaultLockPanel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(vaultListPanel, LV_OBJ_FLAG_HIDDEN);
  vaultUpdateRows(); vaultStatusText("Unlocked - auto-lock after 2 minutes idle");
  return true;
}
void vaultUpdateRows() {
  for (uint8_t i = 0; i < 6; ++i) {
    if (!vaultRows[i]) continue;
    const uint8_t index = vaultListPage * 6 + i;
    lv_label_set_text(lv_obj_get_child(vaultRows[i], 0),
      vaultUnlocked && index < vaultCount ? vaultRecords[index].title : "-");
    if (vaultUnlocked && index < vaultCount) lv_obj_clear_state(vaultRows[i], LV_STATE_DISABLED);
    else lv_obj_add_state(vaultRows[i], LV_STATE_DISABLED);
  }
}
void vaultFieldEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  vaultHideKeyboard();
  vaultKeyboard = lv_keyboard_create(lv_scr_act());
  lv_obj_set_size(vaultKeyboard, SCREEN_WIDTH, 180);
  lv_obj_align(vaultKeyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_textarea_set_password_show_time(vaultPinField, 0);
  lv_keyboard_mode_t mode = lv_event_get_target(event) == vaultPinField ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER;
  lv_keyboard_set_mode(vaultKeyboard, mode);
  styleUtilityKeyboard(vaultKeyboard, lv_color_hex(themeAccent(0xC5A3FF)));
  lv_keyboard_set_textarea(vaultKeyboard, lv_event_get_target(event));
  lv_obj_add_event_cb(vaultKeyboard, [](lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_READY || lv_event_get_code(e) == LV_EVENT_CANCEL) vaultHideKeyboard();
  }, LV_EVENT_ALL, nullptr);
  markUserActivity();
}
void vaultShowEdit(uint8_t index) {
  vaultEditIndex = index;
  const VaultRecord empty;
  const VaultRecord &record = index < vaultCount ? vaultRecords[index] : empty;
  lv_textarea_set_text(vaultTitleField, record.title);
  lv_textarea_set_text(vaultUserField, record.username);
  lv_textarea_set_text(vaultPassField, record.password);
  lv_textarea_set_text(vaultNoteField, record.note);
  vaultHideKeyboard();
  lv_obj_clear_flag(vaultEditPanel, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(vaultEditPanel);
}
void vaultShowDetail() {
  if (!vaultUnlocked || vaultSelected >= vaultCount) return;
  lv_label_set_text(vaultDetailTitle, vaultRecords[vaultSelected].title);
  lv_label_set_text(vaultDetailUser, vaultRecords[vaultSelected].username);
  vaultHideKeyboard();
  lv_obj_clear_flag(vaultDetailPanel, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(vaultDetailPanel);
}
void vaultRowEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED || !vaultUnlocked) return;
  const uint8_t index = vaultListPage * 6 + reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  if (index >= vaultCount) return;
  vaultSelected = index; vaultShowDetail();
}
void vaultSaveEntryEvent(lv_event_t *) {
  if (!vaultUnlocked || !vaultTitleField) return;
  const char *title = lv_textarea_get_text(vaultTitleField);
  const char *password = lv_textarea_get_text(vaultPassField);
  if (!title[0] || !password[0]) { vaultStatusText("Title and password are required"); return; }
  for (const char *p = password; *p; ++p) {
    if (static_cast<uint8_t>(*p) < 32 || static_cast<uint8_t>(*p) > 126) {
      vaultStatusText("USB typing supports printable ASCII only"); return;
    }
#if defined(CONFIG_TINYUSB_HID_ENABLED) && CONFIG_TINYUSB_HID_ENABLED
    const uint8_t *layout = vaultFrenchLayout ? KeyboardLayout_fr_FR : KeyboardLayout_en_US;
    if (!layout[static_cast<uint8_t>(*p)]) { vaultStatusText("A character is unavailable in the selected USB keyboard layout"); return; }
#endif
  }
  const uint8_t index = vaultEditIndex < vaultCount ? vaultEditIndex : vaultCount;
  if (index >= VAULT_RECORD_LIMIT) { vaultStatusText("Vault limit: 24 entries"); return; }
  VaultRecord previous = vaultRecords[index];
  VaultRecord &record = vaultRecords[index];
  strlcpy(record.title, title, sizeof(record.title));
  strlcpy(record.username, lv_textarea_get_text(vaultUserField), sizeof(record.username));
  strlcpy(record.password, password, sizeof(record.password));
  strlcpy(record.note, lv_textarea_get_text(vaultNoteField), sizeof(record.note));
  if (vaultEditIndex == 255) ++vaultCount;
  if (!vaultSaveRecords()) {
    vaultRecords[index] = previous;
    if (vaultEditIndex == 255) --vaultCount;
    VaultCrypto::wipe(&previous, sizeof(previous));
    vaultStatusText("Encrypted TF write failed; edit discarded"); return;
  }
  VaultCrypto::wipe(&previous, sizeof(previous));
  vaultClearField(vaultPassField); vaultHideKeyboard();
  lv_obj_add_flag(vaultEditPanel, LV_OBJ_FLAG_HIDDEN); vaultUpdateRows();
  vaultStatusText("Saved with AES-256-GCM; previous encrypted file kept as .bak");
}
void vaultTypeConfirmed() {
  if (!vaultUnlocked || vaultSelected >= vaultCount) return;
#if defined(CONFIG_TINYUSB_HID_ENABLED) && CONFIG_TINYUSB_HID_ENABLED
  vaultUsbKeyboard.begin(vaultFrenchLayout ? KeyboardLayout_fr_FR : KeyboardLayout_en_US);
  vaultUsbKeyboard.setShiftKeyReports(true);
  if (!tud_hid_n_ready(0)) { vaultStatusText("USB HID not ready - connect Type-C to the target host"); return; }
  const char *password = vaultRecords[vaultSelected].password;
  const uint8_t *layout = vaultFrenchLayout ? KeyboardLayout_fr_FR : KeyboardLayout_en_US;
  for (const char *p = password; *p; ++p)
    if (!layout[static_cast<uint8_t>(*p)]) { vaultStatusText("Unsupported character for this host layout"); return; }
  vaultTyping = true; vaultTypePressed = false; vaultTypeIndex = 0; vaultTypeNextAt = millis();
  lv_obj_add_flag(vaultConfirmPanel, LV_OBJ_FLAG_HIDDEN);
  vaultStatusText("Typing password into the focused host field... Cancel to abort");
#else
  vaultStatusText("USB HID is not enabled in this Arduino core build");
#endif
}
void vaultActionEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  const uintptr_t action = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  markUserActivity();
  if (action == 0) vaultUnlockOrCreate(false);
  else if (action == 1) vaultUnlockOrCreate(true);
  else if (action == 2) vaultLock();
  else if (action == 3 && vaultUnlocked) vaultShowEdit(255);
  else if (action == 4 && vaultUnlocked) vaultShowEdit(vaultSelected);
  else if (action == 5) { vaultHideKeyboard(); lv_obj_add_flag(vaultEditPanel, LV_OBJ_FLAG_HIDDEN); vaultClearField(vaultPassField); }
  else if (action == 6) { vaultHideKeyboard(); lv_obj_add_flag(vaultDetailPanel, LV_OBJ_FLAG_HIDDEN); }
  else if (action == 7 && vaultUnlocked) {
    vaultHideKeyboard(); lv_obj_clear_flag(vaultConfirmPanel, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(vaultConfirmPanel);
  } else if (action == 8) vaultTypeConfirmed();
  else if (action == 9) { vaultStopTyping(); lv_obj_add_flag(vaultConfirmPanel, LV_OBJ_FLAG_HIDDEN); vaultStatusText("Typing cancelled"); }
  else if (action == 10) { vaultListPage = max(0, static_cast<int>(vaultListPage) - 1); vaultUpdateRows(); }
  else if (action == 11) { if ((vaultListPage + 1) * 6 < vaultCount) ++vaultListPage; vaultUpdateRows(); }
  else if (action == 12) {
    vaultFrenchLayout = !vaultFrenchLayout; preferences.putBool("vault_fr", vaultFrenchLayout);
    vaultStatusText(vaultFrenchLayout ? "USB layout: AZERTY (France)" : "USB layout: QWERTY (US)");
  } else if (action == 13) { vaultStopTyping(); vaultStatusText("Typing aborted; all keys released"); }
}
void createVaultPage() {
  lv_obj_t *page = extPage(EXT_VAULT, "Password vault", 0xC5A3FF);
  vaultFrenchLayout = preferences.getBool("vault_fr", false);
  vaultExists = extSdReady() && (SD.exists("/vault/passwords.bin") || SD.exists("/vault/passwords.bak"));
  vaultStatus = extLabel(page, "AES-256-GCM on TF - USB typing requires confirmation", 24, 86, 432, 16);
  lv_obj_set_height(vaultStatus, 40);
  vaultLockPanel = lv_obj_create(page);
  lv_obj_remove_style_all(vaultLockPanel); lv_obj_set_pos(vaultLockPanel, 0, 126); lv_obj_set_size(vaultLockPanel, 480, 354);
  lv_obj_clear_flag(vaultLockPanel, LV_OBJ_FLAG_SCROLLABLE);
  extLabel(vaultLockPanel, vaultExists ? "Enter master PIN" : "Create a new vault", 24, 0, 432, 24, true);
  vaultPinField = extField(vaultLockPanel, "10-32 digit master PIN", 24, 48, 432, true);
  lv_textarea_set_max_length(vaultPinField, 32);
  lv_textarea_set_password_show_time(vaultPinField, 0);
  lv_obj_add_event_cb(vaultPinField, vaultFieldEvent, LV_EVENT_CLICKED, nullptr);
  extButton(vaultLockPanel, "Unlock", 24, 116, 204, 52, vaultActionEvent, 0, true);
  extButton(vaultLockPanel, "Create vault", 244, 116, 212, 52, vaultActionEvent, 1);
  extLabel(vaultLockPanel, "PBKDF2-HMAC-SHA256, 600,000 rounds. No PIN recovery. Keep a backup of the encrypted TF file. A stolen unlocked device is not protected.", 24, 188, 432, 16);
  vaultListPanel = lv_obj_create(page);
  lv_obj_remove_style_all(vaultListPanel); lv_obj_set_pos(vaultListPanel, 0, 126); lv_obj_set_size(vaultListPanel, 480, 354);
  lv_obj_clear_flag(vaultListPanel, LV_OBJ_FLAG_SCROLLABLE);
  for (uint8_t i = 0; i < 6; ++i) {
    vaultRows[i] = extButton(vaultListPanel, "-", 24, i * 38, 432, 34, vaultRowEvent, i);
    lv_obj_set_width(lv_obj_get_child(vaultRows[i], 0), 404);
    lv_label_set_long_mode(lv_obj_get_child(vaultRows[i], 0), LV_LABEL_LONG_DOT);
  }
  extButton(vaultListPanel, "Add", 24, 240, 86, 46, vaultActionEvent, 3, true);
  extButton(vaultListPanel, "Lock", 118, 240, 86, 46, vaultActionEvent, 2);
  extButton(vaultListPanel, "US / FR", 212, 240, 124, 46, vaultActionEvent, 12);
  extButton(vaultListPanel, "<", 344, 240, 52, 46, vaultActionEvent, 10);
  extButton(vaultListPanel, ">", 404, 240, 52, 46, vaultActionEvent, 11);
  extLabel(vaultListPanel, "Auto-lock: 2 min idle / exit / sleep. Passwords are never shown or logged.", 24, 302, 432, 16);
  lv_obj_add_flag(vaultListPanel, LV_OBJ_FLAG_HIDDEN);
  vaultDetailPanel = lv_obj_create(page);
  lv_obj_remove_style_all(vaultDetailPanel); lv_obj_set_size(vaultDetailPanel, 480, 480);
  lv_obj_set_style_bg_color(vaultDetailPanel, themePageBackground(), 0); lv_obj_set_style_bg_opa(vaultDetailPanel, LV_OPA_COVER, 0);
  lv_obj_clear_flag(vaultDetailPanel, LV_OBJ_FLAG_SCROLLABLE);
  vaultDetailTitle = extLabel(vaultDetailPanel, "", 24, 32, 350, 28, true);
  vaultDetailUser = extLabel(vaultDetailPanel, "", 24, 92, 432, 20);
  extLabel(vaultDetailPanel, "Password hidden. Focus the password field on the USB host before typing.", 24, 150, 432, 20);
  extButton(vaultDetailPanel, "Type password", 24, 236, 432, 56, vaultActionEvent, 7, true);
  extButton(vaultDetailPanel, "Edit entry", 24, 308, 208, 52, vaultActionEvent, 4);
  extButton(vaultDetailPanel, "Cancel typing", 248, 308, 208, 52, vaultActionEvent, 13);
  extButton(vaultDetailPanel, "Back to list", 24, 392, 432, 52, vaultActionEvent, 6);
  lv_obj_add_flag(vaultDetailPanel, LV_OBJ_FLAG_HIDDEN);
  vaultEditPanel = lv_obj_create(page);
  lv_obj_remove_style_all(vaultEditPanel); lv_obj_set_size(vaultEditPanel, 480, 480);
  lv_obj_set_style_bg_color(vaultEditPanel, themePageBackground(), 0); lv_obj_set_style_bg_opa(vaultEditPanel, LV_OPA_COVER, 0);
  lv_obj_clear_flag(vaultEditPanel, LV_OBJ_FLAG_SCROLLABLE);
  extLabel(vaultEditPanel, "Encrypted entry", 24, 16, 432, 24, true);
  vaultTitleField = extField(vaultEditPanel, "Title", 24, 56, 432);
  vaultUserField = extField(vaultEditPanel, "Username", 24, 116, 432);
  vaultPassField = extField(vaultEditPanel, "Password (printable ASCII)", 24, 176, 432, true);
  vaultNoteField = extField(vaultEditPanel, "Note (encrypted)", 24, 236, 432);
  lv_textarea_set_max_length(vaultTitleField, 48); lv_textarea_set_max_length(vaultUserField, 64);
  lv_textarea_set_max_length(vaultPassField, 128); lv_textarea_set_max_length(vaultNoteField, 96);
  lv_obj_t *fields[] = {vaultTitleField, vaultUserField, vaultPassField, vaultNoteField};
  for (lv_obj_t *field : fields) lv_obj_add_event_cb(field, vaultFieldEvent, LV_EVENT_CLICKED, nullptr);
  extButton(vaultEditPanel, "Save encrypted", 24, 300, 270, 48, vaultSaveEntryEvent, 0, true);
  extButton(vaultEditPanel, "Cancel", 310, 300, 146, 48, vaultActionEvent, 5);
  lv_obj_add_flag(vaultEditPanel, LV_OBJ_FLAG_HIDDEN);
  vaultConfirmPanel = lv_obj_create(page);
  lv_obj_remove_style_all(vaultConfirmPanel); lv_obj_set_size(vaultConfirmPanel, 480, 480);
  lv_obj_set_style_bg_color(vaultConfirmPanel, themePageBackground(), 0); lv_obj_set_style_bg_opa(vaultConfirmPanel, LV_OPA_COVER, 0);
  lv_obj_clear_flag(vaultConfirmPanel, LV_OBJ_FLAG_SCROLLABLE);
  extLabel(vaultConfirmPanel, "Confirm USB typing", 24, 60, 432, 28, true);
  extLabel(vaultConfirmPanel, "Only type into a trusted host with the password field focused. The clock acts as a real keyboard. No Enter is sent.", 24, 124, 432, 22);
  extButton(vaultConfirmPanel, "Type now", 24, 280, 432, 56, vaultActionEvent, 8, true);
  extButton(vaultConfirmPanel, "Cancel", 24, 352, 432, 52, vaultActionEvent, 9);
  lv_obj_add_flag(vaultConfirmPanel, LV_OBJ_FLAG_HIDDEN);
#if defined(CONFIG_TINYUSB_HID_ENABLED) && CONFIG_TINYUSB_HID_ENABLED
  vaultUsbKeyboard.begin(vaultFrenchLayout ? KeyboardLayout_fr_FR : KeyboardLayout_en_US);
  vaultKeyboardReady = true;
#endif
}
void serviceVault() {
  if (!extensionVisible[EXT_VAULT]) return;
  if (vaultUnlocked && !vaultTyping && millis() - lastUserActivityAt > 120000) { vaultLock(); return; }
  if (!vaultTyping) return;
#if defined(CONFIG_TINYUSB_HID_ENABLED) && CONFIG_TINYUSB_HID_ENABLED
  if (!vaultUnlocked || vaultSelected >= vaultCount || !tud_hid_n_ready(0)) {
    vaultStopTyping(); vaultStatusText("Typing stopped: locked or USB disconnected"); return;
  }
  if (static_cast<int32_t>(millis() - vaultTypeNextAt) < 0) return;
  markUserActivity();
  const char *password = vaultRecords[vaultSelected].password;
  if (!password[vaultTypeIndex]) {
    vaultStopTyping(); vaultStatusText("Password typed; all keys released"); return;
  }
  const uint8_t character = password[vaultTypeIndex];
  const size_t result = vaultTypePressed ? vaultUsbKeyboard.release(character) : vaultUsbKeyboard.press(character);
  if (!result) { vaultStopTyping(); vaultStatusText("Typing aborted: USB report / layout error"); return; }
  if (vaultTypePressed) ++vaultTypeIndex;
  vaultTypePressed = !vaultTypePressed; vaultTypeNextAt = millis() + 12;
#endif
}
void cleanupVault() {
  vaultLock();
  vaultStatus = vaultLockPanel = vaultListPanel = vaultDetailPanel = vaultEditPanel =
    vaultConfirmPanel = vaultPinField = vaultTitleField = vaultUserField = vaultPassField =
    vaultNoteField = vaultDetailTitle = vaultDetailUser = nullptr;
  for (lv_obj_t *&row : vaultRows) row = nullptr;
}
