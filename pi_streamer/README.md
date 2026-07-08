# Pi MQTT Streamer

This folder contains a Raspberry Pi camera sender that:

- captures H.264 with `rpicam-vid`
- publishes the stream to the Node/MediaMTX server
- accepts MQTT commands to start, stop and reconfigure the stream
- reports state, config and errors back through MQTT

## Supported output modes

- `mediamtx_rtsp`
  - recommended
  - uses `ffmpeg` to publish the Pi stream to `rtsp://server:8554/path`
  - MediaMTX can then expose the same stream as WebRTC, HLS and RTSP
- `tcp`
  - keeps the old raw TCP test path
  - useful only for direct lab testing with the existing viewer tools

## Install on the Pi

Clone or copy this repository to the Pi, then run:

```bash
chmod +x pi_streamer/install_pi_streamer.sh
./pi_streamer/install_pi_streamer.sh
```

The install script creates a virtual environment, installs Python dependencies and also installs `ffmpeg`, which is required for `mediamtx_rtsp` mode.

## Configure

Edit:

```text
pi_streamer/config.json
```

Important stream fields:

- `stream.mode`
  - `mediamtx_rtsp` or `tcp`
- `stream.host`
  - IP of the server that runs Node and MediaMTX
- `stream.port`
  - `8554` for MediaMTX RTSP publish, or your TCP test port in `tcp` mode
- `stream.path`
  - MediaMTX stream path, for example `pi-zero-01`
- `stream.transport`
  - usually `tcp`
- `stream.width`, `stream.height`
- `stream.framerate`
- `stream.bitrate`
- `stream.sharpness`, `stream.brightness`, `stream.contrast`, `stream.saturation`

Important MQTT fields:

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

Commands:

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

Status topics:

- `camera/pi-zero-01/status/online`
- `camera/pi-zero-01/status/state`
- `camera/pi-zero-01/status/error`
- `camera/pi-zero-01/status/config`
- `camera/pi-zero-01/status/target`
- `camera/pi-zero-01/status/pong`

## Local server target

If Node and MediaMTX run on the same LAN server, the typical settings are:

```json
{
  "stream": {
    "mode": "mediamtx_rtsp",
    "host": "192.168.178.27",
    "port": 8554,
    "path": "pi-zero-01"
  },
  "mqtt": {
    "host": "192.168.178.27",
    "port": 1883,
    "base_topic": "camera/pi-zero-01"
  }
}
```

Then the browser-side URLs become:

- WebRTC: `http://SERVER_IP:8889/pi-zero-01`
- HLS: `http://SERVER_IP:8888/pi-zero-01`
- RTSP: `rtsp://SERVER_IP:8554/pi-zero-01`

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
