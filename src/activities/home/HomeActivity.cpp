#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <Serialization.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "../reader/BookReadingStats.h"
#include "../reader/BookStatsActivity.h"
#include "../reader/BookStatsTracking.h"
#include "../reader/EpubReaderUtils.h"
#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FilenameFontSystem.h"
#include "GlobalActions.h"
#include "MappedInputManager.h"
#include "RecentBookProgress.h"
#include "RecentBooksStore.h"
#include "SavedItemsHomeActivity.h"
#include "components/UITheme.h"
#include "components/themes/dashboard/DashboardTheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "components/themes/minimal/MinimalTheme.h"
#include "fontIds.h"

namespace {
constexpr uint32_t CAROUSEL_CACHE_MAGIC = 0x43434152;  // "CCAR"
// Cached frames contain only carousel artwork. Bump this whenever its
// rendering changes so stale snapshots are rebuilt after OTA.
constexpr uint16_t CAROUSEL_CACHE_VERSION = 7;
constexpr char LEGACY_CAROUSEL_CACHE_PATH[] = "/.crosspoint/home_carousel_cache.bin";
constexpr char CAROUSEL_CACHE_TMP_PATH[] = "/.crosspoint/home_carousel_cache.tmp";
constexpr uint32_t CAROUSEL_FRAME_MIN_FREE_AFTER_ALLOC = 64U * 1024U;
constexpr uint32_t CAROUSEL_FRAME_MIN_MAX_ALLOC_AFTER_ALLOC = 24U * 1024U;
constexpr unsigned long HOME_BOOK_SWAP_LONG_PRESS_MS = 1000;
constexpr int HOME_BOOK_SWAP_RECENT_COUNT = 2;

enum class HomeMenuAction {
  BrowseFiles,
  ContinueReading,
  Library,
  Plugins,
  ReadingStats,
  Bookmarks,
  FileTransfer,
  Settings,
};

struct HomeMenuEntry {
  const char* label;
  UIIcon icon;
  HomeMenuAction action;
};

struct HomeMenuEntries {
  static constexpr int kCapacity = 8;
  std::array<HomeMenuEntry, kCapacity> entries{};
  int count = 0;

  void push(const HomeMenuEntry& entry) {
    if (count >= kCapacity) return;
    entries[count++] = entry;
  }

  int size() const { return count; }

  const HomeMenuEntry& operator[](int index) const { return entries[index]; }
};

bool containsPoint(const Rect& rect, const int x, const int y) {
  return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

struct CarouselCacheHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t frameCount;
  uint32_t frameBufferSize;
  uint64_t keyHash;
  uint16_t screenWidth;
  uint16_t screenHeight;
  uint16_t centerCoverW;
  uint16_t centerCoverH;
  uint16_t sideCoverW;
  uint16_t sideCoverH;
};

uint64_t fnvHash64(const std::string& s) {
  uint64_t hash = 14695981039346656037ull;
  for (char c : s) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

bool hasAnyBookStats(const BookReadingStats& stats) {
  return stats.sessionCount > 0 || stats.totalReadingSeconds > 0 || stats.totalPagesTurned > 0 || stats.isCompleted ||
         stats.startDate.isValid() || stats.finishedDate.isValid();
}

bool hasAnyGlobalStats(const GlobalReadingStats& stats) {
  return stats.totalSessions > 0 || stats.totalReadingSeconds > 0 || stats.totalPagesTurned > 0 ||
         stats.completedBooks > 0 || stats.displayLongestReadingStreak() > 0;
}

bool hasHeapForCarouselFrameCache() {
  return ESP.getFreeHeap() >= CAROUSEL_FRAME_MIN_FREE_AFTER_ALLOC &&
         ESP.getMaxAllocHeap() >= CAROUSEL_FRAME_MIN_MAX_ALLOC_AFTER_ALLOC;
}

std::string getRecentBookCachePath(const RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub::resolveCachePathForFilePath(book.path, "/.crosspoint");
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    return "/.crosspoint/xtc_" + std::to_string(std::hash<std::string>{}(book.path));
  }
  if (FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path)) {
    return "/.crosspoint/txt_" + std::to_string(std::hash<std::string>{}(book.path));
  }
  return "";
}

BookReadingStats visibleRecentBookStats(const RecentBook& book, BookReadingStats stats) {
  const std::string cachePath = getRecentBookCachePath(book);
  if (!BookStatsTracking::isEnabled(cachePath)) {
    BookReadingStats paceOnly;
    paceOnly.setAverageForwardPageMillis(stats.averageForwardPageMillis());
    paceOnly.paceSampleCount = stats.paceSampleCount;
    paceOnly.estimatedTimeLeftSeconds = stats.estimatedTimeLeftSeconds;
    return paceOnly;
  }
  return stats;
}

BookReadingStats loadRecentBookStats(const RecentBook& book) {
  if (!FsHelpers::hasEpubExtension(book.path) && !FsHelpers::hasXtcExtension(book.path)) {
    return BookReadingStats{};
  }

  return visibleRecentBookStats(book, BookReadingStats::load(getRecentBookCachePath(book)));
}

float loadRecentBookProgress(const RecentBook& book) {
  return FsHelpers::hasEpubExtension(book.path) ? RecentBookProgress::loadCachedEpubPercent(book)
                                                : RecentBookProgress::loadPercent(book);
}

std::string loadEpubHighlightedChapterTitle(const RecentBook& book) {
  const std::string cachePath = getRecentBookCachePath(book);
  EpubReaderUtils::Progress progress;
  if (!EpubReaderUtils::loadProgressPath(cachePath + "/progress.bin", progress, "HOME")) {
    return {};
  }

  // This metadata owner contains several strings and file handles. Keep it off
  // the small activity stack, and never parse/index a book just to paint Home.
  auto metadata = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  if (!metadata) {
    LOG_ERR("HOME", "Cannot allocate chapter metadata");
    return {};
  }
  if (!metadata->load() || progress.spineIndex >= metadata->getSpineCount()) return {};
  const int tocIndex = metadata->getSpineEntry(progress.spineIndex).tocIndex;
  if (tocIndex < 0 || tocIndex >= metadata->getTocCount()) return {};
  return metadata->getTocEntry(tocIndex).title;
}

void updateRecentBookCover(const RecentBook& book) {
  if (!RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.coverBmpPath, book.coverState)) {
    LOG_ERR("HOME", "failed to update recent book metadata: %s", book.path.c_str());
  }
}

void markCoverMissing(RecentBook& book) {
  book.coverBmpPath.clear();
  book.coverState = RecentBook::CoverState::Missing;
  updateRecentBookCover(book);
}

bool hasThumbnailPlaceholder(const std::string& coverBmpPath) {
  return coverBmpPath.find("[WIDTH]") != std::string::npos || coverBmpPath.find("[HEIGHT]") != std::string::npos;
}

std::string getReusableCoverPath(const RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub(book.path, "/.crosspoint").getThumbBmpPath();
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    return Xtc(book.path, "/.crosspoint").getThumbBmpPath();
  }
  return book.coverBmpPath;
}

bool ensureReusableCoverPath(RecentBook& book) {
  if (book.coverState == RecentBook::CoverState::Missing || hasThumbnailPlaceholder(book.coverBmpPath)) {
    return false;
  }

  const std::string reusablePath = getReusableCoverPath(book);
  if (reusablePath.empty() || reusablePath == book.coverBmpPath) {
    return false;
  }

  book.coverBmpPath = reusablePath;
  updateRecentBookCover(book);
  return true;
}

const char* savedItemsLabel(bool hasBookmarks, bool hasClippings) {
  if (hasBookmarks && hasClippings) return tr(STR_BOOKMARKS_AND_CLIPPINGS);
  if (hasClippings) return tr(STR_CLIPPINGS);
  return tr(STR_BOOKMARKS);
}

void appendHomeMenuItems(HomeMenuEntries& items, bool hasPlugins, bool hasReadingStats, bool hasBookmarks,
                         bool hasClippings) {
  items.push({tr(STR_BROWSE_FILES), Folder, HomeMenuAction::BrowseFiles});
  items.push({tr(STR_LIBRARY), Library, HomeMenuAction::Library});

  if (hasPlugins) {
    items.push({tr(STR_PLUGINS), UIIcon::Plugins, HomeMenuAction::Plugins});
  }
  if (hasReadingStats) {
    items.push({tr(STR_READING_STATS), Chart, HomeMenuAction::ReadingStats});
  }
  if (hasBookmarks || hasClippings) {
    items.push({savedItemsLabel(hasBookmarks, hasClippings), BookmarkIcon, HomeMenuAction::Bookmarks});
  }

  items.push({tr(STR_FILE_TRANSFER), Transfer, HomeMenuAction::FileTransfer});
  items.push({tr(STR_SETTINGS_TITLE), Settings, HomeMenuAction::Settings});
}

HomeMenuEntries buildHomeMenuItems(bool hasPlugins, bool hasReadingStats, bool hasBookmarks, bool hasClippings) {
  HomeMenuEntries items;
  appendHomeMenuItems(items, hasPlugins, hasReadingStats, hasBookmarks, hasClippings);
  return items;
}

HomeMenuEntries buildMinimalMenuItems(bool hasPlugins, bool hasReadingStats, bool hasBookmarks, bool hasClippings) {
  HomeMenuEntries items;
  if (SETTINGS.isLibraryFileBrowserSwapped()) {
    items.push({tr(STR_BROWSE_FILES), Folder, HomeMenuAction::BrowseFiles});
  } else {
    items.push({tr(STR_LIBRARY), Library, HomeMenuAction::Library});
  }

  if (hasPlugins) {
    items.push({tr(STR_PLUGINS), UIIcon::Plugins, HomeMenuAction::Plugins});
  }
  if (hasBookmarks || hasClippings) {
    items.push({savedItemsLabel(hasBookmarks, hasClippings), BookmarkIcon, HomeMenuAction::Bookmarks});
  }
  if (hasReadingStats) {
    items.push({tr(STR_READING_STATS), Chart, HomeMenuAction::ReadingStats});
  }

  items.push({tr(STR_FILE_TRANSFER), Transfer, HomeMenuAction::FileTransfer});
  return items;
}

HomeMenuEntries buildSelectableHomeMenuItems(bool hasPlugins, bool hasReadingStats, bool hasBookmarks,
                                             bool hasClippings, bool includeContinueReading) {
  HomeMenuEntries items;
  if (includeContinueReading) {
    items.push({tr(STR_CONTINUE_READING), Book, HomeMenuAction::ContinueReading});
  }
  appendHomeMenuItems(items, hasPlugins, hasReadingStats, hasBookmarks, hasClippings);
  return items;
}

HomeMenuAction homeActionForInitialMenuItem(HomeMenuItem item) {
  switch (item) {
    case HomeMenuItem::FILE_BROWSER:
      return HomeMenuAction::BrowseFiles;
    case HomeMenuItem::LIBRARY:
      return HomeMenuAction::Library;
    case HomeMenuItem::PLUGINS:
      return HomeMenuAction::Plugins;
    case HomeMenuItem::FILE_TRANSFER:
      return HomeMenuAction::FileTransfer;
    case HomeMenuItem::SETTINGS_MENU:
      return HomeMenuAction::Settings;
    case HomeMenuItem::NONE:
    default:
      return HomeMenuAction::ContinueReading;
  }
}

int findMenuActionIndex(const HomeMenuEntries& items, HomeMenuAction action) {
  for (int i = 0; i < items.size(); ++i) {
    if (items[i].action == action) {
      return i;
    }
  }
  return -1;
}

