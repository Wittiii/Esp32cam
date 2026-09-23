#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
sudo install -d -m 755 /etc/systemd/journald.conf.d
sudo install -m 644 "$SCRIPT_DIR/journald-pi-camera.conf" /etc/systemd/journald.conf.d/60-pi-camera.conf
sudo systemctl restart systemd-journald
sudo journalctl --flush
echo "Persistent system journal enabled (64 MiB retention target, up to 7 days)."
echo "Camera file logs start automatically with the updated controller."
