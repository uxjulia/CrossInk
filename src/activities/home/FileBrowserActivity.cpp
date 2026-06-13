#include "FileBrowserActivity.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>

#include "BookActions.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FileBrowserActionActivity.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/themes/minimal/MinimalTheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long COMPLETED_FEEDBACK_MS = 1000;
constexpr int ROOT_HINT_GAP = 20;
constexpr uint32_t FILE_BROWSER_APPEND_MIN_FREE_AFTER_ALLOC = 48U * 1024U;
constexpr uint32_t FILE_BROWSER_APPEND_MIN_MAX_ALLOC_AFTER_ALLOC = 16U * 1024U;

bool isDefaultSleepFolderPath(const std::string& path) { return path == "/sleep" || path == "/.sleep"; }

bool isSleepImageFile(const std::string& path) {
  return FsHelpers::hasBmpExtension(path) || FsHelpers::hasPngExtension(path);
}

bool isMacOSMetadataEntry(std::string_view filename) {
  return filename.rfind("._", 0) == 0 || filename == ".DS_Store" || filename == ".Spotlight-V100" ||
         filename == ".Trashes" || filename == ".fseventsd";
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.length() != b.length()) return false;
  for (size_t i = 0; i < a.length(); ++i) {
    if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

bool isWindowsMetadataEntry(std::string_view filename) {
  return equalsIgnoreCase(filename, "System Volume Information") || equalsIgnoreCase(filename, "$RECYCLE.BIN") ||
         equalsIgnoreCase(filename, "desktop.ini") || equalsIgnoreCase(filename, "Thumbs.db") ||
         equalsIgnoreCase(filename, "IndexerVolumeGuid") || equalsIgnoreCase(filename, "WPSettings.dat");
}

size_t estimateNextVectorCapacity(size_t size, size_t capacity) {
  if (size < capacity) {
    return capacity;
  }
  if (capacity == 0) {
    return 1;
  }
  return capacity * 2;
}

bool hasHeapForFileEntryAppend(const std::vector<std::string>& files, size_t entryLen) {
  const size_t nextCapacity = estimateNextVectorCapacity(files.size(), files.capacity());
  const uint32_t vectorGrowthBytes =
      (nextCapacity == files.capacity()) ? 0U : static_cast<uint32_t>(nextCapacity * sizeof(std::string));
  const uint32_t stringBytes = static_cast<uint32_t>(entryLen + 1);
  const uint32_t largestNeeded = std::max(vectorGrowthBytes, stringBytes);

  return ESP.getFreeHeap() >= vectorGrowthBytes + stringBytes + FILE_BROWSER_APPEND_MIN_FREE_AFTER_ALLOC &&
         ESP.getMaxAllocHeap() >= largestNeeded + FILE_BROWSER_APPEND_MIN_MAX_ALLOC_AFTER_ALLOC;
}

bool hasFileMetadata(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path) || FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);
}

std::string buildFullPath(std::string basepath, const std::string& entry) {
  if (basepath.back() != '/') basepath += "/";
  return basepath + entry;
}

std::string normalizeDirectoryPath(std::string path) {
  while (path.length() > 1 && path.back() == '/') {
    path.pop_back();
  }
  return path;
}

bool isSleepFolderPreferencePath(const std::string& path) { return !path.empty() && !isDefaultSleepFolderPath(path); }

bool containsHiddenPathSegment(const std::string& path) {
  if (path.empty()) return false;
  size_t segmentStart = (path.front() == '/') ? 1 : 0;
  while (segmentStart < path.length()) {
    const size_t segmentEnd = path.find('/', segmentStart);
    if (segmentStart < path.length() && path[segmentStart] == '.') {
      return true;
    }
    if (segmentEnd == std::string::npos) {
      break;
    }
    segmentStart = segmentEnd + 1;
  }
  return false;
}

