"""Host-side regression tests; no camera, MQTT broker or live config is used.

Run with: python -m unittest discover -s test -p "test_pi_*.py" -v
Install pi_streamer/requirements.txt into the test environment first.
"""

import io
import json
import os
from pathlib import Path
import tempfile
import threading
import time
import unittest
from dataclasses import FrozenInstanceError, asdict, replace
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

from pi_streamer import pi_cam_controller as camera


def stream_settings(**changes):
    return camera.StreamSettings(**{
        "host": "127.0.0.1", "port": 8554, "width": 1280, "height": 720,
        "framerate": 20, "bitrate": 2500000, "sharpness": 1.0,
        "brightness": 0.0, "contrast": 1.0, "saturation": 1.0, **changes,
    })


class StreamSettingsTests(unittest.TestCase):
    def test_invalid_settings_are_rejected(self):
        for changes in (
            {"width": 1279}, {"framerate": 0}, {"framerate": 3.5},
            {"port": 65536}, {"bitrate": True}, {"brightness": float("nan")},
            {"contrast": float("inf")}, {"mode": "shell"}, {"codec": "mjpeg"},
            {"host": "host/name"}, {"host": "user@host"}, {"path": "name\n"},
            {"host": "localhost\x00"}, {"path": "camera\x00"}, {"path": "camera\x1b"},
            {"inline_headers": "maybe"},
        ):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                stream_settings(**changes)

    def test_legacy_string_values_are_normalized_without_truthiness_bug(self):
        settings = stream_settings(framerate="15", nopreview="false")
        self.assertEqual(settings.framerate, 15)
        self.assertIs(settings.nopreview, False)
        capture = camera.ServerCaptureSettings(enabled="false", interval_seconds="60")
        self.assertIs(capture.enabled, False)
        self.assertEqual(capture.interval_seconds, 60)

    def test_settings_cannot_be_mutated_bypassing_validation(self):
        with self.assertRaises(FrozenInstanceError):
            stream_settings().width = 1

    def test_ipv6_url_has_brackets(self):
        self.assertEqual(stream_settings(host="::1").target_url(), "rtsp://[::1]:8554/pi-zero-01")


class StreamerFixture:
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "config.json"
        self.original = {
            "stream": asdict(stream_settings()),
            "server_capture": asdict(camera.ServerCaptureSettings()),
            "mqtt": {"host": "example.invalid", "username": "test", "password": "test-secret"},
        }
        self.path.write_text(json.dumps(self.original), encoding="utf-8")
        self.streamer = camera.CameraStreamer(stream_settings(), camera.ServerCaptureSettings(), self.path)


