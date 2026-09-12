#pragma once

#include <cstdint>

namespace dfrcfg {
constexpr bool kVictronEnabled = true;
constexpr char kVictronName[] = "synthetic-test-device";
constexpr char kVictronMac[] = "010203040506";
constexpr char kVictronEncryptionKey[] = "00000000000000000000000000000000";
constexpr uint32_t kVictronStartDelayMs = 60000;
constexpr uint16_t kVictronBleScanIntervalMs = 200;
constexpr uint16_t kVictronBleScanWindowMs = 50;
constexpr uint32_t kVictronScanRestartCheckMs = 5000;
constexpr uint32_t kVictronSoftRecoveryIntervalMs = 60000;
constexpr uint32_t kVictronStaleAfterMs = 30000;
constexpr uint32_t kVictronFirstDataTimeoutMs = 120000;
constexpr uint32_t kVictronInitRetryMs = 60000;
constexpr uint32_t kVictronRecoveryRebootAfterMs = 600000;
constexpr uint8_t kVictronMaxRecoveryReboots = 2;
}  // namespace dfrcfg
