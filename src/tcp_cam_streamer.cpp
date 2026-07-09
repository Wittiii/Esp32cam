#include "tcp_cam_streamer.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <stdarg.h>
#include <stdio.h>

#include "esp32/spiram.h"
#include "esp_camera.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

#include "app_config.h"
#include "camera_pins.h"

namespace {

WiFiClient g_streamClient;
char g_statusHistory[appcfg::kStatusHistorySize][appcfg::kStatusLineSize] = {};
bool g_psramAvailable = false;
bool g_wifiStartIssued = false;
framesize_t g_frameSize = FRAMESIZE_VGA;
int g_jpegQuality = 12;
int g_brightness = 1;
int g_contrast = 0;
int g_saturation = -1;
int g_sharpness = 0;
int g_hmirror = 0;
int g_vflip = 0;
int g_ledEnabled = 0;
size_t g_statusHistoryCount = 0;
size_t g_statusHistoryNext = 0;
uint32_t g_lastWifiAttemptMs = 0;
uint32_t g_lastTcpAttemptMs = 0;
uint32_t g_lastStatsMs = 0;
uint32_t g_lastFrameErrorMs = 0;
uint32_t g_framesSent = 0;

const char *frameSizeName() {
  switch (g_frameSize) {
    case FRAMESIZE_QVGA:
      return "QVGA";
    case FRAMESIZE_VGA:
      return "VGA";
    case FRAMESIZE_SVGA:
      return "SVGA";
    case FRAMESIZE_XGA:
      return "XGA";
    case FRAMESIZE_HD:
      return "HD";
    case FRAMESIZE_SXGA:
      return "SXGA";
    case FRAMESIZE_UXGA:
      return "UXGA";
    default:
      return "OTHER";
  }
}

framesize_t frameSizeFromIndex(int index) {
  switch (index) {
    case 0:
      return FRAMESIZE_QVGA;
    case 1:
      return FRAMESIZE_VGA;
    case 2:
      return FRAMESIZE_SVGA;
    case 3:
      return FRAMESIZE_XGA;
    case 4:
      return FRAMESIZE_HD;
    case 5:
      return FRAMESIZE_SXGA;
    case 6:
      return FRAMESIZE_UXGA;
    default:
      return g_frameSize;
  }
}

int frameSizeToIndex(framesize_t size) {
  switch (size) {
    case FRAMESIZE_QVGA:
      return 0;
    case FRAMESIZE_VGA:
      return 1;
    case FRAMESIZE_SVGA:
      return 2;
    case FRAMESIZE_XGA:
      return 3;
    case FRAMESIZE_HD:
      return 4;
    case FRAMESIZE_SXGA:
      return 5;
    case FRAMESIZE_UXGA:
      return 6;
    default:
      return 1;
  }
}

const char *flashModeName(FlashMode_t mode) {
  switch (mode) {
    case FM_QIO:
      return "QIO";
    case FM_QOUT:
      return "QOUT";
    case FM_DIO:
      return "DIO";
    case FM_DOUT:
      return "DOUT";
    case FM_FAST_READ:
      return "FAST_READ";
    case FM_SLOW_READ:
      return "SLOW_READ";
    default:
      return "UNKNOWN";
  }
}

const char *psramChipSizeName(esp_spiram_size_t size) {
  switch (size) {
    case ESP_SPIRAM_SIZE_16MBITS:
      return "16Mbit";
    case ESP_SPIRAM_SIZE_32MBITS:
      return "32Mbit";
    case ESP_SPIRAM_SIZE_64MBITS:
      return "64Mbit";
    default:
      return "invalid";
  }
}

bool writeAll(const uint8_t *buffer, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    if (!g_streamClient.connected()) {
      return false;
    }

    const size_t written = g_streamClient.write(buffer + sent, length - sent);
    if (written == 0) {
      delay(1);
      continue;
    }
    sent += written;
  }
  return true;
}

