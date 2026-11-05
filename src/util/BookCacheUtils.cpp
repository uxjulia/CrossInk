#include "BookCacheUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <string_view>
#include <vector>

#include "sync/BookSyncFile.h"

namespace {

struct PreservedCacheFile {
  const char* name;
  const char* tmpName;
};

constexpr PreservedCacheFile EPUB_USER_STATE_FILES[] = {
    {"progress.bin", "upload_preserve_progress.bin"},
    {"progress.bin.bak", "upload_preserve_progress.bin.bak"},
    {"reader_settings.bin", "upload_preserve_reader_settings.bin"},
    {"dictionary_history.txt", "upload_preserve_dictionary_history.txt"},
    {"reading_stats_off", "upload_preserve_reading_stats_off"},
    {"sync.bin", "upload_preserve_sync.bin"},
    {"sync.bin.bak", "upload_preserve_sync.bin.bak"},
    {"sync.bin.tmp", "upload_preserve_sync.bin.tmp"},
};

constexpr PreservedCacheFile PAGE_PROGRESS_FILES[] = {
    {"progress.bin", "upload_preserve_progress.bin"}, {"reading_stats_off", "upload_preserve_reading_stats_off"},
    {"sync.bin", "upload_preserve_sync.bin"},         {"sync.bin.bak", "upload_preserve_sync.bin.bak"},
    {"sync.bin.tmp", "upload_preserve_sync.bin.tmp"},
};

constexpr PreservedCacheFile CACHE_CLEAR_USER_STATE_FILES[] = {
    {"dictionary_history.txt", "clear_preserve_dictionary_history.txt"},
    {"reading_stats_off", "clear_preserve_reading_stats_off"},
    {"sync.bin", "clear_preserve_sync.bin"},
    {"sync.bin.bak", "clear_preserve_sync.bin.bak"},
    {"sync.bin.tmp", "clear_preserve_sync.bin.tmp"},
};

struct ResolvedPreservedCacheFile {
  std::string name;
  std::string tmpName;
};

struct StatsFileCandidate {
  std::string name;
  int version = -1;
};

constexpr size_t MAX_PRESERVED_CACHE_FILES = std::size(EPUB_USER_STATE_FILES) > std::size(PAGE_PROGRESS_FILES)
                                                 ? std::size(EPUB_USER_STATE_FILES)
                                                 : std::size(PAGE_PROGRESS_FILES);
// Keep both save slots and legacy interrupted saves for up to eight versions.
constexpr size_t MAX_STATS_FILES_TO_PRESERVE = 8 * 3;
constexpr char STATS_PREFIX[] = "stats";
constexpr char STATS_SUFFIX[] = ".bin";

std::string getBookCachePath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return Epub(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasTxtExtension(path)) {
    return Txt(path, "/.crosspoint").getCachePath();
  }
  return "";
}

const PreservedCacheFile* preservedFilesForPath(const std::string& path, size_t& count) {
  if (FsHelpers::hasEpubExtension(path)) {
    count = std::size(EPUB_USER_STATE_FILES);
    return EPUB_USER_STATE_FILES;
  }
  if (FsHelpers::hasXtcExtension(path) || FsHelpers::hasTxtExtension(path)) {
    count = std::size(PAGE_PROGRESS_FILES);
    return PAGE_PROGRESS_FILES;
  }
  count = 0;
  return nullptr;
}

std::string_view statsPayloadName(const char* name) {
  std::string_view value = name ? name : "";
  if (value.ends_with(".bak") || value.ends_with(".tmp")) value.remove_suffix(4);
  return value;
}

bool isStatsFileName(const char* name) {
  const auto payload = statsPayloadName(name);
  return payload.size() >= sizeof(STATS_PREFIX) + sizeof(STATS_SUFFIX) - 2 && payload.starts_with(STATS_PREFIX) &&
         payload.ends_with(STATS_SUFFIX);
}

