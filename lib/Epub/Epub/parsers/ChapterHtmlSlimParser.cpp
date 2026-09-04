#include "ChapterHtmlSlimParser.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <Utf8.h>
#include <XmlParserUtils.h>
#include <expat.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <new>
#include <string_view>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageDimsProbe.h"
#include "Epub/converters/ImageToFramebufferDecoder.h"
#include "Epub/htmlEntities.h"
#include "Epub/tables/TableColumnLayout.h"
#include "PreviewBlockLocator.h"

// Minimum file size (in bytes) to show indexing popup - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_POPUP = 10 * 1024;  // 10KB
constexpr size_t PARSE_BUFFER_SIZE = 1024;
// Initial slab for the parse arena. Covers both style stacks (~2 KB) with headroom for growth.
constexpr size_t PARSE_ARENA_SLAB_SIZE = 4 * 1024;
constexpr size_t DEFAULT_BUFFERED_WORDS_BEFORE_LAYOUT = 350;
constexpr size_t CSS_BUFFERED_WORDS_BEFORE_LAYOUT = 320;
constexpr uint16_t DEFAULT_TEXT_RUN_BYTES_BEFORE_LAYOUT = 2048;
constexpr size_t SINGLE_READING_AID_BUFFERED_WORDS_BEFORE_LAYOUT = 240;
constexpr uint16_t SINGLE_READING_AID_TEXT_RUN_BYTES_BEFORE_LAYOUT = 1536;
constexpr size_t COMBINED_READING_AID_BUFFERED_WORDS_BEFORE_LAYOUT = 175;
constexpr uint16_t COMBINED_READING_AID_TEXT_RUN_BYTES_BEFORE_LAYOUT = 1024;
constexpr size_t SECTION_ADVANCE_PREWARM_READ_BUFFER_SIZE = 512;
constexpr uint32_t SECTION_ADVANCE_PREWARM_MAX_CODEPOINTS = 4096;
// The whole-section advance prewarm is a batch-I/O optimization, not a requirement:
// layout falls back to per-paragraph advance loads (ParsedText loads glyph metrics per
// block, masked to the styles that block uses). Running it costs a 16KB contiguous
// codepoint scratch array plus the resident per-style advance tables (~30KB total
// observed), on top of the 44KB layout floor. 80KB keeps the prewarm for ordinary
// chapter opens (observed surviving from 81KB free) and skips it for low-heap
// extension starts (observed aborting the build when run from 70KB free).
constexpr uint32_t MIN_FREE_HEAP_FOR_SECTION_PREWARM = 80 * 1024;
constexpr uint32_t MIN_MAX_ALLOC_FOR_SECTION_PREWARM = 24 * 1024;
constexpr uint8_t INITIAL_PAGE_ELEMENT_RESERVE = 8;
constexpr uint8_t INITIAL_TABLE_FRAGMENT_ROW_RESERVE = 8;
constexpr uint32_t PAGE_ELEMENT_RESERVE_MIN_MAX_ALLOC = 1024;
constexpr uint16_t WARNING_PAGE_SIDE_MARGIN = 24;
constexpr uint8_t WARNING_TITLE_MAX_LINES = 2;
constexpr uint8_t WARNING_BODY_MAX_LINES = 6;
// Cap chapter anchors so converter-generated IDs do not grow memory without bound.
constexpr size_t MAX_ANCHORS_PER_CHAPTER = 1024;
constexpr size_t MAX_PENDING_FOOTNOTES_BEFORE_LAYOUT = Page::MAX_FOOTNOTES_PER_PAGE * 3;
// Rich tables retain ParsedText and per-cell vectors until the closing tag. The
// C3 SD-font resume path normally has only about 85-90 KiB free and a 49 KiB
// largest block, so leave that baseline to the compact row model as well.
// Select before the first cell is captured; never build both representations
// and retry after an OOM.
constexpr uint32_t MIN_FREE_HEAP_FOR_RICH_TABLE = 96U * 1024U;
constexpr uint32_t MIN_MAX_ALLOC_FOR_RICH_TABLE = 56U * 1024U;

static constexpr const char* const HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
static constexpr const char* const BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote"};
static constexpr const char* const BOLD_TAGS[] = {"b", "strong"};
static constexpr const char* const ITALIC_TAGS[] = {"i", "em"};
static constexpr const char* const UNDERLINE_TAGS[] = {"u", "ins"};
static constexpr const char* const STRIKETHROUGH_TAGS[] = {"s", "strike", "del"};
static constexpr const char* const IMAGE_TAGS[] = {"img", "image"};
static constexpr const char* const SKIP_TAGS[] = {"head", "style", "script", "title", "rp", "rt"};

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

static char asciiLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool isSvgImagePath(const std::string_view path) {
  const size_t end = path.find_first_of("?#");
  const std::string_view cleanPath = end == std::string_view::npos ? path : path.substr(0, end);
  const size_t dot = cleanPath.rfind('.');
  if (dot == std::string_view::npos) return false;

  const std::string_view ext = cleanPath.substr(dot);
  if (ext.size() == 4) {
    return asciiLower(ext[0]) == '.' && asciiLower(ext[1]) == 's' && asciiLower(ext[2]) == 'v' &&
           asciiLower(ext[3]) == 'g';
  }
  if (ext.size() == 5) {
    return asciiLower(ext[0]) == '.' && asciiLower(ext[1]) == 's' && asciiLower(ext[2]) == 'v' &&
           asciiLower(ext[3]) == 'g' && asciiLower(ext[4]) == 'z';
  }
  return false;
}

bool appendUniquePrewarmCodepoint(const uint32_t cp, uint32_t* codepoints, uint32_t& cpCount, const uint32_t maxCount) {
  if (cp == 0) return false;
  for (uint32_t i = 0; i < cpCount; ++i) {
    if (codepoints[i] == cp) return false;
  }
  if (cpCount >= maxCount) return true;
  codepoints[cpCount++] = cp;
  return false;
}

void resetPrewarmUtf8(uint32_t& accumulator, uint8_t& remaining) {
  accumulator = 0;
  remaining = 0;
}

bool feedPrewarmUtf8Byte(const uint8_t byte, uint32_t* codepoints, uint32_t& cpCount, uint32_t& accumulator,
                         uint8_t& remaining) {
  if (remaining == 0) {
    if (byte < 0x80) {
      return appendUniquePrewarmCodepoint(byte, codepoints, cpCount, SECTION_ADVANCE_PREWARM_MAX_CODEPOINTS);
    }
    if ((byte & 0xE0) == 0xC0) {
      accumulator = byte & 0x1F;
      remaining = 1;
    } else if ((byte & 0xF0) == 0xE0) {
      accumulator = byte & 0x0F;
      remaining = 2;
    } else if ((byte & 0xF8) == 0xF0) {
      accumulator = byte & 0x07;
      remaining = 3;
    } else {
      return appendUniquePrewarmCodepoint(REPLACEMENT_GLYPH, codepoints, cpCount,
                                          SECTION_ADVANCE_PREWARM_MAX_CODEPOINTS);
    }
    return false;
  }

  if ((byte & 0xC0) != 0x80) {
    resetPrewarmUtf8(accumulator, remaining);
    return appendUniquePrewarmCodepoint(REPLACEMENT_GLYPH, codepoints, cpCount, SECTION_ADVANCE_PREWARM_MAX_CODEPOINTS);
  }

  accumulator = (accumulator << 6U) | (byte & 0x3FU);
  --remaining;
  if (remaining == 0) {
    const uint32_t cp = accumulator;
    accumulator = 0;
    return appendUniquePrewarmCodepoint(cp, codepoints, cpCount, SECTION_ADVANCE_PREWARM_MAX_CODEPOINTS);
  }
  return false;
}

static bool tokenEqualsIgnoreCase(const char* value, const char* token, const size_t tokenLen) {
  for (size_t i = 0; i < tokenLen; ++i) {
    if (asciiLower(value[i]) != asciiLower(token[i])) return false;
  }
  return true;
}

std::string trimAndNormalize(const std::string& str) {
  if (str.empty()) return "";
  size_t start = 0;
  while (start < str.size() && isWhitespace(str[start])) {
    start++;
  }
  if (start == str.size()) return "";
  size_t end = str.size() - 1;
  while (end > start && isWhitespace(str[end])) {
    end--;
  }
  std::string result;
  result.reserve(end - start + 1);
  bool inSpace = false;
  for (size_t i = start; i <= end; i++) {
    if (isWhitespace(str[i])) {
      if (!inSpace) {
        result.push_back(' ');
        inSpace = true;
      }
    } else {
      result.push_back(str[i]);
      inSpace = false;
    }
  }
  return result;
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

bool isHeaderOrBlock(const char* name) {
  return matches(name, HEADER_TAGS, std::size(HEADER_TAGS)) || matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS)) ||
         strcmp(name, "caption") == 0;
}

bool isTableStructuralTag(const char* name) {
  return strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 || strcmp(name, "td") == 0 || strcmp(name, "th") == 0;
}

void stripPublisherSpacing(BlockStyle& blockStyle) {
  blockStyle.marginLeft = 0;
  blockStyle.marginRight = 0;
  blockStyle.marginTop = 0;
  blockStyle.marginBottom = 0;
  blockStyle.paddingLeft = 0;
  blockStyle.paddingRight = 0;
  blockStyle.paddingTop = 0;
  blockStyle.paddingBottom = 0;
  blockStyle.textIndentDefined = false;
  blockStyle.textIndent = 0;
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

void ChapterHtmlSlimParser::applySmallCapsToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (css.hasFontVariantCaps()) {
    entry.hasSmallCaps = true;
    entry.smallCaps = css.fontVariantCaps == CssFontVariantCaps::SmallCaps;
  }
}

void ChapterHtmlSlimParser::applyVerticalAlignToEntry(StyleStackEntry& entry, const CssStyle& css) {
  if (!css.hasVerticalAlign()) return;
  if (css.verticalAlign == CssVerticalAlign::Super) {
    entry.hasSup = true;
    entry.sup = true;
  } else if (css.verticalAlign == CssVerticalAlign::Sub) {
    entry.hasSub = true;
    entry.sub = true;
  }
}

// Update effective bold/italic/underline based on block style and inline style stack
void ChapterHtmlSlimParser::updateEffectiveInlineStyle() {
  // Start with block-level styles
  effectiveBold = currentCssStyle.hasFontWeight() && currentCssStyle.fontWeight == CssFontWeight::Bold;
  effectiveItalic = currentCssStyle.hasFontStyle() && currentCssStyle.fontStyle == CssFontStyle::Italic;
  effectiveUnderline = currentCssStyle.hasTextDecoration() &&
                       (currentCssStyle.textDecoration & CssTextDecoration::Underline) != CssTextDecoration::None;
  effectiveStrikethrough = currentCssStyle.hasTextDecoration() &&
                           (currentCssStyle.textDecoration & CssTextDecoration::LineThrough) != CssTextDecoration::None;
  effectiveBackgroundBlack =
      honorsPublisherDecorations() && currentCssStyle.hasBackgroundBlack() && currentCssStyle.backgroundBlack;
  bool paragraphDirectionDefined = false;
  bool paragraphIsRtl = false;
  if (blockStyleCount_ > 0) {
    const auto& blockStyle = blockStyleBuf_[blockStyleCount_ - 1];
    paragraphDirectionDefined = blockStyle.directionDefined;
    paragraphIsRtl = blockStyle.isRtl;
  }
  effectiveDirectionDefined = paragraphDirectionDefined;
  effectiveDirection = paragraphIsRtl ? CssTextDirection::Rtl : CssTextDirection::Ltr;
  effectiveSup = currentCssStyle.hasVerticalAlign() && currentCssStyle.verticalAlign == CssVerticalAlign::Super;
  effectiveSub = currentCssStyle.hasVerticalAlign() && currentCssStyle.verticalAlign == CssVerticalAlign::Sub;
  effectiveSmallCaps =
      currentCssStyle.hasFontVariantCaps() && currentCssStyle.fontVariantCaps == CssFontVariantCaps::SmallCaps;

  // Apply inline style stack in order
  for (size_t i = 0; i < inlineStyleCount_; ++i) {
    const auto& entry = inlineStyleBuf_[i];
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
    if (honorsPublisherDecorations() && entry.hasBackgroundBlack) {
      effectiveBackgroundBlack = entry.backgroundBlack;
    }
    if (entry.hasDirection) {
      effectiveDirectionDefined = true;
      effectiveDirection = entry.direction;
      if (entry.setsParagraphDirection) {
        paragraphDirectionDefined = true;
        paragraphIsRtl = entry.direction == CssTextDirection::Rtl;
      }
    }
    if (entry.hasSup) {
      effectiveSup = entry.sup;
      if (entry.sup) effectiveSub = false;
    }
    if (entry.hasSub) {
      effectiveSub = entry.sub;
      if (entry.sub) effectiveSup = false;
    }
    if (entry.hasSmallCaps) {
      effectiveSmallCaps = entry.smallCaps;
    }
  }

  if (currentTextBlock && currentTextBlock->isEmpty()) {
    auto& style = currentTextBlock->getBlockStyle();
    style.directionDefined = paragraphDirectionDefined;
    style.isRtl = paragraphIsRtl;
  }
}

