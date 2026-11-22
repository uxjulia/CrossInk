#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "CompactTableLayout.h"
#include "GfxRenderer.h"
#include "TableColumnLayout.h"
#include "TableLayoutPolicy.h"
#include "TableTextLineOrder.h"

namespace {

size_t countOccurrences(const std::string& text, const std::string& needle) {
  size_t count = 0;
  for (size_t offset = text.find(needle); offset != std::string::npos; offset = text.find(needle, offset + 1)) {
    ++count;
  }
  return count;
}

BlockStyle leftStyle() {
  BlockStyle style;
  style.alignment = CssTextAlign::Left;
  style.isRtl = false;
  return style;
}

CompactTableLayout::RowResult finishAndDrain(CompactTableLayout& layout, TableFragmentRow& row,
                                             std::vector<std::shared_ptr<TextBlock>>& flat,
                                             std::vector<FootnoteEntry>& notes, uint32_t& offset) {
  flat.clear();
  using Result = CompactTableLayout::RowResult;
  const auto result = layout.finishRow(row, notes, offset);
  if (result == Result::Flatten || result == Result::FlattenRow) {
    if (!layout.visitCapturedParagraphs(
            [](void* context, std::shared_ptr<TextBlock> line, uint32_t) {
              static_cast<std::vector<std::shared_ptr<TextBlock>>*>(context)->push_back(std::move(line));
              return true;
            },
            &flat)) {
      return Result::Abort;
    }
  }
  return result;
}

TEST(CompactTableLayoutTest, C3AlwaysUsesCompactButOtherTargetsKeepBothHeapGates) {
  constexpr unsigned KiB = 1024;
  EXPECT_TRUE(TableLayoutPolicy::useCompact(true, 150 * KiB, 120 * KiB));
  EXPECT_TRUE(TableLayoutPolicy::useCompact(true, 85 * KiB, 49 * KiB));
  EXPECT_FALSE(TableLayoutPolicy::useCompact(false, 96 * KiB, 56 * KiB));
  EXPECT_TRUE(TableLayoutPolicy::useCompact(false, 96 * KiB - 1, 56 * KiB));
  EXPECT_TRUE(TableLayoutPolicy::useCompact(false, 96 * KiB, 56 * KiB - 1));
}

TEST(CompactTableLayoutTest, SourceFixtureModelsLargeAndUnsupportedTables) {
  std::ifstream fixture(COMPACT_TABLE_FIXTURE_PATH);
  ASSERT_TRUE(fixture.good());
  const std::string source((std::istreambuf_iterator<char>(fixture)), std::istreambuf_iterator<char>());
  const size_t gridStart = source.find("<table class=\"compact-grid\">");
  const size_t gridEnd = source.find("</table>", gridStart);
  ASSERT_NE(gridStart, std::string::npos);
  ASSERT_NE(gridEnd, std::string::npos);
  const std::string grid = source.substr(gridStart, gridEnd - gridStart);
  const size_t captionStart = grid.find("<caption>");
  const size_t firstRowStart = grid.find("<tr>");

  ASSERT_NE(captionStart, std::string::npos);
  ASSERT_NE(firstRowStart, std::string::npos);
  EXPECT_LT(captionStart, firstRowStart);
  EXPECT_NE(grid.find("Table 2-1: Compact table caption"), std::string::npos);
  EXPECT_EQ(countOccurrences(grid, "<tr>"), 33u);  // group heading + header + 31 data rows
  EXPECT_EQ(countOccurrences(grid, "<td"), 31u * 8u);
  EXPECT_EQ(countOccurrences(grid, "<th"), 9u);  // eight headers plus the full-width heading
  EXPECT_NE(source.find("class=\"compact-grid\""), std::string::npos);
  EXPECT_NE(source.find("colspan=\"8\""), std::string::npos);
  EXPECT_NE(source.find("rowspan=\"2\""), std::string::npos);
  EXPECT_NE(source.find("<th"), std::string::npos);
  EXPECT_NE(source.find("text-align: center"), std::string::npos);
  EXPECT_NE(source.find("direction: rtl"), std::string::npos);
  EXPECT_NE(source.find("<strong>"), std::string::npos);
  EXPECT_NE(source.find("nested-table"), std::string::npos);
  EXPECT_NE(source.find("too-wide"), std::string::npos);
  EXPECT_NE(source.find("长"), std::string::npos);
}

TEST(CompactTableLayoutTest, IntrinsicPlanKeepsNumericalColumnsCompactAndHonorsViewport) {
  TableColumnLayout::Measures measure;
  measure.count = 3;
  measure.minimum = {85, 24, 22, 0};
  measure.preferred = {210, 44, 35, 0};
  for (const uint16_t width : {uint16_t{320}, uint16_t{200}, uint16_t{100}}) {
    const auto plan = TableColumnLayout::proportionalPlan(measure, width, 4);
    ASSERT_EQ(plan.count, 3);
    EXPECT_GT(plan.ends[0], width / 3);
    EXPECT_GE(plan.ends[0], 28);
    EXPECT_GE(plan.ends[1] - plan.ends[0], 28);
    EXPECT_GE(plan.ends[2] - plan.ends[1], 28);
    EXPECT_EQ(plan.ends[2], width);
  }
  EXPECT_EQ(TableColumnLayout::proportionalPlan(measure, 83, 4).count, 0);
}

TableColumnLayout::Measures probe(std::string_view input) {
  return TableColumnLayout::measureSimpleTable(
      [&](char* buffer, const unsigned count) {
        const auto n = std::min<size_t>({input.size(), count, 7});
        if (n == 0) return 0;
        std::copy_n(input.begin(), n, buffer);
        input.remove_prefix(n);
        return static_cast<int>(n);
      },
      [](const char* token, const bool bold) {
        return static_cast<int>(std::string_view(token).size()) * (bold ? 7 : 6);
      },
      480, 4);
}

TEST(CompactTableLayoutTest, RawProbeIgnoresSpanningNotesAndStopsAtTheFirstTable) {
  const auto measure = probe(
      "<table><caption>Table 2</caption><thead><tr><th></th><th>ΔRSS</th><th>p</th></tr></thead>"
      "<tbody><tr><td>A wider community label</td><td>0.224</td><td>0.009</td></tr>"
      "<tr><td colspan='3'>An unrelated long note won't change the column widths</td></tr></tbody></table>"
      "<table><tr><td>Other</td><td>Table</td></tr></table>");
  EXPECT_EQ(measure.count, 3);
  EXPECT_EQ(measure.status, TableColumnLayout::Measures::ProbeStatus::Accepted);
  EXPECT_GT(measure.preferred[0], measure.preferred[1]);
  EXPECT_EQ(measure.preferred[2], 30);
  EXPECT_EQ(probe("<table><tr><td>a</td><td>b</td></tr><tr><td>c</td></tr></table>").count, 0);
  EXPECT_EQ(probe("<table><tr><td><table></table></td><td>b</td></tr></table>").count, 0);
  EXPECT_EQ(probe("<table><tr><td>" + std::string(45, 'x') + "</td><td>b</td></tr></table>").count, 0);
  EXPECT_EQ(probe("<table><caption>" + std::string(32768, 'x') + "</caption></table>").count, 0);
  std::string manyWords;
  for (unsigned i = 0; i < 400; ++i) manyWords += "x ";
  const auto tooManyWords = probe("<table><tr><td>" + manyWords + "</td><td>b</td></tr></table>");
  EXPECT_EQ(tooManyWords.count, 0);
  EXPECT_EQ(tooManyWords.status, TableColumnLayout::Measures::ProbeStatus::WordLimit);
}

TEST(CompactTableLayoutTest, LookaheadUsesLargerBoundedReadBatches) {
  std::string input = "<table><caption>" + std::string(32768, 'x') + "</caption></table>";
  size_t offset = 0;
  unsigned reads = 0;
  unsigned largestRequest = 0;
  const auto measure = TableColumnLayout::measureSimpleTable(
      [&](char* buffer, const unsigned count) {
        ++reads;
        largestRequest = std::max(largestRequest, count);
        const auto n = std::min(input.size() - offset, static_cast<size_t>(count));
        if (n == 0) return 0;
        std::copy_n(input.data() + offset, n, buffer);
        offset += n;
        return static_cast<int>(n);
      },
      [](const char*, const bool) { return 0; }, 480, 4);
  EXPECT_EQ(measure.count, 0);
  EXPECT_EQ(measure.status, TableColumnLayout::Measures::ProbeStatus::ScanLimit);
  EXPECT_EQ(measure.bytesScanned, 32768u);
  EXPECT_EQ(largestRequest, 256u);
  EXPECT_EQ(reads, 128u);
}

TEST(CompactTableLayoutTest, ProbeReportsStorageReadFailureSeparately) {
  const auto measure = TableColumnLayout::measureSimpleTable([](char*, const unsigned) { return -1; },
                                                             [](const char*, const bool) { return 0; }, 480, 4);
  EXPECT_EQ(measure.status, TableColumnLayout::Measures::ProbeStatus::ReadFailure);
  EXPECT_EQ(measure.bytesScanned, 0u);
}

TEST(CompactTableLayoutTest, FinishesRowsFromFixedFootnoteStorage) {
  GfxRenderer renderer;
  CompactTableLayout layout(renderer, 0, 120, 240, 16, 4, leftStyle());
  ASSERT_TRUE(layout.beginRow());
  ASSERT_TRUE(layout.beginCell(false, 1, 41, leftStyle()));
  ASSERT_TRUE(layout.appendWord("linked", EpdFontFamily::REGULAR, false, false, 1));
  FootnoteEntry note;
  std::strcpy(note.number, "7");
  std::strcpy(note.href, "#note-7");
  note.linkId = 1;
  ASSERT_TRUE(layout.endCell({{0, note}}));

  TableFragmentRow row;
  uint32_t visibleOffset = 0;
  ASSERT_EQ(layout.finishRow(row, visibleOffset), CompactTableLayout::RowResult::Ok);
  ASSERT_EQ(layout.capturedFootnoteCount(), 1);
  EXPECT_STREQ(layout.capturedFootnote(0).number, "7");
  EXPECT_STREQ(layout.capturedFootnote(0).href, "#note-7");
  EXPECT_EQ(layout.capturedFootnote(0).linkId, 1);
}

TEST(CompactTableLayoutTest, PlannedGeometryCarriesThroughSpanningAndUnspannedRows) {
  GfxRenderer renderer;
  TableColumnLayout::Plan plan{{82, 122, 160, 0}, 3};
  CompactTableLayout layout(renderer, 0, 160, 480, 16, 4, leftStyle(), 0, plan);
  ASSERT_TRUE(layout.valid());
  for (const bool merged : {false, true}) {
    ASSERT_TRUE(layout.beginRow());
    const uint8_t cols = merged ? 2 : 3;
    for (uint8_t column = 0; column < cols; ++column) {
      ASSERT_TRUE(layout.beginCell(false, merged && column == 0 ? 2 : 1, 0, leftStyle()));
      ASSERT_TRUE(layout.appendWord("word", EpdFontFamily::REGULAR, false, false, 0));
      ASSERT_TRUE(layout.endCell({}));
    }
    TableFragmentRow row;
    std::vector<std::shared_ptr<TextBlock>> flattened;
    std::vector<FootnoteEntry> notes;
    uint32_t offset = 0;
    ASSERT_EQ(finishAndDrain(layout, row, flattened, notes, offset), CompactTableLayout::RowResult::Ok);
    ASSERT_EQ(row.cells.size(), cols);
    EXPECT_EQ(row.cells.front().rightEdge, merged ? 122 : 82);
    EXPECT_EQ(row.cells.front().colSpan, merged ? 2 : 1);
    EXPECT_EQ(row.cells.back().rightEdge, 160);
  }
}

TEST(CompactTableLayoutTest, TallRowTriesLegacyBeforePublishOrFlattensOnlyItself) {
  GfxRenderer renderer;
  const TableColumnLayout::Plan plan{{120, 148, 270, 0}, 3};
  const auto emit = [&](CompactTableLayout& layout, const bool tall, TableFragmentRow& row,
                        std::vector<std::shared_ptr<TextBlock>>& flat) {
    EXPECT_TRUE(layout.beginRow());
    for (uint8_t col = 0; col < 3; ++col) {
      EXPECT_TRUE(layout.beginCell(false, 1, 0, leftStyle()));
      const int words = tall && col == 1 ? 8 : 1;
      for (int i = 0; i < words; ++i) {
        EXPECT_TRUE(layout.appendWord("abcdefghijklmnopqrst", EpdFontFamily::REGULAR, false, false, 0));
      }
      EXPECT_TRUE(layout.endCell({}));
    }
    std::vector<FootnoteEntry> notes;
    uint32_t offset = 0;
    return finishAndDrain(layout, row, flat, notes, offset);
  };
  TableFragmentRow row;
  std::vector<std::shared_ptr<TextBlock>> flat;
  CompactTableLayout firstTall(renderer, 0, 270, 96, 16, 4, leftStyle(), 0, plan);
  ASSERT_EQ(emit(firstTall, true, row, flat), CompactTableLayout::RowResult::Ok);
  ASSERT_EQ(row.cells.size(), 3);
  EXPECT_EQ(row.cells.front().rightEdge, 0);  // legacy equal width prevented flattening
  EXPECT_TRUE(flat.empty());

  CompactTableLayout laterTall(renderer, 0, 270, 96, 16, 4, leftStyle(), 0, plan);
  ASSERT_EQ(emit(laterTall, false, row, flat), CompactTableLayout::RowResult::Ok);
  EXPECT_EQ(row.cells.front().rightEdge, 120);
  ASSERT_EQ(emit(laterTall, true, row, flat), CompactTableLayout::RowResult::FlattenRow);
  EXPECT_FALSE(flat.empty());
  EXPECT_FALSE(laterTall.flattened());
  ASSERT_EQ(emit(laterTall, false, row, flat), CompactTableLayout::RowResult::Ok);
  EXPECT_EQ(row.cells.front().rightEdge, 120);
  EXPECT_TRUE(flat.empty());

  CompactTableLayout equal(renderer, 0, 270, 96, 16, 4, leftStyle());
  ASSERT_EQ(emit(equal, false, row, flat), CompactTableLayout::RowResult::Ok);
  ASSERT_EQ(emit(equal, true, row, flat), CompactTableLayout::RowResult::Ok);
  // Use a narrower viewport so a long row also exercises row-local fallback
  // in a table whose prepass did not produce a proportional plan.
  CompactTableLayout narrow(renderer, 0, 84, 96, 16, 4, leftStyle());
  ASSERT_EQ(emit(narrow, false, row, flat), CompactTableLayout::RowResult::Ok);
  ASSERT_EQ(emit(narrow, true, row, flat), CompactTableLayout::RowResult::FlattenRow);
  EXPECT_FALSE(flat.empty());
  EXPECT_FALSE(narrow.flattened());
  ASSERT_EQ(emit(narrow, false, row, flat), CompactTableLayout::RowResult::Ok);
  EXPECT_TRUE(flat.empty());
}

TEST(CompactTableLayoutTest, OversizedCapturePrefixStreamsMoreThanOneGridCellsLineLimit) {
  GfxRenderer renderer;
  CompactTableLayout layout(renderer, 0, 60, 96, 16, 4, leftStyle());
  ASSERT_TRUE(layout.beginRow());
  ASSERT_TRUE(layout.beginCell(false, 1, 42, leftStyle()));
  for (int i = 0; i < 160; ++i) {
    ASSERT_TRUE(layout.appendWord("abcdefghijklmnopqrst", EpdFontFamily::REGULAR, false, false, 0));
  }
  ASSERT_TRUE(layout.endCell({}));
  TableFragmentRow row;
  std::vector<FootnoteEntry> notes;
  uint32_t offset = 0;
  ASSERT_EQ(layout.finishRow(row, notes, offset), CompactTableLayout::RowResult::FlattenRow);
  EXPECT_TRUE(layout.hasActiveRow());
  unsigned lines = 0;
  ASSERT_TRUE(layout.visitCapturedParagraphs(
      [](void* context, std::shared_ptr<TextBlock> line, uint32_t offset) {
        EXPECT_NE(line, nullptr);
        EXPECT_EQ(offset, 42u);
        ++*static_cast<unsigned*>(context);
        return true;
      },
      &lines));
  EXPECT_GT(lines, 64u);
  EXPECT_FALSE(layout.hasActiveRow());
  EXPECT_TRUE(layout.beginRow());
}

TEST(CompactTableLayoutTest, WideLeadingColumnKeepsEightColumnTableLabelsReadable) {
  constexpr uint16_t tableWidth = 480;
  EXPECT_EQ(TableColumnLayout::columnWidth(tableWidth, 8, 0, 1), 106);
  EXPECT_EQ(TableColumnLayout::columnWidth(tableWidth, 8, 1, 1), 54);
  EXPECT_EQ(TableColumnLayout::columnWidth(tableWidth, 8, 0, 8), tableWidth);
  EXPECT_EQ(TableColumnLayout::columnStart(tableWidth, 8, 8), tableWidth);

  // Other grids keep their established equal column sizing.
  EXPECT_EQ(TableColumnLayout::columnWidth(tableWidth, 6, 0, 1), 80);
  EXPECT_EQ(TableColumnLayout::columnWidth(tableWidth, 6, 5, 1), 80);
}

TEST(CompactTableLayoutTest, VisitsWrappedCellsInVisualReadingOrder) {
  struct Cell {
    std::vector<int> lines;
  };
  struct Row {
    std::vector<Cell> cells;
  };

  const Row row{{{{1, 2}}, {{3}}, {{4, 5}}}};
  std::vector<std::pair<size_t, size_t>> visited;
  ASSERT_TRUE(
      TableTextLineOrder::forEachCellLineInVisualOrder(row, [&](const size_t cellIndex, const size_t lineIndex) {
        visited.emplace_back(cellIndex, lineIndex);
        return true;
      }));

  const std::vector<std::pair<size_t, size_t>> expected{{0, 0}, {1, 0}, {2, 0}, {0, 1}, {2, 1}};
  EXPECT_EQ(visited, expected);
}

TEST(CompactTableLayoutTest, CharacterSpacingCrossesAttachedStyleBoundaries) {
  for (bool rtl : {false, true}) {
    for (int8_t spacing : {-2, 2}) {
      GfxRenderer renderer;
      auto style = leftStyle();
      style.isRtl = rtl;
      CompactTableLayout layout(renderer, 0, 80, 200, 10, 2, style, spacing);
      ASSERT_TRUE(layout.beginRow());
      ASSERT_TRUE(layout.beginCell(false, 1, 0, style));
      ASSERT_TRUE(layout.appendWord("AB", EpdFontFamily::REGULAR, false, false, 0));
      ASSERT_TRUE(layout.appendWord("CD", EpdFontFamily::BOLD, true, false, 0));
      ASSERT_TRUE(layout.endCell({}));
      TableFragmentRow row;
      std::vector<std::shared_ptr<TextBlock>> flattened;
      std::vector<FootnoteEntry> notes;
      uint32_t offset = 0;
      ASSERT_EQ(finishAndDrain(layout, row, flattened, notes, offset), CompactTableLayout::RowResult::Ok);
      ASSERT_EQ(row.cells.size(), 1u);
      ASSERT_EQ(row.cells[0].lines.size(), 1u);
      const auto& positions = row.cells[0].lines[0]->xPositions;
      ASSERT_EQ(positions.size(), 2u);
      EXPECT_EQ(rtl ? positions[0] - positions[1] : positions[1] - positions[0], 2 + spacing / 2);
    }
  }
}

TEST(CompactTableLayoutTest, CompactLayoutUsesWideLeadingColumnForEightCellRows) {
  GfxRenderer renderer;
  renderer.codepointWidth = 10;
  CompactTableLayout layout(renderer, 0, 480, 300, 10, 6, leftStyle());
  ASSERT_TRUE(layout.beginRow());

  for (uint8_t column = 0; column < 8; ++column) {
    ASSERT_TRUE(layout.beginCell(column == 0, 1, 0, leftStyle()));
    ASSERT_TRUE(layout.appendWord(column == 0 ? "Oatmeal" : "100", EpdFontFamily::REGULAR, false, false, 0));
    ASSERT_TRUE(layout.endCell({}));
  }

  TableFragmentRow row;
  std::vector<std::shared_ptr<TextBlock>> flatLines;
  std::vector<FootnoteEntry> footnotes;
  uint32_t offset = 0;
  ASSERT_EQ(finishAndDrain(layout, row, flatLines, footnotes, offset), CompactTableLayout::RowResult::Ok);
  ASSERT_EQ(row.cells.size(), 8u);
  EXPECT_EQ(row.cells.front().lines.size(), 1u);
}

TEST(CompactTableLayoutTest, PreservesColspanAndFullWidthRows) {
  GfxRenderer renderer;
  CompactTableLayout layout(renderer, 0, 80, 200, 10, 2, leftStyle());
  ASSERT_TRUE(layout.valid());

  ASSERT_TRUE(layout.beginRow());
  ASSERT_TRUE(layout.beginCell(true, 8, 12, leftStyle()));
  ASSERT_TRUE(layout.appendWord("Group", EpdFontFamily::BOLD, false, false, 0));
  ASSERT_TRUE(layout.endCell({}));
  TableFragmentRow row;
  std::vector<std::shared_ptr<TextBlock>> flatLines;
  std::vector<FootnoteEntry> footnotes;
  uint32_t offset = 0;
  const auto result = finishAndDrain(layout, row, flatLines, footnotes, offset);

  EXPECT_EQ(result, CompactTableLayout::RowResult::Ok);
  ASSERT_EQ(row.cells.size(), 1u);
  EXPECT_EQ(row.cells.front().colSpan, 8);
  EXPECT_EQ(layout.fragmentColumnCount(), 1);
}

TEST(CompactTableLayoutTest, BreaksUtf8AndOversizedCodepointsWithoutAbort) {
  GfxRenderer renderer;
  renderer.codepointWidth = 3;
  CompactTableLayout layout(renderer, 0, 12, 200, 10, 1, leftStyle());
  ASSERT_TRUE(layout.beginRow());
  ASSERT_TRUE(layout.beginCell(false, 1, 0, leftStyle()));
  const std::string longUtf8 = "长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长长";
  ASSERT_TRUE(layout.appendWord(longUtf8, EpdFontFamily::REGULAR, false, false, 0));
  ASSERT_TRUE(layout.endCell({}));
  TableFragmentRow row;
  std::vector<std::shared_ptr<TextBlock>> flatLines;
  std::vector<FootnoteEntry> footnotes;
  uint32_t offset = 0;
  EXPECT_EQ(finishAndDrain(layout, row, flatLines, footnotes, offset), CompactTableLayout::RowResult::Ok);
  ASSERT_EQ(row.cells.size(), 1u);
  EXPECT_GE(row.cells.front().lines.size(), 2u);

  ASSERT_TRUE(layout.beginRow());
  ASSERT_TRUE(layout.beginCell(false, 1, 0, leftStyle()));
  renderer.codepointWidth = 32;
  ASSERT_TRUE(layout.appendWord("☃", EpdFontFamily::REGULAR, false, false, 0));
  ASSERT_TRUE(layout.endCell({}));
  row = {};
  flatLines.clear();
  footnotes.clear();
  const auto oversizedResult = finishAndDrain(layout, row, flatLines, footnotes, offset);
  EXPECT_NE(oversizedResult, CompactTableLayout::RowResult::Abort);
  ASSERT_EQ(row.cells.size(), 1u);
  ASSERT_EQ(row.cells.front().lines.size(), 1u);
  EXPECT_EQ(row.cells.front().lines.front()->words.size(), 1u);
}

TEST(CompactTableLayoutTest, UnsupportedRowsFlattenDeterministically) {
  GfxRenderer renderer;
  CompactTableLayout layout(renderer, 0, 80, 200, 10, 2, leftStyle());
  ASSERT_TRUE(layout.beginRow());
  ASSERT_TRUE(layout.beginCell(false, 9, 0, leftStyle()));
  ASSERT_TRUE(layout.appendWord("too-wide", EpdFontFamily::REGULAR, false, false, 0));
  ASSERT_TRUE(layout.endCell({}));
  layout.markUnsupported();
  TableFragmentRow row;
  std::vector<std::shared_ptr<TextBlock>> flatLines;
  std::vector<FootnoteEntry> footnotes;
  uint32_t offset = 0;
  EXPECT_EQ(finishAndDrain(layout, row, flatLines, footnotes, offset), CompactTableLayout::RowResult::Flatten);
  EXPECT_FALSE(flatLines.empty());
}

}  // namespace

TEST(CompactTableLayoutTest, SourceRangesSurviveEmergencyUtf8Wrapping) {
  GfxRenderer renderer;
  renderer.codepointWidth = 3;
  CompactTableLayout layout(renderer, 0, 12, 200, 10, 1, leftStyle());
  ASSERT_TRUE(layout.beginRow());
  ASSERT_TRUE(layout.beginCell(false, 1, 70, leftStyle()));
  ASSERT_TRUE(layout.appendWord("长长长长长长", EpdFontFamily::REGULAR, false, false, 0, 70));
  ASSERT_TRUE(layout.endCell({}));
  TableFragmentRow row;
  std::vector<std::shared_ptr<TextBlock>> flat;
  std::vector<FootnoteEntry> notes;
  uint32_t offset = 0;
  ASSERT_EQ(finishAndDrain(layout, row, flat, notes, offset), CompactTableLayout::RowResult::Ok);
  ASSERT_EQ(row.cells.size(), 1U);
  uint32_t position = 70;
  for (const auto& line : row.cells[0].lines) {
    for (const auto& range : line->sources) {
      EXPECT_EQ(range.start, position);
      EXPECT_GT(range.end, range.start);
      position = range.end;
    }
  }
  EXPECT_EQ(position, 76U);
}
