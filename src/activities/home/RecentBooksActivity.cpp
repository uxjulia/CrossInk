#include "RecentBooksActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <memory>

#include "BookActions.h"
#include "FileBrowserActionActivity.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/reader/EpubReaderActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/CompactHeader.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr size_t MAX_LIST_RECENT_BOOKS = 10;
// Hold threshold for the long-press action menu (firmware convention).
constexpr unsigned long LONG_PRESS_MS = 1000;
constexpr unsigned long ACTION_FEEDBACK_MS = 1000;
constexpr fui::ActionId ACTION_ROW = 1;
}  // namespace

RecentBooksActivity::RecentBooksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("RecentBooks", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void RecentBooksActivity::loadRecentBooks() {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(books.size(), MAX_LIST_RECENT_BOOKS));

  for (const auto& book : books) {
    if (recentBooks.size() >= MAX_LIST_RECENT_BOOKS) {
      break;
    }
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }
    recentBooks.push_back(book);
  }
}

void RecentBooksActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<RecentBooksActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->recentBooks.size())) return;
  self->selectorIndex = static_cast<size_t>(event.value);
  if (event.longPress) {
    self->app.clearTapFlash();
    self->showBookActionMenu(self->selectorIndex);
    return;
  }
  // Opening the book leaves this screen; a lingering flash would gray an
  // unrelated row when the list next appears.
  self->app.clearTapFlash();
  self->onSelectBook(self->recentBooks[self->selectorIndex].path);
}

void RecentBooksActivity::onEnter() {
  Activity::onEnter();

  // Prune entries whose backing files are gone; this is one of two interaction
  // points where the persistent store gets cleaned.
  if (RECENT_BOOKS.pruneMissing()) {
    RECENT_BOOKS.saveToFile();
  }

  // Load data
  loadRecentBooks();

  selectorIndex = 0;
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &RecentBooksActivity::onRowEvent, this);
  app.setScreen(&RecentBooksActivity::listScreen, this);
  requestUpdate();
}

void RecentBooksActivity::onExit() {
  Activity::onExit();
  recentBooks.clear();
}

void RecentBooksActivity::loop() {
  if (pendingCacheDeletedFeedback && millis() - cacheDeletedFeedbackShowTime >= ACTION_FEEDBACK_MS) {
    pendingCacheDeletedFeedback = false;
    requestUpdate();
    return;
  }

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    onGoHome();
    return;
  }
  const int listSize = static_cast<int>(recentBooks.size());
  // After a long-press has fired, swallow input until Confirm is physically released
  // (so the release doesn't also open the book; re-arm only once the button is up).
  if (longPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressFired = false;
    }
    return;
  }

  // Long-press Confirm on the selected book: open the same action menu shape used by File Browser.
  // Fires when the hold times out while still held (firmware hold-to-act pattern,
  // cf. FileBrowserActivity BACK long-press).
  if (!recentBooks.empty() && selectorIndex < recentBooks.size() &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MS) {
    longPressFired = true;
    showBookActionMenu(selectorIndex, true);
    return;
  }

  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent dispatch.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      // No pressed-state repaint: the render it triggers would drop a slow
      // tap's release inside the uiReady window (tap-to-activate needed two
      // taps), and it costs a second e-ink refresh per tap.
      if (app.invalidated()) requestUpdate();
      if (event) return;  // dispatched to onRowEvent
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!recentBooks.empty() && selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  // Swipes scroll the viewport; the selection stays put and button navigation
  // pulls the view back to it.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
    const int next = scrollListBy(topIndex, delta, visibleRows, listSize);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  const auto moveSelection = [this, listSize](const int index) {
    selectorIndex = static_cast<size_t>(index);
    topIndex = followListSelection(static_cast<int>(selectorIndex), topIndex, visibleRows, listSize);
    requestUpdate();
  };
  buttonNavigator.onNextRelease([this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize));
  });
  buttonNavigator.onPreviousRelease([this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize));
  });
  buttonNavigator.onNextContinuous([this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, visibleRows));
  });
  buttonNavigator.onPreviousContinuous([this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, visibleRows));
  });
}

void RecentBooksActivity::reloadAfterBookAction() {
  loadRecentBooks();
  if (recentBooks.empty()) {
    selectorIndex = 0;
  } else if (selectorIndex >= recentBooks.size()) {
    selectorIndex = recentBooks.size() - 1;
  }
  topIndex =
      followListSelection(static_cast<int>(selectorIndex), topIndex, visibleRows, static_cast<int>(recentBooks.size()));
  requestUpdate(true);
}

void RecentBooksActivity::promptDeleteBook(const RecentBook& book) {
  const std::string path = book.path;
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      return;
    }

    BookActions::clearFileMetadata(path);
    if (!Storage.remove(path.c_str())) {
      LOG_ERR("RBA", "Failed to delete file: %s", path.c_str());
      return;
    }

    RECENT_BOOKS.removeByPath(path);
    reloadAfterBookAction();
  };

  const std::string heading = tr(STR_DELETE) + std::string("? ");
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, book.title),
                         std::move(handler));
}

void RecentBooksActivity::promptRemoveBook(const std::string& path, const std::string& title) {
  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      return;
    }
    if (RECENT_BOOKS.removeByPath(path)) {
      reloadAfterBookAction();
    }
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title,
                                             /*ignoreInitialConfirmRelease=*/false),
      std::move(handler));
}

