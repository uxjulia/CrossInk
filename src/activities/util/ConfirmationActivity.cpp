#include "ConfirmationActivity.h"

#include <I18n.h>

#include <algorithm>

#include "../../components/UITheme.h"
#include "HalDisplay.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body)
    : Activity("Confirmation", renderer, mappedInput), heading(heading), body(body) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  const auto& metrics = UITheme::getInstance().getMetrics();
  lineHeight = renderer.getLineHeight(fontId);
  const int maxWidth = renderer.getScreenWidth() - (margin * 2);
  const int contentTop = margin;
  const int contentBottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - margin;
  const int contentHeight = contentBottom - contentTop;
  const int maxTotalLines = std::max(1, contentHeight / lineHeight);

  if (!heading.empty()) {
    const int headingLineCap = body.empty() ? maxTotalLines : std::min(2, std::max(1, maxTotalLines - 1));
    headingLines = renderer.wrappedText(fontId, heading.c_str(), maxWidth, headingLineCap, EpdFontFamily::BOLD);
  }
  int totalHeight = 0;
  if (!headingLines.empty()) totalHeight += static_cast<int>(headingLines.size()) * lineHeight;
  if (!body.empty()) {
    const int bodyGap = headingLines.empty() ? 0 : spacing;
    const int bodyHeight = std::max(lineHeight, contentHeight - totalHeight - bodyGap);
    const int bodyLineCap = std::max(1, bodyHeight / lineHeight);
    bodyLines = renderer.wrappedText(fontId, body.c_str(), maxWidth, bodyLineCap, EpdFontFamily::REGULAR);
    if (!bodyLines.empty()) {
      totalHeight += bodyGap + static_cast<int>(bodyLines.size()) * lineHeight;
    }
  }

  // Center the dialog text inside the space above the bottom button hints.
  startY = contentTop + std::max(0, (contentHeight - totalHeight) / 2);

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();

  int currentY = startY;
  for (const auto& line : headingLines) {
    renderer.drawCenteredText(fontId, currentY, line.c_str(), true, EpdFontFamily::BOLD);
    currentY += lineHeight;
  }

  if (!headingLines.empty() && !bodyLines.empty()) {
    currentY += spacing;
  }

  for (const auto& line : bodyLines) {
    renderer.drawCenteredText(fontId, currentY, line.c_str(), true, EpdFontFamily::REGULAR);
    currentY += lineHeight;
  }

  // Draw UI Elements
  const auto labels = mappedInput.mapLabels(I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    ActivityResult res;
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }
}
