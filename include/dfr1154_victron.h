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
  uint16_t yieldTodayWh;
  float loadCurrent;
  int8_t rssi;
  uint32_t lastUpdateMs;
};

void begin();
void loop();
Reading reading();
const char *status();
const char *chargeStateName(uint8_t state);

}  // namespace dfrvictron
