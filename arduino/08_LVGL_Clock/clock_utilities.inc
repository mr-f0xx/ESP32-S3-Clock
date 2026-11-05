// Small offline tools. Construct only the current tool to conserve LVGL memory.
// AMOLED-friendly: true-black page, near-black keys and the active theme accent
// for highlights (display, operators, primary actions, keyboard toggles).
lv_obj_t *utilityPage = nullptr;
lv_obj_t *utilityDisplay = nullptr;
lv_obj_t *utilityNote = nullptr;
lv_obj_t *utilityStatus = nullptr;
lv_obj_t *utilityNotePanel = nullptr;
lv_obj_t *utilityNoteKeyboard = nullptr;
uint8_t utilityActive = 255;
UtilityCalculator utilityCalculator;
String utilitySavedNote;
bool stopwatchRunning = false;
uint32_t stopwatchAccumulated = 0;
uint32_t stopwatchStartedAt = 0;

// Notes is a reader first: opening it lists /notes/*.txt from the shared SD
// mount and never constructs the keyboard. The keyboard exists only after an
// explicit New/Edit action. The old single NVS note remains a no-SD fallback
// and is migrated to /notes/legacy-note.txt once the card is available.
constexpr uint8_t UTILITY_NOTE_LIMIT = 24;
constexpr uint16_t UTILITY_NOTE_MAX_LENGTH = 4096;
constexpr char UTILITY_NOTE_DIRECTORY[] = "/notes";
constexpr char UTILITY_NOTE_LEGACY_FILE[] = "/notes/legacy-note.txt";
String utilityNotePaths[UTILITY_NOTE_LIMIT];
uint8_t utilityNoteCount = 0;
int8_t utilityNoteSelected = -1;
String utilityNoteDraft;
String utilityNoteStatusText;
bool utilityNoteEditing = false;
bool utilityNoteDirty = false;
bool utilityNoteOnSd = false;
bool utilityNotesPrepared = false;

void rebuildUtilityPage();
void openUtilityApp(uint8_t app);

// Per-app accent, collapsed onto the theme accent for amber / neon palettes.
uint32_t utilityAccentColor() {
  const uint32_t base = utilityActive == 0 ? UI_ACCENT_BLUE
                      : utilityActive == 1 ? UI_ACCENT_MINT
                      : UI_ACCENT_ROSE;
  return themeAccent(base);
}

// kind: 0 = neutral key, 1 = accent outline (operators / secondary), 2 = filled
// accent primary action (=, Start, Save).
lv_obj_t *utilityButton(lv_obj_t *parent, const char *text, int x, int y,
                        int w, int h, lv_event_cb_t cb, uintptr_t data = 0,
                        uint8_t kind = 0) {
  const lv_color_t accent = lv_color_hex(utilityAccentColor());
  lv_obj_t *button = lv_btn_create(parent);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, w, h);
  lv_obj_set_style_radius(button, themeControlRadius(14), 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  if (kind == 2) {
    lv_obj_set_style_bg_color(button, accent, 0);
    lv_obj_set_style_border_width(button, 0, 0);
  } else {
    lv_obj_set_style_bg_color(button, themeControlSurface(utilityAccentColor()), 0);
    lv_obj_set_style_border_width(button, kind == 1 ? 2 : 1, 0);
    lv_obj_set_style_border_color(button,
      kind == 1 ? accent : lv_color_hex(themeBorder(0x2A3040)), 0);
    lv_obj_set_style_border_opa(button, kind == 1 ? LV_OPA_COVER : LV_OPA_60, 0);
  }
  lv_obj_set_style_bg_color(button, accent, LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(button, kind == 2 ? LV_OPA_80 : LV_OPA_30,
                          LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, reinterpret_cast<void *>(data));
  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, &lv_font_ui_20, 0);
  lv_obj_set_style_text_color(label,
    kind == 2 ? themeTextOnAccent() : (kind == 1 ? accent : themeText()), 0);
  lv_obj_center(label);
  return button;
}

