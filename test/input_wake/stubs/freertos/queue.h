#pragma once
#include "FreeRTOS.h"
inline void* xQueueCreate(int, int) { return nullptr; }
inline int xQueueSend(void*, const void*, int) { return 0; }
inline int xQueueReceive(void*, void*, int) { return 0; }
inline void xQueueReset(void*) {}
