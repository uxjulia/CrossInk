#include "ButtonRemapActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace {
// UI steps correspond to logical roles in order: Back, Confirm, Left, Right.
constexpr uint8_t kRoleCount = 4;
// Marker used when a role has not been assigned yet.
constexpr uint8_t kUnassigned = 0xFF;
// Duration to show temporary error text when reassigning a button.
constexpr unsigned long kErrorDisplayMs = 1500;
constexpr int16_t kValueBadgePadding = 8;
}  // namespace

namespace fui = freeink::ui;

ButtonRemapActivity::ButtonRemapActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         const bool isReaderMode, const bool headerReaderContext)
    : Activity("ButtonRemap", renderer, mappedInput),
      readerMode(isReaderMode),
      headerReaderContext(headerReaderContext),
      ui(renderer) {}

void ButtonRemapActivity::onEnter() {
  Activity::onEnter();

  // Start with all roles unassigned to avoid duplicate blocking.
  currentStep = 0;
  tempMapping[0] = kUnassigned;
  tempMapping[1] = kUnassigned;
  tempMapping[2] = kUnassigned;
  tempMapping[3] = kUnassigned;
  errorMessage.clear();
  errorUntil = 0;
  refreshListItems();
  ui.reset();
  ui.app.setScreen(&ButtonRemapActivity::listScreen, this);
  requestUpdate();
}

void ButtonRemapActivity::onExit() { Activity::onExit(); }

void ButtonRemapActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finish();
    return;
  }
  // Clear any temporary warning after its timeout.
  if (errorUntil > 0 && millis() > errorUntil) {
    errorMessage.clear();
    errorUntil = 0;
    requestUpdate();
    return;
  }

  // Side buttons:
  // - Up: reset mapping to defaults and exit.
  // - Down: cancel without saving.
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    // Persist default mapping immediately so the user can recover quickly.
    if (readerMode) {
      SETTINGS.readerFrontButtonsEnabled = 0;  // Revert to system mapping
      SETTINGS.readerFrontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      SETTINGS.readerFrontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      SETTINGS.readerFrontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
      SETTINGS.readerFrontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
    } else {
      SETTINGS.frontButtonBack = CrossPointSettings::FRONT_HW_BACK;
      SETTINGS.frontButtonConfirm = CrossPointSettings::FRONT_HW_CONFIRM;
      SETTINGS.frontButtonLeft = CrossPointSettings::FRONT_HW_LEFT;
      SETTINGS.frontButtonRight = CrossPointSettings::FRONT_HW_RIGHT;
    }
    SETTINGS.saveToFile();
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    // Exit without changing settings.
    finish();
    return;
  }

  {
    // Make sure UI done rendering before accepting another assignment.
    // This avoids rapid double-presses that can advance the step without a visible redraw.
    RenderLock lock(*this);

    // Wait for a front button press to assign to the current role.
    const int pressedButton = mappedInput.getPressedFrontButton();
    if (pressedButton < 0) {
      return;
    }

    // Update temporary mapping and advance the remap step.
    // Only accept the press if this hardware button isn't already assigned elsewhere.
    if (!validateUnassigned(static_cast<uint8_t>(pressedButton))) {
      requestUpdate();
      return;
    }
    tempMapping[currentStep] = static_cast<uint8_t>(pressedButton);
    currentStep++;

    if (currentStep >= kRoleCount) {
      // All roles assigned; save to settings and exit.
      applyTempMapping();
      SETTINGS.saveToFile();
      finish();
      return;
    }

    requestUpdate();
  }
}

