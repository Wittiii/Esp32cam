#include <Arduino.h>
#include "esp32_cam_controller.h"

void setup() {
  esp32cam::setupController();
}

void loop() {
  esp32cam::loopController();
}
