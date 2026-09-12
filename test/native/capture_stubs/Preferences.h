#pragma once
#include <cstddef>
#include <cstdint>
extern bool captureStoreReady;
extern bool captureWriteFails;
extern unsigned captureWrites;
struct Preferences {
  bool begin(const char *, bool) { return captureStoreReady; }
  bool getBool(const char *, bool fallback) { return fallback; }
  uint32_t getUInt(const char *, uint32_t fallback) { return fallback; }
  size_t putBool(const char *, bool) { ++captureWrites; return captureWriteFails ? 0 : sizeof(uint8_t); }
  size_t putUInt(const char *, uint32_t) { ++captureWrites; return captureWriteFails ? 0 : sizeof(uint32_t); }
};
