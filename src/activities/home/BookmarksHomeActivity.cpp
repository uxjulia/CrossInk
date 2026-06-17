#include "BookmarksHomeActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "../reader/EpubReaderBookmarkListActivity.h"
#include "BookmarkStore.h"
#include "CrossPointState.h"
#include "FileBrowserActionActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr unsigned long BOOKMARK_DELETE_HOLD_MS = 1000;
}

void BookmarksHomeActivity::reloadBookmarks() {
  books.clear();
  BookmarkStore::getAllBookmarkedBooks(books);
  if (books.empty()) {
    selectedIndex = 0;
  } else if (selectedIndex >= static_cast<int>(books.size())) {
    selectedIndex = static_cast<int>(books.size()) - 1;
  }
}

void BookmarksHomeActivity::onEnter() {
  Activity::onEnter();

  reloadBookmarks();
  selectedIndex = 0;
  requestUpdate();
}

void BookmarksHomeActivity::onExit() {
  Activity::onExit();
  books.clear();
}

void BookmarksHomeActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  if (!books.empty() && !longPressOpenHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= BOOKMARK_DELETE_HOLD_MS) {
    longPressOpenHandled = true;
    showBookmarkBookActionMenu(selectedIndex, true);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (longPressOpenHandled) {
      longPressOpenHandled = false;
      return;
    }
    if (!books.empty() && selectedIndex >= 0 && selectedIndex < static_cast<int>(books.size())) {
      openBookmarkList(selectedIndex);
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

void BookmarksHomeActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BOOKMARKS));

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

void BookmarksHomeActivity::showBookmarkBookActionMenu(int bookIndex, bool ignoreInitialConfirmRelease) {
  if (bookIndex < 0 || bookIndex >= static_cast<int>(books.size())) return;

  const BookmarkedBookEntry entry = books[bookIndex];
  std::vector<FileBrowserActionActivity::MenuItem> items;
  items.reserve(1);
  items.push_back({FileBrowserAction::Delete, StrId::STR_DELETE});

  startActivityForResult(std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, entry.bookTitle,
                                                                     std::move(items), ignoreInitialConfirmRelease),
                         [this, entry](const ActivityResult& result) {
                           longPressOpenHandled = false;
                           const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
                           if (!result.isCancelled && actionResult &&
                               static_cast<FileBrowserAction>(actionResult->action) == FileBrowserAction::Delete) {
                             BOOKMARKS.loadForBook(entry.bookPath, entry.bookTitle, entry.bookAuthor, entry.bookType);
                             BOOKMARKS.clearAll();
                           }
                           reloadBookmarks();
                           requestUpdate();
                         });
}

void BookmarksHomeActivity::openBookmarkList(int bookIndex) {
  const BookmarkedBookEntry entry = books[bookIndex];
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
            LOG_ERR("BKA", "openBookmarkList: result.data holds unexpected variant type, not BookmarkResult");
            requestUpdate();
          }
        } else {
          reloadBookmarks();
          requestUpdate();
        }
      });
}
