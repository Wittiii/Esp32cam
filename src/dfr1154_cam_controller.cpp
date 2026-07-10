#include "dfr1154_cam_controller.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <DFRobot_LTR308.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <Wire.h>
#include <esp_camera.h>
#include <math.h>
#include <memory>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dfr1154_config.h"
#include "dfr1154_pins.h"
#include "dfr1154_timelapse.h"
#include "esp32_rtsp_streamer.h"

namespace {

WiFiServer g_rtspServer(dfrcfg::kRtspPort);
WiFiClient g_mqttSocket;
PubSubClient g_mqttClient(g_mqttSocket);
Preferences g_preferences;
DFRobot_LTR308 g_lightSensor;
std::unique_ptr<Esp32RtspStreamer> g_streamer;

bool g_cameraReady = false;
bool g_psramReady = false;
bool g_lightReady = false;
bool g_wifiStarted = false;
bool g_rtspServerStarted = false;
bool g_mdnsReady = false;
bool g_timeConfigured = false;
bool g_streamEnabled = true;
bool g_networkServicesPending = false;

framesize_t g_frameSize = FRAMESIZE_SVGA;
int g_jpegQuality = 10;
int g_brightness = 1;
int g_contrast = 0;
int g_saturation = -2;
int g_sharpness = 0;
int g_hmirror = 0;
int g_vflip = 1;
int g_ledEnabled = 0;
int g_streamFps = dfrcfg::kDefaultRtspFps;

// 0 = off, 1 = on, 2 = automatic from the LTR-308.
int g_irMode = 2;
bool g_irEnabled = false;
float g_irOnBelowLux = dfrcfg::kDefaultIrOnBelowLux;
float g_irOffAboveLux = dfrcfg::kDefaultIrOffAboveLux;
float g_ambientLux = NAN;

String g_lastStatus = "booting";
String g_lastError;
uint32_t g_lastWifiAttemptMs = 0;
uint32_t g_lastMqttAttemptMs = 0;
uint32_t g_lastFrameAtMs = 0;
uint32_t g_lastStatsMs = 0;
uint32_t g_lastLightReadMs = 0;
uint32_t g_framesSent = 0;
float g_lastMeasuredFps = 0.0f;

int clampValue(int value, int minimum, int maximum) {
  return value < minimum ? minimum : (value > maximum ? maximum : value);
}

String uint64String(uint64_t value) {
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  return String(buffer);
}

String mqttTopic(const char *suffix) {
  return String(dfrcfg::kMqttBaseTopic) + "/" + suffix;
}

int directSessionCount() {
  return g_streamer != nullptr ? g_streamer->sessionCount() : 0;
}

int directStreamingSessionCount() {
  return g_streamer != nullptr ? g_streamer->streamingSessionCount() : 0;
}

const char *frameSizeName() {
  switch (g_frameSize) {
    case FRAMESIZE_QVGA: return "QVGA";
    case FRAMESIZE_VGA: return "VGA";
    case FRAMESIZE_SVGA: return "SVGA";
    case FRAMESIZE_XGA: return "XGA";
    case FRAMESIZE_HD: return "HD";
    case FRAMESIZE_SXGA: return "SXGA";
    case FRAMESIZE_UXGA: return "UXGA";
    case FRAMESIZE_QXGA: return "QXGA";
    default: return "OTHER";
  }
}

framesize_t frameSizeFromIndex(int index) {
  switch (index) {
    case 0: return FRAMESIZE_QVGA;
    case 1: return FRAMESIZE_VGA;
    case 2: return FRAMESIZE_SVGA;
    case 3: return FRAMESIZE_XGA;
    case 4: return FRAMESIZE_HD;
    case 5: return FRAMESIZE_SXGA;
    case 6: return FRAMESIZE_UXGA;
    case 7: return FRAMESIZE_QXGA;
    default: return g_frameSize;
  }
}

int frameSizeToIndex(framesize_t size) {
  switch (size) {
    case FRAMESIZE_QVGA: return 0;
    case FRAMESIZE_VGA: return 1;
    case FRAMESIZE_SVGA: return 2;
    case FRAMESIZE_XGA: return 3;
    case FRAMESIZE_HD: return 4;
    case FRAMESIZE_SXGA: return 5;
    case FRAMESIZE_UXGA: return 6;
    case FRAMESIZE_QXGA: return 7;
    default: return 2;
  }
}

const char *irModeName() {
  switch (g_irMode) {
    case 0: return "off";
    case 1: return "on";
    default: return "auto";
  }
}

String rtspUrl() {
  if (WiFi.status() != WL_CONNECTED) return "";
  return "rtsp://" + WiFi.localIP().toString() + ":" + String(dfrcfg::kRtspPort) + "/" +
         dfrcfg::kRtspPresentation + "/" + dfrcfg::kRtspStream;
}

String archiveUrl() {
  if (WiFi.status() != WL_CONNECTED) return "";
  return "http://" + WiFi.localIP().toString() + ":" + String(dfrcfg::kArchivePort);
}

String streamState() {
  if (!g_cameraReady) return "camera_error";
  if (WiFi.status() != WL_CONNECTED) return "wifi_down";
  if (!g_streamEnabled) return "paused";
  return directStreamingSessionCount() > 0 ? "streaming" : "ready";
}

void publishStatus(bool forceConfig = false);
void configureLightSensor();

void recordStatus(const String &message) {
  g_lastStatus = message;
  Serial.printf("[STAT] %s\n", message.c_str());
}

void statusf(const char *format, ...) {
  char buffer[192];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  recordStatus(buffer);
}

void setError(const String &message) {
  if (g_lastError == message) return;
  g_lastError = message;
  Serial.printf("[ERROR] %s\n", message.c_str());
  publishStatus();
}

void clearError() {
  if (g_lastError.length() == 0) return;
  g_lastError = "";
  publishStatus();
}

void applyLedState(bool enabled) {
  g_ledEnabled = enabled ? 1 : 0;
  digitalWrite(DFR_LED_PIN, enabled ? HIGH : LOW);
}

void applyIrState(bool enabled) {
  if (g_irEnabled == enabled) return;
  g_irEnabled = enabled;
  digitalWrite(DFR_IR_PIN, enabled ? HIGH : LOW);
  statusf("ir %s mode=%s lux=%.2f", enabled ? "on" : "off", irModeName(), g_ambientLux);
}

void evaluateIrAutomation() {
  if (g_irMode == 0) {
    applyIrState(false);
    return;
  }
  if (g_irMode == 1) {
    applyIrState(true);
    return;
  }
  if (!g_lightReady || !isfinite(g_ambientLux)) return;

  if (!g_irEnabled && g_ambientLux <= g_irOnBelowLux) {
    applyIrState(true);
  } else if (g_irEnabled && g_ambientLux >= g_irOffAboveLux) {
    applyIrState(false);
  }
}

bool readAmbientLuxNow() {
  if (!g_lightReady) return false;
  const uint32_t raw = g_lightSensor.getData();
  const double lux = g_lightSensor.getLux(raw);
  if (!isfinite(lux) || lux < 0.0) return false;
  g_ambientLux = static_cast<float>(lux);
  evaluateIrAutomation();
  return true;
}

void handleLightSensor() {
  if (!g_lightReady) return;
  const uint32_t now = millis();
  if (now - g_lastLightReadMs < dfrcfg::kLightReadIntervalMs) return;
  g_lastLightReadMs = now;
  readAmbientLuxNow();
}

void observeJpegFrame(
    const uint8_t *data,
    size_t length,
    uint16_t width,
    uint16_t height,
    uint32_t capturedAtMs) {
  dfrtimelapse::observeJpegFrame(data, length, width, height, capturedAtMs);
}

void rebuildStreamer() {
  g_streamer.reset();
  if (!g_cameraReady || !g_streamEnabled || WiFi.status() != WL_CONNECTED) return;

  camera_fb_t *probe = esp_camera_fb_get();
  if (probe == nullptr) {
    setError("camera probe frame failed");
    return;
  }
  const uint16_t width = probe->width;
  const uint16_t height = probe->height;
  esp_camera_fb_return(probe);

  g_streamer.reset(new Esp32RtspStreamer(width, height, observeJpegFrame));
  const String hostPort = WiFi.localIP().toString() + ":" + String(dfrcfg::kRtspPort);
  g_streamer->setURI(hostPort, dfrcfg::kRtspPresentation, dfrcfg::kRtspStream);
  statusf("rtsp ready url=%s", rtspUrl().c_str());
}

void ensureRtspServer() {
  if (g_rtspServerStarted) return;
  g_rtspServer.begin();
  g_rtspServerStarted = true;
  statusf("rtsp server listening port=%u", dfrcfg::kRtspPort);
}

void publishSimple(const char *suffix, const String &value, bool retain = true) {
  if (g_mqttClient.connected()) {
    g_mqttClient.publish(mqttTopic(suffix).c_str(), value.c_str(), retain);
  }
}

String buildConfigJson() {
  String json = "{";
  json += "\"framesize\":" + String(frameSizeToIndex(g_frameSize));
  json += ",\"framesize_name\":\"" + String(frameSizeName()) + "\"";
  json += ",\"jpeg_quality\":" + String(g_jpegQuality);
  json += ",\"stream_fps\":" + String(g_streamFps);
  json += ",\"brightness\":" + String(g_brightness);
  json += ",\"contrast\":" + String(g_contrast);
  json += ",\"saturation\":" + String(g_saturation);
  json += ",\"sharpness\":" + String(g_sharpness);
  json += ",\"hmirror\":" + String(g_hmirror);
  json += ",\"vflip\":" + String(g_vflip);
  json += ",\"led\":" + String(g_ledEnabled);
  json += ",\"stream_enabled\":";
  json += g_streamEnabled ? "true" : "false";
  json += ",\"ir_mode\":" + String(g_irMode);
  json += ",\"ir_mode_name\":\"" + String(irModeName()) + "\"";
  json += ",\"ir_enabled\":";
  json += g_irEnabled ? "true" : "false";
  json += ",\"ir_on_lux\":" + String(g_irOnBelowLux, 1);
  json += ",\"ir_off_lux\":" + String(g_irOffAboveLux, 1);
  json += ",\"timelapse_enabled\":";
  json += dfrtimelapse::enabled() ? "true" : "false";
  json += ",\"timelapse_interval_seconds\":" + String(dfrtimelapse::intervalSeconds());
  json += ",\"timelapse_limit_gb\":" +
          String(static_cast<double>(dfrtimelapse::storageLimitBytes()) / (1024.0 * 1024.0 * 1024.0), 1);
  json += ",\"psram\":";
  json += g_psramReady ? "true" : "false";
  json += "}";
  return json;
}

void publishStatus(bool forceConfig) {
  static String previousConfig;
  if (!g_mqttClient.connected()) return;

  publishSimple("status/online", "true");
  publishSimple("status/state", streamState());
  publishSimple("status/error", g_lastError);
  publishSimple("status/ip", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "-");
  publishSimple("status/rtsp_url", rtspUrl());
  publishSimple("status/archive_url", archiveUrl());
  publishSimple("status/mdns", String(dfrcfg::kMdnsHostname) + ".local");
  publishSimple("status/last_status", g_lastStatus);
  publishSimple("status/clients", String(directStreamingSessionCount()));
  publishSimple("status/direct_sessions", String(directSessionCount()));
  publishSimple("status/direct_streaming_clients", String(directStreamingSessionCount()));
  publishSimple("status/frame_fps", String(g_lastMeasuredFps, 1));
  publishSimple("status/ambient_lux", isfinite(g_ambientLux) ? String(g_ambientLux, 2) : "-");
  publishSimple("status/ir_mode", irModeName());
  publishSimple("status/ir_enabled", g_irEnabled ? "true" : "false");
  publishSimple("status/light_sensor", g_lightReady ? "ready" : "unavailable");
  publishSimple("status/sd", dfrtimelapse::sdReady() ? "ready" : "unavailable");

  publishSimple("status/timelapse/state", dfrtimelapse::state());
  publishSimple("status/timelapse/error", dfrtimelapse::error());
  publishSimple("status/timelapse/storage_bytes", uint64String(dfrtimelapse::storageBytes()));
  publishSimple("status/timelapse/storage_limit_bytes", uint64String(dfrtimelapse::storageLimitBytes()));
  publishSimple("status/timelapse/card_total_bytes", uint64String(dfrtimelapse::cardTotalBytes()));
  publishSimple("status/timelapse/card_used_bytes", uint64String(dfrtimelapse::cardUsedBytes()));
  publishSimple("status/timelapse/last_image", dfrtimelapse::lastImage());
  publishSimple("status/timelapse/output_dir", dfrcfg::kTimelapseDirectory);
  publishSimple("status/timelapse/enabled", dfrtimelapse::enabled() ? "true" : "false");
  publishSimple("status/timelapse/interval_seconds", String(dfrtimelapse::intervalSeconds()));

  const String config = buildConfigJson();
  if (forceConfig || config != previousConfig) {
    publishSimple("status/config", config);
    previousConfig = config;
  }
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
  g_preferences.putInt("irmode", g_irMode);
  g_preferences.putFloat("ironlux", g_irOnBelowLux);
  g_preferences.putFloat("irofflux", g_irOffAboveLux);
}

void loadControllerSettings() {
  g_frameSize = frameSizeFromIndex(g_preferences.getInt("framesize", 2));
  g_jpegQuality = clampValue(g_preferences.getInt("quality", 10), 4, 63);
  g_streamFps = clampValue(g_preferences.getInt("fps", dfrcfg::kDefaultRtspFps), 1, 20);
  g_brightness = clampValue(g_preferences.getInt("bright", 1), -2, 2);
  g_contrast = clampValue(g_preferences.getInt("contrast", 0), -2, 2);
  g_saturation = clampValue(g_preferences.getInt("saturate", -2), -2, 2);
  g_sharpness = clampValue(g_preferences.getInt("sharp", 0), -2, 2);
  g_hmirror = g_preferences.getInt("hmirror", 0) ? 1 : 0;
  g_vflip = g_preferences.getInt("vflip", 1) ? 1 : 0;
  g_ledEnabled = g_preferences.getBool("led", false) ? 1 : 0;
  g_streamEnabled = g_preferences.getBool("stream", true);
  g_irMode = clampValue(g_preferences.getInt("irmode", 2), 0, 2);
  g_irOnBelowLux = g_preferences.getFloat("ironlux", dfrcfg::kDefaultIrOnBelowLux);
  g_irOffAboveLux = g_preferences.getFloat("irofflux", dfrcfg::kDefaultIrOffAboveLux);
  if (g_irOffAboveLux <= g_irOnBelowLux) g_irOffAboveLux = g_irOnBelowLux + 2.0f;
}

bool applySensorSettings(bool rebuildAfter) {
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) {
    setError("camera sensor unavailable");
    return false;
  }
  if (sensor->set_framesize(sensor, g_frameSize) != 0) {
    setError("camera framesize apply failed");
    return false;
  }
  if (sensor->set_quality(sensor, g_jpegQuality) != 0) {
    setError("camera quality apply failed");
    return false;
  }
  if (sensor->set_brightness(sensor, g_brightness) != 0) {
    setError("camera brightness apply failed");
    return false;
  }
  if (sensor->set_contrast(sensor, g_contrast) != 0) {
    setError("camera contrast apply failed");
    return false;
  }
  if (sensor->set_saturation(sensor, g_saturation) != 0) {
    setError("camera saturation apply failed");
    return false;
  }
  if (sensor->set_sharpness(sensor, g_sharpness) != 0) {
    setError("camera sharpness apply failed");
    return false;
  }
  if (sensor->set_hmirror(sensor, g_hmirror) != 0) {
    setError("camera hmirror apply failed");
    return false;
  }
  if (sensor->set_vflip(sensor, g_vflip) != 0) {
    setError("camera vflip apply failed");
    return false;
  }
  applyLedState(g_ledEnabled != 0);
  evaluateIrAutomation();
  if (rebuildAfter) rebuildStreamer();
  clearError();
  return true;
}

