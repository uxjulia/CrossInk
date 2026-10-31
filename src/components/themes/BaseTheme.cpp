#include "BaseTheme.h"

#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "DeviceCapabilities.h"
#include "I18n.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "components/HeaderDate.h"
#include "components/TouchActionButtons.h"
#include "components/TouchRegistry.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int homeMenuMargin = 20;
constexpr int homeMarginTop = 30;

}  // namespace

void BaseTheme::drawBatteryOutline(const GfxRenderer& renderer, int x, int y, int battWidth, int rectHeight,
                                   const bool foregroundBlack) {
  // Top line
  renderer.drawLine(x + 1, y, x + battWidth - 3, y, foregroundBlack);
  // Bottom line
  renderer.drawLine(x + 1, y + rectHeight - 1, x + battWidth - 3, y + rectHeight - 1, foregroundBlack);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rectHeight - 2, foregroundBlack);
  // Battery end
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rectHeight - 2, foregroundBlack);
  renderer.drawPixel(x + battWidth - 1, y + 3, foregroundBlack);
  renderer.drawPixel(x + battWidth - 1, y + rectHeight - 4, foregroundBlack);
  renderer.drawLine(x + battWidth - 1, y + 4, x + battWidth - 1, y + rectHeight - 5, foregroundBlack);
}

void BaseTheme::drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY,
                                         const bool foregroundBlack) {
  // Draw lightning bolt (white/inverted on black fill for visibility)
  renderer.drawLine(boltX + 4, boltY + 0, boltX + 5, boltY + 0, foregroundBlack);
  renderer.drawLine(boltX + 3, boltY + 1, boltX + 4, boltY + 1, foregroundBlack);
  renderer.drawLine(boltX + 2, boltY + 2, boltX + 5, boltY + 2, foregroundBlack);
  renderer.drawLine(boltX + 3, boltY + 3, boltX + 4, boltY + 3, foregroundBlack);
  renderer.drawLine(boltX + 2, boltY + 4, boltX + 3, boltY + 4, foregroundBlack);
  renderer.drawLine(boltX + 1, boltY + 5, boltX + 4, boltY + 5, foregroundBlack);
  renderer.drawLine(boltX + 2, boltY + 6, boltX + 3, boltY + 6, foregroundBlack);
  renderer.drawLine(boltX + 1, boltY + 7, boltX + 2, boltY + 7, foregroundBlack);
}

void BaseTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage,
                                const bool foregroundBlack) const {
  const bool charging = gpio.isUsbConnected();

  if (charging) {
    // Solid fill when charging so lightning bolt is visible.
    renderer.fillRect(rect.x + 2, rect.y + 2, rect.width - 5, rect.height - 4, foregroundBlack);
    drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2, !foregroundBlack);
  } else {
    if (percentage > 10) {
      renderer.fillRect(rect.x + 2, rect.y + 2, 3, rect.height - 4, foregroundBlack);
    }
    if (percentage > 40) {
      renderer.fillRect(rect.x + 6, rect.y + 2, 3, rect.height - 4, foregroundBlack);
    }
    if (percentage > 70) {
      renderer.fillRect(rect.x + 10, rect.y + 2, 3, rect.height - 4, foregroundBlack);
    }
  }
}

void BaseTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const uint16_t percentage,
                                const bool showPercentage, const bool foregroundBlack, const int fontId) const {
  // Left aligned: icon on left, percentage on right (reader mode)
  // The icon's nub makes its visual center sit slightly below its bounding
  // box. Lift it one pixel to center it with the percentage text.
  const int y = rect.y + 5 + (renderer.getLineHeight(fontId) - renderer.getLineHeight(SMALL_FONT_ID)) / 2;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    renderer.drawText(fontId, rect.x + batteryPercentSpacing + rect.width, rect.y, percentageText.c_str(),
                      foregroundBlack);
  }

  const Rect iconRect{rect.x, y, rect.width, rect.height};
  drawBatteryOutline(renderer, rect.x, y, rect.width, rect.height, foregroundBlack);
  fillBatteryIcon(renderer, iconRect, percentage, foregroundBlack);
}

void BaseTheme::drawBatteryRight(const GfxRenderer& renderer, Rect rect, const uint16_t percentage,
                                 const bool showPercentage, const bool foregroundBlack, const int fontId) const {
  // Right aligned: percentage on left, icon on right (UI headers)
  // rect.x is already positioned for the icon (drawHeader calculated it)
  const int y = rect.y + 5 + (renderer.getLineHeight(fontId) - renderer.getLineHeight(SMALL_FONT_ID)) / 2;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    const int textWidth = renderer.getTextWidth(fontId, percentageText.c_str());
    renderer.drawText(fontId, rect.x - textWidth - batteryPercentSpacing, rect.y, percentageText.c_str(),
                      foregroundBlack);
  }

  const Rect iconRect{rect.x, y, rect.width, rect.height};
  drawBatteryOutline(renderer, rect.x, y, rect.width, rect.height, foregroundBlack);
  fillBatteryIcon(renderer, iconRect, percentage, foregroundBlack);
}

Rect BaseTheme::buttonMenuTouchTarget(const Rect rowRect, const Rect menuRect, const bool isLastItem,
                                      const int rowSpacing) {
  if (!isLastItem) return rowRect;

  const int touchTop = std::max(menuRect.y, rowRect.y - rowSpacing / 2);
  const int rowBottom = rowRect.y + rowRect.height;
  const int menuBottom = menuRect.y + menuRect.height;
  return Rect{rowRect.x, touchTop, rowRect.width, std::max(rowBottom, menuBottom) - touchTop};
}

void BaseTheme::drawProgressBar(const GfxRenderer& renderer, Rect rect, const size_t current,
                                const size_t total) const {
  if (total == 0) {
    return;
  }

  // Use 64-bit arithmetic to avoid overflow for large files
  const int percent = static_cast<int>((static_cast<uint64_t>(current) * 100) / total);

  // Draw outline
  renderer.drawRect(rect.x, rect.y, rect.width, rect.height);

  // Draw filled portion
  const int fillWidth = (rect.width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4);
  }

  // Draw percentage text centered below bar
  const std::string percentText = std::to_string(percent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, rect.y + rect.height + 15, percentText.c_str());
}

void BaseTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4, const bool allowInvertedText) const {
  if (gpio.hasTouch()) return;

  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  const bool invertText = allowInvertedText && orig_orientation == GfxRenderer::Orientation::PortraitInverted;
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 106;
  constexpr int buttonHeight = BaseMetrics::values.buttonHintsHeight;
  const int buttonY =
      BaseMetrics::values.buttonHintsHeight + UITheme::getButtonHintsBottomInset(renderer);  // Distance from bottom
  const int textYOffset = std::max(1, (buttonHeight - renderer.getTextHeight(UI_10_FONT_ID)) / 2);
  // Keyed to the portrait panel width: the 528-wide X3 gets more spacing than
  // the 480-wide boards (X4, X4 Pro, and the other 800x480 panels).
  constexpr int narrowButtonPositions[] = {25, 130, 245, 350};
  constexpr int wideButtonPositions[] = {38, 154, 268, 384};
  const int* buttonPositions = renderer.getScreenWidth() >= 528 ? wideButtonPositions : narrowButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    const int x = UITheme::getHintSafeX(renderer, buttonPositions[i], buttonWidth);
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      TouchRegistry::getInstance().add(Rect{x, pageHeight - buttonY, buttonWidth, buttonHeight}, i,
                                       TouchRegistry::Button);
      renderer.fillRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, false);
      renderer.drawRect(x, pageHeight - buttonY, buttonWidth, buttonHeight);
    } else if (labels[i] != nullptr) {
      // Fast refreshes retain the previous hint pixels. Clear a label that was
      // present on the last screen before leaving this button slot inactive.
      renderer.fillRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, false);
    }
  }

  renderer.setOrientation(invertText ? GfxRenderer::Orientation::PortraitInverted : GfxRenderer::Orientation::Portrait);
  const int textY =
      invertText ? UITheme::getButtonHintsBottomInset(renderer) + textYOffset : pageHeight - buttonY + textYOffset;

  for (int i = 0; i < 4; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[invertText ? 3 - i : i];
      const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, labels[i]);
      const int textX = UITheme::getHintSafeX(renderer, x + (buttonWidth - 1 - textWidth) / 2, textWidth);
      renderer.drawText(UI_10_FONT_ID, textX, textY, labels[i]);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void BaseTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  if (gpio.hasTouch()) return;

  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = BaseMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 80;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 4;

  if (deviceHasEdgeSideButtons(gpio)) {
    // Edge-button layout (X3, X4 Pro): Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      const int leftX = UITheme::getHintSafeX(renderer, buttonMargin, buttonWidth);
      renderer.drawRect(leftX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, topBtn);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      const int textX = leftX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = UITheme::getHintSafeX(renderer, screenWidth - buttonMargin - buttonWidth, buttonWidth);
      renderer.drawRect(rightX, x3ButtonY, buttonWidth, buttonHeight);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, bottomBtn);
      const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
      const int textX = rightX + (buttonWidth - textHeight) / 2;
      const int textY = x3ButtonY + (buttonHeight + textWidth) / 2;
      renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, bottomBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    const auto insets = renderer.getViewableInsets().rotated(static_cast<unsigned>(renderer.getOrientation()));
    const int topButtonY = std::clamp(
        345, static_cast<int>(insets.edges[0]),
        std::max(static_cast<int>(insets.edges[0]), renderer.getScreenHeight() - insets.edges[2] - 2 * buttonHeight));
    const char* labels[] = {topBtn, bottomBtn};
    const int x = UITheme::getHintSafeX(renderer, screenWidth - buttonMargin - buttonWidth, buttonWidth);

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawLine(x, topButtonY, x + buttonWidth - 1, topButtonY);
      renderer.drawLine(x, topButtonY, x, topButtonY + buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY, x + buttonWidth - 1, topButtonY + buttonHeight - 1);
    }

    if ((topBtn != nullptr && topBtn[0] != '\0') || (bottomBtn != nullptr && bottomBtn[0] != '\0')) {
      renderer.drawLine(x, topButtonY + buttonHeight, x + buttonWidth - 1, topButtonY + buttonHeight);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawLine(x, topButtonY + buttonHeight, x, topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x + buttonWidth - 1, topButtonY + buttonHeight, x + buttonWidth - 1,
                        topButtonY + 2 * buttonHeight - 1);
      renderer.drawLine(x, topButtonY + 2 * buttonHeight - 1, x + buttonWidth - 1, topButtonY + 2 * buttonHeight - 1);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topButtonY + i * buttonHeight;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        const int textHeight = renderer.getTextHeight(SMALL_FONT_ID);
        const int textX = x + (buttonWidth - textHeight) / 2;
        const int textY = y + (buttonHeight + textWidth) / 2;
        renderer.drawTextRotated90CW(SMALL_FONT_ID, textX, textY, labels[i]);
      }
    }
  }
}

int BaseTheme::getListRowStep(bool hasSubtitle, const int rowHeightScale) const {
  const int rowScale = std::max(1, rowHeightScale);
  int rowHeight =
      ((hasSubtitle) ? BaseMetrics::values.listWithSubtitleRowHeight : BaseMetrics::values.listRowHeight) * rowScale;
  return rowHeight;
}

int BaseTheme::getListPageItems(int contentHeight, bool hasSubtitle, const int rowHeightScale) const {
  const int rowStep = getListRowStep(hasSubtitle, rowHeightScale);
  if (rowStep <= 0) return 1;
  return std::max(1, contentHeight / rowStep);
}

void BaseTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed,
                         const std::function<bool(int index)>& isHeader, const int rowHeightScale,
                         const bool showSelection) const {
  const int rowScale = std::max(1, rowHeightScale);
  int rowHeight =
      ((rowSubtitle != nullptr) ? BaseMetrics::values.listWithSubtitleRowHeight : BaseMetrics::values.listRowHeight) *
      rowScale;
  int pageItems = rowHeight > 0 ? std::max(1, rect.height / rowHeight) : 1;
  constexpr int sectionHeaderTopPadding = 15;

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    constexpr int indicatorWidth = 20;
    constexpr int arrowSize = 6;
    constexpr int margin = 15;  // Offset from right edge

    const int centerX = rect.x + rect.width - indicatorWidth / 2 - margin;
    const int indicatorTop = rect.y;  // Offset to avoid overlapping side button hints
    const int indicatorBottom = rect.y + rect.height - arrowSize;

    // Draw up arrow at top (^) - narrow point at top, wide base at bottom
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + i * 2;
      const int startX = centerX - i;
      renderer.drawLine(startX, indicatorTop + i, startX + lineWidth - 1, indicatorTop + i);
    }

    // Draw down arrow at bottom (v) - wide base at top, narrow point at bottom
    for (int i = 0; i < arrowSize; ++i) {
      const int lineWidth = 1 + (arrowSize - 1 - i) * 2;
      const int startX = centerX - (arrowSize - 1 - i);
      renderer.drawLine(startX, indicatorBottom - arrowSize + 1 + i, startX + lineWidth - 1,
                        indicatorBottom - arrowSize + 1 + i);
    }
  }

  // Draw selection (skip header rows)
  int contentWidth = rect.width - 5;
  if (showSelection && selectedIndex >= 0) {
    renderer.fillRect(rect.x, rect.y + selectedIndex % pageItems * rowHeight - 2, rect.width, rowHeight);
  }
  constexpr int maxValueWidth = 240;
  constexpr int minValueGap = 10;

  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  const int rectBottom = rect.y + rect.height;
  if (showSelection && selectedIndex >= 0 && !(isHeader && isHeader(selectedIndex))) {
    int selY = rect.y;
    for (int j = pageStartIndex; j < selectedIndex; j++) {
      selY += rowHeight;
      if (isHeader && isHeader(j + 1)) selY += sectionHeaderTopPadding;
    }
    if (selY + rowHeight <= rectBottom) {
      renderer.fillRect(rect.x, selY - 2, rect.width, rowHeight);
    }
  }

  // Draw all visible page items
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;
    if (!(isHeader && isHeader(i))) {
      TouchRegistry::getInstance().add(Rect{rect.x, itemY - 2, rect.width, rowHeight}, i, TouchRegistry::Item);
    }

    int rowTextWidth = contentWidth - BaseMetrics::values.contentSidePadding * 2;
    std::string valueText;
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      if (!valueText.empty()) {
        int maxValW = std::max(0, rowTextWidth - 40 - minValueGap);
        valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), maxValW);
        int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str()) + minValueGap;
        rowTextWidth -= valueWidth;
      }
    }

    auto itemName = rowTitle(i);
    auto font = UI_10_FONT_ID;
    auto item = renderer.truncatedText(font, itemName.c_str(), rowTextWidth);
    if (isHeader && isHeader(i)) {
      renderer.drawText(font, rect.x + BaseMetrics::values.contentSidePadding, itemY, item.c_str(), true,
                        EpdFontFamily::BOLD);
      continue;
    }
    renderer.drawText(font, rect.x + BaseMetrics::values.contentSidePadding, itemY, item.c_str(), i != selectedIndex);

    // Apply checkerboard dither to create gray text effect for dimmed items
    if (rowDimmed && rowDimmed(i) && i != selectedIndex) {
      const int titleWidth = renderer.getTextWidth(font, item.c_str());
      const int lineH = renderer.getLineHeight(font);
      const int tx = rect.x + BaseMetrics::values.contentSidePadding;
      for (int py = itemY; py < itemY + lineH; py++)
        for (int px = tx; px < tx + titleWidth; px++)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }

    if (rowSubtitle != nullptr) {
      std::string subtitleText = rowSubtitle(i);
      if (!subtitleText.empty()) {
        auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
        renderer.drawText(SMALL_FONT_ID, rect.x + BaseMetrics::values.contentSidePadding, itemY + 22, subtitle.c_str(),
                          i != selectedIndex);
      }
    }

    if (!valueText.empty()) {
      const auto valueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
      int valueY = itemY;
      if (rowSubtitle != nullptr) {
        valueY = itemY + 10;
      }
      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - BaseMetrics::values.contentSidePadding - valueTextWidth,
                        valueY, valueText.c_str(), i != selectedIndex);
    }
  }
}

void BaseTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle,
                           const bool readerContext, const bool showStatus, const bool filenameTitle) const {
  namespace fui = freeink::ui;
  const auto spec = uiScaleSpec();
  fui::GfxRendererFrame<1> ui(renderer, spec.smallFontId, spec.bodyFontId, spec.titleFontId);
  const fui::ThemeTokens tokens = uiThemeTokens(ui.target);
  ui.target.setFont(fui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const fui::Rect band{static_cast<int16_t>(rect.x), static_cast<int16_t>(rect.y), static_cast<int16_t>(rect.width),
                       static_cast<int16_t>(rect.height)};

  fui::HeaderProps props;
  if (filenameTitle) ui.target.setFont(fui::GfxRendererTarget::FONT_TITLE, renderer.filenameFontId(spec.titleFontId));
  props.title = title;
  props.rightLabel = subtitle;
  props.borderEdges = fui::EdgeBottom;
  props.titleText = tokens.titleText;
  props.titleText.align = tokens.headerTitleAlign;
  props.subtitleText = tokens.smallText;
  props.styles = tokens.popup;
  props.sidePadding = tokens.headerSidePadding;
  const bool batteryDetached = metrics.headerBatteryDetached;
  if (batteryDetached) {
    const int titleLineHeight = ui.target.lineHeight(fui::GfxRendererTarget::FONT_TITLE);
    const int titleTop = static_cast<int>(band.height) - tokens.headerUnderline - tokens.spaceMd - titleLineHeight;
    props.titleOffsetY = static_cast<int16_t>(titleTop - (static_cast<int>(band.height) - titleLineHeight) / 2);
  }
  if (title != nullptr && props.styles.normal.border.kind == fui::PaintKind::None && tokens.headerUnderline > 0) {
    props.styles.normal.border = fui::Paint::solid(fui::Color::Black);
    props.styles.normal.borderWidth = tokens.headerUnderline;
  }
  fui::header(ui.frame, band, props);

  if (showStatus) drawDisplayStatusBar(renderer, rect.y);
}

void BaseTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  constexpr int underlineHeight = 2;  // Height of selection underline
  constexpr int underlineGap = 4;     // Gap between text and underline
  constexpr int maxListValueWidth = 200;

  int currentX = rect.x + BaseMetrics::values.contentSidePadding;
  int rightSpace = BaseMetrics::values.contentSidePadding;
  if (rightLabel) {
    auto truncatedRightLabel =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - BaseMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + 10;
  }

  auto truncatedLabel = renderer.truncatedText(
      UI_12_FONT_ID, label, rect.width - BaseMetrics::values.contentSidePadding - rightSpace, EpdFontFamily::REGULAR);
  renderer.drawText(UI_12_FONT_ID, currentX, rect.y, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);
}

void BaseTheme::drawTabBar(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  constexpr int underlineHeight = 2;  // Height of selection underline
  constexpr int underlineGap = 4;     // Gap between text and underline

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);

  int currentX = rect.x + BaseMetrics::values.contentSidePadding;

  for (size_t i = 0; i < tabs.size(); ++i) {
    const auto& tab = tabs[i];
    const int textWidth =
        renderer.getTextWidth(UI_12_FONT_ID, tab.label, tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    TouchRegistry::getInstance().add(
        Rect{currentX - 3, rect.y, textWidth + BaseMetrics::values.tabSpacing, rect.height}, static_cast<int>(i),
        TouchRegistry::Tab);

    // Draw underline for selected tab
    if (tab.selected) {
      if (selected) {
        renderer.fillRect(currentX - 3, rect.y, textWidth + 6, lineHeight + underlineGap);
      } else {
        renderer.fillRect(currentX, rect.y + lineHeight + underlineGap, textWidth, underlineHeight);
      }
    }

    // Draw tab label
    renderer.drawText(UI_12_FONT_ID, currentX, rect.y, tab.label, !(tab.selected && selected),
                      tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

    currentX += textWidth + BaseMetrics::values.tabSpacing;
  }
}

bool BaseTheme::tabIndexFromPoint(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                                  const int x, const int y, int& index) const {
  if (tabs.empty() || y < rect.y || y >= rect.y + rect.height) {
    return false;
  }

  int currentX = rect.x + BaseMetrics::values.contentSidePadding;
  for (size_t i = 0; i < tabs.size(); i++) {
    const auto& tab = tabs[i];
    const int textWidth =
        renderer.getTextWidth(UI_12_FONT_ID, tab.label, tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    const int left = (i == 0) ? rect.x : currentX - BaseMetrics::values.tabSpacing / 2;
    const int right = currentX + textWidth + BaseMetrics::values.tabSpacing / 2;
    if (x >= left && x < right) {
      index = static_cast<int>(i);
      return true;
    }
    currentX += textWidth + BaseMetrics::values.tabSpacing;
  }

  return false;
}

// Draw the "Recent Book" cover card on the home screen
// TODO: Refactor method to make it cleaner, split into smaller methods
void BaseTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, const std::function<bool()>& storeCoverBuffer,
                                    const BookReadingStats* /*stats*/, float /*progressPercent*/,
                                    const GlobalReadingStats* /*globalStats*/,
                                    const char* /*currentChapterTitle*/) const {
  const bool hasContinueReading = !recentBooks.empty();
  const bool bookSelected = hasContinueReading && selectorIndex == 0;

  // --- Top "book" card for the current title (selectorIndex == 0) ---
  // When there's no cover image, use fixed size (half screen)
  // When there's cover image, adapt width to image aspect ratio, keep height fixed at 400px
  const int baseHeight = rect.height;  // Fixed height (400px)

  int bookWidth, bookX;
  bool hasCoverImage = false;

  if (hasContinueReading && !recentBooks[0].coverBmpPath.empty()) {
    // Try to get actual image dimensions from BMP header
    const std::string coverBmpPath =
        UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

    FsFile file;
    if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        hasCoverImage = true;
        const int imgWidth = bitmap.getWidth();
        const int imgHeight = bitmap.getHeight();

        // Calculate width based on aspect ratio, maintaining baseHeight
        if (imgWidth > 0 && imgHeight > 0) {
          const float aspectRatio = static_cast<float>(imgWidth) / static_cast<float>(imgHeight);
          bookWidth = static_cast<int>(baseHeight * aspectRatio);

          // Ensure width doesn't exceed reasonable limits (max 90% of screen width)
          const int maxWidth = static_cast<int>(rect.width * 0.9f);
          if (bookWidth > maxWidth) {
            bookWidth = maxWidth;
          }
        } else {
          bookWidth = rect.width / 2;  // Fallback
        }
      }
    }
  }

  if (!hasCoverImage) {
    // No cover: use half screen size
    bookWidth = rect.width / 2;
  }

  bookX = rect.x + (rect.width - bookWidth) / 2;
  const int bookY = rect.y;
  const int bookHeight = baseHeight;
  if (hasContinueReading) {
    TouchRegistry::getInstance().add(Rect{bookX, bookY, bookWidth, bookHeight}, 0, TouchRegistry::Cover);
  }

  // Bookmark dimensions (used in multiple places)
  const int bookmarkWidth = bookWidth / 8;
  const int bookmarkHeight = bookHeight / 5;
  const int bookmarkX = bookX + bookWidth - bookmarkWidth - 10;
  const int bookmarkY = bookY + 5;

  // Draw book card regardless, fill with message based on `hasContinueReading`
  {
    // Draw cover image as background if available (inside the box)
    // Only load from SD on first render, then use stored buffer

    if (hasContinueReading && !recentBooks[0].coverBmpPath.empty() && !coverRendered) {
      const std::string coverBmpPath =
          UITheme::getCoverThumbPath(recentBooks[0].coverBmpPath, BaseMetrics::values.homeCoverHeight);

      // First time: load cover from SD and render
      FsFile file;
      if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          // Draw the cover image (bookWidth and bookHeight already match image aspect ratio)
          renderer.drawBitmap(bitmap, bookX, bookY, bookWidth, bookHeight);

          // Draw border around the card
          renderer.drawRect(bookX, bookY, bookWidth, bookHeight);

          // No bookmark ribbon when cover is shown - it would just cover the art

          // Store the buffer with cover image for fast navigation
          coverBufferStored = storeCoverBuffer();
          coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer

          // First render: if selected, draw selection indicators now
          if (bookSelected) {
            renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
            renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
          }
        }
      }
    }

    if (!bufferRestored && !coverRendered) {
      // No cover image: draw border or fill, plus bookmark as visual flair
      if (bookSelected) {
        renderer.fillRect(bookX, bookY, bookWidth, bookHeight);
      } else {
        renderer.drawRect(bookX, bookY, bookWidth, bookHeight);
      }

      // Draw bookmark ribbon when no cover image (visual decoration)
      if (hasContinueReading) {
        const int notchDepth = bookmarkHeight / 3;
        const int centerX = bookmarkX + bookmarkWidth / 2;

        const int xPoints[5] = {
            bookmarkX,                  // top-left
            bookmarkX + bookmarkWidth,  // top-right
            bookmarkX + bookmarkWidth,  // bottom-right
            centerX,                    // center notch point
            bookmarkX                   // bottom-left
        };
        const int yPoints[5] = {
            bookmarkY,                                // top-left
            bookmarkY,                                // top-right
            bookmarkY + bookmarkHeight,               // bottom-right
            bookmarkY + bookmarkHeight - notchDepth,  // center notch point
            bookmarkY + bookmarkHeight                // bottom-left
        };

        // Draw bookmark ribbon (inverted if selected)
        renderer.fillPolygon(xPoints, yPoints, 5, !bookSelected);
      }
    }

    // If buffer was restored, draw selection indicators if needed
    if (bufferRestored && bookSelected && coverRendered) {
      // Draw selection border (no bookmark inversion needed since cover has no bookmark)
      renderer.drawRect(bookX + 1, bookY + 1, bookWidth - 2, bookHeight - 2);
      renderer.drawRect(bookX + 2, bookY + 2, bookWidth - 4, bookHeight - 4);
    } else if (!coverRendered && !bufferRestored) {
      // Selection border already handled above in the no-cover case
    }
  }

  if (!hasContinueReading) {
    // No book to continue reading
    const int y =
        bookY + (bookHeight - renderer.getLineHeight(UI_12_FONT_ID) - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_NO_OPEN_BOOK));
    renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_12_FONT_ID), tr(STR_START_READING));
  }
}

