#pragma once

#include <cstdint>
#include <string>

#include "Print.h"

using String = std::string;

struct EspHostStub {
  uint32_t freeHeap = UINT32_MAX;
  uint32_t largest = UINT32_MAX;
  uint32_t getFreeHeap() const { return freeHeap; }
  uint32_t getMaxAllocHeap() const { return largest; }
};

inline EspHostStub ESP;
inline uint32_t millis() { return 0; }
inline void delay(uint32_t) {}
