#pragma once

#include <AppCapabilities.h>
#include <Epub/Page.h>
#include <Epub/WordSpacing.h>
#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

#include "ReaderRuling.h"

template <size_t TextCapacity, size_t WordCapacity, size_t LineCapacity, bool KeepSourceBlocks>
class ReaderPreviewModel {
 public:
  static constexpr size_t TEXT_CAPACITY = TextCapacity;
  static constexpr size_t WORD_CAPACITY = WordCapacity;
  static constexpr size_t LINE_CAPACITY = LineCapacity;
  static constexpr bool RETAINS_SOURCE_BLOCKS = KeepSourceBlocks;

  bool capture(const Page& page, const GfxRenderer& renderer, const int fontId, const uint8_t lineHeightPercent,
               const int xOffset = 0, const int yOffset = 0) {
    clear();
    sourceXOffset = xOffset;
    sourceYOffset = yOffset;
    sourceLineHeightPixels = std::max(1, (renderer.getLineHeight(fontId) * lineHeightPercent + 50) / 100);
    bool previousElementWasLine = false;
    bool previousLineEndedWithInsertedHyphen = false;
    uint16_t previousLineLastWord = 0;
    for (const auto& element : page.elements) {
      if (!element || element->getTag() != TAG_PageLine) {
        previousElementWasLine = false;
        previousLineEndedWithInsertedHyphen = false;
        continue;
      }
      if (lineCount >= lines.size()) break;
      const auto& pageLine = static_cast<const PageLine&>(*element);
      const auto& block = pageLine.getBlock();
      if (!block) continue;
      characterSpacing = block->getCharacterSpacing();
      if (wordCount + block->wordCount() > words.size()) break;

      size_t blockTextSize = 0;
      for (uint16_t i = 0; i < block->wordCount(); ++i) {
        blockTextSize += static_cast<size_t>(block->wordTextLen(i)) + 1U;
      }
      if (blockTextSize > text.size() - textSize) break;

      Line& line = lines[lineCount++];
      line.x = pageLine.xPos;
      line.y = pageLine.yPos - block->getBlockStyle().ruledTextOffset;
      if constexpr (KeepSourceBlocks) line.sourceBlock = block;
      line.firstWord = wordCount;
      line.wordCount = block->wordCount();
      line.style = block->getBlockStyle();
      line.sourceIndentVisible = block->wordCount() > 0 && block->wordXpos(0) != 0;
      line.startsParagraph =
          !previousElementWasLine || lineCount == 1 || startsNewParagraph(lines[lineCount - 2], line, *block);
      if (!hasBaseline) {
        firstLineY = line.y;
        hasBaseline = true;
      }

      for (uint16_t i = 0; i < block->wordCount(); ++i) {
        const uint16_t textLength = block->wordTextLen(i);
        Word& word = words[wordCount++];
        word.textOffset = textSize;
        word.x = block->wordXpos(i);
        word.style = block->wordStyle(i);
        word.hidden = block->wordHidden(i);
        word.blackBackground = (block->wordFlags(i) & TextBlock::WORD_FLAG_BACKGROUND_BLACK) != 0;
        word.backgroundSpace = block->wordBackgroundSpace(i);
        word.focusBoundary = block->focusBoundary(i);
        word.hasSpaceBefore = block->wordHasSpaceBefore(i);
        word.mayBreakBefore = word.hasSpaceBefore;
        word.insertedHyphenAfter = block->wordEndsWithInsertedHyphen(i);
        std::memcpy(text.data() + textSize, block->wordText(i), textLength);
        textSize += textLength;
        text[textSize++] = '\0';
        if (i == 0 && !line.startsParagraph && previousLineEndedWithInsertedHyphen) {
          // The source layout split one logical word across two captured
          // lines. Hide its layout-only hyphen when the fragments fit
          // together, but retain this boundary as a legal preview break.
          char* previousText = text.data() + words[previousLineLastWord].textOffset;
          const size_t previousLength = std::strlen(previousText);
          if (previousLength > 0 && previousText[previousLength - 1] == '-') previousText[previousLength - 1] = '\0';
          word.hasSpaceBefore = false;
          word.mayBreakBefore = true;
        }
        if (!word.hasSpaceBefore && i > 0) {
          const Word& previous = words[wordCount - 2];
          const int attachedX = previous.x + wordAdvance(renderer, fontId, previous, previous.focusBoundary != 0) +
                                renderer.getKerning(fontId, lastCodepoint(wordText(previous)),
                                                    firstCodepoint(wordText(word)), previous.style, characterSpacing);
          // Some blocks do not report every visible word gap. Recover one
          // only when the rendered source positions prove it was present.
          word.hasSpaceBefore = word.x > attachedX || block->guideDotXOffset(i - 1) > 0;
          word.mayBreakBefore = word.hasSpaceBefore;
        }
        if (!word.hasSpaceBefore && wordCount > 1 && !word.mayBreakBefore) {
          const Word& previous = words[wordCount - 2];
          word.mayBreakBefore =
              hasCjkBreakOpportunity(lastCodepoint(wordText(previous)), firstCodepoint(wordText(word)));
        }
      }
      previousLineEndedWithInsertedHyphen =
          block->wordCount() > 0 && block->wordEndsWithInsertedHyphen(block->wordCount() - 1);
      if (previousLineEndedWithInsertedHyphen) previousLineLastWord = static_cast<uint16_t>(wordCount - 1);
      previousElementWasLine = true;
    }
    return hasBaseline && wordCount > 0;
  }

