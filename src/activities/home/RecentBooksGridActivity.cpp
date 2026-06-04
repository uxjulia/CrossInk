#include "RecentBooksGridActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "BookActions.h"
#include "CrossPointSettings.h"
#include "FileBrowserActionActivity.h"
#include "MappedInputManager.h"
#include "RecentBookProgress.h"
#include "RecentBooksStore.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/themes/lyra/LyraTheme.h"
#include "fontIds.h"

namespace {
constexpr int kCoverCornerRadius = 2;
constexpr int kGridColumns = 3;
constexpr unsigned long kLongPressMs = 1000;
constexpr float kCircleRadians = 6.2831853f;
constexpr float kCircleRadiansPerPercent = kCircleRadians / 100.0f;
constexpr int kLyraGridContentTop =
    LyraMetrics::values.topPadding + LyraMetrics::values.headerHeight + LyraMetrics::values.verticalSpacing;
constexpr int kLyraGridSpacing = LyraMetrics::values.verticalSpacing;

void drawGridHeader(const GfxRenderer& renderer, const int pageWidth) {
  const Rect rect{0, LyraMetrics::values.topPadding, pageWidth, LyraMetrics::values.headerHeight};
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  const int batteryX = rect.x + rect.width - 12 - LyraMetrics::values.batteryWidth;
  GUI.drawBatteryRight(renderer,
                       Rect{batteryX, rect.y + 5, LyraMetrics::values.batteryWidth, LyraMetrics::values.batteryHeight},
                       showBatteryPercentage);

  const int titleMaxWidth = rect.width - LyraMetrics::values.contentSidePadding * 3;
  const std::string title =
      renderer.truncatedText(UI_12_FONT_ID, tr(STR_MENU_RECENT_BOOKS), titleMaxWidth, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, rect.x + LyraMetrics::values.contentSidePadding,
                    rect.y + LyraMetrics::values.batteryBarHeight + 3, title.c_str(), true, EpdFontFamily::BOLD);
  renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width - 1, rect.y + rect.height - 3, 3, true);
}

void drawInlineProgressCircle(const GfxRenderer& renderer, const int x, const int y, const int size,
                              const float progressPercent) {
  const int radius = size / 2;
  if (radius <= 2) return;

  const int centerX = x + radius;
  const int centerY = y + radius;
  const int outerRadius = radius;
  const int innerRadius = std::max(1, radius - std::max(2, size / 4));
  const int outerRadiusSq = outerRadius * outerRadius;
  const int innerRadiusSq = innerRadius * innerRadius;
  const float sweepRadians = std::clamp(progressPercent, 0.0f, 100.0f) * kCircleRadiansPerPercent;

  for (int dy = -outerRadius; dy <= outerRadius; ++dy) {
    for (int dx = -outerRadius; dx <= outerRadius; ++dx) {
      const int distanceSq = dx * dx + dy * dy;
      if (distanceSq > outerRadiusSq || distanceSq < innerRadiusSq) {
        continue;
      }

      const int px = centerX + dx;
      const int py = centerY + dy;
      renderer.fillRectDither(px, py, 1, 1, Color::LightGray);

      float angle = std::atan2(static_cast<float>(dx), static_cast<float>(-dy));
      if (angle < 0.0f) {
        angle += kCircleRadians;
      }
      if (angle <= sweepRadians) {
        renderer.drawPixel(px, py, true);
      }
    }
  }
}

int moveHorizontalInGrid(const int currentIndex, const int totalItems, const bool moveRight) {
  if (totalItems <= 0) return 0;
  return moveRight ? ButtonNavigator::nextIndex(currentIndex, totalItems)
                   : ButtonNavigator::previousIndex(currentIndex, totalItems);
}