void collectMetadataPathsRecursively(const std::string& dirPath, std::vector<std::string>& paths) {
  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    LOG_ERR("FileBrowser", "Failed to scan directory metadata before delete: %s", dirPath.c_str());
    return;
  }

  char name[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    const std::string childPath = buildFullPath(dirPath, name);
    if (file.isDirectory()) {
      collectMetadataPathsRecursively(childPath, paths);
    } else if (hasFileMetadata(childPath)) {
      paths.push_back(childPath);
    }
    file.close();
  }
  dir.close();
}

std::string getFileName(std::string filename);
}  // namespace

void FileBrowserActivity::loadFiles() {
  files.clear();
  fileListMemoryLimited = false;

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) {
      root.close();
    }
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (isMacOSMetadataEntry(name) || isWindowsMetadataEntry(name) || (!SETTINGS.showHiddenFiles && name[0] == '.')) {
      file.close();
      continue;
    }

    bool shouldAdd = false;
    size_t entryLen = std::strlen(name);
    if (file.isDirectory()) {
      if (entryLen + 1 >= sizeof(name)) {
        LOG_ERR("FileBrowser", "Skipping oversized directory entry: %s", name);
        file.close();
        continue;
      }
      name[entryLen++] = '/';
      name[entryLen] = '\0';
      shouldAdd = true;
    } else {
      std::string_view filename{name};
      if (mode == Mode::PickFirmware) {
        // Firmware picker: only show .bin files.
        shouldAdd = FsHelpers::checkFileExtension(filename, ".bin");
      } else if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename) ||
                 FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
                 FsHelpers::hasBmpExtension(filename) || FsHelpers::hasPngExtension(filename)) {
        shouldAdd = true;
      }
    }

    if (!shouldAdd) {
      file.close();
      continue;
    }

    if (!hasHeapForFileEntryAppend(files, entryLen)) {
      fileListMemoryLimited = true;
      LOG_ERR("FileBrowser", "Low heap while loading %s (entries=%u free=%u maxAlloc=%u)", basepath.c_str(),
              static_cast<unsigned>(files.size()), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      file.close();
      root.close();
      files.clear();
      return;
    }

    files.emplace_back(name);
    file.close();
  }
  root.close();
  FsHelpers::sortFileList(files);
}

void FileBrowserActivity::onEnter() {
  Activity::onEnter();

  selectorIndex = 0;

  // If Confirm was held while this activity opened (typical when launched from a menu), ignore
  // its release — otherwise we'd immediately auto-open whatever is at index 0.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  auto root = Storage.open(basepath.c_str());
  if (!root) {
    basepath = "/";
    loadFiles();
    requestUpdate();
    return;
  }

  const bool rootIsDirectory = root.isDirectory();
  root.close();

  if (!rootIsDirectory) {
    lockLongPressBack = mappedInput.isPressed(MappedInputManager::Button::Back);

    const std::string oldPath = basepath;
    basepath = FsHelpers::extractFolderPath(basepath);
    loadFiles();

    const auto pos = oldPath.find_last_of('/');
    const std::string fileName = oldPath.substr(pos + 1);
    selectorIndex = findEntry(fileName);
  } else {
    loadFiles();
  }

  requestUpdate();
}

void FileBrowserActivity::onExit() {
  Activity::onExit();
  files.clear();
}

void FileBrowserActivity::promptDeleteFile(const std::string& fullPath, const std::string& entry) {
  auto handler = [this, fullPath](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("FileBrowser", "Delete cancelled by user");
      return;
    }

    LOG_DBG("FileBrowser", "Attempting to delete: %s", fullPath.c_str());
    BookActions::clearFileMetadata(fullPath);
    if (!Storage.remove(fullPath.c_str())) {
      LOG_ERR("FileBrowser", "Failed to delete file: %s", fullPath.c_str());
      return;
    }

    LOG_DBG("FileBrowser", "Deleted successfully");
    if (isPinnedSleepFavorite(fullPath)) {
      unpinSleepFavorite();
    }

    loadFiles();
    if (files.empty()) {
      selectorIndex = 0;
    } else if (selectorIndex >= files.size()) {
      selectorIndex = files.size() - 1;
    }
    requestUpdate(true);
  };

  const std::string heading = tr(STR_DELETE) + std::string("? ");
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entry), handler);
}

