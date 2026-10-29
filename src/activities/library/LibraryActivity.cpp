#include "LibraryActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <DateFormatting.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryFileTypes.h>
#include <LibraryRecentOrder.h>
#include <LibraryText.h>
#include <Memory.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>

#include "CrossPointState.h"
#include "FilenameFontSystem.h"
#include "activities/home/BookActions.h"
#include "activities/home/FileBrowserActionActivity.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/library/LibrarySettingsActivity.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/EpubReaderActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/libraryIcons.h"
#include "components/icons/listIcons.h"
#include "sync/BookSyncFile.h"
#include "sync/SyncPending.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_DELETE = 3;
constexpr fui::ActionId ACTION_CONTROL = 2;
constexpr unsigned long LONG_PRESS_MS = 1000;
constexpr unsigned long ACTION_FEEDBACK_MS = 1000;
constexpr int HEADER_CONTROL_SIZE = 44;
constexpr int HEADER_CONTROL_GAP = 10;
constexpr int FOOTER_HEIGHT = 28;
constexpr int GRID_COLUMNS = 3;
constexpr int GRID_PAGE_SIZE = 9;
constexpr int GRID_GAP = 8;
constexpr int GRID_SELECTION_PADDING = 4;
constexpr int GRID_SELECTION_OUTLINE_GAP = 2;
constexpr int GRID_SELECTION_OUTER_INSET = GRID_SELECTION_PADDING + GRID_SELECTION_OUTLINE_GAP;
constexpr int GRID_COVER_CORNER_RADIUS = 2;

uint8_t visibleLibraryFileTypes() {
  return (SETTINGS.libraryShowEpub ? library::FileEpub : 0) | (SETTINGS.libraryShowXtc ? library::FileXtc : 0) |
         (SETTINGS.libraryShowTxt ? library::FileTxt : 0) | (SETTINGS.libraryShowMarkdown ? library::FileMarkdown : 0);
}

const RecentBook* recentBookForPath(const std::string& path) {
  const auto& books = RECENT_BOOKS.getBooks();
  const auto it =
      std::find_if(books.begin(), books.end(), [&path](const RecentBook& book) { return book.path == path; });
  return it == books.end() ? nullptr : &*it;
}

bool hasValidGridThumb(const std::string& path, const int width, const int height) {
  FsFile file;
  if (path.empty() || !Storage.exists(path.c_str()) || !Storage.openFileForRead("LIB", path, file)) return false;
  Bitmap bitmap(file);
  const bool valid =
      bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() == width && bitmap.getHeight() == height;
  file.close();
  return valid;
}

int headerControlRightInset() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Inline battery themes need a separate lane for the options button.
  return metrics.headerBatteryDetached || metrics.headerBatterySide == 1 ? 10 : metrics.batteryWidth + 80;
}
}  // namespace

LibraryActivity::LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Library", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void LibraryActivity::onEnter() {
  {
    RenderLock lock(*this);
    filenameFontSystem.ensureLoaded(renderer);
  }

  pendingInput.clear();
  inputOverflow = false;
  touchTracking = false;
  confirmLongPressCaptured = false;
  PendingOverlayResume resume;
  const bool restoreSyncReturn = APP_STATE.pendingOverlayResume.origin == PendingOverlayOrigin::Library &&
                                 APP_STATE.consumePendingOverlayResume(resume);
  {
    RenderLock lock;
    Activity::onEnter();
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
    // The activity can be constructed while a landscape reader is still active.
    // Refresh FreeInkUI's captured screen size and safe area after rotating.
    app.setDevice(uiTarget.deviceContext());
    if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();
    applySharedUiTheme(app, uiTarget);
    seriesScratch.reserve(128);
    genreScratch.reserve(128);
    subtitleScratch.reserve(448);
    sort = SETTINGS.librarySortMethod <= static_cast<uint8_t>(Sort::Genre)
               ? static_cast<Sort>(SETTINGS.librarySortMethod)
               : Sort::RecentlyRead;
    descending = SETTINGS.librarySortDescending != 0;
    if (restoreSyncReturn) {
      if (resume.tab <= static_cast<uint8_t>(Sort::Genre)) sort = static_cast<Sort>(resume.tab);
      descending = resume.pane != 0;
      query = std::move(resume.libraryQuery);
    }
    app.on(ACTION_ROW, &LibraryActivity::onRowEvent, this);
    app.on(ACTION_DELETE, &LibraryActivity::onDeleteEvent, this);
    app.on(ACTION_CONTROL, &LibraryActivity::onControlEvent, this);
    app.setScreen(&LibraryActivity::listScreen, this);
    // The index survives a firmware reflash, but its first boot reconciliation
    // can still take time. Show feedback whenever that scan is due.
    scanCancelledThisVisit = false;
    initialScanPending = sort != Sort::RecentlyRead &&
                         (library::libraryIndexNeedsRefresh() || !Storage.exists(library::libraryIndexPath()));
  }

  // Paint the scan message before the main task starts reading the card.
  // The render task normally paints only after onEnter() returns.
  if (initialScanPending && requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    RenderLock lock;
    drawScanScreen(tr(STR_LIBRARY_SCANNING));
  }

  {
    RenderLock lock;
    refreshIndexIfNeeded();
    initialScanPending = false;
    resetViewport();
    if (restoreSyncReturn && rowCount() > 0) {
      selection = std::clamp<int>(resume.selectedIndex, CONTROL_COUNT, CONTROL_COUNT + rowCount() - 1);
      topIndex = std::clamp<int>(resume.scrollPosition, 0, rowCount() - 1);
      listNav.selected = selection - CONTROL_COUNT;
      listNav.top = topIndex;
      // Touch lists hide selection, so reset()'s first-build follow would
      // otherwise discard the restored scroll position.
      listNav.followOnBuild = false;
      gridPageStart = ((selection - CONTROL_COUNT) / GRID_PAGE_SIZE) * GRID_PAGE_SIZE;
      loadGridProgress();
    }
    ignoreConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    requestUpdate();
  }
}

void LibraryActivity::onExit() {
  pendingInput.clear();
  index.close();
  filterBits.reset();
  filterRanks.reset();
  Activity::onExit();
}

void LibraryActivity::refreshIndexIfNeeded(const bool showScanning) {
  if (sort == Sort::RecentlyRead) {
    index.close();
    scanFailed = false;
    uiReady = false;
    applyFilter();
    return;
  }
  if (scanCancelledThisVisit) {
    // Respect the cancel until the user asks for a refresh; keep showing
    // whatever index is already on the card.
    if (!index.isOpen() && !index.open(library::libraryIndexPath()))
      index.openForReconciliation(library::libraryIndexPath());
    uiReady = false;
    applyFilter();
    return;
  }
  // Reuse the index across ordinary visits; still reconcile after cold boots, after
  // file changes, and when the format or metadata setting no longer matches.
  if (library::libraryIndexNeedsRefresh() || (!index.isOpen() && !index.open(library::libraryIndexPath())) ||
      index.header().metadataEnabled != static_cast<uint8_t>(SETTINGS.libraryUseMetadata != 0)) {
    rebuildIndex(showScanning);
    return;
  }
  scanFailed = false;
  uiReady = false;
  applyFilter();
}

bool LibraryActivity::rebuildIndex(const bool showScanning) {
  if (sort == Sort::RecentlyRead) {
    library::invalidateLibraryIndex();
    if (RECENT_BOOKS.pruneMissing()) RECENT_BOOKS.saveToFile();
    refreshIndexIfNeeded();
    return true;
  }
  uiReady = false;
  index.close();
  if (showScanning) drawScanScreen(tr(STR_LIBRARY_SCANNING));
  library::BuildStats stats;
  library::BuildCallbacks callbacks;
  callbacks.context = this;
  callbacks.cancelRequested = &LibraryActivity::scanCancelRequested;
  callbacks.progress = &LibraryActivity::onScanProgress;
  scanCancelledThisVisit = false;
  scanBackHeldAtStart = mappedInput.isPressed(MappedInputManager::Button::Back);
  scanFailed = !library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0, &callbacks);
  scanFailureText = stats.failure == library::BuildFailure::Cancelled      ? StrId::STR_LIBRARY_SCAN_CANCELLED
                    : stats.failure == library::BuildFailure::TooManyBooks ? StrId::STR_LIBRARY_TOO_MANY_BOOKS
                                                                           : StrId::STR_LIBRARY_SCAN_FAILED;
  if (stats.failure == library::BuildFailure::Cancelled) {
    scanCancelledThisVisit = true;
    ignoreBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
  }
  if (scanFailed) LOG_ERR("LIB", "Library scan failed; retaining the previous index");
  if (!index.open(library::libraryIndexPath())) {
    // A failed one-time upgrade leaves the previous index on the card. Keep
    // its books readable while the next visit retries the rebuild.
    if (!scanFailed || !index.openForReconciliation(library::libraryIndexPath())) {
      LOG_ERR("LIB", "Cannot open library index");
      if (!scanFailed) scanFailureText = StrId::STR_LIBRARY_SCAN_FAILED;
      scanFailed = true;
    } else {
      LOG_INF("LIB", "Using previous Library index until rebuild succeeds");
      if (index.header().formatVersion < 4 && (sort == Sort::Series || sort == Sort::Genre)) {
        sort = Sort::Title;
        descending = false;
      }
    }
  }
  applyFilter();
  return !scanFailed;
}

bool LibraryActivity::scanTouchEnabled() const {
  // hasTouch() also honours the Disable Touchscreen setting.
  return mappedInput.hasTouch();
}

