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
}  // namespace

GlobalReadingStats GlobalReadingStats::load() {
  GlobalReadingStats stats;
  FsFile f;
  if (!Storage.openFileForRead("GSTATS", GLOBAL_STATS_PATH, f)) {
    return stats;
  }
  uint8_t data[GLOBAL_STATS_FILE_SIZE] = {};
  const int n = f.read(data, GLOBAL_STATS_FILE_SIZE);
  f.close();

  if (n == GLOBAL_STATS_FILE_SIZE_V1 && data[0] == GLOBAL_STATS_VERSION_V1) {
    stats.totalSessions = static_cast<uint32_t>(data[1]) | (static_cast<uint32_t>(data[2]) << 8) |
                          (static_cast<uint32_t>(data[3]) << 16) | (static_cast<uint32_t>(data[4]) << 24);
    stats.totalReadingSeconds = static_cast<uint32_t>(data[5]) | (static_cast<uint32_t>(data[6]) << 8) |
                                (static_cast<uint32_t>(data[7]) << 16) | (static_cast<uint32_t>(data[8]) << 24);
    stats.totalPagesTurned = static_cast<uint32_t>(data[9]) | (static_cast<uint32_t>(data[10]) << 8) |
                             (static_cast<uint32_t>(data[11]) << 16) | (static_cast<uint32_t>(data[12]) << 24);
    return stats;
  }

  if (n != GLOBAL_STATS_FILE_SIZE || data[0] != GLOBAL_STATS_VERSION) {
    LOG_DBG("GSTATS", "Global stats missing or version mismatch, starting fresh");
    return stats;
  }
  stats.totalSessions = static_cast<uint32_t>(data[1]) | (static_cast<uint32_t>(data[2]) << 8) |
                        (static_cast<uint32_t>(data[3]) << 16) | (static_cast<uint32_t>(data[4]) << 24);
  stats.totalReadingSeconds = static_cast<uint32_t>(data[5]) | (static_cast<uint32_t>(data[6]) << 8) |
                              (static_cast<uint32_t>(data[7]) << 16) | (static_cast<uint32_t>(data[8]) << 24);
  stats.totalPagesTurned = static_cast<uint32_t>(data[9]) | (static_cast<uint32_t>(data[10]) << 8) |
                           (static_cast<uint32_t>(data[11]) << 16) | (static_cast<uint32_t>(data[12]) << 24);
  stats.completedBooks = static_cast<uint32_t>(data[13]) | (static_cast<uint32_t>(data[14]) << 8) |
                         (static_cast<uint32_t>(data[15]) << 16) | (static_cast<uint32_t>(data[16]) << 24);
  return stats;
}

void GlobalReadingStats::save() const {
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
