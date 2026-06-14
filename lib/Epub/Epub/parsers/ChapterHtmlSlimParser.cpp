#include "ChapterHtmlSlimParser.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MemoryBudget.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>
#include <strings.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <new>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "Epub/htmlEntities.h"

// Minimum file size (in bytes) to show indexing popup - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_POPUP = 10 * 1024;  // 10KB
constexpr size_t PARSE_BUFFER_SIZE = 1024;
constexpr size_t IMAGE_EXTRACT_CHUNK_SIZE = 1024;
constexpr uint32_t MIN_FREE_HEAP_FOR_TEXT_LAYOUT = 44 * 1024;
constexpr uint32_t MIN_MAX_ALLOC_FOR_TEXT_LAYOUT = 32 * 1024;
constexpr uint32_t MIN_FREE_HEAP_FOR_TABLE_BUFFERING = 64 * 1024;
constexpr uint32_t MIN_MAX_ALLOC_FOR_TABLE_BUFFERING = 40 * 1024;
constexpr size_t MAX_BUFFERED_WORDS_BEFORE_LAYOUT = 350;
constexpr uint8_t INITIAL_PAGE_ELEMENT_RESERVE = 8;
constexpr uint8_t INITIAL_TABLE_FRAGMENT_ROW_RESERVE = 8;
constexpr uint32_t PAGE_ELEMENT_RESERVE_MIN_MAX_ALLOC = 1024;
// Cap chapter anchors so converter-generated IDs do not grow memory without bound.
constexpr size_t MAX_ANCHORS_PER_CHAPTER = 1024;

static constexpr const char* const HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
static constexpr const char* const BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote"};
static constexpr const char* const BOLD_TAGS[] = {"b", "strong"};
static constexpr const char* const ITALIC_TAGS[] = {"i", "em"};
static constexpr const char* const UNDERLINE_TAGS[] = {"u", "ins"};
static constexpr const char* const STRIKETHROUGH_TAGS[] = {"s", "strike", "del"};
static constexpr const char* const IMAGE_TAGS[] = {"img"};
static constexpr const char* const SKIP_TAGS[] = {"head"};

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

static char asciiLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

static bool tokenEqualsIgnoreCase(const char* value, const char* token, const size_t tokenLen) {
  for (size_t i = 0; i < tokenLen; ++i) {
    if (asciiLower(value[i]) != asciiLower(token[i])) return false;
  }
  return true;
}

bool matches(const char* tag_name, const char* const* possible_tags, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

const char* getAttribute(const XML_Char** atts, const char* attrName) {
  if (!atts) return nullptr;
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], attrName) == 0) return atts[i + 1];
  }
  return nullptr;
}

bool isNonNavigableInlineElement(const char* name) { return strcmp(name, "span") == 0; }

bool isInternalEpubLink(const char* href) {
  if (!href || href[0] == '\0') return false;
  if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) return false;
  if (strncmp(href, "mailto:", 7) == 0) return false;
  if (strncmp(href, "ftp://", 6) == 0) return false;
  if (strncmp(href, "tel:", 4) == 0) return false;
  if (strncmp(href, "javascript:", 11) == 0) return false;
  return true;
}

bool attributeContainsToken(const char* value, const char* token) {
  if (!value || !token || token[0] == '\0') return false;

  const size_t tokenLen = strlen(token);
  const char* pos = value;
  while (*pos != '\0') {
    while (isWhitespace(*pos)) {
      pos++;
    }
    const char* end = pos;
    while (*end != '\0' && !isWhitespace(*end)) {
      end++;
    }
    if (static_cast<size_t>(end - pos) == tokenLen && tokenEqualsIgnoreCase(pos, token, tokenLen)) {
      return true;
    }
    pos = end;
  }

  return false;
}

bool classContainsToken(const std::string& value, const char* token) {
  return attributeContainsToken(value.c_str(), token);
}

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, std::size(HEADER_TAGS)) || matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS));
}

bool isTableStructuralTag(const char* name) {
  return strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 || strcmp(name, "td") == 0 || strcmp(name, "th") == 0;
}

void ChapterHtmlSlimParser::skipCurrentElement() {
  skipUntilDepth = depth;
  skipEndElementStateUntilDepth = depth;
  depth += 1;
}

void ChapterHtmlSlimParser::skipDescendantsOfCurrentElement() {
  skipUntilDepth = depth - 1;
  skipEndElementStateUntilDepth = depth;
}

void ChapterHtmlSlimParser::applyDirectionToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasDirection()) {
    entry.hasDirection = true;
    entry.direction = css.direction;
  }
}

// Update effective bold/italic/underline based on block style and inline style stack
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  // Start with block-level styles
  effectiveBold = currentCssStyle.hasFontWeight() && currentCssStyle.fontWeight == CssFontWeight::Bold;
  effectiveItalic = currentCssStyle.hasFontStyle() && currentCssStyle.fontStyle == CssFontStyle::Italic;
  effectiveUnderline =
      currentCssStyle.hasTextDecoration() && currentCssStyle.textDecoration == CssTextDecoration::Underline;
  effectiveStrikethrough =
      currentCssStyle.hasTextDecoration() && currentCssStyle.textDecoration == CssTextDecoration::LineThrough;
  effectiveBackgroundBlack = currentCssStyle.hasBackgroundBlack() && currentCssStyle.backgroundBlack;
  effectiveDirectionDefined = currentCssStyle.hasDirection();
  effectiveDirection = currentCssStyle.direction;
  effectiveSup = currentCssStyle.hasVerticalAlign() && currentCssStyle.verticalAlign == CssVerticalAlign::Super;
  effectiveSub = currentCssStyle.hasVerticalAlign() && currentCssStyle.verticalAlign == CssVerticalAlign::Sub;

  // Apply inline style stack in order
  for (const auto& entry : inlineStyleStack) {
    if (entry.hasBold) {
      effectiveBold = entry.bold;
    }
    if (entry.hasItalic) {
      effectiveItalic = entry.italic;
    }
    if (entry.hasUnderline) {
      effectiveUnderline = entry.underline;
    }
    if (entry.hasStrikethrough) {
      effectiveStrikethrough = entry.strikethrough;
    }
    if (entry.hasBackgroundBlack) {
      effectiveBackgroundBlack = entry.backgroundBlack;
    }
    if (entry.hasDirection) {
      effectiveDirectionDefined = true;
      effectiveDirection = entry.direction;
    }
    if (entry.hasSup) {
      effectiveSup = entry.sup;
      if (entry.sup) effectiveSub = false;
    }
    if (entry.hasSub) {
      effectiveSub = entry.sub;
      if (entry.sub) effectiveSup = false;
    }
  }

  if (currentTextBlock && currentTextBlock->isEmpty()) {
    auto& style = currentTextBlock->getBlockStyle();
    if (effectiveDirectionDefined) {
      style.directionDefined = true;
      style.isRtl = effectiveDirection == CssTextDirection::Rtl;
    } else {
      style.directionDefined = false;
      style.isRtl = false;
    }
  }
}

bool ChapterHtmlSlimParser::shouldAbortForLowMemory(const char* stage) {
  if (lowMemoryAbort) {
    return true;
  }

  auto heap = MemoryBudget::snapshot();
  if (heap.freeHeap >= MIN_FREE_HEAP_FOR_TEXT_LAYOUT && heap.maxAllocHeap >= MIN_MAX_ALLOC_FOR_TEXT_LAYOUT) {
    return false;
  }

  if (!attemptedTextLayoutFontCacheRelease) {
    attemptedTextLayoutFontCacheRelease = true;
    if (renderer.releaseSdCardFontForLowMemory(fontId)) {
      const auto afterRelease = MemoryBudget::snapshot();
      LOG_DBG("EHP", "Released SD font caches before %s: free=%u->%u maxAlloc=%u->%u", stage, heap.freeHeap,
              afterRelease.freeHeap, heap.maxAllocHeap, afterRelease.maxAllocHeap);
      heap = afterRelease;
      if (heap.freeHeap >= MIN_FREE_HEAP_FOR_TEXT_LAYOUT && heap.maxAllocHeap >= MIN_MAX_ALLOC_FOR_TEXT_LAYOUT) {
        return false;
      }
    }
  }

  LOG_ERR("EHP", "Low heap during %s (%u free, %u max alloc); aborting section build", stage, heap.freeHeap,
          heap.maxAllocHeap);
  lowMemoryAbort = true;
  return true;
}

bool ChapterHtmlSlimParser::startNewPage(const char* reason) {
  currentPage.reset(new (std::nothrow) Page());
  if (!currentPage) {
    const auto heap = MemoryBudget::snapshot();
    LOG_ERR("EHP", "Failed to create page during %s (%u free, %u max alloc)", reason, heap.freeHeap, heap.maxAllocHeap);
    lowMemoryAbort = true;
    return false;
  }

  const auto heap = MemoryBudget::snapshot();
  if (heap.freeHeap >= MIN_FREE_HEAP_FOR_TEXT_LAYOUT && heap.maxAllocHeap >= PAGE_ELEMENT_RESERVE_MIN_MAX_ALLOC) {
    currentPage->elements.reserve(INITIAL_PAGE_ELEMENT_RESERVE);
  }
  currentPageNextY = 0;
  return true;
}

void ChapterHtmlSlimParser::flushPendingAnchor() {
  if (pendingAnchorId.empty()) return;

  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  if (std::find(tocAnchors.begin(), tocAnchors.end(), pendingAnchorId) != tocAnchors.end()) {
    if (currentPage && !currentPage->elements.empty()) {
      completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
      completedPageCount++;
      if (!startNewPage("TOC anchor page break")) {
        return;
      }
    }
  }

  // Record deferred anchor after previous block is flushed (and any TOC page break)
  anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
  pendingAnchorId.clear();
}

void ChapterHtmlSlimParser::addPendingPublisherPageMarker(const char* label) {
  if (!label || label[0] == '\0' || tableDepth > 0) {
    return;
  }

  if (partWordBufferIndex > 0) {
    flushPartWordBuffer();
  }

  PendingPublisherPageMarker marker;
  marker.wordIndex = wordsExtractedInBlock + (currentTextBlock ? static_cast<int>(currentTextBlock->size()) : 0);
  strncpy(marker.label, label, sizeof(marker.label) - 1);
  marker.label[sizeof(marker.label) - 1] = '\0';
  pendingPublisherPageMarkers.push_back(marker);
}

