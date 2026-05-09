#include "Page.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Serialization.h>

namespace {

constexpr uint16_t MAX_PAGE_ELEMENTS = 1024;
constexpr uint8_t MAX_TABLE_ROWS_PER_FRAGMENT = 64;
constexpr uint8_t MAX_TABLE_CELLS_PER_ROW = 8;
constexpr uint8_t MAX_TABLE_LINES_PER_CELL = 64;
static_assert(TableFragmentCell::MAX_SERIALIZED_LINES == MAX_TABLE_LINES_PER_CELL);
static_assert(TableFragmentRow::MAX_SERIALIZED_CELLS == MAX_TABLE_CELLS_PER_ROW);
static_assert(PageTableFragment::MAX_SERIALIZED_ROWS == MAX_TABLE_ROWS_PER_FRAGMENT);

template <typename Predicate>
void renderFilteredPageElements(const std::vector<std::shared_ptr<PageElement>>& elements, GfxRenderer& renderer,
                                const int fontId, const int xOffset, const int yOffset, Predicate&& predicate) {
  for (const auto& element : elements) {
    if (predicate(*element)) {
      element->render(renderer, fontId, xOffset, yOffset);
    }
  }
}

}  // namespace

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset);
}

bool PageLine::serialize(FsFile& file) {
  if (!serialization::tryWritePod(file, xPos) || !serialization::tryWritePod(file, yPos)) {
    LOG_ERR("PGE", "Serialization failed: could not write PageLine coordinates");
    return false;
  }

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  if (!serialization::tryReadPod(file, xPos) || !serialization::tryReadPod(file, yPos)) {
    LOG_ERR("PGE", "Deserialization failed: truncated PageLine coordinates");
    return nullptr;
  }

  auto tb = TextBlock::deserialize(file);
  if (!tb) {
    LOG_ERR("PGE", "Deserialization failed: PageLine text block was invalid");
    return nullptr;
  }

  auto* pageLine = new (std::nothrow) PageLine(std::move(tb), xPos, yPos);
  if (!pageLine) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageLine");
    return nullptr;
  }
  return std::unique_ptr<PageLine>(pageLine);
}

void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  // Images don't use fontId or text rendering
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset);
}

bool PageImage::serialize(FsFile& file) {
  if (!serialization::tryWritePod(file, xPos) || !serialization::tryWritePod(file, yPos)) {
    LOG_ERR("PGE", "Serialization failed: could not write PageImage coordinates");
    return false;
  }

  // serialize ImageBlock
  return imageBlock->serialize(file);
}

std::unique_ptr<PageImage> PageImage::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  if (!serialization::tryReadPod(file, xPos) || !serialization::tryReadPod(file, yPos)) {
    LOG_ERR("PGE", "Deserialization failed: truncated PageImage coordinates");
    return nullptr;
  }

  auto ib = ImageBlock::deserialize(file);
  if (!ib) {
    LOG_ERR("PGE", "Deserialization failed: PageImage block was invalid");
    return nullptr;
  }

  auto* pageImage = new (std::nothrow) PageImage(std::move(ib), xPos, yPos);
  if (!pageImage) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageImage");
    return nullptr;
  }
  return std::unique_ptr<PageImage>(pageImage);
}

void PageHorizontalRule::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  (void)fontId;
  if (width == 0 || thickness == 0) {
    return;
  }

  renderer.drawLine(xPos + xOffset, yPos + yOffset, xPos + xOffset + width - 1, yPos + yOffset, thickness, true);
}

bool PageHorizontalRule::serialize(FsFile& file) {
  return serialization::tryWritePod(file, xPos) && serialization::tryWritePod(file, yPos) &&
         serialization::tryWritePod(file, width) && serialization::tryWritePod(file, thickness);
}

std::unique_ptr<PageHorizontalRule> PageHorizontalRule::deserialize(FsFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint16_t width = 0;
  uint8_t thickness = 0;
  if (!serialization::tryReadPod(file, xPos) || !serialization::tryReadPod(file, yPos) ||
      !serialization::tryReadPod(file, width) || !serialization::tryReadPod(file, thickness)) {
    LOG_ERR("PGE", "Deserialization failed: truncated PageHorizontalRule metadata");
    return nullptr;
  }

  if (width == 0 || thickness == 0) {
    LOG_ERR("PGE", "Deserialization failed: invalid horizontal rule metadata (width=%u thickness=%u)", width,
            thickness);
    return nullptr;
  }

  auto* rule = new (std::nothrow) PageHorizontalRule(width, thickness, xPos, yPos);
  if (!rule) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageHorizontalRule");
    return nullptr;
  }
  return std::unique_ptr<PageHorizontalRule>(rule);
}

