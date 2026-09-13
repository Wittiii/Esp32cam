#define BOUNDED_MQTT_PLATFORM_HEADER "mqtt_socket_platform.h"
#include "bounded_mqtt_socket.h"

#include <cassert>
#include <cstring>
#include <iostream>

namespace {
using Reason = BoundedMqttSocket::FailureReason;
const uint8_t packet[] = {0x30, 3, 1, 2, 3};
unsigned waits = 0;
void feedWatchdog() { ++waits; }

std::ptrdiff_t wouldBlock(int = 0, const uint8_t * = nullptr, size_t = 0, int = 0) {
  errno = EAGAIN;
  return -1;
}

void checkDefaultsAndImmediateSuccess() {
  mqttsocketstub::reset();
  BoundedMqttSocket socket;
  assert(socket.kWriteTimeoutMs == 1000);
  assert(socket.failureCount() == 0);
  assert(socket.lastFailure().reason == Reason::None);
  assert(std::strcmp(socket.reasonName(Reason::None), "none") == 0);
  mqttsocketstub::sendHandler = [](int, const uint8_t *bytes, size_t size, int) {
    assert(bytes == packet && size == sizeof(packet));
    return static_cast<std::ptrdiff_t>(size);
  };
  assert(socket.write(packet, sizeof(packet)) == sizeof(packet));
  assert(socket.stops == 0 && socket.failureCount() == 0);
  assert(mqttsocketstub::delays == 0);
  mqttsocketstub::sendHandler = [](int, const uint8_t *bytes, size_t size, int) {
    assert(*bytes == 0xC0 && size == 1);
    return 1;
  };
  assert(socket.write(static_cast<uint8_t>(0xC0)) == 1);
}

void checkTemporaryCongestionPastOldDeadline() {
  mqttsocketstub::reset();
  BoundedMqttSocket socket;
  waits = 0;
  socket.setWaitCallback(feedWatchdog);
  mqttsocketstub::sendHandler = [](int, const uint8_t *, size_t size, int) {
    if (millis() < 250) return wouldBlock();
    return static_cast<std::ptrdiff_t>(size);
  };
  assert(socket.write(packet, sizeof(packet)) == sizeof(packet));
  assert(millis() == 250 && waits == 250 && mqttsocketstub::delays == 250);
  assert(socket.stops == 0 && socket.failureCount() == 0);
}

void checkPermanentCongestionDeadline() {
  mqttsocketstub::reset();
  BoundedMqttSocket socket;
  mqttsocketstub::sendHandler = wouldBlock;
  assert(socket.write(packet, sizeof(packet)) == 0);
  assert(millis() == 1000 && mqttsocketstub::sends == 1000);
  assert(socket.stops == 1 && socket.failureCount() == 1);
  const auto &failure = socket.lastFailure();
  assert(failure.reason == Reason::Timeout && failure.errorNumber == EAGAIN);
  assert(failure.elapsedMs == 1000 && failure.sentBytes == 0 && failure.totalBytes == sizeof(packet));
  assert(std::strcmp(socket.reasonName(failure.reason), "timeout") == 0);
}

void checkPartialWriteSharesSingleDeadline() {
  mqttsocketstub::reset();
  BoundedMqttSocket socket;
  mqttsocketstub::sendHandler = [](int, const uint8_t *bytes, size_t size, int) {
    if (mqttsocketstub::sends == 1) {
      assert(bytes == packet && size == sizeof(packet));
      mqttsocketstub::nowMs += 600;
      return std::ptrdiff_t{2};
    }
    assert(bytes == packet + 2 && size == sizeof(packet) - 2);
    return wouldBlock();
  };
  assert(socket.write(packet, sizeof(packet)) == 0);
  assert(millis() == 1000 && mqttsocketstub::delays == 400);
  assert(socket.stops == 1 && socket.failureCount() == 1);
  assert(socket.lastFailure().reason == Reason::Timeout);
  assert(socket.lastFailure().sentBytes == 2 && socket.lastFailure().totalBytes == sizeof(packet));
}

void checkHardFailurePreservesErrorAndPartialCount() {
  mqttsocketstub::reset();
  BoundedMqttSocket socket;
  mqttsocketstub::sendHandler = [](int, const uint8_t *, size_t, int) {
    if (mqttsocketstub::sends == 1) return 2;
    errno = ECONNRESET;
    return -1;
  };
  assert(socket.write(packet, sizeof(packet)) == 0);
  assert(socket.stops == 1 && mqttsocketstub::delays == 0);
  const auto &failure = socket.lastFailure();
  assert(failure.reason == Reason::SocketError && failure.errorNumber == ECONNRESET);
  assert(failure.elapsedMs == 0 && failure.sentBytes == 2 && failure.totalBytes == sizeof(packet));
  assert(std::strcmp(socket.reasonName(failure.reason), "socket_error") == 0);
}

void checkTransientErrorsAndPartialSuccess() {
  mqttsocketstub::reset();
  BoundedMqttSocket socket;
  mqttsocketstub::sendHandler = [](int, const uint8_t *bytes, size_t size, int) {
    switch (mqttsocketstub::sends) {
      case 1: errno = EINTR; return std::ptrdiff_t{-1};
      case 2: errno = EWOULDBLOCK; return std::ptrdiff_t{-1};
      case 3: errno = EAGAIN; return std::ptrdiff_t{-1};
      case 4: return std::ptrdiff_t{2};
      default:
        assert(bytes == packet + 2 && size == sizeof(packet) - 2);
        return static_cast<std::ptrdiff_t>(size);
    }
  };
  assert(socket.write(packet, sizeof(packet)) == sizeof(packet));
  assert(millis() == 3 && socket.stops == 0 && socket.failureCount() == 0);
}

void checkClockRollover() {
  mqttsocketstub::reset(UINT32_MAX - 49);
  BoundedMqttSocket socket;
  const uint32_t startedAt = millis();
  mqttsocketstub::sendHandler = [startedAt](int, const uint8_t *, size_t size, int) {
    if (static_cast<uint32_t>(millis() - startedAt) < 250) return wouldBlock();
    return static_cast<std::ptrdiff_t>(size);
  };
  assert(socket.write(packet, sizeof(packet)) == sizeof(packet));
  assert(millis() == 200 && socket.failureCount() == 0);

  mqttsocketstub::reset(UINT32_MAX - 49);
  mqttsocketstub::sendHandler = wouldBlock;
  assert(socket.write(packet, sizeof(packet)) == 0);
  assert(millis() == 950);
  assert(socket.lastFailure().reason == Reason::Timeout && socket.lastFailure().elapsedMs == 1000);
}

void checkDisconnectedAndEmptyWrites() {
  mqttsocketstub::reset();
  BoundedMqttSocket socket;
  socket.online = false;
  assert(socket.write(nullptr, 0) == 0);
  assert(socket.failureCount() == 0 && socket.stops == 0 && mqttsocketstub::sends == 0);
  assert(socket.write(packet, sizeof(packet)) == 0);
  assert(socket.failureCount() == 1 && socket.lastFailure().reason == Reason::Disconnected);
  assert(socket.lastFailure().errorNumber == ENOTCONN);
  assert(mqttsocketstub::sends == 0);
  assert(std::strcmp(socket.reasonName(socket.lastFailure().reason), "disconnected") == 0);

  socket.connect("broker", 1883);
  socket.descriptor = -1;
  assert(socket.write(packet, sizeof(packet)) == 0);
  assert(socket.failureCount() == 2 && mqttsocketstub::sends == 0);

  socket.connect("broker", 1883);
  mqttsocketstub::sendHandler = [](int, const uint8_t *, size_t, int) { return 0; };
  assert(socket.write(packet, sizeof(packet)) == 0);
  assert(socket.failureCount() == 3 && socket.lastFailure().reason == Reason::Disconnected);
  assert(socket.lastFailure().errorNumber == 0);
}

void checkDiagnosticsSurviveReconnectAndSuccess() {
  mqttsocketstub::reset();
  BoundedMqttSocket socket;
  mqttsocketstub::sendHandler = wouldBlock;
  assert(socket.write(packet, sizeof(packet)) == 0);
  const auto original = socket.lastFailure();
  socket.stop();
  socket.connect("broker", 1883);
  mqttsocketstub::sendHandler = [](int, const uint8_t *, size_t size, int) {
    return static_cast<std::ptrdiff_t>(size);
  };
  assert(socket.write(packet, sizeof(packet)) == sizeof(packet));
  assert(socket.write(nullptr, 0) == 0);
  assert(socket.failureCount() == 1);
  const auto &after = socket.lastFailure();
  assert(after.reason == original.reason && after.errorNumber == original.errorNumber);
  assert(after.elapsedMs == original.elapsedMs && after.sentBytes == original.sentBytes);
  assert(after.totalBytes == original.totalBytes);
}
}  // namespace

int main() {
  checkDefaultsAndImmediateSuccess();
  checkTemporaryCongestionPastOldDeadline();
  checkPermanentCongestionDeadline();
  checkPartialWriteSharesSingleDeadline();
  checkHardFailurePreservesErrorAndPartialCount();
  checkTransientErrorsAndPartialSuccess();
  checkClockRollover();
  checkDisconnectedAndEmptyWrites();
  checkDiagnosticsSurviveReconnectAndSuccess();
  std::cout << "9 bounded MQTT socket regression groups passed\n";
}
