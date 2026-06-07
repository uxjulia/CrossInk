#include "BookCacheUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include <cstring>
#include <iterator>

namespace {

struct PreservedCacheFile {
  const char* name;
  const char* tmpName;
};

constexpr PreservedCacheFile EPUB_USER_STATE_FILES[] = {
    {"progress.bin", "upload_preserve_progress.bin"},
    {"progress.bin.bak", "upload_preserve_progress.bin.bak"},
    {"stats.bin", "upload_preserve_stats.bin"},
    {"reader_settings.bin", "upload_preserve_reader_settings.bin"},
};

constexpr PreservedCacheFile PAGE_PROGRESS_FILES[] = {
    {"progress.bin", "upload_preserve_progress.bin"},
};
constexpr size_t MAX_PRESERVED_CACHE_FILES = std::size(EPUB_USER_STATE_FILES) > std::size(PAGE_PROGRESS_FILES)
                                                 ? std::size(EPUB_USER_STATE_FILES)
                                                 : std::size(PAGE_PROGRESS_FILES);

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

bool restorePreservedFiles(const std::string& cachePath, const PreservedCacheFile* files, const size_t count,
                           const bool* movedFiles = nullptr) {
  if (count == 0) {
    return true;
  }

  bool restoredAny = false;
  bool ok = true;
  for (size_t i = 0; i < count; i++) {
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

bool preserveUserStateFiles(const std::string& cachePath, const PreservedCacheFile* files, const size_t count,
                            bool* movedFiles) {
  bool ok = true;
  for (size_t i = 0; i < count; i++) {
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

void clearBookCache(const std::string& path) {
  if (!clearBookCacheForPath(path)) {
    return;
  }
  LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
}

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

  if (preservedCount > MAX_PRESERVED_CACHE_FILES) {
    LOG_ERR("BookCache", "Too many preserved cache files: count=%u max=%u", static_cast<unsigned>(preservedCount),
            static_cast<unsigned>(MAX_PRESERVED_CACHE_FILES));
    return false;
  }

  bool movedFiles[MAX_PRESERVED_CACHE_FILES] = {};
  const bool preserveOk = preserveUserStateFiles(cachePath, preservedFiles, preservedCount, movedFiles);
  if (!preserveOk) {
    if (!restorePreservedFiles(cachePath, preservedFiles, preservedCount, movedFiles)) {
      LOG_ERR("BookCache", "Failed to roll back preserved state after aborting cache clear: %s", cachePath.c_str());
    }
    LOG_ERR("BookCache", "Aborted cache clear because user state could not be preserved: %s", cachePath.c_str());
    return false;
  }
  const bool clearOk = clearBookCacheForPath(path);
  const bool restoreOk = restorePreservedFiles(cachePath, preservedFiles, preservedCount, movedFiles);
  if (clearOk) {
    LOG_DBG("BookCache", "Done checking metadata cache for: %s", path.c_str());
  }
  return preserveOk && clearOk && restoreOk;
}
