#pragma once
using gpio_num_t = int;
constexpr int GPIO_MODE_OUTPUT = 1;
inline void gpio_set_direction(int, int) {}
inline void gpio_set_level(int, int) {}
inline void gpio_hold_en(int) {}
inline void gpio_deep_sleep_hold_en() {}
inline void esp_sleep_config_gpio_isolate() {}
inline void esp_deep_sleep_start() {}

constexpr int ESP_SLEEP_WAKEUP_ALL = 0;
inline void esp_sleep_enable_timer_wakeup(uint64_t) {}
inline void esp_sleep_enable_gpio_wakeup() {}
inline void esp_sleep_disable_wakeup_source(int) { ++power_test::wakeCleanup; }
inline void esp_light_sleep_start() {
  ++power_test::lightSleeps;
  if (power_test::timerWakesBeforeAction > 0)
    --power_test::timerWakesBeforeAction;
  else if (power_test::pressOnWake)
    power_test::powerPressed = true;
  else
    power_test::usbPowered = false;
}

constexpr int ESP_SLEEP_WAKEUP_UNDEFINED = 0;
inline int esp_sleep_get_wakeup_cause() { return power_test::wakeupCause; }
