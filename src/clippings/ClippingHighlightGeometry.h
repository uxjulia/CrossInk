#pragma once

#include <cstdint>

namespace ClippingHighlightGeometry {

struct WordRect {
  uint16_t pageWordIndex = 0;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct GapRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

inline bool matchesTableSelection(const uint16_t selection, const uint16_t wordSelection) {
  return selection == UINT16_MAX || selection == wordSelection;
}

inline bool isTableColumnCandidate(const uint16_t wordSelection) { return wordSelection != UINT16_MAX; }

inline bool matchesTableColumn(const uint16_t savedSelection, const uint16_t candidateSelection,
                               const uint8_t columnCount) {
  return columnCount > 0 && isTableColumnCandidate(savedSelection) && isTableColumnCandidate(candidateSelection) &&
         savedSelection % columnCount == candidateSelection % columnCount;
}

inline bool gapBetweenAdjacentWords(const WordRect& previous, const WordRect& current, GapRect& gap) {
  if (static_cast<uint32_t>(current.pageWordIndex) != static_cast<uint32_t>(previous.pageWordIndex) + 1U ||
      previous.y != current.y || previous.width <= 0 || current.width <= 0 || previous.height <= 0 ||
      current.height <= 0) {
    return false;
  }

  const int previousRight = previous.x + previous.width;
  const int currentRight = current.x + current.width;
  const int leftWordRight = previous.x <= current.x ? previousRight : currentRight;
  const int rightWordLeft = previous.x <= current.x ? current.x : previous.x;
  if (rightWordLeft <= leftWordRight) {
    return false;
  }

  const int gapHeight = previous.height < current.height ? previous.height : current.height;
  gap = {leftWordRight, current.y, rightWordLeft - leftWordRight, gapHeight};
  return true;
}

}  // namespace ClippingHighlightGeometry
