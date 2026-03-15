#pragma once
#include <condition_variable>
#include <mutex>
#include <thread>

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFF
#define taskENTER_CRITICAL(x) do {} while(0)
#define taskEXIT_CRITICAL(x) do {} while(0)
#define eIncrement 1
#define portTICK_PERIOD_MS 1

// TaskHandle wraps a real thread + a notification counter protected by a condvar.
struct SimTaskHandle {
    std::thread thread;
    std::mutex  mtx;
    std::condition_variable cv;
    uint32_t    notifyCount = 0;
    std::thread::id id;
};
typedef SimTaskHandle* TaskHandle_t;