int moveVerticalInGrid(const int currentIndex, const int totalItems, const int columns, const int itemsPerPage,
                       const bool moveDown) {
  if (totalItems <= 0 || columns <= 0) return 0;

  const int safeItemsPerPage = std::max(columns, itemsPerPage);
  // Contract: safeItemsPerPage should describe whole grid rows. Partial rows
  // are allowed only on the final page after totalItems is applied below.
  if (safeItemsPerPage % columns != 0) {
    LOG_ERR("RBGA", "moveVerticalInGrid requires whole rows (itemsPerPage=%d columns=%d)", safeItemsPerPage, columns);
    return currentIndex;
  }
  const int totalPages = (totalItems + safeItemsPerPage - 1) / safeItemsPerPage;
  const int currentPage = currentIndex / safeItemsPerPage;
  const int indexInPage = currentIndex % safeItemsPerPage;
  const int currentRow = indexInPage / columns;
  const int currentColumn = indexInPage % columns;
  const int rowsPerPage = safeItemsPerPage / columns;

  if (moveDown) {
    if (currentRow < rowsPerPage - 1) {
      const int nextRowCandidate = currentIndex + columns;
      if (nextRowCandidate < totalItems && (nextRowCandidate / safeItemsPerPage) == currentPage) {
        return nextRowCandidate;
      }
    }

    const int nextPage = (currentPage + 1) % totalPages;
    const int nextPageStart = nextPage * safeItemsPerPage;
    const int nextPageCount = std::min(safeItemsPerPage, totalItems - nextPageStart);
    if (nextPageCount <= 0) return currentIndex;

    if (currentColumn < nextPageCount) {
      return nextPageStart + currentColumn;
    }
    return nextPageStart + nextPageCount - 1;
  }

  if (currentRow > 0) {
    return currentIndex - columns;
  }

  const int previousPage = (currentPage - 1 + totalPages) % totalPages;
  const int previousPageStart = previousPage * safeItemsPerPage;
  const int previousPageCount = std::min(safeItemsPerPage, totalItems - previousPageStart);
  if (previousPageCount <= 0) return currentIndex;

  int previousPageCandidate = previousPageStart + ((previousPageCount - 1) / columns) * columns + currentColumn;
  while (previousPageCandidate >= previousPageStart + previousPageCount) {
    previousPageCandidate -= columns;
  }
  return std::max(previousPageStart, previousPageCandidate);
}

void updateRecentBookCoverPath(const RecentBook& book, const std::string& coverBmpPath) {
  if (!RECENT_BOOKS.updateBook(book.path, book.title, book.author, coverBmpPath)) {
    LOG_ERR("RBGA", "failed to update recent book metadata: %s", book.path.c_str());
  }
}

bool hasThumbnailPlaceholder(const std::string& coverBmpPath) {
  return coverBmpPath.find("[WIDTH]") != std::string::npos || coverBmpPath.find("[HEIGHT]") != std::string::npos;
}

bool needsCoverThumbGeneration(const RecentBook& book, const std::string& thumbPath) {
  if (thumbPath.empty() || !Storage.exists(thumbPath.c_str())) {
    return true;
  }
  if (!FsHelpers::hasXtcExtension(book.path)) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("RBGA", thumbPath, file)) {
    return true;
  }
  Bitmap bitmap(file);
  const bool hasExpectedSize = bitmap.parseHeaders() == BmpReaderError::Ok &&
                               bitmap.getWidth() == RecentBooksGridActivity::COVER_WIDTH &&
                               bitmap.getHeight() == RecentBooksGridActivity::COVER_HEIGHT;
  file.close();
  return !hasExpectedSize;
}

void calculateCoverFillCrop(const Bitmap& bitmap, float& cropX, float& cropY) {
  cropX = 0.0f;
  cropY = 0.0f;
  const float srcW = static_cast<float>(bitmap.getWidth());
  const float srcH = static_cast<float>(bitmap.getHeight());
  if (srcW <= 0.0f || srcH <= 0.0f) return;

  const float srcRatio = srcW / srcH;
  const float targetRatio = static_cast<float>(RecentBooksGridActivity::COVER_WIDTH) /
                            static_cast<float>(RecentBooksGridActivity::COVER_HEIGHT);
  if (srcRatio > targetRatio) {
    cropX = std::max(0.0f, 1.0f - (targetRatio / srcRatio));
  } else if (srcRatio < targetRatio) {
    cropY = std::max(0.0f, 1.0f - (srcRatio / targetRatio));
  }
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

void ensureReusableCoverPath(RecentBook& book) {
  if (book.coverBmpPath.empty() || hasThumbnailPlaceholder(book.coverBmpPath)) {
    return;
  }

  const std::string reusablePath = getReusableCoverPath(book);
  if (reusablePath.empty() || reusablePath == book.coverBmpPath) {
    return;
  }

  book.coverBmpPath = reusablePath;
  updateRecentBookCoverPath(book, reusablePath);
}
}  // namespace