void ChapterHtmlSlimParser::attachPendingPublisherPageMarkers(const int yPos) {
  if (!currentPage || pendingPublisherPageMarkers.empty()) {
    return;
  }

  auto markerIt = pendingPublisherPageMarkers.begin();
  while (markerIt != pendingPublisherPageMarkers.end() && markerIt->wordIndex <= wordsExtractedInBlock) {
    currentPage->addPublisherPageMarker(markerIt->label, yPos);
    ++markerIt;
  }
  pendingPublisherPageMarkers.erase(pendingPublisherPageMarkers.begin(), markerIt);
}

// flush the contents of partWordBuffer to currentTextBlock
void ChapterHtmlSlimParser::flushPartWordBuffer() {
  if (lowMemoryAbort) {
    partWordBufferIndex = 0;
    nextWordContinues = false;
    return;
  }

  // Determine font style from depth-based tracking and CSS effective style
  const bool isBold = boldUntilDepth < depth || effectiveBold;
  const bool isItalic = italicUntilDepth < depth || effectiveItalic;
  const bool isUnderline = underlineUntilDepth < depth || effectiveUnderline;
  const bool isStrikethrough = strikethroughUntilDepth < depth || effectiveStrikethrough;

  // Combine style flags using bitwise OR
  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (isBold) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::BOLD);
  }
  if (isItalic) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::ITALIC);
  }
  if (isUnderline) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::UNDERLINE);
  }
  if (isStrikethrough) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::STRIKETHROUGH);
  }
  if (effectiveSup) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUP);
  } else if (effectiveSub) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SUB);
  }

  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  currentTextBlock->addWord(partWordBuffer, fontStyle, false, nextWordContinues, effectiveBackgroundBlack);
  partWordBufferIndex = 0;
  nextWordContinues = false;
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const BlockStyle& blockStyle) {
  if (shouldAbortForLowMemory("text block start")) {
    return;
  }

  nextWordContinues = false;  // New block = new paragraph, no continuation
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      BlockStyle incoming = blockStyle;
      const bool currentIsEmptyBr = currentTextBlock->getBlockStyle().fromBrElement;
      if (currentIsEmptyBr) {
        // The empty block was created by a <br> section separator. Inject a full line of
        // blank space before the following paragraph so the scene/section break is visible.
        // This only fires when the <br> block stayed empty (i.e. no inline text was added).
        const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId) * lineCompression + 0.5f);
        incoming.marginTop = static_cast<int16_t>(incoming.marginTop + lineHeight);
      }

      // The stack accumulates horizontal properties from ancestors. Vertical margins are
      // per-element; merge them only while the placeholder block is still empty.
      const auto style = currentTextBlock->getBlockStyle();
      auto combinedStyle = style.getCombinedBlockStyle(incoming, BlockStyle::CombineAxis::Vertical);
      combinedStyle.fromBrElement = incoming.fromBrElement;
      currentTextBlock->setBlockStyle(combinedStyle);

      flushPendingAnchor();
      return;
    }

    makePages();
  }
  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  flushPendingAnchor();
  currentTextBlock.reset(new (std::nothrow) ParsedText(extraParagraphSpacing, forceParagraphIndents, hyphenationEnabled,
                                                       bionicReadingEnabled, guideReadingEnabled, blockStyle));
  if (!currentTextBlock) {
    const auto heap = MemoryBudget::snapshot();
    LOG_ERR("EHP", "Failed to create text block (%u free, %u max alloc)", heap.freeHeap, heap.maxAllocHeap);
    lowMemoryAbort = true;
    return;
  }
  wordsExtractedInBlock = 0;
}

void ChapterHtmlSlimParser::finalizeCurrentTableCell() {
  if (lowMemoryAbort) {
    return;
  }

  if (tableDepth != 1 || !currentTextBlock) {
    return;
  }

  if (!currentTableBuffer) {
    makePages();
    currentTextBlock.reset();
    pendingFootnotes.clear();
    currentTableCellIsHeader = false;
    currentTableCellColSpan = 1;
    wordsExtractedInBlock = 0;
    nextWordContinues = false;
    return;
  }

  if (currentTableBuffer->rows.empty()) {
    currentTableBuffer->rows.push_back({});
  }

  BufferedTableCell cell;
  cell.isHeader = currentTableCellIsHeader;
  cell.colSpan = currentTableCellColSpan;
  cell.text = std::move(currentTextBlock);
  cell.footnotes = std::move(pendingFootnotes);
  pendingFootnotes.clear();

  if (cell.text && cell.text->size() > MAX_SIMPLE_TABLE_CELL_WORDS) {
    currentTableBuffer->unsupported = true;
  }

  auto& row = currentTableBuffer->rows.back();
  row.hasHeaderCell = row.hasHeaderCell || cell.isHeader;
  row.hasDataCell = row.hasDataCell || !cell.isHeader;
  row.effectiveColumnCount = static_cast<uint16_t>(row.effectiveColumnCount + cell.colSpan);
  row.cells.push_back(std::move(cell));

  currentTableBuffer->totalCells++;
  currentTableBuffer->maxCols = std::max<uint16_t>(currentTableBuffer->maxCols, row.effectiveColumnCount);
  if (currentTableBuffer->totalCells > MAX_SIMPLE_TABLE_CELLS ||
      currentTableBuffer->maxCols > MAX_SIMPLE_TABLE_COLUMNS) {
    currentTableBuffer->unsupported = true;
  }

  currentTableCellIsHeader = false;
  currentTableCellColSpan = 1;
  wordsExtractedInBlock = 0;
  nextWordContinues = false;
  fallbackCurrentTableBufferIfNeeded("cell complete");
}

void ChapterHtmlSlimParser::emitHorizontalRule(const BlockStyle& blockStyle) {
  if (partWordBufferIndex > 0) {
    flushPartWordBuffer();
  }

  if (currentTextBlock) {
    const BlockStyle parentBlockStyle = currentTextBlock->getBlockStyle();
    startNewTextBlock(parentBlockStyle);
  }

  if (!currentPage) {
    if (!startNewPage("horizontal rule")) {
      return;
    }
  }

  const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(fontId) * lineCompression + 0.5f);
  const int16_t defaultVerticalSpacing = static_cast<int16_t>(lineHeight / 2);
  const int16_t topSpacing =
      static_cast<int16_t>((blockStyle.marginTop > 0 ? blockStyle.marginTop : defaultVerticalSpacing) +
                           (blockStyle.paddingTop > 0 ? blockStyle.paddingTop : 0));
  const int16_t bottomSpacing =
      static_cast<int16_t>((blockStyle.marginBottom > 0 ? blockStyle.marginBottom : defaultVerticalSpacing) +
                           (blockStyle.paddingBottom > 0 ? blockStyle.paddingBottom : 0));
  constexpr uint8_t ruleThickness = 2;
  const int16_t availableWidth =
      std::max<int16_t>(1, static_cast<int16_t>(viewportWidth - blockStyle.totalHorizontalInset()));
  const int16_t width = std::max<int16_t>(1, static_cast<int16_t>(availableWidth / 4));
  const int16_t xPos = static_cast<int16_t>(blockStyle.leftInset() + ((availableWidth - width) / 2));
  const int16_t totalHeight = static_cast<int16_t>(topSpacing + ruleThickness + bottomSpacing);

  if (!currentPage->elements.empty() && currentPageNextY + totalHeight > viewportHeight) {
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    if (!startNewPage("horizontal-rule page break")) {
      return;
    }
  }

  currentPageNextY += topSpacing;
  attachPendingPublisherPageMarkers(currentPageNextY);

  auto pageRule = std::shared_ptr<PageHorizontalRule>(
      new (std::nothrow) PageHorizontalRule(width, ruleThickness, xPos, currentPageNextY));
  if (!pageRule) {
    LOG_ERR("EHP", "Failed to create PageHorizontalRule");
    return;
  }
  currentPage->elements.push_back(pageRule);
  currentPageNextY = static_cast<int16_t>(currentPageNextY + ruleThickness + bottomSpacing);

  if (!pendingAnchorId.empty()) {
    anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
    pendingAnchorId.clear();
  }
}

void ChapterHtmlSlimParser::emitBufferedTableAsParagraphs(BufferedTable& table) {
  if (!currentPage) {
    if (!startNewPage("table paragraph fallback")) {
      return;
    }
  }

  if (table.blockStyle.marginTop > 0) {
    currentPageNextY += table.blockStyle.marginTop;
  }
  if (table.blockStyle.paddingTop > 0) {
    currentPageNextY += table.blockStyle.paddingTop;
  }

  for (auto& row : table.rows) {
    for (auto& cell : row.cells) {
      if (!cell.text) {
        continue;
      }

      pendingFootnotes = std::move(cell.footnotes);
      currentTextBlock = std::move(cell.text);
      wordsExtractedInBlock = 0;
      makePages();
      currentTextBlock.reset();
      pendingFootnotes.clear();
      if (lowMemoryAbort) {
        break;
      }
    }
    std::vector<BufferedTableCell>().swap(row.cells);
    if (lowMemoryAbort) {
      break;
    }
  }
  std::vector<BufferedTableRow>().swap(table.rows);
  if (lowMemoryAbort) {
    return;
  }

  if (table.blockStyle.marginBottom > 0) {
    currentPageNextY += table.blockStyle.marginBottom;
  }
  if (table.blockStyle.paddingBottom > 0) {
    currentPageNextY += table.blockStyle.paddingBottom;
  }

  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}

