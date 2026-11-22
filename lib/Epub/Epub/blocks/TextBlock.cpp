#include "TextBlock.h"

#include <BidiUtils.h>
#include <Epub/PageDeserializationBudget.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <PrintSerialization.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <limits>

#include "../../../ScalableFont/ScalableFontSizing.h"

namespace {
static_assert(ScalableContentMaxPointSize < TextBlock::BACKGROUND_SPACE);

constexpr uint16_t MAX_WORDS_PER_TEXT_BLOCK = 512;
constexpr size_t MIN_SERIALIZED_BLOCK_STYLE_BYTES = sizeof(CssTextAlign) + sizeof(bool) + 9 * sizeof(int16_t) +
                                                    3 * sizeof(bool) + sizeof(uint8_t) + 2 * sizeof(uint16_t);

bool tryReadRubyString(FsFile& file, std::string& ruby, PageDeserializationBudget::HeapBudget& heapBudget) {
  uint32_t length = 0;
  if (!serialization::tryReadPod(file, length) || length > ruby.max_size() ||
      length > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
    return false;
  }

  const int remaining = file.available();
  if (remaining < 0 || length > static_cast<uint32_t>(remaining)) {
    LOG_ERR("TXB", "Truncated cached ruby annotation (%u bytes, %d available)", length, remaining);
    return false;
  }

  if (length > ruby.capacity()) {
    const size_t allocationBytes = static_cast<size_t>(length) + 1;
    if (heapBudget.needsRefresh(allocationBytes, sizeof(char))) {
      const auto heap = MemoryBudget::snapshot();
      heapBudget.refresh(heap.freeHeap, heap.maxAllocHeap);
    }
    if (!heapBudget.canReserve(allocationBytes, sizeof(char))) {
      LOG_ERR("TXB", "Low heap for cached ruby annotation (%u bytes, free=%u max=%u)", length,
              static_cast<unsigned>(heapBudget.availableAfterReserves()),
              static_cast<unsigned>(heapBudget.largestFreeBlock()));
      return false;
    }
    heapBudget.recordReserve(allocationBytes, sizeof(char));
  }

  ruby.resize(length);
  return length == 0 || file.read(&ruby[0], static_cast<int>(length)) == static_cast<int>(length);
}

uint16_t measureBackgroundWidth(const GfxRenderer& renderer, const int fontId, const char* word,
                                const EpdFontFamily::Style style, const int8_t characterSpacing, const char* display) {
  if (word[0] == ' ' && word[1] == '\0') {
    return renderer.getSpaceWidth(fontId, style);
  }
  return static_cast<uint16_t>(
      std::max(0, renderer.getTextAdvanceX(fontId, word, style, 0, characterSpacing, display)));
}

bool hasSyntheticIndentPrefix(const char* word, const uint16_t len) {
  return len >= 3 && static_cast<uint8_t>(word[0]) == 0xE2 && static_cast<uint8_t>(word[1]) == 0x80 &&
         static_cast<uint8_t>(word[2]) == 0x83;
}

}  // namespace

size_t TextBlock::arenaSize(const uint16_t wordCount, const bool hasFocus, const bool hasGuideDots,
                            const bool hasWordFlags, const bool hasWordSpaces, const uint16_t textBytes,
                            const bool hasWordSizes, const bool hasSourceRanges) {
  // 16-bit arrays first so direct loads stay aligned on RISC-V, then byte arrays, then text.
  size_t size = static_cast<size_t>(wordCount) * (sizeof(uint16_t) + sizeof(int16_t) + sizeof(uint8_t));
  if (hasFocus) {
    size += static_cast<size_t>(wordCount) * (sizeof(uint16_t) + sizeof(uint8_t));
  }
  if (hasGuideDots) {
    size += static_cast<size_t>(wordCount) * sizeof(uint16_t);
  }
  if (hasWordFlags) {
    size += static_cast<size_t>(wordCount) * sizeof(uint8_t);
  }
  if (hasWordSpaces) {
    size += wordSpacesBytes(wordCount);
  }
  return size + textBytes + (hasWordSizes ? wordCount : 0) +
         (hasSourceRanges ? static_cast<size_t>(wordCount) * sizeof(SourceRange) : 0);
}

void TextBlock::bindArenaPointers() {
  uint8_t* base = arena.get();
  const size_t wc = numWords;
  if (sourceRangesPresent) {
    sourceRangesArr = reinterpret_cast<const SourceRange*>(base);
    base += wc * sizeof(SourceRange);
  }
  textOffArr = reinterpret_cast<const uint16_t*>(base);
  xposArr = reinterpret_cast<const int16_t*>(base + wc * 2);
  size_t off = wc * 4;
  if (focusPresent) {
    focusRunOffsetArr = reinterpret_cast<const uint16_t*>(base + off);
    off += wc * 2;
  }
  if (guideDotsPresent) {
    guideDotXOffsetArr = reinterpret_cast<const uint16_t*>(base + off);
    off += wc * 2;
  }
  stylesArr = base + off;
  off += wc;
  if (focusPresent) {
    focusBoundaryArr = base + off;
    off += wc;
  }
  if (wordFlagsPresent) {
    wordFlagsArr = base + off;
    off += wc;
  }
  if (wordSpacesPresent) {
    wordSpacesArr = base + off;
    off += wordSpacesBytes(numWords);
  }
  if (wordSizesPresent) {
    wordSizesArr = base + off;
    off += wc;
  }
  textArr = reinterpret_cast<const char*>(base + off);
}

