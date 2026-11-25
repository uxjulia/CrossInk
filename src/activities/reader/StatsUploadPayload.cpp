#include "StatsUploadPayload.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
class Writer {
  char* out;
  size_t remaining;
  bool valid = true;

 public:
  Writer(char* out, size_t capacity) : out(out), remaining(capacity) {}
  void add(const char* format, ...) {
    if (!valid) return;
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(out, remaining, format, args);
    va_end(args);
    if (n < 0 || static_cast<size_t>(n) >= remaining) {
      valid = false;
      return;
    }
    out += n;
    remaining -= n;
  }
  template <size_t N>
  void array(const char* key, const std::array<uint32_t, N>& values) {
    add(",\"%s\":[", key);
    for (size_t i = 0; i < N; ++i) add("%s%lu", i ? "," : "", static_cast<unsigned long>(values[i]));
    add("]");
  }
  bool ok() const { return valid; }
};

// IDs are protocol identifiers, never user-entered names. Restrict them so
// embedding them does not need an allocating JSON string escaper.
bool validId(const char* id, size_t maxLength) {
  if (!id || !*id || strlen(id) > maxLength) return false;
  for (; *id; ++id) {
    if (!((*id >= 'a' && *id <= 'z') || (*id >= 'A' && *id <= 'Z') || (*id >= '0' && *id <= '9') || *id == '-' ||
          *id == '_'))
      return false;
  }
  return true;
}

unsigned long long unixDate(const ReadingStatsDate& date) {
  return date.isValid() ? 946684800ULL + static_cast<unsigned long long>(readingStatsDayIndex(date)) * 86400ULL : 0;
}
}  // namespace

namespace StatsUploadPayload {
bool global(char* out, size_t capacity, const char* deviceId, const GlobalReadingStats& s, uint32_t dailyDay,
            uint32_t dailySeconds, const StatsCounterId& dailyId) {
  if (!out || !capacity || !validId(deviceId, 64)) return false;
  Writer w(out, capacity);
  char counterId[33];
  s.counterId.hex(counterId);
  w.add(
      "{\"device_id\":\"%s\",\"device\":\"CrossInk\",\"v\":5,\"sessions\":%lu,\"seconds\":%lu,\"pages\":%lu,"
      "\"completed\":%lu,\"counter_id\":\"%s\"",
      deviceId, static_cast<unsigned long>(s.totalSessions), static_cast<unsigned long>(s.totalReadingSeconds),
      static_cast<unsigned long>(s.totalPagesTurned), static_cast<unsigned long>(s.completedBooks), counterId);
  w.array("tod", s.timeOfDaySeconds);
  w.array("dow", s.dayOfWeekSeconds);
  w.add(",\"anchor_day\":%lu,\"streak\":%u,\"history_b64\":\"", static_cast<unsigned long>(s.readingHistoryAnchorDay),
        s.longestReadingStreak);
  static constexpr char BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const auto& bytes = s.readingHistoryBits;
  for (size_t i = 0; i < bytes.size(); i += 3) {
    const uint32_t value = (static_cast<uint32_t>(bytes[i]) << 16) |
                           (i + 1 < bytes.size() ? static_cast<uint32_t>(bytes[i + 1]) << 8 : 0) |
                           (i + 2 < bytes.size() ? bytes[i + 2] : 0);
    w.add("%c%c%c%c", BASE64[(value >> 18) & 63], BASE64[(value >> 12) & 63],
          i + 1 < bytes.size() ? BASE64[(value >> 6) & 63] : '=', i + 2 < bytes.size() ? BASE64[value & 63] : '=');
  }
  w.add("\"");
  if (dailyDay != UINT32_MAX) {
    ReadingStatsDate date;
    if (!readingStatsDateFromDayIndex(dailyDay, date)) return false;
    dailyId.hex(counterId);
    w.add(",\"daily\":[{\"date\":\"%04u-%02u-%02u\",\"seconds\":%lu,\"counter_id\":\"%s\"}]", date.year, date.month,
          date.day, static_cast<unsigned long>(dailySeconds), counterId);
  }
  w.add("}");
  return w.ok();
}

bool book(char* out, size_t capacity, const char* deviceId, const char* document, const BookReadingStats& s) {
  BookBatch batch(out, capacity);
  return batch.begin(deviceId) && batch.add(document, s) && batch.finish();
}

bool BookBatch::begin(const char* deviceId) {
  length = 0;
  items = 0;
  if (!out || !capacity || !validId(deviceId, 64)) return false;
  Writer w(out, capacity);
  w.add("{\"device_id\":\"%s\",\"items\":[", deviceId);
  if (!w.ok()) return false;
  length = strlen(out);
  return true;
}

bool BookBatch::add(const char* document, const BookReadingStats& s) {
  if (length == 0 || items >= MAX_ITEMS || !validId(document, 32) || strlen(document) != 32) return false;
  // Leave room for the closing "]}".
  if (capacity - length < 3) return false;
  Writer w(out + length, capacity - length - 2);
  char counterId[33];
  s.counterId.hex(counterId);
  w.add(
      "%s{\"document\":\"%s\",\"v\":5,\"sessions\":%u,\"seconds\":%lu,\"pages\":%lu,\"completed\":%s,"
      "\"avg_fwd\":%u,\"pace_n\":%u,\"eta\":%lu,\"start_manual\":%s,\"finish_manual\":%s,\"start_date\":%llu,"
      "\"finished_date\":%llu,\"counter_id\":\"%s\"",
      items ? "," : "", document, s.sessionCount, static_cast<unsigned long>(s.totalReadingSeconds),
      static_cast<unsigned long>(s.totalPagesTurned), s.isCompleted ? "true" : "false", s.avgSecondsPerForwardPage,
      s.paceSampleCount, static_cast<unsigned long>(s.estimatedTimeLeftSeconds), s.startDateManual ? "true" : "false",
      s.finishedDateManual ? "true" : "false", unixDate(s.startDate), unixDate(s.finishedDate), counterId);
  w.array("tod", s.timeOfDaySeconds);
  w.array("dow", s.dayOfWeekSeconds);
  w.add("}");
  if (!w.ok()) {
    out[length] = '\0';  // roll back the partial item
    return false;
  }
  length += strlen(out + length);
  ++items;
  return true;
}

bool BookBatch::finish() {
  if (length == 0 || items == 0 || capacity - length < 3) return false;
  memcpy(out + length, "]}", 3);
  length += 2;
  return true;
}
}  // namespace StatsUploadPayload
