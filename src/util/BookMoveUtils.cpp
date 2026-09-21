#include "BookMoveUtils.h"

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Txt.h>
#include <Xtc.h>

#include <cstring>

#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"

namespace {
constexpr char READ_FOLDER[] = "/Read";

bool getCachePath(const std::string& bookPath, const char* bookType, std::string& cachePath) {
  if (strcmp(bookType, "epub") == 0) {
    cachePath = Epub::cachePathForFilePath(bookPath, "/.crosspoint");
  } else if (strcmp(bookType, "xtc") == 0) {
    cachePath = Xtc(bookPath, "/.crosspoint").getCachePath();
  } else if (strcmp(bookType, "txt") == 0) {
    cachePath = Txt(bookPath, "/.crosspoint").getCachePath();
  } else {
    LOG_ERR("BookMove", "Unknown book type for state migration: %s", bookType);
    return false;
  }
  return true;
}
}  // namespace

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

RenameMigrationResult migrateRenamedBookState(const std::string& oldPath, const std::string& newPath,
                                              const std::string& oldCachePath, const std::string& title,
                                              const std::string& author, const char* bookType) {
  if (!bookType) {
    LOG_ERR("BookMove", "Missing book type for state migration");
    return RenameMigrationResult::RolledBack;
  }

  std::string newCachePath;
  if (!getCachePath(newPath, bookType, newCachePath)) return RenameMigrationResult::RolledBack;

  bool cacheMoved = false;
  bool bookRenamed = false;
  bool recentMoved = false;
  bool openPathMoved = false;
  const bool shouldMoveOpenPath = APP_STATE.openEpubPath == oldPath;
  BookmarkStore::RenameMigration bookmarkMigration;
  ClippingStore::RenameMigration clippingMigration;

  const auto commitMetadata = [&]() {
    if (!BookmarkStore::commitRenameMigration(bookmarkMigration)) {
      LOG_ERR("BookMove", "Renamed book kept stale bookmark rollback files: %s", newPath.c_str());
    }
    if (!ClippingStore::commitRenameMigration(clippingMigration)) {
      LOG_ERR("BookMove", "Renamed book kept stale clipping rollback files: %s", newPath.c_str());
    }
  };

  const auto rollbackMovedReferences = [&]() {
    bool rolledBack = true;
    if (openPathMoved) {
      APP_STATE.openEpubPath = oldPath;
      if (!APP_STATE.saveToFile()) {
        LOG_ERR("BookMove", "Failed to restore open book path after rename: %s", oldPath.c_str());
        rolledBack = false;
      } else {
        openPathMoved = false;
      }
    }
    if (recentMoved) {
      if (!RECENT_BOOKS.updatePath(newPath, oldPath, newCachePath, oldCachePath)) {
        rolledBack = false;
      } else {
        recentMoved = false;
      }
    }
    return rolledBack;
  };

  const auto rollbackPreparedStorage = [&]() {
    bool rolledBack = true;
    if (!ClippingStore::rollbackRenameMigration(clippingMigration)) {
      LOG_ERR("BookMove", "Failed to restore clipping metadata after rename: %s", newPath.c_str());
      rolledBack = false;
    }
    if (!BookmarkStore::rollbackRenameMigration(bookmarkMigration)) {
      LOG_ERR("BookMove", "Failed to restore bookmark metadata after rename: %s", newPath.c_str());
      rolledBack = false;
    }
    if (cacheMoved && Storage.exists(newCachePath.c_str()) &&
        !Storage.rename(newCachePath.c_str(), oldCachePath.c_str())) {
      LOG_ERR("BookMove", "Failed to roll back cache migration %s -> %s", newCachePath.c_str(), oldCachePath.c_str());
      rolledBack = false;
    }
    return rolledBack;
  };

  const auto keepRenamed = [&]() {
    LOG_ERR("BookMove", "Could not restore the original filename; preserving state at the new path");
    // Both calls are idempotent when that reference is already at newPath,
    // and repair a partially completed rollback when it is still at oldPath.
    if (!RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath)) {
      LOG_ERR("BookMove", "Failed to recover recent book at new path %s", newPath.c_str());
    }
    if (shouldMoveOpenPath) {
      APP_STATE.openEpubPath = newPath;
      if (!APP_STATE.saveToFile()) {
        LOG_ERR("BookMove", "Failed to recover open book at new path %s", newPath.c_str());
      }
    }
    commitMetadata();
    return RenameMigrationResult::KeepRenamed;
  };

  const auto recover = [&]() {
    if (bookRenamed) {
      if (!Storage.rename(newPath.c_str(), oldPath.c_str())) {
        return keepRenamed();
      }
      bookRenamed = false;

      if (!rollbackMovedReferences()) {
        if (Storage.rename(oldPath.c_str(), newPath.c_str())) {
          bookRenamed = true;
          return keepRenamed();
        }
        LOG_ERR("BookMove", "Failed to re-establish new book path after incomplete state rollback: %s",
                newPath.c_str());
      }
    }

    if (!rollbackPreparedStorage()) {
      LOG_ERR("BookMove", "Storage rollback was incomplete; the original book path remains available");
    }
    return RenameMigrationResult::RolledBack;
  };

  // Publish the new-path metadata before the book itself is renamed. Until
  // the physical rename succeeds, the old book and its source metadata remain
  // intact; after it succeeds, the new-path metadata is already discoverable.
  if (!BookmarkStore::beginRenameMigration(oldPath, newPath, title, author, bookType, bookmarkMigration)) {
    LOG_ERR("BookMove", "Failed to migrate bookmarks for renamed book %s -> %s", oldPath.c_str(), newPath.c_str());
    return recover();
  }

  if (strcmp(bookType, "epub") == 0) {
    if (!ClippingStore::beginRenameMigration(oldPath, newPath, title, author, bookType, clippingMigration)) {
      LOG_ERR("BookMove", "Failed to migrate clippings for renamed book %s -> %s", oldPath.c_str(), newPath.c_str());
      return recover();
    }
  }

  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("BookMove", "Failed to rename cache dir %s -> %s", oldCachePath.c_str(), newCachePath.c_str());
      return recover();
    }
    cacheMoved = true;
  }

  if (!Storage.rename(oldPath.c_str(), newPath.c_str())) {
    LOG_ERR("BookMove", "Failed to rename file: %s -> %s", oldPath.c_str(), newPath.c_str());
    return recover();
  }
  bookRenamed = true;

  if (!RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath)) {
    return recover();
  }
  recentMoved = true;

  if (shouldMoveOpenPath) {
    APP_STATE.openEpubPath = newPath;
    openPathMoved = true;
    if (!APP_STATE.saveToFile()) {
      LOG_ERR("BookMove", "Failed to save renamed open book path: %s", newPath.c_str());
      return recover();
    }
  }

  commitMetadata();

  return RenameMigrationResult::Success;
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
    (void)RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath);
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
