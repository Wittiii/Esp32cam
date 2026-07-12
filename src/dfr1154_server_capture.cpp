#include "dfr1154_server_capture.h"

#include <Preferences.h>

#include "dfr1154_config.h"

namespace {

Preferences g_preferences;
bool g_enabled = true;
uint32_t g_intervalSeconds = dfrcfg::kDefaultTimelapseIntervalSeconds;

}  // namespace

namespace dfrcapture {

bool begin() {
  // Keep the old NVS namespace so existing interval and limit settings survive the rename.
  g_preferences.begin("dfrtime", false);
  g_enabled = g_preferences.getBool("enabled", true);
  g_intervalSeconds = constrain(
      g_preferences.getUInt("interval", dfrcfg::kDefaultTimelapseIntervalSeconds),
      5UL,
      86400UL);
  Serial.println("[CAPTURE] snapshots are delegated to the Node server");
  return true;
}

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

bool enabled() { return g_enabled; }
uint32_t intervalSeconds() { return g_intervalSeconds; }
String state() { return g_enabled ? "enabled" : "disabled"; }

}  // namespace dfrcapture