int statsFileVersion(const char* name) {
  const auto payload = statsPayloadName(name);
  if (payload == "stats.bin") return 0;
  constexpr std::string_view prefix = "stats_v";
  constexpr size_t suffixLength = sizeof(STATS_SUFFIX) - 1;
  if (!payload.starts_with(prefix) || !payload.ends_with(STATS_SUFFIX) ||
      payload.size() <= prefix.size() + suffixLength)
    return -1;
  int version = 0;
  for (size_t i = prefix.size(); i < payload.size() - suffixLength; ++i) {
    // Device-qualified names keep the version before the origin suffix.
    if (payload[i] == '_') break;
    if (payload[i] < '0' || payload[i] > '9') return -1;
    const int digit = payload[i] - '0';
    if (version > (std::numeric_limits<int>::max() - digit) / 10) return -1;
    version = version * 10 + digit;
  }
  return version;
}

void appendFixedPreservedFiles(std::vector<ResolvedPreservedCacheFile>& files, const PreservedCacheFile* fixedFiles,
                               const size_t fixedCount) {
  for (size_t i = 0; i < fixedCount; ++i) {
    files.push_back({fixedFiles[i].name, fixedFiles[i].tmpName});
  }
}

bool appendStatsPreservedFiles(const std::string& cachePath, std::vector<ResolvedPreservedCacheFile>& files,
                               const char* tmpPrefix) {
  if (!tmpPrefix) {
    LOG_ERR("BookCache", "Missing stats preservation temp prefix: %s", cachePath.c_str());
    return false;
  }

  FsFile dir = Storage.open(cachePath.c_str());
  if (!dir) {
    if (Storage.exists(cachePath.c_str())) {
      LOG_ERR("BookCache", "Failed to open cache directory for stats preservation: %s", cachePath.c_str());
      return false;
    }
    return true;
  }
  if (!dir.isDirectory()) {
    dir.close();
    LOG_ERR("BookCache", "Cache path is not a directory during stats preservation: %s", cachePath.c_str());
    return false;
  }

  std::vector<StatsFileCandidate> candidates;
  candidates.reserve(MAX_STATS_FILES_TO_PRESERVE + 1);
  char name[96];
  for (FsFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDirectory = file.isDirectory();
    const size_t nameLen = file.getName(name, sizeof(name));
    file.close();
    if (isDirectory || nameLen == 0 || !isStatsFileName(name)) {
      continue;
    }
    if (candidates.size() == MAX_STATS_FILES_TO_PRESERVE) {
      dir.close();
      LOG_ERR("BookCache", "Too many stats files to preserve safely: %s", cachePath.c_str());
      return false;
    }
    candidates.push_back({name, statsFileVersion(name)});
  }
  dir.close();

  std::sort(candidates.begin(), candidates.end(), [](const StatsFileCandidate& lhs, const StatsFileCandidate& rhs) {
    if (lhs.version != rhs.version) {
      return lhs.version > rhs.version;
    }
    return lhs.name > rhs.name;
  });

  for (size_t i = 0; i < candidates.size(); ++i) {
    files.push_back({candidates[i].name, std::string(tmpPrefix) + candidates[i].name});
  }
  return true;
}

bool resolvePreservedFiles(const std::string& cachePath, const PreservedCacheFile* fixedFiles, const size_t fixedCount,
                           const bool includeStatsFiles, const char* statsTmpPrefix,
                           std::vector<ResolvedPreservedCacheFile>& files) {
  files.clear();
  files.reserve(fixedCount + (includeStatsFiles ? MAX_STATS_FILES_TO_PRESERVE : 0));
  appendFixedPreservedFiles(files, fixedFiles, fixedCount);
  if (includeStatsFiles && !appendStatsPreservedFiles(cachePath, files, statsTmpPrefix)) {
    return false;
  }
  return true;
}

bool restorePreservedFiles(const std::string& cachePath, const std::vector<ResolvedPreservedCacheFile>& files,
                           const bool* movedFiles = nullptr) {
  if (files.empty()) {
    return true;
  }

  bool restoredAny = false;
  bool ok = true;
  for (size_t i = 0; i < files.size(); i++) {
    if (movedFiles && !movedFiles[i]) {
      continue;
    }
    const std::string tmpPath = cachePath + "." + files[i].tmpName;
    if (!Storage.exists(tmpPath.c_str())) {
      continue;
    }

    Storage.mkdir(cachePath.c_str());
    const std::string finalPath = cachePath + "/" + files[i].name;
    if (Storage.exists(finalPath.c_str())) {
      Storage.remove(finalPath.c_str());
    }
    if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
      LOG_ERR("BookCache", "Failed to restore preserved cache state: %s", finalPath.c_str());
      ok = false;
    } else {
      restoredAny = true;
    }
  }

  if (restoredAny) {
    LOG_DBG("BookCache", "Restored preserved user cache state: %s", cachePath.c_str());
  }
  return ok;
}