bool sendPacket(const uint8_t magic[4], const uint8_t *payload, size_t length) {
  uint8_t header[appcfg::kPacketHeaderSize] = {
      magic[0],
      magic[1],
      magic[2],
      magic[3],
      static_cast<uint8_t>((length >> 24) & 0xFF),
      static_cast<uint8_t>((length >> 16) & 0xFF),
      static_cast<uint8_t>((length >> 8) & 0xFF),
      static_cast<uint8_t>(length & 0xFF),
  };

  return writeAll(header, sizeof(header)) &&
         (length == 0 || writeAll(payload, length));
}

void storeStatusMessage(const char *message) {
  snprintf(
      g_statusHistory[g_statusHistoryNext],
      appcfg::kStatusLineSize,
      "%s",
      message);
  g_statusHistoryNext = (g_statusHistoryNext + 1) % appcfg::kStatusHistorySize;
  if (g_statusHistoryCount < appcfg::kStatusHistorySize) {
    ++g_statusHistoryCount;
  }
}

bool sendStatusPacket(const char *message) {
  if (!g_streamClient.connected()) {
    return false;
  }

  const size_t length = strnlen(message, appcfg::kStatusLineSize);
  const bool ok = sendPacket(
      appcfg::kStatusMagic,
      reinterpret_cast<const uint8_t *>(message),
      length);
  if (!ok) {
    Serial.println("[TCP] Status send failed");
    g_streamClient.stop();
  }
  return ok;
}

void recordStatus(const char *message) {
  Serial.printf("[STAT] %s\n", message);
  storeStatusMessage(message);
  sendStatusPacket(message);
}

void statusf(const char *format, ...) {
  char message[appcfg::kStatusLineSize];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  recordStatus(message);
}

void flushStatusHistory() {
  if (!g_streamClient.connected()) {
    return;
  }

  const size_t start =
      (g_statusHistoryNext + appcfg::kStatusHistorySize - g_statusHistoryCount) %
      appcfg::kStatusHistorySize;
  for (size_t i = 0; i < g_statusHistoryCount; ++i) {
    const size_t index = (start + i) % appcfg::kStatusHistorySize;
    if (g_statusHistory[index][0] != '\0' && !sendStatusPacket(g_statusHistory[index])) {
      return;
    }
  }
}

void sendCameraConfigStatus() {
  statusf(
      "cfg framesize=%d(%s) jpeg_quality=%d brightness=%d contrast=%d saturation=%d sharpness=%d hmirror=%d vflip=%d led=%d",
      frameSizeToIndex(g_frameSize),
      frameSizeName(),
      g_jpegQuality,
      g_brightness,
      g_contrast,
      g_saturation,
      g_sharpness,
      g_hmirror,
      g_vflip,
      g_ledEnabled);
}

void applyLedState(int enabled) {
  g_ledEnabled = enabled ? 1 : 0;
  digitalWrite(LED_GPIO_NUM, g_ledEnabled ? HIGH : LOW);
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      recordStatus("wifi station started");
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      recordStatus("wifi access point connected");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      statusf("wifi ip=%s", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      statusf("wifi disconnected reason=%d", info.wifi_sta_disconnected.reason);
      g_streamClient.stop();
      break;
    default:
      break;
  }
}

void logBootDiagnostics() {
  statusf(
      "boot flash=%luMB mode=%s speed=%luMHz",
      ESP.getFlashChipSize() / (1024UL * 1024UL),
      flashModeName(ESP.getFlashChipMode()),
      ESP.getFlashChipSpeed() / 1000000UL);
  statusf(
      "heap total=%luKB free=%luKB",
      ESP.getHeapSize() / 1024UL,
      ESP.getFreeHeap() / 1024UL);
}

