#pragma once

#include <Arduino.h>

namespace dfrvictron {

struct Reading {
  bool configured;
  bool initialized;
  bool valid;
  uint8_t chargeState;
  uint8_t errorCode;
  float batteryVoltage;
  float batteryCurrent;
  float panelPower;
  uint32_t yieldTodayWh;
  float loadCurrent;
  int8_t rssi;
  uint32_t lastUpdateMs;
  uint32_t restartCount;
  uint32_t scanRestartCount;
  uint32_t advertisementCount;
  uint32_t decodeErrorCount;
};

void begin();
void loop();
Reading reading();
const char *status();
const char *chargeStateName(uint8_t state);

}  // namespace dfrvictron