void RecentBooksActivity::showBookActionMenu(const size_t bookIndex, const bool ignoreInitialConfirmRelease) {
  if (bookIndex >= recentBooks.size()) return;

  const RecentBook book = recentBooks[bookIndex];
  std::vector<FileBrowserActionActivity::MenuItem> items =
      BookActions::buildBookActionItems(book.path, /*includeRemoveFromRecents=*/true);
  if (BookActions::canSendNearby(book.path)) {
    items.push_back({FileBrowserAction::SendNearby, StrId::STR_SEND_NEARBY_BOOK});
  }

  startActivityForResult(
      std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, book.title, std::move(items),
                                                  ignoreInitialConfirmRelease),
      [this, book](const ActivityResult& result) {
        longPressFired = false;
        if (result.isCancelled) {
          return;
        }

        const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
        if (!actionResult) {
          LOG_ERR("RBA", "Book action result missing");
          return;
        }

        switch (static_cast<FileBrowserAction>(actionResult->action)) {
          case FileBrowserAction::Delete:
            promptDeleteBook(book);
            return;
          case FileBrowserAction::DeleteCache:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(
                    renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE_CACHE), book.title),
                [this, book](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled) {
                    if (!BookActions::clearBookCache(book.path)) {
                      LOG_ERR("RBA", "Failed to clear book cache for: %s", book.path.c_str());
                    } else {
                      pendingCacheDeletedFeedback = true;
                      cacheDeletedFeedbackShowTime = millis();
                    }
                  }
                  reloadAfterBookAction();
                });
            return;
          case FileBrowserAction::DeleteStats:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(
                    renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE_BOOK_STATS), book.title),
                [this, book](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled) {
                    if (!BookActions::deleteBookStats(book.path)) {
                      LOG_ERR("RBA", "Failed to delete book stats for: %s", book.path.c_str());
                    } else {
                      BookActions::drawToast(renderer, tr(STR_BOOK_STATS_DELETED));
                      delay(1000);
                    }
                  }
                  reloadAfterBookAction();
                });
            return;
          case FileBrowserAction::ResetReaderSettings:
            startActivityForResult(
                std::make_unique<ConfirmationActivity>(
                    renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_RESET_BOOK_READER_SETTINGS),
                    book.title),
                [this, book](const ActivityResult& confirmation) {
                  if (!confirmation.isCancelled) {
                    if (!BookActions::resetBookReaderSettings(book.path)) {
                      LOG_ERR("RBA", "Failed to reset reader settings for: %s", book.path.c_str());
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
            startActivityForResult(
                std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "RecentEpubRenderModeSelect",
                                                          StrId::STR_EPUB_RENDER_MODE,
                                                          BookActions::epubRenderModeOptions(), currentIndex),
                [this, book](const ActivityResult& selectionResult) {
                  if (!selectionResult.isCancelled) {
                    const auto* selection = std::get_if<OptionSelectionResult>(&selectionResult.data);
                    if (selection != nullptr &&
                        !EpubReaderActivity::saveBookRenderMode(
                            book.path, BookActions::epubRenderModeForDisplayIndex(selection->index))) {
                      LOG_ERR("RBA", "Failed to save render mode for: %s", book.path.c_str());
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
          case FileBrowserAction::SetSleepFolder:
          case FileBrowserAction::ClearSleepFolder:
          case FileBrowserAction::ViewBookmarks:
          case FileBrowserAction::ViewClippings:
          case FileBrowserAction::DeleteBookmarks:
          case FileBrowserAction::DeleteClippings:
            return;
        }
      });
}

void RecentBooksActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<RecentBooksActivity*>(user)->buildListScreen(screen);
}

void RecentBooksActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (recentBooks.empty()) {
    screen.centeredText(tr(STR_NO_RECENT_BOOKS), screen.theme().bodyText);
    return;
  }

  // Transient per-render: points into the recentBooks strings.
  std::vector<fui::ListItem> items;
  items.reserve(recentBooks.size());
  for (const auto& book : recentBooks) {
    fui::ListItem item;
    item.label = book.title.c_str();
    if (!book.author.empty()) item.subtitle = book.author.c_str();
    item.icon = listIconFor(UITheme::getFileIcon(book.path), 32);  // subtitle rows carry the larger icon
    item.actionValue = static_cast<int16_t>(items.size());
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectorIndex);
  props.action = ACTION_ROW;
  props.inputMask = static_cast<uint16_t>(fui::InputTouch | fui::InputLongPress);  // physical buttons stay in loop()
  props.iconSize = 28;
  props.labelText = screen.theme().bodyText;
  props.labelText.bold = true;
  const fui::Rect listBounds = screen.body();
  const auto rows = configureUiList(props, screen.theme(), listBounds, UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(recentBooks.size()));  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void RecentBooksActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_MENU_RECENT_BOOKS), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_MENU_RECENT_BOOKS));
  }

  uiReady = false;
  app.render();
  uiReady = true;

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_HOME)), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (pendingCacheDeletedFeedback) {
    GUI.drawPopup(renderer, tr(STR_BOOK_CACHE_DELETED));
  }

  renderer.displayBuffer();
}
