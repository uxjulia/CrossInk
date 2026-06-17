#include "BookMoveUtils.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"

namespace {
constexpr char READ_FOLDER[] = "/Read";
}

namespace BookMoveUtils {

std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

bool migrateMovedEpubState(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                           const std::string& title, const std::string& author, const bool keepInRecents) {
  bool ok = true;

  const std::string newCachePath = Epub::cachePathForFilePath(newPath, "/.crosspoint");
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("BookMove", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(),
              newCachePath.c_str());
      ok = false;
    }
  }

  if (!BookmarkStore::migrateForFilePath(oldPath, newPath, title, author, "epub")) {
    LOG_ERR("BookMove", "Failed to migrate bookmarks for moved book %s -> %s", oldPath.c_str(), newPath.c_str());
    ok = false;
  }

  if (!ClippingStore::migrateForFilePath(oldPath, newPath, title, author, "epub")) {
    LOG_ERR("BookMove", "Failed to migrate clippings for moved book %s -> %s", oldPath.c_str(), newPath.c_str());
    ok = false;
  }

  if (keepInRecents) {
    RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath);
  } else {
    RECENT_BOOKS.removeByPath(oldPath);
    RECENT_BOOKS.removeByPath(newPath);
  }

  if (APP_STATE.openEpubPath == oldPath) {
    APP_STATE.openEpubPath = newPath;
    APP_STATE.saveToFile();
  }

  return ok;
}

}  // namespace BookMoveUtils
