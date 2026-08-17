#!/usr/bin/env python3
"""Convert a video to the length-prefixed baseline-JPEG JWP1 container."""

from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path


MAGIC = b"JWP1"
VERSION = 1
HEADER_SIZE = 32
PIXEL_FORMAT_BASELINE_JPEG = 1
HEADER = struct.Struct("<4sHHHHHHIIII")
FRAME_LENGTH = struct.Struct("<I")


def find_ffmpeg(explicit: str | None) -> str:
    if explicit:
        return explicit
    executable = shutil.which("ffmpeg")
    if executable:
        return executable
    try:
        import imageio_ffmpeg  # type: ignore

        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError as exc:
        raise SystemExit(
            "ffmpeg was not found. Install ffmpeg, or run: "
            f"{sys.executable} -m pip install imageio-ffmpeg"
        ) from exc


def jpeg_frames(stream):
    pending = bytearray()
    while True:
        chunk = stream.read(64 * 1024)
        if chunk:
            pending.extend(chunk)

        while True:
            start = pending.find(b"\xff\xd8")
            if start < 0:
                if len(pending) > 1:
                    del pending[:-1]
                break
            if start:
                del pending[:start]
            end = pending.find(b"\xff\xd9", 2)
            if end < 0:
                break
            end += 2
            yield bytes(pending[:end])
            del pending[:end]

        if not chunk:
            if pending:
                raise RuntimeError("ffmpeg returned a partial JPEG frame")
            return
        if len(pending) > 2 * 1024 * 1024:
            raise RuntimeError("JPEG frame exceeds the 2 MiB preprocessing limit")