bool initCamera() {
  g_psramReady = psramFound();
  if (!g_psramReady) {
    setError("DFR1154 PSRAM not detected");
    return false;
  }

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = DFR_CAM_Y2;
  config.pin_d1 = DFR_CAM_Y3;
  config.pin_d2 = DFR_CAM_Y4;
  config.pin_d3 = DFR_CAM_Y5;
  config.pin_d4 = DFR_CAM_Y6;
  config.pin_d5 = DFR_CAM_Y7;
  config.pin_d6 = DFR_CAM_Y8;
  config.pin_d7 = DFR_CAM_Y9;
  config.pin_xclk = DFR_CAM_XCLK;
  config.pin_pclk = DFR_CAM_PCLK;
  config.pin_vsync = DFR_CAM_VSYNC;
  config.pin_href = DFR_CAM_HREF;
  config.pin_sccb_sda = DFR_CAM_SIOD;
  config.pin_sccb_scl = DFR_CAM_SIOC;
  config.pin_pwdn = DFR_CAM_PWDN;
  config.pin_reset = DFR_CAM_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = g_frameSize;
  config.jpeg_quality = g_jpegQuality;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  const esp_err_t result = esp_camera_init(&config);
  if (result != ESP_OK) {
    setError("camera init failed: 0x" + String(static_cast<uint32_t>(result), HEX));
    return false;
  }

  g_cameraReady = true;
  applySensorSettings(false);
  statusf("camera ready sensor=OV3660 psram=%luMB frame=%s", ESP.getPsramSize() / (1024UL * 1024UL), frameSizeName());
  return true;
}

