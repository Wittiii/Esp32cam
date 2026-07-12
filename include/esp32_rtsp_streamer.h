#pragma once

#include <stddef.h>
#include <stdint.h>

#include "CStreamer.h"

class Esp32RtspStreamer : public CStreamer {
 public:
  Esp32RtspStreamer(unsigned short width, unsigned short height);

  void streamImage(uint32_t curMsec) override;

};