void LibraryActivity::drawScanScreen(const char* message) const {
  renderer.clearScreen();
  if (scanTouchEnabled()) {
    int bounds[4]{};
    renderer.getOrientedViewableTRBL(&bounds[0], &bounds[1], &bounds[2], &bounds[3]);
    renderer.drawCenteredText(UI_10_FONT_ID,
                              renderer.getScreenHeight() - bounds[2] - renderer.getLineHeight(UI_10_FONT_ID) * 2,
                              tr(STR_TAP_TO_CANCEL));
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  // drawPopup() pushes the whole buffer, including the hint above.
  GUI.drawPopup(renderer, message);
}

bool LibraryActivity::scanCancelRequested(void* context) {
  auto* self = static_cast<LibraryActivity*>(context);
  auto& input = self->mappedInput;
  // The build blocks the main loop, so poll here as web uploads do.
  input.update();
  int x = 0;
  int y = 0;
  if (self->scanTouchEnabled() && input.wasScreenTouchDown(x, y)) {
    // The release must not reach the list as a tap once the scan unwinds.
    input.suppressCurrentTouchContact();
    return true;
  }
  if (input.wasHomeGesture()) return true;
  const bool backHeld = input.isPressed(MappedInputManager::Button::Back);
  if (self->scanBackHeldAtStart) {
    if (!backHeld) self->scanBackHeldAtStart = false;
    return false;
  }
  // Polls stall during an EPUB parse, so a held button counts as well as a
  // press edge.
  return backHeld || input.wasPressed(MappedInputManager::Button::Back);
}

void LibraryActivity::onScanProgress(void* context, const library::BuildProgress& progress) {
  const auto* self = static_cast<const LibraryActivity*>(context);
  if (progress.phase == library::BuildPhase::Organizing) {
    self->drawScanScreen(tr(STR_LIBRARY_ORGANIZING));
    return;
  }
  char message[96];
  snprintf(message, sizeof(message), tr(STR_LIBRARY_SCAN_COUNT), static_cast<unsigned>(progress.books));
  self->drawScanScreen(message);
}

void LibraryActivity::readRecentBook(const size_t historyRow, RecentBook& book) const {
  book = RECENT_BOOKS.getBooks()[historyRow];
  if (!SETTINGS.libraryUseMetadata || book.title.empty()) {
    const auto slash = book.path.find_last_of('/');
    book.title = book.path.substr(slash == std::string::npos ? 0 : slash + 1);
    const auto dot = book.title.find_last_of('.');
    if (dot != std::string::npos && dot != 0) book.title.resize(dot);
  }
  if (!SETTINGS.libraryUseMetadata) book.author.clear();
}

void LibraryActivity::resolveRecents() {
  recentCount = 0;
  const auto& books = RECENT_BOOKS.getBooks();
  const std::string needle = library::fold(query);
  const uint8_t visibleTypes = visibleLibraryFileTypes();
  std::string combined;
  std::string folded;
  for (size_t row = 0; row < books.size() && recentCount < RecentBooksStore::MAX_RECENT_BOOKS; ++row) {
    const auto& book = books[row];
    if ((library::fileTypeFor(book.path) & visibleTypes) == 0) continue;
    if (!needle.empty()) {
      readRecentBook(row, rowScratch);
      combined.assign(rowScratch.title);
      combined.push_back(' ');
      combined.append(rowScratch.author);
      library::foldInto(combined, folded);
      if (!library::matchesQuery(folded, needle)) continue;
    }
    if (SETTINGS.libraryHideFinishedBooks && BookActions::isBookCompleted(book.path)) continue;
    recentRows[recentCount++] = static_cast<uint16_t>(row);
  }
}

library::SortOrder LibraryActivity::indexOrder() const {
  switch (sort) {
    case Sort::Title:
      return descending ? library::SortOrder::TitleDesc : library::SortOrder::TitleAsc;
    case Sort::AuthorLast:
      return descending ? library::SortOrder::AuthorDesc : library::SortOrder::AuthorAsc;
    case Sort::AuthorFirst:
      return descending ? library::SortOrder::AuthorFirstDesc : library::SortOrder::AuthorFirstAsc;
    case Sort::RecentlyRead:
      return library::SortOrder::RecentAsc;
    case Sort::Series:
      return descending ? library::SortOrder::SeriesDesc : library::SortOrder::SeriesAsc;
    case Sort::Genre:
      return descending ? library::SortOrder::GenreDesc : library::SortOrder::GenreAsc;
    case Sort::DateAdded:
      return descending ? library::SortOrder::RecentDesc : library::SortOrder::RecentAsc;
  }
  return library::SortOrder::RecentDesc;
}

const char* LibraryActivity::sortLabel() const {
  switch (sort) {
    case Sort::Title:
      return tr(STR_TITLE);
    case Sort::AuthorLast:
      return tr(STR_LIBRARY_AUTHOR_LAST_NAME);
    case Sort::AuthorFirst:
      return tr(STR_LIBRARY_AUTHOR_FIRST_NAME);
    case Sort::RecentlyRead:
      return tr(STR_LIBRARY_RECENTLY_OPENED);
    case Sort::Series:
      return tr(STR_LIBRARY_SERIES);
    case Sort::Genre:
      return tr(STR_LIBRARY_GENRE);
    case Sort::DateAdded:
      return tr(STR_LIBRARY_DATE_ADDED);
  }
  return tr(STR_LIBRARY_DATE_ADDED);
}

bool LibraryActivity::hasActiveFilter() const {
  return !query.empty() || !SETTINGS.libraryShowEpub || !SETTINGS.libraryShowXtc || !SETTINGS.libraryShowTxt ||
         !SETTINGS.libraryShowMarkdown || SETTINGS.libraryHideFinishedBooks;
}

int LibraryActivity::rowCount() const {
  if (sort == Sort::RecentlyRead) return static_cast<int>(recentCount);
  if (hasActiveFilter()) return filteredCount;
  return index.bookCount();
}

bool LibraryActivity::gridEnabled() const {
  return sort == Sort::RecentlyRead && SETTINGS.recentBooksView == CrossPointSettings::RECENT_BOOKS_GRID;
}

void LibraryActivity::loadGridProgress() {
  const int row = selection - CONTROL_COUNT;
  if (!gridEnabled() || row < 0 || row >= rowCount()) {
    gridProgressRow = -1;
    gridProgress = -1.0f;
    return;
  }
  if (gridProgressRow == row) return;
  gridProgressRow = row;
  gridProgress = -1.0f;
  RecentBook book;
  if (readBook(row, book)) {
    gridProgress = FsHelpers::hasEpubExtension(book.path) ? RecentBookProgress::loadCachedEpubPercent(book)
                                                          : RecentBookProgress::loadPercent(book);
  }
}

uint16_t LibraryActivity::ordinalForRow(const int row) {
  if (sort == Sort::RecentlyRead || row < 0 || row >= rowCount()) return UINT16_MAX;
  if (hasActiveFilter()) {
    const uint16_t sourceRow = filteredSourceRow(static_cast<uint16_t>(row));
    return sourceRow == UINT16_MAX ? UINT16_MAX : index.ordinalForRow(filterOrder, sourceRow);
  }
  return index.ordinalForRow(indexOrder(), static_cast<uint16_t>(row));
}

// Position in filterOrder of the row-th match: find its block by the running
// counts, then count bits within that block.
uint16_t LibraryActivity::filteredSourceRow(const uint16_t row) const {
  // Sized when the filter ran: the index may be closed (count 0) while a row
  // is still drawn before the activity swaps out.
  if (!filterBits || !filterRanks || row >= filteredCount || filterSourceCount == 0) return UINT16_MAX;
  const uint16_t blocks = (filterSourceCount + FILTER_BLOCK_ROWS - 1) / FILTER_BLOCK_ROWS;
  const uint16_t* ranks = filterRanks.get();
  uint16_t block = static_cast<uint16_t>(std::upper_bound(ranks, ranks + blocks, row) - ranks - 1);
  uint16_t seen = filterRanks[block];
  for (uint32_t source = static_cast<uint32_t>(block) * FILTER_BLOCK_ROWS; source < filterSourceCount; source++) {
    if ((filterBits[source / 8] & (1u << (source % 8))) == 0) continue;
    if (seen++ == row) return static_cast<uint16_t>(source);
  }
  return UINT16_MAX;
}

bool LibraryActivity::readBook(const int row, RecentBook& book, const bool fullPath) {
  if (sort == Sort::RecentlyRead) {
    const uint16_t historyRow =
        library::recentHistoryRow(row, RECENT_BOOKS.getCount(), recentRows, recentCount, descending);
    if (historyRow == UINT16_MAX) {
      LOG_ERR("LIB", "Cannot read recent book row %d", row);
      return false;
    }
    readRecentBook(historyRow, book);
    return true;
  }
  library::ClixRecord record{};
  const auto ordinal = ordinalForRow(row);
  if (ordinal == UINT16_MAX || !index.readRecord(ordinal, record) ||
      !index.readDisplayText(record, book.title, book.author) ||
      !(fullPath ? index.readPath(record, book.path) : index.readName(record, book.path))) {
    LOG_ERR("LIB", "Cannot read book row %d", row);
    return false;
  }
  return true;
}

void LibraryActivity::applyFilter() {
  filteredCount = 0;
  filterFailed = false;
  filterBits.reset();
  filterRanks.reset();
  filterSourceCount = 0;
  if (sort == Sort::RecentlyRead) {
    resolveRecents();
    return;
  }
  if (!hasActiveFilter() || !index.isOpen() || index.bookCount() == 0) return;
  const uint16_t sourceCount = index.bookCount();
  filterOrder = indexOrder();
  filterSourceCount = sourceCount;
  filterBits = makeUniqueNoThrow<uint8_t[]>((sourceCount + 7u) / 8u);
  filterRanks = makeUniqueNoThrow<uint16_t[]>((sourceCount + FILTER_BLOCK_ROWS - 1u) / FILTER_BLOCK_ROWS);
  if (!filterBits || !filterRanks) {
    LOG_ERR("LIB", "Cannot allocate Library search results");
    filterBits.reset();
    filterRanks.reset();
    filterFailed = true;
    return;
  }
  const std::string needle = library::fold(query);
  const uint8_t visibleTypes = visibleLibraryFileTypes();
  std::unique_ptr<uint32_t[]> folderOffsets;
  uint16_t folderStride = 0;
  std::string title;
  std::string author;
  std::string name;
  std::string path;
  std::string cachePath;
  std::string combined;
  std::string folded;
  // Blob fields are byte-length-prefixed. Reserve once for the entire scan,
  // avoiding concat/fold allocations for every book in the index.
  title.reserve(UINT8_MAX);
  author.reserve(UINT8_MAX);
  name.reserve(UINT8_MAX);
  path.reserve(256);
  cachePath.reserve(64);
  combined.reserve(2 * UINT8_MAX + 1);
  folded.reserve(2 * UINT8_MAX + 1);
  const auto readIndexedPath = [&](const library::ClixRecord& record) {
    if (!folderOffsets) {
      folderOffsets = makeUniqueNoThrow<uint32_t[]>(library::LibraryIndexFile::FOLDER_CHECKPOINT_COUNT);
      if (!folderOffsets || !index.buildFolderCheckpoints(folderOffsets.get(), folderStride)) {
        LOG_ERR("LIB", "Cannot prepare Library folder lookup");
        return false;
      }
    }
    if (!index.readPath(record, path, folderOffsets.get(), folderStride)) {
      LOG_ERR("LIB", "Cannot read Library book path");
      return false;
    }
    return true;
  };
  for (uint16_t row = 0; row < sourceCount; ++row) {
    if ((row & 31) == 31) delay(1);
    if (row % FILTER_BLOCK_ROWS == 0) filterRanks[row / FILTER_BLOCK_ROWS] = filteredCount;
    const uint16_t ordinal = index.ordinalForRow(filterOrder, row);
    library::ClixRecord record{};
    if (ordinal == UINT16_MAX || !index.readRecord(ordinal, record) || !index.readName(record, name)) {
      LOG_ERR("LIB", "Cannot read Library search data");
      filterFailed = true;
      filteredCount = 0;
      break;
    }
    if ((library::fileTypeFor(name) & visibleTypes) == 0) continue;
    if (!needle.empty() && !index.readDisplayText(record, title, author)) {
      LOG_ERR("LIB", "Cannot read Library search text");
      filterFailed = true;
      filteredCount = 0;
      break;
    }
    if (!needle.empty()) {
      combined.assign(title);
      combined.push_back(' ');
      combined.append(author);
      library::foldInto(combined, folded);
    }
    // Only matching books need their completion state read from the SD card.
    if (!needle.empty() && !library::matchesQuery(folded, needle)) continue;
    if (SETTINGS.libraryHideFinishedBooks) {
      const uint8_t type = library::fileTypeFor(name);
      cachePath.clear();
      if (type == library::FileEpub) {
        uint64_t pathHash = 0;
        if (!index.readPathHash(record, pathHash)) {
          LOG_ERR("LIB", "Cannot read Library book hash");
          filterFailed = true;
          filteredCount = 0;
          break;
        }
        cachePath = "/.crosspoint/epub_" + std::to_string(pathHash);
        if (!Storage.exists(cachePath.c_str())) {
          // Older EPUB caches used std::hash. Reading their stats here avoids
          // requiring the user to open each finished book to migrate its cache.
          if (!readIndexedPath(record)) {
            filterFailed = true;
            filteredCount = 0;
            break;
          }
          cachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(path));
        }
      } else if (type == library::FileXtc) {
        if (!readIndexedPath(record)) {
          filterFailed = true;
          filteredCount = 0;
          break;
        }
        cachePath = Xtc(path, "/.crosspoint").getCachePath();
      }
      if (!cachePath.empty() && BookReadingStats::load(cachePath).isCompleted) continue;
    }
    filterBits[row / 8] |= static_cast<uint8_t>(1u << (row % 8));
    filteredCount++;
  }
}