void logPsramDiagnostics() {
  const bool psramInit = esp_spiram_is_initialized();
  const uint32_t psramSizeBytes = ESP.getPsramSize();
  esp_spiram_size_t psramChipSize = ESP_SPIRAM_SIZE_INVALID;
  int psramCsIo = -1;

  if (psramInit) {
    psramChipSize = esp_spiram_get_chip_size();
    psramCsIo = static_cast<int>(esp_spiram_get_cs_io());
  }

  statusf(
      "psram found=%s init=%s size=%lu chip=%s cs_io=%d",
      g_psramAvailable ? "yes" : "no",
      psramInit ? "yes" : "no",
      psramSizeBytes,
      psramChipSizeName(psramChipSize),
      psramCsIo);

  if (!g_psramAvailable) {
    recordStatus("psram missing: likely 5V supply, board clone, or defective psram");
  }
}

void configureMdns() {
  if (!MDNS.begin(appcfg::kMdnsHostname)) {
    recordStatus("mdns setup failed");
    return;
  }

  MDNS.addService("arduino", "tcp", 3232);
  MDNS.addServiceTxt("arduino", "tcp", "board", "esp32cam");
  statusf("mdns ready host=%s.local", appcfg::kMdnsHostname);
}

void configureOta() {
  ArduinoOTA.setHostname(appcfg::kOtaHostname);
  if (strlen(appcfg::kOtaPassword) > 0) {
    ArduinoOTA.setPassword(appcfg::kOtaPassword);
  }

  ArduinoOTA.onStart([]() {
    recordStatus("ota update started");
  });
  ArduinoOTA.onEnd([]() {
    recordStatus("ota update finished");
  });
  ArduinoOTA.onError([](ota_error_t error) {
    statusf("ota error=%u", static_cast<uint32_t>(error));
  });

  ArduinoOTA.begin();
  statusf("ota ready host=%s", appcfg::kOtaHostname);
}

bool initCamera() {
  g_psramAvailable = psramFound();
  g_frameSize = g_psramAvailable ? FRAMESIZE_SVGA : FRAMESIZE_VGA;
  g_jpegQuality = g_psramAvailable ? 10 : 12;

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = g_frameSize;
  config.jpeg_quality = g_jpegQuality;
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  if (g_psramAvailable) {
    config.frame_size = g_frameSize;
    config.jpeg_quality = g_jpegQuality;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
  }

  const esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    statusf("camera init failed err=0x%lx", static_cast<uint32_t>(err));
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor != nullptr) {
    sensor->set_brightness(sensor, g_brightness);
    sensor->set_contrast(sensor, g_contrast);
    sensor->set_saturation(sensor, g_saturation);
    sensor->set_sharpness(sensor, g_sharpness);
    sensor->set_hmirror(sensor, g_hmirror);
    sensor->set_vflip(sensor, g_vflip);
  }

  pinMode(LED_GPIO_NUM, OUTPUT);
  applyLedState(0);

  statusf(
      "camera ready psram=%s frame=%s",
      g_psramAvailable ? "yes" : "no",
      frameSizeName());
  sendCameraConfigStatus();
  return true;
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  const uint32_t now = millis();
  if (now - g_lastWifiAttemptMs < appcfg::kWifiRetryMs) {
    return;
  }

  g_lastWifiAttemptMs = now;

  if (!g_wifiStartIssued) {
    g_wifiStartIssued = true;
    statusf("wifi connecting ssid=%s", appcfg::kWifiSsid);
    WiFi.begin(appcfg::kWifiSsid, appcfg::kWifiPassword);
    return;
  }

  recordStatus("wifi reconnecting");
  WiFi.reconnect();
}

bool ensureTcpConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    if (g_streamClient.connected()) {
      g_streamClient.stop();
    }
    return false;
  }

  if (g_streamClient.connected()) {
    return true;
  }

  const uint32_t now = millis();
  if (now - g_lastTcpAttemptMs < appcfg::kTcpRetryMs) {
    return false;
  }

  g_lastTcpAttemptMs = now;
  Serial.printf("[TCP] Connecting to %s:%u\n", appcfg::kViewerHost, appcfg::kViewerPort);
  g_streamClient.stop();

  if (!g_streamClient.connect(appcfg::kViewerHost, appcfg::kViewerPort)) {
    statusf("viewer unreachable host=%s", appcfg::kViewerHost);
    return false;
  }

  g_streamClient.setNoDelay(true);
  recordStatus("viewer connected");
  flushStatusHistory();
  statusf(
      "viewer session psram=%s frame=%s",
      g_psramAvailable ? "yes" : "no",
      frameSizeName());
  sendCameraConfigStatus();
  return true;
}

