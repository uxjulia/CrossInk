#include "ScreenCalibrationActivity.h"

#include <HalScreenCalibration.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "ScreenCalibrationLayout.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;
namespace {
constexpr int SAFE_INSET = ScreenInsets::MAX_INSET + 16;
constexpr int EDGE_COUNT = 4;
constexpr int CONTROL_COUNT = ScreenCalibrationLayout::CONTROL_COUNT;
constexpr int ROW_COUNT = CONTROL_COUNT + 3;
constexpr int ROW_SAVE = CONTROL_COUNT + 1;
constexpr int ROW_GAP = ScreenCalibrationLayout::ROW_GAP;
constexpr int BUTTON_GAP = 12;
constexpr int SECTION_GAP = ScreenCalibrationLayout::SECTION_GAP;
constexpr int CONTROL_PADDING = 12;
constexpr int CORNER_RADIUS = 2;
constexpr int VALUE_WIDTH = 44;
constexpr fui::ActionId ACTION_CONTROL = 1;
constexpr StrId ROW_LABELS[] = {StrId::STR_TOP,
                                StrId::STR_RIGHT_EDGE,
                                StrId::STR_BOTTOM,
                                StrId::STR_LEFT_EDGE,
                                StrId::STR_CALIBRATION_TOP_CORNERS,
                                StrId::STR_CALIBRATION_BOTTOM_CORNERS,
                                StrId::STR_CALIBRATION_RESET,
                                StrId::STR_SAVE,
                                StrId::STR_CANCEL};

// Selected controls get a light fill and a heavier outline; an edge being
// edited is outlined heavier still so Up/Down visibly change its value.
void drawControlFrame(const GfxRenderer& renderer, const fui::Rect& bounds, const bool selected, const bool editing) {
  if (selected) {
    renderer.fillRoundedRect(bounds.x, bounds.y, bounds.width, bounds.height, CORNER_RADIUS, Color::LightGray);
  }
  renderer.drawRoundedRect(bounds.x, bounds.y, bounds.width, bounds.height, editing ? 3 : (selected ? 2 : 1),
                           CORNER_RADIUS, true);
}

// Drawn with lines rather than "-"/"+" glyphs so both marks share a center.
void drawStepButton(const GfxRenderer& renderer, const int x, const int y, const int size, const bool plus,
                    const bool active) {
  if (active) {
    renderer.fillRoundedRect(x, y, size, size, CORNER_RADIUS, Color::Black);
  } else {
    renderer.fillRoundedRect(x, y, size, size, CORNER_RADIUS, Color::White);
    renderer.drawRoundedRect(x, y, size, size, 1, CORNER_RADIUS, true);
  }
  const int mark = std::max(12, size / 3);
  const int cx = x + size / 2;
  const int cy = y + size / 2;
  renderer.fillRect(cx - mark / 2, cy - 1, mark, 2, !active);
  if (plus) renderer.fillRect(cx - 1, cy - mark / 2, 2, mark, !active);
}

int previewReserve(const GfxRenderer& renderer) {
  auto maximum = renderer.getViewableInsets();
  maximum.edges.fill(ScreenInsets::MAX_INSET);
  const int legacyTop = UITheme::getInstance().getMetrics().topPadding + UITheme::getTopStatusBarInset(renderer);
  const int top = HalScreenCalibration::panelLayout().topStatusBarY(maximum, 0, legacyTop) +
                  UITheme::getDisplayStatusBarTextHeight(renderer) + ReaderStatusBarConfig::TOP_TEXT_INSET;
  const int bottom = ScreenInsets::MAX_INSET + UITheme::getReaderStatusBarTextHeight(renderer);
  return std::max({SAFE_INSET, top + 12, bottom + 12});
}

struct KeyHint {
  const char* key;
  const char* action;
};

// One key per line in two aligned columns; keys sit against the shared gap,
// on the right for right-to-left languages so each line still reads key first.
void drawKeyLegend(const GfxRenderer& renderer, const fui::Rect& area, const KeyHint* hints, const int count) {
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  constexpr int COLUMN_GAP = 12;
  constexpr int LINE_GAP = 4;
  int keyWidth = 0;
  int actionWidth = 0;
  for (int i = 0; i < count; ++i) {
    keyWidth = std::max(keyWidth, renderer.getTextWidth(UI_10_FONT_ID, hints[i].key, EpdFontFamily::BOLD));
    actionWidth = std::max(actionWidth, renderer.getTextWidth(UI_10_FONT_ID, hints[i].action));
  }
  // Long translations wrap within the action column rather than overflow the screen.
  actionWidth = std::min(actionWidth, std::max(0, area.width - keyWidth - COLUMN_GAP));
  const bool rtl = I18N.isRightToLeft();
  const int left = area.x + (area.width - keyWidth - COLUMN_GAP - actionWidth) / 2;
  int y = area.y;
  for (int i = 0; i < count; ++i) {
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, hints[i].action, actionWidth, 2);
    if (y + static_cast<int>(lines.size()) * lineHeight > area.bottom()) break;
    const int keyTextWidth = renderer.getTextWidth(UI_10_FONT_ID, hints[i].key, EpdFontFamily::BOLD);
    const int keyX = rtl ? left + actionWidth + COLUMN_GAP : left + keyWidth - keyTextWidth;
    renderer.drawText(UI_10_FONT_ID, keyX, y, hints[i].key, true, EpdFontFamily::BOLD);
    for (const auto& line : lines) {
      const int actionX =
          rtl ? keyX - COLUMN_GAP - renderer.getTextWidth(UI_10_FONT_ID, line.c_str()) : left + keyWidth + COLUMN_GAP;
      renderer.drawText(UI_10_FONT_ID, actionX, y, line.c_str());
      y += lineHeight;
    }
    y += LINE_GAP;
  }
}
}  // namespace

