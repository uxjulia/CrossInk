#include "BookCacheUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <vector>

namespace {

struct PreservedCacheFile {
  const char* name;
  const char* tmpName;
};

constexpr PreservedCacheFile EPUB_USER_STATE_FILES[] = {
    {"progress.bin", "upload_preserve_progress.bin"},
    {"progress.bin.bak", "upload_preserve_progress.bin.bak"},
    {"reader_settings.bin", "upload_preserve_reader_settings.bin"},
};

constexpr PreservedCacheFile PAGE_PROGRESS_FILES[] = {
    {"progress.bin", "upload_preserve_progress.bin"},
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
constexpr size_t MAX_STATS_FILES_TO_PRESERVE = 8;
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

bool isStatsFileName(const char* name) {
  if (!name) {
    return false;
  }
  const size_t nameLen = strlen(name);
  constexpr size_t prefixLen = std::size(STATS_PREFIX) - 1;
  constexpr size_t suffixLen = std::size(STATS_SUFFIX) - 1;
  return nameLen >= prefixLen + suffixLen && strncmp(name, STATS_PREFIX, prefixLen) == 0 &&
         strcmp(name + nameLen - suffixLen, STATS_SUFFIX) == 0;
}

int statsFileVersion(const char* name) {
  if (!name || strcmp(name, "stats.bin") == 0) {
    return 0;
  }

  constexpr char VERSION_PREFIX[] = "stats_v";
  constexpr size_t versionPrefixLen = std::size(VERSION_PREFIX) - 1;
  constexpr size_t suffixLen = std::size(STATS_SUFFIX) - 1;
  const size_t nameLen = strlen(name);
  if (nameLen <= versionPrefixLen + suffixLen || strncmp(name, VERSION_PREFIX, versionPrefixLen) != 0 ||
      strcmp(name + nameLen - suffixLen, STATS_SUFFIX) != 0) {
    return -1;
  }

  int version = 0;
  for (size_t i = versionPrefixLen; i < nameLen - suffixLen; ++i) {
    if (name[i] < '0' || name[i] > '9') {
      return -1;
    }
    version = version * 10 + (name[i] - '0');
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
    if (i >= MAX_STATS_FILES_TO_PRESERVE) {
      LOG_DBG("BookCache", "Dropping older stats file during cache preservation: %s", candidates[i].name.c_str());
      continue;
    }
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

bool preserveUserStateFiles(const std::string& cachePath, const std::vector<ResolvedPreservedCacheFile>& files,
                            bool* movedFiles) {
  bool ok = true;
  for (size_t i = 0; i < files.size(); i++) {
    if (movedFiles) {
      movedFiles[i] = false;
    }
    const std::string sourcePath = cachePath + "/" + files[i].name;
    const std::string tmpPath = cachePath + "." + files[i].tmpName;

    if (Storage.exists(tmpPath.c_str()) && !Storage.remove(tmpPath.c_str())) {
      LOG_ERR("BookCache", "Failed to remove stale preserved state temp: %s", tmpPath.c_str());
      ok = false;
      continue;
    }
    if (!Storage.exists(sourcePath.c_str())) {
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

  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("BookCache", "Cache does not exist, no action needed: %s", cachePath.c_str());
    return true;
  }

  std::vector<ResolvedPreservedCacheFile> preservedFiles;
  if (!resolvePreservedFiles(cachePath, fixedPreservedFiles, fixedPreservedCount, includeStatsFiles, statsTmpPrefix,
                             preservedFiles)) {
    return false;
  }

  bool movedFiles[MAX_PRESERVED_CACHE_FILES + MAX_STATS_FILES_TO_PRESERVE] = {};
  const bool preserveOk = preserveUserStateFiles(cachePath, preservedFiles, movedFiles);
  if (!preserveOk) {
    if (!restorePreservedFiles(cachePath, preservedFiles, movedFiles)) {
      LOG_ERR("BookCache", "Failed to roll back preserved state after aborting cache clear: %s", cachePath.c_str());
    }
    LOG_ERR("BookCache", "Aborted cache clear because preserved state could not be moved: %s", cachePath.c_str());
    return false;
  }

  const bool clearOk = Storage.removeDir(cachePath.c_str());
  const bool restoreOk = restorePreservedFiles(cachePath, preservedFiles, movedFiles);
  if (!clearOk) {
    LOG_ERR("BookCache", "Failed to clear cache directory: %s", cachePath.c_str());
  }
  return clearOk && restoreOk;
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

bool clearBookCachePreservingUserState(const std::string& path) {
  size_t preservedCount = 0;
  const PreservedCacheFile* preservedFiles = preservedFilesForPath(path, preservedCount);
  if (!preservedFiles || preservedCount == 0) {
    return clearBookCacheForPath(path);
  }

  const std::string cachePath = getBookCachePath(path);
  if (cachePath.empty()) {
    return false;
  }

  std::vector<ResolvedPreservedCacheFile> resolvedFiles;
  const bool includeStatsFiles = FsHelpers::hasEpubExtension(path);
  if (!resolvePreservedFiles(cachePath, preservedFiles, preservedCount, includeStatsFiles, "upload_preserve_",
                             resolvedFiles)) {
    return false;
  }

  bool movedFiles[MAX_PRESERVED_CACHE_FILES + MAX_STATS_FILES_TO_PRESERVE] = {};
  const bool preserveOk = preserveUserStateFiles(cachePath, resolvedFiles, movedFiles);
  if (!preserveOk) {
    if (!restorePreservedFiles(cachePath, resolvedFiles, movedFiles)) {
      LOG_ERR("BookCache", "Failed to roll back preserved state after aborting cache clear: %s", cachePath.c_str());
    }
    LOG_ERR("BookCache", "Aborted cache clear because user state could not be preserved: %s", cachePath.c_str());
    return false;
  }
  const bool clearOk = clearBookCacheForPath(path);
  const bool restoreOk = restorePreservedFiles(cachePath, resolvedFiles, movedFiles);
  if (clearOk) {
    LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
  }
  return clearOk && restoreOk;
}

bool clearBookCacheDirectoryPreservingStats(const std::string& cachePath) {
  return clearCacheDirectoryPreservingFiles(cachePath, nullptr, 0, true, "clear_preserve_");
}
