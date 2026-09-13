#include "dfr1154_camera_gain.h"

#include <cassert>
#include <climits>
#include <iostream>
#include <vector>

namespace {

void checkRawTableAndSensorWrites() {
  const uint16_t expected[] = {32, 64, 128, 248, 512, 1023};
  unsigned calls = 0;
  uint16_t writtenRaw = 0;
  for (int index = 0; index != 6; ++index) {
    uint16_t raw = 0;
    assert(dfrcamgain::rawLimit(index, raw));
    assert(raw == expected[index]);
    assert(dfrcamgain::applyGainCeiling(index, [&](uint16_t value) {
      ++calls;
      writtenRaw = value;
      return true;
    }));
    assert(writtenRaw == expected[index]);
  }
  assert(calls == 6);
  uint16_t defaultRaw = 0;
  assert(dfrcamgain::rawLimit(dfrcamgain::kDefaultIndex, defaultRaw));
  assert(defaultRaw == 0x00F8);
}

void checkInvalidIndicesNeverReachSensor() {
  unsigned calls = 0;
  for (int index : {-1, 6, 7, INT_MIN, INT_MAX}) {
    uint16_t raw = 123;
    assert(!dfrcamgain::validIndex(index));
    assert(!dfrcamgain::rawLimit(index, raw));
    assert(raw == 123);
    assert(!dfrcamgain::applyGainCeiling(index, [&](uint16_t) {
      ++calls;
      return true;
    }));
  }
  assert(calls == 0);
  assert(!dfrcamgain::applyGainCeiling(3, [&](uint16_t raw) {
    ++calls;
    assert(raw == 248);
    return false; // Sensor/SCCB rejection is propagated, not reported as success.
  }));
  assert(calls == 1);
}

void checkLegacyMigration() {
  for (uint8_t version : {0, 1, 2, 3}) {
    assert(dfrcamgain::loadIndex(0, true, version) == 3);
    assert(dfrcamgain::loadIndex(6, true, version) == 5);
    for (int index = 1; index <= 5; ++index)
      assert(dfrcamgain::loadIndex(index, true, version) == index);
    for (int invalid : {-1, 7, INT_MIN, INT_MAX})
      assert(dfrcamgain::loadIndex(invalid, true, version) == 3);
    assert(dfrcamgain::loadIndex(0, false, version) == 3);
    assert(dfrcamgain::loadIndex(5, false, version) == 3);
  }
}

void checkMigrationPersistenceOrderAndFailures() {
  std::vector<int> operations;
  assert(dfrcamgain::persistMigration(3, [&]() {
    operations.push_back(1);
    return true;
  }, [&](uint8_t version) {
    operations.push_back(2);
    assert(version == 4);
    return true;
  }));
  assert((operations == std::vector<int>{1, 2}));

  operations.clear();
  assert(!dfrcamgain::persistMigration(3, [&]() {
    operations.push_back(1);
    return false;
  }, [&](uint8_t) {
    operations.push_back(2);
    return true;
  }));
  assert((operations == std::vector<int>{1}));

  operations.clear();
  assert(!dfrcamgain::persistMigration(3, [&]() {
    operations.push_back(1);
    return true;
  }, [&](uint8_t) {
    operations.push_back(2);
    return false;
  }));
  assert((operations == std::vector<int>{1, 2}));
}

void checkRepeatedBootAndLaterUserChoice() {
  int savedIndex = 0;
  uint8_t savedVersion = 3;
  const int migratedIndex = dfrcamgain::loadIndex(savedIndex, true, savedVersion);
  assert(migratedIndex == 3);
  assert(dfrcamgain::persistMigration(savedVersion, [&]() {
    savedIndex = migratedIndex;
    return true;
  }, [&](uint8_t version) {
    savedVersion = version;
    return true;
  }));
  assert(savedVersion == 4 && savedIndex == 3);
  assert(dfrcamgain::loadIndex(savedIndex, true, savedVersion) == 3);

  // A deliberate 2x selection after migration must survive every later boot.
  savedIndex = 0;
  for (uint8_t version : {4, 5, 255}) {
    assert(dfrcamgain::loadIndex(savedIndex, true, version) == 0);
    unsigned writes = 0;
    assert(dfrcamgain::persistMigration(version, [&]() {
      ++writes;
      return true;
    }, [&](uint8_t) {
      ++writes;
      return true;
    }));
    assert(writes == 0);
  }
  assert(dfrcamgain::loadIndex(6, true, 4) == 3);
  assert(dfrcamgain::loadIndex(-1, true, 4) == 3);
  assert(dfrcamgain::loadIndex(0, false, 4) == 3);
}

}  // namespace

int main() {
  checkRawTableAndSensorWrites();
  checkInvalidIndicesNeverReachSensor();
  checkLegacyMigration();
  checkMigrationPersistenceOrderAndFailures();
  checkRepeatedBootAndLaterUserChoice();
  std::cout << "5 OV3660 gain/migration regression groups passed\n";
}
