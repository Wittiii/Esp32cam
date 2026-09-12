#include <cassert>
#include <iostream>
#include "Arduino.h"
#include "dfr1154_server_capture.h"

CaptureTestSerial Serial;
bool captureStoreReady = false;
bool captureWriteFails = false;
unsigned captureWrites = 0;

int main() {
  assert(!dfrcapture::begin());
  assert(!dfrcapture::setEnabled(false));
  assert(!dfrcapture::setIntervalSeconds(10));
  assert(captureWrites == 0);
  captureStoreReady = true;
  assert(dfrcapture::begin());
  assert(dfrcapture::enabled());
  assert(dfrcapture::intervalSeconds() == 60);
  assert(dfrcapture::setEnabled(true));
  assert(dfrcapture::setIntervalSeconds(60));
  assert(captureWrites == 0);
  assert(dfrcapture::setEnabled(false));
  assert(captureWrites == 1 && !dfrcapture::enabled());
  assert(dfrcapture::setIntervalSeconds(0));
  assert(captureWrites == 2 && dfrcapture::intervalSeconds() == 5);
  assert(dfrcapture::setIntervalSeconds(1));
  assert(captureWrites == 2);
  assert(dfrcapture::setIntervalSeconds(UINT32_MAX));
  assert(captureWrites == 3 && dfrcapture::intervalSeconds() == 86400);
  captureWriteFails = true;
  assert(!dfrcapture::setEnabled(true));
  assert(!dfrcapture::enabled());
  assert(!dfrcapture::setIntervalSeconds(60));
  assert(dfrcapture::intervalSeconds() == 86400);
  assert(dfrcapture::begin());
  assert(!dfrcapture::enabled() && dfrcapture::intervalSeconds() == 86400);
  std::cout << "Capture persistence regression checks passed\n";
}