void RecentBooksGridActivity::loadRecentBooks() {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(books.size(), static_cast<size_t>(MAX_GRID_BOOKS)));

  for (const auto& book : books) {
    if (recentBooks.size() >= MAX_GRID_BOOKS) break;
    if (!Storage.exists(book.path.c_str())) continue;
    recentBooks.push_back(BookState{book});
  }
}

void RecentBooksGridActivity::ensureProgressLoaded(const int index) {
  if (index < 0 || index >= static_cast<int>(recentBooks.size())) return;
  if (recentBooks[index].progressLoaded) {
    return;
  }

  recentBooks[index].progress = RecentBookProgress::loadPercent(recentBooks[index].book);
  recentBooks[index].progressLoaded = true;
}

void RecentBooksGridActivity::loadPageCovers(int pageStart) {
  const int pageEnd = std::min(pageStart + BOOKS_PER_PAGE, static_cast<int>(recentBooks.size()));

  bool needsGeneration = false;
  for (int i = pageStart; i < pageEnd; ++i) {
    RecentBook& book = recentBooks[i].book;
    ensureReusableCoverPath(book);
    if (book.coverBmpPath.empty()) {
      needsGeneration = true;
      break;
    }
    const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, COVER_WIDTH, COVER_HEIGHT);
    if (needsCoverThumbGeneration(book, thumbPath)) {
      needsGeneration = true;
      break;
    }
  }
  if (!needsGeneration) {
    loadedPageStart = pageStart;
    return;
  }

  bool showingLoading = false;
  Rect popupRect;
  const int totalToProcess = pageEnd - pageStart;
  int processedCount = 0;

  for (int i = pageStart; i < pageEnd; ++i) {
    RecentBook& book = recentBooks[i].book;
    const std::string coverPath =
        book.coverBmpPath.empty() ? "" : UITheme::getCoverThumbPath(book.coverBmpPath, COVER_WIDTH, COVER_HEIGHT);
    if (needsCoverThumbGeneration(book, coverPath)) {
      if (FsHelpers::hasEpubExtension(book.path)) {
        Epub epub(book.path, "/.crosspoint");
        if (epub.load(false, true)) {
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + (processedCount * 90) / totalToProcess);
          if (epub.generateThumbBmp(COVER_WIDTH, COVER_HEIGHT)) {
            const std::string reusablePath = epub.getThumbBmpPath();
            book.coverBmpPath = reusablePath;
            updateRecentBookCoverPath(book, reusablePath);
          } else {
            updateRecentBookCoverPath(book, "");
            book.coverBmpPath = "";
          }
        }
      } else if (FsHelpers::hasXtcExtension(book.path)) {
        Xtc xtc(book.path, "/.crosspoint");
        if (xtc.load()) {
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + (processedCount * 90) / totalToProcess);
          if (xtc.generateThumbBmp(COVER_WIDTH, COVER_HEIGHT)) {
            const std::string reusablePath = xtc.getThumbBmpPath();
            book.coverBmpPath = reusablePath;
            updateRecentBookCoverPath(book, reusablePath);
          } else {
            updateRecentBookCoverPath(book, "");
            book.coverBmpPath = "";
          }
        }
      }
    }
    processedCount++;
  }

  loadedPageStart = pageStart;
  if (showingLoading) {
    requestUpdate();
  }
}

void RecentBooksGridActivity::onEnter() {
  Activity::onEnter();
  loadRecentBooks();
  selectorIndex = 0;
  loadedPageStart = NO_PAGE_LOADED;
  ensureProgressLoaded(selectorIndex);
  requestUpdate();
}

void RecentBooksGridActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
}

