#pragma once
#include <cstdint>
#define CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH 1
#define CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF 1
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
struct esp_core_dump_summary_t {
  char exc_task[16];
  uint32_t exc_pc;
  struct { uint32_t bt[16]; uint32_t depth; bool corrupted; } exc_bt_info;
  uint8_t app_elf_sha256[65];
  struct { uint32_t exc_cause; } ex_info;
};
extern int testIntegrity;
extern int testSummaryResult;
extern unsigned testSummaryCalls;
extern esp_core_dump_summary_t testSummary;
inline esp_err_t esp_core_dump_image_check() { return testIntegrity; }
inline esp_err_t esp_core_dump_get_summary(esp_core_dump_summary_t *out) {
  ++testSummaryCalls;
  *out = testSummary;
  return testSummaryResult;
}
