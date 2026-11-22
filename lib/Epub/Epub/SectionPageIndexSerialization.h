#pragma once

#include <PrintSerialization.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

#include "SectionPageIndex.h"

struct SectionPageIndexOffsets {
  uint32_t page = 0;
  uint32_t anchorMap = 0;
  uint32_t paragraph = 0;
  uint32_t listItem = 0;
  uint32_t visibleText = 0;
};

// Find the first page whose sorted LUT value is >= target. Reading one small
// run at a time avoids a storage seek and HAL read for every 16-bit entry.
inline std::optional<uint16_t> findSectionPageAtOrAfter(FsFile& file, const uint32_t entriesOffset,
                                                        const uint16_t count, const uint16_t target) {
  if (count == 0 || !file.seek(entriesOffset)) return std::nullopt;

  constexpr uint16_t CHUNK_ENTRIES = 64;
  uint16_t entries[CHUNK_ENTRIES];  // 128 bytes on stack, independent of chapter length.
  uint32_t start = 0;
  while (start < count) {
    const auto chunkCount = static_cast<uint16_t>(std::min<uint32_t>(CHUNK_ENTRIES, count - start));
    const size_t chunkBytes = static_cast<size_t>(chunkCount) * sizeof(uint16_t);
    if (file.read(entries, chunkBytes) != static_cast<int>(chunkBytes)) {
      return std::nullopt;
    }
    for (uint16_t i = 0; i < chunkCount; ++i) {
      if (entries[i] >= target) return static_cast<uint16_t>(start + i);
    }
    start += chunkCount;
  }

  // Match the historic behavior: targets after the final LUT value map to the
  // last page in the section.
  return static_cast<uint16_t>(count - 1);
}

// Writes section-cache lookup tables field by field. The chunked index's
// in-memory ownership is deliberately not part of the persisted format.
template <typename AnchorMapWriter>
bool writeSectionPageIndex(FsFile& file, const SectionPageIndex& index, AnchorMapWriter&& writeAnchorMap,
                           SectionPageIndexOffsets& offsets, uint8_t* stage = nullptr, const size_t stageCapacity = 0) {
  if (index.size() > SectionPageIndex::kMaxEntries) return false;

  serialization::BufferedFilePrint output(file, stage, stageCapacity);
  offsets.page = static_cast<uint32_t>(file.position());
  for (size_t i = 0; i < index.size(); ++i) {
    if (index[i].fileOffset == 0 || !serialization::tryWritePod(output, index[i].fileOffset)) return false;
  }
  if (!output.commit()) return false;

  offsets.anchorMap = static_cast<uint32_t>(file.position());
  if (!std::forward<AnchorMapWriter>(writeAnchorMap)(output) || !output.commit()) return false;

  offsets.paragraph = static_cast<uint32_t>(file.position());
  const uint16_t count = static_cast<uint16_t>(index.size());
  if (!serialization::tryWritePod(output, count)) return false;
  for (size_t i = 0; i < index.size(); ++i) {
    if (!serialization::tryWritePod(output, index[i].paragraphIndex)) return false;
  }
  if (!output.commit()) return false;

  offsets.listItem = static_cast<uint32_t>(file.position());
  for (size_t i = 0; i < index.size(); ++i) {
    if (!serialization::tryWritePod(output, index[i].listItemIndex)) return false;
  }
  if (!output.commit()) return false;

  offsets.visibleText = static_cast<uint32_t>(file.position());
  for (size_t i = 0; i < index.size(); ++i) {
    if (!serialization::tryWritePod(output, index[i].visibleTextOffset)) return false;
  }
  for (size_t i = 0; i < index.size(); ++i) {
    if (!serialization::tryWritePod(output, index[i].referenceTextOffset)) return false;
  }
  return output.commit();
}
