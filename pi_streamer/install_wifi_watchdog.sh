#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
command -v nmcli >/dev/null || { echo "NetworkManager/nmcli is required" >&2; exit 1; }
test -d /sys/class/net/wlan0 || { echo "wlan0 does not exist" >&2; exit 1; }
sudo install -m 644 "$SCRIPT_DIR/wifi_watchdog.py" /usr/local/lib/pi-cam-wifi-watchdog.py
sudo install -m 644 "$SCRIPT_DIR/pi-cam-wifi-watchdog.service" /etc/systemd/system/pi-cam-wifi-watchdog.service
sudo install -m 644 "$SCRIPT_DIR/pi-cam-wifi-watchdog.timer" /etc/systemd/system/pi-cam-wifi-watchdog.timer
sudo systemctl daemon-reload
sudo systemctl enable --now pi-cam-wifi-watchdog.timer
echo "Wi-Fi watchdog enabled. Check: systemctl status pi-cam-wifi-watchdog.timer"
