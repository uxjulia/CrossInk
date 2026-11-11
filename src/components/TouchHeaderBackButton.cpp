#include "TouchHeaderBackButton.h"

#include <FreeInkUIIcon.h>

#include <algorithm>

#include "UIScale.h"
#include "UIThemeTokens.h"
#include "components/CompactHeader.h"
#include "components/HeaderDate.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/touchHeaderIcons.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace TouchHeaderBackButton {

namespace {
constexpr int actionSize = 52;
constexpr int touchSize = 80;
constexpr int titleGap = 8;
constexpr int defaultRightReserve = 52;

int effectiveVerticalOffset(const Layout& layout, const Rect& header, const int requestedOffset) {
  const int iconBottom = layout.iconRect.y + (layout.iconRect.height + ICON_SIZE) / 2;
  const int availableOffset = std::max(0, header.y + header.height - iconBottom);
  return std::clamp(requestedOffset, 0, availableOffset);
}
}  // namespace

Layout layout(const Rect& header) {
  const int actionWidth = std::min(actionSize, header.width);
  const int actionHeight = std::min(actionSize, header.height);
  // Center the action lane vertically so taller theme headers split their
  // spare space above and below the back button/title instead of placing all
  // of it above the title.
  const int actionX = header.x;
  const int actionY = header.y + (header.height - actionHeight + UITheme::getDisplayStatusBarHeightIncrease()) / 2;
  const int touchWidth = std::min(touchSize, header.width);
  // Keep the whole navigation target inside its header, above the shared divider.
  const int touchHeight = std::max(0, header.height - 1);
  const int touchX = header.x;
  const int touchY = header.y;
  return {Rect{actionX, actionY, actionWidth, actionHeight}, Rect{touchX, touchY, touchWidth, touchHeight},
          actionX + actionWidth + titleGap};
}

Rect standardHeaderRect(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return UITheme::getHeaderRect(renderer, metrics.headerHeight);
}

int height(const ThemeMetrics& metrics, const MappedInputManager& input) {
  return input.hasTouchHardware() ? CompactHeader::height(metrics) : metrics.headerHeight;
}

Rect headerRect(const GfxRenderer& renderer, const MappedInputManager& input) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return UITheme::getHeaderRect(renderer, height(metrics, input));
}

Rect headerRect(const GfxRenderer& renderer, const MappedInputManager& input, const Rect& area) {
  return UITheme::getHeaderRect(renderer, height(UITheme::getInstance().getMetrics(), input), area);
}

Rect compactHeaderRect(const GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return UITheme::getHeaderRect(renderer, CompactHeader::height(metrics));
}

int contentTop(const GfxRenderer& renderer, const MappedInputManager& input, const int areaTop) {
  const auto header = headerRect(renderer, input);
  return std::max(areaTop, header.y) + header.height;
}

bool wasTapped(const MappedInputManager& input, const Rect& header) {
  if (!input.hasTouchHardware()) return false;
  const Rect touchRect = layout(header).touchRect;
  return input.wasTapInRect(touchRect.x, touchRect.y, touchRect.width, touchRect.height);
}

bool wasTapped(const MappedInputManager& input, const GfxRenderer& renderer) {
  return wasTapped(input, headerRect(renderer, input));
}

bool wasTapped(const MappedInputManager& input, const GfxRenderer& renderer, const char* title) {
  int x = 0;
  int y = 0;
  if (!input.hasTouchHardware() || !input.wasScreenTapped(x, y)) return false;
  const Rect header = headerRect(renderer, input);
  const Layout back = layout(header);
  constexpr int titleEndPadding = 12;
  const int titleWidth = renderer.getTextWidth(uiScaleSpec().titleFontId, title, EpdFontFamily::BOLD);
  const int touchWidth =
      std::min(header.width, std::max(back.touchRect.width, back.titleX - header.x + titleWidth + titleEndPadding));
  return x >= back.touchRect.x && x < back.touchRect.x + touchWidth && y >= back.touchRect.y &&
         y < back.touchRect.y + back.touchRect.height;
}