def convert(args: argparse.Namespace) -> None:
    source = args.input.resolve()
    output = args.output.resolve()
    if not source.is_file():
        raise SystemExit(f"Input video does not exist: {source}")
    if args.width <= 0 or args.height <= 0 or args.fps <= 0:
        raise SystemExit("Width, height and fps must be positive")
    if not 2 <= args.quality <= 31:
        raise SystemExit("ffmpeg MJPEG quality must be between 2 and 31")
    if args.loop_crossfade_seconds < 0:
        raise SystemExit("Loop crossfade duration cannot be negative")
    if not 0.51 <= args.deband_strength <= 64:
        raise SystemExit("Deband strength must be between 0.51 and 64")
    if not 4 <= args.deband_radius <= 32:
        raise SystemExit("Deband radius must be between 4 and 32")
    if args.loop_crossfade_seconds > 0:
        if args.duration is None:
            raise SystemExit(
                "--loop-crossfade-seconds requires an explicit --duration"
            )
        if args.loop_crossfade_seconds * 2 >= args.duration:
            raise SystemExit(
                "Loop crossfade duration must be less than half the clip duration"
            )

    ffmpeg = find_ffmpeg(args.ffmpeg)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    fps_times_100 = round(args.fps * 100)
    output_width = args.height if args.rotate_ccw else args.width
    output_height = args.width if args.rotate_ccw else args.height
    filters = [
        f"fps={args.fps}",
        (
            f"scale={args.width}:{args.height}:"
            "force_original_aspect_ratio=increase:flags=lanczos"
        ),
        f"crop={args.width}:{args.height}",
    ]
    if args.sharpen > 0:
        # Recover fine texture lost while downscaling a 4K source to the
        # 480-pixel panel. Keep the chroma plane untouched to avoid halos.
        filters.append(f"unsharp=5:5:{args.sharpen}:5:5:0.0")
    if args.deband:
        # Offline debanding adds very fine spatial dither before RGB565
        # quantization on the ESP32, reducing visible gradient steps at no
        # runtime cost.
        filters.append(
            f"gradfun=strength={args.deband_strength}:"
            f"radius={args.deband_radius}"
        )
    if args.rotate_ccw:
        # Keep the same 480x360 crop as the landscape asset, then rotate that
        # exact composition.  The firmware can therefore swap between the two
        # files without doing any per-frame pixel rotation.
        filters.append("transpose=2")
    base_filter = ",".join(filters)
    command = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        os.fspath(source),
        "-an",
    ]
    if args.loop_crossfade_seconds > 0:
        output_frames = round(args.duration * args.fps)
        transition_frames = round(args.loop_crossfade_seconds * args.fps) + 1
        tail_start_frame = output_frames - transition_frames
        transition_duration = (transition_frames - 1) / args.fps
        filter_complex = (
            f"[0:v]{base_filter},trim=end_frame={output_frames},"
            f"setpts=N/({args.fps}*TB),split=3[middlesrc][tailsrc][headsrc];"
            f"[middlesrc]trim=start_frame={transition_frames}:"
            f"end_frame={tail_start_frame},"
            f"setpts=N/({args.fps}*TB)[middle];"
            f"[tailsrc]trim=start_frame={tail_start_frame}:"
            f"end_frame={output_frames},setpts=N/({args.fps}*TB)[tail];"
            f"[headsrc]trim=end_frame={transition_frames},"
            f"setpts=N/({args.fps}*TB)[head];"
            f"[tail][head]blend=all_expr="
            f"A*(1-T/{transition_duration})+B*(T/{transition_duration}):"
            "shortest=1[transition];"
            f"[middle][transition]concat=n=2:v=1:a=0,"
            f"tpad=stop_mode=clone:stop_duration={1.0 / args.fps},"
            f"fps={args.fps},"
            f"format=yuvj{args.chroma}p[outv]"
        )
        command.extend(["-filter_complex", filter_complex, "-map", "[outv]"])
    else:
        video_filter = f"{base_filter},format=yuvj{args.chroma}p"
        command.extend(["-vf", video_filter])
    if args.duration is not None:
        command.extend(["-t", str(args.duration)])
    command.extend(
        [
            "-c:v",
            "mjpeg",
            "-q:v",
            str(args.quality),
            "-f",
            "image2pipe",
            "pipe:1",
        ]
    )

    print("Running:", subprocess.list2cmdline(command))
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert process.stdout is not None
    assert process.stderr is not None

    frame_count = 0
    frame_offsets: list[int] = []
    max_frame_bytes = 0
    payload_crc32 = 0
    try:
        with temporary.open("wb+") as destination:
            destination.write(b"\0" * HEADER_SIZE)
            for frame in jpeg_frames(process.stdout):
                frame_offsets.append(destination.tell())
                length = FRAME_LENGTH.pack(len(frame))
                destination.write(length)
                destination.write(frame)
                payload_crc32 = zlib.crc32(frame, payload_crc32)
                max_frame_bytes = max(max_frame_bytes, len(frame))
                frame_count += 1
                if frame_count % max(1, round(args.fps)) == 0:
                    print(f"\rFrames: {frame_count}", end="", flush=True)

            stderr = process.stderr.read().decode("utf-8", errors="replace")
            return_code = process.wait()
            if return_code != 0:
                raise RuntimeError(stderr.strip() or f"ffmpeg exited with {return_code}")
            if frame_count == 0:
                raise RuntimeError("ffmpeg produced no frames")

            transition_offset = 0
            if args.loop_crossfade_seconds > 0:
                fade_frames = max(
                    1, round(args.loop_crossfade_seconds * args.fps) + 1
                )
                transition_frame = max(0, frame_count - fade_frames)
                transition_offset = frame_offsets[transition_frame]

            header = HEADER.pack(
                MAGIC,
                VERSION,
                HEADER_SIZE,
                output_width,
                output_height,
                fps_times_100,
                PIXEL_FORMAT_BASELINE_JPEG,
                frame_count,
                max_frame_bytes,
                payload_crc32 & 0xFFFFFFFF,
                transition_offset,
            )
            destination.seek(0)
            destination.write(header)
            destination.flush()
            os.fsync(destination.fileno())

        temporary.replace(output)
    except BaseException:
        process.kill()
        temporary.unlink(missing_ok=True)
        raise

    print(
        f"\nWrote {output}\n"
        f"  {output_width}x{output_height}, {frame_count} frames @ {args.fps:g} fps\n"
        f"  {output.stat().st_size / (1024 * 1024):.2f} MiB, "
        f"largest JPEG {max_frame_bytes / 1024:.1f} KiB, "
        f"CRC32 {payload_crc32 & 0xFFFFFFFF:08X}, "
        f"loop-transition offset {transition_offset}"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="Source video")
    parser.add_argument("--output", type=Path, required=True, help="Output .jwp file")
    parser.add_argument("--width", type=int, default=480)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument(
        "--rotate-ccw", action="store_true",
        help="Rotate the finished landscape composition 90 degrees counter-clockwise",
    )
    parser.add_argument("--fps", type=float, default=30.0)
    parser.add_argument(
        "--duration", type=float, default=4.0, help="Seconds to keep (default: 4)"
    )
    parser.add_argument(
        "--quality", type=int, default=7,
        help="ffmpeg MJPEG q:v value, 2=best and 31=smallest (default: 7)",
    )
    parser.add_argument(
        "--chroma", choices=("420", "422", "444"), default="420",
        help="JPEG chroma sampling. esp_new_jpeg supports 420/422 input; "
             "444 is preprocessing-only and is rejected by the current decoder",
    )
    parser.add_argument(
        "--sharpen", type=float, default=0.0,
        help="Post-scale luma sharpening amount, normally 0.4-0.8 (default: 0)",
    )
    parser.add_argument(
        "--loop-crossfade-seconds", type=float, default=0.0,
        help="Blend the clip tail into its head for a seamless loop (default: 0)",
    )
    parser.add_argument(
        "--deband", dest="deband", action="store_true",
        help="Enable the optional offline gradfun debanding/dither filter",
    )
    parser.add_argument(
        "--no-deband", dest="deband", action="store_false",
        help="Disable the offline gradfun debanding/dither filter (default)",
    )
    parser.add_argument(
        "--deband-strength", type=float, default=1.2,
        help="gradfun strength when debanding is enabled (default: 1.2)",
    )
    parser.add_argument(
        "--deband-radius", type=int, default=16,
        help="gradfun neighborhood radius (default: 16)",
    )
    parser.set_defaults(deband=False)
    parser.add_argument("--ffmpeg", help="Explicit ffmpeg executable path")
    return parser.parse_args()


if __name__ == "__main__":
    convert(parse_args())
