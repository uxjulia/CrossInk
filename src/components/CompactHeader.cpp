#include "CompactHeader.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>

#include <algorithm>
#include <string>

#include "HeaderDate.h"
#include "UITheme.h"
#include "fontIds.h"

namespace {
constexpr int kHeaderHeight = 67;
// All touch navigation headers reserve an 80px target above their divider.
constexpr int kTouchHeaderHeight = 81;
constexpr int kHeaderTopGap = 6;
constexpr int kHeaderTitleLift = 5;
constexpr int kHeaderBaselineLift = 2;

int visibleHeaderHeight(const ThemeMetrics& metrics) {
  return std::min(metrics.headerHeight, kHeaderHeight + UITheme::getDisplayStatusBarHeightIncrease());
}

int headerHeight(const ThemeMetrics& metrics) {
  return gpio.hasTouch() ? kTouchHeaderHeight + UITheme::getDisplayStatusBarHeightIncrease()
                         : visibleHeaderHeight(metrics);
}

int titleBaselineY(const GfxRenderer& renderer, const ThemeMetrics& metrics, const Rect& header) {
  const int availableH = header.height - metrics.batteryBarHeight;
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int titleY = header.y + metrics.batteryBarHeight + (availableH - titleLineHeight) / 2 - kHeaderTitleLift;
  return titleY + renderer.getFontAscenderSize(UI_12_FONT_ID) - kHeaderBaselineLift;
}
}  // namespace

namespace CompactHeader {
int height(const ThemeMetrics& metrics) { return headerHeight(metrics); }

int headerBottomY(const ThemeMetrics& metrics) { return metrics.topPadding + height(metrics); }

int contentTop(const ThemeMetrics& metrics) { return headerBottomY(metrics) + kHeaderTopGap; }

Rect headerRect(const GfxRenderer& renderer) {
  return UITheme::getHeaderRect(renderer, height(UITheme::getInstance().getMetrics()));
}

int headerBottomY(const GfxRenderer& renderer) {
  const auto header = headerRect(renderer);
  return header.y + header.height;
}

int contentTop(const GfxRenderer& renderer) { return headerBottomY(renderer) + kHeaderTopGap; }

void drawTitle(const GfxRenderer& renderer, const char* title, const bool showDate, const Rect* headerOverride) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header = headerOverride ? *headerOverride : headerRect(renderer);
  const int pageWidth = header.x + header.width;
  GUI.drawHeader(renderer, header, "");

  const int titleX = header.x + metrics.contentSidePadding;
  const int batteryStartX = pageWidth - metrics.contentSidePadding - metrics.batteryWidth;
  const int dateStartX = showDate ? pageWidth - headerDateReservedWidth(renderer) : pageWidth;
  const int titleRightX = std::min(batteryStartX, dateStartX) - metrics.contentSidePadding;
  const int maxTitleWidth = std::max(1, titleRightX - titleX);
  const int baselineY = titleBaselineY(renderer, metrics, header);
  const std::string visibleTitle = renderer.truncatedText(UI_12_FONT_ID, title, maxTitleWidth, EpdFontFamily::BOLD);

  renderer.drawText(UI_12_FONT_ID, titleX, baselineY - renderer.getFontAscenderSize(UI_12_FONT_ID),
                    visibleTitle.c_str(), true, EpdFontFamily::BOLD);
  if (showDate) {
    drawHeaderDateAtBaseline(renderer, pageWidth, baselineY);
  }
}
}  // namespace CompactHeader