bool recoverPreservedTemp(const std::string& cachePath, const std::string& name, const std::string& tmpName) {
  const std::string tmpPath = cachePath + "." + tmpName;
  if (!Storage.exists(tmpPath.c_str())) return true;
  const std::string sourcePath = cachePath + "/" + name;
  if (Storage.exists(sourcePath.c_str())) return true;
  if ((!Storage.exists(cachePath.c_str()) && !Storage.mkdir(cachePath.c_str())) ||
      !Storage.rename(tmpPath.c_str(), sourcePath.c_str())) {
    LOG_ERR("BookCache", "Failed to recover preserved cache state: %s", sourcePath.c_str());
    return false;
  }
  return true;
}

std::string statsRecoveryMarkerPath(const std::string& cachePath, const char* statsTmpPrefix) {
  return cachePath + "." + statsTmpPrefix + "stats_pending";
}

bool createStatsRecoveryMarker(const std::string& cachePath, const char* statsTmpPrefix) {
  const std::string markerPath = statsRecoveryMarkerPath(cachePath, statsTmpPrefix);
  FsFile marker;
  if (!Storage.openFileForWrite("BookCache", markerPath, marker)) {
    LOG_ERR("BookCache", "Failed to create stats recovery marker: %s", markerPath.c_str());
    return false;
  }
  const uint8_t value = 1;
  const bool written = marker.write(&value, sizeof(value)) == sizeof(value) && marker.sync();
  const bool closed = marker.close();
  if (!written || !closed) {
    LOG_ERR("BookCache", "Failed to save stats recovery marker: %s", markerPath.c_str());
    return false;
  }
  return true;
}

bool removeStatsRecoveryMarker(const std::string& cachePath, const char* statsTmpPrefix) {
  const std::string markerPath = statsRecoveryMarkerPath(cachePath, statsTmpPrefix);
  if (!Storage.exists(markerPath.c_str())) return true;
  if (Storage.remove(markerPath.c_str())) return true;
  LOG_ERR("BookCache", "Failed to remove stats recovery marker: %s", markerPath.c_str());
  return false;
}

