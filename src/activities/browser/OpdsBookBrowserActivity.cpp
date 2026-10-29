#include "OpdsBookBrowserActivity.h"

#include <Arduino.h>
#include <DownloadFileSwap.h>
#include <FreeInkUIIcon.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <OpdsFeedParser.h>
#include <OpdsSearchTemplate.h>
#include <WiFi.h>
#include <ZipFile.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/listIcons.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/OpdsDownloadFolder.h"
#include "util/OpdsFeedError.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr size_t OPDS_DOWNLOAD_BUFFER_SIZE = 2048;
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_SEARCH = 2;
constexpr fui::ActionId ACTION_CANCEL = 3;
constexpr fui::ActionId ACTION_DESCRIPTION = 4;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;

std::string buildBookFilenameBase(const OpdsEntry& book, const OpdsFilenameFormat format) {
  if (book.author.empty()) return book.title;
  if (book.title.empty()) return book.author;
  if (format == OpdsFilenameFormat::TITLE_AUTHOR) return book.title + " - " + book.author;
  return book.author + " - " + book.title;
}

// Mayberry prefixes folder titles with U+1F4C1 (file folder), which the UI
// fonts lack; show "/name" instead.
void replaceFolderEmoji(std::string& title) {
  constexpr char FOLDER_EMOJI[] = "\xF0\x9F\x93\x81";
  constexpr size_t FOLDER_EMOJI_LEN = sizeof(FOLDER_EMOJI) - 1;
  if (title.compare(0, FOLDER_EMOJI_LEN, FOLDER_EMOJI) != 0) return;
  size_t prefixLen = FOLDER_EMOJI_LEN;
  while (prefixLen < title.size() && title[prefixLen] == ' ') ++prefixLen;
  title.replace(0, prefixLen, "/");
}

}  // namespace

OpdsBookBrowserActivity::OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 OpdsServer server)
    : Activity("OpdsBookBrowser", renderer, mappedInput),
      buttonNavigator(),
      server(std::move(server)),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  sdFontSystem.releaseLoadedFont(renderer);

  state = BrowserState::CHECK_WIFI;
  entryCount = 0;
  catalogReleasedForDownload = false;
  navigationHistory.clear();
  searchTemplate = "";
  currentPath = "";
  selectorIndex = 0;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);

  uiReady = false;
  visibleRows = 1;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &OpdsBookBrowserActivity::onRowEvent, this);
  app.on(ACTION_SEARCH, &OpdsBookBrowserActivity::onSearchEvent, this);
  app.on(ACTION_CANCEL, &OpdsBookBrowserActivity::onCancelEvent, this);
  app.on(ACTION_DESCRIPTION, &OpdsBookBrowserActivity::onDescriptionEvent, this);
  app.setScreen(&OpdsBookBrowserActivity::rootScreen, this);
  requestUpdate();

  if (!ensureEntryBuffer()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }

#ifdef SIMULATOR
  // Use deterministic catalog data so the UI can be exercised without WiFi or an OPDS server.
  fetchFeed(currentPath);
#else
  checkAndConnectWifi();
#endif
}

void OpdsBookBrowserActivity::onExit() {
  library::invalidateLibraryIndex();
  Activity::onExit();
  clearEntries();
  entries.reset();
  navigationHistory.clear();

#ifndef SIMULATOR
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  // OPDS launches from minimal network boot, so restore the full app state
  // even if setup failed before WiFi was started.
  silentRestart();
#endif
}

void OpdsBookBrowserActivity::activateSelected() {
  if (!entries || entryCount == 0 || selectorIndex < 0 || selectorIndex >= static_cast<int>(entryCount)) return;
  auto& entry = entries[selectorIndex];
  if (entry.type == OpdsEntryType::BOOK) {
    if (entry.description[0] || !entry.selfHref.empty() || entry.indirect || entry.unsupportedAcquisition) {
      openPublication(entry);
    } else {
      requestDownload(entry);
    }
    return;
  }
  const bool pageLink =
      (hasPrevPageRow && selectorIndex == 0) || (hasNextPageRow && selectorIndex == static_cast<int>(entryCount) - 1);
  navigateToEntry(entry, pageLink);
}

void OpdsBookBrowserActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::BROWSING) return;
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->entryCount)) return;
  self->selectorIndex = event.value;
  // The tapped row leaves the screen either way (new feed or download view);
  // a lingering tap flash would gray an unrelated row on the next list.
  self->app.clearTapFlash();
  self->activateSelected();
}

void OpdsBookBrowserActivity::onSearchEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::BROWSING) return;
  self->app.clearTapFlash();
  self->launchSearch();
}

void OpdsBookBrowserActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::DOWNLOADING) return;
  self->app.clearTapFlash();
  self->cancelDownload = true;
}

void OpdsBookBrowserActivity::scrollDescription(const int direction) {
  RenderLock lock;
  const int next = scrollListBy(descriptionTop, direction * descriptionRows, descriptionRows,
                                static_cast<int>(descriptionLines.size()));
  if (next != descriptionTop) {
    descriptionTop = next;
    requestUpdate();
  }
}

