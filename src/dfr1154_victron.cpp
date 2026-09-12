#include "dfr1154_victron.h"

#include <NimBLEDevice.h>
#include <ctype.h>
#include <math.h>
#include <mbedtls/aes.h>
#include <string.h>

#include "dfr1154_config.h"
#include <esp_wifi.h>

#include <esp_attr.h>
namespace {

// ============================================================================
// Konstanten
// ============================================================================

constexpr uint32_t kRecoveryRtcMagic = 0x56424C45;

// Eigener Magic-Wert für den BLE-Debug-Speicher
constexpr uint32_t kBleDebugRtcMagic = 0x424C4544;

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


// ============================================================================
// BLE Debug Stages
//
// Der jeweils LETZTE gesetzte Wert bleibt nach einem Crash erhalten.
// ============================================================================

enum BleDebugStage : uint32_t {
  BLE_STAGE_NONE = 0,

  BLE_STAGE_BEGIN_ENTERED = 10,

  BLE_STAGE_BEFORE_DISABLE_WDT = 20,
  BLE_STAGE_BEFORE_NIMBLE_INIT = 21,
  BLE_STAGE_AFTER_NIMBLE_INIT = 22,
  BLE_STAGE_AFTER_ENABLE_WDT = 23,

  BLE_STAGE_BEFORE_GET_SCAN = 30,
  BLE_STAGE_AFTER_GET_SCAN = 31,

  BLE_STAGE_BEFORE_SET_CALLBACKS = 40,
  BLE_STAGE_AFTER_SET_CALLBACKS = 41,

  BLE_STAGE_BEFORE_ACTIVE_SCAN = 50,
  BLE_STAGE_AFTER_ACTIVE_SCAN = 51,

  BLE_STAGE_BEFORE_SET_INTERVAL = 60,
  BLE_STAGE_AFTER_SET_INTERVAL = 61,

  BLE_STAGE_BEFORE_SET_WINDOW = 70,
  BLE_STAGE_AFTER_SET_WINDOW = 71,

  BLE_STAGE_BEFORE_MAX_RESULTS = 80,
  BLE_STAGE_AFTER_MAX_RESULTS = 81,

  BLE_STAGE_BEFORE_SCAN_START = 90,
  BLE_STAGE_AFTER_SCAN_START = 91,

