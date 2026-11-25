#include "OptionSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <utility>

#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;

Rect optionListRect(const GfxRenderer& renderer, const MappedInputManager& mappedInput, const bool readerMode) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool isLandscape = readerMode && (orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                                          orientation == GfxRenderer::Orientation::LandscapeCounterClockwise);
  const int hintGutterWidth = isLandscape ? UITheme::getButtonHintsReserve(renderer) : 0;
  const int contentX = orientation == GfxRenderer::Orientation::LandscapeClockwise ? hintGutterWidth : 0;
  const int contentTop = TouchHeaderBackButton::contentTop(renderer, mappedInput) + metrics.verticalSpacing;
  return Rect{
      contentX, contentTop, renderer.getScreenWidth() - hintGutterWidth,
      renderer.getScreenHeight() - contentTop - UITheme::getButtonHintsReserve(renderer) - metrics.verticalSpacing};
}
}  // namespace

OptionSelectionActivity::OptionSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 std::string activityName, const StrId titleId,
                                                 std::vector<std::string> options, const uint8_t selectedIndex,
                                                 const bool readerMode, const bool showTouchHeaderBackButton)
    : Activity(std::move(activityName), renderer, mappedInput),
      titleId_(titleId),
      options_(std::move(options)),
      currentIndex_(selectedIndex),
      selectedIndex_(selectedIndex),
      readerMode_(readerMode),
      showTouchHeaderBackButton_(showTouchHeaderBackButton),
      uiTarget_(makeUiTarget(renderer)),
      app_(uiTarget_, uiTarget_.deviceContext()) {}

void OptionSelectionActivity::onEnter() {
  Activity::onEnter();
  if (options_.empty()) {
    cancel();
    return;
  }
  if (currentIndex_ < 0 || currentIndex_ >= static_cast<int>(options_.size())) currentIndex_ = 0;
  if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(options_.size())) selectedIndex_ = 0;
  topIndex_ = 0;
  visibleRows_ = 1;
  initialViewportPending_ = true;
  uiReady_ = false;
  applySharedUiTheme(app_, uiTarget_);
  app_.on(ACTION_ROW, &OptionSelectionActivity::onRowEvent, this);
  app_.setScreen(&OptionSelectionActivity::optionsScreen, this);
  requestUpdate();
}

void OptionSelectionActivity::loop() {
  if ((showTouchHeaderBackButton_ && TouchHeaderBackButton::wasTapped(mappedInput, renderer)) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    mappedInput.suppressNextBackRelease();
    cancel();
    return;
  }
  if (uiReady_) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app_.route(snap);
      if (app_.invalidated()) requestUpdate();
      if (event) return;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    select();
    return;
  }
  const int listSize = static_cast<int>(options_.size());
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows_ : -visibleRows_;
    const int next = scrollListBy(topIndex_, delta, visibleRows_, listSize);
    if (next != topIndex_) {
      topIndex_ = next;
      requestUpdate();
    }
    return;
  }
  const auto moveSelection = [this, listSize](const int index) {
    selectedIndex_ = index;
    topIndex_ = followListSelection(selectedIndex_, topIndex_, visibleRows_, listSize);
    requestUpdate();
  };
  buttonNavigator_.onNextRelease(
      [this, listSize, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectedIndex_, listSize)); });
  buttonNavigator_.onPreviousRelease(
      [this, listSize, &moveSelection] { moveSelection(ButtonNavigator::previousIndex(selectedIndex_, listSize)); });
  buttonNavigator_.onNextContinuous([this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::nextPageIndex(selectedIndex_, listSize, visibleRows_));
  });
  buttonNavigator_.onPreviousContinuous([this, listSize, &moveSelection] {
    moveSelection(ButtonNavigator::previousPageIndex(selectedIndex_, listSize, visibleRows_));
  });
}

void OptionSelectionActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void OptionSelectionActivity::select() {
  setResult(OptionSelectionResult{static_cast<uint8_t>(selectedIndex_)});
  finish();
}

void OptionSelectionActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<OptionSelectionActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->options_.size())) return;
  self->selectedIndex_ = event.value;
  self->app_.clearTapFlash();
  self->select();
}

void OptionSelectionActivity::optionsScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<OptionSelectionActivity*>(user)->buildOptionsScreen(screen);
}

void OptionSelectionActivity::buildOptionsScreen(UiApp::ScreenType& screen) {
  const Rect bounds = optionListRect(renderer, mappedInput, readerMode_);
  setUiContentMargin(screen, renderer,
                     fui::Insets{static_cast<int16_t>(bounds.y),
                                 static_cast<int16_t>(renderer.getScreenWidth() - bounds.x - bounds.width),
                                 static_cast<int16_t>(renderer.getScreenHeight() - bounds.y - bounds.height),
                                 static_cast<int16_t>(bounds.x)});
  std::vector<fui::ListItem> items;
  items.reserve(options_.size());
  for (size_t i = 0; i < options_.size(); ++i) {
    fui::ListItem item;
    item.label = options_[i].c_str();
    item.value = i == static_cast<size_t>(currentIndex_) ? tr(STR_SELECTED) : nullptr;
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }
  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex_);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows_ = rows > 0 ? rows : 1;
  const int optionCount = static_cast<int>(options_.size());
  topIndex_ = initialViewportPending_ ? followListSelection(selectedIndex_, 0, visibleRows_, optionCount)
                                      : scrollListBy(topIndex_, 0, visibleRows_, optionCount);
  initialViewportPending_ = false;
  props.topIndex = static_cast<uint16_t>(topIndex_);
  screen.list(props);
}

void OptionSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto orientation = renderer.getOrientation();
  const bool landscape = readerMode_ && (orientation == GfxRenderer::Orientation::LandscapeClockwise ||
                                         orientation == GfxRenderer::Orientation::LandscapeCounterClockwise);
  const int gutter = landscape ? UITheme::getButtonHintsReserve(renderer) : 0;
  const int contentX = orientation == GfxRenderer::Orientation::LandscapeClockwise ? gutter : 0;
  const Rect header = TouchHeaderBackButton::headerRect(
      renderer, mappedInput, Rect{contentX, 0, renderer.getScreenWidth() - gutter, renderer.getScreenHeight()});
  if (showTouchHeaderBackButton_ && mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget_, header, I18N.get(titleId_), readerMode_);
  } else {
    GUI.drawHeader(renderer, header, I18N.get(titleId_), nullptr, readerMode_);
  }
  uiReady_ = false;
  renderUiApp(app_, uiTarget_);
  uiReady_ = true;
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, readerMode_);
  renderer.displayBuffer();
}
