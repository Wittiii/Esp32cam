#pragma once
#include <cstdint>
#include <string>
using String = std::string;
struct CaptureTestSerial { void println(const char *) {} };
extern CaptureTestSerial Serial;
template <typename T, typename Low, typename High>
T constrain(T value, Low low, High high) {
  return value < low ? static_cast<T>(low) : value > high ? static_cast<T>(high) : value;
}
