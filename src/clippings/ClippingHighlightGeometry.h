#pragma once

#include <cstdint>

namespace ClippingHighlightGeometry {

struct LineBand {
  int y = 0;
  int height = 0;
};

inline LineBand lineBand(const int y, const int fontLineHeight, const int layoutLineHeight,
                         const uint8_t lineHeightPercent) {
  const int scaledHeight = (fontLineHeight * lineHeightPercent + 50) / 100;
  const int height = layoutLineHeight > 0 ? layoutLineHeight : (scaledHeight > 0 ? scaledHeight : 1);
  // Compress the marker around the font's line box, keeping it behind the
  // letters rather than in the leading above a tightly spaced line.
  const int inset = fontLineHeight > height ? (fontLineHeight - height) / 2 : 0;
  return {y + inset, height};
}

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
