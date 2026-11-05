#!/usr/bin/env python3
"""Regenerate native clock glyphs with lv_font_conv 1.5.3 and fonttools.

Sources are pinned to a Google Fonts revision. Use --sizes 72 to add only the
medium size without rewriting the existing 48/96 px assets. No runtime image or
font zoom is needed. Orbitron and Cinzel retain their existing bold weight.
"""
import argparse
import json
import pathlib
import shutil
import subprocess
import tempfile
import urllib.parse
import urllib.request

REVISION = "eb20e9cf19d97cf579fa3867d729d4222d6c59e1"
ROOT = pathlib.Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "arduino/08_LVGL_Clock/src/fonts"
FONTS = {
    "kameron": ("ofl/kameron/Kameron[wght].ttf", None),
    "silkscreen": ("ofl/silkscreen/Silkscreen-Regular.ttf", None),
    "dotgothic16": ("ofl/dotgothic16/DotGothic16-Regular.ttf", None),
    "orbitron": ("ofl/orbitron/Orbitron[wght].ttf", 700),
    "audiowide": ("ofl/audiowide/Audiowide-Regular.ttf", None),
    "cinzel": ("ofl/cinzel/Cinzel[wght].ttf", 700),
    "bebasneue": ("ofl/bebasneue/BebasNeue-Regular.ttf", None),
    "digitalnumbers": ("ofl/digitalnumbers/DigitalNumbers-Regular.ttf", None),
    "rajdhani": ("ofl/rajdhani/Rajdhani-SemiBold.ttf", None),
    "robotomono": ("ofl/robotomono/RobotoMono[wght].ttf", None),
    "neonlines": ("ofl/monoton/Monoton-Regular.ttf", None),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sizes", type=int, nargs="+", choices=(48, 72, 96), default=[48, 72, 96])
    args = parser.parse_args()
    converter = shutil.which("lv_font_conv")
    if not converter:
        parser.error("lv_font_conv 1.5.3 must be in PATH")
    with tempfile.TemporaryDirectory() as tmp:
        for name, (path, weight) in FONTS.items():
            url = ("https://api.github.com/repos/google/fonts/contents/"
                   + urllib.parse.quote(path) + "?ref=" + REVISION)
            request = urllib.request.Request(url, headers={"User-Agent": "esp32-clock-fonts"})
            with urllib.request.urlopen(request, timeout=60) as response:
                source = json.load(response)
            # The contents API omits base64 for files over 1 MiB. Fetch the
            # pinned Git blob instead, which supports raw responses up to 100 MiB.
            blob_url = f"https://api.github.com/repos/google/fonts/git/blobs/{source['sha']}"
            blob_request = urllib.request.Request(blob_url, headers={
                "User-Agent": "esp32-clock-fonts", "Accept": "application/vnd.github.raw+json"})
            with urllib.request.urlopen(blob_request, timeout=60) as response:
                data = response.read()
            if len(data) != source["size"]:
                raise RuntimeError(f"Incomplete font download: {name}")
            font = pathlib.Path(tmp) / (name + ".ttf")
            font.write_bytes(data)
            print(f"{name}: {len(data)} bytes", flush=True)
            # Normalize and subset before handing the font to lv_font_conv's
            # older OpenType parser (large CFF tables can exceed its DataView).
            from fontTools.ttLib import TTFont
            from fontTools import subset
            face = TTFont(font)
            if weight is not None:
                from fontTools.varLib.instancer import instantiateVariableFont
                instantiateVariableFont(face, {"wght": weight}, inplace=True)
            options = subset.Options()
            options.name_IDs = ["*"]
            options.name_legacy = True
            options.name_languages = ["*"]
            subsetter = subset.Subsetter(options=options)
            subsetter.populate(text="0123456789:-")
            subsetter.subset(face)
            face.save(font)
            face.close()
            for size in args.sizes:
                output = OUTPUT / f"lv_font_{name}_{size}.c"
                subprocess.run([
                    converter, "--bpp", "4", "--size", str(size), "--font", str(font),
                    "--range", "0x30-0x3A,0x2D", "--format", "lvgl", "--no-compress",
                    "--lv-font-name", f"lv_font_{name}_{size}", "--lv-include", "lvgl.h",
                    "-o", str(output),
                ], check=True)
                text = output.read_text()
                text = text.replace(str(font), f"google/fonts/{path}").replace(str(output), output.name)
                output.write_text(text)
        font = ROOT / "arduino/libraries/lvgl/scripts/built_in_font/Montserrat-Medium.ttf"
        for size in args.sizes:
            output = OUTPUT / f"lv_font_montserrat_time_{size}.c"
            subprocess.run([
                converter, "--bpp", "4", "--size", str(size), "--font", str(font),
                "--range", "0x30-0x3A,0x2D", "--format", "lvgl", "--no-compress",
                "--lv-font-name", f"lv_font_montserrat_time_{size}", "--lv-include", "lvgl.h",
                "-o", str(output),
            ], check=True)
            text = output.read_text().replace(str(font), "lvgl/scripts/built_in_font/Montserrat-Medium.ttf")
            text = text.replace(str(output), output.name)
            output.write_text(text)


if __name__ == "__main__":
    main()
