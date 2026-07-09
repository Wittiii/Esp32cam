# ESP32-CAM RTSP + MQTT

Die Firmware streamt jetzt direkt als RTSP-Quelle auf dem ESP32. MediaMTX zieht den Stream danach direkt vom ESP32 ab.

RTSP-Quelle des ESP32:

```text
rtsp://esp32-cam-streamer.local:8554/mjpeg/1
```

Standard-MQTT-Topic:

```text
camera/esp32-cam-01
```

Befehle:

```text
camera/esp32-cam-01/cmd/start
camera/esp32-cam-01/cmd/stop
camera/esp32-cam-01/cmd/restart
camera/esp32-cam-01/cmd/ping
camera/esp32-cam-01/cmd/set
```

Beispiel fuer `cmd/set` als JSON:

```json
{
  "framesize": 2,
  "jpeg_quality": 10,
  "brightness": 1,
  "contrast": 0,
  "saturation": -1,
  "sharpness": 1,
  "hmirror": 0,
  "vflip": 0,
  "led": 1,
  "stream_fps": 10,
  "stream_enabled": true
}
```

Alternativ geht auch ein einfacher `key=value`-String:

```text
framesize=2;jpeg_quality=10;led=1;stream_fps=10
```

Wichtige Status-Topics:

```text
camera/esp32-cam-01/status/online
camera/esp32-cam-01/status/state
camera/esp32-cam-01/status/error
camera/esp32-cam-01/status/ip
camera/esp32-cam-01/status/rtsp_url
camera/esp32-cam-01/status/config
camera/esp32-cam-01/status/last_status
camera/esp32-cam-01/status/clients
camera/esp32-cam-01/status/pong
```

MediaMTX-Beispiel:

```yaml
paths:
  esp32-cam-01:
    source: rtsp://esp32-cam-streamer.local:8554/mjpeg/1
```

Falls `.local` im LAN nicht sauber aufgeloest wird, nimm statt `esp32-cam-streamer.local` direkt die IP aus `status/ip`.
