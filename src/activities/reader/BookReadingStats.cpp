#include "BookReadingStats.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <optional>

#include "TwoSlotFile.h"

namespace {
// Binary layout v1 (11 bytes):
//   [0]     version (= 1)
//   [1-2]   sessionCount        uint16_t LE
//   [3-6]   totalReadingSeconds uint32_t LE
//   [7-10]  totalPagesTurned    uint32_t LE
//
// Binary layout v2 (12 bytes):
//   [0]     version (= 2)
//   [1-2]   sessionCount        uint16_t LE
//   [3-6]   totalReadingSeconds uint32_t LE
//   [7-10]  totalPagesTurned    uint32_t LE
//   [11]    isCompleted         uint8_t
//
// Binary layout v3 (16 bytes):
//   [0]      version (= 3)
//   [1-2]    sessionCount              uint16_t LE
//   [3-6]    totalReadingSeconds       uint32_t LE
//   [7-10]   totalPagesTurned          uint32_t LE
//   [11]     isCompleted               uint8_t
//   [12-13]  avgSecondsPerForwardPage  uint16_t LE
//   [14-15]  paceSampleCount           uint16_t LE
//
// Binary layout v4 (69 bytes):
//   [0]      version (= 4)
//   [1-2]    sessionCount              uint16_t LE
//   [3-6]    totalReadingSeconds       uint32_t LE
//   [7-10]   totalPagesTurned          uint32_t LE
//   [11]     isCompleted               uint8_t
//   [12-13]  avgSecondsPerForwardPage  uint16_t LE
//   [14-15]  paceSampleCount           uint16_t LE
//   [16]     flags bit0=startDateManual bit1=finishedDateManual
//   [17-18]  startDate.year            uint16_t LE
//   [19]     startDate.month           uint8_t
//   [20]     startDate.day             uint8_t
//   [21-22]  finishedDate.year         uint16_t LE
//   [23]     finishedDate.month        uint8_t
//   [24]     finishedDate.day          uint8_t
//   [25-40]  timeOfDaySeconds[4]       uint32_t LE each
//   [41-68]  dayOfWeekSeconds[7]       uint32_t LE each
//
// Binary layout v5 (73 bytes):
//   [0-68]   v4 fields
//   [69-72]  estimatedTimeLeftSeconds  uint32_t LE, 0 means unavailable
// Binary layout v6 (75 bytes): v5 fields plus paceFractionMillis at [73-74].
static constexpr uint8_t STATS_FILE_VERSION = 6;
static constexpr uint8_t STATS_FILE_VERSION_V5 = 5;
static constexpr uint8_t STATS_FILE_VERSION_V2 = 2;
static constexpr uint8_t STATS_FILE_VERSION_V1 = 1;
static constexpr uint8_t STATS_FILE_VERSION_V3 = 3;
static constexpr uint8_t STATS_FILE_VERSION_V4 = 4;
static constexpr int STATS_FILE_SIZE_V1 = 11;
static constexpr int STATS_FILE_SIZE_V2 = 12;
static constexpr int STATS_FILE_SIZE_V3 = 16;
static constexpr int STATS_FILE_SIZE_V4 = 69;
static constexpr int STATS_FILE_SIZE_V5 = 73;
static constexpr int STATS_FILE_SIZE = 75;
static constexpr uint16_t MAX_PACE_SAMPLE_COUNT = 1000;
static constexpr uint8_t FLAG_START_DATE_MANUAL = 1u << 0;
static constexpr uint8_t FLAG_FINISHED_DATE_MANUAL = 1u << 1;
static constexpr uint8_t OLDEST_VERSIONED_STATS_FILE_VERSION = 4;
static constexpr const char* LEGACY_STATS_FILE_NAME = "stats.bin";

std::string statsFileNameForVersion(const uint8_t version) {
  char buf[16];
  snprintf(buf, sizeof(buf), "stats_v%u.bin", version);
  return std::string(buf);
}

constexpr int statsSizeForVersion(const uint8_t version) {
  return version == STATS_FILE_VERSION      ? STATS_FILE_SIZE
         : version == STATS_FILE_VERSION_V5 ? STATS_FILE_SIZE_V5
                                            : STATS_FILE_SIZE_V4;
}

uint16_t readLe16(const uint8_t* data, const int offset) {
  return static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
}

bool validPaceFraction(const uint8_t* data) {
  const uint16_t fraction = readLe16(data, 73);
  return fraction < 1000 && (readLe16(data, 12) != UINT16_MAX || fraction == 0);
}

bool openRecoverableStatsFile(const std::string& path, FsFile& f, const uint8_t version = STATS_FILE_VERSION) {
  const std::string selected = two_slot::preferredPath("STATS", path, statsSizeForVersion(version), true);
  if (Storage.openFileForRead("STATS", selected, f)) return true;

  for (const char* suffix : {".tmp", ".bak"}) {
    const std::string recoveryPath = path + suffix;
    if (!Storage.exists(recoveryPath.c_str())) continue;

    // A temp file is newer than the backup, but it is safe to prefer only
    // after the complete current payload reached storage. A partial temp falls
    // through to the older, known-good backup.
    if (strcmp(suffix, ".tmp") == 0) {
      FsFile candidate;
      if (!Storage.openFileForRead("STATS", recoveryPath, candidate)) continue;
      uint8_t data[STATS_FILE_SIZE] = {};
      const size_t fileSize = candidate.fileSize();
      const int n = candidate.read(data, STATS_FILE_SIZE);
      candidate.close();
      if (fileSize != static_cast<size_t>(statsSizeForVersion(version)) || n != statsSizeForVersion(version) ||
          data[0] != version || (version == STATS_FILE_VERSION && !validPaceFraction(data)))
        continue;
    }

    if (Storage.rename(recoveryPath.c_str(), path.c_str())) {
      LOG_INF("STATS", "Recovered interrupted stats save: %s", path.c_str());
      const std::string selected = two_slot::preferredPath("STATS", path, statsSizeForVersion(version), true);
      if (Storage.openFileForRead("STATS", selected, f)) return true;
    }

    if (Storage.openFileForRead("STATS", recoveryPath, f)) {
      LOG_ERR("STATS", "Could not restore %s; reading recovery file directly", path.c_str());
      return true;
    }
  }
  return false;
}

bool openStatsFileForRead(const std::string& cachePath, FsFile& f) {
  const std::string currentName = statsFileNameForVersion(STATS_FILE_VERSION);
  if (openRecoverableStatsFile(cachePath + "/" + currentName, f)) {
    return true;
  }

  // Keep all supported versioned migration sources, including v4 after the v6 bump.
  for (uint8_t version = STATS_FILE_VERSION - 1; version >= OLDEST_VERSIONED_STATS_FILE_VERSION; --version) {
    const std::string previousName = statsFileNameForVersion(version);
    if (openRecoverableStatsFile(cachePath + "/" + previousName, f, version)) {
      LOG_DBG("STATS", "Migrating %s to %s", previousName.c_str(), currentName.c_str());
      return true;
    }
  }

  if (Storage.openFileForRead("STATS", cachePath + "/" + LEGACY_STATS_FILE_NAME, f)) {
    LOG_DBG("STATS", "Migrating legacy %s to %s", LEGACY_STATS_FILE_NAME, currentName.c_str());
    return true;
  }

  return false;
}

uint32_t readLe32(const uint8_t* data, const int offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void readCommonStats(const uint8_t* data, BookReadingStats& stats) {
  stats.sessionCount = readLe16(data, 1);
  stats.totalReadingSeconds = readLe32(data, 3);
  stats.totalPagesTurned = readLe32(data, 7);
}

void writeLe16(uint8_t* data, const int offset, const uint16_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
}

void writeLe32(uint8_t* data, const int offset, const uint32_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
  data[offset + 2] = (value >> 16) & 0xFF;
  data[offset + 3] = (value >> 24) & 0xFF;
}

ReadingStatsDate readDate(const uint8_t* data, const int offset) {
  ReadingStatsDate date;
  date.year = readLe16(data, offset);
  date.month = data[offset + 2];
  date.day = data[offset + 3];
  if (!date.isValid()) {
    date.clear();
  }
  return date;
}

}  // namespace

bool BookReadingStats::peekCompleted(const std::string& cachePath) {
  // Inspect in place, including recovery snapshots from older firmware.
  const auto inspect = [](const std::string& path, const uint8_t version, const bool temp) -> std::optional<bool> {
    if (!Storage.exists(path.c_str())) return std::nullopt;
    FsFile f;
    if (!Storage.openFileForRead("STATS", path, f)) return false;
    uint8_t data[STATS_FILE_SIZE] = {};
    const size_t fileSize = two_slot::payloadLength(f, statsSizeForVersion(version));
    const int n = f.read(data, STATS_FILE_SIZE);
    f.close();
    const bool complete = n >= 0 && static_cast<size_t>(n) == fileSize;
    const bool known = complete && ((n == STATS_FILE_SIZE_V2 && data[0] == STATS_FILE_VERSION_V2) ||
                                    (n == STATS_FILE_SIZE_V3 && data[0] == STATS_FILE_VERSION_V3) ||
                                    (n == STATS_FILE_SIZE_V4 && data[0] == STATS_FILE_VERSION_V4) ||
                                    (n == STATS_FILE_SIZE_V5 && data[0] == STATS_FILE_VERSION_V5) ||
                                    (n == STATS_FILE_SIZE && data[0] == STATS_FILE_VERSION && validPaceFraction(data)));
    if (temp && !(known && n == statsSizeForVersion(version) && data[0] == version)) return std::nullopt;
    return known && data[11] != 0;
  };
  for (uint8_t version = STATS_FILE_VERSION; version >= OLDEST_VERSIONED_STATS_FILE_VERSION; --version) {
    const std::string path = cachePath + "/" + statsFileNameForVersion(version);
    const std::string selected = two_slot::preferredPath("STATS", path, statsSizeForVersion(version), true);
    for (const char* suffix : {"", ".tmp", ".bak"}) {
      const auto completed = inspect(suffix[0] == 0 ? selected : path + suffix, version, strcmp(suffix, ".tmp") == 0);
      if (completed.has_value()) return *completed;
    }
  }
  return inspect(cachePath + "/" + LEGACY_STATS_FILE_NAME, 0, false).value_or(false);
}

BookReadingStats BookReadingStats::load(const std::string& cachePath) {
  BookReadingStats stats;
  loadForUpload(cachePath, stats);
  return stats;
}

bool BookReadingStats::loadForUpload(const std::string& cachePath, BookReadingStats& stats) {
  stats = {};
  FsFile f;
  if (!openStatsFileForRead(cachePath, f)) {
    return false;
  }
  uint8_t data[STATS_FILE_SIZE] = {};
  const size_t fileSize = two_slot::payloadLength(f, STATS_FILE_SIZE);
  const int n = f.read(data, STATS_FILE_SIZE);
  f.close();
  if (n < 0 || static_cast<size_t>(n) != fileSize) {
    LOG_ERR("STATS", "Incomplete or oversized stats snapshot");
    return false;
  }

  if (n == STATS_FILE_SIZE_V1 && data[0] == STATS_FILE_VERSION_V1) {
    readCommonStats(data, stats);
    return true;
  }

  if (n == STATS_FILE_SIZE_V2 && data[0] == STATS_FILE_VERSION_V2) {
    readCommonStats(data, stats);
    stats.isCompleted = data[11] != 0;
    return true;
  }

  if (n == STATS_FILE_SIZE_V3 && data[0] == STATS_FILE_VERSION_V3) {
    readCommonStats(data, stats);
    stats.isCompleted = data[11] != 0;
    stats.avgSecondsPerForwardPage = readLe16(data, 12);
    stats.paceSampleCount = readLe16(data, 14);
    return true;
  }

  if (n != STATS_FILE_SIZE && n != STATS_FILE_SIZE_V5 && n != STATS_FILE_SIZE_V4) {
    LOG_DBG("STATS", "Stats missing or version mismatch, starting fresh");
    return false;
  }
  if (n == STATS_FILE_SIZE_V4 && data[0] != STATS_FILE_VERSION_V4) {
    LOG_DBG("STATS", "Stats missing or version mismatch, starting fresh");
    return false;
  }
  if (n == STATS_FILE_SIZE_V5 && data[0] != STATS_FILE_VERSION_V5) {
    LOG_DBG("STATS", "Stats missing or version mismatch, starting fresh");
    return false;
  }
  if (n == STATS_FILE_SIZE && data[0] != STATS_FILE_VERSION) {
    LOG_DBG("STATS", "Stats missing or version mismatch, starting fresh");
    return false;
  }
  readCommonStats(data, stats);
  stats.isCompleted = data[11] != 0;
  stats.avgSecondsPerForwardPage = readLe16(data, 12);
  stats.paceSampleCount = readLe16(data, 14);
  const uint8_t flags = data[16];
  stats.startDateManual = (flags & FLAG_START_DATE_MANUAL) != 0;
  stats.finishedDateManual = (flags & FLAG_FINISHED_DATE_MANUAL) != 0;
  stats.startDate = readDate(data, 17);
  stats.finishedDate = readDate(data, 21);
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) {
    stats.timeOfDaySeconds[i] = readLe32(data, 25 + static_cast<int>(i) * 4);
  }
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) {
    stats.dayOfWeekSeconds[i] = readLe32(data, 41 + static_cast<int>(i) * 4);
  }
  if (n >= STATS_FILE_SIZE_V5) {
    stats.estimatedTimeLeftSeconds = readLe32(data, 69);
  }
  if (n == STATS_FILE_SIZE) {
    const uint16_t fraction = readLe16(data, 73);
    if (!validPaceFraction(data)) {
      LOG_ERR("STATS", "Invalid fractional reading pace");
      stats = {};
      return false;
    }
    stats.paceFractionMillis = fraction;
  }
  return true;
}

