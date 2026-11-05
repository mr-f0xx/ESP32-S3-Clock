#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_font_t lv_font_kameron_72;
extern const lv_font_t lv_font_silkscreen_72;
extern const lv_font_t lv_font_dotgothic16_72;
extern const lv_font_t lv_font_orbitron_72;
extern const lv_font_t lv_font_audiowide_72;
extern const lv_font_t lv_font_cinzel_72;
extern const lv_font_t lv_font_bebasneue_72;
extern const lv_font_t lv_font_digitalnumbers_72;
extern const lv_font_t lv_font_rajdhani_72;
extern const lv_font_t lv_font_robotomono_72;
extern const lv_font_t lv_font_neonlines_72;
extern const lv_font_t lv_font_montserrat_time_72;

extern const lv_font_t lv_font_kameron_48;
extern const lv_font_t lv_font_silkscreen_48;
extern const lv_font_t lv_font_dotgothic16_48;
extern const lv_font_t lv_font_montserrat_time_96;
extern const lv_font_t lv_font_montserrat_alarm_bold_96;
extern const lv_font_t lv_font_kameron_96;
extern const lv_font_t lv_font_silkscreen_96;
extern const lv_font_t lv_font_dotgothic16_96;
extern const lv_font_t lv_font_orbitron_48;
extern const lv_font_t lv_font_orbitron_96;
extern const lv_font_t lv_font_audiowide_48;
extern const lv_font_t lv_font_audiowide_96;
extern const lv_font_t lv_font_cinzel_48;
extern const lv_font_t lv_font_cinzel_96;
extern const lv_font_t lv_font_bebasneue_48;
extern const lv_font_t lv_font_bebasneue_96;
extern const lv_font_t lv_font_digitalnumbers_48;
extern const lv_font_t lv_font_digitalnumbers_96;
extern const lv_font_t lv_font_rajdhani_48;
extern const lv_font_t lv_font_rajdhani_96;
extern const lv_font_t lv_font_robotomono_48;
extern const lv_font_t lv_font_robotomono_96;
extern const lv_font_t lv_font_neonlines_48;
extern const lv_font_t lv_font_neonlines_96;
extern const lv_font_t lv_font_inter_weather_52;
extern const lv_font_t lv_font_montserrat_25;

// French accent supplements (Latin-1 letters, ligatures and typographic
// punctuation) generated from the same Montserrat-Medium source as the LVGL
// built-in fonts. They are chained as the fallback of the RAM font copies
// below so every UI size can render é è ê à ç ù œ « » … without replacing the
// base ASCII glyphs.
extern const lv_font_t lv_font_accent_14;
extern const lv_font_t lv_font_accent_16;
extern const lv_font_t lv_font_accent_18;
extern const lv_font_t lv_font_accent_20;
extern const lv_font_t lv_font_accent_22;
extern const lv_font_t lv_font_accent_24;
extern const lv_font_t lv_font_accent_25;
extern const lv_font_t lv_font_accent_28;
extern const lv_font_t lv_font_accent_30;
extern const lv_font_t lv_font_accent_32;

#ifdef __cplusplus
}
#endif

// Runtime copies of the built-in Montserrat fonts with the matching accent
// fallback attached. Defined in clock_ui.inc and initialized by initUiFonts()
// before any widget is created. UI code references these instead of the const
// built-ins so French text renders correctly at every size.
extern lv_font_t lv_font_ui_14;
extern lv_font_t lv_font_ui_16;
extern lv_font_t lv_font_ui_18;
extern lv_font_t lv_font_ui_20;
extern lv_font_t lv_font_ui_22;
extern lv_font_t lv_font_ui_24;
extern lv_font_t lv_font_ui_25;
extern lv_font_t lv_font_ui_28;
extern lv_font_t lv_font_ui_30;
extern lv_font_t lv_font_ui_32;
