from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from pi_streamer import wifi_watchdog as watchdog


class WifiWatchdogTests(unittest.TestCase):
    def test_reboots_only_once_per_outage_and_rearms_after_stable_recovery(self):
        state = watchdog.new_state("boot-a")
        state, reboot = watchdog.evaluate(state, "boot-a", 100, False)
        self.assertFalse(reboot)
        state, reboot = watchdog.evaluate(state, "boot-a", 3699, False)
        self.assertFalse(reboot)
        state, reboot = watchdog.evaluate(state, "boot-a", 3700, False)
        self.assertTrue(reboot)

        # The persisted latch survives a boot with a different monotonic epoch.
        state, reboot = watchdog.evaluate(state, "boot-b", 5, False)
        self.assertFalse(reboot)
        state, reboot = watchdog.evaluate(state, "boot-b", 5000, False)
        self.assertFalse(reboot)
        state, reboot = watchdog.evaluate(state, "boot-b", 5010, True)
        self.assertFalse(reboot)
        state, reboot = watchdog.evaluate(state, "boot-b", 5609, True)
        self.assertFalse(reboot)
        self.assertTrue(state["reboot_latched"])
        state, reboot = watchdog.evaluate(state, "boot-b", 5610, True)
        self.assertFalse(reboot)
        self.assertFalse(state["reboot_latched"])
        state, reboot = watchdog.evaluate(state, "boot-b", 5700, False)
        self.assertFalse(reboot)
        state, reboot = watchdog.evaluate(state, "boot-b", 9300, False)
        self.assertTrue(reboot)

    def test_interrupted_outage_or_unknown_check_cannot_reboot(self):
        state = watchdog.new_state("boot")
        for when, result in ((0, False), (3599, None), (3600, False),
                             (7199, True), (7200, False), (10799, False)):
            state, reboot = watchdog.evaluate(state, "boot", when, result)
            self.assertFalse(reboot)

    def test_state_is_durable_and_corrupt_state_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            state = watchdog.new_state("boot")
            state["reboot_latched"] = True
            watchdog.save_state(path, state)
            self.assertEqual(watchdog.load_state(path, "boot"), state)
            path.write_text("{}")
            with self.assertRaises(ValueError):
                watchdog.load_state(path, "boot")

    def test_reboot_is_requested_only_after_state_save(self):
        state = watchdog.new_state("boot")
        state["offline_since"] = 0
        with patch.object(watchdog, "STATE_FILE", Path("test-state")), \
             patch.object(watchdog.Path, "read_text", return_value="boot"), \
             patch.object(watchdog, "load_state", return_value=state), \
             patch.object(watchdog, "wifi_connected", return_value=False), \
             patch.object(watchdog.time, "monotonic", return_value=3600), \
             patch.object(watchdog, "save_state", side_effect=OSError("disk full")), \
             patch.object(watchdog.subprocess, "run") as run:
            self.assertEqual(watchdog.main(), 1)
            run.assert_not_called()

    def test_networkmanager_state_and_errors(self):
        with patch.object(watchdog.subprocess, "run") as run:
            run.return_value.returncode = 0
            run.return_value.stdout = "100 (connected)\n"
            self.assertTrue(watchdog.wifi_connected())
            run.return_value.stdout = "30 (disconnected)\n"
            self.assertFalse(watchdog.wifi_connected())
            run.return_value.returncode = 1
            self.assertIsNone(watchdog.wifi_connected())


if __name__ == "__main__":
    unittest.main()