int BaseTheme::getMenuRowHeight(const GfxRenderer&) const { return UITheme::getInstance().getMetrics().menuRowHeight; }

void BaseTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<const char*(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rowIcon;
  constexpr int maxVisibleItems = 7;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int rowStep = metrics.menuRowHeight + metrics.menuSpacing;
  const int availableHeight = std::max(0, rect.height - metrics.verticalSpacing);
  const int pageItems = std::clamp((availableHeight + metrics.menuSpacing) / rowStep, 1, maxVisibleItems);
  const int totalPages = (buttonCount + pageItems - 1) / pageItems;

  const int pageStartIndex = (std::max(0, selectedIndex) / pageItems) * pageItems;

  if (totalPages > 1) {
    constexpr int indicatorWidth = 20;
    constexpr int arrowSize = 6;
    constexpr int margin = 15;  // Offset from right edge

    const int centerX = rect.x + rect.width - indicatorWidth / 2 - margin;
    const int menuHeight = pageItems * rowStep - metrics.menuSpacing;
    const int indicatorTop = rect.y + metrics.verticalSpacing;
    const int indicatorBottom = indicatorTop + menuHeight - arrowSize;

    // Draw up arrow (^) only when there are items above the current page
    if (pageStartIndex > 0) {
      for (int i = 0; i < arrowSize; ++i) {
        const int lineWidth = 1 + i * 2;
        const int startX = centerX - i;
        renderer.drawLine(startX, indicatorTop + i, startX + lineWidth - 1, indicatorTop + i);
      }
    }

    // Draw down arrow (v) only when there are items below the current page
    if (pageStartIndex + pageItems < buttonCount) {
      for (int i = 0; i < arrowSize; ++i) {
        const int lineWidth = 1 + (arrowSize - 1 - i) * 2;
        const int startX = centerX - (arrowSize - 1 - i);
        renderer.drawLine(startX, indicatorBottom - arrowSize + 1 + i, startX + lineWidth - 1,
                          indicatorBottom - arrowSize + 1 + i);
      }
    }
  }

  for (int i = pageStartIndex; i < buttonCount && i < pageStartIndex + pageItems; ++i) {
    const int displayIndex = i - pageStartIndex;
    const int tileY = metrics.verticalSpacing + rect.y + static_cast<int>(displayIndex) * rowStep;

    const bool selected = selectedIndex == i;
    constexpr int paginationGutter = 30;
    const int tileSidePadding = metrics.contentSidePadding + (totalPages > 1 ? paginationGutter / 2 : 0);
    const int tileWidth = rect.width - tileSidePadding * 2;
    const Rect tileRect{rect.x + tileSidePadding, tileY, tileWidth, metrics.menuRowHeight};
    TouchRegistry::getInstance().add(buttonMenuTouchTarget(tileRect, rect, i == buttonCount - 1, metrics.menuSpacing),
                                     i, TouchRegistry::Item);

    if (selected) {
      renderer.fillRect(rect.x + tileSidePadding, tileY, tileWidth, metrics.menuRowHeight);
    } else {
      renderer.drawRect(rect.x + tileSidePadding, tileY, tileWidth, metrics.menuRowHeight);
    }

    const char* label = buttonLabel != nullptr ? buttonLabel(i) : "";
    if (!label) label = "";
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int textX = rect.x + tileSidePadding + (tileWidth - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY =
        tileY + (metrics.menuRowHeight - lineHeight) / 2;  // vertically centered assuming y is top of text
    // Invert text when the tile is selected, to contrast with the filled background
    renderer.drawText(UI_10_FONT_ID, textX, textY, label, selectedIndex != i);
  }
}

Rect BaseTheme::drawPopup(const GfxRenderer& renderer, const char* message, const bool preserveBackdrop) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int marginX = metrics.popupMarginX;
  const int marginY = metrics.popupMarginY;
  const int frameThickness = metrics.popupFrameThickness;
  const EpdFontFamily::Style popupFontFamily = metrics.popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const auto visible = renderer.truncatedText(
      UI_12_FONT_ID, message, std::max(0, safe.width - 2 * (marginX + frameThickness)), popupFontFamily);
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, visible.c_str(), popupFontFamily);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + marginX * 2;
  const int h = textHeight + marginY * 2;
  const int x = safe.x + (safe.width - w) / 2;
  const int y = std::clamp(safe.y + static_cast<int>(safe.height * metrics.popupTopOffsetRatio),
                           safe.y + frameThickness, safe.y + safe.height - h - frameThickness);

  const int backupX = x - frameThickness;
  const int backupY = y - frameThickness;
  const int backupW = w + frameThickness * 2;
  const int backupH = h + frameThickness * 2;
  // Only the small popup rectangle is retained, briefly, rather than another
  // framebuffer. Allocation failure skips feedback without damaging the page.
  std::unique_ptr<uint8_t[]> backdrop;
  if (preserveBackdrop) {
    const size_t bytes = renderer.getRegionByteSize(backupX, backupY, backupW, backupH);
    if (bytes != 0) backdrop = makeUniqueNoThrow<uint8_t[]>(bytes);
    if (!backdrop || !renderer.copyRegionToBuffer(backupX, backupY, backupW, backupH, backdrop.get(), bytes)) {
      LOG_ERR("GUI", "Unable to preserve loading popup backdrop");
      return Rect{x, y, w, h};
    }
  }

  const bool useRoundedPopup = metrics.popupCornerRadius > 0;
  if (useRoundedPopup) {
    renderer.fillRoundedRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2,
                             metrics.popupCornerRadius + frameThickness, Color::White);
    renderer.fillRoundedRect(x, y, w, h, metrics.popupCornerRadius, Color::Black);
  } else {
    renderer.fillRect(x - frameThickness, y - frameThickness, w + frameThickness * 2, h + frameThickness * 2, true);
    renderer.fillRect(x, y, w, h, false);
  }

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + marginY + metrics.popupTextBaselineOffsetY;
  renderer.drawText(UI_12_FONT_ID, textX, textY, visible.c_str(), metrics.popupTextInverted, popupFontFamily);
  renderer.displayBuffer();
  if (backdrop) {
    renderer.copyBufferToRegion(backupX, backupY, backupW, backupH, backdrop.get(),
                                renderer.getRegionByteSize(backupX, backupY, backupW, backupH));
  }
  return Rect{x, y, w, h};
}