void FileBrowserActivity::promptDeleteDirectory(const std::string& fullPath, const std::string& entry,
                                                const bool ignoreInitialConfirmRelease) {
  const std::string dirPath = normalizeDirectoryPath(fullPath);
  auto handler = [this, dirPath](const ActivityResult& res) {
    longPressConfirmHandled = false;
    if (res.isCancelled) {
      LOG_DBG("FileBrowser", "Delete cancelled by user");
      return;
    }

    std::vector<std::string> metadataPaths;
    collectMetadataPathsRecursively(dirPath, metadataPaths);

    LOG_DBG("FileBrowser", "Attempting to delete directory: %s", dirPath.c_str());
    if (!Storage.removeDir(dirPath.c_str())) {
      LOG_ERR("FileBrowser", "Failed to delete directory: %s", dirPath.c_str());
      return;
    }

    LOG_DBG("FileBrowser", "Deleted successfully");
    for (const auto& metadataPath : metadataPaths) {
      BookActions::clearFileMetadata(metadataPath);
    }

    const std::string favoritePrefix = dirPath + "/";
    if (!APP_STATE.favoriteSleepImagePath.empty() && APP_STATE.favoriteSleepImagePath.rfind(favoritePrefix, 0) == 0) {
      unpinSleepFavorite();
    }
    if (isPreferredSleepFolder(dirPath)) {
      clearPreferredSleepFolder();
    }

    loadFiles();
    if (files.empty()) {
      selectorIndex = 0;
    } else if (selectorIndex >= files.size()) {
      selectorIndex = files.size() - 1;
    }
    requestUpdate(true);
  };

  const std::string heading = tr(STR_DELETE) + std::string("? ");
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, entry, ignoreInitialConfirmRelease),
      handler);
}

void FileBrowserActivity::showDirectoryActionMenu(const std::string& entry, bool ignoreInitialConfirmRelease) {
  const std::string fullPath = normalizeDirectoryPath(buildFullPath(basepath, entry));
  const bool useDefaultFolders = isDefaultSleepFolderPath(fullPath) || isPreferredSleepFolder(fullPath);
  std::vector<FileBrowserActionActivity::MenuItem> items;
  items.push_back({useDefaultFolders ? FileBrowserAction::ClearSleepFolder : FileBrowserAction::SetSleepFolder,
                   useDefaultFolders ? StrId::STR_USE_DEFAULT_SLEEP_FOLDERS : StrId::STR_SET_AS_SLEEP_FOLDER});
  items.push_back({FileBrowserAction::Delete, StrId::STR_DELETE});

  startActivityForResult(std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, getFileName(entry),
                                                                     std::move(items), ignoreInitialConfirmRelease),
                         [this, fullPath, entry](const ActivityResult& result) {
                           longPressConfirmHandled = false;
                           if (result.isCancelled) {
                             return;
                           }

                           const auto action =
                               static_cast<FileBrowserAction>(std::get<FileBrowserActionResult>(result.data).action);
                           switch (action) {
                             case FileBrowserAction::Delete:
                               promptDeleteDirectory(fullPath, entry);
                               return;
                             case FileBrowserAction::SetSleepFolder:
                               setPreferredSleepFolder(fullPath);
                               return;
                             case FileBrowserAction::ClearSleepFolder:
                               clearPreferredSleepFolder();
                               return;
                             case FileBrowserAction::DeleteCache:
                             case FileBrowserAction::DeleteStats:
                             case FileBrowserAction::ToggleCompleted:
                             case FileBrowserAction::RemoveFromRecents:
                             case FileBrowserAction::PinFavorite:
                             case FileBrowserAction::UnpinFavorite:
                               return;
                           }
                         });
}

void FileBrowserActivity::pinSleepFavorite(const std::string& fullPath) {
  APP_STATE.favoriteSleepImagePath = fullPath;
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("FileBrowser", "Failed to save favorite sleep image path: %s", fullPath.c_str());
    return;
  }
  LOG_INF("FileBrowser", "Pinned favorite sleep image: %s", fullPath.c_str());
  requestUpdate();
}

