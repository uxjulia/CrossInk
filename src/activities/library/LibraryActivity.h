#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>

#include <memory>
#include <string>

#include "LibraryInputBuffer.h"
#include "RecentBadgeWork.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "activities/home/FinishedBookCache.h"
#include "components/OptionPopup.h"
#include "components/SwipeDeleteRow.h"
#include "util/ButtonNavigator.h"

class LibraryActivity final : public Activity {
 public:
  LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  void onFrontlightPanelClosed() override;
  bool blocksGlobalInput() const override { return actionPopup.isActive(); }

#ifdef SIMULATOR
  size_t simulatorPendingInputs() const { return pendingInput.size(); }
  int simulatorSelection() const { return selection; }
  int simulatorRowCount() const { return rowCount(); }
  int simulatorTopIndex() const { return topIndex; }
  const std::string& simulatorQuery() const { return query; }
  uint8_t simulatorSort() const { return static_cast<uint8_t>(sort); }
  bool simulatorDescending() const { return descending; }
  void simulatorSelectRow(int row) {
    selection = CONTROL_COUNT + row;
    reloadAfterBookAction();
  }
  void simulatorOpenContextMenu() { showBookActionMenu(selection - CONTROL_COUNT); }
  bool simulatorReadBook(int row, RecentBook& book) { return readBook(row, book); }
  void simulatorSetView(uint8_t method, bool reverse, const std::string& search = "") {
    sort = static_cast<Sort>(method);
    descending = reverse;
    query = search;
    refreshIndexIfNeeded();
    resetViewport();
  }
  void simulatorRefresh() { refreshLibrary(); }
#endif

 private:
  enum class Sort : uint8_t { DateAdded, Title, AuthorLast, AuthorFirst, RecentlyRead, Series, Genre };
  // Touch header controls precede the book rows; button-only navigation visits books directly.
  static constexpr int CONTROL_COUNT = 5;
  using UiApp = freeink::ui::FreeInkApp<32, 4>;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  SwipeDeleteRow swipeDelete;
  ButtonNavigator buttonNavigator;
  freeink::ui::ListNav listNav;
  OptionPopup actionPopup;
  library::LibraryIndexFile index{true};  // Reuse one decoded display row across its labels, icon and group.
  Sort sort = Sort::RecentlyRead;
  bool descending = true;
  int selection = CONTROL_COUNT;
  bool showSelection = true;
  int topIndex = 0;
  int gridPageStart = 0;
  int loadedGridPageStart = -1;
  int nextGridCoverRow = -1;
  bool gridCoverAdded = false;
  int16_t gridCoverWidth = 0;
  int16_t gridCoverHeight = 0;
  int gridProgressRow = -1;
  float gridProgress = -1.0f;
  bool uiReady = false;
  bool initialScanPending = false;
  bool confirmLongPressCaptured = false;
  bool ignoreConfirmRelease = false;
  // Set when the Back press that cancelled a scan is still held.
  bool ignoreBackRelease = false;
  // Back held when a scan starts belongs to whatever opened Library (a reader
  // long-press shortcut, say); only a fresh press after its release cancels.
  bool scanBackHeldAtStart = false;
  // A cancelled scan stays cancelled for this visit: sort changes and book
  // actions show the previous index instead of starting the scan again.
  bool scanCancelledThisVisit = false;
  // These fields belong only to the input task, including during rendering.
  LibraryInputBuffer pendingInput;
  bool inputOverflow = false;
  bool touchTracking = false;
  int touchStartX = 0;
  int touchStartY = 0;
  int touchLastX = 0;
  int touchLastY = 0;
  bool scanFailed = false;
  StrId scanFailureText = StrId::STR_LIBRARY_SCAN_FAILED;
  bool filterFailed = false;
  bool pendingCacheDeletedFeedback = false;
  unsigned long cacheDeletedFeedbackShowTime = 0;
  std::string query;
  // Searches and filters keep one bit per indexed book plus a running count per
  // 256-book block: about 4 KiB at the format ceiling, where a u16 per match
  // would need 64 KiB of contiguous C3 heap. Bits are positions in filterOrder.
  static constexpr uint16_t FILTER_BLOCK_ROWS = 256;
  std::unique_ptr<uint8_t[]> filterBits;
  std::unique_ptr<uint16_t[]> filterRanks;
  library::SortOrder filterOrder = library::SortOrder::TitleAsc;
  uint16_t filterSourceCount = 0;
  uint16_t filteredCount = 0;
  // Indices into the bounded recent-books history, independent of the Library index.
  uint16_t recentRows[RecentBooksStore::MAX_RECENT_BOOKS]{};
  size_t recentCount = 0;
  // SDK rowProvider consumes the strings before asking for the next row.
  // Reuse one row instead of retaining every title/author in the library.
  RecentBook rowScratch;
  std::string groupHeading;
  std::string previousGroupScratch;
  std::string groupKeyScratch;
  std::string previousGroupKeyScratch;
  std::string seriesScratch;
  std::string genreScratch;
  std::string subtitleScratch;
  // Finished state for recently drawn rows, keyed by a path hash so sort,
  // filter and search changes cannot mislabel a row. Index rows use the stored
  // path hash, so cache hits do not read the full path. Any dialog or the
  // frontlight panel clears it because they can change a book's status.
  FinishedBookCache finishedCache;
  RecentBadgeWork recentBadgeWork;
  uint32_t recentBadgeIdleSince = 0;
  bool isFinishedRow(int row);
  void resolveRecentBadgeWhileIdle();

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onDeleteEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onControlEvent(const freeink::ui::ActionEvent& event, void* user);
  static void provideRow(void* user, uint16_t row, freeink::ui::ListItem& item);
  void buildListScreen(UiApp::ScreenType& screen);
  void buildGrid(UiApp::ScreenType& screen);
  void loadGridPageCovers();
  bool loadGridCover(int row);
  void loadGridProgress();
  bool gridEnabled() const;
  void buildSortHeader(UiApp::ScreenType& screen);
  const char* sortLabel() const;
  library::SortOrder indexOrder() const;
  int rowCount() const;
  uint16_t ordinalForRow(int row);
  uint16_t filteredSourceRow(uint16_t row) const;
  bool readBook(int row, RecentBook& book, bool fullPath = true);
  uint32_t groupForRow(int row);
  uint16_t dateGroupForRow(int row);
  bool metadataGroupForRow(int row, std::string& out);
  bool hasActiveFilter() const;
  void latchInput();
  void queueInput(LibraryInputBuffer::Type type, int x = -1, int y = -1);
  void handleInput(const LibraryInputBuffer::Event& input);
  void refreshIndexIfNeeded(bool showScanning = false);
  bool rebuildIndex(bool showScanning);
  void drawScanScreen(const char* message) const;
  bool scanTouchEnabled() const;
  static bool scanCancelRequested(void* context);
  static void onScanProgress(void* context, const library::BuildProgress& progress);
  void readRecentBook(size_t historyRow, RecentBook& book) const;
  void resolveRecents();
  void applyFilter();
  void resetViewport();
  void reloadAfterBookAction();
  void openBook(int row);
  void openSortPicker(int selectedIndex = -1);
  void openMenu();
  void refreshLibrary();
  void openSearch();
  void openSettings();
  void activateControl(int control);
  // Owns the allocation-failure check and render lock for every child result.
  void openDialog(std::unique_ptr<Activity>&& activity, ActivityResultHandler handler);
  void promptDeleteBook(const RecentBook& book);
  void promptRemoveBook(const std::string& path, const std::string& title);
  void showBookActionMenu(size_t bookIndex, bool ignoreInitialConfirmRelease = false);
};