  BLE_STAGE_READY = 100
};


// ============================================================================
// RTC Daten
// ============================================================================

// Vorhandene Recovery-Daten
RTC_DATA_ATTR uint32_t g_recoveryRtcMagic = 0;
RTC_DATA_ATTR uint32_t g_totalRecoveryRebootCount = 0;
RTC_DATA_ATTR uint8_t g_consecutiveRecoveryRebootCount = 0;
RTC_DATA_ATTR bool g_recoveryAwaitingData = false;


// BLE Debug-Daten.
// Diese sollen Panic/Watchdog-Neustarts überleben.
__NOINIT_ATTR uint32_t g_bleDebugRtcMagic;
__NOINIT_ATTR uint32_t g_bleDebugStage;


// ============================================================================
// Globale Laufzeitdaten
// ============================================================================

dfrvictron::Reading g_reading = {};

portMUX_TYPE g_readingMux =
    portMUX_INITIALIZER_UNLOCKED;

NimBLEScan *g_scan = nullptr;

uint8_t g_encryptionKey[kKeyLength] = {};

char g_normalizedTargetMac[
    kMacHexLength + 1] = {};

// NimBLE stores address bytes least-significant first. Compare the bytes directly
// so unrelated advertisements do not allocate a temporary MAC string.
uint8_t g_targetMacBytes[kMacHexLength / 2] = {};
bool g_configurationLoaded = false;
bool g_firstReadingPending = false;

uint16_t g_lastNonce = 0;

uint32_t g_initializedAtMs = 0;
uint32_t g_lastInitAttemptMs = 0;
uint32_t g_lastScanRestartCheckMs = 0;
uint32_t g_lastSoftRecoveryMs = 0;
uint32_t g_initFailureCount = 0;

bool g_startRequested = false;
bool g_haveNonce = false;
bool g_scannerEverStarted = false;


// ============================================================================
// BLE Debug
// ============================================================================

void initializeBleDebugState() {

  if (g_bleDebugRtcMagic ==
      kBleDebugRtcMagic) {
    return;
  }

  g_bleDebugRtcMagic =
      kBleDebugRtcMagic;

  g_bleDebugStage =
      BLE_STAGE_NONE;
}


void setBleDebugStage(
    uint32_t stage) {

  initializeBleDebugState();

  // Direkte RTC-Zuweisung.
  // Kein Serial/MQTT hier, damit wir den BLE-Ablauf
  // möglichst wenig beeinflussen.
  g_bleDebugStage = stage;
}


const char *bleDebugStageName(
    uint32_t stage) {

  switch (stage) {

    case BLE_STAGE_NONE:
      return "none";

    case BLE_STAGE_BEGIN_ENTERED:
      return "begin_entered";

    case BLE_STAGE_BEFORE_DISABLE_WDT:
      return "before_disable_loop_wdt";

    case BLE_STAGE_BEFORE_NIMBLE_INIT:
      return "before_nimble_init";

    case BLE_STAGE_AFTER_NIMBLE_INIT:
      return "after_nimble_init";

    case BLE_STAGE_AFTER_ENABLE_WDT:
      return "after_enable_loop_wdt";

    case BLE_STAGE_BEFORE_GET_SCAN:
      return "before_get_scan";

    case BLE_STAGE_AFTER_GET_SCAN:
      return "after_get_scan";

    case BLE_STAGE_BEFORE_SET_CALLBACKS:
      return "before_set_callbacks";

    case BLE_STAGE_AFTER_SET_CALLBACKS:
      return "after_set_callbacks";

    case BLE_STAGE_BEFORE_ACTIVE_SCAN:
      return "before_set_active_scan";

    case BLE_STAGE_AFTER_ACTIVE_SCAN:
      return "after_set_active_scan";

    case BLE_STAGE_BEFORE_SET_INTERVAL:
      return "before_set_interval";

    case BLE_STAGE_AFTER_SET_INTERVAL:
      return "after_set_interval";

    case BLE_STAGE_BEFORE_SET_WINDOW:
      return "before_set_window";

    case BLE_STAGE_AFTER_SET_WINDOW:
      return "after_set_window";

    case BLE_STAGE_BEFORE_MAX_RESULTS:
      return "before_set_max_results";

    case BLE_STAGE_AFTER_MAX_RESULTS:
      return "after_set_max_results";

    case BLE_STAGE_BEFORE_SCAN_START:
      return "before_scan_start";

    case BLE_STAGE_AFTER_SCAN_START:
      return "after_scan_start";

    case BLE_STAGE_READY:
      return "ready";

    default:
      return "unknown";
  }
}


// ============================================================================
// Hilfsfunktionen
// ============================================================================

uint16_t readLe16(
    const uint8_t *data) {

  return
      static_cast<uint16_t>(
          data[0]) |

      (static_cast<uint16_t>(
           data[1])
       << 8);
}


int hexNibble(char value) {

  if (value >= '0' &&
      value <= '9') {
    return value - '0';
  }

  value =
      static_cast<char>(
          tolower(
              static_cast<unsigned char>(
                  value)));

  if (value >= 'a' &&
      value <= 'f') {

    return value - 'a' + 10;
  }

  return -1;
}


bool parseEncryptionKey(
    const char *hex,
    uint8_t output[kKeyLength]) {

  if (hex == nullptr ||
      strlen(hex) !=
          kKeyLength * 2) {

    return false;
  }

  for (size_t index = 0;
       index < kKeyLength;
       ++index) {

    const int high =
        hexNibble(
            hex[index * 2]);

    const int low =
        hexNibble(
            hex[index * 2 + 1]);

    if (high < 0 ||
        low < 0) {

      return false;
    }

    output[index] =
        static_cast<uint8_t>(
            (high << 4) | low);
  }

  return true;
}


bool normalizeMac(
    const char *input,
    char output[
        kMacHexLength + 1]) {

  if (input == nullptr) {
    return false;
  }

  size_t outputIndex = 0;

  for (size_t inputIndex = 0;
       input[inputIndex] != '\0';
       ++inputIndex) {

    const unsigned char value =
        static_cast<unsigned char>(
            input[inputIndex]);

    if (value == ':' ||
        value == '-') {

      continue;
    }

    if (!isxdigit(value) ||
        outputIndex >=
            kMacHexLength) {

      return false;
    }

    output[outputIndex++] =
        static_cast<char>(
            tolower(value));
  }

  if (outputIndex !=
      kMacHexLength) {

    return false;
  }

  output[outputIndex] = '\0';

  return true;
}


bool hasValidConfiguration() {
  // Configuration is immutable for this firmware. reading()/status() run often;
  // do not parse the key and address on every camera/MQTT loop iteration.
  static const bool configured = []() {
    uint8_t ignoredKey[kKeyLength];
    char ignoredMac[kMacHexLength + 1];
    return dfrcfg::kVictronEnabled &&
        parseEncryptionKey(dfrcfg::kVictronEncryptionKey, ignoredKey) &&
        normalizeMac(dfrcfg::kVictronMac, ignoredMac);
  }();
  return configured;
}


// ============================================================================
// AES
// ============================================================================

bool decryptPayload(
    const uint8_t *ciphertext,
    size_t length,
    uint16_t nonce,
    uint8_t *plaintext) {

  uint8_t counter[16] = {};
  uint8_t streamBlock[16] = {};

  size_t counterOffset = 0;

  counter[0] =
      static_cast<uint8_t>(
          nonce & 0xFF);

  counter[1] =
      static_cast<uint8_t>(
          nonce >> 8);


  mbedtls_aes_context aes;

  mbedtls_aes_init(&aes);


  const int setKeyResult =
      mbedtls_aes_setkey_enc(
          &aes,
          g_encryptionKey,
          128);


  const int decryptResult =
      setKeyResult == 0

          ? mbedtls_aes_crypt_ctr(
                &aes,
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


// ============================================================================
// Decode Errors
// ============================================================================

void recordDecodeError() {

  portENTER_CRITICAL(
      &g_readingMux);

  ++g_reading.decodeErrorCount;

  portEXIT_CRITICAL(
      &g_readingMux);
}


// ============================================================================
// Victron Advertisement
// ============================================================================

bool decodeSolarAdvertisement(
    const uint8_t *data,
    size_t length,
    int8_t rssi) {

  if (data == nullptr ||

      length <
          kAdvertisementHeaderLength +
              kMinimumSolarPayloadLength) {

    recordDecodeError();

    return false;
  }


  if (readLe16(data) !=
          kVictronCompanyId ||

      data[kRecordOffset] !=
          kProductAdvertisement ||

      data[kDeviceTypeOffset] !=
          kSolarChargerRecord) {

    return false;
  }


  portENTER_CRITICAL(
      &g_readingMux);

  ++g_reading.advertisementCount;

  portEXIT_CRITICAL(
      &g_readingMux);


  if (data[kKeyCheckOffset] !=
      g_encryptionKey[0]) {

    recordDecodeError();

    return false;
  }


  const uint16_t nonce =
      readLe16(
          data +
          kNonceOffset);


  if (g_haveNonce &&
      nonce == g_lastNonce) {

    return false;
  }


  size_t ciphertextLength =
      length -
      kCiphertextOffset;


  if (ciphertextLength >
      kMaximumCiphertextLength) {

    ciphertextLength =
        kMaximumCiphertextLength;
  }


  uint8_t plaintext[
      kMaximumCiphertextLength] = {};


  if (!decryptPayload(
          data +
              kCiphertextOffset,

          ciphertextLength,

          nonce,

          plaintext)) {

    recordDecodeError();

    return false;
  }


  const float batteryVoltage =
      static_cast<int16_t>(
          readLe16(
              plaintext + 2)) *
      0.01f;


  const float batteryCurrent =
      static_cast<int16_t>(
          readLe16(
              plaintext + 4)) *
      0.1f;


  const uint16_t loadRaw =
      readLe16(
          plaintext + 10) &
      0x01FF;

  // Victron uses all-ones sentinels for unavailable yield and PV power. They
  // must not become 655350 Wh / 65535 W in the Pi's retained energy history.
  // Like unavailable voltage/current, reject an incomplete core measurement;
  // keep the previous reading and its original freshness timestamp.
  const uint16_t yieldRaw = readLe16(plaintext + 6);
  const uint16_t panelPowerRaw = readLe16(plaintext + 8);


  if (yieldRaw == 0xFFFF || panelPowerRaw == 0xFFFF ||
      !isfinite(
          batteryVoltage) ||

      batteryVoltage < 0.0f ||

      batteryVoltage > 100.0f ||

      !isfinite(
          batteryCurrent) ||

      batteryCurrent < -500.0f ||

      batteryCurrent > 500.0f) {

    recordDecodeError();

    return false;
  }


  portENTER_CRITICAL(
      &g_readingMux);


  g_firstReadingPending = g_firstReadingPending || !g_reading.valid;


  g_reading.valid =
      true;


  g_reading.chargeState =
      plaintext[0];


  g_reading.errorCode =
      plaintext[1];


  g_reading.batteryVoltage =
      batteryVoltage;


  g_reading.batteryCurrent =
      batteryCurrent;


  g_reading.yieldTodayWh =
      yieldRaw * 10UL;


  g_reading.panelPower =
      panelPowerRaw;


  g_reading.loadCurrent =
      loadRaw == 0x01FF

          ? 0.0f

          : loadRaw * 0.1f;


  g_reading.rssi =
      rssi;


  g_reading.lastUpdateMs =
      millis();


  g_consecutiveRecoveryRebootCount =
      0;


  g_recoveryAwaitingData =
      false;


  portEXIT_CRITICAL(
      &g_readingMux);


  g_lastNonce = nonce;

  g_haveNonce = true;


  return true;
}


// ============================================================================
// Recovery
// ============================================================================

void initializeRecoveryState() {

  if (g_recoveryRtcMagic ==
      kRecoveryRtcMagic) {

    return;
  }


  g_recoveryRtcMagic =
      kRecoveryRtcMagic;


  g_totalRecoveryRebootCount =
      0;


  g_consecutiveRecoveryRebootCount =
      0;


  g_recoveryAwaitingData =
      false;
}


void setInitializationState(
    bool initialized) {

  portENTER_CRITICAL(
      &g_readingMux);

  g_reading.initialized =
      initialized;

  portEXIT_CRITICAL(
      &g_readingMux);
}


void recordInitFailure(
    const char *message) {

  ++g_initFailureCount;


  setInitializationState(
      false);


  Serial.printf(
      "[Victron] %s; retry in %lu seconds\n",

      message,

      dfrcfg::
          kVictronInitRetryMs /
          1000UL);
}


// ============================================================================
// MAC
// ============================================================================

bool addressMatches(
    const NimBLEAddress &address) {
  return memcmp(address.getVal(), g_targetMacBytes,
                sizeof(g_targetMacBytes)) == 0;
}


// ============================================================================
// BLE Advertisement Callback
// ============================================================================

void processAdvertisement(
    const NimBLEAdvertisedDevice *device) {

  if (device == nullptr) {
    return;
  }


  if (!addressMatches(
          device->getAddress())) {

    return;
  }


  // Read AD structures in-place: manufacturer strings otherwise allocate on
  // each packet. Bounds also cover malformed/truncated advertising data.
  const auto &payload = device->getPayload();
  for (size_t offset = 0; offset < payload.size();) {
    const size_t fieldLength = payload[offset];
    if (fieldLength == 0) {
      break;
    }
    if (fieldLength > payload.size() - offset - 1) {
      recordDecodeError();
      break;
    }
    if (payload[offset + 1] == 0xFF &&
        decodeSolarAdvertisement(payload.data() + offset + 2,
                                 fieldLength - 1,
                                 static_cast<int8_t>(device->getRSSI()))) {
      break;
    }
    offset += fieldLength + 1;
  }
}


class VictronAdvertisementCallbacks
    : public NimBLEScanCallbacks {

 public:

  void onResult(
      const NimBLEAdvertisedDevice
          *device)
      override {

    processAdvertisement(
        device);
  }
};


VictronAdvertisementCallbacks
    g_advertisementCallbacks;


// ============================================================================
// Scanner
// ============================================================================

bool startScanner() {

  if (g_scan == nullptr) {
    return false;
  }


  if (g_scan->isScanning()) {
    return true;
  }


  const bool started =
      g_scan->start(
          0,
          false,
          true);


  if (started) {

    if (g_scannerEverStarted) {

      portENTER_CRITICAL(
          &g_readingMux);

      ++g_reading
            .scanRestartCount;

      portEXIT_CRITICAL(
          &g_readingMux);
    }


    g_scannerEverStarted =
        true;
  }


  return started;
}


// ============================================================================
// Soft Recovery
// ============================================================================

void softRecoverScanner(
    uint32_t now) {

  if (g_scan == nullptr) {
    return;
  }


  if (g_lastSoftRecoveryMs != 0 &&

      now -
              g_lastSoftRecoveryMs <
          dfrcfg::
              kVictronSoftRecoveryIntervalMs) {

    return;
  }


  g_lastSoftRecoveryMs =
      now;


  if (g_scan->isScanning()) {
    if (!g_scan->stop()) {
      recordInitFailure("NimBLE scanner stop failed during recovery");
      return;
    }
  }


  if (startScanner()) {

    Serial.println(
        "[Victron] stale/no data; NimBLE scanner restarted");

  } else {

    recordInitFailure(
        "NimBLE scanner recovery failed");
  }
}


}  // namespace


// ============================================================================
// Öffentliche API
// ============================================================================

namespace dfrvictron {


// ============================================================================
// begin()
// ============================================================================

void begin() {

  g_startRequested =
      true;


  initializeRecoveryState();

  initializeBleDebugState();


  if (reading().initialized) {
    return;
  }


  const uint32_t now =
      millis();


  if (g_lastInitAttemptMs != 0 &&

      now -
              g_lastInitAttemptMs <
          dfrcfg::
              kVictronInitRetryMs) {

    return;
  }


  g_lastInitAttemptMs =
      now;


  // Ab hier wissen wir:
  // dfrvictron::begin() wurde wirklich erreicht.
  setBleDebugStage(
      BLE_STAGE_BEGIN_ENTERED);


  // --------------------------------------------------------------------------
  // Config
  // --------------------------------------------------------------------------

  const bool configured =
      hasValidConfiguration();


  portENTER_CRITICAL(
      &g_readingMux);

  g_reading.configured =
      configured;

  portEXIT_CRITICAL(
      &g_readingMux);


  if (!configured) {

    Serial.println(
        "[Victron] invalid MAC or encryption key configuration");

    return;
  }


  if (!g_configurationLoaded) {
    if (!parseEncryptionKey(dfrcfg::kVictronEncryptionKey, g_encryptionKey) ||
        !normalizeMac(dfrcfg::kVictronMac, g_normalizedTargetMac)) {
      recordInitFailure("device configuration rejected");
      return;
    }

    for (size_t index = 0; index < sizeof(g_targetMacBytes); ++index) {
      const size_t hexOffset = (sizeof(g_targetMacBytes) - index - 1) * 2;
      g_targetMacBytes[index] = static_cast<uint8_t>(
          (hexNibble(g_normalizedTargetMac[hexOffset]) << 4) |
          hexNibble(g_normalizedTargetMac[hexOffset + 1]));
    }
    // Never rewrite the address/key while the NimBLE callback could use them
    // during a scanner initialization retry.
    g_configurationLoaded = true;
  }


  feedLoopWDT();


  // ==========================================================================
  // NimBLE INIT
  // ==========================================================================

  if (!NimBLEDevice::isInitialized()) {

  setBleDebugStage(
      BLE_STAGE_BEFORE_DISABLE_WDT);

  disableLoopWDT();


  // ------------------------------------------------------------
  // WiFi Powersave explizit auf MIN_MODEM setzen
  // Wichtig für WiFi + Bluetooth Coexistence
  // ------------------------------------------------------------

  wifi_ps_type_t currentPs =
      WIFI_PS_NONE;

  esp_err_t getPsResult =
      esp_wifi_get_ps(&currentPs);

  Serial.printf(
      "[Victron] WiFi PS before BLE: result=%d mode=%d\n",
      static_cast<int>(getPsResult),
      static_cast<int>(currentPs));


  const esp_err_t setPsResult =
      esp_wifi_set_ps(
          WIFI_PS_MIN_MODEM);

  Serial.printf(
      "[Victron] force WIFI_PS_MIN_MODEM: result=%d\n",
      static_cast<int>(setPsResult));


  delay(100);


  currentPs =
      WIFI_PS_NONE;

  const esp_err_t verifyPsResult =
      esp_wifi_get_ps(&currentPs);

  Serial.printf(
      "[Victron] WiFi PS verified: result=%d mode=%d\n",
      static_cast<int>(verifyPsResult),
      static_cast<int>(currentPs));


  // ------------------------------------------------------------
  // Jetzt NimBLE starten
  // ------------------------------------------------------------

  setBleDebugStage(
      BLE_STAGE_BEFORE_NIMBLE_INIT);

  const bool initialized =
      NimBLEDevice::init("");

  setBleDebugStage(
      BLE_STAGE_AFTER_NIMBLE_INIT);


  enableLoopWDT();

  setBleDebugStage(
      BLE_STAGE_AFTER_ENABLE_WDT);


  delay(20);

  feedLoopWDT();


  if (!initialized) {

    recordInitFailure(
        "NimBLE initialization failed");

    return;
  }
}


  // ==========================================================================
  // getScan()
  // ==========================================================================

  setBleDebugStage(
      BLE_STAGE_BEFORE_GET_SCAN);


  g_scan =
      NimBLEDevice::getScan();


  setBleDebugStage(
      BLE_STAGE_AFTER_GET_SCAN);


  if (g_scan == nullptr) {

    recordInitFailure(
        "NimBLE scanner unavailable");

    return;
  }


  // ==========================================================================
  // Callbacks
  // ==========================================================================

  setBleDebugStage(
      BLE_STAGE_BEFORE_SET_CALLBACKS);


  g_scan->setScanCallbacks(
      &g_advertisementCallbacks,
      true);


  setBleDebugStage(
      BLE_STAGE_AFTER_SET_CALLBACKS);


  // ==========================================================================
  // Passive Scan
  // ==========================================================================

  setBleDebugStage(
      BLE_STAGE_BEFORE_ACTIVE_SCAN);


  g_scan->setActiveScan(
      false);


  setBleDebugStage(
      BLE_STAGE_AFTER_ACTIVE_SCAN);


  // ==========================================================================
  // Interval
  // ==========================================================================

  setBleDebugStage(
      BLE_STAGE_BEFORE_SET_INTERVAL);


  g_scan->setInterval(
      dfrcfg::
          kVictronBleScanIntervalMs);


  setBleDebugStage(
      BLE_STAGE_AFTER_SET_INTERVAL);


  // ==========================================================================
  // Window
  // ==========================================================================

  setBleDebugStage(
      BLE_STAGE_BEFORE_SET_WINDOW);


  g_scan->setWindow(
      dfrcfg::
          kVictronBleScanWindowMs);


  setBleDebugStage(
      BLE_STAGE_AFTER_SET_WINDOW);


  // ==========================================================================
  // Results
  // ==========================================================================

  setBleDebugStage(
      BLE_STAGE_BEFORE_MAX_RESULTS);


  g_scan->setMaxResults(
      0);


  setBleDebugStage(
      BLE_STAGE_AFTER_MAX_RESULTS);


  feedLoopWDT();


  // ==========================================================================
  // START SCANNER
  // ==========================================================================

  setBleDebugStage(
      BLE_STAGE_BEFORE_SCAN_START);


  if (!startScanner()) {

    recordInitFailure(
        "NimBLE scanner start failed");

    return;
  }


  setBleDebugStage(
      BLE_STAGE_AFTER_SCAN_START);


  // ==========================================================================
  // Fertig
  // ==========================================================================

  setInitializationState(
      true);


  g_initializedAtMs =
      millis();


  g_lastScanRestartCheckMs =
      g_initializedAtMs;


  setBleDebugStage(
      BLE_STAGE_READY);


  Serial.printf(
      "[Victron] NimBLE scanner ready for %s\n",

      dfrcfg::
          kVictronName);
}


// ============================================================================
// loop()
// ============================================================================

void loop() {
  const Reading current = reading();

  if (!current.initialized) {

    if (g_startRequested) {
      begin();
    }

    return;
  }


  // Take the timestamp AFTER the snapshot. The BLE task can publish a reading
  // concurrently; now - a newer lastUpdateMs would underflow to ~49 days.
  const uint32_t now = millis();

  portENTER_CRITICAL(&g_readingMux);
  const bool firstReadingPending = g_firstReadingPending;
  g_firstReadingPending = false;
  portEXIT_CRITICAL(&g_readingMux);
  if (firstReadingPending) {
    // Serial output belongs to the loop task, never the BLE host callback.
    Serial.println("[Victron] first valid SmartSolar advertisement received");
  }


  const bool
      timedOutWaitingForFirstData =

          !current.valid &&

          g_initializedAtMs != 0 &&

          now -
                  g_initializedAtMs >=
              dfrcfg::
                  kVictronFirstDataTimeoutMs;


  const bool dataIsStale =

      current.valid &&

      now -
              current.lastUpdateMs >=
          dfrcfg::
              kVictronStaleAfterMs;


  if (timedOutWaitingForFirstData ||
      dataIsStale) {

    softRecoverScanner(
        now);
  }


  bool recoveryRebootRequired =
      false;

  uint8_t recoveryAttempt =
      0;


  portENTER_CRITICAL(
      &g_readingMux);

  // The soft scan restart above can yield to the BLE task. Read time again
  // under the same lock as lastUpdateMs before deciding to reboot the camera.
  const uint32_t recoveryNow = millis();

  const bool
      dataStaleForRecovery =

          g_reading.valid &&

          recoveryNow -
                  g_reading.lastUpdateMs >=
              dfrcfg::
                  kVictronRecoveryRebootAfterMs;


  const bool
      recoveryStillHasNoData =

          !g_reading.valid &&

          g_initializedAtMs != 0 &&

          recoveryNow -
                  g_initializedAtMs >=
              dfrcfg::
                  kVictronRecoveryRebootAfterMs;


  if ((dataStaleForRecovery ||
       recoveryStillHasNoData) &&

      g_consecutiveRecoveryRebootCount <
          dfrcfg::
              kVictronMaxRecoveryReboots) {


    ++g_totalRecoveryRebootCount;


    ++g_consecutiveRecoveryRebootCount;


    g_recoveryAwaitingData =
        true;


    recoveryAttempt =
        g_consecutiveRecoveryRebootCount;


    recoveryRebootRequired =
        true;
  }


  portEXIT_CRITICAL(
      &g_readingMux);


  if (recoveryRebootRequired) {

    Serial.printf(
        "[Victron] data unavailable; full recovery reboot %u/%u\n",

        recoveryAttempt,

        dfrcfg::
            kVictronMaxRecoveryReboots);


    Serial.flush();

    delay(100);

    ESP.restart();
  }


  if (now -
          g_lastScanRestartCheckMs <
      dfrcfg::
          kVictronScanRestartCheckMs) {

    return;
  }


  g_lastScanRestartCheckMs =
      now;


  if (!g_scan->isScanning()) {

    if (!startScanner()) {

      recordInitFailure(
          "NimBLE scanner stopped unexpectedly");
    }
  }
}


// ============================================================================
// reading()
// ============================================================================

Reading reading() {

  initializeRecoveryState();


  portENTER_CRITICAL(
      &g_readingMux);


  Reading copy =
      g_reading;


  copy.restartCount =
      g_totalRecoveryRebootCount;


  portEXIT_CRITICAL(
      &g_readingMux);


  copy.configured =
      hasValidConfiguration();


  return copy;
}


// ============================================================================
// status()
// ============================================================================

const char *status() {

  const Reading value =
      reading();


  if (!dfrcfg::kVictronEnabled) {
    return "disabled";
  }


  if (!value.configured) {
    return "not_configured";
  }


  if (!value.initialized) {

    if (millis() <
        dfrcfg::
            kVictronStartDelayMs) {

      return "startup_delay";
    }


    return
        g_initFailureCount == 0

            ? "initializing"

            : "init_retry";
  }


  if (!value.valid) {

    if (value.decodeErrorCount >
        0) {

      return "decrypt_error";
    }


    if (g_initializedAtMs != 0 &&

        millis() -
                g_initializedAtMs >=
            dfrcfg::
                kVictronFirstDataTimeoutMs) {

      return "no_data";
    }


    return "waiting";
  }


  if (millis() -
          value.lastUpdateMs >=
      dfrcfg::
          kVictronStaleAfterMs) {

    return "stale";
  }


  return "ready";
}


// ============================================================================
// Debug-Ausgabe
// ============================================================================

const char *debugStage() {

  initializeBleDebugState();

  return bleDebugStageName(
      g_bleDebugStage);
}


uint32_t debugStageCode() {

  initializeBleDebugState();

  return g_bleDebugStage;
}


// ============================================================================
// Charger State
// ============================================================================

const char *chargeStateName(
    uint8_t state) {

  switch (state) {

    case 0:
      return "off";

    case 1:
      return "low_power";

    case 2:
      return "fault";

    case 3:
      return "bulk";

    case 4:
      return "absorption";

    case 5:
      return "float";

    case 6:
      return "storage";

    case 7:
      return "equalize";

    case 9:
      return "inverting";

    case 11:
      return "power_supply";

    case 252:
      return "external_control";

    default:
      return "unknown";
  }
}


}  // namespace dfrvictron