void BaseTheme::fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int barHeight = metrics.popupProgressBarHeight;
  const int barWidth =
      std::max(0, layout.width - metrics.popupMarginX * 2);  // twice the margin in drawPopup to match text width
  const int barX = layout.x + (layout.width - barWidth) / 2;
  const int barY = layout.y + layout.height - metrics.popupMarginY / 2 - barHeight / 2 - 1;
  if (barWidth <= 0 || barHeight <= 0) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const int scaledProgress = metrics.popupProgressClampPercent ? std::clamp(progress, 0, 100) : progress;
  const int fillWidth = barWidth * scaledProgress / 100;

  if (metrics.popupProgressDrawOutline) {
    renderer.drawRect(barX, barY, barWidth, barHeight, 1, metrics.popupProgressOutlineInverted);
  }
  if (fillWidth > 0) {
    renderer.fillRect(barX, barY, fillWidth, barHeight, metrics.popupProgressFillInverted);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void BaseTheme::drawReaderStatusBar(const GfxRenderer& renderer, const ReaderStatusBarPosition position,
                                    const ReaderStatusBarContent& content,
                                    const ReaderStatusBarConfig* overrideConfig) const {
  const ReaderStatusBarConfig config = overrideConfig ? *overrideConfig : SETTINGS.readerStatusBar(position);
  if (config.hidden) return;
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const int fontId = content.outsideReader ? UITheme::getDisplayStatusBarFontId() : UITheme::getReaderStatusBarFontId();
  const int textLaneHeight = content.outsideReader ? UITheme::getDisplayStatusBarTextHeight(renderer)
                                                   : UITheme::getReaderStatusBarTextHeight(renderer);
  const bool top = position == ReaderStatusBarPosition::Top;
  const bool foregroundBlack = !content.darkMode;
  const bool clockAvailable = halClock.isAvailable() || content.previewClock != nullptr;
  const bool hasText = config.hasTextItems(clockAvailable) || (!top && content.autoTurnLabel != nullptr);
  const int progressHeight = config.progressBar != CrossPointSettings::HIDE_PROGRESS
                                 ? static_cast<int>((config.progressBarThickness + 1) * 2)
                                 : 0;
  const int progressSpace = progressHeight > 0 ? progressHeight + metrics.progressBarMarginTop : 0;
  const int textHeight = hasText ? textLaneHeight : 0;
  const int totalHeight = readerStatusBarTotalHeight(position, hasText, progressSpace, textLaneHeight);
  if (totalHeight <= 0) return;

  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  const int screenWidth = renderer.getScreenWidth();
  const int edgeY = content.previewOriginY >= 0
                        ? content.previewOriginY
                        : (top ? UITheme::getTopStatusBarY(renderer) + content.edgePadding
                               : renderer.getScreenHeight() - marginBottom - totalHeight - content.edgePadding);
  const int textY =
      edgeY + (top ? progressSpace : 0) +
      (hasText ? (top ? ReaderStatusBarConfig::TOP_TEXT_INSET : (textHeight - renderer.getLineHeight(fontId)) / 2) : 0);

  const float chapterProgress =
      content.chapterProgress >= 0
          ? content.chapterProgress
          : (content.chapterPageCount > 0 ? 100.0f * content.chapterPage / content.chapterPageCount : 0.0f);
  if (progressHeight > 0 && content.showProgress) {
    const float percent =
        config.progressBar == CrossPointSettings::BOOK_PROGRESS ? content.bookProgress : chapterProgress;
    const int bookmarkOffset = !top && !hasText && content.bookmarked
                                   ? metrics.statusBarHorizontalMargin + 1 + ReaderStatusBarConfig::BOOKMARK_WIDTH + 4
                                   : 0;
    const int barWidth = std::max(0, screenWidth - marginLeft - marginRight - bookmarkOffset);
    const int filled = static_cast<int>(barWidth * std::clamp(percent, 0.0f, 100.0f) / 100.0f);
    const int barY = top ? edgeY : edgeY + totalHeight - progressHeight;
    renderer.fillRect(marginLeft + bookmarkOffset, barY, filled, progressHeight, foregroundBlack);
  }
  int bookmarkReserve = 0;
  if (!top && content.bookmarked) {
    constexpr int bookmarkWidth = ReaderStatusBarConfig::BOOKMARK_WIDTH;
    constexpr int bookmarkHeight = ReaderStatusBarConfig::BOOKMARK_HEIGHT;
    const int bookmarkY = hasText ? textY + (metrics.batteryHeight - bookmarkHeight) / 2 + 5 +
                                        (renderer.getLineHeight(fontId) - renderer.getLineHeight(SMALL_FONT_ID)) / 2
                                  : edgeY;
    const int bookmarkX = marginLeft + metrics.statusBarHorizontalMargin + 1;
    renderer.fillRect(bookmarkX, bookmarkY, bookmarkWidth, bookmarkHeight, foregroundBlack);
    const int notchX[3] = {bookmarkX, bookmarkX + bookmarkWidth, bookmarkX + bookmarkWidth / 2};
    const int notchY[3] = {bookmarkY + bookmarkHeight, bookmarkY + bookmarkHeight, bookmarkY + bookmarkHeight - 5};
    renderer.fillPolygon(notchX, notchY, 3, content.darkMode);
    bookmarkReserve = bookmarkWidth + 4;
  }
  if (!hasText) return;

  const bool batteryIcon = config.batteryStyle != ReaderStatusBarBatteryStyle::PercentOnly;
  const bool batteryPercent = config.batteryStyle != ReaderStatusBarBatteryStyle::IconOnly;
  // Measure and draw the same value even when the battery poll interval expires mid-render.
  const uint16_t batteryPercentage =
      content.batteryPercentage >= 0
          ? static_cast<uint16_t>(content.batteryPercentage)
          : (config.contains(ReaderStatusBarItem::Battery) ? powerManager.getBatteryPercentage() : 0);
  const auto itemText = [&](const ReaderStatusBarItem item, char* scratch, const size_t len) -> const char* {
    switch (item) {
      case ReaderStatusBarItem::Date:
        return formatHeaderDateText(scratch, len) ? scratch : nullptr;
      case ReaderStatusBarItem::Clock:
        if (content.previewClock) return content.previewClock;
        return halClock.isAvailable() &&
                       halClock.formatTime(scratch, len, SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)
                   ? scratch
                   : nullptr;
      case ReaderStatusBarItem::Battery:
        // Icon styles are drawn separately; percentage-only renders as plain text.
        snprintf(scratch, len, "%u%%", static_cast<unsigned>(batteryPercentage));
        return scratch;
      case ReaderStatusBarItem::TimeLeftBook:
        return content.timeLeftBook;
      case ReaderStatusBarItem::TimeLeftChapter:
        return content.timeLeftChapter;
      case ReaderStatusBarItem::ChapterPageCount:
        if (!content.showProgress || content.chapterPageCount <= 0) return nullptr;
        snprintf(scratch, len, "%s%d/%d", content.pageCountEstimated ? "~" : "", content.chapterPage,
                 content.chapterPageCount);
        return scratch;
      case ReaderStatusBarItem::StablePageNumber:
        if (!content.showProgress || content.stablePageCount <= 0 || content.stablePage <= 0) return nullptr;
        snprintf(scratch, len, "%d/%d", content.stablePage, content.stablePageCount);
        return scratch;
      case ReaderStatusBarItem::BookProgressPercentage:
        if (!content.showProgress) return nullptr;
        snprintf(scratch, len, "%.*f%%", static_cast<int>(config.percentageFormat), content.bookProgress);
        return scratch;
      case ReaderStatusBarItem::ChapterProgressPercentage:
        if (!content.showProgress || (content.chapterProgress < 0 && content.chapterPageCount <= 0)) return nullptr;
        snprintf(scratch, len, "%.*f%%", static_cast<int>(config.percentageFormat), chapterProgress);
        return scratch;
      case ReaderStatusBarItem::TitleBook:
        return content.bookTitle;
      case ReaderStatusBarItem::TitleChapter:
        return content.chapterTitle;
      default:
        return nullptr;
    }
  };
  const auto measureItem = [&](const ReaderStatusBarItem item, const int maxWidth) {
    if (item == ReaderStatusBarItem::Battery && batteryIcon) {
      int width = metrics.batteryWidth;
      if (batteryPercent) {
        char percent[8];
        snprintf(percent, sizeof(percent), "%u%%", static_cast<unsigned>(batteryPercentage));
        width += batteryPercentSpacing + renderer.getTextWidth(fontId, percent);
      }
      return width;
    }
    char scratch[48];
    const char* value = itemText(item, scratch, sizeof(scratch));
    return value && value[0] ? std::min(maxWidth, renderer.getTextWidth(fontId, value)) : 0;
  };
  const auto drawItem = [&](const ReaderStatusBarItem item, const int x, const int allowedWidth,
                            const bool alignRight) {
    if (item == ReaderStatusBarItem::Battery && batteryIcon) {
      if (allowedWidth < metrics.batteryWidth) return;
      char percent[8];
      snprintf(percent, sizeof(percent), "%u%%", static_cast<unsigned>(batteryPercentage));
      const bool showPercent = batteryPercent && allowedWidth >= metrics.batteryWidth + batteryPercentSpacing +
                                                                     renderer.getTextWidth(fontId, percent);
      if (top && alignRight) {
        drawBatteryRight(
            renderer, Rect{x + allowedWidth - metrics.batteryWidth, textY, metrics.batteryWidth, metrics.batteryHeight},
            batteryPercentage, showPercent, foregroundBlack, fontId);
      } else {
        drawBatteryLeft(renderer, Rect{x, textY, metrics.batteryWidth, metrics.batteryHeight}, batteryPercentage,
                        showPercent, foregroundBlack, fontId);
      }
      return;
    }
    char scratch[48];
    const char* value = itemText(item, scratch, sizeof(scratch));
    if (!value || !value[0] || allowedWidth <= 0) return;
    std::string clipped;
    int width = renderer.getTextWidth(fontId, value);
    if (width > allowedWidth) {
      clipped = renderer.truncatedText(fontId, value, allowedWidth);
      value = clipped.c_str();
      width = renderer.getTextWidth(fontId, value);
    }
    renderer.drawText(fontId, x + (alignRight ? allowedWidth - width : 0), textY, value, foregroundBlack);
  };

  constexpr int itemGap = 8;
  int leftEdge =
      top ? std::max(marginLeft, StatusBarMetrics::sideInset) : marginLeft + metrics.statusBarHorizontalMargin + 1;
  int rightEdge = screenWidth - (top ? std::max(marginRight, StatusBarMetrics::sideInset)
                                     : marginRight + metrics.statusBarHorizontalMargin);
  if (content.horizontalBounds) {
    // A header can be narrower than the screen because of button-hint gutters.
    leftEdge = std::max(leftEdge, content.horizontalBounds->x + StatusBarMetrics::sideInset);
    rightEdge = std::min(rightEdge,
                         content.horizontalBounds->x + content.horizontalBounds->width - StatusBarMetrics::sideInset);
  }
  const int available = std::max(0, rightEdge - leftEdge);

  if (config.centered) {
    // One centered group in slot order; the bottom bar's auto-turn label still replaces the center item.
    std::array<ReaderStatusBarItem, ReaderStatusBarConfig::SLOT_COUNT> items = config.slots;
    if (!top && content.autoTurnLabel) items[ReaderStatusBarConfig::CENTER] = ReaderStatusBarItem::TitleChapter;
    std::array<int, ReaderStatusBarConfig::SLOT_COUNT> itemWidths{};
    for (unsigned i = 0; i < itemWidths.size(); ++i) itemWidths[i] = measureItem(items[i], available);
    const char* glyph = readerStatusBarSeparatorGlyph(config.separator);
    const int groupCenterX =
        content.horizontalBounds ? content.horizontalBounds->x + content.horizontalBounds->width / 2 : screenWidth / 2;
    const int separatorWidth = renderer.getTextWidth(fontId, glyph);
    auto group = layoutCenteredReaderStatusBarItems(leftEdge, rightEdge, groupCenterX, itemWidths, separatorWidth,
                                                    bookmarkReserve);
    // Ellipsized text ends short of its trimmed slot; lay out again on the drawn widths so no gap is left.
    bool trimmed = false;
    for (unsigned i = 0; i < items.size(); ++i) {
      if (group.widths[i] <= 0 || group.widths[i] >= itemWidths[i]) continue;
      if (items[i] == ReaderStatusBarItem::Battery && batteryIcon) {
        // A squeezed battery drops its percentage, or disappears, so no separator sits beside a gap.
        itemWidths[i] = group.widths[i] >= metrics.batteryWidth ? metrics.batteryWidth : 0;
        trimmed = true;
        continue;
      }
      char scratch[48];
      const char* value = itemText(items[i], scratch, sizeof(scratch));
      if (!value || !value[0]) continue;
      // +1 because truncatedText keeps only candidates strictly narrower than the limit, so drawItem
      // reproduces this same string instead of dropping another character.
      itemWidths[i] = renderer.getTextWidth(fontId, renderer.truncatedText(fontId, value, group.widths[i]).c_str()) + 1;
      trimmed = true;
    }
    if (trimmed) {
      group = layoutCenteredReaderStatusBarItems(leftEdge, rightEdge, groupCenterX, itemWidths, separatorWidth,
                                                 bookmarkReserve);
    }
    for (unsigned i = 0; i < items.size(); ++i) {
      if (group.widths[i] > 0) drawItem(items[i], group.x[i], group.widths[i], false);
    }
    for (int i = 0; i < group.separatorCount; ++i) {
      renderer.drawText(fontId, group.separatorX[i], textY, glyph, foregroundBlack);
    }
    return;
  }

  std::array<int, ReaderStatusBarConfig::SLOT_COUNT> widths{};
  for (unsigned i = ReaderStatusBarConfig::LEFT_FIRST; i <= ReaderStatusBarConfig::LEFT_THIRD; ++i) {
    widths[i] = measureItem(config.slots[i], available);
  }
  for (unsigned i = ReaderStatusBarConfig::RIGHT_FIRST; i <= ReaderStatusBarConfig::RIGHT_THIRD; ++i) {
    widths[i] = measureItem(config.slots[i], available);
  }
  fitReaderStatusBarSideWidths(widths, available, bookmarkReserve, itemGap);
  const auto placement = layoutReaderStatusBarItems(leftEdge, rightEdge, widths, bookmarkReserve, itemGap);
  for (unsigned i = ReaderStatusBarConfig::LEFT_FIRST; i <= ReaderStatusBarConfig::LEFT_THIRD; ++i) {
    if (widths[i] > 0) drawItem(config.slots[i], placement.x[i], widths[i], false);
  }
  for (unsigned i = ReaderStatusBarConfig::RIGHT_FIRST; i <= ReaderStatusBarConfig::RIGHT_THIRD; ++i) {
    if (widths[i] > 0) drawItem(config.slots[i], placement.x[i], widths[i], true);
  }

  const int centerWidth = placement.centerRight - placement.centerLeft;
  const auto centerItem =
      !top && content.autoTurnLabel ? ReaderStatusBarItem::TitleChapter : config.slots[ReaderStatusBarConfig::CENTER];
  if (centerWidth > 0) {
    const int width = measureItem(centerItem, centerWidth);
    if (width > 0) {
      const int centerX = content.horizontalBounds
                              ? content.horizontalBounds->x + (content.horizontalBounds->width - width) / 2
                              : (screenWidth - width) / 2;
      const int centeredX = std::clamp(centerX, placement.centerLeft, placement.centerRight - width);
      drawItem(centerItem, centeredX, width, false);
    }
  }
}

#ifdef SIMULATOR
static int g_lastDisplayStatusBarOriginY = -1;
int BaseTheme::takeLastDisplayStatusBarOriginY() {
  const int origin = g_lastDisplayStatusBarOriginY;
  g_lastDisplayStatusBarOriginY = -1;
  return origin;
}
#endif

void BaseTheme::drawDisplayStatusBar(const GfxRenderer& renderer, const int topY, const Rect* horizontalBounds) const {
  ReaderStatusBarContent content;
  content.outsideReader = true;
  content.horizontalBounds = horizontalBounds;
  // Every screen shares the reader's calibrated row. Only deliberate downward displacement (for example a
  // top button-hint gutter) moves the bar below it; legacy topPadding rects and getHeaderRect() rects must
  // not pull it off the row.
  const int rowTop = UITheme::getHeaderRect(renderer, 0).y;
  const int row = UITheme::getTopStatusBarY(renderer);
  const bool displaced = topY > std::max(rowTop, static_cast<int>(UITheme::getInstance().getMetrics().topPadding));
  content.previewOriginY = displaced ? topY + (row - rowTop) : row;
#ifdef SIMULATOR
  g_lastDisplayStatusBarOriginY = content.previewOriginY;
#endif
  const auto config = SETTINGS.displayStatusBar.asReaderConfig();
  drawReaderStatusBar(renderer, ReaderStatusBarPosition::Top, content, &config);
}

void BaseTheme::drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  auto truncatedLabel =
      renderer.truncatedText(SMALL_FONT_ID, label, rect.width - metrics.contentSidePadding * 2, EpdFontFamily::REGULAR);
  const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedLabel.c_str());
  renderer.drawText(SMALL_FONT_ID, rect.x + (rect.width - textWidth) / 2, rect.y, truncatedLabel.c_str());
}

void BaseTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                              int contentStartX, int contentWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineY = rect.y + rect.height + lineHeight + metrics.verticalSpacing;
  const int thickness = cursorMode ? metrics.textFieldCursorThickness : metrics.textFieldNormalThickness;
  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY,
                      rect.x + contentStartX + contentWidth + metrics.textFieldLineEndOffset, lineY, thickness, true);
  } else {
    const int lineW = textWidth + metrics.textFieldHorizontalPadding * 2;
    const int lineStart = rect.x + (rect.width - lineW) / 2;
    renderer.drawLine(lineStart, lineY, lineStart + lineW + metrics.textFieldLineEndOffset, lineY, thickness, true);
  }
}

void BaseTheme::drawOptionPopup(const GfxRenderer& renderer, const char* title, OptionLabels options, int selectedIndex,
                                const bool showConfirmationFooter, const char* cancelLabel, const char* saveLabel,
                                const bool saveFocused, const int primaryOptionIndex, const char* noteLabel,
                                const char* noteBody, const int firstOptionIndex, const char* secondNoteLabel,
                                const char* secondNoteBody) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int pageWidth = safe.width;
  const int pageHeight = safe.height;

  const int optionFontId = uiScaleSpec().bodyFontId;
  const EpdFontFamily::Style optionStyle =
      metrics.optionPopupOptionFontBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

