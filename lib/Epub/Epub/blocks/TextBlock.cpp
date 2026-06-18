#include "TextBlock.h"

#include <BidiUtils.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr uint16_t MAX_WORDS_PER_TEXT_BLOCK = 512;
constexpr uint32_t MAX_SERIALIZED_WORD_BYTES = 4096;
constexpr uint32_t SERIALIZED_TEXT_BLOCK_TAIL_BYTES =
    sizeof(EpdFontFamily::Style) + sizeof(bool) + sizeof(int16_t) * 7 + sizeof(bool) + sizeof(bool) + sizeof(bool);
constexpr uint32_t SERIALIZED_MIN_WORD_METADATA_BYTES =
    sizeof(uint32_t) + sizeof(int16_t) + sizeof(EpdFontFamily::Style) + sizeof(uint8_t);
constexpr uint32_t SERIALIZED_POST_WORD_MIN_METADATA_BYTES =
    sizeof(int16_t) + sizeof(EpdFontFamily::Style) + sizeof(uint8_t);

uint16_t measureBackgroundWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                                const EpdFontFamily::Style style) {
  if (word.size() == 1 && word[0] == ' ') {
    return renderer.getSpaceWidth(fontId, style);
  }
  return static_cast<uint16_t>(std::max(0, renderer.getTextAdvanceX(fontId, word.c_str(), style)));
}

bool isWhitespaceOnlyBackgroundToken(const std::string& word) {
  if (word.empty()) {
    return false;
  }

  for (size_t i = 0; i < word.size();) {
    const auto c = static_cast<uint8_t>(word[i]);
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t') {
      ++i;
      continue;
    }
    if (c == 0xC2 && i + 1 < word.size() && static_cast<uint8_t>(word[i + 1]) == 0xA0) {
      i += 2;
      continue;
    }
    if (c == 0xE2 && i + 2 < word.size() && static_cast<uint8_t>(word[i + 1]) == 0x80 &&
        static_cast<uint8_t>(word[i + 2]) == 0xAF) {
      i += 3;
      continue;
    }
    return false;
  }

  return true;
}

