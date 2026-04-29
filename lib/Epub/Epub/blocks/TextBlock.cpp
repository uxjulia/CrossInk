#include "TextBlock.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>

void TextBlock::render(const GfxRenderer& renderer, const int fontId, const int x, const int y) const {
  // Validate iterator bounds before rendering
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() ||
      words.size() != wordBionicBoundary.size() || words.size() != wordBionicSuffixX.size() ||
      words.size() != wordGuideDotXOffset.size()) {
    LOG_ERR("TXB", "Render skipped: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, suffixX=%u, dotX=%u)\n",
            (uint32_t)words.size(), (uint32_t)wordXpos.size(), (uint32_t)wordStyles.size(),
            (uint32_t)wordBionicBoundary.size(), (uint32_t)wordBionicSuffixX.size(),
            (uint32_t)wordGuideDotXOffset.size());
    return;
  }

  for (size_t i = 0; i < words.size(); i++) {
    const int wordX = wordXpos[i] + x;
    const EpdFontFamily::Style currentStyle = wordStyles[i];
    const uint8_t boundary = wordBionicBoundary[i];

    if (boundary > 0) {
      // Bionic split: draw bold prefix (max 9 codepoints = 36 UTF-8 bytes + null).
      // suffixX is pre-computed at cache creation time to avoid font metric lookups at render time.
      const auto boldStyle = static_cast<EpdFontFamily::Style>(currentStyle | EpdFontFamily::BOLD);
      char boldBuf[40];
      const size_t boldLen = std::min<size_t>({static_cast<size_t>(boundary), words[i].size(), sizeof(boldBuf) - 1});
      memcpy(boldBuf, words[i].c_str(), boldLen);
      boldBuf[boldLen] = '\0';
      renderer.drawText(fontId, wordX, y, boldBuf, true, boldStyle);
      const int suffixX = wordX + wordBionicSuffixX[i];
      renderer.drawText(fontId, suffixX, y, words[i].c_str() + boldLen, true, currentStyle);
    } else {
      renderer.drawText(fontId, wordX, y, words[i].c_str(), true, currentStyle);
    }

    if (wordGuideDotXOffset[i] > 0) {
      renderer.drawText(fontId, wordX + wordGuideDotXOffset[i], y, "\xc2\xb7", true, EpdFontFamily::REGULAR);
    }

    if ((currentStyle & EpdFontFamily::UNDERLINE) != 0) {
      const std::string& w = words[i];
      const int fullWordWidth = renderer.getTextWidth(fontId, w.c_str(), currentStyle);
      // y is the top of the text line; add ascender to reach baseline, then offset 2px below
      const int underlineY = y + renderer.getFontAscenderSize(fontId) + 2;

      int startX = wordX;
      int underlineWidth = fullWordWidth;

      // if word starts with em-space ("\xe2\x80\x83"), account for the additional indent before drawing the line
      if (w.size() >= 3 && static_cast<uint8_t>(w[0]) == 0xE2 && static_cast<uint8_t>(w[1]) == 0x80 &&
          static_cast<uint8_t>(w[2]) == 0x83) {
        const char* visiblePtr = w.c_str() + 3;
        const int prefixWidth = renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", currentStyle);
        const int visibleWidth = renderer.getTextWidth(fontId, visiblePtr, currentStyle);
        startX = wordX + prefixWidth;
        underlineWidth = visibleWidth;
      }

      renderer.drawLine(startX, underlineY, startX + underlineWidth, underlineY, 3, true);
    }

    if ((currentStyle & EpdFontFamily::STRIKETHROUGH) != 0) {
      const std::string& w = words[i];
      const int fullWordWidth = renderer.getTextWidth(fontId, w.c_str(), currentStyle);
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
        const int visibleWidth = renderer.getTextWidth(fontId, visiblePtr, currentStyle);
        startX = wordX + prefixWidth;
        strikeWidth = visibleWidth;
      }

      renderer.drawLine(startX, strikeY, startX + strikeWidth, strikeY, 3, true);
    }
  }
}