void RecentBooksGridActivity::loop() {
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
    }
    return;
  }

  if (!recentBooks.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(recentBooks.size()) &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= kLongPressMs) {
    longPressFired = true;
    showBookActionMenu(selectorIndex, true);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(recentBooks.size())) {
      LOG_DBG("RBGA", "Selected recent book: %s", recentBooks[selectorIndex].book.path.c_str());
      onSelectBook(recentBooks[selectorIndex].book.path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  const int listSize = static_cast<int>(recentBooks.size());
  enum class NavDirection { Right, Left, Down, Up };
  auto handleNav = [this, listSize](NavDirection direction) {
    switch (direction) {
      case NavDirection::Right:
        selectorIndex = moveHorizontalInGrid(selectorIndex, listSize, true);
        break;
      case NavDirection::Left:
        selectorIndex = moveHorizontalInGrid(selectorIndex, listSize, false);
        break;
      case NavDirection::Down:
        selectorIndex = moveVerticalInGrid(selectorIndex, listSize, kGridColumns, BOOKS_PER_PAGE, true);
        break;
      case NavDirection::Up:
        selectorIndex = moveVerticalInGrid(selectorIndex, listSize, kGridColumns, BOOKS_PER_PAGE, false);
        break;
    }
    ensureProgressLoaded(selectorIndex);
    requestUpdate();
  };

  buttonNavigator.onRelease({MappedInputManager::Button::Right}, [&] { handleNav(NavDirection::Right); });
  buttonNavigator.onRelease({MappedInputManager::Button::Left}, [&] { handleNav(NavDirection::Left); });
  buttonNavigator.onRelease({MappedInputManager::Button::Down}, [&] { handleNav(NavDirection::Down); });
  buttonNavigator.onRelease({MappedInputManager::Button::Up}, [&] { handleNav(NavDirection::Up); });

  buttonNavigator.onContinuous({MappedInputManager::Button::Right}, [&] { handleNav(NavDirection::Right); });
  buttonNavigator.onContinuous({MappedInputManager::Button::Left}, [&] { handleNav(NavDirection::Left); });
  buttonNavigator.onContinuous({MappedInputManager::Button::Down}, [&] { handleNav(NavDirection::Down); });
  buttonNavigator.onContinuous({MappedInputManager::Button::Up}, [&] { handleNav(NavDirection::Up); });
}

void RecentBooksGridActivity::reloadAfterBookAction() {
  loadRecentBooks();
  if (recentBooks.empty()) {
    selectorIndex = 0;
  } else if (selectorIndex >= static_cast<int>(recentBooks.size())) {
    selectorIndex = static_cast<int>(recentBooks.size()) - 1;
  }
  loadedPageStart = NO_PAGE_LOADED;
  ensureProgressLoaded(selectorIndex);
  requestUpdate(true);
}

void RecentBooksGridActivity::promptDeleteBook(const RecentBook& book) {
  const std::string path = book.path;
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBGA", "Delete cancelled");
      return;
    }

    LOG_DBG("RBGA", "Attempting to delete: %s", path.c_str());
    BookActions::clearFileMetadata(path);
    if (!Storage.remove(path.c_str())) {
      LOG_ERR("RBGA", "Failed to delete file: %s", path.c_str());
      return;
    }

    RECENT_BOOKS.removeByPath(path);
    reloadAfterBookAction();
  };

  const std::string heading = tr(STR_DELETE) + std::string("? ");
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, book.title),
                         std::move(handler));
}

void RecentBooksGridActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      LOG_DBG("RBGA", "Remove from recents cancelled");
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      LOG_DBG("RBGA", "Removed from recents: %s", path.c_str());
      reloadAfterBookAction();
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title),
      std::move(handler));
}

