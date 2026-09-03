#pragma once
#include <HalStorage.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "FootnoteEntry.h"
#include "PageCountEstimator.h"
#include "blocks/ImageBlock.h"
#include "blocks/TextBlock.h"

enum PageElementTag : uint8_t {
  TAG_PageLine = 1,
  TAG_PageImage = 2,  // New tag
  TAG_PageTableFragment = 3,
  TAG_PageHorizontalRule = 4,
};

// represents something that has been added to a page
class PageElement {
 public:
  int16_t xPos;
  int16_t yPos;
  explicit PageElement(const int16_t xPos, const int16_t yPos) : xPos(xPos), yPos(yPos) {}
  virtual ~PageElement() = default;
  virtual void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true) = 0;
  virtual bool serialize(FsFile& file) = 0;
  virtual PageElementTag getTag() const = 0;  // Add type identification
};

// a line from a block element
class PageLine final : public PageElement {
  std::shared_ptr<TextBlock> block;

 public:
  PageLine(std::shared_ptr<TextBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), block(std::move(block)) {}
  const std::shared_ptr<TextBlock>& getBlock() const { return block; }
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true) override;
  bool serialize(FsFile& file) override;
  PageElementTag getTag() const override { return TAG_PageLine; }
  static std::unique_ptr<PageLine> deserialize(FsFile& file);
};

// New PageImage class
class PageImage final : public PageElement {
  std::unique_ptr<ImageBlock> imageBlock;

 public:
  PageImage(std::unique_ptr<ImageBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), imageBlock(std::move(block)) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true) override;
  void renderPlaceholder(GfxRenderer& renderer, int xOffset, int yOffset, bool foregroundBlack) const;
  bool serialize(FsFile& file) override;
  PageElementTag getTag() const override { return TAG_PageImage; }
  static std::unique_ptr<PageImage> deserialize(FsFile& file);
  const ImageBlock& getImageBlock() const { return *imageBlock; }
};

class PageHorizontalRule final : public PageElement {
  uint16_t width;
  uint8_t thickness;

 public:
  PageHorizontalRule(uint16_t width, uint8_t thickness, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), width(width), thickness(thickness) {}

  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true) override;
  bool serialize(FsFile& file) override;
  PageElementTag getTag() const override { return TAG_PageHorizontalRule; }
  static std::unique_ptr<PageHorizontalRule> deserialize(FsFile& file);
};

struct TableFragmentCell {
  static constexpr uint8_t MAX_SERIALIZED_LINES = 64;
  bool isHeader = false;
  uint8_t colSpan = 1;
  std::vector<std::shared_ptr<TextBlock>> lines;

  bool serialize(FsFile& file) const;
  static bool deserialize(FsFile& file, TableFragmentCell& outCell);
};

struct TableFragmentRow {
  static constexpr uint8_t MAX_SERIALIZED_CELLS = 8;
  uint16_t height = 0;
  bool headerSeparator = false;
  std::vector<TableFragmentCell> cells;

  bool serialize(FsFile& file) const;
  static bool deserialize(FsFile& file, TableFragmentRow& outRow);
};

struct PageTextLine {
  const TextBlock* block = nullptr;
  int xPos = 0;
  int yPos = 0;
  int clipX = 0;
  int clipY = 0;
  int clipWidth = 0;
  int clipHeight = 0;
  int lineHeight = 0;
  bool isTableText = false;
  // Identifies a logical table column within this page layout. Normal text and
  // unsupported table shapes use the sentinel, so their selection behavior is
  // unchanged.
  uint16_t tableSelection = UINT16_MAX;
};

using PageTextLineVisitor = bool (*)(const PageTextLine& line, void* context);

class PageTableFragment final : public PageElement {
  friend class Page;

 public:
  static constexpr uint8_t MAX_SERIALIZED_ROWS = 64;

 private:
  uint16_t width;
  uint8_t columnCount;
  uint8_t cellPadding;
  uint16_t lineHeight;
  std::vector<TableFragmentRow> rows;

 public:
  PageTableFragment(uint16_t width, uint8_t columnCount, uint8_t cellPadding, uint16_t lineHeight,
                    std::vector<TableFragmentRow> rows, int16_t xPos, int16_t yPos)
      : PageElement(xPos, yPos),
        width(width),
        columnCount(columnCount),
        cellPadding(cellPadding),
        lineHeight(lineHeight),
        rows(std::move(rows)) {}

  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true) override;
  bool serialize(FsFile& file) override;
  PageElementTag getTag() const override { return TAG_PageTableFragment; }
  static std::unique_ptr<PageTableFragment> deserialize(FsFile& file);
  uint16_t getHeight() const;
};

class Page {
 public:
  struct PublisherPageMarker {
    int16_t yPos = 0;
    char label[16] = {};
  };

