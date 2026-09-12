#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct NimBLEAddress {
  uint8_t bytes[6] = {};
  const uint8_t *getVal() const { return bytes; }
};
struct NimBLEAdvertisedDevice {
  NimBLEAddress address;
  std::vector<uint8_t> payload;
  const NimBLEAddress &getAddress() const { return address; }
  const std::vector<uint8_t> &getPayload() const { return payload; }
  int getRSSI() const { return -60; }
};
struct NimBLEScanCallbacks {
  virtual ~NimBLEScanCallbacks() = default;
  virtual void onResult(const NimBLEAdvertisedDevice *) {}
};
struct NimBLEScan {
  bool scanning = true;
  bool stopFails = false;
  unsigned starts = 0;
  unsigned stops = 0;
  void (*onStop)() = nullptr;
  bool isScanning() const { return scanning; }
  bool start(uint32_t, bool, bool) {
    ++starts;
    scanning = true;
    return true;
  }
  bool stop() {
    ++stops;
    if (stopFails) {
      return false;
    }
    scanning = false;
    if (onStop) {
      onStop();
    }
    return true;
  }
  void setScanCallbacks(NimBLEScanCallbacks *, bool) {}
  void setActiveScan(bool) {}
  void setInterval(uint16_t) {}
  void setWindow(uint16_t) {}
  void setMaxResults(uint8_t) {}
};
extern NimBLEScan testScanner;
struct NimBLEDevice {
  static bool isInitialized() { return true; }
  static bool init(const char *) { return true; }
  static NimBLEScan *getScan() { return &testScanner; }
};