TextBlock::TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>& wordStyles, const std::vector<uint8_t>& focusBoundary,
                     const std::vector<uint16_t>& focusRunOffset, const std::vector<uint16_t>& guideDotXOffset,
                     const std::vector<uint8_t>& wordFlags, const std::vector<bool>& wordHasSpaceBefore,
                     const BlockStyle& blockStyle, std::vector<std::string> rubyTexts,
                     const std::vector<uint8_t>& wordSizes, const char* initialLetter, const int8_t characterSpacing,
                     const std::vector<uint32_t>& fontFamilies, const std::vector<SourceRange>& sourceRanges,
                     const std::vector<std::string>& display)
    : blockStyle(blockStyle), characterSpacing(characterSpacing), rubyTexts(std::move(rubyTexts)) {
  // A ruby-less line needs no per-word ruby vector. ParsedText passes one for
  // every extracted line once a book contains any ruby, so free all-empty
  // vectors before they stay resident with the page.
  if (!hasRuby()) {
    this->rubyTexts = std::vector<std::string>{};
  }

  initialLetterBytes = static_cast<uint8_t>(std::min<size_t>(12, strlen(initialLetter)));
  if (!fontFamilies.empty()) {
    // Four bytes per token, only on lines that use a publisher font.
    wordFamilies = makeUniqueNoThrow<uint32_t[]>(words.size());
    if (!wordFamilies || fontFamilies.size() != words.size()) {
      LOG_ERR("TXB", "Cannot allocate publisher font references");
      isValid = false;
      return;
    }
    std::copy(fontFamilies.begin(), fontFamilies.end(), wordFamilies.get());
  }
  sourceRangesPresent = !sourceRanges.empty();
  if (sourceRangesPresent && sourceRanges.size() != words.size()) {
    LOG_ERR("TXB", "Invalid source coordinate count");
    isValid = false;
    return;
  }
  for (const auto& range : sourceRanges) {
    if (!range.wellFormed()) {
      LOG_ERR("TXB", "Invalid source coordinates");
      isValid = false;
      return;
    }
  }
  wordSizesPresent = !wordSizes.empty();
  if (wordSizesPresent && wordSizes.size() != words.size()) {
    LOG_ERR("TXB", "Invalid word font sizes");
    isValid = false;
    return;
  }
  const bool hasFocus = !focusBoundary.empty();
  const bool hasGuideDots = !guideDotXOffset.empty();
  const bool hasWordFlags = !wordFlags.empty();
  const bool hasWordSpaces = !wordHasSpaceBefore.empty();
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() || words.size() > MAX_WORDS_PER_TEXT_BLOCK ||
      (hasFocus && (words.size() != focusBoundary.size() || words.size() != focusRunOffset.size())) ||
      (!hasFocus && !focusRunOffset.empty()) || (hasGuideDots && words.size() != guideDotXOffset.size()) ||
      (hasWordFlags && words.size() != wordFlags.size()) ||
      (hasWordSpaces && words.size() != wordHasSpaceBefore.size()) ||
      (!this->rubyTexts.empty() && words.size() != this->rubyTexts.size())) {
    LOG_ERR("TXB",
            "Construction failed: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, runOffset=%u, "
            "dotX=%u, flags=%u, spaces=%u)",
            static_cast<uint32_t>(words.size()), static_cast<uint32_t>(wordXpos.size()),
            static_cast<uint32_t>(wordStyles.size()), static_cast<uint32_t>(focusBoundary.size()),
            static_cast<uint32_t>(focusRunOffset.size()), static_cast<uint32_t>(guideDotXOffset.size()),
            static_cast<uint32_t>(wordFlags.size()), static_cast<uint32_t>(wordHasSpaceBefore.size()));
    isValid = false;
    return;
  }

  numWords = static_cast<uint16_t>(words.size());
  focusPresent = hasFocus;
  guideDotsPresent = hasGuideDots;
  wordFlagsPresent = hasWordFlags;
  wordSpacesPresent = hasWordSpaces;
  if (numWords == 0) {
    return;
  }

  if (!display.empty() && display.size() != words.size()) {
    LOG_ERR("TXB", "Display form count differs from word count");
    isValid = false;
    return;
  }
  size_t totalText = initialLetterBytes;
  for (const auto& form : display) {
    if (!form.empty()) {
      displayPresent = true;
      totalText += form.size() + 1;
    }
  }
  for (const auto& word : words) {
    totalText += word.size() + 1;
  }
  if (totalText > UINT16_MAX) {
    LOG_ERR("TXB", "Construction failed: text size %u exceeds arena limit", static_cast<uint32_t>(totalText));
    numWords = 0;
    focusPresent = false;
    guideDotsPresent = false;
    wordFlagsPresent = false;
    wordSpacesPresent = false;
    isValid = false;
    return;
  }
  textBytes = static_cast<uint16_t>(totalText);

  const size_t size = arenaSize(numWords, focusPresent, guideDotsPresent, wordFlagsPresent, wordSpacesPresent,
                                textBytes, wordSizesPresent, sourceRangesPresent);
  arena = makeUniqueNoThrow<uint8_t[]>(size);
  if (!arena) {
    LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
    numWords = 0;
    textBytes = 0;
    focusPresent = false;
    guideDotsPresent = false;
    wordFlagsPresent = false;
    wordSpacesPresent = false;
    isValid = false;
    return;
  }
  bindArenaPointers();
  if (sourceRangesPresent) {
    // Runtime-sized metadata shares the existing arena: eight bytes per word,
    // without another allocation for every rendered line.
    memcpy(const_cast<SourceRange*>(sourceRangesArr), sourceRanges.data(), numWords * sizeof(SourceRange));
  }
  if (wordSizesPresent) memcpy(const_cast<uint8_t*>(wordSizesArr), wordSizes.data(), numWords);

  auto* textOff = const_cast<uint16_t*>(textOffArr);
  auto* xpos = const_cast<int16_t*>(xposArr);
  auto* styles = const_cast<uint8_t*>(stylesArr);
  auto* text = const_cast<char*>(textArr);
  uint16_t off = 0;
  for (uint16_t i = 0; i < numWords; i++) {
    textOff[i] = off;
    xpos[i] = wordXpos[i];
    styles[i] = static_cast<uint8_t>(wordStyles[i]);
    if (i == 0 && initialLetterBytes) {
      memcpy(text + off, initialLetter, initialLetterBytes);
      off += initialLetterBytes;
    }
    memcpy(text + off, words[i].data(), words[i].size());
    off += static_cast<uint16_t>(words[i].size());
    text[off++] = '\0';
    if (displayPresent && !display[i].empty()) {
      const auto bytes = display[i].size() + 1;
      memcpy(text + off, display[i].c_str(), bytes);
      off += bytes;
    }
  }
  if (focusPresent) {
    auto* runOffset = const_cast<uint16_t*>(focusRunOffsetArr);
    auto* boundary = const_cast<uint8_t*>(focusBoundaryArr);
    for (uint16_t i = 0; i < numWords; i++) {
      runOffset[i] = focusRunOffset[i];
      boundary[i] = focusBoundary[i];
    }
  }
  if (guideDotsPresent) {
    auto* dotX = const_cast<uint16_t*>(guideDotXOffsetArr);
    for (uint16_t i = 0; i < numWords; i++) {
      dotX[i] = guideDotXOffset[i];
    }
  }
  if (wordFlagsPresent) {
    auto* flags = const_cast<uint8_t*>(wordFlagsArr);
    for (uint16_t i = 0; i < numWords; i++) {
      flags[i] = wordFlags[i];
    }
  }
  if (wordSpacesPresent) {
    auto* spaces = const_cast<uint8_t*>(wordSpacesArr);
    std::memset(spaces, 0, wordSpacesBytes(numWords));
    for (uint16_t i = 0; i < numWords; i++) {
      if (wordHasSpaceBefore[i]) {
        spaces[i / 8U] |= static_cast<uint8_t>(1U << (i % 8U));
      }
    }
  }
}