void ChapterHtmlSlimParser::emitBufferedTableAsFragments(BufferedTable& table) {
  struct PreparedRow {
    TableFragmentRow fragmentRow;
    std::vector<FootnoteEntry> footnotes;
  };

  struct PreparedSegment {
    uint8_t columnCount = 0;
    std::vector<PreparedRow> rows;
  };

  if (!currentPage) {
    if (!startNewPage("table fragments")) {
      return;
    }
  }

  const int horizontalInset = table.blockStyle.totalHorizontalInset();
  const uint16_t tableWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
  const uint16_t lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  std::vector<PreparedSegment> preparedSegments;
  preparedSegments.reserve(table.rows.size());

  auto releasePreparedSegments = [&preparedSegments]() {
    for (auto& segment : preparedSegments) {
      for (auto& row : segment.rows) {
        std::vector<TableFragmentCell>().swap(row.fragmentRow.cells);
        std::vector<FootnoteEntry>().swap(row.footnotes);
      }
      std::vector<PreparedRow>().swap(segment.rows);
    }
    std::vector<PreparedSegment>().swap(preparedSegments);
  };

  auto prepareRow = [&](const BufferedTableRow& row, const uint8_t columnCount, PreparedSegment& segment) -> bool {
    const uint16_t baseColumnWidth = columnCount > 0 ? tableWidth / columnCount : 0;
    const uint16_t innerColumnWidth = (baseColumnWidth > TABLE_CELL_PADDING * 2)
                                          ? static_cast<uint16_t>(baseColumnWidth - TABLE_CELL_PADDING * 2)
                                          : 0;
    if (columnCount == 0 || innerColumnWidth < 20) {
      LOG_DBG("EHP", "Table layout fallback: width %u too small for %u columns", tableWidth, columnCount);
      return false;
    }

    PreparedRow prepared;
    prepared.fragmentRow.cells.resize(columnCount);
    prepared.fragmentRow.headerSeparator = row.hasHeaderCell && !row.hasDataCell;

    uint32_t rowHeight = static_cast<uint32_t>(lineHeight) + TABLE_CELL_PADDING * 2;
    if (rowHeight > viewportHeight) {
      LOG_DBG("EHP", "Table layout fallback: row height %lu exceeds viewport %u", static_cast<unsigned long>(rowHeight),
              viewportHeight);
      return false;
    }
    for (size_t colIndex = 0; colIndex < row.cells.size(); colIndex++) {
      const auto& sourceCell = row.cells[colIndex];
      auto& destCell = prepared.fragmentRow.cells[colIndex];
      destCell.isHeader = sourceCell.isHeader;

      if (sourceCell.text) {
        sourceCell.text->layoutAndExtractLines(
            renderer, fontId, innerColumnWidth,
            [&destCell](const std::shared_ptr<TextBlock>& textBlock) { destCell.lines.push_back(textBlock); });
      }

      for (const auto& [wordIndex, footnote] : sourceCell.footnotes) {
        (void)wordIndex;
        prepared.footnotes.push_back(footnote);
      }

      if (destCell.lines.size() > TableFragmentCell::MAX_SERIALIZED_LINES) {
        LOG_DBG("EHP", "Table layout fallback: cell line count %u exceeds fragment max %u",
                static_cast<uint32_t>(destCell.lines.size()), TableFragmentCell::MAX_SERIALIZED_LINES);
        return false;
      }

      const uint32_t cellLineCount = std::max<size_t>(1, destCell.lines.size());
      const uint32_t cellHeight = cellLineCount * lineHeight + TABLE_CELL_PADDING * 2;
      if (cellHeight > viewportHeight) {
        LOG_DBG("EHP", "Table layout fallback: row height %lu exceeds viewport %u",
                static_cast<unsigned long>(cellHeight), viewportHeight);
        return false;
      }

      rowHeight = std::max<uint32_t>(rowHeight, cellHeight);
    }

    prepared.fragmentRow.height = static_cast<uint16_t>(rowHeight);
    segment.rows.push_back(std::move(prepared));
    return true;
  };

  for (const auto& row : table.rows) {
    const bool rowHasMergedCells = std::any_of(row.cells.begin(), row.cells.end(),
                                               [](const BufferedTableCell& cell) { return cell.colSpan != 1; });
    const bool isFullWidthSingleCellRow =
        row.cells.size() == 1 && table.maxCols > 0 && row.cells[0].colSpan == table.maxCols;

    if (rowHasMergedCells && !isFullWidthSingleCellRow) {
      LOG_DBG("EHP", "Table layout fallback: unsupported colspan structure");
      releasePreparedSegments();
      emitBufferedTableAsParagraphs(table);
      return;
    }

    const uint8_t segmentColumnCount = isFullWidthSingleCellRow ? 1 : static_cast<uint8_t>(table.maxCols);
    if (preparedSegments.empty() || preparedSegments.back().columnCount != segmentColumnCount) {
      preparedSegments.push_back({});
      preparedSegments.back().columnCount = segmentColumnCount;
      preparedSegments.back().rows.reserve(table.rows.size());
    }

    if (!prepareRow(row, segmentColumnCount, preparedSegments.back())) {
      releasePreparedSegments();
      emitBufferedTableAsParagraphs(table);
      return;
    }
  }

  if (table.blockStyle.marginTop > 0) {
    currentPageNextY += table.blockStyle.marginTop;
  }
  if (table.blockStyle.paddingTop > 0) {
    currentPageNextY += table.blockStyle.paddingTop;
  }
  for (auto& segment : preparedSegments) {
    size_t nextRowIndex = 0;
    while (nextRowIndex < segment.rows.size()) {
      if (!currentPage) {
        if (!startNewPage("table fragment continuation")) {
          return;
        }
      }

      std::vector<TableFragmentRow> fragmentRows;
      std::vector<FootnoteEntry> fragmentFootnotes;
      fragmentRows.reserve(std::min<size_t>(segment.rows.size() - nextRowIndex, INITIAL_TABLE_FRAGMENT_ROW_RESERVE));
      uint16_t fragmentHeight = 1;  // Bottom border.

      while (nextRowIndex < segment.rows.size()) {
        const uint16_t nextHeight =
            static_cast<uint16_t>(fragmentHeight + segment.rows[nextRowIndex].fragmentRow.height);
        if (!fragmentRows.empty() && currentPageNextY + nextHeight > viewportHeight) {
          break;
        }
        if (fragmentRows.empty() && currentPageNextY + nextHeight > viewportHeight && !currentPage->elements.empty()) {
          completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
          completedPageCount++;
          if (!startNewPage("table fragment page break")) {
            return;
          }
          continue;
        }

        fragmentHeight = nextHeight;
        fragmentRows.push_back(std::move(segment.rows[nextRowIndex].fragmentRow));
        fragmentFootnotes.insert(fragmentFootnotes.end(), segment.rows[nextRowIndex].footnotes.begin(),
                                 segment.rows[nextRowIndex].footnotes.end());
        nextRowIndex++;
      }

      if (fragmentRows.empty()) {
        fragmentHeight = static_cast<uint16_t>(1 + segment.rows[nextRowIndex].fragmentRow.height);
        fragmentRows.push_back(std::move(segment.rows[nextRowIndex].fragmentRow));
        fragmentFootnotes.insert(fragmentFootnotes.end(), segment.rows[nextRowIndex].footnotes.begin(),
                                 segment.rows[nextRowIndex].footnotes.end());
        nextRowIndex++;
      }

      currentPage->elements.push_back(
          std::make_shared<PageTableFragment>(tableWidth, segment.columnCount, TABLE_CELL_PADDING, lineHeight,
                                              std::move(fragmentRows), table.blockStyle.leftInset(), currentPageNextY));
      for (const auto& footnote : fragmentFootnotes) {
        currentPage->addFootnote(footnote.number, footnote.href);
      }
      currentPageNextY += fragmentHeight;

      if (nextRowIndex < segment.rows.size()) {
        completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
        completedPageCount++;
        if (!startNewPage("table fragment split")) {
          return;
        }
      }
    }
  }

  if (table.blockStyle.marginBottom > 0) {
    currentPageNextY += table.blockStyle.marginBottom;
  }
  if (table.blockStyle.paddingBottom > 0) {
    currentPageNextY += table.blockStyle.paddingBottom;
  }

  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}

void ChapterHtmlSlimParser::emitCurrentTableBuffer() {
  if (!currentTableBuffer) {
    return;
  }

  auto table = std::move(currentTableBuffer);
  currentTableCellIsHeader = false;

  if (table->rows.empty() || table->maxCols == 0) {
    return;
  }

  if (table->unsupported) {
    LOG_DBG("EHP", "Table layout fallback: unsupported structure (%u rows, %u cols, %u cells)",
            static_cast<uint32_t>(table->rows.size()), table->maxCols, table->totalCells);
    emitBufferedTableAsParagraphs(*table);
    return;
  }

  emitBufferedTableAsFragments(*table);
}

void ChapterHtmlSlimParser::fallbackCurrentTableBufferToParagraphs(const char* reason) {
  if (!currentTableBuffer) {
    return;
  }

  const auto heap = MemoryBudget::snapshot();
  LOG_DBG("EHP", "Table layout fallback: %s (%u rows, %u cols, %u cells, free=%u, maxAlloc=%u)", reason,
          static_cast<uint32_t>(currentTableBuffer->rows.size()), currentTableBuffer->maxCols,
          currentTableBuffer->totalCells, heap.freeHeap, heap.maxAllocHeap);

  auto activeTextBlock = std::move(currentTextBlock);
  auto activeFootnotes = std::move(pendingFootnotes);
  const int activeWordsExtracted = wordsExtractedInBlock;
  const bool activeNextWordContinues = nextWordContinues;
  const bool activeTableCellIsHeader = currentTableCellIsHeader;
  const uint8_t activeTableCellColSpan = currentTableCellColSpan;

  emitBufferedTableAsParagraphs(*currentTableBuffer);
  currentTableBuffer.reset();

  currentTextBlock = std::move(activeTextBlock);
  pendingFootnotes = std::move(activeFootnotes);
  wordsExtractedInBlock = activeWordsExtracted;
  nextWordContinues = activeNextWordContinues;
  currentTableCellIsHeader = activeTableCellIsHeader;
  currentTableCellColSpan = activeTableCellColSpan;
}