bool isMinimalTheme() {
  return static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::MINIMAL;
}

bool isDashboardTheme() {
  return static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::DASHBOARD;
}

bool usesMinimalHomeInteraction() { return isMinimalTheme() || isDashboardTheme(); }

bool showMinimalHomeButtonHints(const MappedInputManager& mappedInput) { return !mappedInput.hasTouch(); }

bool isAnyFrontButtonPressed(const MappedInputManager& mappedInput) {
  return mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) ||
         mappedInput.isFrontButtonPressed(HalGPIO::BTN_CONFIRM) ||
         mappedInput.isFrontButtonPressed(HalGPIO::BTN_LEFT) || mappedInput.isFrontButtonPressed(HalGPIO::BTN_RIGHT);
}

int minimalHomeNavCount(const bool hasCurrentBook) { return hasCurrentBook ? 4 : 3; }

std::string exactHomeCoverPath(const RecentBook& book, const Rect& coverRect) {
  if (book.coverBmpPath.empty()) {
    return {};
  }
  if (FsHelpers::hasEpubExtension(book.path)) {
    return Epub(book.path, "/.crosspoint").getAdaptiveThumbBmpPath(coverRect.width, coverRect.height);
  }
  return UITheme::getCoverThumbPath(book.coverBmpPath, coverRect.width, coverRect.height, false);
}

void appendCarouselCoverStateToKey(std::string& key, const RecentBook& book) {
  key += book.path;
  key += '\0';
  key += book.title;
  key += '\0';
  key += book.coverBmpPath;
  key += '\0';

  if (book.coverBmpPath.empty()) {
    key += "0:0";
    key += '\0';
    return;
  }

  const std::string centerPath =
      UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kCenterThumbW, LyraCarouselTheme::kCenterThumbH);
  const std::string sidePath =
      UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH);
  key += Storage.exists(centerPath.c_str()) ? '1' : '0';
  key += ':';
  key += Storage.exists(sidePath.c_str()) ? '1' : '0';
  key += '\0';
}

void buildCarouselCacheKey(const std::vector<RecentBook>& recentBooks, const GfxRenderer& renderer, std::string& key,
                           uint64_t& keyHash) {
  key.clear();
  key.reserve(512);
  key += SETTINGS.filenameFallbackFont;
  key += '\0';
  key += std::to_string(filenameFontSystem.fingerprint());
  key += '\0';
  // Artwork includes Dark Mode's image-polarity correction. Progress, stats,
  // headers and menus are drawn live, so reading cannot invalidate this cache.
  key += SETTINGS.screenInverted ? "dark:1" : "dark:0";
  key += "safe-home:1";
  for (const auto edge : renderer.getViewableInsets().edges) key += static_cast<char>(edge);
  // Artwork positions follow the global header's reserved space.
  if (SETTINGS.displayStatusBarTextSize != 0) {
    key += "status-size:";
    key += static_cast<char>('0' + SETTINGS.displayStatusBarTextSize);
  }
  key += '\0';
  for (const auto& book : recentBooks) {
    appendCarouselCoverStateToKey(key, book);
  }
  keyHash = fnvHash64(key);
}

std::string carouselCachePath(int bookIdx) {
  char path[64];
  snprintf(path, sizeof(path), "/.crosspoint/home_carousel_cache_%d.bin", bookIdx);
  return path;
}

void invalidateCarouselDiskCache() {
  for (int i = 0; i < HomeActivity::kMaxCachedBooks; ++i) {
    const std::string path = carouselCachePath(i);
    if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
  }
  if (Storage.exists(LEGACY_CAROUSEL_CACHE_PATH)) Storage.remove(LEGACY_CAROUSEL_CACHE_PATH);
  if (Storage.exists(CAROUSEL_CACHE_TMP_PATH)) Storage.remove(CAROUSEL_CACHE_TMP_PATH);
}

bool isCarouselCacheHeaderValid(const CarouselCacheHeader& header, uint64_t cacheKeyHash, int bookCount,
                                const GfxRenderer& renderer) {
  return header.magic == CAROUSEL_CACHE_MAGIC && header.version == CAROUSEL_CACHE_VERSION &&
         header.keyHash == cacheKeyHash && header.frameCount == bookCount &&
         header.frameBufferSize == renderer.getBufferSize() && header.screenWidth == renderer.getScreenWidth() &&
         header.screenHeight == renderer.getScreenHeight() && header.centerCoverW == LyraCarouselTheme::kCenterThumbW &&
         header.centerCoverH == LyraCarouselTheme::kCenterThumbH &&
         header.sideCoverW == LyraCarouselTheme::kSideCoverW && header.sideCoverH == LyraCarouselTheme::kSideCoverH;
}

bool readCarouselCacheHeader(FsFile& file, CarouselCacheHeader& header) {
  CarouselCacheHeader readHeader{};
  if (!serialization::tryReadPod(file, readHeader)) {
    return false;
  }
  header = readHeader;
  return true;
}

bool hasValidCarouselDiskCache(const std::vector<RecentBook>& recentBooks, const GfxRenderer& renderer, int bookIdx) {
  const int bookCount = static_cast<int>(recentBooks.size());
  if (bookIdx < 0 || bookIdx >= bookCount) return false;

  std::string cacheKey;
  uint64_t cacheKeyHash = 0;
  buildCarouselCacheKey(recentBooks, renderer, cacheKey, cacheKeyHash);

  FsFile cacheFile;
  if (!Storage.openFileForRead("HOME", carouselCachePath(bookIdx), cacheFile)) {
    return false;
  }

  CarouselCacheHeader header{};
  const bool readOk = readCarouselCacheHeader(cacheFile, header);
  cacheFile.close();
  return readOk && isCarouselCacheHeaderValid(header, cacheKeyHash, bookCount, renderer);
}

int getVisibleRecentBookCount(const std::vector<RecentBook>& recentBooks) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return std::min(static_cast<int>(recentBooks.size()), metrics.homeRecentBooksCount);
}

int getHomeMenuSelectionOffset(const std::vector<RecentBook>& recentBooks) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.homeContinueReadingInMenu ? 0 : getVisibleRecentBookCount(recentBooks);
}

}  // namespace

// ---------------------------------------------------------------------------
// Static carousel frame cache — survives HomeActivity re-creation so that
// returning to home (e.g. after settings) doesn't re-read covers from SD.
// Freed explicitly in onSelectBook() before entering the reader.
// ---------------------------------------------------------------------------
namespace {
class CarouselCache {
 public:
  // One frame is 48 KB on current panels. Keep it out of the C3's constrained
  // internal heap whenever PSRAM is available; the owning buffers are static
  // because this cache deliberately survives HomeActivity recreation.
  HeapByteBuffer frameStorage[HomeActivity::kCarouselFrameCount];
  uint8_t* frames[HomeActivity::kCarouselFrameCount] = {};
  int frameBookIdx[HomeActivity::kCarouselFrameCount] = {-1};
  int frameCount = 0;
  int lastCenterIdx = -1;
  std::string key;
  uint64_t keyHash = 0;

  int findFrameSlot(int bookIdx) const {
    for (int i = 0; i < HomeActivity::kCarouselFrameCount; ++i) {
      if (frameBookIdx[i] == bookIdx && frames[i] != nullptr) return i;
    }
    return -1;
  }

  void invalidate() {
    for (int i = 0; i < HomeActivity::kCarouselFrameCount; ++i) {
      frameStorage[i].reset();
      frames[i] = nullptr;
      frameBookIdx[i] = -1;
    }
    frameCount = 0;
    lastCenterIdx = -1;
    key.clear();
    keyHash = 0;
  }
};

CarouselCache gCarouselCache;
}  // namespace

static_assert(HomeActivity::kMaxCachedBooks >= LyraCarouselMetrics::values.homeRecentBooksCount,
              "kMaxCachedBooks must cover all carousel slots");

int HomeActivity::getMenuItemCount() const {
  if (coverGridUi) return static_cast<int>(recentBooks.size()) + (4 + hasPlugins);
  const auto& metrics = UITheme::getInstance().getMetrics();
  int count = 4;  // File Browser, Library, File transfer, Settings
  if (!metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    count += getVisibleRecentBookCount();
  } else if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    count++;  // Continue Reading menu item
  }
  if (hasPlugins) count++;
  if (hasReadingStats) {
    count++;
  }
  if (hasBookmarks || hasClippings) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(coverGridUi ? maxBooks : std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& storedBook : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    RecentBook book = storedBook;
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    ensureReusableCoverPath(book);
    recentBooks.push_back(book);
  }
}

bool HomeActivity::loadCoverGridThumbnails() {
  recentsLoading = true;
  bool showingLoading = false;
  bool pathsChanged = false;
  Rect popupRect;
  for (size_t i = 0; i < recentBooks.size(); ++i) {
    auto& book = recentBooks[i];
    if (book.coverState == RecentBook::CoverState::Missing || !Storage.exists(book.path.c_str())) continue;
    const int width = coverGridUi->thumbWidthFor(i);
    const int height = coverGridUi->thumbHeightFor(i);
    if (book.coverBmpPath.empty()) {
      if (FsHelpers::hasEpubExtension(book.path)) {
        auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
        if (epub) book.coverBmpPath = epub->getThumbBmpPath();
      } else if (FsHelpers::hasXtcExtension(book.path)) {
        auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
        if (xtc) book.coverBmpPath = xtc->getThumbBmpPath();
      }
      pathsChanged = pathsChanged || !book.coverBmpPath.empty();
    }
    const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, width, height, false);
    if (thumbPath.empty()) continue;
    FsFile thumbFile;
    if (Storage.exists(thumbPath.c_str()) && Storage.openFileForRead("HOME", thumbPath, thumbFile)) {
      Bitmap thumb(thumbFile);
      const bool matches =
          thumb.parseHeaders() == BmpReaderError::Ok && thumb.getWidth() == width && thumb.getHeight() == height;
      thumbFile.close();
      if (matches) continue;
    }
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING));
    }
    // fillPopupProgress() refreshes the panel itself.
    GUI.fillPopupProgress(renderer, popupRect, static_cast<int>(100 * i / std::max<size_t>(1, recentBooks.size())));
    if (FsHelpers::hasEpubExtension(book.path)) {
      auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
      if (!epub) {
        LOG_ERR("HOME", "Cannot allocate EPUB for cover grid thumbnail");
        continue;
      }
      if (!epub->generateThumbBmpFromSource(width, height, &renderer, SETTINGS.getReaderFontId())) {
        LOG_ERR("HOME", "Cannot create cover grid thumbnail: %s", book.path.c_str());
      }
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
      if (!xtc) {
        LOG_ERR("HOME", "Cannot allocate XTC for cover grid thumbnail");
        continue;
      }
      if (xtc->load()) xtc->generateThumbBmp(static_cast<uint16_t>(width), static_cast<uint16_t>(height));
    }
  }
  recentsLoaded = true;
  recentsLoading = false;
  // The loading popup or newly resolved artwork must be replaced by a repaint.
  return showingLoading || pathsChanged;
}

