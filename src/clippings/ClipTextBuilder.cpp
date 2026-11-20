#include "ClipTextBuilder.h"

#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace ClipTextBuilder {
namespace {

bool hasEmSpace(const char* word) { return word[0] == '\xe2' && word[1] == '\x80' && word[2] == '\x83'; }

size_t spaceLength(std::string_view text) {
  if (text.empty()) return 0;
  const auto c = static_cast<unsigned char>(text[0]);
  if (c == ' ' || c == '\r' || c == '\n' || c == '\t') return 1;
  if (c == 0xC2 && text.size() >= 2 && static_cast<unsigned char>(text[1]) == 0xA0) return 2;
  if (c == 0xE2 && text.size() >= 3 && static_cast<unsigned char>(text[1]) == 0x80) {
    const auto last = static_cast<unsigned char>(text[2]);
    if (last == 0x83 || last == 0xAF) return 3;
  }
  return 0;
}

std::string_view trimWordSpaces(std::string_view word) {
  while (const size_t length = spaceLength(word)) word.remove_prefix(length);
  while (!word.empty()) {
    const size_t size = word.size();
    if (spaceLength(word.substr(size - 1)) == 1) {
      word.remove_suffix(1);
    } else if (size >= 2 && spaceLength(word.substr(size - 2)) == 2) {
      word.remove_suffix(2);
    } else if (size >= 3 && spaceLength(word.substr(size - 3)) == 3) {
      word.remove_suffix(3);
    } else {
      break;
    }
  }
  return word;
}

// Append straight into the reserved clipping, retaining authored ASCII spaces
// and collapsing other whitespace only within this word. The caller passes
// a nonempty, trimmed fragment, so a text run is emitted before the first gap.
void appendWordText(std::string& output, std::string_view word) {
  size_t run = 0;
  for (size_t i = 0; i < word.size();) {
    const size_t length = spaceLength(word.substr(i));
    if (length == 0) {
      ++i;
      continue;
    }
    output.append(word.data() + run, i - run);
    if (word[i] == ' ' || output.back() != ' ') output += ' ';
    i += length;
    run = i;
  }
  output.append(word.data() + run, word.size() - run);
}

bool areWordsVisuallyAttached(const WordRef& previousWord, const WordRef& word) {
  if (word.pageIdx != previousWord.pageIdx || word.y != previousWord.y) return false;

  int tolerance = 2;
  // Positive tracking adds space between style fragments as well as within them.
  // Only relax the tolerance for consecutive words of the same layout block.
  if (previousWord.pageWordIndex + 1 == word.pageWordIndex && word.followsWordInBlock) {
    tolerance += word.trackingJoinSlack;
  } else if (word.pageWordIndex + 1 == previousWord.pageWordIndex && previousWord.followsWordInBlock) {
    tolerance += previousWord.trackingJoinSlack;
  }

  if (word.x >= previousWord.x) {
    return word.x <= previousWord.x + previousWord.w + tolerance;
  }
  return previousWord.x <= word.x + word.w + tolerance;
}

bool hasLayoutSpaceBetween(const WordRef& previousWord, const WordRef& word) {
  // Layout ordinals remain consecutive across line wraps on the same page.
  if (word.pageIdx != previousWord.pageIdx) return false;

  // Selection traverses RTL rows in reverse, but the space flag follows layout order.
  if (previousWord.pageWordIndex + 1 == word.pageWordIndex) return word.hasSpaceBefore;
  return word.pageWordIndex + 1 == previousWord.pageWordIndex && previousWord.hasSpaceBefore;
}

std::string_view selectedWordText(const ClipWordStore& wordStore, const WordRef& word,
                                  const SelectionBounds* selectionBounds, bool& includesWordEnd) {
  const bool isFirstBound = selectionBounds && word.pageIdx == selectionBounds->firstPageIdx &&
                            word.pageWordIndex == selectionBounds->firstPageWordOrdinal;
  const bool isLastBound = selectionBounds && word.pageIdx == selectionBounds->lastPageIdx &&
                           word.pageWordIndex == selectionBounds->lastPageWordOrdinal;
  if (!selectionBounds || (!isFirstBound && !isLastBound)) {
    includesWordEnd = word.textLength != 0;
    return {wordStore.text(word), word.textLength};
  }

  const size_t textLength = word.textLength;
  const size_t begin = isFirstBound ? std::min<size_t>(selectionBounds->firstWordByteOffset, textLength) : 0;
  const size_t end = isLastBound ? std::min<size_t>(selectionBounds->lastWordByteEndOffset, textLength) : textLength;
  if (end <= begin) return {};
  includesWordEnd = end == textLength;
  return {wordStore.text(word) + begin, end - begin};
}

uint32_t countCharacters(const char* text, const size_t begin, const size_t end) {
  uint32_t count = 0;
  for (size_t i = begin; i < end; ++i)
    if ((static_cast<uint8_t>(text[i]) & 0xC0) != 0x80) ++count;
  return count;
}

bool selectedSourceRange(const ClipWordStore& store, const WordRef& word, const SelectionBounds* bounds,
                         uint32_t& start, uint32_t& end) {
  start = word.sourceStart;
  end = word.sourceEnd;
  if (!bounds) return true;
  const bool first = word.pageIdx == bounds->firstPageIdx && word.pageWordIndex == bounds->firstPageWordOrdinal;
  const bool last = word.pageIdx == bounds->lastPageIdx && word.pageWordIndex == bounds->lastPageWordOrdinal;
  const size_t begin = first ? std::min<size_t>(bounds->firstWordByteOffset, word.textLength) : 0;
  const size_t finish = last ? std::min<size_t>(bounds->lastWordByteEndOffset, word.textLength) : word.textLength;
  if (begin == 0 && finish == word.textLength) return true;
  const char* text = store.text(word);
  if (start >= end || end == UINT32_MAX || finish < begin || (static_cast<uint8_t>(text[begin]) & 0xC0) == 0x80 ||
      (static_cast<uint8_t>(text[finish]) & 0xC0) == 0x80)
    return false;
  const uint32_t indent = hasEmSpace(text) ? 1 : 0;
  const uint32_t insertedHyphen = word.endsWithInsertedHyphen ? 1 : 0;
  // Whole-word source ranges survive NFC composition. An interior byte bound
  // needs a one-to-one character map; never expand an ambiguous fragment.
  if (end - start != countCharacters(text, 0, word.textLength) - indent - insertedHyphen) return false;
  start += countCharacters(text, 0, begin) - (indent && begin >= 3 ? 1 : 0);
  end -= countCharacters(text, finish, word.textLength) - (insertedHyphen && finish < word.textLength ? 1 : 0);
  return start < end;
}

bool isSelectedTableWord(const WordRef& word, const uint16_t tableSelection) {
  return tableSelection == UINT16_MAX || word.tableSelection == tableSelection;
}

}  // namespace

