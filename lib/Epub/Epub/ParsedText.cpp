#include "ParsedText.h"

#include <Arena.h>
#include <ArenaVector.h>
#include <BidiUtils.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <vector>

#include "InlineImageToken.h"
#include "TokenBoundary.h"
#include "WordSpacing.h"
#include "hyphenation/Hyphenator.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

// Soft hyphen byte pattern used throughout EPUBs (UTF-8 for U+00AD).
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;
constexpr size_t PATHOLOGICAL_TOKEN_MIN_BYTES = 128;
constexpr size_t PATHOLOGICAL_TOKEN_SCAN_BYTES = 256;
constexpr size_t RTL_PARAGRAPH_PROBE_WORDS = 3;
constexpr int RTL_PER_WORD_PROBE_DEPTH = 64;
constexpr size_t MIN_JUSTIFY_GAPS = 1;
constexpr char GUIDE_DOT_UTF8[] = "\xc2\xb7";
constexpr uint32_t GUIDE_DOT_CODEPOINT = 0x00B7;
constexpr size_t FOCUS_READING_PERCENT = 43;
constexpr size_t LAYOUT_ARENA_SLAB_BYTES = 4096;
constexpr size_t INITIAL_TOKEN_VECTOR_RESERVE = 16;

bool mayContainRtlBytes(const char* str) {
  for (const auto* p = reinterpret_cast<const unsigned char*>(str); *p; ++p) {
    if (*p >= 0xD6 && *p <= 0xDB) return true;
  }
  return false;
}

// Returns the first rendered codepoint of a word (skipping leading soft hyphens and variation selectors).
uint32_t firstCodepoint(const std::string& word) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  while (true) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (cp == 0) return 0;
    if (cp != 0x00AD && !utf8IsVariationSelector(cp)) return cp;
  }
}

// Returns the last rendered codepoint of a word by scanning backward for the start of the last UTF-8 sequence.
uint32_t lastCodepoint(const std::string& word) {
  size_t end = word.size();
  while (end > 0) {
    // UTF-8 continuation bytes start with 10xxxxxx; scan backward to find the leading byte.
    size_t i = end - 1;
    while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) {
      --i;
    }
    const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + i);
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (!utf8IsVariationSelector(cp)) return cp;
    end = i;
  }
  return 0;
}

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

bool isNoBreakBeforeCjkPunctuation(const uint32_t cp) {
  if (TokenBoundary::allowsBreakAfterExplicitHyphen(cp)) return true;
  switch (cp) {
    case '.':
    case ',':
    case ':':
    case ';':
    case '!':
    case '?':
    case ')':
    case ']':
    case '}':
    case 0x00BB:  // »
    case 0x2019:  // ’
    case 0x201D:  // ”
    case 0x3001:  // 、
    case 0x3002:  // 。
    case 0x3009:  // 〉
    case 0x300B:  // 》
    case 0x300D:  // 」
    case 0x300F:  // 』
    case 0x3011:  // 】
    case 0x3015:  // 〕
    case 0x3017:  // 〗
    case 0x3019:  // 〙
    case 0x301B:  // 〛
    case 0xFF01:  // ！
    case 0xFF09:  // ）
    case 0xFF0C:  // ，
    case 0xFF0E:  // ．
    case 0xFF1A:  // ：
    case 0xFF1B:  // ；
    case 0xFF1F:  // ？
    case 0xFF3D:  // ］
    case 0xFF5D:  // ｝
      return true;
    default:
      return false;
  }
}

bool isClosingPunctuationForJustify(const uint32_t cp) { return isNoBreakBeforeCjkPunctuation(cp); }

bool isNoBreakAfterCjkPunctuation(const uint32_t cp) {
  switch (cp) {
    case '(':
    case '[':
    case '{':
    case 0x00AB:  // «
    case 0x2018:  // ‘
    case 0x201C:  // “
    case 0x3008:  // 〈
    case 0x300A:  // 《
    case 0x300C:  // 「
    case 0x300E:  // 『
    case 0x3010:  // 【
    case 0x3014:  // 〔
    case 0x3016:  // 〖
    case 0x3018:  // 〘
    case 0x301A:  // 〚
    case 0xFF08:  // （
    case 0xFF3B:  // ［
    case 0xFF5B:  // ｛
      return true;
    default:
      return false;
  }
}

bool containsCjkBreakableCodepoint(const std::string& text) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
  while (*ptr) {
    const uint32_t cp = utf8NextCodepoint(&ptr);
    if (utf8IsCjkBreakable(cp)) {
      return true;
    }
  }
  return false;
}

uint32_t countCodepoints(const std::string_view text) {
  const auto* ptr = reinterpret_cast<const unsigned char*>(text.data());
  const auto* const end = ptr + text.size();
  uint32_t count = 0;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }
  return count;
}

bool cjkBoundaryAllowsBreak(const uint32_t leftCp, const uint32_t rightCp) {
  if (utf8IsCombiningMark(rightCp) || utf8IsVariationSelector(rightCp)) return false;
  if (TokenBoundary::allowsBreakAfterExplicitHyphen(leftCp)) return !isNoBreakBeforeCjkPunctuation(rightCp);
  if (!utf8IsCjkBreakable(leftCp) && !utf8IsCjkBreakable(rightCp)) return false;
  if (isNoBreakAfterCjkPunctuation(leftCp) || isNoBreakBeforeCjkPunctuation(rightCp)) return false;
  return true;
}

// Korean separates words with spaces, so a boundary touching Hangul is not a gap-less break inside
// a line. hangulLineEndBreaks() still lets a Hangul word split there at a line end.
bool hasCjkBreakOpportunityBetween(const uint32_t leftCp, const uint32_t rightCp) {
  if (TokenBoundary::allowsBreakAfterExplicitHyphen(leftCp)) return cjkBoundaryAllowsBreak(leftCp, rightCp);
  if (utf8IsHangul(leftCp) || utf8IsHangul(rightCp)) return false;
  return cjkBoundaryAllowsBreak(leftCp, rightCp);
}

// Line-end split points inside a Hangul word, using the CJK boundary rules (no hyphen is drawn).
std::vector<Hyphenator::BreakInfo> hangulLineEndBreaks(const std::string& word) {
  std::vector<Hyphenator::BreakInfo> breaks;
  if (word.empty()) return breaks;
  const auto* const start = reinterpret_cast<const unsigned char*>(word.c_str());
  const auto* ptr = start;
  uint32_t prev = utf8NextCodepoint(&ptr);
  while (*ptr) {
    const size_t offset = static_cast<size_t>(ptr - start);
    const uint32_t cur = utf8NextCodepoint(&ptr);
    if ((utf8IsHangul(prev) || utf8IsHangul(cur)) && cjkBoundaryAllowsBreak(prev, cur)) {
      breaks.push_back({offset, false});
    }
    prev = cur;
  }
  return breaks;
}

std::vector<size_t> cjkCharacterBreakByteOffsets(const std::string& text) {
  // Count first: most Latin and Hangul words have no accepted boundaries and
  // need no allocation. The second pass stores only the returned offsets.
  const auto scan = [&](std::vector<size_t>* offsets) {
    const auto* ptr = reinterpret_cast<const unsigned char*>(text.c_str());
    const auto* const start = ptr;
    uint32_t previous = utf8NextCodepoint(&ptr);
    if (previous == 0) return size_t{0};
    bool hasBreakable = utf8IsCjkBreakable(previous) || TokenBoundary::allowsBreakAfterExplicitHyphen(previous);
    bool hasLeadingContent = false;
    size_t count = 0;
    while (*ptr) {
      const size_t offset = static_cast<size_t>(ptr - start);
      const uint32_t current = utf8NextCodepoint(&ptr);
      if (current == 0) break;
      hasBreakable |= utf8IsCjkBreakable(current) || TokenBoundary::allowsBreakAfterExplicitHyphen(current);
      const bool dash = TokenBoundary::allowsBreakAfterExplicitHyphen(previous);
      if (!dash) hasLeadingContent = true;
      if ((!dash || hasLeadingContent) && hasCjkBreakOpportunityBetween(previous, current)) {
        ++count;
        if (offsets) offsets->push_back(offset);
      }
      previous = current;
    }
    return hasBreakable ? count : size_t{0};
  };
  const size_t count = scan(nullptr);
  if (count == 0) return {};
  std::vector<size_t> allowedOffsets;
  allowedOffsets.reserve(count);
  scan(&allowedOffsets);
  return allowedOffsets;
}

int computeJustifyExtra(const int spareSpace, const size_t gapCount) {
  if (gapCount < MIN_JUSTIFY_GAPS || spareSpace <= 0) return 0;
  return spareSpace / static_cast<int>(gapCount);
}

bool isBase64LikeChar(const char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

bool isPathologicalUnbrokenToken(const std::string& word) {
  if (word.size() < PATHOLOGICAL_TOKEN_MIN_BYTES) {
    return false;
  }

  const size_t scanBytes = std::min(word.size(), PATHOLOGICAL_TOKEN_SCAN_BYTES);
  size_t base64LikeCount = 0;
  for (size_t i = 0; i < scanBytes; ++i) {
    const char c = word[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      return false;
    }
    if (isBase64LikeChar(c)) {
      ++base64LikeCount;
    }
  }

  return base64LikeCount * 100 >= scanBytes * 95;
}

// Removes every soft hyphen in-place so rendered glyphs match measured widths.
void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

// Returns the advance width for a word while ignoring soft hyphen glyphs and optionally appending a visible hyphen.
// Uses advance width (sum of glyph advances + kerning) rather than bounding box width so that italic glyph overhangs
// don't inflate inter-word spacing.
uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool appendHyphen, const int8_t characterSpacing) {
  if (word.size() == 1 && word[0] == ' ' && !appendHyphen) {
    return renderer.getSpaceWidth(fontId, style);
  }
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) {
    return renderer.getTextAdvanceX(fontId, word.c_str(), style, 0, characterSpacing);
  }

  std::string sanitized = word;
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return renderer.getTextAdvanceX(fontId, sanitized.c_str(), style, 0, characterSpacing);
}

bool isWordCharacter(uint32_t cp);

struct FocusTokenMetadata {
  EpdFontFamily::Style style;
  uint8_t boundary;
};

uint32_t firstCodepointAtByteOffset(const std::string& word, const size_t byteOffset) {
  if (byteOffset >= word.size()) return 0;
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + byteOffset);
  return utf8NextCodepoint(&ptr);
}

uint32_t lastCodepointBeforeByteOffset(const std::string& word, const size_t byteOffset) {
  if (word.empty() || byteOffset == 0) return 0;
  size_t i = std::min(byteOffset, word.size()) - 1;
  while (i > 0 && (static_cast<uint8_t>(word[i]) & 0xC0) == 0x80) {
    --i;
  }
  const auto* ptr = reinterpret_cast<const unsigned char*>(word.c_str() + i);
  return utf8NextCodepoint(&ptr);
}

FocusTokenMetadata computeFocusMetadata(const std::string_view segment, const EpdFontFamily::Style baseStyle,
                                        const bool focusReadingEnabled) {
  if (!focusReadingEnabled || (baseStyle & EpdFontFamily::BOLD) != 0 || segment.empty()) {
    return {baseStyle, 0};
  }

  size_t charCount = 0;
  const auto* countPtr = reinterpret_cast<const unsigned char*>(segment.data());
  const auto* const countEnd = countPtr + segment.length();
  while (countPtr < countEnd) {
    const auto* const cpStart = countPtr;
    const uint32_t cp = utf8NextCodepoint(&countPtr);
    if (!isWordCharacter(cp)) {
      break;
    }
    if (countPtr <= cpStart) {
      break;
    }
    charCount++;
  }

  if (charCount == 0) {
    return {baseStyle, 0};
  }

  // Target 43% for 1-bold at 4 chars and 3-bold at 7 chars with floor truncation.
  size_t targetBoldChars = (charCount * FOCUS_READING_PERCENT) / 100;
  targetBoldChars = std::clamp<size_t>(targetBoldChars, 1, 9);

  if (targetBoldChars >= charCount) {
    return {static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::BOLD), 0};
  }

  const auto* splitPtr = reinterpret_cast<const unsigned char*>(segment.data());
  for (size_t i = 0; i < targetBoldChars; ++i) {
    utf8NextCodepoint(&splitPtr);
  }
  const size_t splitByteOffset = splitPtr - reinterpret_cast<const unsigned char*>(segment.data());
  return {baseStyle, static_cast<uint8_t>(std::min<size_t>(splitByteOffset, UINT8_MAX))};
}