void OpdsBookBrowserActivity::onDescriptionEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::DESCRIPTION) return;
  self->app.clearTapFlash();
  if (event.value == 0)
    self->requestDownload(self->entries[self->selectorIndex]);
  else
    self->scrollDescription(event.value);
}

void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }

  if (state == BrowserState::DESCRIPTION) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
      RenderLock lock;
      state = BrowserState::BROWSING;
      uiReady = false;
      descriptionLines.clear();
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      requestDownload(entries[selectorIndex]);
      return;
    }
    if (uiReady) {
      const auto snap = touchSnapshotFrom(mappedInput);
      if ((snap.touchPressed || snap.touchReleased) && app.route(snap)) return;
    }
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up)
      scrollDescription(1);
    else if (swipe == MappedInputManager::SwipeDir::Down)
      scrollDescription(-1);
    buttonNavigator.onNext([this] { scrollDescription(1); });
    buttonNavigator.onPrevious([this] { scrollDescription(-1); });
    return;
  }

  if (state == BrowserState::ERROR) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        if (catalogReleasedForDownload) {
          restoreCatalogAfterDownload();
        } else {
          showLoadingBeforeFetch();
          fetchFeed(currentPath);
        }
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state == BrowserState::CHECK_WIFI ? onGoHome() : navigateBack();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
#ifdef SIMULATOR
    if (uiReady) {
      const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
      if (snap.touchPressed || snap.touchReleased) app.route(snap);
    }
    if (cancelDownload || mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancelDownload = false;
      finishDownload({}, HttpDownloader::ABORTED, "");
    }
#endif
    return;
  }

  if (state == BrowserState::BROWSING) {
    if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
      navigateBack();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelected();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (!searchTemplate.empty() && selectorIndex == 0) launchSearch();
    }

    // Touch goes through the FreeInkApp: render() registered every tap target
    // (rows, header search button); route the snapshot and let the registered
    // handlers dispatch.
    if (uiReady) {
      const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
      if (snap.touchPressed || snap.touchReleased) {
        const auto event = app.route(snap);
        // No pressed-state repaint: the render it triggers would drop a slow
        // tap's release inside the uiReady window (tap-to-activate needed two
        // taps), and it costs a second e-ink refresh per tap.
        if (app.invalidated()) requestUpdate();
        if (event) return;  // dispatched to onRowEvent/onSearchEvent
        if (state != BrowserState::BROWSING) return;
      }
    }

    if (entryCount > 0) {
      // Swipes scroll the viewport; the selection stays put (it may scroll
      // off-screen) and button navigation pulls the view back to it.
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
        const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
        const int next = scrollListBy(topIndex, delta, visibleRows, static_cast<int>(entryCount));
        if (next != topIndex) {
          topIndex = next;
          requestUpdate();
        }
        return;
      }

      const auto moveSelection = [this](const int index) {
        selectorIndex = index;
        topIndex = followListSelection(selectorIndex, topIndex, visibleRows, static_cast<int>(entryCount));
        requestUpdate();
      };
      buttonNavigator.onNextRelease(
          [this, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectorIndex, entryCount)); });
      buttonNavigator.onPreviousRelease(
          [this, &moveSelection] { moveSelection(ButtonNavigator::previousIndex(selectorIndex, entryCount)); });
      buttonNavigator.onNextContinuous([this, &moveSelection] {
        moveSelection(ButtonNavigator::nextPageIndex(selectorIndex, entryCount, visibleRows));
      });
      buttonNavigator.onPreviousContinuous([this, &moveSelection] {
        moveSelection(ButtonNavigator::previousPageIndex(selectorIndex, entryCount, visibleRows));
      });
    }
  }
}

bool OpdsBookBrowserActivity::preventAutoSleep() {
  switch (state) {
    case BrowserState::CHECK_WIFI:
    case BrowserState::WIFI_SELECTION:
    case BrowserState::LOADING:
    case BrowserState::DOWNLOADING:
    case BrowserState::SEARCH_INPUT:
      return true;
    case BrowserState::BROWSING:
    case BrowserState::DESCRIPTION:
    case BrowserState::ERROR:
      return false;
  }
  return false;
}

void OpdsBookBrowserActivity::rootScreen(UiApp::ScreenType& screen, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  switch (self->state) {
    case BrowserState::BROWSING:
      self->buildBrowsingScreen(screen);
      break;
    case BrowserState::DESCRIPTION:
      self->buildDescriptionScreen(screen);
      break;
    case BrowserState::DOWNLOADING:
      self->buildDownloadScreen(screen);
      break;
    default:
      self->buildStatusScreen(screen);
      break;
  }
}

