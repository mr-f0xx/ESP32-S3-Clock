// LVGL clock UI, dynamic background and image loading.
// Included once by 08_LVGL_Clock.ino inside its private namespace.

// RAM copies of the built-in Montserrat fonts with a French accent fallback.
// The glyph bitmaps stay in flash; only these small descriptors live in RAM.
lv_font_t lv_font_ui_14;
lv_font_t lv_font_ui_16;
lv_font_t lv_font_ui_18;
lv_font_t lv_font_ui_20;
lv_font_t lv_font_ui_22;
lv_font_t lv_font_ui_24;
lv_font_t lv_font_ui_25;
lv_font_t lv_font_ui_28;
lv_font_t lv_font_ui_30;
lv_font_t lv_font_ui_32;

void initUiFonts() {
  struct UiFontBinding {
    lv_font_t *destination;
    const lv_font_t *base;
    const lv_font_t *accent;
  };
  const UiFontBinding bindings[] = {
    {&lv_font_ui_14, &lv_font_montserrat_14, &lv_font_accent_14},
    {&lv_font_ui_16, &lv_font_montserrat_16, &lv_font_accent_16},
    {&lv_font_ui_18, &lv_font_montserrat_18, &lv_font_accent_18},
    {&lv_font_ui_20, &lv_font_montserrat_20, &lv_font_accent_20},
    {&lv_font_ui_22, &lv_font_montserrat_22, &lv_font_accent_22},
    {&lv_font_ui_24, &lv_font_montserrat_24, &lv_font_accent_24},
    {&lv_font_ui_25, &lv_font_montserrat_25, &lv_font_accent_25},
    {&lv_font_ui_28, &lv_font_montserrat_28, &lv_font_accent_28},
    {&lv_font_ui_30, &lv_font_montserrat_30, &lv_font_accent_30},
    {&lv_font_ui_32, &lv_font_montserrat_32, &lv_font_accent_32},
  };
  for (const UiFontBinding &binding : bindings) {
    *binding.destination = *binding.base;
    binding.destination->fallback = binding.accent;
  }
}

// True when the awake main page has no background image and therefore shows
// the large centered "standby-style" clock on pure black instead of the small
// top-left clock over a wallpaper.
bool mainClockCentered = false;

const lv_font_t *currentStandbyClockFont() {
  switch (clockFontIndex) {
    case CLOCK_FONT_KAMERON: return &lv_font_kameron_96;
    case CLOCK_FONT_SILKSCREEN: return &lv_font_silkscreen_96;
    case CLOCK_FONT_DOTGOTHIC16: return &lv_font_dotgothic16_96;
    case CLOCK_FONT_ORBITRON: return &lv_font_orbitron_96;
    case CLOCK_FONT_AUDIOWIDE: return &lv_font_audiowide_96;
    case CLOCK_FONT_CINZEL: return &lv_font_cinzel_96;
    case CLOCK_FONT_BEBASNEUE: return &lv_font_bebasneue_96;
    case CLOCK_FONT_DIGITALNUMBERS: return &lv_font_digitalnumbers_96;
    case CLOCK_FONT_RAJDHANI: return &lv_font_rajdhani_96;
    case CLOCK_FONT_ROBOTOMONO: return &lv_font_robotomono_96;
    case CLOCK_FONT_NEONLINES: return &lv_font_neonlines_96;
    default: return &lv_font_montserrat_time_96;
  }
}

const lv_font_t *currentClockFont() {
  if (lowPowerMode || mainClockCentered) return currentStandbyClockFont();
  switch (clockFontIndex) {
    case CLOCK_FONT_KAMERON: return &lv_font_kameron_48;
    case CLOCK_FONT_SILKSCREEN: return &lv_font_silkscreen_48;
    case CLOCK_FONT_DOTGOTHIC16: return &lv_font_dotgothic16_48;
    case CLOCK_FONT_ORBITRON: return &lv_font_orbitron_48;
    case CLOCK_FONT_AUDIOWIDE: return &lv_font_audiowide_48;
    case CLOCK_FONT_CINZEL: return &lv_font_cinzel_48;
    case CLOCK_FONT_BEBASNEUE: return &lv_font_bebasneue_48;
    case CLOCK_FONT_DIGITALNUMBERS: return &lv_font_digitalnumbers_48;
    case CLOCK_FONT_RAJDHANI: return &lv_font_rajdhani_48;
    case CLOCK_FONT_ROBOTOMONO: return &lv_font_robotomono_48;
    case CLOCK_FONT_NEONLINES: return &lv_font_neonlines_48;
    default: return &lv_font_montserrat_48;
  }
}