void HomeActivity::loadAllBookStats() {
  const auto start = millis();
  const int count = std::min(static_cast<int>(recentBooks.size()), kMaxCachedBooks);
  for (int i = 0; i < count; ++i) {
    cachedBookStats[i] = loadRecentBookStats(recentBooks[i]);
    cachedBookProgress[i] = loadRecentBookProgress(recentBooks[i]);
  }
  bookStatsCached = true;
  LOG_DBG("HOME", "carousel: cached stats/progress for %d book(s) in %lums", count, millis() - start);
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;
  // Every thumbnail generation path shows progress before its ZIP/image work.
  auto showLoadingProgress = [&](const int value) {
    if (!showingLoading) {
      showingLoading = true;
      // Thumbnail generation may need a 32 KB contiguous inflate buffer. The
      // Home cover snapshot is only a redraw cache, so release it before ZIP
      // work. Keep it when every thumbnail already exists: navigation can then
      // restore the cover from RAM instead of decoding it from SD again.
      if (coverBuffer) {
        freeCoverBuffer();
        coverRendered = false;
      }
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING));
    }
    GUI.fillPopupProgress(renderer, popupRect, std::clamp(value, 0, 100));  // refreshes the panel itself
  };

  const bool isCarouselTheme =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;
  const bool isMinimal = isMinimalTheme();
  const bool isDashboard = isDashboardTheme();
  const Rect homeCover = isDashboard ? DashboardTheme::getHomeCoverRect(renderer)
                                     : (isMinimal ? MinimalTheme::getHomeCoverRect(renderer) : Rect{});
  const size_t recentBookCount = recentBooks.size();
  // Home only loads kMaxCachedBooks recents; fixed storage avoids an aborting std::vector allocation on low heap.
  std::array<char, kMaxCachedBooks> bookUpdated{};
  const int progressIncrement = 90 / static_cast<int>(std::max<size_t>(1, recentBookCount));

  int progress = 0;
  for (size_t bookIdx = 0; bookIdx < recentBooks.size(); ++bookIdx) {
    RecentBook& book = recentBooks[bookIdx];
    if (!Storage.exists(book.path.c_str())) {
      progress++;
      continue;
    }
    ensureReusableCoverPath(book);
    if (!book.coverBmpPath.empty()) {
      if (isCarouselTheme) {
        // For carousel: generate exact-size thumbnails for the center image rect and side slots.
        // Load the source image once even when both sizes are missing.
        const std::string centerPath = UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kCenterThumbW,
                                                                  LyraCarouselTheme::kCenterThumbH);
        const std::string sidePath = UITheme::getCoverThumbPath(book.coverBmpPath, LyraCarouselTheme::kSideCoverW,
                                                                LyraCarouselTheme::kSideCoverH);
        const bool centerMissing = !Storage.exists(centerPath.c_str());
        const bool sideMissing = !Storage.exists(sidePath.c_str());

        if (centerMissing || sideMissing) {
          if (FsHelpers::hasEpubExtension(book.path)) {
            Epub epub(book.path, "/.crosspoint");
            showLoadingProgress(10 + progress * progressIncrement);
            if (!epub.load(true, true, Epub::XLocationLoadMode::Skip)) {
              LOG_ERR("HOME", "carousel: failed to load EPUB cache for thumb generation: %s", book.path.c_str());
              coverRendered = false;
              requestUpdate();
              progress++;
              continue;
            }
            bool success = true;
            if (centerMissing)
              success = epub.generateThumbBmp(LyraCarouselTheme::kCenterThumbW, LyraCarouselTheme::kCenterThumbH,
                                              &renderer, SETTINGS.getReaderFontId()) &&
                        success;
            if (sideMissing)
              success = epub.generateThumbBmp(LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH, &renderer,
                                              SETTINGS.getReaderFontId()) &&
                        success;
            if (!success) {
              if (!epub.hasCoverImage()) markCoverMissing(book);
            } else if (bookIdx < bookUpdated.size()) {
              bookUpdated[bookIdx] = true;
            }
            coverRendered = false;
            requestUpdate();
          } else if (FsHelpers::hasXtcExtension(book.path)) {
            Xtc xtc(book.path, "/.crosspoint");
            if (xtc.load()) {
              showLoadingProgress(10 + progress * progressIncrement);
              bool success = true;
              if (centerMissing)
                success =
                    xtc.generateThumbBmp(LyraCarouselTheme::kCenterThumbW, LyraCarouselTheme::kCenterThumbH) && success;
              if (sideMissing)
                success =
                    xtc.generateThumbBmp(LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH) && success;
              if (success) {
                if (bookIdx < bookUpdated.size()) bookUpdated[bookIdx] = true;
              }
              coverRendered = false;
              requestUpdate();
            }
          }
        }
      } else {
        // Non-carousel: generate the active theme's thumbnail size.
        const bool supportsExactHomeThumb =
            FsHelpers::hasEpubExtension(book.path) || FsHelpers::hasXtcExtension(book.path);
        const bool useExactHomeThumb = (isDashboard || isMinimal) && supportsExactHomeThumb;
        // Never pass fully clipped bounds to converters: zero dimensions select
        // their default thumbnail size instead of the layout's image size.
        if (useExactHomeThumb && (homeCover.width <= 0 || homeCover.height <= 0)) continue;
        const std::string coverPath = useExactHomeThumb ? exactHomeCoverPath(book, homeCover)
                                                        : UITheme::getCoverThumbPath(book.coverBmpPath, coverHeight);
        if (coverPath.empty() || !Storage.exists(coverPath.c_str())) {
          if (FsHelpers::hasEpubExtension(book.path)) {
            Epub epub(book.path, "/.crosspoint");
            showLoadingProgress(10 + progress * progressIncrement);
            if (!epub.load(true, true, Epub::XLocationLoadMode::Skip)) {
              LOG_ERR("HOME", "failed to load EPUB cache for thumb generation: %s", book.path.c_str());
              coverRendered = false;
              requestUpdate();
              progress++;
              continue;
            }
            const bool success = useExactHomeThumb
                                     ? epub.generateAdaptiveThumbBmp(homeCover.width, homeCover.height, &renderer,
                                                                     SETTINGS.getReaderFontId())
                                     : epub.generateThumbBmp(0, coverHeight, &renderer, SETTINGS.getReaderFontId());
            if (!success) {
              if (!epub.hasCoverImage()) markCoverMissing(book);
            } else if (bookIdx < bookUpdated.size()) {
              bookUpdated[bookIdx] = true;  // non-carousel path reuses same tracking
            }
            coverRendered = false;
            requestUpdate();
          } else if (FsHelpers::hasXtcExtension(book.path)) {
            Xtc xtc(book.path, "/.crosspoint");
            if (xtc.load()) {
              showLoadingProgress(10 + progress * progressIncrement);
              const bool success = useExactHomeThumb ? xtc.generateThumbBmp(static_cast<uint16_t>(homeCover.width),
                                                                            static_cast<uint16_t>(homeCover.height))
                                                     : xtc.generateThumbBmp(coverHeight);
              if (success) {
                if (bookIdx < bookUpdated.size()) bookUpdated[bookIdx] = true;
              }
              coverRendered = false;
              requestUpdate();
            }
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;

  if (isCarouselTheme && std::any_of(bookUpdated.begin(), bookUpdated.end(), [](char updated) { return updated; })) {
    // A changed cover affects the centre and side artwork in every position.
    // Rebuild only the viewed position; other positions remain lazy.
    invalidateCarouselDiskCache();
    freeCarouselFrames();
    gCarouselCache.invalidate();
    preRenderCarouselFrames();
    requestUpdate();
  }
}

void HomeActivity::onEnter() {
  {
    RenderLock lock(*this);
    filenameFontSystem.ensureLoaded(renderer);
  }

  Activity::onEnter();

  hasPlugins = SETTINGS.pluginsEnabled != 0;
  if (UITheme::hasCoverGridHome()) {
    coverGridUi = makeUniqueNoThrow<CoverGridHomeUi>(renderer);
    if (!coverGridUi) LOG_ERR("HOME", "Cannot allocate cover grid UI; using standard Home");
  }
  const bool isCarouselTheme =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;

  // Check if any books have bookmarks (directory scan only, no file parsing)
  hasBookmarks = BookmarkStore::hasAnyBookmarks();
  hasClippings = ClippingStore::hasAnyClippings();

  selectorIndex = 0;
  lastCarouselBookIndex = 0;
  carouselCoverTouchDownIndex = -1;
  carouselCoverTouchDownWasSelected = false;
  carouselMenuTouchDownIndex = -1;
  minimalMenuOpen = false;
  minimalSuppressInitialFrontRelease = usesMinimalHomeInteraction();
  backPressSeen = false;
  minimalMenuIndex = 0;
  minimalHomeNavIndex = -1;
  carouselFramesReady = false;
  carouselWarmupPending = isCarouselTheme;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int recentBooksToLoad =
      coverGridUi ? CoverGridHomeUi::MAX_BOOKS
                  : std::min(kMaxCachedBooks, std::max(metrics.homeRecentBooksCount, HOME_BOOK_SWAP_RECENT_COUNT));
  RECENT_BOOKS.ensureLoaded();
  loadRecentBooks(recentBooksToLoad);
  gridHasContinueReading = !recentBooks.empty();

  const auto selectInitialBook = [this, &metrics](const std::string& path) {
    if (path.empty()) {
      return false;
    }

    for (int i = 0; i < static_cast<int>(recentBooks.size()); ++i) {
      if (recentBooks[i].path == path) {
        if (metrics.homeRecentBooksCount == 1 && i > 0 && !coverGridUi) {
          std::rotate(recentBooks.begin(), recentBooks.begin() + i, recentBooks.end());
          selectorIndex = 0;
          lastCarouselBookIndex = 0;
        } else {
          selectorIndex = i;
          lastCarouselBookIndex = i;
        }
        return true;
      }
    }
    return false;
  };

  if (!selectInitialBook(initialBookPath)) {
    selectInitialBook(APP_STATE.openEpubPath);
  }

  globalStats = GlobalReadingStats::load();
  showAllDevicesStats = GlobalReadingStats::hasSyncedStats();
  allDevicesGlobalStats = showAllDevicesStats ? GlobalReadingStats::loadAggregated(globalStats) : globalStats;
  if (isCarouselTheme) {
    loadAllBookStats();
  }
  updateHighlightedBookContext(false);

  if (coverGridUi) {
    const int base = static_cast<int>(recentBooks.size());
    switch (initialMenuItem) {
      case HomeMenuItem::FILE_BROWSER:
        selectorIndex = base;
        break;
      case HomeMenuItem::LIBRARY:
        selectorIndex = base + 1;
        break;
      case HomeMenuItem::PLUGINS:
        selectorIndex = base + 2;
        break;
      case HomeMenuItem::FILE_TRANSFER:
        selectorIndex = base + 2 + hasPlugins;
        break;
      case HomeMenuItem::SETTINGS_MENU:
        selectorIndex = base + 3 + hasPlugins;
        break;
      case HomeMenuItem::NONE:
        break;
    }
    coverGridUi->begin(recentBooks, hasPlugins, gridHasContinueReading,
                       gridHasContinueReading ? loadRecentBookProgress(recentBooks.front()) : -1.0f);
  } else if (initialMenuItem != HomeMenuItem::NONE) {
    const bool includeContinueReading = metrics.homeContinueReadingInMenu && !recentBooks.empty();
    const auto menuItems =
        buildSelectableHomeMenuItems(hasPlugins, hasReadingStats, hasBookmarks, hasClippings, includeContinueReading);
    const int menuIndex = findMenuActionIndex(menuItems, homeActionForInitialMenuItem(initialMenuItem));
    if (menuIndex >= 0) {
      selectorIndex = getHomeMenuSelectionOffset(recentBooks) + menuIndex;
    }
  }

  if (usesMinimalHomeInteraction() && initialMenuItem == HomeMenuItem::PLUGINS && hasPlugins) {
    minimalMenuOpen = true;
    const auto menuItems = buildMinimalMenuItems(hasPlugins, hasReadingStats, hasBookmarks, hasClippings);
    minimalMenuIndex = findMenuActionIndex(menuItems, HomeMenuAction::Plugins);
  }

  if (isCarouselTheme && hasValidCarouselDiskCache(recentBooks, renderer, getHighlightedBookIndex())) {
    preRenderCarouselFrames();
  }

  requestUpdate();
}

int HomeActivity::getHighlightedBookIndex() const {
  if (recentBooks.empty()) {
    return -1;
  }

  if (coverGridUi) {
    return selectorIndex < static_cast<int>(recentBooks.size()) ? selectorIndex : 0;
  }

  const int visibleBookCount = getVisibleRecentBookCount();
  const int highlightedBookIdx = (selectorIndex < visibleBookCount) ? selectorIndex : lastCarouselBookIndex;
  return std::clamp(highlightedBookIdx, 0, visibleBookCount - 1);
}

int HomeActivity::getVisibleRecentBookCount() const {
  return coverGridUi ? static_cast<int>(recentBooks.size()) : ::getVisibleRecentBookCount(recentBooks);
}

bool HomeActivity::canSwapHomeBook() const {
  return !coverGridUi && UITheme::getInstance().getMetrics().homeRecentBooksCount == 1 && recentBooks.size() > 1;
}

void HomeActivity::showNextRecentBookOnHome() {
  if (!canSwapHomeBook()) {
    return;
  }

  std::rotate(recentBooks.begin(), recentBooks.begin() + 1, recentBooks.end());
  selectorIndex = 0;
  lastCarouselBookIndex = 0;
  bookStatsCached = false;
  updateHighlightedBookContext();
  invalidateCoverCache();
  requestUpdate();
}

std::string HomeActivity::getCurrentBookPath() const {
  const int idx = getHighlightedBookIndex();
  return idx >= 0 ? recentBooks[idx].path : std::string{};
}

std::string HomeActivity::getCurrentBookTitle() const {
  const int idx = getHighlightedBookIndex();
  return idx >= 0 ? recentBooks[idx].title : std::string{};
}

std::unique_ptr<Activity> HomeActivity::createFrontlightReadingStatsActivity() {
  const std::string path = APP_STATE.openEpubPath;
  const bool validEpub = FsHelpers::hasEpubExtension(path) && Storage.exists(path.c_str());
  std::string title = tr(STR_READING_STATS);
  float progress = -1.0f;
  if (validEpub) {
    const auto recent = std::find_if(recentBooks.begin(), recentBooks.end(),
                                     [&path](const RecentBook& book) { return book.path == path; });
    if (recent != recentBooks.end()) {
      title = recent->title;
      progress = RecentBookProgress::loadCachedEpubPercent(*recent);
    } else {
      const size_t slash = path.find_last_of('/');
      title = slash == std::string::npos ? path : path.substr(slash + 1);
    }
  }
  const std::string cachePath = validEpub ? Epub::cachePathForFilePath(path, "/.crosspoint") : std::string{};
  const bool showBookStats = validEpub && BookStatsTracking::isEnabled(cachePath);
  const BookReadingStats bookStats = showBookStats ? BookReadingStats::load(cachePath) : BookReadingStats{};
  if (!SETTINGS.shouldTrackReadingStats()) return {};
  if (!showBookStats) {
    title = tr(STR_READING_STATS);
    progress = -1.0f;
  }
  const GlobalReadingStats deviceStats = GlobalReadingStats::load();
  auto activity = GlobalReadingStats::hasSyncedStats()
                      ? makeUniqueNoThrow<BookStatsActivity>(
                            renderer, mappedInput, title, showBookStats ? cachePath : std::string{}, bookStats,
                            progress, false, 0, deviceStats, GlobalReadingStats::loadAggregated(deviceStats))
                      : makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, title,
                                                             showBookStats ? cachePath : std::string{}, bookStats,
                                                             progress, false, 0, deviceStats);
  if (activity && showBookStats) activity->setSyncBookPath(path);
  return activity;
}

void HomeActivity::onFrontlightPanelOpened() {
  insetsBeforeFrontlightPanel = renderer.getViewableInsets();
  themeBeforeFrontlightPanel = SETTINGS.uiTheme;
  scaleBeforeFrontlightPanel = SETTINGS.uiScale;
  filenameFontBeforeFrontlightPanel = filenameFontSystem.fingerprint();
  statusSizeBeforeFrontlightPanel = SETTINGS.displayStatusBarTextSize;
  // Save the selection before changed theme metrics can reinterpret its index.
  initialBookPath = getCurrentBookPath();
}

void HomeActivity::onFrontlightPanelClosed() {
  if (themeBeforeFrontlightPanel != SETTINGS.uiTheme || scaleBeforeFrontlightPanel != SETTINGS.uiScale ||
      filenameFontBeforeFrontlightPanel != filenameFontSystem.fingerprint() ||
      statusSizeBeforeFrontlightPanel != SETTINGS.displayStatusBarTextSize ||
      insetsBeforeFrontlightPanel != renderer.getViewableInsets()) {
    // Drawer Settings keeps Home alive. Recreate its theme-specific controls,
    // cover snapshots and thumbnail loading state through the normal lifecycle.
    // ActivityManager owns the replacement; its heavy caches allocate onEnter,
    // after the outgoing Home has released its buffers in onExit.
    auto home = makeUniqueNoThrow<HomeActivity>(renderer, mappedInput, HomeMenuItem::NONE, HalDisplay::FAST_REFRESH,
                                                initialBookPath);
    if (home) {
      activityManager.replaceActivity(std::move(home));
      return;
    }
    LOG_ERR("HOME", "Cannot rebuild Home after layout change");
  }
  globalStats = GlobalReadingStats::load();
  showAllDevicesStats = GlobalReadingStats::hasSyncedStats();
  allDevicesGlobalStats = showAllDevicesStats ? GlobalReadingStats::loadAggregated(globalStats) : globalStats;
  bookStatsCached = false;
  updateHighlightedBookContext();
  requestUpdate();
}

void HomeActivity::updateHighlightedBookContext(const bool allowChapterTitleRead) {
  currentBookStats = BookReadingStats{};
  currentBookProgressPercent = -1.0f;
  currentBookChapterTitle.clear();

  const int idx = getHighlightedBookIndex();
  const bool useCachedStats = idx >= 0 && bookStatsCached && idx < kMaxCachedBooks;
  if (idx >= 0) {
    const RecentBook& book = recentBooks[idx];
    const bool isEpub = FsHelpers::hasEpubExtension(book.path);
    const bool loadChapterTitle = isDashboardTheme();
    if (useCachedStats) {
      currentBookStats = visibleRecentBookStats(book, cachedBookStats[idx]);
      currentBookProgressPercent = cachedBookProgress[idx];
      if (allowChapterTitleRead && loadChapterTitle && isEpub) {
        currentBookChapterTitle = loadEpubHighlightedChapterTitle(book);
      }
    } else {
      currentBookStats = loadRecentBookStats(book);
      currentBookProgressPercent = loadRecentBookProgress(book);
      if (isEpub && allowChapterTitleRead && loadChapterTitle) {
        currentBookChapterTitle = loadEpubHighlightedChapterTitle(book);
      }
    }
  }

  hasReadingStats =
      SETTINGS.shouldTrackReadingStats() && (hasAnyBookStats(currentBookStats) || hasAnyGlobalStats(globalStats) ||
                                             (showAllDevicesStats && hasAnyGlobalStats(allDevicesGlobalStats)));
}

void HomeActivity::onExit() {
  Activity::onExit();

  coverGridUi.reset();
  carouselMenuTouchDownIndex = -1;
  freeCoverBuffer();
  gCarouselCache.invalidate();
  freeCarouselFrames();
  carouselWarmupPending = false;
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  if (ESP.getFreeHeap() < needed || ESP.getMaxAllocHeap() < needed) {
    LOG_DBG("HOME", "Skipping cover buffer cache (%zu bytes, free=%u, maxAlloc=%u)", needed, ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  coverBufferInverted = SETTINGS.screenInverted != 0;
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::invalidateCoverCache() {
  coverRendered = false;
  freeCoverBuffer();
}

void HomeActivity::invalidatePolarityMismatchedCaches() {
  const bool darkModeEnabled = SETTINGS.screenInverted != 0;
  if (coverBufferStored && coverBufferInverted != darkModeEnabled) {
    invalidateCoverCache();
  }
  if (carouselFramesReady && carouselFramesInverted != darkModeEnabled) {
    freeCarouselFrames();
    gCarouselCache.invalidate();
    carouselWarmupPending = true;
  }
}

void HomeActivity::freeCarouselFrames() {
  // Instance pointers are aliases into the static cache — do not free here.
  for (int i = 0; i < kCarouselFrameCount; ++i) carouselFrames[i] = nullptr;
  carouselFramesReady = false;
}

bool HomeActivity::allocateCarouselFrameSlots(int targetFrameCount) {
  const size_t bufferSize = renderer.getBufferSize();
  const bool usePsram = psramHeapAvailable();
  int frameCount = 0;
  for (int attemptFrameCount = targetFrameCount; attemptFrameCount >= 1; --attemptFrameCount) {
    bool allocFailed = false;
    for (int i = 0; i < attemptFrameCount; ++i) {
      // This is a full framebuffer-sized cache, too large for stack/static
      // storage. PSRAM keeps the X4 Pro reader-exit path from fragmenting its
      // internal heap; C3 continues to use the guarded default heap path.
      auto frame = usePsram ? makePsramByteBufferNoThrow(bufferSize) : makeHeapByteBufferNoThrow(bufferSize);
      if (!frame) {
        LOG_ERR("HOME", "preRenderCarouselFrames: malloc failed for frame %d while allocating %d frame(s)", i,
                attemptFrameCount);
        allocFailed = true;
        break;
      }
      if (!usePsram && !hasHeapForCarouselFrameCache()) {
        LOG_INF("HOME", "carousel: low heap after frame cache alloc (%u free, %u maxAlloc); skipping cache",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        allocFailed = true;
        break;
      }
      gCarouselCache.frameStorage[i] = std::move(frame);
      gCarouselCache.frames[i] = gCarouselCache.frameStorage[i].get();
      gCarouselCache.frameBookIdx[i] = -1;
    }

    if (!allocFailed) {
      frameCount = attemptFrameCount;
      break;
    }

    for (int i = 0; i < attemptFrameCount; ++i) {
      gCarouselCache.frameStorage[i].reset();
      gCarouselCache.frames[i] = nullptr;
      gCarouselCache.frameBookIdx[i] = -1;
    }
  }

  if (frameCount == 0) {
    gCarouselCache.invalidate();
    return false;
  }

  gCarouselCache.frameCount = frameCount;
  LOG_INF("HOME", "carousel: frame cache capacity %d/%d (%s)", frameCount, targetFrameCount,
          usePsram ? "PSRAM" : "internal heap");
  return true;
}

void HomeActivity::renderCarouselFrameToCurrentBuffer(int bookIdx) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int homeTop = metrics.homeTopPadding + UITheme::getHeaderRect(renderer, 0).y - metrics.topPadding;
  // Frame snapshots are independent of the live Home cover state in the members.
  bool frameCoverRendered = false, frameCoverStored = false, frameBufferRestored = false;
  LyraCarouselTheme::setPreRenderIndex(bookIdx);
  renderer.clearScreen();
  // Snapshot the expensive artwork only. Fresh progress/stats and controls are
  // added after restoring it, without rereading or repainting the covers.
  GUI.drawRecentBookCover(renderer, Rect{safe.x, homeTop, safe.width, metrics.homeCoverTileHeight}, recentBooks,
                          static_cast<int>(recentBooks.size()), frameCoverRendered, frameCoverStored,
                          frameBufferRestored, []() { return true; });
}

bool HomeActivity::saveCarouselFrameToDisk(uint64_t cacheKeyHash, int bookCount, int bookIdx, int slotIdx) {
  if (slotIdx < 0 || slotIdx >= kCarouselFrameCount || !carouselFrames[slotIdx] || bookIdx < 0 ||
      bookIdx >= bookCount) {
    return false;
  }
  const std::string cachePath = carouselCachePath(bookIdx);

  Storage.mkdir("/.crosspoint");
  if (Storage.exists(CAROUSEL_CACHE_TMP_PATH)) {
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
  }

  FsFile file;
  if (!Storage.openFileForWrite("HOME", CAROUSEL_CACHE_TMP_PATH, file)) {
    return false;
  }

  const CarouselCacheHeader header = {
      CAROUSEL_CACHE_MAGIC,
      CAROUSEL_CACHE_VERSION,
      static_cast<uint16_t>(bookCount),
      static_cast<uint32_t>(renderer.getBufferSize()),
      cacheKeyHash,
      static_cast<uint16_t>(renderer.getScreenWidth()),
      static_cast<uint16_t>(renderer.getScreenHeight()),
      static_cast<uint16_t>(LyraCarouselTheme::kCenterThumbW),
      static_cast<uint16_t>(LyraCarouselTheme::kCenterThumbH),
      static_cast<uint16_t>(LyraCarouselTheme::kSideCoverW),
      static_cast<uint16_t>(LyraCarouselTheme::kSideCoverH),
  };
  if (!serialization::tryWritePod(file, header)) {
    file.close();
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
    LOG_ERR("HOME", "carousel: failed to write SD cache header");
    return false;
  }

  const auto start = millis();
  // Save only the viewed position. Other positions are prepared when selected,
  // so entering Home never renders or writes the entire carousel in advance.
  const bool writeFailed = file.write(carouselFrames[slotIdx], renderer.getBufferSize()) != renderer.getBufferSize();

  const bool syncOk = file.sync();
  file.close();

  if (writeFailed || !syncOk) {
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
    LOG_ERR("HOME", "carousel: failed to write SD cache snapshot");
    return false;
  }

  if (Storage.exists(cachePath.c_str())) {
    Storage.remove(cachePath.c_str());
  }
  if (!Storage.rename(CAROUSEL_CACHE_TMP_PATH, cachePath.c_str())) {
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
    LOG_ERR("HOME", "carousel: failed to promote SD cache snapshot");
    return false;
  }

  if (Storage.exists(LEGACY_CAROUSEL_CACHE_PATH)) Storage.remove(LEGACY_CAROUSEL_CACHE_PATH);
  LOG_DBG("HOME", "carousel: saved SD artwork for book %d in %lums", bookIdx, millis() - start);
  return true;
}

bool HomeActivity::loadCarouselFrameFromDisk(uint64_t cacheKeyHash, int bookCount, int bookIdx, int slotIdx) {
  if (slotIdx < 0 || slotIdx >= kCarouselFrameCount || !gCarouselCache.frames[slotIdx] || bookIdx < 0 ||
      bookIdx >= bookCount) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("HOME", carouselCachePath(bookIdx), file)) {
    return false;
  }

  CarouselCacheHeader header{};
  if (!readCarouselCacheHeader(file, header) ||
      !isCarouselCacheHeaderValid(header, cacheKeyHash, bookCount, renderer)) {
    file.close();
    return false;
  }

  const size_t expectedBytes = renderer.getBufferSize();
  size_t totalBytesRead = 0;
  while (totalBytesRead < expectedBytes) {
    const int bytesRead = file.read(gCarouselCache.frames[slotIdx] + totalBytesRead, expectedBytes - totalBytesRead);
    if (bytesRead <= 0) {
      break;
    }
    totalBytesRead += static_cast<size_t>(bytesRead);
  }
  file.close();
  if (totalBytesRead != expectedBytes) {
    LOG_ERR("HOME", "carousel: short read for slot %d (%zu/%zu bytes)", slotIdx, totalBytesRead, expectedBytes);
    return false;
  }

  gCarouselCache.frameBookIdx[slotIdx] = bookIdx;
  carouselFrames[slotIdx] = gCarouselCache.frames[slotIdx];
  return true;
}

int HomeActivity::chooseCarouselEvictionSlot(int centerIdx, int bookCount, std::optional<int> protectedBookIdx) const {
  for (int i = 0; i < kCarouselFrameCount; ++i) {
    if (gCarouselCache.frames[i] && gCarouselCache.frameBookIdx[i] < 0) {
      return i;
    }
  }

  int evictSlot = -1;
  int maxDist = -1;
  for (int i = 0; i < kCarouselFrameCount; ++i) {
    if (!gCarouselCache.frames[i]) continue;
    const int cachedBookIdx = gCarouselCache.frameBookIdx[i];
    if (protectedBookIdx.has_value() && cachedBookIdx == protectedBookIdx.value()) continue;
    const int diff = std::abs(cachedBookIdx - centerIdx);
    const int dist = std::min(diff, bookCount - diff);
    if (dist > maxDist) {
      maxDist = dist;
      evictSlot = i;
    }
  }
  return evictSlot;
}

void HomeActivity::preRenderCarouselFrames() {
  const int bookCount = static_cast<int>(recentBooks.size());
  if (bookCount == 0) return;

  // Build cache key from book paths plus thumb-asset availability so we don't
  // reuse a stale snapshot built before carousel-sized thumbs existed.
  std::string newKey;
  uint64_t newKeyHash = 0;
  buildCarouselCacheKey(recentBooks, renderer, newKey, newKeyHash);

  // Cache hit: same books in same order — reuse without any SD reads
  if (newKey == gCarouselCache.key && gCarouselCache.frameCount > 0) {
    for (int i = 0; i < gCarouselCache.frameCount; ++i) carouselFrames[i] = gCarouselCache.frames[i];
    carouselFramesReady = true;
    carouselFramesInverted = SETTINGS.screenInverted != 0;
    coverRendered = false;
    coverBufferStored = false;
    return;
  }

  // Cache miss: free old cache and re-render
  if (!renderer.getFrameBuffer()) return;
  freeCoverBuffer();  // reclaim 48KB before allocating frames
  gCarouselCache.invalidate();

  if (!allocateCarouselFrameSlots(1)) return;

  const int initialBookIdx = getHighlightedBookIndex();
  const bool loaded = loadCarouselFrameFromDisk(newKeyHash, bookCount, initialBookIdx, 0);
  if (!loaded) renderCarouselFrame(initialBookIdx, 0);
  gCarouselCache.lastCenterIdx = initialBookIdx;
  gCarouselCache.key = newKey;
  gCarouselCache.keyHash = newKeyHash;
  carouselFramesReady = true;
  carouselFramesInverted = SETTINGS.screenInverted != 0;
  coverRendered = false;
  coverBufferStored = false;
  if (!loaded) saveCarouselFrameToDisk(newKeyHash, bookCount, initialBookIdx, 0);
}

void HomeActivity::loop() {
  if (quickActionsLongPowerHandled) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Power)) {
      quickActionsLongPowerHandled = false;
    }
    return;
  }

  if (SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS &&
      mappedInput.isPressed(MappedInputManager::Button::Power) &&
      mappedInput.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration()) {
    quickActionsLongPowerHandled = true;
    handleShortcutAction(CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS);
    return;
  }

  if (quickActionsPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (coverGridUi) {
    const int touched = coverGridUi->selectedAction(mappedInput);
    if (coverGridUi->app.invalidated()) requestUpdate();
    if (touched >= 0 && touched < getMenuItemCount()) {
      selectorIndex = touched;
      activateCoverGridSelection();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) backPressSeen = true;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) && backPressSeen && gridHasContinueReading) {
      onSelectBook(recentBooks.front().path);
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateCoverGridSelection();
      return;
    }

    const int bookCount = static_cast<int>(recentBooks.size());
    const auto cycleBand = [this](const int base, const int count, const int dir) {
      if (count <= 0) return;
      const int current = selectorIndex - base;
      selectorIndex =
          base + (current < 0 || current >= count ? (dir > 0 ? 0 : count - 1) : (current + count + dir) % count);
      requestUpdate();
    };
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [&] { cycleBand(0, bookCount, -1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [&] { cycleBand(0, bookCount, 1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this, &cycleBand] {
      cycleBand(static_cast<int>(recentBooks.size()), 4 + hasPlugins, -1);
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this, &cycleBand] {
      cycleBand(static_cast<int>(recentBooks.size()), 4 + hasPlugins, 1);
    });
    return;
  }

  if (usesMinimalHomeInteraction()) {
    const int pressedFrontButton = mappedInput.getPressedFrontButton();
    const int releasedFrontButton = mappedInput.getReleasedFrontButton();

    if (minimalSuppressInitialFrontRelease) {
      if (releasedFrontButton >= 0) {
        minimalSuppressInitialFrontRelease = false;
        return;
      }
      if (isAnyFrontButtonPressed(mappedInput)) {
        return;
      }
      minimalSuppressInitialFrontRelease = false;
    }

    if (homeBookSwapLongPressHandled) {
      if (releasedFrontButton == HalGPIO::BTN_BACK || !mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK)) {
        homeBookSwapLongPressHandled = false;
      }
      return;
    }

    if (minimalMenuOpen) {
      const auto menuItems = buildMinimalMenuItems(hasPlugins, hasReadingStats, hasBookmarks, hasClippings);
      const int menuCount = static_cast<int>(menuItems.size());
      if (menuCount <= 0) {
        minimalMenuOpen = false;
        minimalHomeNavIndex = -1;
        requestUpdate();
        return;
      }

      if (minimalMenuIndex >= menuCount) {
        minimalMenuIndex = menuCount - 1;
      }

      auto activateMinimalMenuAction = [this, &menuItems]() {
        switch (menuItems[minimalMenuIndex].action) {
          case HomeMenuAction::BrowseFiles:
            onFileBrowserOpen();
            break;
          case HomeMenuAction::Library:
            onLibraryOpen();
            break;
          case HomeMenuAction::Plugins:
            onPluginsOpen();
            break;
          case HomeMenuAction::ReadingStats:
            onReadingStatsOpen();
            break;
          case HomeMenuAction::Bookmarks:
            onSavedItemsOpen();
            break;
          case HomeMenuAction::FileTransfer:
            onFileTransferOpen();
            break;
          case HomeMenuAction::ContinueReading:
          case HomeMenuAction::Settings:
            break;
        }
      };

      int touchedMenuIndex = -1;
      if (mappedInput.wasItemTouchedDown(touchedMenuIndex) && touchedMenuIndex >= 0 && touchedMenuIndex < menuCount) {
        if (minimalMenuIndex != touchedMenuIndex) {
          minimalMenuIndex = touchedMenuIndex;
          requestUpdate();
        }
        return;
      }
      if (mappedInput.wasItemTapped(touchedMenuIndex) && touchedMenuIndex >= 0 && touchedMenuIndex < menuCount) {
        minimalMenuIndex = touchedMenuIndex;
        activateMinimalMenuAction();
        return;
      }

      int touchX = 0;
      int touchY = 0;
      if (mappedInput.wasScreenTouchDown(touchX, touchY) &&
          !containsPoint(MinimalTheme::buttonMenuPanelRect(renderer, menuCount), touchX, touchY)) {
        minimalMenuOpen = false;
        minimalHomeNavIndex = -1;
        requestUpdate();
        return;
      }

      buttonNavigator.onPreviousPress([this, menuCount] {
        minimalMenuIndex = ButtonNavigator::previousIndex(minimalMenuIndex, menuCount);
        requestUpdate();
      });
      buttonNavigator.onNextPress([this, menuCount] {
        minimalMenuIndex = ButtonNavigator::nextIndex(minimalMenuIndex, menuCount);
        requestUpdate();
      });
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        minimalMenuOpen = false;
        minimalHomeNavIndex = -1;
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        activateMinimalMenuAction();
      }
      return;
    }

    switch (mappedInput.wasSwipe()) {
      case MappedInputManager::SwipeDir::Down:
        minimalHomeNavIndex = 2;
        onSettingsOpen();
        return;
      case MappedInputManager::SwipeDir::Right:
        minimalHomeNavIndex = 1;
        onMinimalBrowseOpen();
        return;
      case MappedInputManager::SwipeDir::Up:
        minimalHomeNavIndex = 0;
        minimalMenuOpen = true;
        minimalMenuIndex = 0;
        requestUpdate();
        return;
      case MappedInputManager::SwipeDir::Left:
        if (mappedInput.hasTouch() && canSwapHomeBook()) {
          showNextRecentBookOnHome();
          return;
        }
        break;
      case MappedInputManager::SwipeDir::None:
        break;
    }

    if (canSwapHomeBook() && mappedInput.isFrontButtonPressed(HalGPIO::BTN_BACK) &&
        mappedInput.getHeldTime() >= HOME_BOOK_SWAP_LONG_PRESS_MS) {
      homeBookSwapLongPressHandled = true;
      showNextRecentBookOnHome();
      return;
    }

    const int homeNavCount = minimalHomeNavCount(!recentBooks.empty());
    if (minimalHomeNavIndex >= homeNavCount) {
      minimalHomeNavIndex = homeNavCount - 1;
    }

    // Touch readers do not show the front-button hints, so retain their
    // existing side-button handling without moving the non-touch hint focus.
    if (mappedInput.hasTouch()) {
      if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
        minimalHomeNavIndex = minimalHomeNavIndex < 0
                                  ? homeNavCount - 1
                                  : ButtonNavigator::previousIndex(minimalHomeNavIndex, homeNavCount);
        requestUpdate();
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
        minimalHomeNavIndex =
            minimalHomeNavIndex < 0 ? 0 : ButtonNavigator::nextIndex(minimalHomeNavIndex, homeNavCount);
        requestUpdate();
        return;
      }
    }

    auto activateMinimalHomeNav = [this](int index) {
      switch (index) {
        case 0:
          minimalMenuOpen = true;
          minimalMenuIndex = 0;
          requestUpdate();
          break;
        case 1:
          onMinimalBrowseOpen();
          break;
        case 2:
          onSettingsOpen();
          break;
        case 3:
          onContinueReading();
          break;
      }
    };

    int touchedHomeNav = -1;
    if (mappedInput.wasItemTouchedDown(touchedHomeNav) && touchedHomeNav >= 0 && touchedHomeNav < homeNavCount) {
      if (minimalHomeNavIndex != touchedHomeNav) {
        minimalHomeNavIndex = touchedHomeNav;
        requestUpdate();
      }
      return;
    }
    int touchedBookIndex = -1;
    if (mappedInput.wasCoverTouchedDown(touchedBookIndex) && touchedBookIndex >= 0 && !recentBooks.empty()) {
      if (minimalHomeNavIndex != 3) {
        minimalHomeNavIndex = 3;
        requestUpdate();
      }
      return;
    }
    if (mappedInput.wasItemTapped(touchedHomeNav) && touchedHomeNav >= 0 && touchedHomeNav < homeNavCount) {
      minimalHomeNavIndex = touchedHomeNav;
      activateMinimalHomeNav(minimalHomeNavIndex);
      return;
    }
    if (mappedInput.wasCoverTapped(touchedBookIndex) && touchedBookIndex >= 0 && !recentBooks.empty()) {
      minimalHomeNavIndex = 3;
      onContinueReading();
      return;
    }

    if (releasedFrontButton == HalGPIO::BTN_BACK) {
      minimalHomeNavIndex = 0;
      activateMinimalHomeNav(minimalHomeNavIndex);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_CONFIRM) {
      minimalHomeNavIndex = 1;
      activateMinimalHomeNav(minimalHomeNavIndex);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_LEFT) {
      minimalHomeNavIndex = 2;
      activateMinimalHomeNav(minimalHomeNavIndex);
      return;
    }
    if (releasedFrontButton == HalGPIO::BTN_RIGHT) {
      if (!recentBooks.empty()) {
        minimalHomeNavIndex = 3;
        activateMinimalHomeNav(minimalHomeNavIndex);
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      onContinueReading();
      return;
    }
    return;
  }

  const bool isCarousel =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;
  const bool carouselTouchOnly = isCarousel && mappedInput.hasTouchHardware();
  const int previousHighlightedBookIdx = getHighlightedBookIndex();
  const int visibleBookCount = getVisibleRecentBookCount();
  const int carouselMenuItemCount =
      isCarousel ? static_cast<int>(buildHomeMenuItems(hasPlugins, hasReadingStats, hasBookmarks, hasClippings).size())
                 : 0;

  MappedInputManager::SwipeDir carouselSwipe = MappedInputManager::SwipeDir::None;
  int carouselSwipeStartX = 0;
  int carouselSwipeStartY = 0;
  int carouselSwipeEndX = 0;
  int carouselSwipeEndY = 0;
  const bool hasCarouselSwipe =
      carouselTouchOnly && mappedInput.wasSwipeWithPoints(carouselSwipe, carouselSwipeStartX, carouselSwipeStartY,
                                                          carouselSwipeEndX, carouselSwipeEndY);
  const bool carouselSwipeStartsInMenu =
      hasCarouselSwipe && containsPoint(LyraCarouselTheme::buttonMenuTouchRect(renderer, carouselMenuItemCount),
                                        carouselSwipeStartX, carouselSwipeStartY);
  if (hasCarouselSwipe && carouselMenuTouchDownIndex >= 0) {
    carouselMenuTouchDownIndex = -1;
    requestUpdate();
  }

  // A touch swipe can also satisfy the generic Back gesture. Keep it in the
  // carousel path so a left-edge swipe cannot open the selected book instead.
  if (!hasCarouselSwipe && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    backPressSeen = true;
  }

  // Minimal and Dashboard already returned through their dedicated home
  // interaction path above. On other themes, Back opens the most recent book.
  // Requiring a press observed on Home ignores the stale release that can
  // arrive after Back closed the previous activity.
  if (!carouselTouchOnly && !hasCarouselSwipe && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      backPressSeen && !recentBooks.empty()) {
    onContinueReading();
    return;
  }

  auto activateHomeMenuAction = [this](const HomeMenuAction action) {
    switch (action) {
      case HomeMenuAction::BrowseFiles:
        onFileBrowserOpen();
        break;
      case HomeMenuAction::ContinueReading:
        onContinueReading();
        break;
      case HomeMenuAction::Library:
        onLibraryOpen();
        break;
      case HomeMenuAction::Plugins:
        onPluginsOpen();
        break;
      case HomeMenuAction::ReadingStats:
        onReadingStatsOpen();
        break;
      case HomeMenuAction::Bookmarks:
        onSavedItemsOpen();
        break;
      case HomeMenuAction::FileTransfer:
        onFileTransferOpen();
        break;
      case HomeMenuAction::Settings:
        onSettingsOpen();
        break;
    }
  };

  auto activateSelectedHomeItem = [this, visibleBookCount, &activateHomeMenuAction]() {
    const auto& metrics = UITheme::getInstance().getMetrics();
    if (!metrics.homeContinueReadingInMenu && selectorIndex < visibleBookCount) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }

    auto menuItems = buildSelectableHomeMenuItems(hasPlugins, hasReadingStats, hasBookmarks, hasClippings,
                                                  metrics.homeContinueReadingInMenu && !recentBooks.empty());
    const int menuSelectedIndex = selectorIndex - getHomeMenuSelectionOffset(recentBooks);
    if (menuSelectedIndex < 0 || menuSelectedIndex >= static_cast<int>(menuItems.size())) {
      return;
    }

    activateHomeMenuAction(menuItems[menuSelectedIndex].action);
  };

  if (homeBookSwapLongPressHandled) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      homeBookSwapLongPressHandled = false;
    }
    return;
  }

  if (!isCarousel && canSwapHomeBook() && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= HOME_BOOK_SWAP_LONG_PRESS_MS) {
    homeBookSwapLongPressHandled = true;
    showNextRecentBookOnHome();
    return;
  }

  if (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA &&
      mappedInput.hasTouch() && canSwapHomeBook() && mappedInput.wasSwipe() == MappedInputManager::SwipeDir::Left) {
    showNextRecentBookOnHome();
    return;
  }

  if (isCarousel) {
    const int bookCount = visibleBookCount;
    const int menuItemCount = carouselMenuItemCount;
    bool inCarouselRow = (selectorIndex < bookCount);
    const int menuIdx = inCarouselRow ? 0 : (selectorIndex - bookCount);

    auto handleTouch = [&](const bool activate) {
      int touchedMenuIndex = -1;
      if (!activate && mappedInput.wasItemTouchedDown(touchedMenuIndex)) {
        if (touchedMenuIndex < 0 || touchedMenuIndex >= menuItemCount) return false;
        carouselMenuTouchDownIndex = touchedMenuIndex;
        requestUpdate();
        return true;
      }
      if (activate && mappedInput.wasItemTapped(touchedMenuIndex)) {
        if (touchedMenuIndex < 0 || touchedMenuIndex >= menuItemCount) return false;
        carouselMenuTouchDownIndex = -1;
        const auto menuItems = buildHomeMenuItems(hasPlugins, hasReadingStats, hasBookmarks, hasClippings);
        activateHomeMenuAction(menuItems[touchedMenuIndex].action);
        return true;
      }

      int bookIndex = -1;
      if (bookCount > 0 &&
          (activate ? mappedInput.wasCoverTapped(bookIndex) : mappedInput.wasCoverTouchedDown(bookIndex))) {
        bookIndex = std::clamp(bookIndex, 0, bookCount - 1);
        const int previousSelectorIndex = selectorIndex;
        const bool wasSelectedAtTouchStart = !activate && inCarouselRow && previousSelectorIndex == bookIndex;
        const bool shouldActivateBook =
            activate && ((carouselCoverTouchDownIndex == bookIndex && carouselCoverTouchDownWasSelected) ||
                         (carouselCoverTouchDownIndex < 0 && inCarouselRow && previousSelectorIndex == bookIndex));
        selectorIndex = bookIndex;
        lastCarouselBookIndex = bookIndex;
        if (!activate) {
          carouselCoverTouchDownIndex = bookIndex;
          carouselCoverTouchDownWasSelected = wasSelectedAtTouchStart;
        } else {
          carouselCoverTouchDownIndex = -1;
          carouselCoverTouchDownWasSelected = false;
        }
        if (selectorIndex != previousSelectorIndex) {
          invalidateCoverCache();
          // Touch-down returns early so the selected cover can repaint before
          // the finger lifts; keep the stats/context used by the next action
          // in sync with that new selection.
          updateHighlightedBookContext(false);
        }
        if (shouldActivateBook) {
          activateSelectedHomeItem();
        } else if (selectorIndex != previousSelectorIndex) {
          requestUpdate();
        }
        return true;
      }
      return false;
    };

    if (!hasCarouselSwipe && handleTouch(/*activate=*/false)) {
      return;
    }
    if (!hasCarouselSwipe && handleTouch(/*activate=*/true)) {
      return;
    }
    // A release outside the icon strip (including a cancelled tap) must clear
    // the transient touch highlight without changing carousel selection.
    if (!hasCarouselSwipe && carouselMenuTouchDownIndex >= 0 && mappedInput.wasScreenTouchReleased()) {
      carouselMenuTouchDownIndex = -1;
      requestUpdate();
      return;
    }

    auto moveRight = [&]() {
      if (inCarouselRow && bookCount > 0) {
        selectorIndex = (selectorIndex + 1) % bookCount;
        lastCarouselBookIndex = selectorIndex;
      } else if (!inCarouselRow) {
        selectorIndex = bookCount + (menuIdx + 1) % menuItemCount;
      }
      requestUpdate();
    };
    auto moveLeft = [&]() {
      if (inCarouselRow && bookCount > 0) {
        selectorIndex = (selectorIndex + bookCount - 1) % bookCount;
        lastCarouselBookIndex = selectorIndex;
      } else if (!inCarouselRow) {
        selectorIndex = bookCount + (menuIdx + menuItemCount - 1) % menuItemCount;
      }
      requestUpdate();
    };

    bool handledHorizontalNav = false;
    if (hasCarouselSwipe) {
      // A swipe that starts in the icon strip is consumed. It must not select
      // an icon or turn into carousel navigation as it passes through one.
      if (carouselSwipeStartsInMenu) return;

      switch (carouselSwipe) {
        case MappedInputManager::SwipeDir::Left:
        case MappedInputManager::SwipeDir::Right:
          if (bookCount <= 0) return;
          if (!inCarouselRow) {
            selectorIndex = std::clamp(lastCarouselBookIndex, 0, bookCount - 1);
            lastCarouselBookIndex = selectorIndex;
            inCarouselRow = true;
            invalidateCoverCache();
          }
          if (carouselSwipe == MappedInputManager::SwipeDir::Left) {
            moveRight();
          } else {
            moveLeft();
          }
          handledHorizontalNav = true;
          break;
        case MappedInputManager::SwipeDir::None:
        case MappedInputManager::SwipeDir::Up:
        case MappedInputManager::SwipeDir::Down:
          break;
      }
    }

    if (!carouselTouchOnly) {
      if (!handledHorizontalNav && mappedInput.wasPressed(MappedInputManager::Button::Right)) {
        moveRight();
      }
      if (!handledHorizontalNav && mappedInput.wasPressed(MappedInputManager::Button::Left)) {
        moveLeft();
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
        if (inCarouselRow) {
          lastCarouselBookIndex = selectorIndex;
          selectorIndex = bookCount;
          invalidateCoverCache();
        } else {
          selectorIndex = lastCarouselBookIndex;
          invalidateCoverCache();
        }
        requestUpdate();
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
        if (inCarouselRow) {
          lastCarouselBookIndex = selectorIndex;
          selectorIndex = bookCount;
          invalidateCoverCache();
        } else {
          selectorIndex = lastCarouselBookIndex;
          invalidateCoverCache();
        }
        requestUpdate();
      }
    }
  } else {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto menuItems = buildSelectableHomeMenuItems(hasPlugins, hasReadingStats, hasBookmarks, hasClippings,
                                                        metrics.homeContinueReadingInMenu && !recentBooks.empty());
    auto handleTouch = [&](const bool activate) {
      int touchedBookIndex = -1;
      if (activate ? mappedInput.wasCoverTapped(touchedBookIndex) : mappedInput.wasCoverTouchedDown(touchedBookIndex)) {
        if (touchedBookIndex < 0 || touchedBookIndex >= visibleBookCount) return false;
        const int previousSelectorIndex = selectorIndex;
        selectorIndex = metrics.homeContinueReadingInMenu ? 0 : touchedBookIndex;
        if (activate) {
          activateSelectedHomeItem();
        } else if (selectorIndex != previousSelectorIndex) {
          requestUpdate();
        }
        return true;
      }

      int touchedMenuIndex = -1;
      if (activate ? mappedInput.wasItemTapped(touchedMenuIndex) : mappedInput.wasItemTouchedDown(touchedMenuIndex)) {
        if (touchedMenuIndex < 0 || touchedMenuIndex >= static_cast<int>(menuItems.size())) return false;
        const int previousSelectorIndex = selectorIndex;
        selectorIndex = getHomeMenuSelectionOffset(recentBooks) + touchedMenuIndex;
        if (activate) {
          activateSelectedHomeItem();
        } else if (selectorIndex != previousSelectorIndex) {
          requestUpdate();
        }
        return true;
      }
      return false;
    };

    if (handleTouch(/*activate=*/false)) {
      return;
    }
    if (handleTouch(/*activate=*/true)) {
      return;
    }

    const int menuCount = getMenuItemCount();
    buttonNavigator.onNext([this, menuCount] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, menuCount] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
    });
  }

  if (getHighlightedBookIndex() != previousHighlightedBookIdx) {
    updateHighlightedBookContext();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !(isCarousel && mappedInput.hasTouchHardware())) {
    activateSelectedHomeItem();
  }
}