uint32_t BookReadingStats::averageForwardPageMillis() const {
  return static_cast<uint32_t>(avgSecondsPerForwardPage) * 1000U + paceFractionMillis;
}

void BookReadingStats::setAverageForwardPageMillis(const uint32_t millis) {
  const uint32_t bounded = std::min<uint32_t>(millis, static_cast<uint32_t>(UINT16_MAX) * 1000U);
  avgSecondsPerForwardPage = bounded / 1000U;
  paceFractionMillis = bounded % 1000U;
}

void BookReadingStats::recordForwardPageRead(uint32_t seconds) {
  if (seconds == 0) {
    return;
  }
  if (seconds > UINT16_MAX) {
    seconds = UINT16_MAX;
  }

  const uint32_t sampleMillis = seconds * 1000U;
  if (paceSampleCount == 0 || averageForwardPageMillis() == 0) {
    setAverageForwardPageMillis(sampleMillis);
    paceSampleCount = 1;
    return;
  }

  const uint16_t weight = std::min(paceSampleCount, MAX_PACE_SAMPLE_COUNT);
  // Keep the fractional part across updates and reopen; round only at millisecond precision.
  const uint64_t total = static_cast<uint64_t>(averageForwardPageMillis()) * weight + sampleMillis;
  setAverageForwardPageMillis(static_cast<uint32_t>((total + (weight + 1U) / 2U) / (weight + 1U)));
  if (paceSampleCount < MAX_PACE_SAMPLE_COUNT) ++paceSampleCount;
}