class StreamerTests(StreamerFixture, unittest.TestCase):
    def test_unchanged_settings_and_startup_do_not_rewrite_sd_card(self):
        with patch.object(camera.os, "replace") as replace_file:
            other = camera.CameraStreamer(stream_settings(), camera.ServerCaptureSettings(), self.path)
            self.assertFalse(other.update_settings(framerate="20"))
            other.update_server_capture_settings(enabled="false")
            replace_file.assert_not_called()

    def test_failed_atomic_replace_preserves_file_and_runtime_settings(self):
        before = self.path.read_bytes()
        with patch.object(camera.os, "replace", side_effect=OSError("disk full")):
            with self.assertRaises(OSError):
                self.streamer.update_settings(framerate=10)
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(self.streamer.settings.framerate, 20)
        self.assertEqual(list(self.path.parent.glob("*.tmp")), [])

    def test_update_preserves_mqtt_credentials_and_updates_capture_state(self):
        self.streamer.update_server_capture_settings(enabled="true", interval_seconds=120)
        data = json.loads(self.path.read_text(encoding="utf-8"))
        self.assertEqual(data["mqtt"], self.original["mqtt"])
        self.assertEqual(data["server_capture"], {"enabled": True, "interval_seconds": 120})
        self.assertEqual(self.streamer.capture_request_state, "enabled")

    def test_invalid_setting_never_reaches_disk(self):
        before = self.path.read_bytes()
        with self.assertRaises(ValueError):
            self.streamer.update_server_capture_settings(interval_seconds=-1)
        self.assertEqual(self.path.read_bytes(), before)

    def test_legacy_capture_config_is_migrated_once(self):
        legacy = dict(self.original)
        legacy.pop("server_capture")
        legacy["timelapse"] = {"enabled": True, "interval_seconds": 90, "max_size_mb": 50}
        self.path.write_text(json.dumps(legacy), encoding="utf-8")
        # load_config also needs complete MQTT settings; only migration is under
        # test here, so use the already normalized settings directly.
        camera.CameraStreamer(stream_settings(), camera.ServerCaptureSettings(True, 90), self.path)
        result = json.loads(self.path.read_text(encoding="utf-8"))
        self.assertNotIn("timelapse", result)
        self.assertEqual(result["server_capture"], {"enabled": True, "interval_seconds": 90})

    def test_stop_does_not_hold_status_lock_while_waiting_for_process(self):
        lock_available = threading.Event()
        reader_threads = []

        def wait_for_child(timeout):
            def report_status():
                self.streamer.stream_health()
                lock_available.set()
            reader = threading.Thread(target=report_status)
            reader_threads.append(reader)
            reader.start()
            self.assertTrue(lock_available.wait(1), "shutdown held the status lock while waiting")
            return 0

        process = MagicMock()
        process.poll.return_value = None
        process.wait.side_effect = wait_for_child
        process.stdin = io.BytesIO()
        process.stdout = io.BytesIO()
        process.stderr = io.BytesIO()
        self.streamer._capture_process = process
        self.streamer._stream_state = "streaming"
        self.streamer.stop()
        for reader in reader_threads:
            reader.join(1)
        self.assertTrue(process.stdout.closed)
        self.assertTrue(process.stderr.closed)
        self.assertIsNone(self.streamer._capture_process)
        self.assertEqual(self.streamer.state, "stopped")

    def test_capture_start_failure_closes_publisher_pipes(self):
        publisher = MagicMock()
        publisher.poll.return_value = 0
        publisher.stdin = io.BytesIO()
        publisher.stdout = None
        publisher.stderr = io.BytesIO()
        with patch.object(camera.subprocess, "Popen", side_effect=[publisher, FileNotFoundError("rpicam missing")]):
            with self.assertRaises(RuntimeError):
                self.streamer.start()
        self.assertTrue(publisher.stdin.closed)
        self.assertTrue(publisher.stderr.closed)
        self.assertIsNone(self.streamer._publish_process)
        self.assertEqual(self.streamer.state, "error")

    def test_stderr_reader_keeps_draining_during_shutdown_with_bounded_lines(self):
        self.streamer._stop_requested.set()
        process = SimpleNamespace(stderr=io.BytesIO(b"x" * 12000 + b"\nlast\n"))
        with patch.object(camera.LOG, "info") as log:
            self.streamer._pump_stderr(process, "test")
        self.assertEqual(process.stderr.tell(), 12006)
        self.assertTrue(all(len(call.args[2]) <= 4096 for call in log.call_args_list))

    def test_incomplete_teardown_is_not_mistaken_for_a_running_stream(self):
        process = MagicMock()
        process.poll.return_value = 0
        process.stdin = io.BytesIO()
        process.stdout = io.BytesIO()
        process.stderr = io.BytesIO()
        reader = MagicMock()
        reader.is_alive.return_value = True
        self.streamer._capture_process = process
        self.streamer._stderr_threads = [reader]
        self.streamer._stream_state = "streaming"
        self.assertFalse(self.streamer.stop())
        self.assertEqual(self.streamer.state, "error")
        self.assertIs(self.streamer._capture_process, process)
        with patch.object(camera.subprocess, "Popen") as spawn:
            with self.assertRaisesRegex(RuntimeError, "require cleanup"):
                self.streamer.start()
            spawn.assert_not_called()
        reader.is_alive.return_value = False
        self.assertTrue(self.streamer.stop())
        self.assertIsNone(self.streamer._capture_process)
        self.assertTrue(process.stdout.closed)

    @unittest.skipIf(os.name == "nt", "select() on pipes is a POSIX runtime feature")
    def test_eof_reports_error_without_accessing_cleared_global_handles(self):
        read_fd, write_fd = os.pipe()
        os.close(write_fd)
        with os.fdopen(read_fd, "rb", buffering=0) as output:
            process = SimpleNamespace(stdout=output, poll=lambda: 0)
            self.streamer._capture_process = None
            self.streamer._run_stream_pump(process, None, None, "tcp")
        self.assertEqual(self.streamer.state, "error")
        self.assertEqual(self.streamer.last_error, "stream ended")


