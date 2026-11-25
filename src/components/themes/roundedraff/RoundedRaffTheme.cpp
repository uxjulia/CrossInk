#include "RoundedRaffTheme.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <HalTiltSensor.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/TouchRegistry.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace {
constexpr int kCoverRadius = 18;
constexpr int kMenuRadius = 30;
constexpr int kBottomRadius = 15;
constexpr int kRowRadius = 20;
constexpr int kInteractiveInsetX = 15;
constexpr int kSelectableRowGap = 6;
constexpr int kHomeMenuSidePadding = RoundedRaffMetrics::values.contentSidePadding;
constexpr int kListSidePadding = 10;
constexpr int kTabHorizontalInset = 2;
constexpr int kTitleFontId = UI_12_FONT_ID;     // Requested main title size: 12px
constexpr int kSubtitleFontId = SMALL_FONT_ID;  // Requested subtitle size: 8px
constexpr int kGuideFontId = SMALL_FONT_ID;     // Closest available to requested 6px

void drawScrollBar(const GfxRenderer& renderer, Rect rect, int itemCount, int pageStartIndex, int pageItems) {
  if (itemCount <= 0 || pageItems <= 0 || itemCount <= pageItems) {
    return;
  }

  const int barW = RoundedRaffMetrics::values.scrollBarWidth;
  const int barX = rect.x + rect.width - RoundedRaffMetrics::values.scrollBarRightOffset - barW;
  const int barY = rect.y;
  const int barH = rect.height;

  const int thumbH = std::max(10, (barH * pageItems) / itemCount);
  const int maxStart = std::max(1, itemCount - pageItems);
  const int maxTravel = std::max(1, barH - thumbH);
  const int clampedStart = std::clamp(pageStartIndex, 0, maxStart);
  const int thumbY = barY + (clampedStart * maxTravel) / maxStart;

  renderer.fillRect(barX, thumbY, barW, thumbH);
}

}  // namespace
int coverWidth = 0;

void RoundedRaffTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle,
                                  const bool readerContext, const bool showStatus, const bool filenameTitle) const {
  // Home screen header is custom-rendered in drawRecentBookCover.
  if (title == nullptr) {
    if (showStatus) drawDisplayStatusBar(renderer, rect.y);
    return;
  }
  BaseTheme::drawHeader(renderer, rect, title, subtitle, readerContext, showStatus, filenameTitle);
}

void RoundedRaffTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                                  bool selected) const {
  if (tabs.empty()) {
    return;
  }

  const int tabCount = static_cast<int>(tabs.size());
  const int tabY = rect.y + 4;
  const int tabHeight = rect.height - 12;

  for (int i = 0; i < tabCount; i++) {
    const int slotX = rect.x + (i * rect.width) / tabCount;
    const int nextSlotX = rect.x + ((i + 1) * rect.width) / tabCount;
    const int slotWidth = nextSlotX - slotX;
    const int tabInsetX = std::min(kTabHorizontalInset, slotWidth / 2);
    const int tabX = slotX + tabInsetX;
    const int tabWidth = std::max(0, slotWidth - tabInsetX * 2);
    const auto& tab = tabs[i];
    TouchRegistry::getInstance().add(Rect{slotX, rect.y, slotWidth, rect.height}, i, TouchRegistry::Tab);

    if (tab.selected) {
      renderer.fillRoundedRect(tabX, tabY, tabWidth, tabHeight, 18, selected ? Color::Black : Color::DarkGray);
    }

    const int textWidth = renderer.getTextWidth(kTitleFontId, tab.label, EpdFontFamily::BOLD);
    const int textX = tabX + (tabWidth - textWidth) / 2;
    const int textY = tabY + (tabHeight - renderer.getLineHeight(kTitleFontId)) / 2;
    renderer.drawText(kTitleFontId, textX, textY, tab.label, !(tab.selected), EpdFontFamily::BOLD);
  }

  // Full-width divider between tabs and setting rows.
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
}

bool RoundedRaffTheme::tabIndexFromPoint(const GfxRenderer& renderer, const Rect rect, const std::vector<TabInfo>& tabs,
                                         const int x, const int y, int& index) const {
  (void)renderer;
  if (tabs.empty() || y < rect.y || y >= rect.y + rect.height || x < rect.x || x >= rect.x + rect.width) {
    return false;
  }

  const int slotWidth = std::max(1, rect.width / static_cast<int>(tabs.size()));
  index = std::min(static_cast<int>(tabs.size()) - 1, (x - rect.x) / slotWidth);
  return true;
}

void RoundedRaffTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                           int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                           bool& bufferRestored, const std::function<bool()>& storeCoverBuffer,
                                           const BookReadingStats* stats, float progressPercent,
                                           const GlobalReadingStats* globalStats,
                                           const char* currentChapterTitle) const {
  (void)stats;
  (void)progressPercent;
  (void)globalStats;
  (void)currentChapterTitle;
  (void)selectorIndex;
  (void)bufferRestored;
  const int tileWidth = rect.width - 2 * RoundedRaffMetrics::values.contentSidePadding;
  const int tileHeight = rect.height;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();
  if (coverWidth == 0) {
    coverWidth = RoundedRaffMetrics::values.homeCoverHeight * 2 / 3;
  }
  const int imgY = tileY + (tileHeight - RoundedRaffMetrics::values.homeCoverHeight) / 2;
  const int tileX = rect.x + RoundedRaffMetrics::values.contentSidePadding;
  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    TouchRegistry::getInstance().add(Rect{tileX, tileY, tileWidth, tileHeight}, 0, TouchRegistry::Cover);
    RecentBook book = recentBooks[0];
    if (!coverRendered) {
      std::string coverPath = book.coverBmpPath;
      bool hasCover = true;
      if (coverPath.empty()) {
        hasCover = false;
      } else {
        const std::string coverBmpPath =
            UITheme::getCoverThumbPath(coverPath, RoundedRaffMetrics::values.homeCoverHeight);

        // First time: load cover from SD and render
        HalFile file;
        if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
          Bitmap bitmap(file);
          if (bitmap.parseHeaders() == BmpReaderError::Ok) {
            coverWidth = bitmap.getWidth();
            renderer.drawBitmap(bitmap, tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                                RoundedRaffMetrics::values.homeCoverHeight);
            renderer.maskRoundedRectOutsideCorners(tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                                                   RoundedRaffMetrics::values.homeCoverHeight, kCoverRadius,
                                                   Color::LightGray);
          } else {
            hasCover = false;
          }
          file.close();
        }
      }

      // Draw either way
      renderer.drawRoundedRect(tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                               RoundedRaffMetrics::values.homeCoverHeight, 1, kCoverRadius, true);

      if (!hasCover) {
        // Render empty cover
        renderer.fillRect(tileX + (tileWidth - coverWidth) / 2, imgY + (RoundedRaffMetrics::values.homeCoverHeight / 3),
                          coverWidth, 2 * RoundedRaffMetrics::values.homeCoverHeight / 3, true);
        drawLucideIcon(renderer, icon_book_open_32, tileX + (tileWidth - coverWidth) / 2 + 24, imgY + 24);
        renderer.maskRoundedRectOutsideCorners(tileX + (tileWidth - coverWidth) / 2, imgY, coverWidth,
                                               RoundedRaffMetrics::values.homeCoverHeight, kCoverRadius,
                                               Color::LightGray);
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    renderer.fillRoundedRect(tileX, tileY, tileWidth, imgY - tileY, kRowRadius, true, true, false, false,
                             Color::LightGray);
    renderer.fillRectDither(tileX, imgY, (tileWidth - coverWidth) / 2, RoundedRaffMetrics::values.homeCoverHeight,
                            Color::LightGray);
    renderer.fillRectDither(tileX + (tileWidth + coverWidth) / 2, imgY, (tileWidth - coverWidth) / 2,
                            RoundedRaffMetrics::values.homeCoverHeight, Color::LightGray);
    renderer.fillRoundedRect(tileX, imgY + RoundedRaffMetrics::values.homeCoverHeight, tileWidth,
                             tileHeight - (imgY - tileY + RoundedRaffMetrics::values.homeCoverHeight), kRowRadius,
                             false, false, true, true, Color::LightGray);
  } else {
    renderer.fillRoundedRect(tileX, tileY, tileWidth, tileHeight, kRowRadius, Color::LightGray);
    renderer.drawCenteredText(kTitleFontId, rect.y + rect.height / 2 - renderer.getLineHeight(kTitleFontId) / 2,
                              tr(STR_NO_OPEN_BOOK));
  }
}

int RoundedRaffTheme::getMenuRowHeight(const GfxRenderer& renderer) const {
  return renderer.getLineHeight(kTitleFontId) + 15;
}

void RoundedRaffTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                                      const std::function<const char*(int index)>& buttonLabel,
                                      const std::function<UIIcon(int index)>& rowIcon) const {
  (void)rowIcon;
  const int sidePadding = kHomeMenuSidePadding;
  const int rowX = rect.x + sidePadding;
  const int rowHeight = getMenuRowHeight(renderer);
  const int rowGap = kSelectableRowGap;
  const int rowStep = rowHeight + rowGap;
  const int pageItems = std::max(1, rect.height / rowStep);
  const int safeSelectedIndex = std::max(0, selectedIndex);
  const int pageStartIndex = (safeSelectedIndex / pageItems) * pageItems;
  const int menuTop = rect.y;
  const int textLineHeight = renderer.getLineHeight(kTitleFontId);
  const int menuMaxWidth = std::max(0, rect.width - sidePadding * 2);

  for (int i = pageStartIndex; i < buttonCount && i < pageStartIndex + pageItems; ++i) {
    const char* label = buttonLabel != nullptr ? buttonLabel(i) : "";
    if (!label) label = "";
    const int rowY = menuTop + (i - pageStartIndex) * rowStep;
    constexpr int kRowPaddingX = 30;  // 20px L/R
    const int maxLabelWidth = std::max(0, menuMaxWidth - kRowPaddingX);
    const std::string truncatedLabel = renderer.truncatedText(kTitleFontId, label, maxLabelWidth, EpdFontFamily::BOLD);
    const int rowWidth = std::min(
        menuMaxWidth, renderer.getTextWidth(kTitleFontId, truncatedLabel.c_str(), EpdFontFamily::BOLD) + kRowPaddingX);
    const bool isSelected = selectedIndex == i;
    const Rect rowRect{rowX, rowY, menuMaxWidth, rowHeight};
    TouchRegistry::getInstance().add(buttonMenuTouchTarget(rowRect, rect, i == buttonCount - 1, rowGap), i,
                                     TouchRegistry::Item);
    renderer.fillRoundedRect(rowX, rowY, rowWidth, rowHeight, kMenuRadius, isSelected ? Color::Black : Color::White);
    const int textY = rowY + (rowHeight - textLineHeight) / 2;
    const int textX = rowX + kInteractiveInsetX;
    if (selectedIndex == i) {
      renderer.drawText(kTitleFontId, textX, textY, truncatedLabel.c_str(), false, EpdFontFamily::BOLD);
    } else {
      renderer.drawText(kTitleFontId, textX, textY, truncatedLabel.c_str(), true, EpdFontFamily::BOLD);
    }
  }

  drawScrollBar(renderer, rect, buttonCount, pageStartIndex, pageItems);
}

void RoundedRaffTheme::drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode,
                                     int contentStartX, int contentWidth) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineY = rect.y + rect.height + lineHeight + metrics.verticalSpacing;
  const int thickness = cursorMode ? 3 : 2;

  if (contentWidth > 0) {
    renderer.drawLine(rect.x + contentStartX, lineY, rect.x + contentStartX + contentWidth - 1, lineY, thickness, true);
    return;
  }

  constexpr int hPadding = 8;
  const int lineW = textWidth + hPadding * 2;
  const int lineStart = rect.x + (rect.width - lineW) / 2;
  renderer.drawLine(lineStart, lineY, lineStart + lineW - 1, lineY, thickness, true);
}

int RoundedRaffTheme::getListRowStep(bool hasSubtitle, const int rowHeightScale) const {
  const int rowScale = std::max(1, rowHeightScale);
  const int rowHeight =
      (hasSubtitle ? RoundedRaffMetrics::values.listWithSubtitleRowHeight : RoundedRaffMetrics::values.listRowHeight) *
      rowScale;
  return rowHeight + kSelectableRowGap;
}

int RoundedRaffTheme::getListPageItems(int contentHeight, bool hasSubtitle, const int rowHeightScale) const {
  return std::max(1, contentHeight / getListRowStep(hasSubtitle, rowHeightScale));
}

void RoundedRaffTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                                const std::function<std::string(int index)>& rowTitle,
                                const std::function<std::string(int index)>& rowSubtitle,
                                const std::function<UIIcon(int index)>& rowIcon,
                                const std::function<std::string(int index)>& rowValue, bool highlightValue,
                                const std::function<bool(int index)>& rowDimmed,
                                const std::function<bool(int index)>& isHeader, const int rowHeightScale,
                                const bool showSelection) const {
  (void)rowIcon;
  (void)highlightValue;
  (void)rowDimmed;
  const int rowScale = std::max(1, rowHeightScale);
  const bool hasSubtitle = static_cast<bool>(rowSubtitle);
  const int titleLineHeight = renderer.getLineHeight(kTitleFontId);
  const int subtitleLineHeight = renderer.getLineHeight(kSubtitleFontId);
  constexpr int subtitleTopPadding = 10;
  constexpr int subtitleBottomPadding = 10;
  constexpr int subtitleInterLineGap = 4;
  const int subtitleRowHeight =
      subtitleTopPadding + titleLineHeight + subtitleInterLineGap + subtitleLineHeight + subtitleBottomPadding;
  const int rowHeight = (hasSubtitle ? subtitleRowHeight : RoundedRaffMetrics::values.listRowHeight) * rowScale;
  const auto isHeaderRow = [&isHeader](int index) { return isHeader != nullptr && isHeader(index); };
  bool hasHeaderRows = false;
  for (int i = 0; i < itemCount; ++i) {
    if (isHeaderRow(i)) {
      hasHeaderRows = true;
      break;
    }
  }
  const int sectionHeaderTopPadding = halTiltSensor.isAvailable() ? 10 : 20;
  constexpr int sectionHeaderFontId = kTitleFontId;
  constexpr int sectionHeaderUnderlineGap = 4;
  const int sectionHeaderLineHeight = renderer.getLineHeight(sectionHeaderFontId);
  const int sectionHeaderRowHeight = sectionHeaderLineHeight + sectionHeaderUnderlineGap;
  const int selectableRowGap = hasHeaderRows ? 0 : kSelectableRowGap;
  const auto visualRowHeight = [&](int index) { return isHeaderRow(index) ? sectionHeaderRowHeight : rowHeight; };
  int totalContentHeight = 0;
  for (int i = 0; i < itemCount; ++i) {
    if (i > 0) totalContentHeight += selectableRowGap;
    if (i > 0 && isHeaderRow(i)) totalContentHeight += sectionHeaderTopPadding;
    totalContentHeight += visualRowHeight(i);
  }
  const bool contentFits = totalContentHeight <= rect.height;
  const int rowStep = rowHeight + selectableRowGap;
  const int pageItems = contentFits ? std::max(1, itemCount) : std::max(1, rect.height / rowStep);
  const int pageStartIndex = std::max(0, selectedIndex / pageItems) * pageItems;

  const int sidePadding = kListSidePadding;
  const int rowX = rect.x + sidePadding;
  const int rowWidth = rect.width - sidePadding * 2;

  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    int rowY = rect.y;
    for (int j = pageStartIndex; j < i; ++j) {
      rowY += visualRowHeight(j) + selectableRowGap;
      if (isHeaderRow(j + 1)) rowY += sectionHeaderTopPadding;
    }
    const bool isSelected = showSelection && i == selectedIndex;
    const int currentRowHeight = visualRowHeight(i);

    if (isHeaderRow(i)) {
      std::string label = rowTitle(i);
      std::transform(label.begin(), label.end(), label.begin(),
                     [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
      const auto title = renderer.truncatedText(sectionHeaderFontId, label.c_str(), rowWidth - kInteractiveInsetX * 2,
                                                EpdFontFamily::BOLD);
      const int textY = rowY;
      renderer.drawText(sectionHeaderFontId, rowX + kInteractiveInsetX, textY, title.c_str(), true,
                        EpdFontFamily::BOLD);
      renderer.drawLine(rowX, rowY + currentRowHeight - 1, rowX + rowWidth, rowY + currentRowHeight - 1, true);
      continue;
    }
    TouchRegistry::getInstance().add(Rect{rowX, rowY, rowWidth, currentRowHeight}, i, TouchRegistry::Item);

    renderer.fillRoundedRect(rowX, rowY, rowWidth, currentRowHeight, kRowRadius,
                             isSelected ? Color::Black : Color::White);

    constexpr int kMinTitleWidth = 40;
    constexpr int kMinValueGap = kInteractiveInsetX;
    int textAreaWidth = rowWidth - kInteractiveInsetX * 2;
    if (rowValue) {
      std::string valueText = rowValue(i);
      if (!valueText.empty()) {
        const int maxValueWidth = std::max(0, rowWidth - kInteractiveInsetX * 2 - kMinValueGap - kMinTitleWidth);
        if (maxValueWidth > 0) {
          const std::string truncatedValue =
              renderer.truncatedText(kTitleFontId, valueText.c_str(), maxValueWidth, EpdFontFamily::REGULAR);
          const int valueW = renderer.getTextWidth(kTitleFontId, truncatedValue.c_str(), EpdFontFamily::REGULAR);
          renderer.drawText(kTitleFontId, rowX + rowWidth - kInteractiveInsetX - valueW,
                            rowY + (currentRowHeight - renderer.getLineHeight(kTitleFontId)) / 2,
                            truncatedValue.c_str(), !isSelected, EpdFontFamily::REGULAR);
          textAreaWidth = std::max(0, textAreaWidth - valueW - kMinValueGap);
        }
      }
    }

    if (hasSubtitle) {
      const std::string subtitleRaw = rowSubtitle(i);
      auto title = renderer.truncatedText(kTitleFontId, rowTitle(i).c_str(), textAreaWidth, EpdFontFamily::BOLD);

      if (subtitleRaw.empty()) {
        // If there is no subtitle/author, center title vertically in the full row.
        const int centeredTitleY = rowY + (currentRowHeight - titleLineHeight) / 2;
        renderer.drawText(kTitleFontId, rowX + kInteractiveInsetX, centeredTitleY, title.c_str(), !isSelected,
                          EpdFontFamily::BOLD);
      } else {
        const int titleY = rowY + subtitleTopPadding;
        const int subtitleY = titleY + titleLineHeight + subtitleInterLineGap;
        auto subtitle =
            renderer.truncatedText(kSubtitleFontId, subtitleRaw.c_str(), textAreaWidth, EpdFontFamily::REGULAR);
        renderer.drawText(kTitleFontId, rowX + kInteractiveInsetX, titleY, title.c_str(), !isSelected,
                          EpdFontFamily::BOLD);
        renderer.drawText(kSubtitleFontId, rowX + kInteractiveInsetX, subtitleY, subtitle.c_str(), !isSelected,
                          EpdFontFamily::REGULAR);
      }
    } else {
      auto title = renderer.truncatedText(kTitleFontId, rowTitle(i).c_str(), textAreaWidth, EpdFontFamily::BOLD);
      renderer.drawText(kTitleFontId, rowX + kInteractiveInsetX,
                        rowY + (currentRowHeight - renderer.getLineHeight(kTitleFontId)) / 2, title.c_str(),
                        !isSelected, EpdFontFamily::BOLD);
    }
  }

  drawScrollBar(renderer, rect, itemCount, pageStartIndex, pageItems);
}

void RoundedRaffTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                       const char* btn4, const bool allowInvertedText) const {
  if (gpio.hasTouch()) return;

  const GfxRenderer::Orientation origOrientation = renderer.getOrientation();
  const bool invertText = allowInvertedText && origOrientation == GfxRenderer::Orientation::PortraitInverted;
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int sidePadding = std::max(
      20, static_cast<int>(std::max(renderer.getViewableInsets().edges[1], renderer.getViewableInsets().edges[3])));
  const int groupGap = 10;
  const int bottomMargin = 10 + UITheme::getButtonHintsBottomInset(renderer);
  const int hintHeight = RoundedRaffMetrics::values.buttonHintsHeight - 10;  // 30px total guide height
  const int groupWidth = (pageWidth - sidePadding * 2 - groupGap) / 2;
  const int outlineY = pageHeight - hintHeight - bottomMargin;

  const char* leftOuterLabel = invertText ? btn4 : btn1;
  const char* leftInnerLabel = invertText ? btn3 : btn2;
  const char* rightInnerLabel = invertText ? btn2 : btn3;
  const char* rightOuterLabel = invertText ? btn1 : btn4;

  const bool backDisabled = (leftOuterLabel == nullptr || leftOuterLabel[0] == '\0');
  const int leftGroupX = sidePadding;
  const int rightGroupX = leftGroupX + groupWidth + groupGap;
  if (!backDisabled) {
    TouchRegistry::getInstance().add(Rect{leftGroupX, outlineY, groupWidth / 2, hintHeight}, HalGPIO::BTN_BACK,
                                     TouchRegistry::Button);
  }
  const std::string backLabel = backDisabled ? "" : std::string(leftOuterLabel);
  // Callers should provide the button labels. If a label is not specified, it should render empty.
  const std::string selectText = (leftInnerLabel && leftInnerLabel[0] != '\0') ? std::string(leftInnerLabel) : "";
  const std::string upText = (rightInnerLabel && rightInnerLabel[0] != '\0') ? std::string(rightInnerLabel) : "";
  const std::string downText = (rightOuterLabel && rightOuterLabel[0] != '\0') ? std::string(rightOuterLabel) : "";
  if (!selectText.empty()) {
    TouchRegistry::getInstance().add(
        Rect{leftGroupX + groupWidth / 2, outlineY, groupWidth - groupWidth / 2, hintHeight}, HalGPIO::BTN_CONFIRM,
        TouchRegistry::Button);
  }
  if (!upText.empty()) {
    TouchRegistry::getInstance().add(Rect{rightGroupX, outlineY, groupWidth / 2, hintHeight}, HalGPIO::BTN_LEFT,
                                     TouchRegistry::Button);
  }
  if (!downText.empty()) {
    TouchRegistry::getInstance().add(
        Rect{rightGroupX + groupWidth / 2, outlineY, groupWidth - groupWidth / 2, hintHeight}, HalGPIO::BTN_RIGHT,
        TouchRegistry::Button);
  }

  const int outerButtonWidth = groupWidth / 2;
  const int innerButtonWidth = groupWidth - outerButtonWidth;
  const auto drawHintGroup = [&renderer, outlineY, hintHeight](const int x, const int width) {
    // Ensure button hints always "win" visually even if other elements accidentally render into this area.
    renderer.fillRect(x, outlineY, width, hintHeight, false);
    renderer.drawRoundedRect(x, outlineY, width, hintHeight, 2, kBottomRadius, true);
  };
  const auto drawGroupedHints = [&drawHintGroup, outerButtonWidth, innerButtonWidth](
                                    const int groupX, const char* outerLabel, const char* innerLabel) {
    if (outerLabel != nullptr && innerLabel != nullptr) {
      drawHintGroup(groupX, outerButtonWidth + innerButtonWidth);
    } else if (outerLabel != nullptr) {
      drawHintGroup(groupX, outerButtonWidth);
    } else if (innerLabel != nullptr) {
      drawHintGroup(groupX + outerButtonWidth, innerButtonWidth);
    }
  };

  // A nullptr means the caller intentionally wants the reader pixels retained.
  // Empty strings still render an empty group so old hint pixels are cleared.
  drawGroupedHints(leftGroupX, leftOuterLabel, leftInnerLabel);
  drawGroupedHints(rightGroupX, rightInnerLabel, rightOuterLabel);

  const int selectWidth = renderer.getTextWidth(kGuideFontId, selectText.c_str(), EpdFontFamily::REGULAR);
  const int upWidth = renderer.getTextWidth(kGuideFontId, upText.c_str(), EpdFontFamily::REGULAR);
  const int downWidth = renderer.getTextWidth(kGuideFontId, downText.c_str(), EpdFontFamily::REGULAR);
  constexpr int innerEdgePadding = 16;

  const int backX = leftGroupX + innerEdgePadding;
  const int selectX = leftOuterLabel == nullptr ? leftGroupX + outerButtonWidth + (innerButtonWidth - selectWidth) / 2
                                                : leftGroupX + groupWidth - innerEdgePadding - selectWidth;
  const int upX =
      rightOuterLabel == nullptr ? rightGroupX + (outerButtonWidth - upWidth) / 2 : rightGroupX + innerEdgePadding;
  const int downX = rightInnerLabel == nullptr ? rightGroupX + outerButtonWidth + (innerButtonWidth - downWidth) / 2
                                               : rightGroupX + groupWidth - innerEdgePadding - downWidth;

  renderer.setOrientation(invertText ? GfxRenderer::Orientation::PortraitInverted : GfxRenderer::Orientation::Portrait);
  const int textY = (invertText ? bottomMargin : outlineY) + (hintHeight - renderer.getLineHeight(kGuideFontId)) / 2;

  if (!backDisabled) {
    renderer.drawText(kGuideFontId, backX, textY, backLabel.c_str(), true, EpdFontFamily::REGULAR);
  }
  renderer.drawText(kGuideFontId, selectX, textY, selectText.c_str(), true, EpdFontFamily::REGULAR);

  renderer.drawText(kGuideFontId, upX, textY, upText.c_str(), true, EpdFontFamily::REGULAR);
  renderer.drawText(kGuideFontId, downX, textY, downText.c_str(), true, EpdFontFamily::REGULAR);

  renderer.setOrientation(origOrientation);
}
