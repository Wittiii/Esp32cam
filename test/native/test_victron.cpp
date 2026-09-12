#include <cassert>
#include <cmath>
#include <cstdio>

// Compile the production implementation against deterministic hardware stubs.
// This translation unit never includes the user's secret/config files.
#include "../../src/dfr1154_victron.cpp"

uint32_t testNowMs = 1000;
void (*testBeforeCritical)() = nullptr;
unsigned testSerialLines = 0;
unsigned testReboots = 0;
bool testAesFails = false;
TestSerial Serial;
TestEsp ESP;
NimBLEScan testScanner;

void resetState() {
  testNowMs = 1000;
  testBeforeCritical = nullptr;
  testSerialLines = 0;
  testReboots = 0;
  testAesFails = false;
  testScanner = {};
  g_scan = &testScanner;
  g_reading = {};
  g_reading.configured = true;
  g_reading.initialized = true;
  g_reading.valid = true;
  g_reading.lastUpdateMs = testNowMs;
  g_initializedAtMs = 100;
  g_lastScanRestartCheckMs = testNowMs;
  g_lastSoftRecoveryMs = 0;
  g_lastInitAttemptMs = 0;
  g_initFailureCount = 0;
  g_haveNonce = false;
  g_firstReadingPending = false;
  g_recoveryRtcMagic = kRecoveryRtcMagic;
  g_totalRecoveryRebootCount = 0;
  g_consecutiveRecoveryRebootCount = 0;
  g_recoveryAwaitingData = false;
  g_configurationLoaded = false;
  g_scannerEverStarted = false;
  g_startRequested = false;
  memset(g_encryptionKey, 0, sizeof(g_encryptionKey));
  memset(g_targetMacBytes, 0, sizeof(g_targetMacBytes));
}

void put16(std::vector<uint8_t> &data, size_t offset, uint16_t value) {
  data[offset] = static_cast<uint8_t>(value);
  data[offset + 1] = static_cast<uint8_t>(value >> 8);
}

std::vector<uint8_t> solarPacket(uint16_t nonce = 1) {
  std::vector<uint8_t> data(22, 0);
  put16(data, 0, kVictronCompanyId);
  data[kRecordOffset] = kProductAdvertisement;
  data[kDeviceTypeOffset] = kSolarChargerRecord;
  put16(data, kNonceOffset, nonce);
  data[kCiphertextOffset] = 3;
  put16(data, kCiphertextOffset + 2, 1280);  // 12.8 V
  put16(data, kCiphertextOffset + 4, static_cast<uint16_t>(-15));  // -1.5 A
  put16(data, kCiphertextOffset + 6, 234);  // 2340 Wh
  put16(data, kCiphertextOffset + 8, 123);  // 123 W
  put16(data, kCiphertextOffset + 10, 17);  // 1.7 A
  return data;
}

void testCallbackBeforeSnapshotDoesNotReboot() {
  resetState();
  testBeforeCritical = []() {
    testBeforeCritical = nullptr;
    ++testNowMs;
    g_reading.lastUpdateMs = testNowMs;
  };
  dfrvictron::loop();
  assert(testReboots == 0);
  assert(testScanner.stops == 0);
  assert(g_totalRecoveryRebootCount == 0);
}

void testCallbackDuringScanRecoveryDoesNotReboot() {
  resetState();
  testNowMs = 100000;
  g_reading.lastUpdateMs = 50000;
  testScanner.onStop = []() {
    ++testNowMs;
    g_reading.lastUpdateMs = testNowMs;
  };
  dfrvictron::loop();
  assert(testScanner.stops == 1);
  assert(testScanner.starts == 1);
  assert(testReboots == 0);
  assert(g_totalRecoveryRebootCount == 0);
}

void testMillisWrapAndRecoveryLimit() {
  resetState();
  testNowMs = 10;
  g_reading.lastUpdateMs = UINT32_MAX - 10;
  dfrvictron::loop();
  assert(testReboots == 0);
  assert(testScanner.stops == 0);

  g_reading.lastUpdateMs = testNowMs - dfrcfg::kVictronRecoveryRebootAfterMs;
  dfrvictron::loop();
  assert(testReboots == 1);
  assert(g_totalRecoveryRebootCount == 1);
  g_consecutiveRecoveryRebootCount = dfrcfg::kVictronMaxRecoveryReboots;
  dfrvictron::loop();
  assert(testReboots == 1);
}

void testFailedStopIsNotSuccessfulRecovery() {
  resetState();
  testNowMs = 100000;
  g_reading.lastUpdateMs = 50000;
  testScanner.stopFails = true;
  dfrvictron::loop();
  assert(testScanner.stops == 1);
  assert(testScanner.starts == 0);
  assert(g_initFailureCount == 1);
  assert(!g_reading.initialized);
}

