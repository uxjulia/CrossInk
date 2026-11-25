#include "KeyboardLayoutsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId ACTION_ROW = 1;
}

KeyboardLayoutsActivity::KeyboardLayoutsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("KeyboardLayouts", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void KeyboardLayoutsActivity::onEnter() {
  Activity::onEnter();
  workingMask = keyboard_layouts::enabled();
  selectedIndex = 0;
  topIndex = 0;
  visibleRows = 1;
  edited = false;
  uiReady = false;
  for (uint8_t i = 0; i < keyboard_layouts::COUNT; ++i) {
    rowItems[i].label = I18N.getLanguageName(keyboard_layouts::ALL[i].language);
    rowItems[i].actionValue = static_cast<int16_t>(i);
  }
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &KeyboardLayoutsActivity::onRowEvent, this);
  app.setScreen(&KeyboardLayoutsActivity::listScreen, this);
  requestUpdate();
}

void KeyboardLayoutsActivity::onExit() {
  if (edited && workingMask != SETTINGS.keyboardLayouts) {
    SETTINGS.keyboardLayouts = workingMask;
    SETTINGS.saveToFile();
  }
  Activity::onExit();
}

bool KeyboardLayoutsActivity::isLocked(const uint8_t index) const {
  const uint16_t bit = keyboard_layouts::bitAt(index);
  if (!(workingMask & bit)) return false;
  return ((workingMask & ~bit) & keyboard_layouts::LATIN_BITS) == 0;
}

void KeyboardLayoutsActivity::toggleSelected() {
  const auto index = static_cast<uint8_t>(selectedIndex);
  if (!isLocked(index)) {
    workingMask = static_cast<uint16_t>(workingMask ^ keyboard_layouts::bitAt(index));
    edited = true;
  }
  app.clearTapFlash();
  requestUpdate();
}

void KeyboardLayoutsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<KeyboardLayoutsActivity*>(user);
  if (event.value < 0 || event.value >= keyboard_layouts::COUNT) return;
  self->selectedIndex = event.value;
  self->toggleSelected();
}

void KeyboardLayoutsActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (uiReady) {
    const auto snapshot = touchSnapshotFrom(mappedInput);
    if (snapshot.touchPressed || snapshot.touchReleased) {
      const auto event = app.route(snapshot);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggleSelected();
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int next = scrollListBy(topIndex, swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows,
                                  visibleRows, keyboard_layouts::COUNT);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  const auto move = [this](const int index) {
    selectedIndex = index;
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, keyboard_layouts::COUNT);
    requestUpdate();
  };
  buttonNavigator.onNextRelease(
      [this, &move] { move(ButtonNavigator::nextIndex(selectedIndex, keyboard_layouts::COUNT)); });
  buttonNavigator.onPreviousRelease(
      [this, &move] { move(ButtonNavigator::previousIndex(selectedIndex, keyboard_layouts::COUNT)); });
  buttonNavigator.onNextContinuous(
      [this, &move] { move(ButtonNavigator::nextPageIndex(selectedIndex, keyboard_layouts::COUNT, visibleRows)); });
  buttonNavigator.onPreviousContinuous(
      [this, &move] { move(ButtonNavigator::previousPageIndex(selectedIndex, keyboard_layouts::COUNT, visibleRows)); });
}

void KeyboardLayoutsActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<KeyboardLayoutsActivity*>(user)->buildListScreen(screen);
}

void KeyboardLayoutsActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  setUiContentMargin(screen, renderer,
                     fui::Insets{static_cast<int16_t>(TouchHeaderBackButton::contentTop(renderer, mappedInput)), 0,
                                 static_cast<int16_t>(UITheme::getButtonHintsReserve(renderer)), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  for (uint8_t i = 0; i < keyboard_layouts::COUNT; ++i) {
    rowItems[i].toggle = !isLocked(i);
    rowItems[i].toggleChecked = (workingMask & keyboard_layouts::bitAt(i)) != 0;
    rowItems[i].value = isLocked(i) ? tr(STR_DEFAULT_VALUE) : nullptr;
  }

  fui::ListProps props;
  props.items = rowItems;
  props.count = keyboard_layouts::COUNT;
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, keyboard_layouts::COUNT);
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void KeyboardLayoutsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_KEYBOARD_LAYOUTS), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_KEYBOARD_LAYOUTS));
  }
  uiReady = false;
  renderUiApp(app, uiTarget);
  uiReady = true;
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