// Shared chrome for every state: reserve the firmware's button-hint band and
// draw the themed header (padding, centering, and rule come from the theme).
void OpdsBookBrowserActivity::screenHeader(UiApp::ScreenType& screen, const bool withSearch) {
  screen.takeBottom(static_cast<int16_t>(UITheme::getButtonHintsReserve(renderer)));
  const bool useTouchBackHeader =
      (state == BrowserState::BROWSING || state == BrowserState::DESCRIPTION) && mappedInput.hasTouchHardware();
  if (useTouchBackHeader) {
    const Rect headerRect = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    const auto backLayout = TouchHeaderBackButton::layout(headerRect);
    const bool showSearch = withSearch && !searchTemplate.empty();
    TouchHeaderBackButton::draw(renderer, uiTarget, headerRect,
                                server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str(), false,
                                showSearch ? static_cast<int>(backLayout.iconRect.width + 8) : 0);
    screen.takeTop(static_cast<int16_t>(headerRect.height));

    if (showSearch) {
      fui::ButtonProps search;
      search.action = ACTION_SEARCH;
      search.styles = fui::plainStyles(fui::Paint::solid(fui::Color::Black));
      search.minTouchSize = screen.theme().minTouchSize;
      search.radius = 8;
      const fui::Rect searchRect{static_cast<int16_t>(headerRect.x + headerRect.width - backLayout.iconRect.width),
                                 static_cast<int16_t>(backLayout.iconRect.y),
                                 static_cast<int16_t>(backLayout.iconRect.width),
                                 static_cast<int16_t>(backLayout.iconRect.height)};
      screen.button(search, searchRect);
      // Keep the touch target clear of the divider, but draw the glyph on the
      // shared Back/title baseline instead of at the top of its action lane.
      const int16_t iconX =
          static_cast<int16_t>(searchRect.x + (searchRect.width - TouchHeaderBackButton::ICON_SIZE) / 2);
      const int16_t iconY = static_cast<int16_t>(backLayout.iconRect.y + TouchHeaderBackButton::TITLE_VERTICAL_OFFSET +
                                                 (backLayout.iconRect.height - TouchHeaderBackButton::ICON_SIZE) / 2);
      screen.target().bitmap(
          fui::Rect{iconX, iconY, TouchHeaderBackButton::ICON_SIZE, TouchHeaderBackButton::ICON_SIZE},
          fui::bitmapFromIcon(icon_search_32), fui::BitmapMode::Center, fui::Paint::solid(fui::Color::Black));
    }
  } else {
    fui::HeaderProps header;
    header.title = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
    header.borderEdges = fui::EdgeBottom;
    if (withSearch && !searchTemplate.empty()) {
      header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
      header.trailingAction = ACTION_SEARCH;
      // Optically align the icon with the title glyphs: text hangs low in its
      // line cell by the font's internal leading; drop the button to match.
      const int titleFontId = uiScaleSpec().titleFontId;
      header.actionOffsetY =
          static_cast<int16_t>((renderer.getLineHeight(titleFontId) - renderer.getTextHeight(titleFontId)) / 2);
    }
    screen.header(header);
  }
  // Same breathing room between header and content as the legacy screens.
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().verticalSpacing));
}

void OpdsBookBrowserActivity::buildBrowsingScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, true);

  if (entryCount == 0) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }

  // Rows are formatted on demand for the visible viewport only, so a redraw
  // never allocates; after a download the heap can be too fragmented for it.
  fui::ListProps props;
  props.rowProvider = &OpdsBookBrowserActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(entryCount);
  props.selectedIndex = static_cast<int16_t>(selectorIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the nav chevron and the row edge
  const auto rows = configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(entryCount));  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void OpdsBookBrowserActivity::provideRow(void* user, const uint16_t index, fui::ListItem& item) {
  auto& self = *static_cast<OpdsBookBrowserActivity*>(user);
  const auto& entry = self.entries[index];
  item.label = entry.title.c_str();
  if (entry.type == OpdsEntryType::BOOK && !entry.author.empty()) item.subtitle = entry.author.c_str();
  if (entry.type == OpdsEntryType::NAVIGATION) {
    item.value = ">";
    if (entry.count >= 0) {
      auto& label = self.countLabels[index];
      snprintf(label.data(), label.size(), "(%ld) >", static_cast<long>(entry.count));
      item.value = label.data();
    }
  }
  item.actionValue = static_cast<int16_t>(index);
}

void OpdsBookBrowserActivity::buildDescriptionScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, false);
  const auto& book = entries[selectorIndex];
  const auto& theme = screen.theme();
  const int16_t lh = screen.target().lineHeight(theme.bodyText.font);
  const int16_t gap = theme.spaceMd;
  fui::TextStyle title = theme.bodyText;
  title.maxLines = 2;
  screen.target().text(screen.takeTop(lh * 2, gap), book.title.c_str(), title);
  if (!book.author.empty()) screen.target().text(screen.takeTop(lh, gap), book.author.c_str(), theme.smallText);
  const fui::Rect actions = screen.takeBottom(theme.rowHeight, gap);
  const fui::Rect body = screen.body().inset(fui::Insets{0, gap, 0, gap});
  if (descriptionWidth != body.width) {
    descriptionLines = renderer.wrappedText(uiScaleSpec().bodyFontId,
                                            (book.unsupportedAcquisition || book.href.empty()) && !book.description[0]
                                                ? tr(STR_NOT_SUPPORTED)
                                                : book.description.data(),
                                            body.width, 64);
    descriptionWidth = body.width;
  }
  descriptionRows = std::max(1, static_cast<int>(body.height / lh));
  descriptionTop = scrollListBy(descriptionTop, 0, descriptionRows, static_cast<int>(descriptionLines.size()));
  for (int row = 0; row < descriptionRows && descriptionTop + row < static_cast<int>(descriptionLines.size()); ++row) {
    screen.target().text(fui::Rect{body.x, static_cast<int16_t>(body.y + row * lh), body.width, lh},
                         descriptionLines[descriptionTop + row].c_str(), theme.bodyText);
  }
  const int16_t width = actions.width / 3;
  const char* labels[] = {tr(STR_PREV_PAGE), tr(STR_DOWNLOAD), tr(STR_NEXT_PAGE)};
  for (int i = 0; i < 3; ++i) {
    fui::ButtonProps button;
    button.label = labels[i];
    button.action = ACTION_DESCRIPTION;
    button.value = i - 1;
    button.enabled = i == 1 ? (!book.unsupportedAcquisition && !book.href.empty())
                            : (i == 0 ? descriptionTop > 0
                                      : descriptionTop + descriptionRows < static_cast<int>(descriptionLines.size()));
    screen.button(button, fui::Rect{static_cast<int16_t>(actions.x + i * width), actions.y, width, actions.height});
  }
}