void HomeActivity::activateCoverGridSelection() {
  if (selectorIndex < 0) return;
  if (selectorIndex < static_cast<int>(recentBooks.size())) {
    onSelectBook(recentBooks[selectorIndex].path);
    return;
  }
  const int tab = selectorIndex - static_cast<int>(recentBooks.size());
  switch (tab) {
    case 0:
      onFileBrowserOpen();
      break;
    case 1:
      onLibraryOpen();
      break;
    case 2:
      if (hasPlugins) {
        onPluginsOpen();
      } else {
        onFileTransferOpen();
      }
      break;
    case 3:
      if (hasPlugins) {
        onFileTransferOpen();
      } else {
        onSettingsOpen();
      }
      break;
    case 4:
      if (hasPlugins) onSettingsOpen();
      break;
  }
}

bool HomeActivity::handleShortcutAction(const CrossPointSettings::SHORT_PWRBTN action) {
  if (action == CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER) {
    onFileBrowserOpen();
    return true;
  }

  if (action != CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS) {
    return false;
  }

  if (quickActionsLongPowerHandled && mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    quickActionsLongPowerHandled = false;
    return true;
  }

  QuickActions::showConfiguredPopup(
      quickActionsPopup, [this] { requestUpdate(); },
      [this](const auto selectedAction) {
        if (selectedAction == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH) {
          // OptionPopup has already dismissed itself. Repaint Home before flushing
          // so the full refresh cannot preserve the popup in the panel image.
          initialRefreshMode = HalDisplay::FULL_REFRESH;
          requestUpdate();
          return;
        }
        dispatchShortcutAction(selectedAction);
      },
      [](const auto selectedAction) {
        return isPowerButtonActionAvailableOutsideReader(selectedAction) ||
               selectedAction == CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER;
      });
  return true;
}

