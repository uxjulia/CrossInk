#pragma once

#include "FreeRTOS.h"

extern "C" BaseType_t xTaskCreatePinnedToCore(TaskFunction_t taskCode, const char* name, uint32_t stackDepth,
                                              void* parameters, UBaseType_t priority, TaskHandle_t* createdTask,
                                              BaseType_t coreId);
