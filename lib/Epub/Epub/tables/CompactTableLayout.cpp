#include "CompactTableLayout.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <string>

#include "TableColumnLayout.h"

namespace {

constexpr uint8_t FOOTNOTE_LIMIT = Page::MAX_FOOTNOTES_PER_PAGE;

EpdFontFamily::Style baseStyle(const EpdFontFamily::Style style, const bool fallback) {
  return fallback ? EpdFontFamily::REGULAR : style;
}

}  // namespace

CompactTableLayout::CompactTableLayout(GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const uint16_t viewportHeight, const uint16_t lineHeight,
                                       const uint8_t cellPadding, BlockStyle tableStyle, const int8_t characterSpacing,
                                       const TableColumnLayout::Plan columnPlan)
    : renderer_(renderer),
      fontId_(fontId),
      characterSpacing_(characterSpacing),
      viewportWidth_(viewportWidth),
      viewportHeight_(viewportHeight),
      lineHeight_(lineHeight),
      cellPadding_(cellPadding),
      tableStyle_(tableStyle),
      columnPlan_(columnPlan) {
  // One shared row buffer avoids a ParsedText arena per table cell. It starts
  // small for ordinary rows and grows at most once to the bounded 4 KiB maximum.
  buffer_ = makeUniqueNoThrow<char[]>(INITIAL_ROW_TEXT_BYTES);
  if (!buffer_) {
    LOG_ERR("EHP", "Compact table row buffer allocation failed (%u bytes)", INITIAL_ROW_TEXT_BYTES);
    fatal_ = true;
    return;
  }
  bufferCapacity_ = INITIAL_ROW_TEXT_BYTES;
}

bool CompactTableLayout::ensureBuffer(const size_t additionalBytes) {
  const size_t required = static_cast<size_t>(bufferUsed_) + additionalBytes;
  if (required <= bufferCapacity_) return true;
  if (required > MAX_ROW_TEXT_BYTES) {
    LOG_DBG("EHP", "Compact table row text exceeds %u bytes", MAX_ROW_TEXT_BYTES);
    return false;
  }

  const uint16_t nextCapacity = MAX_ROW_TEXT_BYTES;
  // The replacement is bounded to one 4 KiB allocation and immediately owns
  // the old contents; no second table representation is retained.
  auto grown = makeUniqueNoThrow<char[]>(nextCapacity);
  if (!grown) {
    LOG_ERR("EHP", "Compact table row buffer growth failed (%u bytes)", nextCapacity);
    return false;
  }
  memcpy(grown.get(), buffer_.get(), bufferUsed_);
  buffer_ = std::move(grown);
  bufferCapacity_ = nextCapacity;
  return true;
}

bool CompactTableLayout::ensureTokenCapacity(const size_t required) {
  if (tokens_.ensureCapacity(required)) return true;
  LOG_ERR("EHP", "Compact table token storage allocation failed (%u tokens)", MAX_ROW_TOKENS);
  allocationFailure_ = true;
  return false;
}

bool CompactTableLayout::ensureCellCapacity(const size_t required) {
  if (cells_.ensureCapacity(required)) return true;
  LOG_ERR("EHP", "Compact table cell storage allocation failed (%u cells)", MAX_CELLS_PER_ROW);
  allocationFailure_ = true;
  return false;
}

bool CompactTableLayout::ensureFootnoteCapacity(const size_t required) {
  if (rowFootnotes_.ensureCapacity(required)) return true;
  LOG_ERR("EHP", "Compact table footnote storage allocation failed (%u entries)", Page::MAX_FOOTNOTES_PER_PAGE);
  allocationFailure_ = true;
  return false;
}

uint16_t CompactTableLayout::tableWidth() const {
  const int inset = tableStyle_.totalHorizontalInset();
  return inset < viewportWidth_ ? static_cast<uint16_t>(viewportWidth_ - inset) : viewportWidth_;
}

uint16_t CompactTableLayout::innerWidthForSpan(const uint8_t columns, const uint8_t startColumn,
                                               const uint8_t span) const {
  return TableColumnLayout::innerWidth(tableWidth(), columns, startColumn, span, cellPadding_, columnPlan_);
}

uint16_t CompactTableLayout::measure(const uint16_t offset, const uint16_t length, const EpdFontFamily::Style style) {
  if (length == 0) return 0;
  const size_t end = static_cast<size_t>(offset) + length;
  if (end >= bufferCapacity_) return 0;
  const char saved = buffer_[end];
  buffer_[end] = '\0';
  const int width = renderer_.getTextAdvanceX(fontId_, buffer_.get() + offset, style, 0, characterSpacing_);
  buffer_[end] = saved;
  return static_cast<uint16_t>(std::max(0, width));
}

