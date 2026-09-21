#include "dfr1154_diagnostics.h"

#include <esp_core_dump.h>
#include <esp_attr.h>
#include <stdio.h>
#include <string.h>

namespace {
constexpr uint32_t kMagic = 0x44475231;
struct Trace {
  uint32_t magic;
  uint32_t bootId;
  char active[40];
  uint32_t startedMs;
  char slowest[40];
  uint32_t slowestMs;
};
RTC_NOINIT_ATTR Trace trace;
String report;

void copyStage(char *destination, const char *source) {
  snprintf(destination, 40, "%.39s", source);
}

String hexAddress(uint32_t address) {
  char text[11];
  snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(address));
  return String(text);
}

// Bound and escape even strings obtained from a saved core dump.
String jsonText(const char *text, size_t limit) {
  String result = "\"";
  for (size_t i = 0; i < limit && text[i]; ++i) {
    const unsigned char c = text[i];
    if (c == '"' || c == '\\') result += '\\';
    result += c >= 32 && c < 127 ? static_cast<char>(c) : '?';
  }
  return result + "\"";
}
}  // namespace

namespace dfrdiag {
void begin(esp_reset_reason_t reason) {
  const bool previousValid = trace.magic == kMagic &&
      (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT ||
       reason == ESP_RST_WDT || reason == ESP_RST_PANIC || reason == ESP_RST_SW);
  report.reserve(1400);
  const uint32_t bootId = esp_random();
  report = "{\"boot_id\":" + jsonText(hexAddress(bootId).c_str(), 10);
  report += ",\"reset_reason_code\":" + String(static_cast<int>(reason));
  report += ",\"diagnostics_build\":\"" __DATE__ " " __TIME__ "\"";
  report += ",\"previous_trace_valid\":";
  report += previousValid ? "true" : "false";
  if (previousValid) {
    report += ",\"previous_boot_id\":\"" + hexAddress(trace.bootId) + "\"";
    report += ",\"previous_stage\":" + jsonText(trace.active, sizeof(trace.active));
    report += ",\"previous_stage_started_ms\":" + String(trace.startedMs);
    report += ",\"previous_slowest_stage\":" + jsonText(trace.slowest, sizeof(trace.slowest));
    report += ",\"previous_slowest_ms\":" + String(trace.slowestMs);
  }
  memset(&trace, 0, sizeof(trace));
  trace.bootId = bootId;
  copyStage(trace.active, "between_instrumented_sections");
  trace.startedMs = millis();
  trace.magic = kMagic;

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
  const esp_err_t integrity = esp_core_dump_image_check();
  report += ",\"coredump_check\":" + String(static_cast<int>(integrity));
  if (integrity == ESP_OK) {
    esp_core_dump_summary_t summary = {};
    const esp_err_t result = esp_core_dump_get_summary(&summary);
    report += ",\"coredump_summary_result\":" + String(static_cast<int>(result));
    if (result == ESP_OK) {
      // Do not erase the dump: preserve the complete evidence for USB retrieval.
      // A stored dump has no boot ID and may predate this particular restart.
      report += ",\"coredump_freshness\":\"unknown_stored_dump\"";
      report += ",\"task\":" + jsonText(summary.exc_task, sizeof(summary.exc_task));
      report += ",\"pc\":\"" + hexAddress(summary.exc_pc) + "\"";
      report += ",\"exception_cause\":" + String(summary.ex_info.exc_cause);
      report += ",\"elf_sha256\":" + jsonText(
          reinterpret_cast<const char *>(summary.app_elf_sha256), sizeof(summary.app_elf_sha256));
      report += ",\"backtrace_corrupted\":";
      report += summary.exc_bt_info.corrupted ? "true" : "false";
      report += ",\"backtrace\":[";
      const size_t capacity = sizeof(summary.exc_bt_info.bt) / sizeof(summary.exc_bt_info.bt[0]);
      for (size_t i = 0; i < summary.exc_bt_info.depth && i < capacity; ++i) {
        if (i) report += ',';
        report += "\"" + hexAddress(summary.exc_bt_info.bt[i]) + "\"";
      }
      report += ']';
    }
  }
#else
  report += ",\"coredump_unavailable\":true";
#endif
  report += '}';
  Serial.printf("[DIAG] %s\n", report.c_str());
}

const String &bootReport() { return report; }

String timingReport() {
  return "{\"boot_id\":\"" + hexAddress(trace.bootId) +
      "\",\"uptime_ms\":" + String(millis()) +
      ",\"slowest_stage\":" + jsonText(trace.slowest, sizeof(trace.slowest)) +
      ",\"slowest_ms\":" + String(trace.slowestMs) + "}";
}

Scope::Scope(const char *stage) : stage_(stage), previousStarted_(trace.startedMs), started_(millis()) {
  memcpy(previous_, trace.active, sizeof(previous_));
  copyStage(trace.active, stage);
  trace.startedMs = started_;
}

Scope::~Scope() {
  const uint32_t elapsed = millis() - started_;
  if (elapsed > trace.slowestMs) {
    copyStage(trace.slowest, stage_);
    trace.slowestMs = elapsed;
  }
  memcpy(trace.active, previous_, sizeof(previous_));
  trace.startedMs = previousStarted_;
}
}  // namespace dfrdiag