void shutdownCamera() {
  g_streamer.reset();
  if (!g_cameraReady) return;
  esp_camera_deinit();
  g_cameraReady = false;
}

bool restartCameraPipeline() {
  recordStatus("camera reconfiguring");
  shutdownCamera();
  delay(50);
  if (!initCamera()) return false;
  configureLightSensor();
  g_lastLightReadMs = 0;
  readAmbientLuxNow();
  rebuildStreamer();
  clearError();
  return true;
}

void configureLightSensor() {
  // DFRobot requires camera initialization before starting the shared SCCB/I2C bus.
  g_lightReady = false;
  g_ambientLux = NAN;
  Wire.end();
  delay(5);
  Wire.begin(DFR_CAM_SIOD, DFR_CAM_SIOC);
  g_lightReady = g_lightSensor.begin();
  if (!g_lightReady) {
    recordStatus("LTR-308 light sensor unavailable; IR auto mode suspended");
    return;
  }
  g_lightSensor.setMeasurementRate(
      DFRobot_LTR308::eConversion_100ms_18b,
      DFRobot_LTR308::eRate_500ms);
  g_lastLightReadMs = 0;
  readAmbientLuxNow();
  statusf("LTR-308 ready");
}

void configureOta() {
  ArduinoOTA.setHostname(dfrcfg::kOtaHostname);
  if (strlen(dfrcfg::kOtaPassword) > 0) ArduinoOTA.setPassword(dfrcfg::kOtaPassword);
  ArduinoOTA.onStart([]() { recordStatus("ota update started"); });
  ArduinoOTA.onEnd([]() { recordStatus("ota update finished"); });
  ArduinoOTA.onError([](ota_error_t error) {
    setError("ota error=" + String(static_cast<uint32_t>(error)));
  });
  ArduinoOTA.begin();
  statusf("ota ready host=%s", dfrcfg::kOtaHostname);
}