bool CompactTableLayout::beginRow() {
  if (fatal_ || rowActive_ || cellActive_) return false;
  bufferUsed_ = 0;
  tokenCount_ = 0;
  cellCount_ = 0;
  rowFootnoteCount_ = 0;
  effectiveColumnCount_ = 0;
  styleRunCount_ = 0;
  haveLastStyle_ = false;
  baseStyleFallback_ = false;
  allocationFailure_ = false;
  unsupported_ = false;
  rowActive_ = true;
  return true;
}

bool CompactTableLayout::beginCell(const bool isHeader, const uint8_t colSpan, const uint32_t visibleTextOffset,
                                   const BlockStyle& style) {
  if (fatal_ || !rowActive_ || cellActive_ || cellCount_ >= MAX_CELLS_PER_ROW || colSpan == 0) return false;
  if (static_cast<uint16_t>(effectiveColumnCount_ + colSpan) > MAX_COLUMNS) {
    unsupported_ = true;
  }
  if (!ensureCellCapacity(static_cast<size_t>(cellCount_) + 1)) return false;
  auto& cell = cells_[cellCount_];
  cell = Cell{};
  cell.firstToken = tokenCount_;
  cell.colSpan = colSpan;
  cell.isHeader = isHeader;
  cell.visibleTextOffset = visibleTextOffset;
  cell.style = style;
  effectiveColumnCount_ = static_cast<uint16_t>(effectiveColumnCount_ + colSpan);
  ++cellCount_;
  cellActive_ = true;
  return true;
}

bool CompactTableLayout::appendWord(const std::string_view text, const EpdFontFamily::Style requestedStyle,
                                    const bool attachToPrevious, const bool backgroundBlack, const uint8_t linkId,
                                    const uint32_t sourceOffset, const bool hidden, const bool backgroundSpace) {
  if (fatal_ || !rowActive_ || !cellActive_ || text.empty()) return !fatal_;
  if (tokenCount_ >= MAX_ROW_TOKENS) {
    LOG_DBG("EHP", "Compact table row exceeds %u text runs", MAX_ROW_TOKENS);
    return false;
  }
  if (!ensureTokenCapacity(static_cast<size_t>(tokenCount_) + 1)) return false;

  EpdFontFamily::Style style = baseStyle(requestedStyle, baseStyleFallback_);
  if (!haveLastStyle_) {
    lastStyle_ = style;
    styleRunCount_ = 1;
    haveLastStyle_ = true;
  } else if (style != lastStyle_) {
    if (styleRunCount_ >= MAX_STYLE_RUNS) {
      // Keep the text and geometry, but drop rich font variants for this row.
      // This is deterministic and does not require recapturing a second model.
      baseStyleFallback_ = true;
      for (uint16_t i = 0; i < tokenCount_; ++i) {
        tokens_[i].style = EpdFontFamily::REGULAR;
      }
      style = EpdFontFamily::REGULAR;
    } else {
      ++styleRunCount_;
      lastStyle_ = style;
    }
  }

  const size_t bytes = text.size() + 1;  // Keep every token NUL-terminated for exact metric probes.
  if (!ensureBuffer(bytes)) {
    return false;
  }
  const uint16_t offset = bufferUsed_;
  memcpy(buffer_.get() + offset, text.data(), text.size());
  buffer_[offset + text.size()] = '\0';
  bufferUsed_ = static_cast<uint16_t>(bufferUsed_ + bytes);

  auto& token = tokens_[tokenCount_++];
  token.sourceOffset = sourceOffset;
  token.offset = offset;
  token.length = static_cast<uint16_t>(text.size());
  token.style = style;
  token.flags =
      static_cast<uint8_t>((backgroundBlack ? TextBlock::WORD_FLAG_BACKGROUND_BLACK : 0) |
                           ((linkId << TextBlock::WORD_FLAG_LINK_ID_SHIFT) & TextBlock::WORD_FLAG_LINK_ID_MASK));
  token.attachToPrevious = attachToPrevious;
  token.paint = TextBlock::packWordSize(0, hidden, backgroundSpace);
  return true;
}

