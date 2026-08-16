#pragma once

#include <Arduino.h>

namespace dfrcfg {

constexpr char kWifiSsid[] = "FRITZ!Box 7590 BK";
constexpr char kWifiPassword[] = "35428536880518248119";

constexpr char kOtaHostname[] = "dfr1154-cam-01";
constexpr char kOtaPassword[] = "1234";
constexpr char kMdnsHostname[] = "dfr1154-cam-01";

constexpr char kMqttHost[] = "192.168.178.56";
constexpr uint16_t kMqttPort = 1883;
constexpr char kMqttUsername[] = "pwi";
constexpr char kMqttPassword[] = "19082004";
constexpr char kMqttClientId[] = "dfr1154-cam-01";
constexpr char kMqttBaseTopic[] = "camera/dfr1154-cam-01";

constexpr uint16_t kRtspPort = 8554;
constexpr char kRtspPresentation[] = "mjpeg";
constexpr char kRtspStream[] = "1";
constexpr int kDefaultRtspFps = 10;

constexpr uint32_t kDefaultTimelapseIntervalSeconds = 60;

constexpr float kDefaultIrOnBelowLux = 5.0f;
constexpr float kDefaultIrOffAboveLux = 10.0f;

constexpr uint32_t kWifiRetryMs = 5000;
constexpr uint32_t kMqttRetryMs = 5000;
constexpr uint32_t kStatusIntervalMs = 5000;
constexpr uint32_t kLightReadIntervalMs = 1000;

constexpr bool kBme280Enabled = true;
constexpr uint32_t kBme280ReadIntervalMs = 5000;
constexpr uint32_t kBme280RetryIntervalMs = 30000;
constexpr uint32_t kBme280StaleAfterMs = 20000;

// Get both values from VictronConnect: Product info -> Instant readout details.
constexpr bool kVictronEnabled = false;
constexpr char kVictronName[] = "SmartSolar MPPT 150/45";
constexpr char kVictronMac[] = "";
constexpr char kVictronEncryptionKey[] = "";
constexpr uint32_t kVictronScanIntervalMs = 5000;
constexpr uint32_t kVictronStaleAfterMs = 30000;

constexpr char kTimezone[] = "CET-1CEST,M3.5.0,M10.5.0/3";
constexpr char kNtpServer1[] = "pool.ntp.org";
constexpr char kNtpServer2[] = "time.cloudflare.com";

}  // namespace dfrcfg
