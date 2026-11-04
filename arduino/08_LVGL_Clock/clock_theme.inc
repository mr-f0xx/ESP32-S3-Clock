// UI theming: AMOLED true-black (default) and a monochrome CRT amber look.
// Included once by 08_LVGL_Clock.ino inside its private namespace, before the
// UI modules. Every page background is pure black on the AMOLED panel in both
// themes; the amber theme recolors text, icons, accents and borders to a single
// phosphor hue and turns app tiles into black, amber-outlined keys.

enum UiTheme : uint8_t { UI_THEME_BLACK = 0, UI_THEME_AMBER = 1, UI_THEME_COUNT };

// Classic amber phosphor palette.
constexpr uint32_t CRT_AMBER = 0xFFB000;        // bright trace / primary text
constexpr uint32_t CRT_AMBER_DIM = 0xB37700;    // secondary text
constexpr uint32_t CRT_AMBER_FAINT = 0x6B4A00;  // borders / dividers
constexpr uint32_t CRT_AMBER_GLOW = 0xFF9000;   // soft glow shadow

inline bool themeIsAmber() { return uiTheme == UI_THEME_AMBER; }

// Accent family (tiles, sliders, progress, charts). Amber collapses every accent
// to the single phosphor hue; the black theme keeps the original per-app color.
inline uint32_t themeAccent(uint32_t normal) {
  return themeIsAmber() ? CRT_AMBER : normal;
}
// Card / button surface. Amber keeps surfaces black so only traces emit light.
inline uint32_t themeSurface(uint32_t normal) {
  return themeIsAmber() ? 0x000000 : normal;
}
inline uint32_t themeBorder(uint32_t normal) {
  return themeIsAmber() ? CRT_AMBER_FAINT : normal;
}
inline lv_color_t themeText() {
  return lv_color_hex(themeIsAmber() ? CRT_AMBER : 0xFFFFFF);
}
inline lv_color_t themeTextDim(uint32_t normal) {
  return lv_color_hex(themeIsAmber() ? CRT_AMBER_DIM : normal);
}
inline lv_color_t themeIcon() { return themeText(); }

// Page backgrounds are always true black for the AMOLED panel.
inline lv_color_t themePageBackground() { return lv_color_black(); }

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
  // Dynamic content in the new language / colors.
  updateNetworkLabels();
  updateControlPanelStatus();
  syncAlarmUi();
  updateMusicPageText();
  updateAudiobookPageText();
  updateRadioPageText();
  updateCalendarToday();
  updateWifiSetupPage();
}