void OpdsBookBrowserActivity::buildDownloadScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, false);

  // Centered block: status line, book title, progress bar, cancel button.
  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(centered.font);
  const int16_t gap = theme.spaceMd;
  const int16_t barH = 16;
  const int16_t btnH = theme.rowHeight;
  const int16_t blockH = static_cast<int16_t>(lh * 2 + barH + btnH + gap * 3);
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));

  screen.target().text(screen.takeTop(lh, gap), tr(STR_DOWNLOADING), centered);
  screen.target().text(screen.takeTop(lh, gap), statusMessage.c_str(), centered);

  const fui::Rect bar = screen.takeTop(barH, gap).inset(fui::Insets{0, 50, 0, 50});
  if (downloadTotal > 0) {
    fui::ProgressBarProps progress;
    progress.value = static_cast<int32_t>(downloadProgress);
    progress.max = static_cast<int32_t>(downloadTotal);
    progress.border = fui::Paint::solid(fui::Color::Black);
    progress.borderWidth = 1;
    fui::progressBar(screen.frame(), bar, progress);
  }

  const fui::Rect btnArea = screen.takeTop(btnH);
  const int16_t btnW = static_cast<int16_t>(btnArea.width / 3);
  fui::ButtonProps cancel;
  cancel.label = tr(STR_CANCEL);
  cancel.action = ACTION_CANCEL;
  screen.button(cancel, fui::Rect{static_cast<int16_t>(btnArea.x + (btnArea.width - btnW) / 2), btnArea.y, btnW, btnH});
}

void OpdsBookBrowserActivity::buildStatusScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, false);

  fui::TextStyle centered = screen.theme().bodyText;
  centered.align = fui::TextAlign::Center;
  if (state == BrowserState::ERROR) {
    const int16_t lh = screen.target().lineHeight(centered.font);
    const int16_t gap = screen.theme().spaceMd;
    const bool showTapHint = mappedInput.hasTouch();
    const int16_t blockH = static_cast<int16_t>(lh * (showTapHint ? 3 : 2) + gap * (showTapHint ? 2 : 1));
    const fui::Rect body = screen.body();
    if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));
    screen.target().text(screen.takeTop(lh, gap), tr(STR_ERROR_MSG), centered);
    screen.target().text(screen.takeTop(lh, gap), errorMessage.c_str(), centered);
    if (showTapHint) screen.target().text(screen.takeTop(lh), tr(STR_TAP_TO_RETRY), centered);
    return;
  }
  // CHECK_WIFI / LOADING (and the brief child-activity handoff states).
  screen.centeredText(statusMessage.c_str(), centered);
}

void OpdsBookBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  MappedInputManager::Labels labels;
  switch (state) {
    case BrowserState::BROWSING: {
      const char* confirmLabel = (entryCount > 0 && entries[selectorIndex].type == OpdsEntryType::BOOK &&
                                  !entries[selectorIndex].description[0] && entries[selectorIndex].selfHref.empty() &&
                                  !entries[selectorIndex].indirect && !entries[selectorIndex].unsupportedAcquisition)
                                     ? tr(STR_DOWNLOAD)
                                     : tr(STR_OPEN);
      const char* searchLabel = (!searchTemplate.empty() && selectorIndex == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
      labels =
          mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
      break;
    }
    case BrowserState::DESCRIPTION:
      labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_DOWNLOAD), tr(STR_PREV_PAGE),
                                     tr(STR_NEXT_PAGE));
      break;
    case BrowserState::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case BrowserState::ERROR:
      labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_RETRY), "", "");
      break;
    default:
      labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  uiReady = false;
  renderUiApp(app, uiTarget);
  uiReady = true;
  renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
}

