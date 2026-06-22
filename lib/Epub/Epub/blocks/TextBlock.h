#pragma once
#include <EpdFontFamily.h>
#include <HalStorage.h>

#include <memory>
#include <string>
#include <vector>

#include "Block.h"
#include "BlockStyle.h"

// Represents a line of text on a page
class TextBlock final : public Block {
 private:
  std::vector<std::string> words;
  std::vector<int16_t> wordXpos;
  std::vector<EpdFontFamily::Style> wordStyles;
  // Per-word bionic boundary: N > 0 means the first N bytes of words[i] are rendered bold,
  // the remainder in the base style. 0 means no split (whole word uses wordStyles[i]).
  // Empty when no word in the block has a bionic split.
  std::vector<uint8_t> wordBionicBoundary;
  // Pre-computed pixel offset from word start to the regular suffix, stored when boundary > 0.
  // Eliminates getTextAdvanceX from the render path. 0 when boundary == 0.
  // Empty in lockstep with wordBionicBoundary.
  std::vector<uint16_t> wordBionicSuffixX;
  // Pre-computed pixel offset from word start to the guide dot that follows it. 0 = no dot.
  // Eliminates the guide dot as a separate TextBlock entry, saving ~12 bytes per inter-word gap.
  // Empty when no word in the block has a guide dot.
  std::vector<uint16_t> wordGuideDotXOffset;
  // Per-word flags. Bit 0 paints a simple black CSS background behind this word/token.
  // Bit 1 marks a visible hyphen inserted by layout/hyphenation, not the EPUB text.
  std::vector<uint8_t> wordBackgroundBlack;
  BlockStyle blockStyle;

 public:
  static constexpr uint8_t WORD_FLAG_BACKGROUND_BLACK = 0x01;
  static constexpr uint8_t WORD_FLAG_INSERTED_HYPHEN = 0x02;

  explicit TextBlock(std::vector<std::string> words, std::vector<int16_t> word_xpos,
                     std::vector<EpdFontFamily::Style> word_styles, std::vector<uint8_t> bionic_boundary,
                     std::vector<uint16_t> bionic_suffix_x, std::vector<uint16_t> guide_dot_x_offset,
                     std::vector<uint8_t> background_black, const BlockStyle& blockStyle = BlockStyle())
      : words(std::move(words)),
        wordXpos(std::move(word_xpos)),
        wordStyles(std::move(word_styles)),
        wordBionicBoundary(std::move(bionic_boundary)),
        wordBionicSuffixX(std::move(bionic_suffix_x)),
        wordGuideDotXOffset(std::move(guide_dot_x_offset)),
        wordBackgroundBlack(std::move(background_black)),
        blockStyle(blockStyle) {}
  ~TextBlock() override = default;
  void setBlockStyle(const BlockStyle& blockStyle) { this->blockStyle = blockStyle; }
  const BlockStyle& getBlockStyle() const { return blockStyle; }
  const std::vector<std::string>& getWords() const { return words; }
  const std::vector<int16_t>& getWordXpos() const { return wordXpos; }
  const std::vector<EpdFontFamily::Style>& getWordStyles() const { return wordStyles; }
  bool wordEndsWithInsertedHyphen(size_t index) const {
    return index < wordBackgroundBlack.size() && (wordBackgroundBlack[index] & WORD_FLAG_INSERTED_HYPHEN) != 0;
  }
  bool isEmpty() override { return words.empty(); }
  size_t wordCount() const { return words.size(); }
  // given a renderer works out where to break the words into lines
  void render(const GfxRenderer& renderer, int fontId, int x, int y, bool foregroundBlack = true) const;
  BlockType getType() override { return TEXT_BLOCK; }
  bool serialize(HalFile& file) const;
  static std::unique_ptr<TextBlock> deserialize(HalFile& file);
};