void styleUtilityNote(lv_obj_t *note, lv_color_t accent) {
  lv_obj_set_style_bg_color(note, lv_color_hex(themeSurface(0x0A0A0F)), 0);
  lv_obj_set_style_bg_opa(note, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(note, themeControlRadius(12), 0);
  lv_obj_set_style_border_width(note, 1, 0);
  lv_obj_set_style_border_color(note, accent, 0);
  lv_obj_set_style_text_color(note, themeText(), 0);
  lv_obj_set_style_text_font(note, &lv_font_ui_20, 0);
  lv_obj_set_style_bg_color(note, accent, LV_PART_CURSOR);
  lv_obj_set_style_border_color(note, accent, LV_PART_CURSOR);
  lv_obj_set_style_text_color(note, themeTextDim(0x8A93A6), LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_bg_color(note, accent, LV_PART_SELECTED);
  lv_obj_set_style_text_color(note, themeTextOnAccent(), LV_PART_SELECTED);
}

void styleUtilityKeyboard(lv_obj_t *keyboard, lv_color_t accent) {
  lv_obj_set_style_bg_color(keyboard, lv_color_hex(themeSurface(0x050508)), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(keyboard, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(keyboard, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(keyboard, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(keyboard, lv_color_hex(themeSurface(0x101018)), LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_text_color(keyboard, themeText(), LV_PART_ITEMS);
  lv_obj_set_style_border_color(keyboard, lv_color_hex(themeBorder(0x202634)), LV_PART_ITEMS);
  lv_obj_set_style_border_width(keyboard, 1, LV_PART_ITEMS);
  lv_obj_set_style_radius(keyboard, themeControlRadius(6), LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(keyboard, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(keyboard, accent, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(keyboard, themeTextOnAccent(), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(keyboard, accent, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_set_style_text_color(keyboard, themeTextOnAccent(), LV_PART_ITEMS | LV_STATE_PRESSED);
}

void setUtilityNoteStatus(const String &text) {
  utilityNoteStatusText = text;
  if (utilityStatus) lv_label_set_text(utilityStatus, utilityNoteStatusText.c_str());
}

// Notes writes/remounts the same FAT volume used by the SD audio players. Stop
// those readers before a destructive SD operation instead of unmounting under
// an open AudioFileSourceSD.
void utilityReleaseSdAudio() {
  stopMusicPlayback();
  stopAudiobookPlayback();
}

String utilityNoteBaseName(const String &path) {
  const int slash = path.lastIndexOf('/');
  String name = slash >= 0 ? path.substring(slash + 1) : path;
  if (name.length() > 4) {
    String suffix = name.substring(name.length() - 4);
    suffix.toLowerCase();
    if (suffix == ".txt") name = name.substring(0, name.length() - 4);
  }
  return name;
}

bool utilityNoteIsTextFile(const String &path) {
  const String name = utilityNoteBaseName(path);
  if (!name.length() || name[0] == '.') return false;
  String lower = path;
  lower.toLowerCase();
  return lower.endsWith(".txt");
}

bool utilityNoteReadFile(const String &path, String &text) {
  text = "";
  File file = SD.open(path.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return false;
  }
  const size_t size = file.size();
  if (size > UTILITY_NOTE_MAX_LENGTH) {
    file.close();
    return false;
  }
  text.reserve(size);
  char buffer[128];
  size_t remaining = size;
  while (remaining > 0 && file.available()) {
    const size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    const size_t got = file.readBytes(buffer, want);
    if (!got) break;
    text.concat(buffer, static_cast<unsigned int>(got));
    remaining -= got;
  }
  file.close();
  return remaining == 0;
}

// Write to a temporary file, verify it, keep the previous revision as .bak,
// then rename into place. A failed rename restores the backup rather than
// leaving the visible note half-written.
bool utilityNoteWriteFileAtomic(const String &path, const String &text) {
  if (text.length() > UTILITY_NOTE_MAX_LENGTH) return false;
  const String temporary = path + ".tmp";
  const String backup = path + ".bak";
  SD.remove(temporary.c_str());
  File file = SD.open(temporary.c_str(), FILE_WRITE);
  if (!file) return false;
  const size_t written = file.print(text);
  file.flush();
  file.close();
  if (written != text.length()) {
    SD.remove(temporary.c_str());
    return false;
  }
  String verified;
  if (!utilityNoteReadFile(temporary, verified) || verified != text) {
    SD.remove(temporary.c_str());
    return false;
  }
  const bool hadOriginal = SD.exists(path.c_str());
  if (hadOriginal) {
    SD.remove(backup.c_str());
    if (!SD.rename(path.c_str(), backup.c_str())) {
      SD.remove(temporary.c_str());
      return false;
    }
  }
  if (!SD.rename(temporary.c_str(), path.c_str())) {
    if (hadOriginal) SD.rename(backup.c_str(), path.c_str());
    SD.remove(temporary.c_str());
    return false;
  }
  return true;
}

void utilityNoteScanLibrary() {
  utilityNoteCount = 0;
  if (!SD.exists(UTILITY_NOTE_DIRECTORY) && !SD.mkdir(UTILITY_NOTE_DIRECTORY)) return;
  File directory = SD.open(UTILITY_NOTE_DIRECTORY);
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return;
  }
  File entry;
  while (utilityNoteCount < UTILITY_NOTE_LIMIT && (entry = directory.openNextFile())) {
    if (!entry.isDirectory()) {
      const char *entryPath = entry.path();
      String path = entryPath ? entryPath : entry.name();
      if (!path.startsWith("/")) path = String(UTILITY_NOTE_DIRECTORY) + "/" + path;
      if (utilityNoteIsTextFile(path) && entry.size() <= UTILITY_NOTE_MAX_LENGTH)
        utilityNotePaths[utilityNoteCount++] = path;
    }
    entry.close();
  }
  directory.close();
  for (uint8_t i = 0; i < utilityNoteCount; ++i)
    for (uint8_t j = i + 1; j < utilityNoteCount; ++j)
      if (utilityNotePaths[j].compareTo(utilityNotePaths[i]) < 0) {
        const String swap = utilityNotePaths[i];
        utilityNotePaths[i] = utilityNotePaths[j];
        utilityNotePaths[j] = swap;
      }
}

bool utilityNoteMigrateLegacy(const String &legacy) {
  if (!legacy.length()) return true;
  if (SD.exists(UTILITY_NOTE_LEGACY_FILE)) return true;
  if (!utilityNoteWriteFileAtomic(UTILITY_NOTE_LEGACY_FILE, legacy)) return false;
  String verified;
  if (!utilityNoteReadFile(UTILITY_NOTE_LEGACY_FILE, verified) || verified != legacy)
    return false;
  preferences.remove("utility_note");
  return true;
}

void prepareUtilityNotes(bool forceRemount) {
  utilityNotesPrepared = true;
  utilityNoteEditing = false;
  utilityNoteDirty = false;
  utilityNoteSelected = -1;
  utilityNoteOnSd = false;
  utilityNoteDraft = "";
  utilitySavedNote = "";
  utilityNoteCount = 0;
  const String legacy = preferences.getString("utility_note", "");
  if (!weatherWallpaperSdReadyForOtherUsers()) {
    utilitySavedNote = legacy;
    utilityNoteDraft = legacy;
    setUtilityNoteStatus(tr(STR_SD_BUSY));
    return;
  }
  if (forceRemount) utilityReleaseSdAudio();
  const bool mounted = forceRemount ? remountMusicSd() : mountMusicSd();
  if (!mounted) {
    utilitySavedNote = legacy;
    utilityNoteDraft = legacy;
    setUtilityNoteStatus(tr(STR_SD_UNAVAILABLE));
    return;
  }
  const bool migrated = utilityNoteMigrateLegacy(legacy);
  utilityNoteScanLibrary();
  if (utilityNoteCount) {
    char text[48];
    snprintf(text, sizeof(text), tr(STR_NOTE_COUNT_FMT),
             static_cast<unsigned>(utilityNoteCount));
    setUtilityNoteStatus(text);
  } else if (legacy.length() && !migrated) {
    utilitySavedNote = legacy;
    utilityNoteDraft = legacy;
    setUtilityNoteStatus(tr(STR_NOTE_SAVE_FAILED));
  } else {
    setUtilityNoteStatus(tr(STR_NOTE_EMPTY));
  }
}

String utilityNoteNextPath() {
  for (uint16_t index = 1; index <= 999; ++index) {
    char path[40];
    snprintf(path, sizeof(path), "%s/note-%03u.txt", UTILITY_NOTE_DIRECTORY,
             static_cast<unsigned>(index));
    if (!SD.exists(path)) return String(path);
  }
  return String();
}

bool saveUtilityNote() {
  if (!utilityNote) return true;
  utilityNoteDraft = lv_textarea_get_text(utilityNote);
  if (!utilityNoteDirty && utilityNoteDraft == utilitySavedNote) return true;
  if (!utilityNoteDraft.length() && !utilityNoteOnSd && utilityNoteSelected < 0) {
    setUtilityNoteStatus(tr(STR_NOTE_EMPTY));
    return false;
  }
  if (weatherWallpaperSdReadyForOtherUsers() && mountMusicSd()) {
    utilityReleaseSdAudio();
    const String path = utilityNoteOnSd && utilityNoteSelected >= 0 &&
                        static_cast<uint8_t>(utilityNoteSelected) < utilityNoteCount
                          ? utilityNotePaths[utilityNoteSelected]
                          : utilityNoteNextPath();
    if (!path.length() || !utilityNoteWriteFileAtomic(path, utilityNoteDraft)) {
      setUtilityNoteStatus(tr(STR_NOTE_SAVE_FAILED));
      return false;
    }
    if (!utilityNoteOnSd) preferences.remove("utility_note");
    utilitySavedNote = utilityNoteDraft;
    utilityNoteDirty = false;
    utilityNoteOnSd = true;
    utilityNoteScanLibrary();
    utilityNoteSelected = -1;
    for (uint8_t i = 0; i < utilityNoteCount; ++i)
      if (utilityNotePaths[i] == path) utilityNoteSelected = static_cast<int8_t>(i);
    setUtilityNoteStatus(tr(STR_NOTE_SD_SAVED));
    return true;
  }
  // An SD-backed note must not silently fall back to a different local note.
  if (utilityNoteOnSd) {
    setUtilityNoteStatus(tr(STR_SD_UNAVAILABLE));
    return false;
  }
  if (preferences.putString("utility_note", utilityNoteDraft) == 0) {
    setUtilityNoteStatus(tr(STR_NOTE_SAVE_FAILED));
    return false;
  }
  utilitySavedNote = utilityNoteDraft;
  utilityNoteDirty = false;
  utilityNoteSelected = -1;
  setUtilityNoteStatus(tr(STR_NOTE_LOCAL_SAVED));
  return true;
}

void utilityBackEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED || !utilityPage) return;
  markUserActivity();
  if (utilityActive == 1) {
    if (utilityNoteEditing) {
      if (!saveUtilityNote()) return;
      utilityNoteEditing = false;
      consumeCurrentTouchRelease();
      rebuildUtilityPage();
      return;
    }
    if (utilityNoteSelected >= 0) {
      utilityNoteSelected = -1;
      char text[48];
      snprintf(text, sizeof(text), tr(STR_NOTE_COUNT_FMT),
               static_cast<unsigned>(utilityNoteCount));
      setUtilityNoteStatus(utilityNoteCount ? String(text) : String(tr(STR_NOTE_EMPTY)));
      consumeCurrentTouchRelease();
      rebuildUtilityPage();
      return;
    }
  }
  consumeCurrentTouchRelease();
  appMenuClickSuppressedUntil = millis() + 350;
  lv_obj_del_async(utilityPage);
  utilityPage = utilityDisplay = utilityNote = utilityStatus = nullptr;
  utilityNotePanel = utilityNoteKeyboard = nullptr;
  utilityActive = 255;
}

void utilitySaveEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED || !utilityNote) return;
  markUserActivity();
  if (!saveUtilityNote()) return;
  utilityNoteEditing = false;
  consumeCurrentTouchRelease();
  rebuildUtilityPage();
}

void utilityNoteChanged(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
  utilityNoteDirty = true;
  setUtilityNoteStatus("");
}

void utilityNoteListEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  const uint8_t index = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
  if (index >= utilityNoteCount) return;
  String text;
  if (!utilityNoteReadFile(utilityNotePaths[index], text)) {
    setUtilityNoteStatus(tr(STR_NOTE_OPEN_FAILED));
    return;
  }
  utilityNoteSelected = static_cast<int8_t>(index);
  utilityNoteDraft = utilitySavedNote = text;
  utilityNoteOnSd = true;
  utilityNoteDirty = false;
  setUtilityNoteStatus(utilityNoteBaseName(utilityNotePaths[index]));
  consumeCurrentTouchRelease();
  rebuildUtilityPage();
}

void utilityNoteNewEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  utilityNoteSelected = -1;
  utilityNoteOnSd = false;
  utilityNoteDraft = "";
  utilitySavedNote = "";
  utilityNoteDirty = true;
  utilityNoteEditing = true;
  setUtilityNoteStatus("");
  consumeCurrentTouchRelease();
  rebuildUtilityPage();
}

void utilityNoteEditEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  utilityNoteEditing = true;
  utilityNoteDirty = false;
  setUtilityNoteStatus("");
  consumeCurrentTouchRelease();
  rebuildUtilityPage();
}

void utilityNoteRefreshEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  prepareUtilityNotes(true);
  consumeCurrentTouchRelease();
  rebuildUtilityPage();
}

void utilityCalculatorEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED || !utilityDisplay) return;
  markUserActivity();
  utilityCalculator.key(static_cast<char>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))));
  lv_label_set_text(utilityDisplay, utilityCalculator.display);
}

