#include "RecentBooksGridActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Xtc.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "fontIds.h"

namespace {
constexpr int kCoverCornerRadius = 2;

int moveHorizontalInGrid(const int currentIndex, const int totalItems, const bool moveRight) {
  if (totalItems <= 0) return 0;
  return moveRight ? ButtonNavigator::nextIndex(currentIndex, totalItems)
                   : ButtonNavigator::previousIndex(currentIndex, totalItems);
}

int moveVerticalInGrid(const int currentIndex, const int totalItems, const int columns, const int itemsPerPage,
                       const bool moveDown) {
  if (totalItems <= 0 || columns <= 0) return 0;

  const int safeItemsPerPage = std::max(columns, itemsPerPage);
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
}  // namespace

void RecentBooksGridActivity::loadRecentBooks() {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(books.size(), static_cast<size_t>(MAX_GRID_BOOKS)));

  for (const auto& book : books) {
    if (recentBooks.size() >= MAX_GRID_BOOKS) break;
    if (!Storage.exists(book.path.c_str())) continue;
    recentBooks.push_back(book);
  }
}

void RecentBooksGridActivity::loadPageCovers(int pageStart) {
  const int pageEnd = std::min(pageStart + BOOKS_PER_PAGE, static_cast<int>(recentBooks.size()));

  bool needsGeneration = false;
  for (int i = pageStart; i < pageEnd; ++i) {
    if (recentBooks[i].coverBmpPath.empty()) continue;
    const std::string thumbPath = UITheme::getCoverThumbPath(recentBooks[i].coverBmpPath, COVER_HEIGHT);
    if (!Storage.exists(thumbPath.c_str())) {
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
    RecentBook& book = recentBooks[i];
    const std::string coverPath =
        book.coverBmpPath.empty() ? "" : UITheme::getCoverThumbPath(book.coverBmpPath, COVER_HEIGHT);
    if (coverPath.empty() || !Storage.exists(coverPath.c_str())) {
      if (FsHelpers::hasEpubExtension(book.path)) {
        Epub epub(book.path, "/.crosspoint");
        if (epub.load(false, true)) {
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + processedCount * (90 / totalToProcess));
          if (!epub.generateThumbBmp(COVER_HEIGHT)) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
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
          GUI.fillPopupProgress(renderer, popupRect, 10 + processedCount * (90 / totalToProcess));
          if (!xtc.generateThumbBmp(COVER_HEIGHT)) {
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
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
  loadedPageStart = -1;
  requestUpdate();
}

void RecentBooksGridActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
}

void RecentBooksGridActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && selectorIndex < recentBooks.size()) {
      LOG_DBG("RBGA", "Selected recent book: %s", recentBooks[selectorIndex].path.c_str());
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  const int listSize = static_cast<int>(recentBooks.size());
  constexpr int columns = 3;
  constexpr int itemsPerPage = 9;

  buttonNavigator.onRelease({MappedInputManager::Button::Right}, [this, listSize] {
    selectorIndex = moveHorizontalInGrid(static_cast<int>(selectorIndex), listSize, true);
    requestUpdate();
  });
  buttonNavigator.onRelease({MappedInputManager::Button::Left}, [this, listSize] {
    selectorIndex = moveHorizontalInGrid(static_cast<int>(selectorIndex), listSize, false);
    requestUpdate();
  });
  buttonNavigator.onRelease({MappedInputManager::Button::Down}, [this, listSize] {
    selectorIndex = moveVerticalInGrid(static_cast<int>(selectorIndex), listSize, columns, itemsPerPage, true);
    requestUpdate();
  });
  buttonNavigator.onRelease({MappedInputManager::Button::Up}, [this, listSize] {
    selectorIndex = moveVerticalInGrid(static_cast<int>(selectorIndex), listSize, columns, itemsPerPage, false);
    requestUpdate();
  });

  buttonNavigator.onContinuous({MappedInputManager::Button::Right}, [this, listSize] {
    selectorIndex = moveHorizontalInGrid(static_cast<int>(selectorIndex), listSize, true);
    requestUpdate();
  });
  buttonNavigator.onContinuous({MappedInputManager::Button::Left}, [this, listSize] {
    selectorIndex = moveHorizontalInGrid(static_cast<int>(selectorIndex), listSize, false);
    requestUpdate();
  });
  buttonNavigator.onContinuous({MappedInputManager::Button::Down}, [this, listSize] {
    selectorIndex = moveVerticalInGrid(static_cast<int>(selectorIndex), listSize, columns, itemsPerPage, true);
    requestUpdate();
  });
  buttonNavigator.onContinuous({MappedInputManager::Button::Up}, [this, listSize] {
    selectorIndex = moveVerticalInGrid(static_cast<int>(selectorIndex), listSize, columns, itemsPerPage, false);
    requestUpdate();
  });
}

void RecentBooksGridActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_MENU_RECENT_BOOKS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  constexpr int titleStripHeight = 32;
  constexpr int titleGridGap = 16;
  constexpr int columns = 3;
  constexpr int selectionPadding = 4;
  constexpr int selectionOutlineGap = 2;
  constexpr int selectionOuterInset = selectionPadding + selectionOutlineGap;
  const int rowSpacing = metrics.verticalSpacing + 4;
  const int totalGridWidth = columns * COVER_WIDTH + (columns - 1) * metrics.verticalSpacing;
  const int startXOffset = (pageWidth - totalGridWidth) / 2;

  const int totalBooks = static_cast<int>(recentBooks.size());
  const int totalPages = (totalBooks + BOOKS_PER_PAGE - 1) / BOOKS_PER_PAGE;
  const int currentPage = (totalBooks > 0) ? (static_cast<int>(selectorIndex) / BOOKS_PER_PAGE) : 0;
  const int pageStart = currentPage * BOOKS_PER_PAGE;
  const int pageCount = std::min(BOOKS_PER_PAGE, totalBooks - pageStart);

  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_RECENT_BOOKS));
  } else {
    if (selectorIndex < recentBooks.size()) {
      const int titleLh = renderer.getLineHeight(UI_12_FONT_ID);
      const int titleY = contentTop + (titleStripHeight - titleLh) / 2;
      const std::string truncTitle = renderer.truncatedText(UI_12_FONT_ID, recentBooks[selectorIndex].title.c_str(),
                                                            totalGridWidth, EpdFontFamily::BOLD);
      renderer.drawText(UI_12_FONT_ID, startXOffset, titleY, truncTitle.c_str(), true, EpdFontFamily::BOLD);
    }

    for (int i = 0; i < pageCount; ++i) {
      const int bookIdx = pageStart + i;
      const int col = i % columns;
      const int row = i / columns;
      const int x = startXOffset + col * (COVER_WIDTH + metrics.verticalSpacing);
      const int y = contentTop + titleStripHeight + titleGridGap + row * (COVER_HEIGHT + rowSpacing);

      int bx = x;
      int by = y;
      int bw = COVER_WIDTH;
      int bh = COVER_HEIGHT;
      bool drawn = false;
      const std::string thumbPath = recentBooks[bookIdx].coverBmpPath.empty()
                                        ? ""
                                        : UITheme::getCoverThumbPath(recentBooks[bookIdx].coverBmpPath, COVER_HEIGHT);
      if (!thumbPath.empty() && Storage.exists(thumbPath.c_str())) {
        FsFile file;
        if (Storage.openFileForRead("RBGA", thumbPath, file)) {
          Bitmap bmp(file);
          if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getWidth() > 0 && bmp.getHeight() > 0) {
            bw = std::min(COVER_WIDTH, bmp.getWidth());
            bh = std::min(COVER_HEIGHT, bmp.getHeight());
            bx = x + (COVER_WIDTH - bw) / 2;
            by = y + (COVER_HEIGHT - bh) / 2;
            renderer.drawBitmap(bmp, bx, by, bw, bh);
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

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  if (!recentBooks.empty() && loadedPageStart != pageStart) {
    loadPageCovers(pageStart);
  }
}
