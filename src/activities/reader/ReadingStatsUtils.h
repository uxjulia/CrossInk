#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

constexpr size_t READING_TIME_BUCKET_COUNT = 4;
constexpr size_t READING_DAY_OF_WEEK_COUNT = 7;
constexpr size_t READING_HISTORY_DAYS = 730;
constexpr size_t READING_HISTORY_BYTES = (READING_HISTORY_DAYS + 7) / 8;

enum class ReadingTimeBucket : uint8_t { Morning = 0, Afternoon, Evening, Night };

struct ReadingStatsDate {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;

  bool isValid() const;
  void clear();
};

struct ReadingStatsDateTime {
  ReadingStatsDate date;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;

  bool isValid() const;
};

bool isLeapYear(uint16_t year);
uint8_t daysInMonth(uint16_t year, uint8_t month);
bool isValidReadingStatsDate(const ReadingStatsDate& date);
int compareReadingStatsDate(const ReadingStatsDate& lhs, const ReadingStatsDate& rhs);
void addDaysToReadingStatsDate(ReadingStatsDate& date, int delta);
void addSecondsToReadingStatsDateTime(ReadingStatsDateTime& dt, uint32_t seconds);
uint32_t readingStatsDayIndex(const ReadingStatsDate& date);
bool readingStatsDateFromDayIndex(uint32_t dayIndex, ReadingStatsDate& outDate);
uint8_t readingStatsDayOfWeekIndex(const ReadingStatsDate& date);  // Monday = 0
ReadingTimeBucket readingTimeBucketForHour(uint8_t hour);
bool getCurrentLocalReadingStatsDateTime(ReadingStatsDateTime& outDateTime);
uint16_t readingSpanDaysInclusive(const ReadingStatsDate& start, const ReadingStatsDate& end);
uint16_t readingSpanDaysElapsed(const ReadingStatsDate& start, const ReadingStatsDate& end);
void formatReadingStatsShortDate(const ReadingStatsDate& date, char* buf, size_t len);
void formatReadingStatsMonthToken(const ReadingStatsDate& date, char* buf, size_t len);

void recordReadingSpanIntoBuckets(std::array<uint32_t, READING_TIME_BUCKET_COUNT>& timeOfDaySeconds,
                                  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT>& dayOfWeekSeconds,
                                  const ReadingStatsDateTime& localStart, uint32_t seconds);
void markReadingHistoryDay(uint32_t& anchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& bits, uint32_t dayIndex);
void recordReadingSpanIntoHistory(uint32_t& anchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                                  const ReadingStatsDateTime& localStart, uint32_t seconds);
void mergeReadingHistory(uint32_t& targetAnchorDay, std::array<uint8_t, READING_HISTORY_BYTES>& targetBits,
                         uint32_t sourceAnchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& sourceBits);
uint16_t computeReadingHistoryLongestStreak(uint32_t anchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& bits);
uint16_t computeReadingHistoryCurrentStreak(uint32_t anchorDay, const std::array<uint8_t, READING_HISTORY_BYTES>& bits,
                                            const ReadingStatsDate* today);
