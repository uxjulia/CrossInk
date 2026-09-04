#include "LyraTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalTiltSensor.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "DeviceCapabilities.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "components/TouchRegistry.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "components/icons/readingStatsIcons.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
constexpr int topHintButtonY = 345;
constexpr int maxListValueWidth = 240;
constexpr uint32_t mainMenuIconSize = 32;
constexpr uint32_t listIconSize = 24;
constexpr int mainMenuColumns = 2;
int coverWidth = 0;

int centeredRowY(const int rowY, const int rowHeight, const int contentHeight) {
  return rowY + std::max(0, rowHeight - contentHeight) / 2;
}

int mainMenuIconYOffset(const UIIcon icon) {
  switch (icon) {
    case UIIcon::Chart:
      return -6;
    case UIIcon::Folder:
      return -4;
    case UIIcon::Recent:
      return -3;
    case UIIcon::Transfer:
      return -2;
    case UIIcon::Settings:
      return -2;
    case UIIcon::Library:
      return -4;
    default:
      return 0;
  }
}

}  // namespace

const freeink::Icon* LyraTheme::iconForName(UIIcon icon, uint32_t size) {
  if (size == 24) {
    switch (icon) {
      case UIIcon::Folder:
        return &icon_folder_24;
      case UIIcon::Text:
        return &icon_file_text_24;
      case UIIcon::Image:
        return &icon_image_24;
      case UIIcon::Book:
        return &icon_book_marked_24;
      case UIIcon::File:
        return &icon_file_24;
      default:
        return nullptr;
    }
  } else if (size == 32) {
    switch (icon) {
      case UIIcon::Chart:
        return &icon_reading_stats_32;
      case UIIcon::Folder:
        return &icon_folder_32;
      case UIIcon::Book:
        return &icon_book_marked_32;
      case UIIcon::Recent:
        return &icon_history_32;
      case UIIcon::Settings:
        return &icon_lyra_settings_32;
      case UIIcon::Transfer:
        return &icon_lyra_transfer_32;
      case UIIcon::Library:
        return &icon_lyra_library_32;
      case UIIcon::Wifi:
        return &icon_wifi_32;
      case UIIcon::Hotspot:
        return &icon_radio_tower_32;
      default:
        return nullptr;
    }
  }
  return nullptr;
}

void LyraTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  int currentX = rect.x + LyraMetrics::values.contentSidePadding;
  int rightSpace = LyraMetrics::values.contentSidePadding;
  if (rightLabel) {
    auto truncatedRightLabel =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    int rightLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, truncatedRightLabel.c_str());
    renderer.drawText(SMALL_FONT_ID, rect.x + rect.width - LyraMetrics::values.contentSidePadding - rightLabelWidth,
                      rect.y + 7, truncatedRightLabel.c_str());
    rightSpace += rightLabelWidth + hPaddingInSelection;
  }

  auto truncatedLabel = renderer.truncatedText(
      UI_10_FONT_ID, label, rect.width - LyraMetrics::values.contentSidePadding - rightSpace, EpdFontFamily::REGULAR);
  renderer.drawText(UI_10_FONT_ID, currentX, rect.y + 6, truncatedLabel.c_str(), true, EpdFontFamily::REGULAR);

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

void LyraTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  int currentX = rect.x + LyraMetrics::values.contentSidePadding;

  if (selected) {
    renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
  }

  const int textY = rect.y + (rect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;

  for (size_t i = 0; i < tabs.size(); ++i) {
    const auto& tab = tabs[i];
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tab.label, EpdFontFamily::REGULAR);
    TouchRegistry::getInstance().add(
        Rect{currentX, rect.y, textWidth + LyraMetrics::values.tabSpacing + 2 * hPaddingInSelection, rect.height},
        static_cast<int>(i), TouchRegistry::Tab);

    if (tab.selected) {
      if (selected) {
        renderer.fillRoundedRect(currentX, rect.y + 1, textWidth + 2 * hPaddingInSelection, rect.height - 4,
                                 cornerRadius, Color::Black);
      } else {
        renderer.fillRectDither(currentX, rect.y, textWidth + 2 * hPaddingInSelection, rect.height - 3,
                                Color::LightGray);
        renderer.drawLine(currentX, rect.y + rect.height - 3, currentX + textWidth + 2 * hPaddingInSelection,
                          rect.y + rect.height - 3, 2, true);
      }
    }

    renderer.drawText(UI_10_FONT_ID, currentX + hPaddingInSelection, textY, tab.label, !(tab.selected && selected),
                      EpdFontFamily::REGULAR);

    currentX += textWidth + LyraMetrics::values.tabSpacing + 2 * hPaddingInSelection;
  }

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

bool LyraTheme::tabIndexFromPoint(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                                  const int x, const int y, int& index) const {
  if (tabs.empty() || y < rect.y || y >= rect.y + rect.height) {
    return false;
  }

  int currentX = rect.x + LyraMetrics::values.contentSidePadding;
  for (size_t i = 0; i < tabs.size(); i++) {
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tabs[i].label, EpdFontFamily::REGULAR);
    const int tabWidth = textWidth + 2 * hPaddingInSelection;
    const int left = (i == 0) ? rect.x : currentX - LyraMetrics::values.tabSpacing / 2;
    const int right = currentX + tabWidth + LyraMetrics::values.tabSpacing / 2;
    if (x >= left && x < right) {
      index = static_cast<int>(i);
      return true;
    }
    currentX += tabWidth + LyraMetrics::values.tabSpacing;
  }

  return false;
}

int LyraTheme::getListRowStep(bool hasSubtitle, const int rowHeightScale) const {
  const int rowScale = std::max(1, rowHeightScale);
  int rowHeight =
      ((hasSubtitle) ? LyraMetrics::values.listWithSubtitleRowHeight : LyraMetrics::values.listRowHeight) * rowScale;
  return rowHeight;
}

int LyraTheme::getListPageItems(int contentHeight, bool hasSubtitle, const int rowHeightScale) const {
  const int rowStep = getListRowStep(hasSubtitle, rowHeightScale);
  if (rowStep <= 0) return 1;
  return std::max(1, contentHeight / rowStep);
}

void LyraTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed,
                         const std::function<bool(int index)>& isHeader, const int rowHeightScale,
                         const bool showSelection) const {
  drawListWithMetrics(renderer, rect, itemCount, selectedIndex, rowTitle, rowSubtitle, rowIcon, rowValue,
                      highlightValue, rowDimmed, isHeader, LyraMetrics::values, false, rowHeightScale, showSelection);
}

