// UI theming. The default remains true-black AMOLED; additional palettes give
// the OS a CRT, retro-futuristic, cyberpunk, aurora or deep-ocean character.
// Included once by 08_LVGL_Clock.ino inside its private namespace, before the
// UI modules.

enum UiTheme : uint8_t {
  UI_THEME_BLACK = 0,
  UI_THEME_AMBER,
  UI_THEME_RETRO,
  UI_THEME_CYBERPUNK,
  UI_THEME_AURORA,
  UI_THEME_OCEAN,
  UI_THEME_COUNT
};

// Classic amber phosphor palette.
constexpr uint32_t CRT_AMBER = 0xFFB000;        // bright trace / primary text
constexpr uint32_t CRT_AMBER_DIM = 0xB37700;    // secondary text
constexpr uint32_t CRT_AMBER_FAINT = 0x6B4A00;  // borders / dividers
constexpr uint32_t CRT_AMBER_GLOW = 0xFF9000;   // soft glow shadow

struct ThemePalette {
  uint32_t background;
  uint32_t surface;
  uint32_t border;
  uint32_t text;
  uint32_t dim;
  uint32_t accent;
  uint32_t glow;
  bool monochromeAccent;
};

// Black and amber keep the historical behavior. The newer themes use a single
// neon accent so tiles, sliders and charts stay cohesive, while surfaces and
// borders carry the retro-futuristic / cyberpunk mood.
static constexpr ThemePalette THEME_PALETTES[UI_THEME_COUNT] = {
  // AMOLED Black: per-app accents, pure black background.
  {0x000000, 0x000000, 0x000000, 0xFFFFFF, 0x9299A3, 0x000000, 0x000000, false},
  // CRT Amber: monochrome phosphor.
  {0x000000, 0x000000, CRT_AMBER_FAINT, CRT_AMBER, CRT_AMBER_DIM,
   CRT_AMBER, CRT_AMBER_GLOW, true},
  // Retro Futuristic: synthwave violet / hot pink / electric cyan.
  {0x0A0018, 0x140028, 0x6A0DAD, 0xF5E6FF, 0xB39DDB,
   0xFF6EC7, 0xFF6EC7, true},
  // Cyberpunk: near-black blue, hazard yellow and neon cyan traces.
  {0x050510, 0x0A0A1A, 0x00F0FF, 0xE0F7FF, 0x7A8CA3,
   0xFCEE0A, 0x00F0FF, true},
  // Aurora: polar teal / mint.
  {0x001018, 0x001820, 0x0E3A3A, 0xE6FFFA, 0x88B8B0,
   0x00FFC6, 0x000000, true},
  // Ocean: deep navy / bioluminescent blue.
  {0x000814, 0x001220, 0x023E8A, 0xCAF0F8, 0x6C8EA0,
   0x00B4D8, 0x000000, true},
};

inline const ThemePalette &themePalette() {
  return THEME_PALETTES[uiTheme < UI_THEME_COUNT ? uiTheme : UI_THEME_BLACK];
}

inline bool themeIsAmber() { return uiTheme == UI_THEME_AMBER; }
inline bool themeIsRetro() { return uiTheme == UI_THEME_RETRO; }
inline bool themeIsCyberpunk() { return uiTheme == UI_THEME_CYBERPUNK; }

// Accent family (tiles, sliders, progress, charts). Amber collapses every accent
// to the single phosphor hue; the black theme keeps the original per-app color.
inline uint32_t themeAccent(uint32_t normal) {
  if (themeIsAmber()) return CRT_AMBER;
  const ThemePalette &p = themePalette();
  return p.monochromeAccent ? p.accent : normal;
}
// Card / button surface. Amber keeps surfaces black so only traces emit light.
inline uint32_t themeSurface(uint32_t normal) {
  if (themeIsAmber()) return 0x000000;
  if (uiTheme == UI_THEME_BLACK) return normal;
  return themePalette().surface;
}
inline uint32_t themeBorder(uint32_t normal) {
  if (themeIsAmber()) return CRT_AMBER_FAINT;
  if (uiTheme == UI_THEME_BLACK) return normal;
  return themePalette().border;
}
inline lv_color_t themeText() {
  if (themeIsAmber()) return lv_color_hex(CRT_AMBER);
  if (uiTheme == UI_THEME_BLACK) return lv_color_white();
  return lv_color_hex(themePalette().text);
}
inline lv_color_t themeTextDim(uint32_t normal) {
  if (themeIsAmber()) return lv_color_hex(CRT_AMBER_DIM);
  if (uiTheme == UI_THEME_BLACK) return lv_color_hex(normal);
  return lv_color_hex(themePalette().dim);
}
inline lv_color_t themeIcon() { return themeText(); }