ScreenCalibrationActivity::ScreenCalibrationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("ScreenCalibration", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void ScreenCalibrationActivity::onEnter() {
  Activity::onEnter();
  RenderLock lock(*this);
  draft = renderer.getViewableInsets();
  previousOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Portrait);
  uiTarget.setFont(fui::GfxRendererTarget::FONT_SMALL, UI_10_FONT_ID);
  auto device = uiTarget.deviceContext();
  // Controls stay reachable even if the candidate border is behind the bezel.
  device.safeArea = fui::Insets{SAFE_INSET, SAFE_INSET, SAFE_INSET, SAFE_INSET};
  app.setDevice(device);
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_CONTROL, &ScreenCalibrationActivity::onControl, this);
  app.setScreen(&ScreenCalibrationActivity::calibrationScreen, this);
  LOG_DBG("CAL", "Calibration activity allocation: %u bytes", static_cast<unsigned>(sizeof(*this)));
  requestUpdate();
}

void ScreenCalibrationActivity::onExit() {
  // ActivityManager holds RenderLock while exiting an activity.
  renderer.setOrientation(previousOrientation);
  Activity::onExit();
}

void ScreenCalibrationActivity::activate() {
  if (selected < CONTROL_COUNT) {
    editing = !editing;
  } else if (selected == CONTROL_COUNT) {
    draft = HalScreenCalibration::defaults();
    saveFailed = false;
  } else if (selected == ROW_SAVE) {
    if (draft == renderer.getViewableInsets() || HalScreenCalibration::save(draft)) {
      renderer.setViewableInsets(draft);
      finish();
      return;
    }
    saveFailed = true;
  } else {
    finish();  // Working copy is discarded.
    return;
  }
  requestUpdate();
}

