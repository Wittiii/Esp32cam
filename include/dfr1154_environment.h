#pragma once

#include <Arduino.h>

namespace dfrbme {

struct Reading {
  bool valid;
  uint8_t address;
  float temperatureC;
  float humidityPercent;
  float pressureHpa;
  float latestTemperatureC;
  float latestHumidityPercent;
  float latestPressureHpa;
  size_t averageSamples;
  uint32_t lastUpdateMs;
  uint32_t readFailures;
};

void begin();
void loop();
Reading reading();
const char *status();

}  // namespace dfrbme