bool readIncomingExact(uint8_t *buffer, size_t length, uint32_t timeoutMs) {
  size_t offset = 0;
  const uint32_t start = millis();
  while (offset < length) {
    if (!g_streamClient.connected()) {
      return false;
    }
    const size_t available = g_streamClient.available();
    if (available == 0) {
      if (millis() - start >= timeoutMs) {
        return false;
      }
      delay(1);
      continue;
    }
    const size_t toRead = min(length - offset, available);
    const int readCount = g_streamClient.read(buffer + offset, toRead);
    if (readCount <= 0) {
      if (millis() - start >= timeoutMs) {
        return false;
      }
      delay(1);
      continue;
    }
    offset += static_cast<size_t>(readCount);
  }
  return true;
}

int clampValue(int value, int minValue, int maxValue) {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

void applyControlPayload(char *payload) {
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) {
    recordStatus("control rejected: sensor unavailable");
    return;
  }

  bool changed = false;
  char *context = nullptr;
  for (char *token = strtok_r(payload, ";", &context); token != nullptr;
       token = strtok_r(nullptr, ";", &context)) {
    char *separator = strchr(token, '=');
    if (separator == nullptr) {
      continue;
    }

    *separator = '\0';
    const char *key = token;
    const int value = atoi(separator + 1);

    if (strcmp(key, "framesize") == 0) {
      const framesize_t newSize = frameSizeFromIndex(value);
      if (newSize != g_frameSize && sensor->set_framesize(sensor, newSize) == 0) {
        g_frameSize = newSize;
        changed = true;
      }
    } else if (strcmp(key, "jpeg_quality") == 0) {
      const int bounded = clampValue(value, 4, 63);
      if (bounded != g_jpegQuality && sensor->set_quality(sensor, bounded) == 0) {
        g_jpegQuality = bounded;
        changed = true;
      }
    } else if (strcmp(key, "brightness") == 0) {
      const int bounded = clampValue(value, -2, 2);
      if (bounded != g_brightness && sensor->set_brightness(sensor, bounded) == 0) {
        g_brightness = bounded;
        changed = true;
      }
    } else if (strcmp(key, "contrast") == 0) {
      const int bounded = clampValue(value, -2, 2);
      if (bounded != g_contrast && sensor->set_contrast(sensor, bounded) == 0) {
        g_contrast = bounded;
        changed = true;
      }
    } else if (strcmp(key, "saturation") == 0) {
      const int bounded = clampValue(value, -2, 2);
      if (bounded != g_saturation && sensor->set_saturation(sensor, bounded) == 0) {
        g_saturation = bounded;
        changed = true;
      }
    } else if (strcmp(key, "sharpness") == 0) {
      const int bounded = clampValue(value, -2, 2);
      if (bounded != g_sharpness && sensor->set_sharpness(sensor, bounded) == 0) {
        g_sharpness = bounded;
        changed = true;
      }
    } else if (strcmp(key, "hmirror") == 0) {
      const int bounded = value ? 1 : 0;
      if (bounded != g_hmirror && sensor->set_hmirror(sensor, bounded) == 0) {
        g_hmirror = bounded;
        changed = true;
      }
    } else if (strcmp(key, "vflip") == 0) {
      const int bounded = value ? 1 : 0;
      if (bounded != g_vflip && sensor->set_vflip(sensor, bounded) == 0) {
        g_vflip = bounded;
        changed = true;
      }
    } else if (strcmp(key, "led") == 0) {
      const int bounded = value ? 1 : 0;
      if (bounded != g_ledEnabled) {
        applyLedState(bounded);
        changed = true;
      }
    }
  }

  if (changed) {
    recordStatus("camera control applied");
    sendCameraConfigStatus();
  }
}

