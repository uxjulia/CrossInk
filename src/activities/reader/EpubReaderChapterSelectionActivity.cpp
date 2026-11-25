#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "FilenameFontSystem.h"
#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "util/InputReleaseGuard.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
}

EpubReaderChapterSelectionActivity::EpubReaderChapterSelectionActivity(GfxRenderer& renderer,
                                                                       MappedInputManager& mappedInput,
                                                                       const std::shared_ptr<Epub>& epub,
                                                                       const std::string& epubPath,
                                                                       const int currentTocIndex)
    : Activity("EpubReaderChapterSelection", renderer, mappedInput),
      epub(epub),
      epubPath(epubPath),
      currentTocIndex(currentTocIndex),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

int EpubReaderChapterSelectionActivity::getTotalItems() const { return epub->getTocItemsCount(); }

void EpubReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();
  {
    RenderLock lock(*this);
    filenameFontSystem.ensureLoaded(renderer);
  }
  ignoreInitialUpRelease = mappedInput.isPhysicalPressed(MappedInputManager::Button::Up);
  ignoreInitialDownRelease = mappedInput.isPhysicalPressed(MappedInputManager::Button::Down);
  mappedInput.setReaderTouchscreenOverride(true);

  // epub is a required collaborator: the caller dereferences it before constructing
  // this activity, and loop()/render() use it unguarded on every frame.
  selectorIndex = currentTocIndex;
  if (selectorIndex < 0) selectorIndex = 0;
  topIndex = 0;
  visibleRows = 1;
  initialViewportPending = true;
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &EpubReaderChapterSelectionActivity::onRowEvent, this);
  app.setScreen(&EpubReaderChapterSelectionActivity::chapterScreen, this);
  requestUpdate();
}

void EpubReaderChapterSelectionActivity::onExit() {
  mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();
}

void EpubReaderChapterSelectionActivity::selectChapter() {
  const auto tocItem = epub->getTocItem(selectorIndex);
  if (tocItem.spineIndex < 0) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
  } else {
    setResult(ChapterResult{tocItem.spineIndex, tocItem.anchor});
  }
  finish();
}

void EpubReaderChapterSelectionActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EpubReaderChapterSelectionActivity*>(user);
  if (event.value < 0 || event.value >= self->getTotalItems()) return;
  self->selectorIndex = event.value;
  self->app.clearTapFlash();
  self->selectChapter();
}

void EpubReaderChapterSelectionActivity::loop() {
  // A held side shortcut opens this list before its release. Keep that hold
  // from scrolling the new list or selecting a different chapter.
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Up, ignoreInitialUpRelease) ||
      InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Down,
                                               ignoreInitialDownRelease)) {
    return;
  }

  const int totalItems = getTotalItems();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput, safe);
  if (TouchHeaderBackButton::wasTapped(mappedInput, header) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
    const int next = scrollListBy(topIndex, delta, visibleRows, totalItems);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  const auto moveSelection = [this, totalItems](const int index) {
    selectorIndex = index;
    topIndex = followListSelection(selectorIndex, topIndex, visibleRows, totalItems);
    requestUpdate();
  };
  buttonNavigator.onNextRelease(
      [this, totalItems, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectorIndex, totalItems)); });
  buttonNavigator.onPreviousRelease(
      [this, totalItems, &moveSelection] { moveSelection(ButtonNavigator::previousIndex(selectorIndex, totalItems)); });
  buttonNavigator.onNextContinuous([this, totalItems, &moveSelection] {
    moveSelection(ButtonNavigator::nextPageIndex(selectorIndex, totalItems, visibleRows));
  });
  buttonNavigator.onPreviousContinuous([this, totalItems, &moveSelection] {
    moveSelection(ButtonNavigator::previousPageIndex(selectorIndex, totalItems, visibleRows));
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) selectChapter();
}

void EpubReaderChapterSelectionActivity::chapterScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<EpubReaderChapterSelectionActivity*>(user)->buildChapterScreen(screen);
}

void EpubReaderChapterSelectionActivity::buildChapterScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  setUiContentMargin(screen, renderer,
                     fui::Insets{static_cast<int16_t>(TouchHeaderBackButton::contentTop(renderer, mappedInput, safe.y)),
                                 static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
                                 static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height),
                                 static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int totalItems = getTotalItems();
  FilenameUiFontScope fonts(uiTarget, renderer);
  fui::ListProps props;
  props.labelText = screen.theme().bodyText;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = initialViewportPending ? followListSelection(selectorIndex, 0, visibleRows, totalItems)
                                    : scrollListBy(topIndex, 0, visibleRows, totalItems);
  initialViewportPending = false;

  const size_t drawCount =
      std::min({static_cast<size_t>(visibleRows), CHAPTER_WINDOW_SIZE, static_cast<size_t>(totalItems - topIndex)});
  for (size_t i = 0; i < drawCount; ++i) {
    const auto item = epub->getTocItem(topIndex + static_cast<int>(i));
    const size_t indent = item.level > 0 ? static_cast<size_t>(item.level - 1) * 2 : 0;
    labelWindow[i].assign(indent, ' ');
    labelWindow[i] += item.title;
    itemWindow[i] = fui::ListItem{};
    itemWindow[i].label = labelWindow[i].c_str();
    itemWindow[i].actionValue = static_cast<int16_t>(topIndex + static_cast<int>(i));
  }

  props.items = itemWindow.data();
  props.count = static_cast<uint16_t>(drawCount);
  props.selectedIndex = static_cast<int16_t>(selectorIndex - topIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.topIndex = 0;
  screen.list(props);
  fui::drawListScrollIndicator(screen.target(), screen.body(), static_cast<size_t>(totalItems), visibleRows, topIndex,
                               screen.theme().listScrollWidth, screen.theme().listScrollSide,
                               screen.theme().listScrollInset);
}

void EpubReaderChapterSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput, safe);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_SELECT_CHAPTER), true);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_SELECT_CHAPTER), nullptr, true);
  }
  uiReady = false;
  renderUiApp(app, uiTarget);
  uiReady = true;
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}
