#pragma once

#include <cstdint>
#include <string>

#include "Print.h"

using String = std::string;

struct EspHostStub {
  uint32_t getFreeHeap() const { return UINT32_MAX; }
  uint32_t getMaxAllocHeap() const { return UINT32_MAX; }
};

inline EspHostStub ESP;
inline uint32_t micros() { return 0; }
inline uint32_t millis() { return 0; }
inline void delay(uint32_t) {}