bool TextBlock::hasRuby() const {
  for (const auto& ruby : rubyTexts) {
    if (!ruby.empty()) return true;
  }
  return false;
}

int TextBlock::wordFontId(const GfxRenderer& renderer, int fontId, uint16_t i) const {
  return renderer.resolveBookFont(resolvedFontId(renderer, fontId), wordFamilies ? wordFamilies[i] : 0,
                                  wordFontSize(i));
}
int TextBlock::maxAscender(const GfxRenderer& renderer, int fontId) const {
  int height = renderer.getFontAscenderSize(resolvedFontId(renderer, fontId));
  if (wordSizesPresent || wordFamilies)
    for (uint16_t i = 0; i < numWords; ++i)
      height = std::max(height, renderer.getFontAscenderSize(wordFontId(renderer, fontId, i)));
  return height;
}
int TextBlock::maxLineHeight(const GfxRenderer& renderer, int fontId) const {
  int height = renderer.getLineHeight(resolvedFontId(renderer, fontId));
  if (wordSizesPresent || wordFamilies)
    for (uint16_t i = 0; i < numWords; ++i)
      height = std::max(height, renderer.getLineHeight(wordFontId(renderer, fontId, i)));
  return height;
}

int TextBlock::wordYOffset(const GfxRenderer& renderer, int fontId, uint16_t i) const {
  const int ascender = renderer.getFontAscenderSize(wordFontId(renderer, fontId, i));
  const int baseline = maxAscender(renderer, fontId);
  return baselineOffset(baseline, ascender, wordStyle(i));
}

int TextBlock::baselineOffset(int baseline, int ascender, EpdFontFamily::Style style) const {
  int offset = baseline - ascender + getRubyShift(baseline);
  if (style & EpdFontFamily::SUP)
    offset -= ascender * 2 / 5;
  else if (style & EpdFontFamily::SUB)
    offset += ascender / 4;
  return offset;
}

