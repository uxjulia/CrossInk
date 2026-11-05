#include "DailyReadingStats.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "util/StatsStorage.h"

namespace DailyReadingStats {
namespace {
struct Cache {
  uint32_t day = UINT32_MAX;
  Counter counter{};
  uint32_t dirty = 0;
};
Cache cache[2];
std::string localDirectory;
bool prepareDirectory() {
  if (localDirectory.empty()) localDirectory = StatsStorage::localPath(DIRECTORY, true);
  return !localDirectory.empty();
}
uint32_t migratedMonths[2] = {UINT32_MAX, UINT32_MAX};
constexpr size_t MONTH_BYTES = 12 + 31 * 8 + 4;
// One bounded 256-byte month on the stack avoids heap churn at each checkpoint.
struct Month {
  Counter days[31]{};
  uint32_t generation = 0;
  int slot = -1;
};
static_assert(sizeof(Month) <= 256);
uint32_t le32(const uint8_t* b) {
  return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
}
void put32(uint8_t* b, uint32_t value) {
  for (size_t i = 0; i < 4; ++i) b[i] = value >> (i * 8);
}
uint32_t hashBytes(uint32_t hash, const uint8_t* bytes, size_t size) {
  for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
  return hash;
}
void legacyPath(char* path, uint32_t day, const char* suffix) {
  snprintf(path, 64, "%s/%05lu.bin%s", localDirectory.c_str(), static_cast<unsigned long>(day), suffix);
}
void monthPath(char* path, uint32_t first, int slot) {
  ReadingStatsDate date;
  readingStatsDateFromDayIndex(first, date);
  snprintf(path, 64, "%s/%04u-%02u.%c.bin", localDirectory.c_str(), date.year, date.month, slot ? 'b' : 'a');
}
bool monthFor(uint32_t day, uint32_t& first, uint8_t& count) {
  ReadingStatsDate date;
  if (!readingStatsDateFromDayIndex(day, date)) {
    LOG_ERR("DailyStats", "Invalid day index");
    return false;
  }
  first = day - date.day + 1;
  count = daysInMonth(date.year, date.month);
  return true;
}
// A corrupt slot can fall back; I/O errors and unknown versions must never be overwritten.
bool readSlot(uint32_t first, uint8_t count, int slot, bool& exists, bool& valid, uint32_t& generation,
              Counter* counters = nullptr, const Counter* synced = nullptr) {
  char path[64];
  monthPath(path, first, slot);
  exists = Storage.exists(path);
  valid = false;
  if (!exists) return true;
  FsFile file;
  if (!Storage.openFileForRead("DailyStats", path, file)) {
    LOG_ERR("DailyStats", "Cannot read monthly snapshot: %s", path);
    return false;
  }
  uint8_t data[12]{};
  const size_t size = file.fileSize();
  const int n = file.read(data, sizeof(data));
  if (n < 0 || (size == MONTH_BYTES && n != sizeof(data))) {
    file.close();
    LOG_ERR("DailyStats", "Monthly header read failed: %s", path);
    return false;
  }
  if (size > MONTH_BYTES || (n > 0 && data[0] > 2)) {
    file.close();
    LOG_ERR("DailyStats", "Refusing newer monthly snapshot: %s", path);
    return false;
  }
  bool ok = size == MONTH_BYTES && n == sizeof(data) && data[0] == 2 && data[1] == 'D' && data[2] == 'R' &&
            data[3] == 0 && le32(data + 4) == first;
  generation = le32(data + 8);
  uint32_t hash = hashBytes(2166136261u, data, sizeof(data));
  for (size_t i = 0; ok && i < 31; ++i) {
    if (file.read(data, 8) != 8) {
      file.close();
      LOG_ERR("DailyStats", "Monthly record read failed: %s", path);
      return false;
    }
    hash = hashBytes(hash, data, 8);
    const Counter counter{le32(data), le32(data + 4)};
    ok = ok && counter.uploaded <= counter.seconds && (i < count || (!counter.seconds && !counter.uploaded));
    // Cleanup also proves that the newest acknowledgments cover every older total.
    if (synced && counter.seconds > synced[i].uploaded) ok = false;
    if (counters) counters[i] = counter;
  }
  if (ok && file.read(data, 4) != 4) {
    file.close();
    LOG_ERR("DailyStats", "Monthly checksum read failed: %s", path);
    return false;
  }
  ok = ok && le32(data) == hash;
  const bool closed = file.close();
  if (!closed) {
    LOG_ERR("DailyStats", "Cannot close monthly snapshot");
    return false;
  }
  valid = ok;
  return true;
}
bool loadMonth(uint32_t first, uint8_t count, Month& month, bool convertingLegacy = false) {
  month = {};
  unsigned existingSlots = 0;
  for (int slot = 0; slot < 2; ++slot) {
    bool exists, valid;
    uint32_t generation = 0;
    if (!readSlot(first, count, slot, exists, valid, generation)) return false;
    if (exists) existingSlots |= 1u << slot;
    const uint32_t delta = generation - month.generation;
    if (valid && (month.slot < 0 || (delta != 0 && delta < 0x80000000u))) {
      month.generation = generation;
      month.slot = slot;
    }
  }
  if (month.slot < 0) {
    // Conversion starts with slot a and cannot delete sources until b is verified.
    // An interrupted first snapshot can therefore be rebuilt from the intact legacy month.
    if (convertingLegacy && existingSlots == 1) return true;
    // New months initialize an empty b before creating a. A lone torn b has
    // never held committed reading history and can safely be initialized again.
    if (existingSlots == 2) return true;
    if (existingSlots) LOG_ERR("DailyStats", "No valid monthly snapshot");
    return !existingSlots;
  }
  bool exists, valid;
  if (!readSlot(first, count, month.slot, exists, valid, month.generation, month.days) || !valid) {
    LOG_ERR("DailyStats", "Monthly snapshot changed while reading");
    return false;
  }
  return true;
}
bool saveMonth(uint32_t first, uint8_t count, Month& month) {
  if (!Storage.exists(localDirectory.c_str()) && !Storage.mkdir(localDirectory.c_str())) {
    LOG_ERR("DailyStats", "Cannot create daily stats directory");
    return false;
  }
  const int target = month.slot == 0 ? 1 : 0;
  const uint32_t generation = month.generation + 1;
  char path[64];
  monthPath(path, first, target);
  FsFile file;
  if (!Storage.openFileForWrite("DailyStats", path, file)) {
    LOG_ERR("DailyStats", "Cannot write monthly snapshot");
    return false;
  }
  uint8_t data[12] = {2, 'D', 'R', 0};
  put32(data + 4, first);
  put32(data + 8, generation);
  uint32_t hash = hashBytes(2166136261u, data, sizeof(data));
  bool ok = file.write(data, sizeof(data)) == sizeof(data);
  for (const auto& counter : month.days) {
    put32(data, counter.seconds);
    put32(data + 4, counter.uploaded);
    hash = hashBytes(hash, data, 8);
    if (ok) ok = file.write(data, 8) == 8;
  }
  put32(data, hash);
  ok = ok && file.write(data, 4) == 4 && file.sync();
  const bool closed = file.close();
  bool exists = false, valid = false;
  uint32_t verifiedGeneration = 0;
  if (!ok || !closed || !readSlot(first, count, target, exists, valid, verifiedGeneration) || !valid ||
      verifiedGeneration != generation) {
    LOG_ERR("DailyStats", "Monthly checkpoint failed; previous snapshot retained");
    return false;
  }
  month.slot = target;
  month.generation = generation;
  return true;
}
bool readLegacy(uint32_t day, Counter& counter, uint8_t& validMask) {
  bool exists = false;
  validMask = 0;
  counter = {};
  bool valid = false;
  uint8_t bit = 0;
  for (const char* suffix : {"", ".bak", ".tmp"}) {
    char path[64];
    legacyPath(path, day, suffix);
    const uint8_t mask = 1u << bit++;
    if (!Storage.exists(path)) continue;
    exists = true;
    FsFile file;
    if (!Storage.openFileForRead("DailyStats", path, file)) {
      LOG_ERR("DailyStats", "Cannot read legacy counter");
      return false;
    }
    uint8_t data[13]{};
    const size_t size = file.fileSize();
    const int n = file.read(data, sizeof(data));
    const bool closed = file.close();
    if (!closed || n < 0 || (size == sizeof(data) && n != sizeof(data)) || size > sizeof(data) ||
        (n > 0 && data[0] > 1)) {
      LOG_ERR("DailyStats", "Cannot migrate legacy counter: %s", path);
      return false;
    }
    if (size != sizeof(data) || n != sizeof(data) || data[0] != 1 ||
        le32(data + 9) != hashBytes(2166136261u ^ day, data, 9) || le32(data + 5) > le32(data + 1))
      continue;
    counter.seconds = std::max(counter.seconds, le32(data + 1));
    counter.uploaded = std::max(counter.uploaded, le32(data + 5));
    valid = true;
    validMask |= mask;
  }
  if (exists && !valid) LOG_ERR("DailyStats", "No valid legacy counter");
  return !exists || valid;
}
bool digits(const char* name, size_t n, uint32_t& value) {
  value = 0;
  for (size_t i = 0; i < n; ++i) {
    if (name[i] < '0' || name[i] > '9') return false;
    value = value * 10 + name[i] - '0';
  }
  return true;
}
bool fileMonth(const char* name, uint32_t& first) {
  const size_t length = strlen(name);
  uint32_t day;
  uint8_t count;
  if (((length == 9 && strcmp(name + 5, ".bin") == 0) ||
       (length == 13 && (strcmp(name + 5, ".bin.bak") == 0 || strcmp(name + 5, ".bin.tmp") == 0))) &&
      digits(name, 5, day))
    return monthFor(day, first, count);
  uint32_t year, month;
  if (length != 13 || name[4] != '-' || name[7] != '.' || (name[8] != 'a' && name[8] != 'b') ||
      strcmp(name + 9, ".bin") != 0 || !digits(name, 4, year) || !digits(name + 5, 2, month))
    return false;
  const ReadingStatsDate date{static_cast<uint16_t>(year), static_cast<uint8_t>(month), 1};
  if (!date.isValid()) return false;
  first = readingStatsDayIndex(date);
  return true;
}
// Close the directory before migration, writes, or network work. Rescanning by month
// keeps memory bounded and avoids invalidating a directory cursor during conversion.
bool nextMonth(uint32_t after, uint32_t& first) {
  first = UINT32_MAX;
  if (!Storage.exists(localDirectory.c_str())) return true;
  FsFile dir = Storage.open(localDirectory.c_str());
  if (!dir || !dir.isDirectory()) {
    dir.close();
    LOG_ERR("DailyStats", "Cannot open daily stats directory");
    return false;
  }
  bool ok = true;
  for (FsFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    char name[32]{};
    const bool regular = !file.isDirectory();
    const size_t length = file.getName(name, sizeof(name));
    file.close();
    if (!regular) continue;
    if (!length) {
      ok = false;
      break;
    }
    uint32_t candidate;
    if (fileMonth(name, candidate) && (after == UINT32_MAX || candidate > after)) first = std::min(first, candidate);
  }
  ok = ok && !FsHelpers::directoryIterationFailed(dir);
  dir.close();
  if (!ok) LOG_ERR("DailyStats", "Daily stats directory scan failed");
  return ok;
}
bool migrateMonth(uint32_t first, uint8_t count) {
  if (migratedMonths[0] == first || migratedMonths[1] == first) return true;
  bool anyLegacy = false;
  for (uint8_t i = 0; !anyLegacy && i < count; ++i) {
    for (const char* suffix : {"", ".bak", ".tmp"}) {
      char path[64];
      legacyPath(path, first + i, suffix);
      anyLegacy |= Storage.exists(path);
    }
  }
  if (anyLegacy) {
    Month month;
    if (!loadMonth(first, count, month, true)) return false;
    for (uint8_t i = 0; i < count; ++i) {
      Counter legacy;
      uint8_t validMask;
      if (!readLegacy(first + i, legacy, validMask)) return false;
      month.days[i].seconds = std::max(month.days[i].seconds, legacy.seconds);
      month.days[i].uploaded = std::max(month.days[i].uploaded, legacy.uploaded);
    }
    // Both recovery copies must contain every converted day before any source is deleted.
    if (!saveMonth(first, count, month) || !saveMonth(first, count, month)) return false;
    for (uint8_t i = 0; i < count; ++i) {
      Counter legacy;
      uint8_t validMask;
      if (!readLegacy(first + i, legacy, validMask)) return false;
      // Remove corrupt siblings first. If cleanup fails, at least one valid legacy
      // copy remains until its final removal, so a reboot can always retry conversion.
      for (int validPass = 0; validPass < 2; ++validPass) {
        uint8_t bit = 0;
        for (const char* suffix : {"", ".bak", ".tmp"}) {
          const bool valid = (validMask & (1u << bit++)) != 0;
          if (valid != bool(validPass)) continue;
          char path[64];
          legacyPath(path, first + i, suffix);
          if (Storage.exists(path) && !Storage.remove(path)) {
            LOG_ERR("DailyStats", "Legacy cleanup deferred: %s", path);
            return false;
          }
        }
      }
    }
  }
  migratedMonths[1] = migratedMonths[0];
  migratedMonths[0] = first;
  return true;
}
bool pruneMonth(uint32_t first, uint8_t count) {
  if (!migrateMonth(first, count)) return false;
  Month month;
  if (!loadMonth(first, count, month)) return false;
  if (month.slot < 0) return true;
  for (const auto& counter : month.days)
    if (counter.seconds != counter.uploaded) return true;
  // A corrupt sibling could hide a newer unsynced checkpoint. Unlike normal
  // recovery reads, deletion requires every existing slot to be valid and covered.
  for (int slot = 0; slot < 2; ++slot) {
    bool exists, valid;
    uint32_t generation = 0;
    if (!readSlot(first, count, slot, exists, valid, generation, nullptr, month.days)) return false;
    if (exists && !valid) {
      LOG_ERR("DailyStats", "Retaining uncertain monthly history");
      return false;
    }
  }
  // Keep the newest fully acknowledged copy until last. A failed delete or reboot
  // leaves enough information to retry, without reviving an older unacknowledged copy.
  for (int slot : {1 - month.slot, month.slot}) {
    char path[64];
    monthPath(path, first, slot);
    if (Storage.exists(path) && !Storage.remove(path)) {
      LOG_ERR("DailyStats", "Monthly cleanup deferred: %s", path);
      return false;
    }
  }
  for (auto& c : cache)
    if (c.day >= first && c.day - first < count) c = {};
  for (auto& migrated : migratedMonths)
    if (migrated == first) migrated = UINT32_MAX;
  return true;
}
bool save(Cache& c) {
  if (!c.dirty) return true;
  if (!write(c.day, c.counter)) return false;
  c.dirty = 0;
  return true;
}
}  // namespace

#ifdef DAILY_READING_STATS_TEST
void resetForTest() {
  localDirectory.clear();
  for (auto& c : cache) c = {};
  migratedMonths[0] = migratedMonths[1] = UINT32_MAX;
}
#endif

bool read(uint32_t day, Counter& counter) {
  counter = {};
  if (!prepareDirectory()) return false;
  uint32_t first;
  uint8_t count;
  if (!monthFor(day, first, count) || !migrateMonth(first, count)) return false;
  Month month;
  if (!loadMonth(first, count, month)) return false;
  counter = month.days[day - first];
  return true;
}

bool write(uint32_t day, const Counter& counter) {
  if (!prepareDirectory()) return false;
  uint32_t first;
  uint8_t count;
  if (!monthFor(day, first, count) || !migrateMonth(first, count)) return false;
  Month month;
  if (!loadMonth(first, count, month)) return false;
  auto& previous = month.days[day - first];
  if (counter.uploaded > counter.seconds || counter.seconds < previous.seconds) {
    LOG_ERR("DailyStats", "Refusing non-monotonic daily counter");
    return false;
  }
  const Counter merged{counter.seconds, std::max(counter.uploaded, previous.uploaded)};
  if (merged.seconds != previous.seconds || merged.uploaded != previous.uploaded) {
    if (month.slot < 0) {
      // Establish an empty recovery copy first, so even the first real checkpoint
      // can be interrupted without leaving this month permanently unreadable.
      month.slot = 0;
      month.generation = UINT32_MAX;
      if (!saveMonth(first, count, month)) return false;
    }
    previous = merged;
    if (!saveMonth(first, count, month)) return false;
  }
  for (auto& c : cache)
    if (c.day == day) c.counter.uploaded = std::max(c.counter.uploaded, merged.uploaded);
  return true;
}

bool uploadPending(bool (*upload)(void*, uint32_t, uint32_t), void* context) {
  if (!prepareDirectory() || !flush()) return false;
  uint32_t after = UINT32_MAX;
  for (;;) {
    uint32_t first;
    if (!nextMonth(after, first)) return false;
    if (first == UINT32_MAX) return true;
    uint8_t count;
    uint32_t unused;
    if (!monthFor(first, unused, count)) return false;
    for (uint8_t i = 0; i < count; ++i) {
      Counter counter;
      if (!read(first + i, counter)) return false;
      if (counter.seconds == counter.uploaded) continue;
      if (!upload(context, first + i, counter.seconds)) return false;
      counter.uploaded = counter.seconds;
      if (!write(first + i, counter)) return false;
    }
    after = first;
  }
}

bool pruneSyncedMonths(const ReadingStatsDate& today) {
  if (!today.isValid()) {
    LOG_ERR("DailyStats", "Skipping cleanup without a valid local date");
    return false;
  }
  ReadingStatsDate previous{today.year, today.month, 1};
  if (previous.month == 1) {
    if (previous.year == 2000) return true;
    --previous.year;
    previous.month = 12;
  } else {
    --previous.month;
  }
  const uint32_t keepFrom = readingStatsDayIndex(previous);
  // Accepted but uncheckpointed time must participate in the retention decision.
  if (!prepareDirectory() || !flush()) return false;
  bool ok = true;
  uint32_t after = UINT32_MAX;
  for (;;) {
    uint32_t first;
    if (!nextMonth(after, first)) return false;
    if (first == UINT32_MAX || first >= keepFrom) return ok;
    uint8_t count;
    uint32_t unused;
    if (!monthFor(first, unused, count)) return false;
    if (!pruneMonth(first, count)) ok = false;
    after = first;
  }
}

bool record(uint32_t day, uint32_t seconds, bool allowCheckpoint) {
  Cache* chosen = nullptr;
  for (auto& c : cache)
    if (c.day == day) chosen = &c;
  if (!chosen) {
    chosen = cache[0].day == UINT32_MAX ? &cache[0] : &cache[1];
    if (!save(*chosen)) return false;
    Counter counter;
    if (!read(day, counter)) return false;
    *chosen = {day, counter, 0};
  }
  chosen->counter.seconds =
      UINT32_MAX - chosen->counter.seconds < seconds ? UINT32_MAX : chosen->counter.seconds + seconds;
  chosen->dirty += seconds;
  if (allowCheckpoint && chosen->dirty >= READING_STATS_CHECKPOINT_SECONDS && !save(*chosen)) {
    LOG_ERR("DailyStats", "Checkpoint deferred; accepted seconds retained in RAM");
  }
  // Success means the seconds were accepted; flush() separately reports durability.
  return true;
}

bool checkpointIfDue() {
  bool ok = true;
  for (auto& c : cache)
    if (c.dirty >= READING_STATS_CHECKPOINT_SECONDS && !save(c)) ok = false;
  return ok;
}

bool flush() {
  bool ok = true;
  for (auto& c : cache)
    if (!save(c)) ok = false;
  return ok;
}

bool secondsFor(uint32_t day, uint32_t& seconds) {
  seconds = 0;
  for (const auto& c : cache) {
    if (c.day == day) {
      seconds = c.counter.seconds;
      return true;
    }
  }
  Counter counter;
  if (!read(day, counter)) return false;
  seconds = counter.seconds;
  return true;
}

Summary summarize(uint32_t today) {
  constexpr uint32_t days = 7;
  Summary summary;
  uint64_t total = 0;
  summary.hasSevenDayAverage = today + 1 >= days;
  for (uint32_t offset = 0; offset < days && offset <= today; ++offset) {
    uint32_t seconds = 0;
    if (!secondsFor(today - offset, seconds)) {
      LOG_ERR("DailyStats", "Cannot read daily counter for day %lu", static_cast<unsigned long>(today - offset));
      summary.hasSevenDayAverage = false;
      continue;
    }
    if (offset == 0) {
      summary.hasToday = true;
      summary.todaySeconds = seconds;
    }
    total += seconds;
  }
  summary.sevenDayAverageSeconds = static_cast<uint32_t>(total / days);
  return summary;
}

bool Session::accept(const ReadingStatsDateTime& end, uint8_t utcOffset, uint32_t seconds, uint32_t sessionSeconds,
                     bool allowCheckpoint) {
  bool ok = true;
  if (sessionSeconds >= 10) {
    uint8_t retained = 0;
    for (uint8_t i = 0; i < pendingCount; ++i) {
      if (!record(pending[i].day, pending[i].seconds, allowCheckpoint)) {
        pending[retained++] = pending[i];
        ok = false;
      }
    }
    pendingCount = retained;
  }
  if (!isValidReadingStatsInterval(startTime, end, offset, utcOffset, seconds)) return ok;
  const uint64_t start = uint64_t(readingStatsDayIndex(startTime.date)) * 86400 + startTime.hour * 3600u +
                         startTime.minute * 60u + startTime.second;
  uint64_t cursor = start;
  uint32_t remaining = seconds;
  while (remaining) {
    const uint32_t segment = std::min(remaining, 86400u - uint32_t(cursor % 86400));
    const uint32_t day = cursor / 86400;
    const bool deferred = sessionSeconds < 10 || !record(day, segment, allowCheckpoint);
    if (deferred) {
      if (sessionSeconds >= 10) ok = false;
      uint8_t slot = 0;
      while (slot < pendingCount && pending[slot].day != day) ++slot;
      if (slot == pendingCount) {
        if (pendingCount >= 10) {
          LOG_ERR("DailyStats", "Pending day limit exceeded during storage failure");
          return false;
        }
        pending[pendingCount++] = {day, 0};
      }
      pending[slot].seconds =
          UINT32_MAX - pending[slot].seconds < segment ? UINT32_MAX : pending[slot].seconds + segment;
    }
    remaining -= segment;
    cursor += segment;
  }
  return ok;
}
}  // namespace DailyReadingStats