void OpdsBookBrowserActivity::showLoadingBeforeFetch() {
  {
    RenderLock lock;
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    clearEntries();
    selectorIndex = 0;
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("OPDS", "Loading screen could not be rendered before feed fetch");
    requestUpdate(true);
  }
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  RenderLock lock;  // Loading is visible; publish the new feed and final state together.
  if (!ensureEntryBuffer()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }

#ifdef SIMULATOR
  clearEntries();
  searchTemplate = "simulator://search?query={searchTerms}";

  if (path.empty()) {
    appendEntry(OpdsEntry{OpdsEntryType::NAVIGATION, "Browse fiction", "", "/fiction", ""});
    appendEntry(OpdsEntry{OpdsEntryType::BOOK, "The Left Hand of Darkness", "Ursula K. Le Guin",
                          "/books/the-left-hand-of-darkness.epub", ""});
    appendEntry(
        OpdsEntry{OpdsEntryType::BOOK, "A Room of One's Own", "Virginia Woolf", "/books/a-room-of-ones-own.epub", ""});
    appendEntry(OpdsEntry{OpdsEntryType::BOOK, "Frankenstein", "Mary Shelley", "/books/frankenstein.epub", ""});
    snprintf(entries[1].description.data(), entries[1].description.size(), "%s",
             "A visitor to a distant world must learn to understand its people. "
             "This catalog description can be read before choosing Download. "
             "Use the page buttons or swipe to read the rest at larger text sizes.");
  } else {
    appendEntry(
        OpdsEntry{OpdsEntryType::BOOK, "The Dispossessed", "Ursula K. Le Guin", "/books/the-dispossessed.epub", ""});
    appendEntry(OpdsEntry{OpdsEntryType::BOOK, "Kindred", "Octavia E. Butler", "/books/kindred.epub", ""});
    appendEntry(OpdsEntry{OpdsEntryType::BOOK, "The Time Machine", "H. G. Wells", "/books/the-time-machine.epub", ""});
  }

  selectorIndex = 0;
  topIndex = 0;
  state = BrowserState::BROWSING;
  requestUpdate();
  return;
#endif

  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  clearEntries();
  const std::string url = UrlUtils::buildUrl(server.url, path);
  // Keep the normalized server URL alive for the synchronous fetch so
  // HttpDownloader can scope Basic auth even for legacy scheme-less entries.
  const std::string authorizationOrigin = UrlUtils::ensureProtocol(server.url);
  LOG_DBG("OPDS", "Fetching: %s", UrlUtils::forLog(url).c_str());
  OpdsFeedParser parser(entries.get(), MAX_OPDS_FEED_ENTRIES);
  parser.setPreferredLanguage(I18n::getInstance().getCode());
  HttpRequestDiagnostics diagnostics;
  auto result = HttpDownloader::OK;
  if (parser) {
    HttpDownloader::DownloadOptions downloadOptions;
    downloadOptions.transport = HttpDownloader::Transport::WOLFSSL;
    downloadOptions.authorizationOrigin = authorizationOrigin;
    result = HttpDownloader::streamUrl(
        url, [&parser](const uint8_t* data, const size_t len) { return parser.write(data, len) == len; }, nullptr,
        server.username, server.password, std::move(downloadOptions), &diagnostics);
  }
  parser.flush();  // Finalize before publishing or discarding the bounded entry array.

  // A parser rejection is the cause of a callback-aborted download, rather
  // than a server/connectivity error. Do not hide it behind the HTTP result.
  if (!parser && (result == HttpDownloader::OK || diagnostics.callbackRejected)) {
    clearEntries();
    state = BrowserState::ERROR;
    if (parser.getErrorReason() == OpdsParserError::BUFFER_MEMORY) {
      errorMessage = tr(STR_OPDS_FEED_BUFFER_MEMORY_ERROR);
    } else if (parser.getErrorReason() == OpdsParserError::PARSER_MEMORY) {
      errorMessage = tr(STR_MEMORY_ERROR);
    } else {
      errorMessage = tr(STR_PARSE_FEED_FAILED);
    }
    requestUpdate();
    return;
  }

  if (result != HttpDownloader::OK) {
    clearEntries();
    state = BrowserState::ERROR;
    switch (classifyFeedFailure(diagnostics,
                                result == HttpDownloader::HTTP_ERROR || result == HttpDownloader::UNAUTHORIZED)) {
      case OpdsFeedError::Unauthorized:
        errorMessage = tr(STR_FEED_UNAUTHORIZED);
        break;
      case OpdsFeedError::Redirected:
        errorMessage = tr(STR_FEED_REDIRECTED);
        break;
      case OpdsFeedError::ServerError:
        errorMessage = tr(STR_FEED_SERVER_ERROR);
        break;
      case OpdsFeedError::Unreachable:
        errorMessage = tr(STR_FEED_UNREACHABLE);
        break;
      default:
        errorMessage = tr(STR_FETCH_FEED_FAILED);
        break;
    }
    requestUpdate();
    return;
  }

  searchTemplate = parser.getSearchTemplate();
  const auto& nextUrl = parser.getNextPageUrl();
  const auto& prevUrl = parser.getPrevPageUrl();
  entryCount = parser.getEntryCount();
  for (size_t i = 0; i < entryCount; ++i) {
    if (entries[i].type == OpdsEntryType::NAVIGATION) replaceFolderEmoji(entries[i].title);
  }
  if (parser.wasTruncated()) {
    LOG_DBG("OPDS", "Feed truncated to %zu entries", entryCount);
  }

  if (!prevUrl.empty()) {
    hasPrevPageRow = true;
    for (size_t i = entryCount; i > 0; --i) {
      entries[i] = std::move(entries[i - 1]);
    }
    entries[0] = OpdsEntry{OpdsEntryType::NAVIGATION,
                           std::string(mappedInput.resolveLabel(mappedInput.withPreviousPageArrow(tr(STR_PREV_PAGE)))),
                           "", prevUrl, ""};
    entryCount++;
  }
  if (!nextUrl.empty()) {
    hasNextPageRow = appendEntry(OpdsEntry{
        OpdsEntryType::NAVIGATION,
        std::string(mappedInput.resolveLabel(mappedInput.withNextPageArrow(tr(STR_NEXT_PAGE)))), "", nextUrl, ""});
    if (!hasNextPageRow) LOG_DBG("OPDS", "No room for next-page entry");
  }

  selectorIndex = 0;
  topIndex = 0;
  state = entryCount == 0 ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entryCount == 0) errorMessage = tr(STR_NO_ENTRIES);
  requestUpdate();
}

