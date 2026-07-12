#pragma once

#include <Arduino.h>

namespace dfrcapture {

bool begin();
bool setEnabled(bool enabled);
bool setIntervalSeconds(uint32_t intervalSeconds);

bool enabled();
uint32_t intervalSeconds();
String state();

}  // namespace dfrcapture