  // Plain sample text needs no Page, TextBlock ownership, or source geometry.
  // Split once into the model's bounded storage; rendering reuses its metrics.
  bool captureParagraph(const char* paragraph) {
    clear();
    if (!paragraph || std::strlen(paragraph) >= text.size()) return false;
    std::strcpy(text.data(), paragraph);
    char* cursor = text.data();
    while (*cursor) {
      while (*cursor == ' ') ++cursor;
      if (!*cursor) break;
      if (wordCount == words.size()) {
        clear();
        return false;
      }
      Word& word = words[wordCount];
      word = {};
      word.textOffset = static_cast<uint16_t>(cursor - text.data());
      word.hasSpaceBefore = wordCount > 0;
      word.mayBreakBefore = wordCount > 0;
      ++wordCount;
      while (*cursor && *cursor != ' ') ++cursor;
      if (*cursor) *cursor++ = '\0';
    }
    if (wordCount == 0) return false;
    lines[0] = {};
    lines[0].wordCount = wordCount;
    lineCount = 1;
    hasBaseline = true;
    return true;
  }

  void renderText(const GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset,
                  const int contentWidth, const uint8_t lineHeightPercent, const uint8_t wordSpacing,
                  const uint8_t paragraphAlignment, const bool focusReadingEnabled, const bool guideReadingEnabled,
                  const bool foregroundBlack, const int bottom = std::numeric_limits<int>::max(),
                  const int8_t spacing = 0, const uint8_t ruledLines = 0) const {
    characterSpacing = spacing;
    if (!valid()) return;
    if constexpr (!KeepSourceBlocks) {
      if (renderer.isFontCacheScanning()) {
        // A newly selected SD font has no glyph metrics yet. Scan the whole
        // bounded sample so changed line breaks cannot expose unprepared glyphs.
        for (uint16_t index = 0; index < wordCount; ++index) {
          drawWord(renderer, fontId, xOffset, yOffset, words[index], focusReadingEnabled, foregroundBlack);
        }
        // Spaces are measured between words even though they are not drawn.
        renderer.drawText(fontId, xOffset, yOffset, " ", foregroundBlack, EpdFontFamily::REGULAR,
                          BidiUtils::BidiBaseDir::AUTO, characterSpacing);
        if (guideReadingEnabled) {
          renderer.drawText(fontId, xOffset, yOffset, GUIDE_DOT_UTF8, foregroundBlack, EpdFontFamily::REGULAR,
                            BidiUtils::BidiBaseDir::AUTO, characterSpacing);
        }
        return;
      }
    }
    const int fontHeight = renderer.getLineHeight(fontId);
    const int currentLineHeight = std::max(1, (fontHeight * lineHeightPercent + 50) / 100);
    const ReaderRuling ruling{ruledLines, xOffset, contentWidth, bottom, lineHeightPercent};
    const bool drawRules = ruling.enabled(renderer);
    std::optional<int> pendingRule;
    const auto flushRule = [&]() {
      if (pendingRule) ruling.draw(renderer, *pendingRule);
    };
    int y = firstLineY + yOffset;
    for (size_t paragraphStart = 0; paragraphStart < lineCount;) {
      size_t paragraphEnd = paragraphStart + 1;
      while (paragraphEnd < lineCount && !lines[paragraphEnd].startsParagraph) ++paragraphEnd;

      const Line& line = lines[paragraphStart];
      const uint16_t firstWord = line.firstWord;
      const uint16_t paragraphWordEnd = lines[paragraphEnd - 1].firstWord + lines[paragraphEnd - 1].wordCount;
      const int leftInset = std::max(0, static_cast<int>(line.style.leftInset()));
      const int rightInset = std::max(0, static_cast<int>(line.style.rightInset()));
      const int availableLeft = xOffset + leftInset;
      const int availableWidth = std::max(1, contentWidth - leftInset - rightInset);
      CssTextAlign alignment = paragraphAlignment < static_cast<uint8_t>(CssTextAlign::None)
                                   ? static_cast<CssTextAlign>(paragraphAlignment)
                                   : line.style.alignment;
      if (alignment == CssTextAlign::None) alignment = CssTextAlign::Justify;

      uint16_t wordIndex = firstWord;
      bool firstPreviewLine = true;
      prepareMetrics(renderer, fontId, firstWord, paragraphWordEnd, wordSpacing, focusReadingEnabled,
                     guideReadingEnabled);
      prepareLineBreaks(firstWord, paragraphWordEnd, availableWidth, previewFirstLineIndent(line, alignment));
      while (wordIndex < paragraphWordEnd) {
        if (!ruledLines && bottom != std::numeric_limits<int>::max() && y + renderer.getTextHeight(fontId) > bottom) {
          flushRule();
          return;
        }
        const int firstLineIndent = firstPreviewLine ? previewFirstLineIndent(line, alignment) : 0;
        const uint16_t lineEnd = nextBreak[wordIndex];
        RuledLineLayout row{currentLineHeight, 0};
        if (ruledLines) {
          RuledLineLayout::Ink ink;
          for (uint16_t i = wordIndex; i < lineEnd; ++i) {
            drawWord(renderer, fontId, 0, 0, words[i], focusReadingEnabled, foregroundBlack, &ink);
          }
          const auto& lastWord = words[lineEnd - 1];
          if (lineEnd < paragraphWordEnd && lastWord.insertedHyphenAfter) {
            const auto hyphen = renderer.getTextVerticalBounds(fontId, "-", lastWord.style);
            ink.include(hyphen.top, hyphen.bottom);
          }
          if (guideReadingEnabled && std::any_of(words.begin() + wordIndex + 1, words.begin() + lineEnd,
                                                 [](const Word& word) { return word.hasSpaceBefore; })) {
            const auto dot = renderer.getTextVerticalBounds(fontId, GUIDE_DOT_UTF8);
            ink.include(dot.top, dot.bottom);
          }
          row = RuledLineLayout::center(renderer, fontId, currentLineHeight, ink);
          if (y + row.height > bottom) {
            flushRule();
            return;
          }
        }
        renderReflowedLine(renderer, fontId, wordIndex, lineEnd, y + row.offset, availableLeft, availableWidth,
                           firstLineIndent, alignment, lineEnd == paragraphWordEnd, wordSpacing, focusReadingEnabled,
                           guideReadingEnabled, foregroundBlack);
        if (drawRules) {
          flushRule();
          pendingRule = y + row.height;
        }
        wordIndex = lineEnd;
        firstPreviewLine = false;
        y += row.height;
      }

      if (paragraphEnd < lineCount) {
        const int sourceGap = lines[paragraphEnd].y - lines[paragraphEnd - 1].y -
                              (lines[paragraphEnd - 1].style.lineHeight ? lines[paragraphEnd - 1].style.lineHeight
                                                                        : sourceLineHeightPixels);
        if (sourceGap > 0) y += sourceGap * currentLineHeight / std::max(1, static_cast<int>(sourceLineHeightPixels));
      }
      paragraphStart = paragraphEnd;
    }
    flushRule();
  }