int TextBlock::resolvedFontId(const GfxRenderer& renderer, const int fontId) const {
  return renderer.getFontIdForSize(fontId, blockStyle.fontSize);
}

void TextBlock::render(const GfxRenderer& renderer, int fontId, const int x, const int y,
                       const bool foregroundBlack) const {
  renderOrMeasure(renderer, fontId, x, y, foregroundBlack, nullptr);
}

RuledLineLayout TextBlock::ruledLayout(const GfxRenderer& renderer, int fontId, int requested) const {
  RuledLineLayout::Ink ink;
  renderOrMeasure(renderer, fontId, 0, 0, true, &ink);
  const int baseFont = resolvedFontId(renderer, fontId);
  int referenceFont = baseFont;
  if (numWords && (wordSizesPresent || wordFamilies)) {
    // An entire row can use an inline publisher face. Mixed runs share the
    // paragraph baseline and use its regular face as their reference.
    referenceFont = wordFontId(renderer, baseFont, 0);
    for (uint16_t i = 1; i < numWords && referenceFont != baseFont; ++i) {
      if (wordFontId(renderer, baseFont, i) != referenceFont) referenceFont = baseFont;
    }
  }
  const int fontTop = baselineOffset(maxAscender(renderer, baseFont), renderer.getFontAscenderSize(referenceFont),
                                     EpdFontFamily::REGULAR);
  return RuledLineLayout::center(renderer, referenceFont, requested, ink, fontTop);
}

