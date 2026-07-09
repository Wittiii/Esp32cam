import argparse
import json
import logging
import socket
import subprocess
import threading
import time
from dataclasses import asdict, dataclass, fields
from pathlib import Path
from typing import Any, Callable

import paho.mqtt.client as mqtt


LOG = logging.getLogger("pi-cam-controller")


StatusListener = Callable[[], None]


@dataclass
class StreamSettings:
    host: str
    port: int
    width: int
    height: int
    framerate: int
    bitrate: int
    sharpness: float
    brightness: float
    contrast: float
    saturation: float
    mode: str = "mediamtx_rtsp"
    path: str = "pi-zero-01"
    transport: str = "tcp"
    codec: str = "h264"
    inline_headers: bool = True
    nopreview: bool = True
    ffmpeg_path: str = "ffmpeg"

    def target_url(self) -> str:
        if self.mode == "mediamtx_rtsp":
            stream_path = str(self.path or "pi-zero-01").lstrip("/")
            return f"rtsp://{self.host}:{self.port}/{stream_path}"
        if self.mode == "tcp":
            return f"tcp://{self.host}:{self.port}"
        raise ValueError(f"unsupported stream mode: {self.mode}")


@dataclass
class TimelapseSettings:
    enabled: bool = False
    interval_seconds: int = 60
    output_dir: str = "timelapse"
    max_storage_gb: float = 22.0
    jpeg_quality: int = 2
    storage_check_seconds: int = 15

    @property
    def storage_limit_bytes(self) -> int:
        return int(self.max_storage_gb * 1024 * 1024 * 1024)


@dataclass
class MqttSettings:
    host: str
    port: int
    username: str
    password: str
    client_id: str
    base_topic: str
    keepalive: int = 60