  // Keep the source blocks alive so unchanged settings use the reader's exact
  // rendering, including justification, ruby, bidi, and focus run positions.
  // TextBlock::render writes pixels through the renderer, so this must remain
  // a mutable reference despite cppcheck not seeing that dependency.
  // cppcheck-suppress constParameterReference
  void renderSource(GfxRenderer& renderer, const int fontId, const bool foregroundBlack,
                    const ReaderRuling& ruling = {}) const {
    if (!valid()) return;
    if constexpr (KeepSourceBlocks) {
      for (size_t i = 0; i < lineCount; ++i) {
        const auto& line = lines[i];
        line.sourceBlock->render(renderer, fontId, sourceXOffset + line.x,
                                 sourceYOffset + line.y + line.style.ruledTextOffset, foregroundBlack);
      }
      if (ruling.enabled(renderer)) {
        for (size_t i = 0; i < lineCount; ++i) {
          const auto& line = lines[i];
          ruling.drawBlock(renderer, *line.sourceBlock, fontId, sourceYOffset + line.y + line.style.ruledTextOffset);
        }
      }
    }
  }

  int sourceLeft() const { return sourceXOffset; }

  bool valid() const { return hasBaseline && lineCount > 0 && wordCount > 0; }

 private:
  mutable int8_t characterSpacing = 0;
  struct Word {
    uint16_t textOffset = 0;
    int16_t x = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    uint8_t focusBoundary = 0;
    bool hasSpaceBefore = false;
    bool mayBreakBefore = false;
    bool insertedHyphenAfter : 1 = false;
    bool hidden : 1 = false;
    bool blackBackground : 1 = false;
    bool backgroundSpace : 1 = false;
  };

