#include "TouchActionButtons.h"

#include <GfxRenderer.h>

#include <algorithm>

#include "fontIds.h"

namespace TouchActionButtons {

Layout vertical(const Rect container, const uint8_t count, const int buttonHeight, const int gap) {
  Layout result;
  result.container = container;
  result.count = std::min<uint8_t>(count, static_cast<uint8_t>(kMaxButtons));
  if (result.count == 0 || container.width <= 0 || container.height <= 0) return result;

  const int safeHeight = std::max(1, buttonHeight);
  const int safeGap = std::max(0, gap);
  const int totalHeight = safeHeight * result.count + safeGap * (result.count - 1);
  const int firstY = container.y + std::max(0, container.height - totalHeight);
  for (uint8_t i = 0; i < result.count; ++i) {
    result.buttons[i] = Rect{container.x, firstY + i * (safeHeight + safeGap), container.width, safeHeight};
  }
  return result;
}

int indexAt(const Layout& layout, const int x, const int y, const int touchPadding) {
  const int padding = std::max(0, touchPadding);
  for (uint8_t i = 0; i < layout.count; ++i) {
    const Rect& button = layout.buttons[i];
    if (button.width <= 0 || button.height <= 0) continue;
    if (x >= button.x - padding && x < button.x + button.width + padding && y >= button.y - padding &&
        y < button.y + button.height + padding) {
      return i;
    }
  }
  return -1;
}

void draw(const GfxRenderer& renderer, const Layout& layout, const char* const* labels, const int primaryIndex,
          const int selectedIndex, int fontId) {
  if (labels == nullptr) return;
  if (fontId == 0) fontId = UI_10_FONT_ID;

  for (uint8_t i = 0; i < layout.count; ++i) {
    const Rect& button = layout.buttons[i];
    const bool selected = static_cast<int>(i) == selectedIndex;
    if (selected) renderer.fillRect(button.x, button.y, button.width, button.height, true);
    renderer.drawRect(button.x, button.y, button.width, button.height, true);
    const char* label = labels[i] != nullptr ? labels[i] : "";
    const EpdFontFamily::Style style =
        static_cast<int>(i) == primaryIndex ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int textWidth = renderer.getTextWidth(fontId, label, style);
    const int textY = button.y + (button.height - renderer.getLineHeight(fontId)) / 2;
    renderer.drawText(fontId, button.x + (button.width - textWidth) / 2, textY, label, !selected, style);
  }
}

}  // namespace TouchActionButtons
