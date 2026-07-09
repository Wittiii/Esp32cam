# Pi MQTT Streamer

This folder contains a Raspberry Pi camera sender that:

- captures H.264 with `rpicam-vid`
- publishes the live stream to MediaMTX or a raw TCP receiver
- keeps retrying the stream after reconnects or target outages
- optionally writes a timelapse from the same live video stream
- reports stream and timelapse state back through MQTT

## Supported output modes

- `mediamtx_rtsp`
  - recommended
  - uses `ffmpeg` to publish the Pi stream to `rtsp://server:8554/path`
- `tcp`
  - legacy raw TCP test mode
  - useful only for direct lab testing

## Install on the Pi

```bash
chmod +x pi_streamer/install_pi_streamer.sh
./pi_streamer/install_pi_streamer.sh
```

The install script creates a virtual environment and installs `ffmpeg`, which is required for `mediamtx_rtsp` and the timelapse writer.

## Configure

Edit:

```text
pi_streamer/config.json
```

### Stream fields

- `stream.mode`
- `stream.host`
- `stream.port`
- `stream.path`
- `stream.transport`
- `stream.width`, `stream.height`
- `stream.framerate`
- `stream.bitrate`
- `stream.sharpness`, `stream.brightness`, `stream.contrast`, `stream.saturation`

### Timelapse fields

- `timelapse.enabled`
  - starts the timelapse worker automatically
- `timelapse.interval_seconds`
  - one image every N seconds
- `timelapse.output_dir`
  - directory for JPEG images
- `timelapse.max_storage_gb`
  - timelapse stops automatically when this limit is reached
- `timelapse.jpeg_quality`
  - FFmpeg MJPEG quality scale from `2` to `31`
  - lower is better, `2` is high quality
- `timelapse.storage_check_seconds`
  - how often storage usage is recalculated

### MQTT fields

- `mqtt.host`
- `mqtt.port`
- `mqtt.client_id`
- `mqtt.base_topic`

Recommended topic layout:

```text
camera/pi-zero-01
```

## Start manually

```bash
.venv/bin/python pi_streamer/pi_cam_controller.py --config pi_streamer/config.json --autostart
```

## MQTT topics

Base topic example:

```text
camera/pi-zero-01
```

### Stream commands

- `camera/pi-zero-01/cmd/start`
- `camera/pi-zero-01/cmd/stop`
- `camera/pi-zero-01/cmd/restart`
- `camera/pi-zero-01/cmd/ping`
- `camera/pi-zero-01/cmd/set`

`cmd/set` expects a JSON object. Example:

```json
{
  "width": 1920,
  "height": 1080,
  "framerate": 15,
  "bitrate": 3500000,
  "sharpness": 1.2,
  "brightness": 0.05
}
```

### Timelapse commands

- `camera/pi-zero-01/cmd/timelapse/start`
- `camera/pi-zero-01/cmd/timelapse/stop`
- `camera/pi-zero-01/cmd/timelapse/set`

`cmd/timelapse/set` expects a JSON object. Example:

```json
{
  "interval_seconds": 60,
  "max_storage_gb": 22.0,
  "output_dir": "timelapse"
}
```

### Status topics

- `camera/pi-zero-01/status/online`
- `camera/pi-zero-01/status/state`
- `camera/pi-zero-01/status/error`
- `camera/pi-zero-01/status/config`
- `camera/pi-zero-01/status/target`
- `camera/pi-zero-01/status/pong`
- `camera/pi-zero-01/status/timelapse/state`
- `camera/pi-zero-01/status/timelapse/error`
- `camera/pi-zero-01/status/timelapse/storage_bytes`
- `camera/pi-zero-01/status/timelapse/storage_limit_bytes`
- `camera/pi-zero-01/status/timelapse/last_image`
- `camera/pi-zero-01/status/timelapse/output_dir`
- `camera/pi-zero-01/status/timelapse/enabled`
- `camera/pi-zero-01/status/timelapse/interval_seconds`

## Timelapse behavior

- The timelapse is generated from the same live stream bytes that are already leaving the camera.
- No second camera capture process is opened for snapshots.
- When the configured storage limit is reached, the timelapse stops and reports `storage_full`.
- The live stream itself keeps running.
- The operator must free space and then send `cmd/timelapse/start` again.

## systemd

Use:

```text
pi_streamer/pi-cam-controller.service.example
```

Adapt paths and username, copy it to `/etc/systemd/system/`, then:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now pi-cam-controller
```