void FileBrowserActivity::unpinSleepFavorite() {
  if (APP_STATE.favoriteSleepImagePath.empty()) {
    return;
  }

  APP_STATE.favoriteSleepImagePath.clear();
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("FileBrowser", "Failed to clear favorite sleep image path");
    return;
  }
  LOG_INF("FileBrowser", "Cleared favorite sleep image");
  requestUpdate();
}

bool FileBrowserActivity::isPinnedSleepFavorite(const std::string& fullPath) const {
  return APP_STATE.favoriteSleepImagePath == fullPath;
}

void FileBrowserActivity::setPreferredSleepFolder(const std::string& fullPath) {
  const std::string normalizedPath = normalizeDirectoryPath(fullPath);
  const std::string nextPath = isSleepFolderPreferencePath(normalizedPath) ? normalizedPath : std::string();
  if (APP_STATE.preferredSleepFolderPath == nextPath) {
    requestUpdate();
    return;
  }

  APP_STATE.preferredSleepFolderPath = nextPath;
  APP_STATE.clearRecentSleepHistory();
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("FileBrowser", "Failed to save preferred sleep folder path: %s", normalizedPath.c_str());
    return;
  }
  LOG_INF("FileBrowser", "Preferred sleep folder set to: %s", nextPath.empty() ? "<default>" : nextPath.c_str());
  requestUpdate();
}

void FileBrowserActivity::clearPreferredSleepFolder() {
  if (APP_STATE.preferredSleepFolderPath.empty()) {
    requestUpdate();
    return;
  }

  APP_STATE.preferredSleepFolderPath.clear();
  APP_STATE.clearRecentSleepHistory();
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("FileBrowser", "Failed to clear preferred sleep folder path");
    return;
  }
  LOG_INF("FileBrowser", "Cleared preferred sleep folder");
  requestUpdate();
}

bool FileBrowserActivity::isPreferredSleepFolder(const std::string& fullPath) const {
  return APP_STATE.preferredSleepFolderPath == normalizeDirectoryPath(fullPath);
}

bool FileBrowserActivity::isSleepFavoriteFolder(const std::string& fullPath) const {
  const std::string normalizedPath = normalizeDirectoryPath(fullPath);
  return isDefaultSleepFolderPath(normalizedPath) || isPreferredSleepFolder(normalizedPath);
}