void LibraryActivity::resetViewport() {
  swipeDelete.close();
  selection = rowCount() || !mappedInput.hasTouchHardware() ? CONTROL_COUNT : 3;
  showSelection = !mappedInput.hasTouchHardware();
  topIndex = 0;
  gridPageStart = 0;
  loadedGridPageStart = -1;
  nextGridCoverRow = -1;
  gridProgressRow = -1;
  listNav.reset(selection - CONTROL_COUNT);
  uiReady = false;
  loadGridProgress();
}

void LibraryActivity::reloadAfterBookAction() {
  swipeDelete.close();
  refreshIndexIfNeeded(true);
  selection = std::min(selection, std::max(CONTROL_COUNT, CONTROL_COUNT + rowCount() - 1));
  listNav.selected = selection - CONTROL_COUNT;
  listNav.top = topIndex;
  listNav.follow(rowCount());
  topIndex = listNav.top;
  gridPageStart = ((selection - CONTROL_COUNT) / GRID_PAGE_SIZE) * GRID_PAGE_SIZE;
  loadedGridPageStart = -1;
  nextGridCoverRow = -1;
  gridProgressRow = -1;
  loadGridProgress();
  requestUpdate();
}

void LibraryActivity::openDialog(std::unique_ptr<Activity>&& child, ActivityResultHandler handler) {
  if (!child) {
    LOG_ERR("LIB", "Cannot allocate Library dialog");
    return;
  }
  pendingInput.clear();
  touchTracking = false;
  app.clearTapFlash();
  startActivityForResult(std::move(child), [this, handler = std::move(handler)](const ActivityResult& result) {
    RenderLock lock;
    ignoreConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    confirmLongPressCaptured = false;
    uiReady = false;
    handler(result);
    finishedCache.clear();
    requestUpdate();
  });
}

void LibraryActivity::openBook(const int row) {
  RecentBook book;
  if (!readBook(row, book)) return;
  app.clearTapFlash();
  index.close();
  onSelectBook(book.path);
}

void LibraryActivity::openSortPicker(const int selectedIndex) {
  static constexpr StrId choices[] = {StrId::STR_LIBRARY_DATE_ADDED,
                                      StrId::STR_TITLE,
                                      StrId::STR_LIBRARY_AUTHOR_LAST_NAME,
                                      StrId::STR_LIBRARY_AUTHOR_FIRST_NAME,
                                      StrId::STR_LIBRARY_RECENTLY_OPENED,
                                      StrId::STR_LIBRARY_SERIES,
                                      StrId::STR_LIBRARY_GENRE};
  const bool buttonOnly = !mappedInput.hasTouchHardware();
  auto onSelect = [this, buttonOnly](const int selected) {
    if (buttonOnly && selected == 0) {
      descending = !descending;
    } else {
      const int method = selected - (buttonOnly ? 1 : 0);
      if (method < 0 || method > static_cast<int>(Sort::Genre)) return;
      sort = static_cast<Sort>(method);
      if (!buttonOnly) descending = sort == Sort::DateAdded || sort == Sort::RecentlyRead;
    }
    SETTINGS.librarySortMethod = static_cast<uint8_t>(sort);
    SETTINGS.librarySortDescending = descending;
    if (!SETTINGS.saveToFile()) LOG_ERR("LIB", "Cannot save Library sort");
    refreshIndexIfNeeded(true);
    resetViewport();
    if (buttonOnly && selected == 0) openSortPicker(0);
  };
  actionPopup.setDismissOnOutsideTouchDown(true);
  if (buttonOnly) {
    const char* options[] = {descending ? "Z-A" : "A-Z", I18N.get(choices[0]), I18N.get(choices[1]),
                             I18N.get(choices[2]),       I18N.get(choices[3]), I18N.get(choices[4]),
                             I18N.get(choices[5]),       I18N.get(choices[6])};
    actionPopup.show(tr(STR_LIBRARY_SORT_BY), options, 8, selectedIndex < 0 ? 0 : selectedIndex, std::move(onSelect));
    actionPopup.setDividerAfterOption(0);
  } else {
    actionPopup.show(StrId::STR_LIBRARY_SORT_BY, choices, 7, static_cast<int>(sort), std::move(onSelect));
  }
  if (index.isOpen() && index.header().formatVersion < 4)
    actionPopup.setDisabledOptions(buttonOnly ? std::vector<bool>{false, false, false, false, false, false, true, true}
                                              : std::vector<bool>{false, false, false, false, false, true, true});
  requestUpdate();
}

void LibraryActivity::openMenu() {
  static constexpr StrId choices[] = {StrId::STR_SEARCH, StrId::STR_SETTINGS_SHORT, StrId::STR_LIBRARY_RESCAN};
  actionPopup.show(StrId::STR_MENU, choices, 3, 0, [this](const int selected) {
    if (selected == 0) openSearch();
    if (selected == 1) openSettings();
    if (selected == 2) refreshLibrary();
  });
  requestUpdate();
}

void LibraryActivity::openSearch() {
  openDialog(
      makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH), query, 48, InputType::Text),
      [this](const ActivityResult& result) {
        const auto* text = std::get_if<KeyboardResult>(&result.data);
        if (result.isCancelled || !text) return;
        query = text->text;
        applyFilter();
        resetViewport();
      });
}

void LibraryActivity::refreshLibrary() {
  rebuildIndex(true);
  resetViewport();
  requestUpdate();
}

void LibraryActivity::openSettings() {
  const bool useMetadata = SETTINGS.libraryUseMetadata != 0;
  openDialog(makeUniqueNoThrow<LibrarySettingsActivity>(renderer, mappedInput),
             [this, useMetadata](const ActivityResult&) {
               if ((SETTINGS.libraryUseMetadata != 0) != useMetadata) {
                 rebuildIndex(true);
               } else {
                 applyFilter();
               }
               resetViewport();
             });
}

void LibraryActivity::activateControl(const int control) {
  app.clearTapFlash();
  if (control == 0) refreshLibrary();
  if (control == 1) openSearch();
  if (control == 2) openSettings();
  if (control == 3) openSortPicker();
  if (control == 4) {
    descending = !descending;
    SETTINGS.librarySortDescending = descending;
    if (!SETTINGS.saveToFile()) LOG_ERR("LIB", "Cannot save Library sort direction");
    applyFilter();
    topIndex = 0;
    if (gridEnabled()) resetViewport();
    requestUpdate();
  }
}

