#pragma once
#include <Arduino.h>

#include <cstdint>
using BaseType_t = int;
using QueueHandle_t = void*;
using TaskHandle_t = void*;
using TickType_t = uint32_t;
using StackType_t = uint32_t;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
struct portMUX_TYPE {};
#define portMUX_INITIALIZER_UNLOCKED \
  {                                  \
  }
#define portENTER_CRITICAL(mux) ((void)(mux), ++criticalDepth)
#define portEXIT_CRITICAL(mux) ((void)(mux), --criticalDepth)
struct FakeTaskYield {
  TickType_t ticks;
};
inline bool taskRunning = false;
constexpr int pdTRUE = 1, pdFALSE = 0;
#define pdMS_TO_TICKS(ms) (ms)
#define portYIELD_FROM_ISR() ((void)0)
