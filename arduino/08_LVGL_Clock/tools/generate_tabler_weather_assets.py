"""Generate native 48 px LVGL Alpha-8 weather icons from Tabler artwork."""

from io import BytesIO
from pathlib import Path
import sys
from urllib.request import urlopen

from PIL import Image, ImageChops, ImageDraw


TABLER_VERSION = "3.45.0"
SOURCE_SIZE = 240
OUTPUT_SIZE = 48
OFFICIAL_ICONS = (
    "sun",
    "moon",
    "cloud",
    "mist",
    "cloud-rain",
    "cloud-snow",
    "snowflake",
    "cloud-storm",
    "temperature-snow",
    "temperature-sun",
    "cloud-question",
)


def download_alpha(name: str) -> Image.Image:
    url = (
        f"https://unpkg.com/@tabler/icons-png@{TABLER_VERSION}"
        f"/icons/outline/{name}.png"
    )
    with urlopen(url, timeout=30) as response:
        image = Image.open(BytesIO(response.read())).convert("RGBA")
    if image.size != (SOURCE_SIZE, SOURCE_SIZE):
        image = image.resize((SOURCE_SIZE, SOURCE_SIZE), Image.Resampling.LANCZOS)
    return image.getchannel("A")


def shifted(image: Image.Image, x: int, y: int) -> Image.Image:
    result = Image.new("L", image.size)
    result.paste(image, (x, y))
    return result


def cloudy_composite(sun_high: Image.Image, cloud: Image.Image) -> Image.Image:
    """Keep more cloud at lower-left and more sun-high at upper-right."""
    size = SOURCE_SIZE
    unit = size // 24
    upper_right = Image.new("L", (size, size))
    lower_left = Image.new("L", (size, size))
    ImageDraw.Draw(upper_right).polygon(
        ((0, 0), (size - 1, 0), (size - 1, size - 1)), fill=255
    )
    ImageDraw.Draw(lower_left).polygon(
        ((0, 0), (0, size - 1), (size - 1, size - 1)), fill=255
    )

    sun_part = ImageChops.multiply(
        shifted(sun_high, unit, -unit), upper_right
    )
    cloud_part = ImageChops.multiply(
        shifted(cloud, -unit, unit), lower_left
    )
    result = ImageChops.lighter(sun_part, cloud_part)

    draw = ImageDraw.Draw(result)
    start = 2 * unit
    end = 22 * unit
    width = 2 * unit
    radius = width // 2
    draw.line((start, start, end, end), fill=255, width=width)
    draw.ellipse(
        (start - radius, start - radius, start + radius, start + radius),
        fill=255,
    )
    draw.ellipse(
        (end - radius, end - radius, end + radius, end + radius),
        fill=255,
    )
    return result


def identifier(name: str) -> str:
    return "tabler_weather_" + name.replace("-", "_")


def format_bytes(data: bytes) -> str:
    lines = []
    for offset in range(0, len(data), 16):
        chunk = ", ".join(
            f"0x{value:02X}" for value in data[offset:offset + 16]
        )
        lines.append(f"  {chunk},")
    return "\n".join(lines)


def append_icon(sections: list[str], name: str, alpha: Image.Image) -> None:
    alpha = alpha.resize((OUTPUT_SIZE, OUTPUT_SIZE), Image.Resampling.LANCZOS)
    symbol = identifier(name)
    sections.extend(
        [
            f"static const uint8_t {symbol}_map[] PROGMEM = {{",
            format_bytes(alpha.tobytes()),
            "};",
            f"static const lv_img_dsc_t {symbol} = {{",
            "  .header = {",
            "    .cf = LV_IMG_CF_ALPHA_8BIT,",
            "    .always_zero = 0,",
            "    .reserved = 0,",
            f"    .w = {OUTPUT_SIZE},",
            f"    .h = {OUTPUT_SIZE},",
            "  },",
            f"  .data_size = sizeof({symbol}_map),",
            f"  .data = {symbol}_map,",
            "};",
            "",
        ]
    )


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: generate_tabler_weather_assets.py OUTPUT_HEADER")

    icons = {name: download_alpha(name) for name in OFFICIAL_ICONS}
    sun_high = download_alpha("sun-high")
    icons["cloudy-composite"] = cloudy_composite(sun_high, icons["cloud"])

    order = (
        "sun",
        "moon",
        "cloud",
        "cloudy-composite",
        "mist",
        "cloud-rain",
        "cloud-snow",
        "snowflake",
        "cloud-storm",
        "temperature-snow",
        "temperature-sun",
        "cloud-question",
    )
    sections = [
        "#pragma once",
        "",
        f"// Generated from official @tabler/icons-png {TABLER_VERSION} outline artwork.",
        "// Tabler Icons: MIT license, https://github.com/tabler/tabler-icons",
        "// Native 48 px LVGL Alpha-8 images, recolored by the LVGL image object.",
        "// cloudy-composite combines clipped sun-high and cloud Tabler geometry.",
        "",
    ]
    for name in order:
        append_icon(sections, name, icons[name])

    Path(sys.argv[1]).write_text(
        "\n".join(sections), encoding="utf-8", newline="\n"
    )


if __name__ == "__main__":
    main()