bool CompactTableLayout::endCell(const std::vector<std::pair<int, FootnoteEntry>>& footnotes) {
  if (fatal_ || !rowActive_ || !cellActive_ || cellCount_ == 0) return false;
  auto& cell = cells_[cellCount_ - 1];
  cell.tokenCount = static_cast<uint16_t>(tokenCount_ - cell.firstToken);
  const size_t count = std::min<size_t>(footnotes.size(), FOOTNOTE_LIMIT);
  const size_t toCapture = std::min<size_t>(count, FOOTNOTE_LIMIT - rowFootnoteCount_);
  if (!ensureFootnoteCapacity(static_cast<size_t>(rowFootnoteCount_) + toCapture)) return false;
  for (size_t i = 0; i < count; ++i) {
    if (rowFootnoteCount_ >= FOOTNOTE_LIMIT) break;
    rowFootnotes_[rowFootnoteCount_++] = footnotes[i].second;
  }
  cellActive_ = false;
  return true;
}

int CompactTableLayout::gapBefore(const LineToken& previous, const uint16_t offset, const EpdFontFamily::Style style,
                                  const bool attached) const {
  if (!attached) return renderer_.getSpaceWidth(fontId_, style);
  if (characterSpacing_ == 0) return 0;
  auto* cursor = reinterpret_cast<const unsigned char*>(buffer_.get() + previous.offset);
  const auto* end = cursor + previous.length();
  uint32_t left = 0;
  while (cursor < end) left = utf8NextCodepoint(&cursor);
  cursor = reinterpret_cast<const unsigned char*>(buffer_.get() + offset);
  const uint32_t right = utf8NextCodepoint(&cursor);
  return renderer_.getKerning(fontId_, left, right, style, characterSpacing_) -
         renderer_.getKerning(fontId_, left, right, style);
}

TextBlock::SourceRange CompactTableLayout::sourceRange(const uint16_t offset, const uint16_t length) const {
  uint16_t low = 0;
  uint16_t high = tokenCount_;
  while (low < high) {
    const uint16_t middle = static_cast<uint16_t>(low + (high - low) / 2);
    if (tokens_[middle].offset <= offset) {
      low = static_cast<uint16_t>(middle + 1);
    } else {
      high = middle;
    }
  }
  if (low == 0) return {};
  const Token& token = tokens_[low - 1];
  if (token.sourceOffset == UINT32_MAX || offset + length > token.offset + token.length) return {};
  auto* cursor = reinterpret_cast<const unsigned char*>(buffer_.get() + token.offset);
  const auto* start = reinterpret_cast<const unsigned char*>(buffer_.get() + offset);
  const auto* finish = start + length;
  uint32_t position = token.sourceOffset;
  while (cursor < start) {
    utf8NextCodepoint(&cursor);
    ++position;
  }
  TextBlock::SourceRange range{position, position};
  while (cursor < finish) {
    utf8NextCodepoint(&cursor);
    ++range.end;
  }
  return range;
}

