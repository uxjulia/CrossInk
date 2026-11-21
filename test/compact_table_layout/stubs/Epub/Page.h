#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "Epub/FootnoteEntry.h"
#include "Epub/blocks/TextBlock.h"

struct TableFragmentCell {
  static constexpr uint8_t MAX_SERIALIZED_LINES = 64;
  bool isHeader = false;
  uint8_t colSpan = 1;
  uint16_t rightEdge = 0;
  std::vector<std::shared_ptr<TextBlock>> lines;
};

struct TableFragmentRow {
  static constexpr uint8_t MAX_SERIALIZED_CELLS = 8;
  uint16_t height = 0;
  bool headerSeparator = false;
  std::vector<TableFragmentCell> cells;
};

class Page {
 public:
  static constexpr uint16_t MAX_FOOTNOTES_PER_PAGE = 16;
};