void ButtonRemapActivity::render(RenderLock&&) {
  const auto labelForHardware = [&](uint8_t hardwareIndex) -> const char* {
    for (uint8_t i = 0; i < kRoleCount; i++) {
      if (tempMapping[i] == hardwareIndex) {
        return getRoleName(i);
      }
    }
    return "-";
  };

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const char* header = readerMode ? tr(STR_REMAP_FRONT_BUTTONS_READER) : tr(STR_REMAP_FRONT_BUTTONS);
  const Rect headerRect = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, headerRect, header, headerReaderContext);
  } else {
    GUI.drawHeader(renderer, headerRect, header, nullptr, headerReaderContext);
  }
  GUI.drawSubHeader(renderer,
                    Rect{0, TouchHeaderBackButton::contentTop(renderer, mappedInput), pageWidth, metrics.tabBarHeight},
                    tr(STR_REMAP_PROMPT));

  const int topOffset =
      TouchHeaderBackButton::contentTop(renderer, mappedInput) + metrics.tabBarHeight + metrics.verticalSpacing;
  ui.render();

  // Temporary warning banner for duplicates.
  if (!errorMessage.empty()) {
    GUI.drawHelpText(
        renderer,
        Rect{0, pageHeight - UITheme::getButtonHintsReserve(renderer) - metrics.contentSidePadding - 15, pageWidth, 20},
        errorMessage.c_str());
  }

  // Provide side button actions at the bottom of the screen (split across two lines).
  GUI.drawHelpText(renderer,
                   Rect{0, topOffset + 4 * metrics.listRowHeight + 4 * metrics.verticalSpacing, pageWidth, 20},
                   tr(STR_REMAP_RESET_HINT));
  GUI.drawHelpText(renderer,
                   Rect{0, topOffset + 4 * metrics.listRowHeight + 5 * metrics.verticalSpacing + 20, pageWidth, 20},
                   tr(STR_REMAP_CANCEL_HINT));

  // Live preview of logical labels under front buttons.
  // This mirrors the on-device front button order: Back, Confirm, Left, Right.
  GUI.drawButtonHints(renderer, labelForHardware(CrossPointSettings::FRONT_HW_BACK),
                      labelForHardware(CrossPointSettings::FRONT_HW_CONFIRM),
                      labelForHardware(CrossPointSettings::FRONT_HW_LEFT),
                      labelForHardware(CrossPointSettings::FRONT_HW_RIGHT));
  renderer.displayBuffer();
}

void ButtonRemapActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<ButtonRemapActivity*>(user)->buildListScreen(screen);
}

void ButtonRemapActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int topOffset =
      TouchHeaderBackButton::contentTop(renderer, mappedInput) + metrics.tabBarHeight + metrics.verticalSpacing;
  setUiContentMargin(
      screen, renderer,
      fui::Insets{static_cast<int16_t>(topOffset), 0,
                  static_cast<int16_t>(UITheme::getButtonHintsReserve(renderer) + metrics.verticalSpacing), 0});

  refreshListItems();

  fui::ListProps props;
  props.items = listItems.data();
  props.count = listItems.size();
  props.selectedIndex = currentStep;
  props.inputMask = fui::InputNone;
  props.labelText = screen.theme().bodyText;
  props.valueText = screen.theme().smallText;
  const fui::Rect listRect = screen.body();
  configureUiList(props, screen.theme(), listRect);
  screen.list(props);
  drawSelectedValueBadge(screen, listRect, props);
}

void ButtonRemapActivity::refreshListItems() {
  for (uint8_t index = 0; index < kRoleCount; ++index) {
    const uint8_t assignedButton = tempMapping[index];
    listItems[index] = fui::ListItem{};
    listItems[index].label = getRoleName(index);
    listItems[index].value = assignedButton == kUnassigned ? tr(STR_UNASSIGNED) : getHardwareName(assignedButton);
  }
}

bool ButtonRemapActivity::usesLyraValueBadge() const {
  switch (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme)) {
    case CrossPointSettings::UI_THEME::LYRA:
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
    case CrossPointSettings::UI_THEME::LYRA_CAROUSEL:
    case CrossPointSettings::UI_THEME::MINIMAL:
      return true;
    default:
      return false;
  }
}