bool ChapterHtmlSlimParser::shouldAbortForLowMemory(const char* stage) {
  if (lowMemoryAbort) {
    return true;
  }

  auto heap = MemoryBudget::snapshot();
  if (MemoryBudget::hasHeapForEpubTextLayoutStart(heap)) {
    return false;
  }

  if (!attemptedTextLayoutFontCacheRelease) {
    attemptedTextLayoutFontCacheRelease = true;
    if (renderer.releaseSdCardFontForLowMemory(fontId)) {
      const auto afterRelease = MemoryBudget::snapshot();
      LOG_DBG("EHP", "Released SD font caches before %s: free=%u->%u maxAlloc=%u->%u", stage, heap.freeHeap,
              afterRelease.freeHeap, heap.maxAllocHeap, afterRelease.maxAllocHeap);
      heap = afterRelease;
      if (MemoryBudget::hasHeapForEpubTextLayoutStart(heap)) {
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
  if (heap.freeHeap >= MemoryBudget::EPUB_TEXT_LAYOUT_MIN_FREE &&
      heap.maxAllocHeap >= PAGE_ELEMENT_RESERVE_MIN_MAX_ALLOC) {
    currentPage->elements.reserve(INITIAL_PAGE_ELEMENT_RESERVE);
  }
  currentPageNextY = 0;
  currentPageParagraphIndex = 0;
  currentPageListItemIndex = 0;
  currentPageVisibleOffsetSet = false;
  return true;
}

void ChapterHtmlSlimParser::markCurrentPageFromCurrentTextBlock() {
  currentPageParagraphIndex = currentTextBlockParagraphIndex;
  currentPageListItemIndex = currentTextBlockListItemIndex;
}

void ChapterHtmlSlimParser::markCurrentPageFromCurrentElement() {
  currentPageParagraphIndex = xpathParagraphIndex;
  currentPageListItemIndex = xpathListItemIndex;
}

void ChapterHtmlSlimParser::completeCurrentPage() {
  completePageFn(std::move(currentPage), currentPageParagraphIndex, currentPageListItemIndex, currentPageVisibleOffset);
}

void ChapterHtmlSlimParser::setCurrentPageVisibleOffset(const uint32_t offset) {
  if (currentPageVisibleOffsetSet) return;
  currentPageVisibleOffset = completedPageCount == 0 ? 0 : offset;
  currentPageVisibleOffsetSet = true;
}

void ChapterHtmlSlimParser::flushPendingAnchor() {
  if (pendingAnchorId.empty()) return;

  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  if (std::find(tocAnchors.begin(), tocAnchors.end(), pendingAnchorId) != tocAnchors.end()) {
    if (currentPage && !currentPage->elements.empty()) {
      completeCurrentPage();
      completedPageCount++;
      stopPreviewIfPageLimitReached();
      if (previewStopRequested) {
        return;
      }
      if (!startNewPage("TOC anchor page break")) {
        return;
      }
    }
  }

  // Record deferred anchor after previous block is flushed (and any TOC page break)
  anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
  pendingAnchorId.clear();
  pendingAnchorFromInlineA = false;
}

// Resolves previewAnchor to the element ordinal the preview should start rendering at. Runs before
// the render pass so the preview can begin at the block enclosing the anchor instead of
// mid-paragraph. Leaves previewStartOrdinal at 0 on any failure, which falls back to matching the
// anchor id, i.e. the behaviour before block resolution existed.
void ChapterHtmlSlimParser::locatePreviewBlockStart() {
  PreviewBlockLocator locator(previewAnchor.c_str(), isHeaderOrBlock);
  if (!locator.ok()) {
    LOG_ERR("EHP", "Couldn't create parser to locate preview block");
    return;
  }

  // Heap rather than stack: PARSE_BUFFER_SIZE is a quarter of the parsing task's stack. The buffer
  // is released before the render pass opens the file, so the two never overlap.
  auto buf = makeUniqueNoThrow<char[]>(PARSE_BUFFER_SIZE);
  if (!buf) {
    LOG_ERR("EHP", "Couldn't allocate buffer to locate preview block");
    return;
  }

  FsFile file;
  if (!Storage.openFileForRead("EHP", filepath, file)) {
    return;
  }

  while (!locator.done()) {
    const size_t len = file.read(buf.get(), PARSE_BUFFER_SIZE);
    const bool isFinal = file.available() == 0;
    if (len == 0 && !isFinal) {
      LOG_ERR("EHP", "File read error while locating preview block");
      break;
    }
    if (!locator.feed(buf.get(), static_cast<int>(len), isFinal) || isFinal) {
      break;
    }
  }
  file.close();

  previewStartOrdinal = locator.startOrdinal();
  if (previewStartOrdinal == 0) {
    LOG_DBG("EHP", "No enclosing block located for preview anchor '%s'; starting at the anchor", previewAnchor.c_str());
    return;
  }
  LOG_DBG("EHP", "Preview anchor '%s' resolves to block at element #%u", previewAnchor.c_str(), previewStartOrdinal);
}

bool ChapterHtmlSlimParser::handlePreviewScanStart(const XML_Char** atts) {
  previewElementOrdinal += 1;
  if (previewStartOrdinal != 0 && previewElementOrdinal == previewStartOrdinal) {
    startPreviewAtAnchor();
    return false;
  }

  // Fallback for when no block was located, and a safety net in case the two passes disagree on
  // element ordinals: starting at the anchor is the pre-existing behaviour.
  const char* idValue = getAttribute(atts, "id");
  if (!idValue || strcmp(idValue, previewAnchor.c_str()) != 0) {
    depth += 1;
    return true;
  }

  startPreviewAtAnchor();
  return false;
}

void ChapterHtmlSlimParser::startPreviewAtAnchor() {
  previewAnchorFound = true;
  completedPageCount = 0;
  currentPageNextY = 0;
  pendingAnchorId.clear();
  pendingAnchorFromInlineA = false;
  anchorData.clear();
  anchorData.push_back({previewAnchor, 0});

  if (!currentTextBlock) {
    auto paragraphAlignmentBlockStyle = BlockStyle();
    paragraphAlignmentBlockStyle.textAlignDefined = true;
    paragraphAlignmentBlockStyle.alignment = (paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                                 ? CssTextAlign::Justify
                                                 : static_cast<CssTextAlign>(paragraphAlignment);
    startNewTextBlock(paragraphAlignmentBlockStyle);
  }
}

void ChapterHtmlSlimParser::stopPreviewIfPageLimitReached() {
  if (!isPreviewBuild() || !previewAnchorFound || previewStopRequested || completedPageCount < previewMaxPages) {
    return;
  }

  previewStopRequested = true;
  if (activeParser) {
    XML_StopParser(activeParser, XML_TRUE);
  }
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
  if (effectiveSmallCaps) {
    fontStyle = static_cast<EpdFontFamily::Style>(fontStyle | EpdFontFamily::SMALL_CAPS);
  }

  if (currentCompactTable && currentCompactTable->valid() && currentCompactTable->hasActiveCell()) {
    if (!currentCompactTable->appendWord(std::string_view(partWordBuffer, static_cast<size_t>(partWordBufferIndex)),
                                         fontStyle, nextWordContinues,
                                         honorsPublisherDecorations() && effectiveBackgroundBlack,
                                         insideFootnoteLink ? currentFootnote.linkId : 0)) {
      // The row exceeded the compact model's fixed text/token budget. Degrade
      // this table to paragraphs the way the rich path does for its own limits,
      // rather than failing the whole section build.
      LOG_DBG("EHP", "Compact table row capacity exceeded; flattening table");
      compactTableUnsupported = true;
      currentCompactTable->markUnsupported();
    }
    currentTextRunBytes = static_cast<uint16_t>(
        std::min<size_t>(currentTextRunBytes + static_cast<size_t>(partWordBufferIndex), UINT16_MAX));
    partWordBufferIndex = 0;
    nextWordContinues = false;
    return;
  }

  if (!currentTextBlock) {
    // Text outside a compact table cell has no cell buffer. Ignore malformed
    // table content rather than dereferencing a null paragraph block.
    LOG_ERR("EHP", "Discarding text without a paragraph or compact table cell");
    partWordBufferIndex = 0;
    nextWordContinues = false;
    return;
  }

  // flush the buffer
  partWordBuffer[partWordBufferIndex] = '\0';
  currentTextBlock->addWord(partWordBuffer, fontStyle, false, nextWordContinues,
                            honorsPublisherDecorations() && effectiveBackgroundBlack,
                            insideFootnoteLink ? currentFootnote.linkId : 0, partWordVisibleOffset);
  currentTextRunBytes = static_cast<uint16_t>(
      std::min<size_t>(currentTextRunBytes + static_cast<size_t>(partWordBufferIndex), UINT16_MAX));
  partWordBufferIndex = 0;
  nextWordContinues = false;
}

size_t ChapterHtmlSlimParser::bufferedWordsBeforeLayoutLimit() const {
  if (bionicReadingEnabled && guideReadingEnabled) {
    return COMBINED_READING_AID_BUFFERED_WORDS_BEFORE_LAYOUT;
  }
  if (bionicReadingEnabled || guideReadingEnabled) {
    return SINGLE_READING_AID_BUFFERED_WORDS_BEFORE_LAYOUT;
  }
  return embeddedStyle ? CSS_BUFFERED_WORDS_BEFORE_LAYOUT : DEFAULT_BUFFERED_WORDS_BEFORE_LAYOUT;
}

uint16_t ChapterHtmlSlimParser::textRunBytesBeforeLayoutLimit() const {
  if (bionicReadingEnabled && guideReadingEnabled) {
    return COMBINED_READING_AID_TEXT_RUN_BYTES_BEFORE_LAYOUT;
  }
  if (bionicReadingEnabled || guideReadingEnabled) {
    return SINGLE_READING_AID_TEXT_RUN_BYTES_BEFORE_LAYOUT;
  }
  return DEFAULT_TEXT_RUN_BYTES_BEFORE_LAYOUT;
}

void ChapterHtmlSlimParser::flushLongTextRunIfNeeded(const bool force) {
  if (!currentTextBlock) {
    currentTextRunBytes = 0;
    return;
  }
  // A ruby group needs all of its base words together to distribute the
  // annotation and calculate its line-break constraints correctly.
  if (inRuby) return;

  const size_t wordLimit = bufferedWordsBeforeLayoutLimit();
  const uint16_t byteLimit = textRunBytesBeforeLayoutLimit();
  const size_t wordCount = currentTextBlock->size();
  if (!force && wordCount <= wordLimit && currentTextRunBytes <= byteLimit) {
    return;
  }

  const int horizontalInset = currentTextBlock->getBlockStyle().totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
  if (!currentTextBlock->layoutAndExtractLines(
          renderer, fontId, effectiveWidth,
          [this](const std::shared_ptr<TextBlock>& textBlock, const uint32_t offset) {
            addLineToPage(textBlock, offset);
          },
          false)) {
    LOG_ERR("EHP", "Failed to lay out long text run");
    lowMemoryAbort = true;
    return;
  }
  currentTextRunBytes = 0;
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
        const int16_t lineHeight = static_cast<int16_t>(effectiveLineHeight());
        incoming.marginTop = static_cast<int16_t>(incoming.marginTop + lineHeight);
      }

      // The stack accumulates horizontal properties from ancestors. Vertical margins are
      // per-element; merge them only while the placeholder block is still empty.
      const auto style = currentTextBlock->getBlockStyle();
      auto combinedStyle = style.getCombinedBlockStyle(incoming, BlockStyle::CombineAxis::Vertical);
      combinedStyle.fromBrElement = incoming.fromBrElement;
      currentTextBlock->setBlockStyle(combinedStyle);
      currentTextBlockParagraphIndex = xpathParagraphIndex;
      currentTextBlockListItemIndex = xpathListItemIndex;

      flushPendingAnchor();
      return;
    }

    makePages();
  }
  currentTextRunBytes = 0;
  // If the pending anchor is a TOC chapter boundary, force a page break after the previous
  // block is flushed so the chapter starts on a fresh page.
  flushPendingAnchor();
  currentTextBlock.reset(new (std::nothrow)
                             ParsedText(extraParagraphSpacing, forceParagraphIndents, hyphenationEnabled,
                                        bionicReadingEnabled, guideReadingEnabled, wordSpacing, blockStyle));
  if (!currentTextBlock) {
    const auto heap = MemoryBudget::snapshot();
    LOG_ERR("EHP", "Failed to create text block (%u free, %u max alloc)", heap.freeHeap, heap.maxAllocHeap);
    lowMemoryAbort = true;
    return;
  }
  currentTextBlockParagraphIndex = xpathParagraphIndex;
  currentTextBlockListItemIndex = xpathListItemIndex;
  wordsExtractedInBlock = 0;
}

void ChapterHtmlSlimParser::pushCssAncestor(const int depth, const char* tag, const std::string_view classAttr) {
  if (usesSimpleCssLookup()) {
    return;
  }
  ancestorStack_.push_back({depth, std::string(tag), std::string(classAttr)});
}

void ChapterHtmlSlimParser::finalizeCurrentTableCell() {
  if (lowMemoryAbort) {
    return;
  }

  if (tableDepth != 1 || !currentTextBlock) {
    if (tableDepth == 1 && currentCompactTable && currentCompactTable->hasActiveCell()) {
      if (!currentCompactTable->endCell(pendingFootnotes)) {
        LOG_ERR("EHP", "Failed to finalize compact table cell");
        lowMemoryAbort = true;
      }
      pendingFootnotes.clear();
      currentTableCellIsHeader = false;
      currentTableCellColSpan = 1;
      currentTextRunBytes = 0;
      wordsExtractedInBlock = 0;
      nextWordContinues = false;
      return;
    }
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
  cell.visibleTextOffset = currentTableCellVisibleOffset;
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
  if (currentTableBuffer->maxCols > MAX_SIMPLE_TABLE_COLUMNS) {
    currentTableBuffer->unsupported = true;
  }

  currentTableCellIsHeader = false;
  currentTableCellColSpan = 1;
  wordsExtractedInBlock = 0;
  nextWordContinues = false;
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

  if (!headingOpenerActive && !currentPage->elements.empty() && currentPageNextY + totalHeight > viewportHeight) {
    completeCurrentPage();
    completedPageCount++;
    stopPreviewIfPageLimitReached();
    if (previewStopRequested) {
      return;
    }
    if (!startNewPage("horizontal-rule page break")) {
      return;
    }
  }

  currentPageNextY += topSpacing;
  attachPendingPublisherPageMarkers(currentPageNextY);

  auto pageRule = makeUniqueNoThrow<PageHorizontalRule>(width, ruleThickness, xPos, currentPageNextY);
  if (!pageRule) {
    LOG_ERR("EHP", "Failed to create PageHorizontalRule");
    lowMemoryAbort = true;
    return;
  }
  currentPage->elements.push_back(std::move(pageRule));
  setCurrentPageVisibleOffset(visibleTextOffset);
  markCurrentPageFromCurrentElement();
  currentPageNextY = static_cast<int16_t>(currentPageNextY + ruleThickness + bottomSpacing);
  headingOpenerActive = false;

  if (!pendingAnchorId.empty()) {
    anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
    pendingAnchorId.clear();
    pendingAnchorFromInlineA = false;
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

bool ChapterHtmlSlimParser::flushStreamingTableFragment(BufferedTable& table) {
  if (table.streamingFragmentRows.empty()) {
    return true;
  }

  if (!currentPage && !startNewPage("streaming table fragment")) {
    return false;
  }

  const int horizontalInset = table.blockStyle.totalHorizontalInset();
  const uint16_t tableWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
  const uint16_t lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  const uint16_t fragmentHeight = table.streamingFragmentHeight;
  auto fragment = makeUniqueNoThrow<PageTableFragment>(
      tableWidth, table.streamingFragmentColumnCount, TABLE_CELL_PADDING, lineHeight,
      std::move(table.streamingFragmentRows), table.blockStyle.leftInset(), currentPageNextY);
  if (!fragment) {
    LOG_ERR("EHP", "Failed to create streaming PageTableFragment");
    lowMemoryAbort = true;
    return false;
  }

  currentPage->elements.push_back(std::move(fragment));
  setCurrentPageVisibleOffset(table.streamingFragmentVisibleOffset);
  markCurrentPageFromCurrentElement();
  for (const auto& footnote : table.streamingFragmentFootnotes) {
    currentPage->addFootnote(footnote.number, footnote.href, footnote.linkId);
  }
  table.streamingFragmentFootnotes.clear();
  table.streamingFragmentRows.clear();
  table.streamingFragmentHeight = 1;
  table.streamingFragmentColumnCount = 0;
  currentPageNextY = static_cast<int16_t>(currentPageNextY + fragmentHeight);
  return true;
}

void ChapterHtmlSlimParser::emitStreamingTableRowsAsParagraphs(BufferedTable& table) {
  for (auto& row : table.rows) {
    for (auto& cell : row.cells) {
      if (!cell.text) continue;
      pendingFootnotes = std::move(cell.footnotes);
      currentTextBlock = std::move(cell.text);
      wordsExtractedInBlock = 0;
      makePages();
      currentTextBlock.reset();
      pendingFootnotes.clear();
      if (lowMemoryAbort) return;
    }
  }
  table.rows.clear();
  table.totalCells = 0;
  table.maxCols = 0;
}

void ChapterHtmlSlimParser::fallbackStreamingTableToParagraphs(const char* reason) {
  if (!currentTableBuffer) return;
  auto& table = *currentTableBuffer;
  LOG_DBG("EHP", "Streaming table fallback: %s", reason);
  if (!flushStreamingTableFragment(table)) return;
  if (!table.streamingTopSpacingApplied) {
    if (!currentPage && !startNewPage("streaming table paragraph fallback")) return;
    currentPageNextY =
        static_cast<int16_t>(currentPageNextY + table.blockStyle.marginTop + table.blockStyle.paddingTop);
    table.streamingTopSpacingApplied = true;
  }
  table.streamingFlattened = true;
  emitStreamingTableRowsAsParagraphs(table);
}

bool ChapterHtmlSlimParser::streamCurrentTableRow() {
  if (!currentTableBuffer || !currentTableBuffer->streaming || currentTableBuffer->rows.empty()) {
    return true;
  }

  auto& table = *currentTableBuffer;
  if (table.streamingFlattened) {
    emitStreamingTableRowsAsParagraphs(table);
    return !lowMemoryAbort;
  }

  auto& row = table.rows.back();
  if (table.unsupported || row.cells.empty()) {
    fallbackStreamingTableToParagraphs(table.unsupported ? "unsupported structure" : "empty row");
    return !lowMemoryAbort;
  }

  if (table.streamingColumnCount == 0) {
    table.streamingColumnCount = static_cast<uint8_t>(row.effectiveColumnCount);
  }
  const bool isFullWidthSingleCellRow = row.cells.size() == 1 && row.cells[0].colSpan == table.streamingColumnCount;
  const bool rowHasMergedCells =
      std::any_of(row.cells.begin(), row.cells.end(), [](const BufferedTableCell& cell) { return cell.colSpan != 1; });
  if (table.streamingColumnCount == 0 || table.streamingColumnCount > MAX_SIMPLE_TABLE_COLUMNS ||
      (row.effectiveColumnCount != table.streamingColumnCount && !isFullWidthSingleCellRow) ||
      (rowHasMergedCells && !isFullWidthSingleCellRow)) {
    fallbackStreamingTableToParagraphs("inconsistent column structure");
    return !lowMemoryAbort;
  }

  const uint8_t columnCount = isFullWidthSingleCellRow ? 1 : table.streamingColumnCount;
  const int horizontalInset = table.blockStyle.totalHorizontalInset();
  const uint16_t tableWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
  for (uint8_t column = 0; column < columnCount; ++column) {
    if (TableColumnLayout::innerWidth(tableWidth, columnCount, column, 1, TABLE_CELL_PADDING) < 20) {
      fallbackStreamingTableToParagraphs("column width too small");
      return !lowMemoryAbort;
    }
  }

  TableFragmentRow fragmentRow;
  fragmentRow.cells.resize(columnCount);
  fragmentRow.headerSeparator = row.hasHeaderCell && !row.hasDataCell;
  const uint16_t lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  uint32_t rowHeight = static_cast<uint32_t>(lineHeight) + TABLE_CELL_PADDING * 2;
  std::vector<FootnoteEntry> rowFootnotes;
  for (size_t cellIndex = 0; cellIndex < row.cells.size(); ++cellIndex) {
    const auto& sourceCell = row.cells[cellIndex];
    auto& destCell = fragmentRow.cells[cellIndex];
    destCell.isHeader = sourceCell.isHeader;
    if (sourceCell.text &&
        !sourceCell.text->layoutAndExtractLinesPreservingSource(
            renderer, fontId,
            TableColumnLayout::innerWidth(tableWidth, columnCount, static_cast<uint8_t>(cellIndex), 1,
                                          TABLE_CELL_PADDING),
            [&destCell](const std::shared_ptr<TextBlock>& textBlock) { destCell.lines.push_back(textBlock); }, true)) {
      fallbackStreamingTableToParagraphs("cell layout failed");
      return !lowMemoryAbort;
    }
    for (const auto& [wordIndex, footnote] : sourceCell.footnotes) {
      (void)wordIndex;
      rowFootnotes.push_back(footnote);
    }
    if (destCell.lines.size() > TableFragmentCell::MAX_SERIALIZED_LINES) {
      fallbackStreamingTableToParagraphs("cell has too many lines");
      return !lowMemoryAbort;
    }
    const uint32_t cellHeight = std::max<size_t>(1, destCell.lines.size()) * lineHeight + TABLE_CELL_PADDING * 2;
    if (cellHeight > viewportHeight) {
      fallbackStreamingTableToParagraphs("row exceeds viewport");
      return !lowMemoryAbort;
    }
    rowHeight = std::max(rowHeight, cellHeight);
  }
  fragmentRow.height = static_cast<uint16_t>(rowHeight);

  if (!currentPage && !startNewPage("streaming table row")) return false;
  if (!table.streamingTopSpacingApplied) {
    currentPageNextY =
        static_cast<int16_t>(currentPageNextY + table.blockStyle.marginTop + table.blockStyle.paddingTop);
    table.streamingTopSpacingApplied = true;
  }

  const bool startsNewFragment =
      !table.streamingFragmentRows.empty() &&
      (table.streamingFragmentColumnCount != columnCount ||
       table.streamingFragmentRows.size() >= PageTableFragment::MAX_SERIALIZED_ROWS ||
       currentPageNextY + table.streamingFragmentHeight + fragmentRow.height > viewportHeight);
  if (startsNewFragment && !flushStreamingTableFragment(table)) return false;

  if (table.streamingFragmentRows.empty() && currentPageNextY + 1 + fragmentRow.height > viewportHeight &&
      !currentPage->elements.empty()) {
    completeCurrentPage();
    completedPageCount++;
    stopPreviewIfPageLimitReached();
    if (previewStopRequested) return true;
    if (!startNewPage("streaming table page break")) return false;
  }

  if (table.streamingFragmentRows.empty()) {
    table.streamingFragmentColumnCount = columnCount;
    table.streamingFragmentVisibleOffset = row.cells.front().visibleTextOffset;
  }
  table.streamingFragmentHeight = static_cast<uint16_t>(table.streamingFragmentHeight + fragmentRow.height);
  table.streamingFragmentRows.push_back(std::move(fragmentRow));
  table.streamingFragmentFootnotes.insert(table.streamingFragmentFootnotes.end(), rowFootnotes.begin(),
                                          rowFootnotes.end());
  std::vector<BufferedTableRow>().swap(table.rows);
  table.totalCells = 0;
  table.maxCols = 0;
  return true;
}

void ChapterHtmlSlimParser::finishStreamingTable(BufferedTable& table) {
  if (!table.streamingFlattened && !streamCurrentTableRow()) return;
  if (!flushStreamingTableFragment(table)) return;
  currentPageNextY =
      static_cast<int16_t>(currentPageNextY + table.blockStyle.marginBottom + table.blockStyle.paddingBottom);
  if (extraParagraphSpacing) {
    currentPageNextY += renderer.getLineHeight(fontId) * lineCompression / 2;
  }
}

bool ChapterHtmlSlimParser::flushCompactTableFragment() {
  if (compactFragmentRows.empty()) return true;
  if (!currentPage && !startNewPage("compact table fragment")) return false;

  const auto& style = currentCompactTable->tableStyle();
  const int horizontalInset = style.totalHorizontalInset();
  const uint16_t width =
      horizontalInset < viewportWidth ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;
  const uint16_t lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  auto fragment =
      makeUniqueNoThrow<PageTableFragment>(width, compactFragmentColumnCount, TABLE_CELL_PADDING, lineHeight,
                                           std::move(compactFragmentRows), style.leftInset(), currentPageNextY);
  if (!fragment) {
    LOG_ERR("EHP", "Failed to create compact PageTableFragment");
    lowMemoryAbort = true;
    return false;
  }

  const uint16_t fragmentHeight = compactFragmentHeight;
  currentPage->elements.push_back(std::move(fragment));
  setCurrentPageVisibleOffset(compactFragmentVisibleOffset);
  markCurrentPageFromCurrentElement();
  for (const auto& footnote : compactFragmentFootnotes) {
    currentPage->addFootnote(footnote.number, footnote.href, footnote.linkId);
  }
  compactFragmentFootnotes.clear();
  compactFragmentRows.clear();
  compactFragmentHeight = 1;
  compactFragmentColumnCount = 0;
  currentPageNextY = static_cast<int16_t>(currentPageNextY + fragmentHeight);
  return true;
}

bool ChapterHtmlSlimParser::emitCompactTableRow(TableFragmentRow& row,
                                                std::vector<std::shared_ptr<TextBlock>>& flatLines,
                                                const std::vector<FootnoteEntry>& footnotes,
                                                const uint32_t rowVisibleOffset, const uint8_t fragmentColumnCount,
                                                const bool flatten) {
  if (!currentCompactTable) return false;
  const auto& style = currentCompactTable->tableStyle();
  if (!currentPage && !startNewPage(flatten ? "compact table paragraph fallback" : "compact table row")) return false;
  if (!compactTableTopSpacingApplied) {
    currentPageNextY = static_cast<int16_t>(currentPageNextY + style.marginTop + style.paddingTop);
    compactTableTopSpacingApplied = true;
  }

  if (flatten) {
    if (!flushCompactTableFragment()) return false;
    for (const auto& line : flatLines) {
      if (!line) continue;
      if (!currentPage->elements.empty() &&
          currentPageNextY + renderer.getLineHeight(fontId) * lineCompression > viewportHeight) {
        completeCurrentPage();
        completedPageCount++;
        stopPreviewIfPageLimitReached();
        // A preview that reached its page limit has finished normally.
        if (previewStopRequested) return true;
        if (!startNewPage("compact table paragraph page break")) return false;
      }
      auto pageLine = makeUniqueNoThrow<PageLine>(line, style.leftInset(), currentPageNextY);
      if (!pageLine) {
        LOG_ERR("EHP", "Failed to create compact table paragraph line");
        lowMemoryAbort = true;
        return false;
      }
      currentPage->elements.push_back(std::move(pageLine));
      setCurrentPageVisibleOffset(rowVisibleOffset);
      markCurrentPageFromCurrentElement();
      currentPageNextY = static_cast<int16_t>(currentPageNextY + renderer.getLineHeight(fontId) * lineCompression);
    }
    for (const auto& footnote : footnotes) {
      currentPage->addFootnote(footnote.number, footnote.href, footnote.linkId);
    }
    return !lowMemoryAbort;
  }

  if (row.cells.empty() || row.cells.size() > TableFragmentRow::MAX_SERIALIZED_CELLS) {
    return false;
  }
  const uint8_t columnCount = fragmentColumnCount;
  const bool startsNewFragment =
      !compactFragmentRows.empty() && (compactFragmentColumnCount != columnCount ||
                                       compactFragmentRows.size() >= PageTableFragment::MAX_SERIALIZED_ROWS ||
                                       currentPageNextY + compactFragmentHeight + row.height > viewportHeight);
  if (startsNewFragment && !flushCompactTableFragment()) return false;

  if (compactFragmentRows.empty() && currentPageNextY + 1 + row.height > viewportHeight &&
      !currentPage->elements.empty()) {
    completeCurrentPage();
    completedPageCount++;
    stopPreviewIfPageLimitReached();
    if (previewStopRequested) return true;
    if (!startNewPage("compact table page break")) return false;
  }

  if (compactFragmentRows.empty()) {
    compactFragmentColumnCount = columnCount;
    compactFragmentVisibleOffset = rowVisibleOffset;
  }
  compactFragmentHeight = static_cast<uint16_t>(compactFragmentHeight + row.height);
  const size_t availableFootnoteSlots = Page::MAX_FOOTNOTES_PER_PAGE > compactFragmentFootnotes.size()
                                            ? Page::MAX_FOOTNOTES_PER_PAGE - compactFragmentFootnotes.size()
                                            : 0;
  const size_t footnotesToCopy = std::min(availableFootnoteSlots, footnotes.size());
  compactFragmentFootnotes.insert(compactFragmentFootnotes.end(), footnotes.begin(),
                                  footnotes.begin() + footnotesToCopy);
  compactFragmentRows.push_back(std::move(row));
  return true;
}

void ChapterHtmlSlimParser::finishCompactTable() {
  if (!currentCompactTable) return;
  if (!flushCompactTableFragment()) return;
  const auto& style = currentCompactTable->tableStyle();
  currentPageNextY = static_cast<int16_t>(currentPageNextY + style.marginBottom + style.paddingBottom);
  if (extraParagraphSpacing) currentPageNextY += renderer.getLineHeight(fontId) * lineCompression / 2;
}

void ChapterHtmlSlimParser::emitBufferedTableAsFragments(BufferedTable& table) {
  struct PreparedRow {
    TableFragmentRow fragmentRow;
    std::vector<FootnoteEntry> footnotes;
    uint32_t visibleTextOffset = 0;
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
    if (columnCount == 0) {
      LOG_DBG("EHP", "Table layout fallback: width %u too small for %u columns", tableWidth, columnCount);
      return false;
    }
    for (uint8_t column = 0; column < columnCount; ++column) {
      if (TableColumnLayout::innerWidth(tableWidth, columnCount, column, 1, TABLE_CELL_PADDING) < 20) {
        LOG_DBG("EHP", "Table layout fallback: width %u too small for %u columns", tableWidth, columnCount);
        return false;
      }
    }

    PreparedRow prepared;
    prepared.visibleTextOffset = row.cells.empty() ? visibleTextOffset : row.cells.front().visibleTextOffset;
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
        if (!sourceCell.text->layoutAndExtractLinesPreservingSource(
                renderer, fontId,
                TableColumnLayout::innerWidth(tableWidth, columnCount, static_cast<uint8_t>(colIndex), 1,
                                              TABLE_CELL_PADDING),
                [&destCell](const std::shared_ptr<TextBlock>& textBlock) { destCell.lines.push_back(textBlock); },
                true)) {
          LOG_DBG("EHP", "Table layout fallback: cell text layout failed");
          return false;
        }
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
      const uint32_t fragmentVisibleOffset = segment.rows[nextRowIndex].visibleTextOffset;

      while (nextRowIndex < segment.rows.size()) {
        const uint16_t nextHeight =
            static_cast<uint16_t>(fragmentHeight + segment.rows[nextRowIndex].fragmentRow.height);
        if (!fragmentRows.empty() && currentPageNextY + nextHeight > viewportHeight) {
          break;
        }
        if (fragmentRows.empty() && currentPageNextY + nextHeight > viewportHeight && !currentPage->elements.empty()) {
          completeCurrentPage();
          completedPageCount++;
          stopPreviewIfPageLimitReached();
          if (previewStopRequested) {
            return;
          }
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

      auto fragment =
          makeUniqueNoThrow<PageTableFragment>(tableWidth, segment.columnCount, TABLE_CELL_PADDING, lineHeight,
                                               std::move(fragmentRows), table.blockStyle.leftInset(), currentPageNextY);
      if (!fragment) {
        LOG_ERR("EHP", "Failed to create PageTableFragment");
        lowMemoryAbort = true;
        return;
      }
      currentPage->elements.push_back(std::move(fragment));
      setCurrentPageVisibleOffset(fragmentVisibleOffset);
      markCurrentPageFromCurrentElement();
      for (const auto& footnote : fragmentFootnotes) {
        currentPage->addFootnote(footnote.number, footnote.href, footnote.linkId);
      }
      currentPageNextY += fragmentHeight;

      if (nextRowIndex < segment.rows.size()) {
        completeCurrentPage();
        completedPageCount++;
        stopPreviewIfPageLimitReached();
        if (previewStopRequested) {
          return;
        }
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

void ChapterHtmlSlimParser::flushMalformedPartialContent() {
  if (partWordBufferIndex > 0) {
    flushPartWordBuffer();
  }

  if (tableDepth > 0) {
    if (tableDepth == 1 && currentTextBlock) {
      finalizeCurrentTableCell();
    }
    if (tableDepth == 1 && currentCompactTable) {
      if (currentCompactTable->hasActiveCell()) {
        currentCompactTable->endCell(pendingFootnotes);
        pendingFootnotes.clear();
      }
      if (currentCompactTable->hasActiveRow()) {
        currentCompactTable->setFlattened();
        TableFragmentRow row;
        std::vector<std::shared_ptr<TextBlock>> flatLines;
        std::vector<FootnoteEntry> rowFootnotes;
        rowFootnotes.reserve(Page::MAX_FOOTNOTES_PER_PAGE);
        uint32_t rowVisibleOffset = visibleTextOffset;
        const auto result = currentCompactTable->finishRow(row, flatLines, rowFootnotes, rowVisibleOffset);
        if (result == CompactTableLayout::RowResult::Abort) {
          lowMemoryAbort = true;
        } else if (result == CompactTableLayout::RowResult::Flatten) {
          compactTableFlattened = true;
          if (!emitCompactTableRow(row, flatLines, rowFootnotes, rowVisibleOffset,
                                   currentCompactTable->fragmentColumnCount(), true)) {
            lowMemoryAbort = true;
          }
        }
      }
      finishCompactTable();
      currentCompactTable.reset();
    } else if (currentTableBuffer) {
      fallbackCurrentTableBufferToParagraphs("malformed markup");
    }
    tableDepth = 0;
    tableRowIndex = 0;
    tableColIndex = 0;
    currentTableCellIsHeader = false;
    currentTableCellColSpan = 1;
  }
}

bool ChapterHtmlSlimParser::appendMalformedMarkupWarningPage() {
  if (currentPage && !currentPage->elements.empty()) {
    completeCurrentPage();
    completedPageCount++;
  }
  currentPage.reset();

  if (!startNewPage("malformed markup warning")) {
    return false;
  }

  const uint16_t textWidth =
      viewportWidth > WARNING_PAGE_SIDE_MARGIN * 2 ? viewportWidth - WARNING_PAGE_SIDE_MARGIN * 2 : viewportWidth;
  const auto titleLines = renderer.wrappedText(fontId, tr(STR_EPUB_CHAPTER_INCOMPLETE_TITLE), textWidth,
                                               WARNING_TITLE_MAX_LINES, EpdFontFamily::BOLD);
  const auto bodyLines =
      renderer.wrappedText(fontId, tr(STR_EPUB_CHAPTER_INCOMPLETE_BODY), textWidth, WARNING_BODY_MAX_LINES);
  const int lineHeight = effectiveLineHeight();
  const int titleBodyGap = lineHeight;
  const int contentLineCount = static_cast<int>(titleLines.size() + bodyLines.size());
  const int contentHeight =
      contentLineCount * lineHeight + (!titleLines.empty() && !bodyLines.empty() ? titleBodyGap : 0);
  int y = std::max(0, (static_cast<int>(viewportHeight) - contentHeight) / 2);

  auto addLine = [this](const std::string& line, const EpdFontFamily::Style style, const int yPos) -> bool {
    std::vector<std::string> words;
    std::vector<int16_t> xPos;
    std::vector<EpdFontFamily::Style> styles;
    words.reserve(1);
    xPos.reserve(1);
    styles.reserve(1);
    words.push_back(line);
    xPos.push_back(0);
    styles.push_back(style);

    auto block = std::make_shared<TextBlock>(std::move(words), std::move(xPos), std::move(styles),
                                             std::vector<uint8_t>{}, std::vector<uint16_t>{}, std::vector<uint16_t>{},
                                             std::vector<uint8_t>{}, std::vector<bool>{false});
    auto pageLine = makeUniqueNoThrow<PageLine>(
        std::move(block),
        static_cast<int16_t>(
            std::max(0, (static_cast<int>(viewportWidth) - renderer.getTextWidth(fontId, line.c_str(), style)) / 2)),
        static_cast<int16_t>(yPos));
    if (!pageLine) {
      LOG_ERR("EHP", "Failed to create malformed-markup warning line");
      lowMemoryAbort = true;
      return false;
    }
    currentPage->elements.push_back(std::move(pageLine));
    setCurrentPageVisibleOffset(visibleTextOffset);
    return true;
  };

  for (const auto& line : titleLines) {
    if (!addLine(line, EpdFontFamily::BOLD, y)) return false;
    y += lineHeight;
  }
  if (!titleLines.empty() && !bodyLines.empty()) {
    y += titleBodyGap;
  }
  for (const auto& line : bodyLines) {
    if (!addLine(line, EpdFontFamily::REGULAR, y)) return false;
    y += lineHeight;
  }

  if (!currentPage || currentPage->elements.empty()) {
    LOG_ERR("EHP", "Failed to create malformed markup warning page");
    return false;
  }

  completeCurrentPage();
  completedPageCount++;
  currentPage.reset();
  return true;
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (self->isScanningForPreviewAnchor()) {
    if (self->handlePreviewScanStart(atts)) {
      return;
    }
  }
  if (self->previewStopRequested) {
    return;
  }
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

  // Borrow parser-owned attribute bytes during this callback; copy only when
  // storing an ancestor/id beyond the current element start.
  std::string_view classAttr;
  std::string_view styleAttr;
  const char* dirAttr = nullptr;
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      const char* attrValue = atts[i + 1] ? atts[i + 1] : "";
      if (strcmp(atts[i], "class") == 0) {
        classAttr = attrValue;
      } else if (strcmp(atts[i], "style") == 0) {
        styleAttr = attrValue;
      } else if (strcmp(atts[i], "id") == 0) {
        if (self->isPreviewBuild() && self->previewAnchorFound && strcmp(attrValue, self->previewAnchor.c_str()) == 0) {
          continue;
        }
        // Defer both anchor recording and TOC page breaks until startNewTextBlock,
        // after the previous block is flushed to pages via makePages().
        const char* idValue = attrValue;
        const bool isTocAnchor =
            std::find(self->tocAnchors.begin(), self->tocAnchors.end(), idValue) != self->tocAnchors.end();
        if (isTocAnchor || (!isNonNavigableInlineElement(name) && self->anchorData.size() < MAX_ANCHORS_PER_CHAPTER)) {
          // Flush displaced block anchors before overwriting. Keep dense inline <a id>
          // runs coalesced so converter-generated anchors do not churn heap in link-heavy chapters.
          const bool previousAnchorShouldBeRecorded = !self->pendingAnchorFromInlineA;
          if (previousAnchorShouldBeRecorded && !self->pendingAnchorId.empty()) {
            self->flushPendingAnchor();
          }
          self->pendingAnchorId = idValue;
          self->pendingAnchorFromInlineA = !isTocAnchor && strcmp(name, "a") == 0;

          // A chapter's TOC target is sometimes an empty inline <a id> tucked *inside* an
          // already-open heading, ahead of the chapter-number/ornament/title runs
          // (e.g. <h1><a id="rsecN"/>Chapter N<img/>Title</h1>). Resolving it lazily at the
          // next block flush defers the chapter's page break until after the "Chapter N" run
          // is already on the page: the break then lands mid-heading, orphaning the number and
          // pushing the ornament + title onto the next page. While the heading opener is still
          // active and has produced no content yet, resolve it now so any needed break lands at
          // the heading's start and the whole opener stays together.
          if (isTocAnchor && self->headingOpenerActive && self->partWordBufferIndex == 0 && self->currentTextBlock &&
              self->currentTextBlock->isEmpty()) {
            self->flushPendingAnchor();
          }
        }
      } else if (strcmp(atts[i], "dir") == 0) {
        dirAttr = attrValue;
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
    cssStyle = self->usesSimpleCssLookup() ? self->cssParser->resolveStyle(name, classAttr)
                                           : self->cssParser->resolveStyle(name, classAttr, self->ancestorStack_);
    if (!styleAttr.empty()) {
      CssStyle inlineStyle = CssParser::parseInlineStyle(styleAttr);
      cssStyle.applyOver(inlineStyle);
    }
    if (self->shouldAbortForLowMemory("CSS style resolution")) {
      return;
    }
  }

  if (dirAttr && dirAttr[0] != '\0') {
    if (strcasecmp(dirAttr, "rtl") == 0) {
      cssStyle.direction = CssTextDirection::Rtl;
      cssStyle.defined.direction = 1;
    } else if (strcasecmp(dirAttr, "ltr") == 0) {
      cssStyle.direction = CssTextDirection::Ltr;
      cssStyle.defined.direction = 1;
    }
  }

  if (!cssStyle.hasDirection() && self->effectiveDirectionDefined) {
    cssStyle.direction = self->effectiveDirection;
    cssStyle.defined.direction = 1;
  }

  // font-variant-caps is inherited in CSS; propagate the parent's small-caps
  // state to this element's style unless it sets its own (including "normal",
  // which explicitly opts back out).
  if (!cssStyle.hasFontVariantCaps() && self->effectiveSmallCaps) {
    cssStyle.fontVariantCaps = CssFontVariantCaps::SmallCaps;
    cssStyle.defined.fontVariantCaps = 1;
  }

  const char* roleAttr = getAttribute(atts, "role");
  const char* epubTypeAttr = getAttribute(atts, "epub:type");
  const bool isPublisherPageBreak =
      attributeContainsToken(roleAttr, "doc-pagebreak") || attributeContainsToken(epubTypeAttr, "pagebreak");
  if (isPublisherPageBreak) {
    if (self->honorsPublisherDecorations()) {
      const char* markerLabel = getAttribute(atts, "title");
      if (!markerLabel || markerLabel[0] == '\0') {
        markerLabel = getAttribute(atts, "aria-label");
      }
      self->addPendingPublisherPageMarker(markerLabel);
    }
    self->skipCurrentElement();
    return;
  }

  // Skip elements with display:none before all fast paths (tables, links, etc.).
  if (cssStyle.hasDisplay() && cssStyle.display == CssDisplay::None) {
    self->skipCurrentElement();
    return;
  }

  // Special handling for tables/cells: stream simple table rows into page fragments,
  // with a clean flat-paragraph fallback for anything more complex.
  if (self->flattensTables()) {
    if (strcmp(name, "table") == 0) {
      if (self->tableDepth > 0) {
        self->tableDepth += 1;
        return;
      }

      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      self->tableDepth += 1;
      self->tableRowIndex = 0;
      self->tableColIndex = 0;
      self->depth += 1;
      return;
    }

    if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
      self->tableRowIndex += 1;
      self->tableColIndex = 0;
      self->depth += 1;
      return;
    }

    if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      self->tableColIndex += 1;

      auto tableCellBlockStyle = BlockStyle();
      tableCellBlockStyle.textAlignDefined = true;
      const auto align = (self->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                             ? CssTextAlign::Justify
                             : static_cast<CssTextAlign>(self->paragraphAlignment);
      tableCellBlockStyle.alignment = align;
      self->startNewTextBlock(tableCellBlockStyle);

      char headerText[32];
      snprintf(headerText, sizeof(headerText), "Tab Row %d, Cell %d:", self->tableRowIndex, self->tableColIndex);
      StyleStackEntry headerStyle;
      headerStyle.depth = self->depth;
      headerStyle.hasBold = true;
      headerStyle.bold = false;
      headerStyle.hasItalic = true;
      headerStyle.italic = true;
      headerStyle.hasUnderline = true;
      headerStyle.underline = false;
      bool pushedHeaderStyle = false;
      if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
        self->inlineStyleBuf_[self->inlineStyleCount_++] = headerStyle;
        pushedHeaderStyle = true;
      } else {
        LOG_ERR("EHP", "inline style stack overflow (table cell label)");
      }
      self->updateEffectiveInlineStyle();
      self->syntheticCharacterData = true;
      self->characterData(userData, headerText, static_cast<int>(strlen(headerText)));
      self->syntheticCharacterData = false;
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      self->nextWordContinues = false;
      if (pushedHeaderStyle) {
        self->inlineStyleCount_--;
        self->updateEffectiveInlineStyle();
      }

      self->depth += 1;
      return;
    }

    if (self->tableDepth == 1 && strcmp(name, "hr") == 0) {
      self->depth += 1;
      return;
    }
  }

  if (strcmp(name, "table") == 0) {
    // skip nested tables
    if (self->tableDepth > 0) {
      if (self->currentTableBuffer) {
        self->currentTableBuffer->unsupported = true;
      }
      if (self->currentCompactTable) {
        self->compactTableUnsupported = true;
        self->currentCompactTable->markUnsupported();
      }
      self->tableDepth += 1;
      return;
    }

    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    const float emSize = self->renderer.getLineHeight(self->fontId) * self->lineCompression;
    auto tableBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Left, self->viewportWidth);

    const auto heap = MemoryBudget::snapshot();
    const bool useCompact = !MemoryBudget::hasHeap(heap, MIN_FREE_HEAP_FOR_RICH_TABLE, MIN_MAX_ALLOC_FOR_RICH_TABLE);
    if (useCompact) {
      // Finish the preceding paragraph before allocating compact-table state.
      // On C3 this releases its layout buffers before the table's row buffers
      // need the same constrained heap.
      if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
        self->makePages();
        if (self->lowMemoryAbort) {
          return;
        }
      }
      self->currentTextBlock.reset();
      const uint16_t lineHeight =
          static_cast<uint16_t>(self->renderer.getLineHeight(self->fontId) * self->lineCompression);
      self->currentCompactTable =
          makeUniqueNoThrow<CompactTableLayout>(self->renderer, self->fontId, self->viewportWidth, self->viewportHeight,
                                                lineHeight, TABLE_CELL_PADDING, tableBlockStyle);
      if (!self->currentCompactTable || !self->currentCompactTable->valid()) {
        LOG_ERR("EHP", "Failed to allocate compact table layout (free=%u, maxAlloc=%u)", heap.freeHeap,
                heap.maxAllocHeap);
        self->lowMemoryAbort = true;
        return;
      }
      self->compactTableFlattened = false;
      self->compactTableUnsupported = false;
      self->compactTableTopSpacingApplied = false;
      self->compactFragmentRows.clear();
      self->compactFragmentRows.reserve(PageTableFragment::MAX_SERIALIZED_ROWS);
      self->compactFragmentFootnotes.clear();
      self->compactFragmentFootnotes.reserve(Page::MAX_FOOTNOTES_PER_PAGE);
      self->compactFragmentHeight = 1;
      self->compactFragmentColumnCount = 0;
      LOG_DBG("EHP", "Compact table layout selected (free=%u, maxAlloc=%u)", heap.freeHeap, heap.maxAllocHeap);
    } else {
      self->currentTableBuffer = makeUniqueNoThrow<BufferedTable>();
      if (!self->currentTableBuffer) {
        LOG_ERR("EHP", "Failed to buffer rich table (free=%u, maxAlloc=%u)", heap.freeHeap, heap.maxAllocHeap);
        self->lowMemoryAbort = true;
        return;
      }
      self->currentTableBuffer->blockStyle = tableBlockStyle;
      self->currentTableBuffer->streaming = true;
      LOG_DBG("EHP", "Rich table layout selected (free=%u, maxAlloc=%u)", heap.freeHeap, heap.maxAllocHeap);
    }
    self->tableDepth += 1;
    self->tableRowIndex = 0;
    self->tableColIndex = 0;
    self->pushCssAncestor(self->depth, name, classAttr);
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "tr") == 0) {
    self->tableRowIndex += 1;
    self->tableColIndex = 0;
    if (self->currentCompactTable) {
      if (!self->currentCompactTable->beginRow()) {
        LOG_ERR("EHP", "Failed to begin compact table row");
        self->lowMemoryAbort = true;
        return;
      }
    } else if (self->currentTableBuffer) {
      self->currentTableBuffer->rows.push_back({});
    }
    self->pushCssAncestor(self->depth, name, classAttr);
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
        }
        if (self->currentCompactTable) {
          self->compactTableUnsupported = true;
          self->currentCompactTable->markUnsupported();
        }
      } else {
        parsedColSpan = static_cast<uint8_t>(parsedValue);
      }
    }
    if (self->currentTableBuffer && rowspan && strcmp(rowspan, "1") != 0) {
      self->currentTableBuffer->unsupported = true;
    }
    if (self->currentCompactTable && rowspan && strcmp(rowspan, "1") != 0) {
      self->compactTableUnsupported = true;
      self->currentCompactTable->markUnsupported();
    }
    self->currentTableCellColSpan = parsedColSpan;
    self->currentTableCellVisibleOffset = self->visibleTextOffset;

    auto tableCellBlockStyle = BlockStyle();
    tableCellBlockStyle.textAlignDefined = true;
    // Default table cells to left alignment so narrow columns don't inherit paragraph
    // justification or other reader-wide alignment settings that damage readability.
    tableCellBlockStyle.alignment = cssStyle.hasTextAlign() ? cssStyle.textAlign : CssTextAlign::Left;
    // Cell paragraphs are transparent wrappers. Mark the indent explicitly so the
    // generic three-space paragraph fallback does not indent every table cell.
    tableCellBlockStyle.textIndentDefined = true;
    tableCellBlockStyle.textIndent = 0;
    if (cssStyle.hasDirection()) {
      tableCellBlockStyle.isRtl = cssStyle.direction == CssTextDirection::Rtl;
      tableCellBlockStyle.directionDefined = true;
    }
    self->currentTableCellIsHeader = strcmp(name, "th") == 0;
    {
      StyleStackEntry cellStyle;
      cellStyle.depth = self->depth;
      if (self->currentTableCellIsHeader) {
        cellStyle.hasBold = true;
        cellStyle.bold = true;
      }
      if (cssStyle.hasBackgroundBlack()) {
        cellStyle.hasBackgroundBlack = true;
        cellStyle.backgroundBlack = cssStyle.backgroundBlack;
      }
      ChapterHtmlSlimParser::applyDirectionToEntry(cellStyle, cssStyle);
      cellStyle.setsParagraphDirection = true;
      ChapterHtmlSlimParser::applySmallCapsToEntry(cellStyle, cssStyle);
      if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
        self->inlineStyleBuf_[self->inlineStyleCount_++] = cellStyle;
      } else {
        LOG_ERR("EHP", "inline style stack overflow (table cell)");
      }
      self->updateEffectiveInlineStyle();
    }
    if (self->currentCompactTable) {
      if (!self->currentCompactTable->beginCell(self->currentTableCellIsHeader, parsedColSpan,
                                                self->currentTableCellVisibleOffset, tableCellBlockStyle)) {
        // Too many cells for the compact row model: flatten instead of aborting.
        LOG_DBG("EHP", "Compact table cell capacity exceeded; flattening table");
        self->compactTableUnsupported = true;
        self->currentCompactTable->markUnsupported();
      }
      self->currentTextBlock.reset();
      self->currentTextRunBytes = 0;
      self->pendingFootnotes.clear();
    } else {
      self->startNewTextBlock(tableCellBlockStyle);
    }

    self->pushCssAncestor(self->depth, name, classAttr);
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && strcmp(name, "caption") != 0 &&
      (matches(name, HEADER_TAGS, std::size(HEADER_TAGS)) || matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS)))) {
    // Treat block/header tags inside a table cell as transparent wrappers around the
    // cell's text content instead of forcing the whole table back to paragraph mode.
    // This covers common EPUB patterns like <td><p>...</p></td> and
    // <td><h4><em>...</em></h4></td> while still keeping one buffered cell.
    if (strcmp(name, "br") == 0 && self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = false;
    }
    self->pushCssAncestor(self->depth, name, classAttr);
    self->depth += 1;
    return;
  }

  if (self->tableDepth == 1 && matches(name, IMAGE_TAGS, std::size(IMAGE_TAGS))) {
    if (self->currentTableBuffer) {
      self->currentTableBuffer->unsupported = true;
    }
    if (self->currentCompactTable) {
      self->compactTableUnsupported = true;
      self->currentCompactTable->markUnsupported();
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
    }
    if (self->currentCompactTable) {
      self->compactTableUnsupported = true;
      self->currentCompactTable->markUnsupported();
    }
    self->pushCssAncestor(self->depth, name, classAttr);
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
        } else if (src.empty() && (strcmp(atts[i], "href") == 0 || strcmp(atts[i], "xlink:href") == 0)) {
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

      const size_t fragmentPos = src.find('#');
      if (fragmentPos != std::string::npos) {
        src.resize(fragmentPos);
      }

      // imageRendering: 0=display, 1=placeholder (alt text only), 2=suppress entirely
      if (self->imageRendering == 2) {
        self->skipCurrentElement();
        return;
      }

      // Skip image if CSS display:none
      if (self->cssParser) {
        CssStyle imgDisplayStyle = self->usesSimpleCssLookup()
                                       ? self->cssParser->resolveStyle("img", classAttr)
                                       : self->cssParser->resolveStyle("img", classAttr, self->ancestorStack_);
        if (!styleAttr.empty()) {
          imgDisplayStyle.applyOver(CssParser::parseInlineStyle(styleAttr));
        }
        if (imgDisplayStyle.hasDisplay() && imgDisplayStyle.display == CssDisplay::None) {
          self->skipCurrentElement();
          return;
        }
      }

      if (!src.empty() && self->imageRendering != 1) {
        const std::string resolvedPath = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->contentBase + src));
        if (isSvgImagePath(resolvedPath)) {
          LOG_DBG("EHP", "Skipping unsupported SVG image: %s", resolvedPath.c_str());
          self->skipCurrentElement();
          return;
        }

        {
          const auto releaseHeapBefore = MemoryBudget::snapshot();
          if (MemoryBudget::shouldReleaseSdFontCachesForEpubInlineImage(releaseHeapBefore) &&
              self->renderer.releaseSdCardFontForLowMemory(self->fontId, /*preserveAdvanceTable=*/true)) {
            const auto releaseHeapAfter = MemoryBudget::snapshot();
            LOG_DBG("EHP", "Released SD font caches before image extraction: free=%u->%u maxAlloc=%u->%u src=%s",
                    releaseHeapBefore.freeHeap, releaseHeapAfter.freeHeap, releaseHeapBefore.maxAllocHeap,
                    releaseHeapAfter.maxAllocHeap, src.c_str());
          }

          if (self->lowMemoryImageFallback) {
            self->skipCurrentElement();
            return;
          } else {
            if (ImageDecoderFactory::isFormatSupported(resolvedPath)) {
              // Unsupported formats are skipped regardless of heap, so only
              // formats we can render should trip the low-memory image fallback.
              if (!MemoryBudget::hasHeapForEpubInlineImage("EHP", src.c_str())) {
                self->lowMemoryImageFallback = true;
                self->skipCurrentElement();
                return;
              }

              // Create a unique filename for the cached image
              std::string ext;
              size_t extPos = resolvedPath.rfind('.');
              if (extPos != std::string::npos) {
                ext = resolvedPath.substr(extPos);
              }
              std::string cachedImagePath = self->imageBasePath + std::to_string(self->imageCounter++) + ext;

              // Read just enough compressed data to find dimensions. The full
              // image remains inside the EPUB until its page is first rendered.
              ImageDimensions dims = {0, 0};
              ImageDimsProbe headerProbe;
              bool gotDimensions =
                  self->epub->readItemContentsToStream(resolvedPath, headerProbe, 1024, /*allowEarlyStop=*/true) &&
                  headerProbe.getDimensions(dims);
              std::string sourcePath;
              if (gotDimensions) {
                sourcePath = resolvedPath;
              } else if (self->epub->extractItemToFile(resolvedPath, cachedImagePath)) {
                // Unusual headers fall back to the existing full-file decoder.
                // Retry only if needed to tolerate slow SD-card sync.
                ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(cachedImagePath);
                for (int attempt = 0; attempt < 3 && !gotDimensions; attempt++) {
                  if (attempt > 0) {
                    delay(50);
                  }
                  gotDimensions = decoder && decoder->getDimensions(cachedImagePath, dims);
                }
              }

              if (gotDimensions) {
                if (!MemoryBudget::hasHeapForEpubInlineImage("EHP", cachedImagePath.c_str())) {
                  self->lowMemoryImageFallback = true;
                  Storage.remove(cachedImagePath.c_str());
                  self->skipCurrentElement();
                  return;
                }

                int displayWidth = 0;
                int displayHeight = 0;
                const float emSize = static_cast<float>(self->renderer.getFontAscenderSize(self->fontId));
                CssStyle imgStyle;
                if (!self->isLightMode()) {
                  imgStyle = self->cssParser
                                 ? (self->usesSimpleCssLookup()
                                        ? self->cssParser->resolveStyle("img", classAttr)
                                        : self->cssParser->resolveStyle("img", classAttr, self->ancestorStack_))
                                 : CssStyle{};
                  // Merge inline style (e.g. style="height: 2em") so it overrides stylesheet rules
                  if (!styleAttr.empty()) {
                    imgStyle.applyOver(CssParser::parseInlineStyle(styleAttr));
                  }
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
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
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
                } else if (hasCssWidth && !hasCssHeight && dims.width > 0 && dims.height > 0) {
                  // Use CSS width (resolve % against container width) and derive height from aspect ratio
                  displayWidth =
                      static_cast<int>(imgStyle.imageWidth.toPixels(emSize, static_cast<float>(containerWidth)) + 0.5f);
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
                }

                // Flush any pending text block so it appears before the image
                if (self->partWordBufferIndex > 0) {
                  self->flushPartWordBuffer();
                }
                bool openerNumberPrecededImage = false;
                if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
                  const BlockStyle parentBlockStyle = self->currentTextBlock->getBlockStyle();
                  self->startNewTextBlock(parentBlockStyle);
                  // The block synthesized above copies the heading style. Inside a chapter opener that
                  // style still carries the heading's drop margin (the large gap that pushes "Chapter N"
                  // down the page), which was already spent positioning the number. Remember that so it
                  // is not re-applied a second time as the ornament's top gap.
                  openerNumberPrecededImage = self->headingOpenerActive;
                }

                int16_t imageMarginTop = 0;
                int16_t imageMarginBottom = 0;
                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  const auto& bs = self->currentTextBlock->getBlockStyle();
                  imageMarginTop = openerNumberPrecededImage ? 0 : bs.topInset();
                  if (self->blockStyleCount_ > 1) {
                    imageMarginBottom = self->blockStyleBuf_[self->blockStyleCount_ - 1].bottomInset();
                  }
                }

                // Keep a chapter number with its first ornament, but let later heading images continue on the
                // next page instead of overflowing the viewport (including the reserved status-bar area).
                const bool keepFirstOpenerImageWithHeading =
                    self->headingOpenerActive && (!self->currentPage || !self->currentPage->hasImages());
                if (!keepFirstOpenerImageWithHeading && self->currentPage && !self->currentPage->elements.empty() &&
                    (self->currentPageNextY + imageMarginTop + displayHeight + imageMarginBottom >
                     self->viewportHeight)) {
                  self->completeCurrentPage();
                  self->completedPageCount++;
                  self->stopPreviewIfPageLimitReached();
                  if (self->previewStopRequested) {
                    return;
                  }
                  if (!self->startNewPage("image page break")) {
                    return;
                  }
                } else if (!self->currentPage) {
                  if (!self->startNewPage("image page")) {
                    return;
                  }
                }

                // A viewport-height image leaves no room for a container top
                // margin, including after the page break above starts a fresh page.
                // Keep the placement inside the renderable viewport.
                if (self->currentPageNextY + imageMarginTop + displayHeight > self->viewportHeight) {
                  const int remainingTopMargin = self->viewportHeight - displayHeight - self->currentPageNextY;
                  imageMarginTop = static_cast<int16_t>(std::max(0, remainingTopMargin));
                }
                self->currentPageNextY += imageMarginTop;
                self->attachPendingPublisherPageMarkers(self->currentPageNextY);

                // Create ImageBlock and add to page
                auto imageBlock = makeUniqueNoThrow<ImageBlock>(std::move(cachedImagePath), std::move(sourcePath),
                                                                displayWidth, displayHeight);
                if (!imageBlock) {
                  LOG_ERR("EHP", "Failed to create ImageBlock");
                  self->lowMemoryAbort = true;
                  return;
                }
                int xPos = (self->viewportWidth - displayWidth) / 2;
                auto pageImage = makeUniqueNoThrow<PageImage>(std::move(imageBlock), xPos, self->currentPageNextY);
                if (!pageImage) {
                  LOG_ERR("EHP", "Failed to create PageImage");
                  self->lowMemoryAbort = true;
                  return;
                }
                self->currentPage->elements.push_back(std::move(pageImage));
                self->setCurrentPageVisibleOffset(self->visibleTextOffset);
                self->markCurrentPageFromCurrentElement();
                self->currentPageNextY += displayHeight + imageMarginBottom;

                if (self->currentTextBlock && self->currentTextBlock->isEmpty()) {
                  BlockStyle afterImageStyle = self->blockStyleBuf_[self->blockStyleCount_ - 1].withoutBottom();
                  if (self->headingOpenerActive) {
                    // The title run trailing the ornament copies the heading style too and would inherit
                    // the same drop margin, stacking a second heading-sized gap under the image. Replace
                    // it with a modest half-line gap so the title sits just below the ornament instead.
                    afterImageStyle.marginTop = static_cast<int16_t>(self->effectiveLineHeight() / 2);
                    afterImageStyle.paddingTop = 0;
                  }
                  self->currentTextBlock->setBlockStyle(afterImageStyle);
                }

                self->pushCssAncestor(self->depth, name, classAttr);
                self->depth += 1;
                return;
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
                LOG_ERR("EHP", "Failed to get image dimensions");
              }
            }  // isFormatSupported
          }
        }
      }

      // Fallback to alt text if image processing fails
      if (!alt.empty()) {
        alt = "[Image: " + alt + "]";
        self->startNewTextBlock(self->blockStyleBuf_[self->blockStyleCount_ - 1]
                                    .getCombinedBlockStyle(centeredBlockStyle, BlockStyle::CombineAxis::Horizontal)
                                    .withoutBottom());
        self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
        self->pushCssAncestor(self->depth, name, classAttr);
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

  // Ruby tag handling
  if (strcmp(name, "ruby") == 0) {
    // <ruby> is an inline element: a base that follows text with no whitespace between them
    // continues the same visual word, exactly like <b>/<i> handling in endElement().
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
      self->nextWordContinues = true;
    }
    self->inRuby = true;
    self->rubyStartWordIndex = self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0;
    if (self->currentTextBlock) {
      self->currentTextBlock->ensureRubyCapacity();
    }
    self->rubyTextBuffer.clear();
    self->depth += 1;
    return;
  }
  if (strcmp(name, "rt") == 0) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    self->collectingRubyText = true;
    self->depth += 1;
    return;
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
      self->currentFootnote.linkId = self->nextFootnoteLinkId;
      self->nextFootnoteLinkId = static_cast<uint8_t>((self->nextFootnoteLinkId % 63) + 1);
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
      applySmallCapsToEntry(entry, cssStyle);
      applyVerticalAlignToEntry(entry, cssStyle);
      if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
        self->inlineStyleBuf_[self->inlineStyleCount_++] = entry;
      } else {
        LOG_ERR("EHP", "inline style stack overflow (anchor)");
      }
      self->updateEffectiveInlineStyle();

      // Skip CSS resolution — we already handled styling for this <a> tag
      self->pushCssAncestor(self->depth, name, classAttr);
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

  if (!self->embeddedStyle || self->isLightMode()) {
    stripPublisherSpacing(userAlignmentBlockStyle);
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
    if (self->isLightMode()) {
      self->skipCurrentElement();
      return;
    }
    auto hrBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Left, self->viewportWidth);
    if (!self->embeddedStyle || self->isLightMode()) {
      stripPublisherSpacing(hrBlockStyle);
    }
    self->emitHorizontalRule(hrBlockStyle);
    self->pushCssAncestor(self->depth, name, classAttr);
    self->depth += 1;
    return;
  }

  const bool cssPageBreakBefore =
      userAlignmentBlockStyle.pageBreakBefore &&
      (matches(name, HEADER_TAGS, std::size(HEADER_TAGS)) || matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS)));
  if (cssPageBreakBefore && ((self->currentTextBlock && !self->currentTextBlock->isEmpty()) ||
                             (self->currentPage && !self->currentPage->elements.empty()))) {
    if (self->partWordBufferIndex > 0) {
      self->flushPartWordBuffer();
    }
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->makePages();
    }
    if (self->currentPage && !self->currentPage->elements.empty()) {
      self->completeCurrentPage();
      self->completedPageCount++;
      self->stopPreviewIfPageLimitReached();
      if (self->previewStopRequested) {
        return;
      }
      self->currentPageNextY = 0;
    }
  }

  if (matches(name, HEADER_TAGS, std::size(HEADER_TAGS))) {
    self->headingDepth = self->depth;
    self->headingOpenerActive = true;
    self->currentCssStyle = cssStyle;
    auto headerBlockStyle = BlockStyle::fromCssStyle(cssStyle, emSize, CssTextAlign::Center, self->viewportWidth);
    headerBlockStyle.textAlignDefined = true;
    if (self->embeddedStyle && cssStyle.hasTextAlign()) {
      headerBlockStyle.alignment = cssStyle.textAlign;
    }
    if (!self->embeddedStyle || self->isLightMode()) {
      stripPublisherSpacing(headerBlockStyle);
    }
    const auto accumulated = self->blockStyleBuf_[self->blockStyleCount_ - 1].getCombinedBlockStyle(
        headerBlockStyle, BlockStyle::CombineAxis::Horizontal);
    if (self->blockStyleCount_ < MAX_BLOCK_STYLE_DEPTH) {
      self->blockStyleBuf_[self->blockStyleCount_++] = accumulated;
    } else {
      LOG_ERR("EHP", "block style stack overflow (header)");
    }
    self->startNewTextBlock(accumulated.withoutBottom());
    self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    self->updateEffectiveInlineStyle();
  } else if (matches(name, BLOCK_TAGS, std::size(BLOCK_TAGS)) || strcmp(name, "caption") == 0) {
    if (self->headingOpenerActive) {
      self->headingOpenerActive = false;
    }
    if (strcmp(name, "br") == 0) {
      if (self->partWordBufferIndex > 0) {
        // flush word preceding <br/> to currentTextBlock before calling startNewTextBlock
        self->flushPartWordBuffer();
      }
      // A <br> after text is a line break: start the next block without the
      // container's vertical margins, which browsers do not re-apply at a
      // line break. A <br> that leaves its block empty (a consecutive or
      // standalone <br>) retains those margins and becomes a scene break.
      // Use the active stack style rather than the current block so styles
      // from a closed element cannot leak into the next paragraph.
      BlockStyle brStyle = self->blockStyleBuf_[self->blockStyleCount_ - 1];
      if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
        brStyle = brStyle.withoutTop().withoutBottom();
      }
      brStyle.fromBrElement = true;
      self->startNewTextBlock(brStyle);
    } else {
      self->currentCssStyle = cssStyle;
      const auto accumulated = self->blockStyleBuf_[self->blockStyleCount_ - 1].getCombinedBlockStyle(
          userAlignmentBlockStyle, BlockStyle::CombineAxis::Horizontal);
      if (self->blockStyleCount_ < MAX_BLOCK_STYLE_DEPTH) {
        self->blockStyleBuf_[self->blockStyleCount_++] = accumulated;
      } else {
        LOG_ERR("EHP", "block style stack overflow (block)");
      }
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
                                        self->honorsPublisherDecorations() && self->effectiveBackgroundBlack, 0,
                                        self->visibleTextOffset);
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
    applySmallCapsToEntry(entry, cssStyle);
    if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
      self->inlineStyleBuf_[self->inlineStyleCount_++] = entry;
    } else {
      LOG_ERR("EHP", "inline style stack overflow (underline)");
    }
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
    applySmallCapsToEntry(entry, cssStyle);
    if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
      self->inlineStyleBuf_[self->inlineStyleCount_++] = entry;
    } else {
      LOG_ERR("EHP", "inline style stack overflow (strikethrough)");
    }
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
      entry.underline = (cssStyle.textDecoration & CssTextDecoration::Underline) != CssTextDecoration::None;
      entry.hasStrikethrough = true;
      entry.strikethrough = (cssStyle.textDecoration & CssTextDecoration::LineThrough) != CssTextDecoration::None;
    }
    if (cssStyle.hasBackgroundBlack()) {
      entry.hasBackgroundBlack = true;
      entry.backgroundBlack = cssStyle.backgroundBlack;
    }
    applyDirectionToEntry(entry, cssStyle);
    applySmallCapsToEntry(entry, cssStyle);
    if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
      self->inlineStyleBuf_[self->inlineStyleCount_++] = entry;
    } else {
      LOG_ERR("EHP", "inline style stack overflow (bold)");
    }
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
      entry.underline = (cssStyle.textDecoration & CssTextDecoration::Underline) != CssTextDecoration::None;
      entry.hasStrikethrough = true;
      entry.strikethrough = (cssStyle.textDecoration & CssTextDecoration::LineThrough) != CssTextDecoration::None;
    }
    if (cssStyle.hasBackgroundBlack()) {
      entry.hasBackgroundBlack = true;
      entry.backgroundBlack = cssStyle.backgroundBlack;
    }
    applyDirectionToEntry(entry, cssStyle);
    applySmallCapsToEntry(entry, cssStyle);
    if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
      self->inlineStyleBuf_[self->inlineStyleCount_++] = entry;
    } else {
      LOG_ERR("EHP", "inline style stack overflow (italic)");
    }
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
    ChapterHtmlSlimParser::applySmallCapsToEntry(entry, cssStyle);
    if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
      self->inlineStyleBuf_[self->inlineStyleCount_++] = entry;
    } else {
      LOG_ERR("EHP", "inline style stack overflow (sup/sub)");
    }
    self->updateEffectiveInlineStyle();
  } else if (strcmp(name, "span") == 0 || !isHeaderOrBlock(name)) {
    // Handle span and other inline elements for CSS styling
    if (cssStyle.hasFontWeight() || cssStyle.hasFontStyle() || cssStyle.hasTextDecoration() ||
        cssStyle.hasBackgroundBlack() || cssStyle.hasVerticalAlign() || cssStyle.hasDirection() ||
        cssStyle.hasFontVariantCaps()) {
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
        entry.underline = (cssStyle.textDecoration & CssTextDecoration::Underline) != CssTextDecoration::None;
        entry.hasStrikethrough = true;
        entry.strikethrough = (cssStyle.textDecoration & CssTextDecoration::LineThrough) != CssTextDecoration::None;
      }
      if (cssStyle.hasBackgroundBlack()) {
        entry.hasBackgroundBlack = true;
        entry.backgroundBlack = cssStyle.backgroundBlack;
      }
      applyDirectionToEntry(entry, cssStyle);
      entry.setsParagraphDirection = strcmp(name, "html") == 0 || strcmp(name, "body") == 0;
      applySmallCapsToEntry(entry, cssStyle);
      applyVerticalAlignToEntry(entry, cssStyle);
      if (self->inlineStyleCount_ < MAX_INLINE_STYLE_DEPTH) {
        self->inlineStyleBuf_[self->inlineStyleCount_++] = entry;
      } else {
        LOG_ERR("EHP", "inline style stack overflow (span/inline)");
      }
      self->updateEffectiveInlineStyle();
    }
  }

  // Unprocessed tag, just increasing depth and continue forward
  self->pushCssAncestor(self->depth, name, classAttr);
  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (self->isScanningForPreviewAnchor() || self->previewStopRequested) {
    return;
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

  // Keep the source coordinate independent of wrapping, fonts, and orientation.
  // `head`/`rp` are skipped above; synthetic table labels and ruby annotations do
  // not represent the document body position.
  const uint32_t callbackVisibleOffset = self->visibleTextOffset;
  const bool countVisibleOffsets = !self->syntheticCharacterData && !self->collectingRubyText;
  if (countVisibleOffsets) {
    const auto* ptr = reinterpret_cast<const unsigned char*>(s);
    const auto* const end = ptr + len;
    while (ptr < end) {
      utf8NextCodepoint(&ptr);
      self->visibleTextOffset++;
    }
  }

  // Collect ruby text instead of normal word processing
  if (self->collectingRubyText) {
    self->rubyTextBuffer.append(s, len);
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

  uint32_t codepointOffset = callbackVisibleOffset;
  for (int i = 0; i < len; i++) {
    const bool startsCodepoint = (static_cast<uint8_t>(s[i]) & 0xC0) != 0x80;
    if (isWhitespace(s[i])) {
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        self->flushPartWordBuffer();
      }
      // Whitespace is a real word boundary — reset continuation state
      self->nextWordContinues = false;
      if (startsCodepoint && countVisibleOffsets) codepointOffset++;
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
      self->partWordVisibleOffset = codepointOffset;
      self->nextWordContinues = true;  // Attach space to previous word (no break).
      self->flushPartWordBuffer();

      self->nextWordContinues = true;  // Next real word attaches to this space (no break).

      i++;  // Skip the second byte (0xA0)
      if (countVisibleOffsets) codepointOffset++;
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
      self->partWordVisibleOffset = codepointOffset;
      self->nextWordContinues = true;
      self->flushPartWordBuffer();

      self->nextWordContinues = true;

      i += 2;  // Skip the remaining two bytes (0x80 0xAF)
      if (countVisibleOffsets) codepointOffset++;
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
        uint32_t overflowOffset = self->partWordVisibleOffset;
        const auto* codepoint = reinterpret_cast<const unsigned char*>(self->partWordBuffer);
        const auto* const prefixEnd = codepoint + safeLen;
        while (codepoint < prefixEnd) {
          utf8NextCodepoint(&codepoint);
          overflowOffset++;
        }
        char saved[4];
        for (int j = 0; j < overflow; j++) {
          saved[j] = self->partWordBuffer[safeLen + j];
        }
        self->partWordBufferIndex = safeLen;
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
        for (int j = 0; j < overflow; j++) {
          self->partWordBuffer[j] = saved[j];
        }
        self->partWordBufferIndex = overflow;
        self->partWordVisibleOffset = overflowOffset;
      } else {
        self->flushPartWordBuffer();
        self->nextWordContinues = true;
      }
    }

    if (self->partWordBufferIndex == 0) {
      self->partWordVisibleOffset = codepointOffset;
    }
    self->partWordBuffer[self->partWordBufferIndex++] = s[i];
    if (startsCodepoint && countVisibleOffsets) codepointOffset++;
  }

  // If a paragraph keeps growing, perform the layout and consume all but the last line.
  // This keeps memory bounded for chapters with very long XHTML text runs even when
  // the text does not contain enough word boundaries to trip the word-count guard.
  if (self->partWordBufferIndex > 0 &&
      static_cast<size_t>(self->currentTextRunBytes) + static_cast<size_t>(self->partWordBufferIndex) >
          self->textRunBytesBeforeLayoutLimit()) {
    self->flushPartWordBuffer();
  }
  self->flushLongTextRunIfNeeded();
}

