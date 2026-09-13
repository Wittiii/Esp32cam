#pragma once

#include <array>
#include <cstddef>
#include <utility>

// A bounded, hardware-independent FIFO for one MQTT status snapshot. Payload
// supports default/copy/move construction, assignment and length() (Arduino
// String or std::string). Suffixes are borrowed: callers must use stable storage,
// normally string literals, that remains valid until the message is removed.
// The byte budget counts payload characters; fixed slots additionally bound
// String objects, terminators and allocation overhead.
template <typename Payload, std::size_t Capacity>
class MqttStatusBatch {
  static_assert(Capacity > 0, "An MQTT status batch needs at least one slot");

 public:
  struct Message {
    const char *suffix = nullptr;
    Payload value;
  };

  explicit MqttStatusBatch(std::size_t payloadByteLimit)
      : payloadByteLimit_(payloadByteLimit) {}

  bool push(const char *suffix, const Payload &value) {
    const std::size_t bytes = value.length();
    if (suffix == nullptr || size_ == Capacity ||
        bytes > payloadByteLimit_ - payloadBytes_) {
      return false;
    }

    // Arduino String reports an allocation failure as an empty/short value,
    // rather than throwing. Copy before changing the live FIFO and validate it.
    Payload copied(value);
    if (copied.length() != bytes) return false;

    Message &target = messages_[(head_ + size_) % Capacity];
    target.value = std::move(copied);
    if (target.value.length() != bytes) {
      release(target);
      return false;
    }
    target.suffix = suffix;
    ++size_;
    payloadBytes_ += bytes;
    return true;
  }

  const Message *front() const {
    return empty() ? nullptr : &messages_[head_];
  }

  void pop() {
    if (empty()) return;
    Message &message = messages_[head_];
    payloadBytes_ -= message.value.length();
    release(message);
    head_ = (head_ + 1) % Capacity;
    --size_;
  }

  void clear() {
    while (!empty()) pop();
    head_ = 0;
  }

  bool empty() const { return size_ == 0; }
  std::size_t size() const { return size_; }
  std::size_t payloadBytes() const { return payloadBytes_; }

 private:
  static void release(Message &message) {
    // Merely assigning "" can retain heap capacity. Swapping with a fresh
    // payload releases its storage when the temporary is destroyed.
    Payload retired;
    using std::swap;
    swap(message.value, retired);
    message.suffix = nullptr;
  }

  std::array<Message, Capacity> messages_{};
  const std::size_t payloadByteLimit_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  std::size_t payloadBytes_ = 0;
};