int measureFocusRunOffset(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const uint8_t boundary, const bool rtl,
                          const int8_t characterSpacing) {
  if (boundary == 0 || boundary >= word.size()) {
    return 0;
  }

  const auto boldStyle = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
  char prefixBuf[40];
  const size_t prefixLen = std::min<size_t>(boundary, sizeof(prefixBuf) - 1);
  memcpy(prefixBuf, word.c_str(), prefixLen);
  prefixBuf[prefixLen] = '\0';

  const uint32_t firstSuffixCp = firstCodepointAtByteOffset(word, boundary);
  if (!rtl) {
    // The suffix starts in a separate drawText() call. Keep the final bold
    // glyph's advance and cross-run kerning in one fixed-point rounding step,
    // matching drawText() and preventing one-pixel collisions at the split.
    return renderer.getTextAdvanceX(fontId, prefixBuf, boldStyle, firstSuffixCp, characterSpacing);
  }

  const int suffixWidth = renderer.getTextAdvanceX(fontId, word.c_str() + boundary, style, 0, characterSpacing);
  const int kern = renderer.getKerning(fontId, lastCodepointBeforeByteOffset(word, boundary), firstSuffixCp, boldStyle,
                                       characterSpacing);
  return suffixWidth + kern;
}

uint16_t measureTokenWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                           const EpdFontFamily::Style style, const uint8_t focusBoundary, const bool appendHyphen,
                           const int8_t characterSpacing) {
  uint16_t imageId, imageWidth, imageHeight;
  if (parseInlineImageToken(word.c_str(), imageId, imageWidth, imageHeight)) return imageWidth;
  if (focusBoundary == 0 || focusBoundary >= word.size() || appendHyphen || containsSoftHyphen(word)) {
    return measureWordWidth(renderer, fontId, word, style, appendHyphen, characterSpacing);
  }

  const int suffixX = measureFocusRunOffset(renderer, fontId, word, style, focusBoundary, false, characterSpacing);
  const int suffixWidth = renderer.getTextAdvanceX(fontId, word.c_str() + focusBoundary, style, 0, characterSpacing);
  return static_cast<uint16_t>(std::max(0, suffixX + suffixWidth));
}

int guideDotNaturalGap(const GfxRenderer& renderer, const int fontId, const std::string& leftWord,
                       const std::string& rightWord, const EpdFontFamily::Style leftStyle) {
  return renderer.getSpaceAdvance(fontId, lastCodepoint(leftWord), GUIDE_DOT_CODEPOINT, leftStyle) +
         renderer.getTextAdvanceX(fontId, GUIDE_DOT_UTF8, EpdFontFamily::REGULAR) +
         renderer.getSpaceAdvance(fontId, GUIDE_DOT_CODEPOINT, firstCodepoint(rightWord), EpdFontFamily::REGULAR);
}

size_t guideDotGapSlots(const std::string& rightWord) {
  return 1 + (isClosingPunctuationForJustify(firstCodepoint(rightWord)) ? 0 : 1);
}

int wordSpacingExtraFromGap(const int gap, const uint8_t wordSpacing) { return WordSpacing::extra(gap, wordSpacing); }

int guideDotWordSpacingExtra(const GfxRenderer& renderer, const int fontId, const std::string& leftWord,
                             const std::string& rightWord, const EpdFontFamily::Style leftStyle,
                             const uint8_t wordSpacing) {
  const int naturalWordGap =
      renderer.getSpaceAdvance(fontId, lastCodepoint(leftWord), firstCodepoint(rightWord), leftStyle);
  return wordSpacingExtraFromGap(naturalWordGap, wordSpacing);
}

int naturalGapBeforeToken(const GfxRenderer& renderer, const int fontId, const std::string& leftWord,
                          const std::string& rightWord, const EpdFontFamily::Style leftStyle, const bool continues,
                          const bool noSpaceBefore, const bool guideDotBefore, const uint8_t wordSpacing,
                          const int8_t characterSpacing) {
  uint16_t imageId, imageWidth, imageHeight;
  const bool imageEdge = parseInlineImageToken(leftWord.c_str(), imageId, imageWidth, imageHeight) ||
                         parseInlineImageToken(rightWord.c_str(), imageId, imageWidth, imageHeight);
  if (guideDotBefore) {
    const int extraGap = guideDotWordSpacingExtra(renderer, fontId, leftWord, rightWord, leftStyle, wordSpacing);
    return guideDotNaturalGap(renderer, fontId, leftWord, rightWord, leftStyle) + extraGap;
  }
  if (noSpaceBefore) {
    return imageEdge ? 0
                     : renderer.getKerning(fontId, lastCodepoint(leftWord), firstCodepoint(rightWord), leftStyle,
                                           characterSpacing) -
                           renderer.getKerning(fontId, lastCodepoint(leftWord), firstCodepoint(rightWord), leftStyle);
  }
  if (continues) {
    if (imageEdge) return 0;
    return renderer.getKerning(fontId, lastCodepoint(leftWord), firstCodepoint(rightWord), leftStyle, characterSpacing);
  }
  if (imageEdge) {
    const int gap = renderer.getSpaceWidth(fontId, leftStyle);
    return gap + wordSpacingExtraFromGap(gap, wordSpacing);
  }
  const int naturalGap =
      renderer.getSpaceAdvance(fontId, lastCodepoint(leftWord), firstCodepoint(rightWord), leftStyle);
  return naturalGap + wordSpacingExtraFromGap(naturalGap, wordSpacing);
}

size_t gapSlotsBeforeToken(const std::string& leftWord, const std::string& rightWord, const bool continues,
                           const bool noSpaceBefore, const bool guideDotBefore) {
  uint16_t imageId, imageWidth, imageHeight;
  if ((continues || noSpaceBefore) && (parseInlineImageToken(leftWord.c_str(), imageId, imageWidth, imageHeight) ||
                                       parseInlineImageToken(rightWord.c_str(), imageId, imageWidth, imageHeight)))
    return 0;
  if (guideDotBefore) {
    return guideDotGapSlots(rightWord);
  }
  // A same-word boundary may allow wrapping after a hyphen, but must not stretch.
  // CJK boundaries (noSpaceBefore without continues) remain justification slots.
  if (continues) {
    return rightWord == " " ? 1 : 0;
  }
  return isClosingPunctuationForJustify(firstCodepoint(rightWord)) ? 0 : 1;
}

int guideDotSecondGap(const GfxRenderer& renderer, const int fontId, const std::string& rightWord) {
  return renderer.getSpaceAdvance(fontId, GUIDE_DOT_CODEPOINT, firstCodepoint(rightWord), EpdFontFamily::REGULAR);
}

// Checks if a UTF-8 codepoint should be counted as part of a word for Focus Reading
bool isWordCharacter(uint32_t cp) {
  // ASCII range (Catches 95%+ of characters immediately)
  if (cp < 128) {
    // Bitwise trick: (cp | 0x20) converts uppercase ASCII to lowercase.
    // This checks for A-Z and a-z mathematically, avoiding memory lookups and <cctype>
    return ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z') || cp == '\'';
  }

  // General Punctuation Block, Currency, Math, Arrows, & Symbols (0x2000 - 0x2BFF)
  if (cp >= 0x2000 && cp <= 0x2BFF) {
    // Explicitly allow smart quotes, reject all other general punctuation (em-dashes, etc.)
    return cp == 0x2018 || cp == 0x2019;
  }

  // Latin-1 Punctuation Block (0x00A1 - 0x00BF)
  if (cp >= 0x00A1 && cp <= 0x00BF) {
    // Allow ordinal indicators and micro sign, reject the rest (¡, ¿, «, », etc.)
    return cp == 0x00AA || cp == 0x00B5 || cp == 0x00BA;
  }

  // Rejects Two-em dash, Three-em dash, Double oblique hyphen, etc.
  if (cp >= 0x2E00 && cp <= 0x2E7F) return false;

  // Rejects Modifier Minus (0x02D7), Small Hyphen (0xFE63), and Fullwidth Hyphen (0xFF0D)
  if (cp == 0x02D7 || cp == 0xFE63 || cp == 0xFF0D) return false;
  // Assume all other Unicode ranges (accented letters, Cyrillic, Greek, etc.) are valid

  return true;
}

bool isCjkIdeograph(const uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
         (cp >= 0x20000 && cp <= 0x3FFFF);
}

}  // namespace

void ParsedText::reserveTokenCapacity(const size_t additionalTokens) {
  const size_t requiredSize = words.size() + additionalTokens;
  if (wordStyles.capacity() >= requiredSize && wordContinues.capacity() >= requiredSize &&
      wordNoSpaceBefore.capacity() >= requiredSize && wordFocusBoundary.capacity() >= requiredSize &&
      wordGuideDotBefore.capacity() >= requiredSize && wordBackgroundBlack.capacity() >= requiredSize &&
      wordVisibleOffsetDeltas.capacity() >= requiredSize) {
    return;
  }

  size_t newCapacity = wordStyles.capacity() == 0 ? INITIAL_TOKEN_VECTOR_RESERVE : wordStyles.capacity() * 2;
  if (newCapacity < requiredSize) {
    newCapacity = requiredSize;
  }

  // words and rubyTexts are deques: their chunked growth avoids the large
  // contiguous reallocations this method is intended to prevent.
  wordStyles.reserve(newCapacity);
  if (!wordFontSizes.empty()) wordFontSizes.reserve(newCapacity);
  if (!wordFontFamilies.empty()) wordFontFamilies.reserve(newCapacity);
  wordContinues.reserve(newCapacity);
  wordNoSpaceBefore.reserve(newCapacity);
  wordFocusBoundary.reserve(newCapacity);
  wordGuideDotBefore.reserve(newCapacity);
  wordBackgroundBlack.reserve(newCapacity);
  wordVisibleOffsetDeltas.reserve(newCapacity);
}

uint32_t ParsedText::visibleOffsetBaseAt(const size_t wordIndex) const {
  uint32_t base = visibleOffsetBase;
  for (const auto& rebase : visibleOffsetRebases) {
    if (rebase.wordIndex > wordIndex) break;
    base = rebase.base;
  }
  return base;
}

uint32_t ParsedText::visibleOffsetAt(const size_t wordIndex) const {
  if (wordIndex >= wordVisibleOffsetDeltas.size()) return 0;
  return visibleOffsetBaseAt(wordIndex) + wordVisibleOffsetDeltas[wordIndex];
}

void ParsedText::pushVisibleOffset(const uint32_t offset) {
  uint32_t base = visibleOffsetBase;
  if (wordVisibleOffsetDeltas.empty()) {
    visibleOffsetBase = offset;
    base = offset;
  } else if (!visibleOffsetRebases.empty()) {
    base = visibleOffsetRebases.back().base;
  }
  if (offset < base || offset - base > std::numeric_limits<uint16_t>::max()) {
    visibleOffsetRebases.push_back({wordVisibleOffsetDeltas.size(), offset});
    base = offset;
  }
  wordVisibleOffsetDeltas.push_back(static_cast<uint16_t>(offset - base));
}

void ParsedText::insertVisibleOffset(const size_t wordIndex, const uint32_t offset) {
  const uint32_t base = wordIndex > 0 ? visibleOffsetBaseAt(wordIndex - 1) : visibleOffsetBase;
  for (auto& rebase : visibleOffsetRebases) {
    if (rebase.wordIndex >= wordIndex) rebase.wordIndex++;
  }
  uint32_t insertionBase = base;
  if (offset < base || offset - base > std::numeric_limits<uint16_t>::max()) {
    const auto it = std::find_if(visibleOffsetRebases.begin(), visibleOffsetRebases.end(),
                                 [wordIndex](const auto& rebase) { return rebase.wordIndex > wordIndex; });
    visibleOffsetRebases.insert(it, {wordIndex, offset});
    insertionBase = offset;
  }
  wordVisibleOffsetDeltas.insert(wordVisibleOffsetDeltas.begin() + wordIndex,
                                 static_cast<uint16_t>(offset - insertionBase));
}

