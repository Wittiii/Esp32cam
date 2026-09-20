#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace mqttsocketstub {
static uint32_t nowMs = 0;
static unsigned delays = 0;
static unsigned sends = 0;
static std::function<std::ptrdiff_t(int, const uint8_t *, size_t, int)> sendHandler;

inline void reset(uint32_t now = 0) {
  nowMs = now;
  delays = 0;
  sends = 0;
  sendHandler = {};
}
}  // namespace mqttsocketstub

inline uint32_t millis() { return mqttsocketstub::nowMs; }
inline void delay(uint32_t duration) {
  ++mqttsocketstub::delays;
  mqttsocketstub::nowMs += duration;
}

constexpr int MSG_DONTWAIT = 0x40;
inline std::ptrdiff_t send(int descriptor, const void *bytes, size_t length, int flags) {
  assert(descriptor >= 0);
  assert(flags == MSG_DONTWAIT);
  assert(mqttsocketstub::sendHandler);
  ++mqttsocketstub::sends;
  return mqttsocketstub::sendHandler(descriptor, static_cast<const uint8_t *>(bytes), length, flags);
}

class WiFiClient {
 public:
  virtual ~WiFiClient() = default;
  virtual size_t write(uint8_t) { return 0; }
  virtual size_t write(const uint8_t *, size_t) { return 0; }
  uint8_t connected() { return online; }
  int fd() const { return descriptor; }
  void stop() {
    ++stops;
    online = false;
    descriptor = -1;
  }
  int connect(const char *, uint16_t) {
    online = true;
    descriptor = 7;
    return 1;
  }

  bool online = true;
  int descriptor = 7;
  unsigned stops = 0;
};