class CameraStreamer:
    def __init__(self, stream: StreamSettings, timelapse: TimelapseSettings, config_path: Path) -> None:
        self._stream_settings = stream
        self._timelapse_settings = timelapse
        self._config_path = config_path
        self._capture_process: subprocess.Popen[bytes] | None = None
        self._publish_process: subprocess.Popen[bytes] | None = None
        self._timelapse_process: subprocess.Popen[bytes] | None = None
        self._socket: socket.socket | None = None
        self._stdout_thread: threading.Thread | None = None
        self._timelapse_thread: threading.Thread | None = None
        self._timelapse_stop_requested = threading.Event()
        self._stop_requested = threading.Event()
        self._lock = threading.RLock()
        self._status_listener: StatusListener | None = None

        self._stream_state = "stopped"
        self._stream_error = ""
        self._timelapse_state = "running" if timelapse.enabled else "stopped"
        self._timelapse_error = ""
        self._timelapse_last_image = ""
        self._timelapse_storage_bytes = 0
        self._last_storage_refresh = 0.0

    @property
    def settings(self) -> StreamSettings:
        with self._lock:
            return self._stream_settings

    @property
    def timelapse_settings(self) -> TimelapseSettings:
        with self._lock:
            return self._timelapse_settings

    @property
    def state(self) -> str:
        with self._lock:
            return self._stream_state

    @property
    def last_error(self) -> str:
        with self._lock:
            return self._stream_error

    @property
    def timelapse_state(self) -> str:
        with self._lock:
            return self._timelapse_state

    @property
    def timelapse_error(self) -> str:
        with self._lock:
            return self._timelapse_error

    @property
    def timelapse_storage_bytes(self) -> int:
        with self._lock:
            return self._timelapse_storage_bytes

    @property
    def timelapse_last_image(self) -> str:
        with self._lock:
            return self._timelapse_last_image

    def set_status_listener(self, listener: StatusListener) -> None:
        with self._lock:
            self._status_listener = listener

    def _notify_status(self) -> None:
        listener = None
        with self._lock:
            listener = self._status_listener
        if listener is not None:
            try:
                listener()
            except Exception:
                LOG.exception("status listener failed")

    def _set_stream_state(self, state: str, error: str = "") -> None:
        with self._lock:
            self._stream_state = state
            self._stream_error = error
        self._notify_status()

    def _set_timelapse_state(self, state: str, error: str = "") -> None:
        with self._lock:
            self._timelapse_state = state
            self._timelapse_error = error
        self._notify_status()

    def describe_target(self) -> str:
        return self.settings.target_url()

    def _is_stopping(self) -> bool:
        return self._stop_requested.is_set()

    def _project_root(self) -> Path:
        return self._config_path.parent.parent

    def timelapse_output_dir(self) -> Path:
        output_dir = Path(self.timelapse_settings.output_dir)
        if not output_dir.is_absolute():
            output_dir = self._project_root() / output_dir
        return output_dir

    def timelapse_status(self) -> dict[str, Any]:
        self._refresh_timelapse_storage(force=True)
        return {
            "state": self.timelapse_state,
            "error": self.timelapse_error,
            "storage_bytes": self.timelapse_storage_bytes,
            "storage_limit_bytes": self.timelapse_settings.storage_limit_bytes,
            "last_image": self.timelapse_last_image,
            "output_dir": str(self.timelapse_output_dir()),
            "enabled": self.timelapse_settings.enabled,
            "interval_seconds": self.timelapse_settings.interval_seconds,
        }

    def update_settings(self, **changes: Any) -> bool:
        restart_required = False
        with self._lock:
            data = asdict(self._stream_settings)
            for name in data:
                if name in changes:
                    value = changes[name]
                    if value != data[name]:
                        data[name] = value
                        restart_required = True
            self._stream_settings = StreamSettings(**data)
            self._write_config()
        self._notify_status()
        return restart_required

    def update_timelapse_settings(self, **changes: Any) -> bool:
        restart_required = False
        with self._lock:
            data = asdict(self._timelapse_settings)
            for name in data:
                if name in changes:
                    value = changes[name]
                    if value != data[name]:
                        data[name] = value
                        restart_required = True
            self._timelapse_settings = TimelapseSettings(**data)
            self._write_config()

        self._refresh_timelapse_storage(force=True)
        self._notify_status()
        return restart_required

    def enable_timelapse(self) -> None:
        self.update_timelapse_settings(enabled=True)
        if self._timelapse_capacity_reached():
            self.update_timelapse_settings(enabled=False)
            self._set_timelapse_state("storage_full", "timelapse storage limit reached")
            return

        if self.state == "running":
            self._ensure_timelapse_process()
        else:
            self._set_timelapse_state("stopped")

    def disable_timelapse(self) -> None:
        self.update_timelapse_settings(enabled=False)
        self._stop_timelapse_process(set_state="stopped")

    def _write_config(self) -> None:
        current = {}
        if self._config_path.exists():
            with self._config_path.open("r", encoding="utf-8") as handle:
                current = json.load(handle)

        current["stream"] = asdict(self._stream_settings)
        current["timelapse"] = asdict(self._timelapse_settings)
        with self._config_path.open("w", encoding="utf-8") as handle:
            json.dump(current, handle, indent=2)

    def _build_capture_command(self) -> list[str]:
        settings = self.settings
        command = [
            "rpicam-vid",
            "-t",
            "0",
            "--width",
            str(settings.width),
            "--height",
            str(settings.height),
            "--framerate",
            str(settings.framerate),
            "--bitrate",
            str(settings.bitrate),
            "--sharpness",
            str(settings.sharpness),
            "--brightness",
            str(settings.brightness),
            "--contrast",
            str(settings.contrast),
            "--saturation",
            str(settings.saturation),
            "--codec",
            settings.codec,
            "-o",
            "-",
        ]
        if settings.inline_headers:
            command.append("--inline")
        if settings.nopreview:
            command.append("--nopreview")
        return command

    def _build_publish_command(self) -> list[str]:
        settings = self.settings
        return [
            settings.ffmpeg_path,
            "-loglevel",
            "warning",
            "-fflags",
            "+genpts+nobuffer",
            "-use_wallclock_as_timestamps",
            "1",
            "-flags",
            "low_delay",
            "-r",
            str(settings.framerate),
            "-f",
            "h264",
            "-i",
            "pipe:0",
            "-an",
            "-c:v",
            "copy",
            "-rtsp_transport",
            settings.transport,
            "-f",
            "rtsp",
            settings.target_url(),
        ]

    def _build_timelapse_command(self) -> list[str]:
        stream = self.settings
        timelapse = self.timelapse_settings
        output_dir = self.timelapse_output_dir()
        output_dir.mkdir(parents=True, exist_ok=True)
        pattern = str(output_dir / "frame-%Y%m%d-%H%M%S.jpg")

        return [
            stream.ffmpeg_path,
            "-loglevel",
            "warning",
            "-fflags",
            "+genpts+nobuffer",
            "-use_wallclock_as_timestamps",
            "1",
            "-flags",
            "low_delay",
            "-r",
            str(stream.framerate),
            "-f",
            "h264",
            "-i",
            "pipe:0",
            "-vf",
            f"fps=1/{max(1, timelapse.interval_seconds)}",
            "-fps_mode",
            "vfr",
            "-q:v",
            str(max(2, min(31, timelapse.jpeg_quality))),
            "-strftime",
            "1",
            "-f",
            "image2",
            pattern,
        ]

    def _next_timelapse_output_path(self) -> Path:
        output_dir = self.timelapse_output_dir()
        output_dir.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime())
        return output_dir / f"frame-{stamp}.jpg"

    def _build_timelapse_snapshot_command(self, output_path: Path) -> list[str]:
        stream = self.settings
        timelapse = self.timelapse_settings
        return [
            stream.ffmpeg_path,
            "-hide_banner",
            "-loglevel",
            "warning",
            "-rtsp_transport",
            stream.transport,
            "-i",
            stream.target_url(),
            "-frames:v",
            "1",
            "-q:v",
            str(max(2, min(31, timelapse.jpeg_quality))),
            "-y",
            str(output_path),
        ]

    def _open_socket(self) -> socket.socket:
        settings = self.settings
        sock = socket.create_connection((settings.host, settings.port), timeout=10)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return sock

    def _pump_stderr(self, process: subprocess.Popen[bytes], tag: str) -> None:
        assert process.stderr is not None
        for raw_line in process.stderr:
            line = raw_line.decode("utf-8", errors="replace").rstrip()
            if line:
                LOG.info("[%s] %s", tag, line)
            if self._is_stopping():
                break

    def _calculate_storage_usage(self, root: Path) -> tuple[int, str]:
        total = 0
        latest_file = ""
        latest_mtime = 0.0
        if not root.exists():
            return total, latest_file

        for file_path in root.rglob("*.jpg"):
            try:
                stat = file_path.stat()
            except OSError:
                continue
            total += stat.st_size
            if stat.st_mtime > latest_mtime:
                latest_mtime = stat.st_mtime
                latest_file = str(file_path)
        return total, latest_file

    def _refresh_timelapse_storage(self, force: bool = False) -> bool:
        timelapse = self.timelapse_settings
        interval = max(1, timelapse.storage_check_seconds)
        now = time.monotonic()
        if not force and now - self._last_storage_refresh < interval:
            return False

        total, latest_file = self._calculate_storage_usage(self.timelapse_output_dir())
        changed = False
        with self._lock:
            changed = (
                self._timelapse_storage_bytes != total
                or self._timelapse_last_image != latest_file
            )
            self._timelapse_storage_bytes = total
            self._timelapse_last_image = latest_file
            self._last_storage_refresh = now
        return changed

    def _timelapse_capacity_reached(self) -> bool:
        self._refresh_timelapse_storage(force=True)
        return self.timelapse_storage_bytes >= self.timelapse_settings.storage_limit_bytes

    def _start_timelapse_process(self) -> None:
        if not self.timelapse_settings.enabled:
            self._set_timelapse_state("stopped")
            return
        if self.settings.mode == "mediamtx_rtsp":
            if self._timelapse_thread is not None and self._timelapse_thread.is_alive():
                return
        elif self._timelapse_process is not None:
            return
        if self._timelapse_capacity_reached():
            self.update_timelapse_settings(enabled=False)
            self._set_timelapse_state("storage_full", "timelapse storage limit reached")
            return

        if self.settings.mode == "mediamtx_rtsp":
            LOG.info("starting timelapse snapshot worker for %s", self.settings.target_url())
            self._timelapse_stop_requested.clear()
            self._timelapse_thread = threading.Thread(target=self._run_timelapse_snapshot_loop, daemon=True)
            self._timelapse_thread.start()
            self._set_timelapse_state("running")
            return

        command = self._build_timelapse_command()
        LOG.info("starting timelapse writer: %s", " ".join(command))
        self._timelapse_process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        timelapse_stderr_thread = threading.Thread(
            target=self._pump_stderr,
            args=(self._timelapse_process, "timelapse"),
            daemon=True,
        )
        timelapse_stderr_thread.start()
        self._set_timelapse_state("running")

    def _stop_timelapse_process(self, set_state: str | None = None, error: str = "") -> None:
        self._timelapse_stop_requested.set()
        timelapse_thread = self._timelapse_thread
        self._timelapse_thread = None
        process = self._timelapse_process
        self._timelapse_process = None
        if process is not None:
            try:
                if process.stdin is not None:
                    process.stdin.close()
            except OSError:
                pass
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()

        if timelapse_thread is not None and timelapse_thread.is_alive() and threading.current_thread() is not timelapse_thread:
            timelapse_thread.join(timeout=5)

        if set_state is not None:
            self._set_timelapse_state(set_state, error)
        self._refresh_timelapse_storage(force=True)

    def _ensure_timelapse_process(self) -> None:
        if self.timelapse_settings.enabled and self._timelapse_process is None:
            try:
                self._start_timelapse_process()
            except Exception as exc:
                LOG.error("timelapse start failed: %s", exc)
                self._set_timelapse_state("error", str(exc))

    def _write_publish_chunk(self, chunk: bytes) -> None:
        process = self._publish_process
        if process is None or process.stdin is None:
            raise RuntimeError("publish process is not available")
        process.stdin.write(chunk)
        process.stdin.flush()

    def _write_timelapse_chunk(self, chunk: bytes) -> None:
        if self.settings.mode == "mediamtx_rtsp":
            return

        if not self.timelapse_settings.enabled:
            return

        self._ensure_timelapse_process()
        process = self._timelapse_process
        if process is None or process.stdin is None:
            return

        try:
            process.stdin.write(chunk)
            process.stdin.flush()
        except (BrokenPipeError, OSError, ValueError) as exc:
            if self._is_stopping() or self.timelapse_state in ("stopped", "storage_full"):
                return
            LOG.error("timelapse writer failed: %s", exc)
            self._stop_timelapse_process(set_state="error", error=str(exc))
            return

        refreshed = self._refresh_timelapse_storage()
        if refreshed:
            self._notify_status()
        if self._timelapse_capacity_reached():
            LOG.warning("timelapse storage limit reached, stopping timelapse capture")
            self.update_timelapse_settings(enabled=False)
            self._stop_timelapse_process(set_state="storage_full", error="timelapse storage limit reached")

    def _run_timelapse_snapshot_loop(self) -> None:
        interval_seconds = max(1, self.timelapse_settings.interval_seconds)
        next_capture_at = time.monotonic() + min(5, interval_seconds)

        try:
            while not self._is_stopping() and not self._timelapse_stop_requested.is_set():
                wait_seconds = max(0.0, next_capture_at - time.monotonic())
                if self._timelapse_stop_requested.wait(wait_seconds):
                    break

                if self._timelapse_capacity_reached():
                    LOG.warning("timelapse storage limit reached, stopping timelapse capture")
                    self.update_timelapse_settings(enabled=False)
                    self._set_timelapse_state("storage_full", "timelapse storage limit reached")
                    break

                output_path = self._next_timelapse_output_path()
                command = self._build_timelapse_snapshot_command(output_path)
                LOG.info("capturing timelapse frame: %s", output_path.name)

                process: subprocess.Popen[bytes] | None = None
                try:
                    process = subprocess.Popen(
                        command,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.PIPE,
                        bufsize=0,
                    )
                    with self._lock:
                        self._timelapse_process = process

                    _, stderr = process.communicate(timeout=20)
                    with self._lock:
                        if self._timelapse_process is process:
                            self._timelapse_process = None

                    stderr_text = stderr.decode("utf-8", errors="replace").strip() if stderr else ""
                    if process.returncode != 0:
                        message = stderr_text or f"timelapse snapshot failed with code {process.returncode}"
                        LOG.warning(message)
                        self._set_timelapse_state("running", message)
                    else:
                        self._set_timelapse_state("running")
                        if self._refresh_timelapse_storage(force=True):
                            self._notify_status()
                except subprocess.TimeoutExpired:
                    if process is not None:
                        process.kill()
                        with self._lock:
                            if self._timelapse_process is process:
                                self._timelapse_process = None
                    self._set_timelapse_state("running", "timelapse snapshot timeout")
                except Exception as exc:
                    with self._lock:
                        if self._timelapse_process is process:
                            self._timelapse_process = None
                    LOG.warning("timelapse snapshot failed: %s", exc)
                    self._set_timelapse_state("running", str(exc))

                interval_seconds = max(1, self.timelapse_settings.interval_seconds)
                next_capture_at += interval_seconds
                if next_capture_at < time.monotonic():
                    next_capture_at = time.monotonic() + interval_seconds
        finally:
            with self._lock:
                self._timelapse_process = None
                if threading.current_thread() is self._timelapse_thread:
                    self._timelapse_thread = None

    def _stop_after_failure(self, reason: str) -> None:
        self._set_stream_state("error", reason)
        self.stop()

    def _pump_stdout(self) -> None:
        assert self._capture_process is not None
        assert self._capture_process.stdout is not None

        capture_stdout = self._capture_process.stdout

        try:
            while not self._is_stopping():
                chunk = capture_stdout.read(64 * 1024)
                if not chunk:
                    break

                if self.settings.mode == "tcp":
                    assert self._socket is not None
                    self._socket.sendall(chunk)
                else:
                    self._write_publish_chunk(chunk)

                self._write_timelapse_chunk(chunk)
        except (BrokenPipeError, ConnectionError, OSError, ValueError, RuntimeError) as exc:
            if self._is_stopping():
                LOG.info("stream pump stopped cleanly")
                return
            LOG.error("stream transport failed: %s", exc)
            self._stop_after_failure(f"stream transport: {exc}")
            return
        finally:
            publish_process = self._publish_process
            if publish_process is not None and publish_process.stdin is not None:
                try:
                    publish_process.stdin.close()
                except (OSError, ValueError):
                    pass

        if self._is_stopping():
            return

        capture_code = self._capture_process.poll()
        publish_code = self._publish_process.poll() if self._publish_process is not None else 0
        if publish_code not in (None, 0):
            self._stop_after_failure(f"publisher exited with code {publish_code}")
            return
        if capture_code not in (None, 0):
            self._stop_after_failure(f"capture exited with code {capture_code}")
            return
        self._stop_after_failure("stream ended")

    def start(self) -> None:
        with self._lock:
            if self._capture_process is not None:
                LOG.info("stream already running")
                return

            capture_command = self._build_capture_command()
            self._stop_requested.clear()
            self._set_stream_state("starting")
            LOG.info("starting capture: %s", " ".join(capture_command))

            try:
                if self.settings.mode == "tcp":
                    LOG.info("opening tcp destination: %s", self.settings.target_url())
                    self._socket = self._open_socket()
                elif self.settings.mode == "mediamtx_rtsp":
                    publish_command = self._build_publish_command()
                    LOG.info("starting publisher: %s", " ".join(publish_command))
                    self._publish_process = subprocess.Popen(
                        publish_command,
                        stdin=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        bufsize=0,
                    )
                else:
                    raise RuntimeError(f"unsupported stream mode: {self.settings.mode}")

                self._capture_process = subprocess.Popen(
                    capture_command,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    bufsize=0,
                )
            except Exception as exc:
                self._cleanup_stream_handles()
                self._set_stream_state("error", str(exc))
                raise RuntimeError(f"failed to start stream: {exc}") from exc

            self._ensure_timelapse_process()

            stdout_thread = threading.Thread(target=self._pump_stdout, daemon=True)
            stderr_threads: list[threading.Thread] = []

            if self._publish_process is not None:
                stderr_threads.append(
                    threading.Thread(target=self._pump_stderr, args=(self._publish_process, "ffmpeg"), daemon=True)
                )

            stderr_threads.append(
                threading.Thread(target=self._pump_stderr, args=(self._capture_process, "rpicam"), daemon=True)
            )

            self._stdout_thread = stdout_thread
            stdout_thread.start()
            for thread in stderr_threads:
                thread.start()
            self._set_stream_state("running")

    def _cleanup_stream_handles(self) -> None:
        if self._socket is not None:
            try:
                self._socket.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self._socket.close()
            self._socket = None

        if self._publish_process is not None:
            try:
                if self._publish_process.stdin is not None:
                    self._publish_process.stdin.close()
            except OSError:
                pass
            if self._publish_process.poll() is None:
                self._publish_process.terminate()
                try:
                    self._publish_process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self._publish_process.kill()
            self._publish_process = None

        if self._capture_process is not None:
            if self._capture_process.poll() is None:
                self._capture_process.terminate()
                try:
                    self._capture_process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self._capture_process.kill()
            self._capture_process = None

    def stop(self) -> None:
        with self._lock:
            self._stop_requested.set()
            previous_state = self._stream_state
            self._cleanup_stream_handles()
            self._stop_timelapse_process(set_state="stopped" if not self.timelapse_settings.enabled else self.timelapse_state)
            if previous_state != "error":
                self._stream_state = "stopped"
                self._stream_error = ""
        self._notify_status()

    def restart(self) -> None:
        self.stop()
        time.sleep(0.5)
        self.start()


