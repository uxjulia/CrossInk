#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "blocks/BlockStyle.h"
#include "blocks/TextBlock.h"

class GfxRenderer;
struct Arena;
template <typename T>
class ArenaVector;

class ParsedText {
 public:
  struct SourceGap {
    uint16_t beforeCharacter;
    uint32_t length;
  };
  void setInitialLetter(const char* prefix);
  struct InlineImagePlacement {
    uint16_t id;
    int16_t x;
    uint16_t height;
  };

 private:
  // words/rubyTexts are std::deque, not std::vector: a paragraph can hold thousands
  // of tokens (CJK splits every character), and a vector grows by reallocating its
  // whole element array into one contiguous block (32 B/std::string -> 64-128 KB at
  // a few thousand tokens). On the ESP32-C3 that single large contiguous request
  // fails under a fragmented, BLE-resident heap and the throwing operator new
  // abort()s the firmware (fresh-open CJK crash). A deque grows in fixed ~512 B nodes
  // (largest contiguous alloc stays ~2 KB regardless of token count), so it never
  // triggers that. The per-token parallel arrays below stay vectors: 1 byte / 1 bit
  // each, they never approach the contiguous-block ceiling.
  char initialLetter[13] = {};
  std::deque<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<uint32_t> wordFontFamilies;  // allocated only for publisher fonts
  std::vector<uint8_t> wordFontSizes;      // empty for ordinary paragraphs; one byte per token otherwise
  int wordFontId(const GfxRenderer& renderer, int base, size_t index) const;
  std::vector<bool> wordContinues;         // true = word attaches to previous (no space before it)
  std::vector<bool> wordNoSpaceBefore;     // true = may break before token, but no synthetic space when joined
  std::vector<uint8_t> wordFocusBoundary;  // UTF-8 byte offset where the regular suffix starts; 0 = no split
  std::vector<bool> wordGuideDotBefore;    // true = virtual guide dot belongs between previous token and this one
  std::vector<uint8_t> wordBackgroundBlack;
  // Sparse CSS inline padding. The value shifts one rendered token without
  // becoming text, so empty styled spans still occupy their intended width.
  struct InlinePadding {
    size_t wordIndex;
    int16_t pixels;
  };
  std::deque<InlinePadding> inlinePaddings;
  // Source character coordinates use compact deltas while a paragraph is
  // pending, then travel with rendered words for layout-independent anchors.
  struct VisibleOffsetRebase {
    size_t wordIndex;
    uint32_t base;
  };
  std::vector<uint16_t> wordVisibleOffsetDeltas;
  std::vector<bool> wordHasSourceText;
  std::vector<uint32_t> wordSourceOffsets;
  // Sparse corrections preserve raw source positions across NFC composition,
  // removed soft hyphens, and hidden text inside a visible word.
  struct SourceCorrection {
    size_t wordIndex;
    uint16_t normalizedIndex;
    uint32_t extra;
  };
  std::vector<SourceCorrection> sourceCorrections;
  std::vector<SourceCorrection> sourceCorrectionScratch;
  uint32_t sourceLengthAt(size_t wordIndex, uint32_t characters) const;
  void splitSourceCorrections(size_t wordIndex, uint32_t characters);
  void stripSourceSoftHyphens(size_t wordIndex);
  std::vector<TextBlock::SourceRange> lineSourceRangesScratch;
  uint32_t visibleOffsetBase = 0;
  std::vector<VisibleOffsetRebase> visibleOffsetRebases;
  // Populated only during a stable-page jump build. Chunked storage avoids a
  // large contiguous request for long CJK paragraphs on constrained devices.
  std::deque<uint32_t> wordReferenceOffsets;
  std::deque<std::string> rubyTexts;
  bool extraParagraphSpacing;
  bool forceParagraphIndents;
  bool hyphenationEnabled;
  bool focusReadingEnabled;
  bool guideReadingEnabled;
  uint8_t wordSpacing;
  int8_t characterSpacing;
  BlockStyle blockStyle;
  bool hasRtlWord;
  bool trackReferenceOffsets;
  // True after an intermediate flush leaves the rest of the same paragraph
  // buffered. The next layout pass must not apply first-line paragraph rules.
  bool isContinuation_ = false;
  bool allowCharacterBreaks_ = false;
  std::vector<std::string> reorderedWordsScratch;
  std::vector<EpdFontFamily::Style> reorderedStylesScratch;
  std::vector<bool> reorderedContinuesScratch;
  std::vector<bool> reorderedNoSpaceBeforeScratch;
  std::vector<uint8_t> reorderedFocusBoundaryScratch;
  std::vector<bool> reorderedGuideDotBeforeScratch;
  std::vector<uint8_t> reorderedBackgroundBlackScratch;
  std::vector<int16_t> reorderedLeadingPaddingScratch;
  std::vector<std::string> lineWordsScratch;
  std::vector<EpdFontFamily::Style> lineStylesScratch;
  std::vector<uint16_t> lineWidthsScratch;
  std::vector<uint8_t> lineFocusBoundaryScratch;
  std::vector<bool> lineGuideDotBeforeScratch;
  std::vector<bool> lineHasSpaceBeforeScratch;
  std::vector<uint8_t> lineBackgroundBlackScratch;
  std::vector<int16_t> lineLeadingPaddingScratch;
  std::vector<uint16_t> visualOrderScratch;
  std::vector<InlineImagePlacement> lineImagesScratch;

