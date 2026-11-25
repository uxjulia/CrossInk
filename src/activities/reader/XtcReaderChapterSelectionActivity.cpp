#include "XtcReaderChapterSelectionActivity.h"

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

XtcReaderChapterSelectionActivity::XtcReaderChapterSelectionActivity(GfxRenderer& renderer,
                                                                     MappedInputManager& mappedInput,
                                                                     const std::shared_ptr<Xtc>& xtc,
                                                                     const uint32_t currentPage)
    : Activity("XtcReaderChapterSelection", renderer, mappedInput),
      xtc(xtc),
      currentPage(currentPage),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

int XtcReaderChapterSelectionActivity::findChapterIndexForPage(const uint32_t page) const {
  if (!xtc) return 0;
  // Page-derived XTC tables are sorted by start page, so the current row can
  // be found with bounded on-demand reads instead of retaining every row.
  xtc::ChapterInfo chapter{};
  size_t chapterIndex = 0;
  if (xtc->getChapterForPage(page, chapter, &chapterIndex)) return static_cast<int>(chapterIndex);
  return 0;
}

void XtcReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();
  {
    RenderLock lock(*this);
    filenameFontSystem.ensureLoaded(renderer);
  }
  ignoreInitialUpRelease = mappedInput.isPhysicalPressed(MappedInputManager::Button::Up);
  ignoreInitialDownRelease = mappedInput.isPhysicalPressed(MappedInputManager::Button::Down);
  mappedInput.setReaderTouchscreenOverride(true);
  if (!xtc) return;

  selectorIndex = findChapterIndexForPage(currentPage);
  topIndex = 0;
  visibleRows = 1;
  initialViewportPending = true;
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &XtcReaderChapterSelectionActivity::onRowEvent, this);
  app.setScreen(&XtcReaderChapterSelectionActivity::chapterScreen, this);
  requestUpdate();
}

void XtcReaderChapterSelectionActivity::onExit() {
  mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();
}

void XtcReaderChapterSelectionActivity::selectChapter() {
  xtc::ChapterInfo chapter{};
  if (selectorIndex >= 0 && xtc->getChapter(static_cast<size_t>(selectorIndex), chapter)) {
    setResult(PageResult{chapter.startPage});
    finish();
  }
}

void XtcReaderChapterSelectionActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<XtcReaderChapterSelectionActivity*>(user);
  const int totalItems = static_cast<int>(self->xtc->getChapterCount());
  if (event.value < 0 || event.value >= totalItems) return;
  self->selectorIndex = event.value;
  self->app.clearTapFlash();
  self->selectChapter();
}

void XtcReaderChapterSelectionActivity::loop() {
  // A held side shortcut opens this list before its release. Keep that hold
  // from scrolling the new list or selecting a different chapter.
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Up, ignoreInitialUpRelease) ||
      InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Down,
                                               ignoreInitialDownRelease)) {
    return;
  }

  const int totalItems = static_cast<int>(xtc->getChapterCount());
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

void XtcReaderChapterSelectionActivity::chapterScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<XtcReaderChapterSelectionActivity*>(user)->buildChapterScreen(screen);
}

void XtcReaderChapterSelectionActivity::buildChapterScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  setUiContentMargin(screen, renderer,
                     fui::Insets{static_cast<int16_t>(TouchHeaderBackButton::contentTop(renderer, mappedInput, safe.y)),
                                 static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
                                 static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height),
                                 static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  const size_t chapterCount = xtc->getChapterCount();
  if (chapterCount == 0) {
    screen.centeredText(tr(STR_NO_CHAPTERS), screen.theme().bodyText);
    return;
  }
  FilenameUiFontScope fonts(uiTarget, renderer);
  fui::ListProps props;
  props.labelText = screen.theme().bodyText;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  const int totalItems = static_cast<int>(chapterCount);
  topIndex = initialViewportPending ? followListSelection(selectorIndex, 0, visibleRows, totalItems)
                                    : scrollListBy(topIndex, 0, visibleRows, totalItems);
  initialViewportPending = false;
  const size_t drawCount =
      std::min({static_cast<size_t>(visibleRows), CHAPTER_WINDOW_SIZE, chapterCount - static_cast<size_t>(topIndex)});
  const size_t loaded = xtc->getChapters(static_cast<size_t>(topIndex), chapterWindow.data(), drawCount);
  for (size_t i = 0; i < loaded; ++i) {
    itemWindow[i] = fui::ListItem{};
    itemWindow[i].label = chapterWindow[i].name[0] == '\0' ? tr(STR_UNNAMED) : chapterWindow[i].name;
    itemWindow[i].actionValue = static_cast<int16_t>(topIndex + static_cast<int>(i));
  }
  props.items = itemWindow.data();
  props.count = static_cast<uint16_t>(loaded);
  props.selectedIndex = selectorIndex - topIndex;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.topIndex = 0;
  screen.list(props);
  fui::drawListScrollIndicator(screen.target(), screen.body(), chapterCount, visibleRows, topIndex,
                               screen.theme().listScrollWidth, screen.theme().listScrollSide,
                               screen.theme().listScrollInset);
}

void XtcReaderChapterSelectionActivity::render(RenderLock&&) {
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