void LibraryActivity::onDeleteEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<LibraryActivity*>(user);
  if (event.value < 0 || event.value != self->swipeDelete.index || event.value >= self->rowCount()) return;
  RecentBook book;
  if (!self->readBook(event.value, book)) return;
  self->swipeDelete.close();
  self->app.clearTapFlash();
  self->selection = event.value + CONTROL_COUNT;
  self->promptDeleteBook(book);
}

void LibraryActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<LibraryActivity*>(user);
  if (event.value < 0 || event.value >= self->rowCount()) return;
  self->selection = event.value + CONTROL_COUNT;
  self->showSelection = false;
  if (event.longPress)
    self->showBookActionMenu(event.value);
  else
    self->openBook(event.value);
}

void LibraryActivity::onControlEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<LibraryActivity*>(user);
  self->selection = event.value;
  self->showSelection = false;
  self->activateControl(event.value);
}

void LibraryActivity::queueInput(const LibraryInputBuffer::Type type, const int x, const int y) {
  if (inputOverflow) return;
  if (pendingInput.push({type, static_cast<int16_t>(x), static_cast<int16_t>(y)})) return;
  // Never replay a partial gesture or Select after losing its navigation.
  LOG_ERR("LIB", "Input buffer full; cancelling pending input");
  pendingInput.clear();
  pendingInput.push({LibraryInputBuffer::Type::TouchRelease});
  inputOverflow = true;
  int heldX = 0;
  int heldY = 0;
  if (mappedInput.isScreenTouchHeld(heldX, heldY)) mappedInput.suppressCurrentTouchContact();
  touchTracking = false;
  ignoreConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
}

void LibraryActivity::latchInput() {
  using Type = LibraryInputBuffer::Type;
  const bool confirmHeld = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  if (ignoreConfirmRelease || confirmLongPressCaptured) {
    (void)mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    if (!confirmHeld) {
      ignoreConfirmRelease = false;
      confirmLongPressCaptured = false;
    }
    return;
  }
  if (confirmHeld && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    confirmLongPressCaptured = true;
    queueInput(Type::ConfirmLongPress);
    return;
  }
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTouchDown(x, y)) {
    touchTracking = true;
    touchStartX = touchLastX = x;
    touchStartY = touchLastY = y;
  }
  if (touchTracking && mappedInput.isScreenTouchHeld(x, y)) {
    touchLastX = x;
    touchLastY = y;
  }
  const bool released = mappedInput.wasScreenTouchReleased();
  const int travelX = std::abs(touchLastX - touchStartX);
  const int travelY = std::abs(touchLastY - touchStartY);
  constexpr int DRAG_SCROLL_PX = 60;
  auto snap = touchSnapshotFrom(mappedInput);
  if (released && touchTracking && std::max(travelX, travelY) >= DRAG_SCROLL_PX && !snap.longPress) {
    snap.touchX = snap.touchY = -1;
  }
  if (snap.touchPressed) queueInput(Type::TouchPress, snap.touchX, snap.touchY);
  if (snap.touchReleased)
    queueInput(snap.longPress ? Type::TouchLongPress : Type::TouchRelease, snap.touchX, snap.touchY);

  MappedInputManager::SwipeDir swipe = MappedInputManager::SwipeDir::None;
  int swipeX = 0, swipeY = 0, endX = 0, endY = 0;
  mappedInput.wasSwipeWithPoints(swipe, swipeX, swipeY, endX, endY);
  // Preserve the SDK's tap tolerance. Only a genuine vertical drag past its
  // swipe distance scrolls when it is slower than the flick window.
  if (swipe == MappedInputManager::SwipeDir::None && released && touchTracking && travelY >= DRAG_SCROLL_PX &&
      travelY > travelX) {
    swipe = touchLastY < touchStartY ? MappedInputManager::SwipeDir::Up : MappedInputManager::SwipeDir::Down;
  }
  if (released) touchTracking = false;
  switch (swipe) {
    case MappedInputManager::SwipeDir::Up:
      queueInput(Type::SwipeUp);
      break;
    case MappedInputManager::SwipeDir::Down:
      queueInput(Type::SwipeDown);
      break;
    case MappedInputManager::SwipeDir::Left:
      queueInput(Type::SwipeLeft, swipeX, swipeY);
      break;
    case MappedInputManager::SwipeDir::Right:
      queueInput(Type::SwipeRight, swipeX, swipeY);
      break;
    default:
      break;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) queueInput(Type::ConfirmRelease);
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (ignoreBackRelease)
      ignoreBackRelease = false;
    else
      queueInput(Type::BackRelease);
  } else if (ignoreBackRelease && !mappedInput.isPressed(MappedInputManager::Button::Back)) {
    ignoreBackRelease = false;
  }
  if (mappedInput.hasTouchHardware()) {
    buttonNavigator.onNextRelease([&] { queueInput(Type::Next); });
    buttonNavigator.onPreviousRelease([&] { queueInput(Type::Previous); });
    buttonNavigator.onNextContinuous([&] { queueInput(Type::NextPage); });
    buttonNavigator.onPreviousContinuous([&] { queueInput(Type::PreviousPage); });
  } else {
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) queueInput(Type::LeftRelease);
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) queueInput(Type::RightRelease);
    buttonNavigator.onRelease({MappedInputManager::Button::Down, MappedInputManager::Button::Down},
                              [&] { queueInput(Type::Next); });
    buttonNavigator.onRelease({MappedInputManager::Button::Up, MappedInputManager::Button::Up},
                              [&] { queueInput(Type::Previous); });
    buttonNavigator.onContinuous({MappedInputManager::Button::Down, MappedInputManager::Button::Down},
                                 [&] { queueInput(Type::NextPage); });
    buttonNavigator.onContinuous({MappedInputManager::Button::Up, MappedInputManager::Button::Up},
                                 [&] { queueInput(Type::PreviousPage); });
  }
}

void LibraryActivity::loop() {
  if (actionPopup.isActive()) {
    pendingInput.clear();
    touchTracking = false;
    RenderLock lock;
    actionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
    if (!actionPopup.isActive() && mappedInput.isPressed(MappedInputManager::Button::Confirm))
      ignoreConfirmRelease = true;
    return;
  }
  latchInput();
  if (RenderLock::peek()) return;
  // Try mode also closes the race if rendering starts after the busy check.
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return;
  inputOverflow = false;
  if (pendingCacheDeletedFeedback && millis() - cacheDeletedFeedbackShowTime >= ACTION_FEEDBACK_MS) {
    pendingCacheDeletedFeedback = false;
    requestUpdate();
  }
  LibraryInputBuffer::Event input;
  if (pendingInput.pop(input)) {
    handleInput(input);
    return;
  }
  // Prepare at most one cover between input checks, then repaint the completed page.
  loadGridPageCovers();
}

void LibraryActivity::handleInput(const LibraryInputBuffer::Event& input) {
  using Type = LibraryInputBuffer::Type;
  if (input.type == Type::TouchRelease && input.x >= 0 && input.y >= 0) {
    const auto header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    const auto back = TouchHeaderBackButton::layout(header).touchRect;
    if (input.y < header.y + header.height && input.x >= back.x && input.x < back.x + back.width && input.y >= back.y &&
        input.y < back.y + back.height) {
      pendingInput.clear();
      onGoHome();
      return;
    }
  }
  if (input.type == Type::ConfirmLongPress) {
    pendingInput.clear();
    ignoreConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (selection >= CONTROL_COUNT && rowCount() > 0)
      showBookActionMenu(selection - CONTROL_COUNT, ignoreConfirmRelease);
    else
      activateControl(selection);
    return;
  }
  if (uiReady &&
      (input.type == Type::TouchPress || input.type == Type::TouchRelease || input.type == Type::TouchLongPress)) {
    fui::InputSnapshot snap{};
    snap.touchPressed = input.type == Type::TouchPress;
    snap.touchReleased = !snap.touchPressed;
    snap.longPress = input.type == Type::TouchLongPress;
    snap.touchX = input.x;
    snap.touchY = input.y;
    if (swipeDelete.dismissTap(app, snap, ACTION_DELETE)) {
      requestUpdate();
      return;
    }
    const auto event = app.route(snap);
    if (app.invalidated()) requestUpdate();
    if (event) {
      pendingInput.clear();
      return;
    }
  }
  if (input.type == Type::ConfirmRelease) {
    pendingInput.clear();
    if (selection < CONTROL_COUNT)
      activateControl(selection);
    else if (rowCount() > 0)
      openBook(selection - CONTROL_COUNT);
    return;
  }
  if (input.type == Type::BackRelease) {
    pendingInput.clear();
    if (!query.empty()) {
      query.clear();
      applyFilter();
      resetViewport();
      requestUpdate();
    } else
      onGoHome();
    return;
  }
  const auto swipe = input.type == Type::SwipeUp      ? MappedInputManager::SwipeDir::Up
                     : input.type == Type::SwipeDown  ? MappedInputManager::SwipeDir::Down
                     : input.type == Type::SwipeLeft  ? MappedInputManager::SwipeDir::Left
                     : input.type == Type::SwipeRight ? MappedInputManager::SwipeDir::Right
                                                      : MappedInputManager::SwipeDir::None;
  if (!gridEnabled() && uiReady &&
      (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Right)) {
    swipeDelete.swipe(app, swipe, input.x, input.y, ACTION_ROW);
    requestUpdate();
    return;
  }
  if (swipe != MappedInputManager::SwipeDir::None) swipeDelete.close();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down ||
      (gridEnabled() &&
       (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Right))) {
    if (mappedInput.hasTouchHardware()) showSelection = false;
    if (gridEnabled()) {
      const int lastPage = rowCount() > 0 ? ((rowCount() - 1) / GRID_PAGE_SIZE) * GRID_PAGE_SIZE : 0;
      const bool nextPage = swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Left;
      gridPageStart = std::clamp(gridPageStart + (nextPage ? GRID_PAGE_SIZE : -GRID_PAGE_SIZE), 0, lastPage);
      if (rowCount() > 0) selection = CONTROL_COUNT + gridPageStart;
      loadedGridPageStart = -1;
      nextGridCoverRow = -1;
      gridProgressRow = -1;
      loadGridProgress();
    } else {
      listNav.top = topIndex;
      const int page = listNav.pageRowsFor(rowCount());
      listNav.scrollBy(swipe == MappedInputManager::SwipeDir::Up ? page : -page, rowCount());
      topIndex = listNav.top;
    }
    requestUpdate();
    return;
  }
  if (!mappedInput.hasTouchHardware()) {
    if (input.type == Type::LeftRelease) {
      pendingInput.clear();
      openSortPicker();
      return;
    }
    if (input.type == Type::RightRelease) {
      pendingInput.clear();
      openMenu();
      return;
    }
  }
  const int count = rowCount() + CONTROL_COUNT;
  const auto move = [this](const int next) {
    swipeDelete.close();
    if (!showSelection) {
      showSelection = true;
      loadGridProgress();
      requestUpdate();
      return;
    }
    selection = next;
    if (gridEnabled()) {
      if (selection >= CONTROL_COUNT) {
        const int nextPage = ((selection - CONTROL_COUNT) / GRID_PAGE_SIZE) * GRID_PAGE_SIZE;
        if (nextPage != gridPageStart) {
          loadedGridPageStart = -1;
          nextGridCoverRow = -1;
        }
        gridPageStart = nextPage;
      }
      loadGridProgress();
    } else if (selection >= CONTROL_COUNT) {
      listNav.selected = selection - CONTROL_COUNT;
      listNav.top = topIndex;
      listNav.follow(rowCount());
      topIndex = listNav.top;
    }
    requestUpdate();
  };
  if (mappedInput.hasTouchHardware()) {
    if (input.type == Type::Next) move(ButtonNavigator::nextIndex(selection, count));
    if (input.type == Type::Previous) move(ButtonNavigator::previousIndex(selection, count));
    const int page = gridEnabled() ? GRID_PAGE_SIZE : listNav.pageRowsFor(rowCount());
    if (input.type == Type::NextPage) move(ButtonNavigator::nextPageIndex(selection, count, page));
    if (input.type == Type::PreviousPage) move(ButtonNavigator::previousPageIndex(selection, count, page));
  } else if (rowCount() > 0) {
    const int bookCount = rowCount();
    const int row = selection - CONTROL_COUNT;
    const auto moveBook = [&](const int next) { move(CONTROL_COUNT + next); };
    if (input.type == Type::Next) moveBook(ButtonNavigator::nextIndex(row, bookCount));
    if (input.type == Type::Previous) moveBook(ButtonNavigator::previousIndex(row, bookCount));
    const int page = gridEnabled() ? GRID_PAGE_SIZE : listNav.pageRowsFor(bookCount);
    if (input.type == Type::NextPage) moveBook(ButtonNavigator::nextPageIndex(row, bookCount, page));
    if (input.type == Type::PreviousPage) moveBook(ButtonNavigator::previousPageIndex(row, bookCount, page));
  }
}