// The default and CRT themes stay true black for the AMOLED panel; the newer
// palettes use very dark tinted backgrounds so their neon traces read correctly.
inline lv_color_t themePageBackground() {
  if (uiTheme == UI_THEME_BLACK || themeIsAmber()) return lv_color_black();
  return lv_color_hex(themePalette().background);
}

inline bool themeHasGlow() {
  return themeIsAmber() || themePalette().glow != 0;
}
inline uint32_t themeGlowColor() {
  if (themeIsAmber()) return CRT_AMBER_GLOW;
  return themePalette().glow;
}

inline const char *themeShortName() {
  switch (uiTheme) {
    case UI_THEME_AMBER: return "CRT";
    case UI_THEME_RETRO: return "RET";
    case UI_THEME_CYBERPUNK: return "CYB";
    case UI_THEME_AURORA: return "AUR";
    case UI_THEME_OCEAN: return "OCN";
    default: return "STD";
  }
}

// Per-page theme refreshers (defined in their own modules). Prototypes live in
// the sketch so applyUiAppearance() can call them regardless of include order.
void refreshClockTheme();
void refreshControlPanelTheme();
void refreshAppMenuTheme();
void refreshCalendarTheme();
void refreshAlarmTheme();
void refreshMusicTheme();
void refreshAudiobookTheme();
void refreshRadioTheme();
void refreshWeatherTheme();
void refreshWorldClockTheme();
void refreshPhotoFrameTheme();
void refreshTimerTheme();

// Queue a full language/theme re-apply. The main loop performs it inside a
// screen fade so the recolor is never visible as a partial repaint.
void requestUiAppearanceChange() { uiAppearanceChangeRequested = true; }

void setUiLanguage(uint8_t language, bool persist) {
  language = min<uint8_t>(language, UI_LANG_COUNT - 1);
  if (language == uiLanguage) return;
  uiLanguage = language;
  if (persist) preferences.putUChar("ui_lang", uiLanguage);
  // Weather condition text is fetched from QWeather with a language parameter;
  // request a fresh cycle so conditions render in the new language.
  refreshWeatherConfiguration();
  requestUiAppearanceChange();
}

void setUiTheme(uint8_t theme, bool persist) {
  theme = min<uint8_t>(theme, UI_THEME_COUNT - 1);
  if (theme == uiTheme) return;
  uiTheme = theme;
  if (persist) preferences.putUChar("ui_theme", uiTheme);
  requestUiAppearanceChange();
}

// Re-apply language + theme to the whole UI. Called after a language or theme
// change (control-panel buttons or the web API) inside a screen fade so the
// recolor is not visible as a partial repaint.
void applyUiAppearance() {
  applyUiLanguageStatic();
  refreshClockTheme();
  refreshControlPanelTheme();
  refreshAppMenuTheme();
  refreshCalendarTheme();
  refreshAlarmTheme();
  refreshMusicTheme();
  refreshAudiobookTheme();
  refreshRadioTheme();
  refreshWeatherTheme();
  refreshWorldClockTheme();
  refreshPhotoFrameTheme();
  refreshTimerTheme();
  // Dynamic content in the new language / colors.
  updateNetworkLabels();
  updateControlPanelStatus();
  syncAlarmUi();
  updateMusicPageText();
  updateAudiobookPageText();
  updateRadioPageText();
  updateCalendarToday();
  updateWifiSetupPage();
  updateWorldClockPage();
  updateTimerPageText();
}