bool TableFragmentCell::serialize(FsFile& file) const {
  if (lines.size() > MAX_TABLE_LINES_PER_CELL) {
    LOG_ERR("PTB", "Serialization failed: cell line count %u exceeds maximum", static_cast<uint32_t>(lines.size()));
    return false;
  }

  if (!serialization::tryWritePod(file, isHeader) ||
      !serialization::tryWritePod(file, static_cast<uint8_t>(lines.size()))) {
    LOG_ERR("PTB", "Serialization failed: could not write table cell header");
    return false;
  }
  for (const auto& line : lines) {
    if (!line || !line->serialize(file)) {
      LOG_ERR("PTB", "Serialization failed: invalid table cell line");
      return false;
    }
  }
  return true;
}

bool TableFragmentCell::deserialize(FsFile& file, TableFragmentCell& outCell) {
  uint8_t lineCount = 0;
  if (!serialization::tryReadPod(file, outCell.isHeader) || !serialization::tryReadPod(file, lineCount)) {
    LOG_ERR("PTB", "Deserialization failed: truncated table cell metadata");
    return false;
  }
  if (lineCount > MAX_TABLE_LINES_PER_CELL) {
    LOG_ERR("PTB", "Deserialization failed: cell line count %u exceeds maximum", lineCount);
    return false;
  }

  outCell.lines.clear();
  outCell.lines.reserve(lineCount);
  for (uint8_t i = 0; i < lineCount; i++) {
    auto line = TextBlock::deserialize(file);
    if (!line) {
      LOG_ERR("PTB", "Deserialization failed: invalid table cell line");
      return false;
    }
    outCell.lines.push_back(std::move(line));
  }
  return true;
}

bool TableFragmentRow::serialize(FsFile& file) const {
  if (cells.size() > MAX_TABLE_CELLS_PER_ROW) {
    LOG_ERR("PTB", "Serialization failed: row cell count %u exceeds maximum", static_cast<uint32_t>(cells.size()));
    return false;
  }

  if (!serialization::tryWritePod(file, height) || !serialization::tryWritePod(file, headerSeparator) ||
      !serialization::tryWritePod(file, static_cast<uint8_t>(cells.size()))) {
    LOG_ERR("PTB", "Serialization failed: could not write table row metadata");
    return false;
  }
  for (const auto& cell : cells) {
    if (!cell.serialize(file)) {
      return false;
    }
  }
  return true;
}

bool TableFragmentRow::deserialize(FsFile& file, TableFragmentRow& outRow) {
  uint8_t cellCount = 0;
  if (!serialization::tryReadPod(file, outRow.height) || !serialization::tryReadPod(file, outRow.headerSeparator) ||
      !serialization::tryReadPod(file, cellCount)) {
    LOG_ERR("PTB", "Deserialization failed: truncated table row metadata");
    return false;
  }
  if (cellCount > MAX_TABLE_CELLS_PER_ROW) {
    LOG_ERR("PTB", "Deserialization failed: row cell count %u exceeds maximum", cellCount);
    return false;
  }

  outRow.cells.clear();
  outRow.cells.reserve(cellCount);
  for (uint8_t i = 0; i < cellCount; i++) {
    TableFragmentCell cell;
    if (!TableFragmentCell::deserialize(file, cell)) {
      return false;
    }
    outRow.cells.push_back(std::move(cell));
  }
  return true;
}

uint16_t PageTableFragment::getHeight() const {
  uint16_t total = 1;  // Bottom border.
  for (const auto& row : rows) {
    total = static_cast<uint16_t>(total + row.height);
  }
  return total;
}

