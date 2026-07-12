
#include "esp32_cam_controller.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <cctype>
#include <memory>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp32/spiram.h"
#include "esp_camera.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

#include "app_config.h"
#include "camera_common.h"
#include "camera_pins.h"
#include "esp32_rtsp_streamer.h"

namespace {

WiFiServer g_rtspServer(appcfg::kRtspPort);
WiFiClient g_mqttSocket;
PubSubClient g_mqttClient(g_mqttSocket);
Preferences g_preferences;
std::unique_ptr<Esp32RtspStreamer> g_streamer;

bool g_psramAvailable = false;
bool g_wifiStartIssued = false;
bool g_rtspServerStarted = false;
bool g_mdnsReady = false;
bool g_cameraReady = false;
bool g_streamEnabled = true;

framesize_t g_frameSize = FRAMESIZE_VGA;
int g_jpegQuality = 12;
int g_brightness = 1;
int g_contrast = 0;
int g_saturation = -1;
int g_sharpness = 0;
int g_hmirror = 0;
int g_vflip = 0;
int g_ledEnabled = 0;
int g_streamFps = appcfg::kDefaultRtspFps;

String g_lastStatus;
String g_lastError;

uint32_t g_lastWifiAttemptMs = 0;
uint32_t g_lastMqttAttemptMs = 0;
uint32_t g_lastFrameAtMs = 0;
uint32_t g_lastStatsMs = 0;
uint32_t g_framesSent = 0;
float g_lastMeasuredFps = 0.0f;

int directSessionCount() {
  return g_streamer != nullptr ? g_streamer->sessionCount() : 0;
}

int directStreamingSessionCount() {
  return g_streamer != nullptr ? g_streamer->streamingSessionCount() : 0;
}

const char *frameSizeName() {
  return camcommon::frameSizeName(g_frameSize);
}

framesize_t frameSizeFromIndex(int index) {
  return camcommon::frameSizeFromIndex(index, g_frameSize, false);
}

int frameSizeToIndex(framesize_t size) {
  return camcommon::frameSizeToIndex(size, 1);
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

int clampValue(int value, int minValue, int maxValue) {
  return camcommon::clampInt(value, minValue, maxValue);
}

String mqttTopic(const char *suffix) {
  String topic = appcfg::kMqttBaseTopic;
  topic += "/";
  topic += suffix;
  return topic;
}

String rtspUrl() {
  if (WiFi.status() != WL_CONNECTED) return "";
  return camcommon::rtspUrl(
      WiFi.localIP(), appcfg::kRtspPort, appcfg::kRtspPresentation, appcfg::kRtspStream);
}

void publishStatus(bool forceConfig = false);

void setLastError(const String &message) {
  g_lastError = message;
  publishStatus();
}

void clearLastError() {
  if (g_lastError.length() == 0) {
    return;
  }

  g_lastError = "";
  publishStatus();
}

void recordStatus(const char *message) {
  g_lastStatus = message;
  Serial.printf("[STAT] %s\n", message);
  publishStatus();
}

void statusf(const char *format, ...) {
  char message[appcfg::kStatusLineSize];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  recordStatus(message);
}

String streamState() {
  if (!g_cameraReady) {
    return "camera_error";
  }
  if (WiFi.status() != WL_CONNECTED) {
    return "wifi_down";
  }
  if (!g_streamEnabled) {
    return "paused";
  }
  if (directStreamingSessionCount() > 0) {
    return "streaming";
  }
  return "ready";
}

void applyLedState(int enabled) {
  g_ledEnabled = enabled ? 1 : 0;
  digitalWrite(LED_GPIO_NUM, g_ledEnabled ? HIGH : LOW);
}

void rebuildStreamer() {
  g_streamer.reset();

  if (!g_cameraReady || !g_streamEnabled || WiFi.status() != WL_CONNECTED) {
    return;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr || sensor->status.framesize > FRAMESIZE_QXGA) {
    setLastError("unable to rebuild rtsp streamer");
    return;
  }

  camera_fb_t *probe = esp_camera_fb_get();
  if (probe == nullptr) {
    setLastError("camera probe frame failed");
    return;
  }

  const uint16_t width = static_cast<uint16_t>(probe->width);
  const uint16_t height = static_cast<uint16_t>(probe->height);
  esp_camera_fb_return(probe);

  g_streamer.reset(new Esp32RtspStreamer(width, height));
  String hostPort = WiFi.localIP().toString();
  hostPort += ":";
  hostPort += String(appcfg::kRtspPort);
  g_streamer->setURI(hostPort, appcfg::kRtspPresentation, appcfg::kRtspStream);
  statusf("rtsp ready url=%s", rtspUrl().c_str());
}

void ensureRtspServer() {
  if (g_rtspServerStarted) {
    return;
  }

  g_rtspServer.begin();
  g_rtspServerStarted = true;
  statusf("rtsp server listening port=%u", appcfg::kRtspPort);
}

void publishSimple(const char *suffix, const String &value, bool retain = true) {
  if (!g_mqttClient.connected()) {
    return;
  }
  g_mqttClient.publish(mqttTopic(suffix).c_str(), value.c_str(), retain);
}

String buildConfigJson() {
  String json = "{";
  json += "\"framesize\":";
  json += String(frameSizeToIndex(g_frameSize));
  json += ",\"framesize_name\":\"";
  json += frameSizeName();
  json += "\",\"jpeg_quality\":";
  json += String(g_jpegQuality);
  json += ",\"brightness\":";
  json += String(g_brightness);
  json += ",\"contrast\":";
  json += String(g_contrast);
  json += ",\"saturation\":";
  json += String(g_saturation);
  json += ",\"sharpness\":";
  json += String(g_sharpness);
  json += ",\"hmirror\":";
  json += String(g_hmirror);
  json += ",\"vflip\":";
  json += String(g_vflip);
  json += ",\"led\":";
  json += String(g_ledEnabled);
  json += ",\"stream_fps\":";
  json += String(g_streamFps);
  json += ",\"stream_enabled\":";
  json += g_streamEnabled ? "true" : "false";
  json += ",\"psram\":";
  json += g_psramAvailable ? "true" : "false";
  json += "}";
  return json;
}

void publishStatus(bool forceConfig) {
  static String lastConfig;

  if (!g_mqttClient.connected()) {
    return;
  }

  publishSimple("status/online", "true");
  publishSimple("status/state", streamState());
  publishSimple("status/error", g_lastError);
  publishSimple("status/ip", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "-");
  publishSimple("status/rtsp_url", rtspUrl());
  publishSimple("status/mdns", String(appcfg::kMdnsHostname) + ".local");
  publishSimple("status/last_status", g_lastStatus);
  publishSimple("status/clients", String(directStreamingSessionCount()));
  publishSimple("status/direct_sessions", String(directSessionCount()));
  publishSimple("status/direct_streaming_clients", String(directStreamingSessionCount()));
  publishSimple("status/frame_fps", String(g_lastMeasuredFps, 1));

  const String config = buildConfigJson();
  if (forceConfig || config != lastConfig) {
    publishSimple("status/config", config);
    lastConfig = config;
  }
}

void configureMdns() {
  if (g_mdnsReady || WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (!MDNS.begin(appcfg::kMdnsHostname)) {
    recordStatus("mdns setup failed");
    return;
  }

  MDNS.addService("arduino", "tcp", 3232);
  MDNS.addService("rtsp", "tcp", appcfg::kRtspPort);
  g_mdnsReady = true;
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

bool applySensorSettings(bool rebuildAfter) {
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) {
    setLastError("camera sensor unavailable");
    return false;
  }

  sensor->set_quality(sensor, g_jpegQuality);
  sensor->set_brightness(sensor, g_brightness);
  sensor->set_contrast(sensor, g_contrast);
  sensor->set_saturation(sensor, g_saturation);
  sensor->set_sharpness(sensor, g_sharpness);
  sensor->set_hmirror(sensor, g_hmirror);
  sensor->set_vflip(sensor, g_vflip);
  applyLedState(g_ledEnabled);

  if (rebuildAfter) {
    rebuildStreamer();
  }

  clearLastError();
  publishStatus(true);
  return true;
}

void saveControllerSettings() {
  g_preferences.putInt("framesize", frameSizeToIndex(g_frameSize));
  g_preferences.putInt("quality", g_jpegQuality);
  g_preferences.putInt("fps", g_streamFps);
  g_preferences.putInt("bright", g_brightness);
  g_preferences.putInt("contrast", g_contrast);
  g_preferences.putInt("saturate", g_saturation);
  g_preferences.putInt("sharp", g_sharpness);
  g_preferences.putInt("hmirror", g_hmirror);
  g_preferences.putInt("vflip", g_vflip);
  g_preferences.putBool("led", g_ledEnabled != 0);
  g_preferences.putBool("stream", g_streamEnabled);
}

void loadControllerSettings() {
  g_frameSize = frameSizeFromIndex(clampValue(g_preferences.getInt("framesize", frameSizeToIndex(g_frameSize)), 0, 6));
  g_jpegQuality = clampValue(g_preferences.getInt("quality", g_jpegQuality), 4, 63);
  g_streamFps = clampValue(g_preferences.getInt("fps", appcfg::kDefaultRtspFps), 1, 25);
  g_brightness = clampValue(g_preferences.getInt("bright", 1), -2, 2);
  g_contrast = clampValue(g_preferences.getInt("contrast", 0), -2, 2);
  g_saturation = clampValue(g_preferences.getInt("saturate", -1), -2, 2);
  g_sharpness = clampValue(g_preferences.getInt("sharp", 0), -2, 2);
  g_hmirror = g_preferences.getInt("hmirror", 0) ? 1 : 0;
  g_vflip = g_preferences.getInt("vflip", 0) ? 1 : 0;
  g_ledEnabled = g_preferences.getBool("led", false) ? 1 : 0;
  g_streamEnabled = g_preferences.getBool("stream", true);
}

bool initCamera() {
  g_psramAvailable = psramFound();
  g_frameSize = g_psramAvailable ? FRAMESIZE_SVGA : FRAMESIZE_VGA;
  g_jpegQuality = g_psramAvailable ? 8 : 10;
  loadControllerSettings();
  if (!g_psramAvailable && frameSizeToIndex(g_frameSize) > 1) {
    g_frameSize = FRAMESIZE_VGA;
  }

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
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
  }

  const esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    statusf("camera init failed err=0x%lx", static_cast<uint32_t>(err));
    setLastError("camera init failed");
    return false;
  }

  pinMode(LED_GPIO_NUM, OUTPUT);
  applyLedState(0);

  g_cameraReady = true;
  applySensorSettings(false);
  statusf(
      "camera ready psram=%s frame=%s",
      g_psramAvailable ? "yes" : "no",
      frameSizeName());
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

void mqttCallback(char *topic, byte *payload, unsigned int length);

void ensureMqtt() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  g_mqttClient.loop();
  if (g_mqttClient.connected()) {
    return;
  }

  const uint32_t now = millis();
  if (now - g_lastMqttAttemptMs < appcfg::kMqttRetryMs) {
    return;
  }
  g_lastMqttAttemptMs = now;

  g_mqttClient.setServer(appcfg::kMqttHost, appcfg::kMqttPort);
  g_mqttClient.setCallback(mqttCallback);
  g_mqttClient.setBufferSize(512);

  const char *willTopic = mqttTopic("status/online").c_str();
  bool connected = false;
  if (strlen(appcfg::kMqttUsername) > 0) {
    connected = g_mqttClient.connect(
        appcfg::kMqttClientId,
        appcfg::kMqttUsername,
        appcfg::kMqttPassword,
        willTopic,
        1,
        true,
        "false");
  } else {
    connected = g_mqttClient.connect(
        appcfg::kMqttClientId,
        willTopic,
        1,
        true,
        "false");
  }

  if (!connected) {
    statusf("mqtt connect failed rc=%d", g_mqttClient.state());
    return;
  }

  g_mqttClient.subscribe(mqttTopic("cmd/start").c_str());
  g_mqttClient.subscribe(mqttTopic("cmd/stop").c_str());
  g_mqttClient.subscribe(mqttTopic("cmd/restart").c_str());
  g_mqttClient.subscribe(mqttTopic("cmd/set").c_str());
  g_mqttClient.subscribe(mqttTopic("cmd/ping").c_str());
  statusf("mqtt connected topic=%s", appcfg::kMqttBaseTopic);
  publishStatus(true);
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
      configureMdns();
      ensureRtspServer();
      rebuildStreamer();
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      statusf("wifi disconnected reason=%d", info.wifi_sta_disconnected.reason);
      g_streamer.reset();
      break;
    default:
      break;
  }
}