bool OpdsBookBrowserActivity::ensureEntryBuffer() {
  if (entries) return true;
  entries = makeUniqueNoThrow<OpdsEntry[]>(OPDS_BROWSER_ENTRY_CAPACITY);
  if (!entries) LOG_ERR("OPDS", "Cannot allocate catalog (%zu bytes)", sizeof(OpdsEntry) * OPDS_BROWSER_ENTRY_CAPACITY);
  return entries != nullptr;
}

void OpdsBookBrowserActivity::clearEntries() {
  // The app's interaction table still references the old row indices until
  // the next render, so stop routing touches while clearing the backing data.
  uiReady = false;
  // A failed fetch can populate slots before entryCount is published. Clear
  // the full bounded array so partial-feed strings cannot survive a retry.
  for (size_t i = 0; entries && i < OPDS_BROWSER_ENTRY_CAPACITY; ++i) {
    entries[i] = OpdsEntry{};
  }
  entryCount = 0;
  hasPrevPageRow = false;
  hasNextPageRow = false;
}

bool OpdsBookBrowserActivity::appendEntry(OpdsEntry&& entry) {
  if (!entries || entryCount >= OPDS_BROWSER_ENTRY_CAPACITY) return false;
  entries[entryCount++] = std::move(entry);
  return true;
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry, const bool pageLink) {
  // Pagination stays in the same catalog; Back should return to its parent.
  if (!pageLink) navigationHistory.push_back(currentPath);
  // Resolve to a full URL so sub-sub-navigation retains parent path context
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::resolveReference(feedUrl, entry.href);

  showLoadingBeforeFetch();
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::navigateBack() {
  catalogReleasedForDownload = false;
  if (navigationHistory.empty()) {
    onGoHome();
  } else {
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();
    showLoadingBeforeFetch();
    fetchFeed(currentPath);
  }
}

bool OpdsBookBrowserActivity::loadPublication(OpdsEntry& book, const std::string& url) {
  auto detail = makeUniqueNoThrow<OpdsEntry>();
  auto parser = detail ? makeUniqueNoThrow<OpdsJsonParser>(detail.get(), 1, true) : nullptr;
  if (!parser) {
    LOG_ERR("OPDS", "Cannot allocate publication parser");
    return false;
  }
  parser->setPreferredLanguage(I18n::getInstance().getCode());
  HttpDownloader::DownloadOptions options;
  options.transport = HttpDownloader::Transport::WOLFSSL;
  const std::string authorizationOrigin = UrlUtils::ensureProtocol(server.url);
  options.authorizationOrigin = authorizationOrigin;
  size_t received = 0;
  const auto result = HttpDownloader::streamUrl(
      url,
      [&](const uint8_t* data, size_t length) {
        // A metadata document is small; do not accept an unbounded mistaken download.
        if (length > 256 * 1024 - received) return false;
        received += length;
        return parser->write(data, length) == length;
      },
      nullptr, server.username, server.password, std::move(options));
  parser->flush();
  if (result != HttpDownloader::OK || parser->error() || parser->getEntryCount() != 1) {
    LOG_ERR("OPDS", "Failed to load publication details");
    return false;
  }
  if (!detail->title.empty()) book.title = std::move(detail->title);
  if (!detail->author.empty()) book.author = std::move(detail->author);
  if (detail->description[0]) book.description = detail->description;
  if (!detail->href.empty()) {
    book.href = UrlUtils::resolveReference(url, detail->href);
    book.indirect = detail->indirect;
    book.unsupportedAcquisition = detail->unsupportedAcquisition;
  }
  book.selfHref.clear();
  return true;
}

void OpdsBookBrowserActivity::openPublication(OpdsEntry& book) {
  if (!book.selfHref.empty()) {
    const std::string url = UrlUtils::resolveReference(UrlUtils::buildUrl(server.url, currentPath), book.selfHref);
    {
      RenderLock lock;
      state = BrowserState::LOADING;
      statusMessage = tr(STR_LOADING);
      uiReady = false;
    }
    requestUpdateAndWait();
    RenderLock lock;
    loadPublication(book, url);
  }
  RenderLock lock;
  descriptionLines.clear();
  descriptionWidth = descriptionTop = 0;
  state = BrowserState::DESCRIPTION;
  uiReady = false;
  requestUpdate();
}

void OpdsBookBrowserActivity::requestDownload(const OpdsEntry& selectedBook) {
  if (selectedBook.unsupportedAcquisition || selectedBook.href.empty()) return;
  auto resolved = makeUniqueNoThrow<OpdsEntry>();
  if (!resolved) {
    LOG_ERR("OPDS", "Cannot allocate selected publication");
    return;
  }
  *resolved = selectedBook;
  if (resolved->indirect) {
    {
      RenderLock lock;
      state = BrowserState::LOADING;
      statusMessage = tr(STR_LOADING);
      uiReady = false;
    }
    requestUpdateAndWait();
    RenderLock lock;
    std::string baseUrl = UrlUtils::buildUrl(server.url, currentPath);
    for (int hop = 0; resolved->indirect && hop < 4; ++hop) {
      const std::string url = UrlUtils::resolveReference(baseUrl, resolved->href);
      if (!loadPublication(*resolved, url)) break;
      baseUrl = url;
    }
    if (resolved->indirect || resolved->unsupportedAcquisition || resolved->href.empty()) {
      state = BrowserState::ERROR;
      errorMessage = tr(STR_NOT_SUPPORTED);
      requestUpdate();
      return;
    }
  }
  const auto& book = *resolved;
  std::string path;
  if (!OpdsDownloadFolder::normalize(OpdsDownloadFolder::effective(server.downloadFolder, SETTINGS.opdsDownloadFolder),
                                     path)) {
    LOG_ERR("OPDS", "Invalid download folder");
    RenderLock lock;
    state = BrowserState::ERROR;
    errorMessage = tr(STR_OPDS_INVALID_FOLDER);
    requestUpdate();
    return;
  }
  if (path != "/") path += '/';
  path += StringUtils::sanitizeFilename(buildBookFilenameBase(book, server.filenameFormat));
  path += ".epub";
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  DownloadRequest request{UrlUtils::resolveReference(feedUrl, book.href), book.title, std::move(path)};
  if (server.filenameFormat == OpdsFilenameFormat::SERVER_FILENAME) {
    downloadBook(std::move(request));
    return;
  }
  // Recover an interrupted replacement before deciding whether the book exists.
  if (!DownloadFileSwap::recover(request.filename)) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }
  if (!Storage.exists(request.filename.c_str())) {
    downloadBook(std::move(request));
    return;
  }
  // Copy the destination before moving the request (argument evaluation order).
  const std::string destination = request.filename;
  confirmDownload(std::move(request), destination);
}