class MqttTests(StreamerFixture, unittest.TestCase):
    def setUp(self):
        super().setUp()
        self.supervisor = camera.StreamSupervisor(self.streamer)
        self.client = MagicMock()
        self.client.publish.return_value.rc = camera.mqtt.MQTT_ERR_SUCCESS
        settings = camera.MqttSettings("example.invalid", 1883, "test", "test", "test-camera", "camera/test")
        with patch.object(camera.mqtt, "Client", return_value=self.client):
            self.controller = camera.MqttController(settings, self.streamer, self.supervisor)

    def test_callback_queues_command_without_running_it_or_touching_disk(self):
        with patch.object(self.controller, "_execute_command") as execute, patch.object(self.streamer, "_write_config") as save:
            self.controller._on_message(self.client, None, SimpleNamespace(topic="camera/test/cmd/set", payload=b'{"framerate":10}'))
            execute.assert_not_called()
            save.assert_not_called()
            self.assertEqual(self.controller._commands.qsize(), 1)

    def test_invalid_command_does_not_break_stream_watchdog_state(self):
        self.streamer._stream_state = "streaming"
        self.controller._execute_command("camera/test/cmd/set", b'{"framerate":0}')
        self.assertEqual(self.streamer.state, "streaming")
        self.assertEqual(self.streamer.settings.framerate, 20)
        self.assertIn("framerate", self.controller._command_error)

    def test_remote_executable_change_is_rejected_before_other_changes(self):
        self.controller._execute_command("camera/test/cmd/set", b'{"ffmpeg_path":"/tmp/program","framerate":10}')
        self.assertEqual(self.streamer.settings.ffmpeg_path, "ffmpeg")
        self.assertEqual(self.streamer.settings.framerate, 20)
        self.assertIn("local configuration", self.controller._command_error)

    def test_false_capture_alias_is_false_and_entire_command_is_validated_first(self):
        self.streamer.enable_server_capture()
        self.controller._execute_command("camera/test/cmd/set", b'{"server_capture_enabled":"false"}')
        self.assertFalse(self.streamer.server_capture_settings.enabled)
        self.controller._execute_command("camera/test/cmd/set", b'{"framerate":10,"server_capture_interval_seconds":-1}')
        self.assertEqual(self.streamer.settings.framerate, 20)

    def test_combined_setting_command_replaces_config_once_before_restart(self):
        self.supervisor.request_start()
        with patch.object(camera.os, "replace", wraps=camera.os.replace) as replace_file:
            self.controller._execute_command(
                "camera/test/cmd/set", b'{"framerate":10,"server_capture_enabled":true}')
            replace_file.assert_called_once()
        self.assertEqual(self.controller._command_error, "")
        self.assertEqual(self.streamer.settings.framerate, 10)
        self.assertTrue(self.streamer.server_capture_settings.enabled)
        self.assertTrue(self.supervisor._restart_requested)
        stored = json.loads(self.path.read_text(encoding="utf-8"))
        self.assertEqual(stored["stream"]["framerate"], 10)
        self.assertTrue(stored["server_capture"]["enabled"])

    def test_combined_setting_write_failure_changes_neither_group_or_restart_state(self):
        self.supervisor.request_start()
        before = self.path.read_bytes()
        with patch.object(camera.os, "replace", side_effect=OSError("disk full")):
            self.controller._execute_command(
                "camera/test/cmd/set", b'{"framerate":10,"server_capture_enabled":true}')
        self.assertIn("disk full", self.controller._command_error)
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(self.streamer.settings.framerate, 20)
        self.assertFalse(self.streamer.server_capture_settings.enabled)
        self.assertFalse(self.supervisor._restart_requested)

    def test_timelapse_enabled_can_be_updated_without_interval(self):
        self.controller._execute_command("camera/test/cmd/timelapse/set", b'{"enabled":true}')
        self.assertTrue(self.streamer.server_capture_settings.enabled)

    def test_pending_commands_and_payload_memory_are_bounded(self):
        message = SimpleNamespace(topic="camera/test/cmd/ping", payload=b"ping")
        for _ in range(100):
            self.controller._on_message(self.client, None, message)
        self.assertEqual(self.controller._commands.qsize(), 32)
        other = camera.MqttController(camera.MqttSettings("example.invalid", 1883, "", "", "test", "camera/test"), self.streamer, self.supervisor)
        other._on_message(self.client, None, SimpleNamespace(topic=message.topic, payload=b"x" * 8193))
        self.assertTrue(other._commands.empty())

    def test_offline_status_does_not_queue_messages_or_resolve_dns(self):
        with patch.object(self.controller, "_local_ip") as local_ip:
            for _ in range(100):
                self.controller._publish_status()
            local_ip.assert_not_called()
        self.client.publish.assert_not_called()

    def test_ip_status_reuses_mqtt_socket_without_another_connection_or_dns(self):
        self.client.socket.return_value = SimpleNamespace(getsockname=lambda: ("192.0.2.10", 12345))
        with patch.object(camera.socket, "socket") as new_socket:
            self.assertEqual(self.controller._local_ip(), "192.0.2.10")
            new_socket.assert_not_called()
        self.client.socket.return_value = None
        self.assertEqual(self.controller._local_ip(), "")
        self.client.socket.return_value = SimpleNamespace(getsockname=MagicMock(side_effect=OSError("closed")))
        self.assertEqual(self.controller._local_ip(), "")

    def test_unchanged_status_is_coalesced_but_pings_always_reply(self):
        self.controller._connected = True
        self.controller._publish("status/state", "streaming", retain=True)
        self.controller._publish("status/state", "streaming", retain=True)
        self.assertEqual(self.client.publish.call_count, 1)
        self.controller._publish("status/pong", "pong", retain=True)
        self.controller._publish("status/pong", "pong", retain=True)
        self.assertEqual(self.client.publish.call_count, 3)

    def test_connect_callback_never_resolves_dns_or_runs_full_status(self):
        with patch.object(self.controller, "_publish_status") as status:
            self.controller._on_connect(self.client, None, None, 0, None)
        status.assert_not_called()
        self.assertTrue(self.controller._status_requested.is_set())
        self.assertEqual(self.client.subscribe.call_count, 8)

    def test_graceful_disconnect_marks_device_offline_before_disconnect(self):
        self.controller._connected = True
        self.controller.stop()
        topics = [call.args[0] for call in self.client.publish.call_args_list]
        self.assertEqual(topics, ["camera/test/status/mqtt_connected", "camera/test/status/online"])
        self.client.publish.return_value.wait_for_publish.assert_called_with(timeout=1)
        self.client.disconnect.assert_called_once()