bool extractPayloadToken(const String &payload, const String &key, String &token) {
  return camcommon::extractPayloadToken(payload, key.c_str(), token);
}

bool extractPayloadInt(const String &payload, const char *key, int &value) {
  return camcommon::extractPayloadInt(payload, key, value);
}

bool applySettingByKey(const char *key, int value, bool &rebuildRequired) {
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) {
    setLastError("camera sensor unavailable");
    return false;
  }

  if (strcmp(key, "framesize") == 0) {
    const framesize_t newSize = frameSizeFromIndex(value);
    if (newSize != g_frameSize && sensor->set_framesize(sensor, newSize) == 0) {
      g_frameSize = newSize;
      rebuildRequired = true;
      return true;
    }
    return false;
  }
  if (strcmp(key, "jpeg_quality") == 0) {
    const int bounded = clampValue(value, 4, 63);
    if (bounded != g_jpegQuality && sensor->set_quality(sensor, bounded) == 0) {
      g_jpegQuality = bounded;
      return true;
    }
    return false;
  }
  if (strcmp(key, "brightness") == 0) {
    const int bounded = clampValue(value, -2, 2);
    if (bounded != g_brightness && sensor->set_brightness(sensor, bounded) == 0) {
      g_brightness = bounded;
      return true;
    }
    return false;
  }
  if (strcmp(key, "contrast") == 0) {
    const int bounded = clampValue(value, -2, 2);
    if (bounded != g_contrast && sensor->set_contrast(sensor, bounded) == 0) {
      g_contrast = bounded;
      return true;
    }
    return false;
  }
  if (strcmp(key, "saturation") == 0) {
    const int bounded = clampValue(value, -2, 2);
    if (bounded != g_saturation && sensor->set_saturation(sensor, bounded) == 0) {
      g_saturation = bounded;
      return true;
    }
    return false;
  }
  if (strcmp(key, "sharpness") == 0) {
    const int bounded = clampValue(value, -2, 2);
    if (bounded != g_sharpness && sensor->set_sharpness(sensor, bounded) == 0) {
      g_sharpness = bounded;
      return true;
    }
    return false;
  }
  if (strcmp(key, "hmirror") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded != g_hmirror && sensor->set_hmirror(sensor, bounded) == 0) {
      g_hmirror = bounded;
      return true;
    }
    return false;
  }
  if (strcmp(key, "vflip") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded != g_vflip && sensor->set_vflip(sensor, bounded) == 0) {
      g_vflip = bounded;
      return true;
    }
    return false;
  }
  if (strcmp(key, "led") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded != g_ledEnabled) {
      applyLedState(bounded);
      return true;
    }
    return false;
  }
  if (strcmp(key, "stream_fps") == 0) {
    const int bounded = clampValue(value, 1, 25);
    if (bounded != g_streamFps) {
      g_streamFps = bounded;
      return true;
    }
    return false;
  }
  if (strcmp(key, "stream_enabled") == 0) {
    const bool enabled = value != 0;
    if (enabled != g_streamEnabled) {
      g_streamEnabled = enabled;
      rebuildRequired = true;
      return true;
    }
    return false;
  }

  return false;
}

