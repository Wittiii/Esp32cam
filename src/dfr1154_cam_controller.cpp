

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
#include <esp_system.h>
#include <math.h>
#include <memory>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "camera_common.h"
#include "dfr1154_config.h"
#include "dfr1154_environment.h"
#include "dfr1154_pins.h"
#include "dfr1154_server_capture.h"
#include "dfr1154_victron.h"
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
bool g_otaReady = false;
bool g_otaActive = false;
bool g_mqttEverConnected = false;
bool g_victronBeginAttempted = false;

volatile bool g_wifiGotIpEvent = false;
volatile bool g_wifiDisconnectedEvent = false;
volatile uint8_t g_wifiDisconnectReason = 0;

framesize_t g_frameSize = FRAMESIZE_UXGA;
int g_jpegQuality = 10;
int g_brightness = 1;
int g_contrast = 0;
int g_saturation = -2;
int g_sharpness = 0;
int g_hmirror = 0;
int g_vflip = 1;
int g_ledEnabled = 0;
int g_streamFps = dfrcfg::kDefaultRtspFps;

// OV3660 controls exposed by DFRobot's CameraWebServer example.
int g_gainCeiling = 0;
int g_colorbar = 0;
int g_awb = 1;
int g_agc = 1;
int g_aec = 1;
int g_awbGain = 1;
int g_agcGain = 0;
int g_aecValue = 300;
int g_aec2 = 0;
int g_dcw = 1;
int g_bpc = 0;
int g_wpc = 1;
int g_rawGma = 1;
int g_lenc = 1;
int g_specialEffect = 0;
int g_wbMode = 0;
int g_aeLevel = 0;

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
uint32_t g_lastCameraRecoveryAttemptMs = 0;
uint32_t g_wifiReconnectCount = 0;
uint32_t g_mqttReconnectCount = 0;
uint32_t g_cameraRecoveryCount = 0;
uint32_t g_mqttPublishFailures = 0;
uint8_t g_wifiAttemptCount = 0;
uint8_t g_mqttAttemptCount = 0;
uint8_t g_cameraRecoveryFailures = 0;
bool g_cameraRecoveryPending = false;
float g_lastMeasuredFps = 0.0f;
esp_reset_reason_t g_bootResetReason = ESP_RST_UNKNOWN;

String g_lastCommandId;
String g_lastCommandName;
String g_lastCommandResult = "none";
String g_lastCommandMessage;
uint32_t g_lastCommandAtMs = 0;
String g_pendingMqttTopic;
String g_pendingMqttPayload;
bool g_pendingMqttMessage = false;

int clampValue(int value, int minimum, int maximum) {
  return camcommon::clampInt(value, minimum, maximum);
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
  return camcommon::frameSizeName(g_frameSize);
}

framesize_t frameSizeFromIndex(int index) {
  return camcommon::frameSizeFromIndex(index, g_frameSize, true);
}

int frameSizeToIndex(framesize_t size) {
  return camcommon::frameSizeToIndex(size, 2);
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
  return camcommon::rtspUrl(
      WiFi.localIP(), dfrcfg::kRtspPort, dfrcfg::kRtspPresentation, dfrcfg::kRtspStream);
}

String streamState() {
  if (g_otaActive) return "updating";
  if (g_cameraRecoveryPending) return "recovering";
  if (!g_cameraReady) return "camera_error";
  if (WiFi.status() != WL_CONNECTED) return "wifi_down";
  if (!g_streamEnabled) return "paused";
  return directStreamingSessionCount() > 0 ? "streaming" : "ready";
}

void publishStatus(bool forceConfig = false);
void configureLightSensor();

uint32_t boundedReconnectDelay(uint8_t attempts, uint32_t initialMs, uint32_t maximumMs) {
  uint32_t delayMs = initialMs;
  const uint8_t steps = attempts > 1 ? attempts - 1 : 0;
  for (uint8_t index = 0; index < steps && delayMs < maximumMs; ++index) {
    delayMs = delayMs > maximumMs / 2UL ? maximumMs : delayMs * 2UL;
  }
  return delayMs;
}

const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_watchdog";
    case ESP_RST_TASK_WDT: return "task_watchdog";
    case ESP_RST_WDT: return "other_watchdog";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}

void serviceMqttDuringRtspWrite() {
  if (g_mqttClient.connected()) g_mqttClient.loop();
  feedLoopWDT();
  delay(0);
}

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

void setCommandResult(
    const String &command,
    const String &requestId,
    const String &result,
    const String &message) {
  g_lastCommandName = command;
  g_lastCommandId = requestId;
  g_lastCommandResult = result;
  g_lastCommandMessage = message;
  g_lastCommandAtMs = millis();
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

  g_streamer.reset(new Esp32RtspStreamer(width, height));
  // Match the proven Micro-RTSP transport used before the reconnect changes.
  g_streamer->setNonBlockingTcpWrites(false);
  g_streamer->setServiceCallback(serviceMqttDuringRtspWrite);
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
    feedLoopWDT();
    if (!g_mqttClient.publish(mqttTopic(suffix).c_str(), value.c_str(), retain)) {
      ++g_mqttPublishFailures;
    }
    feedLoopWDT();
    delay(0);
  }
}