void OpdsBookBrowserActivity::confirmDownload(DownloadRequest request, const std::string& destination) {
  RenderLock lock;
  state = BrowserState::BROWSING;
  auto dialog =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, std::string(tr(STR_REPLACE)) + "?", destination);
  if (!dialog) {
    LOG_ERR("OPDS", "Cannot allocate overwrite dialog");
    state = BrowserState::ERROR;
    errorMessage = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }
  // A refreshed catalog may reorder or remove rows. Approval must still apply
  // to the original URL and filename, never the book now at the same index.
  startActivityForResult(std::move(dialog),
                         [this, request = std::move(request), destination](const ActivityResult& result) mutable {
                           if (result.isCancelled) {
                             requestUpdate();
                             return;
                           }
                           downloadBook(std::move(request), destination);
                         });
}

void OpdsBookBrowserActivity::downloadBook(DownloadRequest request, const std::string& approvedPath) {
  {
    RenderLock lock;
    state = BrowserState::DOWNLOADING;
    statusMessage = request.title;
    downloadProgress = downloadTotal = 0;
    cancelDownload = false;
    goHomeAfterCancel = false;
    downloadSelectorIndex = selectorIndex;
    downloadTopIndex = topIndex;
    catalogReleasedForDownload = true;
    clearEntries();
    // clearEntries() alone leaves the fixed entry/description array allocated.
    entries.reset();
    std::vector<std::string>().swap(descriptionLines);
    descriptionWidth = descriptionTop = 0;
  }
  requestUpdate(true);

#ifdef SIMULATOR
  downloadProgress = 1;
  downloadTotal = 2;
  requestUpdate(true);
  return;
#endif

  // This temporary is intentionally retained until downloadToFile returns;
  // DownloadOptions borrows it to avoid copying the server URL per transfer.
  const std::string authorizationOrigin = UrlUtils::ensureProtocol(server.url);
  // The request path was resolved before any overwrite prompt. Never re-read
  // the global setting here or a retry could create a different directory.
  const std::string downloadFolder = FsHelpers::extractFolderPath(request.filename);
  if (downloadFolder != "/" && !Storage.mkdir(downloadFolder.c_str()) && !Storage.exists(downloadFolder.c_str())) {
    LOG_ERR("OPDS", "Could not create download folder %s", downloadFolder.c_str());
    RenderLock lock;
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }

  LOG_DBG("OPDS", "Downloading: %s -> %s", UrlUtils::forLog(request.url).c_str(), request.filename.c_str());

  bool cancelRequested = false;
  auto pollCancel = [this, &cancelRequested] {
    if (cancelRequested || cancelDownload) {
      cancelRequested = true;
      return true;
    }
    mappedInput.update();
    if (mappedInput.wasHomeGesture()) {
      goHomeAfterCancel = true;
      cancelRequested = true;
    }
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancelRequested = true;
    }
    if (uiReady) {
      const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
      if (snap.touchPressed || snap.touchReleased) app.route(snap);
    }
    // app.route() can set cancelDownload through onCancelEvent() during this poll.
    // cppcheck-suppress knownConditionTrueFalse
    return cancelRequested || cancelDownload;
  };
  HttpDownloader::DownloadOptions downloadOptions;
  downloadOptions.shouldCancel = pollCancel;
  downloadOptions.bufferSize = OPDS_DOWNLOAD_BUFFER_SIZE;
  downloadOptions.reserveFullRecord = true;
  downloadOptions.transport = HttpDownloader::Transport::WOLFSSL;
  downloadOptions.authorizationOrigin = authorizationOrigin;
  downloadOptions.stageAsPart = true;
  std::string resolvedPath;
  downloadOptions.useServerFilename = server.filenameFormat == OpdsFilenameFormat::SERVER_FILENAME;
  downloadOptions.overwriteApprovedPath = approvedPath;
  downloadOptions.resolvedPath = &resolvedPath;
  downloadOptions.checkFreeSpace = true;
  downloadOptions.validate = [](const std::string& path) {
    ZipFile zip(path);
    size_t size = 0;
    return zip.getInflatedFileSize("META-INF/container.xml", &size) && size > 0;
  };
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;

  const auto result = HttpDownloader::downloadToFile(
      request.url, request.filename,
      [this, &lastRenderedPercent, &lastProgressUpdateMs](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
        const unsigned long now = millis();
        if (percent >= 100 || lastRenderedPercent < 0 ||
            percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
            now - lastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
          lastRenderedPercent = percent;
          lastProgressUpdateMs = now;
          requestUpdate(true);
        }
      },
      &cancelRequested, server.username, server.password, downloadOptions);

  // The downloader has closed TLS and file handles before the catalog reload.
  finishDownload(std::move(request), result, resolvedPath);
}