void TextBlock::renderOrMeasure(const GfxRenderer& renderer, int fontId, const int x, const int y,
                                const bool foregroundBlack, RuledLineLayout::Ink* ink) const {
  if (!isValid) {
    LOG_ERR("TXB", "Render skipped: invalid block");
    return;
  }

  const auto drawText = [&](int id, int left, int top, const char* text, bool black, EpdFontFamily::Style style,
                            BidiUtils::BidiBaseDir dir, int8_t spacing, const char* display = nullptr) {
    if (ink) {
      const auto bounds = renderer.getTextVerticalBounds(id, text, style, dir, spacing, display);
      ink->include(top + bounds.top, top + bounds.bottom);
      ink->observeText(text);
    } else {
      renderer.drawText(id, left, top, text, black, style, dir, spacing, display);
    }
  };
  const bool scanning = !ink && renderer.isFontCacheScanning();
  const auto generation = scanning || ink ? 0 : renderer.textCullGeneration();
  const bool cached = generation && stripBoundsGeneration == generation && stripBoundsX == x && stripBoundsY == y &&
                      stripBoundsFont == fontId;
  const int stripTop = renderer.getWriteOriginY();
  const int stripEnd = stripTop + renderer.getWriteRows();
  const auto groupIntersects = [&](uint16_t group) {
    return stripBounds[group][0] < stripBounds[group][1] && stripBounds[group][1] > stripTop &&
           stripBounds[group][0] < stripEnd;
  };
  if (cached) {
    bool visible = false;
    for (uint16_t group = 0; group < STRIP_GROUPS; ++group) visible |= groupIntersects(group);
    if (!visible) return;
  }
  const bool collectBounds = generation && !cached;
  if (collectBounds) {
    stripBoundsX = x;
    stripBoundsY = y;
    stripBoundsFont = fontId;
  }
  fontId = resolvedFontId(renderer, fontId);
  const int baseFont = fontId;
  const int baseline = maxAscender(renderer, baseFont);
  const uint16_t groups = generation ? STRIP_GROUPS : 1;
  const uint16_t groupSize = (numWords + groups - 1) / groups;
  for (uint16_t group = 0; group < groups; ++group) {
    if (cached && !groupIntersects(group)) continue;
    int bounds[2];
    if (collectBounds) renderer.beginTextBounds(bounds);
    const uint16_t end = std::min<int>(numWords, (group + 1) * groupSize);
    for (uint16_t i = group * groupSize; i < end; i++) {
      fontId = wordFontId(renderer, baseFont, i);
      const int ascender = renderer.getFontAscenderSize(fontId);
      const char* word = visibleWordText(i);
      const uint16_t wordLen = visibleWordTextLen(i);
      const int wordX = wordXpos(i) + x;
      const EpdFontFamily::Style currentStyle = wordStyle(i);
      const uint8_t boundary = focusBoundary(i);
      const auto baseDir =
          static_cast<BidiUtils::BidiBaseDir>(BidiUtils::detectParagraphLevel(word, blockStyle.isRtl ? 1 : 0));

      const auto boldStyle = static_cast<EpdFontFamily::Style>(currentStyle | EpdFontFamily::BOLD);
      char boldBuf[40];
      const size_t boldLen = std::min<size_t>({boundary, wordLen, sizeof(boldBuf) - 1});
      if (boldLen) {
        memcpy(boldBuf, word, boldLen);
        boldBuf[boldLen] = '\0';
      }
      const bool blackBackground = (wordFlags(i) & WORD_FLAG_BACKGROUND_BLACK) != 0;
      const int wordY = y + baselineOffset(baseline, ascender, currentStyle);
      if (!scanning && blackBackground && (ink || renderer.getRenderMode() == GfxRenderer::BW)) {
        const int start = wordX;
        const bool rtl = baseDir == BidiUtils::BidiBaseDir::RTL;
        const int advance =
            boldLen ? focusRunOffset(i) + renderer.getTextAdvanceX(fontId, rtl ? boldBuf : word + boldLen,
                                                                   rtl ? boldStyle : currentStyle, 0, characterSpacing)
                    : measureBackgroundWidth(renderer, fontId, word, currentStyle, characterSpacing, displayForm(i));
        int end = start + advance;
        // Cover a collapsed or justified space only when the source space itself was painted.
        if (i + 1 < numWords && wordBackgroundSpace(i + 1) && (wordFlags(i + 1) & WORD_FLAG_BACKGROUND_BLACK)) {
          const int nextX = x + wordXpos(i + 1);
          if (nextX >= start) end = std::max(end, nextX);
        }
        if (i && wordBackgroundSpace(i) && (wordFlags(i - 1) & WORD_FLAG_BACKGROUND_BLACK)) {
          const int prevX = x + wordXpos(i - 1);
          if (prevX > start) end = std::max(end, prevX);
        }
        const int backgroundWidth = end - start;
        if (backgroundWidth > 0) {
          const int height = renderer.getLineHeight(fontId);
          if (ink)
            ink->include(wordY, wordY + height);
          else
            renderer.fillRect(start, wordY, backgroundWidth, height, foregroundBlack);
        }
      }

      if (wordHidden(i) && !scanning) continue;
      // A monochrome publisher background needs contrasting, solid glyphs.
      const bool paintGlyphs = !blackBackground || ink || scanning || renderer.getRenderMode() == GfxRenderer::BW;
      const bool textBlack = blackBackground ? !foregroundBlack : foregroundBlack;

      if (paintGlyphs && boldLen > 0) {
        const int secondRunX = wordX + focusRunOffset(i);
        if (baseDir == BidiUtils::BidiBaseDir::RTL) {
          drawText(fontId, wordX, wordY, word + boldLen, textBlack, currentStyle, baseDir, characterSpacing);
          drawText(fontId, secondRunX, wordY, boldBuf, textBlack, boldStyle, baseDir, characterSpacing);
        } else {
          drawText(fontId, wordX, wordY, boldBuf, textBlack, boldStyle, baseDir, characterSpacing);
          drawText(fontId, secondRunX, wordY, word + boldLen, textBlack, currentStyle, baseDir, characterSpacing);
        }
      } else if (paintGlyphs) {
        drawText(fontId, wordX, wordY, word, textBlack, currentStyle, baseDir, characterSpacing, displayForm(i));
      }

      if (i < rubyTexts.size() && !rubyTexts[i].empty() && (currentStyle & EpdFontFamily::RUBY_CONTINUE) == 0) {
        int rubyX = wordX;
        if (!scanning && !ink) {
          uint16_t groupWords = 1;
          while (i + groupWords < numWords && (wordStyle(i + groupWords) & EpdFontFamily::RUBY_CONTINUE) != 0) {
            ++groupWords;
          }
          int groupWidth = 0;
          for (uint16_t j = 0; j < groupWords; ++j) {
            groupWidth += renderer.getTextAdvanceX(wordFontId(renderer, baseFont, i + j), wordText(i + j),
                                                   wordStyle(i + j), 0, characterSpacing, displayForm(i + j));
          }
          const int rubyWidth =
              renderer.getTextAdvanceX(fontId, rubyTexts[i].c_str(), EpdFontFamily::SUP, 0, characterSpacing);
          // ParsedText reserves any edge overhang in the line layout, so the ruby
          // can remain centered over its base text without screen-edge clamping.
          rubyX += (groupWidth - rubyWidth) / 2;
        }
        // Scan coordinates are irrelevant. Still collect the ruby run with its
        // exact font/style without measuring (which can rasterize monochrome TTFs).
        drawText(fontId, rubyX, wordY - ascender, rubyTexts[i].c_str(), foregroundBlack, EpdFontFamily::SUP, baseDir,
                 characterSpacing);
      }

      if (!paintGlyphs) continue;
      const uint16_t dotOffset = guideDotXOffset(i);
      if (dotOffset > 0) {
        drawText(fontId, wordX + dotOffset, wordY, "\xc2\xb7", textBlack, EpdFontFamily::REGULAR, baseDir,
                 characterSpacing);
      }

      const auto decoration = [&](EpdFontFamily::Style flag, int lineY) {
        if (ink) {
          ink->include(lineY, lineY + 3);
          return;
        }

        int startX = wordX;
        const bool hasIndent = hasSyntheticIndentPrefix(word, wordLen);
        if (hasIndent) startX += renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", currentStyle, 0, characterSpacing);
        const char* display = displayForm(i);
        int width =
            display && !hasIndent
                ? renderer.getTextAdvanceX(fontId, word, currentStyle, 0, characterSpacing, display)
                : renderer.getTextWidth(fontId, hasIndent ? word + 3 : word, currentStyle, baseDir, characterSpacing);
        if (!display && characterSpacing == 0 && (currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0)
          width = (width + 1) / 2;

        int endX = startX + width;
        if (i + 1 < numWords) {
          const auto nextStyle = wordStyle(i + 1);
          const bool sameBaseline = (nextStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) ==
                                    (currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB));
          if ((nextStyle & flag) != 0 && sameBaseline) {
            const int nextX = wordXpos(i + 1) + x;
            endX = std::max(endX, nextX);
            startX = std::min(startX, nextX);
          }
        }
        renderer.drawLine(startX, lineY, endX, lineY, 3, textBlack);
      };

      if (!scanning && (currentStyle & EpdFontFamily::UNDERLINE) != 0)
        decoration(EpdFontFamily::UNDERLINE, wordY + ascender + 2);
      if (!scanning && (currentStyle & EpdFontFamily::STRIKETHROUGH) != 0) {
        int32_t unusedAdvance = 0;
        int height = 0;
        const bool smallCaps = (currentStyle & EpdFontFamily::SMALL_CAPS) != 0;
        if (!renderer.getCodepointMetrics(fontId, smallCaps ? 'X' : 'x', currentStyle, unusedAdvance, height) ||
            height <= 0)
          height = ascender * 2 / 3;
        if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0)
          height = (height + 1) / 2;
        else if (smallCaps)
          height = (height * 3 + 3) / 4;
        const int strikeY = wordY + ascender - std::max(1, height / 2);

        decoration(EpdFontFamily::STRIKETHROUGH, strikeY);
      }
    }
    if (collectBounds) {
      renderer.endTextBounds();
      stripBounds[group][0] = std::clamp<int>(bounds[0], 0, renderer.getDisplayHeight());
      stripBounds[group][1] = std::clamp<int>(bounds[1] + 1, 0, renderer.getDisplayHeight());
    }
  }
  if (collectBounds) stripBoundsGeneration = generation;
}

