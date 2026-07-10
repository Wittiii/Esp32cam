#pragma once

#include <stddef.h>
#include <stdint.h>

#include "CStreamer.h"

using JpegFrameObserver =
    void (*)(const uint8_t *data, size_t length, uint16_t width, uint16_t height, uint32_t capturedAtMs);

class Esp32RtspStreamer : public CStreamer {
 public:
  Esp32RtspStreamer(
      unsigned short width,
      unsigned short height,
      JpegFrameObserver frameObserver = nullptr);

  void streamImage(uint32_t curMsec) override;

 private:
  JpegFrameObserver frameObserver_;
};
