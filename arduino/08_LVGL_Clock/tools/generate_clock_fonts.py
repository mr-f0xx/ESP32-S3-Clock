#!/usr/bin/env python3
"""Regenerate the four clock faces (requires lv_font_conv 1.5.3 in PATH).

Source files are fetched from a pinned Google Fonts revision. Only clock glyphs
are generated; original OFL notices are in licenses/clock_fonts/.
"""
import base64
import json
import pathlib
import subprocess
import tempfile
import urllib.parse
import urllib.request

REVISION = "eb20e9cf19d97cf579fa3867d729d4222d6c59e1"
FONTS = {
    "digitalnumbers": "ofl/digitalnumbers/DigitalNumbers-Regular.ttf",
    "rajdhani": "ofl/rajdhani/Rajdhani-SemiBold.ttf",
    "robotomono": "ofl/robotomono/RobotoMono[wght].ttf",
    "neonlines": "ofl/monoton/Monoton-Regular.ttf",
}
OUTPUT = pathlib.Path(__file__).resolve().parents[1] / "src" / "fonts"

with tempfile.TemporaryDirectory() as tmp:
    for name, path in FONTS.items():
        url = ("https://api.github.com/repos/google/fonts/contents/"
               + urllib.parse.quote(path) + "?ref=" + REVISION)
        request = urllib.request.Request(url, headers={"User-Agent": "esp32-clock-fonts"})
        with urllib.request.urlopen(request) as response:
            source = json.load(response)
        font = pathlib.Path(tmp) / (name + ".ttf")
        font.write_bytes(base64.b64decode(source["content"]))
        for size in (48, 96):
            output = OUTPUT / f"lv_font_{name}_{size}.c"
            subprocess.run([
                "lv_font_conv", "--bpp", "4", "--size", str(size), "--font", str(font),
                "--range", "0x30-0x3A,0x2D", "--format", "lvgl", "--no-compress",
                "--lv-font-name", f"lv_font_{name}_{size}", "--lv-include", "lvgl.h",
                "-o", str(output),
            ], check=True)
            # lv_font_conv records transient absolute paths in its generated header.
            text = output.read_text()
            text = text.replace(str(font), f"google/fonts/{path}").replace(str(output), output.name)
            output.write_text(text)
