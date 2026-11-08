#include "CompactTableLayout.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>
#include <iterator>
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
                                       const uint8_t cellPadding, BlockStyle tableStyle, const int8_t characterSpacing)
    : renderer_(renderer),
      fontId_(fontId),
      characterSpacing_(characterSpacing),
      viewportWidth_(viewportWidth),
      viewportHeight_(viewportHeight),
      lineHeight_(lineHeight),
      cellPadding_(cellPadding),
      tableStyle_(tableStyle) {
  // One shared row buffer avoids a ParsedText arena per table cell. It is kept
  // for the table lifetime and grows at most once to the bounded 4 KiB maximum.
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

uint16_t CompactTableLayout::tableWidth() const {
  const int inset = tableStyle_.totalHorizontalInset();
  return inset < viewportWidth_ ? static_cast<uint16_t>(viewportWidth_ - inset) : viewportWidth_;
}

uint16_t CompactTableLayout::innerWidthForSpan(const uint8_t columns, const uint8_t startColumn,
                                               const uint8_t span) const {
  return TableColumnLayout::innerWidth(tableWidth(), columns, startColumn, span, cellPadding_);
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
  for (auto& cell : cells_) {
    cell = Cell{};
  }
  return true;
}

bool CompactTableLayout::beginCell(const bool isHeader, const uint8_t colSpan, const uint32_t visibleTextOffset,
                                   const BlockStyle& style) {
  if (fatal_ || !rowActive_ || cellActive_ || cellCount_ >= MAX_CELLS_PER_ROW || colSpan == 0) return false;
  if (static_cast<uint16_t>(effectiveColumnCount_ + colSpan) > MAX_COLUMNS) {
    unsupported_ = true;
  }
  auto& cell = cells_[cellCount_];
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
                                    const uint32_t sourceOffset) {
  if (fatal_ || !rowActive_ || !cellActive_ || text.empty()) return !fatal_;
  if (tokenCount_ >= MAX_ROW_TOKENS) {
    LOG_DBG("EHP", "Compact table row exceeds %u text runs", MAX_ROW_TOKENS);
    return false;
  }

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
  return true;
}

bool CompactTableLayout::endCell(const std::vector<std::pair<int, FootnoteEntry>>& footnotes) {
  if (fatal_ || !rowActive_ || !cellActive_ || cellCount_ == 0) return false;
  auto& cell = cells_[cellCount_ - 1];
  cell.tokenCount = static_cast<uint16_t>(tokenCount_ - cell.firstToken);
  const size_t count = std::min<size_t>(footnotes.size(), FOOTNOTE_LIMIT);
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
  const auto* end = cursor + previous.length;
  uint32_t left = 0;
  while (cursor < end) left = utf8NextCodepoint(&cursor);
  cursor = reinterpret_cast<const unsigned char*>(buffer_.get() + offset);
  const uint32_t right = utf8NextCodepoint(&cursor);
  return renderer_.getKerning(fontId_, left, right, style, characterSpacing_) -
         renderer_.getKerning(fontId_, left, right, style);
}

TextBlock::SourceRange CompactTableLayout::sourceRange(const uint16_t offset, const uint16_t length) const {
  const auto end = tokens_.begin() + tokenCount_;
  const auto after = std::upper_bound(tokens_.begin(), end, offset,
                                      [](uint16_t value, const Token& token) { return value < token.offset; });
  if (after == tokens_.begin()) return {};
  const Token& token = *(after - 1);
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

bool CompactTableLayout::emitLine(std::array<LineToken, MAX_ROW_TOKENS>& line, const uint16_t lineCount,
                                  const uint16_t lineWidth, const uint16_t maxWidth, const BlockStyle& style,
                                  TableFragmentCell& output) {
  if (lineCount == 0) return true;

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
  bool anyFlags = false;
  for (uint16_t i = 0; i < lineCount; ++i) {
    sources.push_back(sourceRange(line[i].offset, line[i].length));
    words.emplace_back(buffer_.get() + line[i].offset, line[i].length);
    styles.push_back(line[i].style);
    flags.push_back(line[i].flags);
    hasSpaceBefore.push_back(i > 0 && !line[i].attachToPrevious);
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
        rightEdge -= gapBefore(line[i], line[i + 1].offset, line[i + 1].style, line[i + 1].attachToPrevious);
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
        x += gapBefore(line[i], line[i + 1].offset, line[i + 1].style, line[i + 1].attachToPrevious);
      }
    }
  }

  auto owned = std::unique_ptr<TextBlock>(
      new (std::nothrow) TextBlock(words, xPositions, styles, {}, {}, {}, anyFlags ? flags : std::vector<uint8_t>{},
                                   hasSpaceBefore, style, {}, {}, "", characterSpacing_, {}, sources));
  if (!owned || !owned->valid()) {
    LOG_ERR("EHP", "Compact table TextBlock allocation failed (%u words)", lineCount);
    allocationFailure_ = true;
    return false;
  }
  output.lines.emplace_back(std::move(owned));
  return output.lines.size() <= TableFragmentCell::MAX_SERIALIZED_LINES;
}

