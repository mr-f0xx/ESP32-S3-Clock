# Weather UI preview

This 480 x 480 browser mock mirrors the pixel coordinates, font sizes, line
heights, colors, borders and radii used by `clock_weather.inc`.

Run `serve.ps1` with PowerShell, then open `http://127.0.0.1:8765/`. Use the page buttons,
horizontal drag, or the left/right arrow keys to switch pages. The coordinate
grid is useful when transferring a reviewed layout back to LVGL.

The bundled fonts are the Montserrat source used by LVGL and the Inter variable
font used to generate `lv_font_inter_weather_52`.
