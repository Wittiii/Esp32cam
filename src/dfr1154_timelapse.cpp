#include "dfr1154_timelapse.h"

#include <FS.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <WebServer.h>
#include <errno.h>
#include <esp_camera.h>
#include <sys/socket.h>
#include <time.h>

#include "dfr1154_config.h"
#include "dfr1154_pins.h"

namespace {

WebServer g_archiveServer(dfrcfg::kArchivePort);
Preferences g_preferences;

bool g_sdReady = false;
bool g_httpReady = false;
bool g_enabled = true;
bool g_storageFull = false;
uint32_t g_intervalSeconds = dfrcfg::kDefaultTimelapseIntervalSeconds;
uint64_t g_configuredLimitBytes = dfrcfg::kDefaultTimelapseLimitBytes;
uint64_t g_effectiveLimitBytes = dfrcfg::kDefaultTimelapseLimitBytes;
uint64_t g_storageBytes = 0;
uint64_t g_cardTotalBytes = 0;
uint64_t g_cardUsedBytes = 0;
uint32_t g_lastCaptureAtMs = 0;
String g_state = "initializing";
String g_error;
String g_lastImage;

constexpr uint32_t kDefaultListLimit = 50;
constexpr uint32_t kMaximumListLimit = 100;
constexpr size_t kArchiveChunkBytes = 4096;
constexpr uint32_t kArchiveWriteTimeoutMs = 3000;

uint32_t boundedQueryNumber(const char *name, uint32_t fallback, uint32_t maximum) {
  if (!g_archiveServer.hasArg(name)) return fallback;
  const long parsed = g_archiveServer.arg(name).toInt();
  if (parsed <= 0) return fallback;
  return min(static_cast<uint32_t>(parsed), maximum);
}

String uint64String(uint64_t value) {
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
  return String(buffer);
}

bool hasJpegExtension(const String &name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".jpg") || lower.endsWith(".jpeg");
}

String baseName(const String &path) {
  const int slash = path.lastIndexOf('/');
  return slash >= 0 ? path.substring(slash + 1) : path;
}

bool validImageName(const String &name) {
  return name.length() > 10 && name.indexOf('/') < 0 && name.indexOf("..") < 0 &&
         name.startsWith("frame-") && hasJpegExtension(name);
}

String imagePath(const String &name) {
  return String(dfrcfg::kTimelapseDirectory) + "/" + name;
}

void updateState() {
  if (!g_sdReady) {
    g_state = "sd_error";
  } else if (g_storageFull) {
    g_state = "storage_full";
  } else if (!g_enabled) {
    g_state = "stopped";
  } else {
    g_state = "running";
  }
}

void updateCardUsage() {
  if (!g_sdReady) return;
  g_cardTotalBytes = SD.totalBytes();
  g_cardUsedBytes = SD.usedBytes();

  const uint64_t safeCardLimit = g_cardTotalBytes > 0 ? (g_cardTotalBytes * 95ULL) / 100ULL : 0;
  g_effectiveLimitBytes = g_configuredLimitBytes;
  if (safeCardLimit > 0 && g_effectiveLimitBytes > safeCardLimit) {
    g_effectiveLimitBytes = safeCardLimit;
  }
}

void scanStorage() {
  g_storageBytes = 0;
  g_lastImage = "";
  time_t latestWrite = 0;

  if (!g_sdReady) return;
  File directory = SD.open(dfrcfg::kTimelapseDirectory);
  if (!directory || !directory.isDirectory()) return;

  File file = directory.openNextFile();
  while (file) {
    if (!file.isDirectory() && hasJpegExtension(file.name())) {
      g_storageBytes += file.size();
      const time_t modified = file.getLastWrite();
      if (modified >= latestWrite) {
        latestWrite = modified;
        g_lastImage = String(file.name());
      }
    }
    file.close();
    file = directory.openNextFile();
  }
  directory.close();
  updateCardUsage();
  g_storageFull = g_effectiveLimitBytes > 0 && g_storageBytes >= g_effectiveLimitBytes;
  updateState();
}

bool authorized() {
  if (strlen(dfrcfg::kArchiveToken) == 0) return true;
  return g_archiveServer.header("X-Archive-Token") == dfrcfg::kArchiveToken;
}

void sendUnauthorized() {
  g_archiveServer.send(401, "application/json", "{\"ok\":false,\"error\":\"unauthorized\"}");
}