void utilityStopwatchEvent(lv_event_t *event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
  markUserActivity();
  const uintptr_t action = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  const uint32_t now = millis();
  if (action == 0) {
    if (stopwatchRunning) stopwatchAccumulated += now - stopwatchStartedAt;
    else stopwatchStartedAt = now;
    stopwatchRunning = !stopwatchRunning;
    lv_obj_t *label = lv_obj_get_child(static_cast<lv_obj_t *>(lv_event_get_target(event)), 0);
    lv_label_set_text(label, tr(stopwatchRunning ? STR_TIMER_PAUSE : STR_TIMER_START));
  } else {
    stopwatchAccumulated = 0;
    if (stopwatchRunning) stopwatchStartedAt = now;
  }
  serviceUtilityApps();
}

void serviceUtilityApps() {
  if (utilityActive != 2 || !utilityDisplay || !appMenuVisible) return;
  const uint32_t elapsed = stopwatchAccumulated + (stopwatchRunning ? millis() - stopwatchStartedAt : 0);
  const uint32_t seconds = elapsed / 1000;
  char text[24];
  snprintf(text, sizeof(text), "%02lu:%02lu:%02lu.%01lu",
           static_cast<unsigned long>(seconds / 3600),
           static_cast<unsigned long>((seconds / 60) % 60),
           static_cast<unsigned long>(seconds % 60),
           static_cast<unsigned long>((elapsed / 100) % 10));
  if (strcmp(lv_label_get_text(utilityDisplay), text)) lv_label_set_text(utilityDisplay, text);
}

