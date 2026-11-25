#pragma once
#include <Arduino.h>
using gpio_num_t = int;
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = 0x103;
constexpr int GPIO_INTR_ANYEDGE = 3;
inline int serviceResult = ESP_OK, failAttachPin = -1, failTypePin = -1;
inline int serviceCalls = 0, attachCalls = 0;
struct WakePin {
  void (*handler)(void*) = nullptr;
  void* arg = nullptr;
  int type = 0;
};
inline WakePin wakeHandlers[64];
inline int gpio_install_isr_service(int) {
  ++serviceCalls;
  return serviceResult;
}
inline int gpio_set_intr_type(int pin, int type) {
  if (pin == failTypePin) return ESP_FAIL;
  wakeHandlers[pin].type = type;
  return ESP_OK;
}
inline int gpio_isr_handler_add(int pin, void (*handler)(void*), void* arg) {
  ++attachCalls;
  if (pin == failAttachPin) return ESP_FAIL;
  wakeHandlers[pin].handler = handler;
  wakeHandlers[pin].arg = arg;
  return ESP_OK;
}
inline int gpio_isr_handler_remove(int pin) {
  wakeHandlers[pin].handler = nullptr;
  return ESP_OK;
}
inline int gpio_intr_disable(int) { return ESP_OK; }
inline void gpio_hold_dis(int) {}
inline void gpio_hold_en(int) {}
inline void edge(int pin) {
  gpioLevels[pin] ^= 1;
  if (wakeHandlers[pin].handler) wakeHandlers[pin].handler(wakeHandlers[pin].arg);
}
