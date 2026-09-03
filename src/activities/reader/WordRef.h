#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

struct WordRef {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int pageIdx = 0;
  uint16_t pageWordIndex = 0;
  uint16_t textOffset = 0;
  uint16_t textLength = 0;
  uint16_t tableSelection = UINT16_MAX;
  EpdFontFamily::Style style = EpdFontFamily::REGULAR;
  bool paragraphStart = false;
  bool endsWithInsertedHyphen = false;
  bool lineIsRtl = false;
};

static_assert(std::is_trivially_copyable_v<WordRef>, "Clip word metadata must stay allocation-free");

struct ClipWordStore {
  static constexpr size_t MAX_TEXT_POOL_BYTES = UINT16_MAX;

  std::vector<WordRef> words;
  std::string textPool;

  const char* text(const WordRef& word) const {
    return word.textOffset < textPool.size() ? textPool.data() + word.textOffset : "";
  }

  bool canAppend(const size_t textLength) const {
    return textLength <= UINT16_MAX && textPool.size() <= MAX_TEXT_POOL_BYTES &&
           textLength + 1 <= MAX_TEXT_POOL_BYTES - textPool.size();
  }

  bool appendText(WordRef& word, const char* text) {
    const size_t length = strlen(text);
    if (!canAppend(length)) return false;

    word.textOffset = static_cast<uint16_t>(textPool.size());
    word.textLength = static_cast<uint16_t>(length);
    textPool.append(text, length);
    textPool.push_back('\0');
    return true;
  }
};
