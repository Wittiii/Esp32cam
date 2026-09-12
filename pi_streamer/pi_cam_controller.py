import argparse
import json
import logging
import math
import os
import queue
import select
import signal
import socket
import subprocess
import tempfile
import threading
import time
from dataclasses import asdict, dataclass, fields
from pathlib import Path
from typing import Any, Callable

import paho.mqtt.client as mqtt


LOG = logging.getLogger("pi-cam-controller")


StatusListener = Callable[[], None]


def _boolean(value: Any, name: str) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, str) and value.lower() in ("true", "false"):
        return value.lower() == "true"
    if type(value) is int and value in (0, 1):
        return bool(value)
    raise ValueError(f"{name} must be true or false")


def _number(value: Any, name: str, minimum: float, maximum: float, integer: bool = False) -> int | float:
    try:
        if isinstance(value, bool):
            raise ValueError
        number = float(value)
    except (TypeError, ValueError, OverflowError) as exc:
        raise ValueError(f"{name} must be a number") from exc
    if not math.isfinite(number) or not minimum <= number <= maximum or (integer and not number.is_integer()):
        raise ValueError(f"{name} must be {'an integer' if integer else 'a number'} between {minimum} and {maximum}")
    return int(number) if integer else number


@dataclass(frozen=True)
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

    def __post_init__(self) -> None:
        if (not isinstance(self.host, str) or not self.host or
                any(not c.isprintable() or c.isspace() or c in "/@?#[]" for c in self.host)):
            raise ValueError("host must be a hostname or an IP address, without a URL or credentials")
        if self.mode not in ("mediamtx_rtsp", "tcp"):
            raise ValueError("mode must be mediamtx_rtsp or tcp")
        if self.transport not in ("tcp", "udp"):
            raise ValueError("transport must be tcp or udp")
        if self.codec != "h264":
            raise ValueError("the stream pipeline requires codec h264")
        if (not isinstance(self.path, str) or not self.path.strip("/") or
                any(not c.isprintable() or c.isspace() or c in "?#" for c in self.path)):
            raise ValueError("path must be a non-empty RTSP path without whitespace or query parameters")
        if not isinstance(self.ffmpeg_path, str) or not self.ffmpeg_path or "\x00" in self.ffmpeg_path:
            raise ValueError("ffmpeg_path must name an executable")
        for name, minimum, maximum in (
            ("port", 1, 65535), ("width", 16, 4096), ("height", 16, 4096),
            ("framerate", 1, 60), ("bitrate", 10000, 50000000),
        ):
            object.__setattr__(self, name, _number(getattr(self, name), name, minimum, maximum, integer=True))
        if self.width % 2 or self.height % 2:
            raise ValueError("width and height must be even for H.264")
        for name, minimum, maximum in (
            ("sharpness", 0, 16), ("brightness", -1, 1), ("contrast", 0, 32), ("saturation", 0, 32),
        ):
            object.__setattr__(self, name, _number(getattr(self, name), name, minimum, maximum))
        for name in ("inline_headers", "nopreview"):
            object.__setattr__(self, name, _boolean(getattr(self, name), name))

    def target_url(self) -> str:
        host = f"[{self.host}]" if ":" in self.host else self.host
        if self.mode == "mediamtx_rtsp":
            stream_path = str(self.path or "pi-zero-01").lstrip("/")
            return f"rtsp://{host}:{self.port}/{stream_path}"
        if self.mode == "tcp":
            return f"tcp://{host}:{self.port}"
        raise ValueError(f"unsupported stream mode: {self.mode}")


