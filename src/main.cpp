#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ArduinoOTA.h>
#include <stdarg.h>
#include <stdio.h>
#include "esp_camera.h"
#include "esp32/spiram.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

namespace {

constexpr char kWifiSsid[] = "FRITZ!Box 7590 BK";
constexpr char kWifiPassword[] = "35428536880518248119";

constexpr char kViewerHost[] = "192.168.178.27";
constexpr uint16_t kViewerPort = 5005;

constexpr char kOtaHostname[] = "esp32-cam-streamer";
constexpr char kOtaPassword[] = "1234";

constexpr uint32_t kFrameErrorReportMs = 5000;
constexpr uint32_t kWifiRetryMs = 5000;
constexpr uint32_t kTcpRetryMs = 2000;
constexpr size_t kPacketHeaderSize = 8;
constexpr uint8_t kFrameMagic[4] = {'J', 'P', 'E', 'G'};
constexpr uint8_t kStatusMagic[4] = {'S', 'T', 'A', 'T'};
constexpr size_t kStatusHistorySize = 10;
constexpr size_t kStatusLineSize = 128;

constexpr int PWDN_GPIO_NUM = 32;
constexpr int RESET_GPIO_NUM = -1;
constexpr int XCLK_GPIO_NUM = 0;
constexpr int SIOD_GPIO_NUM = 26;
constexpr int SIOC_GPIO_NUM = 27;
constexpr int Y9_GPIO_NUM = 35;
constexpr int Y8_GPIO_NUM = 34;
constexpr int Y7_GPIO_NUM = 39;
constexpr int Y6_GPIO_NUM = 36;
constexpr int Y5_GPIO_NUM = 21;
constexpr int Y4_GPIO_NUM = 19;
constexpr int Y3_GPIO_NUM = 18;
constexpr int Y2_GPIO_NUM = 5;
constexpr int VSYNC_GPIO_NUM = 25;
constexpr int HREF_GPIO_NUM = 23;
constexpr int PCLK_GPIO_NUM = 22;

WiFiClient g_streamClient;
char g_statusHistory[kStatusHistorySize][kStatusLineSize] = {};
bool g_psramAvailable = false;
bool g_wifiStartIssued = false;
size_t g_statusHistoryCount = 0;
size_t g_statusHistoryNext = 0;
uint32_t g_lastWifiAttemptMs = 0;
uint32_t g_lastTcpAttemptMs = 0;
uint32_t g_lastStatsMs = 0;
uint32_t g_lastFrameErrorMs = 0;
uint32_t g_framesSent = 0;

const char *frameSizeName() {
  return g_psramAvailable ? "SVGA" : "VGA";
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
  uint8_t header[kPacketHeaderSize] = {
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
  snprintf(g_statusHistory[g_statusHistoryNext], kStatusLineSize, "%s", message);
  g_statusHistoryNext = (g_statusHistoryNext + 1) % kStatusHistorySize;
  if (g_statusHistoryCount < kStatusHistorySize) {
    ++g_statusHistoryCount;
  }
}

bool sendStatusPacket(const char *message) {
  if (!g_streamClient.connected()) {
    return false;
  }

  const size_t length = strnlen(message, kStatusLineSize);
  const bool ok = sendPacket(kStatusMagic, reinterpret_cast<const uint8_t *>(message), length);
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
  char message[kStatusLineSize];
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
      (g_statusHistoryNext + kStatusHistorySize - g_statusHistoryCount) % kStatusHistorySize;
  for (size_t i = 0; i < g_statusHistoryCount; ++i) {
    const size_t index = (start + i) % kStatusHistorySize;
    if (g_statusHistory[index][0] != '\0' && !sendStatusPacket(g_statusHistory[index])) {
      return;
    }
  }
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

void configureOta() {
  ArduinoOTA.setHostname(kOtaHostname);
  if (strlen(kOtaPassword) > 0) {
    ArduinoOTA.setPassword(kOtaPassword);
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
  statusf("ota ready host=%s", kOtaHostname);
}

bool initCamera() {
  g_psramAvailable = psramFound();

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
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_DRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  if (g_psramAvailable) {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 10;
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
    sensor->set_brightness(sensor, 1);
    sensor->set_saturation(sensor, -1);
  }

  statusf("camera ready psram=%s frame=%s", g_psramAvailable ? "yes" : "no", frameSizeName());
  return true;
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  const uint32_t now = millis();
  if (now - g_lastWifiAttemptMs < kWifiRetryMs) {
    return;
  }

  g_lastWifiAttemptMs = now;

  if (!g_wifiStartIssued) {
    g_wifiStartIssued = true;
    statusf("wifi connecting ssid=%s", kWifiSsid);
    WiFi.begin(kWifiSsid, kWifiPassword);
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
  if (now - g_lastTcpAttemptMs < kTcpRetryMs) {
    return false;
  }

  g_lastTcpAttemptMs = now;
  Serial.printf("[TCP] Connecting to %s:%u\n", kViewerHost, kViewerPort);
  g_streamClient.stop();

  if (!g_streamClient.connect(kViewerHost, kViewerPort)) {
    statusf("viewer unreachable host=%s", kViewerHost);
    return false;
  }

  g_streamClient.setNoDelay(true);
  recordStatus("viewer connected");
  flushStatusHistory();
  statusf("viewer session psram=%s frame=%s", g_psramAvailable ? "yes" : "no", frameSizeName());
  return true;
}

bool streamFrame() {
  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr) {
    const uint32_t now = millis();
    if (now - g_lastFrameErrorMs >= kFrameErrorReportMs) {
      g_lastFrameErrorMs = now;
      recordStatus("camera frame capture failed");
    }
    delay(30);
    return false;
  }

  uint8_t header[kPacketHeaderSize] = {
      kFrameMagic[0],
      kFrameMagic[1],
      kFrameMagic[2],
      kFrameMagic[3],
      static_cast<uint8_t>((frame->len >> 24) & 0xFF),
      static_cast<uint8_t>((frame->len >> 16) & 0xFF),
      static_cast<uint8_t>((frame->len >> 8) & 0xFF),
      static_cast<uint8_t>(frame->len & 0xFF),
  };

  const bool ok =
      writeAll(header, sizeof(header)) && writeAll(frame->buf, frame->len);

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

void setup() {
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
  configureOta();
}

void loop() {
  ArduinoOTA.handle();
  ensureWifi();

  if (ensureTcpConnection()) {
    streamFrame();
  } else {
    delay(10);
  }

  printStats();
  delay(1);
}
