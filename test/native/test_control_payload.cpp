#include "control_payload.h"

#include <cassert>
#include <iostream>
#include <string>

static bool token(const std::string &payload, const char *key, std::string &value) {
  controlpayload::Token found;
  if (!controlpayload::find(payload.data(), payload.size(), key, found)) return false;
  value.clear();
  return controlpayload::decode(found, [&](char c) { value += c; return true; });
}

int main() {
  std::string value;
  assert(token(R"({"framesize":6,"stream_enabled":true})", "framesize", value) && value == "6");
  assert(token(R"({"framesize":6,"stream_enabled":true})", "stream_enabled", value) && value == "true");
  assert(token(" gainceiling = 3 ; awb_gain=0 ; awb=1 ", "awb", value) && value == "1");
  assert(!token("awb_gain=1;notled=1", "led", value));
  assert(!token("ir_off_lux=100", "off_lux", value));
  assert(!token(R"({"metadata":{"led":1}})", "led", value));
  assert(token(R"({"metadata":[{"led":1},null],"led":0})", "led", value) && value == "0");
  assert(!token(R"({"message":"\"led\":1"})", "led", value));
  assert(!token(R"({"led":1,"led":0})", "led", value));
  assert(!token("led=1; led=0", "led", value));
  assert(!token(R"({"led":1,})", "led", value));
  assert(!token(R"({"led":1)", "led", value));
  assert(!token(R"({"led":1}garbage)", "led", value));
  assert(!token(R"({"led":01})", "led", value));
  assert(!token(R"({"led":truegarbage})", "led", value));
  assert(!token(R"({"led":1,"nested":[})", "led", value));
  assert(!token(R"({"led":1,"nested":[[[[[[[[[[0]]]]]]]]]]})", "led", value));
  assert(token(R"({"led":0,"_request_id":"id-\"a\\b\n"})", "_request_id", value));
  assert(value == "id-\"a\\b\n");
  assert(token(R"({"_request_id":"\u00E4\uD83D\uDE03"})", "_request_id", value));
  assert(value == "\xC3\xA4\xF0\x9F\x98\x83");
  assert(!token(R"({"_request_id":"\uD800"})", "_request_id", value));
  assert(!token(R"({"_request_id":"\uDC00"})", "_request_id", value));
  assert(!token(R"({"_request_id":"\u0000"})", "_request_id", value));
  const std::string nulPayload("led=1\0;framesize=7", 19);
  assert(!token(nulPayload, "led", value));
  assert(token(R"({"led":"","brightness":-2,"unused":1.5e+2})", "brightness", value) && value == "-2");
  assert(token(R"({"led":""})", "led", value) && value.empty());

  // Every truncated prefix must terminate safely, including inside escapes
  // and nested structures. Only complete objects may expose a setting.
  const std::string complete = R"({"led":1,"metadata":{"str":"\u00E4","arr":[1,true,null]}})";
  for (size_t size = 0; size < complete.size(); ++size)
    assert(!token(complete.substr(0, size), "led", value));
  assert(token(complete, "led", value) && value == "1");
  int number = 123;
  assert(controlpayload::parseInteger("-2147483648", number) && number == INT_MIN);
  assert(controlpayload::parseInteger("2147483647", number) && number == INT_MAX);
  assert(!controlpayload::parseInteger("2147483648", number) && number == INT_MAX);
  assert(!controlpayload::parseInteger("-2147483649", number));
  assert(!controlpayload::parseInteger("99999999999999999999999999999999", number));
  assert(!controlpayload::parseInteger("", number));
  assert(!controlpayload::parseInteger(" \t", number));
  assert(!controlpayload::parseInteger("12garbage", number));
  assert(!controlpayload::parseInteger("1.5", number));
  assert(controlpayload::parseInteger(" +12 \t", number) && number == 12);
  std::cout << "Control payload regression checks passed\n";
}