void XMLCALL ChapterHtmlSlimParser::defaultHandlerExpand(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);
  if (self->isScanningForPreviewAnchor() || self->previewStopRequested) {
    return;
  }
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
  if (self->isScanningForPreviewAnchor()) {
    if (self->depth > 0) {
      self->depth -= 1;
    }
    return;
  }
  if (self->previewStopRequested) {
    return;
  }
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

  // Ruby text: </rt> distributes ruby to base words, </ruby> resets ruby state
  if (strcmp(name, "rt") == 0) {
    self->collectingRubyText = false;
    if (self->inRuby && self->currentTextBlock) {
      const int currentWordCount = static_cast<int>(self->currentTextBlock->size());
      const int baseWordCount = currentWordCount - self->rubyStartWordIndex;
      std::string cleanRuby = trimAndNormalize(self->rubyTextBuffer);
      if (!cleanRuby.empty()) {
        if (baseWordCount > 0) {
          self->currentTextBlock->setRubyGroupAt(self->rubyStartWordIndex, baseWordCount, cleanRuby);
          self->rubyStartWordIndex = currentWordCount;
        } else if (self->rubyStartWordIndex > 0) {
          int leaderIdx = self->rubyStartWordIndex - 1;
          while (leaderIdx >= 0 &&
                 (self->currentTextBlock->getWordStyleAt(leaderIdx) & EpdFontFamily::RUBY_CONTINUE) != 0) {
            leaderIdx--;
          }
          if (leaderIdx >= 0) {
            std::string prevRuby = self->currentTextBlock->getRubyTextAt(leaderIdx);
            self->currentTextBlock->setRubyForWordAt(leaderIdx, prevRuby + cleanRuby);
          }
        }
      }
    }
    self->rubyTextBuffer.clear();
    // Inline close: the next base (e.g. 字 in <ruby>漢<rt>かん</rt>字<rt>じ</rt></ruby>) joins the
    // preceding one with no space. Whitespace in the source resets this in characterData().
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->nextWordContinues = true;
    }
    self->depth -= 1;
    return;
  }
  if (strcmp(name, "ruby") == 0 && self->inRuby) {
    self->inRuby = false;
    self->rubyStartWordIndex = -1;
    self->rubyTextBuffer.clear();
    // Inline close: text following </ruby> joins the annotated base with no space.
    if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
      self->nextWordContinues = true;
    }
    self->depth -= 1;
    return;
  }
  // Check if any style state will change after we decrement depth
  // If so, we MUST flush the partWordBuffer with the CURRENT style first
  // Note: depth hasn't been decremented yet, so we check against (depth - 1)
  const bool willPopStyleStack =
      self->inlineStyleCount_ > 0 && self->inlineStyleBuf_[self->inlineStyleCount_ - 1].depth == self->depth - 1;
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
      entry.linkId = self->currentFootnote.linkId;
      int wordIndex =
          self->wordsExtractedInBlock + (self->currentTextBlock ? static_cast<int>(self->currentTextBlock->size()) : 0);
      self->pendingFootnotes.push_back({wordIndex, entry});
      if (self->pendingFootnotes.size() >= MAX_PENDING_FOOTNOTES_BEFORE_LAYOUT && self->tableDepth == 0) {
        if (self->partWordBufferIndex > 0) {
          self->flushPartWordBuffer();
        }
        self->flushLongTextRunIfNeeded(true);
      }
    }
    self->insideFootnoteLink = false;
  }

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
    self->skipEndElementStateUntilDepth = INT_MAX;
  }

  if (self->tableDepth == 1 && (strcmp(name, "td") == 0 || strcmp(name, "th") == 0)) {
    self->finalizeCurrentTableCell();
    self->nextWordContinues = false;
  }

  if (self->tableDepth == 1 && (strcmp(name, "tr") == 0)) {
    if (self->currentCompactTable) {
      if (self->compactTableUnsupported || self->compactTableFlattened) {
        self->currentCompactTable->setFlattened();
      }
      TableFragmentRow row;
      std::vector<std::shared_ptr<TextBlock>> flatLines;
      std::vector<FootnoteEntry> rowFootnotes;
      rowFootnotes.reserve(Page::MAX_FOOTNOTES_PER_PAGE);
      uint32_t rowVisibleOffset = self->visibleTextOffset;
      const auto result = self->currentCompactTable->finishRow(row, flatLines, rowFootnotes, rowVisibleOffset);
      if (result == CompactTableLayout::RowResult::Abort) {
        self->lowMemoryAbort = true;
        return;
      }
      const bool flatten = result == CompactTableLayout::RowResult::Flatten;
      if (flatten) self->compactTableFlattened = true;
      if (!self->emitCompactTableRow(row, flatLines, rowFootnotes, rowVisibleOffset,
                                     self->currentCompactTable->fragmentColumnCount(), flatten)) {
        self->lowMemoryAbort = true;
        return;
      }
      self->nextWordContinues = false;
      return;
    }
    if (self->currentTableBuffer && self->currentTableBuffer->streaming && !self->streamCurrentTableRow()) {
      return;
    }
    self->nextWordContinues = false;
  }

  if (self->tableDepth == 1 && strcmp(name, "table") == 0) {
    if (self->currentCompactTable) {
      self->finishCompactTable();
      self->currentCompactTable.reset();
      self->compactTableFlattened = false;
      self->compactTableUnsupported = false;
    } else if (self->currentTableBuffer && self->currentTableBuffer->streaming) {
      self->finishStreamingTable(*self->currentTableBuffer);
      self->currentTableBuffer.reset();
    } else {
      self->emitCurrentTableBuffer();
    }
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
  if (self->inlineStyleCount_ > 0 && self->inlineStyleBuf_[self->inlineStyleCount_ - 1].depth == self->depth) {
    self->inlineStyleCount_--;
    self->updateEffectiveInlineStyle();
  }

  // Clear block style when leaving header or block elements
  if (headerOrBlockTag) {
    self->currentCssStyle.reset();
    self->updateEffectiveInlineStyle();

    // br is self-closing and not a container — it doesn't push/pop the stack.
    if (strcmp(name, "br") != 0 && self->blockStyleCount_ > 1) {
      // Apply closing element's bottom margin to the current text block so
      // container spacing appears after the element's content (on the last child),
      // not on the first child via the empty-block merge in startNewTextBlock.
      if (self->currentTextBlock) {
        const auto style = self->currentTextBlock->getBlockStyle();
        self->currentTextBlock->setBlockStyle(style.addBottom(self->blockStyleBuf_[self->blockStyleCount_ - 1]));
      }
      self->blockStyleCount_--;
      self->updateEffectiveInlineStyle();
    }
  }
  if (self->tableDepth == 1 && strcmp(name, "caption") == 0 && self->currentCompactTable && self->currentTextBlock) {
    // Captions are ordinary text, not table cells. Emit them before the first
    // grid row so compact-table capture never sees caption text without a
    // destination block.
    self->makePages();
    self->currentTextBlock.reset();
    self->currentTextRunBytes = 0;
    self->nextWordContinues = false;
  }
  if (self->headingDepth == self->depth) {
    self->headingDepth = -1;
  }

  if (strcmp(name, "html") == 0) {
    self->htmlEnded_ = true;
  }
}

