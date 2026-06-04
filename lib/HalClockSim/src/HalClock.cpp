#include "HalClock.h"

HalClock halClock;

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  hour = 0;
  minute = 0;
  return false;
}

bool HalClock::getDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const {
  year = 0;
  month = 0;
  day = 0;
  hour = 0;
  minute = 0;
  return false;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  (void)buf;
  (void)bufSize;
  (void)utcOffsetQuarterHoursBiased;
  (void)use12Hour;
  return false;
}

bool HalClock::formatDate(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased) const {
  (void)buf;
  (void)bufSize;
  (void)utcOffsetQuarterHoursBiased;
  return false;
}
