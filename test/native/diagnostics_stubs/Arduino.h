#pragma once
#include <string>
#include <cstdint>
#include <type_traits>
class String : public std::string {
 public:
  using std::string::string;
  using std::string::operator=;
  String(const std::string &value) : std::string(value) {}
  template<typename T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
  String(T value) : std::string(std::to_string(value)) {}
};
extern uint32_t testNow;
inline uint32_t millis() { return testNow; }
struct TestSerial {
  template<typename... Args> void printf(const char *, Args...) {}
};
extern TestSerial Serial;