bool CompactTableLayout::appendLineToken(std::array<LineToken, MAX_ROW_TOKENS>& line, uint16_t& lineCount,
                                         uint16_t& lineWidth, const uint16_t maxWidth, const uint16_t offset,
                                         const uint16_t length, const EpdFontFamily::Style style, const uint8_t flags,
                                         const bool attachToPrevious, bool& emittedAny, const BlockStyle& cellStyle,
                                         TableFragmentCell& output) {
  const uint16_t width = measure(offset, length, style);
  const int gap = lineCount > 0 ? gapBefore(line[lineCount - 1], offset, style, attachToPrevious) : 0;
  if (lineCount > 0 && static_cast<int>(lineWidth) + gap + width > maxWidth) {
    if (!emitLine(line, lineCount, lineWidth, maxWidth, cellStyle, output)) return false;
    lineCount = 0;
    lineWidth = 0;
    emittedAny = true;
  }
  if (lineCount >= MAX_ROW_TOKENS) return false;
  line[lineCount++] = {offset, length, width, style, flags, attachToPrevious};
  lineWidth = static_cast<uint16_t>(std::max(0, static_cast<int>(lineWidth) + (lineCount > 1 ? gap : 0) + width));
  return true;
}

bool CompactTableLayout::wrapCell(const Cell& cell, const uint16_t maxWidth, TableFragmentCell& output) {
  if (maxWidth < 1) return false;
  auto& line = lineScratch_;
  uint16_t lineCount = 0;
  uint16_t lineWidth = 0;
  bool emittedAny = false;

  auto flush = [&]() -> bool {
    if (lineCount == 0) return true;
    const bool ok = emitLine(line, lineCount, lineWidth, maxWidth, cell.style, output);
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
      const int gap = lineCount > 0 ? gapBefore(line[lineCount - 1], remainingOffset, style, attach) : 0;
      if (fullWidth <= maxWidth && (lineCount == 0 || static_cast<int>(lineWidth) + gap + fullWidth <= maxWidth)) {
        if (!appendLineToken(line, lineCount, lineWidth, maxWidth, remainingOffset, remainingLength, style, token.flags,
                             attach, emittedAny, cell.style, output)) {
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
      line[lineCount++] = {remainingOffset, chunkLen, chunkWidth, style, token.flags, attach};
      lineWidth = chunkWidth;
      if (!flush()) return false;
      remainingOffset = static_cast<uint16_t>(remainingOffset + chunkLen);
      remainingLength = static_cast<uint16_t>(remainingLength - chunkLen);
      attach = true;
    }
  }

  if (!flush()) return false;
  return output.lines.size() <= MAX_LINES_PER_CELL;
}

bool CompactTableLayout::flattenCell(const Cell& cell, std::vector<std::shared_ptr<TextBlock>>& output) {
  TableFragmentCell flattened;
  if (!wrapCell(cell, static_cast<uint16_t>(tableWidth() > cellPadding_ * 2 ? tableWidth() - cellPadding_ * 2 : 1),
                flattened)) {
    return false;
  }
  output.insert(output.end(), std::make_move_iterator(flattened.lines.begin()),
                std::make_move_iterator(flattened.lines.end()));
  return true;
}

CompactTableLayout::RowResult CompactTableLayout::finishRowInternal(const bool forceFlatten, TableFragmentRow& gridRow,
                                                                    std::vector<std::shared_ptr<TextBlock>>& flatLines,
                                                                    std::vector<FootnoteEntry>& footnotes,
                                                                    uint32_t& visibleTextOffset) {
  if (fatal_ || !rowActive_ || cellActive_) return RowResult::Abort;
  if (cellCount_ == 0 || effectiveColumnCount_ == 0) forceFlatten ? flattened_ = true : unsupported_ = true;
  const bool flatten = forceFlatten || unsupported_ || flattened_;
  visibleTextOffset = cellCount_ > 0 ? cells_[0].visibleTextOffset : visibleTextOffset;
  footnotes.clear();
  for (uint8_t i = 0; i < rowFootnoteCount_; ++i) {
    footnotes.push_back(rowFootnotes_[i]);
  }

  if (flatten) {
    for (uint8_t i = 0; i < cellCount_; ++i) {
      if (!flattenCell(cells_[i], flatLines)) return RowResult::Abort;
    }
    flattened_ = true;
  } else {
    if (columnCount_ == 0) {
      columnCount_ = static_cast<uint8_t>(effectiveColumnCount_);
    }
    const bool fullWidth = cellCount_ == 1 && cells_[0].colSpan == columnCount_;
    bool validGrid = fullWidth || effectiveColumnCount_ == columnCount_;
    if (!validGrid || columnCount_ == 0 || columnCount_ > MAX_COLUMNS) {
      unsupported_ = true;
      return finishRowInternal(true, gridRow, flatLines, footnotes, visibleTextOffset);
    }

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
      hasHeader = hasHeader || cells_[i].isHeader;
      hasData = hasData || !cells_[i].isHeader;
      if (!wrapCell(cells_[i], innerWidthForSpan(columnCount_, logicalColumn, destination.colSpan), destination)) {
        if (allocationFailure_) return RowResult::Abort;
        // Release the partially wrapped cells before re-wrapping as paragraphs;
        // the flatten retry only runs when heap is already scarce.
        gridRow.cells.clear();
        flatLines.clear();
        return finishRowInternal(true, gridRow, flatLines, footnotes, visibleTextOffset);
      }
      const uint16_t cellHeight =
          static_cast<uint16_t>(std::max<size_t>(1, destination.lines.size()) * lineHeight_ + cellPadding_ * 2);
      rowHeight = std::max(rowHeight, cellHeight);
      logicalColumn = static_cast<uint8_t>(logicalColumn + destination.colSpan);
    }
    if (rowHeight >= viewportHeight_) {
      // A grid row must fit on one page fragment. Rewrap the same captured
      // tokens as ordinary lines so the parser can page-break between lines.
      gridRow.cells.clear();
      flatLines.clear();
      return finishRowInternal(true, gridRow, flatLines, footnotes, visibleTextOffset);
    }
    gridRow.height = rowHeight;
    gridRow.headerSeparator = hasHeader && !hasData;
  }

  rowActive_ = false;
  cellActive_ = false;
  return flatten ? RowResult::Flatten : RowResult::Ok;
}

CompactTableLayout::RowResult CompactTableLayout::finishRow(TableFragmentRow& gridRow,
                                                            std::vector<std::shared_ptr<TextBlock>>& flatLines,
                                                            std::vector<FootnoteEntry>& footnotes,
                                                            uint32_t& visibleTextOffset) {
  flatLines.clear();
  const RowResult result =
      finishRowInternal(unsupported_ || flattened_, gridRow, flatLines, footnotes, visibleTextOffset);
  if (result == RowResult::Abort) {
    fatal_ = true;
    LOG_ERR("EHP", "Compact table row layout failed");
  }
  return result;
}
