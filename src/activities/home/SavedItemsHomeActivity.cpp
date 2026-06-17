#include "SavedItemsHomeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "../reader/EpubReaderBookmarkListActivity.h"
#include "../reader/EpubReaderClippingListActivity.h"
#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "CrossPointState.h"
#include "FileBrowserActionActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long SAVED_ITEM_DELETE_HOLD_MS = 1000;

void mergeBookmarkEntry(std::vector<SavedBookEntry>& out, const BookmarkedBookEntry& entry) {
  auto it = std::find_if(out.begin(), out.end(), [&](const SavedBookEntry& existing) {
    return existing.bookPath == entry.bookPath && existing.bookType == entry.bookType;
  });
  if (it != out.end()) {
    it->bookmarkCount = entry.count;
    if (it->bookTitle.empty()) it->bookTitle = entry.bookTitle;
    if (it->bookAuthor.empty()) it->bookAuthor = entry.bookAuthor;
    return;
  }
  out.push_back(
      {entry.bookTitle, entry.bookAuthor, entry.bookPath, entry.bookType, entry.count, static_cast<uint16_t>(0)});
}

void mergeClippingEntry(std::vector<SavedBookEntry>& out, const ClippedBookEntry& entry) {
  auto it = std::find_if(out.begin(), out.end(), [&](const SavedBookEntry& existing) {
    return existing.bookPath == entry.bookPath && existing.bookType == entry.bookType;
  });
  if (it != out.end()) {
    it->clippingCount = entry.count;
    if (it->bookTitle.empty()) it->bookTitle = entry.bookTitle;
    if (it->bookAuthor.empty()) it->bookAuthor = entry.bookAuthor;
    return;
  }
  out.push_back(
      {entry.bookTitle, entry.bookAuthor, entry.bookPath, entry.bookType, static_cast<uint16_t>(0), entry.count});
}
}  // namespace

void SavedItemsHomeActivity::reloadSavedBooks() {
  books.clear();

  std::vector<BookmarkedBookEntry> bookmarkedBooks;
  std::vector<ClippedBookEntry> clippedBooks;
  BookmarkStore::getAllBookmarkedBooks(bookmarkedBooks);
  ClippingStore::getAllClippedBooks(clippedBooks);

  books.reserve(bookmarkedBooks.size() + clippedBooks.size());
  for (const auto& entry : bookmarkedBooks) {
    mergeBookmarkEntry(books, entry);
  }
  for (const auto& entry : clippedBooks) {
    mergeClippingEntry(books, entry);
  }

  if (books.empty()) {
    selectedIndex = 0;
  } else if (selectedIndex >= static_cast<int>(books.size())) {
    selectedIndex = static_cast<int>(books.size()) - 1;
  }
}

void SavedItemsHomeActivity::onEnter() {
  Activity::onEnter();
  reloadSavedBooks();
  selectedIndex = 0;
  requestUpdate();
}

void SavedItemsHomeActivity::onExit() {
  books.clear();
  Activity::onExit();
}

void SavedItemsHomeActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  if (!books.empty() && !longPressOpenHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= SAVED_ITEM_DELETE_HOLD_MS) {
    longPressOpenHandled = true;
    showSavedBookActionMenu(selectedIndex, true);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (longPressOpenHandled) {
      longPressOpenHandled = false;
      return;
    }
    if (!books.empty() && selectedIndex >= 0 && selectedIndex < static_cast<int>(books.size())) {
      openSavedItems(selectedIndex);
    }
    return;
  }

  const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, true);
  const int listSize = static_cast<int>(books.size());

  buttonNavigator.onNextRelease([this, listSize] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, listSize);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, listSize] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, listSize);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, listSize, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, listSize, pageItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, listSize, pageItems);
    requestUpdate();
  });
}

void SavedItemsHomeActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_BOOKMARKS_AND_CLIPPINGS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (books.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_BOOKMARKS));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, books.size(), selectedIndex,
        [this](int index) { return books[index].bookTitle; }, [this](int index) { return books[index].bookAuthor; },
        nullptr);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void SavedItemsHomeActivity::openSavedItems(const int bookIndex) {
  if (bookIndex < 0 || bookIndex >= static_cast<int>(books.size())) return;
  const SavedBookEntry entry = books[bookIndex];
  const bool hasBookmarks = entry.bookmarkCount > 0;
  const bool hasClippings = entry.clippingCount > 0;

  if (hasBookmarks && hasClippings) {
    showSavedKindMenu(bookIndex);
  } else if (hasBookmarks) {
    openBookmarkList(entry);
  } else if (hasClippings) {
    openClippingList(entry);
  }
}

