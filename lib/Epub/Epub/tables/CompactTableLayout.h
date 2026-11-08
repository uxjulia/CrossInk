#pragma once

#include <EpdFontFamily.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "Epub/FootnoteEntry.h"
#include "Epub/Page.h"
#include "Epub/blocks/BlockStyle.h"

class GfxRenderer;

// A deliberately small table capture used when a rich ParsedText table would
// consume the remaining contiguous heap. Only the current source row is kept.
// The row text is shared by all cells and grows once (2 KiB -> 4 KiB max).
class CompactTableLayout final {
 public:
  static constexpr uint8_t MAX_COLUMNS = 8;
  static constexpr uint8_t MAX_STYLE_RUNS = 64;
  static constexpr uint16_t INITIAL_ROW_TEXT_BYTES = 2048;
  static constexpr uint16_t MAX_ROW_TEXT_BYTES = 4096;

  enum class RowResult : uint8_t { Ok, Flatten, Abort };

  CompactTableLayout(GfxRenderer& renderer, int fontId, uint16_t viewportWidth, uint16_t viewportHeight,
                     uint16_t lineHeight, uint8_t cellPadding, BlockStyle tableStyle, int8_t characterSpacing = 0);
  ~CompactTableLayout() = default;

  CompactTableLayout(const CompactTableLayout&) = delete;
  CompactTableLayout& operator=(const CompactTableLayout&) = delete;

  bool valid() const { return buffer_ != nullptr && !fatal_; }
  bool beginRow();
  bool beginCell(bool isHeader, uint8_t colSpan, uint32_t visibleTextOffset, const BlockStyle& style);
  bool appendWord(std::string_view text, EpdFontFamily::Style style, bool attachToPrevious, bool backgroundBlack,
                  uint8_t linkId, uint32_t sourceOffset = UINT32_MAX);
  bool endCell(const std::vector<std::pair<int, FootnoteEntry>>& footnotes);
  void markUnsupported() { unsupported_ = true; }
  bool unsupported() const { return unsupported_; }
  bool flattened() const { return flattened_; }
  void setFlattened() { flattened_ = true; }
  bool hasActiveCell() const { return cellActive_; }
  bool hasActiveRow() const { return rowActive_; }
  uint8_t fragmentColumnCount() const { return fragmentColumnCount_; }
  const BlockStyle& tableStyle() const { return tableStyle_; }

  // Converts the captured row directly into existing PageTableFragment data.
  // When the row is unsupported, flatLines receives ordinary TextBlocks so the
  // parser can explicitly flatten it without reconstructing a ParsedText model.
  RowResult finishRow(TableFragmentRow& gridRow, std::vector<std::shared_ptr<TextBlock>>& flatLines,
                      std::vector<FootnoteEntry>& footnotes, uint32_t& visibleTextOffset);

 private:
  struct Token {
    uint32_t sourceOffset = UINT32_MAX;
    uint16_t offset = 0;
    uint16_t length = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    uint8_t flags = 0;
    bool attachToPrevious = false;
  };

  struct Cell {
    uint16_t firstToken = 0;
    uint16_t tokenCount = 0;
    uint8_t colSpan = 1;
    bool isHeader = false;
    uint32_t visibleTextOffset = 0;
    BlockStyle style;
  };

  struct LineToken {
    uint16_t offset = 0;
    uint16_t length = 0;
    uint16_t width = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    uint8_t flags = 0;
    bool attachToPrevious = false;
  };

  static constexpr uint16_t MAX_ROW_TOKENS = 256;
  // Keep enough metadata to flatten a malformed/wide row without retaining a
  // second rich model; valid grids still enforce MAX_COLUMNS above.
  static constexpr uint8_t MAX_CELLS_PER_ROW = 32;
  static constexpr uint8_t MAX_LINES_PER_CELL = TableFragmentCell::MAX_SERIALIZED_LINES;

  GfxRenderer& renderer_;
  int fontId_;
  int8_t characterSpacing_;
  uint16_t viewportWidth_;
  uint16_t viewportHeight_;
  uint16_t lineHeight_;
  uint8_t cellPadding_;
  BlockStyle tableStyle_;

  std::unique_ptr<char[]> buffer_;
  uint16_t bufferCapacity_ = 0;
  uint16_t bufferUsed_ = 0;
  std::array<Token, MAX_ROW_TOKENS> tokens_{};
  // Reused line scratch; keeping this on the compact layout object avoids a
  // multi-kilobyte stack frame while wrapping a pathological cell.
  std::array<LineToken, MAX_ROW_TOKENS> lineScratch_{};
  uint16_t tokenCount_ = 0;
  std::array<Cell, MAX_CELLS_PER_ROW> cells_{};
  std::array<FootnoteEntry, Page::MAX_FOOTNOTES_PER_PAGE> rowFootnotes_{};
  uint8_t rowFootnoteCount_ = 0;
  uint8_t cellCount_ = 0;
  uint16_t effectiveColumnCount_ = 0;
  uint8_t columnCount_ = 0;
  uint8_t styleRunCount_ = 0;
  EpdFontFamily::Style lastStyle_ = EpdFontFamily::REGULAR;
  bool haveLastStyle_ = false;
  bool baseStyleFallback_ = false;
  bool unsupported_ = false;
  bool flattened_ = false;
  bool fatal_ = false;
  bool allocationFailure_ = false;
  bool rowActive_ = false;
  bool cellActive_ = false;
  uint8_t fragmentColumnCount_ = 0;

  TextBlock::SourceRange sourceRange(uint16_t offset, uint16_t length) const;
  bool ensureBuffer(size_t additionalBytes);
  uint16_t tableWidth() const;
  uint16_t innerWidthForSpan(uint8_t columns, uint8_t startColumn, uint8_t span) const;
  int gapBefore(const LineToken& previous, uint16_t offset, EpdFontFamily::Style style, bool attached) const;
  uint16_t measure(uint16_t offset, uint16_t length, EpdFontFamily::Style style);
  bool appendLineToken(std::array<LineToken, MAX_ROW_TOKENS>& line, uint16_t& lineCount, uint16_t& lineWidth,
                       uint16_t maxWidth, uint16_t offset, uint16_t length, EpdFontFamily::Style style, uint8_t flags,
                       bool attachToPrevious, bool& emittedAny, const BlockStyle& cellStyle, TableFragmentCell& output);
  bool emitLine(std::array<LineToken, MAX_ROW_TOKENS>& line, uint16_t lineCount, uint16_t lineWidth, uint16_t maxWidth,
                const BlockStyle& style, TableFragmentCell& output);
  bool wrapCell(const Cell& cell, uint16_t maxWidth, TableFragmentCell& output);
  bool flattenCell(const Cell& cell, std::vector<std::shared_ptr<TextBlock>>& output);
  RowResult finishRowInternal(bool forceFlatten, TableFragmentRow& gridRow,
                              std::vector<std::shared_ptr<TextBlock>>& flatLines, std::vector<FootnoteEntry>& footnotes,
                              uint32_t& visibleTextOffset);
};