void BookReadingStats::recordReadingSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  recordReadingSpanIntoBuckets(timeOfDaySeconds, dayOfWeekSeconds, localStart, seconds);
}

void BookReadingStats::formatDuration(uint32_t seconds, char* buf, size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  if (hours == 0) {
    snprintf(buf, len, "%lu min", static_cast<unsigned long>(minutes));
  } else {
    snprintf(buf, len, "%luh %lu min", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  }
}

bool BookReadingStats::save(const std::string& cachePath) const {
  const std::string statsFileName = statsFileNameForVersion(STATS_FILE_VERSION);
  const std::string statsPath = cachePath + "/" + statsFileName;
  const std::string backupPath = statsPath + ".bak";

  uint8_t data[STATS_FILE_SIZE];
  memset(data, 0, sizeof(data));
  data[0] = STATS_FILE_VERSION;
  writeLe16(data, 1, sessionCount);
  writeLe32(data, 3, totalReadingSeconds);
  writeLe32(data, 7, totalPagesTurned);
  data[11] = isCompleted ? 1 : 0;
  writeLe16(data, 12, avgSecondsPerForwardPage);
  writeLe16(data, 14, paceSampleCount);
  data[16] = (startDateManual ? FLAG_START_DATE_MANUAL : 0u) | (finishedDateManual ? FLAG_FINISHED_DATE_MANUAL : 0u);
  writeLe16(data, 17, startDate.isValid() ? startDate.year : 0);
  data[19] = startDate.isValid() ? startDate.month : 0;
  data[20] = startDate.isValid() ? startDate.day : 0;
  writeLe16(data, 21, finishedDate.isValid() ? finishedDate.year : 0);
  data[23] = finishedDate.isValid() ? finishedDate.month : 0;
  data[24] = finishedDate.isValid() ? finishedDate.day : 0;
  for (size_t i = 0; i < timeOfDaySeconds.size(); ++i) {
    writeLe32(data, 25 + static_cast<int>(i) * 4, timeOfDaySeconds[i]);
  }
  for (size_t i = 0; i < dayOfWeekSeconds.size(); ++i) {
    writeLe32(data, 41 + static_cast<int>(i) * 4, dayOfWeekSeconds[i]);
  }
  writeLe32(data, 69, estimatedTimeLeftSeconds);
  writeLe16(data, 73, paceFractionMillis);

  return two_slot::write("STATS", statsPath.c_str(), backupPath.c_str(), data, sizeof(data),
                         [](const uint8_t* bytes, size_t size) {
                           return size == STATS_FILE_SIZE && bytes[0] == STATS_FILE_VERSION && validPaceFraction(bytes);
                         });
}

bool BookReadingStats::remove(const std::string& cachePath) {
  const std::string statsFileName = statsFileNameForVersion(STATS_FILE_VERSION);
  const std::string statsPath = cachePath + "/" + statsFileName;
  const std::string legacyStatsPath = cachePath + "/" + LEGACY_STATS_FILE_NAME;
  bool ok = true;

  // Remove fallback sources from oldest to newest. Keeping the canonical file
  // until every fallback is gone prevents an interrupted reset from reviving
  // older stats on the next load.
  if (Storage.exists(legacyStatsPath.c_str()) && !Storage.remove(legacyStatsPath.c_str())) {
    LOG_ERR("STATS", "Could not delete %s", LEGACY_STATS_FILE_NAME);
    ok = false;
  }
  for (uint8_t version = OLDEST_VERSIONED_STATS_FILE_VERSION; version < STATS_FILE_VERSION; ++version) {
    const std::string previousPath = cachePath + "/" + statsFileNameForVersion(version);
    // A previous firmware may have left a recoverable save behind.
    for (const char* suffix : {".tmp", ".bak", ""}) {
      const std::string path = previousPath + suffix;
      if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
        LOG_ERR("STATS", "Could not delete %s", path.c_str());
        ok = false;
      }
    }
  }
  const std::string newest = two_slot::preferredPath("STATS", statsPath, STATS_FILE_SIZE, true);
  const std::string older = newest == statsPath ? statsPath + ".bak" : statsPath;
  for (const auto& path : {statsPath + ".tmp", older}) {
    if (Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) {
      LOG_ERR("STATS", "Could not delete %s", path.c_str());
      ok = false;
    }
  }
  // Keep the newest valid slot until all fallbacks are gone.
  if (ok && Storage.exists(newest.c_str()) && !Storage.remove(newest.c_str())) {
    LOG_ERR("STATS", "Could not delete %s", newest.c_str());
    ok = false;
  }
  return ok;
}