void draw(GfxRenderer& renderer, const Rect& header, const char* title, const bool readerContext,
          const int rightReserve, const char* subtitle, const int verticalOffset, const bool showStatus) {
  auto target = makeUiTarget(renderer);
  draw(renderer, target, header, title, readerContext, rightReserve, subtitle, verticalOffset, showStatus);
}

void draw(GfxRenderer& renderer, fui::GfxRendererTarget& target, const Rect& header, const char* title,
          const bool readerContext, const int rightReserve, const char* subtitle, const int verticalOffset,
          const bool showStatus) {
  Layout back = layout(header);
  const int offset = effectiveVerticalOffset(back, header, verticalOffset);
  back.iconRect.y += offset;
  // Keep the status row on the same baseline as Home. The optional offset only
  // adds breathing room below it for the navigation icon and title.
  GUI.drawHeader(renderer, header, "", subtitle, readerContext, showStatus);

  fui::TextStyle titleStyle = uiThemeTokens(target).titleText;
  titleStyle.align = fui::TextAlign::Left;
  titleStyle.maxLines = 1;
  const int reservedRight = rightReserve > 0 ? rightReserve : defaultRightReserve;
  const int titleWidth = std::max(0, header.x + header.width - reservedRight - back.titleX);
  target.text(fui::Rect{static_cast<int16_t>(back.titleX), static_cast<int16_t>(back.iconRect.y),
                        static_cast<int16_t>(titleWidth), static_cast<int16_t>(back.iconRect.height)},
              title, titleStyle);

  const int iconX = back.iconRect.x + (back.iconRect.width - ICON_SIZE) / 2;
  const int iconY = back.iconRect.y + (back.iconRect.height - ICON_SIZE) / 2;
  target.bitmap(fui::Rect{static_cast<int16_t>(iconX), static_cast<int16_t>(iconY), ICON_SIZE, ICON_SIZE},
                fui::bitmapFromIcon(icon_back_32), fui::BitmapMode::Center);
}

void drawCompact(GfxRenderer& renderer, const char* title, const bool readerContext, const bool showDate,
                 const int verticalOffset, const int minRightReserve) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header = compactHeaderRect(renderer);
  const int rightReserve = std::max(minRightReserve, metrics.batteryWidth + 2 * metrics.headerSidePadding +
                                                         (showDate ? headerDateReservedWidth(renderer) : 0));
  draw(renderer, header, title, readerContext, rightReserve, nullptr, verticalOffset);
  if (showDate) {
    const Layout back = layout(header);
    const int offset = effectiveVerticalOffset(back, header, verticalOffset);
    const int titleBaselineY = back.iconRect.y + offset +
                               std::max(0, (back.iconRect.height - renderer.getLineHeight(UI_12_FONT_ID)) / 2) +
                               renderer.getFontAscenderSize(UI_12_FONT_ID);
    drawHeaderDateAtBaseline(renderer, header.x + header.width, titleBaselineY);
  }
}

const int TRAILING_ACTION_RESERVE = actionSize + titleGap;

void drawTrailingIcon(GfxRenderer& renderer, const Rect& header, const freeink::Icon& icon, const int verticalOffset) {
  Layout back = layout(header);
  back.iconRect.y += effectiveVerticalOffset(back, header, verticalOffset);
  const int x = header.x + header.width - back.iconRect.width + (back.iconRect.width - icon.w) / 2;
  const int y = back.iconRect.y + (back.iconRect.height - icon.h) / 2;
  auto target = makeUiTarget(renderer);
  target.bitmap(fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(icon.w),
                          static_cast<int16_t>(icon.h)},
                fui::bitmapFromIcon(icon), fui::BitmapMode::Center);
}

Rect trailingTouchRect(const Rect& header) {
  const Rect touch = layout(header).touchRect;
  return Rect{header.x + header.width - (touch.x - header.x) - touch.width, touch.y, touch.width, touch.height};
}

}  // namespace TouchHeaderBackButton
