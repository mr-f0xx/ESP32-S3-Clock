# Clock font licenses

The generated LVGL bitmap subsets in this directory use these Google Fonts:

- Kameron
- Silkscreen
- DotGothic16
- Inter (weather time and temperature)
- Montserrat (UI accent fallback, `lv_font_accent_*`)

All of these source families are distributed under the SIL Open Font License 1.1.
Only `0-9`, `:` and `-` were converted at 48 px for the clock display.
The Inter weather subset contains space, `-`, `0-9`, `:`, and `\u00b0` at a true
52 px size; LVGL object scaling is not used.

The Montserrat accent subsets (`lv_font_accent_14` … `lv_font_accent_32`, 4 bpp)
carry only the non-ASCII glyphs needed for French: `0xAB 0xBB 0xC0-0xFF
0x152-0x153 0x178 0x2013-0x2014 0x2018-0x2019 0x201C-0x201D 0x2026 0x20AC`
(« », À-ÿ covering é è ê ë à â ç î ï ô ù û ü ÿ, Œ œ Ÿ, dashes, quotes, …, €).
ASCII still comes from LVGL's built-in Montserrat. At run time `initUiFonts()`
copies each built-in size into a RAM `lv_font_ui_*` struct and attaches the
matching accent subset as its `fallback`, so the UI renders French without
modifying LVGL's const flash fonts. Generated with `lv_font_conv` from the
upstream Montserrat Medium TTF.

Source: https://github.com/google/fonts
Montserrat: https://github.com/JulietaUla/Montserrat
License: https://openfontlicense.org/open-font-license-official-text/