void ParsedText::eraseVisibleOffsetPrefix(const size_t count) {
  if (count >= wordVisibleOffsetDeltas.size()) {
    wordVisibleOffsetDeltas.clear();
    visibleOffsetRebases.clear();
    visibleOffsetBase = 0;
    return;
  }
  const uint32_t newBase = visibleOffsetBaseAt(count);
  wordVisibleOffsetDeltas.erase(wordVisibleOffsetDeltas.begin(), wordVisibleOffsetDeltas.begin() + count);
  size_t write = 0;
  for (auto rebase : visibleOffsetRebases) {
    if (rebase.wordIndex <= count) continue;
    rebase.wordIndex -= count;
    visibleOffsetRebases[write++] = rebase;
  }
  visibleOffsetRebases.resize(write);
  visibleOffsetBase = newBase;
}

int16_t ParsedText::inlinePaddingBefore(const size_t wordIndex) const {
  const auto it =
      std::lower_bound(inlinePaddings.begin(), inlinePaddings.end(), wordIndex,
                       [](const InlinePadding& padding, const size_t index) { return padding.wordIndex < index; });
  return it != inlinePaddings.end() && it->wordIndex == wordIndex ? it->pixels : 0;
}

void ParsedText::shiftInlinePaddingsAfter(const size_t wordIndex) {
  for (auto& padding : inlinePaddings) {
    if (padding.wordIndex > wordIndex) {
      ++padding.wordIndex;
    }
  }
}

void ParsedText::eraseInlinePaddingPrefix(const size_t count) {
  auto firstRemaining =
      std::lower_bound(inlinePaddings.begin(), inlinePaddings.end(), count,
                       [](const InlinePadding& padding, const size_t index) { return padding.wordIndex < index; });
  inlinePaddings.erase(inlinePaddings.begin(), firstRemaining);
  for (auto& padding : inlinePaddings) {
    padding.wordIndex -= count;
  }
}

