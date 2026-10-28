#pragma once
#include <stdint.h>
typedef int esp_sleep_source_t;
constexpr int ESP_SLEEP_WAKEUP_ALL = 0;
int esp_sleep_get_wakeup_cause();
int esp_sleep_disable_wakeup_source(int);
int esp_sleep_enable_timer_wakeup(uint64_t);
void esp_deep_sleep_start();