  struct Line {
    std::shared_ptr<TextBlock> sourceBlock;
    int16_t x = 0;
    int16_t y = 0;
    uint16_t firstWord = 0;
    uint16_t wordCount = 0;
    BlockStyle style{};
    bool startsParagraph = true;
    bool sourceIndentVisible = false;
  };

  std::array<char, TEXT_CAPACITY> text{};
  std::array<Word, WORD_CAPACITY> words{};
  std::array<Line, LINE_CAPACITY> lines{};
  mutable std::array<int32_t, WORD_CAPACITY> breakCost{};
  mutable std::array<uint16_t, WORD_CAPACITY> nextBreak{};
  mutable std::array<int16_t, WORD_CAPACITY> measuredAdvance{};
  mutable std::array<int16_t, WORD_CAPACITY> measuredGap{};
  mutable std::array<int16_t, WORD_CAPACITY> insertedHyphenExtra{};
  uint16_t textSize = 0;
  uint16_t wordCount = 0;
  uint16_t lineCount = 0;
  int sourceXOffset = 0;
  int sourceYOffset = 0;
  int16_t firstLineY = 0;
  int16_t sourceLineHeightPixels = 1;
  bool hasBaseline = false;

  static constexpr char GUIDE_DOT_UTF8[] = "\xc2\xb7";
  static constexpr uint32_t GUIDE_DOT_CODEPOINT = 0x00B7;

  const char* wordText(const Word& word) const { return text.data() + word.textOffset; }

  bool startsNewParagraph(const Line& previous, const Line& current, const TextBlock& currentBlock) const {
    if (current.y <= previous.y ||
        current.y - previous.y > (previous.style.lineHeight ? previous.style.lineHeight : sourceLineHeightPixels) + 1) {
      return true;
    }
    const bool naturallyAligned = current.style.alignment == CssTextAlign::None ||
                                  current.style.alignment == CssTextAlign::Left ||
                                  current.style.alignment == CssTextAlign::Justify;
    if (naturallyAligned && current.style.textIndentDefined && currentBlock.wordCount() > 0 &&
        currentBlock.wordXpos(0) != 0) {
      return true;
    }
    return previous.style.leftInset() != current.style.leftInset() ||
           previous.style.rightInset() != current.style.rightInset() ||
           previous.style.alignment != current.style.alignment ||
           previous.style.textIndent != current.style.textIndent ||
           previous.style.textIndentDefined != current.style.textIndentDefined ||
           previous.style.isRtl != current.style.isRtl;
  }

  int previewFirstLineIndent(const Line& line, const CssTextAlign alignment) const {
    const bool naturalAlignment = alignment == CssTextAlign::Justify || alignment == CssTextAlign::Left;
    if (!naturalAlignment || !line.startsParagraph || !line.sourceIndentVisible || !line.style.textIndentDefined)
      return 0;
    return std::max(0, static_cast<int>(line.style.textIndent));
  }

  static int16_t boundedMetric(const int value) {
    return static_cast<int16_t>(std::clamp(value, static_cast<int>(INT16_MIN), static_cast<int>(INT16_MAX)));
  }