void PageTableFragment::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  if (columnCount == 0 || rows.empty() || width < 2) {
    return;
  }

  const int drawX = xPos + xOffset;
  const int drawY = yPos + yOffset;
  const uint16_t totalHeight = getHeight();

  std::vector<int16_t> columnStarts(columnCount + 1);
  for (uint8_t i = 0; i < columnCount; i++) {
    columnStarts[i] = static_cast<int16_t>((static_cast<uint32_t>(width) * i) / columnCount);
  }
  columnStarts[columnCount] = static_cast<int16_t>(width - 1);

  renderer.drawRect(drawX, drawY, width, totalHeight, true);
  for (uint8_t i = 1; i < columnCount; i++) {
    const int x = drawX + columnStarts[i];
    renderer.drawLine(x, drawY, x, drawY + totalHeight - 1, true);
  }

  int currentY = 0;
  for (size_t rowIndex = 0; rowIndex < rows.size(); rowIndex++) {
    const auto& row = rows[rowIndex];

    for (size_t colIndex = 0; colIndex < row.cells.size() && colIndex < columnCount; colIndex++) {
      const auto& cell = row.cells[colIndex];
      const int cellTextX = drawX + columnStarts[colIndex] + cellPadding;
      const int cellTextY = drawY + currentY + cellPadding;

      for (size_t lineIndex = 0; lineIndex < cell.lines.size(); lineIndex++) {
        cell.lines[lineIndex]->render(renderer, fontId, cellTextX,
                                      cellTextY + static_cast<int>(lineIndex) * lineHeight);
      }
    }

    currentY += row.height;
    if (rowIndex + 1 < rows.size()) {
      const int lineWidth = row.headerSeparator ? 2 : 1;
      renderer.drawLine(drawX, drawY + currentY, drawX + width - 1, drawY + currentY, lineWidth, true);
    }
  }
}

bool PageTableFragment::serialize(FsFile& file) {
  if (rows.size() > MAX_TABLE_ROWS_PER_FRAGMENT) {
    LOG_ERR("PTB", "Serialization failed: fragment row count %u exceeds maximum", static_cast<uint32_t>(rows.size()));
    return false;
  }

  if (!serialization::tryWritePod(file, xPos) || !serialization::tryWritePod(file, yPos) ||
      !serialization::tryWritePod(file, width) || !serialization::tryWritePod(file, columnCount) ||
      !serialization::tryWritePod(file, cellPadding) || !serialization::tryWritePod(file, lineHeight) ||
      !serialization::tryWritePod(file, static_cast<uint8_t>(rows.size()))) {
    LOG_ERR("PTB", "Serialization failed: could not write fragment metadata");
    return false;
  }
  for (const auto& row : rows) {
    if (!row.serialize(file)) {
      return false;
    }
  }
  return true;
}

std::unique_ptr<PageTableFragment> PageTableFragment::deserialize(FsFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint16_t width = 0;
  uint8_t columnCount = 0;
  uint8_t cellPadding = 0;
  uint16_t lineHeight = 0;
  uint8_t rowCount = 0;
  if (!serialization::tryReadPod(file, xPos) || !serialization::tryReadPod(file, yPos) ||
      !serialization::tryReadPod(file, width) || !serialization::tryReadPod(file, columnCount) ||
      !serialization::tryReadPod(file, cellPadding) || !serialization::tryReadPod(file, lineHeight) ||
      !serialization::tryReadPod(file, rowCount)) {
    LOG_ERR("PTB", "Deserialization failed: truncated fragment metadata");
    return nullptr;
  }

  if (rowCount == 0 || rowCount > MAX_TABLE_ROWS_PER_FRAGMENT || columnCount == 0 ||
      columnCount > MAX_TABLE_CELLS_PER_ROW || width < 2 || lineHeight == 0) {
    LOG_ERR("PTB", "Deserialization failed: invalid fragment metadata (rows=%u cols=%u width=%u lineHeight=%u)",
            rowCount, columnCount, width, lineHeight);
    return nullptr;
  }

  std::vector<TableFragmentRow> rows;
  rows.reserve(rowCount);
  for (uint8_t i = 0; i < rowCount; i++) {
    TableFragmentRow row;
    if (!TableFragmentRow::deserialize(file, row)) {
      return nullptr;
    }
    rows.push_back(std::move(row));
  }

  auto* fragment =
      new (std::nothrow) PageTableFragment(width, columnCount, cellPadding, lineHeight, std::move(rows), xPos, yPos);
  if (!fragment) {
    LOG_ERR("PTB", "Deserialization failed: could not allocate PageTableFragment");
    return nullptr;
  }
  return std::unique_ptr<PageTableFragment>(fragment);
}