class StreamSupervisor:
    def __init__(self, streamer: CameraStreamer, retry_initial: float = 2.0, retry_max: float = 30.0) -> None:
        self._streamer = streamer
        self._retry_initial = retry_initial
        self._retry_max = retry_max
        self._retry_delay = retry_initial
        self._desired_running = False
        self._stop_event = threading.Event()
        self._wakeup = threading.Event()
        self._thread = threading.Thread(target=self._run, name="stream-supervisor", daemon=True)
        self._lock = threading.RLock()

    @property
    def desired_running(self) -> bool:
        with self._lock:
            return self._desired_running

    def start(self, autostart: bool = False) -> None:
        with self._lock:
            self._desired_running = autostart
            self._retry_delay = self._retry_initial
        self._thread.start()
        if autostart:
            self._wakeup.set()

    def stop(self) -> None:
        with self._lock:
            self._desired_running = False
        self._stop_event.set()
        self._wakeup.set()
        self._streamer.stop()
        if self._thread.is_alive():
            self._thread.join(timeout=5)

    def request_start(self) -> None:
        with self._lock:
            self._desired_running = True
            self._retry_delay = self._retry_initial
        LOG.info("stream desired state set to running")
        self._wakeup.set()

    def request_stop(self) -> None:
        with self._lock:
            self._desired_running = False
            self._retry_delay = self._retry_initial
        LOG.info("stream desired state set to stopped")
        self._streamer.stop()
        self._wakeup.set()

    def request_restart(self) -> None:
        with self._lock:
            self._desired_running = True
            self._retry_delay = self._retry_initial
        LOG.info("stream restart requested")
        self._streamer.stop()
        self._wakeup.set()

    def apply_stream_settings(self, changes: dict[str, Any]) -> None:
        restart_required = self._streamer.update_settings(**changes)
        if restart_required and self.desired_running:
            LOG.info("stream settings changed, scheduling restart")
            self._streamer.stop()
            with self._lock:
                self._retry_delay = self._retry_initial
            self._wakeup.set()

    def _run(self) -> None:
        while not self._stop_event.is_set():
            if self.desired_running and self._streamer.state in ("stopped", "error"):
                try:
                    self._streamer.start()
                    with self._lock:
                        self._retry_delay = self._retry_initial
                    self._wakeup.wait(1.0)
                    self._wakeup.clear()
                    continue
                except Exception as exc:
                    with self._lock:
                        delay = self._retry_delay
                        self._retry_delay = min(self._retry_delay * 2.0, self._retry_max)
                    LOG.warning("stream start failed, retrying in %.1fs: %s", delay, exc)
                    self._wakeup.wait(delay)
                    self._wakeup.clear()
                    continue

            self._wakeup.wait(1.0)
            self._wakeup.clear()


