"""One guarded reboot after an hour without NetworkManager Wi-Fi connectivity."""

import json
import logging
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time


OFFLINE_SECONDS = 60 * 60
RECOVERY_SECONDS = 10 * 60
STATE_FILE = Path(os.environ.get("STATE_DIRECTORY", "/var/lib/pi-cam-wifi-watchdog")) / "state.json"
LOG = logging.getLogger("pi-cam-wifi-watchdog")


def wifi_connected() -> bool | None:
    """None means monitoring failed; it must never count as disconnected."""
    try:
        result = subprocess.run(
            ["nmcli", "--terse", "--get-values", "GENERAL.STATE", "device", "show", "wlan0"],
            capture_output=True, text=True, timeout=5, check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        LOG.warning("Cannot read wlan0 state: %s", exc)
        return None
    if result.returncode != 0:
        LOG.warning("Cannot read wlan0 state: nmcli exit %s", result.returncode)
        return None
    match = re.match(r"^(\d+)(?:\s|$)", result.stdout.strip())
    if not match:
        LOG.warning("Cannot parse wlan0 state: %r", result.stdout.strip()[:80])
        return None
    return int(match.group(1)) == 100


def new_state(boot_id: str) -> dict:
    return {"boot_id": boot_id, "offline_since": None, "online_since": None,
            "reboot_latched": False, "last_status": None}


def evaluate(state: dict, boot_id: str, now: float, connected: bool | None) -> tuple[dict, bool]:
    """Pure state transition. Monotonic timestamps never cross a boot boundary."""
    state = dict(state)
    if state["boot_id"] != boot_id:
        state["boot_id"] = boot_id
        state["offline_since"] = None
        state["online_since"] = None

    status = "unknown" if connected is None else "online" if connected else "offline"
    state["last_status"] = status
    if connected is None:
        # An unavailable monitor cannot prove a continuous Wi-Fi outage.
        state["offline_since"] = None
        state["online_since"] = None
        return state, False
    if connected:
        state["offline_since"] = None
        if state["online_since"] is None:
            state["online_since"] = now
        if state["reboot_latched"] and now - state["online_since"] >= RECOVERY_SECONDS:
            state["reboot_latched"] = False
        return state, False

    state["online_since"] = None
    if state["offline_since"] is None:
        state["offline_since"] = now
    if not state["reboot_latched"] and now - state["offline_since"] >= OFFLINE_SECONDS:
        state["reboot_latched"] = True
        return state, True
    return state, False


def load_state(path: Path, boot_id: str) -> dict:
    try:
        with path.open("r", encoding="utf-8") as source:
            state = json.load(source)
    except FileNotFoundError:
        return new_state(boot_id)
    # Corrupt state must fail closed. Resetting it could create reboot loops.
    if not isinstance(state, dict) or not all(key in state for key in new_state(boot_id)):
        raise ValueError("invalid watchdog state")
    if not isinstance(state["reboot_latched"], bool):
        raise ValueError("invalid reboot latch")
    return state


def save_state(path: Path, state: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent,
                                         prefix=".state-", delete=False) as target:
            temporary = Path(target.name)
            os.chmod(temporary, 0o600)
            json.dump(state, target)
            target.flush()
            os.fsync(target.fileno())
        os.replace(temporary, path)
        if os.name == "posix":
            directory_fd = os.open(path.parent, os.O_RDONLY)
            try:
                os.fsync(directory_fd)
            finally:
                os.close(directory_fd)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    try:
        boot_id = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        previous = load_state(STATE_FILE, boot_id)
        current, reboot = evaluate(previous, boot_id, time.monotonic(), wifi_connected())
        if current != previous:
            save_state(STATE_FILE, current)
            if current["last_status"] != previous["last_status"]:
                LOG.info("wlan0 changed to %s", current["last_status"])
            if previous["reboot_latched"] and not current["reboot_latched"]:
                LOG.info("Wi-Fi stable for ten minutes; reboot guard rearmed")
        if reboot:
            # The latch is durable before asking systemd to reboot.
            LOG.error("wlan0 disconnected for at least one hour; requesting one reboot")
            subprocess.run(["systemctl", "reboot"], check=True, timeout=10)
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        LOG.error("Wi-Fi watchdog stopped without reboot: %s", exc)
        return 1


if __name__ == "__main__":
    sys.exit(main())