void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset, [](const PageElement&) { return true; });
}

void Page::renderText(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset,
                             [](const PageElement& element) { return element.getTag() != TAG_PageImage; });
}

void Page::renderImages(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset,
                             [](const PageElement& element) { return element.getTag() == TAG_PageImage; });
}

bool Page::serialize(FsFile& file) const {
  const uint16_t count = elements.size();
  if (elements.size() > MAX_PAGE_ELEMENTS) {
    LOG_ERR("PGE", "Serialization failed: element count %u exceeds maximum", static_cast<uint32_t>(elements.size()));
    return false;
  }
  if (!serialization::tryWritePod(file, count)) {
    LOG_ERR("PGE", "Serialization failed: could not write element count");
    return false;
  }

  for (const auto& el : elements) {
    // Use getTag() method to determine type
    if (!serialization::tryWritePod(file, static_cast<uint8_t>(el->getTag()))) {
      LOG_ERR("PGE", "Serialization failed: could not write element tag");
      return false;
    }

    if (!el->serialize(file)) {
      return false;
    }
  }

  // Serialize footnotes (clamp to MAX_FOOTNOTES_PER_PAGE to match addFootnote/deserialize limits)
  const uint16_t fnCount = std::min<uint16_t>(footnotes.size(), MAX_FOOTNOTES_PER_PAGE);
  if (!serialization::tryWritePod(file, fnCount)) {
    LOG_ERR("PGE", "Failed to write footnote count");
    return false;
  }
  for (uint16_t i = 0; i < fnCount; i++) {
    const auto& fn = footnotes[i];
    if (file.write(fn.number, sizeof(fn.number)) != sizeof(fn.number) ||
        file.write(fn.href, sizeof(fn.href)) != sizeof(fn.href)) {
      LOG_ERR("PGE", "Failed to write footnote");
      return false;
    }
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(FsFile& file) {
  auto* rawPage = new (std::nothrow) Page();
  if (!rawPage) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate Page");
    return nullptr;
  }
  auto page = std::unique_ptr<Page>(rawPage);

  uint16_t count;
  if (!serialization::tryReadPod(file, count)) {
    LOG_ERR("PGE", "Deserialization failed: could not read element count");
    return nullptr;
  }
  if (count > MAX_PAGE_ELEMENTS) {
    LOG_ERR("PGE", "Deserialization failed: element count %u exceeds maximum", count);
    return nullptr;
  }
  page->elements.reserve(count);

  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag;
    if (!serialization::tryReadPod(file, tag)) {
      LOG_ERR("PGE", "Deserialization failed: truncated element tag");
      return nullptr;
    }

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      if (!pl) {
        return nullptr;
      }
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      if (!pi) {
        return nullptr;
      }
      page->elements.push_back(std::move(pi));
    } else if (tag == TAG_PageTableFragment) {
      auto fragment = PageTableFragment::deserialize(file);
      if (!fragment) {
        return nullptr;
      }
      page->elements.push_back(std::move(fragment));
    } else if (tag == TAG_PageHorizontalRule) {
      auto rule = PageHorizontalRule::deserialize(file);
      if (!rule) {
        return nullptr;
      }
      page->elements.push_back(std::move(rule));
    } else {
      LOG_ERR("PGE", "Deserialization failed: Unknown tag %u", tag);
      return nullptr;
    }
  }

  // Deserialize footnotes
  uint16_t fnCount;
  if (!serialization::tryReadPod(file, fnCount)) {
    LOG_ERR("PGE", "Failed to read footnote count");
    return nullptr;
  }
  if (fnCount > MAX_FOOTNOTES_PER_PAGE) {
    LOG_ERR("PGE", "Invalid footnote count %u", fnCount);
    return nullptr;
  }
  page->footnotes.resize(fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    auto& entry = page->footnotes[i];
    if (file.read(entry.number, sizeof(entry.number)) != sizeof(entry.number) ||
        file.read(entry.href, sizeof(entry.href)) != sizeof(entry.href)) {
      LOG_ERR("PGE", "Failed to read footnote %u", i);
      return nullptr;
    }
    entry.number[sizeof(entry.number) - 1] = '\0';
    entry.href[sizeof(entry.href) - 1] = '\0';
  }

  return page;
}
