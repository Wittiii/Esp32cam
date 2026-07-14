#pragma once

#include <stddef.h>
#include <stdint.h>

#include "CStreamer.h"

class Esp32RtspStreamer : public CStreamer {
 public:
  Esp32RtspStreamer(unsigned short width, unsigned short height);

  void streamImage(uint32_t curMsec) override;
  bool lastFrameSucceeded() const { return lastFrameSucceeded_; }
  uint32_t consecutiveCaptureFailures() const { return consecutiveCaptureFailures_; }

 private:
  bool lastFrameSucceeded_ = false;
  uint32_t consecutiveCaptureFailures_ = 0;
};