void configureMdns() {
  if (g_mdnsReady || WiFi.status() != WL_CONNECTED) return;
  if (!MDNS.begin(dfrcfg::kMdnsHostname)) {
    recordStatus("mdns setup failed");
    return;
  }
  MDNS.addService("rtsp", "tcp", dfrcfg::kRtspPort);
  MDNS.addService("http", "tcp", dfrcfg::kArchivePort);
  g_mdnsReady = true;
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      statusf("wifi ip=%s", WiFi.localIP().toString().c_str());
      if (!g_timeConfigured) {
        configTzTime(dfrcfg::kTimezone, dfrcfg::kNtpServer1, dfrcfg::kNtpServer2);
        g_timeConfigured = true;
      }
      g_networkServicesPending = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      statusf("wifi disconnected reason=%d", info.wifi_sta_disconnected.reason);
      g_streamer.reset();
      MDNS.end();
      g_mdnsReady = false;
      g_networkServicesPending = false;
      break;
    default:
      break;
  }
}

void handleNetworkServices() {
  if (!g_networkServicesPending || WiFi.status() != WL_CONNECTED) return;
  g_networkServicesPending = false;
  configureMdns();
  ensureRtspServer();
  dfrtimelapse::startHttpServer();
  rebuildStreamer();
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  const uint32_t now = millis();
  if (now - g_lastWifiAttemptMs < dfrcfg::kWifiRetryMs) return;
  g_lastWifiAttemptMs = now;

  if (!g_wifiStarted) {
    g_wifiStarted = true;
    statusf("wifi connecting ssid=%s", dfrcfg::kWifiSsid);
    WiFi.begin(dfrcfg::kWifiSsid, dfrcfg::kWifiPassword);
  } else {
    WiFi.reconnect();
  }
}

