#include "mqtt_status_batch.h"

#include <cassert>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

namespace {

void checkEmptyAndInvalidSuffix() {
  MqttStatusBatch<std::string, 2> batch(16);
  assert(batch.empty());
  assert(batch.size() == 0 && batch.payloadBytes() == 0);
  assert(batch.front() == nullptr);
  batch.pop();
  batch.clear();
  assert(!batch.push(nullptr, "value"));
  assert(batch.empty() && batch.payloadBytes() == 0);
  assert(batch.push("status/value", "value"));
  const auto *front = batch.front();
  assert(!batch.push(nullptr, ""));
  assert(batch.front() == front && front->value == "value");
  assert(batch.size() == 1 && batch.payloadBytes() == 5);
}

void checkCapacityAndFailedPush() {
  MqttStatusBatch<std::string, 2> batch(64);
  assert(batch.push("first", "one"));
  assert(batch.push("second", "two"));
  const auto *front = batch.front();
  assert(!batch.push("third", "three"));
  assert(!batch.push("empty", ""));
  assert(batch.front() == front && front->value == "one");
  assert(batch.size() == 2 && batch.payloadBytes() == 6);
  batch.pop();
  assert(batch.front()->value == "two");
  assert(batch.size() == 1 && batch.payloadBytes() == 3);
  batch.pop();
  assert(batch.empty() && batch.payloadBytes() == 0);
}

void checkByteBudgetAndEmptyPayload() {
  MqttStatusBatch<std::string, 4> batch(5);
  assert(!batch.push("too-large", "123456"));
  assert(batch.push("first", "123"));
  assert(batch.push("second", "45"));
  assert(!batch.push("over-budget", "6"));
  assert(batch.push("empty", "")); // Legal MQTT empty retained payload.
  assert(batch.size() == 3 && batch.payloadBytes() == 5);
  batch.pop();
  assert(batch.payloadBytes() == 2);
  assert(batch.push("reclaimed", "123"));
  assert(batch.payloadBytes() == 5);
  batch.pop();
  assert(batch.front()->value.empty());
  assert(std::string(batch.front()->suffix) == "empty");
  batch.pop();
  assert(batch.front()->value == "123");

  MqttStatusBatch<std::string, 1> zeroBudget(0);
  assert(!zeroBudget.push("nonempty", "a"));
  assert(zeroBudget.push("empty", ""));
  assert(zeroBudget.size() == 1 && zeroBudget.payloadBytes() == 0);

  MqttStatusBatch<std::string, 1> largeBudget(
      std::numeric_limits<std::size_t>::max());
  assert(largeBudget.push("value", "safe"));
  assert(largeBudget.payloadBytes() == 4);
}

void checkWraparoundAndBorrowedSuffix() {
  MqttStatusBatch<std::string, 3> batch(64);
  const char *suffix = "stable/literal";
  for (int round = 0; round < 100; ++round) {
    for (int item = 0; item < 3; ++item) {
      assert(batch.push(suffix, std::to_string(round * 3 + item)));
    }
    for (int item = 0; item < 3; ++item) {
      assert(batch.front()->suffix == suffix);
      assert(batch.front()->value == std::to_string(round * 3 + item));
      batch.pop();
    }
    assert(batch.empty() && batch.payloadBytes() == 0);
    // Move the next batch's head across the array boundary each round.
    assert(batch.push(suffix, "skip"));
    batch.pop();
  }

  assert(batch.push("first", "one"));
  assert(batch.push("second", "two"));
  batch.pop();
  assert(batch.push("third", "three"));
  assert(batch.push("fourth", "four"));
  for (const char *expected : {"two", "three", "four"}) {
    assert(batch.front()->value == expected);
    batch.pop();
  }
}

void checkClearAndCopiedPayloadOwnership() {
  MqttStatusBatch<std::string, 3> batch(1024);
  std::string original(256, 'a');
  assert(batch.push("snapshot", original));
  original.assign("changed");
  assert(batch.front()->value == std::string(256, 'a'));
  assert(batch.push("second", "value"));
  batch.clear();
  assert(batch.front() == nullptr);
  assert(batch.size() == 0 && batch.payloadBytes() == 0);
  batch.clear();
  assert(batch.push("new", "after-clear"));
  assert(batch.front()->value == "after-clear");
  assert(batch.payloadBytes() == 11);
}

// Arduino String-like silent copy failure. The ownership counter also checks
// that removed payloads are released immediately instead of retained in slots.
class FailablePayload {
 public:
  static bool failCopy;
  static std::size_t ownedCharacters;

  FailablePayload() = default;
  explicit FailablePayload(std::string value) : value_(std::move(value)) {
    ownedCharacters += value_.length();
  }
  FailablePayload(const FailablePayload &other)
      : value_(failCopy ? std::string{} : other.value_) {
    ownedCharacters += value_.length();
  }
  FailablePayload(FailablePayload &&other) noexcept
      : value_(std::move(other.value_)) {
    other.value_.clear();
  }
  FailablePayload &operator=(FailablePayload &&other) noexcept {
    if (this != &other) {
      ownedCharacters -= value_.length();
      value_ = std::move(other.value_);
      other.value_.clear();
    }
    return *this;
  }
  ~FailablePayload() { ownedCharacters -= value_.length(); }
  std::size_t length() const { return value_.length(); }

 private:
  std::string value_;
};

bool FailablePayload::failCopy = false;
std::size_t FailablePayload::ownedCharacters = 0;

void checkAllocationFailureAndRelease() {
  assert(FailablePayload::ownedCharacters == 0);
  {
    MqttStatusBatch<FailablePayload, 3> batch(2048);
    FailablePayload value(std::string(256, 'x'));
    assert(FailablePayload::ownedCharacters == 256);
    assert(batch.push("first", value));
    assert(FailablePayload::ownedCharacters == 512);
    const auto *front = batch.front();
    FailablePayload::failCopy = true;
    assert(!batch.push("failed", value));
    FailablePayload::failCopy = false;
    assert(batch.front() == front && front->value.length() == 256);
    assert(batch.size() == 1 && batch.payloadBytes() == 256);
    assert(FailablePayload::ownedCharacters == 512);

    assert(batch.push("second", value));
    assert(FailablePayload::ownedCharacters == 768);
    batch.pop();
    assert(FailablePayload::ownedCharacters == 512);
    batch.clear();
    assert(FailablePayload::ownedCharacters == 256);
    assert(batch.empty() && batch.payloadBytes() == 0);

    assert(batch.push("destructor", value));
    assert(FailablePayload::ownedCharacters == 512);
  }
  assert(FailablePayload::ownedCharacters == 0);
}

} // namespace

int main() {
  checkEmptyAndInvalidSuffix();
  checkCapacityAndFailedPush();
  checkByteBudgetAndEmptyPayload();
  checkWraparoundAndBorrowedSuffix();
  checkClearAndCopiedPayloadOwnership();
  checkAllocationFailureAndRelease();
  std::cout << "6 bounded MQTT status batch regression groups passed\n";
}