void publishExternalSensors() {
  static bool bmePublishScheduleStarted = false;
  static uint32_t lastBmePublishMs = 0;
  const uint32_t now = millis();
  if (!bmePublishScheduleStarted) {
    bmePublishScheduleStarted = true;
    lastBmePublishMs = now;
  }
  if (now - lastBmePublishMs >= dfrcfg::kBme280PublishIntervalMs) {
    lastBmePublishMs = now;
    const dfrbme::Reading bme = dfrbme::reading();
    publishSimple("status/bme280", dfrbme::status());
    publishSimple("sensor/bme280/address", bme.address == 0 ? "-" : "0x" + String(bme.address, HEX));
    publishSimple("sensor/bme280/temperature_c", bme.valid ? String(bme.temperatureC, 2) : "-");
    publishSimple("sensor/bme280/humidity_percent", bme.valid ? String(bme.humidityPercent, 2) : "-");
    publishSimple("sensor/bme280/pressure_hpa", bme.valid ? String(bme.pressureHpa, 2) : "-");
    publishSimple("sensor/bme280/latest_temperature_c", bme.valid ? String(bme.latestTemperatureC, 2) : "-");
    publishSimple("sensor/bme280/latest_humidity_percent", bme.valid ? String(bme.latestHumidityPercent, 2) : "-");
    publishSimple("sensor/bme280/latest_pressure_hpa", bme.valid ? String(bme.latestPressureHpa, 2) : "-");
    publishSimple("sensor/bme280/average_samples", String(bme.averageSamples));
    publishSimple("sensor/bme280/read_failures", String(bme.readFailures));
    publishSimple(
        "sensor/bme280/age_seconds",
        bme.valid ? String((now - bme.lastUpdateMs) / 1000UL) : "-");

    String bmeJson = "{\"status\":\"" + String(dfrbme::status()) + "\"";
    if (bme.valid) {
      bmeJson += ",\"temperature_c\":" + String(bme.temperatureC, 2);
      bmeJson += ",\"humidity_percent\":" + String(bme.humidityPercent, 2);
      bmeJson += ",\"pressure_hpa\":" + String(bme.pressureHpa, 2);
      bmeJson += ",\"latest_temperature_c\":" + String(bme.latestTemperatureC, 2);
      bmeJson += ",\"latest_humidity_percent\":" + String(bme.latestHumidityPercent, 2);
      bmeJson += ",\"latest_pressure_hpa\":" + String(bme.latestPressureHpa, 2);
      bmeJson += ",\"average_samples\":" + String(bme.averageSamples);
    }
    bmeJson += "}";
    publishSimple("sensor/bme280/json", bmeJson);
  }

  const dfrvictron::Reading victron = dfrvictron::reading();
  publishSimple("status/victron_ble", dfrvictron::status());
  publishSimple(
    "status/victron_ble_stage",
    dfrvictron::debugStage());

publishSimple(
    "status/victron_ble_stage_code",
    String(dfrvictron::debugStageCode()));
  publishSimple("victron/mppt/configured", victron.configured ? "true" : "false");
  publishSimple("victron/mppt/restart_count", String(victron.restartCount));
  publishSimple("victron/mppt/scan_restart_count", String(victron.scanRestartCount));
  publishSimple("victron/mppt/advertisement_count", String(victron.advertisementCount));
  publishSimple("victron/mppt/decode_error_count", String(victron.decodeErrorCount));
  publishSimple("victron/mppt/charger_state", victron.valid ? dfrvictron::chargeStateName(victron.chargeState) : "-");
  publishSimple("victron/mppt/charger_state_id", victron.valid ? String(victron.chargeState) : "-");
  publishSimple("victron/mppt/error_code", victron.valid ? String(victron.errorCode) : "-");
  publishSimple("victron/mppt/battery_voltage_v", victron.valid ? String(victron.batteryVoltage, 2) : "-");
  publishSimple("victron/mppt/battery_current_a", victron.valid ? String(victron.batteryCurrent, 2) : "-");
  publishSimple("victron/mppt/panel_power_w", victron.valid ? String(victron.panelPower, 0) : "-");
  publishSimple("victron/mppt/yield_today_wh", victron.valid ? String(victron.yieldTodayWh) : "-");
  publishSimple("victron/mppt/load_current_a", victron.valid ? String(victron.loadCurrent, 2) : "-");
  publishSimple("victron/mppt/rssi", victron.valid ? String(victron.rssi) : "-");
  publishSimple(
      "victron/mppt/age_seconds",
      victron.valid ? String((millis() - victron.lastUpdateMs) / 1000UL) : "-");

  String victronJson = "{\"status\":\"" + String(dfrvictron::status()) + "\"";
  victronJson += ",\"restart_count\":" + String(victron.restartCount);
  victronJson += ",\"scan_restart_count\":" + String(victron.scanRestartCount);
  victronJson += ",\"advertisement_count\":" + String(victron.advertisementCount);
  victronJson += ",\"decode_error_count\":" + String(victron.decodeErrorCount);
  if (victron.valid) {
    victronJson += ",\"charger_state\":\"" + String(dfrvictron::chargeStateName(victron.chargeState)) + "\"";
    victronJson += ",\"error_code\":" + String(victron.errorCode);
    victronJson += ",\"battery_voltage_v\":" + String(victron.batteryVoltage, 2);
    victronJson += ",\"battery_current_a\":" + String(victron.batteryCurrent, 2);
    victronJson += ",\"panel_power_w\":" + String(victron.panelPower, 0);
    victronJson += ",\"yield_today_wh\":" + String(victron.yieldTodayWh);
    victronJson += ",\"load_current_a\":" + String(victron.loadCurrent, 2);
    victronJson += ",\"rssi\":" + String(victron.rssi);
  }
  victronJson += "}";
  publishSimple("victron/mppt/json", victronJson);
}