class MqttController:
    def __init__(self, mqtt_settings: MqttSettings, streamer: CameraStreamer, supervisor: StreamSupervisor) -> None:
        self._mqtt_settings = mqtt_settings
        self._streamer = streamer
        self._supervisor = supervisor
        self._client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=mqtt_settings.client_id,
            protocol=mqtt.MQTTv311,
        )
        self._client.reconnect_delay_set(min_delay=1, max_delay=30)
        if mqtt_settings.username:
            self._client.username_pw_set(mqtt_settings.username, mqtt_settings.password)
        self._client.on_connect = self._on_connect
        self._client.on_message = self._on_message
        self._client.on_disconnect = self._on_disconnect

    @property
    def base_topic(self) -> str:
        return self._mqtt_settings.base_topic.rstrip("/")

    def _topic(self, suffix: str) -> str:
        return f"{self.base_topic}/{suffix}"

    def _publish(self, suffix: str, payload: str, retain: bool = False) -> None:
        self._client.publish(self._topic(suffix), payload=payload, qos=1, retain=retain)

    def publish_streamer_status(self) -> None:
        self._publish_status()

    def _publish_status(self) -> None:
        self._publish("status/state", self._streamer.state, retain=True)
        self._publish("status/error", self._streamer.last_error, retain=True)
        self._publish("status/config", json.dumps(asdict(self._streamer.settings)), retain=True)
        self._publish("status/target", self._streamer.describe_target(), retain=True)
        self._publish("status/desired", "running" if self._supervisor.desired_running else "stopped", retain=True)

        timelapse = self._streamer.timelapse_status()
        self._publish("status/timelapse/state", timelapse["state"], retain=True)
        self._publish("status/timelapse/error", timelapse["error"], retain=True)
        self._publish("status/timelapse/storage_bytes", str(timelapse["storage_bytes"]), retain=True)
        self._publish("status/timelapse/storage_limit_bytes", str(timelapse["storage_limit_bytes"]), retain=True)
        self._publish("status/timelapse/last_image", timelapse["last_image"], retain=True)
        self._publish("status/timelapse/output_dir", timelapse["output_dir"], retain=True)
        self._publish("status/timelapse/enabled", str(timelapse["enabled"]).lower(), retain=True)
        self._publish("status/timelapse/interval_seconds", str(timelapse["interval_seconds"]), retain=True)

    def _on_connect(self, client: mqtt.Client, userdata: Any, flags: Any, reason_code: Any, properties: Any) -> None:
        LOG.info("mqtt connected: %s", reason_code)
        for suffix in (
            "cmd/start",
            "cmd/stop",
            "cmd/restart",
            "cmd/set",
            "cmd/ping",
            "cmd/timelapse/start",
            "cmd/timelapse/stop",
            "cmd/timelapse/set",
        ):
            client.subscribe(self._topic(suffix), qos=1)
        self._publish("status/online", "true", retain=True)
        self._publish_status()

    def _on_disconnect(
        self,
        client: mqtt.Client,
        userdata: Any,
        disconnect_flags: Any,
        reason_code: Any,
        properties: Any,
    ) -> None:
        LOG.warning("mqtt disconnected: %s", reason_code)

    def _handle_stream_set(self, payload: dict[str, Any]) -> None:
        allowed = {field.name for field in fields(StreamSettings)}
        changes = {key: value for key, value in payload.items() if key in allowed}
        if not changes:
            raise ValueError("no valid stream settings in payload")
        self._supervisor.apply_stream_settings(changes)

    def _handle_timelapse_set(self, payload: dict[str, Any]) -> None:
        allowed = {field.name for field in fields(TimelapseSettings)}
        changes = {key: value for key, value in payload.items() if key in allowed and key != "enabled"}
        if not changes:
            raise ValueError("no valid timelapse settings in payload")
        self._streamer.update_timelapse_settings(**changes)
        if self._streamer.timelapse_settings.enabled and self._streamer.state == "running":
            self._streamer.disable_timelapse()
            self._streamer.enable_timelapse()

    def _on_message(self, client: mqtt.Client, userdata: Any, message: mqtt.MQTTMessage) -> None:
        topic = message.topic
        payload_text = message.payload.decode("utf-8", errors="replace").strip()
        LOG.info("mqtt message topic=%s payload=%s", topic, payload_text)

        try:
            if topic == self._topic("cmd/start"):
                self._supervisor.request_start()
            elif topic == self._topic("cmd/stop"):
                self._supervisor.request_stop()
            elif topic == self._topic("cmd/restart"):
                self._supervisor.request_restart()
            elif topic == self._topic("cmd/ping"):
                self._publish("status/pong", payload_text or "pong", retain=True)
            elif topic == self._topic("cmd/set"):
                payload = json.loads(payload_text)
                if not isinstance(payload, dict):
                    raise ValueError("payload must be a JSON object")
                self._handle_stream_set(payload)
            elif topic == self._topic("cmd/timelapse/start"):
                self._streamer.enable_timelapse()
            elif topic == self._topic("cmd/timelapse/stop"):
                self._streamer.disable_timelapse()
            elif topic == self._topic("cmd/timelapse/set"):
                payload = json.loads(payload_text)
                if not isinstance(payload, dict):
                    raise ValueError("payload must be a JSON object")
                self._handle_timelapse_set(payload)
            else:
                LOG.warning("ignoring unknown topic: %s", topic)
        except Exception as exc:
            LOG.exception("mqtt command failed")
            self._streamer._set_stream_state("error", str(exc))
        finally:
            self._publish_status()

    def start(self) -> None:
        self._client.will_set(self._topic("status/online"), payload="false", qos=1, retain=True)
        self._client.connect_async(self._mqtt_settings.host, self._mqtt_settings.port, self._mqtt_settings.keepalive)
        self._client.loop_start()

    def stop(self) -> None:
        self._client.disconnect()
        self._client.loop_stop()


