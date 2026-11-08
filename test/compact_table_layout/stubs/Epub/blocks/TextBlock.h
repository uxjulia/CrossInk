#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "EpdFontFamily.h"
#include "Epub/blocks/BlockStyle.h"

class TextBlock {
 public:
  struct SourceRange {
    uint32_t start = UINT32_MAX;
    uint32_t end = UINT32_MAX;
  };
  static constexpr uint8_t WORD_FLAG_BACKGROUND_BLACK = 0x01;
  static constexpr uint8_t WORD_FLAG_LINK_ID_SHIFT = 2;
  static constexpr uint8_t WORD_FLAG_LINK_ID_MASK = 0xFC;
  std::vector<std::string> words;
  std::vector<int16_t> xPositions;
  std::vector<EpdFontFamily::Style> styles;
  BlockStyle style;
  std::vector<SourceRange> sources;

  TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& xPositions,
            const std::vector<EpdFontFamily::Style>& styles, const std::vector<uint8_t>&, const std::vector<uint16_t>&,
            const std::vector<uint16_t>&, const std::vector<uint8_t>&, const std::vector<bool>&,
            const BlockStyle& style, std::vector<std::string> = {}, const std::vector<uint8_t>& = {}, const char* = "",
            int8_t = 0, const std::vector<uint32_t>& = {}, const std::vector<SourceRange>& sourceRanges = {})
      : words(words), xPositions(xPositions), styles(styles), style(style), sources(sourceRanges) {}

  bool valid() const { return true; }
};
