#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Txt.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

#include "../reader/BookReadingStats.h"
#include "../reader/BookStatsActivity.h"
#include "BookmarkStore.h"
#include "BookmarksHomeActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr uint32_t TXT_CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t TXT_CACHE_VERSION = 2;

float clampProgressPercent(const float progress) {
  return std::clamp(progress, 0.0f, 100.0f);
}

float loadEpubProgressPercent(const RecentBook& book) {
  Epub epub(book.path, "/.crosspoint");
  if (!epub.load(false, true)) {
    return -1.0f;
  }

  FsFile file;
  if (!Storage.openFileForRead("HOME", epub.getCachePath() + "/progress.bin", file)) {
    return -1.0f;
  }

  uint8_t data[6];
  const int bytesRead = file.read(data, sizeof(data));
  file.close();
  if (bytesRead != 6) {
    return -1.0f;
  }

  const int spineIndex = data[0] | (data[1] << 8);
  const int currentPage = data[2] | (data[3] << 8);
  const int pageCount = data[4] | (data[5] << 8);
  if (pageCount <= 0) {
    return 0.0f;
  }

  const float chapterProgress = static_cast<float>(currentPage + 1) / static_cast<float>(pageCount);
  return clampProgressPercent(epub.calculateProgress(spineIndex, chapterProgress) * 100.0f);
}

float loadXtcProgressPercent(const RecentBook& book) {
  Xtc xtc(book.path, "/.crosspoint");
  if (!xtc.load()) {
    return -1.0f;
  }

  FsFile file;
  if (!Storage.openFileForRead("HOME", xtc.getCachePath() + "/progress.bin", file)) {
    return -1.0f;
  }

  uint8_t data[4];
  const int bytesRead = file.read(data, sizeof(data));
  file.close();
  if (bytesRead != 4) {
    return -1.0f;
  }

  const uint32_t currentPage = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                               (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return clampProgressPercent(static_cast<float>(xtc.calculateProgress(currentPage)));
}

float loadTxtProgressPercent(const RecentBook& book) {
  Txt txt(book.path, "/.crosspoint");
  if (!txt.load()) {
    return -1.0f;
  }

  FsFile progressFile;
  if (!Storage.openFileForRead("HOME", txt.getCachePath() + "/progress.bin", progressFile)) {
    return -1.0f;
  }

  uint8_t progressData[4];
  const int progressBytes = progressFile.read(progressData, sizeof(progressData));
  progressFile.close();
  if (progressBytes != 4) {
    return -1.0f;
  }

  const uint32_t currentPage = static_cast<uint32_t>(progressData[0]) | (static_cast<uint32_t>(progressData[1]) << 8);

  FsFile indexFile;
  if (!Storage.openFileForRead("HOME", txt.getCachePath() + "/index.bin", indexFile)) {
    return -1.0f;
  }

  uint32_t magic = 0;
  serialization::readPod(indexFile, magic);
  uint8_t version = 0;
  serialization::readPod(indexFile, version);
  uint32_t fileSize = 0;
  serialization::readPod(indexFile, fileSize);
  int32_t cachedWidth = 0;
  serialization::readPod(indexFile, cachedWidth);
  int32_t cachedLines = 0;
  serialization::readPod(indexFile, cachedLines);
  int32_t fontId = 0;
  serialization::readPod(indexFile, fontId);
  int32_t margin = 0;
  serialization::readPod(indexFile, margin);
  uint8_t alignment = 0;
  serialization::readPod(indexFile, alignment);
  uint32_t totalPages = 0;
  serialization::readPod(indexFile, totalPages);
  indexFile.close();
  (void)cachedWidth;
  (void)cachedLines;
  (void)fontId;
  (void)margin;
  (void)alignment;

  if (magic != TXT_CACHE_MAGIC || version != TXT_CACHE_VERSION || fileSize != txt.getFileSize() || totalPages == 0) {
    return -1.0f;
  }

  return clampProgressPercent((static_cast<float>(currentPage + 1) / static_cast<float>(totalPages)) * 100.0f);
}

float loadRecentBookProgressPercent(const RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    return loadEpubProgressPercent(book);
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    return loadXtcProgressPercent(book);
  }
  if (FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path)) {
    return loadTxtProgressPercent(book);
  }
  return -1.0f;
}
}  // namespace

