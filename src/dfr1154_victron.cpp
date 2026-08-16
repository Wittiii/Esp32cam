#include "dfr1154_victron.h"

#include <VictronBLE.h>
#include <string.h>

#include "dfr1154_config.h"

namespace {

VictronBLE g_victron;
dfrvictron::Reading g_reading = {};
portMUX_TYPE g_readingMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t g_lastScanStartMs = 0;

bool hasValidConfiguration() {
  return dfrcfg::kVictronEnabled && strlen(dfrcfg::kVictronMac) > 0 &&
         strlen(dfrcfg::kVictronEncryptionKey) == 32;
}

void onVictronData(const VictronDevice *device) {
  if (device == nullptr || !device->dataValid ||
      device->deviceType != DEVICE_TYPE_SOLAR_CHARGER) {
    return;
  }

  portENTER_CRITICAL(&g_readingMux);
  g_reading.valid = true;
  g_reading.chargeState = device->solar.chargeState;
  g_reading.errorCode = device->solar.errorCode;
  g_reading.batteryVoltage = device->solar.batteryVoltage;
  g_reading.batteryCurrent = device->solar.batteryCurrent;
  g_reading.panelPower = device->solar.panelPower;
  g_reading.yieldTodayWh = device->solar.yieldToday;
  g_reading.loadCurrent = device->solar.loadCurrent;
  g_reading.rssi = device->rssi;
  g_reading.lastUpdateMs = millis();
  portEXIT_CRITICAL(&g_readingMux);
}

}  // namespace

namespace dfrvictron {

void begin() {
  g_reading.configured = hasValidConfiguration();
  if (!g_reading.configured) {
    Serial.println("[Victron] not configured; set MAC and encryption key");
    return;
  }

  g_victron.setCallback(onVictronData);
  g_victron.setMinInterval(1000);
  if (!g_victron.addDevice(
          dfrcfg::kVictronName,
          dfrcfg::kVictronMac,
          dfrcfg::kVictronEncryptionKey,
          DEVICE_TYPE_SOLAR_CHARGER) ||
      !g_victron.begin(1)) {
    Serial.println("[Victron] BLE initialization failed");
    return;
  }
  g_reading.initialized = true;
  Serial.printf("[Victron] BLE scanner ready for %s\n", dfrcfg::kVictronName);
}

void loop() {
  if (!g_reading.initialized) return;
  const uint32_t now = millis();
  if (g_lastScanStartMs != 0 && now - g_lastScanStartMs < dfrcfg::kVictronScanIntervalMs) return;
  g_lastScanStartMs = now;
  g_victron.loop();
}

Reading reading() {
  portENTER_CRITICAL(&g_readingMux);
  const Reading copy = g_reading;
  portEXIT_CRITICAL(&g_readingMux);
  return copy;
}

const char *status() {
  const Reading value = reading();
  if (!dfrcfg::kVictronEnabled) return "disabled";
  if (!value.configured) return "not_configured";
  if (!value.initialized) return "init_error";
  if (!value.valid) return "waiting";
  if (millis() - value.lastUpdateMs >= dfrcfg::kVictronStaleAfterMs) return "stale";
  return "ready";
}

const char *chargeStateName(uint8_t state) {
  switch (state) {
    case CHARGER_OFF: return "off";
    case CHARGER_LOW_POWER: return "low_power";
    case CHARGER_FAULT: return "fault";
    case CHARGER_BULK: return "bulk";
    case CHARGER_ABSORPTION: return "absorption";
    case CHARGER_FLOAT: return "float";
    case CHARGER_STORAGE: return "storage";
    case CHARGER_EQUALIZE: return "equalize";
    case CHARGER_INVERTING: return "inverting";
    case CHARGER_POWER_SUPPLY: return "power_supply";
    case CHARGER_EXTERNAL_CONTROL: return "external_control";
    default: return "unknown";
  }
}

}  // namespace dfrvictron
