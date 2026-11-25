#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

namespace RoundedRaffMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 15,
                                 .batteryHeight = 12,
                                 .topPadding = 0,
                                 .batteryBarHeight = 45,
                                 .headerHeight = 70,
                                 .verticalSpacing = 10,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 15,
                                 .listRowHeight = 42,
                                 .listWithSubtitleRowHeight = 69,
                                 .listRowGap = 6,
                                 .listRowRadius = 20,
                                 .listInset = 20,
                                 .listSidePadding = 20,
                                 .listSelectionStyle = 0,
                                 .listScrollWidth = 4,
                                 .listScrollSide = 0,
                                 .listTitleBold = true,
                                 .headerSidePadding = 18,
                                 .headerUnderlineSize = 0,
                                 .headerTitleAlign = 0,
                                 .headerBatterySide = 0,
                                 .headerBatteryDetached = true,
                                 .menuRowHeight = 42,
                                 .menuSpacing = 6,
                                 .tabSpacing = 10,
                                 .tabBarHeight = 50,
                                 .tabBarAppearance = ThemeTabBarAppearance::Pill,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 75,
                                 // Compact cover tile so the full roundedraff home menu fits above button hints.
                                 .homeCoverHeight = 260,
                                 .homeCoverTileHeight = 300,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = true,
                                 .homeMenuTopOffset = 20,
                                 .buttonHintsHeight = 40,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyHeight = 56,
                                 .keyboardKeySpacing = 10,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = 0,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 94,
                                 .popupTopOffsetRatio = 0.12f,
                                 .popupMarginX = 20,
                                 .popupMarginY = 14,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 18,
                                 .popupTextBold = true,
                                 .popupTextInverted = false,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = true,
                                 .popupProgressClampPercent = true,
                                 .popupProgressFillInverted = false,
                                 .popupProgressOutlineInverted = false,
                                 .optionPopupItemSpacing = 6,
                                 .optionPopupInnerPadding = 24,
                                 .optionPopupSelectionHPadding = 20,
                                 .optionPopupSelectionVPadding = 10,
                                 .optionPopupTitleGap = 16,
                                 .optionPopupUseSmallFont = false,
                                 .optionPopupOptionFontBold = true,
                                 .optionPopupSelectionRadius = 30,
                                 .optionPopupSelectionLight = false,
                                 .optionPopupDrawAllRows = true,
                                 .optionPopupDialogSideMargin = 20,
                                 .optionPopupTitleSeparator = true,
                                 .textFieldHorizontalPadding = 8,
                                 .textFieldNormalThickness = 2,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = -1};
}

class RoundedRaffTheme : public BaseTheme {
 public:
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle = nullptr,
                  bool readerContext = false, bool showStatus = true, bool filenameTitle = false) const override;
  void drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                  bool selected) const override;
  bool tabIndexFromPoint(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs, int x, int y,
                         int& index) const override;
  void drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                           int selectorIndex, bool& coverRendered, bool& coverBufferStored, bool& bufferRestored,
                           const std::function<bool()>& storeCoverBuffer, const BookReadingStats* stats = nullptr,
                           float progressPercent = -1.0f, const GlobalReadingStats* globalStats = nullptr,
                           const char* currentChapterTitle = nullptr) const override;
  int getMenuRowHeight(const GfxRenderer& renderer) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<const char*(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  void drawTextField(const GfxRenderer& renderer, Rect rect, int textWidth, bool cursorMode = false,
                     int contentStartX = 0, int contentWidth = 0) const override;
  int getListRowStep(bool hasSubtitle, int rowHeightScale = 1) const override;
  int getListPageItems(int contentHeight, bool hasSubtitle, int rowHeightScale = 1) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle = nullptr,
                const std::function<UIIcon(int index)>& rowIcon = nullptr,
                const std::function<std::string(int index)>& rowValue = nullptr, bool highlightValue = false,
                const std::function<bool(int index)>& rowDimmed = nullptr,
                const std::function<bool(int index)>& isHeader = nullptr, int rowHeightScale = 1,
                bool showSelection = true) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3, const char* btn4,
                       bool allowInvertedText = false) const override;
  bool homeMenuShowsContinueReading() const { return true; }
};