void ScreenCalibrationActivity::navigate(const int delta) {
  if (editing && selected < CONTROL_COUNT) {
    if (selected < EDGE_COUNT)
      draft.adjust(static_cast<unsigned>(selected), delta);
    else
      draft.adjustCorner(static_cast<unsigned>(selected - EDGE_COUNT), delta);
    saveFailed = false;
  } else {
    selected = (selected + delta + ROW_COUNT) % ROW_COUNT;
  }
  requestUpdate();
}

void ScreenCalibrationActivity::onControl(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<ScreenCalibrationActivity*>(user);
  // Rows, then decrease controls, then increase controls.
  if (event.value >= ROW_COUNT) {
    self.selected = (event.value - ROW_COUNT) % CONTROL_COUNT;
    self.editing = true;
    self.navigate(event.value < ROW_COUNT + CONTROL_COUNT ? -1 : 1);
  } else {
    self.selected = event.value;
    self.editing = false;
    self.activate();
  }
}

void ScreenCalibrationActivity::loop() {
  RenderLock lock(*this);  // Draft, hit regions, and orientation share the render task.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (editing) {
      editing = false;
      requestUpdate();
    } else {
      finishAfterBackPress();
    }
    return;
  }
  if (uiReady) {
    const auto input = touchSnapshotFrom(mappedInput);
    if (input.touchPressed || input.touchReleased) {
      const auto event = app.route(input);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    // Settings activates rows on release; do not let Save/Cancel reopen us.
    if (selected >= ROW_SAVE) mappedInput.suppressNextConfirmRelease();
    activate();
    return;
  }
  navigator.onNextRelease([this] { navigate(1); });
  navigator.onPreviousRelease([this] { navigate(-1); });
  navigator.onNextContinuous([this] { navigate(1); });
  navigator.onPreviousContinuous([this] { navigate(-1); });
}

