#pragma once
#include <mutex>
using SemaphoreHandle_t = std::mutex*;
constexpr int portMAX_DELAY = -1;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::mutex; }
inline void xSemaphoreTake(SemaphoreHandle_t mutex, int) { mutex->lock(); }
inline void xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->unlock(); }