bool TextBlock::serialize(Print& file) const {
  if (!isValid) {
    LOG_ERR("TXB", "Serialization failed: invalid block");
    return false;
  }

  if (!serialization::tryWritePod(file, numWords) ||
      !serialization::tryWritePod(file, static_cast<uint8_t>(focusPresent ? 1 : 0)) ||
      !serialization::tryWritePod(file, static_cast<uint8_t>(guideDotsPresent ? 1 : 0)) ||
      !serialization::tryWritePod(file, static_cast<uint8_t>(wordFlagsPresent ? 1 : 0)) ||
      !serialization::tryWritePod(file, static_cast<uint8_t>(wordSpacesPresent ? 1 : 0)) ||
      !serialization::tryWritePod(file, static_cast<uint8_t>(wordSizesPresent)) ||
      !serialization::tryWritePod(file, static_cast<uint8_t>(sourceRangesPresent | (displayPresent << 1))) ||
      !serialization::tryWritePod(file, characterSpacing) || !serialization::tryWritePod(file, initialLetterBytes) ||
      !serialization::tryWritePod(file, textBytes)) {
    LOG_ERR("TXB", "Serialization failed: could not write block header");
    return false;
  }
  if (numWords > 0) {
    const size_t size = arenaSize(numWords, focusPresent, guideDotsPresent, wordFlagsPresent, wordSpacesPresent,
                                  textBytes, wordSizesPresent, sourceRangesPresent);
    if (file.write(arena.get(), size) != size) {
      LOG_ERR("TXB", "Serialization failed: arena write (%u bytes)", static_cast<uint32_t>(size));
      return false;
    }
  }

  const uint8_t hasFamilies = wordFamilies ? 1 : 0;
  if (!serialization::tryWritePod(file, hasFamilies)) return false;
  if (hasFamilies && file.write(reinterpret_cast<const uint8_t*>(wordFamilies.get()), numWords * sizeof(uint32_t)) !=
                         numWords * sizeof(uint32_t))
    return false;

  uint16_t rubyCount = 0;
  for (uint16_t i = 0; i < numWords && i < rubyTexts.size(); ++i) {
    if (!rubyTexts[i].empty()) ++rubyCount;
  }
  if (!serialization::tryWritePod(file, rubyCount)) return false;
  for (uint16_t i = 0; i < numWords && i < rubyTexts.size(); ++i) {
    if (rubyTexts[i].empty()) continue;
    if (!serialization::tryWritePod(file, i) || !serialization::tryWriteString(file, rubyTexts[i])) return false;
  }

  return serialization::tryWritePod(file, blockStyle.alignment) &&
         serialization::tryWritePod(file, blockStyle.textAlignDefined) &&
         serialization::tryWritePod(file, blockStyle.marginTop) &&
         serialization::tryWritePod(file, blockStyle.marginBottom) &&
         serialization::tryWritePod(file, blockStyle.marginLeft) &&
         serialization::tryWritePod(file, blockStyle.marginRight) &&
         serialization::tryWritePod(file, blockStyle.paddingTop) &&
         serialization::tryWritePod(file, blockStyle.paddingBottom) &&
         serialization::tryWritePod(file, blockStyle.paddingLeft) &&
         serialization::tryWritePod(file, blockStyle.paddingRight) &&
         serialization::tryWritePod(file, blockStyle.textIndent) &&
         serialization::tryWritePod(file, blockStyle.textIndentDefined) &&
         serialization::tryWritePod(file, blockStyle.isRtl) &&
         serialization::tryWritePod(file, blockStyle.directionDefined) &&
         serialization::tryWritePod(file, blockStyle.fontSize) &&
         serialization::tryWritePod(file, blockStyle.lineHeight) &&
         serialization::tryWritePod(file, blockStyle.ruledTextOffset);
}

