#pragma once

#include <Print.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class GfxRenderer;

class Epub {
 public:
  struct SourceChildRange {
    char name[12] = {};
    uint16_t offset = 0;
    uint16_t count = 0;
  };
  struct SourceSpineMapEntry {
    uint16_t sourceSpineIndex = UINT16_MAX;
    uint16_t firstRange = 0;
    uint8_t rangeCount = 0;
    uint8_t containerDepth = 0;
  };
  struct SpineItem {
    std::string href;
  };

  explicit Epub(std::vector<std::string> spineContents) : spineContents(std::move(spineContents)) {}

  size_t streamChunkSize = 1024;
  bool failRead = false;
  bool sourceMapRequired = false;

  bool sourceMapPresent = false;
  bool hasSourceSpineMap() const { return sourceMapPresent; }
  bool requiresSourceSpineMap() const { return sourceMapRequired; }
  bool getSourceSpineMapEntry(int, SourceSpineMapEntry&) const { return false; }
  const SourceChildRange* getSourceChildRange(const SourceSpineMapEntry&, size_t) const { return nullptr; }
  bool findCurrentSpineForSource(int, uint8_t, const char*, uint16_t, int&, uint16_t&) const { return false; }
  size_t getCumulativeSpineItemSize(int index) const {
    size_t size = 0;
    for (int i = 0; i <= index && i < getSpineItemsCount(); ++i) size += spineContents[i].size();
    return size;
  }
  size_t getBookSize() const { return getCumulativeSpineItemSize(getSpineItemsCount() - 1); }
  float calculateProgress(int index, float intra) const {
    const size_t prior = getCumulativeSpineItemSize(index - 1);
    const size_t total = getBookSize();
    return total ? (prior + intra * spineContents[index].size()) / total : 0.0f;
  }

  int getSpineItemsCount() const { return static_cast<int>(spineContents.size()); }

  SpineItem getSpineItem(const int spineIndex) const {
    if (spineIndex < 0 || spineIndex >= getSpineItemsCount()) return {};
    return {"chapter" + std::to_string(spineIndex) + ".xhtml"};
  }

  bool readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize, bool = false) const {
    if (failRead) return false;
    for (int spineIndex = 0; spineIndex < getSpineItemsCount(); spineIndex++) {
      if (itemHref != getSpineItem(spineIndex).href) continue;
      const auto& contents = spineContents[spineIndex];
      size_t offset = 0;
      while (offset < contents.size()) {
        const size_t size = std::min({chunkSize, streamChunkSize, contents.size() - offset});
        out.write(reinterpret_cast<const uint8_t*>(contents.data() + offset), size);
        offset += size;
      }
      return true;
    }
    return false;
  }

 private:
  std::vector<std::string> spineContents;
};