void ChapterHtmlSlimParser::fallbackCurrentTableBufferIfNeeded(const char* stage) {
  if (!currentTableBuffer) {
    return;
  }

  if (currentTableBuffer->unsupported) {
    fallbackCurrentTableBufferToParagraphs(stage);
    return;
  }

  const auto heap = MemoryBudget::snapshot();
  if (!MemoryBudget::hasHeap(heap, MIN_FREE_HEAP_FOR_TABLE_BUFFERING, MIN_MAX_ALLOC_FOR_TABLE_BUFFERING)) {
    fallbackCurrentTableBufferToParagraphs(stage);
  }
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (self->shouldAbortForLowMemory("element start")) {
    return;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  if (strcmp(name, "p") == 0) {
    self->xpathParagraphIndex++;
  }
  if (strcmp(name, "li") == 0) {
    self->xpathListItemIndex++;
  }

  // Extract class, style, and id attributes
  std::string classAttr;
  std::string styleAttr;
  std::string dirAttr;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "class") == 0) {
        classAttr = atts[i + 1];
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = atts[i + 1];
      } else if (strcmp(atts[i], "id") == 0) {
        // Defer both anchor recording and TOC page breaks until startNewTextBlock,
        // after the previous block is flushed to pages via makePages().
        const char* idValue = atts[i + 1];
        const bool isTocAnchor =
            std::find(self->tocAnchors.begin(), self->tocAnchors.end(), idValue) != self->tocAnchors.end();
        if (isTocAnchor || (!isNonNavigableInlineElement(name) && self->anchorData.size() < MAX_ANCHORS_PER_CHAPTER)) {
          self->pendingAnchorId = idValue;
        }
      } else if (strcmp(atts[i], "dir") == 0) {
        dirAttr = atts[i + 1];
      }
    }
  }

  auto centeredBlockStyle = BlockStyle();
  centeredBlockStyle.textAlignDefined = true;
  centeredBlockStyle.alignment = CssTextAlign::Center;

  // Compute CSS style for this element early so display:none can short-circuit
  // before tag-specific branches emit any content or metadata.
  CssStyle cssStyle;
  if (self->cssParser) {
    cssStyle = self->cssParser->resolveStyle(name, classAttr, self->ancestorStack_);
    if (!styleAttr.empty()) {
      CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);
      cssStyle.applyOver(inlineStyle);
    }
    if (self->shouldAbortForLowMemory("CSS style resolution")) {
      return;
    }
  }

  if (!dirAttr.empty()) {
    if (strcasecmp(dirAttr.c_str(), "rtl") == 0) {
      cssStyle.direction = CssTextDirection::Rtl;
      cssStyle.defined.direction = 1;
    } else if (strcasecmp(dirAttr.c_str(), "ltr") == 0) {
      cssStyle.direction = CssTextDirection::Ltr;
      cssStyle.defined.direction = 1;
    }
  }

  if (!cssStyle.hasDirection() && self->effectiveDirectionDefined) {
    cssStyle.direction = self->effectiveDirection;
    cssStyle.defined.direction = 1;
  }

  const char* roleAttr = getAttribute(atts, "role");
  const char* epubTypeAttr = getAttribute(atts, "epub:type");
  const bool isPublisherPageBreak =
      attributeContainsToken(roleAttr, "doc-pagebreak") || attributeContainsToken(epubTypeAttr, "pagebreak");
  if (isPublisherPageBreak) {
    const char* markerLabel = getAttribute(atts, "title");
    if (!markerLabel || markerLabel[0] == '\0') {
      markerLabel = getAttribute(atts, "aria-label");
    }
    self->addPendingPublisherPageMarker(markerLabel);
    self->skipCurrentElement();
    return;
  }

  // Skip elements with display:none before all fast paths (tables, links, etc.).
  if (cssStyle.hasDisplay() && cssStyle.display == CssDisplay::None) {
    const bool isHiddenOrnamentalTextFallback = strcmp(name, "p") == 0 && self->unsupportedOrnamentalBreakDepth >= 0 &&
                                                self->ornamentalBreakDepth == self->unsupportedOrnamentalBreakDepth &&
                                                classContainsToken(classAttr, "ornamental-break-as-text");
    if (!isHiddenOrnamentalTextFallback) {
      self->skipCurrentElement();
      return;
    }
  }

  if (self->ornamentalBreakDepth < 0 && classContainsToken(classAttr, "ornamental-break")) {
    self->ornamentalBreakDepth = self->depth;
    self->unsupportedOrnamentalBreakDepth = -1;
  }

  // Special handling for tables/cells: buffer simple tables for grid layout, with
  // a clean flat-paragraph fallback for anything more complex.
  if (strcmp(name, "table") == 0) {
    // skip nested tables
    if (self->tableDepth > 0) {
      if (self->currentTableBuffer) {
        self->currentTableBuffer->unsupported = true;
        self->fallbackCurrentTableBufferIfNeeded("nested table");
      }
      self->tableDepth += 1;
      return;
    }

    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    const float emSize = self->renderer.getLineHeight(self->fontId) * self->lineCompression;
    auto tableBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Left, self->viewportWidth);

    self->currentTableBuffer.reset(new BufferedTable());
    self->currentTableBuffer->blockStyle = tableBlockStyle;
    self->tableDepth += 1;
    self->tableRowIndex = 0;
    self->tableColIndex = 0;
    self->ancestorStack_.push_back({self->depth, std::string(name), classAttr});
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    self->tableRowIndex += 1;
    self->tableColIndex = 0;
    if (self->currentTableBuffer) {
      self->currentTableBuffer->rows.push_back({});
    }
    self->ancestorStack_.push_back({self->depth, std::string(name), classAttr});
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->tableColIndex += 1;

    if (self->currentTableBuffer && self->currentTableBuffer->rows.empty()) {
      self->currentTableBuffer->rows.push_back({});
    }
    const char* colspan = getAttribute(atts, "colspan");
    const char* rowspan = getAttribute(atts, "rowspan");
    uint8_t parsedColSpan = 1;
    if (colspan && colspan[0] != '\0') {
      char* endPtr = nullptr;
      const long parsedValue = std::strtol(colspan, &endPtr, 10);
      if (endPtr == colspan || *endPtr != '\0' || parsedValue <= 0 || parsedValue > UINT8_MAX) {
        if (self->currentTableBuffer) {
          self->currentTableBuffer->unsupported = true;
          self->fallbackCurrentTableBufferIfNeeded("invalid colspan");
        }
      } else {
        parsedColSpan = static_cast<uint8_t>(parsedValue);
      }
    }
    if (self->currentTableBuffer && rowspan && strcmp(rowspan, "1") != 0) {
      self->currentTableBuffer->unsupported = true;
      self->fallbackCurrentTableBufferIfNeeded("rowspan");
    }
    self->currentTableCellColSpan = parsedColSpan;

    auto tableCellBlockStyle = BlockStyle();
    tableCellBlockStyle.textAlignDefined = true;
    // Default table cells to left alignment so narrow columns don't inherit paragraph
    // justification or other reader-wide alignment settings that damage readability.
    tableCellBlockStyle.alignment = cssStyle.hasTextAlign() ? cssStyle.textAlign : CssTextAlign::Left;
    if (cssStyle.hasDirection()) {
      tableCellBlockStyle.isRtl = cssStyle.direction == CssTextDirection::Rtl;
      tableCellBlockStyle.directionDefined = true;
    }
    self->currentTableCellIsHeader = strcmp(name, "th") == 0;
    if (self->currentTableCellIsHeader) {
      StyleStackEntry headerStyle;
      headerStyle.depth = self->depth;
      headerStyle.hasBold = true;
      headerStyle.bold = true;
      if (cssStyle.hasBackgroundBlack()) {
        headerStyle.hasBackgroundBlack = true;
        headerStyle.backgroundBlack = cssStyle.backgroundBlack;
      }
      ChapterHtmlSlimParser::applyDirectionToEntry(headerStyle, cssStyle);
      self->inlineStyleStack.push_back(headerStyle);
      self->updateEffectiveInlineStyle();
    }
    self->startNewTextBlock(tableCellBlockStyle);

    self->ancestorStack_.push_back({self->depth, std::string(name), classAttr});
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 &&
      (matches(name, HEADER_TAGS, std::size(HEADER_TAGS)) || matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS)))) {
    // Treat block/header tags inside a table cell as transparent wrappers around the
    // cell's text content instead of forcing the whole table back to paragraph mode.
    // This covers common EPUB patterns like <td><p>...</p></td> and
    // <td><h4><em>...</em></h4></td> while still keeping one buffered cell.
    if (strcmp(name, "br") == 0 && self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = false;
    }
    self->ancestorStack_.push_back({self->depth, std::string(name), classAttr});
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    if (self->currentTableBuffer) {
      self->currentTableBuffer->unsupported = true;
      self->fallbackCurrentTableBufferIfNeeded("table image");
    }
    const char* altAttr = getAttribute(atts, "alt");
    if (altAttr && altAttr[0] != '\0') {
      self->characterData(userData, altAttr, strlen(altAttr));
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      self->nextWordContinues = false;
    }
    self->skipCurrentElement();
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "hr") == 0) {
    if (self->currentTableBuffer) {
      self->currentTableBuffer->unsupported = true;
      self->fallbackCurrentTableBufferIfNeeded("table horizontal rule");
    }
    self->ancestorStack_.push_back({self->depth, std::string(name), classAttr});
    self->depth += 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    std::string src;
    std::string alt;
    if (atts != nullptr) {
      bool amznM8Removed = false;
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "src") == 0) {
          src = atts[i + 1];
        } else if (strcmp(atts[i], "alt") == 0) {
          alt = atts[i + 1];
        } else if (strncmp(atts[i], "data-AmznRemoved-M8", 19) == 0) {
          amznM8Removed = true;
        }
      }
      // Skip low-res Kindle fallback images (not intended for modern readers)
      if (amznM8Removed) {
        LOG_DBG("EHP", "Skipping Kindle M8 low-res fallback image");
        self->skipCurrentElement();
        return;
      }

      // imageRendering: 0=display, 1=placeholder (alt text only), 2=suppress entirely
      if (self->imageRendering == 2) {
        self->skipCurrentElement();
        return;
      }

      // Skip image if CSS display:none
      if (self->cssParser) {
        CssStyle imgDisplayStyle = self->cssParser->resolveStyle("img", classAttr, self->ancestorStack_);
        if (!styleAttr.empty()) {
          imgDisplayStyle.applyOver(CssParser::parseInlineStyle(styleAttr));
        }
        if (imgDisplayStyle.hasDisplay() && imgDisplayStyle.display == CssDisplay::None) {
          self->skipCurrentElement();
          return;
        }
      }

      if (!src.empty() && self->imageRendering != 1) {
        LOG_DBG("EHP", "Found image: src=%s", src.c_str());

        {
          const auto releaseHeapBefore = MemoryBudget::snapshot();
          if (MemoryBudget::shouldReleaseSdFontCachesForEpubInlineImage(releaseHeapBefore) &&
              self->renderer.releaseSdCardFontForLowMemory(self->fontId)) {
            const auto releaseHeapAfter = MemoryBudget::snapshot();
            LOG_DBG("EHP", "Released SD font caches before image extraction: free=%u->%u maxAlloc=%u->%u src=%s",
                    releaseHeapBefore.freeHeap, releaseHeapAfter.freeHeap, releaseHeapBefore.maxAllocHeap,
                    releaseHeapAfter.maxAllocHeap, src.c_str());
          }

          const auto heapBeforeImage = MemoryBudget::snapshot();
          LOG_DBG("EHP", "Heap before image extraction: free=%u maxAlloc=%u src=%s", heapBeforeImage.freeHeap,
                  heapBeforeImage.maxAllocHeap, src.c_str());
          if (!self->lowMemoryImageFallback && !MemoryBudget::hasHeapForEpubInlineImage("EHP", src.c_str())) {
            self->lowMemoryImageFallback = true;
          }

          if (self->lowMemoryImageFallback) {
            self->skipCurrentElement();
            return;
          } else {
            // Resolve the image path relative to the HTML file
            std::string resolvedPath = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->contentBase + src));

            const bool supportedFormat = ImageDecoderFactory::isFormatSupported(resolvedPath);
            if (!supportedFormat && alt.empty() && self->ornamentalBreakDepth >= 0) {
              self->unsupportedOrnamentalBreakDepth = self->ornamentalBreakDepth;
            }

            if (supportedFormat) {
              // Create a unique filename for the cached image
              std::string ext;
              size_t extPos = resolvedPath.rfind('.');
              if (extPos != std::string::npos) {
                ext = resolvedPath.substr(extPos);
              }
              std::string cachedImagePath = self->imageBasePath + std::to_string(self->imageCounter++) + ext;

              // Extract image to cache file
              FsFile cachedImageFile;
              bool extractSuccess = false;
              if (Storage.openFileForWrite("EHP", cachedImagePath, cachedImageFile)) {
                extractSuccess =
                    self->epub->readItemContentsToStream(resolvedPath, cachedImageFile, IMAGE_EXTRACT_CHUNK_SIZE);
                cachedImageFile.flush();
                cachedImageFile.close();
                delay(50);  // Give SD card time to sync
              }

              if (extractSuccess) {
                LOG_DBG("EHP", "Heap after image extraction: free=%u maxAlloc=%u path=%s", ESP.getFreeHeap(),
                        ESP.getMaxAllocHeap(), cachedImagePath.c_str());
                // Get image dimensions
                ImageDimensions dims = {0, 0};
                ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedImagePath);
                if (decoder && decoder->getDimensions(cachedImagePath, dims)) {
                  LOG_DBG("EHP", "Image dimensions: %dx%d", dims.width, dims.height);

                  if (!MemoryBudget::hasHeapForEpubInlineImage("EHP", cachedImagePath.c_str())) {
                    self->lowMemoryImageFallback = true;
                    Storage.remove(cachedImagePath.c_str());
                    self->skipCurrentElement();
                    return;
                  }

                  int displayWidth = 0;
                  int displayHeight = 0;
                  const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
                  CssStyle imgStyle = self->cssParser
                                          ? self->cssParser->resolveStyle("img", classAttr, self->ancestorStack_)
                                          : CssStyle{};
                  // Merge inline style (e.g. style="height: 2em") so it overrides stylesheet rules
                  if (!styleAttr.empty()) {
                    imgStyle.applyOver(CssParser::parseInlineStyle(styleAttr));
                  }
                  const bool hasCssHeight = imgStyle.hasImageHeight();
                  const bool hasCssWidth = imgStyle.hasImageWidth();
                  int containerWidth = self->viewportWidth;
                  if (self->currentTextBlock) {
                    const int inset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
                    if (inset > 0 && inset < self->viewportWidth) {
                      containerWidth = self->viewportWidth - inset;
                    }
                  }

                  if (hasCssHeight && hasCssWidth && dims.width > 0 && dims.height > 0) {
                    // Both CSS height and width set: resolve both, then clamp to viewport preserving requested ratio
                    displayHeight = static_cast<int>(
                        imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                    displayWidth = static_cast<int>(
                        imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                    if (displayHeight < 1) displayHeight = 1;
                    if (displayWidth < 1) displayWidth = 1;
                    if (displayWidth > containerWidth || displayHeight > self->viewportHeight) {
                      float scaleX =
                          (displayWidth > containerWidth) ? static_cast<float>(containerWidth) / displayWidth : 1.0f;
                      float scaleY = (displayHeight > self->viewportHeight)
                                         ? static_cast<float>(self->viewportHeight) / displayHeight
                                         : 1.0f;
                      float scale = (scaleX < scaleY) ? scaleX : scaleY;
                      displayWidth = static_cast<int>(displayWidth * scale + 0.5f);
                      displayHeight = static_cast<int>(displayHeight * scale + 0.5f);
                      if (displayWidth < 1) displayWidth = 1;
                      if (displayHeight < 1) displayHeight = 1;
                    }
                    LOG_DBG("EHP", "Display size from CSS height+width: %dx%d", displayWidth, displayHeight);
                  } else if (hasCssHeight && !hasCssWidth && dims.width > 0 && dims.height > 0) {
                    // Use CSS height (resolve % against viewport height) and derive width from aspect ratio
                    displayHeight = static_cast<int>(
                        imgStyle.imageHeight.toPixels(emSize, static_cast<float>(self->viewportHeight)) + 0.5f);
                    if (displayHeight < 1) displayHeight = 1;
                    displayWidth =
                        static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                    if (displayHeight > self->viewportHeight) {
                      displayHeight = self->viewportHeight;
                      // Rescale width to preserve aspect ratio when height is clamped
                      displayWidth =
                          static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                      if (displayWidth < 1) displayWidth = 1;
                    }
                    if (displayWidth > containerWidth) {
                      displayWidth = containerWidth;
                      // Rescale height to preserve aspect ratio when width is clamped
                      displayHeight =
                          static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                      if (displayHeight < 1) displayHeight = 1;
                    }
                    if (displayWidth < 1) displayWidth = 1;
                    LOG_DBG("EHP", "Display size from CSS height: %dx%d", displayWidth, displayHeight);
                  } else if (hasCssWidth && !hasCssHeight && dims.width > 0 && dims.height > 0) {
                    // Use CSS width (resolve % against container width) and derive height from aspect ratio
                    displayWidth = static_cast<int>(
                        imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
                    if (displayWidth > containerWidth) displayWidth = containerWidth;
                    if (displayWidth < 1) displayWidth = 1;
                    displayHeight =
                        static_cast<int>(displayWidth * (static_cast<float>(dims.height) / dims.width) + 0.5f);
                    if (displayHeight > self->viewportHeight) {
                      displayHeight = self->viewportHeight;
                      // Rescale width to preserve aspect ratio when height is clamped
                      displayWidth =
                          static_cast<int>(displayHeight * (static_cast<float>(dims.width) / dims.height) + 0.5f);
                      if (displayWidth < 1) displayWidth = 1;
                    }
                    if (displayHeight < 1) displayHeight = 1;
                    LOG_DBG("EHP", "Display size from CSS width: %dx%d", displayWidth, displayHeight);
                  } else {
                    // Scale to fit container while preserving aspect ratio
                    int maxWidth = containerWidth;
                    int maxHeight = self->viewportHeight;
                    float scaleX = (dims.width > maxWidth) ? (float)maxWidth / dims.width : 1.0f;
                    float scaleY = (dims.height > maxHeight) ? (float)maxHeight / dims.height : 1.0f;
                    float scale = (scaleX < scaleY) ? scaleX : scaleY;
                    if (scale > 1.0f) scale = 1.0f;

                    displayWidth = (int)(dims.width * scale);
                    displayHeight = (int)(dims.height * scale);
                    LOG_DBG("EHP", "Display size: %dx%d (scale %.2f)", displayWidth, displayHeight, scale);
                  }

                  // Flush any pending text block so it appears before the image
                  if (self->partWordBufferIndex > 0) {
                    self->flushPartWordBuffer();
                  }
                  if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
                    const BlockStyle parentBlockStyle = self->currentTextBlock->getBlockStyle();
                    self->startNewTextBlock(parentBlockStyle);
                  }

                  int16_t imageMarginTop = 0;
                  int16_t imageMarginBottom = 0;
                  if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                    const auto& bs = self->currentTextBlock->getBlockStyle();
                    imageMarginTop = bs.topInset();
                    if (self->blockStyleStack.size() > 1) {
                      imageMarginBottom = self->blockStyleStack.back().bottomInset();
                    }
                  }

                  // Create page for image - only break if image won't fit remaining space
                  if (self->currentPage && !self->currentPage->elements.empty() &&
                      (self->currentPageNextY + imageMarginTop + displayHeight + imageMarginBottom >
                       self->viewportHeight)) {
                    self->completePageFn(std::move(self->currentPage), self->xpathParagraphIndex,
                                         self->xpathListItemIndex);
                    self->completedPageCount++;
                    if (!self->startNewPage("image page break")) {
                      return;
                    }
                  } else if (!self->currentPage) {
                    if (!self->startNewPage("image page")) {
                      return;
                    }
                  }

                  self->currentPageNextY += imageMarginTop;
                  self->attachPendingPublisherPageMarkers(self->currentPageNextY);

                  // Create ImageBlock and add to page
                  auto imageBlock = std::shared_ptr<ImageBlock>(
                      new (std::nothrow) ImageBlock(cachedImagePath, displayWidth, displayHeight));
                  if (!imageBlock) {
                    LOG_ERR("EHP", "Failed to create ImageBlock");
                    return;
                  }
                  int xPos = (self->viewportWidth - displayWidth) / 2;
                  auto pageImage = std::shared_ptr<PageImage>(new (std::nothrow)
                                                                  PageImage(imageBlock, xPos, self->currentPageNextY));
                  if (!pageImage) {
                    LOG_ERR("EHP", "Failed to create PageImage");
                    return;
                  }
                  self->currentPage->elements.push_back(pageImage);
                  self->currentPageNextY += displayHeight + imageMarginBottom;

                  if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                    self->currentTextBlock->setBlockStyle(self->blockStyleStack.back().withoutBottom());
                  }

                  self->ancestorStack_.push_back({self->depth, std::string(name), classAttr});
                  self->depth += 1;
                  return;
                } else {
                  LOG_ERR("EHP", "Failed to get image dimensions");
                  Storage.remove(cachedImagePath.c_str());
                }
              } else {
                Storage.remove(cachedImagePath.c_str());
                const uint32_t postFailureFreeHeap = ESP.getFreeHeap();
                const uint32_t postFailureMaxAllocHeap = ESP.getMaxAllocHeap();
                if (!self->lowMemoryImageFallback &&
                    !MemoryBudget::hasHeapForEpubInlineImage("EHP", cachedImagePath.c_str())) {
                  self->lowMemoryImageFallback = true;
                  LOG_ERR("EHP", "Disabling remaining image extraction after failure (%u free, %u max alloc)",
                          postFailureFreeHeap, postFailureMaxAllocHeap);
                }
                LOG_ERR("EHP", "Failed to extract image");
              }
            }  // isFormatSupported
          }
        }
      }

      // Fallback to alt text if image processing fails
      if (!alt.empty()) {
        alt = "[Image: " + alt + "]";
        self->startNewTextBlock(self->blockStyleStack.back()
                                    .getCombinedBlockStyle(centeredBlockStyle, BlockStyle::CombineAxis::Horizontal)
                                    .withoutBottom());
        self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
        self->ancestorStack_.push_back({self->depth, std::string(name), classAttr});
        self->depth += 1;
        self->characterData(userData, alt.c_str(), alt.length());
        self->skipDescendantsOfCurrentElement();
        return;
      }

      // No alt text, skip
      self->skipCurrentElement();
      return;
    }
  }

  if (matches(name, SKIP_TAGS, std::size(SKIP_TAGS))) {
    // start skip
    self->skipCurrentElement();
    return;
  }

  // Detect internal <a href="..."> links (footnotes, cross-references)
  // Note: <aside epub:type="footnote"> elements are rendered as normal content
  // without special handling. Links pointing to them are collected as footnotes.
  if (strcmp(name, "a") == 0) {
    const char* href = getAttribute(atts, "href");

    bool isInternalLink = isInternalEpubLink(href);

    // Special case: javascript:void(0) links with data attributes
    // Example: <a href="javascript:void(0)"
    // data-xyz="{&quot;name&quot;:&quot;OPS/ch2.xhtml&quot;,&quot;frag&quot;:&quot;id46&quot;}">
    if (href && strncmp(href, "javascript:", 11) == 0) {
      isInternalLink = false;
      // TODO: Parse data-* attributes to extract actual href
    }

    if (isInternalLink) {
      // Flush buffer before style change
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      self->insideFootnoteLink = true;
      self->footnoteLinkDepth = self->depth;
      strncpy(self->currentFootnote.href, href, sizeof(self->currentFootnote.href) - 1);
      self->currentFootnote.href[sizeof(self->currentFootnote.href) - 1] = '\0';
      self->currentFootnote.number[0] = '\0';
      self->currentFootnoteLinkTextLen = 0;

      // Apply underline style to visually indicate the link
      self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
      StyleStackEntry entry;
      entry.depth = self->depth;
      entry.hasUnderline = true;
      entry.underline = true;
      if (cssStyle.hasBackgroundBlack()) {
        entry.hasBackgroundBlack = true;
        entry.backgroundBlack = cssStyle.backgroundBlack;
      }
      applyDirectionToEntry(entry, cssStyle);
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();

      // Skip CSS resolution — we already handled styling for this <a> tag
      self->ancestorStack_.push_back({self->depth, std::string(name), classAttr});
      self->depth += 1;
      return;
    }
  }

  const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));

  const CssTextAlign requestedAlign = static_cast<CssTextAlign>(self->paragraphAlignment);
  auto userAlignmentBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, requestedAlign, self->viewportWidth);

  if (!self->embeddedStyle || requestedAlign != CssTextAlign::None) {
    userAlignmentBlockStyle.textAlignDefined = true;
    userAlignmentBlockStyle.alignment = requestedAlign == CssTextAlign::None ? CssTextAlign::Justify : requestedAlign;
  }

  if (!self->embeddedStyle) {
    userAlignmentBlockStyle.marginLeft = 0;
    userAlignmentBlockStyle.marginRight = 0;
    userAlignmentBlockStyle.marginTop = 0;
    userAlignmentBlockStyle.marginBottom = 0;
    userAlignmentBlockStyle.paddingLeft = 0;
    userAlignmentBlockStyle.paddingRight = 0;
    userAlignmentBlockStyle.paddingTop = 0;
    userAlignmentBlockStyle.paddingBottom = 0;
    userAlignmentBlockStyle.textIndentDefined = false;
    userAlignmentBlockStyle.textIndent = 0;
  }

  // Force paragraph indent to prevent unreadable walls of text.
  // This applies if the publisher set text-indent: 0, omitted it, or if it was stripped by disabling embedded styles.
  if (self->forceParagraphIndents && strcmp(name, "p") == 0) {
    static constexpr float forcedIndentEm = 1.0f;
    if (userAlignmentBlockStyle.alignment == CssTextAlign::Left ||
        userAlignmentBlockStyle.alignment == CssTextAlign::Justify ||
        userAlignmentBlockStyle.alignment == CssTextAlign::None) {
      if (!userAlignmentBlockStyle.textIndentDefined || userAlignmentBlockStyle.textIndent == 0) {
        userAlignmentBlockStyle.textIndentDefined = true;
        userAlignmentBlockStyle.textIndent = static_cast<int16_t>(emSize * forcedIndentEm);
      }
    }
  }

  if (strcmp(name, "hr") == 0) {
    auto hrBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Left, self->viewportWidth);
    if (!self->embeddedStyle) {
      hrBlockStyle.marginLeft = 0;
      hrBlockStyle.marginRight = 0;
      hrBlockStyle.marginTop = 0;
      hrBlockStyle.marginBottom = 0;
      hrBlockStyle.paddingLeft = 0;
      hrBlockStyle.paddingRight = 0;
      hrBlockStyle.paddingTop = 0;
      hrBlockStyle.paddingBottom = 0;
      hrBlockStyle.textIndentDefined = false;
      hrBlockStyle.textIndent = 0;
    }
    self->emitHorizontalRule(hrBlockStyle);
    self->ancestorStack_.push_back({self->depth, std::string(name), classAttr});
    self->depth += 1;
    return;
  }

  if (matches(name, HEADER_TAGS, std::size(HEADER_TAGS))) {
    self->currentCssStyle = cssStyle;
    auto headerBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Center, self->viewportWidth);
    headerBlockStyle.textAlignDefined = true;
    if (self->embeddedStyle && cssStyle.hasTextAlign()) {
      headerBlockStyle.alignment = cssStyle.textAlign;
    }
    if (!self->embeddedStyle) {
      headerBlockStyle.marginLeft = 0;
      headerBlockStyle.marginRight = 0;
      headerBlockStyle.marginTop = 0;
      headerBlockStyle.marginBottom = 0;
      headerBlockStyle.paddingLeft = 0;
      headerBlockStyle.paddingRight = 0;
      headerBlockStyle.paddingTop = 0;
      headerBlockStyle.paddingBottom = 0;
      headerBlockStyle.textIndentDefined = false;
      headerBlockStyle.textIndent = 0;
    }
    const auto accumulated =
        self->blockStyleStack.back().getCombinedBlockStyle(headerBlockStyle, BlockStyle::CombineAxis::Horizontal);
    self->blockStyleStack.push_back(accumulated);
    self->startNewTextBlock(accumulated.withoutBottom());
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS))) {
    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      // Tag the new block so startNewTextBlock can inject a full line-height gap if
      // the block remains empty (i.e. <br> is a section separator between paragraphs).
      // If the block gets text added before the next block opens it becomes non-empty,
      // goes through makePages() normally, and the flag has no effect (inline <br> case).
      BlockStyle brStyle = self->blockStyleStack.back().withoutBottom();
      if (self->currentTextBlock) {
        brStyle = self->currentTextBlock->getBlockStyle();
      }
      brStyle.fromBrElement = true;
      self->startNewTextBlock(brStyle);
    } else {
      self->currentCssStyle = cssStyle;
      const auto accumulated = self->blockStyleStack.back().getCombinedBlockStyle(userAlignmentBlockStyle,
                                                                                  BlockStyle::CombineAxis::Horizontal);
      self->blockStyleStack.push_back(accumulated);
      // Common EPUB shape: <li><p>text</p></li>. Keep the first paragraph in the marker block
      // so the auto bullet does not become its own orphaned paragraph.
      const bool reuseListMarkerBlock = strcmp(name, "p") == 0 && self->pendingListMarkerDepth >= 0 &&
                                        self->depth == self->pendingListMarkerDepth + 1 && self->currentTextBlock &&
                                        self->currentTextBlock->size() == 1;
      if (reuseListMarkerBlock) {
        const auto mergedStyle = self->currentTextBlock->getBlockStyle().getCombinedBlockStyle(
            accumulated.withoutBottom(), BlockStyle::CombineAxis::Vertical);
        self->currentTextBlock->setBlockStyle(mergedStyle);
      } else {
        self->startNewTextBlock(accumulated.withoutBottom());
      }
      self->updateEffectiveInlineStyle();

      if (strcmp(name, "li") == 0) {
        self->currentTextBlock->addWord("\xe2\x80\xa2", EpdFontFamily::REGULAR, false, false,
                                        self->effectiveBackgroundBlack);
        self->pendingListMarkerDepth = self->depth;
      }
    }
  } else if (matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->underlineUntilDepth = std::min(self->underlineUntilDepth, self->depth);
    // Push inline style entry for underline tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasUnderline = true;
    entry.underline = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    if (cssStyle.hasBackgroundBlack()) {
      entry.hasBackgroundBlack = true;
      entry.backgroundBlack = cssStyle.backgroundBlack;
    }
    applyDirectionToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, STRIKETHROUGH_TAGS, std::size(STRIKETHROUGH_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->strikethroughUntilDepth = std::min(self->strikethroughUntilDepth, self->depth);
    // Push inline style entry for strikethrough tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasStrikethrough = true;
    entry.strikethrough = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    if (cssStyle.hasBackgroundBlack()) {
      entry.hasBackgroundBlack = true;
      entry.backgroundBlack = cssStyle.backgroundBlack;
    }
    applyDirectionToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BOLD_TAGS, std::size(BOLD_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    // Push inline style entry for bold tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasBold = true;
    entry.bold = true;
    if (cssStyle.hasFontStyle()) {
      entry.hasItalic = true;
      entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
    }
    if (cssStyle.hasTextDecoration()) {
      entry.hasUnderline = true;
      entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
      entry.hasStrikethrough = true;
      entry.strikethrough = cssStyle.textDecoration == CssTextDecoration::LineThrough;
    }
    if (cssStyle.hasBackgroundBlack()) {
      entry.hasBackgroundBlack = true;
      entry.backgroundBlack = cssStyle.backgroundBlack;
    }
    applyDirectionToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS))) {
    // Flush buffer before style change so preceding text gets current style
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    // Push inline style entry for italic tag
    StyleStackEntry entry;
    entry.depth = self->depth;  // Track depth for matching pop
    entry.hasItalic = true;
    entry.italic = true;
    if (cssStyle.hasFontWeight()) {
      entry.hasBold = true;
      entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
    }
    if (cssStyle.hasTextDecoration()) {
      entry.hasUnderline = true;
      entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
      entry.hasStrikethrough = true;
      entry.strikethrough = cssStyle.textDecoration == CssTextDecoration::LineThrough;
    }
    if (cssStyle.hasBackgroundBlack()) {
      entry.hasBackgroundBlack = true;
      entry.backgroundBlack = cssStyle.backgroundBlack;
    }
    applyDirectionToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "sup") == 0 || strcmp(name, "sub") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    StyleStackEntry entry;
    entry.depth = self->depth;
    if (strcmp(name, "sup") == 0) {
      entry.hasSup = true;
      entry.sup = true;
    } else {
      entry.hasSub = true;
      entry.sub = true;
    }
    ChapterHtmlSlimParser::applyDirectionToEntry(entry, cssStyle);
    self->inlineStyleStack.push_back(entry);
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    // Handle span and other inline elements for CSS styling
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration() ||
        cssStyle.hasBackgroundBlack() || cssStyle.hasVerticalAlign() || cssStyle.hasDirection()) {
      // Flush buffer before style change so preceding text gets current style
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
      StyleStackEntry entry;
      entry.depth = self->depth;  // Track depth for matching pop
      if (cssStyle.hasFontWeight()) {
        entry.hasBold = true;
        entry.bold = cssStyle.fontWeight == CssFontWeight::Bold;
      }
      if (cssStyle.hasFontStyle()) {
        entry.hasItalic = true;
        entry.italic = cssStyle.fontStyle == CssFontStyle::Italic;
      }
      if (cssStyle.hasTextDecoration()) {
        entry.hasUnderline = true;
        entry.underline = cssStyle.textDecoration == CssTextDecoration::Underline;
        entry.hasStrikethrough = true;
        entry.strikethrough = cssStyle.textDecoration == CssTextDecoration::LineThrough;
      }
      if (cssStyle.hasBackgroundBlack()) {
        entry.hasBackgroundBlack = true;
        entry.backgroundBlack = cssStyle.backgroundBlack;
      }
      applyDirectionToEntry(entry, cssStyle);
      if (cssStyle.hasVerticalAlign()) {
        if (cssStyle.verticalAlign == CssVerticalAlign::Super) {
          entry.hasSup = true;
          entry.sup = true;
        } else if (cssStyle.verticalAlign == CssVerticalAlign::Sub) {
          entry.hasSub = true;
          entry.sub = true;
        }
      }
      ChapterHtmlSlimParser::applyDirectionToEntry(entry, cssStyle);
      self->inlineStyleStack.push_back(entry);
      self->updateEffectiveInlineStyle();
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->ancestorStack_.push_back({self->depth, std::string(name), classAttr});
  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (self->tableDepth == 1) {
    self->fallbackCurrentTableBufferIfNeeded("low heap while buffering table");
  }
  if (self->shouldAbortForLowMemory("character data")) {
    return;
  }

  // Skip content of nested table
  if (self->tableDepth > 1) {
    return;
  }

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // Collect footnote link display text (for the number label)
  // Skip whitespace and brackets to normalize noterefs like "[1]" → "1"
  if (self->insideFootnoteLink) {
    int start = 0;
    int end = len - 1;

    // Example input and output texts:
    // "     [  12  ]   " => "12"
    // "   turn to 256  " => "turn to 256"

    // Ignore leading whitespaces and left square brackets
    while (start < len && (isWhitespace(s[start]) || (s[start] == '['))) {
      ++start;
    }

    // Ignore trailing whitespaces and right square brackets
    while (end >= start && (isWhitespace(s[end]) || (s[end] == ']'))) {
      --end;
    }

    // Extract footnote link text
    for (int i = start; (self->currentFootnoteLinkTextLen < sizeof(self->currentFootnote.number) - 1) && (i <= end);
         ++i) {
      self->currentFootnote.number[self->currentFootnoteLinkTextLen++] = s[i];
    }
    self->currentFootnote.number[self->currentFootnoteLinkTextLen] = '\0';
  }

  for (int i = 0; i < len; i++) {
    if (isWhitespace(s[i])) {
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      // Whitespace is a real word boundary — reset continuation state
      self->nextWordContinues = false;
      // Skip the whitespace char
      continue;
    }

    // Detect U+00A0 (non-breaking space, UTF-8: 0xC2 0xA0) or
    //        U+202F (narrow no-break space, UTF-8: 0xE2 0x80 0xAF).
    //
    // Both are rendered as a visible space but must never allow a line break around them.
    // We split the no-break space into its own word token and link the surrounding words
    // with continuation flags so the layout engine treats them as an indivisible group.
    //
    // Example: "200&#xA0;Quadratkilometer" or "200&#x202F;Quadratkilometer"
    //   Input bytes:  "200\xC2\xA0Quadratkilometer"  (or 0xE2 0x80 0xAF for U+202F)
    //   Tokens produced:
    //     [0] "200"               continues=false
    //     [1] " "                 continues=true   (attaches to "200", no gap)
    //     [2] "Quadratkilometer"  continues=true   (attaches to " ", no gap)
    //
    //   The continuation flags prevent the line-breaker from inserting a line break
    //   between "200" and "Quadratkilometer". However, "Quadratkilometer" is now a
    //   standalone word for hyphenation purposes, so Liang patterns can produce
    //   "200 Quadrat-" / "kilometer" instead of the unusable "200" / "Quadratkilometer".
    if (static_cast<uint8_t>(s[i]) == 0xC2 && i + 1 < len && static_cast<uint8_t>(s[i + 1]) == 0xA0) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->nextWordContinues = true;  // Attach space to previous word (no break).
      self->flushPartWordBuffer();

      self->nextWordContinues = true;  // Next real word attaches to this space (no break).

      i++;  // Skip the second byte (0xA0)
      continue;
    }

    // U+202F (narrow no-break space) — identical logic to U+00A0 above.
    if (static_cast<uint8_t>(s[i]) == 0xE2 && i + 2 < len && static_cast<uint8_t>(s[i + 1]) == 0x80 &&
        static_cast<uint8_t>(s[i + 2]) == 0xAF) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }

      self->partWordBuffer[0] = ' ';
      self->partWordBuffer[1] = '\0';
      self->partWordBufferIndex = 1;
      self->nextWordContinues = true;
      self->flushPartWordBuffer();

      self->nextWordContinues = true;

      i += 2;  // Skip the remaining two bytes (0x80 0xAF)
      continue;
    }

    // Skip Zero Width No-Break Space / BOM (U+FEFF) = 0xEF 0xBB 0xBF
    const XML_Char FEFF_BYTE_1 = static_cast<XML_Char>(0xEF);
    const XML_Char FEFF_BYTE_2 = static_cast<XML_Char>(0xBB);
    const XML_Char FEFF_BYTE_3 = static_cast<XML_Char>(0xBF);

    if (s[i] == FEFF_BYTE_1) {
      // Check if the next two bytes complete the 3-byte sequence
      if ((i + 2 < len) && (s[i + 1] == FEFF_BYTE_2) && (s[i + 2] == FEFF_BYTE_3)) {
        // Sequence 0xEF 0xBB 0xBF found!
        i += 2;    // Skip the next two bytes
        continue;  // Move to the next iteration
      }
    }

    // If we're about to run out of space, then cut the word off and start a new one.
    // For CJK text (no spaces), this is the primary word-breaking mechanism.
    // We must avoid splitting multi-byte UTF-8 sequences across word boundaries,
    // otherwise the trailing bytes become orphaned continuation bytes that the
    // decoder can't interpret.
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      int safeLen = utf8SafeTruncateBuffer(self->partWordBuffer, self->partWordBufferIndex);

      if (safeLen < self->partWordBufferIndex && safeLen > 0) {
        // Incomplete UTF-8 sequence at the end — save it before flushing
        int overflow = self->partWordBufferIndex - safeLen;
        char saved[4];
        for (int j = 0; j < overflow; j++) {
          saved[j] = self->partWordBuffer[safeLen + j];
        }
        self->partWordBufferIndex = safeLen;
        self->flushPartWordBuffer();
        for (int j = 0; j < overflow; j++) {
          self->partWordBuffer[j] = saved[j];
        }
        self->partWordBufferIndex = overflow;
      } else {
        self->flushPartWordBuffer();
      }
    }

    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
  }

  // If a paragraph keeps growing, perform the layout and consume all but the last line.
  // This turns the parser's text buffer into page records earlier, which keeps memory
  // bounded for chapters with very long XHTML text runs.
  // Spotted when reading Intermezzo, there are some really long text blocks in there.
  if (self->currentTextBlock && self->currentTextBlock->size() > MAX_BUFFERED_WORDS_BEFORE_LAYOUT) {
    LOG_DBG("EHP", "Text block too long, splitting into multiple pages");
    const int horizontalInset = self->currentTextBlock->getBlockStyle().totalHorizontalInset();
    const uint16_t effectiveWidth = (horizontalInset < self->viewportWidth)
                                        ? static_cast<uint16_t>(self->viewportWidth - horizontalInset)
                                        : self->viewportWidth;
    self->currentTextBlock->layoutAndExtractLines(
        self->renderer, self->fontId, effectiveWidth,
        [self](const std::shared_ptr<TextBlock>& textBlock) { self->addLineToPage(textBlock); }, false);
  }
}

