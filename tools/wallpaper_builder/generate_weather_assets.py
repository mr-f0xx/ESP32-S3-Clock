"""Build LVGL Alpha-8 weather images from official Tabler outline PNGs."""

from pathlib import Path
import sys

from PIL import Image


ICONS = (
    "sun",
    "moon",
    "cloud",
    "cloud-fog",
    "cloud-rain",
    "cloud-snow",
    "cloud-storm",
    "cloud-question",
)
ICON_SIZE = 48


def identifier(name: str) -> str:
    return "tabler_weather_" + name.replace("-", "_")


def format_bytes(data: bytes) -> str:
    rows = []
    for offset in range(0, len(data), 16):
        chunk = ", ".join(f"0x{value:02X}" for value in data[offset:offset + 16])
        rows.append(f"  {chunk},")
    return "\n".join(rows)


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: generate_weather_assets.py INPUT_DIR OUTPUT_HEADER")

    input_dir = Path(sys.argv[1])
    output_path = Path(sys.argv[2])
    sections = [
        "#pragma once",
        "",
        "// Generated from official @tabler/icons-png 3.45.0 outline artwork.",
        "// Tabler Icons: MIT license, https://github.com/tabler/tabler-icons",
        "// Native 48 px LVGL Alpha-8 images, recolored by the LVGL image object.",
        "",
    ]

    for name in ICONS:
        source = input_dir / f"{name}.png"
        image = Image.open(source).convert("RGBA")
        image = image.resize((ICON_SIZE, ICON_SIZE), Image.Resampling.LANCZOS)
        alpha = image.getchannel("A").tobytes()
        symbol = identifier(name)
        sections.extend([
            f"static const uint8_t {symbol}_map[] PROGMEM = {{",
            format_bytes(alpha),
            "};",
            f"static const lv_img_dsc_t {symbol} = {{",
            "  .header = {",
            "    .cf = LV_IMG_CF_ALPHA_8BIT,",
            "    .always_zero = 0,",
            "    .reserved = 0,",
            f"    .w = {ICON_SIZE},",
            f"    .h = {ICON_SIZE},",
            "  },",
            f"  .data_size = sizeof({symbol}_map),",
            f"  .data = {symbol}_map,",
            "};",
            "",
        ])

    output_path.write_text("\n".join(sections), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