def load_config(path: Path) -> tuple[StreamSettings, TimelapseSettings, MqttSettings]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)

    timelapse_data = data.get("timelapse", {})
    return (
        StreamSettings(**data["stream"]),
        TimelapseSettings(**timelapse_data),
        MqttSettings(**data["mqtt"]),
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="MQTT-controlled Raspberry Pi H.264 streamer.")
    parser.add_argument("--config", default="pi_streamer/config.json", help="Path to the JSON config file")
    parser.add_argument("--autostart", action="store_true", help="Start the camera stream immediately")
    parser.add_argument("--log-level", default="INFO", help="Python log level")
    args = parser.parse_args()

    logging.basicConfig(
        level=getattr(logging, args.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    config_path = Path(args.config).resolve()
    stream_settings, timelapse_settings, mqtt_settings = load_config(config_path)
    streamer = CameraStreamer(stream_settings, timelapse_settings, config_path)
    supervisor = StreamSupervisor(streamer)
    controller = MqttController(mqtt_settings, streamer, supervisor)
    streamer.set_status_listener(controller.publish_streamer_status)

    try:
        controller.start()
        supervisor.start(autostart=args.autostart)
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        LOG.info("shutting down")
    finally:
        controller.stop()
        supervisor.stop()


if __name__ == "__main__":
    main()
