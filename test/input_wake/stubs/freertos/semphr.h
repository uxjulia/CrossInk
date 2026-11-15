#pragma once
#include <Arduino.h>

#include "FreeRTOS.h"
struct StaticSemaphore_t {
  bool pending = false;
};
using SemaphoreHandle_t = StaticSemaphore_t*;
inline bool failSignal = false;
inline void (*duringWait)() = nullptr;
inline int signalCreates = 0;
inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* storage) {
  ++signalCreates;
  return failSignal ? nullptr : storage;
}
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) {
  if (failSignal) return nullptr;
  storage->pending = true;
  return storage;
}
inline unsigned uxSemaphoreGetCount(SemaphoreHandle_t signal) { return signal->pending ? 1u : 0u; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t signal) {
  signal->pending = true;
  return pdTRUE;
}
inline BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t signal, BaseType_t*) {
  signal->pending = true;
  return pdTRUE;
}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t signal, TickType_t ticks) {
  if (taskRunning && !signal->pending && ticks) throw FakeTaskYield{ticks};
  if (!signal->pending && ticks && duringWait) duringWait();
  if (!signal->pending) {
    fakeNow += ticks;
    return pdFALSE;
  }
  signal->pending = false;
  return pdTRUE;
}