bool extractPayloadToken(const String &payload, const String &key, String &token) {
  const String jsonKey = "\"" + key + "\"";
  int keyPosition = payload.indexOf(jsonKey);
  if (keyPosition < 0) return false;
  int colon = payload.indexOf(':', keyPosition + jsonKey.length());
  if (colon < 0) return false;

  int start = colon + 1;
  while (start < payload.length() && isspace(static_cast<unsigned char>(payload[start]))) ++start;
  if (start >= payload.length()) return false;

  if (payload[start] == '"') {
    const int end = payload.indexOf('"', start + 1);
    if (end < 0) return false;
    token = payload.substring(start + 1, end);
    return true;
  }

  int end = start;
  while (end < payload.length() && payload[end] != ',' && payload[end] != '}') ++end;
  token = payload.substring(start, end);
  token.trim();
  return token.length() > 0;
}

bool extractPayloadInt(const String &payload, const char *key, int &value) {
  String token;
  if (!extractPayloadToken(payload, key, token)) return false;
  token.toLowerCase();
  if (token == "true" || token == "on") {
    value = 1;
    return true;
  }
  if (token == "false" || token == "off") {
    value = 0;
    return true;
  }
  value = token.toInt();
  return true;
}

bool applySetting(const char *key, int value, bool &streamRebuildRequired, bool &cameraRestartRequired) {
  if (strcmp(key, "framesize") == 0) {
    const framesize_t bounded = frameSizeFromIndex(clampValue(value, 0, 7));
    if (bounded == g_frameSize) return false;
    g_frameSize = bounded;
    cameraRestartRequired = true;
  } else if (strcmp(key, "jpeg_quality") == 0) {
    const int bounded = clampValue(value, 4, 63);
    if (bounded == g_jpegQuality) return false;
    g_jpegQuality = bounded;
    cameraRestartRequired = true;
  } else if (strcmp(key, "stream_fps") == 0) {
    const int bounded = clampValue(value, 1, 20);
    if (bounded == g_streamFps) return false;
    g_streamFps = bounded;
  } else if (strcmp(key, "brightness") == 0) {
    const int bounded = clampValue(value, -2, 2);
    if (bounded == g_brightness) return false;
    g_brightness = bounded;
    cameraRestartRequired = true;
  } else if (strcmp(key, "contrast") == 0) {
    const int bounded = clampValue(value, -2, 2);
    if (bounded == g_contrast) return false;
    g_contrast = bounded;
    cameraRestartRequired = true;
  } else if (strcmp(key, "saturation") == 0) {
    const int bounded = clampValue(value, -2, 2);
    if (bounded == g_saturation) return false;
    g_saturation = bounded;
    cameraRestartRequired = true;
  } else if (strcmp(key, "sharpness") == 0) {
    const int bounded = clampValue(value, -2, 2);
    if (bounded == g_sharpness) return false;
    g_sharpness = bounded;
    cameraRestartRequired = true;
  } else if (strcmp(key, "hmirror") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded == g_hmirror) return false;
    g_hmirror = bounded;
    cameraRestartRequired = true;
  } else if (strcmp(key, "vflip") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded == g_vflip) return false;
    g_vflip = bounded;
    cameraRestartRequired = true;
  } else if (strcmp(key, "led") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded == g_ledEnabled) return false;
    applyLedState(bounded != 0);
  } else if (strcmp(key, "stream_enabled") == 0) {
    const bool enabled = value != 0;
    if (enabled == g_streamEnabled) return false;
    g_streamEnabled = enabled;
    streamRebuildRequired = true;
  } else if (strcmp(key, "ir_mode") == 0) {
    const int bounded = clampValue(value, 0, 2);
    if (bounded == g_irMode) return false;
    g_irMode = bounded;
    evaluateIrAutomation();
  } else if (strcmp(key, "ir_on_lux") == 0) {
    const float bounded = static_cast<float>(clampValue(value, 0, 100000));
    if (fabsf(bounded - g_irOnBelowLux) < 0.01f) return false;
    g_irOnBelowLux = bounded;
    if (g_irOffAboveLux <= g_irOnBelowLux) g_irOffAboveLux = g_irOnBelowLux + 2.0f;
    evaluateIrAutomation();
  } else if (strcmp(key, "ir_off_lux") == 0) {
    const float bounded = static_cast<float>(clampValue(value, 1, 100000));
    if (fabsf(bounded - g_irOffAboveLux) < 0.01f) return false;
    g_irOffAboveLux = bounded;
    if (g_irOffAboveLux <= g_irOnBelowLux) g_irOnBelowLux = max(0.0f, g_irOffAboveLux - 2.0f);
    evaluateIrAutomation();
  } else if (strcmp(key, "timelapse_enabled") == 0) {
    const bool enabled = value != 0;
    if (enabled == dfrtimelapse::enabled()) return false;
    dfrtimelapse::setEnabled(enabled);
  } else if (strcmp(key, "timelapse_interval_seconds") == 0) {
    const uint32_t bounded = static_cast<uint32_t>(max(5, value));
    if (bounded == dfrtimelapse::intervalSeconds()) return false;
    dfrtimelapse::setIntervalSeconds(bounded);
  } else if (strcmp(key, "timelapse_limit_gb") == 0) {
    const uint64_t bounded = static_cast<uint64_t>(max(1, value)) * 1024ULL * 1024ULL * 1024ULL;
    if (bounded == dfrtimelapse::storageLimitBytes()) return false;
    dfrtimelapse::setStorageLimitBytes(bounded);
  } else {
    return false;
  }
  return true;
}