// Compact 3-letter tag for the on-device font pill in the control panel.
const char *clockFontShortName(uint8_t index) {
  switch (index) {
    case CLOCK_FONT_KAMERON: return "KAM";
    case CLOCK_FONT_SILKSCREEN: return "SLK";
    case CLOCK_FONT_DOTGOTHIC16: return "DOT";
    case CLOCK_FONT_ORBITRON: return "ORB";
    case CLOCK_FONT_AUDIOWIDE: return "AUD";
    case CLOCK_FONT_CINZEL: return "CIN";
    case CLOCK_FONT_BEBASNEUE: return "BEB";
    case CLOCK_FONT_DIGITALNUMBERS: return "DIG";
    case CLOCK_FONT_RAJDHANI: return "RAJ";
    case CLOCK_FONT_ROBOTOMONO: return "RMO";
    case CLOCK_FONT_NEONLINES: return "NEO";
    default: return "MON";
  }
}

bool clockFontHasTimeGlyphs(const lv_font_t *font) {
  if (!font) return false;
  lv_font_glyph_dsc_t digitDescriptor{};
  lv_font_glyph_dsc_t colonDescriptor{};
  return lv_font_get_glyph_dsc(font, &digitDescriptor, '0', ':') &&
         lv_font_get_glyph_dsc(font, &colonDescriptor, ':', 0) &&
         lv_font_get_glyph_bitmap(font, '0') != nullptr &&
         lv_font_get_glyph_bitmap(font, ':') != nullptr;
}

void logClockFontStatus(const char *name, const lv_font_t *font) {
  Serial.printf("Clock font %-12s: %s, line height %u\n", name,
                clockFontHasTimeGlyphs(font) ? "ready" : "INVALID",
                font ? font->line_height : 0);
}

void settingsEvent(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
    portalRequested = true;
  }
}