void LyraTheme::drawListWithMetrics(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                                    const std::function<std::string(int index)>& rowTitle,
                                    const std::function<std::string(int index)>& rowSubtitle,
                                    const std::function<UIIcon(int index)>& rowIcon,
                                    const std::function<std::string(int index)>& rowValue, bool highlightValue,
                                    const std::function<bool(int index)>& rowDimmed,
                                    const std::function<bool(int index)>& isHeader, const ThemeMetrics& metrics,
                                    const bool invertSelectedRows, const int rowHeightScale,
                                    const bool showSelection) const {
  const int rowScale = std::max(1, rowHeightScale);
  int rowHeight = ((rowSubtitle != nullptr) ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight) * rowScale;
  if (itemCount <= 0) return;
  const auto isHeaderRow = [&isHeader](int index) { return isHeader != nullptr && isHeader(index); };
  const int sectionHeaderTopPadding = halTiltSensor.isAvailable() ? 10 : 20;
  constexpr int sectionHeaderFontId = UI_10_FONT_ID;
  constexpr int sectionHeaderUnderlineGap = 4;
  const int sectionHeaderLineHeight = renderer.getLineHeight(sectionHeaderFontId);
  const int sectionHeaderRowHeight = sectionHeaderLineHeight + sectionHeaderUnderlineGap;
  const auto visualRowHeight = [&](int index) { return isHeaderRow(index) ? sectionHeaderRowHeight : rowHeight; };
  int totalContentHeight = 0;
  for (int i = 0; i < itemCount; ++i) {
    if (i > 0 && isHeaderRow(i)) totalContentHeight += sectionHeaderTopPadding;
    totalContentHeight += visualRowHeight(i);
  }
  const bool contentFits = totalContentHeight <= rect.height;
  int pageItems = contentFits ? itemCount : (rowHeight > 0 ? std::max(1, rect.height / rowHeight) : 1);

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (!contentFits && totalPages > 1) {
    const int scrollAreaHeight = rect.height;

    // Draw scroll bar
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - metrics.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - metrics.scrollBarWidth, scrollBarY, metrics.scrollBarWidth, scrollBarHeight, true);
  }

  // Draw selection (skip header rows)
  int contentWidth = rect.width - (totalPages > 1 ? (metrics.scrollBarWidth + metrics.scrollBarRightOffset) : 1);
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  if (showSelection && selectedIndex >= 0 && !isHeaderRow(selectedIndex)) {
    int selY = rect.y;
    for (int j = pageStartIndex; j < selectedIndex; j++) {
      selY += visualRowHeight(j);
      if (isHeaderRow(j + 1)) selY += sectionHeaderTopPadding;
    }
    renderer.fillRoundedRect(rect.x + metrics.contentSidePadding, selY, contentWidth - metrics.contentSidePadding * 2,
                             rowHeight, cornerRadius, invertSelectedRows ? Color::Black : Color::LightGray);
  }

  int textX = rect.x + metrics.contentSidePadding + hPaddingInSelection;
  int textWidth = contentWidth - metrics.contentSidePadding * 2 - hPaddingInSelection * 2;
  uint32_t iconSize;
  if (rowIcon != nullptr) {
    iconSize = (rowSubtitle != nullptr) ? mainMenuIconSize : listIconSize;
    textX += iconSize + hPaddingInSelection;
    textWidth -= iconSize + hPaddingInSelection;
  }

  const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  // Draw all items using a running Y to accommodate variable-height section headers.
  int currentY = rect.y;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    if (i > pageStartIndex && isHeaderRow(i)) currentY += sectionHeaderTopPadding;
    const int itemY = currentY;
    const int currentRowHeight = visualRowHeight(i);
    currentY += currentRowHeight;
    const bool selectedRow = showSelection && i == selectedIndex;
    const bool foreground = !(invertSelectedRows && selectedRow);

    if (isHeaderRow(i)) {
      // Section header: bold uppercase label + divider line below
      std::string label = rowTitle(i);
      std::transform(label.begin(), label.end(), label.begin(),
                     [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
      auto truncated = renderer.truncatedText(sectionHeaderFontId, label.c_str(),
                                              contentWidth - metrics.contentSidePadding * 2, EpdFontFamily::BOLD);
      const int headerTextY = itemY;
      renderer.drawText(sectionHeaderFontId, rect.x + metrics.contentSidePadding, headerTextY, truncated.c_str(), true,
                        EpdFontFamily::BOLD);
      renderer.drawLine(rect.x, itemY + currentRowHeight - 1, rect.x + contentWidth, itemY + currentRowHeight - 1,
                        true);
      continue;
    }
    TouchRegistry::getInstance().add(Rect{rect.x, itemY, contentWidth, currentRowHeight}, i, TouchRegistry::Item);

    int rowTextWidth = textWidth;

    // Draw name
    int valueWidth = 0;
    std::string valueText = "";
    if (rowValue != nullptr) {
      valueText = rowValue(i);
      if (!valueText.empty()) {
        valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), maxListValueWidth);
        valueWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str()) + hPaddingInSelection;
        rowTextWidth -= valueWidth;
      }
    }

    auto itemName = rowTitle(i);
    auto item = renderer.truncatedText(UI_10_FONT_ID, itemName.c_str(), rowTextWidth);
    const int titleY = rowSubtitle != nullptr ? itemY + 7 : centeredRowY(itemY, currentRowHeight, titleLineHeight);
    renderer.drawText(UI_10_FONT_ID, textX, titleY, item.c_str(), foreground);

    // Apply checkerboard dither to create gray text effect for dimmed items
    if (rowDimmed && rowDimmed(i) && !selectedRow) {
      const int titleWidth = renderer.getTextWidth(UI_10_FONT_ID, item.c_str());
      for (int py = titleY; py < titleY + titleLineHeight; py++)
        for (int px = textX; px < textX + titleWidth; px++)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      const freeink::Icon* iconBitmap = iconForName(icon, iconSize);
      if (iconBitmap != nullptr) {
        const int iconX = rect.x + metrics.contentSidePadding + hPaddingInSelection;
        const int iconY =
            rowSubtitle != nullptr ? itemY + 16 : centeredRowY(itemY, currentRowHeight, static_cast<int>(iconSize));
        if (invertSelectedRows && selectedRow) {
          drawLucideIcon(renderer, *iconBitmap, iconX, iconY, false);
        } else {
          drawLucideIcon(renderer, *iconBitmap, iconX, iconY);
        }
      }
    }

    if (rowSubtitle != nullptr) {
      // Draw subtitle
      std::string subtitleText = rowSubtitle(i);
      auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText.c_str(), rowTextWidth);
      renderer.drawText(SMALL_FONT_ID, textX, itemY + 30, subtitle.c_str(), foreground);
    }

    // Draw value
    if (!valueText.empty()) {
      if (selectedRow && highlightValue) {
        renderer.fillRoundedRect(rect.x + contentWidth - metrics.contentSidePadding - hPaddingInSelection - valueWidth,
                                 itemY, valueWidth + hPaddingInSelection, rowHeight, cornerRadius, Color::Black);
      }

      const int valueY = rowSubtitle != nullptr ? itemY + 16 : centeredRowY(itemY, currentRowHeight, titleLineHeight);
      const bool valueForeground = invertSelectedRows ? !selectedRow : !(selectedRow && highlightValue);
      renderer.drawText(UI_10_FONT_ID, rect.x + contentWidth - metrics.contentSidePadding - valueWidth, valueY,
                        valueText.c_str(), valueForeground);
    }
  }
}

void LyraTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4, const bool allowInvertedText) const {
  if (gpio.hasTouch()) return;

  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  const bool invertText = allowInvertedText && orig_orientation == GfxRenderer::Orientation::PortraitInverted;
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 80;
  constexpr int smallButtonHeight = 15;
  constexpr int buttonHeight = LyraMetrics::values.buttonHintsHeight;
  constexpr int buttonY = LyraMetrics::values.buttonHintsHeight;  // Distance from bottom
  constexpr int textYOffset = 7;                                  // Distance from top of button to text baseline
  // Keyed to the portrait panel width: the 528-wide X3 gets more spacing than
  // the 480-wide boards (X4, X4 Pro, and the other 800x480 panels).
  constexpr int narrowButtonPositions[] = {58, 146, 254, 342};
  constexpr int wideButtonPositions[] = {65, 157, 291, 383};
  const int* buttonPositions = renderer.getScreenWidth() >= 528 ? wideButtonPositions : narrowButtonPositions;
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    const int x = buttonPositions[i];
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      TouchRegistry::getInstance().add(Rect{x, pageHeight - buttonY, buttonWidth, buttonHeight}, i,
                                       TouchRegistry::Button);
      // Draw the filled background and border for a FULL-sized button
      renderer.fillRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, cornerRadius, Color::White);
      renderer.drawRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, 1, cornerRadius, true, true, false,
                               false, true);
    } else if (labels[i] != nullptr) {
      // Clear the previous full-sized hint before drawing the inactive marker.
      // Dictionary chaining can otherwise leave its old label visible.
      renderer.fillRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, false);
      const int smallButtonY = pageHeight - smallButtonHeight;
      renderer.fillRoundedRect(x, smallButtonY, buttonWidth, smallButtonHeight, cornerRadius, Color::White);
      renderer.drawRoundedRect(x, smallButtonY, buttonWidth, smallButtonHeight, 1, cornerRadius, true, true, false,
                               false, true);
    }
  }

  renderer.setOrientation(invertText ? GfxRenderer::Orientation::PortraitInverted : GfxRenderer::Orientation::Portrait);
  const int textY = invertText ? textYOffset : pageHeight - buttonY + textYOffset;

  for (int i = 0; i < 4; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[invertText ? 3 - i : i];
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(SMALL_FONT_ID, textX, textY, labels[i]);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void LyraTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  if (gpio.hasTouch()) return;

  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = LyraMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 78;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 0;

  if (deviceHasEdgeSideButtons(gpio)) {
    // Edge-button layout (X3, X4 Pro): Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(buttonMargin, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, false, true, false,
                               true, true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, topBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, buttonMargin, x3ButtonY + (buttonHeight + textWidth) / 2, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonWidth;
      renderer.drawRoundedRect(rightX, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, bottomBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, rightX, x3ButtonY + (buttonHeight + textWidth) / 2, bottomBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonWidth;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY + buttonHeight + 5, buttonWidth, buttonHeight, 1, cornerRadius, true,
                               false, true, false, true);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topHintButtonY + (i * buttonHeight) + 5;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        renderer.drawTextRotated90CW(SMALL_FONT_ID, x, y + (buttonHeight + textWidth) / 2, labels[i]);
      }
    }
  }
}

void LyraTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, const std::function<bool()>& storeCoverBuffer,
                                    const BookReadingStats* stats, float progressPercent,
                                    const GlobalReadingStats* globalStats, const char* currentChapterTitle) const {
  (void)globalStats;
  (void)currentChapterTitle;
  const int tileWidth = rect.width - 2 * LyraMetrics::values.contentSidePadding;
  const int tileHeight = rect.height;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();
  if (coverWidth == 0) {
    coverWidth = LyraMetrics::values.homeCoverHeight * 2 / 3;
  }

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    TouchRegistry::getInstance().add(Rect{LyraMetrics::values.contentSidePadding, tileY, tileWidth,
                                          LyraMetrics::values.homeCoverHeight + 2 * hPaddingInSelection},
                                     0, TouchRegistry::Cover);

    RecentBook book = recentBooks[0];
    if (!coverRendered) {
      std::string coverPath = book.coverBmpPath;
      bool hasCover = true;
      int tileX = LyraMetrics::values.contentSidePadding;
      if (coverPath.empty()) {
        hasCover = false;
      } else {
        const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, LyraMetrics::values.homeCoverHeight);

        // First time: load cover from SD and render
        HalFile file;
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            coverWidth = bitmap.getWidth();
            renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,
                                LyraMetrics::values.homeCoverHeight);
          } else {
            hasCover = false;
          }
          file.close();
        }
      }

      // Draw either way
      renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, coverWidth,
                        LyraMetrics::values.homeCoverHeight, true);

      if (!hasCover) {
        // Render empty cover
        renderer.fillRect(tileX + hPaddingInSelection,
                          tileY + hPaddingInSelection + (LyraMetrics::values.homeCoverHeight / 3), coverWidth,
                          2 * LyraMetrics::values.homeCoverHeight / 3, true);
        drawLucideIcon(renderer, icon_book_open_32, tileX + hPaddingInSelection + 24, tileY + hPaddingInSelection + 24);
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    bool bookSelected = (selectorIndex == 0);

    int tileX = LyraMetrics::values.contentSidePadding;
    int textWidth = tileWidth - 2 * hPaddingInSelection - LyraMetrics::values.verticalSpacing - coverWidth;

    if (bookSelected) {
      // Draw selection box
      renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                               Color::LightGray);
      renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection,
                              LyraMetrics::values.homeCoverHeight, Color::LightGray);
      renderer.fillRectDither(tileX + hPaddingInSelection + coverWidth, tileY + hPaddingInSelection,
                              tileWidth - hPaddingInSelection - coverWidth, LyraMetrics::values.homeCoverHeight,
                              Color::LightGray);
      renderer.fillRoundedRect(tileX, tileY + LyraMetrics::values.homeCoverHeight + hPaddingInSelection, tileWidth,
                               hPaddingInSelection, cornerRadius, false, false, true, true, Color::LightGray);
    }

    auto titleLines = renderer.wrappedText(UI_12_FONT_ID, book.title.c_str(), textWidth, 3, EpdFontFamily::BOLD);

    auto author = renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), textWidth);
    const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int statsLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int progressLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int titleBlockHeight = titleLineHeight * static_cast<int>(titleLines.size());
    const int authorHeight = book.author.empty() ? 0 : (renderer.getLineHeight(UI_10_FONT_ID) * 3 / 2);
    const bool hasStats = (stats != nullptr && stats->sessionCount > 0);
    const bool hasProgress = progressPercent >= 0.0f;
    const int statsBlockHeight = hasStats ? (statsLineHeight * 2 + 6) : 0;
    const int progressBlockHeight = hasProgress ? (progressLineHeight + 12) : 0;
    const int totalBlockHeight = titleBlockHeight + authorHeight + statsBlockHeight + progressBlockHeight;
    int titleY = tileY + tileHeight / 2 - totalBlockHeight / 2;
    const int textX = tileX + hPaddingInSelection + coverWidth + LyraMetrics::values.verticalSpacing;
    for (const auto& line : titleLines) {
      renderer.drawText(UI_12_FONT_ID, textX, titleY, line.c_str(), true, EpdFontFamily::BOLD);
      titleY += titleLineHeight;
    }
    if (!book.author.empty()) {
      titleY += renderer.getLineHeight(UI_10_FONT_ID) / 2;
      renderer.drawText(UI_10_FONT_ID, textX, titleY, author.c_str(), true);
      titleY += renderer.getLineHeight(UI_10_FONT_ID);
    }
    if (hasStats) {
      titleY += 6;
      char buf[48];
      char statLine[64];
      BookReadingStats::formatDuration(stats->totalReadingSeconds, buf, sizeof(buf));
      snprintf(statLine, sizeof(statLine), "%s%s", tr(STR_STATS_TOTAL_TIME), buf);
      renderer.drawText(SMALL_FONT_ID, textX, titleY, statLine, true);
      titleY += statsLineHeight;
      BookReadingStats::formatDuration(stats->totalReadingSeconds / stats->sessionCount, buf, sizeof(buf));
      snprintf(statLine, sizeof(statLine), "%s%s", tr(STR_STATS_AVG_SESSION), buf);
      renderer.drawText(SMALL_FONT_ID, textX, titleY, statLine, true);
      titleY += statsLineHeight;
    }
    if (hasProgress) {
      titleY += 8;
      constexpr int progressBarHeight = 4;
      const int progressBarWidth = textWidth;
      const int progressBarY = titleY + progressLineHeight + 2;
      const int filledWidth =
          std::clamp(static_cast<int>((progressPercent / 100.0f) * progressBarWidth), 0, progressBarWidth);
      char progressLabel[16];
      snprintf(progressLabel, sizeof(progressLabel), "%.0f%%", progressPercent);
      renderer.drawText(UI_10_FONT_ID, textX, titleY, progressLabel, true, EpdFontFamily::BOLD);
      renderer.drawRect(textX, progressBarY, progressBarWidth, progressBarHeight, true);
      if (filledWidth > 0) {
        renderer.fillRect(textX + 1, progressBarY + 1, std::max(0, filledWidth - 2),
                          std::max(0, progressBarHeight - 2));
      }
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}