String buildConfigJson() {
  String json = "{";
  json.reserve(768);
  json += "\"framesize\":" + String(frameSizeToIndex(g_frameSize));
  json += ",\"framesize_name\":\"" + String(frameSizeName()) + "\"";
  json += ",\"jpeg_quality\":" + String(g_jpegQuality);
  json += ",\"stream_fps\":" + String(g_streamFps);
  json += ",\"brightness\":" + String(g_brightness);
  json += ",\"contrast\":" + String(g_contrast);
  json += ",\"saturation\":" + String(g_saturation);
  json += ",\"sharpness\":" + String(g_sharpness);
  json += ",\"gainceiling\":" + String(g_gainCeiling);
  json += ",\"colorbar\":" + String(g_colorbar);
  json += ",\"awb\":" + String(g_awb);
  json += ",\"agc\":" + String(g_agc);
  json += ",\"aec\":" + String(g_aec);
  json += ",\"awb_gain\":" + String(g_awbGain);
  json += ",\"agc_gain\":" + String(g_agcGain);
  json += ",\"aec_value\":" + String(g_aecValue);
  json += ",\"aec2\":" + String(g_aec2);
  json += ",\"dcw\":" + String(g_dcw);
  json += ",\"bpc\":" + String(g_bpc);
  json += ",\"wpc\":" + String(g_wpc);
  json += ",\"raw_gma\":" + String(g_rawGma);
  json += ",\"lenc\":" + String(g_lenc);
  json += ",\"special_effect\":" + String(g_specialEffect);
  json += ",\"wb_mode\":" + String(g_wbMode);
  json += ",\"ae_level\":" + String(g_aeLevel);
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
  json += ",\"server_capture_enabled\":";
  json += dfrcapture::enabled() ? "true" : "false";
  json += ",\"server_capture_interval_seconds\":" + String(dfrcapture::intervalSeconds());
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
  publishSimple("status/mdns", String(dfrcfg::kMdnsHostname) + ".local");
  publishSimple("status/last_status", g_lastStatus);
  publishSimple("status/clients", String(directStreamingSessionCount()));
  publishSimple("status/direct_sessions", String(directSessionCount()));
  publishSimple("status/direct_streaming_clients", String(directStreamingSessionCount()));
  publishSimple("status/frame_fps", String(g_lastMeasuredFps, 1));
  publishSimple("status/configured_fps", String(g_streamFps));
  publishSimple("status/mqtt_connected", "true");
  publishSimple("status/wifi_rssi", String(WiFi.RSSI()));
  publishSimple("status/uptime_seconds", String(millis() / 1000UL));
  publishSimple("status/free_heap_bytes", String(ESP.getFreeHeap()));
  publishSimple("status/min_free_heap_bytes", String(ESP.getMinFreeHeap()));
  publishSimple("status/reset_reason", resetReasonName(g_bootResetReason));
  publishSimple("status/firmware_version", dfrcfg::kFirmwareVersion);
  publishSimple("status/wifi_reconnect_count", String(g_wifiReconnectCount));
  publishSimple("status/mqtt_reconnect_count", String(g_mqttReconnectCount));
  publishSimple("status/camera_recovery_count", String(g_cameraRecoveryCount));
  publishSimple("status/mqtt_publish_failures", String(g_mqttPublishFailures));
  publishSimple("status/ambient_lux", isfinite(g_ambientLux) ? String(g_ambientLux, 2) : "-");
  publishSimple("status/ir_mode", irModeName());
  publishSimple("status/ir_enabled", g_irEnabled ? "true" : "false");
  publishSimple("status/light_sensor", g_lightReady ? "ready" : "unavailable");
  publishExternalSensors();
  publishSimple("status/capture_request/state", dfrcapture::state());
  publishSimple("status/capture_request/enabled", dfrcapture::enabled() ? "true" : "false");
  publishSimple("status/capture_request/interval_seconds", String(dfrcapture::intervalSeconds()));
  publishSimple("status/command/id", g_lastCommandId);
  publishSimple("status/command/name", g_lastCommandName);
  publishSimple("status/command/result", g_lastCommandResult);
  publishSimple("status/command/message", g_lastCommandMessage);
  publishSimple("status/command/at_ms", String(g_lastCommandAtMs));

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
  g_preferences.putInt("gainceil", g_gainCeiling);
  g_preferences.putInt("colorbar", g_colorbar);
  g_preferences.putInt("awb", g_awb);
  g_preferences.putInt("agc", g_agc);
  g_preferences.putInt("aec", g_aec);
  g_preferences.putInt("awbgain", g_awbGain);
  g_preferences.putInt("agcgain", g_agcGain);
  g_preferences.putInt("aecvalue", g_aecValue);
  g_preferences.putInt("aec2", g_aec2);
  g_preferences.putInt("dcw", g_dcw);
  g_preferences.putInt("bpc", g_bpc);
  g_preferences.putInt("wpc", g_wpc);
  g_preferences.putInt("rawgma", g_rawGma);
  g_preferences.putInt("lenc", g_lenc);
  g_preferences.putInt("effect", g_specialEffect);
  g_preferences.putInt("wbmode", g_wbMode);
  g_preferences.putInt("aelevel", g_aeLevel);
  g_preferences.putInt("hmirror", g_hmirror);
  g_preferences.putInt("vflip", g_vflip);
  g_preferences.putBool("led", g_ledEnabled != 0);
  g_preferences.putBool("stream", g_streamEnabled);
  g_preferences.putInt("irmode", g_irMode);
  g_preferences.putFloat("ironlux", g_irOnBelowLux);
  g_preferences.putFloat("irofflux", g_irOffAboveLux);
}