bool recoverInterruptedPreservation(const std::string& cachePath, const PreservedCacheFile* fixedFiles,
                                    const size_t fixedCount, const bool includeStatsFiles, const char* statsTmpPrefix) {
  for (size_t i = 0; i < fixedCount; ++i) {
    if (!recoverPreservedTemp(cachePath, fixedFiles[i].name, fixedFiles[i].tmpName)) return false;
  }
  if (!includeStatsFiles) return true;
  if (!statsTmpPrefix) {
    LOG_ERR("BookCache", "Missing stats recovery temp prefix: %s", cachePath.c_str());
    return false;
  }
  // Normal clears do not enumerate the potentially large /.crosspoint folder.
  // A marker is synced before any stats file is moved out of its cache.
  if (!Storage.exists(statsRecoveryMarkerPath(cachePath, statsTmpPrefix).c_str())) return true;

  // Stats moved out of the cache are absent from the cache-directory scan in
  // resolvePreservedFiles(). Find their temporary siblings before that scan.
  const size_t slash = cachePath.find_last_of('/');
  if (slash == std::string::npos) return false;
  const std::string parentPath = slash == 0 ? "/" : cachePath.substr(0, slash);
  const std::string prefix = cachePath.substr(slash + 1) + "." + statsTmpPrefix;
  FsFile parent = Storage.open(parentPath.c_str());
  if (!parent || !parent.isDirectory()) {
    if (parent) parent.close();
    if (!Storage.exists(parentPath.c_str()) && !Storage.exists(cachePath.c_str())) return true;
    LOG_ERR("BookCache", "Failed to inspect preserved stats beside: %s", cachePath.c_str());
    return false;
  }
  std::vector<std::string> pendingStats;
  pendingStats.reserve(MAX_STATS_FILES_TO_PRESERVE);
  char entryName[128];
  for (FsFile entry = parent.openNextFile(); entry; entry = parent.openNextFile()) {
    const bool isDirectory = entry.isDirectory();
    const size_t nameLen = entry.getName(entryName, sizeof(entryName));
    entry.close();
    if (isDirectory || nameLen >= sizeof(entryName) || nameLen <= prefix.size() ||
        strncmp(entryName, prefix.c_str(), prefix.size()) != 0 || !isStatsFileName(entryName + prefix.size()))
      continue;
    if (pendingStats.size() == MAX_STATS_FILES_TO_PRESERVE) {
      parent.close();
      LOG_ERR("BookCache", "Too many interrupted stats files beside: %s", cachePath.c_str());
      return false;
    }
    pendingStats.emplace_back(entryName + prefix.size());
  }
  const bool iterationFailed = FsHelpers::directoryIterationFailed(parent);
  parent.close();
  if (iterationFailed) {
    LOG_ERR("BookCache", "Failed to inspect preserved stats beside: %s", cachePath.c_str());
    return false;
  }
  if (!std::all_of(pendingStats.begin(), pendingStats.end(), [&](const std::string& name) {
        return recoverPreservedTemp(cachePath, name, std::string(statsTmpPrefix) + name);
      }))
    return false;
  return removeStatsRecoveryMarker(cachePath, statsTmpPrefix);
}

bool preserveUserStateFiles(const std::string& cachePath, const std::vector<ResolvedPreservedCacheFile>& files,
                            bool* movedFiles) {
  bool ok = true;
  for (size_t i = 0; i < files.size(); i++) {
    if (movedFiles) {
      movedFiles[i] = false;
    }
    const std::string sourcePath = cachePath + "/" + files[i].name;
    const std::string tmpPath = cachePath + "." + files[i].tmpName;

    if (!Storage.exists(sourcePath.c_str())) {
      if (Storage.exists(tmpPath.c_str())) {
        LOG_ERR("BookCache", "Preserved state has no restored source: %s", sourcePath.c_str());
        ok = false;
      }
      continue;
    }
    if (Storage.exists(tmpPath.c_str()) && !Storage.remove(tmpPath.c_str())) {
      LOG_ERR("BookCache", "Failed to remove stale preserved state temp: %s", tmpPath.c_str());
      ok = false;
      continue;
    }
    if (!Storage.rename(sourcePath.c_str(), tmpPath.c_str())) {
      LOG_ERR("BookCache", "Failed to preserve cache state: %s", sourcePath.c_str());
      ok = false;
    } else if (movedFiles) {
      movedFiles[i] = true;
    }
  }
  return ok;
}

bool clearBookCacheForPath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return Epub(path, "/.crosspoint").clearCache();
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, "/.crosspoint").clearCache();
  }
  if (FsHelpers::hasTxtExtension(path)) {
    return Txt(path, "/.crosspoint").clearCache();
  }
  return false;
}