void LyraTheme::drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const {
  constexpr int padding = 48;
  renderer.drawText(UI_12_FONT_ID, rect.x + padding,
                    rect.y + rect.height / 2 - renderer.getLineHeight(UI_12_FONT_ID) - 2, tr(STR_NO_OPEN_BOOK), true,
                    EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, rect.x + padding, rect.y + rect.height / 2 + 2, tr(STR_START_READING), true);
}

void LyraTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<const char*(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  const auto& menuMetrics = UITheme::getInstance().getMetrics();

  constexpr int maxVisibleItems = 7;
  const int pageItems = maxVisibleItems;
  const int totalPages = (buttonCount + pageItems - 1) / pageItems;

  if (totalPages > 1) {
    const int scrollAreaHeight =
        maxVisibleItems * (menuMetrics.menuRowHeight + menuMetrics.menuSpacing) - menuMetrics.menuSpacing;
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / buttonCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - LyraMetrics::values.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - LyraMetrics::values.scrollBarWidth, scrollBarY, LyraMetrics::values.scrollBarWidth,
                      scrollBarHeight, true);
  }

  const int pageStartIndex = (selectedIndex / pageItems) * pageItems;

  for (int i = pageStartIndex; i < buttonCount && i < pageStartIndex + pageItems; ++i) {
    const int displayIndex = i - pageStartIndex;
    int tileWidth = rect.width - menuMetrics.contentSidePadding * 2;
    if (totalPages > 1) {
      tileWidth -= (LyraMetrics::values.scrollBarWidth + LyraMetrics::values.scrollBarRightOffset);
    }
    Rect tileRect = Rect{rect.x + menuMetrics.contentSidePadding,
                         rect.y + displayIndex * (menuMetrics.menuRowHeight + menuMetrics.menuSpacing), tileWidth,
                         menuMetrics.menuRowHeight};
    TouchRegistry::getInstance().add(
        buttonMenuTouchTarget(tileRect, rect, i == buttonCount - 1, menuMetrics.menuSpacing), i, TouchRegistry::Item);

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, cornerRadius, Color::LightGray);
    }

    const char* label = buttonLabel != nullptr ? buttonLabel(i) : "";
    if (!label) label = "";
    int textX = tileRect.x + 16;
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textY = tileRect.y + (menuMetrics.menuRowHeight - lineHeight) / 2;

    if (rowIcon != nullptr) {
      UIIcon icon = rowIcon(i);
      if (icon == UIIcon::BookmarkIcon) {
        // Draw a small bookmark ribbon icon to match the status bar ribbon.
        const int ribbonWidth = 16;
        const int ribbonHeight = 22;
        const int notchSize = 6;
        // Center the ribbon horizontally within the mainMenuIconSize box
        const int iconX = textX + (mainMenuIconSize - ribbonWidth) / 2;
        const int iconY = textY + 4;
        const int centerX = iconX + ribbonWidth / 2;

        const int polyX[5] = {iconX, iconX + ribbonWidth, iconX + ribbonWidth, centerX, iconX};
        const int polyY[5] = {iconY, iconY, iconY + ribbonHeight, iconY + ribbonHeight - notchSize,
                              iconY + ribbonHeight};
        renderer.fillPolygon(polyX, polyY, 5, true);
        textX += mainMenuIconSize + hPaddingInSelection + 2;
      } else {
        const freeink::Icon* iconBitmap = iconForName(icon, mainMenuIconSize);
        if (iconBitmap != nullptr) {
          drawLucideIcon(renderer, *iconBitmap, textX, textY + 3 + mainMenuIconYOffset(icon));
          textX += mainMenuIconSize + hPaddingInSelection + 2;
        }
      }
    }

    renderer.drawText(UI_12_FONT_ID, textX, textY, label, true);
  }
}
