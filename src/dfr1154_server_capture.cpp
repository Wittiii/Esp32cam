#include "dfr1154_server_capture.h"

#include <Preferences.h>

#include "dfr1154_config.h"

namespace {

Preferences g_preferences;
bool g_preferencesReady = false;
bool g_enabled = true;
uint32_t g_intervalSeconds = dfrcfg::kDefaultTimelapseIntervalSeconds;

}  // namespace

namespace dfrcapture {

bool begin() {
  // Keep the old NVS namespace so existing interval and limit settings survive the rename.
  if (g_preferencesReady) return true;
  g_preferencesReady = g_preferences.begin("dfrtime", false);
  if (!g_preferencesReady) {
    Serial.println("[CAPTURE] settings storage unavailable");
    return false;
  }
  g_enabled = g_preferences.getBool("enabled", true);
  g_intervalSeconds = constrain(
      g_preferences.getUInt("interval", dfrcfg::kDefaultTimelapseIntervalSeconds),
      5UL,
      86400UL);
  Serial.println("[CAPTURE] snapshots are delegated to the Node server");
  return true;
}

bool setEnabled(bool enabledValue) {
  if (!g_preferencesReady) return false;
  if (g_enabled == enabledValue) return true;
  if (g_preferences.putBool("enabled", enabledValue) != sizeof(uint8_t)) return false;
  g_enabled = enabledValue;
  return true;
}

bool setIntervalSeconds(uint32_t interval) {
  if (!g_preferencesReady) return false;
  const uint32_t bounded = constrain(interval, 5UL, 86400UL);
  if (g_intervalSeconds == bounded) return true;
  if (g_preferences.putUInt("interval", bounded) != sizeof(uint32_t)) return false;
  g_intervalSeconds = bounded;
  return true;
}

bool enabled() { return g_enabled; }
uint32_t intervalSeconds() { return g_intervalSeconds; }
String state() { return g_enabled ? "enabled" : "disabled"; }

}  // namespace dfrcapture