void ButtonRemapActivity::drawSelectedValueBadge(UiApp::ScreenType& screen, const fui::Rect listRect,
                                                 const fui::ListProps& props) const {
  if (!usesLyraValueBadge() || currentStep >= listItems.size()) return;
  const char* value = listItems[currentStep].value;
  if (value == nullptr || value[0] == '\0') return;

  // The list owns row sizing and hit regions. This only restores Lyra's
  // selected-value paint using that same row geometry; it adds no interaction.
  const int16_t rowHeight = props.rowHeight;
  const int16_t rowGap = props.rowGap < 0 ? 0 : props.rowGap;
  const int16_t rowInset = props.rowInset < 0 ? screen.theme().listInset : props.rowInset;
  const int16_t sidePadding = props.sidePadding < 0 ? screen.theme().listSidePadding : props.sidePadding;
  fui::Rect row = listRect;
  row.x = static_cast<int16_t>(row.x + rowInset);
  row.width = static_cast<int16_t>(row.width - rowInset * 2);
  row.y = static_cast<int16_t>(row.y + currentStep * (rowHeight + rowGap));
  row.height = rowHeight;
  if (row.width <= 0 || row.height <= 0 || row.bottom() > listRect.bottom()) return;

  fui::TextStyle valueStyle = fui::textStyleWithForeground(props.valueText, fui::Paint::solid(fui::Color::White));
  valueStyle.align = fui::TextAlign::Right;
  const int16_t valueWidth = screen.target().measureText(valueStyle.font, value, valueStyle).width;
  const int16_t badgeWidth = static_cast<int16_t>(valueWidth + kValueBadgePadding * 2);
  if (badgeWidth > row.width - sidePadding * 2) return;

  const fui::Rect badge{static_cast<int16_t>(row.right() - badgeWidth), row.y, badgeWidth, row.height};
  const uint8_t radius = props.rowRadius > 0 ? props.rowRadius : screen.theme().listRowRadius;
  screen.target().fill(badge, fui::Paint::solid(fui::Color::Black), radius);
  screen.target().text(fui::Rect{static_cast<int16_t>(badge.x + kValueBadgePadding), badge.y, valueWidth, badge.height},
                       value, valueStyle);
}

void ButtonRemapActivity::applyTempMapping() {
  // Commit temporary mapping into settings (logical role -> hardware).
  if (readerMode) {
    SETTINGS.readerFrontButtonsEnabled = 1;  // Activate reader-specific mapping
    SETTINGS.readerFrontButtonBack = tempMapping[0];
    SETTINGS.readerFrontButtonConfirm = tempMapping[1];
    SETTINGS.readerFrontButtonLeft = tempMapping[2];
    SETTINGS.readerFrontButtonRight = tempMapping[3];
  } else {
    SETTINGS.frontButtonBack = tempMapping[0];
    SETTINGS.frontButtonConfirm = tempMapping[1];
    SETTINGS.frontButtonLeft = tempMapping[2];
    SETTINGS.frontButtonRight = tempMapping[3];
  }
}

bool ButtonRemapActivity::validateUnassigned(const uint8_t pressedButton) {
  // Block reusing a hardware button already assigned to another role.
  for (uint8_t i = 0; i < kRoleCount; i++) {
    if (tempMapping[i] == pressedButton && i != currentStep) {
      errorMessage = tr(STR_ALREADY_ASSIGNED);
      errorUntil = millis() + kErrorDisplayMs;
      return false;
    }
  }
  return true;
}

const char* ButtonRemapActivity::getRoleName(const uint8_t roleIndex) const {
  switch (roleIndex) {
    case 0:
      std::snprintf(roleNameStorage.data(), roleNameStorage.size(), "%s",
                    mappedInput.resolveLabel(mappedInput.withBackArrow(tr(STR_BACK))));
      return roleNameStorage.data();
    case 1:
      return tr(STR_CONFIRM);
    case 2:
      return tr(STR_DIR_LEFT);
    case 3:
    default:
      return tr(STR_DIR_RIGHT);
  }
}

const char* ButtonRemapActivity::getHardwareName(const uint8_t buttonIndex) const {
  switch (buttonIndex) {
    case CrossPointSettings::FRONT_HW_BACK:
      return tr(STR_HW_BACK_LABEL);
    case CrossPointSettings::FRONT_HW_CONFIRM:
      return tr(STR_HW_CONFIRM_LABEL);
    case CrossPointSettings::FRONT_HW_LEFT:
      return tr(STR_HW_LEFT_LABEL);
    case CrossPointSettings::FRONT_HW_RIGHT:
      return tr(STR_HW_RIGHT_LABEL);
    default:
      return "Unknown";
  }
}
