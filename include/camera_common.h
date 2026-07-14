#pragma once

#include <Arduino.h>
#include <cctype>
#include <esp_camera.h>
#include <limits.h>
#include <stdlib.h>

namespace camcommon {

constexpr char kFirmwareVersion[] = "2026.07.14-rtsp4";

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
  const String jsonKey = "\"" + String(key) + "\"";
  int keyPosition = payload.indexOf(jsonKey);
  if (keyPosition >= 0) {
    const int colon = payload.indexOf(':', keyPosition + jsonKey.length());
    if (colon < 0) return false;
    int start = colon + 1;
    while (start < payload.length() && isspace(static_cast<unsigned char>(payload[start]))) ++start;
    if (start >= payload.length()) return false;
    if (payload[start] == '"') {
      const int end = payload.indexOf('"', start + 1);
      if (end < 0) return false;
      token = payload.substring(start + 1, end);
      return true;
    }
    int end = start;
    while (end < payload.length() && payload[end] != ',' && payload[end] != '}') ++end;
    token = payload.substring(start, end);
    token.trim();
    return token.length() > 0;
  }

  const String legacyKey = String(key) + "=";
  keyPosition = payload.indexOf(legacyKey);
  if (keyPosition < 0) return false;
  const int start = keyPosition + legacyKey.length();
  int end = payload.indexOf(';', start);
  if (end < 0) end = payload.length();
  token = payload.substring(start, end);
  token.trim();
  return token.length() > 0;
}

inline bool extractPayloadInt(const String &payload, const char *key, int &value) {
  String token;
  if (!extractPayloadToken(payload, key, token)) return false;
  token.toLowerCase();
  if (token == "true" || token == "on") value = 1;
  else if (token == "false" || token == "off") value = 0;
  else {
    char *end = nullptr;
    const long parsed = strtol(token.c_str(), &end, 10);
    while (end != nullptr && *end != '\0' && isspace(static_cast<unsigned char>(*end))) ++end;
    if (end == token.c_str() || end == nullptr || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX) {
      return false;
    }
    value = static_cast<int>(parsed);
  }
  return true;
}

}  // namespace camcommon
