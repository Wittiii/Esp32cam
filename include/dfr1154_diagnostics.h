#pragma once

#include <Arduino.h>
#include <esp_system.h>

namespace dfrdiag {
// Main-loop instrumentation only: no extra tasks, watchdog resets or flash writes.
void begin(esp_reset_reason_t reason);
const String &bootReport();
String timingReport();

class Scope {
 public:
  explicit Scope(const char *stage);
  ~Scope();
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
 private:
  const char *stage_;
  char previous_[40];
  uint32_t previousStarted_;
  uint32_t started_;
};
}  // namespace dfrdiag