  void prepareMetrics(const GfxRenderer& renderer, const int fontId, const uint16_t paragraphStart,
                      const uint16_t paragraphEnd, const uint8_t wordSpacing, const bool focusEnabled,
                      const bool guideReadingEnabled) const {
    for (uint16_t index = paragraphStart; index < paragraphEnd; ++index) {
      measuredAdvance[index] = boundedMetric(wordAdvance(renderer, fontId, words[index], focusEnabled));
      measuredGap[index] = index == paragraphStart
                               ? 0
                               : boundedMetric(wordGap(renderer, fontId, words[index - 1], words[index], wordSpacing,
                                                       guideReadingEnabled));
      insertedHyphenExtra[index] =
          words[index].insertedHyphenAfter
              ? boundedMetric(wordAdvance(renderer, fontId, words[index], focusEnabled, '-') +
                              renderer.getTextAdvanceX(fontId, "-", words[index].style, 0, characterSpacing) -
                              measuredAdvance[index])
              : 0;
    }
  }

  void prepareLineBreaks(const uint16_t paragraphStart, const uint16_t paragraphEnd, const int availableWidth,
                         const int firstLineIndent) const {
    constexpr int32_t MAX_COST = std::numeric_limits<int32_t>::max();
    for (int start = static_cast<int>(paragraphEnd) - 1; start >= static_cast<int>(paragraphStart); --start) {
      int lineWidth = 0;
      breakCost[static_cast<size_t>(start)] = MAX_COST;
      nextBreak[static_cast<size_t>(start)] = static_cast<uint16_t>(start + 1);
      const int widthLimit = std::max(1, availableWidth - (start == paragraphStart ? firstLineIndent : 0));
      for (uint16_t end = static_cast<uint16_t>(start); end < paragraphEnd; ++end) {
        if (end > start) {
          lineWidth += measuredGap[end];
        }
        lineWidth += measuredAdvance[end];
        if (lineWidth > widthLimit && end > start) break;
        if (end + 1 < paragraphEnd && !words[end + 1].mayBreakBefore) continue;

        const int candidateWidth = lineWidth + (end + 1 < paragraphEnd ? insertedHyphenExtra[end] : 0);
        if (candidateWidth > widthLimit && end > start) continue;

        int32_t cost = 0;
        if (end + 1 < paragraphEnd) {
          const int remaining = std::max(0, widthLimit - candidateWidth);
          const int64_t candidate = static_cast<int64_t>(remaining) * remaining + breakCost[end + 1];
          cost = candidate > MAX_COST ? MAX_COST : static_cast<int32_t>(candidate);
        }
        if (cost <= breakCost[static_cast<size_t>(start)]) {
          breakCost[static_cast<size_t>(start)] = cost;
          nextBreak[static_cast<size_t>(start)] = static_cast<uint16_t>(end + 1);
        }
      }
    }
  }