void applyControlPayload(const String &payload) {
  static const char *keys[] = {
      "framesize", "jpeg_quality", "stream_fps", "brightness", "contrast", "saturation",
      "sharpness", "hmirror", "vflip", "led", "stream_enabled", "ir_mode", "ir_on_lux",
      "ir_off_lux", "timelapse_enabled", "timelapse_interval_seconds", "timelapse_limit_gb"};

  bool changed = false;
  bool streamRebuildRequired = false;
  bool cameraRestartRequired = false;
  for (const char *key : keys) {
    int value = 0;
    if (extractPayloadInt(payload, key, value) &&
        applySetting(key, value, streamRebuildRequired, cameraRestartRequired)) {
      changed = true;
    }
  }

  if (!changed) {
    setError("no supported setting in MQTT payload");
    return;
  }
  saveControllerSettings();
  if (cameraRestartRequired) {
    if (!restartCameraPipeline()) return;
  } else if (streamRebuildRequired) {
    rebuildStreamer();
  }
  clearError();
  publishStatus(true);
}

void applyTimelapsePayload(const String &payload) {
  int value = 0;
  bool changed = false;
  if (extractPayloadInt(payload, "enabled", value)) {
    dfrtimelapse::setEnabled(value != 0);
    changed = true;
  }
  if (extractPayloadInt(payload, "interval_seconds", value)) {
    dfrtimelapse::setIntervalSeconds(static_cast<uint32_t>(max(5, value)));
    changed = true;
  }
  if (extractPayloadInt(payload, "max_storage_gb", value)) {
    dfrtimelapse::setStorageLimitBytes(static_cast<uint64_t>(max(1, value)) * 1024ULL * 1024ULL * 1024ULL);
    changed = true;
  }
  if (!changed) setError("no supported timelapse setting in MQTT payload");
  publishStatus(true);
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  const String topicText(topic);
  String payloadText;
  payloadText.reserve(length + 1);
  for (unsigned int index = 0; index < length; ++index) payloadText += static_cast<char>(payload[index]);
  Serial.printf("[MQTT] %s: %s\n", topicText.c_str(), payloadText.c_str());

  if (topicText == mqttTopic("cmd/start")) {
    g_streamEnabled = true;
    saveControllerSettings();
    rebuildStreamer();
  } else if (topicText == mqttTopic("cmd/stop")) {
    g_streamEnabled = false;
    saveControllerSettings();
    g_streamer.reset();
  } else if (topicText == mqttTopic("cmd/restart")) {
    publishSimple("status/state", "restarting");
    delay(100);
    ESP.restart();
  } else if (topicText == mqttTopic("cmd/ping")) {
    publishSimple("status/pong", payloadText.length() > 0 ? payloadText : "pong");
  } else if (topicText == mqttTopic("cmd/set")) {
    applyControlPayload(payloadText);
  } else if (topicText == mqttTopic("cmd/timelapse/start")) {
    if (!dfrtimelapse::setEnabled(true)) setError("unable to start timelapse");
  } else if (topicText == mqttTopic("cmd/timelapse/stop")) {
    dfrtimelapse::setEnabled(false);
  } else if (topicText == mqttTopic("cmd/timelapse/set")) {
    applyTimelapsePayload(payloadText);
  }
  publishStatus(true);
}