int HomeActivity::getMenuItemCount() const {
  int count = 4;  // File Browser, Recents, File transfer, Settings
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasOpdsUrl) {
    count++;
  }
  if (hasReadingStats) {
    count++;
  }
  if (hasBookmarks) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (!Storage.exists(book.path.c_str())) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub, try to load the metadata for title/author and cover
        if (FsHelpers::hasEpubExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success = epub.generateThumbBmp(coverHeight);
          if (!success) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = xtc.generateThumbBmp(coverHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  // Check if OPDS browser URL is configured
  hasOpdsUrl = strlen(SETTINGS.opdsServerUrl) > 0;

  // Check if any books have bookmarks (directory scan only, no file parsing)
  hasBookmarks = BookmarkStore::hasAnyBookmarks();

  selectorIndex = 0;

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);

  // Load reading stats for the most recent EPUB book so they can be shown on the home card.
  currentBookStats = BookReadingStats{};
  currentBookProgressPercent = -1.0f;
  if (!recentBooks.empty() && FsHelpers::hasEpubExtension(recentBooks[0].path)) {
    const std::string cachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(recentBooks[0].path));
    currentBookStats = BookReadingStats::load(cachePath);
  }
  if (!recentBooks.empty()) {
    currentBookProgressPercent = loadRecentBookProgressPercent(recentBooks[0]);
  }
  hasReadingStats = currentBookStats.sessionCount > 0;

  globalStats = GlobalReadingStats::load();

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  // Free any existing buffer first
  freeCoverBuffer();

  const size_t bufferSize = renderer.getBufferSize();
  coverBuffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (!coverBuffer) {
    return false;
  }

  memcpy(coverBuffer, frameBuffer, bufferSize);
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer) {
    return false;
  }

  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  const size_t bufferSize = renderer.getBufferSize();
  memcpy(frameBuffer, coverBuffer, bufferSize);
  return true;
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();

  buttonNavigator.onNext([this, menuCount] {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuCount] {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Calculate dynamic indices based on which options are available
    int idx = 0;
    int menuSelectedIndex = selectorIndex - static_cast<int>(recentBooks.size());
    const int fileBrowserIdx = idx++;
    const int recentsIdx = idx++;
    const int opdsLibraryIdx = hasOpdsUrl ? idx++ : -1;
    const int readingStatsIdx = hasReadingStats ? idx++ : -1;
    const int bookmarksIdx = hasBookmarks ? idx++ : -1;
    const int fileTransferIdx = idx++;
    const int settingsIdx = idx;

    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
    } else if (menuSelectedIndex == fileBrowserIdx) {
      onFileBrowserOpen();
    } else if (menuSelectedIndex == recentsIdx) {
      onRecentsOpen();
    } else if (menuSelectedIndex == opdsLibraryIdx) {
      onOpdsBrowserOpen();
    } else if (menuSelectedIndex == readingStatsIdx) {
      onReadingStatsOpen();
    } else if (menuSelectedIndex == bookmarksIdx) {
      onBookmarksOpen();
    } else if (menuSelectedIndex == fileTransferIdx) {
      onFileTransferOpen();
    } else if (menuSelectedIndex == settingsIdx) {
      onSettingsOpen();
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding}, nullptr);

  GUI.drawRecentBookCover(renderer, Rect{0, metrics.homeTopPadding, pageWidth, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this),
                          currentBookStats.sessionCount > 0 ? &currentBookStats : nullptr, currentBookProgressPercent);

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_MENU_RECENT_BOOKS), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Recent, Transfer, Settings};

  if (hasOpdsUrl) {
    // Insert OPDS Browser after File Browser
    menuItems.insert(menuItems.begin() + 2, tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Library);
  }

  if (hasReadingStats) {
    // Insert Reading Stats after OPDS (or after Recents if no OPDS), before File Transfer
    const int insertPos = hasOpdsUrl ? 3 : 2;
    menuItems.insert(menuItems.begin() + insertPos, tr(STR_READING_STATS));
    menuIcons.insert(menuIcons.begin() + insertPos, Chart);
  }

  if (hasBookmarks) {
    int insertPos = 2;
    if (hasOpdsUrl) insertPos++;
    if (hasReadingStats) insertPos++;
    menuItems.insert(menuItems.begin() + insertPos, tr(STR_BOOKMARKS));
    menuIcons.insert(menuIcons.begin() + insertPos, BookmarkIcon);
  }

  const int menuStartY = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.verticalSpacing;
  const int menuEndY = pageHeight - metrics.buttonHintsHeight;
  const int menuCount = static_cast<int>(menuItems.size());
  const int menuSelectedIndex = selectorIndex - static_cast<int>(recentBooks.size());

  GUI.drawButtonMenu(
      renderer,
      Rect{0, menuStartY, pageWidth,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing * 2 +
                         metrics.buttonHintsHeight)},
      menuCount, menuSelectedIndex, [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }
}

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onRecentsOpen() { activityManager.goToRecentBooks(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onReadingStatsOpen() {
  startActivityForResult(
      std::make_unique<BookStatsActivity>(renderer, mappedInput, recentBooks[0].title, currentBookStats, globalStats),
      [this](const ActivityResult&) { requestUpdate(); });
}

void HomeActivity::onBookmarksOpen() {
  startActivityForResult(std::make_unique<BookmarksHomeActivity>(renderer, mappedInput),
                         [this](const ActivityResult&) { requestUpdate(); });
}