#if CROSSINK_APP_CAP_TOUCH
  const bool touchActionStyle = gpio.hasTouch() && primaryOptionIndex >= 0 && options.size() == 2;
  const int itemSpacing = touchActionStyle ? TouchActionButtons::kDefaultGap : metrics.optionPopupItemSpacing;
#else
  const int itemSpacing = metrics.optionPopupItemSpacing;
#endif
  const int innerPadding = metrics.optionPopupInnerPadding;
  const int selectionHPadding = metrics.optionPopupSelectionHPadding;
  const int selectionVPadding = metrics.optionPopupSelectionVPadding;

  const int optionLineHeight = renderer.getLineHeight(optionFontId);
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const bool hasNote = noteLabel && noteBody;
  const int noteLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int noteHeight = hasNote ? noteLineHeight * 2 + metrics.optionPopupTitleGap : 0;
#if CROSSINK_APP_CAP_TOUCH
  const int rowHeight =
      touchActionStyle ? TouchActionButtons::kDefaultHeight : optionLineHeight + selectionVPadding * 2;
#else
  const int rowHeight = optionLineHeight + selectionVPadding * 2;
#endif

  int maxTextWidth = renderer.getTextWidth(UI_12_FONT_ID, title, EpdFontFamily::BOLD);
  for (size_t i = 0; i < options.size(); ++i) {
    const char* opt = options[i];
    const auto style = primaryOptionIndex == static_cast<int>(i) ? EpdFontFamily::BOLD : optionStyle;
    int w = renderer.getTextWidth(optionFontId, opt, style);
    if (w > maxTextWidth) maxTextWidth = w;
  }
  if (hasNote) {
    const int noteLabelWidth = renderer.getTextWidth(UI_10_FONT_ID, noteLabel, EpdFontFamily::BOLD);
    const int noteBodyWidth = renderer.getTextWidth(UI_10_FONT_ID, noteBody);
    const int noteWidth = noteLabelWidth + renderer.getSpaceWidth(UI_10_FONT_ID) + noteBodyWidth;
    maxTextWidth = std::max(maxTextWidth, noteWidth);
    if (secondNoteLabel && secondNoteBody) {
      const int secondWidth = renderer.getTextWidth(UI_10_FONT_ID, secondNoteLabel, EpdFontFamily::BOLD) +
                              renderer.getSpaceWidth(UI_10_FONT_ID) +
                              renderer.getTextWidth(UI_10_FONT_ID, secondNoteBody);
      maxTextWidth = std::max(maxTextWidth, secondWidth);
    }
  }

  const int optionCount = static_cast<int>(options.size());
  if (optionCount <= 0) {
    return;
  }

  constexpr int footerHeight = 56;
  const int footerSpace = showConfirmationFooter ? footerHeight : 0;
  const int maxDialogH =
      std::max(rowHeight + titleLineHeight + metrics.optionPopupTitleGap + noteHeight + innerPadding * 2 + footerSpace,
               pageHeight - metrics.optionPopupDialogSideMargin * 2);
  // Reserve the narrow scroll gutter up front. A wrapped title may reduce the
  // number of visible options, so deciding this after title layout would make
  // the draw and cached touch geometry disagree.
  const int dialogW = std::min((maxTextWidth + innerPadding * 2 + selectionHPadding * 2 + metrics.scrollBarWidth +
                                metrics.scrollBarRightOffset + selectionHPadding) *
                                   12 / 10,
                               pageWidth - metrics.optionPopupDialogSideMargin * 2);
  const int titleContentWidth = std::max(1, dialogW - innerPadding * 2);
  const int maxTitleLines =
      std::max(1, (maxDialogH - innerPadding * 2 - metrics.optionPopupTitleGap - noteHeight - rowHeight - footerSpace) /
                      titleLineHeight);
  const auto titleLines =
      renderer.wrappedText(UI_12_FONT_ID, title, titleContentWidth, maxTitleLines, EpdFontFamily::BOLD);
  const int titleHeight = static_cast<int>(titleLines.size()) * titleLineHeight;
  const int maxListHeight = std::max(
      rowHeight, maxDialogH - innerPadding * 2 - titleHeight - metrics.optionPopupTitleGap - noteHeight - footerSpace);
  const int rowStep = rowHeight + itemSpacing;
  const int maxVisibleOptions = std::max(1, std::min(optionCount, (maxListHeight + itemSpacing) / rowStep));
  const int safeSelectedIndex = std::clamp(selectedIndex, 0, optionCount - 1);
  const int centeredStart = std::clamp(safeSelectedIndex - maxVisibleOptions / 2, 0, optionCount - maxVisibleOptions);
  const int visibleStart =
      firstOptionIndex < 0 ? centeredStart : std::clamp(firstOptionIndex, 0, optionCount - maxVisibleOptions);
  const int visibleEnd = visibleStart + maxVisibleOptions;
  const int visibleCount = visibleEnd - visibleStart;
  const int listHeight = rowHeight * visibleCount + itemSpacing * (visibleCount - 1);
  const bool hasHiddenOptions = visibleCount < optionCount;
  const int scrollBarGutter =
      hasHiddenOptions ? metrics.scrollBarWidth + metrics.scrollBarRightOffset + selectionHPadding : 0;
  const int contentHeight = titleHeight + metrics.optionPopupTitleGap + noteHeight + listHeight;
  const int dialogH = contentHeight + innerPadding * 2 + footerSpace;
  const int dialogX = safe.x + (pageWidth - dialogW) / 2;
  const int dialogY = safe.y + (pageHeight - dialogH) / 2;

  const int frameThickness = metrics.popupFrameThickness;
  const int frameRadius = metrics.popupCornerRadius;

  if (frameRadius > 0) {
    renderer.fillRoundedRect(dialogX - frameThickness, dialogY - frameThickness, dialogW + frameThickness * 2,
                             dialogH + frameThickness * 2, frameRadius + frameThickness, Color::White);
    renderer.fillRoundedRect(dialogX, dialogY, dialogW, dialogH, frameRadius, Color::Black);
    renderer.fillRoundedRect(dialogX + frameThickness, dialogY + frameThickness, dialogW - frameThickness * 2,
                             dialogH - frameThickness * 2,
                             frameRadius - frameThickness > 0 ? frameRadius - frameThickness : 0, Color::White);
  } else {
    renderer.fillRect(dialogX - frameThickness, dialogY - frameThickness, dialogW + frameThickness * 2,
                      dialogH + frameThickness * 2, true);
    renderer.fillRect(dialogX, dialogY, dialogW, dialogH, false);
  }

  int y = dialogY + innerPadding;

  for (const auto& line : titleLines) {
    const int lineWidth = renderer.getTextWidth(UI_12_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, dialogX + (dialogW - lineWidth) / 2, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += titleLineHeight;
  }

  if (metrics.optionPopupTitleSeparator || hasNote) {
    const int sepY = y + metrics.optionPopupTitleGap / 2;
    renderer.drawLine(dialogX + innerPadding, sepY, dialogX + dialogW - innerPadding, sepY, true);
  }

  y += metrics.optionPopupTitleGap;

  if (hasNote) {
    if (secondNoteLabel && secondNoteBody) {
      const auto drawNoteLine = [&](const char* label, const char* body) {
        const int labelWidth = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD);
        const int spaceWidth = renderer.getSpaceWidth(UI_10_FONT_ID);
        const int lineWidth = labelWidth + spaceWidth + renderer.getTextWidth(UI_10_FONT_ID, body);
        const int noteX = dialogX + (dialogW - lineWidth) / 2;
        renderer.drawText(UI_10_FONT_ID, noteX, y, label, true, EpdFontFamily::BOLD);
        renderer.drawText(UI_10_FONT_ID, noteX + labelWidth + spaceWidth, y, body);
        y += noteLineHeight;
      };
      drawNoteLine(noteLabel, noteBody);
      drawNoteLine(secondNoteLabel, secondNoteBody);
    } else {
      const int noteContentWidth = std::max(1, dialogW - innerPadding * 2);
      const std::string noteText = std::string(noteLabel) + " " + noteBody;
      const auto noteLines = renderer.wrappedText(UI_10_FONT_ID, noteText.c_str(), noteContentWidth, 2);
      const int labelWidth = renderer.getTextWidth(UI_10_FONT_ID, noteLabel, EpdFontFamily::BOLD);
      const int spaceWidth = renderer.getSpaceWidth(UI_10_FONT_ID);
      const std::string labelPrefix = std::string(noteLabel) + " ";
      for (size_t i = 0; i < noteLines.size(); ++i) {
        const auto& line = noteLines[i];
        if (i == 0 && (line == noteLabel || line.rfind(labelPrefix, 0) == 0)) {
          const std::string bodyLine =
              line.size() > labelPrefix.size() ? line.substr(labelPrefix.size()) : std::string();
          const int bodyWidth =
              bodyLine.empty() ? 0 : spaceWidth + renderer.getTextWidth(UI_10_FONT_ID, bodyLine.c_str());
          const int lineWidth = labelWidth + bodyWidth;
          const int noteX = dialogX + (dialogW - lineWidth) / 2;
          renderer.drawText(UI_10_FONT_ID, noteX, y, noteLabel, true, EpdFontFamily::BOLD);
          if (!bodyLine.empty()) {
            renderer.drawText(UI_10_FONT_ID, noteX + labelWidth + spaceWidth, y, bodyLine.c_str());
          }
        } else {
          const int lineWidth = renderer.getTextWidth(UI_10_FONT_ID, line.c_str());
          renderer.drawText(UI_10_FONT_ID, dialogX + (dialogW - lineWidth) / 2, y, line.c_str());
        }
        y += noteLineHeight;
      }
      y += std::max(0, 2 - static_cast<int>(noteLines.size())) * noteLineHeight;
    }

    const int separatorY = y + metrics.optionPopupTitleGap / 2;
    renderer.drawLine(dialogX + innerPadding, separatorY, dialogX + dialogW - innerPadding, separatorY, true);
    y += metrics.optionPopupTitleGap;
  }

  const int itemRectX = dialogX + innerPadding;
  const int itemRectW = std::max(1, dialogW - innerPadding * 2 - scrollBarGutter);
  const int selectionRadius = metrics.optionPopupSelectionRadius;

  if (hasHiddenOptions) {
    const int scrollBarX = dialogX + dialogW - innerPadding - metrics.scrollBarRightOffset;
    const int scrollBarHeight = std::max(metrics.scrollBarWidth, (listHeight * visibleCount) / optionCount);
    const int scrollRange = std::max(0, listHeight - scrollBarHeight);
    const int scrollSteps = std::max(1, optionCount - visibleCount);
    const int scrollBarY = y + (scrollRange * visibleStart) / scrollSteps;
    renderer.drawLine(scrollBarX, y, scrollBarX, y + listHeight, true);
    renderer.fillRect(scrollBarX - metrics.scrollBarWidth, scrollBarY, metrics.scrollBarWidth, scrollBarHeight, true);
  }