void FileBrowserActivity::showFileActionMenu(const std::string& entry, bool ignoreInitialConfirmRelease) {
  const std::string fullPath = buildFullPath(basepath, entry);
  std::vector<FileBrowserActionActivity::MenuItem> items = BookActions::buildBookActionItems(fullPath, false);

  const bool canPinFavorite = isSleepFavoriteFolder(basepath) && isSleepImageFile(entry);
  if (canPinFavorite) {
    items.push_back(
        {isPinnedSleepFavorite(fullPath) ? FileBrowserAction::UnpinFavorite : FileBrowserAction::PinFavorite,
         isPinnedSleepFavorite(fullPath) ? StrId::STR_UNPIN_AS_FAVORITE : StrId::STR_PIN_AS_FAVORITE});
  }

  startActivityForResult(
      std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, getFileName(entry), std::move(items),
                                                  ignoreInitialConfirmRelease),
      [this, fullPath, entry](const ActivityResult& result) {
        longPressConfirmHandled = false;
        if (result.isCancelled) {
          return;
        }

        const auto action = static_cast<FileBrowserAction>(std::get<FileBrowserActionResult>(result.data).action);
        switch (action) {
          case FileBrowserAction::Delete:
            promptDeleteFile(fullPath, entry);
            return;
          case FileBrowserAction::DeleteCache:
            startActivityForResult(std::make_unique<ConfirmationActivity>(
                                       renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE_CACHE),
                                       getFileName(entry)),
                                   [this, fullPath](const ActivityResult& confirmation) {
                                     if (!confirmation.isCancelled) {
                                       if (!BookActions::clearBookCache(fullPath)) {
                                         LOG_ERR("FileBrowser", "Failed to clear book cache for: %s", fullPath.c_str());
                                       } else {
                                         BookActions::drawToast(renderer, tr(STR_BOOK_CACHE_DELETED));
                                         delay(1000);
                                       }
                                     }
                                     requestUpdate();
                                   });
            return;
          case FileBrowserAction::DeleteStats:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(renderer, mappedInput,
                                                       BookActions::confirmationHeading(StrId::STR_DELETE_BOOK_STATS),
                                                       getFileName(entry)),
                [this, fullPath](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled) {
                    if (!BookActions::deleteBookStats(fullPath)) {
                      LOG_ERR("FileBrowser", "Failed to delete book stats for: %s", fullPath.c_str());
                    } else {
                      BookActions::drawToast(renderer, tr(STR_BOOK_STATS_DELETED));
                      delay(1000);
                    }
                  }
                  requestUpdate();
                });
            return;
          case FileBrowserAction::ToggleCompleted:
            if (BookActions::toggleEpubCompleted(fullPath, getFileName(entry), completedFeedbackIsFinished)) {
              pendingCompletedFeedback = true;
              completedFeedbackShowTime = millis();
            }
            loadFiles();
            selectorIndex = files.empty() ? 0 : std::min(selectorIndex, files.size() - 1);
            requestUpdate(true);
            return;
          case FileBrowserAction::PinFavorite:
            if (FsHelpers::hasPngExtension(fullPath)) {
              startActivityForResult(
                  std::make_unique<ConfirmationActivity>(renderer, mappedInput, "", tr(STR_PIN_PNG_WARNING)),
                  [this, fullPath](const ActivityResult& confirmation) {
                    if (!confirmation.isCancelled) {
                      pinSleepFavorite(fullPath);
                    }
                  });
            } else {
              pinSleepFavorite(fullPath);
            }
            return;
          case FileBrowserAction::UnpinFavorite:
            unpinSleepFavorite();
            return;
          case FileBrowserAction::SetSleepFolder:
          case FileBrowserAction::ClearSleepFolder:
          case FileBrowserAction::RemoveFromRecents:
            return;
        }
      });
}

void FileBrowserActivity::toggleHiddenFiles() {
  const std::string currentEntry =
      (!files.empty() && selectorIndex < files.size()) ? files[selectorIndex] : std::string();
  SETTINGS.showHiddenFiles = SETTINGS.showHiddenFiles ? 0 : 1;
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("FileBrowser", "Failed to save showHiddenFiles=%u", SETTINGS.showHiddenFiles);
  }

  if (!SETTINGS.showHiddenFiles && containsHiddenPathSegment(basepath)) {
    basepath = "/";
  }

  loadFiles();
  selectorIndex = currentEntry.empty() ? 0 : findEntry(currentEntry);
  if (!files.empty() && selectorIndex >= files.size()) {
    selectorIndex = files.size() - 1;
  }
  requestUpdate();
}

