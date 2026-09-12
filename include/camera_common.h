#pragma once

#include <Arduino.h>
#include <esp_camera.h>

#include "control_payload.h"

namespace camcommon {

constexpr char kFirmwareVersion[] = "2026.07.14-settings3";

inline int clampInt(int value, int minimum, int maximum) {
  return value < minimum ? minimum : (value > maximum ? maximum : value);
}

inline const char *frameSizeName(framesize_t size) {
  switch (size) {
    case FRAMESIZE_QVGA: return "QVGA";
    case FRAMESIZE_VGA: return "VGA";
    case FRAMESIZE_SVGA: return "SVGA";
    case FRAMESIZE_XGA: return "XGA";
    case FRAMESIZE_HD: return "HD";
    case FRAMESIZE_SXGA: return "SXGA";
    case FRAMESIZE_UXGA: return "UXGA";
    case FRAMESIZE_QXGA: return "QXGA";
    default: return "OTHER";
  }
}

inline framesize_t frameSizeFromIndex(int index, framesize_t fallback, bool allowQxga) {
  switch (index) {
    case 0: return FRAMESIZE_QVGA;
    case 1: return FRAMESIZE_VGA;
    case 2: return FRAMESIZE_SVGA;
    case 3: return FRAMESIZE_XGA;
    case 4: return FRAMESIZE_HD;
    case 5: return FRAMESIZE_SXGA;
    case 6: return FRAMESIZE_UXGA;
    case 7: return allowQxga ? FRAMESIZE_QXGA : fallback;
    default: return fallback;
  }
}

inline int frameSizeToIndex(framesize_t size, int fallback) {
  switch (size) {
    case FRAMESIZE_QVGA: return 0;
    case FRAMESIZE_VGA: return 1;
    case FRAMESIZE_SVGA: return 2;
    case FRAMESIZE_XGA: return 3;
    case FRAMESIZE_HD: return 4;
    case FRAMESIZE_SXGA: return 5;
    case FRAMESIZE_UXGA: return 6;
    case FRAMESIZE_QXGA: return 7;
    default: return fallback;
  }
}

inline String rtspUrl(const IPAddress &address, uint16_t port, const char *presentation, const char *stream) {
  return "rtsp://" + address.toString() + ":" + String(port) + "/" + presentation + "/" + stream;
}

inline bool extractPayloadToken(const String &payload, const char *key, String &token) {
  controlpayload::Token field;
  if (!controlpayload::find(payload.c_str(), payload.length(), key, field)) return false;
  token = "";
  if (!token.reserve(static_cast<unsigned int>(field.end - field.begin))) return false;
  return controlpayload::decode(field, [&](char c) { return token.concat(c) != 0; });
}

inline bool extractPayloadInt(const String &payload, const char *key, int &value) {
  String token;
  if (!extractPayloadToken(payload, key, token)) return false;
  token.toLowerCase();
  if (token == "true" || token == "on") value = 1;
  else if (token == "false" || token == "off") value = 0;
  else return controlpayload::parseInteger(token.c_str(), value);
  return true;
}

}  // namespace camcommon
