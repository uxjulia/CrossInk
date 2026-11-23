#pragma once

#include <EpdFontFamily.h>
#include <Memory.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "Epub/FootnoteEntry.h"
#include "Epub/Page.h"
#include "Epub/blocks/BlockStyle.h"
#include "TableColumnPlan.h"

class GfxRenderer;

// A deliberately small table capture used when a rich ParsedText table would
// consume the remaining contiguous heap. Only the current source row is kept.
// The row text is shared by all cells and grows once (2 KiB -> 4 KiB max).
class CompactTableLayout final {
 public:
  static constexpr uint8_t MAX_COLUMNS = 8;
  static constexpr uint8_t MAX_STYLE_RUNS = 64;
  static constexpr uint16_t INITIAL_ROW_TEXT_BYTES = 512;
  static constexpr uint16_t MAX_ROW_TEXT_BYTES = 4096;

  using ParagraphVisitor = bool (*)(void*, std::shared_ptr<TextBlock>, uint32_t);

  enum class RowResult : uint8_t { Ok, Flatten, FlattenRow, Abort };

  CompactTableLayout(GfxRenderer& renderer, int fontId, uint16_t viewportWidth, uint16_t viewportHeight,
                     uint16_t lineHeight, uint8_t cellPadding, BlockStyle tableStyle, int8_t characterSpacing = 0,
                     TableColumnLayout::Plan columnPlan = {});
  ~CompactTableLayout() = default;

  CompactTableLayout(const CompactTableLayout&) = delete;
  CompactTableLayout& operator=(const CompactTableLayout&) = delete;

  bool valid() const { return buffer_ != nullptr && !fatal_; }
  bool beginRow();
  bool beginCell(bool isHeader, uint8_t colSpan, uint32_t visibleTextOffset, const BlockStyle& style);
  bool appendWord(std::string_view text, EpdFontFamily::Style style, bool attachToPrevious, bool backgroundBlack,
                  uint8_t linkId, uint32_t sourceOffset = UINT32_MAX, bool hidden = false,
                  bool backgroundSpace = false);
  bool endCell(const std::vector<std::pair<int, FootnoteEntry>>& footnotes);
  void markUnsupported() { unsupported_ = true; }
  bool unsupported() const { return unsupported_; }
  bool flattened() const { return flattened_; }
  // Drains the captured prefix one line at a time without the grid's
  // per-cell serialization bound. Caller owns pagination and may stop early.
  bool visitCapturedParagraphs(ParagraphVisitor visitor, void* context);
  uint8_t capturedFootnoteCount() const { return rowFootnoteCount_; }
  const FootnoteEntry& capturedFootnote(uint8_t index) const { return rowFootnotes_[index]; }
  void setFlattened() { flattened_ = true; }
  const BlockStyle& activeCellStyle() const { return cells_[cellCount_ - 1].style; }
  bool hasActiveCell() const { return cellActive_; }
  bool hasActiveRow() const { return rowActive_; }
  uint8_t fragmentColumnCount() const { return fragmentColumnCount_; }
  const BlockStyle& tableStyle() const { return tableStyle_; }
  uint8_t cellPadding() const { return cellPadding_; }

  // Resolves the row to a grid or a streamed paragraph fallback. After
  // Flatten/FlattenRow, drain through visitCapturedParagraphs before beginRow.
  RowResult finishRow(TableFragmentRow& gridRow, uint32_t& visibleTextOffset);
  RowResult finishRow(TableFragmentRow& gridRow, std::vector<FootnoteEntry>& footnotes, uint32_t& visibleTextOffset);

 private:
  struct Token {
    uint32_t sourceOffset = UINT32_MAX;
    uint16_t offset = 0;
    uint16_t length = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    uint8_t flags = 0;
    bool attachToPrevious = false;
    uint8_t paint = 0;
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
    uint16_t lengthAndPaint = 0;
    uint16_t width = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    uint8_t flags = 0;

    uint16_t length() const { return lengthAndPaint & 0x1FFF; }
    bool attached() const { return lengthAndPaint & 0x2000; }
    uint8_t paint() const { return lengthAndPaint >> 8 & 0xC0; }
    static uint16_t pack(uint16_t length, bool attach, uint8_t paint) {
      return length | (attach ? 0x2000 : 0) | static_cast<uint16_t>((paint & 0xC0) << 8);
    }
  };
  static_assert(MAX_ROW_TEXT_BYTES <= 0x1FFF && sizeof(LineToken) == 8);

