#pragma once

#include <Arduino.h>
#include <Rtc.h>

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable Rtc _sdkRtc;
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable uint16_t _cachedYear = 2000;
  mutable uint8_t _cachedMonth = 1;
  mutable uint8_t _cachedDay = 1;
  mutable bool _hasCachedTime = false;
  mutable bool _hasCachedDate = false;
  mutable unsigned long _lastPollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  enum DateFormat : uint8_t {
    MONTH_DAY_YEAR_LONG = 0,
    DAY_MONTH_YEAR_LONG = 1,
    MONTH_DAY_YEAR_NUMERIC = 2,
    DAY_MONTH_YEAR_NUMERIC = 3,
    YEAR_MONTH_DAY_NUMERIC = 4,
    MONTH_DAY_NUMERIC = 5,
    DAY_MONTH_NUMERIC = 6,
    MONTH_DAY_LONG = 7,
    DAY_MONTH_LONG = 8,
    DATE_FORMAT_COUNT
  };

  // Call after BoardConfig has selected the active device.
  void begin();

  // True if an RTC is present on this device
  bool isAvailable() const { return _available; }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Returns the raw RTC date/time before any user-configured timezone offset is applied.
  // The RTC is synced in UTC, so callers that need wall-clock local time should apply SETTINGS.clockUtcOffsetQ.
  bool getDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const {
    return getDate(year, month, day, hour, minute);
  }

  // Format date into a caller-provided buffer using the requested display format.
  // utcOffsetQuarterHoursBiased matches formatTime so the date rolls over at local midnight.
  // Returns false if RTC is not available or the RTC date is invalid.
  bool formatDate(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48,
                  DateFormat dateFormat = MONTH_DAY_YEAR_LONG, char numericSeparator = '/') const;

  // Uncached UTC wall time for dated reading spans. No stale RTC fallback.
  bool getReadingDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute,
                          uint8_t& second) const;

  // Sync the RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();

  // Sync the ESP32 system clock without requiring an external RTC.
  bool syncSystemTimeFromNTP();

 private:
  bool getDate(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const;
  bool writeDateTimeToRTC(uint16_t year, uint8_t month, uint8_t day, uint8_t weekday, uint8_t hour, uint8_t minute,
                          uint8_t second);
};
