#pragma once
#include <cstdint>
enum esp_reset_reason_t { ESP_RST_POWERON, ESP_RST_TASK_WDT, ESP_RST_INT_WDT,
                         ESP_RST_WDT, ESP_RST_PANIC, ESP_RST_SW };
inline uint32_t esp_random() { return 123; }
