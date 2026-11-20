#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "EpdFontFamily.h"
#include "GfxRenderer.h"

enum class CssTextAlign : uint8_t { Justify = 0, Left = 1, Center = 2, Right = 3, None = 4 };

struct BlockStyle {
  uint16_t lineHeight = 0;
  int16_t ruledTextOffset = 0;
  CssTextAlign alignment = CssTextAlign::Justify;
  int16_t marginLeft = 0;
  int16_t marginRight = 0;
  int16_t paddingLeft = 0;
  int16_t paddingRight = 0;
  int16_t textIndent = 0;
  bool textIndentDefined = false;
  bool isRtl = false;

  int16_t leftInset() const { return marginLeft + paddingLeft; }
  int16_t rightInset() const { return marginRight + paddingRight; }
};

class TextBlock {
 public:
  struct Word {
    std::string text;
    int16_t x = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    bool endsWithInsertedHyphen = false;
    bool hasSpaceBefore = false;
  };

  explicit TextBlock(std::vector<Word> words, BlockStyle style = {}) : words(std::move(words)), style(style) {}

  void render(GfxRenderer& renderer, int fontId, int x, int y, bool foregroundBlack) const {
    for (const auto& word : words)
      renderer.drawText(fontId, x + word.x, y, word.text.c_str(), foregroundBlack, word.style);
  }

  int maxAscender(const GfxRenderer& renderer, int fontId) const { return renderer.getFontAscenderSize(fontId); }
  int maxLineHeight(const GfxRenderer& renderer, int fontId) const { return renderer.getLineHeight(fontId); }
  int getRubyShift(int) const { return 0; }
  int8_t getCharacterSpacing() const { return 0; }
  uint16_t wordCount() const { return static_cast<uint16_t>(words.size()); }
  const char* wordText(uint16_t index) const { return words[index].text.c_str(); }
  uint16_t wordTextLen(uint16_t index) const { return static_cast<uint16_t>(words[index].text.size()); }
  int16_t wordXpos(uint16_t index) const { return words[index].x; }
  EpdFontFamily::Style wordStyle(uint16_t index) const { return words[index].style; }
  static constexpr uint8_t WORD_FLAG_BACKGROUND_BLACK = 1;
  uint8_t wordFlags(uint16_t) const { return 0; }
  bool wordHidden(uint16_t) const { return false; }
  bool wordBackgroundSpace(uint16_t) const { return false; }
  uint8_t focusBoundary(uint16_t) const { return 0; }
  bool wordEndsWithInsertedHyphen(uint16_t index) const { return words[index].endsWithInsertedHyphen; }
  bool wordHasSpaceBefore(uint16_t index) const { return words[index].hasSpaceBefore; }
  uint16_t guideDotXOffset(uint16_t) const { return 0; }
  const BlockStyle& getBlockStyle() const { return style; }

 private:
  std::vector<Word> words;
  BlockStyle style;
};

enum PageElementTag : uint8_t { TAG_PageLine = 1 };

class PageElement {
 public:
  int16_t xPos;
  int16_t yPos;

  PageElement(int16_t x, int16_t y) : xPos(x), yPos(y) {}
  virtual ~PageElement() = default;
  virtual PageElementTag getTag() const = 0;
};

class PageLine final : public PageElement {
 public:
  PageLine(std::shared_ptr<TextBlock> block, int16_t x, int16_t y) : PageElement(x, y), block(std::move(block)) {}

  const std::shared_ptr<TextBlock>& getBlock() const { return block; }
  PageElementTag getTag() const override { return TAG_PageLine; }

 private:
  std::shared_ptr<TextBlock> block;
};

class Page {
 public:
  std::vector<std::unique_ptr<PageElement>> elements;
};
