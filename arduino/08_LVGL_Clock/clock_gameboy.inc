// Original Game Boy (DMG), MIT Peanut-GB. No proprietary runtime or ROMs.
// CGB-only cartridges are rejected rather than silently mis-emulated.
#define ENABLE_SOUND 0
#define ENABLE_LCD 1
#define PEANUT_GB_12_COLOUR 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 1
#define PEANUT_GB_IS_LITTLE_ENDIAN 1
#include "third_party/peanut_gb/peanut_gb.h"

constexpr uint8_t GB_ROM_LIMIT = 24;
String gbRomPaths[GB_ROM_LIMIT];
uint8_t gbRomCount = 0;
gb_s *gbCore = nullptr;
uint8_t *gbRom = nullptr, *gbRam = nullptr;
uint32_t gbRomSize = 0, gbRamSize = 0;
String gbRomPath, gbSavePath;
bool gbRunning = false, gbPaused = false, gbSaveDirty = false, gbFatal = false;
uint64_t gbNextFrameUs = 0, gbLastSaveAt = 0;
uint8_t gbButtons = 0;
lv_obj_t *gbCanvas = nullptr, *gbList = nullptr, *gbStatus = nullptr;
bool gbLoadRom(const String &path);
void gbBuildList();
uint16_t *gbPixels = nullptr;
lv_color_t gbPalette[4];

