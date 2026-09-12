#include "dfr1154_environment.h"

#include <Adafruit_BME280.h>
#include <Wire.h>
#include <array>
#include <math.h>

#include "dfr1154_config.h"
#include "dfr1154_pins.h"

namespace {

class BoundedBme280 : public Adafruit_BME280 {
 public:
  bool begin(uint8_t address, TwoWire *wire) {
    // Adafruit's init() waits forever when the calibration-ready bit stays
    // set. Bound that wait and propagate I2C errors so a bad sensor cannot
    // hold the camera/MQTT loop until the watchdog reboots the board.
    delete i2c_dev;
    i2c_dev = new Adafruit_I2CDevice(address, wire);
    if (!i2c_dev->begin()) return false;
    uint8_t chipId = 0;
    if (!readRegister(BME280_REGISTER_CHIPID, chipId) || chipId != 0x60) return false;
    _sensorID = chipId;
    const uint8_t reset[] = {BME280_REGISTER_SOFTRESET, 0xB6};
    if (!i2c_dev->write(reset, sizeof(reset))) return false;
    delay(10);
    const uint32_t start = millis();
    for (;;) {
      uint8_t status = 0;
      if (!readRegister(BME280_REGISTER_STATUS, status)) return false;
      if ((status & 0x01) == 0) break;
      if (millis() - start >= 200UL) return false;
      delay(5);
    }
    readCoefficients();
    if (_bme280_calib.dig_T1 == 0 || _bme280_calib.dig_T1 == 0xFFFF ||
        _bme280_calib.dig_P1 == 0 || _bme280_calib.dig_P1 == 0xFFFF ||
        !readRegister(BME280_REGISTER_CHIPID, chipId) || chipId != 0x60) return false;
    setSampling();
    delay(100);
    return true;
  }

  bool connected() {
    uint8_t chipId = 0;
    return readRegister(BME280_REGISTER_CHIPID, chipId) && chipId == 0x60;
  }

 private:
  bool readRegister(uint8_t reg, uint8_t &value) {
    return i2c_dev != nullptr && i2c_dev->write_then_read(&reg, 1, &value, 1);
  }
};

TwoWire g_bmeWire(1);
BoundedBme280 g_bme;
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
  if (!g_bme.connected()) {
    ++g_reading.readFailures;
    return false;
  }
  const float temperature = g_bme.readTemperature();
  const float humidity = g_bme.readHumidity();
  const float pressure = g_bme.readPressure() / 100.0f;
  if (!g_bme.connected() || !isfinite(temperature) || !isfinite(humidity) || !isfinite(pressure) ||
      temperature < -40.0f || temperature > 85.0f ||
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
  g_bmeWire.setTimeOut(50);
  if (probeSensor()) {
    takeReading();
    g_lastReadMs = millis();
  }
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