  void renderReflowedLine(const GfxRenderer& renderer, const int fontId, const uint16_t firstWord,
                          const uint16_t lineEnd, const int y, const int availableLeft, const int availableWidth,
                          const int firstLineIndent, const CssTextAlign alignment, const bool isLastLine,
                          const uint8_t wordSpacing, const bool focusEnabled, const bool guideReadingEnabled,
                          const bool foregroundBlack) const {
    int lineWidth = 0;
    int justifySlots = 0;
    for (uint16_t wordIndex = firstWord; wordIndex < lineEnd; ++wordIndex) {
      const Word& word = words[wordIndex];
      if (wordIndex > firstWord) {
        lineWidth += measuredGap[wordIndex];
        justifySlots += wordJustifySlots(word, guideReadingEnabled);
      }
      lineWidth += measuredAdvance[wordIndex];
    }
    if (!isLastLine && words[lineEnd - 1].insertedHyphenAfter) lineWidth += insertedHyphenExtra[lineEnd - 1];

    int targetLeft = availableLeft;
    if (alignment == CssTextAlign::Center) {
      targetLeft += std::max(0, (availableWidth - lineWidth) / 2);
    } else if (alignment == CssTextAlign::Right) {
      targetLeft += std::max(0, availableWidth - lineWidth);
    } else {
      targetLeft += firstLineIndent;
    }
    const bool justifyLine =
        alignment == CssTextAlign::Justify && !isLastLine && lineEnd > firstWord + 1 && lineWidth < availableWidth;
    const int justifyExtra =
        justifyLine && justifySlots > 0 ? (availableWidth - firstLineIndent - lineWidth) / justifySlots : 0;

    int wordX = targetLeft;
    for (uint16_t wordIndex = firstWord; wordIndex < lineEnd; ++wordIndex) {
      const Word& word = words[wordIndex];
      if (wordIndex > firstWord) {
        const Word& previous = words[wordIndex - 1];
        const int gap = measuredGap[wordIndex];
        const int fullGap = gap + wordJustifySlots(word, guideReadingEnabled) * justifyExtra;
        if (word.backgroundSpace && previous.blackBackground && word.blackBackground &&
            renderer.getRenderMode() == GfxRenderer::BW && !renderer.isFontCacheScanning())
          renderer.fillRect(wordX, y, fullGap, renderer.getLineHeight(fontId), foregroundBlack);
        if (guideReadingEnabled && word.hasSpaceBefore && !word.hidden) {
          const int extra = WordSpacing::extra(renderer.getSpaceAdvance(fontId, lastCodepoint(wordText(previous)),
                                                                        firstCodepoint(wordText(word)), previous.style),
                                               wordSpacing);
          const int firstGap =
              renderer.getSpaceAdvance(fontId, lastCodepoint(wordText(previous)), GUIDE_DOT_CODEPOINT, previous.style);
          renderer.drawText(fontId, wordX + firstGap + extra / 2, y, GUIDE_DOT_UTF8, foregroundBlack,
                            EpdFontFamily::REGULAR, BidiUtils::BidiBaseDir::AUTO, characterSpacing);
        }
        wordX += fullGap;
      }
      drawWord(renderer, fontId, wordX, y, word, focusEnabled, foregroundBlack);
      wordX += measuredAdvance[wordIndex];
      if (wordIndex + 1 == lineEnd && !isLastLine && word.insertedHyphenAfter && !word.hidden) {
        renderer.drawText(fontId,
                          wordX + wordAdvance(renderer, fontId, word, focusEnabled, '-') - measuredAdvance[wordIndex],
                          y, "-", foregroundBlack, word.style, BidiUtils::BidiBaseDir::AUTO, characterSpacing);
        wordX += insertedHyphenExtra[wordIndex];
      }
    }
  }

  static uint32_t firstCodepoint(const char* value) {
    const auto* cursor = reinterpret_cast<const unsigned char*>(value);
    while (true) {
      const uint32_t codepoint = utf8NextCodepoint(&cursor);
      if (codepoint != 0x00AD) return codepoint;
    }
  }

  static uint32_t lastCodepoint(const char* value) {
    const size_t length = std::strlen(value);
    if (length == 0) return 0;
    size_t offset = length - 1;
    while (offset > 0 && (static_cast<uint8_t>(value[offset]) & 0xC0) == 0x80) --offset;
    const auto* cursor = reinterpret_cast<const unsigned char*>(value + offset);
    return utf8NextCodepoint(&cursor);
  }

  static bool isFocusWordCharacter(const uint32_t codepoint) {
    if (codepoint < 128) {
      return ((codepoint | 0x20) >= 'a' && (codepoint | 0x20) <= 'z') || codepoint == '\'';
    }
    if (codepoint >= 0x2000 && codepoint <= 0x2BFF) return codepoint == 0x2018 || codepoint == 0x2019;
    if (codepoint >= 0x00A1 && codepoint <= 0x00BF)
      return codepoint == 0x00AA || codepoint == 0x00B5 || codepoint == 0x00BA;
    if (codepoint >= 0x2E00 && codepoint <= 0x2E7F) return false;
    return codepoint != 0x02D7 && codepoint != 0xFE63 && codepoint != 0xFF0D;
  }

  uint8_t resolvedFocusBoundary(const Word& word, const bool enabled) const {
    if (!enabled || (word.style & EpdFontFamily::BOLD) != 0) return 0;
    if (word.focusBoundary != 0) return word.focusBoundary;
    const char* value = wordText(word);
    const auto* cursor = reinterpret_cast<const unsigned char*>(value);
    size_t characters = 0;
    while (*cursor != '\0') {
      const auto* const start = cursor;
      if (!isFocusWordCharacter(utf8NextCodepoint(&cursor)) || cursor <= start) break;
      ++characters;
    }
    if (characters == 0) return 0;
    const size_t boldCharacters = std::clamp<size_t>((characters * 43) / 100, 1, 9);
    if (boldCharacters >= characters) return 0;
    cursor = reinterpret_cast<const unsigned char*>(value);
    for (size_t i = 0; i < boldCharacters; ++i) utf8NextCodepoint(&cursor);
    return static_cast<uint8_t>(std::min<size_t>(cursor - reinterpret_cast<const unsigned char*>(value), UINT8_MAX));
  }