void handleHealth() {
  if (!authorized()) {
    sendUnauthorized();
    return;
  }

  String json = "{\"ok\":true,\"sdReady\":";
  json += g_sdReady ? "true" : "false";
  json += ",\"state\":\"" + g_state + "\"";
  json += ",\"storageBytes\":" + uint64String(g_storageBytes);
  json += ",\"storageLimitBytes\":" + uint64String(g_effectiveLimitBytes);
  json += ",\"cardTotalBytes\":" + uint64String(g_cardTotalBytes);
  json += ",\"cardUsedBytes\":" + uint64String(g_cardUsedBytes);
  json += "}";
  g_archiveServer.sendHeader("Cache-Control", "no-store");
  g_archiveServer.send(200, "application/json", json);
}

void handleList() {
  if (!authorized()) {
    sendUnauthorized();
    return;
  }
  if (!g_sdReady) {
    g_archiveServer.send(503, "application/json", "{\"ok\":false,\"error\":\"sd_unavailable\"}");
    return;
  }

  const uint32_t offset = g_archiveServer.hasArg("offset")
      ? max(0L, g_archiveServer.arg("offset").toInt())
      : 0;
  const uint32_t limit = boundedQueryNumber("limit", kDefaultListLimit, kMaximumListLimit);

  g_archiveServer.sendHeader("Cache-Control", "no-store");
  g_archiveServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  g_archiveServer.send(200, "application/json", "");
  g_archiveServer.sendContent("{\"ok\":true,\"files\":[");

  bool first = true;
  uint32_t matched = 0;
  uint32_t returned = 0;
  bool hasMore = false;
  File directory = SD.open(dfrcfg::kTimelapseDirectory);
  File file = directory.openNextFile();
  while (file) {
    if (!file.isDirectory() && hasJpegExtension(file.name())) {
      const String name = baseName(file.name());
      if (validImageName(name)) {
        if (matched >= offset && returned < limit) {
          if (!first) g_archiveServer.sendContent(",");
          first = false;
          String item = "{\"name\":\"" + name + "\",\"sizeBytes\":";
          item += String(static_cast<uint32_t>(file.size()));
          item += ",\"modifiedEpoch\":" + String(static_cast<uint32_t>(file.getLastWrite()));
          item += "}";
          g_archiveServer.sendContent(item);
          ++returned;
        } else if (matched >= offset + returned && returned >= limit) {
          hasMore = true;
        }
        ++matched;
      }
    }
    file.close();
    if (hasMore) break;
    file = directory.openNextFile();
  }
  directory.close();

  String tail = "],\"count\":" + String(returned);
  tail += ",\"offset\":" + String(offset);
  tail += ",\"limit\":" + String(limit);
  tail += ",\"nextOffset\":" + String(offset + returned);
  tail += ",\"hasMore\":";
  tail += hasMore ? "true" : "false";
  tail += ",\"storageBytes\":" + uint64String(g_storageBytes) + "}";
  g_archiveServer.sendContent(tail);
  g_archiveServer.sendContent("");
}

void handleFile() {
  if (!authorized()) {
    sendUnauthorized();
    return;
  }

  const String name = g_archiveServer.arg("name");
  if (!validImageName(name)) {
    g_archiveServer.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_name\"}");
    return;
  }

  File file = SD.open(imagePath(name), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    g_archiveServer.send(404, "application/json", "{\"ok\":false,\"error\":\"not_found\"}");
    return;
  }

  const size_t fileSize = file.size();
  g_archiveServer.sendHeader("Cache-Control", "private, max-age=3600");
  g_archiveServer.setContentLength(fileSize);
  g_archiveServer.send(200, "image/jpeg", "");

  WiFiClient client = g_archiveServer.client();
  uint8_t buffer[kArchiveChunkBytes];
  size_t totalSent = 0;
  bool transferFailed = false;
  uint32_t lastProgressAt = millis();

  while (client.connected() && file.available() && !transferFailed) {
    const size_t bytesRead = file.read(buffer, sizeof(buffer));
    if (bytesRead == 0) break;

    size_t offset = 0;
    while (offset < bytesRead && client.connected()) {
      const ssize_t sent = ::send(client.fd(), buffer + offset, bytesRead - offset, MSG_DONTWAIT);
      if (sent > 0) {
        offset += static_cast<size_t>(sent);
        totalSent += static_cast<size_t>(sent);
        lastProgressAt = millis();
        continue;
      }

      if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        transferFailed = true;
        break;
      }
      if (millis() - lastProgressAt >= kArchiveWriteTimeoutMs) {
        transferFailed = true;
        break;
      }
      delay(1);
    }
  }
  file.close();
  if (transferFailed || totalSent != fileSize) {
    Serial.printf(
        "[HTTP] archive transfer aborted name=%s sent=%u expected=%u\n",
        name.c_str(),
        static_cast<unsigned>(totalSent),
        static_cast<unsigned>(fileSize));
    client.stop();
  }
}