void ensureMqtt() {
  if (WiFi.status() != WL_CONNECTED) return;
  g_mqttClient.loop();
  if (g_mqttClient.connected()) return;

  const uint32_t now = millis();
  if (now - g_lastMqttAttemptMs < dfrcfg::kMqttRetryMs) return;
  g_lastMqttAttemptMs = now;

  g_mqttClient.setServer(dfrcfg::kMqttHost, dfrcfg::kMqttPort);
  g_mqttClient.setCallback(mqttCallback);
  g_mqttClient.setBufferSize(1024);
  const String willTopic = mqttTopic("status/online");
  bool connected = false;
  if (strlen(dfrcfg::kMqttUsername) > 0) {
    connected = g_mqttClient.connect(
        dfrcfg::kMqttClientId,
        dfrcfg::kMqttUsername,
        dfrcfg::kMqttPassword,
        willTopic.c_str(),
        1,
        true,
        "false");
  } else {
    connected = g_mqttClient.connect(dfrcfg::kMqttClientId, willTopic.c_str(), 1, true, "false");
  }
  if (!connected) {
    statusf("mqtt connect failed rc=%d", g_mqttClient.state());
    return;
  }

  g_mqttClient.subscribe(mqttTopic("cmd/start").c_str());
  g_mqttClient.subscribe(mqttTopic("cmd/stop").c_str());
  g_mqttClient.subscribe(mqttTopic("cmd/restart").c_str());
  g_mqttClient.subscribe(mqttTopic("cmd/ping").c_str());
  g_mqttClient.subscribe(mqttTopic("cmd/set").c_str());
  g_mqttClient.subscribe(mqttTopic("cmd/timelapse/start").c_str());
  g_mqttClient.subscribe(mqttTopic("cmd/timelapse/stop").c_str());
  g_mqttClient.subscribe(mqttTopic("cmd/timelapse/set").c_str());
  statusf("mqtt connected topic=%s", dfrcfg::kMqttBaseTopic);
  publishStatus(true);
}

