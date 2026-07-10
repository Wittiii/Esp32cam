#include "dfr1154_timelapse.h"

#include <Preferences.h>

#include "dfr1154_config.h"

namespace {

Preferences g_preferences;
bool g_enabled = true;
uint32_t g_intervalSeconds = dfrcfg::kDefaultTimelapseIntervalSeconds;
uint64_t g_storageLimitBytes = dfrcfg::kDefaultTimelapseLimitBytes;

}  // namespace

namespace dfrtimelapse {

bool begin() {
  g_preferences.begin("dfrtime", false);
  g_enabled = g_preferences.getBool("enabled", true);
  g_intervalSeconds = constrain(
      g_preferences.getUInt("interval", dfrcfg::kDefaultTimelapseIntervalSeconds),
      5UL,
      86400UL);
  g_storageLimitBytes =
      g_preferences.getULong64("limit", dfrcfg::kDefaultTimelapseLimitBytes);
  Serial.println("[TIMELAPSE] capture delegated to Node server; SD archive disabled");
  return true;
}

void startHttpServer() {}
void handleHttpClient() {}
void captureIfDue() {}

void observeJpegFrame(
    const uint8_t *,
    size_t,
    uint16_t,
    uint16_t,
    uint32_t) {}

bool setEnabled(bool enabledValue) {
  g_enabled = enabledValue;
  g_preferences.putBool("enabled", g_enabled);
  return true;
}

bool setIntervalSeconds(uint32_t interval) {
  g_intervalSeconds = constrain(interval, 5UL, 86400UL);
  g_preferences.putUInt("interval", g_intervalSeconds);
  return true;
}

bool setStorageLimitBytes(uint64_t limit) {
  if (limit < 64ULL * 1024ULL * 1024ULL) return false;
  g_storageLimitBytes = limit;
  g_preferences.putULong64("limit", g_storageLimitBytes);
  return true;
}

bool sdReady() { return false; }
bool enabled() { return g_enabled; }
bool httpReady() { return false; }
uint32_t intervalSeconds() { return g_intervalSeconds; }
uint64_t storageBytes() { return 0; }
uint64_t storageLimitBytes() { return g_storageLimitBytes; }
uint64_t cardTotalBytes() { return 0; }
uint64_t cardUsedBytes() { return 0; }
String state() { return g_enabled ? "server_capture" : "stopped"; }
String error() { return ""; }
String lastImage() { return ""; }

}  // namespace dfrtimelapse