void loadControllerSettings() {
  const uint8_t configVersion = g_preferences.getUChar("cfgver", 0);
  const bool alignDfrDefaults = configVersion < 2;
  g_frameSize = frameSizeFromIndex(clampValue(g_preferences.getInt("framesize", 6), 0, 7));
  g_jpegQuality = clampValue(g_preferences.getInt("quality", 10), 4, 63);
  g_streamFps = clampValue(g_preferences.getInt("fps", dfrcfg::kDefaultRtspFps), 1, 20);
  g_brightness = clampValue(g_preferences.getInt("bright", 1), -2, 2);
  g_contrast = clampValue(g_preferences.getInt("contrast", 0), -2, 2);
  g_saturation = clampValue(g_preferences.getInt("saturate", -2), -2, 2);
  g_sharpness = clampValue(g_preferences.getInt("sharp", 0), -2, 2);
  g_gainCeiling = clampValue(g_preferences.getInt("gainceil", 0), 0, 6);
  g_colorbar = g_preferences.getInt("colorbar", 0) ? 1 : 0;
  g_awb = g_preferences.getInt("awb", 1) ? 1 : 0;
  g_agc = g_preferences.getInt("agc", 1) ? 1 : 0;
  g_aec = g_preferences.getInt("aec", 1) ? 1 : 0;
  g_awbGain = g_preferences.getInt("awbgain", 1) ? 1 : 0;
  g_agcGain = clampValue(g_preferences.getInt("agcgain", 0), 0, 30);
  g_aecValue = clampValue(g_preferences.getInt("aecvalue", 300), 0, 1200);
  g_aec2 = g_preferences.getInt("aec2", 0) ? 1 : 0;
  g_dcw = g_preferences.getInt("dcw", 1) ? 1 : 0;
  g_bpc = g_preferences.getInt("bpc", 0) ? 1 : 0;
  g_wpc = g_preferences.getInt("wpc", 1) ? 1 : 0;
  g_rawGma = g_preferences.getInt("rawgma", 1) ? 1 : 0;
  g_lenc = g_preferences.getInt("lenc", 1) ? 1 : 0;
  g_specialEffect = clampValue(g_preferences.getInt("effect", 0), 0, 6);
  g_wbMode = clampValue(g_preferences.getInt("wbmode", 0), 0, 4);
  g_aeLevel = clampValue(g_preferences.getInt("aelevel", 0), -2, 2);
  g_hmirror = g_preferences.getInt("hmirror", 0) ? 1 : 0;
  g_vflip = g_preferences.getInt("vflip", 1) ? 1 : 0;
  g_ledEnabled = g_preferences.getBool("led", false) ? 1 : 0;
  g_streamEnabled = g_preferences.getBool("stream", true);
  g_irMode = clampValue(g_preferences.getInt("irmode", 2), 0, 2);
  g_irOnBelowLux = g_preferences.getFloat("ironlux", dfrcfg::kDefaultIrOnBelowLux);
  g_irOffAboveLux = g_preferences.getFloat("irofflux", dfrcfg::kDefaultIrOffAboveLux);
  if (g_irOffAboveLux <= g_irOnBelowLux) g_irOffAboveLux = g_irOnBelowLux + 2.0f;

  // Match DFRobot's CameraWebServer defaults once; later user changes remain persistent.
  if (alignDfrDefaults) {
    g_frameSize = FRAMESIZE_UXGA;
    g_jpegQuality = 10;
    g_brightness = 1;
    g_contrast = 0;
    g_saturation = -2;
    g_sharpness = 0;
    g_vflip = 1;
    saveControllerSettings();
  }
  if (configVersion < 3) {
    saveControllerSettings();
    g_preferences.putUChar("cfgver", 3);
  }
}

bool applySensorSettings(bool rebuildAfter) {
  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor == nullptr) {
    setError("camera sensor unavailable");
    return false;
  }
  bool success = true;
  String failedSetting;
  const auto check = [&](int result, const char *name) {
    if (result == 0) return;
    success = false;
    if (failedSetting.length() == 0) failedSetting = name;
  };

  check(sensor->set_framesize(sensor, g_frameSize), "framesize");
  check(sensor->set_quality(sensor, g_jpegQuality), "jpeg_quality");
  check(sensor->set_brightness(sensor, g_brightness), "brightness");
  check(sensor->set_contrast(sensor, g_contrast), "contrast");
  check(sensor->set_saturation(sensor, g_saturation), "saturation");
  check(sensor->set_sharpness(sensor, g_sharpness), "sharpness");
  check(sensor->set_gainceiling(sensor, static_cast<gainceiling_t>(g_gainCeiling)), "gainceiling");
  check(sensor->set_colorbar(sensor, g_colorbar), "colorbar");
  check(sensor->set_whitebal(sensor, g_awb), "awb");
  check(sensor->set_gain_ctrl(sensor, g_agc), "agc");
  check(sensor->set_exposure_ctrl(sensor, g_aec), "aec");
  check(sensor->set_awb_gain(sensor, g_awbGain), "awb_gain");
  check(sensor->set_agc_gain(sensor, g_agcGain), "agc_gain");
  check(sensor->set_aec_value(sensor, g_aecValue), "aec_value");
  check(sensor->set_aec2(sensor, g_aec2), "aec2");
  check(sensor->set_dcw(sensor, g_dcw), "dcw");
  check(sensor->set_bpc(sensor, g_bpc), "bpc");
  check(sensor->set_wpc(sensor, g_wpc), "wpc");
  check(sensor->set_raw_gma(sensor, g_rawGma), "raw_gma");
  check(sensor->set_lenc(sensor, g_lenc), "lenc");
  check(sensor->set_special_effect(sensor, g_specialEffect), "special_effect");
  check(sensor->set_wb_mode(sensor, g_wbMode), "wb_mode");
  check(sensor->set_ae_level(sensor, g_aeLevel), "ae_level");
  check(sensor->set_hmirror(sensor, g_hmirror), "hmirror");
  check(sensor->set_vflip(sensor, g_vflip), "vflip");
  applyLedState(g_ledEnabled != 0);
  evaluateIrAutomation();
  if (rebuildAfter) rebuildStreamer();
  if (!success) {
    setError("camera setting apply failed: " + failedSetting);
    return false;
  }
  clearError();
  return true;
}