void ParsedText::setInitialLetter(const char* prefix) {
  const size_t length = std::min<size_t>(strlen(prefix), sizeof(initialLetter) - 1);
  memcpy(initialLetter, prefix, length);
  initialLetter[length] = '\0';
}

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool underline,
                         const bool attachToPrevious, const bool backgroundBlack, const uint8_t linkId,
                         const uint32_t visibleTextOffset, const uint32_t referenceTextOffset,
                         const int16_t leadingPadding, const bool noSpaceBefore, const uint8_t pointSize,
                         const uint32_t fontFamily) {
  if (word.empty()) return;

  // The device fonts carry no combining-mark positioning, so EPUB text stored in NFD
  // (a base letter followed by separate combining accents -- common for Vietnamese,
  // and used for many EPUB <h1> chapter headings) renders with the marks detached or
  // misplaced. Compose to NFC here, the single funnel every word passes through, so a
  // precomposed glyph is used instead. This runs once per word at layout time (the
  // result is cached in the section file) and is a cheap no-op for mark-free text.
  word = utf8ComposeNfc(word);

  EpdFontFamily::Style baseStyle = fontStyle;
  if (underline) {
    baseStyle = static_cast<EpdFontFamily::Style>(baseStyle | EpdFontFamily::UNDERLINE);
  }
  const bool wordStartsRtl = !hasRtlWord && mayContainRtlBytes(word.c_str()) &&
                             BidiUtils::startsWithRtl(word.c_str(), RTL_PER_WORD_PROBE_DEPTH);

  bool guideDotBeforeNextToken = false;
  const auto pushToken = [&](std::string token, const bool continues, const bool noSpaceBefore,
                             const EpdFontFamily::Style tokenStyle, const uint8_t focusBoundary,
                             const uint32_t tokenVisibleOffset, const uint32_t tokenReferenceOffset,
                             const int16_t tokenLeadingPadding) {
    reserveTokenCapacity(1);
    const size_t tokenIndex = words.size();
    words.push_back(std::move(token));
    wordStyles.push_back(tokenStyle);
    if (fontFamily && wordFontFamilies.empty()) {
      wordFontFamilies.reserve(wordStyles.capacity());
      wordFontFamilies.resize(tokenIndex, 0);
    }
    if (fontFamily || !wordFontFamilies.empty()) wordFontFamilies.push_back(fontFamily);
    if (pointSize && wordFontSizes.empty()) {
      wordFontSizes.reserve(wordStyles.capacity());
      wordFontSizes.resize(tokenIndex, 0);
    }
    if (pointSize || !wordFontSizes.empty()) wordFontSizes.push_back(pointSize);
    wordContinues.push_back(continues);
    wordNoSpaceBefore.push_back(noSpaceBefore);
    wordFocusBoundary.push_back(focusBoundary);
    wordGuideDotBefore.push_back(guideDotBeforeNextToken);
    guideDotBeforeNextToken = false;
    const uint8_t linkFlags =
        static_cast<uint8_t>((linkId << TextBlock::WORD_FLAG_LINK_ID_SHIFT) & TextBlock::WORD_FLAG_LINK_ID_MASK);
    wordBackgroundBlack.push_back(
        static_cast<uint8_t>((backgroundBlack ? TextBlock::WORD_FLAG_BACKGROUND_BLACK : 0) | linkFlags));
    if (tokenLeadingPadding > 0) {
      inlinePaddings.push_back({tokenIndex, tokenLeadingPadding});
    }
    pushVisibleOffset(tokenVisibleOffset);
    if (trackReferenceOffsets) wordReferenceOffsets.push_back(tokenReferenceOffset);
    if (!rubyTexts.empty()) {
      rubyTexts.push_back("");
    }
  };

  bool effectiveAttachToPrevious = attachToPrevious;
  bool effectiveNoSpaceBefore = noSpaceBefore;
  uint16_t imageId, imageWidth, imageHeight;
  if (attachToPrevious && !words.empty() &&
      parseInlineImageToken(words.back().c_str(), imageId, imageWidth, imageHeight)) {
    // The following text may wrap after the image without gaining a space.
    effectiveAttachToPrevious = false;
    effectiveNoSpaceBefore = true;
  }
  // Only a glued token (attachToPrevious == true, i.e. no whitespace separated it from the
  // previous one in the source) may be turned into a gap-less break opportunity. When real
  // whitespace separated the two words, that space is content and must be rendered: Korean
  // is a space-delimited script written in Hangul, which utf8IsCjkBreakable() covers.
  if (attachToPrevious && !words.empty() &&
      hasCjkBreakOpportunityBetween(lastCodepoint(words.back()), firstCodepoint(word))) {
    effectiveAttachToPrevious = TokenBoundary::allowsBreakAfterExplicitHyphen(lastCodepoint(words.back()));
    effectiveNoSpaceBefore = true;
  }

  // GUIDE READING: store a virtual middle dot (U+00B7) before the next real token.
  if (guideReadingEnabled && !effectiveAttachToPrevious && !effectiveNoSpaceBefore && !words.empty() &&
      !parseInlineImageToken(words.back().c_str(), imageId, imageWidth, imageHeight)) {
    guideDotBeforeNextToken = true;
  }

  if (auto breakOffsets = cjkCharacterBreakByteOffsets(word); !breakOffsets.empty()) {
    // CJK-heavy paragraphs can push hundreds of tiny tokens quickly when CSS toggles
    // inline styles. Reserve once up front to avoid repeated vector growth reallocations.
    reserveTokenCapacity(breakOffsets.size() + 1);
    const bool focusSplitTokens = focusReadingEnabled && !containsCjkBreakableCodepoint(word);
    bool firstToken = true;
    size_t tokenStart = 0;
    uint32_t tokenVisibleOffset = visibleTextOffset;
    uint32_t tokenReferenceOffset = referenceTextOffset;
    for (const size_t breakOffset : breakOffsets) {
      if (breakOffset <= tokenStart || breakOffset > word.size()) continue;
      const std::string_view token(word.data() + tokenStart, breakOffset - tokenStart);
      const bool attach = firstToken ? effectiveAttachToPrevious
                                     : TokenBoundary::allowsBreakAfterExplicitHyphen(lastCodepoint(words.back()));
      const auto focus = computeFocusMetadata(token, baseStyle, focusSplitTokens);
      pushToken(std::string(token), attach, firstToken ? effectiveNoSpaceBefore : true, focus.style, focus.boundary,
                tokenVisibleOffset, tokenReferenceOffset, firstToken ? leadingPadding : 0);
      const uint32_t tokenCodepoints = countCodepoints(token);
      tokenVisibleOffset += tokenCodepoints;
      tokenReferenceOffset += tokenCodepoints;
      firstToken = false;
      tokenStart = breakOffset;
    }
    if (tokenStart < word.size()) {
      const bool attach = firstToken ? effectiveAttachToPrevious
                                     : TokenBoundary::allowsBreakAfterExplicitHyphen(lastCodepoint(words.back()));
      const auto focus = computeFocusMetadata(std::string_view(word).substr(tokenStart), baseStyle, focusSplitTokens);
      pushToken(word.substr(tokenStart), attach, firstToken ? effectiveNoSpaceBefore : true, focus.style,
                focus.boundary, tokenVisibleOffset, tokenReferenceOffset, firstToken ? leadingPadding : 0);
    }
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  if (containsCjkBreakableCodepoint(word)) {
    pushToken(std::move(word), effectiveAttachToPrevious, effectiveNoSpaceBefore, baseStyle, 0, visibleTextOffset,
              referenceTextOffset, leadingPadding);
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  // Already-bold text should stay fully bold; focus splitting would make its suffix regular later.
  if (!this->focusReadingEnabled || (baseStyle & EpdFontFamily::BOLD) != 0) {
    pushToken(std::move(word), effectiveAttachToPrevious, effectiveNoSpaceBefore, baseStyle, 0, visibleTextOffset,
              referenceTextOffset, leadingPadding);
    if (wordStartsRtl) {
      hasRtlWord = true;
    }
    return;
  }

  // --- FOCUS READING LOGIC BELOW ---

  // Worst case: a segment boundary on each byte (highly punctuated UTF-8 text).
  reserveTokenCapacity(word.length());

  // Lambda helper to process and push individual sub-segments of the string
  // Use std::string_view to avoid heap allocations when slicing
  auto processSegment = [&](std::string_view segment, bool isWord, bool attach, bool noSpaceBefore,
                            const int16_t segmentLeadingPadding) {
    const auto* begin = reinterpret_cast<const unsigned char*>(word.data());
    const auto* segmentBegin = reinterpret_cast<const unsigned char*>(segment.data());
    uint32_t segmentOffset = visibleTextOffset;
    uint32_t segmentReferenceOffset = referenceTextOffset;
    while (begin < segmentBegin) {
      utf8NextCodepoint(&begin);
      segmentOffset++;
      segmentReferenceOffset++;
    }
    if (!isWord) {
      // Punctuation and Numbers stay regular
      pushToken(std::string(segment), attach, noSpaceBefore, baseStyle, 0, segmentOffset, segmentReferenceOffset,
                segmentLeadingPadding);
    } else {
      const FocusTokenMetadata focus = computeFocusMetadata(segment, baseStyle, focusReadingEnabled);
      pushToken(std::string(segment), attach, noSpaceBefore, focus.style, focus.boundary, segmentOffset,
                segmentReferenceOffset, segmentLeadingPadding);
    }
  };

  // Tokenize the string by alternating states (Word vs. Non-Word)
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(word.c_str());
  const unsigned char* end = ptr + word.length();

  const unsigned char* segmentStart = ptr;
  uint32_t firstCp = utf8NextCodepoint(&ptr);  // Consume the first char to determine initial state
  bool inWordSegment = isWordCharacter(firstCp);

  bool isFirstSegment = true;

  while (ptr < end) {
    const unsigned char* currentCpStart = ptr;
    uint32_t cp = utf8NextCodepoint(&ptr);
    // A variation selector belongs to the preceding base character regardless
    // of whether that base is a word character or a symbol.
    bool isWordChar = utf8IsVariationSelector(cp) ? inWordSegment : isWordCharacter(cp);

    // Whenever the character type flips, slice off the segment we just completed and process it
    if (isWordChar != inWordSegment) {
      size_t segmentLen = currentCpStart - segmentStart;
      std::string_view segment(reinterpret_cast<const char*>(segmentStart), segmentLen);

      // Only the very first segment inherits the original attachToPrevious flag.
      // Every subsequent segment MUST attach=true so it glues seamlessly to the prefix.
      processSegment(segment, inWordSegment, isFirstSegment ? effectiveAttachToPrevious : true,
                     isFirstSegment ? effectiveNoSpaceBefore : false, isFirstSegment ? leadingPadding : 0);

      // Setup for the next segment
      segmentStart = currentCpStart;
      inWordSegment = isWordChar;
      isFirstSegment = false;
    }
  }

  // Process the final remaining segment
  size_t segmentLen = end - segmentStart;
  std::string_view segment(reinterpret_cast<const char*>(segmentStart), segmentLen);
  processSegment(segment, inWordSegment, isFirstSegment ? effectiveAttachToPrevious : true,
                 isFirstSegment ? effectiveNoSpaceBefore : false, isFirstSegment ? leadingPadding : 0);
  if (wordStartsRtl) {
    hasRtlWord = true;
  }
}

void ParsedText::addInlineImage(const uint16_t id, const uint16_t width, const uint16_t height,
                                const bool attachToPrevious, const uint32_t visibleTextOffset,
                                const uint32_t referenceTextOffset) {
  reserveTokenCapacity(1);
  words.push_back(makeInlineImageToken(id, width, height));
  wordStyles.push_back(EpdFontFamily::REGULAR);
  if (!wordFontSizes.empty()) wordFontSizes.push_back(0);
  if (!wordFontFamilies.empty()) wordFontFamilies.push_back(0);
  // Image edges are legal wrap points even with no source whitespace.
  wordContinues.push_back(false);
  wordNoSpaceBefore.push_back(attachToPrevious);
  wordFocusBoundary.push_back(0);
  wordGuideDotBefore.push_back(false);
  wordBackgroundBlack.push_back(0);
  pushVisibleOffset(visibleTextOffset);
  if (trackReferenceOffsets) wordReferenceOffsets.push_back(referenceTextOffset);
  if (!rubyTexts.empty()) rubyTexts.push_back("");
}

void ParsedText::setRubyForWordAt(size_t index, const std::string& ruby) {
  if (index >= words.size()) return;
  if (rubyTexts.size() <= index) {
    rubyTexts.resize(words.size());
  }
  rubyTexts[index] = ruby;
}

void ParsedText::setRubyGroupAt(size_t startIndex, size_t count, const std::string& ruby) {
  if (startIndex >= words.size()) return;
  if (rubyTexts.size() <= startIndex) {
    rubyTexts.resize(words.size());
  }
  rubyTexts[startIndex] = ruby;
  for (size_t i = 1; i < count; i++) {
    size_t idx = startIndex + i;
    if (idx >= words.size()) break;
    if (rubyTexts.size() <= idx) {
      rubyTexts.resize(words.size());
    }
    rubyTexts[idx] = "";
    wordStyles[idx] =
        static_cast<EpdFontFamily::Style>(static_cast<uint8_t>(wordStyles[idx]) | EpdFontFamily::RUBY_CONTINUE);
    wordContinues[idx] = true;       // Prevent the page breaker from splitting group ruby.
    wordNoSpaceBefore[idx] = false;  // Keep allowsBreak() disabled for the continuation.
  }
}

void ParsedText::ensureRubyCapacity() {
  // No-op: rubyTexts is a std::deque (chunked growth, no capacity to pre-reserve
  // and no large contiguous reallocation to avoid). Kept for call-site stability.
}

int ParsedText::resolveFirstLineIndent(const bool isFirstLine, const GfxRenderer& /*renderer*/,
                                       const int /*fontId*/) const {
  const bool naturalAlign =
      blockStyle.alignment == CssTextAlign::Justify || blockStyle.alignment == CssTextAlign::None ||
      (blockStyle.isRtl ? blockStyle.alignment == CssTextAlign::Right : blockStyle.alignment == CssTextAlign::Left);
  if (!isFirstLine || isContinuation_ || !naturalAlign) {
    return 0;
  }
  if (blockStyle.textIndentDefined) {
    if (blockStyle.textIndent < 0 || !extraParagraphSpacing || forceParagraphIndents) {
      return blockStyle.textIndent;
    }
    return 0;
  }
  return 0;
}

// Consumes data to minimize memory usage
bool ParsedText::layoutAndExtractLines(
    const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
    const std::function<void(std::shared_ptr<TextBlock>, uint32_t, uint32_t)>& processLine, const bool includeLastLine,
    const size_t maxLines) {
  if (words.empty() || maxLines == 0) {
    return true;
  }

  Arena layoutArena(psramHeapAvailable() ? ArenaBacking::PsramPreferred : ArenaBacking::Default);
  if (!layoutArena.init(LAYOUT_ARENA_SLAB_BYTES)) {
    LOG_ERR("PTX", "Failed to allocate layout scratch arena (%u bytes)",
            static_cast<unsigned>(LAYOUT_ARENA_SLAB_BYTES));
    return false;
  }

  if (!blockStyle.directionDefined && hasRtlWord) {
    const size_t wordsToScan = std::min(words.size(), RTL_PARAGRAPH_PROBE_WORDS);
    for (size_t i = 0; i < wordsToScan; ++i) {
      if (BidiUtils::startsWithRtl(words[i].c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH)) {
        blockStyle.isRtl = true;
        break;
      }
    }
  }

  // Ensure SD card font glyph metrics are loaded before measuring word widths.
  // For flash-based fonts isSdCardFont() returns false and this block is skipped
  // entirely — no heap allocation. For SD card fonts this reads glyph metadata
  // (advanceX only, no bitmaps) for all unique codepoints in this paragraph so
  // that calculateWordWidths() can measure text without on-demand SD I/O.
  if (renderer.isSdCardFont(fontId)) {
    // Style mask: only ask the SD font to load advances for styles actually
    // used in this paragraph. Style index is the low two bits (regular/bold/
    // italic/bold-italic); the underline bit is irrelevant to advance metrics.
    uint8_t styleMask = 0;
    for (size_t i = 0; i < wordStyles.size(); ++i) {
      const auto s = wordStyles[i];
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(s) & 0x03));
      if (i < wordFocusBoundary.size() && wordFocusBoundary[i] > 0) {
        const auto boldStyle = static_cast<EpdFontFamily::Style>(s | EpdFontFamily::BOLD);
        styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(boldStyle) & 0x03));
      }
    }
    if (styleMask == 0) styleMask = 0x01;  // defensive: regular only
    renderer.ensureSdCardFontReady(fontId, words, hyphenationEnabled, styleMask);
    if (!rubyTexts.empty()) {
      renderer.ensureSdCardFontReady(fontId, rubyTexts, false, 0x01);
    }
    if (guideReadingEnabled) {
      renderer.ensureSdCardFontReady(fontId, GUIDE_DOT_UTF8, 0x01);
    }
  }

  const int pageWidth = viewportWidth;
  ArenaVector<uint16_t> wordWidths(layoutArena);
  if (!calculateWordWidths(wordWidths, renderer, fontId)) {
    LOG_ERR("PTX", "OOM allocating word width scratch (%u words)", static_cast<unsigned>(words.size()));
    return false;
  }

  ArenaVector<int16_t> naturalGaps(layoutArena);
  ArenaVector<uint8_t> gapSlots(layoutArena);
  ArenaVector<size_t> lineBreakIndices(layoutArena);
  bool breaksOk = false;
  if (hyphenationEnabled || maxLines != SIZE_MAX) {
    // Bounded layout leaves words beyond the emitted lines unsplit for a later width.
    breaksOk = computeHyphenatedLineBreaks(renderer, fontId, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore,
                                           lineBreakIndices, maxLines);
    if (breaksOk) {
      breaksOk = calculateGapMetrics(naturalGaps, gapSlots, renderer, fontId);
    }
  } else {
    breaksOk = computeLineBreaks(layoutArena, renderer, fontId, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore,
                                 naturalGaps, gapSlots, lineBreakIndices);
  }
  if (!breaksOk || lineBreakIndices.empty()) {
    return false;
  }
  const bool allLinesComputed = lineBreakIndices[lineBreakIndices.size() - 1] == words.size();
  const size_t lineCount = lineBreakIndices.size() - (!includeLastLine && allLinesComputed ? 1 : 0);

  for (size_t i = 0; i < lineCount; ++i) {
    if (!extractLine(layoutArena, i, pageWidth, wordWidths, wordContinues, wordNoSpaceBefore, naturalGaps, gapSlots,
                     lineBreakIndices, processLine, renderer, fontId)) {
      return false;
    }
  }

  // Remove consumed words so size() reflects only remaining words
  if (lineCount > 0) {
    const size_t consumed = lineBreakIndices[lineCount - 1];
    words.erase(words.begin(), words.begin() + consumed);
    wordStyles.erase(wordStyles.begin(), wordStyles.begin() + consumed);
    if (!wordFontSizes.empty()) wordFontSizes.erase(wordFontSizes.begin(), wordFontSizes.begin() + consumed);
    if (!wordFontFamilies.empty())
      wordFontFamilies.erase(wordFontFamilies.begin(), wordFontFamilies.begin() + consumed);
    wordContinues.erase(wordContinues.begin(), wordContinues.begin() + consumed);
    wordNoSpaceBefore.erase(wordNoSpaceBefore.begin(), wordNoSpaceBefore.begin() + consumed);
    wordFocusBoundary.erase(wordFocusBoundary.begin(), wordFocusBoundary.begin() + consumed);
    wordGuideDotBefore.erase(wordGuideDotBefore.begin(), wordGuideDotBefore.begin() + consumed);
    wordBackgroundBlack.erase(wordBackgroundBlack.begin(), wordBackgroundBlack.begin() + consumed);
    eraseVisibleOffsetPrefix(consumed);
    eraseInlinePaddingPrefix(consumed);
    if (trackReferenceOffsets) {
      wordReferenceOffsets.erase(wordReferenceOffsets.begin(), wordReferenceOffsets.begin() + consumed);
    }
    if (!rubyTexts.empty()) {
      const size_t rtConsumed = std::min(consumed, rubyTexts.size());
      rubyTexts.erase(rubyTexts.begin(), rubyTexts.begin() + rtConsumed);
    }
  }
  if (lineCount > 0) {
    // A partial flush leaves the remaining words in this same logical
    // paragraph. Mark them so the next pass starts at the normal left edge.
    isContinuation_ = !includeLastLine || !words.empty();
  }
  return true;
}

bool ParsedText::layoutAndExtractLinesPreservingSource(
    const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
    const std::function<void(std::shared_ptr<TextBlock>)>& processLine, const bool allowCharacterBreaks) const {
  ParsedText layoutProbe(*this);
  layoutProbe.allowCharacterBreaks_ = allowCharacterBreaks;
  return layoutProbe.layoutAndExtractLines(
      renderer, fontId, viewportWidth, [&processLine](std::shared_ptr<TextBlock> line, const uint32_t, const uint32_t) {
        processLine(std::move(line));
      });
}

