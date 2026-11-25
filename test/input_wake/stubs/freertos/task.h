#pragma once
#include "FreeRTOS.h"
inline void vTaskDelay(int) {}
inline int xTaskCreate(void (*)(void*), const char*, int, void*, int, void**) { return 0; }
struct StaticTask_t {
  void (*function)(void*) = nullptr;
  void* arg = nullptr;
};
inline StaticTask_t* fakeTask = nullptr;
inline bool failTask = false;
inline TaskHandle_t xTaskCreateStatic(void (*function)(void*), const char*, uint32_t, void* arg, int, StackType_t*,
                                      StaticTask_t* storage) {
  if (failTask) return nullptr;
  storage->function = function;
  storage->arg = arg;
  fakeTask = storage;
  return storage;
}
inline void vTaskSuspend(TaskHandle_t) { throw FakeTaskYield{0}; }
inline void vTaskDelete(TaskHandle_t) { fakeTask = nullptr; }
inline uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 0; }