void ScreenCalibrationActivity::calibrationScreen(UiApp::ScreenType& screen, void* user) {
  auto& self = *static_cast<ScreenCalibrationActivity*>(user);
  const auto& renderer = self.renderer;
  const auto safe = screen.device().safeRect();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const bool touch = self.mappedInput.hasTouch();
  const auto layout = ScreenCalibrationLayout::fit(screen.device().height, lineHeight, previewReserve(renderer),
                                                   UITheme::getButtonHintsReserve(renderer), touch);
  const int panelY = layout.panelY;
  const int rowHeight = layout.rowHeight;
  const int stepSize = layout.stepSize;
  auto text = screen.theme().smallText;
  text.maxLines = 4;
  text.align = fui::TextAlign::Center;
  const auto instructionRect =
      fui::Rect{safe.x, static_cast<int16_t>(layout.helpY), safe.width, static_cast<int16_t>(layout.helpHeight)};
  fui::layoutText(screen.target(), instructionRect,
                  self.saveFailed
                      ? tr(STR_CALIBRATION_SAVE_FAILED)
                      : (self.selected >= EDGE_COUNT && self.selected < CONTROL_COUNT ? tr(STR_CALIBRATION_CORNER_HELP)
                                                                                      : tr(STR_CALIBRATION_HELP)),
                  text, [&](const char* line, const fui::Rect rect) {
                    const int width = renderer.getTextWidth(UI_10_FONT_ID, line);
                    renderer.drawText(UI_10_FONT_ID, rect.x + (rect.width - width) / 2, rect.y, line);
                  });

  // Edge rows: label on the left, a [-] value [+] stepper on the right.
  for (int row = 0; row < CONTROL_COUNT; ++row) {
    const auto bounds = fui::Rect{safe.x, static_cast<int16_t>(panelY + row * (rowHeight + ROW_GAP)), safe.width,
                                  static_cast<int16_t>(rowHeight)};
    const bool selected = self.selected == row;
    const bool editing = selected && self.editing;
    drawControlFrame(renderer, bounds, selected, editing);
    const int textY = bounds.y + (rowHeight - lineHeight) / 2;
    const int stepY = bounds.y + (rowHeight - stepSize) / 2;
    const int plusX = bounds.right() - CONTROL_PADDING / 2 - stepSize;
    const int valueX = plusX - VALUE_WIDTH;
    const int minusX = valueX - stepSize;
    const auto labelStyle = selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const auto label = renderer.truncatedText(UI_10_FONT_ID, I18N.get(ROW_LABELS[row]),
                                              minusX - bounds.x - 2 * CONTROL_PADDING, labelStyle);
    renderer.drawText(UI_10_FONT_ID, bounds.x + CONTROL_PADDING, textY, label.c_str(), true, labelStyle);

    drawStepButton(renderer, minusX, stepY, stepSize, false, editing);
    drawStepButton(renderer, plusX, stepY, stepSize, true, editing);
    char value[8];
    std::snprintf(value, sizeof(value), "%u",
                  (row < EDGE_COUNT ? self.draft.edges[row] : self.draft.cornerInsets[row - EDGE_COUNT]));
    const auto valueStyle = editing ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    renderer.drawText(UI_10_FONT_ID,
                      valueX + (VALUE_WIDTH - renderer.getTextWidth(UI_10_FONT_ID, value, valueStyle)) / 2, textY,
                      value, true, valueStyle);

    // Generous touch targets: each half of the stepper spans the full row height.
    const auto valueCenter = static_cast<int16_t>(valueX + VALUE_WIDTH / 2);
    const fui::Rect decrease{static_cast<int16_t>(minusX - CONTROL_PADDING / 2), bounds.y,
                             static_cast<int16_t>(valueCenter - minusX + CONTROL_PADDING / 2), bounds.height};
    const fui::Rect increase{valueCenter, bounds.y, static_cast<int16_t>(bounds.right() - valueCenter), bounds.height};
    const fui::Rect labelBounds{bounds.x, bounds.y, static_cast<int16_t>(decrease.x - bounds.x), bounds.height};
    screen.frame().hit(labelBounds, ACTION_CONTROL, static_cast<int16_t>(row), fui::InputTouch);
    screen.frame().hit(decrease, ACTION_CONTROL, static_cast<int16_t>(ROW_COUNT + row), fui::InputTouch);
    screen.frame().hit(increase, ACTION_CONTROL, static_cast<int16_t>(ROW_COUNT + CONTROL_COUNT + row),
                       fui::InputTouch);
  }

  // Actions: Reset spans the width; Save and Cancel share the row below it.
  const int resetY = panelY + CONTROL_COUNT * rowHeight + (CONTROL_COUNT - 1) * ROW_GAP + SECTION_GAP;
  const int buttonRowY = resetY + rowHeight + ROW_GAP;
  const int halfWidth = (safe.width - BUTTON_GAP) / 2;
  const fui::Rect buttons[] = {
      fui::Rect{safe.x, static_cast<int16_t>(resetY), safe.width, static_cast<int16_t>(rowHeight)},
      fui::Rect{safe.x, static_cast<int16_t>(buttonRowY), static_cast<int16_t>(halfWidth),
                static_cast<int16_t>(rowHeight)},
      fui::Rect{static_cast<int16_t>(safe.right() - halfWidth), static_cast<int16_t>(buttonRowY),
                static_cast<int16_t>(halfWidth), static_cast<int16_t>(rowHeight)},
  };
  for (int i = 0; i < 3; ++i) {
    const int row = CONTROL_COUNT + i;
    const auto& bounds = buttons[i];
    const bool selected = self.selected == row;
    drawControlFrame(renderer, bounds, selected, false);
    const char* label = I18N.get(ROW_LABELS[row]);
    const auto style = selected || row == ROW_SAVE ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const auto fitted = renderer.truncatedText(UI_10_FONT_ID, label, bounds.width - CONTROL_PADDING, style);
    renderer.drawText(UI_10_FONT_ID,
                      bounds.x + (bounds.width - renderer.getTextWidth(UI_10_FONT_ID, fitted.c_str(), style)) / 2,
                      bounds.y + (rowHeight - lineHeight) / 2, fitted.c_str(), true, style);
    screen.frame().hit(bounds, ACTION_CONTROL, static_cast<int16_t>(row), fui::InputTouch);
  }

  const int noteY = buttonRowY + rowHeight + SECTION_GAP;
  const int noteBottom =
      std::min<int>(safe.bottom(), renderer.getScreenHeight() - UITheme::getButtonHintsReserve(renderer));
  const auto noteRect =
      fui::Rect{safe.x, static_cast<int16_t>(noteY), safe.width, static_cast<int16_t>(std::max(0, noteBottom - noteY))};
  if (!touch) {
    char upDown[48];
    std::snprintf(upDown, sizeof(upDown), "%s/%s", tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    const KeyHint selectHints[] = {
        {upDown, tr(STR_SELECT)}, {tr(STR_CONFIRM), tr(STR_CALIBRATION_EDIT_OR_APPLY)}, {tr(STR_BACK), tr(STR_CANCEL)}};
    const KeyHint editHints[] = {{upDown, tr(STR_CALIBRATION_ADJUST)}, {tr(STR_CONFIRM), tr(STR_DONE)}};
    if (self.editing)
      drawKeyLegend(renderer, noteRect, editHints, 2);
    else
      drawKeyLegend(renderer, noteRect, selectHints, 3);
  }
}

void ScreenCalibrationActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const bool cornerPreview = selected >= EDGE_COUNT && selected < CONTROL_COUNT;
  if (cornerPreview) {
    // The RenderLock keeps this draft-only draw invisible to other render users.
    const auto saved = renderer.getViewableInsets();
    renderer.setViewableInsets(draft);
    ReaderStatusBarConfig config;
    config.slots[ReaderStatusBarConfig::LEFT_FIRST] = ReaderStatusBarItem::Clock;
    config.slots[ReaderStatusBarConfig::RIGHT_FIRST] = ReaderStatusBarItem::Battery;
    ReaderStatusBarContent content;
    content.previewClock = "12:34";
    content.batteryPercentage = 75;
    content.outsideReader = selected == EDGE_COUNT;
    GUI.drawReaderStatusBar(renderer,
                            selected == EDGE_COUNT ? ReaderStatusBarPosition::Top : ReaderStatusBarPosition::Bottom,
                            content, &config);
    renderer.setViewableInsets(saved);
  } else {
    renderer.drawRect(draft.edges[3], draft.edges[0], renderer.getScreenWidth() - draft.edges[3] - draft.edges[1],
                      renderer.getScreenHeight() - draft.edges[0] - draft.edges[2]);
    if (selected < EDGE_COUNT) {
      const int left = draft.edges[3];
      const int top = draft.edges[0];
      const int right = renderer.getScreenWidth() - draft.edges[1] - 1;
      const int bottom = renderer.getScreenHeight() - draft.edges[2] - 1;
      if (selected == 0 || selected == 2) {
        const int y = selected == 0 ? top : bottom - 2;
        renderer.fillRect(left, y, right - left + 1, 3);
      } else {
        const int x = selected == 3 ? left : right - 2;
        renderer.fillRect(x, top, 3, bottom - top + 1);
      }
    }
  }
  renderer.drawCenteredText(UI_10_FONT_ID, previewReserve(renderer), tr(STR_SCREEN_CALIBRATION), true,
                            EpdFontFamily::BOLD);
  uiReady = false;
  app.render();
  uiReady = true;
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(editing ? tr(STR_BACK) : tr(STR_CANCEL)),
                            editing ? tr(STR_CONFIRM) : (selected < CONTROL_COUNT ? tr(STR_EDIT) : tr(STR_SELECT)),
                            editing ? "-" : tr(STR_DIR_UP), editing ? "+" : tr(STR_DIR_DOWN));
  if (!cornerPreview) GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
