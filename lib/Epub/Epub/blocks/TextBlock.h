#pragma once
#include <EpdFontFamily.h>
#include <HalStorage.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "Block.h"
#include "BlockStyle.h"

// Represents one rendered line. Per-word data is packed into one heap arena
// instead of separate vectors/strings, which reduces heap fragmentation when
// pages are loaded and discarded repeatedly on the ESP32-C3.
class TextBlock final : public Block {
 public:
  struct SourceRange {
    uint32_t start = UINT32_MAX;
    uint32_t end = UINT32_MAX;
    bool known() const { return start <= end && end != UINT32_MAX; }
    bool valid() const { return known() && start < end; }
    bool wellFormed() const { return known() || (start == UINT32_MAX && end == UINT32_MAX); }
  };

 private:
  int baselineOffset(int baseline, int ascender, EpdFontFamily::Style style) const;
  BlockStyle blockStyle;
  int8_t characterSpacing = 0;
  uint16_t numWords = 0;
  uint16_t textBytes = 0;  // Total size of the text region, including NULs.
  bool focusPresent = false;
  bool guideDotsPresent = false;
  bool wordFlagsPresent = false;
  bool wordSpacesPresent = false;
  uint8_t initialLetterBytes = 0;  // logical prefix drawn separately as a drop cap
  std::unique_ptr<uint32_t[]> wordFamilies;
  bool sourceRangesPresent = false;
  const SourceRange* sourceRangesArr = nullptr;
  bool wordSizesPresent = false;
  const uint8_t* wordSizesArr = nullptr;
  bool isValid = true;
  std::unique_ptr<uint8_t[]> arena;

  const uint16_t* textOffArr = nullptr;
  const int16_t* xposArr = nullptr;
  // Distance from the word's left edge to the second visual run. The suffix is
  // second for LTR words; the bold logical prefix is second for RTL words.
  const uint16_t* focusRunOffsetArr = nullptr;   // null when !focusPresent
  const uint16_t* guideDotXOffsetArr = nullptr;  // null when !guideDotsPresent
  const uint8_t* stylesArr = nullptr;
  const uint8_t* focusBoundaryArr = nullptr;  // null when !focusPresent
  const uint8_t* wordFlagsArr = nullptr;      // null when !wordFlagsPresent
  const uint8_t* wordSpacesArr = nullptr;     // null when !wordSpacesPresent
  const char* textArr = nullptr;
  std::vector<std::string> rubyTexts;

  TextBlock() = default;  // deserialize() fills the fields directly.
  static constexpr size_t wordSpacesBytes(const uint16_t wordCount) { return (wordCount + 7U) / 8U; }
  static size_t arenaSize(uint16_t wordCount, bool hasFocus, bool hasGuideDots, bool hasWordFlags, bool hasWordSpaces,
                          uint16_t textBytes, bool hasWordSizes = false, bool hasSourceRanges = false);
  void bindArenaPointers();

 public:
  static constexpr uint8_t WORD_FLAG_BACKGROUND_BLACK = 0x01;
  static constexpr uint8_t WORD_FLAG_INSERTED_HYPHEN = 0x02;
  static constexpr uint8_t WORD_FLAG_LINK_ID_SHIFT = 2;
  static constexpr uint8_t WORD_FLAG_LINK_ID_MASK = 0xFC;

  explicit TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>& wordStyles, const std::vector<uint8_t>& focusBoundary,
                     const std::vector<uint16_t>& focusRunOffset, const std::vector<uint16_t>& guideDotXOffset,
                     const std::vector<uint8_t>& wordFlags, const std::vector<bool>& wordHasSpaceBefore,
                     const BlockStyle& blockStyle = BlockStyle(), std::vector<std::string> rubyTexts = {},
                     const std::vector<uint8_t>& wordSizes = {}, const char* initialLetter = "",
                     int8_t characterSpacing = 0, const std::vector<uint32_t>& fontFamilies = {},
                     const std::vector<SourceRange>& sourceRanges = {});
  ~TextBlock() override = default;
  TextBlock(const TextBlock&) = delete;
  TextBlock& operator=(const TextBlock&) = delete;

  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  const BlockStyle& getBlockStyle() const { return blockStyle; }
  bool isEmpty() override { return numWords == 0; }
  bool valid() const { return isValid; }
  uint16_t wordCount() const { return numWords; }
  SourceRange wordSourceRange(uint16_t i) const { return sourceRangesPresent ? sourceRangesArr[i] : SourceRange{}; }
  const char* wordText(const uint16_t i) const { return textArr + textOffArr[i]; }
  uint16_t wordTextLen(const uint16_t i) const {
    const uint16_t end = (i + 1 < numWords) ? textOffArr[i + 1] : textBytes;
    return end - textOffArr[i] - 1;
  }
  int16_t wordXpos(const uint16_t i) const { return xposArr[i]; }
  EpdFontFamily::Style wordStyle(const uint16_t i) const { return static_cast<EpdFontFamily::Style>(stylesArr[i]); }
  uint8_t focusBoundary(const uint16_t i) const { return focusPresent ? focusBoundaryArr[i] : 0; }
  uint16_t focusRunOffset(const uint16_t i) const { return focusPresent ? focusRunOffsetArr[i] : 0; }
  uint16_t guideDotXOffset(const uint16_t i) const { return guideDotsPresent ? guideDotXOffsetArr[i] : 0; }
  uint8_t wordFlags(const uint16_t i) const { return wordFlagsPresent ? wordFlagsArr[i] : 0; }
  bool wordHasSpaceBefore(const uint16_t i) const {
    if (!wordSpacesPresent) return i > 0;
    return (wordSpacesArr[i / 8U] & static_cast<uint8_t>(1U << (i % 8U))) != 0;
  }
  uint8_t wordLinkId(const uint16_t i) const {
    return static_cast<uint8_t>((wordFlags(i) & WORD_FLAG_LINK_ID_MASK) >> WORD_FLAG_LINK_ID_SHIFT);
  }
  bool wordEndsWithInsertedHyphen(const uint16_t i) const { return (wordFlags(i) & WORD_FLAG_INSERTED_HYPHEN) != 0; }
  int8_t getCharacterSpacing() const { return characterSpacing; }
  bool hasRuby() const;
  int getRubyShift(int ascender) const { return hasRuby() ? (ascender / 2) : 0; }
  const std::vector<std::string>& getRubyTexts() const { return rubyTexts; }

  const char* visibleWordText(uint16_t i) const { return wordText(i) + (i == 0 ? initialLetterBytes : 0); }
  uint16_t visibleWordTextLen(uint16_t i) const { return wordTextLen(i) - (i == 0 ? initialLetterBytes : 0); }
  int wordYOffset(const GfxRenderer& renderer, int fontId, uint16_t i) const;
  uint8_t wordFontSize(uint16_t i) const { return wordSizesPresent ? wordSizesArr[i] : 0; }
  int wordFontId(const GfxRenderer& renderer, int fontId, uint16_t i) const;
  int maxAscender(const GfxRenderer& renderer, int fontId) const;
  int maxLineHeight(const GfxRenderer& renderer, int fontId) const;
  int resolvedFontId(const GfxRenderer& renderer, int fontId) const;
  void render(const GfxRenderer& renderer, int fontId, int x, int y, bool foregroundBlack = true) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(Print& file) const;
  static std::unique_ptr<TextBlock> deserialize(HalFile& file);
};
