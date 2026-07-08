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
class MqttSettings:
    host: str
    port: int
    username: str
    password: str
    client_id: str
    base_topic: str
    keepalive: int = 60


class CameraStreamer:
    def __init__(self, stream: StreamSettings, config_path: Path) -> None:
        self._settings = stream
        self._config_path = config_path
        self._capture_process: subprocess.Popen[bytes] | None = None
        self._ffmpeg_process: subprocess.Popen[bytes] | None = None
        self._socket: socket.socket | None = None
        self._stdout_thread: threading.Thread | None = None
        self._stderr_threads: list[threading.Thread] = []
        self._stop_requested = threading.Event()
        self._lock = threading.RLock()
        self._last_error = ""
        self._state = "stopped"
        self._status_listener: StatusListener | None = None

    @property
    def settings(self) -> StreamSettings:
        with self._lock:
            return self._settings

    @property
    def state(self) -> str:
        with self._lock:
            return self._state

    @property
    def last_error(self) -> str:
        with self._lock:
            return self._last_error

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

    def _set_state(self, state: str, error: str = "") -> None:
        with self._lock:
            self._state = state
            self._last_error = error
        self._notify_status()

    def describe_target(self) -> str:
        return self.settings.target_url()

    def _is_stopping(self) -> bool:
        return self._stop_requested.is_set()

    def update_settings(self, **changes: Any) -> bool:
        restart_required = False
        with self._lock:
            data = asdict(self._settings)
            for name in data:
                if name in changes:
                    value = changes[name]
                    if value != data[name]:
                        data[name] = value
                        restart_required = True
            self._settings = StreamSettings(**data)
            self._write_config()
        self._notify_status()
        return restart_required

    def _write_config(self) -> None:
        if not self._config_path:
            return

        current = {}
        if self._config_path.exists():
            with self._config_path.open("r", encoding="utf-8") as handle:
                current = json.load(handle)

        current["stream"] = asdict(self._settings)
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

    def _build_ffmpeg_command(self) -> list[str]:
        settings = self.settings
        return [
            settings.ffmpeg_path,
            "-loglevel",
            "warning",
            "-fflags",
            "nobuffer",
            "-flags",
            "low_delay",
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
            if self._stop_requested.is_set():
                break

    def _stop_after_failure(self, reason: str) -> None:
        self._set_state("error", reason)
        self.stop()

    def _pump_stdout_to_tcp(self) -> None:
        assert self._capture_process is not None
        assert self._capture_process.stdout is not None
        assert self._socket is not None

        capture_stdout = self._capture_process.stdout
        sock = self._socket

        try:
            while not self._is_stopping():
                chunk = capture_stdout.read(64 * 1024)
                if not chunk:
                    break
                sock.sendall(chunk)
        except (BrokenPipeError, ConnectionError, OSError, ValueError) as exc:
            if self._is_stopping():
                LOG.info("tcp transport stopped cleanly")
                return
            LOG.error("tcp transport failed: %s", exc)
            self._stop_after_failure(f"tcp transport: {exc}")
            return

        if self._is_stopping():
            return

        if not self._is_stopping():
            code = self._capture_process.poll()
            reason = f"capture exited with code {code}" if code not in (None, 0) else "tcp stream ended"
            self._stop_after_failure(reason)
            return

    def _pump_stdout_to_rtsp(self) -> None:
        assert self._capture_process is not None
        assert self._capture_process.stdout is not None
        assert self._ffmpeg_process is not None
        assert self._ffmpeg_process.stdin is not None

        capture_stdout = self._capture_process.stdout
        ffmpeg_process = self._ffmpeg_process
        ffmpeg_stdin = ffmpeg_process.stdin

        try:
            while not self._is_stopping():
                chunk = capture_stdout.read(64 * 1024)
                if not chunk:
                    break
                ffmpeg_stdin.write(chunk)
                ffmpeg_stdin.flush()
        except (BrokenPipeError, OSError, ValueError) as exc:
            if self._is_stopping():
                LOG.info("rtsp publisher stopped cleanly")
                return
            LOG.error("ffmpeg transport failed: %s", exc)
            self._stop_after_failure(f"ffmpeg transport: {exc}")
            return
        finally:
            try:
                ffmpeg_stdin.close()
            except (OSError, ValueError):
                pass

        if self._is_stopping():
            return

        if not self._is_stopping():
            capture_code = self._capture_process.poll()
            ffmpeg_code = ffmpeg_process.poll()
            if ffmpeg_code not in (None, 0):
                self._stop_after_failure(f"ffmpeg exited with code {ffmpeg_code}")
                return
            if capture_code not in (None, 0):
                self._stop_after_failure(f"capture exited with code {capture_code}")
                return
            self._stop_after_failure("rtsp stream ended")
            return

    def start(self) -> None:
        with self._lock:
            if self._capture_process is not None:
                LOG.info("stream already running")
                return

            capture_command = self._build_capture_command()
            settings = self._settings
            self._stop_requested.clear()
            self._set_state("starting")
            LOG.info("starting capture: %s", " ".join(capture_command))

            try:
                if settings.mode == "tcp":
                    LOG.info("opening tcp destination: %s", settings.target_url())
                    self._socket = self._open_socket()
                elif settings.mode == "mediamtx_rtsp":
                    ffmpeg_command = self._build_ffmpeg_command()
                    LOG.info("starting publisher: %s", " ".join(ffmpeg_command))
                    self._ffmpeg_process = subprocess.Popen(
                        ffmpeg_command,
                        stdin=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        bufsize=0,
                    )
                else:
                    raise RuntimeError(f"unsupported stream mode: {settings.mode}")

                self._capture_process = subprocess.Popen(
                    capture_command,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    bufsize=0,
                )
            except Exception as exc:
                self._cleanup_handles()
                self._set_state("error", str(exc))
                raise RuntimeError(f"failed to start stream: {exc}") from exc

            if settings.mode == "tcp":
                self._stdout_thread = threading.Thread(target=self._pump_stdout_to_tcp, daemon=True)
            else:
                self._stdout_thread = threading.Thread(target=self._pump_stdout_to_rtsp, daemon=True)

            self._stderr_threads = [
                threading.Thread(target=self._pump_stderr, args=(self._capture_process, "rpicam"), daemon=True)
            ]
            if self._ffmpeg_process is not None:
                self._stderr_threads.append(
                    threading.Thread(target=self._pump_stderr, args=(self._ffmpeg_process, "ffmpeg"), daemon=True)
                )

            self._stdout_thread.start()
            for thread in self._stderr_threads:
                thread.start()
            self._set_state("running")

    def _cleanup_handles(self) -> None:
        if self._socket is not None:
            try:
                self._socket.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self._socket.close()
            self._socket = None

        if self._ffmpeg_process is not None:
            try:
                if self._ffmpeg_process.stdin is not None:
                    self._ffmpeg_process.stdin.close()
            except OSError:
                pass
            if self._ffmpeg_process.poll() is None:
                self._ffmpeg_process.terminate()
                try:
                    self._ffmpeg_process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self._ffmpeg_process.kill()
            self._ffmpeg_process = None

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
            previous_state = self._state
            self._cleanup_handles()
            if previous_state != "error":
                self._state = "stopped"
                self._last_error = ""
        self._notify_status()

    def restart(self) -> None:
        self.stop()
        time.sleep(0.5)
        self.start()


class MqttController:
    def __init__(self, mqtt_settings: MqttSettings, streamer: CameraStreamer) -> None:
        self._mqtt_settings = mqtt_settings
        self._streamer = streamer
        self._client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=mqtt_settings.client_id,
            protocol=mqtt.MQTTv311,
        )
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

    def _on_connect(self, client: mqtt.Client, userdata: Any, flags: Any, reason_code: Any, properties: Any) -> None:
        LOG.info("mqtt connected: %s", reason_code)
        for suffix in ("cmd/start", "cmd/stop", "cmd/restart", "cmd/set", "cmd/ping"):
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

    def _handle_set(self, payload: dict[str, Any]) -> None:
        allowed = {field.name for field in fields(StreamSettings)}
        changes = {key: value for key, value in payload.items() if key in allowed}
        if not changes:
            raise ValueError("no valid stream settings in payload")
        restart_required = self._streamer.update_settings(**changes)
        if restart_required and self._streamer.state == "running":
            self._streamer.restart()
        self._publish_status()

    def _on_message(self, client: mqtt.Client, userdata: Any, message: mqtt.MQTTMessage) -> None:
        topic = message.topic
        payload_text = message.payload.decode("utf-8", errors="replace").strip()
        LOG.info("mqtt message topic=%s payload=%s", topic, payload_text)

        try:
            if topic == self._topic("cmd/start"):
                self._streamer.start()
            elif topic == self._topic("cmd/stop"):
                self._streamer.stop()
            elif topic == self._topic("cmd/restart"):
                self._streamer.restart()
            elif topic == self._topic("cmd/ping"):
                self._publish("status/pong", payload_text or "pong", retain=True)
            elif topic == self._topic("cmd/set"):
                payload = json.loads(payload_text)
                if not isinstance(payload, dict):
                    raise ValueError("payload must be a JSON object")
                self._handle_set(payload)
            else:
                LOG.warning("ignoring unknown topic: %s", topic)
        except Exception as exc:
            LOG.exception("mqtt command failed")
            self._streamer._set_state("error", str(exc))
        finally:
            self._publish_status()

    def start(self) -> None:
        self._client.will_set(self._topic("status/online"), payload="false", qos=1, retain=True)
        self._client.connect(self._mqtt_settings.host, self._mqtt_settings.port, self._mqtt_settings.keepalive)
        self._client.loop_start()

    def stop(self) -> None:
        self._client.loop_stop()
        self._client.disconnect()


def load_config(path: Path) -> tuple[StreamSettings, MqttSettings]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    return StreamSettings(**data["stream"]), MqttSettings(**data["mqtt"])


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
    stream_settings, mqtt_settings = load_config(config_path)
    streamer = CameraStreamer(stream_settings, config_path)
    controller = MqttController(mqtt_settings, streamer)
    streamer.set_status_listener(controller.publish_streamer_status)

    try:
        controller.start()
        if args.autostart:
            streamer.start()
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        LOG.info("shutting down")
    finally:
        controller.stop()
        streamer.stop()


if __name__ == "__main__":
    main()
