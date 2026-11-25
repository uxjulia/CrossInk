#include "FullScreenMessageActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "fontIds.h"

void FullScreenMessageActivity::onEnter() {
  Activity::onEnter();

  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  constexpr int padding = 12;
  const int width = renderer.getScreenWidth() - 2 * (std::max(left, right) + padding);
  const int height = renderer.getScreenHeight() - top - bottom;
  const int titleHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineHeight = std::max(1, renderer.getLineHeight(UI_10_FONT_ID));
  const int gap = lineHeight / 2;
  const auto lines = renderer.wrappedText(UI_10_FONT_ID, I18n::getInstance().get(message), width,
                                          (height - titleHeight - gap) / lineHeight);
  int y = top + (height - titleHeight - gap - static_cast<int>(lines.size()) * lineHeight) / 2;

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, y, I18n::getInstance().get(title), true, EpdFontFamily::BOLD);
  y += titleHeight + gap;
  for (const auto& line : lines) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
    y += lineHeight;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