int ParsedText::calculateRubyExtraStartOffset(const size_t wordIdx, const size_t maxWordIdx,
                                              const GfxRenderer& renderer, const int fontId) const {
  if (rubyTexts.empty() || wordIdx >= rubyTexts.size() || rubyTexts[wordIdx].empty() ||
      (wordStyles[wordIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    return 0;
  }

  size_t groupWordCount = 1;
  while (wordIdx + groupWordCount < maxWordIdx &&
         (wordStyles[wordIdx + groupWordCount] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    groupWordCount++;
  }

  int groupActualWidth = 0;
  for (size_t k = 0; k < groupWordCount; ++k) {
    const size_t index = wordIdx + k;
    groupActualWidth += measureTokenWidth(renderer, wordFontId(renderer, fontId, index), words[index],
                                          wordStyles[index], wordFocusBoundary[index], false, characterSpacing) +
                        inlinePaddingBefore(index);
  }
  const int rubyWidth = renderer.getTextAdvanceX(wordFontId(renderer, fontId, wordIdx), rubyTexts[wordIdx].c_str(),
                                                 EpdFontFamily::SUP, 0, characterSpacing);
  return rubyWidth > groupActualWidth ? (rubyWidth - groupActualWidth) / 2 : 0;
}

int ParsedText::calculateRubyExtraEndOffset(const size_t lineStartIdx, const size_t lineBreakIdx,
                                            const GfxRenderer& renderer, const int fontId) const {
  if (rubyTexts.empty() || lineBreakIdx == 0 || lineStartIdx >= lineBreakIdx) return 0;

  size_t leaderIdx = lineBreakIdx - 1;
  while (leaderIdx > lineStartIdx && (wordStyles[leaderIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    leaderIdx--;
  }
  if (leaderIdx >= rubyTexts.size() || rubyTexts[leaderIdx].empty() ||
      (wordStyles[leaderIdx] & EpdFontFamily::RUBY_CONTINUE) != 0) {
    return 0;
  }

  int groupActualWidth = 0;
  for (size_t index = leaderIdx; index < lineBreakIdx; ++index) {
    groupActualWidth += measureTokenWidth(renderer, wordFontId(renderer, fontId, index), words[index],
                                          wordStyles[index], wordFocusBoundary[index], false, characterSpacing) +
                        inlinePaddingBefore(index);
  }
  const int rubyWidth = renderer.getTextAdvanceX(wordFontId(renderer, fontId, leaderIdx), rubyTexts[leaderIdx].c_str(),
                                                 EpdFontFamily::SUP, 0, characterSpacing);
  return rubyWidth > groupActualWidth ? (rubyWidth - groupActualWidth) / 2 : 0;
}

int ParsedText::wordFontId(const GfxRenderer& renderer, const int base, const size_t index) const {
  return renderer.resolveBookFont(base, wordFontFamilies.empty() ? 0 : wordFontFamilies[index],
                                  wordFontSizes.empty() ? 0 : wordFontSizes[index]);
}

bool ParsedText::calculateWordWidths(ArenaVector<uint16_t>& wordWidths, const GfxRenderer& renderer, const int fontId) {
  if (!wordWidths.reserve(words.size())) {
    return false;
  }

  for (size_t i = 0; i < words.size(); ++i) {
    const int width = measureTokenWidth(renderer, wordFontId(renderer, fontId, i), words[i], wordStyles[i],
                                        wordFocusBoundary[i], false, characterSpacing) +
                      inlinePaddingBefore(i);
    if (!wordWidths.push_back(static_cast<uint16_t>(std::min(width, static_cast<int>(UINT16_MAX))))) {
      return false;
    }
  }

  // Adjust widths for ruby groups to comply with JLReq standards
  if (!rubyTexts.empty()) {
    struct RubyGroupInfo {
      size_t start;
      size_t count;
      int baseWidth;
      int rubyWidth;
      int leftOverlap;
      int rightOverlap;
    };

    std::vector<RubyGroupInfo> groups;
    for (size_t i = 0; i < words.size(); ++i) {
      if (i < rubyTexts.size() && !rubyTexts[i].empty() && (wordStyles[i] & EpdFontFamily::RUBY_CONTINUE) == 0) {
        RubyGroupInfo g;
        g.start = i;
        g.baseWidth = wordWidths[i];
        g.count = 1;
        while (i + g.count < words.size() && (wordStyles[i + g.count] & EpdFontFamily::RUBY_CONTINUE) != 0) {
          g.baseWidth += wordWidths[i + g.count];
          g.count++;
        }
        g.rubyWidth = renderer.getTextAdvanceX(wordFontId(renderer, fontId, i), rubyTexts[i].c_str(),
                                               EpdFontFamily::SUP, 0, characterSpacing);
        g.leftOverlap = std::max(0, (g.rubyWidth - g.baseWidth) / 2);
        g.rightOverlap = std::max(0, (g.rubyWidth - g.baseWidth) / 2);
        groups.push_back(g);
        i += g.count - 1;
      }
    }

    // Adjust widths based on adjacent characters and group-to-group spacing
    for (size_t gIdx = 0; gIdx < groups.size(); ++gIdx) {
      const auto& g = groups[gIdx];

      // 1. Preceding character (left overhang)
      if (g.start > 0) {
        const uint32_t cpPrev = lastCodepoint(words[g.start - 1]);
        if (isCjkIdeograph(cpPrev)) {
          wordWidths[g.start - 1] += g.leftOverlap;
        } else {
          const int maxLeftOverhang = wordWidths[g.start - 1] / 2;
          wordWidths[g.start - 1] += std::max(0, g.leftOverlap - maxLeftOverhang);
        }
      }

      // 2. Succeeding character (right overhang / group collision)
      const size_t nextIdx = g.start + g.count;
      if (nextIdx < words.size()) {
        if (gIdx + 1 < groups.size() && groups[gIdx + 1].start == nextIdx) {
          // Adjacent ruby groups: compute collision
          const auto& nextG = groups[gIdx + 1];
          const int collision = g.rightOverlap + nextG.leftOverlap;
          if (collision > 0) {
            wordWidths[g.start + g.count - 1] += collision;
          }
        } else {
          // Regular character following: check if it's Kanji
          const uint32_t cpNext = firstCodepoint(words[nextIdx]);
          if (isCjkIdeograph(cpNext)) {
            wordWidths[g.start + g.count - 1] += g.rightOverlap;
          } else {
            const int maxRightOverhang = wordWidths[nextIdx] / 2;
            wordWidths[g.start + g.count - 1] += std::max(0, g.rightOverlap - maxRightOverhang);
          }

          // Check if there is another ruby group further ahead separated only by non-ideographs
          if (gIdx + 1 < groups.size()) {
            const auto& nextG = groups[gIdx + 1];
            bool onlyNonIdeographsInBetween = true;
            int gapWidth = 0;
            for (size_t k = nextIdx; k < nextG.start; ++k) {
              const uint32_t cp = firstCodepoint(words[k]);
              if (isCjkIdeograph(cp)) {
                onlyNonIdeographsInBetween = false;
                break;
              }
              gapWidth += wordWidths[k];
            }
            if (onlyNonIdeographsInBetween) {
              const int maxRightOverhang = wordWidths[g.start + g.count - 1] / 2;
              const int maxLeftOverhang = wordWidths[nextG.start - 1] / 2;
              const int allowedRight = std::min(g.rightOverlap, maxRightOverhang);
              const int allowedLeft = std::min(nextG.leftOverlap, maxLeftOverhang);
              const int touchOverlap = allowedRight + allowedLeft - gapWidth;
              if (touchOverlap > 0) {
                wordWidths[g.start + g.count - 1] += touchOverlap;
              }
            }
          }
        }
      }
    }
  }

  return true;
}

bool ParsedText::calculateGapMetrics(ArenaVector<int16_t>& naturalGaps, ArenaVector<uint8_t>& gapSlots,
                                     const GfxRenderer& renderer, const int fontId) {
  if (!naturalGaps.resize(words.size()) || !gapSlots.resize(words.size())) {
    LOG_ERR("PTX", "OOM allocating gap metric scratch (%u words)", static_cast<unsigned>(words.size()));
    return false;
  }

  if (words.empty()) {
    return true;
  }

  naturalGaps[0] = 0;
  gapSlots[0] = 0;
  for (auto& word : words) {
    if (containsSoftHyphen(word)) {
      stripSoftHyphensInPlace(word);
    }
  }
  for (size_t i = 1; i < words.size(); ++i) {
    const bool continues = wordContinues[i];
    const bool noSpaceBefore = wordNoSpaceBefore[i];
    const bool guideDotBefore = wordGuideDotBefore[i];
    naturalGaps[i] = static_cast<int16_t>(naturalGapBeforeToken(renderer, fontId, words[i - 1], words[i],
                                                                wordStyles[i - 1], continues, noSpaceBefore,
                                                                guideDotBefore, wordSpacing, characterSpacing));
    gapSlots[i] = static_cast<uint8_t>(std::min<size_t>(
        UINT8_MAX, gapSlotsBeforeToken(words[i - 1], words[i], continues, noSpaceBefore, guideDotBefore)));
  }
  return true;
}

bool ParsedText::computeLineBreaks(Arena& scratchArena, const GfxRenderer& renderer, const int fontId,
                                   const int pageWidth, ArenaVector<uint16_t>& wordWidths,
                                   std::vector<bool>& continuesVec, std::vector<bool>& noSpaceBeforeVec,
                                   ArenaVector<int16_t>& naturalGaps, ArenaVector<uint8_t>& gapSlots,
                                   ArenaVector<size_t>& lineBreakIndices) {
  if (words.empty()) {
    return false;
  }

  auto nextTokenAttaches = [&](const size_t index, const size_t totalWordCount) {
    return index + 1 < totalWordCount &&
           !TokenBoundary::allowsBreak(continuesVec[index + 1], noSpaceBeforeVec[index + 1]);
  };

  const int firstLineIndent = resolveFirstLineIndent(true, renderer, fontId);

  // Ensure any word that would overflow even as the first entry on a line is split using fallback hyphenation.
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    // First word needs to fit in reduced width if there's an indent
    const int effectiveWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;
    while (wordWidths[i] > effectiveWidth) {
      if (!hyphenateWordAtIndex(i, effectiveWidth, renderer, wordFontId(renderer, fontId, i), wordWidths,
                                /*allowFallbackBreaks=*/true)) {
        break;
      }
    }
  }

  if (!calculateGapMetrics(naturalGaps, gapSlots, renderer, fontId)) {
    return false;
  }

  const size_t totalWordCount = words.size();

  // DP table to store the minimum badness (cost) of lines starting at index i
  ArenaVector<int> dp(scratchArena);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  ArenaVector<size_t> ans(scratchArena);
  if (!dp.resize(totalWordCount) || !ans.resize(totalWordCount)) {
    LOG_ERR("PTX", "OOM allocating line-break scratch (%u words)", static_cast<unsigned>(totalWordCount));
    return false;
  }

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = 0;
    dp[i] = MAX_COST;

    // First line has reduced width due to text-indent
    const int effectivePageWidth = i == 0 ? pageWidth - firstLineIndent : pageWidth;

    for (size_t j = i; j < totalWordCount; ++j) {
      // Add space before word j, unless it's the first word on the line or a continuation
      int gap = 0;
      if (j > static_cast<size_t>(i)) {
        gap = naturalGaps[j];
      }

      const int extraStartOffset =
          j == static_cast<size_t>(i) ? calculateRubyExtraStartOffset(i, totalWordCount, renderer, fontId) : 0;

      currlen += wordWidths[j] + gap + (j == i ? extraStartOffset : 0);

      if (currlen > effectivePageWidth) {
        break;
      }

      // Cannot break after word j if the next word attaches to it (continuation group)
      if (nextTokenAttaches(j, totalWordCount)) {
        continue;
      }

      const int extraEndOffset = calculateRubyExtraEndOffset(i, j + 1, renderer, fontId);

      if (currlen + extraEndOffset > effectivePageWidth) {
        continue;  // Cannot split here as it would overflow the right margin
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;  // Last line
      } else {
        const int remainingSpace = effectivePageWidth - currlen;
        // Use long long for the square to prevent overflow
        const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      // Equal-cost alternatives should keep more CJK text on the current line.
      if (cost <= dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }

    // Handle oversized word: if no valid configuration found, force single-word line
    // This prevents cascade failure where one oversized word breaks all preceding words
    if (dp[i] == MAX_COST) {
      ans[i] = i;  // Just this word on its own line
      // Inherit cost from next word to allow subsequent words to find valid configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    if (!lineBreakIndices.push_back(nextBreakIndex)) {
      LOG_ERR("PTX", "OOM growing line-break result scratch");
      return false;
    }
    currentWordIndex = nextBreakIndex;
  }

  return !lineBreakIndices.empty();
}

// Builds break indices while opportunistically splitting the word that would overflow the current line.
bool ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                             ArenaVector<uint16_t>& wordWidths, std::vector<bool>& continuesVec,
                                             std::vector<bool>& noSpaceBeforeVec, ArenaVector<size_t>& lineBreakIndices,
                                             const size_t maxLines) {
  const int firstLineIndent = resolveFirstLineIndent(true, renderer, fontId);

  size_t currentIndex = 0;
  bool isFirstLine = true;
  auto currentTokenAttaches = [&](const size_t index) {
    return index < wordWidths.size() && !TokenBoundary::allowsBreak(continuesVec[index], noSpaceBeforeVec[index]);
  };

  while (currentIndex < wordWidths.size() && lineBreakIndices.size() < maxLines) {
    const size_t lineStart = currentIndex;
    size_t lastFittingBreak = lineStart;
    int lineWidth = calculateRubyExtraStartOffset(lineStart, wordWidths.size(), renderer, fontId);

    // First line has reduced width due to text-indent
    const int effectivePageWidth = isFirstLine ? pageWidth - firstLineIndent : pageWidth;

    // Consume as many words as possible for current line, splitting when prefixes fit
    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      int spacing = 0;
      if (!isFirstWord) {
        spacing = naturalGapBeforeToken(renderer, fontId, words[currentIndex - 1], words[currentIndex],
                                        wordStyles[currentIndex - 1], continuesVec[currentIndex],
                                        noSpaceBeforeVec[currentIndex], wordGuideDotBefore[currentIndex], wordSpacing,
                                        characterSpacing);
      }
      const int candidateWidth = spacing + wordWidths[currentIndex];

      // Word fits on current line
      const int extraEndOffset = calculateRubyExtraEndOffset(lineStart, currentIndex + 1, renderer, fontId);
      if (lineWidth + candidateWidth <= effectivePageWidth) {
        lineWidth += candidateWidth;
        ++currentIndex;
        if (lineWidth + extraEndOffset <= effectivePageWidth && !currentTokenAttaches(currentIndex)) {
          lastFittingBreak = currentIndex;
        }
        continue;
      }

      // Word would overflow — try to split based on hyphenation points
      const int availableWidth = effectivePageWidth - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;  // Only for first word on line

      if (availableWidth > 0 && (hyphenationEnabled || isFirstWord) &&
          hyphenateWordAtIndex(currentIndex, availableWidth, renderer, wordFontId(renderer, fontId, currentIndex),
                               wordWidths, allowFallbackBreaks)) {
        // Prefix now fits; append it to this line and move to next line
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        lastFittingBreak = currentIndex;
        break;
      }

      // Could not split: force at least one word per line to avoid infinite loop
      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    if (lastFittingBreak > lineStart) currentIndex = lastFittingBreak;

    // Don't break before a continuation word (e.g., orphaned "?" after "question").
    // Backtrack to the start of the continuation group so the whole group moves to the next line.
    while (currentIndex > lineStart + 1 && currentTokenAttaches(currentIndex)) {
      --currentIndex;
    }

    if (!lineBreakIndices.push_back(currentIndex)) {
      LOG_ERR("PTX", "OOM growing hyphenated line-break scratch");
      return false;
    }
    isFirstLine = false;
  }

  return !lineBreakIndices.empty();
}

// Splits words[wordIndex] into prefix (adding a hyphen only when needed) and remainder when a legal breakpoint fits the
// available width.
bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, ArenaVector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks) {
  // Guard against invalid indices or zero available width before attempting to split.
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  const std::string& word = words[wordIndex];
  uint16_t imageId, imageWidth, imageHeight;
  if (parseInlineImageToken(word.c_str(), imageId, imageWidth, imageHeight)) return false;
  const auto style = wordStyles[wordIndex];
  const int leadingPadding = inlinePaddingBefore(wordIndex);

  if (allowFallbackBreaks && isPathologicalUnbrokenToken(word)) {
    return splitTokenAtCodepointBoundary(wordIndex, availableWidth, renderer, fontId, wordWidths);
  }

  // Hangul splits at legal CJK boundaries only at a line end when hyphenation is on.
  auto breakInfos = hangulLineEndBreaks(word);
  const auto hyphenBreaks = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  breakInfos.insert(breakInfos.end(), hyphenBreaks.begin(), hyphenBreaks.end());
  if (breakInfos.empty()) {
    if (allowFallbackBreaks && allowCharacterBreaks_) {
      return splitTokenAtCodepointBoundary(wordIndex, availableWidth, renderer, fontId, wordWidths);
    }
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  // Iterate over each legal breakpoint and retain the widest prefix that still fits.
  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    std::string prefix = word.substr(0, offset);
    if (needsHyphen) {
      prefix.push_back('-');
    }
    const FocusTokenMetadata prefixFocus = computeFocusMetadata(prefix, style, focusReadingEnabled);
    const int prefixWidth =
        leadingPadding + measureTokenWidth(renderer, fontId, prefix, prefixFocus.style, prefixFocus.boundary,
                                           /*appendHyphen=*/false, characterSpacing);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;  // Skip if too wide or not an improvement
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    if (allowFallbackBreaks && allowCharacterBreaks_) {
      return splitTokenAtCodepointBoundary(wordIndex, availableWidth, renderer, fontId, wordWidths);
    }
    // No hyphenation point produced a prefix that fits in the remaining space.
    return false;
  }

  // Split the word at the selected breakpoint and append a hyphen if required.
  std::string remainder = word.substr(chosenOffset);
  uint32_t remainderOffset = visibleOffsetAt(wordIndex);
  uint32_t remainderReferenceOffset =
      trackReferenceOffsets && wordIndex < wordReferenceOffsets.size() ? wordReferenceOffsets[wordIndex] : 0;
  const auto* offsetPtr = reinterpret_cast<const unsigned char*>(word.data());
  const auto* const splitPtr = offsetPtr + chosenOffset;
  while (offsetPtr < splitPtr) {
    utf8NextCodepoint(&offsetPtr);
    remainderOffset++;
    remainderReferenceOffset++;
  }
  words[wordIndex].resize(chosenOffset);
  if (chosenNeedsHyphen) {
    words[wordIndex].push_back('-');
    wordBackgroundBlack[wordIndex] |= TextBlock::WORD_FLAG_INSERTED_HYPHEN;
  }
  const FocusTokenMetadata prefixFocus = computeFocusMetadata(words[wordIndex], style, focusReadingEnabled);
  wordStyles[wordIndex] = prefixFocus.style;
  wordFocusBoundary[wordIndex] = prefixFocus.boundary;

  // Insert the remainder word (with matching style and continuation flag) directly after the prefix.
  const FocusTokenMetadata remainderFocus = computeFocusMetadata(remainder, style, focusReadingEnabled);
  shiftInlinePaddingsAfter(wordIndex);
  words.insert(words.begin() + wordIndex + 1, remainder);
  insertVisibleOffset(wordIndex + 1, remainderOffset);
  if (trackReferenceOffsets) {
    wordReferenceOffsets.insert(wordReferenceOffsets.begin() + wordIndex + 1, remainderReferenceOffset);
  }
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, remainderFocus.style);
  if (!wordFontSizes.empty()) wordFontSizes.insert(wordFontSizes.begin() + wordIndex + 1, wordFontSizes[wordIndex]);
  if (!wordFontFamilies.empty())
    wordFontFamilies.insert(wordFontFamilies.begin() + wordIndex + 1, wordFontFamilies[wordIndex]);
  wordBackgroundBlack.insert(wordBackgroundBlack.begin() + wordIndex + 1, wordBackgroundBlack[wordIndex]);
  // The hyphen remainder starts fresh on the next line and does not inherit a virtual guide dot.
  wordFocusBoundary.insert(wordFocusBoundary.begin() + wordIndex + 1, remainderFocus.boundary);
  wordGuideDotBefore.insert(wordGuideDotBefore.begin() + wordIndex + 1, false);
  wordBackgroundBlack[wordIndex + 1] &= static_cast<uint8_t>(~TextBlock::WORD_FLAG_INSERTED_HYPHEN);
  if (wordIndex + 1 <= rubyTexts.size()) {
    rubyTexts.insert(rubyTexts.begin() + wordIndex + 1, "");
  }

  // Continuation flag handling after splitting a word into prefix + remainder.
  //
  // The prefix keeps the original word's continuation flag so that no-break-space groups
  // stay linked. The remainder always gets continues=false because it starts on the next
  // line and is not attached to the prefix.
  //
  // Example: "200&#xA0;Quadratkilometer" produces tokens:
  //   [0] "200"               continues=false
  //   [1] " "                 continues=true
  //   [2] "Quadratkilometer"  continues=true   <-- the word being split
  //
  // After splitting "Quadratkilometer" at "Quadrat-" / "kilometer":
  //   [0] "200"         continues=false
  //   [1] " "           continues=true
  //   [2] "Quadrat-"    continues=true   (KEPT — still attached to the no-break group)
  //   [3] "kilometer"   continues=false  (NEW — starts fresh on the next line)
  //
  // This lets the backtracking loop keep the entire prefix group ("200 Quadrat-") on one
  // line, while "kilometer" moves to the next line.
  // wordContinues[wordIndex] is intentionally left unchanged — the prefix keeps its original attachment.
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, false);
  wordNoSpaceBefore.insert(wordNoSpaceBefore.begin() + wordIndex + 1, false);
  if (!rubyTexts.empty()) {
    rubyTexts.insert(rubyTexts.begin() + wordIndex + 1, "");
  }

  // Update cached widths to reflect the new prefix/remainder pairing.
  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = measureTokenWidth(renderer, fontId, remainder, remainderFocus.style,
                                                    remainderFocus.boundary, false, characterSpacing);
  if (!wordWidths.insert(wordIndex + 1, remainderWidth)) {
    LOG_ERR("PTX", "OOM inserting hyphenated word width");
    return false;
  }
  return true;
}

bool ParsedText::splitTokenAtCodepointBoundary(const size_t wordIndex, const int availableWidth,
                                               const GfxRenderer& renderer, const int fontId,
                                               ArenaVector<uint16_t>& wordWidths) {
  if (availableWidth <= 0 || wordIndex >= words.size() || wordIndex >= wordWidths.size()) {
    return false;
  }

  const std::string& word = words[wordIndex];
  uint16_t imageId, imageWidth, imageHeight;
  if (parseInlineImageToken(word.c_str(), imageId, imageWidth, imageHeight)) return false;
  if (word.size() < 2) {
    return false;
  }

  const auto style = wordStyles[wordIndex];
  const int leadingPadding = inlinePaddingBefore(wordIndex);
  size_t chosenOffset = 0;
  int chosenWidth = -1;
  std::string prefix;
  prefix.reserve(std::min(word.size(), PATHOLOGICAL_TOKEN_SCAN_BYTES));
  const auto* const wordStart = reinterpret_cast<const unsigned char*>(word.data());
  const auto* cursor = wordStart;
  const auto* const wordEnd = wordStart + word.size();
  while (cursor < wordEnd) {
    const auto* next = cursor;
    if (utf8NextCodepoint(&next) == 0 || next <= cursor || next >= wordEnd) break;

    // Keep combining marks and variation selectors attached to their base codepoint.
    const auto* following = next;
    const uint32_t followingCp = utf8NextCodepoint(&following);
    if (utf8IsCombiningMark(followingCp) || utf8IsVariationSelector(followingCp)) {
      cursor = next;
      continue;
    }

    const size_t candidateOffset = static_cast<size_t>(next - wordStart);
    prefix.assign(word.data(), candidateOffset);
    const FocusTokenMetadata prefixFocus = computeFocusMetadata(prefix, style, focusReadingEnabled);
    const int prefixWidth = leadingPadding + measureTokenWidth(renderer, fontId, prefix, prefixFocus.style,
                                                               prefixFocus.boundary, false, characterSpacing);
    if (prefixWidth > availableWidth) {
      // Prefix widths grow with the prefix, so every longer candidate is also
      // too wide. Stop here instead of measuring the rest of a huge token.
      break;
    }
    chosenOffset = candidateOffset;
    chosenWidth = prefixWidth;
    cursor = next;
  }

  if (chosenOffset == 0) {
    return false;
  }

  std::string remainder = word.substr(chosenOffset);
  uint32_t remainderOffset = visibleOffsetAt(wordIndex);
  uint32_t remainderReferenceOffset =
      trackReferenceOffsets && wordIndex < wordReferenceOffsets.size() ? wordReferenceOffsets[wordIndex] : 0;
  const auto* offsetPtr = reinterpret_cast<const unsigned char*>(word.data());
  const auto* const splitPtr = offsetPtr + chosenOffset;
  while (offsetPtr < splitPtr) {
    utf8NextCodepoint(&offsetPtr);
    remainderOffset++;
    remainderReferenceOffset++;
  }
  words[wordIndex].resize(chosenOffset);
  const FocusTokenMetadata prefixFocus = computeFocusMetadata(words[wordIndex], style, focusReadingEnabled);
  wordStyles[wordIndex] = prefixFocus.style;
  wordFocusBoundary[wordIndex] = prefixFocus.boundary;
  const FocusTokenMetadata remainderFocus = computeFocusMetadata(remainder, style, focusReadingEnabled);
  reserveTokenCapacity(1);
  shiftInlinePaddingsAfter(wordIndex);
  words.insert(words.begin() + wordIndex + 1, remainder);
  insertVisibleOffset(wordIndex + 1, remainderOffset);
  if (trackReferenceOffsets) {
    wordReferenceOffsets.insert(wordReferenceOffsets.begin() + wordIndex + 1, remainderReferenceOffset);
  }
  wordStyles.insert(wordStyles.begin() + wordIndex + 1, remainderFocus.style);
  if (!wordFontSizes.empty()) wordFontSizes.insert(wordFontSizes.begin() + wordIndex + 1, wordFontSizes[wordIndex]);
  if (!wordFontFamilies.empty())
    wordFontFamilies.insert(wordFontFamilies.begin() + wordIndex + 1, wordFontFamilies[wordIndex]);
  wordBackgroundBlack.insert(wordBackgroundBlack.begin() + wordIndex + 1, wordBackgroundBlack[wordIndex]);
  wordFocusBoundary.insert(wordFocusBoundary.begin() + wordIndex + 1, remainderFocus.boundary);
  wordGuideDotBefore.insert(wordGuideDotBefore.begin() + wordIndex + 1, false);
  wordBackgroundBlack[wordIndex + 1] &= static_cast<uint8_t>(~TextBlock::WORD_FLAG_INSERTED_HYPHEN);
  wordContinues.insert(wordContinues.begin() + wordIndex + 1, false);
  wordNoSpaceBefore.insert(wordNoSpaceBefore.begin() + wordIndex + 1, true);

  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = remainder.size() >= PATHOLOGICAL_TOKEN_MIN_BYTES
                                      ? std::numeric_limits<uint16_t>::max()
                                      : measureTokenWidth(renderer, fontId, remainder, remainderFocus.style,
                                                          remainderFocus.boundary, false, characterSpacing);
  if (!wordWidths.insert(wordIndex + 1, remainderWidth)) {
    LOG_ERR("PTX", "OOM inserting split token width");
    return false;
  }
  return true;
}

bool ParsedText::extractLine(Arena& scratchArena, const size_t breakIndex, const int pageWidth,
                             const ArenaVector<uint16_t>& wordWidths, const std::vector<bool>& continuesVec,
                             const std::vector<bool>& noSpaceBeforeVec, const ArenaVector<int16_t>& naturalGaps,
                             const ArenaVector<uint8_t>& gapSlots, const ArenaVector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>, uint32_t, uint32_t)>& processLine,
                             const GfxRenderer& renderer, const int fontId) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const uint32_t lineVisibleOffset = visibleOffsetAt(lastBreakAt);
  const uint32_t lineReferenceOffset =
      trackReferenceOffsets && lastBreakAt < wordReferenceOffsets.size() ? wordReferenceOffsets[lastBreakAt] : 0;
  size_t lineWordCount = lineBreak - lastBreakAt;

  const int firstLineIndent = resolveFirstLineIndent(breakIndex == 0, renderer, fontId);

  auto& lineWords = lineWordsScratch;
  auto& lineWordStyles = lineStylesScratch;
  auto& lineWordWidths = lineWidthsScratch;
  auto& lineFocusBoundary = lineFocusBoundaryScratch;
  auto& lineGuideDotBefore = lineGuideDotBeforeScratch;
  auto& lineBackgroundBlack = lineBackgroundBlackScratch;
  auto& lineLeadingPadding = lineLeadingPaddingScratch;
  lineWords.clear();
  lineWordStyles.clear();
  lineWordWidths.clear();
  lineFocusBoundary.clear();
  lineGuideDotBefore.clear();
  lineBackgroundBlack.clear();
  lineLeadingPadding.clear();
  std::vector<std::string> lineRubyTexts;
  if (!rubyTexts.empty() && lastBreakAt < rubyTexts.size()) {
    lineRubyTexts.resize(lineWordCount);
    const size_t copyCount = std::min(lineBreak, rubyTexts.size()) - lastBreakAt;
    std::copy(rubyTexts.begin() + lastBreakAt, rubyTexts.begin() + lastBreakAt + copyCount, lineRubyTexts.begin());
  }
  const int extraStartOffset = calculateRubyExtraStartOffset(lastBreakAt, lineBreak, renderer, fontId);
  const int extraEndOffset = calculateRubyExtraEndOffset(lastBreakAt, lineBreak, renderer, fontId);
  lineWords.reserve(lineWordCount);
  lineWordStyles.reserve(lineWordCount);
  lineWordWidths.reserve(lineWordCount);
  lineFocusBoundary.reserve(lineWordCount);
  lineGuideDotBefore.reserve(lineWordCount);
  lineBackgroundBlack.reserve(lineWordCount);
  lineLeadingPadding.reserve(lineWordCount);

  for (size_t i = 0; i < lineWordCount; ++i) {
    const size_t sourceIndex = lastBreakAt + i;
    std::string word = std::move(words[sourceIndex]);
    lineWords.push_back(std::move(word));
    lineWordStyles.push_back(wordStyles[sourceIndex]);
    lineWordWidths.push_back(wordWidths[sourceIndex]);
    lineFocusBoundary.push_back(wordFocusBoundary[sourceIndex]);
    if (lineFocusBoundary.back() >= lineWords.back().size()) {
      lineFocusBoundary.back() = 0;
    }
    lineGuideDotBefore.push_back(i > 0 && wordGuideDotBefore[sourceIndex]);
    lineBackgroundBlack.push_back(wordBackgroundBlack[sourceIndex]);
    lineLeadingPadding.push_back(inlinePaddingBefore(sourceIndex));
  }

  // Calculate total word width, count spacing slots, and accumulate natural gaps.
  int lineWordWidthSum = 0;
  size_t actualGapCount = 0;
  int totalNaturalGaps = 0;

  for (size_t wordIdx = 0; wordIdx < lineWordCount; wordIdx++) {
    lineWordWidthSum += lineWordWidths[wordIdx];
    if (wordIdx > 0) {
      const size_t sourceIndex = lastBreakAt + wordIdx;
      actualGapCount += gapSlots[sourceIndex];
      totalNaturalGaps += naturalGaps[sourceIndex];
    }
  }

  // Calculate spacing (account for indent reducing effective page width on first line)
  const int effectivePageWidth = pageWidth - firstLineIndent;
  const bool isLastLine = lineBreak == words.size();
  const CssTextAlign effectiveAlignment =
      (blockStyle.isRtl && !blockStyle.textAlignDefined && blockStyle.alignment == CssTextAlign::Left)
          ? CssTextAlign::Right
          : blockStyle.alignment;

  // Keep the visual overhang of edge ruby groups inside the page margins.
  const int spareSpace = effectivePageWidth - extraStartOffset - extraEndOffset - lineWordWidthSum - totalNaturalGaps;
  const int justifyExtra = (effectiveAlignment == CssTextAlign::Justify && !isLastLine)
                               ? computeJustifyExtra(spareSpace, actualGapCount)
                               : 0;

  visualOrderScratch.clear();
  visualOrderScratch.reserve(lineWordCount);
  const bool shouldResolveVisualOrder = blockStyle.isRtl || hasRtlWord;
  const bool willReorder =
      shouldResolveVisualOrder && BidiUtils::computeVisualWordOrder(lineWords, blockStyle.isRtl, visualOrderScratch);

  ArenaVector<int16_t> lineXPos(scratchArena);
  if (!lineXPos.reserve(lineWordCount)) {
    LOG_ERR("PTX", "OOM allocating line x-position scratch (%u words)", static_cast<unsigned>(lineWordCount));
    return false;
  }
  int activeJustifyExtra = justifyExtra;

  if (willReorder) {
    ArenaVector<uint16_t> reorderedWidths(scratchArena);
    if (!reorderedWidths.reserve(visualOrderScratch.size())) {
      LOG_ERR("PTX", "OOM allocating reordered word-width scratch (%u words)",
              static_cast<unsigned>(visualOrderScratch.size()));
      return false;
    }
    std::vector<std::string> reorderedRubyTexts;
    if (!lineRubyTexts.empty()) reorderedRubyTexts.reserve(visualOrderScratch.size());
    reorderedWordsScratch.clear();
    reorderedStylesScratch.clear();
    reorderedContinuesScratch.clear();
    reorderedNoSpaceBeforeScratch.clear();
    reorderedFocusBoundaryScratch.clear();
    reorderedGuideDotBeforeScratch.clear();
    reorderedBackgroundBlackScratch.clear();
    reorderedLeadingPaddingScratch.clear();
    reorderedWordsScratch.reserve(visualOrderScratch.size());
    reorderedStylesScratch.reserve(visualOrderScratch.size());
    reorderedContinuesScratch.reserve(visualOrderScratch.size());
    reorderedNoSpaceBeforeScratch.reserve(visualOrderScratch.size());
    reorderedFocusBoundaryScratch.reserve(visualOrderScratch.size());
    reorderedGuideDotBeforeScratch.reserve(visualOrderScratch.size());
    reorderedBackgroundBlackScratch.reserve(visualOrderScratch.size());
    reorderedLeadingPaddingScratch.reserve(visualOrderScratch.size());

    for (size_t i = 0; i < visualOrderScratch.size(); ++i) {
      const uint16_t src = visualOrderScratch[i];
      reorderedWordsScratch.push_back(std::move(lineWords[src]));
      reorderedStylesScratch.push_back(lineWordStyles[src]);
      if (!reorderedWidths.push_back(lineWordWidths[src])) {
        LOG_ERR("PTX", "OOM growing reordered word-width scratch");
        return false;
      }
      reorderedFocusBoundaryScratch.push_back(lineFocusBoundary[src]);
      reorderedBackgroundBlackScratch.push_back(lineBackgroundBlack[src]);
      reorderedLeadingPaddingScratch.push_back(lineLeadingPadding[src]);
      if (!lineRubyTexts.empty()) reorderedRubyTexts.push_back(std::move(lineRubyTexts[src]));

      bool continues = false;
      bool guideDotBefore = false;
      if (i > 0) {
        const size_t prevSrc = visualOrderScratch[i - 1];
        const size_t currSrc = src;
        const bool forwardAdjacent = currSrc == prevSrc + 1;
        const bool reverseAdjacent = prevSrc == currSrc + 1;

        if (forwardAdjacent && continuesVec[lastBreakAt + currSrc]) {
          continues = true;
        } else if (reverseAdjacent && continuesVec[lastBreakAt + prevSrc]) {
          continues = true;
        }

        guideDotBefore = forwardAdjacent && lineGuideDotBefore[currSrc];
      }
      reorderedContinuesScratch.push_back(continues);
      reorderedNoSpaceBeforeScratch.push_back(!continues && noSpaceBeforeVec[lastBreakAt + src]);
      reorderedGuideDotBeforeScratch.push_back(!continues && guideDotBefore);
    }

    int reorderedWordWidthSum = 0;
    size_t reorderedGapCount = 0;
    int reorderedNaturalGaps = 0;
    for (size_t wordIdx = 0; wordIdx < reorderedWidths.size(); ++wordIdx) {
      reorderedWordWidthSum += reorderedWidths[wordIdx];
      if (wordIdx > 0) {
        reorderedGapCount += gapSlotsBeforeToken(
            reorderedWordsScratch[wordIdx - 1], reorderedWordsScratch[wordIdx], reorderedContinuesScratch[wordIdx],
            reorderedNoSpaceBeforeScratch[wordIdx], reorderedGuideDotBeforeScratch[wordIdx]);
        reorderedNaturalGaps +=
            naturalGapBeforeToken(renderer, fontId, reorderedWordsScratch[wordIdx - 1], reorderedWordsScratch[wordIdx],
                                  reorderedStylesScratch[wordIdx - 1], reorderedContinuesScratch[wordIdx],
                                  reorderedNoSpaceBeforeScratch[wordIdx], reorderedGuideDotBeforeScratch[wordIdx],
                                  wordSpacing, characterSpacing);
      }
    }

    const int reorderedSpare =
        effectivePageWidth - extraStartOffset - extraEndOffset - reorderedWordWidthSum - reorderedNaturalGaps;
    const int reorderedJustifyExtra = (effectiveAlignment == CssTextAlign::Justify && !isLastLine)
                                          ? computeJustifyExtra(reorderedSpare, reorderedGapCount)
                                          : 0;
    activeJustifyExtra = reorderedJustifyExtra;
    const int justifyContribution = (effectiveAlignment == CssTextAlign::Justify && !isLastLine)
                                        ? reorderedJustifyExtra * static_cast<int>(reorderedGapCount)
                                        : 0;
    const int contentWidth = reorderedWordWidthSum + reorderedNaturalGaps + justifyContribution;

    int xpos = 0;
    if (blockStyle.isRtl) {
      if (effectiveAlignment == CssTextAlign::Right || effectiveAlignment == CssTextAlign::Justify) {
        xpos = effectivePageWidth - contentWidth;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - contentWidth) / 2;
      }
    } else {
      xpos = firstLineIndent;
      if (effectiveAlignment == CssTextAlign::Right) {
        xpos = effectivePageWidth - contentWidth;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - contentWidth) / 2;
      }
    }

    for (size_t wordIdx = 0; wordIdx < reorderedWidths.size(); ++wordIdx) {
      if (!lineXPos.push_back(static_cast<int16_t>(xpos + reorderedLeadingPaddingScratch[wordIdx]))) {
        LOG_ERR("PTX", "OOM growing RTL line x-position scratch");
        return false;
      }
      xpos += reorderedWidths[wordIdx];

      if (wordIdx + 1 < reorderedWidths.size()) {
        const bool nextContinues = reorderedContinuesScratch[wordIdx + 1];
        const bool nextNoSpace = reorderedNoSpaceBeforeScratch[wordIdx + 1];
        const bool nextGuideDot = reorderedGuideDotBeforeScratch[wordIdx + 1];
        int gap = naturalGapBeforeToken(renderer, fontId, reorderedWordsScratch[wordIdx],
                                        reorderedWordsScratch[wordIdx + 1], reorderedStylesScratch[wordIdx],
                                        nextContinues, nextNoSpace, nextGuideDot, wordSpacing, characterSpacing);
        gap += reorderedJustifyExtra *
               static_cast<int>(gapSlotsBeforeToken(reorderedWordsScratch[wordIdx], reorderedWordsScratch[wordIdx + 1],
                                                    nextContinues, nextNoSpace, nextGuideDot));
        xpos += gap;
      }
    }

    lineWords.swap(reorderedWordsScratch);
    lineWordStyles.swap(reorderedStylesScratch);
    lineFocusBoundary.swap(reorderedFocusBoundaryScratch);
    lineGuideDotBefore.swap(reorderedGuideDotBeforeScratch);
    lineBackgroundBlack.swap(reorderedBackgroundBlackScratch);
    lineLeadingPadding.swap(reorderedLeadingPaddingScratch);
    if (!lineRubyTexts.empty()) lineRubyTexts.swap(reorderedRubyTexts);
    lineWordCount = lineWords.size();
  } else {
    // Standard LTR/RTL positioning loop when no visual reordering is needed
    if (blockStyle.isRtl) {
      // RTL: position words from right to left
      int xpos = effectivePageWidth;
      if (effectiveAlignment == CssTextAlign::Left) {
        // Explicit left alignment in RTL context
        xpos = lineWordWidthSum + totalNaturalGaps;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth + lineWordWidthSum + totalNaturalGaps) / 2;
      }
      // For Right and Justify, start from right edge (xpos = effectivePageWidth)

      for (size_t wordIdx = 0; wordIdx < lineWordCount; ++wordIdx) {
        xpos -= lineWordWidths[wordIdx];
        if (!lineXPos.push_back(static_cast<int16_t>(xpos + lineLeadingPadding[wordIdx]))) {
          LOG_ERR("PTX", "OOM growing line x-position scratch");
          return false;
        }

        if (wordIdx + 1 < lineWordCount) {
          const size_t nextSourceIndex = lastBreakAt + wordIdx + 1;
          int gap = naturalGaps[nextSourceIndex];
          gap += justifyExtra * static_cast<int>(gapSlots[nextSourceIndex]);
          xpos -= gap;
        }
      }
    } else {
      // LTR: position words from left to right
      int xpos = firstLineIndent + extraStartOffset;
      if (effectiveAlignment == CssTextAlign::Right) {
        xpos = effectivePageWidth - lineWordWidthSum - totalNaturalGaps;
      } else if (effectiveAlignment == CssTextAlign::Center) {
        xpos = (effectivePageWidth - lineWordWidthSum - totalNaturalGaps) / 2;
      }

      for (size_t wordIdx = 0; wordIdx < lineWordCount; ++wordIdx) {
        if (!lineXPos.push_back(static_cast<int16_t>(xpos + lineLeadingPadding[wordIdx]))) {
          LOG_ERR("PTX", "OOM growing line x-position scratch");
          return false;
        }

        int gap = 0;
        if (wordIdx + 1 < lineWordCount) {
          const size_t nextSourceIndex = lastBreakAt + wordIdx + 1;
          gap = naturalGaps[nextSourceIndex];
          gap += justifyExtra * static_cast<int>(gapSlots[nextSourceIndex]);
        }
        if (wordIdx + 1 < lineWordCount) {
          xpos += lineWordWidths[wordIdx] + gap;
        }
      }
    }
  }

  bool lineHasFocusSplit = false;
  bool lineHasGuideDot = false;
  bool lineHasBackgroundFlags = false;
  for (size_t i = 0; i < lineWordCount; i++) {
    if (lineFocusBoundary[i] > 0) {
      lineHasFocusSplit = true;
    }
    if (i > 0 && lineGuideDotBefore[i]) {
      lineHasGuideDot = true;
    }
    if (lineBackgroundBlack[i] != 0) {
      lineHasBackgroundFlags = true;
    }
    if (lineHasFocusSplit && lineHasGuideDot && lineHasBackgroundFlags) {
      break;
    }
  }

  std::vector<uint32_t> outFontFamilies;
  if (!wordFontFamilies.empty()) outFontFamilies.reserve(lineWordCount);
  std::vector<uint8_t> outFontSizes;
  if (!wordFontSizes.empty()) outFontSizes.reserve(lineWordCount);
  std::vector<std::string> outWords;
  std::vector<int16_t> outXPos;
  std::vector<EpdFontFamily::Style> outStyles;
  std::vector<uint8_t> outBoundaries;
  std::vector<uint16_t> outRunOffsets;
  std::vector<uint16_t> outGuideDotXOffset;
  std::vector<uint8_t> outBackgroundBlack;
  std::vector<std::string> outRubyTexts;
  lineImagesScratch.clear();
  auto& outHasSpaceBefore = lineHasSpaceBeforeScratch;
  outWords.reserve(lineWordCount);
  outXPos.reserve(lineWordCount);
  outStyles.reserve(lineWordCount);
  outHasSpaceBefore.clear();
  outHasSpaceBefore.reserve(lineWordCount);
  if (lineHasFocusSplit) {
    outBoundaries.reserve(lineWordCount);
    outRunOffsets.reserve(lineWordCount);
  }
  if (lineHasGuideDot) {
    outGuideDotXOffset.reserve(lineWordCount);
  }
  if (lineHasBackgroundFlags) {
    outBackgroundBlack.reserve(lineWordCount);
  }

  for (size_t i = 0; i < lineWordCount; i++) {
    uint16_t imageId, imageWidth, imageHeight;
    if (parseInlineImageToken(lineWords[i].c_str(), imageId, imageWidth, imageHeight)) {
      lineImagesScratch.push_back({imageId, lineXPos[i], imageHeight});
      continue;
    }
    const size_t sourceIndex = lastBreakAt + (willReorder ? visualOrderScratch[i] : i);
    const int tokenFontId = wordFontId(renderer, fontId, sourceIndex);
    if (!wordFontSizes.empty()) outFontSizes.push_back(wordFontSizes[sourceIndex]);
    if (!wordFontFamilies.empty()) outFontFamilies.push_back(wordFontFamilies[sourceIndex]);
    const uint8_t boundary = lineFocusBoundary[i];
    const bool wordIsRtl = BidiUtils::detectParagraphLevel(lineWords[i].c_str(), blockStyle.isRtl ? 1 : 0) ==
                           static_cast<int>(BidiUtils::BidiBaseDir::RTL);
    const uint16_t runOffset =
        boundary > 0 ? static_cast<uint16_t>(
                           std::max(0, measureFocusRunOffset(renderer, tokenFontId, lineWords[i], lineWordStyles[i],
                                                             boundary, wordIsRtl, characterSpacing)))
                     : 0;

    outWords.push_back(std::move(lineWords[i]));
    if (!lineRubyTexts.empty()) outRubyTexts.push_back(std::move(lineRubyTexts[i]));
    outXPos.push_back(lineXPos[i]);
    outStyles.push_back(lineWordStyles[i]);
    const bool continues = willReorder ? reorderedContinuesScratch[i] : wordContinues[lastBreakAt + i];
    const bool noSpaceBefore = willReorder ? reorderedNoSpaceBeforeScratch[i] : wordNoSpaceBefore[lastBreakAt + i];
    outHasSpaceBefore.push_back(!continues && !noSpaceBefore);
    if (lineHasFocusSplit) {
      outBoundaries.push_back(boundary);
      outRunOffsets.push_back(runOffset);
    }
    if (lineHasGuideDot) {
      outGuideDotXOffset.push_back(0);
      if (outWords.size() > 1 && lineGuideDotBefore[i]) {
        const int wordSpacingSecondHalf =
            guideDotWordSpacingExtra(renderer, fontId, outWords[outWords.size() - 2], outWords.back(),
                                     outStyles[outStyles.size() - 2], wordSpacing) /
            2;
        const int secondGap =
            guideDotSecondGap(renderer, fontId, outWords.back()) +
            (isClosingPunctuationForJustify(firstCodepoint(outWords.back())) ? 0 : activeJustifyExtra) +
            wordSpacingSecondHalf;
        const int dotX = static_cast<int>(lineXPos[i]) - secondGap -
                         renderer.getTextAdvanceX(fontId, GUIDE_DOT_UTF8, EpdFontFamily::REGULAR);
        const int dotDelta = dotX - static_cast<int>(outXPos[outXPos.size() - 2]);
        outGuideDotXOffset[outGuideDotXOffset.size() - 2] = static_cast<uint16_t>(dotDelta > 0 ? dotDelta : 0);
      }
    }
    if (lineHasBackgroundFlags) {
      outBackgroundBlack.push_back(lineBackgroundBlack[i]);
    }
  }

  auto block =
      std::make_shared<TextBlock>(outWords, outXPos, outStyles, outBoundaries, outRunOffsets, outGuideDotXOffset,
                                  outBackgroundBlack, outHasSpaceBefore, blockStyle, std::move(outRubyTexts),
                                  outFontSizes, initialLetter, characterSpacing, outFontFamilies);
  initialLetter[0] = '\0';
  if (!block->valid()) {
    LOG_ERR("PTX", "Dropping line: TextBlock arena allocation failed");
    return false;
  }
  processLine(std::move(block), lineVisibleOffset, lineReferenceOffset);
  return true;
}
