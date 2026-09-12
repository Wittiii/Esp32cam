#pragma once

#include <cstddef>
#include <cstdint>

extern uint32_t testNowMs;
extern void (*testBeforeCritical)();
extern unsigned testSerialLines;
extern unsigned testReboots;

using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
inline void portENTER_CRITICAL(portMUX_TYPE *) {
  if (testBeforeCritical) {
    testBeforeCritical();
  }
}
inline void portEXIT_CRITICAL(portMUX_TYPE *) {}
inline uint32_t millis() { return testNowMs; }
inline void delay(uint32_t ms) { testNowMs += ms; }
inline void feedLoopWDT() {}
inline void enableLoopWDT() {}
inline void disableLoopWDT() {}

struct TestSerial {
  void println(const char *) { ++testSerialLines; }
  template <typename... Args>
  void printf(const char *, Args...) { ++testSerialLines; }
  void flush() {}
};
struct TestEsp {
  void restart() { ++testReboots; }
};
extern TestSerial Serial;
extern TestEsp ESP;