void HomeActivity::render(RenderLock&&) {
  if (quickActionsPopup.processRender(renderer, mappedInput)) {
    return;
  }

  invalidatePolarityMismatchedCaches();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int homeTop = metrics.homeTopPadding + UITheme::getHeaderRect(renderer, 0).y - metrics.topPadding;
  const auto displayHomeBuffer = [this] {
    renderer.displayBuffer(initialRefreshMode);
    initialRefreshMode = HalDisplay::FAST_REFRESH;
  };

  if (coverGridUi) {
    renderer.clearScreen();
    coverGridUi->setSelection(selectorIndex);
    coverGridUi->render();
    const auto labels = mappedInput.mapLabels(gridHasContinueReading ? tr(STR_READ) : "", tr(STR_SELECT),
                                              tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    displayHomeBuffer();

    // Layout records each slot's thumbnail size, and its cover path, before
    // painting it. New sizes only mean the thumbnails must be checked.
    if (coverGridUi->takeThumbHeightsChanged()) recentsLoaded = false;
    // The panel already shows Home; repaint only for new artwork or to clear
    // the progress popup, rather than repeating an identical frame.
    if (!recentsLoaded && !recentsLoading && loadCoverGridThumbnails()) {
      coverGridUi->refreshCoverPaths();
      requestUpdate();
    }
    return;
  }

  if (usesMinimalHomeInteraction()) {
    renderer.clearScreen();

    if (minimalMenuOpen) {
      GUI.drawHeader(renderer, UITheme::getHeaderRect(renderer, metrics.homeTopPadding), nullptr);
      const auto menuItems = buildMinimalMenuItems(hasPlugins, hasReadingStats, hasBookmarks, hasClippings);
      GUI.drawButtonMenu(
          renderer, Rect{safe.x, homeTop, safe.width, std::max(0, safe.y + safe.height - homeTop)},
          static_cast<int>(menuItems.size()), minimalMenuIndex,
          [&menuItems](int index) { return menuItems[index].label; },
          [&menuItems](int index) { return menuItems[index].icon; });
      if (showMinimalHomeButtonHints(mappedInput)) {
        const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT),
                                                  tr(STR_DIR_UP), tr(STR_DIR_DOWN));
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      }
      displayHomeBuffer();
      return;
    }

    bool bufferRestored = coverBufferStored && restoreCoverBuffer();
    GUI.drawHeader(renderer, UITheme::getHeaderRect(renderer, metrics.homeTopPadding), nullptr);

    coverRectX = safe.x;
    coverRectY = homeTop;
    coverRectW = safe.width;
    coverRectH = metrics.homeCoverTileHeight;

    GUI.drawRecentBookCover(renderer, Rect{safe.x, homeTop, safe.width, metrics.homeCoverTileHeight}, recentBooks,
                            selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                            std::bind(&HomeActivity::storeCoverBuffer, this),
                            (isDashboardTheme() || hasAnyBookStats(currentBookStats)) ? &currentBookStats : nullptr,
                            currentBookProgressPercent, &globalStats, currentBookChapterTitle.c_str());

    const int homeNavCount = minimalHomeNavCount(!recentBooks.empty());
    if (minimalHomeNavIndex >= homeNavCount) {
      minimalHomeNavIndex = homeNavCount - 1;
    }
    if (showMinimalHomeButtonHints(mappedInput)) {
      MinimalTheme::setHomeButtonHintSelection(minimalHomeNavIndex);
      GUI.drawButtonHints(renderer, tr(STR_MENU),
                          SETTINGS.isLibraryFileBrowserSwapped() ? tr(STR_LIBRARY) : tr(STR_BROWSE),
                          tr(STR_SETTINGS_SHORT), recentBooks.empty() ? "" : tr(STR_READ));
    }

    displayHomeBuffer();

    if (!recentsLoaded && !recentsLoading) {
      recentsLoading = true;
      loadRecentCovers(metrics.homeCoverHeight);
    }
    return;
  }

  // Fast path: restore artwork and draw current progress and controls.
  if (carouselFramesReady) {
    uint8_t* frameBuffer = renderer.getFrameBuffer();
    const int bookCount = static_cast<int>(recentBooks.size());
    const bool inCarouselRow = (selectorIndex < bookCount);
    const int centerIdx = inCarouselRow ? selectorIndex : lastCarouselBookIndex;
    int slotIdx = gCarouselCache.findFrameSlot(centerIdx);
    bool saveViewedFrame = false;

    if (frameBuffer && slotIdx < 0 && gCarouselCache.keyHash != 0 && bookCount > 0) {
      const int evictSlot = chooseCarouselEvictionSlot(centerIdx, bookCount);
      if (evictSlot >= 0) {
        if (!loadCarouselFrameFromDisk(gCarouselCache.keyHash, bookCount, centerIdx, evictSlot)) {
          renderCarouselFrame(centerIdx, evictSlot);
          saveViewedFrame = true;
        }
        slotIdx = evictSlot;
      }
    }

    if (frameBuffer && slotIdx >= 0 && carouselFrames[slotIdx]) {
      memcpy(frameBuffer, carouselFrames[slotIdx], renderer.getBufferSize());
      LyraCarouselTheme::setPreRenderIndex(centerIdx);

      // The snapshot contains artwork only; all changing UI uses current state.
      const Rect coverRect{safe.x, homeTop, safe.width, metrics.homeCoverTileHeight};
      const auto& carouselTheme = static_cast<const LyraCarouselTheme&>(GUI);
      GUI.drawHeader(renderer, UITheme::getHeaderRect(renderer, metrics.homeTopPadding), nullptr);
      GUI.drawCarouselBorder(renderer, coverRect, recentBooks, centerIdx, inCarouselRow);
      carouselTheme.drawReadingProgress(renderer, coverRect, recentBooks,
                                        hasAnyBookStats(currentBookStats) ? &currentBookStats : nullptr,
                                        currentBookProgressPercent);
      const auto menuItems = buildHomeMenuItems(hasPlugins, hasReadingStats, hasBookmarks, hasClippings);
      const int menuHighlightIndex = mappedInput.hasTouchHardware()
                                         ? carouselMenuTouchDownIndex
                                         : (inCarouselRow ? -1 : selectorIndex - static_cast<int>(recentBooks.size()));
      GUI.drawButtonMenu(
          renderer, Rect{}, static_cast<int>(menuItems.size()), menuHighlightIndex,
          [&menuItems](int index) { return menuItems[index].label; },
          [&menuItems](int index) { return menuItems[index].icon; });
      const auto labels = mappedInput.mapLabels(tr(STR_READ), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

      displayHomeBuffer();
      if (saveViewedFrame) saveCarouselFrameToDisk(gCarouselCache.keyHash, bookCount, centerIdx, slotIdx);
      // Mirror the slow path: Home is already on the panel before SD work starts.
      if (!recentsLoaded && !recentsLoading) {
        recentsLoading = true;
        loadRecentCovers(metrics.homeCoverHeight);
      }
      return;
    }
  }

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  auto menuItems = buildSelectableHomeMenuItems(hasPlugins, hasReadingStats, hasBookmarks, hasClippings,
                                                metrics.homeContinueReadingInMenu && !recentBooks.empty());
  int homeCoverTileHeight = metrics.homeCoverTileHeight;
  if (SETTINGS.uiTheme == CrossPointSettings::UI_THEME::CLASSIC) {
    // Keep the four always-present actions clear of the button-hint strip on
    // shorter displays; any optional actions paginate below them.
    const int menuRows = std::min(4, static_cast<int>(menuItems.size()));
    const int requiredMenuHeight =
        metrics.verticalSpacing + menuRows * metrics.menuRowHeight + std::max(0, menuRows - 1) * metrics.menuSpacing;
    const int maxCoverHeight = safe.y + safe.height - homeTop - metrics.homeMenuTopOffset - requiredMenuHeight;
    homeCoverTileHeight = std::clamp(maxCoverHeight, 0, metrics.homeCoverTileHeight);
  }

  GUI.drawHeader(renderer, UITheme::getHeaderRect(renderer, metrics.homeTopPadding),
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr,
                 nullptr, false, true, true);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = safe.x;
  coverRectY = homeTop;
  coverRectW = safe.width;
  coverRectH = homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{safe.x, homeTop, safe.width, homeCoverTileHeight}, recentBooks, selectorIndex,
                          coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this),
                          hasAnyBookStats(currentBookStats) ? &currentBookStats : nullptr, currentBookProgressPercent);

  const int menuStartY = homeTop + homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int menuEndY = safe.y + safe.height;
  const int menuHeight = std::max(0, menuEndY - menuStartY);

  const bool isCarouselTheme =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;
  const int menuSelectedIndex = isCarouselTheme && mappedInput.hasTouchHardware()
                                    ? carouselMenuTouchDownIndex
                                    : selectorIndex - getHomeMenuSelectionOffset(recentBooks);
  GUI.drawButtonMenu(
      renderer, Rect{safe.x, menuStartY, safe.width, menuHeight}, static_cast<int>(menuItems.size()), menuSelectedIndex,
      [&menuItems](int index) { return menuItems[index].label; },
      [&menuItems](int index) { return menuItems[index].icon; });

  const char* readLabel = recentBooks.empty() ? "" : tr(STR_READ);
  const auto labels = isCarouselTheme
                          ? mappedInput.mapLabels(readLabel, tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT))
                          : mappedInput.mapLabels(readLabel, tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  displayHomeBuffer();

  // The panel already shows Home, so missing-thumbnail work can start now.
  // loadRecentCovers() requests its own repaint only when artwork changes;
  // repainting the identical first frame cost a full panel refresh.
  if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    loadRecentCovers(metrics.homeCoverHeight);
  }

  if (carouselWarmupPending && !carouselFramesReady) {
    // Resolve any missing cover thumbs first, then warm the carousel snapshot.
    // Cover generation needs more contiguous heap than the frame cache path.
    carouselWarmupPending = false;
    preRenderCarouselFrames();
    if (carouselFramesReady) {
      requestUpdate();
    }
  }
}

