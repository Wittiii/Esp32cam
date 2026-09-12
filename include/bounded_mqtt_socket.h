#pragma once

#include <WiFiClient.h>
#include <errno.h>
#include <sys/socket.h>

// WiFiClient::write retries ten one-second waits on the Arduino ESP32 core.
// Keep MQTT backpressure from freezing camera frames, IR control and OTA.
class BoundedMqttSocket : public WiFiClient {
 public:
  using WiFiClient::write;

  size_t write(uint8_t value) override { return write(&value, 1); }

  size_t write(const uint8_t *bytes, size_t length) override {
    if (length == 0) return 0;
    if (!connected() || fd() < 0) return 0;
    const uint32_t startedAt = millis();
    size_t sentBytes = 0;
    while (sentBytes < length && millis() - startedAt < 100UL) {
      const ssize_t sent = ::send(fd(), bytes + sentBytes, length - sentBytes, MSG_DONTWAIT);
      if (sent > 0) {
        sentBytes += static_cast<size_t>(sent);
        continue;
      }
      if (sent == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
      delay(1);
    }
    if (sentBytes == length) return sentBytes;
    // The peer may already have a packet prefix. Only a new connection can
    // safely carry the next MQTT packet after this timeout/partial write.
    stop();
    return 0;
  }
};