bool LibraryActivity::isFinishedRow(const int row) {
  // Rows are drawn with only the file name (or the full path for recents).
  if (rowScratch.path.empty() || UITheme::getFileIcon(rowScratch.path) != UIIcon::Book) return false;
  const bool recentRow = sort == Sort::RecentlyRead;
  library::ClixRecord record{};
  uint32_t key = 0;
  if (recentRow) {
    key = static_cast<uint32_t>(std::hash<std::string>{}(rowScratch.path));
  } else {
    uint64_t pathHash = 0;
    const uint16_t ordinal = ordinalForRow(row);
    if (ordinal == UINT16_MAX || !index.readRecord(ordinal, record) || !index.readPathHash(record, pathHash)) {
      return false;
    }
    key = static_cast<uint32_t>(pathHash ^ (pathHash >> 32));
  }
  bool finished = false;
  if (finishedCache.lookup(key, finished)) return finished;

  if (recentRow) {
    finished = BookActions::isBookCompletedForList(rowScratch.path);
  } else {
    std::string fullPath;
    if (!index.readPath(record, fullPath)) {
      LOG_ERR("LIB", "Cannot read Library book path");
      finishedCache.store(key, false);  // log once, not on every render
      return false;
    }
    finished = BookActions::isBookCompletedForList(fullPath);
  }
  finishedCache.store(key, finished);
  return finished;
}

void LibraryActivity::onFrontlightPanelClosed() {
  // The panel's Reading Stats page can set or clear a book's finished date.
  // ActivityManager calls this while it already holds the render lock.
  finishedCache.clear();
  requestUpdate();
}

void LibraryActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<LibraryActivity*>(user)->buildListScreen(screen);
}

void LibraryActivity::provideRow(void* user, const uint16_t row, fui::ListItem& item) {
  auto* self = static_cast<LibraryActivity*>(user);
  if (!self->readBook(row, self->rowScratch, false)) {
    item.label = tr(STR_LIBRARY_UNAVAILABLE);
    item.enabled = false;
    return;
  }
  item.label = self->rowScratch.title.c_str();
  if (!self->rowScratch.author.empty()) item.subtitle = self->rowScratch.author.c_str();
  if (self->sort != Sort::RecentlyRead && SETTINGS.libraryUseMetadata &&
      (SETTINGS.libraryShowSeries || SETTINGS.libraryShowGenre)) {
    library::ClixRecord record{};
    const uint16_t ordinal = self->ordinalForRow(row);
    if (ordinal != UINT16_MAX && self->index.readRecord(ordinal, record) &&
        self->index.readSeries(record, self->seriesScratch) && self->index.readGenre(record, self->genreScratch) &&
        ((SETTINGS.libraryShowSeries && !self->seriesScratch.empty()) ||
         (SETTINGS.libraryShowGenre && !self->genreScratch.empty()))) {
      self->subtitleScratch = self->rowScratch.author;
      if (!self->subtitleScratch.empty()) self->subtitleScratch.append(" · ");
      if (SETTINGS.libraryShowSeries && !self->seriesScratch.empty()) {
        self->subtitleScratch.append(tr(STR_LIBRARY_SERIES));
        self->subtitleScratch.append(": ");
        self->subtitleScratch.append(self->seriesScratch);
      }
      if (SETTINGS.libraryShowGenre && !self->genreScratch.empty()) {
        if (SETTINGS.libraryShowSeries && !self->seriesScratch.empty()) self->subtitleScratch.append(" · ");
        self->subtitleScratch.append(tr(STR_LIBRARY_GENRE));
        self->subtitleScratch.append(": ");
        self->subtitleScratch.append(self->genreScratch);
      }
      item.subtitle = self->subtitleScratch.c_str();
    }
  }
  const UIIcon icon = self->isFinishedRow(row) ? UIIcon::BookCheck : UITheme::getFileIcon(self->rowScratch.path);
  item.icon = listIconFor(icon, 32);
  item.actionValue = static_cast<int16_t>(row);
  if (!SETTINGS.libraryListExpanded) return;
  if (self->sort == Sort::DateAdded) {
    const uint16_t date = self->dateGroupForRow(row);
    if (row == 0 || date != self->dateGroupForRow(row - 1)) {
      if (date == 0) {
        self->groupHeading = "?";
      } else {
        char heading[32];
        const char separator = SETTINGS.dateSeparator == CrossPointSettings::DATE_SEPARATOR_PERIOD   ? '.'
                               : SETTINGS.dateSeparator == CrossPointSettings::DATE_SEPARATOR_HYPHEN ? '-'
                                                                                                     : '/';
        self->groupHeading = formatDateParts(heading, sizeof(heading), 1980u + (date >> 9), (date >> 5) & 15u,
                                             date & 31u, SETTINGS.dateFormat, separator)
                                 ? heading
                                 : "?";
      }
      item.sectionHeading = self->groupHeading.c_str();
    }
  } else if (self->sort == Sort::Series || self->sort == Sort::Genre) {
    if (self->metadataGroupForRow(row, self->groupHeading)) {
      library::foldInto(self->groupHeading, self->groupKeyScratch);
      if (row == 0 || !self->metadataGroupForRow(row - 1, self->previousGroupScratch)) {
        item.sectionHeading = self->groupHeading.c_str();
      } else {
        library::foldInto(self->previousGroupScratch, self->previousGroupKeyScratch);
        if (self->groupKeyScratch != self->previousGroupKeyScratch) item.sectionHeading = self->groupHeading.c_str();
      }
    }
  } else if (self->sort != Sort::RecentlyRead) {
    const uint32_t initial = self->groupForRow(row);
    if (row == 0 || initial != self->groupForRow(row - 1)) {
      self->groupHeading.clear();
      utf8AppendCodepoint(initial ? (initial >= 'a' && initial <= 'z' ? initial - 'a' + 'A' : initial) : '#',
                          self->groupHeading);
      item.sectionHeading = self->groupHeading.c_str();
    }
  }
}

bool LibraryActivity::metadataGroupForRow(const int row, std::string& out) {
  library::ClixRecord record{};
  const uint16_t ordinal = ordinalForRow(row);
  if (ordinal == UINT16_MAX || !index.readRecord(ordinal, record)) return false;
  const bool read = sort == Sort::Series ? index.readSeries(record, out) : index.readGenre(record, out);
  if (read && out.empty()) out = "-";
  return read;
}

uint16_t LibraryActivity::dateGroupForRow(const int row) {
  library::ClixRecord record{};
  const uint16_t ordinal = ordinalForRow(row);
  if (!index.readRecord(ordinal, record)) return 0;
  // A failed v5 upgrade may leave a v4 index temporarily visible.
  uint32_t timestamp = record.modificationTime;
  if (index.header().formatVersion >= 5 && !index.readCreationTime(ordinal, timestamp)) return 0;
  const uint16_t date = static_cast<uint16_t>(timestamp >> 16);
  const uint8_t month = static_cast<uint8_t>((date >> 5) & 15u);
  const uint8_t day = static_cast<uint8_t>(date & 31u);
  return month >= 1 && month <= 12 && day >= 1 && day <= 31 ? date : 0;
}