void RecentBooksGridActivity::showBookActionMenu(const int bookIndex, const bool ignoreInitialConfirmRelease) {
  if (bookIndex < 0 || bookIndex >= static_cast<int>(recentBooks.size())) return;

  const RecentBook book = recentBooks[bookIndex].book;
  std::vector<FileBrowserActionActivity::MenuItem> items =
      BookActions::buildBookActionItems(book.path, /*includeRemoveFromRecents=*/true);

  startActivityForResult(std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, book.title,
                                                                     std::move(items), ignoreInitialConfirmRelease),
                         [this, book](const ActivityResult& result) {
                           if (result.isCancelled) {
                             return;
                           }

                           const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
                           if (!actionResult) {
                             LOG_ERR("RBGA", "Book action result missing");
                             return;
                           }

                           switch (static_cast<FileBrowserAction>(actionResult->action)) {
                             case FileBrowserAction::Delete:
                               promptDeleteBook(book);
                               return;
                             case FileBrowserAction::DeleteCache:
                               if (!BookActions::clearBookCache(book.path)) {
                                 LOG_ERR("RBGA", "Failed to clear book cache for: %s", book.path.c_str());
                               } else {
                                 BookActions::drawToast(renderer, tr(STR_BOOK_CACHE_DELETED));
                                 delay(1000);
                               }
                               reloadAfterBookAction();
                               return;
                             case FileBrowserAction::ToggleCompleted: {
                               bool completed = false;
                               if (BookActions::toggleEpubCompleted(book.path, book.title, completed)) {
                                 BookActions::drawToast(
                                     renderer, completed ? tr(STR_MARKED_FINISHED) : tr(STR_MARKED_UNFINISHED));
                                 delay(1000);
                               }
                               reloadAfterBookAction();
                               return;
                             }
                             case FileBrowserAction::RemoveFromRecents:
                               promptRemoveBook(book.path, book.title);
                               return;
                             case FileBrowserAction::PinFavorite:
                             case FileBrowserAction::UnpinFavorite:
                             case FileBrowserAction::SetSleepFolder:
                             case FileBrowserAction::ClearSleepFolder:
                               return;
                           }
                         });
}

void RecentBooksGridActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  drawGridHeader(renderer, pageWidth);
  constexpr int contentTop = kLyraGridContentTop;
  constexpr int titleStripHeight = 32;
  constexpr int titleGridGap = 16;
  constexpr int selectionPadding = 4;
  constexpr int selectionOutlineGap = 2;
  constexpr int selectionOuterInset = selectionPadding + selectionOutlineGap;
  constexpr int gridSpacing = kLyraGridSpacing;
  constexpr int rowSpacing = gridSpacing + 4;
  constexpr int totalGridWidth = kGridColumns * COVER_WIDTH + (kGridColumns - 1) * gridSpacing;
  const int startXOffset = (pageWidth - totalGridWidth) / 2;

  const int totalBooks = static_cast<int>(recentBooks.size());
  const int totalPages = (totalBooks + BOOKS_PER_PAGE - 1) / BOOKS_PER_PAGE;
  const int currentPage = (totalBooks > 0) ? (selectorIndex / BOOKS_PER_PAGE) : 0;
  const int pageStart = currentPage * BOOKS_PER_PAGE;
  const int pageCount = std::min(BOOKS_PER_PAGE, totalBooks - pageStart);

  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    if (selectorIndex >= 0 && selectorIndex < static_cast<int>(recentBooks.size())) {
      const int titleLh = renderer.getLineHeight(UI_10_FONT_ID);
      const int titleY = contentTop + (titleStripHeight - titleLh) / 2;
      const auto& selectedBook = recentBooks[selectorIndex];
      const bool hasProgress = selectedBook.progressLoaded && RecentBookProgress::hasPercent(selectedBook.progress);
      const std::string progressLabel = hasProgress ? RecentBookProgress::formatPercent(selectedBook.progress) : "";

      const int progressIconSize = hasProgress ? std::max(8, titleLh - 2) : 0;
      const char* progressSeparator = "  |   ";
      const int separatorWidth =
          hasProgress ? renderer.getTextWidth(UI_10_FONT_ID, progressSeparator, EpdFontFamily::REGULAR) : 0;
      const int progressWidth =
          hasProgress ? renderer.getTextWidth(UI_10_FONT_ID, progressLabel.c_str(), EpdFontFamily::REGULAR) : 0;
      const int progressIconGap = hasProgress ? renderer.getTextWidth(UI_10_FONT_ID, "  ", EpdFontFamily::REGULAR) : 0;
      const int progressSuffixWidth =
          hasProgress ? separatorWidth + progressWidth + progressIconGap + progressIconSize : 0;
      const int titleMaxWidth = std::max(0, totalGridWidth - progressSuffixWidth);
      const std::string truncTitle =
          renderer.truncatedText(UI_10_FONT_ID, selectedBook.book.title.c_str(), titleMaxWidth, EpdFontFamily::REGULAR);
      renderer.drawText(UI_10_FONT_ID, startXOffset, titleY, truncTitle.c_str(), true, EpdFontFamily::REGULAR);
      if (hasProgress) {
        const int titleWidth = renderer.getTextWidth(UI_10_FONT_ID, truncTitle.c_str(), EpdFontFamily::REGULAR);
        int progressX = startXOffset + titleWidth;
        progressX = std::min(progressX, startXOffset + totalGridWidth - progressSuffixWidth);
        renderer.drawText(UI_10_FONT_ID, progressX, titleY, progressSeparator, true, EpdFontFamily::REGULAR);
        progressX += separatorWidth;
        renderer.drawText(UI_10_FONT_ID, progressX, titleY, progressLabel.c_str(), true, EpdFontFamily::REGULAR);
        const int iconX = progressX + progressWidth + progressIconGap;
        const int iconY = titleY + (titleLh - progressIconSize) / 2;
        drawInlineProgressCircle(renderer, iconX, iconY, progressIconSize, selectedBook.progress);
      }
    }

    for (int i = 0; i < pageCount; ++i) {
      const int bookIdx = pageStart + i;
      const int col = i % kGridColumns;
      const int row = i / kGridColumns;
      const int x = startXOffset + col * (COVER_WIDTH + gridSpacing);
      const int y = contentTop + titleStripHeight + titleGridGap + row * (COVER_HEIGHT + rowSpacing);

      const int bx = x;
      const int by = y;
      constexpr int bw = COVER_WIDTH;
      constexpr int bh = COVER_HEIGHT;
      bool drawn = false;
      const std::string thumbPath =
          recentBooks[bookIdx].book.coverBmpPath.empty()
              ? ""
              : UITheme::getCoverThumbPath(recentBooks[bookIdx].book.coverBmpPath, COVER_WIDTH, COVER_HEIGHT);
      if (!thumbPath.empty() && Storage.exists(thumbPath.c_str())) {
        FsFile file;
        if (Storage.openFileForRead("RBGA", thumbPath, file)) {
          Bitmap bmp(file);
          if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getWidth() > 0 && bmp.getHeight() > 0) {
            float cropX = 0.0f;
            float cropY = 0.0f;
            calculateCoverFillCrop(bmp, cropX, cropY);
            renderer.fillRoundedRect(bx, by, bw, bh, kCoverCornerRadius, Color::White);
            renderer.drawBitmap(bmp, bx, by, bw, bh, cropX, cropY);
            renderer.maskRoundedRectOutsideCorners(bx, by, bw, bh, kCoverCornerRadius, Color::White);
            renderer.drawRoundedRect(bx, by, bw, bh, 2, kCoverCornerRadius, true);
            drawn = true;
          }
          file.close();
        }
      }
      if (!drawn) {
        renderer.fillRoundedRect(bx, by, bw, bh, kCoverCornerRadius, Color::White);
        renderer.drawRoundedRect(bx, by, bw, bh, 2, kCoverCornerRadius, true);
        renderer.drawIcon(BookIcon, bx + (bw - 32) / 2, by + (bh - 32) / 2, 32, 32);
      }
      if (bookIdx == static_cast<int>(selectorIndex)) {
        renderer.drawRoundedRect(bx - selectionPadding, by - selectionPadding, bw + selectionPadding * 2,
                                 bh + selectionPadding * 2, 3, kCoverCornerRadius + selectionPadding, true);
        renderer.drawRoundedRect(bx - selectionOuterInset, by - selectionOuterInset, bw + selectionOuterInset * 2,
                                 bh + selectionOuterInset * 2, 1, kCoverCornerRadius + selectionOuterInset, true);
      }
    }

    if (totalPages > 1) {
      constexpr int dotSize = 8;
      constexpr int dotSpacing = 6;
      const int totalDotWidth = totalPages * dotSize + (totalPages - 1) * dotSpacing;
      const int dotsStartX = (pageWidth - totalDotWidth) / 2;
      const int dotY = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - 4;
      for (int p = 0; p < totalPages; p++) {
        const int dx = dotsStartX + p * (dotSize + dotSpacing);
        if (p == currentPage) {
          renderer.fillRect(dx, dotY, dotSize, dotSize, true);
        } else {
          renderer.drawRect(dx, dotY, dotSize, dotSize, true);
        }
      }
    }
  }

  // The four physical hint slots are already occupied; Up/Down still navigate
  // the grid but are not rendered in this compact hint bar.
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (!recentBooks.empty() && loadedPageStart != pageStart) {
    loadPageCovers(pageStart);
  }
}