bool readBoundedString(FsFile& file, std::string& s) {
  uint32_t len = 0;
  if (!serialization::tryReadPod(file, len)) {
    LOG_ERR("TXB", "Deserialization failed: could not read word length");
    return false;
  }
  if (len > MAX_SERIALIZED_WORD_BYTES) {
    LOG_ERR("TXB", "Deserialization failed: word length %lu exceeds maximum", static_cast<unsigned long>(len));
    return false;
  }

  const int remaining = file.available();
  if (remaining < 0 || static_cast<uint32_t>(remaining) < len) {
    LOG_ERR("TXB", "Deserialization failed: truncated word payload (%lu bytes requested, %d available)",
            static_cast<unsigned long>(len), remaining);
    return false;
  }

  if (len == 0) {
    s.clear();
    return true;
  }

  s.resize(len);
  if (file.read(&s[0], len) != static_cast<int>(len)) {
    LOG_ERR("TXB", "Deserialization failed: could not read %lu-byte word payload", static_cast<unsigned long>(len));
    return false;
  }
  return true;
}

}  // namespace

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y,
                       const bool foregroundBlack) const {
  // Validate iterator bounds before rendering
  const bool hasBionic = !wordBionicBoundary.empty();
  const bool hasGuideDots = !wordGuideDotXOffset.empty();
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() ||
      words.size() != wordBackgroundBlack.size() ||
      (hasBionic && (words.size() != wordBionicBoundary.size() || words.size() != wordBionicSuffixX.size())) ||
      (!hasBionic && !wordBionicSuffixX.empty()) || (hasGuideDots && words.size() != wordGuideDotXOffset.size())) {
    LOG_ERR("TXB",
            "Render skipped: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, suffixX=%u, dotX=%u, bg=%u)\n",
            (uint32_t)words.size(), (uint32_t)wordXpos.size(), (uint32_t)wordStyles.size(),
            (uint32_t)wordBionicBoundary.size(), (uint32_t)wordBionicSuffixX.size(),
            (uint32_t)wordGuideDotXOffset.size(), (uint32_t)wordBackgroundBlack.size());
    return;
  }

  const bool scanning = renderer.isFontCacheScanning();
  const int ascender = renderer.getFontAscenderSize(fontId);
  for (size_t i = 0; i < words.size(); i++) {
    const int wordX = wordXpos[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyles[i];
    const uint8_t boundary = hasBionic ? wordBionicBoundary[i] : 0;
    const auto baseDir = static_cast<BidiUtils::BidiBaseDir>(
        BidiUtils::detectParagraphLevel(words[i].c_str(), blockStyle.isRtl ? 1 : 0));

    if (wordBackgroundBlack[i] != 0 && isWhitespaceOnlyBackgroundToken(words[i])) {
      const uint16_t backgroundWidth = measureBackgroundWidth(renderer, fontId, words[i], currentStyle);
      if (backgroundWidth > 0) {
        renderer.fillRect(wordX, y, backgroundWidth, renderer.getFontAscenderSize(fontId), true);
      }
    }

    // SUP/SUB shift the baseline passed to drawText; the glyph is also scaled 50% inside
    // drawText, so these offsets are chosen relative to the full-size ascender.
    int wordY = y;
    if ((currentStyle & EpdFontFamily::SUP) != 0) {
      wordY -= ascender * 2 / 5;
    } else if ((currentStyle & EpdFontFamily::SUB) != 0) {
      wordY += ascender / 4;
    }

    if (boundary > 0) {
      // Bionic split: draw bold prefix (max 9 codepoints = 36 UTF-8 bytes + null).
      // suffixX is pre-computed at cache creation time to avoid font metric lookups at render time.
      const auto boldStyle = static_cast<EpdFontFamily::Style>(currentStyle | EpdFontFamily::BOLD);
      char boldBuf[40];
      const size_t boldLen = std::min<size_t>({static_cast<size_t>(boundary), words[i].size(), sizeof(boldBuf) - 1});
      memcpy(boldBuf, words[i].c_str(), boldLen);
      boldBuf[boldLen] = '\0';
      renderer.drawText(fontId, wordX, wordY, boldBuf, foregroundBlack, boldStyle, baseDir);
      const int suffixX = wordX + wordBionicSuffixX[i];
      renderer.drawText(fontId, suffixX, wordY, words[i].c_str() + boldLen, foregroundBlack, currentStyle, baseDir);
    } else {
      renderer.drawText(fontId, wordX, wordY, words[i].c_str(), foregroundBlack, currentStyle, baseDir);
    }

    if (hasGuideDots && wordGuideDotXOffset[i] > 0) {
      renderer.drawText(fontId, wordX + wordGuideDotXOffset[i], wordY, "\xc2\xb7", foregroundBlack,
                        EpdFontFamily::REGULAR, baseDir);
    }

    if (!scanning && (currentStyle & EpdFontFamily::UNDERLINE) != 0) {
      const std::string& w = words[i];
      int startX = wordX;
      int underlineWidth = renderer.getTextWidth(fontId, w.c_str(), currentStyle, baseDir);
      const int underlineY = wordY + ascender + 2;

      // Skip em-space prefix so the underline starts under visible glyphs.
      if (w.size() >= 3 && static_cast<uint8_t>(w[0]) == 0xE2 && static_cast<uint8_t>(w[1]) == 0x80 &&
          static_cast<uint8_t>(w[2]) == 0x83) {
        const char* visiblePtr = w.c_str() + 3;
        const int prefixWidth = renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", currentStyle);
        const int visibleWidth = renderer.getTextWidth(fontId, visiblePtr, currentStyle, baseDir);
        startX = wordX + prefixWidth;
        underlineWidth = visibleWidth;
      }

      if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
        underlineWidth = (underlineWidth + 1) / 2;
      }

      renderer.drawLine(startX, underlineY, startX + underlineWidth, underlineY, 3, foregroundBlack);
    }

    if ((currentStyle & EpdFontFamily::STRIKETHROUGH) != 0) {
      const std::string& w = words[i];
      const int fullWordWidth = renderer.getTextWidth(fontId, w.c_str(), currentStyle, baseDir);
      // Position at roughly mid-glyph height. Offset down from the half-ascender
      // point to align with the visual centre of lowercase letters.
      // Added a 6 pixel offset after testing on various fonts to improve the visual alignment of the strike-through
      // line.
      const int strikeY = y + renderer.getFontAscenderSize(fontId) / 2 + 6;

      int startX = wordX;
      int strikeWidth = fullWordWidth;

      // Skip em-space prefix same as underline does
      if (w.size() >= 3 && static_cast<uint8_t>(w[0]) == 0xE2 && static_cast<uint8_t>(w[1]) == 0x80 &&
          static_cast<uint8_t>(w[2]) == 0x83) {
        const char* visiblePtr = w.c_str() + 3;
        const int prefixWidth = renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", currentStyle);
        const int visibleWidth = renderer.getTextWidth(fontId, visiblePtr, currentStyle, baseDir);
        startX = wordX + prefixWidth;
        strikeWidth = visibleWidth;
      }

      if ((currentStyle & (EpdFontFamily::SUP | EpdFontFamily::SUB)) != 0) {
        strikeWidth = (strikeWidth + 1) / 2;
      }

      renderer.drawLine(startX, strikeY, startX + strikeWidth, strikeY, 3, foregroundBlack);
    }
  }
}