bool createFeatheredTimeBackground() {
  timePanelPixels = static_cast<uint8_t *>(heap_caps_malloc(
    TIME_PANEL_BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!timePanelPixels) {
    timePanelPixels = static_cast<uint8_t *>(heap_caps_malloc(
      TIME_PANEL_BUFFER_BYTES, MALLOC_CAP_8BIT));
  }
  if (!timePanelPixels) {
    Serial.println("Unable to allocate feathered time background");
    return false;
  }

  timeBackground = lv_canvas_create(timePanel);
  lv_obj_clear_flag(timeBackground, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  return true;
}

int16_t measureTimePanelWidth(const char *text) {
  lv_point_t textSize{};
  lv_txt_get_size(&textSize, text, currentClockFont(), 0, 0,
                  LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  const bool large = lowPowerMode || mainClockCentered;
  const int16_t horizontalPadding = large ? 32 : TIME_PANEL_HORIZONTAL_PADDING;
  const int16_t minimumWidth = large ? 240 : TIME_PANEL_MIN_WIDTH;
  const int16_t maximumWidth = large ? SCREEN_WIDTH - 16 : TIME_PANEL_MAX_WIDTH;
  return constrain(static_cast<int16_t>(textSize.x) + horizontalPadding * 2,
                   minimumWidth, maximumWidth);
}

void resizeTimePanelForText(const char *text) {
  const int16_t nextWidth = measureTimePanelWidth(text);
  if (lowPowerMode || mainClockCentered) {
    timePanelWidth = nextWidth;
    const int16_t panelHeight = max(
      static_cast<int16_t>(currentClockFont()->line_height + 36),
      static_cast<int16_t>(120));
    lv_obj_set_size(timePanel, timePanelWidth, panelHeight);
    return;
  }
  if (nextWidth == timePanelWidth && timeBackground) return;
  timePanelWidth = nextWidth;
  lv_obj_set_size(timePanel, timePanelWidth, TIME_PANEL_HEIGHT);

  if (!timePanelPixels || !timeBackground) {
    lv_obj_set_style_bg_color(timePanel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(timePanel, LV_OPA_20, 0);
    lv_obj_set_style_radius(timePanel, TIME_PANEL_RADIUS, 0);
    return;
  }

  // A near-black scrim keeps the time legible over album photos while staying
  // invisible on the true-black AMOLED background when no image is loaded.
  const lv_color_t leftColor = lv_color_hex(0x000000);
  const lv_color_t rightColor = lv_color_hex(themeIsAmber() ? 0x1A1206 : 0x101010);
  const float halfWidth = timePanelWidth * 0.5f;
  const float halfHeight = TIME_PANEL_HEIGHT * 0.5f;

  for (int16_t y = 0; y < TIME_PANEL_HEIGHT; ++y) {
    for (int16_t x = 0; x < timePanelWidth; ++x) {
      const float qx = fabsf(x - (timePanelWidth - 1) * 0.5f) -
                       (halfWidth - TIME_PANEL_RADIUS);
      const float qy = fabsf(y - (TIME_PANEL_HEIGHT - 1) * 0.5f) -
                       (halfHeight - TIME_PANEL_RADIUS);
      const float outsideX = max(qx, 0.0f);
      const float outsideY = max(qy, 0.0f);
      const float signedDistance = sqrtf(outsideX * outsideX + outsideY * outsideY) +
                                   min(max(qx, qy), 0.0f) - TIME_PANEL_RADIUS;
      float feather = constrain(-signedDistance / TIME_PANEL_FEATHER, 0.0f, 1.0f);
      feather = feather * feather * (3.0f - 2.0f * feather);

      const uint8_t mix = static_cast<uint8_t>(
        (static_cast<uint32_t>(x) * 255U) / (timePanelWidth - 1));
      const lv_color_t color = lv_color_mix(rightColor, leftColor, mix);
      uint8_t *pixel = timePanelPixels +
                       (static_cast<size_t>(y) * timePanelWidth + x) *
                       LV_IMG_PX_SIZE_ALPHA_BYTE;
      memcpy(pixel, &color, sizeof(lv_color_t));
      pixel[LV_IMG_PX_SIZE_ALPHA_BYTE - 1] =
        static_cast<uint8_t>(feather * 112.0f);
    }
  }

  lv_canvas_set_buffer(timeBackground, timePanelPixels,
                       timePanelWidth, TIME_PANEL_HEIGHT,
                       LV_IMG_CF_TRUE_COLOR_ALPHA);
  lv_obj_center(timeBackground);
}

// Selectable clock digit colors. Index 0 follows the active OS theme.
constexpr uint8_t CLOCK_COLOR_COUNT = 9;
static constexpr uint32_t CLOCK_COLOR_PRESETS[CLOCK_COLOR_COUNT] = {
  0x000000, // theme default (special-cased)
  0xFFFFFF, // white
  0xFFB000, // amber
  0x00F0FF, // cyber cyan
  0xFF6EC7, // synthwave pink
  0x00FFC6, // aurora mint
  0xFFD700, // gold
  0x90E0EF, // ice blue
  0xFF4D4D, // red
};

void applyClockColor() {
  if (!timeLabel) return;
  const lv_color_t color = clockColorIndex == 0
    ? themeText()
    : lv_color_hex(CLOCK_COLOR_PRESETS[
        clockColorIndex < CLOCK_COLOR_COUNT ? clockColorIndex : 0]);
  lv_obj_set_style_text_color(timeLabel, color, 0);
}

void setClockColor(uint8_t color, bool persist) {
  color = min<uint8_t>(color, CLOCK_COLOR_COUNT - 1);
  if (color == clockColorIndex) return;
  clockColorIndex = color;
  if (persist) preferences.putUChar("clock_color", clockColorIndex);
  applyClockColor();
}

void applyClockFont() {
  if (!timeLabel) return;
  const lv_font_t *font = currentClockFont();
  if (!clockFontHasTimeGlyphs(font)) {
    Serial.printf("Clock font %u invalid; falling back to Montserrat\n",
                  static_cast<unsigned>(clockFontIndex));
    clockFontIndex = CLOCK_FONT_MONTSERRAT;
    pendingClockFontIndex = CLOCK_FONT_MONTSERRAT;
    preferences.putUChar("clock_font", clockFontIndex);
    font = (lowPowerMode || mainClockCentered) ? &lv_font_montserrat_time_96
                                              : &lv_font_montserrat_48;
  }
  lv_obj_set_style_text_font(timeLabel, font, 0);
  applyClockColor();
  timePanelWidth = 0;
  resizeTimePanelForText(displayedTimeText[0] ? displayedTimeText : "--:--");
  lv_obj_center(timeLabel);
  Serial.printf("Clock font applied: %s (%s, %u px line)\n",
                clockFontShortName(clockFontIndex),
                lowPowerMode ? "standby" : (mainClockCentered ? "centered" : "main"),
                static_cast<unsigned>(font->line_height));
}

// Awake main page: with a wallpaper/album background keep the small top-left
// clock over its scrim; with no background show the large centered clock on
// pure black (no date), matching the standby look the user expects.
void refreshMainClockLayout() {
  if (!timePanel || !timeLabel || lowPowerMode) return;
  const bool hasBackground =
    backgroundImage && !lv_obj_has_flag(backgroundImage, LV_OBJ_FLAG_HIDDEN);
  mainClockCentered = !hasBackground;
  if (mainClockCentered) {
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(timePanel, LV_OPA_TRANSP, 0);
    if (timeBackground) lv_obj_add_flag(timeBackground, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(timePanel, LV_ALIGN_CENTER, 0, 0);
  } else {
    lv_obj_set_style_bg_color(lv_scr_act(), themePageBackground(), 0);
    if (timeBackground) lv_obj_clear_flag(timeBackground, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(timePanel, LV_ALIGN_TOP_LEFT, 24, 24);
  }
  timePanelWidth = 0;
  applyClockFont();
  lv_obj_invalidate(lv_scr_act());
}

void createClockUi() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, themePageBackground(), 0);
  lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_NONE, 0);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  backgroundImage = lv_img_create(screen);
  lv_obj_center(backgroundImage);
  lv_obj_add_flag(backgroundImage, LV_OBJ_FLAG_HIDDEN);

  shadeObject = lv_obj_create(screen);
  lv_obj_remove_style_all(shadeObject);
  lv_obj_set_size(shadeObject, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_center(shadeObject);
  lv_obj_set_style_bg_color(shadeObject, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(shadeObject, LV_OPA_40, 0);
  lv_obj_clear_flag(shadeObject, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(shadeObject, LV_OBJ_FLAG_HIDDEN);

  timePanel = lv_obj_create(screen);
  lv_obj_remove_style_all(timePanel);
  lv_obj_set_size(timePanel, TIME_PANEL_MIN_WIDTH, TIME_PANEL_HEIGHT);
  lv_obj_set_style_bg_opa(timePanel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(timePanel, 0, 0);
  lv_obj_set_style_shadow_width(timePanel, 0, 0);
  lv_obj_set_style_pad_all(timePanel, 0, 0);
  lv_obj_clear_flag(timePanel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(timePanel, LV_ALIGN_TOP_LEFT, 24, 24);

  createFeatheredTimeBackground();
  timePanelWidth = 0;
  resizeTimePanelForText("--:--");

  timeLabel = lv_label_create(timePanel);
  lv_label_set_text(timeLabel, "--:--");
  lv_obj_set_width(timeLabel, LV_SIZE_CONTENT);
  lv_obj_set_style_text_font(timeLabel, currentClockFont(), 0);
  lv_obj_set_style_text_color(timeLabel, lv_color_white(), 0);
  lv_obj_set_style_text_opa(timeLabel, LV_OPA_COVER, 0);
  lv_obj_set_style_text_align(timeLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(timeLabel);
  lv_obj_move_foreground(timePanel);
  refreshMainClockLayout();

  dateLabel = lv_label_create(screen);
  i18nRegister(dateLabel, STR_WAITING_NTP);
  lv_obj_set_style_text_font(dateLabel, &lv_font_ui_22, 0);
  lv_obj_set_style_text_color(dateLabel, themeTextDim(0xDCEBFF), 0);
  lv_obj_set_style_shadow_color(dateLabel, lv_color_black(), 0);
  lv_obj_set_style_shadow_width(dateLabel, 10, 0);
  lv_obj_set_style_shadow_opa(dateLabel, LV_OPA_70, 0);
  lv_obj_align(dateLabel, LV_ALIGN_CENTER, 0, 48);
  lv_obj_add_flag(dateLabel, LV_OBJ_FLAG_HIDDEN);

  statusLabel = lv_label_create(screen);
  lv_label_set_text(statusLabel, tr(STR_WIFI_NOT_CONFIGURED));
  lv_obj_set_style_text_font(statusLabel, &lv_font_ui_16, 0);
  lv_obj_set_style_text_color(statusLabel, themeTextDim(0x91A4BE), 0);
  lv_obj_align(statusLabel, LV_ALIGN_TOP_MID, 0, 34);
  lv_obj_add_flag(statusLabel, LV_OBJ_FLAG_HIDDEN);

  addressLabel = lv_label_create(screen);
  lv_label_set_text(addressLabel, tr(STR_STARTING_SERVICES));
  lv_obj_set_width(addressLabel, 380);
  lv_obj_set_style_text_align(addressLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(addressLabel, &lv_font_ui_14, 0);
  lv_obj_set_style_text_color(addressLabel, themeTextDim(0xB8C5D8), 0);
  lv_obj_align(addressLabel, LV_ALIGN_BOTTOM_MID, 0, -38);
  lv_obj_add_flag(addressLabel, LV_OBJ_FLAG_HIDDEN);

  settingsButton = lv_btn_create(screen);
  lv_obj_set_size(settingsButton, 56, 56);
  lv_obj_set_style_radius(settingsButton, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(settingsButton, lv_color_hex(themeSurface(0x0A0A0A)), 0);
  lv_obj_set_style_bg_opa(settingsButton, LV_OPA_80, 0);
  lv_obj_set_style_border_width(settingsButton, 1, 0);
  lv_obj_set_style_border_color(settingsButton, lv_color_hex(themeBorder(0x2A2A2A)), 0);
  lv_obj_align(settingsButton, LV_ALIGN_BOTTOM_RIGHT, -22, -20);
  lv_obj_add_event_cb(settingsButton, settingsEvent, LV_EVENT_CLICKED, nullptr);

  createTablerIcon(settingsButton, TABLER_ICON_ADJUSTMENTS, 28, themeIcon());
  lv_obj_add_flag(settingsButton, LV_OBJ_FLAG_HIDDEN);

  // Put the base clock on the panel before constructing the heavier launcher
  // and app pages. If a later UI allocation ever fails, the device is not left
  // showing a completely black screen while the boot code halts.
  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(nullptr);

  createControlPanel();
  createAppMenu();
}

// Re-apply the active theme to the clock/album screen.
void refreshClockTheme() {
  lv_obj_t *screen = lv_scr_act();
  lv_obj_set_style_bg_color(screen, themePageBackground(), 0);
  lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_NONE, 0);
  if (timeLabel) {
    applyClockColor();
    // A faint glow sells the CRT / neon looks. It is only redrawn once a
    // minute, so the complex-draw shadow cost is negligible.
    lv_obj_set_style_shadow_color(timeLabel, lv_color_hex(themeGlowColor()), 0);
    lv_obj_set_style_shadow_width(timeLabel, themeHasGlow() ? 8 : 0, 0);
    lv_obj_set_style_shadow_opa(timeLabel, themeHasGlow() ? LV_OPA_40 : LV_OPA_TRANSP, 0);
  }
  if (dateLabel) lv_obj_set_style_text_color(dateLabel, themeTextDim(0xDCEBFF), 0);
  if (statusLabel) lv_obj_set_style_text_color(statusLabel, themeTextDim(0x91A4BE), 0);
  if (addressLabel) lv_obj_set_style_text_color(addressLabel, themeTextDim(0xB8C5D8), 0);
  if (settingsButton) {
    lv_obj_set_style_bg_color(settingsButton, lv_color_hex(themeSurface(0x0A0A0A)), 0);
    lv_obj_set_style_border_color(settingsButton, lv_color_hex(themeBorder(0x2A2A2A)), 0);
    if (lv_obj_get_child_cnt(settingsButton))
      setTablerIconColor(lv_obj_get_child(settingsButton, 0), themeIcon());
  }
  // Regenerate the feathered scrim with the theme's tint.
  timePanelWidth = 0;
  resizeTimePanelForText(displayedTimeText[0] ? displayedTimeText : "--:--");
}

bool initDisplayAndLvgl() {
  Serial.println("[1/5] Display initialization");
  if (!displayBusMutex) displayBusMutex = xSemaphoreCreateMutex();
  if (!displayBusMutex) {
    Serial.println("Unable to create display bus mutex");
    return false;
  }
  if (!gfx->begin(DISPLAY_BUS_FREQUENCY)) {
    Serial.println("Display initialization failed");
    return false;
  }
  gfx->setBrightness(activeBrightness);
  bus->writeC8D8(0x36, 0xA0);
  gfx->fillScreen(RGB565_BLACK);
  Serial.printf("Display ready (QSPI %lu MHz)\n",
                static_cast<unsigned long>(DISPLAY_BUS_FREQUENCY / 1000000UL));

  Serial.println("[2/5] Touch initialization");
  recoverSharedI2cBus("startup");

  touch.setPins(TP_RST, TP_INT);
  if (!touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    Serial.println("Touch controller not found; continuing without touch");
  } else {
    touch.setMaxCoordinates(SCREEN_WIDTH, SCREEN_HEIGHT);
    touch.setSwapXY(true);
    touch.setMirrorXY(true, false);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), onTouchInterrupt, FALLING);
    touchReady = true;
    touchPresent = true;
    Serial.println("Touch ready");
  }

  Serial.println("QMI8658 orientation initialization");
  qmiReady = qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!qmiReady) {
    Serial.println("QMI8658 not found; automatic orientation disabled");
  } else {
    qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_LOWPOWER_21Hz,
                            SensorQMI8658::LPF_MODE_0);
    qmi.enableAccelerometer();
    Serial.printf("QMI8658 automatic orientation ready at %u Hz\n", ORIENTATION_SAMPLE_HZ);
  }

  Serial.println("PCF85063 initialization");
  rtcReady = rtc.begin(Wire, IIC_SDA, IIC_SCL);
  if (!rtcReady) {
    Serial.println("PCF85063 not found; waiting for NTP without RTC storage");
  } else {
    const RTC_DateTime savedTime = rtc.getDateTime();
    rtcTimeValid = rtc.isClockIntegrityGuaranteed() && isValidRtcTime(savedTime);
    timeSynced = rtcTimeValid;
    Serial.printf("PCF85063 ready, stored time %s\n", rtcTimeValid ? "valid" : "invalid");
  }

  Serial.println("AXP2101 power-key initialization");
  pmuReady = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!pmuReady) {
    Serial.println("AXP2101 not found; PWR sleep mode unavailable");
  } else {
    power.enableBattDetection();
    power.enableBattVoltageMeasure();
    power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    power.clearIrqStatus();
    power.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
    power.enableIRQ(XPOWERS_AXP2101_PKEY_LONG_IRQ);
    power.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);
    power.enableLongPressShutdown();
    power.setLongPressRestart();
    Serial.println("AXP2101 PWR ready: short press sleeps, 4-second press restarts");
  }

  Serial.println("[3/5] LVGL initialization");
  lv_init();
  initUiFonts();
  logClockFontStatus("Montserrat", &lv_font_montserrat_48);
  logClockFontStatus("Kameron", &lv_font_kameron_48);
  logClockFontStatus("Silkscreen", &lv_font_silkscreen_48);
  logClockFontStatus("DotGothic16", &lv_font_dotgothic16_48);
  logClockFontStatus("Orbitron", &lv_font_orbitron_48);
  logClockFontStatus("Audiowide", &lv_font_audiowide_48);
  logClockFontStatus("Cinzel", &lv_font_cinzel_48);
  logClockFontStatus("BebasNeue", &lv_font_bebasneue_48);
  logClockFontStatus("Montserrat 96", &lv_font_montserrat_time_96);
  logClockFontStatus("Kameron 96", &lv_font_kameron_96);
  logClockFontStatus("Silkscreen 96", &lv_font_silkscreen_96);
  logClockFontStatus("DotGothic16 96", &lv_font_dotgothic16_96);
  logClockFontStatus("Orbitron 96", &lv_font_orbitron_96);
  logClockFontStatus("Audiowide 96", &lv_font_audiowide_96);
  logClockFontStatus("Cinzel 96", &lv_font_cinzel_96);
  logClockFontStatus("BebasNeue 96", &lv_font_bebasneue_96);
  logClockFontStatus("digitalnumbers 48", &lv_font_digitalnumbers_48);
  logClockFontStatus("digitalnumbers 96", &lv_font_digitalnumbers_96);
  logClockFontStatus("rajdhani 48", &lv_font_rajdhani_48);
  logClockFontStatus("rajdhani 96", &lv_font_rajdhani_96);
  logClockFontStatus("robotomono 48", &lv_font_robotomono_48);
  logClockFontStatus("robotomono 96", &lv_font_robotomono_96);
  logClockFontStatus("Neon Lines 48", &lv_font_neonlines_48);
  logClockFontStatus("Neon Lines 96", &lv_font_neonlines_96);
  constexpr uint16_t FALLBACK_DRAW_LINES = 40;
  const uint32_t fullScreenPixels =
    static_cast<uint32_t>(SCREEN_WIDTH) * SCREEN_HEIGHT;
  uint32_t drawBufferPixels = fullScreenPixels;

  // A 40-line buffer is split into vertical strips by LVGL's 90/270-degree
  // software rotation. A full PSRAM buffer keeps each invalidated control
  // panel region in one rotated flush and removes the visible strip seams.
  drawBufferMemory = static_cast<lv_color_t *>(heap_caps_malloc(
    fullScreenPixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!drawBufferMemory) {
    fullScreenDrawBufferReady = false;
    drawBufferPixels = SCREEN_WIDTH * FALLBACK_DRAW_LINES;
    drawBufferMemory = static_cast<lv_color_t *>(heap_caps_malloc(
      drawBufferPixels * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (!drawBufferMemory) {
      Serial.println("Unable to allocate LVGL draw buffer");
      return false;
    }
    Serial.println("LVGL draw buffer: 40-line fallback (rotation may tear)");
  } else {
    fullScreenDrawBufferReady = true;
    Serial.printf("LVGL draw buffer: full screen in PSRAM (%lu bytes)\n",
                  static_cast<unsigned long>(fullScreenPixels * sizeof(lv_color_t)));
  }
  lv_disp_draw_buf_init(&drawBuffer, drawBufferMemory, nullptr, drawBufferPixels);

  // Keep LVGL's rotation value for layout and pointer mapping, but perform the
  // pixel rotation in displayFlush() as one transaction. This avoids LVGL 8's
  // segmented 90/270-degree flush path. Fall back to LVGL rotation only when a
  // second PSRAM framebuffer cannot be allocated.
  rotationBufferMemory = static_cast<lv_color_t *>(heap_caps_malloc(
    fullScreenPixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  manualRotationBufferReady = rotationBufferMemory != nullptr;
  if (manualRotationBufferReady) {
    Serial.printf("Display rotation buffer: full screen in PSRAM (%lu bytes)\n",
                  static_cast<unsigned long>(fullScreenPixels * sizeof(lv_color_t)));
  } else {
    Serial.println("Display rotation buffer unavailable; using LVGL fallback");
  }

  static lv_disp_drv_t displayDriver;
  lv_disp_drv_init(&displayDriver);
  displayDriver.hor_res = SCREEN_WIDTH;
  displayDriver.ver_res = SCREEN_HEIGHT;
  displayDriver.flush_cb = displayFlush;
  displayDriver.rounder_cb = rounderCallback;
  displayDriver.draw_buf = &drawBuffer;
  displayDriver.sw_rotate = manualRotationBufferReady ? 0 : 1;
  lv_disp_drv_register(&displayDriver);

  static lv_indev_drv_t inputDriver;
  lv_indev_drv_init(&inputDriver);
  inputDriver.type = LV_INDEV_TYPE_POINTER;
  inputDriver.read_cb = touchRead;
  lv_indev_drv_register(&inputDriver);

  const esp_timer_create_args_t timerArgs = {
    .callback = lvglTick,
    .arg = nullptr,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "lvgl_tick",
    .skip_unhandled_events = true
  };
  if (esp_timer_create(&timerArgs, &lvglTimer) != ESP_OK ||
      esp_timer_start_periodic(lvglTimer, LVGL_TICK_MS * 1000) != ESP_OK) {
    Serial.println("Unable to start LVGL tick timer");
    return false;
  }

  createClockUi();
  lv_mem_monitor_t uiMemory;
  lv_mem_monitor(&uiMemory);
  Serial.printf("LVGL object pool: %u KiB total, %u KiB free, %u%% fragmented\n",
                static_cast<unsigned>(uiMemory.total_size / 1024U),
                static_cast<unsigned>(uiMemory.free_size / 1024U),
                static_cast<unsigned>(uiMemory.frag_pct));
  updateScreenOrientation(true);
  lv_timer_handler();
  Serial.println("LVGL ready");
  return true;
}

void updateNetworkLabels() {
  if (!statusLabel || !addressLabel) return;
  if (WiFi.status() == WL_CONNECTED) {
    lv_label_set_text_fmt(statusLabel, "%s", WiFi.SSID().c_str());
  } else if (portalActive) {
    lv_label_set_text(statusLabel, tr(STR_CONFIG_MODE));
  } else {
    lv_label_set_text(statusLabel, tr(STR_CONNECTING));
  }

  if (portalActive) {
    lv_label_set_text_fmt(addressLabel, tr(STR_AP_OPEN_NETWORK), apSsid);
  } else if (WiFi.status() == WL_CONNECTED) {
    lv_label_set_text_fmt(addressLabel, tr(STR_PHONE_UI), WiFi.localIP().toString().c_str());
  } else {
    lv_label_set_text(addressLabel, tr(STR_TAP_SETTINGS_SETUP));
  }
  updateControlPanelStatus();
}

void setDisplayedTime(const char *text) {
  if (strcmp(displayedTimeText, text) == 0) return;

  strlcpy(displayedTimeText, text, sizeof(displayedTimeText));
  // lv_label_set_text() invalidates the old and new label bounds itself.
  // Keep that small invalid area: invalidating the whole time panel makes
  // LVGL's 90/270-degree software rotation split one clock update into
  // multiple display writes, which is visible as a tear on the AMOLED.
  lv_label_set_text(timeLabel, displayedTimeText);
  resizeTimePanelForText(displayedTimeText);
  lv_obj_center(timeLabel);
}

void updateClock() {
  // Re-evaluate the centered/top-left layout whenever a background image
  // appears or disappears (cheap flag check; re-layout only on change).
  if (!lowPowerMode && timePanel) {
    const bool hasBackground =
      backgroundImage && !lv_obj_has_flag(backgroundImage, LV_OBJ_FLAG_HIDDEN);
    if (mainClockCentered == hasBackground) refreshMainClockLayout();
  }
  if (!rtcReady || !rtcTimeValid) {
    setDisplayedTime("--:--");
    return;
  }

  RTC_DateTime rtcTime;
  if (!readRtcTime(rtcTime)) return;

  char timeText[8];
  snprintf(timeText, sizeof(timeText), "%02d:%02d",
           rtcTime.getHour(), rtcTime.getMinute());
  setDisplayedTime(timeText);
}

bool allocateBackgroundBuffer() {
  if (backgroundPixels) return true;
  backgroundPixels = static_cast<uint8_t *>(heap_caps_malloc(
    IMAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!backgroundPixels) {
    backgroundPixels = static_cast<uint8_t *>(malloc(IMAGE_BYTES));
  }
  if (!backgroundPixels) {
    Serial.println("Unable to allocate image buffer");
    return false;
  }

  backgroundDescriptor.header.always_zero = 0;
  backgroundDescriptor.header.w = SCREEN_WIDTH;
  backgroundDescriptor.header.h = SCREEN_HEIGHT;
  backgroundDescriptor.header.cf = LV_IMG_CF_TRUE_COLOR;
  backgroundDescriptor.data_size = IMAGE_BYTES;
  backgroundDescriptor.data = backgroundPixels;
  return true;
}

bool loadBackgroundImage() {
  const char *imagePath = currentImagePath();
  if (!imagePath || !LittleFS.exists(imagePath)) {
    lv_obj_add_flag(backgroundImage, LV_OBJ_FLAG_HIDDEN);
    return false;
  }
  if (!allocateBackgroundBuffer()) {
    lv_obj_add_flag(backgroundImage, LV_OBJ_FLAG_HIDDEN);
    return false;
  }

  File file = LittleFS.open(imagePath, FILE_READ);
  if (!file || file.size() != IMAGE_BYTES) {
    if (file) file.close();
    lv_obj_add_flag(backgroundImage, LV_OBJ_FLAG_HIDDEN);
    return false;
  }
  const size_t readBytes = file.read(backgroundPixels, IMAGE_BYTES);
  file.close();
  if (readBytes != IMAGE_BYTES) {
    lv_obj_add_flag(backgroundImage, LV_OBJ_FLAG_HIDDEN);
    return false;
  }

  lv_img_cache_invalidate_src(&backgroundDescriptor);
  lv_img_set_src(backgroundImage, &backgroundDescriptor);
  lv_obj_clear_flag(backgroundImage, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_background(backgroundImage);
  lv_obj_invalidate(backgroundImage);
  return true;
}