  static constexpr uint16_t INLINE_ROW_TOKENS = 32;
  static constexpr uint16_t MAX_ROW_TOKENS = 256;
  // Keep enough metadata to flatten a malformed/wide row without retaining a
  // second rich model; valid grids still enforce MAX_COLUMNS above.
  static constexpr uint8_t MAX_CELLS_PER_ROW = 32;
  static constexpr uint8_t INLINE_CELLS_PER_ROW = MAX_COLUMNS;
  static constexpr uint8_t INLINE_ROW_FOOTNOTES = 1;
  static constexpr uint8_t MAX_LINES_PER_CELL = TableFragmentCell::MAX_SERIALIZED_LINES;

  GfxRenderer& renderer_;
  int fontId_;
  int8_t characterSpacing_;
  uint16_t viewportWidth_;
  uint16_t viewportHeight_;
  uint16_t lineHeight_;
  uint8_t cellPadding_;
  BlockStyle tableStyle_;
  TableColumnLayout::Plan columnPlan_;

  std::unique_ptr<char[]> buffer_;
  uint16_t bufferCapacity_ = 0;
  uint16_t bufferUsed_ = 0;

  template <typename T, size_t InlineCapacity, size_t MaxCapacity>
  class InlineSpillArray {
   public:
    bool ensureCapacity(const size_t required) {
      if (required > MaxCapacity) return false;
      if (required <= InlineCapacity || spill_) return true;
      spill_ = makeUniqueNoThrow<T[]>(MaxCapacity - InlineCapacity);
      return static_cast<bool>(spill_);
    }

    T& operator[](const size_t index) {
      return index < InlineCapacity ? inline_[index] : spill_[index - InlineCapacity];
    }
    const T& operator[](const size_t index) const {
      return index < InlineCapacity ? inline_[index] : spill_[index - InlineCapacity];
    }

   private:
    std::array<T, InlineCapacity> inline_{};
    std::unique_ptr<T[]> spill_;
  };

  using TokenStorage = InlineSpillArray<Token, INLINE_ROW_TOKENS, MAX_ROW_TOKENS>;
  using LineStorage = InlineSpillArray<LineToken, INLINE_ROW_TOKENS, MAX_ROW_TOKENS>;
  using CellStorage = InlineSpillArray<Cell, INLINE_CELLS_PER_ROW, MAX_CELLS_PER_ROW>;
  using FootnoteStorage = InlineSpillArray<FootnoteEntry, INLINE_ROW_FOOTNOTES, Page::MAX_FOOTNOTES_PER_PAGE>;

  TokenStorage tokens_;
  LineStorage lineScratch_;
  CellStorage cells_;
  FootnoteStorage rowFootnotes_;
  uint16_t tokenCount_ = 0;
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
  bool plannedRowEmitted_ = false;
  bool rowActive_ = false;
  bool cellActive_ = false;
  uint8_t fragmentColumnCount_ = 0;
  bool lineScratchLimited_ = false;
  bool ensureTokenCapacity(size_t required);
  bool ensureCellCapacity(size_t required);
  bool ensureFootnoteCapacity(size_t required);

  struct LineOutput {
    TableFragmentCell* grid = nullptr;
    ParagraphVisitor paragraph = nullptr;
    void* context = nullptr;
    uint32_t visibleOffset = 0;
  };

  TextBlock::SourceRange sourceRange(uint16_t offset, uint16_t length) const;
  bool ensureBuffer(size_t additionalBytes);
  uint16_t tableWidth() const;
  uint16_t innerWidthForSpan(uint8_t columns, uint8_t startColumn, uint8_t span) const;
  int gapBefore(const LineToken& previous, uint16_t offset, EpdFontFamily::Style style, bool attached) const;
  uint16_t measure(uint16_t offset, uint16_t length, EpdFontFamily::Style style);
  bool appendLineToken(uint16_t& lineCount, uint16_t& lineWidth, uint16_t maxWidth, uint16_t offset, uint16_t length,
                       EpdFontFamily::Style style, uint8_t flags, bool attachToPrevious, uint8_t paint,
                       bool& emittedAny, const BlockStyle& cellStyle, const LineOutput& output);
  bool emitLine(const LineStorage& line, uint16_t lineCount, uint16_t lineWidth, uint16_t maxWidth,
                const BlockStyle& style, const LineOutput& output);
  bool wrapCell(const Cell& cell, uint16_t maxWidth, const LineOutput& output);
  RowResult finishRowInternal(bool forceFlatten, TableFragmentRow& gridRow, uint32_t& visibleTextOffset);
};