bool TextBlock::serialize(FsFile& file) const {
  const bool hasBionic = !wordBionicBoundary.empty();
  const bool hasGuideDots = !wordGuideDotXOffset.empty();
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() ||
      words.size() != wordBackgroundBlack.size() ||
      (hasBionic && (words.size() != wordBionicBoundary.size() || words.size() != wordBionicSuffixX.size())) ||
      (!hasBionic && !wordBionicSuffixX.empty()) || (hasGuideDots && words.size() != wordGuideDotXOffset.size())) {
    LOG_ERR(
        "TXB",
        "Serialization failed: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, suffixX=%u, dotX=%u, bg=%u)\n",
        static_cast<uint32_t>(words.size()), static_cast<uint32_t>(wordXpos.size()),
        static_cast<uint32_t>(wordStyles.size()), static_cast<uint32_t>(wordBionicBoundary.size()),
        static_cast<uint32_t>(wordBionicSuffixX.size()), static_cast<uint32_t>(wordGuideDotXOffset.size()),
        static_cast<uint32_t>(wordBackgroundBlack.size()));
    return false;
  }

  // Word data
  if (!serialization::tryWritePod(file, static_cast<uint16_t>(words.size()))) {
    LOG_ERR("TXB", "Serialization failed: could not write word count");
    return false;
  }
  for (const auto& w : words) {
    if (!serialization::tryWriteString(file, w)) {
      LOG_ERR("TXB", "Serialization failed: could not write word payload");
      return false;
    }
  }
  for (auto x : wordXpos) {
    if (!serialization::tryWritePod(file, x)) return false;
  }
  for (auto s : wordStyles) {
    if (!serialization::tryWritePod(file, s)) return false;
  }
  if (!serialization::tryWritePod(file, static_cast<uint8_t>(hasBionic ? 1 : 0))) {
    return false;
  }
  if (hasBionic) {
    for (auto b : wordBionicBoundary) {
      if (!serialization::tryWritePod(file, b)) return false;
    }
    for (auto sx : wordBionicSuffixX) {
      if (!serialization::tryWritePod(file, sx)) return false;
    }
  }
  if (!serialization::tryWritePod(file, static_cast<uint8_t>(hasGuideDots ? 1 : 0))) {
    return false;
  }
  if (hasGuideDots) {
    for (auto dx : wordGuideDotXOffset) {
      if (!serialization::tryWritePod(file, dx)) return false;
    }
  }
  for (auto bg : wordBackgroundBlack) {
    if (!serialization::tryWritePod(file, bg)) return false;
  }

  // Style (alignment + margins/padding/indent)
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
         serialization::tryWritePod(file, blockStyle.directionDefined);
}

