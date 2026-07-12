# Pi MQTT Streamer

This folder contains a Raspberry Pi camera sender that:

- captures H.264 with `rpicam-vid`
- publishes the live stream to MediaMTX or a raw TCP receiver
- keeps retrying the stream after reconnects or target outages
- delegates timelapse capture to the central server without writing to the Pi SD card
- reports detailed stream health and server-capture settings through MQTT

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

The install script creates a virtual environment and installs `ffmpeg`, which is required for `mediamtx_rtsp`.

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

### Server capture fields

- `server_capture.enabled`
  - asks the Node server to capture snapshots from MediaMTX
- `server_capture.interval_seconds`
  - one image every N seconds
Storage limits are configured only on the Node server. The Pi never receives, reserves or writes archive storage.

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
  "interval_seconds": 60
}
```

### Status topics

- `camera/pi-zero-01/status/online`
- `camera/pi-zero-01/status/state`
- `camera/pi-zero-01/status/error`
- `camera/pi-zero-01/status/config`
- `camera/pi-zero-01/status/target`
- `camera/pi-zero-01/status/pong`
- `camera/pi-zero-01/status/capture_request/state`
- `camera/pi-zero-01/status/capture_request/error`
- `camera/pi-zero-01/status/capture_request/enabled`
- `camera/pi-zero-01/status/capture_request/interval_seconds`

Actual archive usage and capture results are published by the Node server below `status/server_capture/*`.

## Timelapse behavior

- The Pi only publishes H.264 to MediaMTX and never writes timelapse images to its SD card.
- The Node.js server captures JPEGs from the MediaMTX path.
- Per-camera limits and a global server disk reserve are enforced by the server.
- A stream watchdog restarts capture and publishing when no video bytes flow for 15 seconds.

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
