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
It uses the repository containing the script, regardless of the current directory,
and never replaces an existing `config.json`. Python 3.10 or newer and
`rpicam-vid` must be available on the Pi. FFmpeg copies the camera's H.264 stream;
it does not re-encode it on the CPU.

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
- `stream.ffmpeg_path` (optional, local configuration only; never accepted over MQTT)

Width and height must be even (16..4096), framerate 1..60, bitrate
10000..50000000 and port 1..65535. These are validation limits, not a guarantee
that a particular camera supports every combination. Only H.264 is supported.
Booleans accept `true`/`false`, including their string forms, and `0`/`1`.
Invalid or non-finite values are rejected before changing the stream.

### Server capture fields

- `server_capture.enabled`
  - asks the Node server to capture snapshots from MediaMTX
- `server_capture.interval_seconds`
  - one image every N seconds (1..86400)
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

### Stream troubleshooting

- Check `status/online` first. When it is `false`, other retained status values can be stale and commands will not reach the Pi.
- MQTT `Bad user name or password` means the credentials in `config.json` no longer match the broker.
- An RTSP `404 Not Found` for the configured path means MediaMTX currently has no active publisher for that path.
- After changing `config.json` or the controller code, restart the service with `sudo systemctl restart pi-cam-controller`.
- Inspect failures with `journalctl -u pi-cam-controller --no-pager -n 100`.

## Timelapse behavior

- The Pi only publishes H.264 to MediaMTX and never writes timelapse images to its SD card.
- The Node.js server captures JPEGs from the MediaMTX path.
- Per-camera limits and a global server disk reserve are enforced by the server.
- A stream watchdog restarts capture and publishing when no video bytes flow for 15 seconds.
- The watchdog allows 20 seconds for startup. Repeated failures use a backoff
  of up to 30 seconds; a successful process start alone does not reset it.
- MQTT callbacks enqueue commands (maximum 32 pending, 8192 bytes each) instead
  of stopping processes or saving settings on the network thread.
- Unchanged retained status values and settings writes are suppressed. Changed
  settings are flushed to a temporary file and atomically replace the config.
- SIGTERM/SIGINT shut down camera, FFmpeg and MQTT cleanly. The service example
  allows 50 seconds and also terminates child processes in its control group.

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

## Updating an existing installation safely

`config.json` is now local-only. **The first pull that removes it from Git can
remove the previously tracked file on another checkout. Back it up first.**
Generated Python bytecode is also no longer tracked. No image archive or database
is migrated by this change.

From the repository root, stop the service and make a private backup outside Git:

```bash
sudo systemctl stop pi-cam-controller
PI_CONFIG_BACKUP="$(mktemp -d "$HOME/pi-streamer-config.XXXXXX")"
install -m 600 pi_streamer/config.json "$PI_CONFIG_BACKUP/config.json"
echo "Config backup: $PI_CONFIG_BACKUP/config.json"
git status --short
```

Only continue after the backup succeeds. Resolve any local Git changes separately;
do not use a force pull, reset, or a blanket stash on a running installation.
After a successful `git pull --ff-only`, restore the saved config in the same shell:

```bash
install -m 600 "$PI_CONFIG_BACKUP/config.json" pi_streamer/config.json
.venv/bin/python -m pip install -r pi_streamer/requirements.txt
.venv/bin/python -m unittest discover -s test -p 'test_pi_*.py' -v
sudo systemctl restart pi-cam-controller
journalctl -u pi-cam-controller --no-pager -n 100
```

Stop at any error. Keep the backup until the stream and MQTT commands are verified.
Existing systemd units are not automatically updated; apply the example's service
settings to your installed unit and run `sudo systemctl daemon-reload` if changed.

## Network access

Use a trusted LAN/VPN and broker credentials with per-camera topic permissions.
The streamer uses plain MQTT and the camera's RTSP endpoint has no authentication;
neither should be forwarded directly to the Internet. Removing a config from Git
does not remove earlier commits: rotate credentials if they were exposed.

## Tests

See [test instructions](../test/README.md). Host tests use temporary configs and
mock processes; they do not read `pi_streamer/config.json`, connect to a broker,
start a camera, or prove stability on the real Pi hardware.