std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file) {
  uint16_t wc;
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<uint8_t> wordBionicBoundary;
  std::vector<uint16_t> wordBionicSuffixX;
  std::vector<uint16_t> wordGuideDotXOffset;
  std::vector<uint8_t> wordBackgroundBlack;
  BlockStyle blockStyle;

  // Word count
  if (!serialization::tryReadPod(file, wc)) {
    LOG_ERR("TXB", "Deserialization failed: could not read word count");
    return nullptr;
  }

  // A TextBlock is one rendered line of text, so counts far above a few hundred are not legitimate.
  // Clamp aggressively here so corrupted cache data cannot trigger huge STL allocations on the ESP32-C3.
  if (wc > MAX_WORDS_PER_TEXT_BLOCK) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }

  const uint32_t minimumRemainingBytes = static_cast<uint32_t>(wc) * SERIALIZED_MIN_WORD_METADATA_BYTES +
                                         sizeof(uint8_t) + sizeof(uint8_t) + SERIALIZED_TEXT_BLOCK_TAIL_BYTES;
  const int remainingBeforeWords = file.available();
  if (remainingBeforeWords < 0 || static_cast<uint32_t>(remainingBeforeWords) < minimumRemainingBytes) {
    LOG_ERR("TXB", "Deserialization failed: truncated block metadata (%u words need at least %lu bytes, %d available)",
            wc, static_cast<unsigned long>(minimumRemainingBytes), remainingBeforeWords);
    return nullptr;
  }

  // Word data
  words.resize(wc);
  wordXpos.resize(wc);
  wordStyles.resize(wc);
  wordBackgroundBlack.resize(wc);
  for (auto& w : words) {
    if (!readBoundedString(file, w)) {
      return nullptr;
    }
  }

  const uint32_t remainingMetadataBytes = static_cast<uint32_t>(wc) * SERIALIZED_POST_WORD_MIN_METADATA_BYTES +
                                          sizeof(uint8_t) + sizeof(uint8_t) + SERIALIZED_TEXT_BLOCK_TAIL_BYTES;
  const int remainingAfterWords = file.available();
  if (remainingAfterWords < 0 || static_cast<uint32_t>(remainingAfterWords) < remainingMetadataBytes) {
    LOG_ERR("TXB", "Deserialization failed: truncated post-word metadata (%lu bytes needed, %d available)",
            static_cast<unsigned long>(remainingMetadataBytes), remainingAfterWords);
    return nullptr;
  }

  for (auto& x : wordXpos) {
    if (!serialization::tryReadPod(file, x)) return nullptr;
  }
  for (auto& s : wordStyles) {
    if (!serialization::tryReadPod(file, s)) return nullptr;
  }
  uint8_t hasBionic = 0;
  if (!serialization::tryReadPod(file, hasBionic) || hasBionic > 1) {
    LOG_ERR("TXB", "Deserialization failed: invalid bionic metadata flag");
    return nullptr;
  }
  if (hasBionic) {
    wordBionicBoundary.resize(wc);
    wordBionicSuffixX.resize(wc);
    for (auto& b : wordBionicBoundary) {
      if (!serialization::tryReadPod(file, b)) return nullptr;
    }
    for (auto& sx : wordBionicSuffixX) {
      if (!serialization::tryReadPod(file, sx)) return nullptr;
    }
  }
  uint8_t hasGuideDots = 0;
  if (!serialization::tryReadPod(file, hasGuideDots) || hasGuideDots > 1) {
    LOG_ERR("TXB", "Deserialization failed: invalid guide-dot metadata flag");
    return nullptr;
  }
  if (hasGuideDots) {
    wordGuideDotXOffset.resize(wc);
    for (auto& dx : wordGuideDotXOffset) {
      if (!serialization::tryReadPod(file, dx)) return nullptr;
    }
  }
  for (auto& bg : wordBackgroundBlack) {
    if (!serialization::tryReadPod(file, bg)) return nullptr;
  }

  // Style (alignment + margins/padding/indent)
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
      !serialization::tryReadPod(file, blockStyle.directionDefined)) {
    LOG_ERR("TXB", "Deserialization failed: truncated block style metadata");
    return nullptr;
  }

  auto* textBlock = new (std::nothrow) TextBlock(
      std::move(words), std::move(wordXpos), std::move(wordStyles), std::move(wordBionicBoundary),
      std::move(wordBionicSuffixX), std::move(wordGuideDotXOffset), std::move(wordBackgroundBlack), blockStyle);
  if (!textBlock) {
    LOG_ERR("TXB", "Deserialization failed: could not allocate TextBlock");
    return nullptr;
  }

  return std::unique_ptr<TextBlock>(textBlock);
}