void FileBrowserActivity::loop() {
  if (pendingCompletedFeedback) {
    const bool timedOut = (millis() - completedFeedbackShowTime) >= COMPLETED_FEEDBACK_MS;
    const bool navPressed = mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (timedOut || navPressed) {
      pendingCompletedFeedback = false;
      requestUpdate();
      return;
    }
  }

  // Long press BACK/HOME (1s+) toggles hidden files (Books mode only).
  // In firmware-pick mode we keep navigation simple: short Back = up dir / cancel.
  if (mode == Mode::Books && !longPressBackHandled && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= GO_HOME_MS && !lockLongPressBack) {
    longPressBackHandled = true;
    toggleHiddenFiles();
    return;
  }

  if (lockLongPressBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    lockLongPressBack = false;
    return;
  }

  const int pathReserved = renderer.getLineHeight(SMALL_FONT_ID) + UITheme::getInstance().getMetrics().verticalSpacing;
  int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false, pathReserved);
  const bool compactFileRows = SETTINGS.fileBrowserDisplay == CrossPointSettings::FILE_BROWSER_DISPLAY_2_LINES;
  if (compactFileRows) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight =
        renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing - pathReserved;
    pageItems = std::max(1, contentHeight / MinimalTheme::compactFileBrowserRowHeightFor(renderer));
  }

  if (!files.empty()) {
    const std::string& entry = files[selectorIndex];
    const bool isDirectory = (entry.back() == '/');
    if (mode == Mode::Books && !longPressConfirmHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        mappedInput.getHeldTime() >= GO_HOME_MS) {
      longPressConfirmHandled = true;
      if (isDirectory) {
        showDirectoryActionMenu(entry, true);
      } else {
        showFileActionMenu(entry, true);
      }
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (longPressConfirmHandled) {
      longPressConfirmHandled = false;
      return;
    }
    if (lockNextConfirmRelease) {
      lockNextConfirmRelease = false;
      return;
    }
    if (files.empty()) return;

    const std::string& entry = files[selectorIndex];
    bool isDirectory = (entry.back() == '/');

    // Firmware picker: select file -> return path; navigate into directories normally.
    if (mode == Mode::PickFirmware && !isDirectory) {
      std::string cleanBasePath = basepath;
      if (cleanBasePath.back() != '/') cleanBasePath += "/";
      ActivityResult res{FilePathResult{cleanBasePath + entry}};
      res.isCancelled = false;
      setResult(std::move(res));
      finish();
      return;
    }

    if (mode == Mode::Books && mappedInput.getHeldTime() >= GO_HOME_MS) {
      if (isDirectory) {
        showDirectoryActionMenu(entry);
      } else {
        showFileActionMenu(entry);
      }
      return;
    } else {
      // --- SHORT PRESS ACTION: OPEN/NAVIGATE ---
      if (basepath.back() != '/') basepath += "/";

      if (isDirectory) {
        basepath += entry.substr(0, entry.length() - 1);
        loadFiles();
        selectorIndex = 0;
        requestUpdate();
      } else {
        onSelectBook(basepath + entry);
      }
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (longPressBackHandled) {
      longPressBackHandled = false;
      return;
    }
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();

        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = findEntry(dirName);

        requestUpdate();
      } else if (mode == Mode::PickFirmware) {
        // Firmware picker at root: cancel back to caller instead of going home.
        ActivityResult res;
        res.isCancelled = true;
        setResult(std::move(res));
        finish();
      } else {
        onGoHome();
      }
    }
  }

  int listSize = static_cast<int>(files.size());
  buttonNavigator.onNextRelease([this, listSize] {
    selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, listSize] {
    selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
    requestUpdate();
  });
}

