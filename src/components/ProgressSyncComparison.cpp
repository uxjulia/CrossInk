#include "ProgressSyncComparison.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "UITheme.h"
#include "fontIds.h"

namespace ProgressSyncComparison {

constexpr int RESULT_NON_TOUCH_ACTION_HEIGHT = 40;
constexpr int RESULT_NON_TOUCH_ACTION_GAP = 8;
constexpr int RESULT_CARD_PADDING = 12;
constexpr int RESULT_CARD_ROW_GAP = 6;
constexpr int RESULT_CARD_BAR_HEIGHT = 10;
constexpr int RESULT_BADGE_PADDING_X = 6;

// Progress in tenths of a percent, matching the card's "%.1f%%" label so the "Ahead" badge never
// disagrees with the numbers shown on screen.
long displayedTenths(const float percentage) { return std::lround(percentage * 1000.0f); }

// Apply/Upload buttons are pinned above the button hints (or the bottom edge on touch devices) so the
// progress cards can use the space above them. Rendering and hit testing share this layout.
TouchActionButtons::Layout actionLayout(const GfxRenderer& renderer, const Rect& screen, const ThemeMetrics& metrics,
                                        const bool hasTouch, const uint8_t buttonCount) {
  const int buttonHeight = hasTouch ? TouchActionButtons::kDefaultHeight : RESULT_NON_TOUCH_ACTION_HEIGHT;
  const int buttonGap = hasTouch ? TouchActionButtons::kDefaultGap : RESULT_NON_TOUCH_ACTION_GAP;
  const int reservedBottom =
      hasTouch ? metrics.verticalSpacing : UITheme::getButtonHintsReserve(renderer) + metrics.verticalSpacing;
  if (buttonCount == 3 && screen.width > screen.height) {
    // Landscape places the progress cards side by side, leaving room for two rows:
    // the two progress choices share the first row and Skip book takes the second.
    const int totalHeight = buttonHeight * 2 + buttonGap;
    TouchActionButtons::Layout layout;
    layout.container =
        Rect{screen.x + metrics.contentSidePadding, screen.y + screen.height - reservedBottom - totalHeight,
             std::max(1, screen.width - metrics.contentSidePadding * 2), totalHeight};
    layout.count = 3;
    const Rect& area = layout.container;
    const int half = std::max(1, (area.width - buttonGap) / 2);
    layout.buttons[0] = Rect{area.x, area.y, half, buttonHeight};
    layout.buttons[1] = Rect{area.x + area.width - half, area.y, half, buttonHeight};
    layout.buttons[2] = Rect{area.x, area.y + buttonHeight + buttonGap, area.width, buttonHeight};
    return layout;
  }
  const int totalHeight = buttonHeight * buttonCount + buttonGap * (buttonCount - 1);
  const Rect container{screen.x + metrics.contentSidePadding, screen.y + screen.height - reservedBottom - totalHeight,
                       std::max(1, screen.width - metrics.contentSidePadding * 2), totalHeight};
  return TouchActionButtons::vertical(container, buttonCount, buttonHeight, buttonGap);
}

int progressCardHeight(const GfxRenderer& renderer) {
  return RESULT_CARD_PADDING * 2 + renderer.getLineHeight(UI_10_FONT_ID) + RESULT_CARD_ROW_GAP +
         renderer.getLineHeight(UI_12_FONT_ID) + RESULT_CARD_ROW_GAP + RESULT_CARD_BAR_HEIGHT + RESULT_CARD_ROW_GAP +
         renderer.getLineHeight(UI_10_FONT_ID);
}

// Draws text so its baseline lines up with a larger font drawn at rowY.
void drawTextOnBaseline(const GfxRenderer& renderer, const int fontId, const int x, const int rowY, const int rowFontId,
                        const char* text, const bool black = true,
                        const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int y = rowY + renderer.getFontAscenderSize(rowFontId) - renderer.getFontAscenderSize(fontId);
  renderer.drawText(fontId, x, y, text, black, style);
}

// Card layout:
//   Title  source                  [Ahead]
//   42.5%                        Page 3/12
//   [=========                          ]
//   Chapter name...
void drawProgressCard(const GfxRenderer& renderer, const Rect& card, const Card& info) {
  renderer.drawRect(card.x, card.y, card.width, card.height, true);

  const int x = card.x + RESULT_CARD_PADDING;
  const int innerWidth = std::max(1, card.width - RESULT_CARD_PADDING * 2);
  const int right = x + innerWidth;
  const int smallGap = RESULT_CARD_ROW_GAP * 2;
  int y = card.y + RESULT_CARD_PADDING;

  // Row 1: title, optional source, optional "Ahead" badge.
  const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int titleRight = right;
  if (info.ahead) {
    const char* badge = tr(STR_SYNC_AHEAD);
    const int badgeWidth = renderer.getTextWidth(SMALL_FONT_ID, badge) + RESULT_BADGE_PADDING_X * 2;
    const int badgeX = right - badgeWidth;
    renderer.fillRect(badgeX, y, badgeWidth, titleLineHeight, true);
    const int badgeTextY = y + (titleLineHeight - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
    renderer.drawText(SMALL_FONT_ID, badgeX + RESULT_BADGE_PADDING_X, badgeTextY, badge, false);
    titleRight = badgeX - smallGap;
  }
  const std::string title =
      renderer.truncatedText(UI_10_FONT_ID, info.title, std::max(1, titleRight - x), EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, x, y, title.c_str(), true, EpdFontFamily::BOLD);
  if (info.source != nullptr && info.source[0] != '\0') {
    const int sourceX = x + renderer.getTextWidth(UI_10_FONT_ID, title.c_str(), EpdFontFamily::BOLD) + smallGap;
    if (titleRight - sourceX > 0) {
      const std::string source = renderer.truncatedText(SMALL_FONT_ID, info.source, titleRight - sourceX);
      drawTextOnBaseline(renderer, SMALL_FONT_ID, sourceX, y, UI_10_FONT_ID, source.c_str());
    }
  }
  y += titleLineHeight + RESULT_CARD_ROW_GAP;

  // Row 2: large percentage with the page position right-aligned on the same baseline.
  char percentStr[16];
  snprintf(percentStr, sizeof(percentStr), "%.1f%%", info.percentage * 100);
  renderer.drawText(UI_12_FONT_ID, x, y, percentStr, true, EpdFontFamily::BOLD);
  const int percentRight = x + renderer.getTextWidth(UI_12_FONT_ID, percentStr, EpdFontFamily::BOLD) + smallGap;
  if (right - percentRight > 0) {
    const std::string page = renderer.truncatedText(UI_10_FONT_ID, info.pageText, right - percentRight);
    const int pageX = right - renderer.getTextWidth(UI_10_FONT_ID, page.c_str());
    drawTextOnBaseline(renderer, UI_10_FONT_ID, pageX, y, UI_12_FONT_ID, page.c_str());
  }
  y += renderer.getLineHeight(UI_12_FONT_ID) + RESULT_CARD_ROW_GAP;

  // Row 3: progress bar.
  renderer.drawRect(x, y, innerWidth, RESULT_CARD_BAR_HEIGHT, true);
  const float clamped = std::min(1.0f, std::max(0.0f, info.percentage));
  const int fillWidth = static_cast<int>((innerWidth - 4) * clamped);
  if (fillWidth > 0) {
    renderer.fillRect(x + 2, y + 2, fillWidth, RESULT_CARD_BAR_HEIGHT - 4, true);
  }
  y += RESULT_CARD_BAR_HEIGHT + RESULT_CARD_ROW_GAP;

  // Row 4: chapter name.
  const std::string chapter = renderer.truncatedText(UI_10_FONT_ID, info.chapter, innerWidth);
  renderer.drawText(UI_10_FONT_ID, x, y, chapter.c_str());
}

void drawCards(const GfxRenderer& renderer, const Rect& screen, const ThemeMetrics& metrics, const int top,
               const Card& remote, const Card& local) {
  const int contentX = screen.x + metrics.contentSidePadding;
  const int contentWidth = std::max(1, screen.width - metrics.contentSidePadding * 2);
  const int cardHeight = progressCardHeight(renderer);
  const int cardGap = metrics.verticalSpacing;
  if (screen.width > screen.height) {
    const int cardWidth = std::max(1, (contentWidth - cardGap) / 2);
    drawProgressCard(renderer, Rect{contentX, top, cardWidth, cardHeight}, remote);
    drawProgressCard(renderer, Rect{contentX + contentWidth - cardWidth, top, cardWidth, cardHeight}, local);
  } else {
    drawProgressCard(renderer, Rect{contentX, top, contentWidth, cardHeight}, remote);
    drawProgressCard(renderer, Rect{contentX, top + cardHeight + cardGap, contentWidth, cardHeight}, local);
  }
}

}  // namespace ProgressSyncComparison