void XMLCALL ChapterHtmlSlimParser::defaultHandlerExpand(void* userData, const XML_Char* s, const int len) {
  // Check if this looks like an entity reference (&...;)
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    const char* utf8Value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (utf8Value != nullptr) {
      // Known entity: expand to its UTF-8 value
      characterData(userData, utf8Value, strlen(utf8Value));
      return;
    }
    // Unknown entity: preserve original &...; sequence
    characterData(userData, s, len);
    return;
  }
  // Not an entity we recognize - skip it
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (self->lowMemoryAbort) {
    return;
  }

  if (self->skipEndElementStateUntilDepth < self->depth) {
    self->depth -= 1;
    if (self->skipUntilDepth == self->depth) {
      self->skipUntilDepth = INT_MAX;
      self->skipEndElementStateUntilDepth = INT_MAX;
    }
    return;
  }

  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      !self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth - 1;
  const bool willClearBold = self->boldUntilDepth == self->depth - 1;
  const bool willClearItalic = self->italicUntilDepth == self->depth - 1;
  const bool willClearUnderline = self->underlineUntilDepth == self->depth - 1;
  const bool willClearStrikethrough = self->strikethroughUntilDepth == self->depth - 1;

  const bool styleWillChange =
      willPopStyleStack || willClearBold || willClearItalic || willClearUnderline || willClearStrikethrough;
  const bool headerOrBlockTag = isHeaderOrBlock(name);
  const bool tableStructuralTag = isTableStructuralTag(name);

  if (self->tableDepth > 1 && strcmp(name, "table") == 0) {
    // get rid of all text inside the nested table
    self->partWordBufferIndex = 0;
    self->tableDepth -= 1;
    LOG_DBG("EHP", "nested table detected, get rid of its content");
    return;
  }

  // Flush buffer with current style BEFORE any style changes
  if (self->partWordBufferIndex > 0) {
    // Flush if style will change OR if we're closing a block/structural element
    const bool isInlineTag = !headerOrBlockTag && !tableStructuralTag &&
                             !matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) && self->depth != 1;
    const bool shouldFlush = styleWillChange || headerOrBlockTag || matches(name, BOLD_TAGS, std::size(BOLD_TAGS)) ||
                             matches(name, ITALIC_TAGS, std::size(ITALIC_TAGS)) ||
                             matches(name, UNDERLINE_TAGS, std::size(UNDERLINE_TAGS)) ||
                             matches(name, STRIKETHROUGH_TAGS, std::size(STRIKETHROUGH_TAGS)) || tableStructuralTag ||
                             matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS)) || self->depth == 1;

    if (shouldFlush) {
      self->flushPartWordBuffer();
      // If closing an inline element, the next word fragment continues the same visual word
      if (isInlineTag) {
        self->nextWordContinues = true;
      }
    }
  }

  self->depth -= 1;

  // Pop ancestor entries that were pushed at or below the new depth
  while (!self->ancestorStack_.empty() && self->ancestorStack_.back().depth >= self->depth) {
    self->ancestorStack_.pop_back();
  }

  // Closing a footnote link — create entry from collected text and href
  if (self->insideFootnoteLink && self->depth == self->footnoteLinkDepth) {
    if (self->currentFootnote.number[0] != '\0' && self->currentFootnote.href[0] != '\0') {
      FootnoteEntry entry;
      strncpy(entry.number, self->currentFootnote.number, sizeof(entry.number) - 1);
      entry.number[sizeof(entry.number) - 1] = '\0';
      strncpy(entry.href, self->currentFootnote.href, sizeof(entry.href) - 1);
      entry.href[sizeof(entry.href) - 1] = '\0';
      int wordIndex =
          self->wordsExtractedInBlock + (self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0);
      self->pendingFootnotes.push_back({wordIndex, entry});
    }
    self->insideFootnoteLink = false;
  }

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
    self->skipEndElementStateUntilDepth = INT_MAX;
  }

  if (self->ornamentalBreakDepth == self->depth) {
    self->ornamentalBreakDepth = -1;
    self->unsupportedOrnamentalBreakDepth = -1;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    self->finalizeCurrentTableCell();
    self->nextWordContinues = false;
  }

  if (self->tableDepth == 1 && (strcmp(name, "tr") == 0)) {
    self->nextWordContinues = false;
  }

  if (self->tableDepth == 1 && strcmp(name, "table") == 0) {
    self->emitCurrentTableBuffer();
    self->tableDepth -= 1;
    self->tableRowIndex = 0;
    self->tableColIndex = 0;
    auto paragraphAlignmentBlockStyle = BlockStyle();
    paragraphAlignmentBlockStyle.textAlignDefined = true;
    paragraphAlignmentBlockStyle.alignment = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                                 ? CssTextAlign::Justify
                                                 : static_cast<CssTextAlign>(self->paragraphAlignment);
    self->startNewTextBlock(paragraphAlignmentBlockStyle);
    self->nextWordContinues = false;
  }

  if (strcmp(name, "li") == 0 && self->pendingListMarkerDepth == self->depth) {
    self->pendingListMarkerDepth = -1;
  }

  // Leaving bold tag
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic tag
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }

  // Leaving underline tag
  if (self->underlineUntilDepth == self->depth) {
    self->underlineUntilDepth = INT_MAX;
  }

  // Leaving strikethrough tag
  if (self->strikethroughUntilDepth == self->depth) {
    self->strikethroughUntilDepth = INT_MAX;
  }

  // Pop from inline style stack if we pushed an entry at this depth
  // This handles all inline elements: b, i, u, span, etc.
  if (!self->inlineStyleStack.empty() && self->inlineStyleStack.back().depth == self->depth) {
    self->inlineStyleStack.pop_back();
    self->updateEffectiveInlineStyle();
  }

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag) {
    self->currentCssStyle.reset();
    self->updateEffectiveInlineStyle();

    // br is self-closing and not a container — it doesn't push/pop the stack.
    if (strcmp(name, "br") != 0 && self->blockStyleStack.size() > 1) {
      // Apply closing element's bottom margin to the current text block so
      // container spacing appears after the element's content (on the last child),
      // not on the first child via the empty-block merge in startNewTextBlock.
      if (self->currentTextBlock) {
        const auto style = self->currentTextBlock->getBlockStyle();
        self->currentTextBlock->setBlockStyle(style.addBottom(self->blockStyleStack.back()));
      }
      self->blockStyleStack.pop_back();
    }
  }
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  // Initialize block style stack with a root entry representing "no ancestor block elements".
  // The user's paragraph alignment is set as the default so child elements without explicit
  // text-align inherit it correctly through getCombinedBlockStyle.
  BlockStyle rootBlockStyle;
  rootBlockStyle.alignment = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                 ? CssTextAlign::Justify
                                 : static_cast<CssTextAlign>(this->paragraphAlignment);
  blockStyleStack.clear();
  blockStyleStack.reserve(8);
  blockStyleStack.push_back(rootBlockStyle);

  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  const auto align = rootBlockStyle.alignment;
  paragraphAlignmentBlockStyle.alignment = align;
  startNewTextBlock(paragraphAlignmentBlockStyle);

  ancestorStack_.reserve(32);

  XML_Parser parser = XML_ParserCreate(nullptr);
  int done;

  if (!parser) {
    LOG_ERR("EHP", "Couldn't allocate memory for parser");
    return false;
  }

  // Handle HTML entities (like &nbsp;) that aren't in XML spec or DTD
  // Using DefaultHandlerExpand preserves normal entity expansion from DOCTYPE
  XML_SetDefaultHandlerExpand(parser, defaultHandlerExpand);

  FsFile file;
  if (!Storage.openFileForRead("EHP", filepath, file)) {
    destroyXmlParser(parser);
    return false;
  }

  // Get file size to decide whether to show indexing popup.
  if (popupFn && file.size() >= MIN_SIZE_FOR_POPUP) {
    popupFn();
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  // Compute the time taken to parse and build pages
  const uint32_t chapterStartTime = millis();
  do {
    void* const buf = XML_GetBuffer(parser, PARSE_BUFFER_SIZE);
    if (!buf) {
      LOG_ERR("EHP", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      file.close();
      return false;
    }

    const size_t len = file.read(buf, PARSE_BUFFER_SIZE);

    if (len == 0 && file.available() > 0) {
      LOG_ERR("EHP", "File read error");
      destroyXmlParser(parser);
      file.close();
      return false;
    }

    done = file.available() == 0;

    if (XML_ParseBuffer(parser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      LOG_ERR("EHP", "Parse error at line %lu:\n%s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      file.close();
      return false;
    }

    if (lowMemoryAbort) {
      LOG_ERR("EHP", "Aborting section parse due to low heap");
      destroyXmlParser(parser);
      file.close();
      return false;
    }

  } while (!done);
  LOG_DBG("EHP", "Time to parse and build pages: %lu ms (free=%u, maxAlloc=%u)", millis() - chapterStartTime,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  destroyXmlParser(parser);
  file.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    if (shouldAbortForLowMemory("final page layout")) {
      return false;
    }
    makePages();
    if (lowMemoryAbort) {
      return false;
    }
    if (!pendingAnchorId.empty()) {
      anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
      pendingAnchorId.clear();
    }
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    currentPage.reset();
    currentTextBlock.reset();
  }

  return true;
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line) {
  if (lowMemoryAbort) {
    return;
  }

  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  if (!currentPage) {
    if (!startNewPage("line layout")) {
      return;
    }
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    completePageFn(std::move(currentPage), xpathParagraphIndex, xpathListItemIndex);
    completedPageCount++;
    if (!startNewPage("line page break")) {
      return;
    }
  }

  // Track cumulative words to assign footnotes to the page containing their anchor
  wordsExtractedInBlock += line->wordCount();
  auto footnoteIt = pendingFootnotes.begin();
  while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
    currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href);
    ++footnoteIt;
  }
  pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);
  attachPendingPublisherPageMarkers(currentPageNextY);

  // Apply horizontal left inset (margin + padding) as x position offset
  const int16_t xOffset = line->getBlockStyle().leftInset();
  currentPage->elements.push_back(std::make_shared<PageLine>(line, xOffset, currentPageNextY));
  currentPageNextY += lineHeight;
}

