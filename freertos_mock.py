import os

mock_dir = "lib/simulator_mock/src/freertos"
os.makedirs(mock_dir, exist_ok=True)

with open(f"{mock_dir}/FreeRTOS.h", "w") as f:
    f.write("""#pragma once
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFF
#define taskENTER_CRITICAL(x) do {} while(0)
#define taskEXIT_CRITICAL(x) do {} while(0)
#define eIncrement 1
typedef void* TaskHandle_t;
""")

with open(f"{mock_dir}/task.h", "w") as f:
    f.write("""#pragma once
#include "FreeRTOS.h"
#define xTaskCreate(fn, name, stack, params, prio, handle) do { if(handle) *(handle) = (TaskHandle_t)1; } while(0)
#define ulTaskNotifyTake(clear, wait) 1
#define xTaskNotify(handle, val, action) do {} while(0)
#define xTaskGetCurrentTaskHandle() ((TaskHandle_t)1)
""")

with open(f"{mock_dir}/semphr.h", "w") as f:
    f.write("""#pragma once
#include "FreeRTOS.h"
typedef void* SemaphoreHandle_t;
#define xSemaphoreCreateMutex() ((SemaphoreHandle_t)1)
#define xSemaphoreTake(sem, ticks) true
#define xSemaphoreGive(sem) true
#define xSemaphoreGetMutexHolder(sem) ((TaskHandle_t)0)
#define xQueuePeek(a, b, c) pdTRUE
""")

print("FreeRTOS mocked.")