bool CompactTableLayout::emitLine(const LineStorage& line, const uint16_t lineCount, const uint16_t lineWidth,
                                  const uint16_t maxWidth, const BlockStyle& style, const LineOutput& output) {
  if (lineCount == 0) return true;
  if (output.grid && output.grid->lines.size() >= MAX_LINES_PER_CELL) return false;

  std::vector<TextBlock::SourceRange> sources;
  sources.reserve(lineCount);
  std::vector<std::string> words;
  std::vector<int16_t> xPositions;
  std::vector<EpdFontFamily::Style> styles;
  std::vector<uint8_t> flags;
  std::vector<bool> hasSpaceBefore;
  words.reserve(lineCount);
  xPositions.reserve(lineCount);
  styles.reserve(lineCount);
  flags.reserve(lineCount);
  hasSpaceBefore.reserve(lineCount);
  std::vector<uint8_t> wordSizes;
  bool anyFlags = false;
  for (uint16_t i = 0; i < lineCount; ++i) {
    sources.push_back(sourceRange(line[i].offset, line[i].length()));
    words.emplace_back(buffer_.get() + line[i].offset, line[i].length());
    styles.push_back(line[i].style);
    flags.push_back(line[i].flags);
    if (line[i].paint()) {
      if (wordSizes.empty()) wordSizes.resize(lineCount);
      wordSizes[i] = line[i].paint();
    }
    hasSpaceBefore.push_back(i > 0 && !line[i].attached());
    anyFlags = anyFlags || line[i].flags != 0;
  }

  if (style.isRtl) {
    // TextBlock delegates glyph shaping to the renderer, but it still uses the
    // stored x positions in logical word order. Walk from the right edge so
    // RTL cells do not render their words in LTR order.
    int rightEdge = lineWidth;
    switch (style.alignment) {
      case CssTextAlign::Center:
        rightEdge = (static_cast<int>(maxWidth) + lineWidth) / 2;
        break;
      case CssTextAlign::Right:
      case CssTextAlign::Justify:
        rightEdge = maxWidth;
        break;
      default:
        break;
    }
    for (uint16_t i = 0; i < lineCount; ++i) {
      rightEdge -= line[i].width;
      xPositions.push_back(static_cast<int16_t>(std::clamp(rightEdge, 0, INT16_MAX)));
      if (i + 1 < lineCount) {
        rightEdge -= gapBefore(line[i], line[i + 1].offset, line[i + 1].style, line[i + 1].attached());
      }
    }
  } else {
    int x = 0;
    switch (style.alignment) {
      case CssTextAlign::Center:
        x = std::max(0, (static_cast<int>(maxWidth) - lineWidth) / 2);
        break;
      case CssTextAlign::Right:
        x = std::max(0, static_cast<int>(maxWidth) - lineWidth);
        break;
      default:
        break;
    }
    for (uint16_t i = 0; i < lineCount; ++i) {
      xPositions.push_back(static_cast<int16_t>(std::clamp(x, 0, INT16_MAX)));
      x += line[i].width;
      if (i + 1 < lineCount) {
        x += gapBefore(line[i], line[i + 1].offset, line[i + 1].style, line[i + 1].attached());
      }
    }
  }

  auto owned = std::unique_ptr<TextBlock>(
      new (std::nothrow) TextBlock(words, xPositions, styles, {}, {}, {}, anyFlags ? flags : std::vector<uint8_t>{},
                                   hasSpaceBefore, style, {}, wordSizes, "", characterSpacing_, {}, sources));
  if (!owned || !owned->valid()) {
    LOG_ERR("EHP", "Compact table TextBlock allocation failed (%u words)", lineCount);
    allocationFailure_ = true;
    return false;
  }
  if (output.paragraph) return output.paragraph(output.context, std::move(owned), output.visibleOffset);
  output.grid->lines.emplace_back(std::move(owned));
  return true;
}

bool CompactTableLayout::appendLineToken(uint16_t& lineCount, uint16_t& lineWidth, const uint16_t maxWidth,
                                         const uint16_t offset, const uint16_t length, const EpdFontFamily::Style style,
                                         const uint8_t flags, const bool attachToPrevious, const uint8_t paint,
                                         bool& emittedAny, const BlockStyle& cellStyle, const LineOutput& output) {
  const uint16_t width = measure(offset, length, style);
  const int gap = lineCount > 0 ? gapBefore(lineScratch_[lineCount - 1], offset, style, attachToPrevious) : 0;
  if (lineCount > 0 && static_cast<int>(lineWidth) + gap + width > maxWidth) {
    if (!emitLine(lineScratch_, lineCount, lineWidth, maxWidth, cellStyle, output)) return false;
    lineCount = 0;
    lineWidth = 0;
    emittedAny = true;
  }
  if (lineCount >= INLINE_ROW_TOKENS && !lineScratchLimited_ &&
      !lineScratch_.ensureCapacity(static_cast<size_t>(lineCount) + 1)) {
    // Keep all text if a wide line cannot allocate its spill storage. The
    // bounded common-case slice may add a soft wrap at this point.
    LOG_DBG("EHP", "Compact table line scratch limited to %u words", INLINE_ROW_TOKENS);
    lineScratchLimited_ = true;
  }
  if (lineScratchLimited_ && lineCount >= INLINE_ROW_TOKENS) {
    if (!emitLine(lineScratch_, lineCount, lineWidth, maxWidth, cellStyle, output)) return false;
    lineCount = 0;
    lineWidth = 0;
    emittedAny = true;
  }
  if (lineCount >= MAX_ROW_TOKENS) return false;
  lineScratch_[lineCount++] = {offset, LineToken::pack(length, attachToPrevious, paint), width, style, flags};
  lineWidth = static_cast<uint16_t>(std::max(0, static_cast<int>(lineWidth) + (lineCount > 1 ? gap : 0) + width));
  return true;
}

