#pragma once

#include <Arduino.h>

namespace dfrtimelapse {

using ServiceHook = void (*)();

bool begin();
void setServiceHook(ServiceHook hook);
void startHttpServer();
void handleHttpClient();
void captureIfDue();
void observeJpegFrame(
    const uint8_t *data,
    size_t length,
    uint16_t width,
    uint16_t height,
    uint32_t capturedAtMs);

bool setEnabled(bool enabled);
bool setIntervalSeconds(uint32_t intervalSeconds);
bool setStorageLimitBytes(uint64_t storageLimitBytes);

bool sdReady();
bool enabled();
bool httpReady();
uint32_t intervalSeconds();
uint64_t storageBytes();
uint64_t storageLimitBytes();
uint64_t cardTotalBytes();
uint64_t cardUsedBytes();
String state();
String error();
String lastImage();

}  // namespace dfrtimelapse
