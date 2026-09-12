#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="${1:-$(dirname -- "$SCRIPT_DIR")}"
PROJECT_DIR="$(cd -- "$PROJECT_DIR" && pwd)"
VENV_DIR="${2:-$PROJECT_DIR/.venv}"

sudo apt update
sudo apt install -y python3-venv python3-pip ffmpeg

python3 -m venv "$VENV_DIR"
"$VENV_DIR/bin/pip" install --upgrade pip
"$VENV_DIR/bin/pip" install -r "$PROJECT_DIR/pi_streamer/requirements.txt"

if [ ! -f "$PROJECT_DIR/pi_streamer/config.json" ]; then
  (umask 077; cp -- "$PROJECT_DIR/pi_streamer/config.example.json" "$PROJECT_DIR/pi_streamer/config.json")
fi

if ! command -v rpicam-vid >/dev/null 2>&1; then
  echo "Warning: rpicam-vid is missing. Install the camera tools for your Raspberry Pi OS before starting."
fi

echo "Installation complete."
echo "Edit: $PROJECT_DIR/pi_streamer/config.json"
echo "Run : $VENV_DIR/bin/python $PROJECT_DIR/pi_streamer/pi_cam_controller.py --config $PROJECT_DIR/pi_streamer/config.json --autostart"