void ChapterHtmlSlimParser::prewarmSectionAdvanceTable(FsFile& file) const {
  if (!renderer.isSdCardFont(fontId) || isPreviewBuild()) {
    return;
  }

  const auto heap = MemoryBudget::snapshot();
  if (!MemoryBudget::hasHeap(heap, MIN_FREE_HEAP_FOR_SECTION_PREWARM, MIN_MAX_ALLOC_FOR_SECTION_PREWARM)) {
    LOG_DBG("EHP", "Skipping section advance prewarm: low heap (free=%u, maxAlloc=%u, need %u/%u)", heap.freeHeap,
            heap.maxAllocHeap, MIN_FREE_HEAP_FOR_SECTION_PREWARM, MIN_MAX_ALLOC_FOR_SECTION_PREWARM);
    return;
  }

  std::unique_ptr<uint32_t[]> codepoints(new (std::nothrow) uint32_t[SECTION_ADVANCE_PREWARM_MAX_CODEPOINTS]);
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[SECTION_ADVANCE_PREWARM_READ_BUFFER_SIZE]);
  if (!codepoints || !buffer) {
    LOG_ERR("EHP", "Failed to allocate section advance prewarm buffers");
    file.seekSet(0);
    return;
  }

  const uint32_t startMs = millis();
  uint32_t cpCount = 0;
  uint32_t utf8Accumulator = 0;
  uint8_t utf8Remaining = 0;
  bool inTag = false;
  bool inEntity = false;
  bool hitCap = false;

  while (file.available() > 0 && !hitCap) {
    const size_t len = file.read(buffer.get(), SECTION_ADVANCE_PREWARM_READ_BUFFER_SIZE);
    if (len == 0) {
      LOG_DBG("EHP", "Section advance prewarm stopped after short read");
      break;
    }

    for (size_t i = 0; i < len && !hitCap; ++i) {
      const uint8_t byte = buffer[i];
      const char c = static_cast<char>(byte);

      if (inTag) {
        if (c == '>') {
          inTag = false;
        }
        continue;
      }

      if (inEntity) {
        if (c == ';' || isWhitespace(c)) {
          inEntity = false;
        } else if (c == '<') {
          inEntity = false;
          inTag = true;
        }
        continue;
      }

      if (c == '<') {
        inTag = true;
        resetPrewarmUtf8(utf8Accumulator, utf8Remaining);
        continue;
      }
      if (c == '&') {
        inEntity = true;
        resetPrewarmUtf8(utf8Accumulator, utf8Remaining);
        hitCap = appendUniquePrewarmCodepoint(' ', codepoints.get(), cpCount, SECTION_ADVANCE_PREWARM_MAX_CODEPOINTS);
        continue;
      }
      if (isWhitespace(c)) {
        resetPrewarmUtf8(utf8Accumulator, utf8Remaining);
        hitCap = appendUniquePrewarmCodepoint(' ', codepoints.get(), cpCount, SECTION_ADVANCE_PREWARM_MAX_CODEPOINTS);
        continue;
      }

      hitCap = feedPrewarmUtf8Byte(byte, codepoints.get(), cpCount, utf8Accumulator, utf8Remaining);
    }
  }

  if (utf8Remaining != 0 && !hitCap) {
    hitCap = appendUniquePrewarmCodepoint(REPLACEMENT_GLYPH, codepoints.get(), cpCount,
                                          SECTION_ADVANCE_PREWARM_MAX_CODEPOINTS);
  }
  if (hitCap) {
    LOG_DBG("EHP", "Section advance prewarm hit unique codepoint cap (%u)",
            static_cast<unsigned>(SECTION_ADVANCE_PREWARM_MAX_CODEPOINTS));
  }

  if (!file.seekSet(0)) {
    LOG_ERR("EHP", "Failed to rewind section file after advance prewarm");
    return;
  }

  if (cpCount == 0) {
    return;
  }

  renderer.ensureSdCardFontReady(fontId, codepoints.get(), cpCount, /*includeSpace=*/true, hyphenationEnabled,
                                 /*styleMask=*/0x0F);
  LOG_DBG("EHP", "Section advance prewarm: codepoints=%u time=%lu ms free=%u maxAlloc=%u",
          static_cast<unsigned>(cpCount), millis() - startMs, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

ChapterHtmlSlimParser::~ChapterHtmlSlimParser() { abortParse(); }

bool ChapterHtmlSlimParser::ensureInputFileOpen() {
  if (parseFile_) {
    return true;
  }
  if (!Storage.openFileForRead("EHP", filepath, parseFile_)) {
    return false;
  }
  if (parseFileOffset_ > 0 && !parseFile_.seek(parseFileOffset_)) {
    LOG_ERR("EHP", "Failed to seek parser input to %u", static_cast<unsigned>(parseFileOffset_));
    parseFile_.close();
    return false;
  }
  if (parseFileSize_ == 0) {
    parseFileSize_ = parseFile_.size();
  }
  return true;
}

void ChapterHtmlSlimParser::releaseInputFile() {
  if (!parseFile_) {
    return;
  }
  parseFileOffset_ = parseFile_.position();
  parseFile_.close();
}

bool ChapterHtmlSlimParser::beginParse() {
  malformedMarkupTruncated = false;
  htmlEnded_ = false;
  parseFileOffset_ = 0;
  parseFileSize_ = 0;
  // Runs before the render pass opens the file, so only one reader is ever open at a time.
  if (isPreviewBuild()) {
    locatePreviewBlockStart();
  }
  // Initialize block style stack with a root entry representing "no ancestor block elements".
  // The user's paragraph alignment is set as the default so child elements without explicit
  // text-align inherit it correctly through getCombinedBlockStyle.
  BlockStyle rootBlockStyle;
  rootBlockStyle.alignment = (this->paragraphAlignment == static_cast<uint8_t>(CssTextAlign::None))
                                 ? CssTextAlign::Justify
                                 : static_cast<CssTextAlign>(this->paragraphAlignment);
  if (!parseArena_.init(PARSE_ARENA_SLAB_SIZE)) {
    LOG_ERR("EHP", "Failed to init parse arena");
    lowMemoryAbort = true;
    return false;
  }
  inlineStyleBuf_ = arenaNewArray<StyleStackEntry>(parseArena_, MAX_INLINE_STYLE_DEPTH);
  blockStyleBuf_ = arenaNewArray<BlockStyle>(parseArena_, MAX_BLOCK_STYLE_DEPTH);
  if (!inlineStyleBuf_ || !blockStyleBuf_) {
    LOG_ERR("EHP", "Parse arena OOM for style stacks");
    lowMemoryAbort = true;
    parseArena_.release();
    return false;
  }
  inlineStyleCount_ = 0;
  blockStyleCount_ = 0;
  blockStyleBuf_[blockStyleCount_++] = rootBlockStyle;

  auto paragraphAlignmentBlockStyle = BlockStyle();
  paragraphAlignmentBlockStyle.textAlignDefined = true;
  const auto align = rootBlockStyle.alignment;
  paragraphAlignmentBlockStyle.alignment = align;
  if (!isPreviewBuild()) {
    startNewTextBlock(paragraphAlignmentBlockStyle);
    if (lowMemoryAbort) {
      parseArena_.release();
      inlineStyleBuf_ = nullptr;
      blockStyleBuf_ = nullptr;
      return false;
    }
  }

  if (!usesSimpleCssLookup()) {
    ancestorStack_.reserve(32);
  }

  activeParser = XML_ParserCreate(nullptr);
  if (!activeParser) {
    LOG_ERR("EHP", "Couldn't allocate memory for parser");
    lowMemoryAbort = true;
    parseArena_.release();
    inlineStyleBuf_ = nullptr;
    blockStyleBuf_ = nullptr;
    return false;
  }

  // Handle HTML entities (like &nbsp;) that aren't in XML spec or DTD
  // Using DefaultHandlerExpand preserves normal entity expansion from DOCTYPE
  XML_SetDefaultHandlerExpand(activeParser, defaultHandlerExpand);

  if (!Storage.openFileForRead("EHP", filepath, parseFile_)) {
    destroyXmlParser(activeParser);
    activeParser = nullptr;
    parseArena_.release();
    inlineStyleBuf_ = nullptr;
    blockStyleBuf_ = nullptr;
    return false;
  }
  parseFileSize_ = parseFile_.size();

  // Get file size to decide whether to show indexing popup.
  if (popupFn && parseFileSize_ >= MIN_SIZE_FOR_POPUP) {
    popupFn();
  }

  XML_SetUserData(activeParser, this);
  XML_SetElementHandler(activeParser, startElement, endElement);
  XML_SetCharacterDataHandler(activeParser, characterData);

  // Compute the time taken to parse and build pages
  parseStartTime_ = millis();
  prewarmSectionAdvanceTable(parseFile_);
  return true;
}

ChapterHtmlSlimParser::ParseStatus ChapterHtmlSlimParser::parseStep() {
  if (!activeParser) {
    LOG_ERR("EHP", "parseStep called without an active parser");
    return ParseStatus::Error;
  }
  if (!ensureInputFileOpen()) {
    LOG_ERR("EHP", "Failed to reopen parser input");
    return ParseStatus::Error;
  }

  void* const buf = XML_GetBuffer(activeParser, PARSE_BUFFER_SIZE);
  if (!buf) {
    LOG_ERR("EHP", "Couldn't allocate memory for buffer");
    return ParseStatus::Error;
  }

  const size_t len = parseFile_.read(buf, PARSE_BUFFER_SIZE);
  parseFileOffset_ = parseFile_.position();
  if (len == 0 && parseFile_.available() > 0) {
    LOG_ERR("EHP", "File read error");
    return ParseStatus::Error;
  }

  const bool done = parseFile_.available() == 0;
  const XML_Status parseStatus = XML_ParseBuffer(activeParser, static_cast<int>(len), done);
  if (parseStatus == XML_STATUS_ERROR && !previewStopRequested) {
    if (htmlEnded_) {
      LOG_DBG("EHP", "Ignoring trailing data after </html>: %s", XML_ErrorString(XML_GetErrorCode(activeParser)));
      return ParseStatus::Done;
    }
    LOG_ERR("EHP", "Parse error at line %lu:\n%s", XML_GetCurrentLineNumber(activeParser),
            XML_ErrorString(XML_GetErrorCode(activeParser)));
    if (isPreviewBuild()) {
      return ParseStatus::Error;
    }
    malformedMarkupTruncated = true;
    return ParseStatus::Done;
  }

  if (lowMemoryAbort) {
    LOG_ERR("EHP", "Aborting section parse due to low heap");
    return ParseStatus::Error;
  }

  if (done || previewStopRequested || parseStatus == XML_STATUS_SUSPENDED) {
    return ParseStatus::Done;
  }
  return ParseStatus::More;
}

void ChapterHtmlSlimParser::abortParse() {
  if (activeParser) {
    destroyXmlParser(activeParser);
    activeParser = nullptr;
  }
  if (parseFile_) {
    parseFile_.close();
  }
  parseFileOffset_ = 0;
  parseFileSize_ = 0;
  parseArena_.release();
  inlineStyleBuf_ = nullptr;
  inlineStyleCount_ = 0;
  blockStyleBuf_ = nullptr;
  blockStyleCount_ = 0;
}

bool ChapterHtmlSlimParser::finishParse() {
  if (activeParser) {
    LOG_DBG("EHP", "Time to parse and build pages: %lu ms (free=%u, maxAlloc=%u)", millis() - parseStartTime_,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    destroyXmlParser(activeParser);
    activeParser = nullptr;
  }
  parseFile_.close();
  parseFileOffset_ = 0;
  parseFileSize_ = 0;

  if (malformedMarkupTruncated) {
    LOG_DBG("EHP", "Malformed markup encountered; finalizing partial chapter content");
    flushMalformedPartialContent();
    if (lowMemoryAbort) {
      parseArena_.release();
      inlineStyleBuf_ = nullptr;
      blockStyleBuf_ = nullptr;
      return false;
    }
  }

  // Process last page if there is still text
  if (isPreviewBuild() && !previewAnchorFound) {
    LOG_ERR("EHP", "Preview anchor '%s' was not found", previewAnchor.c_str());
    abortParse();
    return false;
  }

  if (currentTextBlock && !previewStopRequested) {
    if (shouldAbortForLowMemory("final page layout")) {
      abortParse();
      return false;
    }
    makePages();
    if (lowMemoryAbort) {
      abortParse();
      return false;
    }
    if (!pendingAnchorId.empty()) {
      anchorData.push_back({std::move(pendingAnchorId), static_cast<uint16_t>(completedPageCount)});
      pendingAnchorId.clear();
      pendingAnchorFromInlineA = false;
    }
    currentTextBlock.reset();
  }
  if (!previewStopRequested && currentPage && !currentPage->elements.empty()) {
    completeCurrentPage();
    completedPageCount++;
    stopPreviewIfPageLimitReached();
  }
  currentPage.reset();

  if (malformedMarkupTruncated && !appendMalformedMarkupWarningPage()) {
    LOG_ERR("EHP", "Failed to append malformed markup warning page");
    parseArena_.release();
    inlineStyleBuf_ = nullptr;
    blockStyleBuf_ = nullptr;
    return false;
  }

  parseArena_.release();
  inlineStyleBuf_ = nullptr;
  blockStyleBuf_ = nullptr;

  return true;
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  if (!beginParse()) {
    return false;
  }
  for (;;) {
    const ParseStatus status = parseStep();
    if (status == ParseStatus::Error) {
      abortParse();
      return false;
    }
    if (status == ParseStatus::Done) {
      break;
    }
  }
  return finishParse();
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line, const uint32_t visibleOffset) {
  if (lowMemoryAbort) {
    return;
  }

  const int lineHeight = effectiveLineHeight() + line->getRubyShift(renderer.getFontAscenderSize(fontId));

  if (!currentPage) {
    if (!startNewPage("line layout")) {
      return;
    }
  }

  if (currentPageNextY + lineHeight > viewportHeight) {
    completeCurrentPage();
    completedPageCount++;
    stopPreviewIfPageLimitReached();
    if (previewStopRequested) {
      return;
    }
    if (!startNewPage("line page break")) {
      return;
    }
  }

  setCurrentPageVisibleOffset(visibleOffset);

  // Keep a link available on every page where its text is visible. Usually this
  // adds one compact entry; a long wrapped link can span lines or pages.
  for (uint16_t wordIndex = 0; wordIndex < line->wordCount(); ++wordIndex) {
    const uint8_t linkId = line->wordLinkId(wordIndex);
    if (linkId == 0) continue;
    const auto entry = std::find_if(pendingFootnotes.begin(), pendingFootnotes.end(),
                                    [linkId](const auto& pending) { return pending.second.linkId == linkId; });
    if (entry != pendingFootnotes.end()) {
      currentPage->addFootnote(entry->second.number, entry->second.href, entry->second.linkId);
    }
  }

  // Track cumulative words to retire links after laying out their final word.
  wordsExtractedInBlock += line->wordCount();
  auto footnoteIt = pendingFootnotes.begin();
  while (footnoteIt != pendingFootnotes.end() && footnoteIt->first <= wordsExtractedInBlock) {
    currentPage->addFootnote(footnoteIt->second.number, footnoteIt->second.href, footnoteIt->second.linkId);
    ++footnoteIt;
  }
  pendingFootnotes.erase(pendingFootnotes.begin(), footnoteIt);
  attachPendingPublisherPageMarkers(currentPageNextY);

  // Apply horizontal left inset (margin + padding) as x position offset
  const int16_t xOffset = line->getBlockStyle().leftInset();
  auto pageLine = makeUniqueNoThrow<PageLine>(line, xOffset, currentPageNextY);
  if (!pageLine) {
    LOG_ERR("EHP", "Failed to create PageLine");
    lowMemoryAbort = true;
    return;
  }
  currentPage->elements.push_back(std::move(pageLine));
  markCurrentPageFromCurrentTextBlock();
  currentPageNextY += lineHeight;
}

int ChapterHtmlSlimParser::effectiveLineHeight() const {
  return std::max(1, static_cast<int>(renderer.getLineHeight(fontId) * lineCompression + 0.5f));
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

  // Apply top spacing before the paragraph (stored in pixels). An
  // intermediate text-run flush has already emitted the first lines and
  // consumed this spacing, so do not apply it again to the remainder.
  const BlockStyle& blockStyle = currentTextBlock->getBlockStyle();
  const int lineHeight = effectiveLineHeight();
  if (!currentTextBlock->isContinuation()) {
    if (blockStyle.marginTop > 0) {
      currentPageNextY += blockStyle.marginTop;
    }
    if (blockStyle.paddingTop > 0) {
      currentPageNextY += blockStyle.paddingTop;
    }
  }

  // Calculate effective width accounting for horizontal margins/padding
  const int horizontalInset = blockStyle.totalHorizontalInset();
  const uint16_t effectiveWidth =
      (horizontalInset < viewportWidth) ? static_cast<uint16_t>(viewportWidth - horizontalInset) : viewportWidth;

  if (!currentTextBlock->layoutAndExtractLines(
          renderer, fontId, effectiveWidth, [this](const std::shared_ptr<TextBlock>& textBlock, const uint32_t offset) {
            addLineToPage(textBlock, offset);
          })) {
    LOG_ERR("EHP", "Failed to lay out text block");
    lowMemoryAbort = true;
    return;
  }
  if (lowMemoryAbort) {
    return;
  }

  // Fallback: transfer any remaining pending footnotes to current page.
  // Normally addLineToPage handles this via word-index tracking, but this catches
  // edge cases where a footnote's word index equals the exact block size.
  if (!pendingFootnotes.empty() && currentPage) {
    for (const auto& [idx, fn] : pendingFootnotes) {
      currentPage->addFootnote(fn.number, fn.href, fn.linkId);
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

  if (blockStyle.pageBreakAfter && currentPage && !currentPage->elements.empty()) {
    completeCurrentPage();
    completedPageCount++;
    stopPreviewIfPageLimitReached();
    if (previewStopRequested) {
      return;
    }
    currentPageNextY = 0;
  }
}