  int wordAdvance(const GfxRenderer& renderer, const int fontId, const Word& word, const bool focusEnabled,
                  const uint32_t nextCodepoint = 0) const {
    const char* value = wordText(word);
    const uint8_t boundary = resolvedFocusBoundary(word, focusEnabled);
    if (boundary == 0 || boundary >= std::strlen(value))
      return renderer.getTextAdvanceX(fontId, value, word.style, nextCodepoint, characterSpacing);
    char prefix[40];
    const size_t length = std::min<size_t>({static_cast<size_t>(boundary), sizeof(prefix) - 1, std::strlen(value)});
    std::memcpy(prefix, value, length);
    prefix[length] = '\0';
    const auto boldStyle = static_cast<EpdFontFamily::Style>(word.style | EpdFontFamily::BOLD);
    return renderer.getTextAdvanceX(fontId, prefix, boldStyle, firstCodepoint(value + length), characterSpacing) +
           renderer.getTextAdvanceX(fontId, value + length, word.style, nextCodepoint, characterSpacing);
  }

  static bool isClosingPunctuation(const uint32_t codepoint) {
    switch (codepoint) {
      case '.':
      case ',':
      case ':':
      case ';':
      case '!':
      case '?':
      case ')':
      case ']':
      case '}':
      case 0x00BB:
      case 0x2019:
      case 0x201D:
      case 0x3001:
      case 0x3002:
      case 0x3009:
      case 0x300B:
      case 0x300D:
      case 0x300F:
      case 0x3011:
      case 0x3015:
      case 0x3017:
      case 0x3019:
      case 0x301B:
      case 0xFF01:
      case 0xFF09:
      case 0xFF0C:
      case 0xFF0E:
      case 0xFF1A:
      case 0xFF1B:
      case 0xFF1F:
      case 0xFF3D:
      case 0xFF5D:
        return true;
      default:
        return false;
    }
  }

  static bool isOpeningPunctuation(const uint32_t codepoint) {
    switch (codepoint) {
      case '(':
      case '[':
      case '{':
      case 0x00AB:
      case 0x2018:
      case 0x201C:
      case 0x3008:
      case 0x300A:
      case 0x300C:
      case 0x300E:
      case 0x3010:
      case 0x3014:
      case 0x3016:
      case 0x3018:
      case 0x301A:
      case 0xFF08:
      case 0xFF3B:
      case 0xFF5B:
        return true;
      default:
        return false;
    }
  }

  static bool hasCjkBreakOpportunity(const uint32_t leftCodepoint, const uint32_t rightCodepoint) {
    if (!utf8IsCjkBreakable(leftCodepoint) && !utf8IsCjkBreakable(rightCodepoint)) return false;
    return !isOpeningPunctuation(leftCodepoint) && !isClosingPunctuation(rightCodepoint) &&
           !utf8IsCombiningMark(rightCodepoint);
  }

  int wordJustifySlots(const Word& word, const bool guideReadingEnabled) const {
    if (!word.mayBreakBefore) return 0;
    const int slots = guideReadingEnabled && word.hasSpaceBefore ? 2 : 1;
    return isClosingPunctuation(firstCodepoint(wordText(word))) ? slots - 1 : slots;
  }

  int wordGap(const GfxRenderer& renderer, const int fontId, const Word& left, const Word& right,
              const uint8_t wordSpacing, const bool guideReadingEnabled) const {
    const uint32_t leftCodepoint = lastCodepoint(wordText(left));
    const uint32_t rightCodepoint = firstCodepoint(wordText(right));
    if (!right.hasSpaceBefore)
      return renderer.getKerning(fontId, leftCodepoint, rightCodepoint, left.style, characterSpacing);
    const int extra =
        WordSpacing::extra(renderer.getSpaceAdvance(fontId, leftCodepoint, rightCodepoint, left.style), wordSpacing);
    if (!guideReadingEnabled) {
      return renderer.getSpaceAdvance(fontId, leftCodepoint, rightCodepoint, left.style) + extra;
    }
    return renderer.getSpaceAdvance(fontId, leftCodepoint, GUIDE_DOT_CODEPOINT, left.style) +
           renderer.getTextAdvanceX(fontId, GUIDE_DOT_UTF8, EpdFontFamily::REGULAR, 0, characterSpacing) +
           renderer.getSpaceAdvance(fontId, GUIDE_DOT_CODEPOINT, rightCodepoint, EpdFontFamily::REGULAR) + extra;
  }

