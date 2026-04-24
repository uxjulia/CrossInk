#include "GlobalReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>

namespace {
// Binary layout v1 (13 bytes):
//   [0]     version (= 1)
//   [1-4]   totalSessions       uint32_t LE
//   [5-8]   totalReadingSeconds uint32_t LE
//   [9-12]  totalPagesTurned    uint32_t LE
//
// Binary layout v2 (17 bytes):
//   [0]      version (= 2)
//   [1-4]    totalSessions       uint32_t LE
//   [5-8]    totalReadingSeconds uint32_t LE
//   [9-12]   totalPagesTurned    uint32_t LE
//   [13-16]  completedBooks      uint32_t LE
static constexpr uint8_t GLOBAL_STATS_VERSION = 2;
static constexpr uint8_t GLOBAL_STATS_VERSION_V1 = 1;
static constexpr int GLOBAL_STATS_FILE_SIZE_V1 = 13;
static constexpr int GLOBAL_STATS_FILE_SIZE = 17;
static constexpr char GLOBAL_STATS_PATH[] = "/.crosspoint/global_stats.bin";
static constexpr char GLOBAL_STATS_BAK_PATH[] = "/.crosspoint/global_stats.bin.bak";

uint32_t readLe32(const uint8_t* data, const int offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void loadCommonFields(const uint8_t* data, GlobalReadingStats& out) {
  out.totalSessions = readLe32(data, 1);
  out.totalReadingSeconds = readLe32(data, 5);
  out.totalPagesTurned = readLe32(data, 9);
}
}  // namespace

static bool loadFromFile(const char* path, GlobalReadingStats& out) {
  FsFile f;
  if (!Storage.openFileForRead("GSTATS", path, f)) return false;
  uint8_t data[GLOBAL_STATS_FILE_SIZE] = {};
  const int n = f.read(data, GLOBAL_STATS_FILE_SIZE);
  f.close();

  if (n == GLOBAL_STATS_FILE_SIZE_V1 && data[0] == GLOBAL_STATS_VERSION_V1) {
    loadCommonFields(data, out);
    out.completedBooks = 0;
    return true;
  }

  if (n != GLOBAL_STATS_FILE_SIZE || data[0] != GLOBAL_STATS_VERSION) return false;
  loadCommonFields(data, out);
  out.completedBooks = readLe32(data, 13);
  return true;
}

GlobalReadingStats GlobalReadingStats::load() {
  GlobalReadingStats stats;
  if (loadFromFile(GLOBAL_STATS_PATH, stats)) return stats;
  if (loadFromFile(GLOBAL_STATS_BAK_PATH, stats)) {
    LOG_DBG("GSTATS", "Recovered global stats from backup");
    return stats;
  }
  LOG_DBG("GSTATS", "Global stats missing or corrupt, starting fresh");
  return stats;
}

void GlobalReadingStats::save() const {
  // Preserve previous file as .bak before truncating — openFileForWrite uses
  // O_TRUNC, so a power failure mid-write would corrupt the primary file
  // without this fallback.
  if (Storage.exists(GLOBAL_STATS_PATH)) {
    Storage.remove(GLOBAL_STATS_BAK_PATH);
    Storage.rename(GLOBAL_STATS_PATH, GLOBAL_STATS_BAK_PATH);
  }

  FsFile f;
  if (!Storage.openFileForWrite("GSTATS", GLOBAL_STATS_PATH, f)) {
    LOG_ERR("GSTATS", "Could not write global_stats.bin");
    return;
  }
  uint8_t data[GLOBAL_STATS_FILE_SIZE];
  data[0] = GLOBAL_STATS_VERSION;
  data[1] = totalSessions & 0xFF;
  data[2] = (totalSessions >> 8) & 0xFF;
  data[3] = (totalSessions >> 16) & 0xFF;
  data[4] = (totalSessions >> 24) & 0xFF;
  data[5] = totalReadingSeconds & 0xFF;
  data[6] = (totalReadingSeconds >> 8) & 0xFF;
  data[7] = (totalReadingSeconds >> 16) & 0xFF;
  data[8] = (totalReadingSeconds >> 24) & 0xFF;
  data[9] = totalPagesTurned & 0xFF;
  data[10] = (totalPagesTurned >> 8) & 0xFF;
  data[11] = (totalPagesTurned >> 16) & 0xFF;
  data[12] = (totalPagesTurned >> 24) & 0xFF;
  data[13] = completedBooks & 0xFF;
  data[14] = (completedBooks >> 8) & 0xFF;
  data[15] = (completedBooks >> 16) & 0xFF;
  data[16] = (completedBooks >> 24) & 0xFF;
  f.write(data, GLOBAL_STATS_FILE_SIZE);
  f.close();
}
