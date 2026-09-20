#pragma once

#include <Arduino.h>

#include "dfr1154_secrets.h"

namespace dfrcfg {

using dfrsecrets::kWifiPassword;
using dfrsecrets::kWifiSsid;

constexpr char kOtaHostname[] = "dfr1154-cam-01";
using dfrsecrets::kOtaPassword;
constexpr char kMdnsHostname[] = "dfr1154-cam-01";

constexpr char kMqttHost[] = "192.168.178.56";
constexpr uint16_t kMqttPort = 1883;
using dfrsecrets::kMqttPassword;
using dfrsecrets::kMqttUsername;
constexpr char kMqttClientId[] = "dfr1154-cam-01";
constexpr char kMqttBaseTopic[] = "camera/dfr1154-cam-01";
constexpr char kFirmwareVersion[] = "2026.08.30-dfr-nimble1";

constexpr uint16_t kRtspPort = 8554;
constexpr char kRtspPresentation[] = "mjpeg";
constexpr char kRtspStream[] = "1";
constexpr int kDefaultRtspFps = 10;

constexpr uint32_t kDefaultTimelapseIntervalSeconds = 60;

constexpr float kDefaultIrOnBelowLux = 5.0f;
constexpr float kDefaultIrOffAboveLux = 10.0f;

constexpr uint32_t kWifiRetryInitialMs = 2000;
constexpr uint32_t kWifiRetryMaxMs = 30000;
constexpr uint8_t kWifiHardReconnectEvery = 6;
constexpr uint32_t kMqttRetryInitialMs = 2000;
constexpr uint32_t kMqttRetryMaxMs = 30000;
constexpr uint32_t kStatusIntervalMs = 5000;
constexpr uint32_t kLightReadIntervalMs = 1000;

constexpr bool kBme280Enabled = true;
constexpr uint32_t kBme280ReadIntervalMs = 5000;
constexpr size_t kBme280AverageSamples = 12;
constexpr uint32_t kBme280PublishIntervalMs = 60000;
constexpr uint32_t kBme280RetryIntervalMs = 30000;
constexpr uint32_t kBme280StaleAfterMs = 20000;

// Get both values from VictronConnect: Product info -> Instant readout details.
constexpr bool kVictronEnabled = true;
constexpr char kVictronName[] = "SmartSolar MPPT 150/45";
constexpr char kVictronMac[] = "f661b210ec0e";
using dfrsecrets::kVictronEncryptionKey;
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

constexpr char kTimezone[] = "CET-1CEST,M3.5.0,M10.5.0/3";
constexpr char kNtpServer1[] = "pool.ntp.org";
constexpr char kNtpServer2[] = "time.cloudflare.com";

}  // namespace dfrcfg