#if CROSSINK_APP_CAP_TOUCH
  if (touchActionStyle && visibleCount == 2) {
    const auto actionLayout = TouchActionButtons::vertical(Rect{itemRectX, y, itemRectW, listHeight},
                                                           static_cast<uint8_t>(visibleCount), rowHeight, itemSpacing);
    const int primaryOffset = primaryOptionIndex - visibleStart;
    const int secondaryOffset = primaryOffset == 0 ? 1 : 0;
    const char* labels[] = {options[visibleStart + primaryOffset], options[visibleStart + secondaryOffset]};
    const int selectedVisualIndex = safeSelectedIndex == primaryOptionIndex ? 0 : 1;
    TouchActionButtons::draw(renderer, actionLayout, labels, 0, saveFocused ? -1 : selectedVisualIndex, optionFontId);
  } else
#endif
  {
    for (int visibleIndex = 0; visibleIndex < visibleCount; visibleIndex++) {
      const int optionIndex = visibleStart + visibleIndex;
      const int itemY = y + visibleIndex * (rowHeight + itemSpacing);
      const bool disabled = options.isDisabled(optionIndex);
      const bool selected = !disabled && !saveFocused && optionIndex == safeSelectedIndex;
      const char* labelText = options[optionIndex];

      if (metrics.optionPopupDrawAllRows || selected) {
        Color rowColor;
        if (selected) {
          rowColor = metrics.optionPopupSelectionLight ? Color::LightGray : Color::Black;
        } else {
          rowColor = Color::White;
        }
        if (selectionRadius > 0) {
          renderer.fillRoundedRect(itemRectX, itemY, itemRectW, rowHeight, selectionRadius, rowColor);
        } else {
          renderer.fillRect(itemRectX, itemY, itemRectW, rowHeight, rowColor == Color::Black);
        }
      }

      const auto style = primaryOptionIndex == optionIndex ? EpdFontFamily::BOLD : optionStyle;
      const int textW = renderer.getTextWidth(optionFontId, labelText, style);
      const int textY = itemY + (rowHeight - optionLineHeight) / 2;
      const int textX = itemRectX + (itemRectW - textW) / 2;
      // Unselected items: text is dark (invert=true means draw on white bg).
      // Selected on dark bg: text must be white (invert=false).
      // Selected on light bg: text stays dark (invert=true).
      const bool invertText = disabled || (selected ? metrics.optionPopupSelectionLight : true);
      renderer.drawText(optionFontId, textX, textY, labelText, invertText, style);
      // Mark unavailable actions without shading their row like a selection.
      if (disabled) renderer.drawLine(textX, textY + optionLineHeight / 2, textX + textW, textY + optionLineHeight / 2);
    }
  }

  if (showConfirmationFooter) {
    const int footerY = dialogY + dialogH - footerHeight;
#if CROSSINK_APP_CAP_TOUCH
    const char* leftLabel = cancelLabel ? cancelLabel : "";
#endif
    const char* rightLabel = saveLabel ? saveLabel : "";
    // Button devices already map Back to cancel, so the on-screen action can
    // use the full footer width for Save.
    renderer.drawLine(dialogX, footerY, dialogX + dialogW, footerY, true);
    const int labelY = footerY + (footerHeight - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
#if CROSSINK_APP_CAP_TOUCH
    if (gpio.hasTouch()) {
      const int dividerX = dialogX + dialogW / 2;
      renderer.drawLine(dividerX, footerY, dividerX, dialogY + dialogH, true);
      if (saveFocused) renderer.fillRect(dividerX, footerY + 1, dialogX + dialogW - dividerX, footerHeight - 1, true);
      renderer.drawText(UI_12_FONT_ID, dialogX + (dialogW / 2 - renderer.getTextWidth(UI_12_FONT_ID, leftLabel)) / 2,
                        labelY, leftLabel, true, EpdFontFamily::REGULAR);
      renderer.drawText(UI_12_FONT_ID, dividerX + (dialogW / 2 - renderer.getTextWidth(UI_12_FONT_ID, rightLabel)) / 2,
                        labelY, rightLabel, !saveFocused, EpdFontFamily::BOLD);
    } else {
#endif
      if (saveFocused) renderer.fillRect(dialogX, footerY + 1, dialogW, footerHeight - 1, true);
      renderer.drawText(UI_12_FONT_ID, dialogX + (dialogW - renderer.getTextWidth(UI_12_FONT_ID, rightLabel)) / 2,
                        labelY, rightLabel, !saveFocused, EpdFontFamily::BOLD);
#if CROSSINK_APP_CAP_TOUCH
    }
#endif
  }
}