bool CompactTableLayout::wrapCell(const Cell& cell, const uint16_t maxWidth, const LineOutput& output) {
  if (maxWidth < 1) return false;
  uint16_t lineCount = 0;
  uint16_t lineWidth = 0;
  bool emittedAny = false;

  auto flush = [&]() -> bool {
    if (lineCount == 0) return true;
    const bool ok = emitLine(lineScratch_, lineCount, lineWidth, maxWidth, cell.style, output);
    lineCount = 0;
    lineWidth = 0;
    emittedAny = true;
    return ok;
  };

  for (uint16_t i = 0; i < cell.tokenCount; ++i) {
    const Token& token = tokens_[cell.firstToken + i];
    const EpdFontFamily::Style style = baseStyle(token.style, baseStyleFallback_);
    uint16_t remainingOffset = token.offset;
    uint16_t remainingLength = token.length;
    bool attach = token.attachToPrevious;
    while (remainingLength > 0) {
      const uint16_t fullWidth = measure(remainingOffset, remainingLength, style);
      const int gap = lineCount > 0 ? gapBefore(lineScratch_[lineCount - 1], remainingOffset, style, attach) : 0;
      if (fullWidth <= maxWidth && (lineCount == 0 || static_cast<int>(lineWidth) + gap + fullWidth <= maxWidth)) {
        if (!appendLineToken(lineCount, lineWidth, maxWidth, remainingOffset, remainingLength, style, token.flags,
                             attach, token.paint, emittedAny, cell.style, output)) {
          return false;
        }
        break;
      }

      if (lineCount > 0) {
        if (!flush()) return false;
        attach = true;
        continue;
      }

      // Emergency codepoint breaking for a pathological token. Every prefix
      // is measured through the renderer, so kerning and UTF-8 width stay exact.
      const auto* const firstCodepoint = reinterpret_cast<const unsigned char*>(buffer_.get() + remainingOffset);
      const auto* cursor = firstCodepoint;
      const auto* const end = cursor + remainingLength;
      uint16_t best = 0;
      while (cursor < end) {
        const uint32_t cp = utf8NextCodepoint(&cursor);
        if (cp == 0) break;
        const uint16_t candidate =
            static_cast<uint16_t>(cursor - reinterpret_cast<const unsigned char*>(buffer_.get() + remainingOffset));
        const uint16_t candidateWidth = measure(remainingOffset, candidate, style);
        if (candidateWidth <= maxWidth) {
          best = candidate;
        } else {
          break;
        }
      }
      if (best == 0) {
        // A single glyph can be wider than the cell (for example an emoji in
        // a narrow CJK column). Keep one complete UTF-8 codepoint so wrapping
        // always makes forward progress. The renderer's cell clip protects
        // the neighbouring column; normal lines still satisfy maxWidth.
        const auto* oneCodepoint = firstCodepoint;
        const uint32_t cp = utf8NextCodepoint(&oneCodepoint);
        if (cp == 0 || oneCodepoint <= firstCodepoint) {
          return false;
        }
        best = static_cast<uint16_t>(oneCodepoint - firstCodepoint);
      }
      const uint16_t chunkLen = best;
      const uint16_t chunkWidth = measure(remainingOffset, chunkLen, style);
      lineScratch_[lineCount++] = {remainingOffset, LineToken::pack(chunkLen, attach, token.paint), chunkWidth, style,
                                   token.flags};
      lineWidth = chunkWidth;
      if (!flush()) return false;
      remainingOffset = static_cast<uint16_t>(remainingOffset + chunkLen);
      remainingLength = static_cast<uint16_t>(remainingLength - chunkLen);
      attach = true;
    }
  }

  if (!flush()) return false;
  return true;
}

bool CompactTableLayout::visitCapturedParagraphs(const ParagraphVisitor visitor, void* context) {
  if (!visitor || fatal_ || !rowActive_ || cellActive_) return false;
  const uint16_t width = static_cast<uint16_t>(tableWidth() > cellPadding_ * 2 ? tableWidth() - cellPadding_ * 2 : 1);
  for (uint8_t i = 0; i < cellCount_; ++i) {
    const auto& cell = cells_[i];
    if (!wrapCell(cell, width, {nullptr, visitor, context, cell.visibleTextOffset})) return false;
  }
  rowActive_ = false;
  return true;
}