bool TextBlock::serialize(FsFile& file) const {
  if (words.size() != wordXpos.size() || words.size() != wordStyles.size() ||
      words.size() != wordBionicBoundary.size() || words.size() != wordBionicSuffixX.size() ||
      words.size() != wordGuideDotXOffset.size()) {
    LOG_ERR("TXB",
            "Serialization failed: size mismatch (words=%u, xpos=%u, styles=%u, boundary=%u, suffixX=%u, dotX=%u)\n",
            static_cast<uint32_t>(words.size()), static_cast<uint32_t>(wordXpos.size()),
            static_cast<uint32_t>(wordStyles.size()), static_cast<uint32_t>(wordBionicBoundary.size()),
            static_cast<uint32_t>(wordBionicSuffixX.size()), static_cast<uint32_t>(wordGuideDotXOffset.size()));
    return false;
  }

  // Word data
  serialization::writePod(file, static_cast<uint16_t>(words.size()));
  for (const auto& w : words) serialization::writeString(file, w);
  for (auto x : wordXpos) serialization::writePod(file, x);
  for (auto s : wordStyles) serialization::writePod(file, s);
  for (auto b : wordBionicBoundary) serialization::writePod(file, b);
  for (auto sx : wordBionicSuffixX) serialization::writePod(file, sx);
  for (auto dx : wordGuideDotXOffset) serialization::writePod(file, dx);

  // Style (alignment + margins/padding/indent)
  serialization::writePod(file, blockStyle.alignment);
  serialization::writePod(file, blockStyle.textAlignDefined);
  serialization::writePod(file, blockStyle.marginTop);
  serialization::writePod(file, blockStyle.marginBottom);
  serialization::writePod(file, blockStyle.marginLeft);
  serialization::writePod(file, blockStyle.marginRight);
  serialization::writePod(file, blockStyle.paddingTop);
  serialization::writePod(file, blockStyle.paddingBottom);
  serialization::writePod(file, blockStyle.paddingLeft);
  serialization::writePod(file, blockStyle.paddingRight);
  serialization::writePod(file, blockStyle.textIndent);
  serialization::writePod(file, blockStyle.textIndentDefined);

  return true;
}

std::unique_ptr<TextBlock> TextBlock::deserialize(FsFile& file) {
  uint16_t wc;
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<uint8_t> wordBionicBoundary;
  std::vector<uint16_t> wordBionicSuffixX;
  std::vector<uint16_t> wordGuideDotXOffset;
  BlockStyle blockStyle;

  // Word count
  serialization::readPod(file, wc);

  // Sanity check: prevent allocation of unreasonably large vectors (max 10000 words per block)
  if (wc > 10000) {
    LOG_ERR("TXB", "Deserialization failed: word count %u exceeds maximum", wc);
    return nullptr;
  }

  // Word data
  words.resize(wc);
  wordXpos.resize(wc);
  wordStyles.resize(wc);
  wordBionicBoundary.resize(wc);
  wordBionicSuffixX.resize(wc);
  wordGuideDotXOffset.resize(wc);
  for (auto& w : words) serialization::readString(file, w);
  for (auto& x : wordXpos) serialization::readPod(file, x);
  for (auto& s : wordStyles) serialization::readPod(file, s);
  for (auto& b : wordBionicBoundary) serialization::readPod(file, b);
  for (auto& sx : wordBionicSuffixX) serialization::readPod(file, sx);
  for (auto& dx : wordGuideDotXOffset) serialization::readPod(file, dx);

  // Style (alignment + margins/padding/indent)
  serialization::readPod(file, blockStyle.alignment);
  serialization::readPod(file, blockStyle.textAlignDefined);
  serialization::readPod(file, blockStyle.marginTop);
  serialization::readPod(file, blockStyle.marginBottom);
  serialization::readPod(file, blockStyle.marginLeft);
  serialization::readPod(file, blockStyle.marginRight);
  serialization::readPod(file, blockStyle.paddingTop);
  serialization::readPod(file, blockStyle.paddingBottom);
  serialization::readPod(file, blockStyle.paddingLeft);
  serialization::readPod(file, blockStyle.paddingRight);
  serialization::readPod(file, blockStyle.textIndent);
  serialization::readPod(file, blockStyle.textIndentDefined);

  return std::unique_ptr<TextBlock>(new TextBlock(std::move(words), std::move(wordXpos), std::move(wordStyles),
                                                  std::move(wordBionicBoundary), std::move(wordBionicSuffixX),
                                                  std::move(wordGuideDotXOffset), blockStyle));
}