uint8_t gbReadRom(gb_s *, const uint_fast32_t address) {
  return address < gbRomSize ? gbRom[address] : 0xFF;
}
uint8_t gbReadRam(gb_s *, const uint_fast32_t address) {
  return address < gbRamSize ? gbRam[address] : 0xFF;
}
void gbWriteRam(gb_s *, const uint_fast32_t address, const uint8_t value) {
  if (address < gbRamSize) { gbRam[address] = value; gbSaveDirty = true; }
}
void gbError(gb_s *, const gb_error_e, const uint16_t) { gbFatal = true; }
void gbDrawLine(gb_s *, const uint8_t *pixels, const uint_fast8_t line) {
  if (!gbPixels || line >= 144) return;
  for (uint8_t row = 0; row < 2; ++row) {
    uint16_t *destination = gbPixels + (line * 2 + row) * 320;
    for (uint8_t x = 0; x < 160; ++x) {
      const uint16_t color = gbPalette[pixels[x] & 3].full;
      destination[x * 2] = color; destination[x * 2 + 1] = color;
    }
  }
}
void gbReleaseResources() {
  gbRunning = false;
  if (gbCore) { free(gbCore); gbCore = nullptr; }
  if (gbRom) { free(gbRom); gbRom = nullptr; }
  if (gbRam) { free(gbRam); gbRam = nullptr; }
  if (gbPixels) { free(gbPixels); gbPixels = nullptr; }
  gbRomSize = gbRamSize = 0;
}
bool gbPersistSave() {
  if (!gbSaveDirty || !gbRam || !gbRamSize || !extSdReady()) return false;
  const String temporary = gbSavePath + ".tmp";
  const String backup = gbSavePath + ".bak";
  File file = SD.open(temporary.c_str(), FILE_WRITE);
  if (!file) return false;
  const size_t written = file.write(gbRam, gbRamSize);
  file.flush(); file.close();
  if (written != gbRamSize) { SD.remove(temporary.c_str()); return false; }
  if (SD.exists(backup.c_str())) SD.remove(backup.c_str());
  if (SD.exists(gbSavePath.c_str())) SD.rename(gbSavePath.c_str(), backup.c_str());
  if (!SD.rename(temporary.c_str(), gbSavePath.c_str())) return false;
  gbSaveDirty = false; return true;
}
void gbScanRoms() {
  gbRomCount = 0;
  if (!extSdReady()) return;
  if (!SD.exists("/roms")) SD.mkdir("/roms");
  File directory = SD.open("/roms"), entry;
  while (directory && gbRomCount < GB_ROM_LIMIT && (entry = directory.openNextFile())) {
    String path = entry.path();
    String lower = path; lower.toLowerCase();
    if (!entry.isDirectory() && lower.endsWith(".gb") && entry.size() <= 8UL * 1024UL * 1024UL)
      gbRomPaths[gbRomCount++] = path;
    entry.close();
  }
  if (directory) directory.close();
}
void gbBuildList() {
  if (!gbList) return;
  lv_obj_clean(gbList);
  extLabel(gbList, "Game Boy - choose a .gb ROM", 24, 20, 432, 24, true);
  extLabel(gbList, "DMG only - /roms on TF - no games included - silent mode", 24, 58, 432, 16);
  gbScanRoms();
  for (uint8_t i = 0; i < min<uint8_t>(gbRomCount, 8); ++i) {
    lv_obj_t *button = extButton(gbList, gbRomPaths[i].substring(6).c_str(), 24, 100 + i * 42, 432, 36,
      [](lv_event_t *event) {
        if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
        const uint8_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
        if (index < gbRomCount && gbLoadRom(gbRomPaths[index])) lv_obj_add_flag(gbList, LV_OBJ_FLAG_HIDDEN);
      }, i);
    lv_obj_set_width(lv_obj_get_child(button, 0), 404);
    lv_label_set_long_mode(lv_obj_get_child(button, 0), LV_LABEL_LONG_DOT);
  }
  if (!gbRomCount) extLabel(gbList, "Copy legally owned / homebrew .gb files into /roms, then reopen this app.", 24, 130, 432, 20);
  extButton(gbList, "Refresh", 24, 440, 432, 36, [](lv_event_t *) { gbBuildList(); });
}
bool gbLoadRom(const String &path) {
  gbPersistSave(); gbReleaseResources();
  if (!extSdReady()) return false;
  File file = SD.open(path.c_str(), FILE_READ);
  if (!file || file.size() < 32768 || file.size() > 8UL * 1024UL * 1024UL) { if (file) file.close(); return false; }
  gbRomSize = file.size();
  gbRom = static_cast<uint8_t *>(heap_caps_malloc(gbRomSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  gbCore = static_cast<gb_s *>(heap_caps_calloc(1, sizeof(gb_s), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  gbPixels = static_cast<uint16_t *>(heap_caps_calloc(320 * 288, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!gbRom || !gbCore || !gbPixels || file.read(gbRom, gbRomSize) != gbRomSize) {
    file.close(); gbReleaseResources();
    if (gbStatus) lv_label_set_text(gbStatus, "ROM / RAM allocation failed"); return false;
  }
  file.close();
  if ((gbRom[0x143] & 0x80) || gbRom[0x148] > 8 || gbRom[0x149] > 5) {
    gbReleaseResources();
    if (gbStatus) lv_label_set_text(gbStatus, "Unsupported ROM: this core is original Game Boy, not GBC"); return false;
  }
  const gb_init_error_e result = gb_init(gbCore, gbReadRom, gbReadRam, gbWriteRam, gbError, nullptr);
  if (result != GB_INIT_NO_ERROR) {
    gbReleaseResources();
    if (gbStatus) lv_label_set_text(gbStatus, "ROM checksum / cartridge mapper unsupported"); return false;
  }
  size_t saveSize = 0;
  gb_get_save_size_s(gbCore, &saveSize);
  gbRamSize = min<size_t>(saveSize, 131072);
  if (gbRamSize) {
    gbRam = static_cast<uint8_t *>(heap_caps_calloc(gbRamSize, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!gbRam) { gbReleaseResources(); return false; }
  }
  gbRomPath = path; gbSavePath = path;
  gbSavePath.remove(gbSavePath.length() - 3); gbSavePath += ".sav";
  if (gbRamSize) {
    File save = SD.open(gbSavePath.c_str(), FILE_READ);
    if (!save || save.size() != gbRamSize) {
      if (save) save.close();
      save = SD.open((gbSavePath + ".bak").c_str(), FILE_READ);
    }
    if (save && save.size() == gbRamSize) save.read(gbRam, gbRamSize);
    if (save) save.close();
  }
  gb_init_lcd(gbCore, gbDrawLine);
  gbCore->direct.joypad = 0xFF;
  gbCanvas = lv_canvas_create(extensionPages[EXT_GAMEBOY]);
  lv_canvas_set_buffer(gbCanvas, gbPixels, 320, 288, LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(gbCanvas, 80, 84);
  lv_obj_clear_flag(gbCanvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  gbRunning = true; gbPaused = gbFatal = gbSaveDirty = false; gbButtons = 0;
  gbNextFrameUs = esp_timer_get_time(); gbLastSaveAt = millis();
  if (gbStatus) lv_label_set_text(gbStatus, "DMG - 30 fps target - silent - battery save");
  return true;
}
void gbControlEvent(lv_event_t *event) {
  const uint8_t bit = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  if (bit >= 8) return;
  if (lv_event_get_code(event) == LV_EVENT_PRESSED) gbButtons |= 1U << bit;
  else if (lv_event_get_code(event) == LV_EVENT_RELEASED || lv_event_get_code(event) == LV_EVENT_PRESS_LOST)
    gbButtons &= ~(1U << bit);
}
void gbActionEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  const uintptr_t action = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  if (action == 0) { gbPaused = !gbPaused; gbButtons = 0; }
  else if (action == 1) {
    if (gbStatus) lv_label_set_text(gbStatus, gbPersistSave() ? "Battery save written to TF" : "No save needed / TF save failed");
  } else if (action == 2) {
    gbPersistSave(); gbReleaseResources();
    if (gbCanvas) { lv_obj_del(gbCanvas); gbCanvas = nullptr; }
    lv_obj_clear_flag(gbList, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(gbList); gbBuildList();
  }
}
void createGameboyPage() {
  lv_obj_t *page = extPage(EXT_GAMEBOY, "Game Boy", 0x9DE879);
  gbCanvas = nullptr; gbStatus = nullptr;
  const uint32_t accent = themeAccent(0x9DE879);
  gbPalette[0] = lv_color_hex(themeIsAmber() ? 0xFFB000 : 0xE0F8D0);
  gbPalette[1] = lv_color_hex(themeIsAmber() ? 0xB87500 : 0x88C070);
  gbPalette[2] = lv_color_hex(themeIsAmber() ? 0x6A4000 : 0x346856);
  gbPalette[3] = lv_color_hex(themeIsAmber() ? 0x1A1000 : 0x081820);
  const struct { const char *label; int x, y, w, h; uint8_t bit; } controls[] = {
    {"^", 66, 378, 44, 40, 6}, {"v", 66, 426, 44, 40, 7},
    {"<", 18, 402, 44, 40, 5}, {">", 114, 402, 44, 40, 4},
    {"B", 330, 414, 56, 52, 1}, {"A", 400, 390, 56, 52, 0},
    {"SEL", 178, 438, 66, 32, 2}, {"STA", 254, 438, 66, 32, 3},
  };
  for (const auto &control : controls) {
    lv_obj_t *button = extButton(page, control.label, control.x, control.y, control.w, control.h, gbControlEvent, control.bit);
    lv_obj_add_event_cb(button, gbControlEvent, LV_EVENT_PRESSED, reinterpret_cast<void *>(static_cast<uintptr_t>(control.bit)));
    lv_obj_add_event_cb(button, gbControlEvent, LV_EVENT_RELEASED, reinterpret_cast<void *>(static_cast<uintptr_t>(control.bit)));
    lv_obj_add_event_cb(button, gbControlEvent, LV_EVENT_PRESS_LOST, reinterpret_cast<void *>(static_cast<uintptr_t>(control.bit)));
  }
  extButton(page, "Pause", 174, 378, 72, 44, gbActionEvent, 0);
  extButton(page, "Save", 254, 378, 66, 44, gbActionEvent, 1);
  extButton(page, "ROMs", 330, 340, 126, 38, gbActionEvent, 2);
  gbStatus = extLabel(page, "Original GB / DMG - silent - CGB not supported", 24, 462, 432, 16);
  gbList = lv_obj_create(page);
  lv_obj_remove_style_all(gbList); lv_obj_set_size(gbList, 480, 480);
  lv_obj_set_style_bg_color(gbList, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(gbList, LV_OPA_COVER, 0);
  lv_obj_clear_flag(gbList, LV_OBJ_FLAG_SCROLLABLE);
  gbBuildList();
  (void)accent;
}
void serviceGameboy() {
  if (!gbRunning || gbPaused || !gbCore || lowPowerMode) return;
  const uint64_t now = esp_timer_get_time();
  if (now < gbNextFrameUs) return;
  // 30 fps display target on QSPI; no catch-up spiral after a slow SD save.
  gbNextFrameUs = now + 33333;
  gbCore->direct.joypad = 0xFF & ~gbButtons;
  gb_run_frame(gbCore);
  if (gbFatal) {
    gbPaused = true;
    if (gbStatus) lv_label_set_text(gbStatus, "Emulation stopped: unsupported opcode / memory access");
  }
  if (gbCanvas) lv_obj_invalidate(gbCanvas);
  if (gbSaveDirty && millis() - gbLastSaveAt > 3000) {
    gbPersistSave(); gbLastSaveAt = millis();
  }
}
void cleanupGameboy() {
  gbPersistSave(); gbReleaseResources();
  gbCanvas = gbList = gbStatus = nullptr; gbButtons = 0;
}