CompactTableLayout::RowResult CompactTableLayout::finishRowInternal(const bool forceFlatten, TableFragmentRow& gridRow,
                                                                    uint32_t& visibleTextOffset) {
  if (fatal_ || !rowActive_ || cellActive_) return RowResult::Abort;
  if (cellCount_ == 0 || effectiveColumnCount_ == 0) forceFlatten ? flattened_ = true : unsupported_ = true;
  const bool flatten = forceFlatten || unsupported_ || flattened_;
  visibleTextOffset = cellCount_ > 0 ? cells_[0].visibleTextOffset : visibleTextOffset;
  if (flatten) {
    flattened_ = true;
  } else {
    if (columnCount_ == 0) {
      columnCount_ = static_cast<uint8_t>(effectiveColumnCount_);
    }
    const bool fullWidth = cellCount_ == 1 && cells_[0].colSpan == columnCount_;
    bool validGrid = fullWidth || effectiveColumnCount_ == columnCount_;
    if (!validGrid || columnCount_ == 0 || columnCount_ > MAX_COLUMNS) {
      unsupported_ = true;
      return finishRowInternal(true, gridRow, visibleTextOffset);
    }

    const bool planned = !fullWidth && columnCount_ == columnPlan_.count;
    const auto fallback = [&] {
      // Release the partially wrapped cells before rewrapping the same tokens.
      gridRow.cells.clear();
      if (planned && !plannedRowEmitted_) {
        // No planned row is visible yet, so keep the old widths for the
        // entire table rather than taking a new and premature fallback.
        columnPlan_ = {};
        return finishRowInternal(false, gridRow, visibleTextOffset);
      }
      const auto result = finishRowInternal(true, gridRow, visibleTextOffset);
      if (result == RowResult::Flatten) {
        // This row alone could not fit. The established column geometry is
        // still usable by subsequent rows, including in the equal grid.
        flattened_ = false;
        return RowResult::FlattenRow;
      }
      return result;
    };

    gridRow.cells.clear();
    // Store one serialized cell per source cell; colSpan carries the logical
    // width so merged cells are not padded with synthetic empty cells.
    gridRow.cells.resize(fullWidth ? 1 : cellCount_);
    fragmentColumnCount_ = fullWidth ? 1 : columnCount_;
    bool hasHeader = false;
    bool hasData = false;
    uint16_t rowHeight = lineHeight_ + cellPadding_ * 2;
    uint8_t logicalColumn = 0;
    for (uint8_t i = 0; i < cellCount_; ++i) {
      auto& destination = gridRow.cells[i];
      destination.isHeader = cells_[i].isHeader;
      // Keep the source span even for a synthetic full-width heading. The
      // fragment may use one physical column for that row, but the payload
      // still describes the meaningful source grid span.
      destination.colSpan = cells_[i].colSpan;
      if (planned) {
        destination.rightEdge = columnPlan_.ends[logicalColumn + destination.colSpan - 1];
      }
      hasHeader = hasHeader || cells_[i].isHeader;
      hasData = hasData || !cells_[i].isHeader;
      if (!wrapCell(cells_[i], innerWidthForSpan(columnCount_, logicalColumn, destination.colSpan), {&destination})) {
        if (allocationFailure_) return RowResult::Abort;
        return fallback();
      }
      const uint16_t cellHeight =
          static_cast<uint16_t>(std::max<size_t>(1, destination.lines.size()) * lineHeight_ + cellPadding_ * 2);
      rowHeight = std::max(rowHeight, cellHeight);
      logicalColumn = static_cast<uint8_t>(logicalColumn + destination.colSpan);
    }
    if (rowHeight >= viewportHeight_) {
      // A grid row must fit on one page fragment. Rewrap the same captured
      // tokens as ordinary lines so the parser can page-break between lines.
      return fallback();
    }
    plannedRowEmitted_ |= planned;
    gridRow.height = rowHeight;
    gridRow.headerSeparator = hasHeader && !hasData;
  }

  rowActive_ = flatten;
  cellActive_ = false;
  return flatten ? RowResult::Flatten : RowResult::Ok;
}

CompactTableLayout::RowResult CompactTableLayout::finishRow(TableFragmentRow& gridRow,
                                                            std::vector<FootnoteEntry>& footnotes,
                                                            uint32_t& visibleTextOffset) {
  const RowResult result = finishRow(gridRow, visibleTextOffset);
  footnotes.clear();
  footnotes.reserve(rowFootnoteCount_);
  for (uint8_t i = 0; i < rowFootnoteCount_; ++i) {
    footnotes.push_back(rowFootnotes_[i]);
  }
  return result;
}

CompactTableLayout::RowResult CompactTableLayout::finishRow(TableFragmentRow& gridRow, uint32_t& visibleTextOffset) {
  const RowResult result = finishRowInternal(unsupported_ || flattened_, gridRow, visibleTextOffset);
  if (result == RowResult::Abort) {
    fatal_ = true;
    LOG_ERR("EHP", "Compact table row layout failed");
  }
  return result;
}
