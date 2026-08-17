#!/usr/bin/env python3
"""Build paired weather JWP files with identical landscape/rotated quality."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import math
import statistics
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import imageio_ffmpeg

from preprocess_wallpaper import find_ffmpeg, jpeg_frames


HEADER = struct.Struct("<4sHHHHHHIIII")
FRAME_LENGTH = struct.Struct("<I")

# Select quality from the landscape stream's 30 fps budget. The rotated copy
# deliberately uses the exact same ffmpeg q value so portrait assets are never
# degraded relative to their landscape counterpart.
# Calibrated from the current 80 MHz QSPI/block-pipeline logs. The higher
# quality rotated assets peak near 26.3 ms in jpeg_dec_process and retain at
# least about 4.6 ms of queue wait while holding 30 fps with zero dropped
# frames. Use only a small part of that margin so touches, fades and SD jitter
# still have roughly 3 ms of scheduling room.
DEFAULT_AVG_BUDGET_KIB = 14.5
DEFAULT_P95_BUDGET_KIB = 17.0
DEFAULT_OUTPUT_FPS = 30.0
LOW_FPS_QUALITY_THRESHOLD = 8
LOW_OUTPUT_FPS = 24.0
LOW_FPS_AVG_BUDGET_KIB = 27.0
LOW_FPS_P95_BUDGET_KIB = 32.0
JPEG_CORE_FIXED_MS = 17.0
JPEG_CORE_MS_PER_KIB = 0.69
ROTATED_BLOCK_OVERHEAD_MS = 1.5

# Video filename prefixes are the user-authored weather labels.  These are
# emitted to wallpaper_playlist.h and consumed by the firmware at runtime.
SCENE_PREFIXES: tuple[tuple[str, str], ...] = (
    # Keep these before the general overcast_ rule: they are intentionally
    # used when the weather code is overcast after local nightfall.
    ("night_overcast_", "Night"),
    ("overcast_night_", "Night"),
    ("clear_", "Clear"),
    ("cloudy_", "Cloudy"),
    ("overcast_", "Overcast"),
    ("rain_", "Rain"),
    ("thunder_", "Thunder"),
    ("snow_", "Snow"),
    ("fog_", "Fog"),
    ("haze_", "Haze"),
    ("sand_", "Sand"),
    ("night_", "Night"),
)


@dataclass(frozen=True)
class JpegMetrics:
    frames: int
    average_bytes: float
    p95_bytes: int
    maximum_bytes: int

    @property
    def average_kib(self) -> float:
        return self.average_bytes / 1024.0

    @property
    def p95_kib(self) -> float:
        return self.p95_bytes / 1024.0

    @property
    def maximum_kib(self) -> float:
        return self.maximum_bytes / 1024.0

    @property
    def estimated_core_ms(self) -> float:
        return JPEG_CORE_FIXED_MS + JPEG_CORE_MS_PER_KIB * self.average_kib


def metrics_from_sizes(sizes: list[int]) -> JpegMetrics:
    if not sizes:
        raise RuntimeError("No JPEG frames were produced")
    ordered = sorted(sizes)
    p95_index = min(len(ordered) - 1, math.ceil(len(ordered) * 0.95) - 1)
    return JpegMetrics(
        frames=len(sizes),
        average_bytes=statistics.fmean(sizes),
        p95_bytes=ordered[p95_index],
        maximum_bytes=ordered[-1],
    )


def mjpeg_metrics(path: Path) -> JpegMetrics:
    with path.open("rb") as stream:
        return metrics_from_sizes([len(frame) for frame in jpeg_frames(stream)])


def jwp_metrics(path: Path) -> tuple[tuple, JpegMetrics]:
    sizes: list[int] = []
    with path.open("rb") as stream:
        header_data = stream.read(HEADER.size)
        if len(header_data) != HEADER.size:
            raise RuntimeError(f"Short JWP header: {path}")
        header = HEADER.unpack(header_data)
        if header[0] != b"JWP1" or header[1] != 1 or header[2] != HEADER.size:
            raise RuntimeError(f"Invalid JWP1 header: {path}")
        for _ in range(header[7]):
            packed_length = stream.read(FRAME_LENGTH.size)
            if len(packed_length) != FRAME_LENGTH.size:
                raise RuntimeError(f"Short frame length table: {path}")
            frame_bytes = FRAME_LENGTH.unpack(packed_length)[0]
            if len(stream.read(frame_bytes)) != frame_bytes:
                raise RuntimeError(f"Short JPEG payload: {path}")
            sizes.append(frame_bytes)
        if stream.read(1):
            raise RuntimeError(f"Unexpected trailing JWP data: {path}")
    return header, metrics_from_sizes(sizes)


def video_duration(path: Path) -> float:
    reader = imageio_ffmpeg.read_frames(str(path))
    try:
        metadata = next(reader)
    finally:
        reader.close()
    return float(metadata["duration"])


def within_budget(metrics: JpegMetrics, avg_kib: float, p95_kib: float) -> bool:
    return metrics.average_kib <= avg_kib and metrics.p95_kib <= p95_kib


def probe_qualities(
    ffmpeg: str,
    source: Path,
    duration: float,
    qualities: list[int],
    probe_fps: float,
    temporary_dir: Path,
    rotate_ccw: bool = False,
) -> dict[int, JpegMetrics]:
    labels = "".join(f"[q{quality}]" for quality in qualities)
    rotation_filter = ",transpose=2" if rotate_ccw else ""
    base_filter = (
        f"[0:v]fps={probe_fps},"
        "scale=480:360:force_original_aspect_ratio=increase:flags=lanczos,"
        f"crop=480:360{rotation_filter},"
        f"trim=duration={duration:.6f},"
        f"setpts=N/({probe_fps}*TB),format=yuvj420p,"
        f"split={len(qualities)}{labels}"
    )
    command = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-y",
        "-i",
        str(source),
        "-an",
        "-filter_complex",
        base_filter,
    ]
    outputs: dict[int, Path] = {}
    for quality in qualities:
        suffix = "rotated" if rotate_ccw else "landscape"
        output = temporary_dir / f"probe_{suffix}_q{quality}.mjpeg"
        outputs[quality] = output
        command.extend(
            [
                "-map",
                f"[q{quality}]",
                "-c:v",
                "mjpeg",
                "-q:v",
                str(quality),
                "-f",
                "mjpeg",
                str(output),
            ]
        )

    completed = subprocess.run(command, capture_output=True, text=True)
    if completed.returncode != 0:
        raise RuntimeError(
            completed.stderr.strip()
            or f"Adaptive-quality probe exited with {completed.returncode}"
        )
    return {quality: mjpeg_metrics(outputs[quality]) for quality in qualities}


def run_converter(
    converter: Path,
    source: Path,
    output: Path,
    duration: float,
    quality: int,
    fps: float,
    rotate_ccw: bool = False,
) -> None:
    command = [
            sys.executable,
            str(converter),
            "--input",
            str(source),
            "--output",
            str(output),
            "--width",
            "480",
            "--height",
            "360",
            "--fps",
            f"{fps:g}",
            "--duration",
            f"{duration:.6f}",
            "--quality",
            str(quality),
            "--chroma",
            "420",
            "--sharpen",
            "0",
            "--no-deband",
            "--loop-crossfade-seconds",
            "0.6",
        ]
    if rotate_ccw:
        command.append("--rotate-ccw")
    subprocess.run(command, check=True)


def scene_for_source(source: Path) -> str:
    lower_name = source.name.lower()
    for prefix, scene in SCENE_PREFIXES:
        if lower_name.startswith(prefix):
            return scene
    accepted = ", ".join(prefix[:-1] for prefix, _ in SCENE_PREFIXES)
    raise SystemExit(
        f"Unlabeled video {source.name}; name it <scene>_NN.mp4. "
        f"Accepted scenes: {accepted}"
    )


def write_firmware_playlist(
    path: Path,
    sources: list[Path],
    scenes: list[str],
    relative_jwp_paths: list[Path],
) -> None:
    lines = [
        "#pragma once",
        "",
        "// Generated by tools/build_playlist.py; do not hand-edit.",
        "constexpr WallpaperAsset WALLPAPER_ASSETS[] = {",
    ]
    for source, scene, relative_jwp_path in zip(
        sources, scenes, relative_jwp_paths, strict=True
    ):
        lines.append(
            f'  {{"/wallpaper/{relative_jwp_path.as_posix()}", '
            f"WallpaperScene::{scene}, \"{source.name}\"}},"
        )
    lines.extend(
        [
            "};",
            "",
            "constexpr uint8_t WALLPAPER_COUNT =",
            "  sizeof(WALLPAPER_ASSETS) / sizeof(WALLPAPER_ASSETS[0]);",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--quality-min", type=int, default=2)
    parser.add_argument("--quality-max", type=int, default=24)
    parser.add_argument("--probe-fps", type=float, default=5.0)
    parser.add_argument(
        "--max-seconds",
        type=float,
        default=40.0,
        help="Maximum retained duration per source video (default: 40)",
    )
    parser.add_argument("--avg-budget-kib", type=float, default=DEFAULT_AVG_BUDGET_KIB)
    parser.add_argument("--p95-budget-kib", type=float, default=DEFAULT_P95_BUDGET_KIB)
    parser.add_argument(
        "--low-fps-avg-budget-kib",
        type=float,
        default=LOW_FPS_AVG_BUDGET_KIB,
        help="Average JPEG budget after q>=10 selects 24 fps (default: 23)",
    )
    parser.add_argument(
        "--low-fps-p95-budget-kib",
        type=float,
        default=LOW_FPS_P95_BUDGET_KIB,
        help="P95 JPEG budget after q>=10 selects 24 fps (default: 26)",
    )
    parser.add_argument(
        "--probe-only",
        action="store_true",
        help="Select and report qualities without replacing the production JWP files",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if not 2 <= args.quality_min <= args.quality_max <= 31:
        raise SystemExit("Quality range must satisfy 2 <= min <= max <= 31")
    if args.probe_fps <= 0:
        raise SystemExit("Probe FPS must be positive")
    if args.max_seconds <= 1.2:
        raise SystemExit("--max-seconds must exceed the 0.6 s loop fade at both ends")
    if (
        args.avg_budget_kib <= 0
        or args.p95_budget_kib <= 0
        or args.low_fps_avg_budget_kib <= 0
        or args.low_fps_p95_budget_kib <= 0
    ):
        raise SystemExit("JPEG byte budgets must be positive")

    tool_dir = Path(__file__).resolve().parent
    project_dir = tool_dir.parent.parent
    source_dir = project_dir / "media" / "source_mp4"
    output_dir = project_dir / "tfcard" / "wallpaper"
    converter = tool_dir / "preprocess_wallpaper.py"
    ffmpeg = find_ffmpeg(None)
    sources = sorted(source_dir.glob("*.mp4"), key=lambda path: path.name.lower())
    if not sources:
        raise SystemExit(f"No MP4 files found in {source_dir}")
    if len(sources) > 255:
        raise SystemExit("The firmware playlist index is limited to 255 files")
    scenes = [scene_for_source(source) for source in sources]

    qualities = list(range(args.quality_min, args.quality_max + 1))
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest_rows: list[list[str]] = []
    relative_jwp_paths: list[Path] = []
    selection_rows: list[
        tuple[int, str, int, float, int, JpegMetrics, bool]
    ] = []

    for index, source in enumerate(sources):
        source_seconds = video_duration(source)
        kept_seconds = min(args.max_seconds, source_seconds)
        if kept_seconds <= 1.2:
            raise SystemExit(f"Video is too short for a 0.6 s loop fade: {source}")

        print(
            f"\n[{index + 1}/{len(sources)}] [{scenes[index].lower()}] {source.name}: "
            f"{source_seconds:.2f} s -> {kept_seconds:.2f} s"
        )
        with tempfile.TemporaryDirectory(prefix="jwp_quality_") as temporary:
            temporary_path = Path(temporary)
            probe = probe_qualities(
                ffmpeg,
                source,
                kept_seconds,
                qualities,
                args.probe_fps,
                temporary_path,
            )

        selected_quality = args.quality_max
        for quality in qualities:
            metrics = probe[quality]
            passes = within_budget(
                metrics, args.avg_budget_kib, args.p95_budget_kib
            )
            print(
                f"  probe q{quality}: avg {metrics.average_kib:5.1f} KiB, "
                f"p95 {metrics.p95_kib:5.1f} KiB, "
                f"estimated JPEG core {metrics.estimated_core_ms:4.1f} ms "
                f"{'PASS' if passes else 'over'}"
            )
            if passes:
                selected_quality = quality
                break

        initial_30fps_quality = selected_quality
        target_fps = DEFAULT_OUTPUT_FPS
        target_avg_budget_kib = args.avg_budget_kib
        target_p95_budget_kib = args.p95_budget_kib

        if initial_30fps_quality >= LOW_FPS_QUALITY_THRESHOLD:
            target_fps = LOW_OUTPUT_FPS
            target_avg_budget_kib = args.low_fps_avg_budget_kib
            target_p95_budget_kib = args.low_fps_p95_budget_kib
            selected_quality = args.quality_max
            print(
                f"  30 fps selected q{initial_30fps_quality}; switching to "
                f"{target_fps:g} fps with avg/p95 "
                f"{target_avg_budget_kib:g}/{target_p95_budget_kib:g} KiB"
            )
            for quality in qualities:
                metrics = probe[quality]
                passes = within_budget(
                    metrics, target_avg_budget_kib, target_p95_budget_kib
                )
                print(
                    f"  {target_fps:g} fps q{quality}: "
                    f"avg {metrics.average_kib:5.1f} KiB, "
                    f"p95 {metrics.p95_kib:5.1f} KiB, "
                    f"estimated JPEG core {metrics.estimated_core_ms:4.1f} ms "
                    f"{'PASS' if passes else 'over'}"
                )
                if passes:
                    selected_quality = quality
                    break

        selected_probe = probe[selected_quality]
        selected_passes = within_budget(
            selected_probe, target_avg_budget_kib, target_p95_budget_kib
        )
        selection_rows.append(
            (
                index,
                source.name,
                initial_30fps_quality,
                target_fps,
                selected_quality,
                selected_probe,
                selected_passes,
            )
        )
        print(f"  selected {target_fps:g} fps q{selected_quality}")

        print(
            f"  rotated output locked to landscape "
            f"{target_fps:g} fps q{selected_quality}"
        )
        if args.probe_only:
            continue

        # Keep the TF-card layout understandable without a manifest: each
        # weather scene gets its own folder and the source stem stays visible.
        # For example: /wallpaper/rain/rain_02.jwp.
        relative_jwp_path = Path(scenes[index].lower()) / f"{source.stem}.jwp"
        output = output_dir / relative_jwp_path
        output.parent.mkdir(parents=True, exist_ok=True)
        quality = selected_quality
        while True:
            run_converter(
                converter, source, output, kept_seconds, quality, target_fps
            )
            header, final_metrics = jwp_metrics(output)
            final_passes = within_budget(
                final_metrics, target_avg_budget_kib, target_p95_budget_kib
            )
            print(
                f"  final {target_fps:g} fps q{quality}: "
                f"avg {final_metrics.average_kib:.1f} KiB, "
                f"p95 {final_metrics.p95_kib:.1f} KiB, "
                f"estimated JPEG core {final_metrics.estimated_core_ms:.1f} ms "
                f"{'PASS' if final_passes else 'over'}"
            )
            if final_passes or quality >= args.quality_max:
                break
            quality += 1
            print(f"  final stream exceeded budget; retrying at q{quality}")

        if not final_passes:
            print(
                f"  WARNING: q{quality} still exceeds the target; keeping q{quality} "
                "as the fastest allowed quality"
            )
        # The portrait copy lives beside the landscape files in a per-scene
        # rotated folder. It is the same 480x360 composition rotated CCW into
        # a 360x480 JWP, so the clock never has to rotate JPEG pixels at run time.
        relative_rotated_path = (
            Path(scenes[index].lower()) / "rotated" / f"{source.stem}.jwp"
        )
        rotated_output = output_dir / relative_rotated_path
        rotated_output.parent.mkdir(parents=True, exist_ok=True)
        rotated_quality = quality
        run_converter(
            converter, source, rotated_output, kept_seconds,
            rotated_quality, target_fps, rotate_ccw=True,
        )
        rotated_header, rotated_metrics = jwp_metrics(rotated_output)
        rotated_passes = within_budget(
            rotated_metrics, target_avg_budget_kib, target_p95_budget_kib
        )
        print(
            f"  rotated {target_fps:g} fps q{rotated_quality} "
            f"(same as landscape): "
            f"avg {rotated_metrics.average_kib:.1f} KiB, "
            f"p95 {rotated_metrics.p95_kib:.1f} KiB, "
            f"estimated JPEG core "
            f"{rotated_metrics.estimated_core_ms + ROTATED_BLOCK_OVERHEAD_MS:.1f} ms "
            f"{'PASS' if rotated_passes else 'over budget; quality preserved'}"
        )

        if rotated_header[3:5] != (360, 480):
            raise RuntimeError(
                f"Rotated JWP has unexpected dimensions: {relative_rotated_path}"
            )
        expected_fps_times_100 = round(target_fps * 100)
        if (
            header[5] != expected_fps_times_100
            or rotated_header[5] != expected_fps_times_100
            or rotated_header[7] != header[7]
        ):
            raise RuntimeError(
                f"Landscape/rotated frame timing differs: {relative_jwp_path}"
            )

        manifest_rows.append(
            [
                str(index),
                scenes[index].lower(),
                relative_jwp_path.as_posix(),
                source.name,
                f"{source_seconds:.2f}",
                f"{kept_seconds:.2f}",
                f"{target_fps:g}",
                str(initial_30fps_quality),
                str(quality),
                str(header[7]),
                str(output.stat().st_size),
                f"{final_metrics.average_kib:.2f}",
                f"{final_metrics.p95_kib:.2f}",
                f"{final_metrics.maximum_kib:.2f}",
                f"{final_metrics.estimated_core_ms:.2f}",
                "yes" if final_passes else "no",
                f"{header[9]:08X}",
                relative_rotated_path.as_posix(),
                str(rotated_quality),
                str(rotated_output.stat().st_size),
                f"{rotated_metrics.average_kib:.2f}",
                f"{rotated_metrics.p95_kib:.2f}",
                "yes" if rotated_passes else "no",
                f"{rotated_header[9]:08X}",
            ]
        )
        relative_jwp_paths.append(relative_jwp_path)

    print("\nAdaptive quality selections:")
    for (
        index,
        source_name,
        initial_quality,
        target_fps,
        quality,
        metrics,
        passes,
    ) in selection_rows:
        initial_note = (
            f", 30fps probe q{initial_quality}"
            if target_fps != DEFAULT_OUTPUT_FPS
            else ""
        )
        print(
            f"  wallpaper{index}: {target_fps:g} fps q{quality}"
            f"{initial_note}, {source_name}, "
            f"probe avg/p95 {metrics.average_kib:.1f}/{metrics.p95_kib:.1f} KiB, "
            f"{'PASS' if passes else 'best effort'}"
        )

    if args.probe_only:
        print("\nProbe-only mode: production JWP files were not changed.")
        return

    manifest = output_dir / "SOURCE.csv"
    with manifest.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "index",
                "scene",
                "jwp_file",
                "source_file",
                "source_seconds",
                "input_seconds",
                "output_fps",
                "initial_30fps_quality",
                "jpeg_quality",
                "output_frames",
                "jwp_bytes",
                "avg_jpeg_kib",
                "p95_jpeg_kib",
                "max_jpeg_kib",
                "estimated_jpeg_core_ms",
                "fps_budget_pass",
                "payload_crc32",
                "rotated_jwp_file",
                "rotated_jpeg_quality",
                "rotated_jwp_bytes",
                "rotated_avg_jpeg_kib",
                "rotated_p95_jpeg_kib",
                "rotated_fps_budget_pass",
                "rotated_payload_crc32",
            ]
        )
        writer.writerows(manifest_rows)
    firmware_playlist = sketch_dir / "wallpaper_playlist.h"
    write_firmware_playlist(firmware_playlist, sources, scenes, relative_jwp_paths)
    print(f"\nBuilt {len(sources)} adaptive-quality assets. Manifest: {manifest}")
    print(f"Firmware playlist metadata: {firmware_playlist}")


if __name__ == "__main__":
    main()