std::unique_ptr<TextBlock> TextBlock::deserialize(HalFile& file) {
  uint16_t wc = 0;
  uint8_t hasFocus = 0;
  uint8_t hasGuideDots = 0;
  uint8_t hasWordFlags = 0;
  uint8_t hasWordSpaces = 0;
  uint8_t hasWordSizes = 0;
  uint8_t hasSourceRanges = 0;
  uint8_t initialLetterBytes = 0;
  int8_t characterSpacing = 0;
  uint16_t textBytes = 0;
  if (!serialization::tryReadPod(file, wc) || !serialization::tryReadPod(file, hasFocus) ||
      !serialization::tryReadPod(file, hasGuideDots) || !serialization::tryReadPod(file, hasWordFlags) ||
      !serialization::tryReadPod(file, hasWordSpaces) || !serialization::tryReadPod(file, hasWordSizes) ||
      !serialization::tryReadPod(file, hasSourceRanges) || !serialization::tryReadPod(file, characterSpacing) ||
      !serialization::tryReadPod(file, initialLetterBytes) || !serialization::tryReadPod(file, textBytes)) {
    LOG_ERR("TXB", "Deserialization failed: could not read block header");
    return nullptr;
  }

  if (wc > MAX_WORDS_PER_TEXT_BLOCK) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }
  if (hasFocus > 1 || hasGuideDots > 1 || hasWordFlags > 1 || hasWordSpaces > 1 || hasWordSizes > 1 ||
      hasSourceRanges > 3) {
    LOG_ERR("TXB", "Deserialization failed: invalid metadata flags");
    return nullptr;
  }
  if (characterSpacing < -5 || characterSpacing > 5 || initialLetterBytes > 12 || (wc == 0 && initialLetterBytes) ||
      (wc == 0 && textBytes != 0) || (wc > 0 && textBytes < wc)) {
    LOG_ERR("TXB", "Deserialization failed: bad text size %u for %u words", textBytes, wc);
    return nullptr;
  }

  std::unique_ptr<TextBlock> block(new (std::nothrow) TextBlock());
  if (!block) {
    LOG_ERR("TXB", "Deserialization failed: could not allocate TextBlock");
    return nullptr;
  }
  block->numWords = wc;
  block->textBytes = textBytes;
  block->focusPresent = hasFocus != 0;
  block->guideDotsPresent = hasGuideDots != 0;
  block->wordFlagsPresent = hasWordFlags != 0;
  block->wordSpacesPresent = hasWordSpaces != 0;
  block->wordSizesPresent = hasWordSizes != 0;
  block->displayPresent = (hasSourceRanges & 2) != 0;
  block->sourceRangesPresent = (hasSourceRanges & 1) != 0;
  block->initialLetterBytes = initialLetterBytes;
  block->characterSpacing = characterSpacing;

  if (wc > 0) {
    const size_t size =
        arenaSize(wc, block->focusPresent, block->guideDotsPresent, block->wordFlagsPresent, block->wordSpacesPresent,
                  textBytes, block->wordSizesPresent, block->sourceRangesPresent);
    const int remaining = file.available();
    if (remaining < 0 || static_cast<size_t>(remaining) < size) {
      LOG_ERR("TXB", "Deserialization failed: truncated arena (%u bytes needed, %d available)",
              static_cast<uint32_t>(size), remaining);
      return nullptr;
    }
    block->arena = makeUniqueNoThrow<uint8_t[]>(size);
    if (!block->arena) {
      LOG_ERR("TXB", "OOM: arena %u bytes", static_cast<uint32_t>(size));
      return nullptr;
    }
    if (file.read(block->arena.get(), size) != static_cast<int>(size)) {
      LOG_ERR("TXB", "Deserialization failed: arena read (%u bytes)", static_cast<uint32_t>(size));
      return nullptr;
    }
    block->bindArenaPointers();

    if (block->sourceRangesPresent) {
      for (uint16_t i = 0; i < wc; ++i) {
        const auto range = block->wordSourceRange(i);
        if (!range.wellFormed()) {
          LOG_ERR("TXB", "Invalid cached source range");
          return nullptr;
        }
      }
    }
    if (block->wordSizesPresent)
      for (uint16_t i = 0; i < wc; ++i) {
        const auto size = block->wordFontSize(i);
        if (size && (size < ScalableContentMinPointSize || size > ScalableContentMaxPointSize)) {
          LOG_ERR("TXB", "Invalid cached word point size");
          return nullptr;
        }
      }
    const uint16_t* textOff = block->textOffArr;
    const char* text = block->textArr;
    if (textOff[0] != 0 || text[textBytes - 1] != '\0') {
      LOG_ERR("TXB", "Deserialization failed: corrupt text layout");
      return nullptr;
    }
    for (uint16_t i = 1; i < wc; i++) {
      if (textOff[i] <= textOff[i - 1] || textOff[i] >= textBytes || text[textOff[i] - 1] != '\0') {
        LOG_ERR("TXB", "Deserialization failed: corrupt word offset %u", i);
        return nullptr;
      }
    }
  }

  if (wc && (initialLetterBytes > block->wordTextLen(0) ||
             (static_cast<uint8_t>(block->wordText(0)[initialLetterBytes]) & 0xc0) == 0x80)) {
    LOG_ERR("TXB", "Invalid initial-letter prefix");
    return nullptr;
  }

  uint8_t hasFamilies = 0;
  if (!serialization::tryReadPod(file, hasFamilies) || hasFamilies > 1) return nullptr;
  if (hasFamilies) {
    block->wordFamilies = makeUniqueNoThrow<uint32_t[]>(wc);
    if (!block->wordFamilies || file.read(block->wordFamilies.get(), wc * sizeof(uint32_t)) != wc * sizeof(uint32_t)) {
      LOG_ERR("TXB", "Cannot read publisher font references");
      return nullptr;
    }
  }
  uint16_t rubyCount = 0;
  if (!serialization::tryReadPod(file, rubyCount) || rubyCount > wc) {
    LOG_ERR("TXB", "Deserialization failed: invalid ruby count %u", rubyCount);
    return nullptr;
  }
  if (rubyCount > 0) {
    PageDeserializationBudget::HeapBudget heapBudget;
    constexpr size_t MIN_RUBY_RECORD_BYTES = sizeof(uint16_t) + sizeof(uint32_t);
    const int availableResult = file.available();
    const size_t available = availableResult > 0 ? static_cast<size_t>(availableResult) : 0;
    if (!PageDeserializationBudget::hasMinimumSerializedBytes(available, rubyCount, MIN_RUBY_RECORD_BYTES,
                                                              MIN_SERIALIZED_BLOCK_STYLE_BYTES)) {
      LOG_ERR("TXB", "Deserialization failed: truncated cached ruby annotations");
      return nullptr;
    }
    const size_t slotBytes = static_cast<size_t>(wc) * sizeof(std::string);
    if (heapBudget.needsRefresh(slotBytes, 1)) {
      const auto heap = MemoryBudget::snapshot();
      heapBudget.refresh(heap.freeHeap, heap.maxAllocHeap);
    }
    if (!heapBudget.canReserve(wc, sizeof(std::string))) {
      LOG_ERR("TXB", "Low heap for cached ruby slots (words=%u bytes=%u free=%u max=%u)", wc,
              static_cast<unsigned>(slotBytes), static_cast<unsigned>(heapBudget.availableAfterReserves()),
              static_cast<unsigned>(heapBudget.largestFreeBlock()));
      return nullptr;
    }
    heapBudget.recordReserve(wc, sizeof(std::string));
    block->rubyTexts.resize(wc);
    for (uint16_t i = 0; i < rubyCount; ++i) {
      uint16_t wordIndex = 0;
      std::string ruby;
      if (!serialization::tryReadPod(file, wordIndex) || wordIndex >= wc ||
          !tryReadRubyString(file, ruby, heapBudget)) {
        LOG_ERR("TXB", "Deserialization failed: invalid ruby annotation");
        return nullptr;
      }
      block->rubyTexts[wordIndex] = std::move(ruby);
    }
  } else {
    // No annotation data follows the count, so the style read below starts here.
  }

  BlockStyle& blockStyle = block->blockStyle;
  if (!serialization::tryReadPod(file, blockStyle.alignment) ||
      !serialization::tryReadPod(file, blockStyle.textAlignDefined) ||
      !serialization::tryReadPod(file, blockStyle.marginTop) ||
      !serialization::tryReadPod(file, blockStyle.marginBottom) ||
      !serialization::tryReadPod(file, blockStyle.marginLeft) ||
      !serialization::tryReadPod(file, blockStyle.marginRight) ||
      !serialization::tryReadPod(file, blockStyle.paddingTop) ||
      !serialization::tryReadPod(file, blockStyle.paddingBottom) ||
      !serialization::tryReadPod(file, blockStyle.paddingLeft) ||
      !serialization::tryReadPod(file, blockStyle.paddingRight) ||
      !serialization::tryReadPod(file, blockStyle.textIndent) ||
      !serialization::tryReadPod(file, blockStyle.textIndentDefined) ||
      !serialization::tryReadPod(file, blockStyle.isRtl) ||
      !serialization::tryReadPod(file, blockStyle.directionDefined) ||
      !serialization::tryReadPod(file, blockStyle.fontSize) ||
      !serialization::tryReadPod(file, blockStyle.lineHeight) ||
      !serialization::tryReadPod(file, blockStyle.ruledTextOffset) ||
      (blockStyle.fontSize != 0 &&
       (blockStyle.fontSize < ScalableContentMinPointSize || blockStyle.fontSize > ScalableContentMaxPointSize))) {
    LOG_ERR("TXB", "Deserialization failed: truncated block style metadata");
    return nullptr;
  }

  return block;
}