uint32_t LibraryActivity::groupForRow(const int row) {
  library::ClixRecord record{};
  if (!index.readRecord(ordinalForRow(row), record)) return 0;
  std::string key;
  if (sort == Sort::Title) {
    key.assign(record.fold, record.foldLen);
  } else {
    std::string author;
    if (!index.readAuthor(record, author)) return 0;
    key = library::fold(sort == Sort::AuthorLast ? library::surnameKey(author) : author);
  }
  return library::foldedGroupInitial(key);
}

void LibraryActivity::buildSortHeader(UiApp::ScreenType& screen) {
  const int16_t bandHeight = std::max<int16_t>(44, screen.theme().rowHeight);
  const auto band = screen.take(fui::LayoutAnchor::Top, bandHeight);
  const int16_t margin = UITheme::getInstance().getMetrics().contentSidePadding;
  auto labelStyle = screen.theme().bodyText;
  labelStyle.bold = true;
  const int16_t labelWidth = std::min<int16_t>(
      band.width / 3, uiTarget.measureText(labelStyle.font, tr(STR_LIBRARY_SORT_BY), labelStyle).width);
  // The 32px glyph sits inside a wider selection box with even side padding.
  const int16_t iconWidth = 48;
  const int16_t methodWidth = std::min<int16_t>(
      band.width - labelWidth - iconWidth - 4 * margin,
      std::max<int16_t>(
          (sort == Sort::AuthorLast || sort == Sort::AuthorFirst) ? 220 : 0,
          uiTarget.measureText(screen.theme().bodyText.font, sortLabel(), screen.theme().bodyText).width + 20));
  const int16_t gap = std::max<int16_t>(0, (band.width - 2 * margin - labelWidth - methodWidth - iconWidth) / 2);
  const fui::Rect label{static_cast<int16_t>(band.x + margin), band.y, labelWidth, band.height};
  const fui::Rect method{static_cast<int16_t>(label.right() + gap), band.y, methodWidth, band.height};
  const fui::Rect direction{static_cast<int16_t>(band.right() - margin - iconWidth), band.y, iconWidth, band.height};
  uiTarget.text(label, tr(STR_LIBRARY_SORT_BY), labelStyle);
  // Each target owns half of the adjacent whitespace. SDK buttons provide
  // their draw/selection state; explicit hit regions avoid enlarged overlap.
  fui::ButtonProps button;
  button.label = sortLabel();
  button.text = screen.theme().bodyText;
  button.styles = fui::plainStyles();
  button.styles.selected = screen.theme().button.selected;
  button.state =
      mappedInput.hasTouchHardware() && showSelection && selection == 3 ? fui::StateSelected : fui::StateNormal;
  screen.button(button, method);
  button.label = nullptr;
  button.icon = fui::bitmapFromIcon(descending ? icon_arrow_down_wide_narrow_32 : icon_arrow_up_narrow_wide_32);
  button.state =
      mappedInput.hasTouchHardware() && showSelection && selection == 4 ? fui::StateSelected : fui::StateNormal;
  screen.button(button, direction);
  const int16_t split = static_cast<int16_t>((method.right() + direction.x) / 2);
  if (mappedInput.hasTouchHardware()) {
    screen.frame().hit(fui::Rect{band.x, band.y, static_cast<int16_t>(split - band.x), band.height}, ACTION_CONTROL, 3,
                       fui::InputTouch);
    screen.frame().hit(fui::Rect{split, band.y, static_cast<int16_t>(band.right() - split), band.height},
                       ACTION_CONTROL, 4, fui::InputTouch);
  }
  uiTarget.fill(fui::Rect{band.x, band.y, band.width, 1}, fui::Paint::solid(fui::Color::Black));
  uiTarget.fill(fui::Rect{band.x, static_cast<int16_t>(band.bottom() - 1), band.width, 1},
                fui::Paint::solid(fui::Color::Black));
}

void LibraryActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int bounds[4]{};
  renderer.getOrientedViewableTRBL(&bounds[0], &bounds[1], &bounds[2], &bounds[3]);
  const int16_t headerBottom = static_cast<int16_t>(TouchHeaderBackButton::contentTop(renderer, mappedInput));
  const int buttonHintsHeight =
      mappedInput.hasTouchHardware() ? 0 : UITheme::getInstance().getMetrics().buttonHintsHeight;
  screen.setContentMarginFromScreen(fui::Insets{headerBottom, static_cast<int16_t>(bounds[1]),
                                                static_cast<int16_t>(FOOTER_HEIGHT + buttonHintsHeight + bounds[2]),
                                                static_cast<int16_t>(bounds[3])});
  // Every header icon owns a 44px touch box, with 10px of clearance.
  const int16_t controlSize = HEADER_CONTROL_SIZE;
  const auto header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  const int16_t right = static_cast<int16_t>(renderer.getScreenWidth() - bounds[1] - headerControlRightInset());
  if (mappedInput.hasTouchHardware()) {
    fui::ButtonProps action;
    action.action = ACTION_CONTROL;
    action.inputMask = fui::InputTouch;
    action.styles = fui::plainStyles();
    action.styles.selected = screen.theme().button.selected;
    action.icon = fui::bitmapFromIcon(icon_refresh_cw_32);
    action.value = 0;
    action.state = showSelection && selection == 0 ? fui::StateSelected : fui::StateNormal;
    screen.button(action,
                  fui::Rect{static_cast<int16_t>(right - 3 * controlSize - 2 * HEADER_CONTROL_GAP),
                            static_cast<int16_t>(header.y + header.height - controlSize), controlSize, controlSize});
    action.icon = fui::bitmapFromIcon(icon_search_32);
    action.value = 1;
    action.state = showSelection && selection == 1 ? fui::StateSelected : fui::StateNormal;
    screen.button(action,
                  fui::Rect{static_cast<int16_t>(right - 2 * controlSize - HEADER_CONTROL_GAP),
                            static_cast<int16_t>(header.y + header.height - controlSize), controlSize, controlSize});
    action.icon = fui::bitmapFromIcon(icon_ellipsis_vertical_32);
    action.value = 2;
    action.state = showSelection && selection == 2 ? fui::StateSelected : fui::StateNormal;
    // Give the small overflow icon a 56px touch target. Keep its bottom edge at
    // the header boundary so the expanded target cannot steal taps from sorting.
    action.hitPadding = fui::Insets{12, 6, 0, 6};
    screen.button(action,
                  fui::Rect{static_cast<int16_t>(right - controlSize),
                            static_cast<int16_t>(header.y + header.height - controlSize), controlSize, controlSize});
  }
  buildSortHeader(screen);
  // Status lines share the sort row's side padding instead of touching the bezel.
  const int16_t statusPadding = static_cast<int16_t>(metrics.contentSidePadding);
  const fui::Insets statusInsets{0, statusPadding, 0, statusPadding};
  if (scanFailed || index.ranksDegraded() || filterFailed) {
    const auto warning =
        screen.take(fui::LayoutAnchor::Top, uiTarget.lineHeight(screen.theme().smallText.font) + 8).inset(statusInsets);
    uiTarget.text(warning,
                  filterFailed ? tr(STR_LIBRARY_SEARCH_FAILED)
                  : scanFailed ? I18n::getInstance().get(scanFailureText)
                               : tr(STR_LIBRARY_UNSORTED),
                  screen.theme().smallText);
  }
  if (!query.empty()) {
    const auto search =
        screen.take(fui::LayoutAnchor::Top, uiTarget.lineHeight(screen.theme().smallText.font) + 8).inset(statusInsets);
    uiTarget.text(search, query.c_str(), screen.theme().smallText);
  }
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (rowCount() == 0) {
    if (!scanFailed && !filterFailed) {
      const char* message = tr(STR_LIBRARY_EMPTY);
      if (hasActiveFilter())
        message = tr(STR_LIBRARY_NO_RESULTS);
      else if (sort == Sort::RecentlyRead)
        message = tr(STR_NO_RECENT_BOOKS);
      screen.centeredText(message, screen.theme().bodyText);
    }
    return;
  }
  if (gridEnabled()) {
    buildGrid(screen);
    return;
  }
  fui::ListProps props;
  props.rowProvider = &LibraryActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(rowCount());
  props.selectedIndex = showSelection ? static_cast<int16_t>(selection - CONTROL_COUNT) : -1;
  const auto reveal = swipeDelete.reveal(ACTION_DELETE);
  props.reveal = &reveal;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.iconSize = 28;
  props.labelText = screen.theme().bodyText;
  props.labelText.bold = true;
  props.labelText.maxLines = 1;
  props.headerText = screen.theme().bodyText;
  props.headerText.bold = true;
  props.rtl = I18N.isRightToLeft();
  configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle);
  props.subtitleText.maxLines = 3;
  listNav.selected = showSelection ? selection - CONTROL_COUNT : -1;
  listNav.top = topIndex;
  listNav.syncToProps(screen.body(), props.rowHeight, props.rowGap, rowCount(), props);
  topIndex = listNav.top;
  {
    FilenameUiFontScope fonts(uiTarget, renderer);
    screen.list(props);
  }
}