  // the list of block index and line numbers on this page
  // Page elements have one owner: their page. Text blocks remain shared by
  // PageLine entries when a laid-out block spans multiple lines or pages.
  std::vector<std::unique_ptr<PageElement>> elements;
  std::vector<FootnoteEntry> footnotes;
  std::vector<PublisherPageMarker> publisherPageMarkers;
  static constexpr uint16_t MAX_FOOTNOTES_PER_PAGE = EPUB_MAX_FOOTNOTES_PER_PAGE;
  static constexpr uint8_t INITIAL_FOOTNOTE_RESERVE = 2;
  static constexpr uint8_t MAX_PUBLISHER_PAGE_MARKERS_PER_PAGE = 8;
  static constexpr uint8_t INITIAL_PUBLISHER_PAGE_MARKER_RESERVE = 2;

  void addFootnote(const char* number, const char* href, const uint8_t linkId = 0) {
    if (linkId != 0 && std::any_of(footnotes.begin(), footnotes.end(),
                                   [linkId](const FootnoteEntry& entry) { return entry.linkId == linkId; })) {
      return;
    }
    if (footnotes.size() >= MAX_FOOTNOTES_PER_PAGE) return;  // Cap per-page footnotes
    if (footnotes.empty()) {
      footnotes.reserve(INITIAL_FOOTNOTE_RESERVE);
    }
    FootnoteEntry entry;
    std::strncpy(entry.number, number, sizeof(entry.number) - 1);
    entry.number[sizeof(entry.number) - 1] = '\0';
    std::strncpy(entry.href, href, sizeof(entry.href) - 1);
    entry.href[sizeof(entry.href) - 1] = '\0';
    entry.linkId = linkId;
    footnotes.push_back(entry);
  }

  void addPublisherPageMarker(const char* label, const int yPos) {
    if (!label || label[0] == '\0' || publisherPageMarkers.size() >= MAX_PUBLISHER_PAGE_MARKERS_PER_PAGE) return;
    if (publisherPageMarkers.empty()) {
      publisherPageMarkers.reserve(INITIAL_PUBLISHER_PAGE_MARKER_RESERVE);
    }
    PublisherPageMarker marker;
    marker.yPos = static_cast<int16_t>(std::clamp(yPos, static_cast<int>(INT16_MIN), static_cast<int>(INT16_MAX)));
    std::strncpy(marker.label, label, sizeof(marker.label) - 1);
    marker.label[sizeof(marker.label) - 1] = '\0';
    publisherPageMarkers.push_back(marker);
  }

  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true) const;
  void renderText(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true) const;
  void renderImages(GfxRenderer& renderer, int fontId, int xOffset, int yOffset, bool foregroundBlack = true) const;
  void renderWithImagePlaceholders(GfxRenderer& renderer, int fontId, int xOffset, int yOffset,
                                   bool foregroundBlack = true) const;
  bool forEachTextLine(PageTextLineVisitor visitor, void* context) const;
  bool serialize(FsFile& file) const;
  static std::unique_ptr<Page> deserialize(FsFile& file);

  // Return the fixed-point page units protected by images on this page. Text
  // pages return zero; image-only pages are one full page (256 units), while
  // mixed pages contribute their visible image-height fraction.
  uint16_t imageEstimateUnits(uint16_t viewportHeight) const;

  // Check if page contains any images (used to force full refresh)
  bool hasImages() const {
    return std::any_of(elements.begin(), elements.end(),
                       [](const std::unique_ptr<PageElement>& el) { return el->getTag() == TAG_PageImage; });
  }

  bool hasImagesNeedingDecode() const {
    return std::any_of(elements.begin(), elements.end(), [](const std::unique_ptr<PageElement>& element) {
      return element->getTag() == TAG_PageImage &&
             static_cast<const PageImage&>(*element).getImageBlock().needsDecode();
    });
  }

  // Get bounding box of all images on the page (union of image rects)
  // Returns false if no images. Coordinates are relative to page origin.
  bool getImageBoundingBox(int16_t& outX, int16_t& outY, int16_t& outW, int16_t& outH) const {
    bool found = false;
    int16_t minX = INT16_MAX, minY = INT16_MAX, maxX = INT16_MIN, maxY = INT16_MIN;
    for (const auto& el : elements) {
      if (el->getTag() == TAG_PageImage) {
        const auto& img = static_cast<const PageImage&>(*el);
        int16_t x = img.xPos;
        int16_t y = img.yPos;
        int16_t right = x + img.getImageBlock().getWidth();
        int16_t bottom = y + img.getImageBlock().getHeight();
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, right);
        maxY = std::max(maxY, bottom);
        found = true;
      }
    }
    if (found) {
      outX = minX;
      outY = minY;
      outW = maxX - minX;
      outH = maxY - minY;
    }
    return found;
  }
};