void handleRtspLoop() {
  if (WiFi.status() != WL_CONNECTED || !g_cameraReady) return;
  if (!g_streamEnabled) {
    WiFiClient waitingClient = g_rtspServer.accept();
    if (waitingClient) waitingClient.stop();
    return;
  }
  if (g_streamer == nullptr) rebuildStreamer();
  if (g_streamer == nullptr) return;

  g_streamer->handleRequests(0);
  const uint32_t now = millis();
  const uint32_t frameIntervalMs = 1000UL / static_cast<uint32_t>(max(1, g_streamFps));
  if (g_streamer->anySessions() &&
      (now - g_lastFrameAtMs >= frameIntervalMs || now < g_lastFrameAtMs)) {
    g_streamer->streamImage(now);
    g_lastFrameAtMs = now;
    ++g_framesSent;
  }

  WiFiClient client = g_rtspServer.accept();
  if (client) {
    client.setNoDelay(true);
    const String remote = client.remoteIP().toString();
    g_streamer->addSession(new WiFiClient(client));
    statusf("rtsp client=%s", remote.c_str());
  }
}

void updateRuntimeStats() {
  const uint32_t now = millis();
  if (now - g_lastStatsMs < dfrcfg::kStatusIntervalMs) return;
  const float seconds = g_lastStatsMs == 0 ? 5.0f : (now - g_lastStatsMs) / 1000.0f;
  g_lastMeasuredFps = g_framesSent / seconds;
  g_framesSent = 0;
  g_lastStatsMs = now;
  statusf(
      "runtime wifi=%s mqtt=%s rtsp=%s sessions=%d fps=%.1f lux=%.2f ir=%s timelapse=%s",
      WiFi.status() == WL_CONNECTED ? "up" : "down",
      g_mqttClient.connected() ? "up" : "down",
      g_streamEnabled ? "on" : "off",
      directStreamingSessionCount(),
      g_lastMeasuredFps,
      g_ambientLux,
      g_irEnabled ? "on" : "off",
      dfrtimelapse::state().c_str());
  publishStatus();
}

}  // namespace

namespace dfr1154 {

void setupController() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[BOOT] DFR1154 camera controller starting");

  pinMode(DFR_LED_PIN, OUTPUT);
  pinMode(DFR_IR_PIN, OUTPUT);
  digitalWrite(DFR_LED_PIN, LOW);
  digitalWrite(DFR_IR_PIN, LOW);

  g_preferences.begin("dfrcam", false);
  loadControllerSettings();
  initCamera();
  configureLightSensor();
  dfrtimelapse::begin();
  dfrtimelapse::setServiceHook(handleRtspLoop);

  statusf(
      "flash=%luMB psram=%luMB heap=%luKB",
      ESP.getFlashChipSize() / (1024UL * 1024UL),
      ESP.getPsramSize() / (1024UL * 1024UL),
      ESP.getFreeHeap() / 1024UL);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWifiEvent);
  ensureWifi();
  configureOta();
  ensureRtspServer();
}

void loopController() {
  ArduinoOTA.handle();
  ensureWifi();
  handleNetworkServices();
  configureMdns();
  ensureMqtt();
  dfrtimelapse::handleHttpClient();
  handleLightSensor();
  handleRtspLoop();
  dfrtimelapse::captureIfDue();
  updateRuntimeStats();
  delay(1);
}

}  // namespace dfr1154