void LibraryActivity::buildGrid(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int count = rowCount();
  const int pageCount = (count + GRID_PAGE_SIZE - 1) / GRID_PAGE_SIZE;
  const auto title = screen.take(fui::LayoutAnchor::Top,
                                 static_cast<int16_t>(uiTarget.lineHeight(screen.theme().bodyText.font) + GRID_GAP));
  fui::Rect pageIndicator{};
  if (pageCount > 1) pageIndicator = screen.take(fui::LayoutAnchor::Bottom, 16, GRID_GAP);
  const fui::Rect area = screen.body();
  const int16_t cellWidth = std::max<int16_t>(1, (area.width - 2 * GRID_GAP) / GRID_COLUMNS);
  const int16_t cellHeight = std::max<int16_t>(1, (area.height - 2 * GRID_GAP) / GRID_COLUMNS);
  const int16_t coverHeight =
      std::max<int16_t>(1, std::min<int16_t>(cellHeight - 2 * GRID_SELECTION_OUTER_INSET,
                                             (cellWidth - 2 * GRID_SELECTION_OUTER_INSET) * 3 / 2));
  const int16_t coverWidth =
      std::max<int16_t>(1, std::min<int16_t>(cellWidth - 2 * GRID_SELECTION_OUTER_INSET, coverHeight * 2 / 3));
  if (coverWidth != gridCoverWidth || coverHeight != gridCoverHeight) {
    gridCoverWidth = coverWidth;
    gridCoverHeight = coverHeight;
    loadedGridPageStart = -1;
    nextGridCoverRow = -1;
  }

  const int selectedRow = selection - CONTROL_COUNT;
  if (selectedRow >= gridPageStart && selectedRow < gridPageStart + GRID_PAGE_SIZE && selectedRow < count &&
      readBook(selectedRow, rowScratch)) {
    const bool hasProgress = gridProgressRow == selectedRow && RecentBookProgress::hasPercent(gridProgress);
    subtitleScratch.clear();
    if (hasProgress) subtitleScratch.assign("  ·  ").append(RecentBookProgress::formatPercent(gridProgress));
    FilenameUiFontScope fonts(uiTarget, renderer);
    auto style = screen.theme().bodyText;
    style.maxLines = 1;
    // Center the whole title row between the sort divider and the first cover.
    const int16_t coverTop = static_cast<int16_t>(area.y + (cellHeight - coverHeight) / 2);
    const int16_t bandTop = static_cast<int16_t>(title.y - metrics.verticalSpacing);
    const fui::Rect titleBand{title.x, bandTop, title.width, static_cast<int16_t>(coverTop - bandTop)};
    const fui::Rect textBand = titleBand.inset(fui::Insets{0, static_cast<int16_t>(metrics.contentSidePadding), 0,
                                                           static_cast<int16_t>(metrics.contentSidePadding)});
    const int16_t suffixWidth =
        hasProgress ? uiTarget.measureText(style.font, subtitleScratch.c_str(), style).width : 0;
    const int16_t titleWidth = std::max<int16_t>(0, textBand.width - suffixWidth);
    const std::string visibleTitle =
        renderer.truncatedText(renderer.filenameFontId(uiScaleSpec().bodyFontId), rowScratch.title.c_str(), titleWidth,
                               style.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    const int16_t drawnTitleWidth = uiTarget.measureText(style.font, visibleTitle.c_str(), style).width;
    uiTarget.text(fui::Rect{textBand.x, textBand.y, titleWidth, textBand.height}, visibleTitle.c_str(), style);
    if (hasProgress)
      uiTarget.text(
          fui::Rect{static_cast<int16_t>(textBand.x + drawnTitleWidth), textBand.y, suffixWidth, textBand.height},
          subtitleScratch.c_str(), style);
  }

  const int visible = std::min(GRID_PAGE_SIZE, count - gridPageStart);
  for (int slot = 0; slot < visible; ++slot) {
    const int row = gridPageStart + slot;
    const int col = slot % GRID_COLUMNS;
    const int line = slot / GRID_COLUMNS;
    const fui::Rect cell{static_cast<int16_t>(area.x + col * (cellWidth + GRID_GAP)),
                         static_cast<int16_t>(area.y + line * (cellHeight + GRID_GAP)), cellWidth, cellHeight};
    const int16_t x = static_cast<int16_t>(cell.x + (cell.width - coverWidth) / 2);
    const int16_t y = static_cast<int16_t>(cell.y + (cell.height - coverHeight) / 2);
    bool drawn = false;
    const bool available = readBook(row, rowScratch);
    if (available) {
      const RecentBook* recent = recentBookForPath(rowScratch.path);
      if (recent && !recent->coverBmpPath.empty()) {
        const std::string path = UITheme::getCoverThumbPath(recent->coverBmpPath, coverWidth, coverHeight);
        if (!path.empty() && Storage.exists(path.c_str())) {
          FsFile file;
          if (Storage.openFileForRead("LIB", path, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0)
              drawn = renderer.drawBitmap(bitmap, x, y, coverWidth, coverHeight);
            file.close();
          }
        }
      }
    }
    if (!drawn) {
      renderer.fillRoundedRect(x, y, coverWidth, coverHeight, GRID_COVER_CORNER_RADIUS, Color::White);
      if (coverWidth >= 36 && coverHeight >= 36)
        drawLucideIcon(renderer, icon_book_marked_32, x + (coverWidth - 32) / 2, y + (coverHeight - 32) / 2);
    }
    renderer.maskRoundedRectOutsideCorners(x, y, coverWidth, coverHeight, GRID_COVER_CORNER_RADIUS, Color::White);
    renderer.drawRoundedRect(x, y, coverWidth, coverHeight, 2, GRID_COVER_CORNER_RADIUS, true);
    if (row == selectedRow) {
      renderer.drawRoundedRect(x - GRID_SELECTION_PADDING, y - GRID_SELECTION_PADDING,
                               coverWidth + 2 * GRID_SELECTION_PADDING, coverHeight + 2 * GRID_SELECTION_PADDING, 3,
                               GRID_COVER_CORNER_RADIUS + GRID_SELECTION_PADDING, true);
      renderer.drawRoundedRect(
          x - GRID_SELECTION_OUTER_INSET, y - GRID_SELECTION_OUTER_INSET, coverWidth + 2 * GRID_SELECTION_OUTER_INSET,
          coverHeight + 2 * GRID_SELECTION_OUTER_INSET, 1, GRID_COVER_CORNER_RADIUS + GRID_SELECTION_OUTER_INSET, true);
    }
    if (available)
      screen.frame().hit(cell, ACTION_ROW, static_cast<int16_t>(row), fui::InputTouch | fui::InputLongPress);
  }

  if (pageCount > 1) {
    constexpr int dotSize = 6;
    constexpr int dotGap = 8;
    const int dotsWidth = pageCount * dotSize + (pageCount - 1) * dotGap;
    const int startX = pageIndicator.x + (pageIndicator.width - dotsWidth) / 2;
    for (int page = 0; page < pageCount; ++page) {
      const int x = startX + page * (dotSize + dotGap);
      if (page == gridPageStart / GRID_PAGE_SIZE)
        renderer.fillRect(x, pageIndicator.y + 4, dotSize, dotSize, true);
      else
        renderer.drawRect(x, pageIndicator.y + 4, dotSize, dotSize, true);
    }
  }
}

void LibraryActivity::loadGridPageCovers() {
  if (!gridEnabled() || gridCoverWidth <= 0 || gridCoverHeight <= 0 || gridPageStart == loadedGridPageStart) return;
  const int pageEnd = std::min(gridPageStart + GRID_PAGE_SIZE, rowCount());
  if (nextGridCoverRow < 0) {
    nextGridCoverRow = gridPageStart;
    gridCoverAdded = false;
  }
  if (nextGridCoverRow < pageEnd && loadGridCover(nextGridCoverRow++)) gridCoverAdded = true;
  if (nextGridCoverRow >= pageEnd) {
    loadedGridPageStart = gridPageStart;
    nextGridCoverRow = -1;
    if (gridCoverAdded) requestUpdate();
  }
}

bool LibraryActivity::loadGridCover(const int row) {
  RecentBook book;
  if (!readBook(row, book)) return false;
  const RecentBook* recent = recentBookForPath(book.path);
  if (!recent || recent->coverState == RecentBook::CoverState::Missing) return false;
  if (!FsHelpers::hasEpubExtension(book.path) && !FsHelpers::hasXtcExtension(book.path)) return false;
  const std::string thumbPath = UITheme::getCoverThumbPath(recent->coverBmpPath, gridCoverWidth, gridCoverHeight);
  if (hasValidGridThumb(thumbPath, gridCoverWidth, gridCoverHeight)) return false;
  if (FsHelpers::hasEpubExtension(book.path)) {
    Epub epub(book.path, "/.crosspoint");
    if (!epub.load(true, true, Epub::XLocationLoadMode::Skip)) {
      LOG_ERR("LIB", "Cannot load EPUB cover for %s", book.path.c_str());
      return false;
    }
    if (epub.generateThumbBmp(gridCoverWidth, gridCoverHeight, &renderer, SETTINGS.getReaderFontId())) {
      if (!RECENT_BOOKS.updateBook(book.path, recent->title, recent->author, epub.getThumbBmpPath(),
                                   recent->coverState))
        LOG_ERR("LIB", "Cannot update EPUB cover path for %s", book.path.c_str());
      return true;
    }
    if (!epub.hasCoverImage()) {
      if (!RECENT_BOOKS.updateBook(book.path, recent->title, recent->author, "", RecentBook::CoverState::Missing))
        LOG_ERR("LIB", "Cannot mark missing EPUB cover for %s", book.path.c_str());
    }
    return false;
  }
  Xtc xtc(book.path, "/.crosspoint");
  if (!xtc.load()) {
    LOG_ERR("LIB", "Cannot load XTC cover for %s", book.path.c_str());
    return false;
  }
  if (!xtc.generateThumbBmp(gridCoverWidth, gridCoverHeight)) return false;
  if (!RECENT_BOOKS.updateBook(book.path, recent->title, recent->author, xtc.getThumbBmpPath(), recent->coverState))
    LOG_ERR("LIB", "Cannot update XTC cover path for %s", book.path.c_str());
  return true;
}

void LibraryActivity::render(RenderLock&&) {
  uiReady = false;
  if (initialScanPending) {
    drawScanScreen(tr(STR_LIBRARY_SCANNING));
    return;
  }
  for (int pass = 0; pass < 8; ++pass) {
    renderer.clearScreen();
    const auto header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    if (mappedInput.hasTouchHardware())
      TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_LIBRARY), false,
                                  3 * HEADER_CONTROL_SIZE + 2 * HEADER_CONTROL_GAP + headerControlRightInset() + 10);
    else
      GUI.drawHeader(renderer, header, tr(STR_LIBRARY));
    renderUiApp(app, uiTarget);
    topIndex = listNav.top;
    if (!listNav.consumeRebuildNeeded()) break;
  }
  uiReady = true;
  if (actionPopup.processRender(renderer, mappedInput)) return;
  const char* confirmLabel = !mappedInput.hasTouchHardware() && rowCount() == 0 ? ""
                             : selection < CONTROL_COUNT                        ? tr(STR_SELECT)
                                                                                : tr(STR_OPEN);
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(query.empty() ? tr(STR_HOME) : tr(STR_BACK)), confirmLabel,
                            mappedInput.hasTouchHardware() ? tr(STR_DIR_UP) : tr(STR_SORT),
                            mappedInput.hasTouchHardware() ? tr(STR_DIR_DOWN) : tr(STR_MENU));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  char footer[32];
  snprintf(footer, sizeof(footer), tr(STR_LIBRARY_FILES_COUNT), static_cast<unsigned>(rowCount()));
  int bounds[4]{};
  renderer.getOrientedViewableTRBL(&bounds[0], &bounds[1], &bounds[2], &bounds[3]);
  const int buttonHintsHeight =
      mappedInput.hasTouchHardware() ? 0 : UITheme::getInstance().getMetrics().buttonHintsHeight;
  renderer.drawCenteredText(SMALL_FONT_ID,
                            renderer.getScreenHeight() - bounds[2] - buttonHintsHeight -
                                (FOOTER_HEIGHT + renderer.getLineHeight(SMALL_FONT_ID)) / 2,
                            footer);
  // drawPopup() refreshes the panel itself; avoid repeating the same frame.
  if (pendingCacheDeletedFeedback) {
    GUI.drawPopup(renderer, tr(STR_BOOK_CACHE_DELETED));
  } else {
    renderer.displayBuffer();
  }
}