void HomeActivity::renderCarouselFrame(int bookIdx, int slotIdx) {
  if (slotIdx < 0 || slotIdx >= kCarouselFrameCount) {
    LOG_ERR("HOME", "carousel: invalid frame slot %d", slotIdx);
    return;
  }
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer || !gCarouselCache.frames[slotIdx]) return;
  renderCarouselFrameToCurrentBuffer(bookIdx);

  memcpy(gCarouselCache.frames[slotIdx], frameBuffer, renderer.getBufferSize());
  gCarouselCache.frameBookIdx[slotIdx] = bookIdx;
  carouselFrames[slotIdx] = gCarouselCache.frames[slotIdx];
}

void HomeActivity::onSelectBook(const std::string& path) {
  // renderCarouselFrame() uses the same static cache on the render task. Hold
  // its lock while invalidating so a book selection cannot free a destination
  // buffer midway through the snapshot copy.
  {
    RenderLock lock;
    gCarouselCache.invalidate();
    freeCarouselFrames();
  }
  if (Storage.exists(CAROUSEL_CACHE_TMP_PATH)) {
    Storage.remove(CAROUSEL_CACHE_TMP_PATH);
  }
  activityManager.goToReader(path);
}

void HomeActivity::onMinimalBrowseOpen() {
  if (SETTINGS.isLibraryFileBrowserSwapped()) {
    onLibraryOpen();
  } else {
    onFileBrowserOpen();
  }
}

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onContinueReading() {
  if (recentBooks.empty()) return;

  const bool isCarousel =
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL;
  const int bookIndex = isCarousel ? getHighlightedBookIndex() : 0;
  if (bookIndex >= 0 && bookIndex < static_cast<int>(recentBooks.size())) {
    onSelectBook(recentBooks[bookIndex].path);
  }
}