bool initCamera() {
  g_cameraReady = false;
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
  // Camera and LTR-308 share I2C0. Using the already configured bus prevents
  // the light sensor from invalidating the camera driver's SCCB handle.
  config.pin_sccb_sda = -1;
  config.pin_sccb_scl = -1;
  config.sccb_i2c_port = 0;
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
  g_cameraRecoveryFailures = 0;
  g_cameraRecoveryPending = false;
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

void requestCameraRecovery(const String &reason) {
  if (!g_cameraRecoveryPending) {
    statusf("camera recovery requested: %s", reason.c_str());
  }
  g_cameraRecoveryPending = true;
}

void handleCameraRecovery() {
  const uint32_t now = millis();
  if (g_cameraReady && !g_cameraRecoveryPending) return;
  if (now - g_lastCameraRecoveryAttemptMs < 10000UL) return;
  g_lastCameraRecoveryAttemptMs = now;
  ++g_cameraRecoveryCount;

  if (restartCameraPipeline()) {
    g_cameraRecoveryFailures = 0;
    g_cameraRecoveryPending = false;
    statusf("camera recovery successful count=%lu", g_cameraRecoveryCount);
    return;
  }

  ++g_cameraRecoveryFailures;
  if (g_cameraRecoveryFailures >= 3) {
    statusf("camera recovery failed %u times; rebooting", g_cameraRecoveryFailures);
    delay(100);
    ESP.restart();
  }
}

void configureLightSensor() {
  g_lightReady = false;
  g_ambientLux = NAN;
  g_lightReady = g_lightSensor.begin();
  // DFRobot_LTR308::begin() selects 400 kHz. OV3660 SCCB register writes are
  // more reliable at the camera driver's normal 100 kHz bus speed.
  Wire.setClock(100000);
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
  if (g_otaReady || WiFi.status() != WL_CONNECTED) return;
  ArduinoOTA.setHostname(dfrcfg::kOtaHostname);
  if (strlen(dfrcfg::kOtaPassword) > 0) ArduinoOTA.setPassword(dfrcfg::kOtaPassword);
  ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.onStart([]() {
    g_otaActive = true;
    g_streamer.reset();
    recordStatus("ota update started; camera stream suspended");
    publishStatus();
  });
  ArduinoOTA.onEnd([]() { recordStatus("ota update finished"); });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { feedLoopWDT(); });
  ArduinoOTA.onError([](ota_error_t error) {
    g_otaActive = false;
    setError("ota error=" + String(static_cast<uint32_t>(error)));
    rebuildStreamer();
  });
  ArduinoOTA.begin();
  g_otaReady = true;
  statusf("ota ready host=%s", dfrcfg::kOtaHostname);
}

void configureMdns() {
  if (g_mdnsReady || WiFi.status() != WL_CONNECTED) return;
  if (!MDNS.begin(dfrcfg::kMdnsHostname)) {
    recordStatus("mdns setup failed");
    return;
  }
  MDNS.addService("arduino", "tcp", 3232);
  MDNS.addService("rtsp", "tcp", dfrcfg::kRtspPort);
  g_mdnsReady = true;
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      g_wifiGotIpEvent = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      g_wifiDisconnectReason = info.wifi_sta_disconnected.reason;
      g_wifiDisconnectedEvent = true;
      break;
    default:
      break;
  }
}

void handleWifiEvents() {
  if (g_wifiDisconnectedEvent) {
    g_wifiDisconnectedEvent = false;
    ++g_wifiReconnectCount;
    g_lastWifiAttemptMs = 0;
    g_otaActive = false;
    g_streamer.reset();
    g_rtspServer.end();
    g_rtspServerStarted = false;
    if (g_mqttClient.connected()) g_mqttClient.disconnect();
    g_mqttSocket.stop();
    g_lastMqttAttemptMs = 0;
    g_mqttAttemptCount = 0;
    if (g_otaReady) {
      ArduinoOTA.end();
      g_otaReady = false;
    }
    if (g_mdnsReady) {
      MDNS.end();
      g_mdnsReady = false;
    }
    g_networkServicesPending = false;
    statusf("wifi disconnected reason=%u", g_wifiDisconnectReason);
  }

  if (g_wifiGotIpEvent) {
    g_wifiGotIpEvent = false;
    g_wifiAttemptCount = 0;
    g_lastWifiAttemptMs = 0;
    statusf("wifi ip=%s", WiFi.localIP().toString().c_str());
    if (!g_timeConfigured) {
      configTzTime(dfrcfg::kTimezone, dfrcfg::kNtpServer1, dfrcfg::kNtpServer2);
      g_timeConfigured = true;
    }
    g_networkServicesPending = true;
  }
}

void handleNetworkServices() {
  if (!g_networkServicesPending || WiFi.status() != WL_CONNECTED) return;
  g_networkServicesPending = false;
  configureMdns();
  configureOta();
  ensureRtspServer();
  rebuildStreamer();
}

void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  const uint32_t now = millis();
  const uint32_t retryDelay = boundedReconnectDelay(
      g_wifiAttemptCount, dfrcfg::kWifiRetryInitialMs, dfrcfg::kWifiRetryMaxMs);
  if (g_lastWifiAttemptMs != 0 && now - g_lastWifiAttemptMs < retryDelay) return;
  g_lastWifiAttemptMs = now;
  ++g_wifiAttemptCount;

  if (!g_wifiStarted) {
    g_wifiStarted = true;
    statusf("wifi connecting ssid=%s", dfrcfg::kWifiSsid);
    WiFi.begin(dfrcfg::kWifiSsid, dfrcfg::kWifiPassword);
  } else if (g_wifiAttemptCount % dfrcfg::kWifiHardReconnectEvery == 0) {
    recordStatus("wifi stack reconnect");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(20);
    WiFi.begin(dfrcfg::kWifiSsid, dfrcfg::kWifiPassword);
  } else {
    WiFi.reconnect();
  }
}

bool extractPayloadToken(const String &payload, const String &key, String &token) {
  return camcommon::extractPayloadToken(payload, key.c_str(), token);
}

bool extractPayloadInt(const String &payload, const char *key, int &value) {
  return camcommon::extractPayloadInt(payload, key, value);
}

enum class SettingResult { unsupported, unchanged, applied, failed };

template <typename Value>
bool applySensorControl(
    sensor_t *sensor,
    int (*setter)(sensor_t *, Value),
    Value value,
    const char *key) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (setter(sensor, value) == 0) {
      // Several OV3660 controls share register 0x5000. Give SCCB time before
      // applying the next item from an MQTT settings batch.
      delay(10);
      return true;
    }
    delay(25 * (attempt + 1));
  }
  statusf("sensor setting rejected key=%s value=%d", key, static_cast<int>(value));
  return false;
}

