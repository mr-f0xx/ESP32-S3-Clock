"""Convert official @tabler/icons-png artwork to native-size LVGL Alpha-8 images."""

from pathlib import Path
from urllib.request import urlopen
import sys

from PIL import Image


TABLER_VERSION = "3.45.0"


ICONS = {
    "wifi": 56,
    "bluetooth": 56,
    "lock": 56,
    "volume": 48,
    "sun": 48,
    "battery": 56,
    "battery-1": 56,
    "battery-2": 56,
    "battery-3": 56,
    "battery-4": 56,
    "battery-charging": 56,
    "battery-off": 56,
    # Clock-mode selector artwork. Keep these at their final LVGL size because
    # Alpha-8 sources must not be zoomed at runtime.
    "photo": 80,
    # App-menu artwork. Every tile uses the same native canvas and the official
    # Tabler outline source so stroke weight and optical scale stay consistent.
    "clock": 80,
    "alarm": 80,
    "adjustments": 80,
    "music": 80,
    "calendar": 80,
    "video": 80,
    "device-tv": 80,
    "cloud": 80,
    "x": 32,
}


def identifier(name: str) -> str:
    return "tabler_" + name.replace("-", "_")


def format_bytes(data: bytes) -> str:
    lines = []
    for offset in range(0, len(data), 16):
        chunk = ", ".join(f"0x{value:02X}" for value in data[offset:offset + 16])
        lines.append(f"  {chunk},")
    return "\n".join(lines)


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: generate_tabler_control_assets.py INPUT_DIR OUTPUT_HEADER")
    input_dir = Path(sys.argv[1])
    output_path = Path(sys.argv[2])
    sections = [
        "#pragma once",
        "",
        "// Generated from official @tabler/icons-png outline artwork.",
        "// Native final sizes; do not use lv_img_set_zoom on Alpha-only images.",
        "// Format: LVGL 8-bit alpha, recolored at runtime.",
        "",
    ]
    for name, size in ICONS.items():
        source = input_dir / f"{name}.png"
        if source.exists():
            image = Image.open(source).convert("RGBA")
        else:
            url = (
                f"https://unpkg.com/@tabler/icons-png@{TABLER_VERSION}"
                f"/icons/outline/{name}.png"
            )
            with urlopen(url, timeout=30) as response:
                from io import BytesIO
                image = Image.open(BytesIO(response.read())).convert("RGBA")
        image = image.resize((size, size), Image.Resampling.LANCZOS)
        data = image.getchannel("A").tobytes()
        symbol = identifier(name)
        sections.extend([
            f"static const uint8_t {symbol}_map[] PROGMEM = {{",
            format_bytes(data),
            "};",
            f"static const lv_img_dsc_t {symbol} = {{",
            "  .header = {",
            "    .cf = LV_IMG_CF_ALPHA_8BIT,",
            "    .always_zero = 0,",
            "    .reserved = 0,",
            f"    .w = {size},",
            f"    .h = {size},",
            "  },",
            f"  .data_size = sizeof({symbol}_map),",
            f"  .data = {symbol}_map,",
            "};",
            "",
        ])
    output_path.write_text("\n".join(sections), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