void HomeActivity::onLibraryOpen() { activityManager.goToLibrary(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onPluginsOpen() { activityManager.goToPlugins(); }

void HomeActivity::onReadingStatsOpen() {
  if (!SETTINGS.shouldTrackReadingStats()) return;
  const int highlightedBookIdx = getHighlightedBookIndex();
  std::string bookTitle =
      highlightedBookIdx >= 0 ? recentBooks[highlightedBookIdx].title : std::string(tr(STR_READING_STATS));
  const bool hasBookStats =
      highlightedBookIdx >= 0 && (FsHelpers::hasEpubExtension(recentBooks[highlightedBookIdx].path) ||
                                  FsHelpers::hasXtcExtension(recentBooks[highlightedBookIdx].path));
  std::string cachePath = hasBookStats ? getRecentBookCachePath(recentBooks[highlightedBookIdx]) : std::string{};
  const bool showBookStats = !cachePath.empty() && BookStatsTracking::isBookEnabled(cachePath);
  if (!showBookStats) {
    cachePath.clear();
    bookTitle = tr(STR_READING_STATS);
  }
  const BookReadingStats displayBookStats = showBookStats ? currentBookStats : BookReadingStats{};
  const float progress = showBookStats ? currentBookProgressPercent : -1.0f;
  const std::string syncBookPath = showBookStats ? recentBooks[highlightedBookIdx].path : std::string{};
  if (showAllDevicesStats) {
    auto stats = std::make_unique<BookStatsActivity>(renderer, mappedInput, bookTitle, cachePath, displayBookStats,
                                                     progress, false, 0, globalStats, allDevicesGlobalStats, true);
    stats->setSyncBookPath(syncBookPath);
    startActivityForResult(std::move(stats), [this](const ActivityResult& result) {
      mappedInput.suppressNextConfirmRelease();
      const auto* statsResult = std::get_if<ReadingStatsResult>(&result.data);
      if (statsResult && statsResult->changed) {
        globalStats = GlobalReadingStats::load();
        showAllDevicesStats = GlobalReadingStats::hasSyncedStats();
        allDevicesGlobalStats = showAllDevicesStats ? GlobalReadingStats::loadAggregated(globalStats) : globalStats;
        bookStatsCached = false;
        updateHighlightedBookContext();
      }
      requestUpdate();
    });
  } else {
    auto stats = std::make_unique<BookStatsActivity>(renderer, mappedInput, bookTitle, cachePath, displayBookStats,
                                                     progress, false, 0, globalStats, true);
    stats->setSyncBookPath(syncBookPath);
    startActivityForResult(std::move(stats), [this](const ActivityResult& result) {
      mappedInput.suppressNextConfirmRelease();
      const auto* statsResult = std::get_if<ReadingStatsResult>(&result.data);
      if (statsResult && statsResult->changed) {
        globalStats = GlobalReadingStats::load();
        showAllDevicesStats = GlobalReadingStats::hasSyncedStats();
        allDevicesGlobalStats = showAllDevicesStats ? GlobalReadingStats::loadAggregated(globalStats) : globalStats;
        bookStatsCached = false;
        updateHighlightedBookContext();
      }
      requestUpdate();
    });
  }
}

void HomeActivity::onSavedItemsOpen() {
  auto savedItems = makeUniqueNoThrow<SavedItemsHomeActivity>(renderer, mappedInput);
  if (!savedItems) {
    LOG_ERR("HOME", "OOM: saved items activity (%u bytes)", static_cast<unsigned>(sizeof(SavedItemsHomeActivity)));
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(savedItems), [this](const ActivityResult&) { requestUpdate(); });
}