void processIncomingControl() {
  while (g_streamClient.connected() &&
         g_streamClient.available() >= static_cast<int>(appcfg::kPacketHeaderSize)) {
    uint8_t header[appcfg::kPacketHeaderSize];
    if (!readIncomingExact(header, sizeof(header), 50)) {
      return;
    }

    if (memcmp(header, appcfg::kControlMagic, 4) != 0) {
      recordStatus("control rejected: invalid header");
      g_streamClient.stop();
      return;
    }

    const size_t payloadLength =
        (static_cast<size_t>(header[4]) << 24) |
        (static_cast<size_t>(header[5]) << 16) |
        (static_cast<size_t>(header[6]) << 8) |
        static_cast<size_t>(header[7]);

    if (payloadLength == 0 || payloadLength >= appcfg::kMaxControlPayloadSize) {
      recordStatus("control rejected: invalid payload size");
      g_streamClient.stop();
      return;
    }

    char payload[appcfg::kMaxControlPayloadSize];
    if (!readIncomingExact(reinterpret_cast<uint8_t *>(payload), payloadLength, 100)) {
      recordStatus("control rejected: payload timeout");
      return;
    }
    payload[payloadLength] = '\0';
    applyControlPayload(payload);
  }
}

bool streamFrame() {
  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr) {
    const uint32_t now = millis();
    if (now - g_lastFrameErrorMs >= appcfg::kFrameErrorReportMs) {
      g_lastFrameErrorMs = now;
      recordStatus("camera frame capture failed");
    }
    delay(30);
    return false;
  }

  uint8_t header[appcfg::kPacketHeaderSize] = {
      appcfg::kFrameMagic[0],
      appcfg::kFrameMagic[1],
      appcfg::kFrameMagic[2],
      appcfg::kFrameMagic[3],
      static_cast<uint8_t>((frame->len >> 24) & 0xFF),
      static_cast<uint8_t>((frame->len >> 16) & 0xFF),
      static_cast<uint8_t>((frame->len >> 8) & 0xFF),
      static_cast<uint8_t>(frame->len & 0xFF),
  };

  const bool ok = writeAll(header, sizeof(header)) && writeAll(frame->buf, frame->len);
  esp_camera_fb_return(frame);

  if (!ok) {
    recordStatus("frame send failed");
    g_streamClient.stop();
    return false;
  }

  ++g_framesSent;
  return true;
}

void printStats() {
  const uint32_t now = millis();
  if (now - g_lastStatsMs < 5000) {
    return;
  }

  const float seconds =
      (g_lastStatsMs == 0) ? 5.0f : (now - g_lastStatsMs) / 1000.0f;
  const float fps = g_framesSent / seconds;
  g_lastStatsMs = now;
  g_framesSent = 0;

  statusf(
      "runtime wifi=%s ip=%s tcp=%s fps=%.1f",
      WiFi.status() == WL_CONNECTED ? "up" : "down",
      WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-",
      g_streamClient.connected() ? "up" : "down",
      fps);
}

}  // namespace

namespace tcpcam {

void setupStreamer() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  Serial.setDebugOutput(false);
  Serial.println();
  Serial.println("[BOOT] ESP32-CAM streamer starting");

  logBootDiagnostics();

  if (!initCamera()) {
    Serial.println("[BOOT] Camera init failed, reboot required");
    return;
  }

  logPsramDiagnostics();
  delay(500);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);

  ensureWifi();
  configureMdns();
  configureOta();
}

void loopStreamer() {
  ArduinoOTA.handle();
  ensureWifi();

  if (ensureTcpConnection()) {
    processIncomingControl();
    streamFrame();
    processIncomingControl();
  } else {
    delay(10);
  }

  printStats();
  delay(1);
}

}  // namespace tcpcam
