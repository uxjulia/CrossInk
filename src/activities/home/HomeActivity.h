#pragma once
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "./FileBrowserActivity.h"
#include "QuickActions.h"
#include "activities/Activity.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "components/OptionPopup.h"
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
  int carouselCoverTouchDownIndex = -1;
  bool carouselCoverTouchDownWasSelected = false;
  // Touch menus use a momentary pressed state. Keep it separate from the
  // keyboard selection so a returned Home screen cannot retain an icon tint.
  int carouselMenuTouchDownIndex = -1;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  // Silent restarts keep the panel's previous frame. The first Home paint must
  // use a clean waveform so X4 panels do not diff against a WiFi screen.
  bool initialFullRefresh = false;
  bool hasReadingStats = false;
  bool hasBookmarks = false;
  bool hasClippings = false;
  bool hasOpdsServers = false;
  bool minimalMenuOpen = false;
  bool minimalSuppressInitialFrontRelease = false;
  bool homeBookSwapLongPressHandled = false;
  bool quickActionsLongPowerHandled = false;
  int minimalMenuIndex = 0;
  int minimalHomeNavIndex = -1;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  // Cached cover pixels already include Dark Mode's image-polarity correction.
  // They must be redrawn rather than reused when the output polarity changes.
  bool coverBufferInverted = false;
  // Home can be entered while Back is still held (e.g. leaving Settings with
  // Back): ignore that stale release until a fresh press is seen here.
  bool backPressSeen = false;
  OptionPopup quickActionsPopup;
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
  std::string currentBookChapterTitle;
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
  bool carouselFramesInverted = false;
  bool carouselWarmupPending = false;

  std::vector<RecentBook> recentBooks;
  const HomeMenuItem initialMenuItem;
  std::string initialBookPath;

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
  void invalidateCoverCache();
  void invalidatePolarityMismatchedCaches();
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
  void updateHighlightedBookContext(bool allowEpubLoad = true);
  void loadRecentBooks(int maxBooks);
  void loadAllBookStats();
  void loadRecentCovers(int coverHeight);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE, bool initialFullRefreshValue = false,
                        std::string initialBookPathValue = {})
      : Activity("Home", renderer, mappedInput),
        initialFullRefresh(initialFullRefreshValue),
        initialMenuItem(initialMenuItemValue),
        initialBookPath(std::move(initialBookPathValue)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return quickActionsPopup.isActive(); }
  bool blocksGlobalInput() const override { return quickActionsPopup.isActive(); }
  bool handleShortcutAction(CrossPointSettings::SHORT_PWRBTN action) override;
  std::string getCurrentBookPath() const override;
  std::string getCurrentBookTitle() const override;
  std::unique_ptr<Activity> createFrontlightReadingStatsActivity() override;
  void onFrontlightPanelClosed() override;
  bool handleFrontlightPanelResult(const FrontlightPanelResult& result) override;
};
