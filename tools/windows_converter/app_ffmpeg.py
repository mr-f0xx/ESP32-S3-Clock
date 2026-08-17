#!/usr/bin/env python3
"""High-speed FFmpeg backend for the AMOLED MJP1 converter UI."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import threading
import time
import zlib
import ctypes
from dataclasses import dataclass
from pathlib import Path

os.environ["AMOLED_CONVERTER_BACKEND_NAME"] = "内置 FFmpeg（高速版）"
os.environ["AMOLED_CONVERTER_BACKEND_TOOLTIP"] = (
    "FFmpeg 已随程序打包，最终用户无需另行安装"
)
os.environ["AMOLED_CONVERTER_TITLE_SUFFIX"] = " — FFmpeg 高速版"

import app as native_app  # noqa: E402
from PyQt6.QtCore import QThread, QTimer, pyqtSignal, pyqtSlot  # noqa: E402
from PyQt6.QtWidgets import (  # noqa: E402
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFileDialog,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QSpinBox,
    QWidget,
)


VIDEO_EXTENSIONS = {
    ".mp4", ".mov", ".m4v", ".mkv", ".avi", ".wmv", ".webm", ".ts"
}


@dataclass(frozen=True)
class TfDrive:
    root: Path
    volume_name: str
    free_bytes: int
    removable: bool

    @property
    def display_name(self) -> str:
        kind = "可移动磁盘" if self.removable else "本地磁盘"
        volume = f" {self.volume_name}" if self.volume_name else ""
        return f"{self.root}{volume} · {kind} · 剩余 {format_size(self.free_bytes)}"


def available_tf_drives() -> list[TfDrive]:
    """Return writable Windows drive letters suitable for explicit TF output."""
    if os.name != "nt":
        return []
    kernel32 = ctypes.windll.kernel32
    drive_mask = kernel32.GetLogicalDrives()
    system_drive = os.environ.get("SystemDrive", "C:").upper()
    drives: list[TfDrive] = []
    for index in range(26):
        if not drive_mask & (1 << index):
            continue
        root_text = f"{chr(ord('A') + index)}:\\"
        if root_text[:2].upper() == system_drive:
            continue
        drive_type = kernel32.GetDriveTypeW(root_text)
        if drive_type not in (2, 3):  # DRIVE_REMOVABLE or DRIVE_FIXED
            continue
        try:
            usage = shutil.disk_usage(root_text)
        except OSError:
            continue
        volume_buffer = ctypes.create_unicode_buffer(261)
        file_system_buffer = ctypes.create_unicode_buffer(261)
        ok = kernel32.GetVolumeInformationW(
            root_text,
            volume_buffer,
            len(volume_buffer),
            None,
            None,
            None,
            file_system_buffer,
            len(file_system_buffer),
        )
        drives.append(
            TfDrive(
                Path(root_text),
                volume_buffer.value if ok else "",
                usage.free,
                drive_type == 2,
            )
        )
    return sorted(drives, key=lambda item: (not item.removable, str(item.root)))


class ConversionCancelled(RuntimeError):
    pass


def find_ffmpeg() -> Path:
    candidates: list[Path] = []
    bundle_root = getattr(sys, "_MEIPASS", None)
    if bundle_root:
        candidates.extend((Path(bundle_root) / "bin").glob("ffmpeg*.exe"))
    executable_dir = Path(sys.executable).resolve().parent
    candidates.extend(
        (executable_dir / "_internal" / "bin").glob("ffmpeg*.exe")
    )
    candidates.extend((Path(__file__).resolve().parent / "bin").glob("ffmpeg*.exe"))
    candidates.extend(Path(__file__).resolve().parent.glob("ffmpeg*.exe"))
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    try:
        import imageio_ffmpeg  # type: ignore

        candidate = Path(imageio_ffmpeg.get_ffmpeg_exe())
        if candidate.is_file():
            return candidate
    except ImportError:
        pass
    system_ffmpeg = shutil.which("ffmpeg")
    if system_ffmpeg:
        return Path(system_ffmpeg)
    raise RuntimeError("找不到内置 FFmpeg；请重新解压完整的高速版程序包。")


def probe_media(ffmpeg: Path, source: Path) -> tuple[float, bool]:
    result = subprocess.run(
        [os.fspath(ffmpeg), "-hide_banner", "-nostdin", "-i", os.fspath(source)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        timeout=30,
        check=False,
    )
    diagnostic = result.stderr.decode("utf-8", errors="replace")
    match = re.search(r"Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?)", diagnostic)
    if not match:
        tail = "\n".join(diagnostic.strip().splitlines()[-6:])
        raise RuntimeError(f"FFmpeg 无法读取视频时长。\n{tail}")
    hours, minutes, seconds = match.groups()
    duration = int(hours) * 3600 + int(minutes) * 60 + float(seconds)
    if duration <= 0:
        raise RuntimeError("FFmpeg 报告的视频时长无效。")
    return duration, bool(re.search(r"Stream #.*Audio:", diagnostic))


def jpeg_qscale(quality: int) -> int:
    return max(2, min(31, round(31 - (quality - 40) * 29 / 50)))


def sample_jpeg_size(
    ffmpeg: Path, source: Path, duration: float, quality: int
) -> int:
    """Encode a short middle sample using the real output settings."""
    seek = max(0.0, duration * 0.5 - 0.25)
    command = [
        os.fspath(ffmpeg),
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostdin",
        "-ss",
        f"{seek:.3f}",
        "-i",
        os.fspath(source),
        "-map",
        "0:v:0",
        "-an",
        "-frames:v",
        "8",
        "-vf",
        (
            "scale=480:360:force_original_aspect_ratio=increase,"
            "crop=480:360,format=yuvj420p"
        ),
        "-c:v",
        "mjpeg",
        "-q:v",
        str(jpeg_qscale(quality)),
        "-f",
        "image2pipe",
        "pipe:1",
    ]
    result = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        timeout=45,
        check=False,
    )
    if result.returncode:
        return 0
    sizes: list[int] = []
    cursor = 0
    payload = result.stdout
    while True:
        start = payload.find(b"\xff\xd8", cursor)
        if start < 0:
            break
        end = payload.find(b"\xff\xd9", start + 2)
        if end < 0:
            break
        sizes.append(end + 2 - start)
        cursor = end + 2
    return round(sum(sizes) / len(sizes)) if sizes else 0


def format_size(byte_count: float) -> str:
    units = ("B", "KiB", "MiB", "GiB", "TiB")
    value = float(max(0, byte_count))
    for unit in units:
        if value < 1024 or unit == units[-1]:
            return f"{value:.1f} {unit}" if unit != "B" else f"{value:.0f} B"
        value /= 1024
    return f"{value:.1f} TiB"


@dataclass(frozen=True)
class MediaEstimate:
    source: Path
    duration: float
    has_audio: bool
    average_jpeg_bytes: int
    main_bytes: int
    rotated_bytes: int
    audio_bytes: int

    @property
    def total_bytes(self) -> int:
        return self.main_bytes + self.rotated_bytes + self.audio_bytes


class FfmpegConversionWorker(native_app.QObject):
    log = pyqtSignal(str)
    status = pyqtSignal(str)
    progress = pyqtSignal(int, int)
    finished = pyqtSignal(bool, str)

    def __init__(self, options: native_app.ConversionOptions) -> None:
        super().__init__()
        self.options = options
        self._cancelled = threading.Event()
        self._process_lock = threading.Lock()
        self._process: subprocess.Popen[bytes] | None = None
        self._outputs: list[native_app.OutputPass] = []
        self._audio_path: Path | None = None
        self._audio_temporary: Path | None = None

    @pyqtSlot()
    def start(self) -> None:
        try:
            ffmpeg = find_ffmpeg()
            duration, has_audio = probe_media(ffmpeg, self.options.source)
            passes = [(self.options.output, 480, 360, False)]
            if self.options.rotated:
                passes.append(
                    (
                        self.options.output.parent
                        / "rotated"
                        / self.options.output.name,
                        360,
                        480,
                        True,
                    )
                )
            self.log.emit(f"FFmpeg：{ffmpeg}")
            self.log.emit(
                f"输入：{self.options.source}（{duration:.1f} 秒）"
            )
            total_passes = len(passes) + (1 if has_audio else 0)
            for pass_index, (path, width, height, rotated) in enumerate(passes):
                self._check_cancelled()
                output = self._open_output(path, width, height)
                self._outputs.append(output)
                self._run_pass(
                    ffmpeg,
                    output,
                    rotated,
                    duration,
                    pass_index,
                    total_passes,
                )
            video_duration = self._outputs[0].frame_count / self.options.fps
            if has_audio:
                self._extract_audio(
                    ffmpeg, video_duration, len(passes), total_passes
                )
            else:
                self.log.emit("输入文件没有音轨；仅生成静音视频。")
            self._finalize_outputs()
            self._finalize_audio()
            self.progress.emit(1000, 1000)
            self.finished.emit(True, "转换完成")
        except ConversionCancelled:
            self._discard_outputs()
            self.finished.emit(False, "转换已取消")
        except Exception as error:  # noqa: BLE001
            self._discard_outputs()
            self.finished.emit(False, str(error))
        finally:
            with self._process_lock:
                self._process = None

    @pyqtSlot()
    def cancel(self) -> None:
        self._cancelled.set()
        with self._process_lock:
            process = self._process
        if process and process.poll() is None:
            process.terminate()

    def _check_cancelled(self) -> None:
        if self._cancelled.is_set():
            raise ConversionCancelled()

    @staticmethod
    def _open_output(path: Path, width: int, height: int) -> native_app.OutputPass:
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.unlink(missing_ok=True)
        file = temporary.open("wb+")
        file.write(b"\0" * native_app.HEADER_SIZE)
        return native_app.OutputPass(path, temporary, file, width, height)

    def _run_pass(
        self,
        ffmpeg: Path,
        output: native_app.OutputPass,
        rotated: bool,
        duration: float,
        pass_index: int,
        pass_count: int,
    ) -> None:
        quality = self.options.jpeg_quality
        qscale = jpeg_qscale(quality)
        video_filter = (
            f"fps={self.options.fps:.6f},"
            "scale=480:360:force_original_aspect_ratio=increase,"
            "crop=480:360"
        )
        if rotated:
            video_filter += ",transpose=2"
        video_filter += ",format=yuvj420p"
        command = [
            os.fspath(ffmpeg),
            "-hide_banner",
            "-loglevel",
            "error",
            "-stats_period",
            "0.25",
            "-nostats",
            "-nostdin",
            "-i",
            os.fspath(self.options.source),
            "-map",
            "0:v:0",
            "-an",
            "-vf",
            video_filter,
            "-c:v",
            "mjpeg",
            "-q:v",
            str(qscale),
            "-progress",
            "pipe:2",
            "-f",
            "image2pipe",
            "pipe:1",
        ]
        label = "旋转版" if rotated else "横屏版"
        self.status.emit(f"FFmpeg 正在转换{label}…")
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            bufsize=0,
        )
        with self._process_lock:
            self._process = process
        errors: list[str] = []
        last_status_at = [0.0]

        def read_progress() -> None:
            assert process.stderr is not None
            for raw_line in iter(process.stderr.readline, b""):
                line = raw_line.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                if line.startswith("out_time_us="):
                    try:
                        seconds = int(line.partition("=")[2]) / 1_000_000
                    except ValueError:
                        continue
                    fraction = min(max(seconds / duration, 0.0), 1.0)
                    overall = round(
                        ((pass_index + fraction) / pass_count) * 1000
                    )
                    self.progress.emit(overall, 1000)
                    now = time.monotonic()
                    if now - last_status_at[0] >= 0.5:
                        last_status_at[0] = now
                        self.status.emit(
                            f"FFmpeg {label}：{seconds:.1f}/{duration:.1f} 秒，"
                            f"已写入 {output.frame_count} 帧"
                        )
                elif "=" not in line or line.startswith("error"):
                    errors.append(line)

        progress_thread = threading.Thread(target=read_progress, daemon=True)
        progress_thread.start()
        assert process.stdout is not None
        buffer = bytearray()
        try:
            while True:
                self._check_cancelled()
                chunk = process.stdout.read(64 * 1024)
                if not chunk:
                    break
                buffer.extend(chunk)
                while True:
                    start = buffer.find(b"\xff\xd8")
                    if start < 0:
                        if len(buffer) > 1:
                            del buffer[:-1]
                        break
                    if start:
                        del buffer[:start]
                    end = buffer.find(b"\xff\xd9", 2)
                    if end < 0:
                        if len(buffer) > native_app.MAX_JPEG_BYTES:
                            raise RuntimeError(
                                "FFmpeg 输出的单帧 JPEG 超过 512 KiB。"
                            )
                        break
                    jpeg = bytes(buffer[: end + 2])
                    del buffer[: end + 2]
                    self._write_frame(output, jpeg)
            return_code = process.wait()
            progress_thread.join(timeout=2)
            self._check_cancelled()
            if return_code != 0:
                detail = "\n".join(errors[-8:]) or f"退出码 {return_code}"
                raise RuntimeError(f"FFmpeg 转换{label}失败：\n{detail}")
            if buffer and any(buffer):
                raise RuntimeError(f"FFmpeg {label}输出末尾含有不完整 JPEG。")
            if not output.frame_count:
                raise RuntimeError(f"FFmpeg {label}没有输出视频帧。")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            with self._process_lock:
                if self._process is process:
                    self._process = None

    def _extract_audio(
        self,
        ffmpeg: Path,
        duration: float,
        pass_index: int,
        pass_count: int,
    ) -> None:
        self._check_cancelled()
        self._audio_path = self.options.output.with_suffix(".wav")
        self._audio_temporary = self._audio_path.with_suffix(".wav.tmp")
        self._audio_temporary.unlink(missing_ok=True)
        command = [
            os.fspath(ffmpeg),
            "-hide_banner",
            "-loglevel",
            "error",
            "-nostdin",
            "-i",
            os.fspath(self.options.source),
            "-map",
            "0:a:0",
            "-vn",
            "-af",
            "aresample=async=1:first_pts=0,apad",
            "-t",
            f"{duration:.6f}",
            "-ac",
            "1",
            "-ar",
            "24000",
            "-c:a",
            "pcm_s16le",
            "-progress",
            "pipe:1",
            "-nostats",
            "-f",
            "wav",
            "-y",
            os.fspath(self._audio_temporary),
        ]
        self.status.emit("FFmpeg 正在提取 24 kHz 单声道 WAV 音频…")
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            bufsize=0,
        )
        with self._process_lock:
            self._process = process
        try:
            assert process.stdout is not None
            for raw_line in iter(process.stdout.readline, b""):
                self._check_cancelled()
                line = raw_line.decode("utf-8", errors="replace").strip()
                if not line.startswith("out_time_us="):
                    continue
                try:
                    seconds = int(line.partition("=")[2]) / 1_000_000
                except ValueError:
                    continue
                fraction = min(max(seconds / duration, 0.0), 1.0)
                overall = round(((pass_index + fraction) / pass_count) * 1000)
                self.progress.emit(overall, 1000)
                self.status.emit(
                    f"FFmpeg 音频：{seconds:.1f}/{duration:.1f} 秒"
                )
            return_code = process.wait()
            self._check_cancelled()
            assert process.stderr is not None
            diagnostic = process.stderr.read().decode("utf-8", errors="replace")
            if return_code != 0:
                detail = "\n".join(diagnostic.strip().splitlines()[-8:])
                raise RuntimeError(f"FFmpeg 音频提取失败：\n{detail}")
            if (
                not self._audio_temporary.is_file()
                or self._audio_temporary.stat().st_size <= 44
            ):
                raise RuntimeError("FFmpeg 没有生成有效 WAV 音频。")
            with self._audio_temporary.open("rb") as audio:
                wav_header = audio.read(12)
                if wav_header[:4] != b"RIFF" or wav_header[8:12] != b"WAVE":
                    raise RuntimeError("FFmpeg 生成的 WAV 文件头无效。")
            self.log.emit(
                f"音频：{self._audio_path}（24 kHz / 16-bit / 单声道，"
                f"{self._audio_temporary.stat().st_size / 1024**2:.1f} MiB）"
            )
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            with self._process_lock:
                if self._process is process:
                    self._process = None

    def _finalize_audio(self) -> None:
        if not self._audio_path or not self._audio_temporary:
            return
        self._audio_temporary.replace(self._audio_path)
        self._audio_path = None
        self._audio_temporary = None

    @staticmethod
    def _write_frame(output: native_app.OutputPass, jpeg: bytes) -> None:
        if len(jpeg) > native_app.MAX_JPEG_BYTES:
            raise RuntimeError("单帧 JPEG 超过 512 KiB，请降低 JPEG 质量。")
        output.frame_offsets.append(output.file.tell())
        output.file.write(native_app.FRAME_LENGTH.pack(len(jpeg)))
        output.file.write(jpeg)
        output.payload_crc32 = zlib.crc32(jpeg, output.payload_crc32)
        output.max_frame_bytes = max(output.max_frame_bytes, len(jpeg))
        output.frame_count += 1

    def _finalize_outputs(self) -> None:
        for output in self._outputs:
            index_offset = output.file.tell()
            if index_offset > 0xFFFFFFFF:
                raise RuntimeError("输出文件超过 4 GiB，无法写入帧索引。")
            output.file.write(
                native_app.INDEX_HEADER.pack(
                    native_app.INDEX_MAGIC, output.frame_count
                )
            )
            for frame_offset in output.frame_offsets:
                output.file.write(native_app.FRAME_LENGTH.pack(frame_offset))
            header = native_app.HEADER.pack(
                native_app.MAGIC,
                1,
                native_app.HEADER_SIZE,
                output.width,
                output.height,
                round(self.options.fps * 100),
                1,
                output.frame_count,
                output.max_frame_bytes,
                output.payload_crc32 & 0xFFFFFFFF,
                index_offset,
            )
            output.file.seek(0)
            output.file.write(header)
            output.file.flush()
            os.fsync(output.file.fileno())
            expected_size = output.temporary.stat().st_size
            output.file.close()
            output.temporary.replace(output.path)
            with output.path.open("rb") as verification:
                stored_header = verification.read(native_app.HEADER_SIZE)
            if stored_header != header or output.path.stat().st_size != expected_size:
                output.path.unlink(missing_ok=True)
                raise RuntimeError(f"输出文件回读验证失败：{output.path}")
            self.log.emit(
                f"完成：{output.path}（{output.frame_count} 帧，"
                f"最大 JPEG {output.max_frame_bytes / 1024:.1f} KiB）"
            )
        self._outputs.clear()

    def _discard_outputs(self) -> None:
        for output in self._outputs:
            try:
                if not output.file.closed:
                    output.file.close()
            finally:
                output.temporary.unlink(missing_ok=True)
        self._outputs.clear()
        if self._audio_temporary:
            self._audio_temporary.unlink(missing_ok=True)
        self._audio_path = None
        self._audio_temporary = None


class FfmpegConverterWindow(QMainWindow):
    estimate_ready = pyqtSignal(int, object, str)
    job_log_ready = pyqtSignal(object, str)
    job_status_ready = pyqtSignal(object, str)
    job_progress_ready = pyqtSignal(object, int, int)
    job_finished_ready = pyqtSignal(object, bool, str)

    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("AMOLED 时钟 MJPEG 转换器 — FFmpeg 批量版")
        self.resize(980, 660)
        self.setAcceptDrops(True)
        self._sources: list[Path] = []
        self._pending: list[native_app.ConversionOptions] = []
        self._active: dict[FfmpegConversionWorker, tuple[QThread, int]] = {}
        self._job_progress: dict[int, int] = {}
        self._job_names: dict[int, str] = {}
        self._completed_jobs = 0
        self._successful_jobs = 0
        self._failures: list[str] = []
        self._cancelling = False
        self._close_when_done = False
        self._estimate_generation = 0
        self._estimates: list[MediaEstimate] = []
        self._batch_tf_target: Path | None = None
        self._parallel_before_tf = 2

        central = QWidget()
        layout = QGridLayout(central)
        row = 0

        self.input_edit = QLineEdit()
        self.input_edit.setReadOnly(True)
        self.input_button = QPushButton("添加文件…")
        self.input_folder_button = QPushButton("导入文件夹…")
        self.clear_input_button = QPushButton("清空")
        input_actions = QWidget()
        input_actions_layout = QHBoxLayout(input_actions)
        input_actions_layout.setContentsMargins(0, 0, 0, 0)
        input_actions_layout.addWidget(self.input_button)
        input_actions_layout.addWidget(self.input_folder_button)
        input_actions_layout.addWidget(self.clear_input_button)
        layout.addWidget(QLabel("输入视频"), row, 0)
        layout.addWidget(self.input_edit, row, 1)
        layout.addWidget(input_actions, row, 2)
        row += 1

        self.output_edit = QLineEdit()
        self.output_button = QPushButton("选择目录…")
        layout.addWidget(QLabel("输出目录"), row, 0)
        layout.addWidget(self.output_edit, row, 1)
        layout.addWidget(self.output_button, row, 2)
        row += 1

        self.tf_card_check = QCheckBox("直接生成到所选盘符的 /video")
        self.tf_drive_combo = QComboBox()
        self.tf_refresh_button = QPushButton("刷新盘符")
        tf_controls = QWidget()
        tf_controls_layout = QHBoxLayout(tf_controls)
        tf_controls_layout.setContentsMargins(0, 0, 0, 0)
        tf_controls_layout.addWidget(self.tf_card_check)
        tf_controls_layout.addWidget(self.tf_drive_combo, 1)
        tf_controls_layout.addWidget(self.tf_refresh_button)
        layout.addWidget(QLabel("TF 卡导入"), row, 0)
        layout.addWidget(tf_controls, row, 1, 1, 2)
        row += 1

        backend = QLineEdit(native_app.BACKEND_NAME)
        backend.setReadOnly(True)
        backend.setToolTip(native_app.BACKEND_TOOLTIP)
        layout.addWidget(QLabel("转换引擎"), row, 0)
        layout.addWidget(backend, row, 1, 1, 2)
        row += 1

        video_group = QGroupBox("批量转换")
        video_layout = QFormLayout(video_group)
        self.fps_spin = QDoubleSpinBox()
        self.fps_spin.setRange(1.0, 40.0)
        self.fps_spin.setDecimals(2)
        self.fps_spin.setValue(12.0)
        video_layout.addRow("帧率", self.fps_spin)
        self.quality_spin = QSpinBox()
        self.quality_spin.setRange(40, 90)
        self.quality_spin.setValue(85)
        self.quality_spin.setSuffix("%")
        video_layout.addRow("JPEG 质量", self.quality_spin)
        self.parallel_spin = QSpinBox()
        self.parallel_spin.setRange(1, min(8, max(1, os.cpu_count() or 1)))
        self.parallel_spin.setValue(min(2, self.parallel_spin.maximum()))
        self.parallel_spin.setToolTip(
            "建议 2；更高会增加 CPU、磁盘占用，机械硬盘或 TF 卡输出时建议 1"
        )
        video_layout.addRow("并行任务", self.parallel_spin)
        self.rotated_check = QCheckBox(
            "同时生成 rotated/ 目录中的 360×480 版本"
        )
        self.rotated_check.setChecked(True)
        video_layout.addRow("", self.rotated_check)
        layout.addWidget(video_group, row, 0, 1, 3)
        row += 1

        self.estimate_label = QLabel("选择视频后将自动抽样估算输出大小")
        self.estimate_label.setWordWrap(True)
        layout.addWidget(self.estimate_label, row, 0, 1, 3)
        row += 1

        self.progress = QProgressBar()
        self.progress.setRange(0, 1000)
        self.progress.setValue(0)
        self.progress.setFormat("总进度 %p%")
        layout.addWidget(self.progress, row, 0, 1, 3)
        row += 1

        self.status_label = QLabel("就绪")
        self.convert_button = QPushButton("开始批量转换")
        layout.addWidget(self.status_label, row, 0, 1, 2)
        layout.addWidget(self.convert_button, row, 2)
        row += 1

        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        layout.addWidget(self.log, row, 0, 1, 3)
        layout.setRowStretch(row, 1)
        layout.setColumnStretch(1, 1)
        self.setCentralWidget(central)

        self._estimate_timer = QTimer(self)
        self._estimate_timer.setSingleShot(True)
        self._estimate_timer.timeout.connect(self._start_estimate)
        self.estimate_ready.connect(self._show_estimate)
        self.job_log_ready.connect(self._job_log)
        self.job_status_ready.connect(self._job_status)
        self.job_progress_ready.connect(self._job_progress_changed)
        self.job_finished_ready.connect(self._job_finished_signal)
        self.input_button.clicked.connect(self._choose_inputs)
        self.input_folder_button.clicked.connect(self._choose_input_directory)
        self.clear_input_button.clicked.connect(self._clear_inputs)
        self.output_button.clicked.connect(self._choose_output_directory)
        self.tf_refresh_button.clicked.connect(self._refresh_tf_drives)
        self.tf_card_check.toggled.connect(self._tf_mode_changed)
        self.convert_button.clicked.connect(self._convert_or_cancel)
        self.fps_spin.valueChanged.connect(self._schedule_estimate)
        self.quality_spin.valueChanged.connect(self._schedule_estimate)
        self.rotated_check.toggled.connect(self._schedule_estimate)
        self._refresh_tf_drives()
        self._tf_mode_changed(False)

    def _choose_inputs(self) -> None:
        paths, _ = QFileDialog.getOpenFileNames(
            self,
            "选择一个或多个视频",
            "",
            (
                "视频文件 (*.mp4 *.mov *.m4v *.mkv *.avi *.wmv *.webm *.ts);;"
                "所有文件 (*)"
            ),
        )
        self._add_sources(Path(path) for path in paths)

    def _choose_input_directory(self) -> None:
        directory = QFileDialog.getExistingDirectory(self, "导入视频文件夹")
        if not directory:
            return
        root = Path(directory)
        videos = sorted(
            (
                path for path in root.rglob("*")
                if path.is_file() and path.suffix.casefold() in VIDEO_EXTENSIONS
            ),
            key=lambda path: os.fspath(path).casefold(),
        )
        if not videos:
            QMessageBox.information(self, "没有视频", "所选文件夹中没有支持的视频文件。")
            return
        self._add_sources(videos)

    def _add_sources(self, paths) -> None:  # noqa: ANN001
        known = {
            os.path.normcase(os.path.abspath(os.fspath(path)))
            for path in self._sources
        }
        added = 0
        for path in paths:
            candidate = Path(path)
            if not candidate.is_file() or candidate.suffix.casefold() not in VIDEO_EXTENSIONS:
                continue
            key = os.path.normcase(os.path.abspath(os.fspath(candidate)))
            if key in known:
                continue
            known.add(key)
            self._sources.append(candidate)
            added += 1
        if not added:
            return
        self._refresh_input_summary()
        if not self.output_edit.text().strip() and not self.tf_card_check.isChecked():
            self.output_edit.setText(os.fspath(self._sources[0].parent))
        self.status_label.setText(f"已添加 {added} 个视频，共 {len(self._sources)} 个")
        self._schedule_estimate()

    def _refresh_input_summary(self) -> None:
        if not self._sources:
            self.input_edit.clear()
            self.input_edit.setToolTip("")
            return
        source_bytes = sum(
            path.stat().st_size for path in self._sources if path.is_file()
        )
        if len(self._sources) == 1:
            summary = os.fspath(self._sources[0])
        else:
            summary = f"已选择 {len(self._sources)} 个视频 · 源文件 {format_size(source_bytes)}"
        self.input_edit.setText(summary)
        preview = "\n".join(os.fspath(path) for path in self._sources[:20])
        if len(self._sources) > 20:
            preview += f"\n…以及另外 {len(self._sources) - 20} 个"
        self.input_edit.setToolTip(preview)

    def _clear_inputs(self) -> None:
        if self._active or self._pending:
            return
        self._sources.clear()
        self._estimates.clear()
        self._estimate_generation += 1
        self._estimate_timer.stop()
        self._refresh_input_summary()
        self.estimate_label.setText("选择视频后将自动抽样估算输出大小")
        self.progress.setValue(0)
        self.status_label.setText("就绪")

    def dragEnterEvent(self, event) -> None:  # noqa: ANN001, N802
        if not self._active and not self._pending and event.mimeData().hasUrls():
            event.acceptProposedAction()
        else:
            event.ignore()

    def dropEvent(self, event) -> None:  # noqa: ANN001, N802
        candidates: list[Path] = []
        for url in event.mimeData().urls():
            path = Path(url.toLocalFile())
            if path.is_dir():
                candidates.extend(
                    child for child in path.rglob("*")
                    if child.is_file() and child.suffix.casefold() in VIDEO_EXTENSIONS
                )
            else:
                candidates.append(path)
        self._add_sources(candidates)
        event.acceptProposedAction()

    def _choose_output_directory(self) -> None:
        path = QFileDialog.getExistingDirectory(
            self, "选择输出目录", self.output_edit.text().strip()
        )
        if path:
            self.output_edit.setText(path)

    def _refresh_tf_drives(self) -> None:
        selected = self.tf_drive_combo.currentData()
        self.tf_drive_combo.clear()
        for drive in available_tf_drives():
            self.tf_drive_combo.addItem(drive.display_name, os.fspath(drive.root))
        if selected:
            index = self.tf_drive_combo.findData(selected)
            if index >= 0:
                self.tf_drive_combo.setCurrentIndex(index)
        if not self.tf_drive_combo.count():
            self.tf_drive_combo.addItem("未发现可用的 TF 卡盘符", None)

    def _tf_mode_changed(self, enabled: bool) -> None:
        if enabled:
            self._parallel_before_tf = self.parallel_spin.value()
            self.parallel_spin.setValue(1)
        elif not self._active and not self._pending:
            self.parallel_spin.setValue(
                min(self._parallel_before_tf, self.parallel_spin.maximum())
            )
        self._set_configuration_enabled(not self._active and not self._pending)

    def _set_configuration_enabled(self, enabled: bool) -> None:
        tf_mode = self.tf_card_check.isChecked()
        self.input_button.setEnabled(enabled)
        self.input_folder_button.setEnabled(enabled)
        self.clear_input_button.setEnabled(enabled)
        self.output_edit.setEnabled(enabled and not tf_mode)
        self.output_button.setEnabled(enabled and not tf_mode)
        self.tf_card_check.setEnabled(enabled)
        self.tf_drive_combo.setEnabled(enabled and tf_mode)
        self.tf_refresh_button.setEnabled(enabled and tf_mode)
        self.fps_spin.setEnabled(enabled)
        self.quality_spin.setEnabled(enabled)
        self.parallel_spin.setEnabled(enabled and not tf_mode)
        self.rotated_check.setEnabled(enabled)

    def _schedule_estimate(self) -> None:
        if not self._sources or self._active:
            return
        self._estimate_generation += 1
        self._estimate_timer.start(350)

    def _start_estimate(self) -> None:
        if not self._sources:
            return
        generation = self._estimate_generation
        sources = list(self._sources)
        fps = self.fps_spin.value()
        quality = self.quality_spin.value()
        rotated = self.rotated_check.isChecked()
        self.estimate_label.setText(
            f"正在抽样估算 {len(sources)} 个文件，请稍候…"
        )

        def estimate() -> None:
            estimates: list[MediaEstimate] = []
            error = ""
            try:
                ffmpeg = find_ffmpeg()
                for source in sources:
                    duration, has_audio = probe_media(ffmpeg, source)
                    average = sample_jpeg_size(
                        ffmpeg, source, duration, quality
                    )
                    if not average:
                        # Conservative fallback if a codec cannot seek for the
                        # preview sample.
                        average = round(
                            480 * 360 * (0.035 + quality * 0.00075)
                        )
                    frames = max(1, round(duration * fps))
                    main_bytes = (
                        native_app.HEADER_SIZE
                        + frames * (native_app.FRAME_LENGTH.size + average)
                        + native_app.INDEX_HEADER.size
                        + frames * native_app.FRAME_LENGTH.size
                    )
                    rotated_bytes = (
                        round(main_bytes * 1.05) if rotated else 0
                    )
                    audio_bytes = (
                        round(duration * 24000 * 2) + 78 if has_audio else 0
                    )
                    estimates.append(
                        MediaEstimate(
                            source,
                            duration,
                            has_audio,
                            average,
                            main_bytes,
                            rotated_bytes,
                            audio_bytes,
                        )
                    )
            except Exception as exc:  # noqa: BLE001
                error = str(exc)
            self.estimate_ready.emit(generation, estimates, error)

        threading.Thread(target=estimate, daemon=True).start()

    @pyqtSlot(int, object, str)
    def _show_estimate(
        self, generation: int, estimates: object, error: str
    ) -> None:
        if generation != self._estimate_generation:
            return
        if error:
            self._estimates = []
            self.estimate_label.setText(f"大小估算失败：{error}")
            return
        self._estimates = list(estimates)
        main_bytes = sum(item.main_bytes for item in self._estimates)
        rotated_bytes = sum(item.rotated_bytes for item in self._estimates)
        audio_bytes = sum(item.audio_bytes for item in self._estimates)
        total = main_bytes + rotated_bytes + audio_bytes
        self.estimate_label.setText(
            f"预计主视频 {format_size(main_bytes)}"
            + (
                f" + 旋转版 {format_size(rotated_bytes)}"
                if rotated_bytes
                else ""
            )
            + (
                f" + WAV {format_size(audio_bytes)}"
                if audio_bytes
                else ""
            )
            + f" = {format_size(total)}；抽样估算范围约 "
            f"{format_size(total * 0.8)}–{format_size(total * 1.25)}"
        )

    def _build_jobs(self, output_directory: Path) -> list[native_app.ConversionOptions]:
        jobs: list[native_app.ConversionOptions] = []
        used_names: set[str] = set()
        for source in self._sources:
            stem = source.stem
            output = output_directory / f"{stem}.mjpeg"
            suffix = 2
            while output.name.casefold() in used_names:
                output = output_directory / f"{stem}_{suffix}.mjpeg"
                suffix += 1
            used_names.add(output.name.casefold())
            jobs.append(
                native_app.ConversionOptions(
                    source=source,
                    output=output,
                    fps=self.fps_spin.value(),
                    jpeg_quality=self.quality_spin.value(),
                    rotated=self.rotated_check.isChecked(),
                )
            )
        return jobs

    def _convert_or_cancel(self) -> None:
        if self._active or self._pending:
            self._cancel_all()
            return
        if not self._sources or any(not source.is_file() for source in self._sources):
            QMessageBox.warning(self, "缺少输入", "请选择有效的视频文件。")
            return
        tf_mode = self.tf_card_check.isChecked()
        tf_root: Path | None = None
        if tf_mode:
            selected_drive = self.tf_drive_combo.currentData()
            if not selected_drive:
                QMessageBox.warning(self, "未选择 TF 卡", "请插入 TF 卡、刷新盘符并选择目标盘符。")
                return
            tf_root = Path(selected_drive)
            if not tf_root.exists():
                QMessageBox.warning(self, "TF 卡不可用", "所选盘符已断开，请刷新后重新选择。")
                return
            output_directory = tf_root / "video"
        else:
            output_text = self.output_edit.text().strip()
            if not output_text:
                QMessageBox.warning(self, "缺少输出", "请选择输出目录。")
                return
            output_directory = Path(output_text)
        try:
            output_directory.mkdir(parents=True, exist_ok=True)
            if tf_mode:
                (output_directory / "rotated").mkdir(parents=True, exist_ok=True)
        except OSError as exc:
            QMessageBox.critical(self, "无法创建输出目录", str(exc))
            return
        if tf_root is not None and self._estimates:
            required = round(sum(item.total_bytes for item in self._estimates) * 1.25)
            try:
                free_bytes = shutil.disk_usage(tf_root).free
            except OSError as exc:
                QMessageBox.critical(self, "无法读取 TF 卡容量", str(exc))
                return
            if required > free_bytes:
                QMessageBox.critical(
                    self,
                    "TF 卡空间不足",
                    f"按估算上限需要 {format_size(required)}，"
                    f"所选盘符仅剩 {format_size(free_bytes)}。",
                )
                return
        jobs = self._build_jobs(output_directory)
        existing: list[Path] = []
        for job in jobs:
            candidates = [job.output, job.output.with_suffix(".wav")]
            if job.rotated:
                candidates.append(
                    job.output.parent / "rotated" / job.output.name
                )
            existing.extend(path for path in candidates if path.exists())
        if existing and QMessageBox.question(
            self,
            "覆盖已有文件",
            f"检测到 {len(existing)} 个同名输出，是否覆盖并继续？",
        ) != QMessageBox.StandardButton.Yes:
            return

        self.log.clear()
        self.log.appendPlainText(f"[输出] {output_directory}")
        if tf_mode:
            self.log.appendPlainText("[TF 卡] 已确认 /video 与 /video/rotated 目录")
        if self._estimates:
            for item in self._estimates:
                self.log.appendPlainText(
                    f"[估算] {item.source.name}: {item.duration:.1f} 秒，"
                    f"平均 JPEG {format_size(item.average_jpeg_bytes)}，"
                    f"合计 {format_size(item.total_bytes)}"
                )
        self._pending = jobs
        self._job_progress = {}
        self._job_names = {
            index: job.source.name for index, job in enumerate(jobs)
        }
        self._completed_jobs = 0
        self._successful_jobs = 0
        self._failures = []
        self._cancelling = False
        self._batch_tf_target = output_directory if tf_mode else None
        self.progress.setValue(0)
        self._set_configuration_enabled(False)
        self.convert_button.setText("取消全部")
        self._start_more_jobs()

    def _start_more_jobs(self) -> None:
        limit = self.parallel_spin.value()
        total = len(self._job_names)
        while self._pending and len(self._active) < limit and not self._cancelling:
            options = self._pending.pop(0)
            index = next(
                i for i in range(total)
                if i not in self._job_progress
                and all(active_index != i for _, active_index in self._active.values())
            )
            self._job_progress[index] = 0
            worker = FfmpegConversionWorker(options)
            thread = QThread(self)
            worker.moveToThread(thread)
            self._active[worker] = (thread, index)
            thread.started.connect(worker.start)
            worker.log.connect(
                lambda text, current_worker=worker:
                    self.job_log_ready.emit(current_worker, text)
            )
            worker.status.connect(
                lambda text, current_worker=worker:
                    self.job_status_ready.emit(current_worker, text)
            )
            worker.progress.connect(
                lambda current, maximum, current_worker=worker:
                    self.job_progress_ready.emit(
                        current_worker, current, maximum
                    )
            )
            worker.finished.connect(
                lambda success, message, current_worker=worker:
                    self.job_finished_ready.emit(
                        current_worker, success, message
                    )
            )
            worker.finished.connect(thread.quit)
            worker.finished.connect(worker.deleteLater)
            thread.finished.connect(thread.deleteLater)
            thread.start()
        self._refresh_batch_status()

    @pyqtSlot(object, str)
    def _job_log(self, worker: object, text: str) -> None:
        active = self._active.get(worker)
        if not active:
            return
        name = self._job_names[active[1]]
        self.log.appendPlainText(f"[{name}] {text}")

    @pyqtSlot(object, str)
    def _job_status(self, worker: object, text: str) -> None:
        active = self._active.get(worker)
        if not active:
            return
        name = self._job_names[active[1]]
        self.status_label.setText(
            f"{name}: {text} · 并行 {len(self._active)} · 排队 {len(self._pending)}"
        )

    @pyqtSlot(object, int, int)
    def _job_progress_changed(
        self, worker: object, current: int, maximum: int
    ) -> None:
        active = self._active.get(worker)
        if not active:
            return
        index = active[1]
        self._job_progress[index] = (
            0 if maximum <= 0 else min(1000, max(0, round(current * 1000 / maximum)))
        )
        total = max(1, len(self._job_names))
        aggregate = sum(self._job_progress.values())
        self.progress.setValue(min(1000, round(aggregate / total)))

    @pyqtSlot(object, bool, str)
    def _job_finished_signal(
        self, worker: object, success: bool, message: str
    ) -> None:
        active = self._active.pop(worker, None)
        if not active:
            return
        _, index = active
        self._job_progress[index] = 1000
        self._completed_jobs += 1
        if success:
            self._successful_jobs += 1
        elif message != "转换已取消":
            self._failures.append(f"{self._job_names[index]}：{message}")
            self.log.appendPlainText(f"[失败] {self._failures[-1]}")
        self._start_more_jobs()
        if not self._active and (not self._pending or self._cancelling):
            self._finish_batch()

    def _refresh_batch_status(self) -> None:
        if not self._job_names:
            return
        self.status_label.setText(
            f"完成 {self._completed_jobs}/{len(self._job_names)} · "
            f"并行 {len(self._active)} · 排队 {len(self._pending)}"
        )

    def _cancel_all(self) -> None:
        self._cancelling = True
        self._pending.clear()
        self.convert_button.setEnabled(False)
        self.status_label.setText("正在取消所有 FFmpeg 任务…")
        for worker in list(self._active):
            worker.cancel()
        if not self._active:
            self._finish_batch()

    def _finish_batch(self) -> None:
        total = len(self._job_names)
        if not self._cancelling:
            self.progress.setValue(1000)
        self.convert_button.setEnabled(True)
        self._set_configuration_enabled(True)
        self.convert_button.setText("开始批量转换")
        if self._cancelling:
            self.status_label.setText(
                f"已取消；此前完成 {self._successful_jobs}/{total}"
            )
        elif self._failures:
            self.status_label.setText(
                f"批量完成：成功 {self._successful_jobs}，失败 {len(self._failures)}"
            )
            QMessageBox.warning(
                self,
                "部分转换失败",
                "\n".join(self._failures[:8]),
            )
        else:
            self.status_label.setText(f"全部 {total} 个视频转换完成")
            destination = (
                f"已直接导入 TF 卡：\n{self._batch_tf_target}\n\n"
                "主视频和 WAV 位于 /video，旋转版位于 /video/rotated。"
                if self._batch_tf_target is not None
                else "主文件、同名 WAV 和 rotated 目录已生成，可复制到 TF 卡的 /video。"
            )
            QMessageBox.information(
                self,
                "批量转换完成",
                destination,
            )
        self._pending.clear()
        self._job_names.clear()
        self._job_progress.clear()
        self._cancelling = False
        self._batch_tf_target = None
        if self._close_when_done:
            self._close_when_done = False
            QTimer.singleShot(0, self.close)

    def closeEvent(self, event) -> None:  # noqa: ANN001
        if self._active or self._pending:
            self._close_when_done = True
            self._cancel_all()
            event.ignore()
            return
        super().closeEvent(event)


def main() -> int:
    native_app.ConversionWorker = FfmpegConversionWorker
    native_app.ConverterWindow = FfmpegConverterWindow
    return native_app.main()


if __name__ == "__main__":
    raise SystemExit(main())