bool deleteImage(const String &name) {
  if (!validImageName(name)) return false;
  const String path = imagePath(name);
  File file = SD.open(path, FILE_READ);
  const size_t size = file ? file.size() : 0;
  if (file) file.close();
  if (!SD.remove(path)) return false;

  g_storageBytes = size <= g_storageBytes ? g_storageBytes - size : 0;
  if (g_lastImage.endsWith(name)) g_lastImage = "";
  updateCardUsage();
  if (g_storageBytes < g_effectiveLimitBytes) g_storageFull = false;
  updateState();
  return true;
}

void handleDelete() {
  if (!authorized()) {
    sendUnauthorized();
    return;
  }

  const String name = g_archiveServer.arg("name");
  if (name.length() > 0) {
    if (!deleteImage(name)) {
      g_archiveServer.send(404, "application/json", "{\"ok\":false,\"error\":\"not_found\"}");
      return;
    }
    g_archiveServer.send(200, "application/json", "{\"ok\":true,\"deleted\":1}");
    return;
  }

  uint32_t deleted = 0;
  // FAT directory iteration can skip the next entry after a deletion. Repeat
  // until a complete pass finds nothing, while keeping memory usage constant.
  for (uint8_t pass = 0; pass < 16; ++pass) {
    uint32_t deletedThisPass = 0;
    File directory = SD.open(dfrcfg::kTimelapseDirectory);
    File file = directory.openNextFile();
    while (file) {
      const String nameToDelete = baseName(file.name());
      const bool remove = !file.isDirectory() && validImageName(nameToDelete);
      file.close();
      if (remove && SD.remove(imagePath(nameToDelete))) {
        ++deleted;
        ++deletedThisPass;
      }
      file = directory.openNextFile();
    }
    directory.close();
    if (deletedThisPass == 0) break;
  }
  scanStorage();
  g_archiveServer.send(200, "application/json", "{\"ok\":true,\"deleted\":" + String(deleted) + "}");
}

String buildImagePath() {
  char name[64];
  const time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm timeInfo {};
    localtime_r(&now, &timeInfo);
    strftime(name, sizeof(name), "frame-%Y%m%d-%H%M%S.jpg", &timeInfo);
  } else {
    snprintf(name, sizeof(name), "frame-uptime-%010lu.jpg", static_cast<unsigned long>(millis()));
  }

  String path = imagePath(name);
  if (!SD.exists(path)) return path;

  for (uint16_t suffix = 1; suffix < 1000; ++suffix) {
    String candidate = path.substring(0, path.length() - 4) + "-" + String(suffix) + ".jpg";
    if (!SD.exists(candidate)) return candidate;
  }
  return "";
}

bool saveFrame(const uint8_t *data, size_t length, uint32_t capturedAtMs) {
  if (!g_enabled || !g_sdReady || data == nullptr || length == 0) return false;
  if (g_effectiveLimitBytes > 0 && g_storageBytes + length > g_effectiveLimitBytes) {
    g_storageFull = true;
    g_enabled = false;
    g_preferences.putBool("enabled", false);
    g_error = "timelapse storage limit reached";
    updateState();
    return false;
  }

  const String path = buildImagePath();
  if (path.length() == 0) {
    g_error = "unable to create unique image name";
    return false;
  }

  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    g_error = "unable to open timelapse image";
    return false;
  }
  const size_t written = file.write(data, length);
  file.close();
  if (written != length) {
    SD.remove(path);
    g_error = "incomplete timelapse image write";
    return false;
  }

  g_storageBytes += written;
  g_lastImage = path;
  g_lastCaptureAtMs = capturedAtMs;
  g_error = "";
  updateCardUsage();
  updateState();
  Serial.printf("[TIMELAPSE] saved %s (%u bytes)\n", path.c_str(), static_cast<unsigned>(written));
  return true;
}

bool captureDue(uint32_t now) {
  if (!g_enabled || !g_sdReady || g_storageFull) return false;
  const uint32_t intervalMs = g_intervalSeconds * 1000UL;
  return g_lastCaptureAtMs == 0 || now - g_lastCaptureAtMs >= intervalMs || now < g_lastCaptureAtMs;
}

}  // namespace

