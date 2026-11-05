// UI theming. Icon Glow is the default: true-black pages and translucent,
// per-app color tints. The retired AMOLED Black entry is no longer selectable.
// Included once by 08_LVGL_Clock.ino inside its private namespace, before the
// UI modules.

enum UiTheme : uint8_t {
  UI_THEME_GLOW = 0,
  UI_THEME_AMBER,
  UI_THEME_RETRO,
  UI_THEME_CYBERPUNK,
  UI_THEME_AURORA,
  UI_THEME_OCEAN,
  UI_THEME_SPECTRUM,
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

// Glow and Spectrum keep per-app accents. Amber and the neon palettes use a
// single accent so tiles, sliders, charts and app controls stay cohesive.
static constexpr ThemePalette THEME_PALETTES[UI_THEME_COUNT] = {
  // Icon Glow: per-app neon accents, pure-black pages and translucent tints.
  {0x000000, 0x080B10, 0x243044, 0xFFFFFF, 0x9AA3B2, 0xFFFFFF, 0x000000, false},
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
  // Spectrum: neutral surfaces with individually coloured launcher glows.
  {0x050510, 0x10101B, 0x40405A, 0xF4F4FF, 0xA0A0B8,
   0xFFFFFF, 0x000000, false},
};

inline const ThemePalette &themePalette() {
  return THEME_PALETTES[uiTheme < UI_THEME_COUNT ? uiTheme : UI_THEME_GLOW];
}

inline bool themeIsAmber() { return uiTheme == UI_THEME_AMBER; }
inline bool themeIsSpectrum() { return uiTheme == UI_THEME_SPECTRUM; }
inline bool themeIsGlow() { return uiTheme == UI_THEME_GLOW; }
// Launcher palettes that give every tile its own vivid icon hue.
inline bool themePerTileColors() { return themeIsSpectrum() || themeIsGlow(); }
inline bool themeIsRetro() { return uiTheme == UI_THEME_RETRO; }
inline bool themeIsCyberpunk() { return uiTheme == UI_THEME_CYBERPUNK; }
// True for the single-neon-accent palettes (retro / cyberpunk / aurora / ocean)
// where every tile and control collapses onto one accent hue.
inline bool themeIsNeon() {
  return !themeIsGlow() && !themeIsAmber() && themePalette().monochromeAccent;
}

// Accent family shared by tiles and every app. Amber collapses to phosphor;
// Glow brightens the per-app accents to match the launcher neon palette.
inline uint32_t themeAccent(uint32_t normal) {
  if (themeIsAmber()) return CRT_AMBER;
  if (themeIsGlow()) {
    switch (normal) {
      case UI_ACCENT_BLUE: return 0x46A8FF;
      case UI_ACCENT_MINT: return 0x53F0AA;
      case UI_ACCENT_AMBER: return 0xFFB020;
      case UI_ACCENT_ROSE: return 0xFF558F;
      case UI_ACCENT_VIOLET: return 0xB57AFF;
      case UI_ACCENT_BILIBILI: return 0xFF5C7A;
      default: return normal;
    }
  }
  const ThemePalette &p = themePalette();
  return p.monochromeAccent ? p.accent : normal;
}
// Near-black card surface. Amber and Glow keep the background AMOLED-friendly.
inline uint32_t themeSurface(uint32_t normal) {
  if (themeIsAmber()) return 0x000000;
  if (themeIsGlow()) return 0x080B10;
  return themePalette().surface;
}
inline uint32_t themeBorder(uint32_t normal) {
  if (themeIsAmber()) return CRT_AMBER_FAINT;
  return themePalette().border;
}
inline lv_color_t themeText() {
  if (themeIsAmber()) return lv_color_hex(CRT_AMBER);
  if (themeIsGlow()) return lv_color_white();
  return lv_color_hex(themePalette().text);
}
inline lv_color_t themeTextDim(uint32_t normal) {
  if (themeIsAmber()) return lv_color_hex(CRT_AMBER_DIM);
  return lv_color_hex(themePalette().dim);
}
inline lv_color_t themeIcon() { return themeText(); }


// Readable text/icon color on top of a filled accent surface. Amber phosphor
// and the bright neon accents need dark glyphs. Spectrum uses white labels.
inline lv_color_t themeTextOnAccent() {
  if (themeIsSpectrum()) return lv_color_white();
  return lv_color_black();
}

// Glow and CRT stay true black for the AMOLED panel; the newer
// palettes use very dark tinted backgrounds so their neon traces read correctly.
inline lv_color_t themePageBackground() {
  if (themeIsGlow() || themeIsAmber()) return lv_color_black();
  return lv_color_hex(themePalette().background);
}

// Shared app-control treatment: Glow/Spectrum use a low-opacity accent tint,
// while the other palettes retain their own surfaces. No icon halo is added.
inline lv_color_t themeTint(uint32_t accent, lv_opa_t opacity) {
  return lv_color_mix(lv_color_hex(themeAccent(accent)), themePageBackground(), opacity);
}
inline lv_color_t themeControlSurface(uint32_t accent) {
  return themePerTileColors() ? themeTint(accent, LV_OPA_20)
                             : lv_color_hex(themeSurface(0x080B10));
}
inline lv_color_t themeControlBorder(uint32_t accent) {
  return themePerTileColors() ? themeTint(accent, LV_OPA_50)
                             : lv_color_hex(themeBorder(0x243044));
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
    case UI_THEME_GLOW: return "GLW";
    case UI_THEME_AMBER: return "CRT";
    case UI_THEME_RETRO: return "RET";
    case UI_THEME_CYBERPUNK: return "CYB";
    case UI_THEME_AURORA: return "AUR";
    case UI_THEME_OCEAN: return "OCN";
    case UI_THEME_SPECTRUM: return "SPC";
    default: return "GLW";
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
void refreshUtilityTheme();

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
  if (persist) preferences.putUChar("ui_theme_v2", uiTheme);
  preferences.putUChar("ui_schema", 2);
  Serial.printf("UI: theme -> %u (refresh requested)\n", uiTheme);
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
  refreshUtilityTheme();
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