AllocationBudget allocationBudget(const ClipWordStore& wordStore, const uint16_t* wordOrder, const int fromOrder,
                                  const int toOrder, const uint16_t tableSelection) {
  AllocationBudget budget;
  for (int i = fromOrder; i <= toOrder; ++i) {
    const auto& word = wordStore.words[wordOrder[i]];
    if (!isSelectedTableWord(word, tableSelection)) continue;
    // Cleaning only removes bytes; allow one separator per selected word.
    budget.textCapacity += word.textLength + 1U;
  }
  return budget;
}

ClippingResult build(const ClipWordStore& wordStore, const uint16_t* wordOrder, const int fromOrder, const int toOrder,
                     const int startPageInSection, const int sectionPageCount, const SelectionBounds* selectionBounds,
                     const uint16_t tableSelection) {
  const auto& words = wordStore.words;
  std::string text;
  // Reserve once for the complete selection instead of repeatedly growing and
  // copying the result. The activity checks this same budget before assembly.
  text.reserve(allocationBudget(wordStore, wordOrder, fromOrder, toOrder, tableSelection).textCapacity);

  const WordRef& firstWord = words[wordOrder[fromOrder]];
  const WordRef& lastWord = words[wordOrder[toOrder]];
  uint16_t startPageWordIndex = firstWord.pageWordIndex;
  uint16_t endPageWordIndex = lastWord.pageWordIndex;
  uint16_t selectedWordCount = 0;
  uint32_t sourceStart = UINT32_MAX, sourceEnd = 0;
  bool allSourceRangesKnown = true;
  bool sourceMappingBlocked = false;
  for (int orderIdx = fromOrder; orderIdx <= toOrder; ++orderIdx) {
    const WordRef& word = words[wordOrder[orderIdx]];
    if (!isSelectedTableWord(word, tableSelection)) continue;
    selectedWordCount++;
    uint32_t selectedStart, selectedEnd;
    if (!selectedSourceRange(wordStore, word, selectionBounds, selectedStart, selectedEnd)) {
      allSourceRangesKnown = false;
      sourceMappingBlocked = true;
    } else if (selectedStart > selectedEnd || selectedEnd == UINT32_MAX) {
      allSourceRangesKnown = false;
    } else if (selectedStart < selectedEnd) {
      sourceStart = std::min(sourceStart, selectedStart);
      sourceEnd = std::max(sourceEnd, selectedEnd);
    }
    if (word.pageIdx == firstWord.pageIdx) {
      startPageWordIndex = std::min(startPageWordIndex, word.pageWordIndex);
    }
    if (word.pageIdx == lastWord.pageIdx) {
      endPageWordIndex = std::max(endPageWordIndex, word.pageWordIndex);
    }
  }

  const WordRef* previousWord = nullptr;
  bool previousEndsWithHyphen = false;
  bool previousInsertedHyphen = false;

  for (int orderIdx = fromOrder; orderIdx <= toOrder; ++orderIdx) {
    const WordRef& word = words[wordOrder[orderIdx]];
    if (!isSelectedTableWord(word, tableSelection)) continue;
    bool includesWordEnd = false;
    auto wordText = trimWordSpaces(selectedWordText(wordStore, word, selectionBounds, includesWordEnd));
    // A partial selection can itself end at an authored hyphen. The layout
    // hyphen is present only when the selected bytes reach the token's end.
    const bool endsWithHyphen = !wordText.empty() && wordText.back() == '-';
    const bool insertedHyphen = word.endsWithInsertedHyphen && includesWordEnd && endsWithHyphen;
    if (insertedHyphen) wordText.remove_suffix(1);

    if (!wordText.empty()) {
      if (!previousInsertedHyphen) {
        const bool yGap =
            previousWord && word.pageIdx == previousWord->pageIdx && word.y > previousWord->y + previousWord->h;
        const bool paragraphStart = previousWord && (hasEmSpace(wordStore.text(word)) || word.paragraphStart || yGap);
        const bool hasLayoutSpace = previousWord && hasLayoutSpaceBetween(*previousWord, word);
        const bool joinsAuthoredHyphen = previousEndsWithHyphen && !hasLayoutSpace &&
                                         !std::isspace(static_cast<unsigned char>(wordText.front())) &&
                                         !std::ispunct(static_cast<unsigned char>(wordText.front()));

        if (paragraphStart) {
          text += '\n';
        } else if (!text.empty() && !joinsAuthoredHyphen) {
          const bool attached = previousWord && areWordsVisuallyAttached(*previousWord, word);
          if (hasLayoutSpace || !attached) text += ' ';
        }
      }
      appendWordText(text, wordText);
    }

    previousWord = &word;
    previousEndsWithHyphen = endsWithHyphen;
    previousInsertedHyphen = insertedHyphen;
  }

  ClippingResult result{std::move(text),
                        static_cast<uint16_t>(startPageInSection + firstWord.pageIdx),
                        static_cast<uint16_t>(startPageInSection + lastWord.pageIdx),
                        static_cast<uint16_t>(std::max(1, sectionPageCount)),
                        startPageWordIndex,
                        endPageWordIndex,
                        UINT16_MAX,
                        tableSelection,
                        selectedWordCount};
  if (allSourceRangesKnown && sourceStart < sourceEnd) {
    result.sourceStart = sourceStart;
    result.sourceEnd = sourceEnd;
  } else if (sourceMappingBlocked) {
    result.sourceEnd = 0;  // Explicitly unresolvable: do not migrate to the whole outer word.
  }
  return result;
}

}  // namespace ClipTextBuilder
