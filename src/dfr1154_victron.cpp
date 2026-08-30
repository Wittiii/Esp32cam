#include "dfr1154_victron.h"

#include <NimBLEDevice.h>
#include <ctype.h>
#include <math.h>
#include <mbedtls/aes.h>
#include <string.h>

#include "dfr1154_config.h"

namespace {

constexpr uint32_t kRecoveryRtcMagic = 0x56424C45;
constexpr uint16_t kVictronCompanyId = 0x02E1;
constexpr uint8_t kProductAdvertisement = 0x10;
constexpr uint8_t kSolarChargerRecord = 0x01;
constexpr size_t kKeyLength = 16;
constexpr size_t kMacHexLength = 12;
constexpr size_t kAdvertisementHeaderLength = 10;
constexpr size_t kMinimumSolarPayloadLength = 12;
constexpr size_t kMaximumCiphertextLength = 21;

constexpr size_t kRecordOffset = 2;
constexpr size_t kDeviceTypeOffset = 6;
constexpr size_t kNonceOffset = 7;
constexpr size_t kKeyCheckOffset = 9;
constexpr size_t kCiphertextOffset = 10;

RTC_DATA_ATTR uint32_t g_recoveryRtcMagic = 0;
RTC_DATA_ATTR uint32_t g_totalRecoveryRebootCount = 0;
RTC_DATA_ATTR uint8_t g_consecutiveRecoveryRebootCount = 0;
RTC_DATA_ATTR bool g_recoveryAwaitingData = false;

dfrvictron::Reading g_reading = {};
portMUX_TYPE g_readingMux = portMUX_INITIALIZER_UNLOCKED;
NimBLEScan *g_scan = nullptr;
uint8_t g_encryptionKey[kKeyLength] = {};
char g_normalizedTargetMac[kMacHexLength + 1] = {};
uint16_t g_lastNonce = 0;
uint32_t g_initializedAtMs = 0;
uint32_t g_lastInitAttemptMs = 0;
uint32_t g_lastScanRestartCheckMs = 0;
uint32_t g_lastSoftRecoveryMs = 0;
uint32_t g_initFailureCount = 0;
bool g_startRequested = false;
bool g_haveNonce = false;
bool g_scannerEverStarted = false;

uint16_t readLe16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

int hexNibble(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  value = static_cast<char>(tolower(static_cast<unsigned char>(value)));
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}

bool parseEncryptionKey(const char *hex, uint8_t output[kKeyLength]) {
  if (hex == nullptr || strlen(hex) != kKeyLength * 2) return false;
  for (size_t index = 0; index < kKeyLength; ++index) {
    const int high = hexNibble(hex[index * 2]);
    const int low = hexNibble(hex[index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    output[index] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool normalizeMac(const char *input, char output[kMacHexLength + 1]) {
  if (input == nullptr) return false;
  size_t outputIndex = 0;
  for (size_t inputIndex = 0; input[inputIndex] != '\0'; ++inputIndex) {
    const unsigned char value = static_cast<unsigned char>(input[inputIndex]);
    if (value == ':' || value == '-') continue;
    if (!isxdigit(value) || outputIndex >= kMacHexLength) return false;
    output[outputIndex++] = static_cast<char>(tolower(value));
  }
  if (outputIndex != kMacHexLength) return false;
  output[outputIndex] = '\0';
  return true;
}

bool hasValidConfiguration() {
  uint8_t ignoredKey[kKeyLength];
  char ignoredMac[kMacHexLength + 1];
  return dfrcfg::kVictronEnabled &&
         parseEncryptionKey(dfrcfg::kVictronEncryptionKey, ignoredKey) &&
         normalizeMac(dfrcfg::kVictronMac, ignoredMac);
}

bool decryptPayload(const uint8_t *ciphertext,
                    size_t length,
                    uint16_t nonce,
                    uint8_t *plaintext) {
  uint8_t counter[16] = {};
  uint8_t streamBlock[16] = {};
  size_t counterOffset = 0;
  counter[0] = static_cast<uint8_t>(nonce & 0xFF);
  counter[1] = static_cast<uint8_t>(nonce >> 8);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  const int setKeyResult = mbedtls_aes_setkey_enc(&aes, g_encryptionKey, 128);
  const int decryptResult =
      setKeyResult == 0
          ? mbedtls_aes_crypt_ctr(&aes,
                                  length,
                                  &counterOffset,
                                  counter,
                                  streamBlock,
                                  ciphertext,
                                  plaintext)
          : setKeyResult;
  mbedtls_aes_free(&aes);
  return decryptResult == 0;
}

void recordDecodeError() {
  portENTER_CRITICAL(&g_readingMux);
  ++g_reading.decodeErrorCount;
  portEXIT_CRITICAL(&g_readingMux);
}

bool decodeSolarAdvertisement(const uint8_t *data,
                              size_t length,
                              int8_t rssi) {
  if (data == nullptr ||
      length < kAdvertisementHeaderLength + kMinimumSolarPayloadLength) {
    recordDecodeError();
    return false;
  }
  if (readLe16(data) != kVictronCompanyId ||
      data[kRecordOffset] != kProductAdvertisement ||
      data[kDeviceTypeOffset] != kSolarChargerRecord) {
    return false;
  }

  portENTER_CRITICAL(&g_readingMux);
  ++g_reading.advertisementCount;
  portEXIT_CRITICAL(&g_readingMux);

  if (data[kKeyCheckOffset] != g_encryptionKey[0]) {
    recordDecodeError();
    return false;
  }

  const uint16_t nonce = readLe16(data + kNonceOffset);
  if (g_haveNonce && nonce == g_lastNonce) return false;

  size_t ciphertextLength = length - kCiphertextOffset;
  if (ciphertextLength > kMaximumCiphertextLength) {
    ciphertextLength = kMaximumCiphertextLength;
  }
  uint8_t plaintext[kMaximumCiphertextLength] = {};
  if (!decryptPayload(data + kCiphertextOffset,
                      ciphertextLength,
                      nonce,
                      plaintext)) {
    recordDecodeError();
    return false;
  }

  const float batteryVoltage =
      static_cast<int16_t>(readLe16(plaintext + 2)) * 0.01f;
  const float batteryCurrent =
      static_cast<int16_t>(readLe16(plaintext + 4)) * 0.1f;
  const uint16_t loadRaw = readLe16(plaintext + 10) & 0x01FF;

  // AES-CTR has no authentication tag. Reject clearly impossible output if
  // a wrong key happens to share the same first-byte check value.
  if (!isfinite(batteryVoltage) || batteryVoltage < 0.0f ||
      batteryVoltage > 100.0f || !isfinite(batteryCurrent) ||
      batteryCurrent < -500.0f || batteryCurrent > 500.0f) {
    recordDecodeError();
    return false;
  }

  bool firstReading = false;
  portENTER_CRITICAL(&g_readingMux);
  firstReading = !g_reading.valid;
  g_reading.valid = true;
  g_reading.chargeState = plaintext[0];
  g_reading.errorCode = plaintext[1];
  g_reading.batteryVoltage = batteryVoltage;
  g_reading.batteryCurrent = batteryCurrent;
  g_reading.yieldTodayWh = readLe16(plaintext + 6) * 10UL;
  g_reading.panelPower = readLe16(plaintext + 8);
  g_reading.loadCurrent = loadRaw == 0x01FF ? 0.0f : loadRaw * 0.1f;
  g_reading.rssi = rssi;
  g_reading.lastUpdateMs = millis();
  g_consecutiveRecoveryRebootCount = 0;
  g_recoveryAwaitingData = false;
  portEXIT_CRITICAL(&g_readingMux);

  g_lastNonce = nonce;
  g_haveNonce = true;
  if (firstReading) {
    Serial.println("[Victron] first valid SmartSolar advertisement received");
  }
  return true;
}

void initializeRecoveryState() {
  if (g_recoveryRtcMagic == kRecoveryRtcMagic) return;
  g_recoveryRtcMagic = kRecoveryRtcMagic;
  g_totalRecoveryRebootCount = 0;
  g_consecutiveRecoveryRebootCount = 0;
  g_recoveryAwaitingData = false;
}

void setInitializationState(bool initialized) {
  portENTER_CRITICAL(&g_readingMux);
  g_reading.initialized = initialized;
  portEXIT_CRITICAL(&g_readingMux);
}

void recordInitFailure(const char *message) {
  ++g_initFailureCount;
  setInitializationState(false);
  Serial.printf("[Victron] %s; retry in %lu seconds\n",
                message,
                dfrcfg::kVictronInitRetryMs / 1000UL);
}

bool addressMatches(const NimBLEAddress &address) {
  char normalized[kMacHexLength + 1];
  return normalizeMac(address.toString().c_str(), normalized) &&
         strcmp(normalized, g_normalizedTargetMac) == 0;
}

void processAdvertisement(NimBLEAdvertisedDevice *device) {
  if (device == nullptr || !addressMatches(device->getAddress()) ||
      !device->haveManufacturerData()) {
    return;
  }
  const std::string manufacturerData = device->getManufacturerData();
  decodeSolarAdvertisement(
      reinterpret_cast<const uint8_t *>(manufacturerData.data()),
      manufacturerData.size(),
      static_cast<int8_t>(device->getRSSI()));
}

class VictronAdvertisementCallbacks : public NimBLEAdvertisedDeviceCallbacks {
 public:
  void onResult(NimBLEAdvertisedDevice *device) override {
    processAdvertisement(device);
  }
};

VictronAdvertisementCallbacks g_advertisementCallbacks;

bool startScanner() {
  if (g_scan == nullptr) return false;
  if (g_scan->isScanning()) return true;
  const bool started = g_scan->start(0, nullptr, false);
  if (started) {
    if (g_scannerEverStarted) {
      portENTER_CRITICAL(&g_readingMux);
      ++g_reading.scanRestartCount;
      portEXIT_CRITICAL(&g_readingMux);
    }
    g_scannerEverStarted = true;
  }
  return started;
}

void softRecoverScanner(uint32_t now) {
  if (g_scan == nullptr ||
      (g_lastSoftRecoveryMs != 0 &&
       now - g_lastSoftRecoveryMs < dfrcfg::kVictronSoftRecoveryIntervalMs)) {
    return;
  }
  g_lastSoftRecoveryMs = now;
  if (g_scan->isScanning()) g_scan->stop();
  if (startScanner()) {
    Serial.println("[Victron] stale/no data; NimBLE scanner restarted");
  } else {
    recordInitFailure("NimBLE scanner recovery failed");
  }
}

}  // namespace

namespace dfrvictron {

void begin() {
  g_startRequested = true;
  initializeRecoveryState();
  if (reading().initialized) return;
  const uint32_t now = millis();
  if (g_lastInitAttemptMs != 0 &&
      now - g_lastInitAttemptMs < dfrcfg::kVictronInitRetryMs) {
    return;
  }
  g_lastInitAttemptMs = now;

  const bool configured = hasValidConfiguration();
  portENTER_CRITICAL(&g_readingMux);
  g_reading.configured = configured;
  portEXIT_CRITICAL(&g_readingMux);
  if (!configured) {
    Serial.println("[Victron] invalid MAC or encryption key configuration");
    return;
  }
  if (!parseEncryptionKey(dfrcfg::kVictronEncryptionKey, g_encryptionKey) ||
      !normalizeMac(dfrcfg::kVictronMac, g_normalizedTargetMac)) {
    recordInitFailure("device configuration rejected");
    return;
  }

  feedLoopWDT();
  if (!NimBLEDevice::getInitialized()) NimBLEDevice::init("");
  g_scan = NimBLEDevice::getScan();
  if (g_scan == nullptr) {
    recordInitFailure("NimBLE initialization failed");
    return;
  }
  g_scan->setAdvertisedDeviceCallbacks(&g_advertisementCallbacks, true);
  g_scan->setActiveScan(false);
  g_scan->setInterval(dfrcfg::kVictronBleScanIntervalMs);
  g_scan->setWindow(dfrcfg::kVictronBleScanWindowMs);
  g_scan->setMaxResults(0);
  feedLoopWDT();

  if (!startScanner()) {
    recordInitFailure("NimBLE scanner start failed");
    return;
  }
  setInitializationState(true);
  g_initializedAtMs = millis();
  g_lastScanRestartCheckMs = g_initializedAtMs;
  Serial.printf("[Victron] NimBLE scanner ready for %s\n", dfrcfg::kVictronName);
}

void loop() {
  if (!g_reading.initialized) {
    if (g_startRequested) begin();
    return;
  }
  const uint32_t now = millis();
  const Reading current = reading();

  const bool timedOutWaitingForFirstData =
      !current.valid && g_initializedAtMs != 0 &&
      now - g_initializedAtMs >= dfrcfg::kVictronFirstDataTimeoutMs;
  const bool dataIsStale =
      current.valid && now - current.lastUpdateMs >= dfrcfg::kVictronStaleAfterMs;
  if (timedOutWaitingForFirstData || dataIsStale) softRecoverScanner(now);

  bool recoveryRebootRequired = false;
  uint8_t recoveryAttempt = 0;
  portENTER_CRITICAL(&g_readingMux);
  const bool dataStaleForRecovery =
      g_reading.valid &&
      now - g_reading.lastUpdateMs >= dfrcfg::kVictronRecoveryRebootAfterMs;
  const bool recoveryStillHasNoData =
      !g_reading.valid && g_initializedAtMs != 0 &&
      now - g_initializedAtMs >= dfrcfg::kVictronRecoveryRebootAfterMs;
  if ((dataStaleForRecovery || recoveryStillHasNoData) &&
      g_consecutiveRecoveryRebootCount < dfrcfg::kVictronMaxRecoveryReboots) {
    ++g_totalRecoveryRebootCount;
    ++g_consecutiveRecoveryRebootCount;
    g_recoveryAwaitingData = true;
    recoveryAttempt = g_consecutiveRecoveryRebootCount;
    recoveryRebootRequired = true;
  }
  portEXIT_CRITICAL(&g_readingMux);
  if (recoveryRebootRequired) {
    Serial.printf("[Victron] data unavailable; full recovery reboot %u/%u\n",
                  recoveryAttempt,
                  dfrcfg::kVictronMaxRecoveryReboots);
    Serial.flush();
    delay(100);
    ESP.restart();
  }

  if (now - g_lastScanRestartCheckMs < dfrcfg::kVictronScanRestartCheckMs) return;
  g_lastScanRestartCheckMs = now;
  if (!g_scan->isScanning() && !startScanner()) {
    recordInitFailure("NimBLE scanner stopped unexpectedly");
  }
}

Reading reading() {
  initializeRecoveryState();
  portENTER_CRITICAL(&g_readingMux);
  Reading copy = g_reading;
  copy.restartCount = g_totalRecoveryRebootCount;
  portEXIT_CRITICAL(&g_readingMux);
  copy.configured = hasValidConfiguration();
  return copy;
}

const char *status() {
  const Reading value = reading();
  if (!dfrcfg::kVictronEnabled) return "disabled";
  if (!value.configured) return "not_configured";
  if (!value.initialized) {
    if (millis() < dfrcfg::kVictronStartDelayMs) return "startup_delay";
    return g_initFailureCount == 0 ? "initializing" : "init_retry";
  }
  if (!value.valid) {
    if (value.decodeErrorCount > 0) return "decrypt_error";
    if (g_initializedAtMs != 0 &&
        millis() - g_initializedAtMs >= dfrcfg::kVictronFirstDataTimeoutMs) {
      return "no_data";
    }
    return "waiting";
  }
  if (millis() - value.lastUpdateMs >= dfrcfg::kVictronStaleAfterMs) return "stale";
  return "ready";
}

const char *chargeStateName(uint8_t state) {
  switch (state) {
    case 0: return "off";
    case 1: return "low_power";
    case 2: return "fault";
    case 3: return "bulk";
    case 4: return "absorption";
    case 5: return "float";
    case 6: return "storage";
    case 7: return "equalize";
    case 9: return "inverting";
    case 11: return "power_supply";
    case 252: return "external_control";
    default: return "unknown";
  }
}

}  // namespace dfrvictron
