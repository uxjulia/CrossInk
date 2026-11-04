#include "StatsBackup.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "GlobalReadingStats.h"
#include "ReadingStatsUtils.h"

namespace {
constexpr char LOG_TAG[] = "SBACK";
constexpr char BACKUP_DIR[] = "/.crossink-stats-backup";
constexpr int DEFAULT_BACKUP_KEEP_COUNT = 7;

struct BackupName {
  char value[64] = {};
};

bool isStatsBackupFileName(const char* name) {
  if (!name || strncmp(name, "stats_", 6) != 0) return false;
  const size_t len = strlen(name);
  return len > 10 && strcmp(name + len - 4, ".bin") == 0;
}

bool copyString(const char* src, char* dst, const size_t dstLen) {
  if (!dst || dstLen == 0) return false;
  const int written = snprintf(dst, dstLen, "%s", src ? src : "");
  return written > 0 && static_cast<size_t>(written) < dstLen;
}

bool buildDatedBackupName(const ReadingStatsDateTime& dt, const bool manual, char* out, const size_t outLen) {
  int written = 0;
  if (manual) {
    written = snprintf(out, outLen, "stats_%04u-%02u-%02u_%02u%02u.bin", static_cast<unsigned>(dt.date.year),
                       static_cast<unsigned>(dt.date.month), static_cast<unsigned>(dt.date.day),
                       static_cast<unsigned>(dt.hour), static_cast<unsigned>(dt.minute));
  } else {
    written = snprintf(out, outLen, "stats_%04u-%02u-%02u.bin", static_cast<unsigned>(dt.date.year),
                       static_cast<unsigned>(dt.date.month), static_cast<unsigned>(dt.date.day));
  }
  return written > 0 && static_cast<size_t>(written) < outLen;
}

bool parseIncrementingIndex(const char* name, uint32_t& outIndex) {
  constexpr char prefix[] = "stats_backup_";
  constexpr size_t prefixLen = sizeof(prefix) - 1;
  if (!name || strncmp(name, prefix, prefixLen) != 0) return false;
  const char* digits = name + prefixLen;
  const char* suffix = strstr(digits, ".bin");
  if (!suffix || suffix == digits || suffix[4] != '\0') return false;

  uint32_t value = 0;
  for (const char* p = digits; p < suffix; ++p) {
    if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
    value = value * 10u + static_cast<uint32_t>(*p - '0');
  }
  if (value == 0) return false;
  outIndex = value;
  return true;
}

bool nextIncrementingBackupName(char* out, const size_t outLen) {
  FsFile dir = Storage.open(BACKUP_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    const int written = snprintf(out, outLen, "stats_backup_%03u.bin", 1u);
    return written > 0 && static_cast<size_t>(written) < outLen;
  }

  char name[128];
  uint32_t maxIndex = 0;
  for (FsFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDirectory = file.isDirectory();
    const size_t nameLen = file.getName(name, sizeof(name));
    file.close();
    if (isDirectory || nameLen == 0) continue;

    uint32_t index = 0;
    if (parseIncrementingIndex(name, index) && index > maxIndex) {
      maxIndex = index;
    }
  }
  dir.close();

  const int written = snprintf(out, outLen, "stats_backup_%03u.bin", static_cast<unsigned>(maxIndex + 1));
  return written > 0 && static_cast<size_t>(written) < outLen;
}

bool chooseBackupName(const bool manual, char* out, const size_t outLen) {
  ReadingStatsDateTime now;
  if (getCurrentLocalReadingStatsDateTime(now)) {
    return buildDatedBackupName(now, manual, out, outLen);
  }
  return nextIncrementingBackupName(out, outLen);
}

bool readStatsFile(std::array<uint8_t, GlobalReadingStats::CURRENT_FILE_SIZE>& buffer, size_t& outSize) {
  outSize = 0;
  if (!GlobalReadingStats::exportLocal(buffer.data(), buffer.size())) return false;
  outSize = GlobalReadingStats::CURRENT_FILE_SIZE;
  return true;
}

bool writeBackupFile(const char* path, const uint8_t* data, const size_t size) {
  const std::string tmpPath = std::string(path) + ".tmp";
  if (Storage.exists(tmpPath.c_str()) && !Storage.remove(tmpPath.c_str())) {
    LOG_ERR(LOG_TAG, "Could not remove stale backup temp file: %s", tmpPath.c_str());
    return false;
  }

  FsFile file;
  if (!Storage.openFileForWrite(LOG_TAG, tmpPath.c_str(), file)) {
    LOG_ERR(LOG_TAG, "Could not open backup temp file: %s", tmpPath.c_str());
    return false;
  }

  const size_t written = file.write(data, size);
  if (written != size) {
    LOG_ERR(LOG_TAG, "Short write for backup temp file %s: %u/%u bytes", tmpPath.c_str(),
            static_cast<unsigned>(written), static_cast<unsigned>(size));
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  file.flush();
  if (!file.sync()) {
    LOG_ERR(LOG_TAG, "Failed to sync backup temp file: %s", tmpPath.c_str());
    file.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (!file.close()) {
    LOG_ERR(LOG_TAG, "Failed to close backup temp file: %s", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (Storage.exists(path) && !Storage.remove(path)) {
    LOG_ERR(LOG_TAG, "Could not replace backup file: %s", path);
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (!Storage.rename(tmpPath.c_str(), path)) {
    LOG_ERR(LOG_TAG, "Could not publish backup file: %s", path);
    Storage.remove(tmpPath.c_str());
    return false;
  }

  return true;
}

// Identical automatic-backup suppression adapted from Sichroteph/YACP commit
// 20af8aee8d3e1d560456753b08d1f52e5488621f (MIT). The fixed 64-byte chunk
// keeps the comparison well below the C3's local-stack budget.
bool backupFileMatches(const char* path, const uint8_t* expected, const size_t expectedSize) {
  FsFile file;
  if (!Storage.openFileForRead(LOG_TAG, path, file) || file.fileSize() != expectedSize) {
    if (file) file.close();
    return false;
  }

  uint8_t chunk[64];
  size_t offset = 0;
  while (offset < expectedSize) {
    const size_t requested = std::min(sizeof(chunk), expectedSize - offset);
    const int read = file.read(chunk, requested);
    if (read != static_cast<int>(requested) || memcmp(chunk, expected + offset, requested) != 0) {
      file.close();
      return false;
    }
    offset += requested;
  }
  file.close();
  return true;
}
}  // namespace

bool backupGlobalStats(const bool manual, char* outFileName, const size_t outFileNameLen) {
  if (!Storage.ensureDirectoryExists(BACKUP_DIR)) {
    LOG_ERR(LOG_TAG, "Could not create stats backup directory: %s", BACKUP_DIR);
    return false;
  }

  char fileName[64];
  if (!chooseBackupName(manual, fileName, sizeof(fileName))) {
    LOG_ERR(LOG_TAG, "Could not choose stats backup filename");
    return false;
  }

  std::array<uint8_t, GlobalReadingStats::CURRENT_FILE_SIZE> data{};
  size_t dataSize = 0;
  if (!readStatsFile(data, dataSize)) return false;

  char backupPath[128];
  const int pathWritten = snprintf(backupPath, sizeof(backupPath), "%s/%s", BACKUP_DIR, fileName);
  if (pathWritten <= 0 || static_cast<size_t>(pathWritten) >= sizeof(backupPath)) {
    LOG_ERR(LOG_TAG, "Could not build backup path");
    return false;
  }

  if (!manual && Storage.exists(backupPath) && backupFileMatches(backupPath, data.data(), dataSize)) {
    if (outFileName != nullptr && outFileNameLen > 0) {
      copyString(fileName, outFileName, outFileNameLen);
    }
    LOG_DBG(LOG_TAG, "Automatic stats backup already current: %s", backupPath);
    return true;
  }

  if (!writeBackupFile(backupPath, data.data(), dataSize)) return false;
  pruneBackups(DEFAULT_BACKUP_KEEP_COUNT);

  if (outFileName != nullptr && outFileNameLen > 0) {
    copyString(fileName, outFileName, outFileNameLen);
  }
  LOG_DBG(LOG_TAG, "Wrote stats backup: %s", backupPath);
  return true;
}

int pruneBackups(int keep) {
  if (keep < 0) return 0;

  FsFile dir = Storage.open(BACKUP_DIR);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }

  char name[128];
  std::vector<BackupName> names;
  names.reserve(16);
  for (FsFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDirectory = file.isDirectory();
    const size_t nameLen = file.getName(name, sizeof(name));
    file.close();
    if (isDirectory || nameLen == 0 || !isStatsBackupFileName(name)) continue;

    BackupName backupName;
    if (copyString(name, backupName.value, sizeof(backupName.value))) {
      names.push_back(backupName);
    }
  }
  dir.close();

  // Daily, dated manual, and clockless backups have independent retention:
  // dates and sequence numbers cannot establish a shared oldest-first order.
  // Partition in place so the C3 needs no second list allocation.
  keep = std::max(keep, 1);
  const auto dailyEnd = std::partition(names.begin(), names.end(), [](const BackupName& name) {
    return strlen(name.value) == 20 && name.value[10] == '-' && name.value[13] == '-';
  });
  const auto manualEnd = std::partition(
      dailyEnd, names.end(), [](const BackupName& name) { return strncmp(name.value, "stats_backup_", 13) != 0; });
  int removed = 0;
  const auto pruneSet = [&](const auto begin, const auto end) {
    if (end - begin <= keep) return;
    std::sort(begin, end, [](const BackupName& lhs, const BackupName& rhs) {
      // Numbered backups cross the three-digit boundary at 1000.
      const size_t leftLength = strlen(lhs.value), rightLength = strlen(rhs.value);
      return leftLength != rightLength ? leftLength < rightLength : strcmp(lhs.value, rhs.value) < 0;
    });
    for (auto it = begin; it != end - keep; ++it) {
      char path[128];
      const int written = snprintf(path, sizeof(path), "%s/%s", BACKUP_DIR, it->value);
      if (written <= 0 || static_cast<size_t>(written) >= sizeof(path)) continue;
      if (Storage.remove(path)) {
        ++removed;
      } else {
        LOG_ERR(LOG_TAG, "Failed to prune stats backup: %s", path);
      }
    }
  };
  pruneSet(names.begin(), dailyEnd);
  pruneSet(dailyEnd, manualEnd);
  pruneSet(manualEnd, names.end());

  if (removed > 0) {
    LOG_DBG(LOG_TAG, "Pruned %d old stats backup(s)", removed);
  }
  return removed;
}
