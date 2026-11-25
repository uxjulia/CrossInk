#include "EpubReaderFootnotesActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

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

EpubReaderFootnotesActivity::EpubReaderFootnotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                         const std::vector<FootnoteEntry>& footnotes)
    : Activity("EpubReaderFootnotes", renderer, mappedInput),
      footnotes(footnotes),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void EpubReaderFootnotesActivity::onEnter() {
  Activity::onEnter();
  ignoreInitialBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
  ignoreInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  ignoreInitialPowerRelease = mappedInput.isPressed(MappedInputManager::Button::Power);
  selectedIndex = 0;
  topIndex = 0;
  visibleRows = 1;
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &EpubReaderFootnotesActivity::onRowEvent, this);
  app.setScreen(&EpubReaderFootnotesActivity::listScreen, this);
  requestUpdate();
}

void EpubReaderFootnotesActivity::onExit() { Activity::onExit(); }

void EpubReaderFootnotesActivity::selectFootnote() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(footnotes.size())) return;
  setResult(FootnoteResult{footnotes[selectedIndex].href});
  finish();
}

void EpubReaderFootnotesActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EpubReaderFootnotesActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->footnotes.size())) return;
  self->selectedIndex = event.value;
  self->app.clearTapFlash();
  self->selectFootnote();
}

void EpubReaderFootnotesActivity::loop() {
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Back,
                                               ignoreInitialBackRelease) ||
      InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Power,
                                               ignoreInitialPowerRelease) ||
      InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Confirm,
                                               ignoreInitialConfirmRelease)) {
    return;
  }

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
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    selectFootnote();
    return;
  }
  const int count = static_cast<int>(footnotes.size());
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int next = scrollListBy(topIndex, swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows,
                                  visibleRows, count);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }
  const auto move = [this, count](const int next) {
    selectedIndex = next;
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, count);
    requestUpdate();
  };
  buttonNavigator.onNext([this, count, &move] { move(ButtonNavigator::nextIndex(selectedIndex, count)); });
  buttonNavigator.onPrevious([this, count, &move] { move(ButtonNavigator::previousIndex(selectedIndex, count)); });
}

void EpubReaderFootnotesActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<EpubReaderFootnotesActivity*>(user)->buildListScreen(screen);
}

void EpubReaderFootnotesActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  setUiContentMargin(screen, renderer,
                     fui::Insets{static_cast<int16_t>(TouchHeaderBackButton::contentTop(renderer, mappedInput, safe.y)),
                                 static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
                                 static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height),
                                 static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (footnotes.empty()) {
    screen.centeredText(tr(STR_NO_FOOTNOTES), screen.theme().bodyText);
    return;
  }
  std::vector<fui::ListItem> items;
  items.reserve(footnotes.size());
  for (size_t i = 0; i < footnotes.size(); ++i) {
    fui::ListItem item;
    item.label = footnotes[i].number[0] == '\0' ? tr(STR_LINK) : footnotes[i].number;
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }
  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(footnotes.size()));
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void EpubReaderFootnotesActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput, safe);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_FOOTNOTES), true);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_FOOTNOTES), nullptr, true);
  }
  uiReady = false;
  renderUiApp(app, uiTarget);
  uiReady = true;
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), footnotes.empty() ? "" : tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}
