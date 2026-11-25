#pragma once
#include <cstdint>
struct Clock {
  bool getDateTime(uint16_t&, uint8_t&, uint8_t&, uint8_t&, uint8_t&) { return false; }
};
inline Clock halClock;