  void drawWord(const GfxRenderer& renderer, const int fontId, const int x, const int y, const Word& word,
                const bool focusEnabled, const bool foregroundBlack, RuledLineLayout::Ink* ink = nullptr) const {
    const auto draw = [&](int id, int left, int top, const char* text, bool black, EpdFontFamily::Style style,
                          BidiUtils::BidiBaseDir dir, int8_t spacing) {
      if (ink) {
        const auto bounds = renderer.getTextVerticalBounds(id, text, style, dir, spacing);
        ink->include(bounds.top, bounds.bottom);
        ink->observeText(text);
      } else
        renderer.drawText(id, left, top, text, black, style, dir, spacing);
    };
    if (word.blackBackground && !renderer.isFontCacheScanning()) {
      if (ink)
        ink->include(0, renderer.getLineHeight(fontId));
      else if (renderer.getRenderMode() == GfxRenderer::BW)
        renderer.fillRect(x, y, wordAdvance(renderer, fontId, word, focusEnabled), renderer.getLineHeight(fontId),
                          foregroundBlack);
      else
        return;
    }
    if (word.hidden && !renderer.isFontCacheScanning()) return;
    const bool textBlack = word.blackBackground ? !foregroundBlack : foregroundBlack;
    const char* value = wordText(word);
    const uint8_t boundary = resolvedFocusBoundary(word, focusEnabled);
    if (boundary == 0 || boundary >= std::strlen(value)) {
      draw(fontId, x, y, value, textBlack, word.style, BidiUtils::BidiBaseDir::AUTO, characterSpacing);
      return;
    }
    char prefix[40];
    const size_t length = std::min<size_t>({static_cast<size_t>(boundary), sizeof(prefix) - 1, std::strlen(value)});
    std::memcpy(prefix, value, length);
    prefix[length] = '\0';
    const auto boldStyle = static_cast<EpdFontFamily::Style>(word.style | EpdFontFamily::BOLD);
    draw(fontId, x, y, prefix, textBlack, boldStyle, BidiUtils::BidiBaseDir::AUTO, characterSpacing);
    draw(fontId,
         x + renderer.getTextAdvanceX(fontId, prefix, boldStyle, firstCodepoint(value + length), characterSpacing), y,
         value + length, textBlack, word.style, BidiUtils::BidiBaseDir::AUTO, characterSpacing);
  }

  void clear() {
    if constexpr (KeepSourceBlocks) {
      for (size_t i = 0; i < lineCount; ++i) lines[i].sourceBlock.reset();
    }
    textSize = 0;
    wordCount = 0;
    lineCount = 0;
    firstLineY = 0;
    sourceLineHeightPixels = 1;
    hasBaseline = false;
  }
};

// Deliberately fixed Latin sample: font/layout test content, not a UI label.
inline constexpr char READER_PREVIEW_PARAGRAPH[] =
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit. In malesuada congue tortor, vel egestas libero dignissim "
    "mollis. "
    "Duis ac massa id elit molestie cursus et vitae justo. Curabitur vel quam gravida, dapibus purus non, vulputate "
    "nisi. "
    "In sodales nisi vitae eleifend egestas. Nam pretium, ex eu iaculis porta, lorem lectus fermentum neque, id "
    "sagittis risus enim in est. "
    "Proin maximus fermentum ipsum nec feugiat. Cras pulvinar gravida leo fermentum rutrum.";
using SampleReaderPreviewModel = ReaderPreviewModel<sizeof(READER_PREVIEW_PARAGRAPH), 80, 1, false>;
static_assert(sizeof(SampleReaderPreviewModel) <= 3U * 1024U, "Sample preview exceeds its C3 budget");

using TouchReaderPreviewModel = ReaderPreviewModel<8U * 1024U, 256, 128, true>;
#if CROSSINK_APP_READER_SAMPLE_PREVIEW
using EpubReaderPreviewModel = SampleReaderPreviewModel;
#else
using EpubReaderPreviewModel = TouchReaderPreviewModel;
#endif