void SavedItemsHomeActivity::showSavedKindMenu(const int bookIndex) {
  if (bookIndex < 0 || bookIndex >= static_cast<int>(books.size())) return;
  const SavedBookEntry entry = books[bookIndex];

  std::vector<FileBrowserActionActivity::MenuItem> items;
  items.reserve(2);
  items.push_back({FileBrowserAction::ViewBookmarks, StrId::STR_BOOKMARKS});
  items.push_back({FileBrowserAction::ViewClippings, StrId::STR_CLIPPINGS});

  startActivityForResult(
      std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, entry.bookTitle, std::move(items)),
      [this, entry](const ActivityResult& result) {
        const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
        if (result.isCancelled || !actionResult) {
          requestUpdate();
          return;
        }

        switch (static_cast<FileBrowserAction>(actionResult->action)) {
          case FileBrowserAction::ViewBookmarks:
            openBookmarkList(entry);
            break;
          case FileBrowserAction::ViewClippings:
            openClippingList(entry);
            break;
          default:
            requestUpdate();
            break;
        }
      });
}

void SavedItemsHomeActivity::showSavedBookActionMenu(const int bookIndex, const bool ignoreInitialConfirmRelease) {
  if (bookIndex < 0 || bookIndex >= static_cast<int>(books.size())) return;
  const SavedBookEntry entry = books[bookIndex];

  std::vector<FileBrowserActionActivity::MenuItem> items;
  items.reserve(2);
  if (entry.bookmarkCount > 0) {
    items.push_back({FileBrowserAction::DeleteBookmarks, StrId::STR_DELETE_BOOKMARKS});
  }
  if (entry.clippingCount > 0) {
    items.push_back({FileBrowserAction::DeleteClippings, StrId::STR_DELETE_CLIPPINGS});
  }

  startActivityForResult(
      std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, entry.bookTitle, std::move(items),
                                                  ignoreInitialConfirmRelease),
      [this, entry](const ActivityResult& result) {
        longPressOpenHandled = false;
        const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
        if (!result.isCancelled && actionResult) {
          switch (static_cast<FileBrowserAction>(actionResult->action)) {
            case FileBrowserAction::DeleteBookmarks:
              BOOKMARKS.loadForBook(entry.bookPath, entry.bookTitle, entry.bookAuthor, entry.bookType);
              BOOKMARKS.clearAll();
              break;
            case FileBrowserAction::DeleteClippings:
              CLIPPINGS.loadForBook(entry.bookPath, entry.bookTitle, entry.bookAuthor, entry.bookType);
              CLIPPINGS.clearAll();
              break;
            default:
              break;
          }
        }
        reloadSavedBooks();
        requestUpdate();
      });
}

void SavedItemsHomeActivity::openBookmarkList(const SavedBookEntry& entry) {
  BOOKMARKS.loadForBook(entry.bookPath, entry.bookTitle, entry.bookAuthor, entry.bookType);

  startActivityForResult(
      std::make_unique<EpubReaderBookmarkListActivity>(renderer, mappedInput, BOOKMARKS.getBookmarks()),
      [this, entry](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto* bm = std::get_if<BookmarkResult>(&result.data);
          if (bm) {
            APP_STATE.pendingBookmarkSpine = bm->spineIndex;
            APP_STATE.pendingBookmarkProgress = bm->progress;
            APP_STATE.pendingBookmarkParagraphIndex = bm->paragraphIndex;
            APP_STATE.pendingClippingIndex = UINT16_MAX;
            APP_STATE.saveToFile();
            onSelectBook(entry.bookPath);
          } else {
            LOG_ERR("SVA", "openBookmarkList: unexpected result variant");
            requestUpdate();
          }
        } else {
          reloadSavedBooks();
          requestUpdate();
        }
      });
}

void SavedItemsHomeActivity::openClippingList(const SavedBookEntry& entry) {
  CLIPPINGS.loadForBook(entry.bookPath, entry.bookTitle, entry.bookAuthor, entry.bookType);

  startActivityForResult(
      std::make_unique<EpubReaderClippingListActivity>(renderer, mappedInput, CLIPPINGS.getClippings()),
      [this, entry](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto* clipping = std::get_if<ClippingJumpResult>(&result.data);
          if (clipping) {
            APP_STATE.pendingBookmarkSpine = clipping->spineIndex;
            APP_STATE.pendingBookmarkProgress =
                clipping->pageCount > 0 ? static_cast<float>(clipping->page) / clipping->pageCount : 0.0f;
            APP_STATE.pendingBookmarkParagraphIndex = clipping->paragraphIndex;
            APP_STATE.pendingClippingIndex = clipping->clippingIndex;
            APP_STATE.saveToFile();
            onSelectBook(entry.bookPath);
          } else {
            LOG_ERR("SVA", "openClippingList: unexpected result variant");
            requestUpdate();
          }
        } else {
          reloadSavedBooks();
          requestUpdate();
        }
      });
}