void ChapterHtmlSlimParser::makePages() {
  if (shouldAbortForLowMemory("page layout")) {
    return;
  }

  if (!currentTextBlock) {
    LOG_ERR("EHP", "!! No text block to make pages for !!");
    return;
  }

  if (!currentPage) {
    if (!startNewPage("page layout")) {
      return;
    }
  }

  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;

  // Apply top spacing before the paragraph (stored in pixels)
  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  if (blockStyle.marginTop > 0) {
    currentPageNextY += blockStyle.marginTop;
  }
  if (blockStyle.paddingTop > 0) {
    currentPageNextY += blockStyle.paddingTop;
  }

  // Calculate effective width accounting for horizontal margins/padding
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  currentTextBlock->layoutAndExtractLines(
      renderer, fontId, effectiveWidth,
      [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); });
  if (lowMemoryAbort) {
    return;
  }

  // Fallback: transfer any remaining pending footnotes to current page.
  // Normally addLineToPage handles this via word-index tracking, but this catches
  // edge cases where a footnote's word index equals the exact block size.
  if (!pendingFootnotes.empty() && currentPage) {
    for (const auto& [idx, fn] : pendingFootnotes) {
      currentPage->addFootnote(fn.number, fn.href);
    }
    pendingFootnotes.clear();
  }
  attachPendingPublisherPageMarkers(currentPageNextY);

  // Apply bottom spacing after the paragraph (stored in pixels)
  if (blockStyle.marginBottom > 0) {
    currentPageNextY += blockStyle.marginBottom;
  }
  if (blockStyle.paddingBottom > 0) {
    currentPageNextY += blockStyle.paddingBottom;
  }

  // Extra paragraph spacing if enabled (default behavior)
  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}