SettingResult applySetting(const char *key, int value, bool &streamRebuildRequired) {
  sensor_t *sensor = esp_camera_sensor_get();
  const auto requireSensor = [&]() {
    if (sensor != nullptr) return true;
    requestCameraRecovery("sensor unavailable while applying settings");
    return false;
  };

  if (strcmp(key, "framesize") == 0) {
    const framesize_t bounded = frameSizeFromIndex(clampValue(value, 0, 7));
    if (bounded == g_frameSize) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_framesize, bounded, key)) return SettingResult::failed;
    g_frameSize = bounded;
    streamRebuildRequired = true;
  } else if (strcmp(key, "jpeg_quality") == 0) {
    const int bounded = clampValue(value, 4, 63);
    if (bounded == g_jpegQuality) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_quality, bounded, key)) return SettingResult::failed;
    g_jpegQuality = bounded;
  } else if (strcmp(key, "stream_fps") == 0) {
    const int bounded = clampValue(value, 1, 20);
    if (bounded == g_streamFps) return SettingResult::unchanged;
    g_streamFps = bounded;
  } else if (strcmp(key, "brightness") == 0) {
    const int bounded = clampValue(value, -2, 2);
    if (bounded == g_brightness) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_brightness, bounded, key)) return SettingResult::failed;
    g_brightness = bounded;
  } else if (strcmp(key, "contrast") == 0) {
    const int bounded = clampValue(value, -2, 2);
    if (bounded == g_contrast) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_contrast, bounded, key)) return SettingResult::failed;
    g_contrast = bounded;
  } else if (strcmp(key, "saturation") == 0) {
    const int bounded = clampValue(value, -2, 2);
    if (bounded == g_saturation) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_saturation, bounded, key)) return SettingResult::failed;
    g_saturation = bounded;
  } else if (strcmp(key, "sharpness") == 0) {
    const int bounded = clampValue(value, -2, 2);
    if (bounded == g_sharpness) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_sharpness, bounded, key)) return SettingResult::failed;
    g_sharpness = bounded;
  } else if (strcmp(key, "gainceiling") == 0) {
    const int bounded = clampValue(value, 0, 6);
    if (bounded == g_gainCeiling) return SettingResult::unchanged;
    const auto gain = static_cast<gainceiling_t>(bounded);
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_gainceiling, gain, key)) return SettingResult::failed;
    g_gainCeiling = bounded;
  } else if (strcmp(key, "colorbar") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded == g_colorbar) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_colorbar, bounded, key)) return SettingResult::failed;
    g_colorbar = bounded;
  } else if (strcmp(key, "awb") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded == g_awb) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_whitebal, bounded, key)) return SettingResult::failed;
    g_awb = bounded;
  } else if (strcmp(key, "agc") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded == g_agc) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_gain_ctrl, bounded, key)) return SettingResult::failed;
    g_agc = bounded;
  } else if (strcmp(key, "aec") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded == g_aec) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_exposure_ctrl, bounded, key)) return SettingResult::failed;
    g_aec = bounded;
  } else if (strcmp(key, "awb_gain") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded == g_awbGain) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_awb_gain, bounded, key)) return SettingResult::failed;
    g_awbGain = bounded;
  } else if (strcmp(key, "agc_gain") == 0) {
    const int bounded = clampValue(value, 0, 30);
    if (bounded == g_agcGain) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_agc_gain, bounded, key)) return SettingResult::failed;
    g_agcGain = bounded;
  } else if (strcmp(key, "aec_value") == 0) {
    const int bounded = clampValue(value, 0, 1200);
    if (bounded == g_aecValue) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_aec_value, bounded, key)) return SettingResult::failed;
    g_aecValue = bounded;
  } else if (strcmp(key, "aec2") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded == g_aec2) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_aec2, bounded, key)) return SettingResult::failed;
    g_aec2 = bounded;
  } else if (strcmp(key, "dcw") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded == g_dcw) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_dcw, bounded, key)) return SettingResult::failed;
    g_dcw = bounded;
  } else if (strcmp(key, "bpc") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded == g_bpc) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_bpc, bounded, key)) return SettingResult::failed;
    g_bpc = bounded;
  } else if (strcmp(key, "wpc") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded == g_wpc) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_wpc, bounded, key)) return SettingResult::failed;
    g_wpc = bounded;
  } else if (strcmp(key, "raw_gma") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded == g_rawGma) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_raw_gma, bounded, key)) return SettingResult::failed;
    g_rawGma = bounded;
  } else if (strcmp(key, "lenc") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded == g_lenc) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_lenc, bounded, key)) return SettingResult::failed;
    g_lenc = bounded;
  } else if (strcmp(key, "special_effect") == 0) {
    const int bounded = clampValue(value, 0, 6);
    if (bounded == g_specialEffect) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_special_effect, bounded, key)) return SettingResult::failed;
    g_specialEffect = bounded;
  } else if (strcmp(key, "wb_mode") == 0) {
    const int bounded = clampValue(value, 0, 4);
    if (bounded == g_wbMode) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_wb_mode, bounded, key)) return SettingResult::failed;
    g_wbMode = bounded;
  } else if (strcmp(key, "ae_level") == 0) {
    const int bounded = clampValue(value, -2, 2);
    if (bounded == g_aeLevel) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_ae_level, bounded, key)) return SettingResult::failed;
    g_aeLevel = bounded;
  } else if (strcmp(key, "hmirror") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded == g_hmirror) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_hmirror, bounded, key)) return SettingResult::failed;
    g_hmirror = bounded;
  } else if (strcmp(key, "vflip") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded == g_vflip) return SettingResult::unchanged;
    if (!requireSensor() || !applySensorControl(sensor, sensor->set_vflip, bounded, key)) return SettingResult::failed;
    g_vflip = bounded;
  } else if (strcmp(key, "led") == 0) {
    const int bounded = value ? 1 : 0;
    if (bounded == g_ledEnabled) return SettingResult::unchanged;
    applyLedState(bounded != 0);
  } else if (strcmp(key, "stream_enabled") == 0) {
    const bool enabled = value != 0;
    if (enabled == g_streamEnabled) return SettingResult::unchanged;
    g_streamEnabled = enabled;
    streamRebuildRequired = true;
  } else if (strcmp(key, "ir_mode") == 0) {
    const int bounded = clampValue(value, 0, 2);
    if (bounded == g_irMode) return SettingResult::unchanged;
    g_irMode = bounded;
    evaluateIrAutomation();
  } else if (strcmp(key, "ir_on_lux") == 0) {
    const float bounded = static_cast<float>(clampValue(value, 0, 100000));
    if (fabsf(bounded - g_irOnBelowLux) < 0.01f) return SettingResult::unchanged;
    g_irOnBelowLux = bounded;
    if (g_irOffAboveLux <= g_irOnBelowLux) g_irOffAboveLux = g_irOnBelowLux + 2.0f;
    evaluateIrAutomation();
  } else if (strcmp(key, "ir_off_lux") == 0) {
    const float bounded = static_cast<float>(clampValue(value, 1, 100000));
    if (fabsf(bounded - g_irOffAboveLux) < 0.01f) return SettingResult::unchanged;
    g_irOffAboveLux = bounded;
    if (g_irOffAboveLux <= g_irOnBelowLux) g_irOnBelowLux = max(0.0f, g_irOffAboveLux - 2.0f);
    evaluateIrAutomation();
  } else if (strcmp(key, "timelapse_enabled") == 0 || strcmp(key, "server_capture_enabled") == 0) {
    const bool enabled = value != 0;
    if (enabled == dfrcapture::enabled()) return SettingResult::unchanged;
    dfrcapture::setEnabled(enabled);
  } else if (strcmp(key, "timelapse_interval_seconds") == 0 || strcmp(key, "server_capture_interval_seconds") == 0) {
    const uint32_t bounded = static_cast<uint32_t>(max(5, value));
    if (bounded == dfrcapture::intervalSeconds()) return SettingResult::unchanged;
    dfrcapture::setIntervalSeconds(bounded);
  } else {
    return SettingResult::unsupported;
  }
  return SettingResult::applied;
}

