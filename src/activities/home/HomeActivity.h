#pragma once
#include <array>
#include <functional>
#include <optional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "activities/Activity.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "util/ButtonNavigator.h"

struct RecentBook;
struct Rect;

class HomeActivity final : public Activity {
 public:
  // Keep one rendered carousel frame in RAM. Additional frames remain available
  // through the SD snapshot cache and are paged in on demand.
  static constexpr int kCarouselFrameCount = 1;
  // Must be >= LyraCarouselMetrics::values.homeRecentBooksCount (asserted in .cpp)
  static constexpr int kMaxCachedBooks = 3;

 private:
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int lastCarouselBookIndex = 0;  // remembered position when leaving carousel row
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasReadingStats = false;
  bool hasBookmarks = false;
  bool hasClippings = false;
  bool hasOpdsServers = false;
  bool minimalMenuOpen = false;
  bool minimalSuppressInitialFrontRelease = false;
  bool homeBookSwapLongPressHandled = false;
  int minimalMenuIndex = 0;
  int minimalHomeNavIndex = -1;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  float currentBookProgressPercent = -1.0f;
  BookReadingStats currentBookStats;
  GlobalReadingStats globalStats;
  GlobalReadingStats allDevicesGlobalStats;
  bool showAllDevicesStats = false;

  // Per-book stats and progress cached at onEnter() to avoid SD reads during navigation.
  std::array<BookReadingStats, kMaxCachedBooks> cachedBookStats{};
  std::array<float, kMaxCachedBooks> cachedBookProgress{};
  bool bookStatsCached = false;

  uint8_t* carouselFrames[kCarouselFrameCount] = {};
  bool carouselFramesReady = false;
  bool carouselWarmupPending = false;

  std::vector<RecentBook> recentBooks;
  const HomeMenuItem initialMenuItem;

  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onContinueReading();
  void onRecentsOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();
  void onReadingStatsOpen();
  void onSavedItemsOpen();

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  bool preRenderCarouselFrames(bool showProgressPopup = false);
  void freeCarouselFrames();
  bool allocateCarouselFrameSlots(int targetFrameCount);
  bool buildCarouselCacheFile(const std::string& cacheKey, uint64_t cacheKeyHash, int bookCount,
                              bool showProgressPopup = false);
  bool loadCarouselFrameFromDisk(uint64_t cacheKeyHash, int bookCount, int bookIdx, int slotIdx);
  int chooseCarouselEvictionSlot(int centerIdx, int bookCount,
                                 std::optional<int> protectedBookIdx = std::nullopt) const;
  void renderCarouselFrameToCurrentBuffer(int bookIdx, BookReadingStats* outStats, float* outProgressPercent,
                                          bool* outUsedCachedStats);
  void renderCarouselFrame(int bookIdx, int slotIdx);
  void updateSlidingWindowCache(int centerIdx, int bookCount);
  int getHighlightedBookIndex() const;
  int getVisibleRecentBookCount() const;
  bool canSwapHomeBook() const;
  void showNextRecentBookOnHome();
  void updateHighlightedBookContext();
  void loadRecentBooks(int maxBooks);
  void loadAllBookStats();
  void loadRecentCovers(int coverHeight);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  std::string getCurrentBookPath() const override;
};
