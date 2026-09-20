#include "dfr1154_environment.h"

#include <Adafruit_BME280.h>
#include <Wire.h>
#include <array>
#include <math.h>

#include "dfr1154_config.h"
#include "dfr1154_pins.h"

namespace {

TwoWire g_bmeWire(1);
Adafruit_BME280 g_bme;
dfrbme::Reading g_reading = {false, 0, NAN, NAN, NAN, NAN, NAN, NAN, 0, 0, 0};
bool g_wireReady = false;
bool g_sensorReady = false;
uint32_t g_lastProbeMs = 0;
uint32_t g_lastReadMs = 0;
uint32_t g_lastWireAttemptMs = 0;

struct Sample {
  float temperatureC;
  float humidityPercent;
  float pressureHpa;
};

static_assert(dfrcfg::kBme280AverageSamples > 0, "BME280 average needs at least one sample");
std::array<Sample, dfrcfg::kBme280AverageSamples> g_samples = {};
size_t g_sampleCount = 0;
size_t g_sampleIndex = 0;
double g_temperatureSum = 0.0;
double g_humiditySum = 0.0;
double g_pressureSum = 0.0;

void resetSampleWindow() {
  g_samples.fill({0.0f, 0.0f, 0.0f});
  g_sampleCount = 0;
  g_sampleIndex = 0;
  g_temperatureSum = 0.0;
  g_humiditySum = 0.0;
  g_pressureSum = 0.0;
}

void markReadingUnavailable() {
  g_reading.valid = false;
  g_reading.address = 0;
  g_reading.averageSamples = 0;
  resetSampleWindow();
}

bool probeSensor() {
  g_lastProbeMs = millis();
  for (const uint8_t address : {0x76, 0x77}) {
    if (g_bme.begin(address, &g_bmeWire)) {
      resetSampleWindow();
      g_reading.valid = false;
      g_sensorReady = true;
      g_reading.address = address;
      Serial.printf("[BME280] ready address=0x%02X SDA=%d SCL=%d\n",
                    address, DFR_GRAVITY_SDA, DFR_GRAVITY_SCL);
      return true;
    }
  }
  g_sensorReady = false;
  markReadingUnavailable();
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

  if (g_sampleCount == g_samples.size()) {
    const Sample &oldest = g_samples[g_sampleIndex];
    g_temperatureSum -= oldest.temperatureC;
    g_humiditySum -= oldest.humidityPercent;
    g_pressureSum -= oldest.pressureHpa;
  } else {
    ++g_sampleCount;
  }

  g_samples[g_sampleIndex] = {temperature, humidity, pressure};
  g_sampleIndex = (g_sampleIndex + 1) % g_samples.size();
  g_temperatureSum += temperature;
  g_humiditySum += humidity;
  g_pressureSum += pressure;

  g_reading.temperatureC = static_cast<float>(g_temperatureSum / g_sampleCount);
  g_reading.humidityPercent = static_cast<float>(g_humiditySum / g_sampleCount);
  g_reading.pressureHpa = static_cast<float>(g_pressureSum / g_sampleCount);
  g_reading.latestTemperatureC = temperature;
  g_reading.latestHumidityPercent = humidity;
  g_reading.latestPressureHpa = pressure;
  g_reading.averageSamples = g_sampleCount;
  g_reading.lastUpdateMs = millis();
  g_reading.valid = true;
  return true;
}

}  // namespace

namespace dfrbme {

void begin() {
  if (!dfrcfg::kBme280Enabled || g_wireReady) return;
  g_lastWireAttemptMs = millis();
  g_wireReady = g_bmeWire.begin(DFR_GRAVITY_SDA, DFR_GRAVITY_SCL, 100000);
  if (!g_wireReady) {
    markReadingUnavailable();
    Serial.println("[BME280] second I2C bus initialization failed");
    return;
  }
  if (probeSensor()) takeReading();
}

void loop() {
  if (!dfrcfg::kBme280Enabled) return;
  const uint32_t now = millis();
  if (!g_wireReady) {
    if (g_lastWireAttemptMs == 0 ||
        now - g_lastWireAttemptMs >= dfrcfg::kBme280RetryIntervalMs) {
      begin();
    }
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
    markReadingUnavailable();
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