@dataclass(frozen=True)
class ServerCaptureSettings:
    enabled: bool = False
    interval_seconds: int = 60

    def __post_init__(self) -> None:
        object.__setattr__(self, "enabled", _boolean(self.enabled, "enabled"))
        object.__setattr__(self, "interval_seconds", _number(self.interval_seconds, "interval_seconds", 1, 86400, integer=True))


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
    def __init__(self, stream: StreamSettings, server_capture: ServerCaptureSettings, config_path: Path) -> None:
        self._stream_settings = stream
        self._server_capture_settings = server_capture
        self._config_path = config_path
        self._capture_process: subprocess.Popen[bytes] | None = None
        self._publish_process: subprocess.Popen[bytes] | None = None
        self._socket: socket.socket | None = None
        self._stdout_thread: threading.Thread | None = None
        self._stderr_threads: list[threading.Thread] = []
        self._stop_requested = threading.Event()
        self._lock = threading.RLock()
        self._lifecycle_lock = threading.Lock()
        self._status_listener: StatusListener | None = None

        self._stream_state = "stopped"
        self._stream_error = ""
        self._capture_request_state = "enabled" if server_capture.enabled else "disabled"
        self._capture_request_error = ""
        self._stream_started_monotonic = 0.0
        self._last_stream_data_monotonic = 0.0
        self._stream_bytes_total = 0
        # Normalize legacy timelapse configs immediately; storage limits belong to the server.
        try:
            self._write_config()
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            LOG.warning("unable to normalize config: %s", exc)

    @property
    def settings(self) -> StreamSettings:
        with self._lock:
            return self._stream_settings

    @property
    def server_capture_settings(self) -> ServerCaptureSettings:
        with self._lock:
            return self._server_capture_settings

    @property
    def state(self) -> str:
        with self._lock:
            return self._stream_state

    @property
    def last_error(self) -> str:
        with self._lock:
            return self._stream_error

    @property
    def capture_request_state(self) -> str:
        with self._lock:
            return self._capture_request_state

    @property
    def capture_request_error(self) -> str:
        with self._lock:
            return self._capture_request_error

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

    def _set_capture_request_state(self, state: str, error: str = "") -> None:
        with self._lock:
            self._capture_request_state = state
            self._capture_request_error = error
        self._notify_status()

    def describe_target(self) -> str:
        return self.settings.target_url()

    def _is_stopping(self) -> bool:
        return self._stop_requested.is_set()

    def server_capture_request(self) -> dict[str, Any]:
        return {
            "state": self.capture_request_state,
            "error": self.capture_request_error,
            "enabled": self.server_capture_settings.enabled,
            "interval_seconds": self.server_capture_settings.interval_seconds,
        }

    def stream_health(self) -> dict[str, Any]:
        now = time.monotonic()
        with self._lock:
            started = self._stream_started_monotonic
            last_data = self._last_stream_data_monotonic
            return {
                "bytes_total": self._stream_bytes_total,
                "uptime_seconds": int(max(0.0, now - started)) if started else 0,
                "last_data_age_seconds": round(max(0.0, now - last_data), 1) if last_data else None,
                "publisher_connected": self._stream_state == "streaming" and bool(last_data),
            }

    def is_stalled(self) -> bool:
        health = self.stream_health()
        if self.state != "streaming" or health["uptime_seconds"] < 20:
            return False
        age = health["last_data_age_seconds"]
        return age is None or age > 15

    def update_settings(self, **changes: Any) -> bool:
        return self.update_configuration(stream_changes=changes)[0]

    def update_server_capture_settings(self, **changes: Any) -> None:
        self.update_configuration(capture_changes=changes)

    def update_configuration(self, stream_changes: dict[str, Any] | None = None,
                             capture_changes: dict[str, Any] | None = None) -> tuple[bool, bool]:
        """Validate and persist a whole command before changing runtime settings."""
        with self._lock:
            stream_data = asdict(self._stream_settings)
            capture_data = asdict(self._server_capture_settings)
            stream_data.update({name: value for name, value in (stream_changes or {}).items() if name in stream_data})
            capture_data.update({name: value for name, value in (capture_changes or {}).items() if name in capture_data})
            updated_stream = StreamSettings(**stream_data)
            updated_capture = ServerCaptureSettings(**capture_data)
            stream_changed = updated_stream != self._stream_settings
            capture_changed = updated_capture != self._server_capture_settings
            if not stream_changed and not capture_changed:
                return False, False
            self._write_config(stream=updated_stream, capture=updated_capture)
            self._stream_settings = updated_stream
            self._server_capture_settings = updated_capture
            if capture_changed:
                self._capture_request_state = "enabled" if updated_capture.enabled else "disabled"
                self._capture_request_error = ""
        self._notify_status()
        return stream_changed, capture_changed

    def enable_server_capture(self) -> None:
        self.update_server_capture_settings(enabled=True)
        self._set_capture_request_state("enabled")

    def disable_server_capture(self) -> None:
        self.update_server_capture_settings(enabled=False)
        self._set_capture_request_state("disabled")

    def _write_config(self, stream: StreamSettings | None = None, capture: ServerCaptureSettings | None = None) -> None:
        current = {}
        if self._config_path.exists():
            with self._config_path.open("r", encoding="utf-8") as handle:
                current = json.load(handle)

        if not isinstance(current, dict):
            raise ValueError("config must be a JSON object")
        updated_stream = asdict(stream or self._stream_settings)
        updated_capture = asdict(capture or self._server_capture_settings)
        if current.get("stream") == updated_stream and current.get("server_capture") == updated_capture and "timelapse" not in current:
            return
        current["stream"] = updated_stream
        current["server_capture"] = updated_capture
        current.pop("timelapse", None)
        # Replace only a fully flushed file, keeping broker credentials private.
        # An interrupted write must not truncate the configuration on the SD card.
        temporary_path = None
        try:
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=self._config_path.parent,
                                             prefix=f".{self._config_path.name}.", suffix=".tmp", delete=False) as handle:
                temporary_path = Path(handle.name)
                json.dump(current, handle, indent=2, allow_nan=False)
                handle.write("\n")
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(temporary_path, self._config_path)
        finally:
            if temporary_path is not None:
                temporary_path.unlink(missing_ok=True)

    def _build_capture_command(self, settings: StreamSettings | None = None) -> list[str]:
        settings = settings or self.settings
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
            "--intra",
            str(max(1, settings.framerate)),
            "-o",
            "-",
        ]
        if settings.inline_headers:
            command.append("--inline")
        if settings.nopreview:
            command.append("--nopreview")
        return command

    def _build_publish_command(self, settings: StreamSettings | None = None) -> list[str]:
        settings = settings or self.settings
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

    def _open_socket(self, settings: StreamSettings | None = None) -> socket.socket:
        settings = settings or self.settings
        sock = socket.create_connection((settings.host, settings.port), timeout=10)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return sock

    def _pump_stderr(self, process: subprocess.Popen[bytes], tag: str) -> None:
        assert process.stderr is not None
        try:
            # A malfunctioning child can emit indefinitely without a newline.
            # Continue draining during shutdown so a full stderr pipe cannot
            # prevent the child from exiting.
            while True:
                raw_line = process.stderr.readline(4096)
                if not raw_line:
                    break
                line = raw_line.decode("utf-8", errors="replace").rstrip()
                if line:
                    LOG.info("[%s] %s", tag, line)
        except (OSError, ValueError):
            if not self._is_stopping():
                LOG.exception("unable to read %s diagnostics", tag)

    def _write_publish_chunk(self, chunk: bytes, process: subprocess.Popen[bytes] | None = None) -> None:
        process = process or self._publish_process
        if process is None or process.stdin is None:
            raise RuntimeError("publish process is not available")
        descriptor = process.stdin.fileno()
        remaining = memoryview(chunk)

        while remaining:
            if self._is_stopping():
                raise InterruptedError("stream stop requested")
            if process.poll() is not None:
                raise BrokenPipeError(f"publisher exited with code {process.returncode}")

            _, writable, _ = select.select([], [descriptor], [], 0.5)
            if not writable:
                continue
            try:
                written = os.write(descriptor, remaining)
            except BlockingIOError:
                continue
            if written <= 0:
                raise BrokenPipeError("publisher pipe closed")
            remaining = remaining[written:]

    def _stop_after_failure(self, reason: str) -> None:
        self._stop_requested.set()
        self._set_stream_state("error", reason)
        # The supervisor owns process teardown. Joining this pump from a
        # concurrent stop() while it waits on teardown locks causes a deadlock.

    def _run_stream_pump(self, capture_process: subprocess.Popen[bytes],
                         publish_process: subprocess.Popen[bytes] | None,
                         stream_socket: socket.socket | None, mode: str) -> None:
        assert capture_process.stdout is not None
        capture_stdout = capture_process.stdout
        capture_descriptor = capture_stdout.fileno()

        try:
            os.set_blocking(capture_descriptor, False)
            if publish_process is not None and publish_process.stdin is not None:
                os.set_blocking(publish_process.stdin.fileno(), False)
            while not self._is_stopping():
                readable, _, _ = select.select([capture_descriptor], [], [], 0.5)
                if not readable:
                    if capture_process.poll() is not None:
                        break
                    continue
                try:
                    chunk = os.read(capture_descriptor, 64 * 1024)
                except BlockingIOError:
                    continue
                if not chunk:
                    break

                if mode == "tcp":
                    assert stream_socket is not None
                    stream_socket.sendall(chunk)
                else:
                    self._write_publish_chunk(chunk, publish_process)
                with self._lock:
                    self._last_stream_data_monotonic = time.monotonic()
                    self._stream_bytes_total += len(chunk)
        except (BrokenPipeError, ConnectionError, OSError, ValueError, RuntimeError) as exc:
            if self._is_stopping():
                LOG.info("stream pump stopped cleanly")
                return
            LOG.error("stream transport failed: %s", exc)
            self._stop_after_failure(f"stream transport: {exc}")
            return
        finally:
            if publish_process is not None and publish_process.stdin is not None:
                try:
                    publish_process.stdin.close()
                except (OSError, ValueError):
                    pass

        if self._is_stopping():
            return

        capture_code = capture_process.poll()
        publish_code = publish_process.poll() if publish_process is not None else 0
        if publish_code not in (None, 0):
            self._stop_after_failure(f"publisher exited with code {publish_code}")
            return
        if capture_code not in (None, 0):
            self._stop_after_failure(f"capture exited with code {capture_code}")
            return
        self._stop_after_failure("stream ended")

    def _pump_stdout(self, capture_process: subprocess.Popen[bytes],
                     publish_process: subprocess.Popen[bytes] | None,
                     stream_socket: socket.socket | None, mode: str) -> None:
        try:
            self._run_stream_pump(capture_process, publish_process, stream_socket, mode)
        finally:
            current_thread = threading.current_thread()
            with self._lock:
                if self._stdout_thread is current_thread:
                    self._stdout_thread = None

    def start(self) -> None:
        with self._lifecycle_lock:
            if self._capture_process is not None:
                if (self._capture_process.poll() is None and self.state == "streaming"
                        and not self._is_stopping()):
                    LOG.info("stream already running")
                    return
                raise RuntimeError("previous stream resources still require cleanup")
            if self._stdout_thread is not None:
                if self._stdout_thread.is_alive():
                    raise RuntimeError("previous stream pump is still stopping")
                self._stdout_thread = None

            settings = self.settings
            capture_command = self._build_capture_command(settings)
            self._stop_requested.clear()
            self._set_stream_state("starting")
            LOG.info("starting capture: %s", " ".join(capture_command))

            try:
                if settings.mode == "tcp":
                    LOG.info("opening tcp destination: %s", settings.target_url())
                    self._socket = self._open_socket(settings)
                elif settings.mode == "mediamtx_rtsp":
                    publish_command = self._build_publish_command(settings)
                    LOG.info("starting publisher: %s", " ".join(publish_command))
                    self._publish_process = subprocess.Popen(
                        publish_command,
                        stdin=subprocess.PIPE,
                        stdout=subprocess.DEVNULL,
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
                self._stop_requested.set()
                self._cleanup_stream_handles()
                self._close_stream_pipes()
                self._set_stream_state("error", str(exc))
                raise RuntimeError(f"failed to start stream: {exc}") from exc

            stdout_thread = threading.Thread(target=self._pump_stdout,
                                             args=(self._capture_process, self._publish_process, self._socket, settings.mode),
                                             name="stream-pump", daemon=True)
            stderr_threads: list[threading.Thread] = []

            if self._publish_process is not None:
                stderr_threads.append(
                    threading.Thread(target=self._pump_stderr, args=(self._publish_process, "ffmpeg"), daemon=True)
                )

            stderr_threads.append(
                threading.Thread(target=self._pump_stderr, args=(self._capture_process, "rpicam"), daemon=True)
            )

            self._stdout_thread = stdout_thread
            self._stderr_threads = stderr_threads
            with self._lock:
                self._stream_started_monotonic = time.monotonic()
                self._last_stream_data_monotonic = 0.0
            self._set_stream_state("streaming")
            stdout_thread.start()
            for thread in stderr_threads:
                thread.start()

    def _cleanup_stream_handles(self) -> None:
        if self._socket is not None:
            try:
                self._socket.shutdown(socket.SHUT_RDWR)
            except (OSError, ValueError):
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
                    try:
                        self._publish_process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        LOG.error("publisher process did not exit after SIGKILL")

        if self._capture_process is not None:
            if self._capture_process.poll() is None:
                self._capture_process.terminate()
                try:
                    self._capture_process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self._capture_process.kill()
                    try:
                        self._capture_process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        LOG.error("capture process did not exit after SIGKILL")

    def _close_stream_pipes(self) -> None:
        for process in (self._capture_process, self._publish_process):
            if process is not None:
                for pipe in (process.stdin, process.stdout, process.stderr):
                    if pipe is not None:
                        try:
                            pipe.close()
                        except (OSError, ValueError):
                            pass
        self._capture_process = None
        self._publish_process = None

    def stop(self) -> bool:
        with self._lifecycle_lock:
            self._stop_requested.set()
            stdout_thread = self._stdout_thread
            self._cleanup_stream_handles()
            # Never hold the status lock while waiting for child processes or
            # worker threads; both need it to report their final state.
            if stdout_thread is not None and stdout_thread is not threading.current_thread():
                stdout_thread.join(timeout=5)
                if stdout_thread.is_alive():
                    LOG.error("stream pump did not stop within 5 seconds")
                    self._set_stream_state("error", "previous stream pump did not stop")
                    return False
                with self._lock:
                    if self._stdout_thread is stdout_thread:
                        self._stdout_thread = None
            for thread in self._stderr_threads:
                thread.join(timeout=1)
            # Close every parent pipe only after its reader has unwound.
            if not any(thread.is_alive() for thread in self._stderr_threads):
                self._close_stream_pipes()
                self._stderr_threads = []
            else:
                self._set_stream_state("error", "previous diagnostics reader did not stop")
                return False
            with self._lock:
                if self._stream_state != "error":
                    self._stream_state = "stopped"
                    self._stream_error = ""
        self._notify_status()
        return True

    def restart(self) -> None:
        if not self.stop():
            raise RuntimeError("previous stream resources still require cleanup")
        time.sleep(0.5)
        self.start()


class StreamSupervisor:
    def __init__(self, streamer: CameraStreamer, retry_initial: float = 2.0, retry_max: float = 30.0) -> None:
        self._streamer = streamer
        self._retry_initial = retry_initial
        self._retry_max = retry_max
        self._retry_delay = retry_initial
        self._desired_running = False
        self._restart_requested = False
        self._reconnect_count = 0
        self._stop_event = threading.Event()
        self._wakeup = threading.Event()
        self._thread = threading.Thread(target=self._run, name="stream-supervisor", daemon=True)
        self._lock = threading.RLock()

    @property
    def desired_running(self) -> bool:
        with self._lock:
            return self._desired_running

    @property
    def reconnect_count(self) -> int:
        with self._lock:
            return self._reconnect_count

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
        if self._thread.is_alive():
            self._thread.join(timeout=35)
            if self._thread.is_alive():
                LOG.error("stream supervisor did not stop within 35 seconds")
        else:
            self._streamer.stop()

    def request_start(self) -> None:
        if self._stop_event.is_set():
            return
        with self._lock:
            self._desired_running = True
            self._retry_delay = self._retry_initial
        LOG.info("stream desired state set to running")
        self._wakeup.set()

    def request_stop(self) -> None:
        with self._lock:
            self._desired_running = False
            self._restart_requested = False
            self._retry_delay = self._retry_initial
        LOG.info("stream desired state set to stopped")
        self._wakeup.set()

    def request_restart(self) -> None:
        if self._stop_event.is_set():
            return
        with self._lock:
            self._desired_running = True
            self._restart_requested = True
            self._retry_delay = self._retry_initial
        LOG.info("stream restart requested")
        self._wakeup.set()

    def apply_stream_settings(self, changes: dict[str, Any]) -> None:
        self.apply_configuration(changes, {})

    def apply_configuration(self, stream_changes: dict[str, Any], capture_changes: dict[str, Any]) -> None:
        restart_required, _ = self._streamer.update_configuration(stream_changes, capture_changes)
        if restart_required and self.desired_running:
            LOG.info("stream settings changed, scheduling restart")
            with self._lock:
                self._retry_delay = self._retry_initial
                self._restart_requested = True
            self._wakeup.set()

    def _run(self) -> None:
        try:
            self._supervise()
        finally:
            self._streamer.stop()

    def _supervise(self) -> None:
        while not self._stop_event.is_set():
            self._wakeup.clear()
            with self._lock:
                desired = self._desired_running
                restart = self._restart_requested
                self._restart_requested = False
            if restart or (not desired and self._streamer.state != "stopped"):
                if not self._streamer.stop():
                    self._wakeup.wait(1.0)
                    continue
                self._streamer._set_stream_state("stopped")
            if self.desired_running and self._streamer.is_stalled():
                health = self._streamer.stream_health()
                LOG.warning(
                    "stream watchdog detected no data for %ss, restarting",
                    health["last_data_age_seconds"],
                )
                self._streamer._set_stream_state("error", "stream watchdog: no data")

            if self.desired_running and self._streamer.state in ("stopped", "error"):
                if self._streamer.state == "error":
                    if not self._streamer.stop():
                        self._wakeup.wait(1.0)
                        continue
                    with self._lock:
                        self._reconnect_count += 1
                        delay = self._retry_delay
                        self._retry_delay = min(self._retry_delay * 2.0, self._retry_max)
                    LOG.warning("stream failed, retrying in %.1fs", delay)
                    self._wakeup.wait(delay)
                    if self._stop_event.is_set() or not self.desired_running:
                        continue
                try:
                    self._streamer.start()
                    self._wakeup.wait(1.0)
                    continue
                except Exception as exc:
                    LOG.warning("stream start failed: %s", exc)
                    continue

            # A successful fork is not proof of a healthy stream. Reset the
            # backoff only once actual camera data has flowed for a while.
            health = self._streamer.stream_health()
            if health["publisher_connected"] and health["uptime_seconds"] >= 20:
                with self._lock:
                    self._retry_delay = self._retry_initial
            self._wakeup.wait(1.0)


class MqttController:
    MAX_COMMAND_BYTES = 8192

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
        self._client.max_queued_messages_set(64)
        self._client.max_inflight_messages_set(20)
        if mqtt_settings.username:
            self._client.username_pw_set(mqtt_settings.username, mqtt_settings.password)
        self._client.on_connect = self._on_connect
        self._client.on_message = self._on_message
        self._client.on_disconnect = self._on_disconnect
        self._connected = False
        self._commands: queue.Queue[tuple[str, bytes]] = queue.Queue(maxsize=32)
        self._stop_event = threading.Event()
        self._status_requested = threading.Event()
        self._worker = threading.Thread(target=self._run_commands, name="mqtt-commands", daemon=True)
        self._published_payloads: dict[str, str] = {}
        self._command_error = ""
        self._last_rejection_log = 0.0

    @property
    def base_topic(self) -> str:
        return self._mqtt_settings.base_topic.rstrip("/")

    def _topic(self, suffix: str) -> str:
        return f"{self.base_topic}/{suffix}"

    def _publish(self, suffix: str, payload: str, retain: bool = False) -> None:
        if not self._connected:
            return
        if retain and suffix != "status/pong" and self._published_payloads.get(suffix) == payload:
            return
        result = self._client.publish(self._topic(suffix), payload=payload, qos=1, retain=retain)
        if retain and result.rc == mqtt.MQTT_ERR_SUCCESS:
            self._published_payloads[suffix] = payload

    def publish_streamer_status(self) -> None:
        # Called by process threads and MQTT callbacks. Neither must do DNS,
        # disk I/O or acquire stream lifecycle locks to report a status change.
        self._status_requested.set()

    def _publish_status(self) -> None:
        if not self._connected:
            return
        stream_config = asdict(self._streamer.settings)
        capture_request = self._streamer.server_capture_request()
        stream_config.update(
            {
                "server_capture_enabled": capture_request["enabled"],
                "server_capture_interval_seconds": capture_request["interval_seconds"],
            }
        )
        health = self._streamer.stream_health()
        self._publish("status/state", self._streamer.state, retain=True)
        self._publish("status/error", self._command_error or self._streamer.last_error, retain=True)
        self._publish("status/config", json.dumps(stream_config), retain=True)
        self._publish("status/target", self._streamer.describe_target(), retain=True)
        self._publish("status/rtsp_url", self._streamer.describe_target(), retain=True)
        self._publish("status/ip", self._local_ip(), retain=True)
        self._publish("status/mdns", f"{socket.gethostname()}.local", retain=True)
        self._publish("status/desired", "running" if self._supervisor.desired_running else "stopped", retain=True)
        self._publish("status/publisher_connected", str(health["publisher_connected"]).lower(), retain=True)
        self._publish("status/stream_bytes_total", str(health["bytes_total"]), retain=True)
        self._publish("status/stream_uptime_seconds", str(health["uptime_seconds"]), retain=True)
        self._publish(
            "status/stream_last_data_age_seconds",
            "" if health["last_data_age_seconds"] is None else str(health["last_data_age_seconds"]),
            retain=True,
        )
        self._publish("status/reconnect_count", str(self._supervisor.reconnect_count), retain=True)
        self._publish("status/configured_fps", str(self._streamer.settings.framerate), retain=True)
        self._publish("status/frame_fps", str(self._streamer.settings.framerate if health["publisher_connected"] else 0), retain=True)
        self._publish("status/clients", "1" if health["publisher_connected"] else "0", retain=True)
        self._publish("status/direct_sessions", "0", retain=True)
        self._publish("status/direct_streaming_clients", "0", retain=True)
        self._publish("status/mqtt_connected", str(self._connected).lower(), retain=True)
        self._publish(
            "status/last_status",
            (
                f"runtime mqtt={'up' if self._connected else 'down'} "
                f"stream={self._streamer.state} publisher={'up' if health['publisher_connected'] else 'down'} "
                f"bytes={health['bytes_total']} reconnects={self._supervisor.reconnect_count}"
            ),
            retain=True,
        )

        self._publish("status/capture_request/state", capture_request["state"], retain=True)
        self._publish("status/capture_request/error", capture_request["error"], retain=True)
        self._publish("status/capture_request/enabled", str(capture_request["enabled"]).lower(), retain=True)
        self._publish("status/capture_request/interval_seconds", str(capture_request["interval_seconds"]), retain=True)

    def _local_ip(self) -> str:
        try:
            # Reuse the established MQTT connection. Resolving the broker's
            # hostname here could block every queued command during DNS loss.
            sock = self._client.socket()
            return str(sock.getsockname()[0]) if sock is not None else ""
        except (OSError, AttributeError):
            return ""

    def _on_connect(self, client: mqtt.Client, userdata: Any, flags: Any, reason_code: Any, properties: Any) -> None:
        try:
            connection_failed = bool(reason_code.is_failure)
        except AttributeError:
            connection_failed = int(reason_code) != 0
        if connection_failed:
            self._connected = False
            LOG.error("mqtt connection rejected: %s", reason_code)
            return

        LOG.info("mqtt connected: %s", reason_code)
        self._connected = True
        self._published_payloads.clear()
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
        self.publish_streamer_status()

    def _on_disconnect(
        self,
        client: mqtt.Client,
        userdata: Any,
        disconnect_flags: Any,
        reason_code: Any,
        properties: Any,
    ) -> None:
        self._connected = False
        LOG.warning("mqtt disconnected: %s", reason_code)

    def _handle_stream_set(self, payload: dict[str, Any]) -> None:
        # Broker access must not grant the ability to execute another program
        # as the camera service user. This option belongs in the local config.
        if "ffmpeg_path" in payload:
            raise ValueError("ffmpeg_path can only be changed in the local configuration")
        allowed = {field.name for field in fields(StreamSettings)} - {"ffmpeg_path"}
        changes = {key: value for key, value in payload.items() if key in allowed}
        timelapse_changes = {}
        aliases = {
            "server_capture_enabled": "enabled",
            "server_capture_interval_seconds": "interval_seconds",
            "timelapse_enabled": "enabled",
            "timelapse_interval_seconds": "interval_seconds",
        }
        for source, target in aliases.items():
            if source in payload:
                timelapse_changes[target] = payload[source]
        if not changes and not timelapse_changes:
            raise ValueError("no valid stream settings in payload")
        # One validated update and one atomic replacement for both groups;
        # a disk failure cannot apply just the stream half of a command.
        self._supervisor.apply_configuration(changes, timelapse_changes)

    def _handle_timelapse_set(self, payload: dict[str, Any]) -> None:
        allowed = {field.name for field in fields(ServerCaptureSettings)}
        changes = {key: value for key, value in payload.items() if key in allowed}
        if not changes:
            raise ValueError("no valid timelapse settings in payload")
        self._streamer.update_server_capture_settings(**changes)

    def _on_message(self, client: mqtt.Client, userdata: Any, message: mqtt.MQTTMessage) -> None:
        if self._stop_event.is_set():
            return
        try:
            if len(message.payload) > self.MAX_COMMAND_BYTES:
                raise ValueError("command payload exceeds 8192 bytes")
            self._commands.put_nowait((message.topic, bytes(message.payload)))
        except (queue.Full, ValueError) as exc:
            self._command_error = str(exc) or "command queue is full"
            self._status_requested.set()
            now = time.monotonic()
            if now - self._last_rejection_log >= 5:
                LOG.warning("mqtt command rejected: %s", self._command_error)
                self._last_rejection_log = now

    def _run_commands(self) -> None:
        while not self._stop_event.is_set():
            try:
                topic, payload = self._commands.get(timeout=0.2)
            except queue.Empty:
                pass
            else:
                try:
                    if not self._stop_event.is_set():
                        self._execute_command(topic, payload)
                finally:
                    self._commands.task_done()
            if self._status_requested.is_set() and not self._stop_event.is_set():
                self._status_requested.clear()
                try:
                    self._publish_status()
                except Exception:
                    LOG.exception("unable to publish camera status")

    def _execute_command(self, topic: str, payload_bytes: bytes) -> None:
        is_timelapse_command = topic.startswith(self._topic("cmd/timelapse/"))
        LOG.info("mqtt command topic=%s", topic)

        try:
            payload_text = payload_bytes.decode("utf-8").strip()
            self._command_error = ""
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
                self._streamer.enable_server_capture()
            elif topic == self._topic("cmd/timelapse/stop"):
                self._streamer.disable_server_capture()
            elif topic == self._topic("cmd/timelapse/set"):
                payload = json.loads(payload_text)
                if not isinstance(payload, dict):
                    raise ValueError("payload must be a JSON object")
                self._handle_timelapse_set(payload)
            else:
                LOG.warning("ignoring unknown topic: %s", topic)
        except Exception as exc:
            LOG.warning("mqtt command failed: %s", exc)
            self._command_error = str(exc)
            if is_timelapse_command:
                self._streamer._set_capture_request_state("error", str(exc))
            # A malformed command must not change a healthy stream to error;
            # that used to defeat its watchdog while capture kept running.
        finally:
            self.publish_streamer_status()

    def start(self) -> None:
        self._client.will_set(self._topic("status/online"), payload="false", qos=1, retain=True)
        self._client.connect_async(self._mqtt_settings.host, self._mqtt_settings.port, self._mqtt_settings.keepalive)
        self._worker.start()
        self._client.loop_start()

    def stop(self) -> None:
        self._stop_event.set()
        if self._worker.is_alive():
            self._worker.join(timeout=5)
        # A clean MQTT disconnect suppresses the will, so explicitly clear the
        # retained online state while the network loop can still deliver it.
        if self._connected:
            for suffix in ("status/mqtt_connected", "status/online"):
                try:
                    info = self._client.publish(self._topic(suffix), payload="false", qos=1, retain=True)
                    info.wait_for_publish(timeout=1)
                except (RuntimeError, ValueError):
                    LOG.warning("unable to publish %s on shutdown", suffix)
        self._client.disconnect()
        self._client.loop_stop()
        self._connected = False


def load_config(path: Path) -> tuple[StreamSettings, ServerCaptureSettings, MqttSettings]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)

    capture_data = dict(data.get("server_capture", data.get("timelapse", {})))
    allowed_capture_fields = {field.name for field in fields(ServerCaptureSettings)}
    capture_data = {key: value for key, value in capture_data.items() if key in allowed_capture_fields}
    return (
        StreamSettings(**data["stream"]),
        ServerCaptureSettings(**capture_data),
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
    stream_settings, server_capture_settings, mqtt_settings = load_config(config_path)
    streamer = CameraStreamer(stream_settings, server_capture_settings, config_path)
    supervisor = StreamSupervisor(streamer)
    controller = MqttController(mqtt_settings, streamer, supervisor)
    streamer.set_status_listener(controller.publish_streamer_status)
    shutdown = threading.Event()

    def request_shutdown(signum: int, frame: Any) -> None:
        shutdown.set()

    signal.signal(signal.SIGTERM, request_shutdown)
    signal.signal(signal.SIGINT, request_shutdown)

    try:
        controller.start()
        supervisor.start(autostart=args.autostart)
        next_status_at = 0.0
        while not shutdown.wait(1):
            now = time.monotonic()
            if now >= next_status_at:
                controller.publish_streamer_status()
                next_status_at = now + 10.0
    except KeyboardInterrupt:
        LOG.info("shutting down")
    finally:
        controller.stop()
        supervisor.stop()


if __name__ == "__main__":
    main()
