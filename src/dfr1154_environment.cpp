#include "dfr1154_environment.h"

#include <Adafruit_BME280.h>
#include <Wire.h>
#include <math.h>

#include "dfr1154_config.h"
#include "dfr1154_pins.h"

namespace {

TwoWire g_bmeWire(1);
Adafruit_BME280 g_bme;
dfrbme::Reading g_reading = {false, 0, NAN, NAN, NAN, 0, 0};
bool g_wireReady = false;
bool g_sensorReady = false;
uint32_t g_lastProbeMs = 0;
uint32_t g_lastReadMs = 0;

bool probeSensor() {
  g_lastProbeMs = millis();
  for (const uint8_t address : {0x76, 0x77}) {
    if (g_bme.begin(address, &g_bmeWire)) {
      g_sensorReady = true;
      g_reading.address = address;
      Serial.printf("[BME280] ready address=0x%02X SDA=%d SCL=%d\n",
                    address, DFR_GRAVITY_SDA, DFR_GRAVITY_SCL);
      return true;
    }
  }
  g_sensorReady = false;
  Serial.println("[BME280] unavailable at 0x76/0x77");
  return false;
}

bool takeReading() {
  const float temperature = g_bme.readTemperature();
  const float humidity = g_bme.readHumidity();
  const float pressure = g_bme.readPressure() / 100.0f;
  if (!isfinite(temperature) || !isfinite(humidity) || !isfinite(pressure) ||
      humidity < 0.0f || humidity > 100.0f || pressure < 100.0f || pressure > 1200.0f) {
    ++g_reading.readFailures;
    return false;
  }

  g_reading.temperatureC = temperature;
  g_reading.humidityPercent = humidity;
  g_reading.pressureHpa = pressure;
  g_reading.lastUpdateMs = millis();
  g_reading.valid = true;
  return true;
}

}  // namespace

namespace dfrbme {

void begin() {
  if (!dfrcfg::kBme280Enabled || g_wireReady) return;
  g_wireReady = g_bmeWire.begin(DFR_GRAVITY_SDA, DFR_GRAVITY_SCL, 100000);
  if (!g_wireReady) {
    Serial.println("[BME280] second I2C bus initialization failed");
    return;
  }
  if (probeSensor()) takeReading();
}

void loop() {
  if (!dfrcfg::kBme280Enabled) return;
  const uint32_t now = millis();
  if (!g_wireReady) {
    begin();
    return;
  }
  if (!g_sensorReady) {
    if (now - g_lastProbeMs >= dfrcfg::kBme280RetryIntervalMs) probeSensor();
    return;
  }
  if (now - g_lastReadMs < dfrcfg::kBme280ReadIntervalMs) return;
  g_lastReadMs = now;
  if (!takeReading() && now - g_reading.lastUpdateMs >= dfrcfg::kBme280StaleAfterMs) {
    g_sensorReady = false;
  }
}

Reading reading() {
  return g_reading;
}

const char *status() {
  if (!dfrcfg::kBme280Enabled) return "disabled";
  if (!g_wireReady) return "bus_error";
  if (!g_sensorReady || !g_reading.valid) return "unavailable";
  if (millis() - g_reading.lastUpdateMs >= dfrcfg::kBme280StaleAfterMs) return "stale";
  return "ready";
}

}  // namespace dfrbme
