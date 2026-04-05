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

  // Header with "Reading Stats" title only
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, screenWidth, metrics.headerHeight}, tr(STR_READING_STATS));

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  // Book title: word-wrapped up to 2 lines, centered
  const int titleMaxWidth = screenWidth - metrics.contentSidePadding * 2;
  const auto titleLines = renderer.wrappedText(UI_10_FONT_ID, bookTitle.c_str(), titleMaxWidth, 2);
  const int titleLineH = renderer.getLineHeight(UI_10_FONT_ID);
  for (const auto& line : titleLines) {
    renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str(), true);
    y += titleLineH;
  }
  y += metrics.verticalSpacing;

  // Pre-compute line heights used in the stat cells
  const int valueLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int labelLineH = renderer.getLineHeight(SMALL_FONT_ID);

  constexpr int sectionHeaderH = 34;
  constexpr int cellH = 100;
  constexpr int labelPad = 6;

  // Draws a stat cell: large bold value + small label, both horizontally centered.
  // Captures y, cellH, valueLineH, labelLineH, labelPad by reference.
  auto drawStatCell = [&](int x, int w, const char* value, const char* label) {
    const int totalTextH = valueLineH + labelPad + labelLineH;
    const int textY = y + (cellH - totalTextH) / 2;

    const int vw = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, x + (w - vw) / 2, textY, value, true, EpdFontFamily::BOLD);

    const int lw = renderer.getTextWidth(SMALL_FONT_ID, label);
    renderer.drawText(SMALL_FONT_ID, x + (w - lw) / 2, textY + valueLineH + labelPad, label, true);
  };

  char buf[32];

  // ─── Section 1: "THIS BOOK" ────────────────────────────────────────────────
  renderer.fillRect(0, y, screenWidth, sectionHeaderH);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y + 9, tr(STR_STATS_THIS_BOOK), false,
                    EpdFontFamily::BOLD);
  y += sectionHeaderH;

  const int thirdW = screenWidth / 3;

  snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(stats.sessionCount));
  drawStatCell(0, thirdW, buf, tr(STR_STATS_SESSIONS_LBL));
  renderer.drawLine(thirdW, y + 12, thirdW, y + cellH - 12, true);

  BookReadingStats::formatDuration(stats.totalReadingSeconds, buf, sizeof(buf));
  drawStatCell(thirdW, thirdW, buf, tr(STR_STATS_TIME_LBL));
  renderer.drawLine(thirdW * 2, y + 12, thirdW * 2, y + cellH - 12, true);

  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(stats.totalPagesTurned));
  drawStatCell(thirdW * 2, thirdW, buf, tr(STR_STATS_PAGES_LBL));

  y += cellH;
  renderer.drawLine(0, y, screenWidth, y, true);
  y += metrics.verticalSpacing;

  // ─── Section 2: "AVERAGES" ─────────────────────────────────────────────────
  renderer.fillRect(0, y, screenWidth, sectionHeaderH);
  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y + 9, tr(STR_STATS_AVERAGES), false,
                    EpdFontFamily::BOLD);
  y += sectionHeaderH;

  const int halfW = screenWidth / 2;

  const uint32_t avgSecs = stats.sessionCount > 0 ? stats.totalReadingSeconds / stats.sessionCount : 0;
  BookReadingStats::formatDuration(avgSecs, buf, sizeof(buf));
  drawStatCell(0, halfW, buf, tr(STR_STATS_AVG_SESSION_LBL));
  renderer.drawLine(halfW, y + 12, halfW, y + cellH - 12, true);

  if (stats.totalReadingSeconds > 60) {
    const float ppm =
        static_cast<float>(stats.totalPagesTurned) * 60.0f / static_cast<float>(stats.totalReadingSeconds);
    snprintf(buf, sizeof(buf), "%.1f", ppm);
  } else {
    snprintf(buf, sizeof(buf), "0.0");
  }
  drawStatCell(halfW, halfW, buf, tr(STR_STATS_PAGES_PER_MIN));

  y += cellH;
  renderer.drawLine(0, y, screenWidth, y, true);
  y += metrics.verticalSpacing;

  // ─── Section 3: "ALL TIME" ─────────────────────────────────────────────────
  // Only rendered if there is enough vertical space before the button hints.
  const int screenHeight = renderer.getScreenHeight();
  const int spaceNeeded = sectionHeaderH + cellH + 1;
  const int spaceAvailable = screenHeight - y - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (spaceAvailable >= spaceNeeded) {
    renderer.fillRect(0, y, screenWidth, sectionHeaderH);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y + 9, tr(STR_STATS_ALL_TIME), false,
                      EpdFontFamily::BOLD);
    y += sectionHeaderH;

    snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(globalStats.totalSessions));
    drawStatCell(0, thirdW, buf, tr(STR_STATS_SESSIONS_LBL));
    renderer.drawLine(thirdW, y + 12, thirdW, y + cellH - 12, true);

    BookReadingStats::formatDuration(globalStats.totalReadingSeconds, buf, sizeof(buf));
    drawStatCell(thirdW, thirdW, buf, tr(STR_STATS_TIME_LBL));
    renderer.drawLine(thirdW * 2, y + 12, thirdW * 2, y + cellH - 12, true);

    snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(globalStats.totalPagesTurned));
    drawStatCell(thirdW * 2, thirdW, buf, tr(STR_STATS_PAGES_LBL));

    y += cellH;
    renderer.drawLine(0, y, screenWidth, y, true);
  }

  // Button hint
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
