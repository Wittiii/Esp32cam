#pragma once

#include <Arduino.h>

namespace appcfg {

constexpr char kWifiSsid[] = "FRITZ!Box 7590 BK";
constexpr char kWifiPassword[] = "35428536880518248119";

// Legacy TCP viewer target, kept for the optional viewer code path.
constexpr char kViewerHost[] = "192.168.178.56";
constexpr uint16_t kViewerPort = 5005;

constexpr char kOtaHostname[] = "esp32-cam-streamer";
constexpr char kOtaPassword[] = "1234";
constexpr char kMdnsHostname[] = "esp32-cam-streamer";

constexpr char kMqttHost[] = "192.168.178.56";
constexpr uint16_t kMqttPort = 1883;
constexpr char kMqttUsername[] = "pwi";
constexpr char kMqttPassword[] = "1234";
constexpr char kMqttClientId[] = "esp32-cam-01";
constexpr char kMqttBaseTopic[] = "camera/esp32-cam-01";

constexpr uint16_t kRtspPort = 8554;
constexpr char kRtspPresentation[] = "mjpeg";
constexpr char kRtspStream[] = "1";
constexpr int kDefaultRtspFps = 12;

constexpr uint32_t kFrameErrorReportMs = 5000;
constexpr uint32_t kWifiRetryMs = 5000;
constexpr uint32_t kTcpRetryMs = 2000;
constexpr uint32_t kMqttRetryMs = 5000;

constexpr size_t kPacketHeaderSize = 8;
constexpr uint8_t kFrameMagic[4] = {'J', 'P', 'E', 'G'};
constexpr uint8_t kStatusMagic[4] = {'S', 'T', 'A', 'T'};
constexpr uint8_t kControlMagic[4] = {'C', 'T', 'R', 'L'};
constexpr size_t kMaxControlPayloadSize = 256;

constexpr size_t kStatusHistorySize = 10;
constexpr size_t kStatusLineSize = 128;

}  // namespace appcfg