  void reserveTokenCapacity(size_t additionalTokens);
  int resolveFirstLineIndent(bool isFirstLine, const GfxRenderer& renderer, int fontId) const;
  bool calculateGapMetrics(ArenaVector<int16_t>& naturalGaps, ArenaVector<uint8_t>& gapSlots,
                           const GfxRenderer& renderer, int fontId);
  bool computeLineBreaks(Arena& scratchArena, const GfxRenderer& renderer, int fontId, int pageWidth,
                         ArenaVector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                         std::vector<bool>& noSpaceBeforeVec, ArenaVector<int16_t>& naturalGaps,
                         ArenaVector<uint8_t>& gapSlots, ArenaVector<size_t>& lineBreakIndices);
  bool computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                   ArenaVector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                   std::vector<bool>& noSpaceBeforeVec, ArenaVector<size_t>& lineBreakIndices,
                                   size_t maxLines);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            ArenaVector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  bool splitTokenAtCodepointBoundary(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                                     ArenaVector<uint16_t>& wordWidths);
  uint32_t visibleOffsetBaseAt(size_t wordIndex) const;
  uint32_t visibleOffsetAt(size_t wordIndex) const;
  void pushVisibleOffset(uint32_t offset);
  void insertVisibleOffset(size_t wordIndex, uint32_t offset);
  void eraseVisibleOffsetPrefix(size_t count);
  int16_t inlinePaddingBefore(size_t wordIndex) const;
  void shiftInlinePaddingsAfter(size_t wordIndex);
  void eraseInlinePaddingPrefix(size_t count);
  int calculateRubyExtraStartOffset(size_t wordIdx, size_t maxWordIdx, const GfxRenderer& renderer, int fontId) const;
  int calculateRubyExtraEndOffset(size_t lineStartIdx, size_t lineBreakIdx, const GfxRenderer& renderer,
                                  int fontId) const;
  bool extractLine(Arena& scratchArena, size_t breakIndex, int pageWidth, const ArenaVector<uint16_t>& wordWidths,
                   const std::vector<bool>& continuesVec, const std::vector<bool>& noSpaceBeforeVec,
                   const ArenaVector<int16_t>& naturalGaps, const ArenaVector<uint8_t>& gapSlots,
                   const ArenaVector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>, uint32_t, uint32_t)>& processLine,
                   const GfxRenderer& renderer, int fontId);
  bool calculateWordWidths(ArenaVector<uint16_t>& wordWidths, const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const bool extraParagraphSpacing, const bool forceParagraphIndents = false,
                      const bool hyphenationEnabled = false, const bool focusReadingEnabled = false,
                      const bool guideReadingEnabled = false, const uint8_t wordSpacing = 0,
                      const BlockStyle& blockStyle = BlockStyle(), const bool trackReferenceOffsets = false,
                      const int8_t characterSpacing = 0)
      : extraParagraphSpacing(extraParagraphSpacing),
        forceParagraphIndents(forceParagraphIndents),
        hyphenationEnabled(hyphenationEnabled),
        focusReadingEnabled(focusReadingEnabled),
        guideReadingEnabled(guideReadingEnabled),
        wordSpacing(wordSpacing),
        characterSpacing(characterSpacing),
        blockStyle(blockStyle),
        hasRtlWord(false),
        trackReferenceOffsets(trackReferenceOffsets) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool underline = false, bool attachToPrevious = false,
               bool backgroundBlack = false, uint8_t linkId = 0, uint32_t visibleTextOffset = 0,
               uint32_t referenceTextOffset = 0, int16_t leadingPadding = 0, bool noSpaceBefore = false,
               uint8_t pointSize = 0, uint32_t fontFamily = 0, bool hasSourceText = true,
               uint32_t sourceTextOffset = UINT32_MAX, const std::vector<SourceGap>& sourceGaps = {});
  void addInlineImage(uint16_t id, uint16_t width, uint16_t height, bool attachToPrevious, uint32_t visibleTextOffset,
                      uint32_t referenceTextOffset);
  const std::vector<InlineImagePlacement>& currentLineImages() const { return lineImagesScratch; }
  void setRubyForWordAt(size_t index, const std::string& ruby);
  void setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby);
  EpdFontFamily::Style getWordStyleAt(size_t index) const {
    return index < wordStyles.size() ? wordStyles[index] : EpdFontFamily::REGULAR;
  }
  std::string getRubyTextAt(size_t index) const { return index < rubyTexts.size() ? rubyTexts[index] : std::string(); }
  void ensureRubyCapacity();
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  BlockStyle& getBlockStyle() { return blockStyle; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  bool isContinuation() const { return isContinuation_; }
  void setContinuation(bool continuation) { isContinuation_ = continuation; }
  bool layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>, uint32_t, uint32_t)>& processLine,
                             bool includeLastLine = true, size_t maxLines = SIZE_MAX);
  bool layoutAndExtractLinesPreservingSource(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                             bool allowCharacterBreaks = false) const;
};