void testSolarParsingAndDuplicateNonce() {
  resetState();
  g_reading.valid = false;
  auto data = solarPacket(UINT16_MAX);
  assert(decodeSolarAdvertisement(data.data(), data.size(), -70));
  assert(g_reading.valid);
  assert(std::fabs(g_reading.batteryVoltage - 12.8f) < 0.001f);
  assert(std::fabs(g_reading.batteryCurrent + 1.5f) < 0.001f);
  assert(g_reading.yieldTodayWh == 2340);
  assert(g_reading.panelPower == 123);
  assert(std::fabs(g_reading.loadCurrent - 1.7f) < 0.001f);
  assert(g_reading.rssi == -70);
  assert(testSerialLines == 0);  // No Serial from the BLE callback.
  assert(g_firstReadingPending);
  ++testNowMs;
  assert(!decodeSolarAdvertisement(data.data(), data.size(), -60));
  assert(g_reading.lastUpdateMs == 1000);
  put16(data, kNonceOffset, 0);  // Nonce rollover is valid.
  assert(decodeSolarAdvertisement(data.data(), data.size(), -60));
  assert(g_reading.lastUpdateMs == 1001);
  dfrvictron::loop();
  assert(!g_firstReadingPending);
  assert(testSerialLines == 1);
}

void testIncompleteMeasurementsDoNotCorruptEnergy() {
  for (size_t field : {size_t(6), size_t(8)}) {
    resetState();
    auto data = solarPacket();
    assert(decodeSolarAdvertisement(data.data(), data.size(), -70));
    ++testNowMs;
    put16(data, kNonceOffset, 2);
    put16(data, kCiphertextOffset + field, 0xFFFF);
    assert(!decodeSolarAdvertisement(data.data(), data.size(), -70));
    assert(g_reading.yieldTodayWh == 2340);
    assert(g_reading.panelPower == 123);
    assert(g_reading.lastUpdateMs == 1000);
    assert(g_reading.decodeErrorCount == 1);
    assert(g_lastNonce == 1);
  }
}

void testMalformedAndExtendedPackets() {
  resetState();
  auto data = solarPacket();
  for (size_t size = 0; size < data.size(); ++size) {
    assert(!decodeSolarAdvertisement(data.data(), size, -60));
  }
  testAesFails = true;
  assert(!decodeSolarAdvertisement(data.data(), data.size(), -60));
  assert(!g_haveNonce);
  testAesFails = false;
  data.resize(64, 0xAA);  // Future appended fields must remain compatible.
  assert(decodeSolarAdvertisement(data.data(), data.size(), -60));
  assert(g_reading.yieldTodayWh == 2340);
}

void testAdTraversalAndBinaryAddress() {
  resetState();
  g_reading.initialized = false;
  dfrvictron::begin();
  NimBLEAdvertisedDevice device;
  const uint8_t address[] = {6, 5, 4, 3, 2, 1};
  memcpy(device.address.bytes, address, sizeof(address));
  auto packet = solarPacket();
  device.payload = {2, 1, 6};  // Flags, then manufacturer data.
  device.payload.push_back(static_cast<uint8_t>(packet.size() + 1));
  device.payload.push_back(0xFF);
  device.payload.insert(device.payload.end(), packet.begin(), packet.end());
  processAdvertisement(&device);
  assert(g_reading.advertisementCount == 1);
  assert(g_reading.yieldTodayWh == 2340);
  ++device.address.bytes[0];
  processAdvertisement(&device);
  assert(g_reading.advertisementCount == 1);
  --device.address.bytes[0];
  device.payload = {5, 0xFF, 0x01};  // Length runs past the buffer.
  processAdvertisement(&device);
  assert(g_reading.decodeErrorCount == 1);
  device.payload = {1, 0xFF};  // Manufacturer type with no data.
  processAdvertisement(&device);
  assert(g_reading.decodeErrorCount == 2);
  device.payload = {0};
  processAdvertisement(&device);
  assert(g_reading.decodeErrorCount == 2);
}

int main() {
  testCallbackBeforeSnapshotDoesNotReboot();
  testCallbackDuringScanRecoveryDoesNotReboot();
  testMillisWrapAndRecoveryLimit();
  testFailedStopIsNotSuccessfulRecovery();
  testSolarParsingAndDuplicateNonce();
  testIncompleteMeasurementsDoNotCorruptEnergy();
  testMalformedAndExtendedPackets();
  testAdTraversalAndBinaryAddress();
  std::puts("Victron: 8 native regression tests passed (AES/hardware stubbed)");
}
