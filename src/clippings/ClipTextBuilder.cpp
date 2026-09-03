#include "ClipTextBuilder.h"

#include <Logging.h>

#include <algorithm>
#include <cctype>

namespace ClipTextBuilder {
namespace {

bool hasEmSpace(const char* word) { return word[0] == '\xe2' && word[1] == '\x80' && word[2] == '\x83'; }

bool isUtf8SpaceAt(const std::string& text, const size_t index, size_t& advance) {
  const auto c = static_cast<unsigned char>(text[index]);
  if (c == 0xC2 && index + 1 < text.size() && static_cast<unsigned char>(text[index + 1]) == 0xA0) {
    advance = 2;
    return true;
  }
  if (c == 0xE2 && index + 2 < text.size() && static_cast<unsigned char>(text[index + 1]) == 0x80) {
    const auto c2 = static_cast<unsigned char>(text[index + 2]);
    if (c2 == 0x83 || c2 == 0xAF) {
      advance = 3;
      return true;
    }
  }
  return false;
}

std::string cleanWordText(const std::string& word) {
  std::string out;
  out.reserve(word.size());
  for (size_t i = 0; i < word.size();) {
    size_t advance = 0;
    if (isUtf8SpaceAt(word, i, advance)) {
      if (!out.empty() && out.back() != ' ') {
        out += ' ';
      }
      i += advance;
      continue;
    }
    const char c = word[i++];
    if (c == '\r' || c == '\n' || c == '\t') {
      if (!out.empty() && out.back() != ' ') {
        out += ' ';
      }
      continue;
    }
    out += c;
  }

  while (!out.empty() && out.front() == ' ') {
    out.erase(out.begin());
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

std::string stripTrailingInsertedHyphen(std::string word, const bool insertedHyphen) {
  if (insertedHyphen && !word.empty() && word.back() == '-') {
    word.pop_back();
  }
  return word;
}

bool areWordsVisuallyAttached(const WordRef& previousWord, const WordRef& word) {
  if (word.pageIdx != previousWord.pageIdx || word.y != previousWord.y) return false;

  if (word.x >= previousWord.x) {
    return word.x <= previousWord.x + previousWord.w + 2;
  }
  return previousWord.x <= word.x + word.w + 2;
}

std::string selectedWordText(const ClipWordStore& wordStore, const WordRef& word,
                             const SelectionBounds* selectionBounds) {
  const bool isFirstBound = selectionBounds && word.pageIdx == selectionBounds->firstPageIdx &&
                            word.pageWordIndex == selectionBounds->firstPageWordOrdinal;
  const bool isLastBound = selectionBounds && word.pageIdx == selectionBounds->lastPageIdx &&
                           word.pageWordIndex == selectionBounds->lastPageWordOrdinal;
  if (!selectionBounds || (!isFirstBound && !isLastBound)) {
    return cleanWordText(wordStore.text(word));
  }

  const size_t textLength = word.textLength;
  const size_t begin = isFirstBound ? std::min<size_t>(selectionBounds->firstWordByteOffset, textLength) : 0;
  const size_t end = isLastBound ? std::min<size_t>(selectionBounds->lastWordByteEndOffset, textLength) : textLength;
  if (end < begin) return {};
  return cleanWordText(std::string(wordStore.text(word) + begin, end - begin));
}

bool isSelectedTableWord(const WordRef& word, const uint16_t tableSelection) {
  return tableSelection == UINT16_MAX || word.tableSelection == tableSelection;
}

}  // namespace

ClippingResult build(const ClipWordStore& wordStore, const uint16_t* wordOrder, const int fromOrder, const int toOrder,
                     const int totalOrder, const int startPageInSection, const int sectionPageCount,
                     const SelectionBounds* selectionBounds, const uint16_t tableSelection) {
  const auto& words = wordStore.words;
  std::string text;
  text.reserve(256);

  const WordRef& firstWord = words[wordOrder[fromOrder]];
  const WordRef& lastWord = words[wordOrder[toOrder]];
  uint16_t startPageWordIndex = firstWord.pageWordIndex;
  uint16_t endPageWordIndex = lastWord.pageWordIndex;
  uint16_t selectedWordCount = 0;
  for (int orderIdx = fromOrder; orderIdx <= toOrder; ++orderIdx) {
    const WordRef& word = words[wordOrder[orderIdx]];
    if (!isSelectedTableWord(word, tableSelection)) continue;
    selectedWordCount++;
    if (word.pageIdx == firstWord.pageIdx) {
      startPageWordIndex = std::min(startPageWordIndex, word.pageWordIndex);
    }
    if (word.pageIdx == lastWord.pageIdx) {
      endPageWordIndex = std::max(endPageWordIndex, word.pageWordIndex);
    }
  }

  constexpr int ANCHOR_WORDS = 4;
  std::string startAnchor;
  int anchorCount = 0;
  const WordRef* previousWord = nullptr;
  std::string previousClean;

  for (int orderIdx = fromOrder; orderIdx <= toOrder; ++orderIdx) {
    const WordRef& word = words[wordOrder[orderIdx]];
    if (!isSelectedTableWord(word, tableSelection)) continue;
    const auto cleanText = selectedWordText(wordStore, word, selectionBounds);
    const auto wordText = stripTrailingInsertedHyphen(cleanText, word.endsWithInsertedHyphen);
    if (wordText.empty()) {
      previousWord = &word;
      previousClean = cleanText;
      continue;
    }
    const bool joinsInsertedHyphen =
        previousWord && previousWord->endsWithInsertedHyphen && !previousClean.empty() && previousClean.back() == '-';
    if (joinsInsertedHyphen) {
      text += wordText;
      previousWord = &word;
      previousClean = cleanText;
      continue;
    }

    const bool yGap =
        previousWord && word.pageIdx == previousWord->pageIdx && word.y > previousWord->y + previousWord->h;
    const bool paragraphStart = previousWord && (hasEmSpace(wordStore.text(word)) || word.paragraphStart || yGap);

    if (previousWord && !text.empty() && !paragraphStart) {
      if (!previousClean.empty() && previousClean.back() == '-' &&
          !std::isspace(static_cast<unsigned char>(wordText[0])) &&
          !std::ispunct(static_cast<unsigned char>(wordText[0]))) {
        text += wordText;
        previousWord = &word;
        previousClean = cleanText;
        continue;
      }
    }

    if (paragraphStart) {
      text += '\n';
    } else if (!text.empty()) {
      const bool attached = previousWord && areWordsVisuallyAttached(*previousWord, word);
      if (!attached) {
        text += ' ';
      }
    }
    text += wordText;

    if (anchorCount < ANCHOR_WORDS) {
      if (!startAnchor.empty()) startAnchor += ' ';
      startAnchor += wordText;
      anchorCount++;
    }
    previousWord = &word;
    previousClean = cleanText;
  }

  std::string endAnchor;
  anchorCount = 0;
  for (int orderIdx = toOrder; orderIdx >= fromOrder && anchorCount < ANCHOR_WORDS; --orderIdx) {
    const WordRef& word = words[wordOrder[orderIdx]];
    if (!isSelectedTableWord(word, tableSelection)) continue;
    const auto wordText =
        stripTrailingInsertedHyphen(selectedWordText(wordStore, word, selectionBounds), word.endsWithInsertedHyphen);
    if (wordText.empty()) continue;
    endAnchor = endAnchor.empty() ? wordText : wordText + ' ' + endAnchor;
    anchorCount++;
  }

  constexpr int CONTEXT_WORDS = 3;
  std::string beforeStart;
  for (int orderIdx = fromOrder - 1; orderIdx >= 0 && (fromOrder - orderIdx) <= CONTEXT_WORDS; --orderIdx) {
    const WordRef& word = words[wordOrder[orderIdx]];
    if (!isSelectedTableWord(word, tableSelection)) continue;
    const auto stripped = stripTrailingInsertedHyphen(cleanWordText(wordStore.text(word)), word.endsWithInsertedHyphen);
    if (stripped.find_first_not_of(' ') == std::string::npos) continue;
    beforeStart = beforeStart.empty() ? stripped : stripped + ' ' + beforeStart;
  }
  std::string afterEnd;
  for (int orderIdx = toOrder + 1; orderIdx < totalOrder && (orderIdx - toOrder) <= CONTEXT_WORDS; ++orderIdx) {
    const WordRef& word = words[wordOrder[orderIdx]];
    if (!isSelectedTableWord(word, tableSelection)) continue;
    const auto stripped = stripTrailingInsertedHyphen(cleanWordText(wordStore.text(word)), word.endsWithInsertedHyphen);
    if (stripped.find_first_not_of(' ') == std::string::npos) continue;
    afterEnd = afterEnd.empty() ? stripped : afterEnd + ' ' + stripped;
  }

  std::string midText;
  constexpr int MID_WORDS = 4;
  if (tableSelection == UINT16_MAX) {
    int midStart = (fromOrder + toOrder) / 2 - (MID_WORDS / 2);
    int midEnd = midStart + MID_WORDS - 1;
    if (midStart < fromOrder) midStart = fromOrder;
    if (midEnd > toOrder) midEnd = toOrder;
    for (int orderIdx = midStart; orderIdx <= midEnd; ++orderIdx) {
      const WordRef& word = words[wordOrder[orderIdx]];
      const auto wordText =
          stripTrailingInsertedHyphen(selectedWordText(wordStore, word, selectionBounds), word.endsWithInsertedHyphen);
      if (!midText.empty()) midText += ' ';
      midText += wordText;
    }
  } else {
    const int midStart = std::max(0, (static_cast<int>(selectedWordCount) - MID_WORDS) / 2);
    int selectedIndex = 0;
    int midCount = 0;
    for (int orderIdx = fromOrder; orderIdx <= toOrder; ++orderIdx) {
      const WordRef& word = words[wordOrder[orderIdx]];
      if (!isSelectedTableWord(word, tableSelection)) continue;
      if (selectedIndex++ < midStart) continue;
      const auto wordText =
          stripTrailingInsertedHyphen(selectedWordText(wordStore, word, selectionBounds), word.endsWithInsertedHyphen);
      if (wordText.empty()) continue;
      if (!midText.empty()) midText += ' ';
      midText += wordText;
      if (++midCount >= MID_WORDS) break;
    }
  }

  return ClippingResult{std::move(text),
                        wordOrder[fromOrder],
                        wordOrder[toOrder],
                        static_cast<uint16_t>(startPageInSection + firstWord.pageIdx),
                        static_cast<uint16_t>(startPageInSection + lastWord.pageIdx),
                        static_cast<uint16_t>(std::max(1, sectionPageCount)),
                        startPageWordIndex,
                        endPageWordIndex,
                        UINT16_MAX,
                        tableSelection,
                        std::move(startAnchor),
                        std::move(endAnchor),
                        std::move(beforeStart),
                        std::move(afterEnd),
                        std::move(midText),
                        selectedWordCount};
}

}  // namespace ClipTextBuilder