void applyControlPayload(const String &payload) {
  static const char *keys[] = {
      "framesize", "jpeg_quality", "stream_fps", "brightness", "contrast", "saturation",
      "sharpness", "gainceiling", "colorbar", "awb", "agc", "aec", "awb_gain",
      "agc_gain", "aec_value", "aec2", "dcw", "bpc", "wpc", "raw_gma", "lenc",
      "special_effect", "wb_mode", "ae_level", "hmirror", "vflip", "led",
      "stream_enabled", "ir_mode", "ir_on_lux", "ir_off_lux", "server_capture_enabled",
      "server_capture_interval_seconds", "timelapse_enabled", "timelapse_interval_seconds"};

  String requestId;
  extractPayloadToken(payload, "_request_id", requestId);
  bool recognized = false;
  bool changed = false;
  bool failed = false;
  bool streamRebuildRequired = false;
  String failedSettings;
  for (const char *key : keys) {
    int value = 0;
    if (!extractPayloadInt(payload, key, value)) continue;
    const SettingResult result = applySetting(key, value, streamRebuildRequired);
    if (result != SettingResult::unsupported) recognized = true;
    if (result == SettingResult::applied) changed = true;
    if (result == SettingResult::failed) {
      failed = true;
      if (failedSettings.length() > 0) failedSettings += ",";
      failedSettings += key;
    }
  }

  if (!recognized) {
    setCommandResult("set", requestId, "error", "no supported setting in payload");
    setError("no supported setting in MQTT payload");
    publishStatus(true);
    return;
  }

  if (changed) saveControllerSettings();
  if (streamRebuildRequired) rebuildStreamer();
  if (failed) {
    const String message = "setting apply failed: " + failedSettings;
    setCommandResult("set", requestId, changed ? "partial" : "error", message);
    setError(message);
  } else {
    clearError();
    setCommandResult("set", requestId, "ok", changed ? "settings applied" : "settings already active");
    statusf("camera control applied live frame=%s q=%d fps=%d", frameSizeName(), g_jpegQuality, g_streamFps);
  }
  publishStatus(true);
}

void applyTimelapsePayload(const String &payload) {
  String requestId;
  extractPayloadToken(payload, "_request_id", requestId);
  int value = 0;
  bool changed = false;
  if (extractPayloadInt(payload, "enabled", value)) {
    dfrcapture::setEnabled(value != 0);
    changed = true;
  }
  if (extractPayloadInt(payload, "interval_seconds", value)) {
    dfrcapture::setIntervalSeconds(static_cast<uint32_t>(max(5, value)));
    changed = true;
  }
  if (!changed) {
    setCommandResult("capture_set", requestId, "error", "no supported capture setting in payload");
    setError("no supported server capture setting in MQTT payload");
  } else {
    clearError();
    setCommandResult("capture_set", requestId, "ok", "server capture settings applied");
  }
  publishStatus(true);
}

void processMqttMessage(const String &topicText, const String &payloadText) {
  String requestId;
  extractPayloadToken(payloadText, "_request_id", requestId);

  if (topicText == mqttTopic("cmd/start")) {
    g_streamEnabled = true;
    saveControllerSettings();
    rebuildStreamer();
    clearError();
    setCommandResult("start", requestId, "ok", "stream enabled");
    recordStatus("mqtt start command");
  } else if (topicText == mqttTopic("cmd/stop")) {
    g_streamEnabled = false;
    saveControllerSettings();
    g_streamer.reset();
    setCommandResult("stop", requestId, "ok", "stream disabled");
    recordStatus("mqtt stop command");
  } else if (topicText == mqttTopic("cmd/restart")) {
    setCommandResult("restart", requestId, "ok", "device rebooting");
    publishSimple("status/state", "restarting");
    publishStatus(true);
    g_mqttClient.loop();
    delay(100);
    ESP.restart();
  } else if (topicText == mqttTopic("cmd/ping")) {
    publishSimple("status/pong", requestId.length() > 0 ? requestId : (payloadText.length() > 0 ? payloadText : "pong"));
    setCommandResult("ping", requestId, "ok", "pong");
  } else if (topicText == mqttTopic("cmd/set")) {
    applyControlPayload(payloadText);
  } else if (topicText == mqttTopic("cmd/timelapse/start")) {
    if (!dfrcapture::setEnabled(true)) {
      setError("unable to start server capture");
      setCommandResult("capture_start", requestId, "error", g_lastError);
    } else {
      setCommandResult("capture_start", requestId, "ok", "server capture requested");
    }
  } else if (topicText == mqttTopic("cmd/timelapse/stop")) {
    dfrcapture::setEnabled(false);
    setCommandResult("capture_stop", requestId, "ok", "server capture disabled");
  } else if (topicText == mqttTopic("cmd/timelapse/set")) {
    applyTimelapsePayload(payloadText);
  }
  publishStatus(true);
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  const String topicText(topic);
  String payloadText;
  payloadText.reserve(length + 1);
  for (unsigned int index = 0; index < length; ++index) payloadText += static_cast<char>(payload[index]);
  Serial.printf("[MQTT] %s: %s\n", topicText.c_str(), payloadText.c_str());
  g_pendingMqttTopic = topicText;
  g_pendingMqttPayload = payloadText;
  g_pendingMqttMessage = true;
}