bool clearCacheDirectoryPreservingFiles(const std::string& cachePath, const PreservedCacheFile* fixedPreservedFiles,
                                        const size_t fixedPreservedCount, const bool includeStatsFiles,
                                        const char* statsTmpPrefix) {
  if (cachePath.empty()) {
    return false;
  }

  if (!recoverInterruptedPreservation(cachePath, fixedPreservedFiles, fixedPreservedCount, includeStatsFiles,
                                      statsTmpPrefix))
    return false;
  if (!Storage.exists(cachePath.c_str())) return true;

  std::vector<ResolvedPreservedCacheFile> preservedFiles;
  if (!resolvePreservedFiles(cachePath, fixedPreservedFiles, fixedPreservedCount, includeStatsFiles, statsTmpPrefix,
                             preservedFiles)) {
    return false;
  }
  const bool hasStats = includeStatsFiles && preservedFiles.size() > fixedPreservedCount;
  if (hasStats && !createStatsRecoveryMarker(cachePath, statsTmpPrefix)) return false;

  bool movedFiles[MAX_PRESERVED_CACHE_FILES + MAX_STATS_FILES_TO_PRESERVE] = {};
  const bool preserveOk = preserveUserStateFiles(cachePath, preservedFiles, movedFiles);
  if (!preserveOk) {
    const bool restored = restorePreservedFiles(cachePath, preservedFiles, movedFiles);
    if (!restored) {
      LOG_ERR("BookCache", "Failed to roll back preserved state after aborting cache clear: %s", cachePath.c_str());
    }
    if (hasStats && restored) removeStatsRecoveryMarker(cachePath, statsTmpPrefix);
    LOG_ERR("BookCache", "Aborted cache clear because preserved state could not be moved: %s", cachePath.c_str());
    return false;
  }

  const bool clearOk = Storage.removeDir(cachePath.c_str());
  const bool restoreOk = restorePreservedFiles(cachePath, preservedFiles, movedFiles);
  const bool markerOk = !hasStats || !restoreOk || removeStatsRecoveryMarker(cachePath, statsTmpPrefix);
  if (!clearOk) {
    LOG_ERR("BookCache", "Failed to clear cache directory: %s", cachePath.c_str());
  }
  return clearOk && restoreOk && markerOk;
}

}  // namespace

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";

  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0;
}

void clearBookCache(const std::string& path) { clearBookCachePreservingUserState(path); }

bool clearBookCachePreservingUserState(const std::string& path, const bool sourceChanged) {
  size_t preservedCount = 0;
  const PreservedCacheFile* preservedFiles = preservedFilesForPath(path, preservedCount);
  if (!preservedFiles || preservedCount == 0) {
    // Non-book uploads have no reader cache or sync identity to invalidate.
    return sourceChanged || clearBookCacheForPath(path);
  }

  const std::string cachePath = getBookCachePath(path);
  if (cachePath.empty()) {
    return false;
  }

  const bool includeStatsFiles = FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path);
  if (!recoverInterruptedPreservation(cachePath, preservedFiles, preservedCount, includeStatsFiles, "upload_preserve_"))
    return false;
  if (sourceChanged && !BookSyncFile::onFileChanged(path)) return false;

  std::vector<ResolvedPreservedCacheFile> resolvedFiles;
  if (!resolvePreservedFiles(cachePath, preservedFiles, preservedCount, includeStatsFiles, "upload_preserve_",
                             resolvedFiles)) {
    return false;
  }
  const bool hasStats = includeStatsFiles && resolvedFiles.size() > preservedCount;
  if (hasStats && !createStatsRecoveryMarker(cachePath, "upload_preserve_")) return false;

  bool movedFiles[MAX_PRESERVED_CACHE_FILES + MAX_STATS_FILES_TO_PRESERVE] = {};
  const bool preserveOk = preserveUserStateFiles(cachePath, resolvedFiles, movedFiles);
  if (!preserveOk) {
    const bool restored = restorePreservedFiles(cachePath, resolvedFiles, movedFiles);
    if (!restored) {
      LOG_ERR("BookCache", "Failed to roll back preserved state after aborting cache clear: %s", cachePath.c_str());
    }
    if (hasStats && restored) removeStatsRecoveryMarker(cachePath, "upload_preserve_");
    LOG_ERR("BookCache", "Aborted cache clear because user state could not be preserved: %s", cachePath.c_str());
    return false;
  }
  const bool clearOk = clearBookCacheForPath(path);
  const bool restoreOk = restorePreservedFiles(cachePath, resolvedFiles, movedFiles);
  const bool markerOk = !hasStats || !restoreOk || removeStatsRecoveryMarker(cachePath, "upload_preserve_");
  return clearOk && restoreOk && markerOk;
}

bool clearBookCacheDirectoryPreservingStats(const std::string& cachePath) {
  return clearCacheDirectoryPreservingFiles(cachePath, CACHE_CLEAR_USER_STATE_FILES,
                                            std::size(CACHE_CLEAR_USER_STATE_FILES), true, "clear_preserve_");
}
