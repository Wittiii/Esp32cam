#pragma once

#ifdef MICRO_RTSP_PLATFORM_HEADER
#include MICRO_RTSP_PLATFORM_HEADER
#elif defined(ARDUINO_ARCH_ESP32)
#include "platglue-esp32.h"
#else
#include "platglue-posix.h"
#endif
