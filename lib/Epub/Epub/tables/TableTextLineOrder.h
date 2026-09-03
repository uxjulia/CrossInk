#pragma once

#include <cstddef>

namespace TableTextLineOrder {

// Visit table text in visual reading order: left-to-right across each wrapped
// line, then top-to-bottom. This keeps generic reader navigation aligned with
// what the reader shows; clipping applies table-column constraints separately.
template <typename Row, typename Visitor>
bool forEachCellLineInVisualOrder(const Row& row, Visitor&& visitor) {
  size_t maxLineCount = 0;
  for (const auto& cell : row.cells) {
    maxLineCount = std::max(maxLineCount, cell.lines.size());
  }

  for (size_t lineIndex = 0; lineIndex < maxLineCount; ++lineIndex) {
    for (size_t cellIndex = 0; cellIndex < row.cells.size(); ++cellIndex) {
      if (lineIndex < row.cells[cellIndex].lines.size() && !visitor(cellIndex, lineIndex)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace TableTextLineOrder
