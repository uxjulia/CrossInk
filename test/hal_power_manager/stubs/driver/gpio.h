#pragma once
#include <esp_sleep.h>
constexpr int GPIO_INTR_LOW_LEVEL = 0;
constexpr int GPIO_INTR_DISABLE = 1;
constexpr int ESP_OK = 0;
inline int gpio_intr_disable(int) {
  if (power_test::interruptDisableFails) return -1;
  power_test::runtimeInterruptEnabled = false;
  return ESP_OK;
}
inline void gpio_wakeup_enable(int, int) { power_test::unsafeLevelWake |= power_test::runtimeInterruptEnabled; }
inline void gpio_wakeup_disable(int) {}
inline void gpio_set_intr_type(int, int) {}
