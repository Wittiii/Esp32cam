#include <cassert>
#include <iostream>
#include "../../src/dfr1154_diagnostics.cpp"
#define DFR_CAMERA_DIAGNOSTICS
#include "dfr1154_trace_scope.h"

uint32_t testNow = 0;
TestSerial Serial;
int testIntegrity = -1;
int testSummaryResult = 0;
unsigned testSummaryCalls = 0;
esp_core_dump_summary_t testSummary = {};

int main() {
  dfrdiag::begin(ESP_RST_POWERON);
  assert(dfrdiag::bootReport().find("\"previous_trace_valid\":false") != String::npos);
  assert(testSummaryCalls == 0);
  testNow = 100;
  {
    dfrdiag::Scope outer("rtsp");
    testNow = 110;
    { dfrdiag::Scope inner("mqtt"); testNow = 140; }
    assert(std::string(trace.active) == "rtsp");
    assert(trace.startedMs == 100);
  }
  assert(std::string(trace.active) == "between_instrumented_sections");
  assert(trace.slowestMs == 40);
  assert(std::string(trace.slowest) == "rtsp");

  // The deepest active marker must survive until its operation returns, then
  // restore the outer capture/send marker (including on an early return).
  for (const char *stage : {"camera_fb_get", "jpeg_decode", "rtsp_tcp_write", "camera_fb_return"}) {
    dfrdiag::Scope outer("rtsp_capture_and_send");
    [&]() {
      DFR_DIAGNOSTIC_SCOPE(stage);
      assert(std::string(trace.active) == stage);
      return;
    }();
    assert(std::string(trace.active) == "rtsp_capture_and_send");
  }

  // Capture the actual in-flight nested operation across a simulated reset.
  copyStage(trace.active, "rtsp_write");
  trace.startedMs = 200;
  dfrdiag::begin(ESP_RST_TASK_WDT);
  assert(dfrdiag::bootReport().find("\"previous_stage\":\"rtsp_write\"") != String::npos);
  assert(dfrdiag::bootReport().find("\"previous_stage_started_ms\":200") != String::npos);
  assert(trace.slowestMs == 0);
  testNow = UINT32_MAX - 10;
  { dfrdiag::Scope scope("wrap"); testNow = 10; }
  assert(trace.slowestMs == 21);

  testIntegrity = ESP_OK;
  memset(testSummary.exc_task, 'x', sizeof(testSummary.exc_task)); // no terminator
  testSummary.exc_task[0] = '"';
  testSummary.exc_bt_info.depth = 100; // must clamp to actual buffer
  testSummary.exc_bt_info.corrupted = true;
  testSummary.exc_bt_info.bt[0] = 0x42001234;
  testSummary.exc_pc = 0x42005678;
  memset(testSummary.app_elf_sha256, 'a', sizeof(testSummary.app_elf_sha256));
  copyStage(trace.active, "123456789012345678901234567890123456789");
  copyStage(trace.slowest, "123456789012345678901234567890123456789");
  dfrdiag::begin(ESP_RST_PANIC);
  assert(testSummaryCalls == 1);
  assert(dfrdiag::bootReport().find("0x42001234") != String::npos);
  assert(dfrdiag::bootReport().find("\"backtrace_corrupted\":true") != String::npos);
  assert(dfrdiag::bootReport().find("unknown_stored_dump") != String::npos);
  assert(dfrdiag::bootReport().size() <= 1024); // Node's server-log preview limit
  std::cout << dfrdiag::bootReport() << '\n';
  std::cout << dfrdiag::timingReport() << '\n';
  dfrdiag::begin(ESP_RST_POWERON);
  assert(dfrdiag::bootReport().find("\"previous_trace_valid\":false") != String::npos);
  testSummaryResult = -2;
  dfrdiag::begin(ESP_RST_PANIC);
  assert(dfrdiag::bootReport().find("\"backtrace\"") == String::npos);
  std::cout << "diagnostics: PASS\n";
}
