#pragma once

#ifdef BOUNDED_MQTT_PLATFORM_HEADER
#include BOUNDED_MQTT_PLATFORM_HEADER
#else
#include <WiFiClient.h>
#include <sys/socket.h>
#endif
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

// WiFiClient::write retries ten one-second waits on the Arduino ESP32 core.
// Keep MQTT backpressure from freezing camera frames, IR control and OTA.
class BoundedMqttSocket : public WiFiClient {
 public:
  using WiFiClient::write;

  static constexpr uint32_t kWriteTimeoutMs = 1000;
  enum class FailureReason { None, Timeout, SocketError, Disconnected };
  struct WriteFailure {
    FailureReason reason = FailureReason::None;
    int errorNumber = 0;
    uint32_t elapsedMs = 0;
    size_t sentBytes = 0;
    size_t totalBytes = 0;
  };

  // This hook may feed the watchdog only. Calling MQTT/OTA handling here would
  // re-enter the client while its current MQTT packet is still being written.
  void setWaitCallback(void (*callback)()) { waitCallback_ = callback; }
  uint32_t failureCount() const { return failureCount_; }
  const WriteFailure &lastFailure() const { return lastFailure_; }

  static const char *reasonName(FailureReason reason) {
    switch (reason) {
      case FailureReason::None: return "none";
      case FailureReason::Timeout: return "timeout";
      case FailureReason::SocketError: return "socket_error";
      case FailureReason::Disconnected: return "disconnected";
    }
    return "unknown";
  }

  size_t write(uint8_t value) override { return write(&value, 1); }

  size_t write(const uint8_t *bytes, size_t length) override {
    if (length == 0) return 0;
    const uint32_t startedAt = millis();
    if (!connected() || fd() < 0)
      return fail(FailureReason::Disconnected, ENOTCONN, startedAt, 0, length);

    size_t sentBytes = 0;
    int lastError = 0;
    while (sentBytes < length) {
      if (static_cast<uint32_t>(millis() - startedAt) >= kWriteTimeoutMs)
        return fail(FailureReason::Timeout, lastError, startedAt, sentBytes, length);

      const auto sent = ::send(fd(), bytes + sentBytes, length - sentBytes, MSG_DONTWAIT);
      if (sent > 0) {
        sentBytes += static_cast<size_t>(sent);
        continue;
      }
      if (sent == 0)
        return fail(FailureReason::Disconnected, 0, startedAt, sentBytes, length);
      lastError = errno;
      if (lastError != EAGAIN && lastError != EWOULDBLOCK && lastError != EINTR)
        return fail(FailureReason::SocketError, lastError, startedAt, sentBytes, length);

      // Wi-Fi, Bluetooth and RTSP share radio/CPU time. Brief backpressure is
      // normal; allow it without restoring the core's multi-second send waits.
      if (waitCallback_) waitCallback_();
      delay(1);
    }
    return sentBytes;
  }

 private:
  size_t fail(FailureReason reason, int errorNumber, uint32_t startedAt,
              size_t sentBytes, size_t totalBytes) {
    if (failureCount_ != UINT32_MAX) ++failureCount_;
    lastFailure_.reason = reason;
    lastFailure_.errorNumber = errorNumber;
    lastFailure_.elapsedMs = static_cast<uint32_t>(millis() - startedAt);
    lastFailure_.sentBytes = sentBytes;
    lastFailure_.totalBytes = totalBytes;
    // The peer may already have a packet prefix. Only a new connection can
    // safely carry the next MQTT packet after a timeout or failed partial write.
    // Keep diagnostics in RAM across stop(), reconnect and successful writes.
    stop();
    return 0;
  }

  void (*waitCallback_)() = nullptr;
  uint32_t failureCount_ = 0;
  WriteFailure lastFailure_;
};
