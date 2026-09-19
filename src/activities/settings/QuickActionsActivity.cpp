#include "QuickActionsActivity.h"

#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>

#include "AppCapabilities.h"
#include "CrossPointSettings.h"
#include "DeviceCapabilities.h"
#include "QuickActions.h"

namespace {
constexpr StrId triggerLabels[] = {
    StrId::STR_NONE_OPT,          StrId::STR_SHORT_PRESS_POWER,        StrId::STR_LONG_PRESS_POWER,
    StrId::STR_LONG_PRESS_BACK,   StrId::STR_LONG_PRESS_MENU_SHORTCUT, StrId::STR_POWER_BUTTON_CHORD,
    StrId::STR_TAP_HOME_SHORTCUT, StrId::STR_LONG_PRESS_HOME_SHORTCUT, StrId::STR_DOUBLE_TAP_HOME_SHORTCUT,
    StrId::STR_SIDE_BUTTON_CHORD};

std::vector<QuickActions::Trigger> availableTriggers() {
  std::vector<QuickActions::Trigger> triggers = {QuickActions::Trigger::None, QuickActions::Trigger::ShortPower,
                                                 QuickActions::Trigger::LongPower, QuickActions::Trigger::PowerUp};
#if CROSSINK_APP_CAP_TOUCH || CROSSINK_APP_DEVICE_X4CLASSIC || defined(SIMULATOR_DEVICE_X4_CLASSIC)
  if (deviceSupportsSideButtonChord(gpio)) {
    triggers.push_back(QuickActions::Trigger::UpDown);
  }
#endif
  if (gpio.hasHomeKey()) {
    triggers.push_back(QuickActions::Trigger::TapHome);
    triggers.push_back(QuickActions::Trigger::LongPressHome);
    triggers.push_back(QuickActions::Trigger::DoubleTapHome);
  }
#if CROSSINK_APP_CAP_TOUCH
  if (!gpio.hasTouch()) {
    triggers.push_back(QuickActions::Trigger::LongBack);
    triggers.push_back(QuickActions::Trigger::LongMenu);
  }
#else
  triggers.push_back(QuickActions::Trigger::LongBack);
  triggers.push_back(QuickActions::Trigger::LongMenu);
#endif
  return triggers;
}

std::vector<uint8_t> availableActions() {
  std::vector<uint8_t> actions;
  actions.reserve(QuickActions::shortcutActionOrder.size());
  for (const auto action : QuickActions::shortcutActionOrder) {
    const auto rawAction = static_cast<uint8_t>(action);
    if (QuickActions::isQuickActionSlotActionAvailable(rawAction)) actions.push_back(rawAction);
  }
  return actions;
}
}  // namespace

#ifdef SIMULATOR
namespace QuickActionsActivityTest {
bool isTriggerAvailable(const QuickActions::Trigger trigger) {
  const auto triggers = availableTriggers();
  return std::find(triggers.begin(), triggers.end(), trigger) != triggers.end();
}
}  // namespace QuickActionsActivityTest
#endif

void QuickActionsActivity::onEnter() {
  Activity::onEnter();
  popup.setDismissOnOutsideTouchDown(true);
  QuickActions::synchronize(SETTINGS);
  draftTrigger = static_cast<QuickActions::Trigger>(SETTINGS.quickActionsTrigger);
  std::copy_n(SETTINGS.quickActionSlots, draftSlots.size(), draftSlots.begin());
  showOverview();
}

void QuickActionsActivity::showOverview() {
  std::vector<std::string> rows;
  rows.reserve(6);
  const auto triggers = availableTriggers();
  if (std::find(triggers.begin(), triggers.end(), draftTrigger) == triggers.end()) {
    draftTrigger = QuickActions::Trigger::None;
  }
  rows.emplace_back(std::string(I18N.get(StrId::STR_SHORTCUT)) + ": " +
                    I18N.get(triggerLabels[static_cast<uint8_t>(draftTrigger)]));
  for (uint8_t i = 0; i < 5; ++i) {
    const uint8_t action = draftSlots[i];
    const char* label =
        QuickActions::isQuickActionSlotActionAvailable(action) ? I18N.get(QuickActions::actionLabel(action)) : "-";
    rows.emplace_back(std::to_string(i + 1) + ". " + label);
  }
  popup.showConfirmed(
      StrId::STR_QUICK_ACTIONS, rows, 0,
      [this](int selected) {
        if (selected == 0)
          editShortcut();
        else if (selected > 0 && selected <= 5)
          editSlot(static_cast<uint8_t>(selected - 1));
      },
      [this] { saveDraft(); }, [this] { finish(); });
  requestUpdate();
}

void QuickActionsActivity::editShortcut() {
  const auto triggers = availableTriggers();
  std::vector<std::string> labels;
  labels.reserve(triggers.size());
  uint8_t current = 0;
  for (uint8_t i = 0; i < triggers.size(); ++i) {
    labels.emplace_back(I18N.get(triggerLabels[static_cast<uint8_t>(triggers[i])]));
    if (triggers[i] == draftTrigger) current = i;
  }
  popup.show(StrId::STR_SHORTCUT, labels, current, [this, triggers](int selected) {
    if (selected < 0 || static_cast<size_t>(selected) >= triggers.size()) return;
    draftTrigger = triggers[selected];
    showOverview();
  });
  popup.setCancelCallback([this] { showOverview(); });
}

void QuickActionsActivity::editSlot(uint8_t slot) {
  const auto actions = availableActions();
  std::vector<std::string> labels;
  labels.reserve(actions.size());
  std::transform(actions.begin(), actions.end(), std::back_inserter(labels),
                 [](const uint8_t action) { return I18N.get(QuickActions::actionLabel(action)); });
  const auto currentIt = std::find(actions.begin(), actions.end(), draftSlots[slot]);
  const uint8_t current = currentIt == actions.end() ? 0 : static_cast<uint8_t>(currentIt - actions.begin());
  popup.show(StrId::STR_QUICK_ACTIONS, labels, current, [this, slot, actions](int selected) {
    if (selected < 0 || static_cast<size_t>(selected) >= actions.size()) return;
    draftSlots[slot] = actions[selected];
    showOverview();
  });
  popup.setCancelCallback([this] { showOverview(); });
}

void QuickActionsActivity::saveDraft() {
  QuickActions::applyTrigger(SETTINGS, draftTrigger);
  std::copy(draftSlots.begin(), draftSlots.end(), SETTINGS.quickActionSlots);
  SETTINGS.saveToFile();
  finish();
}

void QuickActionsActivity::loop() {
  if (popup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

void QuickActionsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  popup.processRender(renderer, mappedInput);
}