void handlePendingMqttMessage() {
  if (!g_pendingMqttMessage) return;
  const String topic = g_pendingMqttTopic;
  const String payload = g_pendingMqttPayload;
  g_pendingMqttMessage = false;
  g_pendingMqttTopic = "";
  g_pendingMqttPayload = "";
  processMqttMessage(topic, payload);
}

void ensureMqtt() {
  if (WiFi.status() != WL_CONNECTED) {
    if (g_mqttClient.connected()) g_mqttClient.disconnect();
    g_mqttSocket.stop();
    g_mqttAttemptCount = 0;
    g_lastMqttAttemptMs = 0;
    return;
  }
  g_mqttClient.loop();
  if (g_mqttClient.connected()) {
    g_mqttAttemptCount = 0;
    return;
  }

  const uint32_t now = millis();
  const uint32_t retryDelay = boundedReconnectDelay(
      g_mqttAttemptCount, dfrcfg::kMqttRetryInitialMs, dfrcfg::kMqttRetryMaxMs);
  if (g_lastMqttAttemptMs != 0 && now - g_lastMqttAttemptMs < retryDelay) return;
  g_lastMqttAttemptMs = now;
  ++g_mqttAttemptCount;

  g_mqttClient.disconnect();
  g_mqttSocket.stop();
  delay(5);

  g_mqttClient.setServer(dfrcfg::kMqttHost, dfrcfg::kMqttPort);
  g_mqttClient.setCallback(mqttCallback);
  g_mqttClient.setSocketTimeout(1);
  g_mqttClient.setKeepAlive(20);
  g_mqttClient.setBufferSize(2048);
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
    g_mqttSocket.stop();
    statusf("mqtt connect failed rc=%d", g_mqttClient.state());
    return;
  }

  g_mqttAttemptCount = 0;
  if (g_mqttEverConnected) ++g_mqttReconnectCount;
  g_mqttEverConnected = true;

  g_mqttClient.subscribe(mqttTopic("cmd/start").c_str(), 1);
  g_mqttClient.subscribe(mqttTopic("cmd/stop").c_str(), 1);
  g_mqttClient.subscribe(mqttTopic("cmd/restart").c_str(), 1);
  g_mqttClient.subscribe(mqttTopic("cmd/ping").c_str(), 1);
  g_mqttClient.subscribe(mqttTopic("cmd/set").c_str(), 1);
  g_mqttClient.subscribe(mqttTopic("cmd/timelapse/start").c_str(), 1);
  g_mqttClient.subscribe(mqttTopic("cmd/timelapse/stop").c_str(), 1);
  g_mqttClient.subscribe(mqttTopic("cmd/timelapse/set").c_str(), 1);
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
    if (g_streamer->lastFrameSucceeded()) {
      ++g_framesSent;
    } else if (g_streamer->consecutiveCaptureFailures() >= 5) {
      requestCameraRecovery("repeated frame capture failures");
    }
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
      dfrcapture::state().c_str());
  publishStatus();
}

void ensureVictron() {
  if (g_victronBeginAttempted || !dfrcfg::kVictronEnabled) return;
  if (millis() < dfrcfg::kVictronStartDelayMs) return;
  if (WiFi.status() != WL_CONNECTED || !g_mqttClient.connected()) return;

  g_victronBeginAttempted = true;
  recordStatus("victron BLE initializing");
  dfrvictron::begin();
}

}  // namespace

namespace dfr1154 {

void setupController() {
  Serial.begin(115200);
  delay(500);
  g_bootResetReason = esp_reset_reason();
  Serial.println("\n[BOOT] DFR1154 camera controller starting");

  pinMode(DFR_LED_PIN, OUTPUT);
  pinMode(DFR_IR_PIN, OUTPUT);
  digitalWrite(DFR_LED_PIN, LOW);
  digitalWrite(DFR_IR_PIN, LOW);

  Wire.begin(DFR_CAM_SIOD, DFR_CAM_SIOC, 100000);

  g_preferences.begin("dfrcam", false);
  loadControllerSettings();
  initCamera();
  configureLightSensor();
  dfrbme::begin();
  dfrcapture::begin();

  statusf(
      "flash=%luMB psram=%luMB heap=%luKB",
      ESP.getFlashChipSize() / (1024UL * 1024UL),
      ESP.getPsramSize() / (1024UL * 1024UL),
      ESP.getFreeHeap() / 1024UL);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWifiEvent);
  enableLoopWDT();
  ensureWifi();
}

void loopController() {
  if (g_otaReady) ArduinoOTA.handle();
  dfrbme::loop();
  handleWifiEvents();
  ensureWifi();
  handleNetworkServices();
  if (g_otaActive) {
    delay(1);
    return;
  }
  dfrvictron::loop();
  ensureMqtt();
  ensureVictron();
  handlePendingMqttMessage();
  if (g_otaReady) ArduinoOTA.handle();
  handleCameraRecovery();
  handleLightSensor();
  handleRtspLoop();
  updateRuntimeStats();
  delay(1);
}

}  // namespace dfr1154