void OpdsBookBrowserActivity::finishDownload(DownloadRequest request, const HttpDownloader::DownloadError result,
                                             const std::string& resolvedPath) {
  if (result == HttpDownloader::OK) {
    if (!clearBookCachePreservingUserState(resolvedPath, true)) {
      LOG_ERR("OPDS", "File saved but cache refresh failed: %s", resolvedPath.c_str());
      RenderLock lock;
      state = BrowserState::ERROR;
      errorMessage = tr(STR_DOWNLOAD_FAILED);
      requestUpdate();
      return;
    }
    restoreCatalogAfterDownload();
  } else if (result == HttpDownloader::FILE_EXISTS) {
    restoreCatalogAfterDownload();
    if (state == BrowserState::BROWSING) confirmDownload(std::move(request), resolvedPath);
  } else if (result == HttpDownloader::ABORTED) {
    LOG_INF("OPDS", "Download cancelled");
    if (goHomeAfterCancel) {
      onGoHome();
      return;
    }
    mappedInput.suppressNextBackRelease();
    restoreCatalogAfterDownload();
  } else {
    RenderLock lock;
    state = BrowserState::ERROR;
    errorMessage = result == HttpDownloader::INSUFFICIENT_SPACE ? tr(STR_SD_CARD_FULL) : tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
  }
}

void OpdsBookBrowserActivity::restoreCatalogAfterDownload() {
  showLoadingBeforeFetch();
  fetchFeed(currentPath);
  RenderLock lock;
  if (state != BrowserState::BROWSING) return;  // Keep the fetch/allocation error and Retry path.
  selectorIndex = std::clamp(downloadSelectorIndex, 0, static_cast<int>(entryCount) - 1);
  topIndex = scrollListBy(downloadTopIndex, 0, visibleRows, static_cast<int>(entryCount));
  catalogReleasedForDownload = false;
  uiReady = false;
  requestUpdate();
}

void OpdsBookBrowserActivity::launchSearch() {
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (query.empty() || searchTemplate.empty()) {
    state = BrowserState::BROWSING;
    requestUpdate();
    return;
  }

  const std::string expanded = expandOpdsSearchTemplate(searchTemplate, opdsPercentEncode(query));
  const std::string feedUrl = UrlUtils::buildUrl(UrlUtils::ensureProtocol(server.url), currentPath);
  const std::string url = UrlUtils::resolveReference(feedUrl, expanded);

  navigationHistory.push_back(currentPath);
  currentPath = url;

  showLoadingBeforeFetch();
  fetchFeed(url);
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    showLoadingBeforeFetch();
    fetchFeed(currentPath);
    return;
  }
  launchWifiSelection();
}

void OpdsBookBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    if (catalogReleasedForDownload) {
      restoreCatalogAfterDownload();
    } else {
      showLoadingBeforeFetch();
      fetchFeed(currentPath);
    }
  } else {
    // Leave WiFi up; onExit's silent reboot handles teardown without fragmenting.
    state = BrowserState::ERROR;
    errorMessage = tr(STR_CONNECTION_FAILED);
    requestUpdate();
  }
}