namespace dfrtimelapse {

bool begin() {
  g_preferences.begin("dfrtime", false);
  g_enabled = g_preferences.getBool("enabled", true);
  g_intervalSeconds = g_preferences.getUInt("interval", dfrcfg::kDefaultTimelapseIntervalSeconds);
  g_intervalSeconds = constrain(g_intervalSeconds, 5UL, 86400UL);
  g_configuredLimitBytes =
      g_preferences.getULong64("limit", dfrcfg::kDefaultTimelapseLimitBytes);

  SPI.begin(DFR_SD_SCK, DFR_SD_MISO, DFR_SD_MOSI, DFR_SD_CS);
  g_sdReady = SD.begin(DFR_SD_CS, SPI, 20000000);
  if (!g_sdReady || SD.cardType() == CARD_NONE) {
    g_sdReady = false;
    g_error = "SD card initialization failed";
    updateState();
    return false;
  }

  if (!SD.exists(dfrcfg::kTimelapseDirectory) && !SD.mkdir(dfrcfg::kTimelapseDirectory)) {
    g_sdReady = false;
    g_error = "unable to create timelapse directory";
    updateState();
    return false;
  }

  scanStorage();
  if (!g_storageFull) g_error = "";
  g_lastCaptureAtMs = millis();
  Serial.printf(
      "[SD] ready total=%lluMB timelapse=%lluMB limit=%lluMB\n",
      static_cast<unsigned long long>(g_cardTotalBytes / (1024ULL * 1024ULL)),
      static_cast<unsigned long long>(g_storageBytes / (1024ULL * 1024ULL)),
      static_cast<unsigned long long>(g_effectiveLimitBytes / (1024ULL * 1024ULL)));
  return true;
}

void startHttpServer() {
  if (g_httpReady) return;
  const char *headers[] = {"X-Archive-Token"};
  g_archiveServer.collectHeaders(headers, 1);
  g_archiveServer.on("/api/health", HTTP_GET, handleHealth);
  g_archiveServer.on("/api/timelapse", HTTP_GET, handleList);
  g_archiveServer.on("/api/timelapse", HTTP_DELETE, handleDelete);
  g_archiveServer.on("/api/timelapse/file", HTTP_GET, handleFile);
  g_archiveServer.on("/api/timelapse/file", HTTP_DELETE, handleDelete);
  g_archiveServer.onNotFound([]() {
    g_archiveServer.send(404, "application/json", "{\"ok\":false,\"error\":\"not_found\"}");
  });
  g_archiveServer.begin();
  g_httpReady = true;
  Serial.printf("[HTTP] archive listening on port %u\n", dfrcfg::kArchivePort);
}

void handleHttpClient() {
  if (g_httpReady) g_archiveServer.handleClient();
}

void observeJpegFrame(
    const uint8_t *data,
    size_t length,
    uint16_t,
    uint16_t,
    uint32_t capturedAtMs) {
  if (captureDue(capturedAtMs)) saveFrame(data, length, capturedAtMs);
}

void captureIfDue() {
  const uint32_t now = millis();
  if (!captureDue(now)) return;

  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr) {
    g_lastCaptureAtMs = now;
    g_error = "camera capture failed";
    return;
  }
  saveFrame(frame->buf, frame->len, now);
  esp_camera_fb_return(frame);
}

bool setEnabled(bool enabledValue) {
  if (enabledValue) {
    scanStorage();
    if (!g_sdReady || g_storageFull) return false;
  }
  g_enabled = enabledValue;
  g_storageFull = enabledValue ? false : g_storageFull;
  g_preferences.putBool("enabled", g_enabled);
  g_lastCaptureAtMs = millis();
  updateState();
  return true;
}

bool setIntervalSeconds(uint32_t interval) {
  g_intervalSeconds = constrain(interval, 5UL, 86400UL);
  g_preferences.putUInt("interval", g_intervalSeconds);
  g_lastCaptureAtMs = millis();
  return true;
}

bool setStorageLimitBytes(uint64_t limit) {
  if (limit < 64ULL * 1024ULL * 1024ULL) return false;
  g_configuredLimitBytes = limit;
  g_preferences.putULong64("limit", limit);
  scanStorage();
  return true;
}

bool sdReady() { return g_sdReady; }
bool enabled() { return g_enabled; }
bool httpReady() { return g_httpReady; }
uint32_t intervalSeconds() { return g_intervalSeconds; }
uint64_t storageBytes() { return g_storageBytes; }
uint64_t storageLimitBytes() { return g_effectiveLimitBytes; }
uint64_t cardTotalBytes() { return g_cardTotalBytes; }
uint64_t cardUsedBytes() { return g_cardUsedBytes; }
String state() { return g_state; }
String error() { return g_error; }
String lastImage() { return g_lastImage; }

}  // namespace dfrtimelapse
