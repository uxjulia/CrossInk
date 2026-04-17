#include "BookStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                     const BookReadingStats& stats, const GlobalReadingStats& globalStats)
    : Activity("BookStats", renderer, mappedInput), bookTitle(title), stats(stats), globalStats(globalStats) {}

void BookStatsActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void BookStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void BookStatsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenWidth = renderer.getScreenWidth();
  const int cardX = metrics.contentSidePadding;
  const int cardW = screenWidth - metrics.contentSidePadding * 2;
  const int thirdW = cardW / 3;
  const int halfW = cardW / 2;

  // Pass "" so drawHeader renders the battery + background + bottom line without a title,
  // then draw the chart icon + title text manually to produce the "<icon> Reading Stats" layout.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, screenWidth, metrics.headerHeight}, "");

  const int availableH = metrics.headerHeight - metrics.batteryBarHeight;
  const int titleX = metrics.contentSidePadding;
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int titleY = metrics.topPadding + metrics.batteryBarHeight + (availableH - lineHeight) / 2;
  const int batteryStartX = screenWidth - metrics.contentSidePadding - metrics.batteryWidth;
  const int maxTitleWidth = batteryStartX - titleX - metrics.contentSidePadding;
  const std::string truncTitle =
      renderer.truncatedText(UI_12_FONT_ID, tr(STR_READING_STATS), maxTitleWidth, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, titleX, titleY, truncTitle.c_str(), true, EpdFontFamily::BOLD);

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int titleLineH = renderer.getLineHeight(UI_10_FONT_ID);

  constexpr int cardTitleH = 40;
  constexpr int cellH = 90;
  constexpr int labelPad = 6;

  // Draws a stat cell: large bold value + small label, horizontally centered in (x, cellY, w, cellH).
  auto drawStatCell = [&](int x, int w, int cellY, const char* value, const char* label) {
    const int totalTextH = valueLineH + labelPad + labelLineH;
    const int textY = cellY + (cellH - totalTextH) / 2;

    const int vw = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, x + (w - vw) / 2, textY, value, true, EpdFontFamily::BOLD);

    const int lw = renderer.getTextWidth(SMALL_FONT_ID, label);
    renderer.drawText(SMALL_FONT_ID, x + (w - lw) / 2, textY + valueLineH + labelPad, label, true);
  };

  char buf[32];

  // ─── Card 1: Book stats ──────────────────────────────────────────────────────
  renderer.drawRect(cardX, y, cardW, cardTitleH + cellH * 2);

  {
    const auto lines =
        renderer.wrappedText(UI_10_FONT_ID, bookTitle.c_str(), cardW - metrics.contentSidePadding * 2, 1);
    if (!lines.empty()) {
      const int tw = renderer.getTextWidth(UI_10_FONT_ID, lines[0].c_str(), EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, cardX + (cardW - tw) / 2, y + (cardTitleH - titleLineH) / 2, lines[0].c_str(),
                        true, EpdFontFamily::BOLD);
    }
  }

  y += cardTitleH;
  renderer.drawLine(cardX, y, cardX + cardW, y);

  const int row1Y = y;
  snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(stats.sessionCount));
  drawStatCell(cardX, thirdW, row1Y, buf, tr(STR_STATS_SESSIONS_LBL));

  BookReadingStats::formatDuration(stats.totalReadingSeconds, buf, sizeof(buf));
  drawStatCell(cardX + thirdW, thirdW, row1Y, buf, tr(STR_STATS_TIME_LBL));

  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(stats.totalPagesTurned));
  drawStatCell(cardX + thirdW * 2, thirdW, row1Y, buf, tr(STR_STATS_PAGES_LBL));

  y += cellH;

  const int row2Y = y;
  const uint32_t avgSecs = stats.sessionCount > 0 ? stats.totalReadingSeconds / stats.sessionCount : 0;
  BookReadingStats::formatDuration(avgSecs, buf, sizeof(buf));
  drawStatCell(cardX, halfW, row2Y, buf, tr(STR_STATS_AVG_SESSION_LBL));

  if (stats.totalReadingSeconds > 60) {
    const float ppm =
        static_cast<float>(stats.totalPagesTurned) * 60.0f / static_cast<float>(stats.totalReadingSeconds);
    snprintf(buf, sizeof(buf), "%.1f", ppm);
  } else {
    snprintf(buf, sizeof(buf), "0.0");
  }
  drawStatCell(cardX + halfW, halfW, row2Y, buf, tr(STR_STATS_PAGES_PER_MIN));

  y += cellH;
  y += metrics.verticalSpacing;

  // ─── Card 2: All Books ───────────────────────────────────────────────────────
  // Only rendered if there is enough vertical space before the button hints.
  const int screenHeight = renderer.getScreenHeight();
  const int card2H = cardTitleH + cellH * 2;
  if (screenHeight - y - metrics.buttonHintsHeight - metrics.verticalSpacing >= card2H) {
    renderer.drawRect(cardX, y, cardW, card2H);

    const int tw = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_STATS_ALL_TIME), EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, cardX + (cardW - tw) / 2, y + (cardTitleH - titleLineH) / 2,
                      tr(STR_STATS_ALL_TIME), true, EpdFontFamily::BOLD);

    y += cardTitleH;
    renderer.drawLine(cardX, y, cardX + cardW, y);

    snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(globalStats.totalSessions));
    drawStatCell(cardX, thirdW, y, buf, tr(STR_STATS_SESSIONS_LBL));

    BookReadingStats::formatDuration(globalStats.totalReadingSeconds, buf, sizeof(buf));
    drawStatCell(cardX + thirdW, thirdW, y, buf, tr(STR_STATS_TIME_LBL));

    snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(globalStats.totalPagesTurned));
    drawStatCell(cardX + thirdW * 2, thirdW, y, buf, tr(STR_STATS_PAGES_LBL));

    y += cellH;

    const uint32_t globalAvgSecs =
        globalStats.totalSessions > 0 ? globalStats.totalReadingSeconds / globalStats.totalSessions : 0;
    BookReadingStats::formatDuration(globalAvgSecs, buf, sizeof(buf));
    drawStatCell(cardX, halfW, y, buf, tr(STR_STATS_AVG_SESSION_LBL));

    if (globalStats.totalReadingSeconds > 60) {
      const float ppm = static_cast<float>(globalStats.totalPagesTurned) * 60.0f /
                        static_cast<float>(globalStats.totalReadingSeconds);
      snprintf(buf, sizeof(buf), "%.1f", ppm);
    } else {
      snprintf(buf, sizeof(buf), "0.0");
    }
    drawStatCell(cardX + halfW, halfW, y, buf, tr(STR_STATS_PAGES_PER_MIN));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