void LibraryActivity::promptDeleteBook(const RecentBook& book) {
  const std::string path = book.path;
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      return;
    }

    BookActions::clearFileMetadata(path);
    if (!Storage.remove(path.c_str())) {
      LOG_ERR("LIB", "Failed to delete file: %s", path.c_str());
      return;
    }
    // A deleted book has nothing left to upload or pull.
    SyncPending::removePath(path);
    BookSyncFile::onDeleted(path);

    library::invalidateLibraryIndex();
    // The card changed, so a cancelled scan must not keep showing this book.
    scanCancelledThisVisit = false;
    RECENT_BOOKS.removeByPath(path);
    reloadAfterBookAction();
  };

  const std::string heading = tr(STR_DELETE_ITEM_CONFIRM);
  openDialog(makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, heading, book.title), std::move(handler));
}

void LibraryActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      reloadAfterBookAction();
    }
  };

  openDialog(makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput,
                                                     std::string(tr(STR_REMOVE_FROM_RECENTS_ACTION)) + "?", title,
                                                     /*ignoreInitialConfirmRelease=*/false),
             std::move(handler));
}

void LibraryActivity::showBookActionMenu(const size_t bookIndex, const bool ignoreInitialConfirmRelease) {
  RecentBook book;
  if (!readBook(static_cast<int>(bookIndex), book)) return;
  const auto& recents = RECENT_BOOKS.getBooks();
  const bool isRecent = std::any_of(recents.begin(), recents.end(),
                                    [&book](const RecentBook& recent) { return recent.path == book.path; });
  std::vector<FileBrowserActionActivity::MenuItem> items =
      BookActions::buildBookActionItems(book.path, /*includeRemoveFromRecents=*/isRecent);
  if (BookActions::canSendNearby(book.path)) {
    items.push_back({FileBrowserAction::SendNearby, StrId::STR_SEND_NEARBY_BOOK});
  }

  openDialog(makeUniqueNoThrow<FileBrowserActionActivity>(renderer, mappedInput, book.title, std::move(items),
                                                          ignoreInitialConfirmRelease),
             [this, book](const ActivityResult& result) {
               confirmLongPressCaptured = false;
               if (result.isCancelled) {
                 return;
               }

               const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
               if (!actionResult) {
                 LOG_ERR("LIB", "Book action result missing");
                 return;
               }

               switch (static_cast<FileBrowserAction>(actionResult->action)) {
                 case FileBrowserAction::ToggleBookStatsTracking: {
                   bool enabled = false;
                   if (!BookActions::toggleBookStatsTracking(book.path, enabled)) {
                     const std::string error = std::string(tr(STR_TRACK_READING_STATS)) + " " + tr(STR_FAILED_LOWER);
                     BookActions::drawToast(renderer, error.c_str());
                   }
                   reloadAfterBookAction();
                   return;
                 }
                 case FileBrowserAction::SyncProgress: {
                   PendingOverlayResume resume;
                   resume.origin = PendingOverlayOrigin::Library;
                   resume.tab = static_cast<uint8_t>(sort);
                   resume.pane = descending;
                   resume.selectedIndex = selection;
                   resume.scrollPosition = topIndex;
                   resume.libraryQuery = query;
                   BookActions::syncProgress(renderer, mappedInput, book.path, std::move(resume));
                   requestUpdate();
                   return;
                 }
                 case FileBrowserAction::ReadingStats:
                   openDialog(BookActions::createReadingStatsActivity(renderer, mappedInput, book.path, book.title),
                              [this](const ActivityResult&) { requestUpdate(); });
                   return;
                 case FileBrowserAction::Delete:
                   promptDeleteBook(book);
                   return;
                 case FileBrowserAction::DeleteCache:
                   openDialog(makeUniqueNoThrow<ConfirmationActivity>(
                                  renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE_CACHE),
                                  book.title),
                              [this, book](const ActivityResult& confirmation) {
                                if (!confirmation.isCancelled) {
                                  if (!BookActions::clearBookCache(book.path)) {
                                    LOG_ERR("LIB", "Failed to clear book cache for: %s", book.path.c_str());
                                  } else {
                                    pendingCacheDeletedFeedback = true;
                                    cacheDeletedFeedbackShowTime = millis();
                                  }
                                }
                                reloadAfterBookAction();
                              });
                   return;
                 case FileBrowserAction::DeleteStats:
                   openDialog(makeUniqueNoThrow<ConfirmationActivity>(
                                  renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE_BOOK_STATS),
                                  book.title),
                              [this, book](const ActivityResult& confirmation) {
                                if (!confirmation.isCancelled) {
                                  if (!BookActions::deleteBookStats(book.path)) {
                                    LOG_ERR("LIB", "Failed to delete book stats for: %s", book.path.c_str());
                                  } else {
                                    BookActions::drawToast(renderer, tr(STR_BOOK_STATS_DELETED));
                                    delay(1000);
                                  }
                                }
                                reloadAfterBookAction();
                              });
                   return;
                 case FileBrowserAction::ResetReaderSettings:
                   openDialog(makeUniqueNoThrow<ConfirmationActivity>(
                                  renderer, mappedInput,
                                  BookActions::confirmationHeading(StrId::STR_RESET_BOOK_READER_SETTINGS), book.title),
                              [this, book](const ActivityResult& confirmation) {
                                if (!confirmation.isCancelled) {
                                  if (!BookActions::resetBookReaderSettings(book.path)) {
                                    LOG_ERR("LIB", "Failed to reset reader settings for: %s", book.path.c_str());
                                  } else {
                                    BookActions::drawToast(renderer, tr(STR_BOOK_READER_SETTINGS_RESET));
                                    delay(1000);
                                  }
                                }
                                reloadAfterBookAction();
                              });
                   return;
                 case FileBrowserAction::ToggleCompleted: {
                   bool completed = false;
                   if (BookActions::toggleBookCompleted(book.path, book.title, completed)) {
                     BookActions::drawToast(renderer, completed ? tr(STR_MARKED_FINISHED) : tr(STR_MARKED_UNFINISHED));
                     delay(1000);
                   }
                   reloadAfterBookAction();
                   return;
                 }
                 case FileBrowserAction::EpubRenderMode: {
                   const uint8_t currentIndex =
                       BookActions::epubRenderModeDisplayIndex(EpubReaderActivity::loadBookRenderMode(book.path));
                   openDialog(makeUniqueNoThrow<OptionSelectionActivity>(
                                  renderer, mappedInput, "LibraryEpubRenderModeSelect", StrId::STR_EPUB_RENDER_MODE,
                                  BookActions::epubRenderModeOptions(), currentIndex),
                              [this, book](const ActivityResult& selectionResult) {
                                if (!selectionResult.isCancelled) {
                                  const auto* selection = std::get_if<OptionSelectionResult>(&selectionResult.data);
                                  if (selection != nullptr &&
                                      !EpubReaderActivity::saveBookRenderMode(
                                          book.path, BookActions::epubRenderModeForDisplayIndex(selection->index))) {
                                    LOG_ERR("LIB", "Failed to save render mode for: %s", book.path.c_str());
                                  }
                                }
                                reloadAfterBookAction();
                              });
                   return;
                 }
                 case FileBrowserAction::RemoveFromRecents:
                   promptRemoveBook(book.path, book.title);
                   return;
                 case FileBrowserAction::SendNearby:
                   activityManager.goToNearbyBookSend(book.path, false);
                   return;
                 case FileBrowserAction::PinFavorite:
                 case FileBrowserAction::UnpinFavorite:
                 case FileBrowserAction::PinBootFavorite:
                 case FileBrowserAction::UnpinBootFavorite:
                 case FileBrowserAction::UploadFolderProgress:
                 case FileBrowserAction::SetSleepFolder:
                 case FileBrowserAction::ClearSleepFolder:
                 case FileBrowserAction::ViewBookmarks:
                 case FileBrowserAction::ViewClippings:
                 case FileBrowserAction::DeleteBookmarks:
                 case FileBrowserAction::DeleteClippings:
                 case FileBrowserAction::Rename:
                   return;
               }
             });
}
