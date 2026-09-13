#pragma once

#include <stdint.h>

namespace dfrcamgain {

constexpr int kDefaultIndex = 3;
constexpr int kMaximumIndex = 5;
constexpr uint8_t kSettingsVersion = 4;

// MQTT/NVS retain the selection index. OV3660's set_gainceiling instead
// writes a raw 10-bit limit to 0x3A18/0x3A19 (unlike OV2640's enum indices).
// Index 3 preserves Espressif's normal 0x00F8 register default (~16x).
constexpr uint16_t kRawLimits[] = {32, 64, 128, 248, 512, 1023};

inline bool validIndex(int index) {
  return index >= 0 && index <= kMaximumIndex;
}

inline bool rawLimit(int index, uint16_t &raw) {
  if (!validIndex(index)) return false;
  raw = kRawLimits[index];
  return true;
}

template <typename WriteRaw>
bool applyGainCeiling(int index, WriteRaw writeRaw) {
  uint16_t raw = 0;
  return rawLimit(index, raw) && writeRaw(raw);
}

inline int loadIndex(int storedIndex, bool settingExists, uint8_t settingsVersion) {
  if (!settingExists) return kDefaultIndex;
  if (settingsVersion < kSettingsVersion) {
    // Old default 0 disabled the sensor's automatic gain headroom. Index 6
    // advertised 128x, beyond this sensor's 10-bit limit: retain at most 64x.
    if (storedIndex == 0) return kDefaultIndex;
    if (storedIndex == 6) return kMaximumIndex;
  }
  return validIndex(storedIndex) ? storedIndex : kDefaultIndex;
}

template <typename SaveGain, typename SaveVersion>
bool persistMigration(uint8_t settingsVersion, SaveGain saveGain, SaveVersion saveVersion) {
  // Never mark a failed gain write as migrated, and never remigrate a user's
  // later explicit choice of index 0 or downgrade a future settings version.
  return settingsVersion >= kSettingsVersion ||
         (saveGain() && saveVersion(kSettingsVersion));
}

}  // namespace dfrcamgain
