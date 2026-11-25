#pragma once
#include <cstdint>
class HalClock {
 public:
  bool valid = true;
  uint16_t year = 2026;
  uint8_t month = 10, day = 1, hour = 0, minute = 0, second = 10;
  bool getDateTime(uint16_t& y, uint8_t& mo, uint8_t& d, uint8_t& h, uint8_t& mi) const {
    y = year;
    mo = month;
    d = day;
    h = hour;
    mi = minute;
    return valid;
  }
  bool getReadingDateTime(uint16_t& y, uint8_t& mo, uint8_t& d, uint8_t& h, uint8_t& mi, uint8_t& s) const {
    s = second;
    return getDateTime(y, mo, d, h, mi);
  }
};
inline HalClock halClock;