void buildUtilityNotesReader(lv_obj_t *page, lv_color_t accent) {
  utilityNotePanel = lv_obj_create(page);
  lv_obj_remove_style_all(utilityNotePanel);
  lv_obj_set_pos(utilityNotePanel, 20, 88);
  lv_obj_set_size(utilityNotePanel, 440, 292);
  lv_obj_set_style_bg_color(utilityNotePanel, lv_color_hex(themeSurface(0x0A0A0F)), 0);
  lv_obj_set_style_bg_opa(utilityNotePanel, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(utilityNotePanel, themeControlRadius(12), 0);
  lv_obj_set_style_border_width(utilityNotePanel, 1, 0);
  lv_obj_set_style_border_color(utilityNotePanel, accent, 0);
  lv_obj_set_style_pad_all(utilityNotePanel, 12, 0);
  lv_obj_set_scroll_dir(utilityNotePanel, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(utilityNotePanel, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_clear_flag(utilityNotePanel, LV_OBJ_FLAG_SCROLL_MOMENTUM);

  utilityStatus = lv_label_create(page);
  lv_obj_set_pos(utilityStatus, 20, 388);
  lv_obj_set_size(utilityStatus, 440, 24);
  lv_obj_set_style_text_font(utilityStatus, &lv_font_ui_16, 0);
  lv_obj_set_style_text_color(utilityStatus, accent, 0);
  lv_label_set_long_mode(utilityStatus, LV_LABEL_LONG_DOT);
  lv_label_set_text(utilityStatus, utilityNoteStatusText.c_str());

  const bool showNote = utilityNoteSelected >= 0 ||
                        (!utilityNoteOnSd && utilitySavedNote.length());
  if (showNote) {
    lv_obj_t *body = lv_label_create(utilityNotePanel);
    lv_obj_set_width(body, 416);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(body, &lv_font_ui_20, 0);
    lv_obj_set_style_text_color(body, themeText(), 0);
    lv_label_set_text(body, utilityNoteDraft.length() ? utilityNoteDraft.c_str()
                                                      : tr(STR_NOTE_EMPTY));
    utilityButton(page, tr(STR_NOTE_NEW), 20, 420, 136, 48, utilityNoteNewEvent, 0, 1);
    utilityButton(page, tr(STR_NOTE_EDIT), 172, 420, 136, 48, utilityNoteEditEvent, 0, 2);
    utilityButton(page, tr(STR_NOTE_REFRESH), 324, 420, 136, 48, utilityNoteRefreshEvent, 0, 0);
    return;
  }

  if (!utilityNoteCount) {
    lv_obj_t *empty = lv_label_create(utilityNotePanel);
    lv_obj_set_width(empty, 416);
    lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(empty, &lv_font_ui_20, 0);
    lv_obj_set_style_text_color(empty, themeTextDim(0x8A93A6), 0);
    lv_label_set_text(empty, tr(STR_NOTE_EMPTY_HINT));
    lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 80);
  } else {
    for (uint8_t i = 0; i < utilityNoteCount; ++i) {
      lv_obj_t *row = utilityButton(utilityNotePanel,
                                    utilityNoteBaseName(utilityNotePaths[i]).c_str(),
                                    0, i * 56, 416, 48, utilityNoteListEvent, i, 0);
      lv_obj_t *label = lv_obj_get_child(row, 0);
      lv_obj_set_width(label, 384);
      lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
      lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
      lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);
    }
  }
  utilityButton(page, tr(STR_NOTE_NEW), 20, 420, 212, 48, utilityNoteNewEvent, 0, 2);
  utilityButton(page, tr(STR_NOTE_REFRESH), 248, 420, 212, 48, utilityNoteRefreshEvent, 0, 1);
}

void buildUtilityNotesEditor(lv_obj_t *page, lv_color_t accent) {
  utilityNote = lv_textarea_create(page);
  lv_obj_set_pos(utilityNote, 20, 72);
  lv_obj_set_size(utilityNote, 440, 112);
  lv_textarea_set_max_length(utilityNote, UTILITY_NOTE_MAX_LENGTH);
  lv_textarea_set_text(utilityNote, utilityNoteDraft.c_str());
  styleUtilityNote(utilityNote, accent);
  lv_obj_add_event_cb(utilityNote, utilityNoteChanged, LV_EVENT_VALUE_CHANGED, nullptr);

  utilityStatus = lv_label_create(page);
  lv_obj_set_pos(utilityStatus, 20, 190);
  lv_obj_set_size(utilityStatus, 270, 44);
  lv_obj_set_style_text_font(utilityStatus, &lv_font_ui_16, 0);
  lv_obj_set_style_text_color(utilityStatus, accent, 0);
  lv_label_set_long_mode(utilityStatus, LV_LABEL_LONG_WRAP);
  lv_label_set_text(utilityStatus, utilityNoteStatusText.c_str());
  utilityButton(page, tr(STR_SAVE_NOTE), 310, 190, 150, 44, utilitySaveEvent, 0, 2);

  utilityNoteKeyboard = lv_keyboard_create(page);
  lv_obj_set_size(utilityNoteKeyboard, 480, 238);
  lv_obj_align(utilityNoteKeyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  styleUtilityKeyboard(utilityNoteKeyboard, accent);
  lv_keyboard_set_textarea(utilityNoteKeyboard, utilityNote);
}

void openUtilityApp(uint8_t app) {
  if (app > 2 || utilityPage || !appMenu) return;
  utilityActive = app;
  const lv_color_t accent = lv_color_hex(utilityAccentColor());
  utilityPage = lv_obj_create(appMenu);
  lv_obj_remove_style_all(utilityPage);
  lv_obj_set_size(utilityPage, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(utilityPage, themePageBackground(), 0);
  lv_obj_set_style_bg_opa(utilityPage, LV_OPA_COVER, 0);
  lv_obj_clear_flag(utilityPage, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(utilityPage, LV_OBJ_FLAG_CLICKABLE);
  createAppBackButton(utilityPage, utilityBackEvent,
                      app == 0 ? UI_ACCENT_BLUE : app == 1 ? UI_ACCENT_MINT : UI_ACCENT_ROSE);
  lv_obj_t *title = lv_label_create(utilityPage);
  lv_label_set_text(title, tr(app == 0 ? STR_CALCULATOR : app == 1 ? STR_NOTES : STR_STOPWATCH));
  lv_obj_set_style_text_font(title, &lv_font_ui_24, 0);
  lv_obj_set_style_text_color(title, accent, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

  if (app == 0) {
    utilityDisplay = lv_label_create(utilityPage);
    lv_obj_set_width(utilityDisplay, 420);
    lv_obj_set_style_text_align(utilityDisplay, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(utilityDisplay, &lv_font_ui_32, 0);
    lv_obj_set_style_text_color(utilityDisplay, accent, 0);
    lv_label_set_text(utilityDisplay, utilityCalculator.display);
    lv_obj_set_pos(utilityDisplay, 30, 78);
    const char *keys[] = {"7", "8", "9", "/", "4", "5", "6", "*", "1", "2", "3", "-", "C", "0", ".", "+", "="};
    for (uint8_t i = 0; i < 17; ++i) {
      const int row = i / 4, col = i % 4;
      const char c = keys[i][0];
      const uint8_t kind = c == '=' ? 2
                         : (c == '/' || c == '*' || c == '-' || c == '+' || c == 'C') ? 1 : 0;
      utilityButton(utilityPage, keys[i], 30 + col * 106, 130 + row * 66,
                    i == 16 ? 420 : 96, 56, utilityCalculatorEvent, keys[i][0], kind);
    }
  } else if (app == 1) {
    if (!utilityNotesPrepared) prepareUtilityNotes(false);
    if (utilityNoteEditing) buildUtilityNotesEditor(utilityPage, accent);
    else buildUtilityNotesReader(utilityPage, accent);
  } else {
    utilityDisplay = lv_label_create(utilityPage);
    lv_obj_set_style_text_font(utilityDisplay, &lv_font_ui_32, 0);
    lv_obj_set_style_text_color(utilityDisplay, accent, 0);
    lv_obj_align(utilityDisplay, LV_ALIGN_TOP_MID, 0, 150);
    utilityButton(utilityPage, tr(stopwatchRunning ? STR_TIMER_PAUSE : STR_TIMER_START),
                  55, 260, 170, 65, utilityStopwatchEvent, 0, 2);
    utilityButton(utilityPage, tr(STR_TIMER_RESET), 255, 260, 170, 65, utilityStopwatchEvent, 1, 1);
    serviceUtilityApps();
  }
  lv_obj_move_foreground(utilityPage);
}

// Rebuild the current tool after a theme or Notes mode change. Calculator and
// stopwatch state live in globals; Notes preserves the draft/selection and does
// not silently save or drop dirty text when the palette changes.
void rebuildUtilityPage() {
  if (!utilityPage || utilityActive > 2) return;
  if (utilityActive == 1 && utilityNote)
    utilityNoteDraft = lv_textarea_get_text(utilityNote);
  const uint8_t app = utilityActive;
  lv_obj_del(utilityPage);
  utilityPage = utilityDisplay = utilityNote = utilityStatus = nullptr;
  utilityNotePanel = utilityNoteKeyboard = nullptr;
  utilityActive = 255;
  openUtilityApp(app);
}

void refreshUtilityTheme() {
  rebuildUtilityPage();
}
