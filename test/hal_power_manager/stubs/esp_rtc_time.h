#pragma once
#include <Arduino.h>
inline uint64_t esp_rtc_get_time_us() { return power_test::rtcMs * 1000; }
