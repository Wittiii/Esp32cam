#include <Arduino.h>

#include "dfr1154_cam_controller.h"

void setup() {
  dfr1154::setupController();
}

void loop() {
  dfr1154::loopController();
}
