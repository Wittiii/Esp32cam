import io
import logging
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from pi_streamer import collect_diagnostics as collector
from pi_streamer.local_diagnostics import LocalLogHandler, log_thread_stacks


class DiagnosticsTests(unittest.TestCase):
    def test_startup_exception_is_saved_by_direct_script_entrypoint(self):
        script = Path(__file__).resolve().parents[1] / "pi_streamer" / "pi_cam_controller.py"
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            result = subprocess.run([sys.executable, str(script), "--config", str(base / "missing.json")],
                                    capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            text = (base / "logs" / "controller.log").read_text()
            self.assertIn("Unhandled application exception", text)
            self.assertIn("FileNotFoundError", text)

    def test_rotation_keeps_recent_records_and_limited_backups(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "controller.log"
            handler = LocalLogHandler(path, maxBytes=100, backupCount=4, encoding="utf-8")
            try:
                for number in range(80):
                    handler.handle(logging.LogRecord("test", logging.INFO, "", 0,
                                                     f"record {number:03d} " + "x" * 40, (), None))
            finally:
                handler.close()
            self.assertEqual(len(list(Path(directory).iterdir())), 5)
            self.assertIn("record 079", path.read_text())
            self.assertNotIn("record 000", path.read_text())

    def test_write_failure_disables_file_handler_without_raising(self):
        with tempfile.TemporaryDirectory() as directory:
            handler = LocalLogHandler(Path(directory) / "controller.log")
            record = logging.LogRecord("test", logging.ERROR, "", 0, "failure", (), None)
            try:
                with patch.object(handler, "shouldRollover", side_effect=OSError("disk full")) as rollover:
                    with patch("sys.stderr", new_callable=io.StringIO) as stderr:
                        handler.handle(record)
                        handler.handle(record)
                        self.assertEqual(rollover.call_count, 1)
                        self.assertIn("disabled", stderr.getvalue())
            finally:
                handler.close()

    def test_collector_only_reads_named_logs_and_bounds_output(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / "controller.log").write_bytes(b"a" * (collector.TAIL_BYTES + 10) + b"recent")
            (base / "config.json").write_text('"password": "DO_NOT_EXPORT"')
            output = base / "report.txt"
            with patch.object(collector, "COMMANDS", ()):
                collector.collect(base, output)
            text = output.read_text()
            self.assertIn("recent", text)
            self.assertIn("earlier output omitted", text)
            self.assertNotIn("DO_NOT_EXPORT", text)
            self.assertLess(output.stat().st_size, collector.TAIL_BYTES + 3000)
            with self.assertRaises(FileExistsError):
                collector.collect(base, output)

    def test_missing_or_hung_system_command_is_reported(self):
        for error in (FileNotFoundError("missing"), subprocess.TimeoutExpired("journalctl", 10)):
            with patch.object(collector.subprocess, "run", side_effect=error):
                self.assertIn("Unavailable or timed out", collector.command_output(("journalctl",)))

    def test_thread_dump_contains_stack_without_local_values(self):
        secret_local = "DO_NOT_EXPORT_LOCAL_VALUE"
        with self.assertLogs("pi-cam-controller", level="ERROR") as captured:
            log_thread_stacks()
        self.assertIn("MainThread", "\n".join(captured.output))
        self.assertNotIn(secret_local, "\n".join(captured.output))


if __name__ == "__main__":
    unittest.main()
