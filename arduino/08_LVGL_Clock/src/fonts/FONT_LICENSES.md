# Clock font licenses

The generated LVGL bitmap subsets in this directory use these Google Fonts:

- Kameron
- Silkscreen
- DotGothic16
- Inter (weather time and temperature)
- Montserrat (UI accent fallback, `lv_font_accent_*`)
- Orbitron (retro-futuristic clock face)
- Audiowide (retro-futuristic clock face)
- Cinzel (elegant serif clock face)
- Bebas Neue (condensed clock face)
- Digital Numbers (seven-segment clock face)
- Rajdhani (geometric clock face)
- Roboto Mono (monospaced clock face)
- Neon Lines (a retro clock subset derived from Monoton)

All of these source families are distributed under the SIL Open Font License 1.1.
Only `0-9`, `:` and `-` were converted at 48 px, 72 px and 120 px for the clock display.
Orbitron and Cinzel are variable fonts upstream; the converted subsets were
instantiated at weight 700 before running `lv_font_conv`.
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

The clock font sources are pinned at Google Fonts revision `eb20e9cf19d97cf579fa3867d729d4222d6c59e1`. Their individual copyright and OFL notices are included in [`licenses/clock_fonts`](../../../../licenses/clock_fonts). Regenerate with `tools/generate_clock_fonts.py` (fonttools 4.60.1) and `lv_font_conv` 1.5.3; use `--sizes 72` to add only the medium assets. Roboto Mono uses its source font default variable weight.

Neon Lines is a converted subset of Monoton, renamed because Monoton is a reserved font name.