namespace {

std::string getFileName(std::string filename) {
  if (filename.back() == '/') {
    filename.pop_back();
    if (!UITheme::getInstance().getTheme().showsFileIcons()) {
      return "[" + filename + "]";
    }
    return filename;
  }
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

std::string getFileExtension(std::string filename) {
  if (filename.back() == '/') {
    return "";
  }
  const auto pos = filename.rfind('.');
  return filename.substr(pos);
}

}  // namespace

void FileBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  std::string folderName =
      (mode == Mode::PickFirmware)
          ? std::string(tr(STR_SELECT_FIRMWARE_FILE))
          : ((basepath == "/") ? std::string(tr(STR_SD_CARD)) : basepath.substr(basepath.rfind('/') + 1));
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName.c_str());

  const int pathLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int pathReserved = pathLineHeight + metrics.verticalSpacing;
  const int pathY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - pathLineHeight;
  const int pathMaxWidth = pageWidth - metrics.contentSidePadding * 2;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing - pathReserved;
  if (files.empty()) {
    const char* emptyMsg = fileListMemoryLimited
                               ? tr(STR_MEMORY_ERROR)
                               : ((mode == Mode::PickFirmware) ? tr(STR_NO_BIN_FILES) : tr(STR_NO_FILES_FOUND));
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, emptyMsg);
  } else {
    const bool compactFileRows = SETTINGS.fileBrowserDisplay == CrossPointSettings::FILE_BROWSER_DISPLAY_2_LINES;
    const std::function<std::string(int)> compactRowMarker =
        compactFileRows ? [this](int index) { return files[index].back() == '/' ? "folder" : ""; }
                        : std::function<std::string(int)>{};
    const auto rowTitle = [this](int index) { return getFileName(files[index]); };
    const auto rowIcon = [this](int index) { return UITheme::getFileIcon(files[index]); };
    const auto rowValue = [this](int index) {
      const std::string extension = SETTINGS.hideFileExtension != 0 ? std::string() : getFileExtension(files[index]);
      const std::string fullPath = buildFullPath(basepath, files[index]);
      if (files[index].back() == '/' && isPreferredSleepFolder(fullPath)) {
        return std::string("*");
      }
      if (isPinnedSleepFavorite(fullPath)) {
        return extension.empty() ? std::string("*") : "* " + extension;
      }
      return extension;
    };
    const Rect listRect{0, contentTop, pageWidth, contentHeight};
    if (compactFileRows) {
      MinimalTheme::drawCompactFileBrowserList(renderer, listRect, files.size(), selectorIndex, rowTitle,
                                               compactRowMarker, rowIcon, rowValue);
    } else {
      GUI.drawList(renderer, listRect, files.size(), selectorIndex, rowTitle, compactRowMarker, rowIcon, rowValue,
                   false);
    }
  }

  // Full path display
  {
    const int separatorY = pathY - metrics.verticalSpacing / 2;
    renderer.drawLine(0, separatorY, pageWidth - 1, separatorY, 3, true);
    // Left-truncate so the deepest directory is always visible
    const char* pathStr = basepath.c_str();
    const char* pathDisplay = pathStr;
    char leftTruncBuf[256];
    if (renderer.getTextWidth(SMALL_FONT_ID, pathStr) > pathMaxWidth) {
      const char ellipsis[] = "\xe2\x80\xa6";  // UTF-8 ellipsis (…)
      const int ellipsisWidth = renderer.getTextWidth(SMALL_FONT_ID, ellipsis);
      const int available = pathMaxWidth - ellipsisWidth;
      // Walk forward from the start until the suffix fits, skipping UTF-8 continuation bytes
      const char* p = pathStr;
      while (*p) {
        if (renderer.getTextWidth(SMALL_FONT_ID, p) <= available) break;
        ++p;
        while (*p && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
      }
      snprintf(leftTruncBuf, sizeof(leftTruncBuf), "%s%s", ellipsis, p);
      pathDisplay = leftTruncBuf;
    }
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, pathY, pathDisplay);
  }

  // Help text
  const char* backLabel = (basepath == "/") ? (mode == Mode::PickFirmware ? tr(STR_BACK) : tr(STR_HOME)) : tr(STR_BACK);
  // In PickFirmware mode, Confirm on a .bin returns the path to the caller (not "open"); show
  // STR_SELECT instead. Directories in the same picker still descend, so keep STR_OPEN there.
  const bool selectingFirmwareFile = mode == Mode::PickFirmware && !files.empty() && files[selectorIndex].back() != '/';
  const char* confirmLabel = files.empty() ? "" : (selectingFirmwareFile ? tr(STR_SELECT) : tr(STR_OPEN));
  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, files.empty() ? "" : tr(STR_DIR_UP),
                                            files.empty() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (mode == Mode::Books && basepath == "/") {
    const int usedPathWidth = renderer.getTextWidth(SMALL_FONT_ID, basepath.c_str());
    const int hintMaxWidth = pathMaxWidth - usedPathWidth - ROOT_HINT_GAP;
    const auto hint = renderer.truncatedText(SMALL_FONT_ID, tr(STR_TOGGLE_HIDDEN_FILES_HINT), hintMaxWidth);
    const int hintWidth = renderer.getTextWidth(SMALL_FONT_ID, hint.c_str());
    renderer.drawText(SMALL_FONT_ID, pageWidth - metrics.contentSidePadding - hintWidth, pathY, hint.c_str());
  }

  if (pendingCompletedFeedback) {
    GUI.drawPopup(renderer, completedFeedbackIsFinished ? tr(STR_MARKED_FINISHED) : tr(STR_MARKED_UNFINISHED));
  }

  renderer.displayBuffer();
}

size_t FileBrowserActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
