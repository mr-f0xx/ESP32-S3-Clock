#!/usr/bin/env python3
"""Validate an MJP1 file without loading it into memory."""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path


HEADER = struct.Struct("<4sHHHHHHIIII")
FRAME_LENGTH = struct.Struct("<I")
INDEX_HEADER = struct.Struct("<4sI")
INDEX_MAGIC = b"IDX1"
MAX_JPEG_BYTES = 512 * 1024


def jpeg_size(jpeg: bytes) -> tuple[int, int]:
    offset = 2
    while offset + 4 <= len(jpeg):
        if jpeg[offset] != 0xFF:
            offset += 1
            continue
        marker = jpeg[offset + 1]
        if marker in (0xD8, 0xD9):
            offset += 2
            continue
        length = int.from_bytes(jpeg[offset + 2 : offset + 4], "big")
        if marker in range(0xC0, 0xC4):
            return (
                int.from_bytes(jpeg[offset + 7 : offset + 9], "big"),
                int.from_bytes(jpeg[offset + 5 : offset + 7], "big"),
            )
        offset += 2 + length
    raise ValueError("JPEG 内没有尺寸信息")


def diagnose_zero_header(source, file_size: int) -> str:
    frames = 0
    maximum = 0
    crc = 0
    dimensions: tuple[int, int] | None = None
    while source.tell() < file_size:
        raw_length = source.read(FRAME_LENGTH.size)
        if len(raw_length) != FRAME_LENGTH.size:
            raise ValueError(f"零文件头之后第 {frames + 1} 帧长度不完整")
        (length,) = FRAME_LENGTH.unpack(raw_length)
        if not 0 < length <= MAX_JPEG_BYTES:
            raise ValueError(
                f"零文件头之后第 {frames + 1} 帧长度 {length} 无效"
            )
        jpeg = source.read(length)
        if len(jpeg) != length:
            raise ValueError(
                f"零文件头之后第 {frames + 1} 帧数据不完整："
                f"{len(jpeg)}/{length}"
            )
        if not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
            raise ValueError(f"零文件头之后第 {frames + 1} 帧不是完整 JPEG")
        if dimensions is None:
            dimensions = jpeg_size(jpeg)
        frames += 1
        maximum = max(maximum, length)
        crc = zlib.crc32(jpeg, crc)
    return (
        f"32 字节文件头全为 00；后续数据可读取为 {frames} 帧 "
        f"{dimensions[0]}x{dimensions[1]} JPEG，最大 {maximum} 字节，"
        f"payload CRC32={crc & 0xFFFFFFFF:08X}。帧率已丢失，文件不符合 MJP1。"
    )


