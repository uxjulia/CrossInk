#pragma once

#include <Serialization.h>

#include <cstdint>
#include <utility>

#include "SectionPageIndex.h"

struct SectionPageIndexOffsets {
  uint32_t page = 0;
  uint32_t anchorMap = 0;
  uint32_t paragraph = 0;
  uint32_t listItem = 0;
  uint32_t visibleText = 0;
};

// Writes section-cache lookup tables field by field. The chunked index's
// in-memory ownership is deliberately not part of the persisted format.
template <typename AnchorMapWriter>
bool writeSectionPageIndex(FsFile& file, const SectionPageIndex& index, AnchorMapWriter&& writeAnchorMap,
                           SectionPageIndexOffsets& offsets) {
  if (index.size() > SectionPageIndex::kMaxEntries) return false;

  offsets.page = static_cast<uint32_t>(file.position());
  for (size_t i = 0; i < index.size(); ++i) {
    if (index[i].fileOffset == 0 || !serialization::tryWritePod(file, index[i].fileOffset)) return false;
  }

  offsets.anchorMap = static_cast<uint32_t>(file.position());
  if (!std::forward<AnchorMapWriter>(writeAnchorMap)(file)) return false;

  offsets.paragraph = static_cast<uint32_t>(file.position());
  const uint16_t count = static_cast<uint16_t>(index.size());
  if (!serialization::tryWritePod(file, count)) return false;
  for (size_t i = 0; i < index.size(); ++i) {
    if (!serialization::tryWritePod(file, index[i].paragraphIndex)) return false;
  }

  offsets.listItem = static_cast<uint32_t>(file.position());
  for (size_t i = 0; i < index.size(); ++i) {
    if (!serialization::tryWritePod(file, index[i].listItemIndex)) return false;
  }

  offsets.visibleText = static_cast<uint32_t>(file.position());
  for (size_t i = 0; i < index.size(); ++i) {
    if (!serialization::tryWritePod(file, index[i].visibleTextOffset)) return false;
  }
  for (size_t i = 0; i < index.size(); ++i) {
    if (!serialization::tryWritePod(file, index[i].referenceTextOffset)) return false;
  }
  return true;
}
