#!/usr/bin/env python3
"""Extract original JPEG frames from a JWP1 file without re-encoding them."""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


HEADER = struct.Struct("<4sHHHHHHIIII")
FRAME_LENGTH = struct.Struct("<I")


def parse_frames(value: str) -> list[int]:
    try:
        frames = [int(part.strip()) for part in value.split(",") if part.strip()]
    except ValueError as error:
        raise argparse.ArgumentTypeError("frames must be comma-separated integers") from error
    if not frames or any(frame < 0 for frame in frames):
        raise argparse.ArgumentTypeError("provide one or more non-negative frame indexes")
    return sorted(set(frames))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Input .jwp file")
    parser.add_argument("--frames", required=True, type=parse_frames)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    with args.input.open("rb") as stream:
        raw_header = stream.read(HEADER.size)
        if len(raw_header) != HEADER.size:
            raise SystemExit("JWP header is too short")
        magic, version, header_size, width, height, fps100, pixel_format, count, _, _, _ = (
            HEADER.unpack(raw_header)
        )
        if magic != b"JWP1" or version != 1 or header_size != HEADER.size:
            raise SystemExit("Unsupported JWP file")
        if args.frames[-1] >= count:
            raise SystemExit(f"highest requested frame exceeds frame count {count}")

        requested = set(args.frames)
        for index in range(count):
            raw_length = stream.read(FRAME_LENGTH.size)
            if len(raw_length) != FRAME_LENGTH.size:
                raise SystemExit(f"short frame length at {index}")
            frame_bytes = FRAME_LENGTH.unpack(raw_length)[0]
            if index in requested:
                jpeg = stream.read(frame_bytes)
                if len(jpeg) != frame_bytes:
                    raise SystemExit(f"short JPEG payload at {index}")
                output = args.output_dir / f"{args.input.stem}_frame{index:04d}.jpg"
                output.write_bytes(jpeg)
                print(f"{output}: {frame_bytes} bytes")
            else:
                stream.seek(frame_bytes, 1)

    print(f"JWP: {width}x{height}, {fps100 / 100:.2f} fps, {count} frames")


if __name__ == "__main__":
    main()