class SupervisorTests(unittest.TestCase):
    def test_failed_stop_keeps_error_state_and_prevents_restart_until_cleanup_finishes(self):
        for desired in (False, True):
            with self.subTest(desired_running=desired):
                streamer = MagicMock()
                streamer.state = "error"
                streamer.is_stalled.return_value = False
                streamer.stop.return_value = False
                supervisor = camera.StreamSupervisor(streamer)
                supervisor._desired_running = desired
                supervisor._wakeup.wait = MagicMock(side_effect=lambda _: supervisor._stop_event.set())
                supervisor._supervise()
                streamer.stop.assert_called_once()
                streamer._set_stream_state.assert_not_called()
                streamer.start.assert_not_called()

    def test_commands_only_signal_supervisor_and_do_not_stop_in_mqtt_thread(self):
        streamer = MagicMock()
        supervisor = camera.StreamSupervisor(streamer)
        supervisor.request_restart()
        self.assertTrue(supervisor.desired_running)
        supervisor.request_stop()
        self.assertFalse(supervisor.desired_running)
        streamer.stop.assert_not_called()

    def test_shutdown_wins_a_race_with_blocked_start(self):
        entered = threading.Event()
        release = threading.Event()
        stopped = threading.Event()
        streamer = MagicMock()
        streamer.state = "stopped"
        streamer.is_stalled.return_value = False

        def start_stream():
            entered.set()
            release.wait(2)
            streamer.state = "streaming"

        streamer.start.side_effect = start_stream
        streamer.stop.side_effect = stopped.set
        supervisor = camera.StreamSupervisor(streamer)
        supervisor.start(autostart=True)
        try:
            self.assertTrue(entered.wait(1))
            terminator = threading.Thread(target=supervisor.stop)
            terminator.start()
            release.set()
            terminator.join(3)
            self.assertFalse(terminator.is_alive())
            self.assertTrue(stopped.is_set())
            self.assertFalse(supervisor.desired_running)
            supervisor.request_restart()
            self.assertFalse(supervisor.desired_running)
        finally:
            release.set()
            supervisor.stop()


if __name__ == "__main__":
    unittest.main()