void applyControlPayload(const String &payload) {
  bool changed = false;
  bool rebuildRequired = false;
  int value = 0;

  const char *keys[] = {
      "framesize",
      "jpeg_quality",
      "brightness",
      "contrast",
      "saturation",
      "sharpness",
      "hmirror",
      "vflip",
      "led",
      "stream_fps",
      "stream_enabled",
  };

  for (const char *key : keys) {
    if (!extractPayloadInt(payload, key, value)) {
      continue;
    }

    if (applySettingByKey(key, value, rebuildRequired)) {
      changed = true;
    }
  }

  if (rebuildRequired) {
    rebuildStreamer();
  }

  if (changed) {
    saveControllerSettings();
    clearLastError();
    statusf(
        "camera control applied frame=%s q=%d fps=%d led=%d",
        frameSizeName(),
        g_jpegQuality,
        g_streamFps,
        g_ledEnabled);
    publishStatus(true);
  } else {
    publishStatus();
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  const String topicText(topic);
  String payloadText;
  payloadText.reserve(length + 1);
  for (unsigned int i = 0; i < length; ++i) {
    payloadText += static_cast<char>(payload[i]);
  }
  payloadText.trim();

  if (topicText == mqttTopic("cmd/start")) {
    g_streamEnabled = true;
    saveControllerSettings();
    rebuildStreamer();
    recordStatus("mqtt start command");
    return;
  }

  if (topicText == mqttTopic("cmd/stop")) {
    g_streamEnabled = false;
    saveControllerSettings();
    rebuildStreamer();
    recordStatus("mqtt stop command");
    return;
  }

  if (topicText == mqttTopic("cmd/restart")) {
    recordStatus("mqtt restart command");
    delay(100);
    ESP.restart();
    return;
  }

  if (topicText == mqttTopic("cmd/ping")) {
    publishSimple("status/pong", payloadText.length() > 0 ? payloadText : "pong");
    return;
  }

  if (topicText == mqttTopic("cmd/set")) {
    applyControlPayload(payloadText);
    return;
  }
}

void printRuntimeStats() {
  const uint32_t now = millis();
  if (now - g_lastStatsMs < 5000) {
    return;
  }

  const float seconds =
      (g_lastStatsMs == 0) ? 5.0f : (now - g_lastStatsMs) / 1000.0f;
  const float fps = g_framesSent / seconds;
  g_lastMeasuredFps = fps;
  g_lastStatsMs = now;
  g_framesSent = 0;

  statusf(
      "runtime wifi=%s ip=%s mqtt=%s rtsp=%s sessions=%d fps=%.1f",
      WiFi.status() == WL_CONNECTED ? "up" : "down",
      WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-",
      g_mqttClient.connected() ? "up" : "down",
      g_streamEnabled ? "on" : "off",
      directStreamingSessionCount(),
      fps);
}

void handleRtspLoop() {
  if (!g_rtspServerStarted) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED || !g_cameraReady) {
    return;
  }

  if (!g_streamEnabled) {
    WiFiClient waitingClient = g_rtspServer.accept();
    if (waitingClient) {
      waitingClient.stop();
    }
    return;
  }

  if (g_streamer == nullptr) {
    rebuildStreamer();
    if (g_streamer == nullptr) {
      return;
    }
  }

  g_streamer->handleRequests(0);

  const uint32_t now = millis();
  const uint32_t frameIntervalMs =
      static_cast<uint32_t>(1000UL / static_cast<uint32_t>(max(1, g_streamFps)));
  if (g_streamer->anySessions() &&
      (now - g_lastFrameAtMs >= frameIntervalMs || now < g_lastFrameAtMs)) {
    g_streamer->streamImage(now);
    g_lastFrameAtMs = now;
    ++g_framesSent;
  }

  WiFiClient rtspClient = g_rtspServer.accept();
  if (rtspClient) {
    rtspClient.setNoDelay(true);
    g_streamer->addSession(new WiFiClient(rtspClient));
    statusf("rtsp client=%s", rtspClient.remoteIP().toString().c_str());
  }
}

}  // namespace

namespace esp32cam {

void setupController() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  Serial.setDebugOutput(false);
  Serial.println();
  Serial.println("[BOOT] ESP32-CAM RTSP controller starting");

  g_preferences.begin("aicam", false);

  logBootDiagnostics();

  if (!initCamera()) {
    Serial.println("[BOOT] Camera init failed, reboot required");
    return;
  }

  logPsramDiagnostics();

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);

  ensureWifi();
  configureOta();
  ensureRtspServer();
}

void loopController() {
  ArduinoOTA.handle();
  ensureWifi();
  configureMdns();
  ensureMqtt();
  handleRtspLoop();
  printRuntimeStats();
  delay(1);
}

}  // namespace esp32cam
