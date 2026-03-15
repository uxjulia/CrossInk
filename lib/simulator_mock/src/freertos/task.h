#pragma once
#include <cstdint>

#include "FreeRTOS.h"

// Thread-local pointer so ulTaskNotifyTake can find the current task's handle.
inline thread_local SimTaskHandle* tl_currentTaskHandle = nullptr;

// Create a real OS thread. The FreeRTOS task function signature is void(*)(void*).
inline int xTaskCreate(void (*fn)(void*), const char* /*name*/, uint32_t /*stackDepth*/, void* param, int /*priority*/,
                       TaskHandle_t* handle) {
  auto* h = new SimTaskHandle();
  h->thread = std::thread([fn, param, h]() {
    tl_currentTaskHandle = h;
    h->id = std::this_thread::get_id();
    fn(param);
  });
  if (handle) *handle = h;
  return 1;  // pdPASS
}

// Block until notified (simulates ulTaskNotifyTake with clear-on-exit).
inline uint32_t ulTaskNotifyTake(int /*clearOnExit*/, uint32_t /*ticksToWait*/) {
  auto* h = tl_currentTaskHandle;
  if (!h) return 1;  // Not in a task thread, don't block
  std::unique_lock<std::mutex> lk(h->mtx);
  h->cv.wait(lk, [h] { return h->notifyCount > 0; });
  h->notifyCount--;
  return 1;
}

// Wake a task by incrementing its notification counter and signalling its condvar.
inline void xTaskNotify(TaskHandle_t handle, uint32_t /*value*/, int /*action*/) {
  if (!handle) return;
  {
    std::lock_guard<std::mutex> lk(handle->mtx);
    handle->notifyCount++;
  }
  handle->cv.notify_one();
}

inline TaskHandle_t xTaskGetCurrentTaskHandle() { return tl_currentTaskHandle; }
inline void vTaskDelete(TaskHandle_t h) {
  if (h) {
    if (h->thread.joinable()) h->thread.detach();
    delete h;
  }
}
inline unsigned int uxTaskGetStackHighWaterMark(TaskHandle_t) { return 2048; }
inline void vTaskList(char*) {}
inline void vTaskDelay(int) {}