def validate(path: Path) -> dict[str, int | float | str]:
    file_size = path.stat().st_size
    with path.open("rb") as source:
        raw_header = source.read(HEADER.size)
        if len(raw_header) != HEADER.size:
            raise ValueError(f"文件不足 {HEADER.size} 字节")
        (
            magic,
            version,
            header_size,
            width,
            height,
            fps_times_100,
            pixel_format,
            frame_count,
            declared_max,
            declared_crc,
            reserved,
        ) = HEADER.unpack(raw_header)
        if raw_header == b"\0" * HEADER.size:
            raise ValueError(diagnose_zero_header(source, file_size))
        if magic != b"MJP1":
            raise ValueError(f"magic={magic!r}，应为 b'MJP1'")
        if version != 1 or header_size != HEADER.size:
            raise ValueError(f"version={version}, header_size={header_size}")
        if (width, height) not in ((480, 360), (360, 480)):
            raise ValueError(f"尺寸 {width}x{height} 不受固件支持")
        if not fps_times_100 or not frame_count:
            raise ValueError(
                f"帧率或帧数无效：fps={fps_times_100 / 100:.2f}, frames={frame_count}"
            )
        if pixel_format != 1:
            raise ValueError(f"pixel_format={pixel_format}，应为 1")
        if not 0 < declared_max <= MAX_JPEG_BYTES:
            raise ValueError(f"max JPEG={declared_max}，上限为 {MAX_JPEG_BYTES}")
        actual_max = 0
        crc = 0
        frame_offsets: list[int] = []
        for index in range(frame_count):
            frame_offsets.append(source.tell())
            raw_length = source.read(FRAME_LENGTH.size)
            if len(raw_length) != FRAME_LENGTH.size:
                raise ValueError(f"第 {index + 1}/{frame_count} 帧长度缺失")
            (length,) = FRAME_LENGTH.unpack(raw_length)
            if not 0 < length <= declared_max:
                raise ValueError(
                    f"第 {index + 1}/{frame_count} 帧长度 {length} "
                    f"超出 1..{declared_max}"
                )
            jpeg = source.read(length)
            if len(jpeg) != length:
                raise ValueError(
                    f"第 {index + 1}/{frame_count} 帧数据不完整："
                    f"{len(jpeg)}/{length}"
                )
            if not jpeg.startswith(b"\xff\xd8") or not jpeg.endswith(b"\xff\xd9"):
                raise ValueError(f"第 {index + 1}/{frame_count} 帧不是完整 JPEG")
            actual_max = max(actual_max, length)
            crc = zlib.crc32(jpeg, crc)

        payload_end = source.tell()
        if reserved:
            if reserved != payload_end:
                raise ValueError(
                    f"帧数据结束于 {payload_end}，索引却声明从 {reserved} 开始"
                )
            raw_index_header = source.read(INDEX_HEADER.size)
            if len(raw_index_header) != INDEX_HEADER.size:
                raise ValueError("IDX1 索引头不完整")
            index_magic, index_count = INDEX_HEADER.unpack(raw_index_header)
            if index_magic != INDEX_MAGIC or index_count != frame_count:
                raise ValueError(
                    f"索引头无效：magic={index_magic!r}, frames={index_count}"
                )
            for index, expected_offset in enumerate(frame_offsets):
                raw_offset = source.read(FRAME_LENGTH.size)
                if len(raw_offset) != FRAME_LENGTH.size:
                    raise ValueError(f"IDX1 第 {index + 1} 项缺失")
                (stored_offset,) = FRAME_LENGTH.unpack(raw_offset)
                if stored_offset != expected_offset:
                    raise ValueError(
                        f"IDX1 第 {index + 1} 项为 {stored_offset}，"
                        f"实际应为 {expected_offset}"
                    )
        trailing = source.read(1)
        if trailing:
            raise ValueError(f"文件末尾仍有未知数据，位置 {source.tell() - 1}")
        if actual_max != declared_max:
            raise ValueError(
                f"头部 max JPEG={declared_max}，实际最大帧={actual_max}"
            )
        crc &= 0xFFFFFFFF
        if crc != declared_crc:
            raise ValueError(
                f"CRC32 不匹配：头部 {declared_crc:08X}，实际 {crc:08X}"
            )
        return {
            "path": str(path),
            "bytes": file_size,
            "width": width,
            "height": height,
            "fps": fps_times_100 / 100,
            "frames": frame_count,
            "seconds": frame_count / (fps_times_100 / 100),
            "max_jpeg": actual_max,
            "crc32": f"{crc:08X}",
            "index": "IDX1" if reserved else "none",
        }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="+", type=Path)
    arguments = parser.parse_args()
    failed = False
    for path in arguments.files:
        try:
            result = validate(path)
            print(
                "OK: {path}\n"
                "  {width}x{height}, {fps:.2f} fps, {frames} 帧, "
                "{seconds:.1f} 秒\n"
                "  {bytes} 字节, 最大 JPEG {max_jpeg} 字节, "
                "CRC32 {crc32}, 索引 {index}".format(**result)
            )
        except (OSError, ValueError) as error:
            failed = True
            print(f"ERROR: {path}\n  {error}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
