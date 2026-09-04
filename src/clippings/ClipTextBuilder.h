#pragma once

#include <cstdint>
#include <vector>

#include "activities/ActivityResult.h"
#include "activities/reader/WordRef.h"

namespace ClipTextBuilder {

struct SelectionBounds {
  uint8_t firstPageIdx = 0;
  uint16_t firstPageWordOrdinal = 0;
  uint16_t firstWordByteOffset = 0;
  uint8_t lastPageIdx = 0;
  uint16_t lastPageWordOrdinal = 0;
  uint16_t lastWordByteEndOffset = 0;
};

ClippingResult build(const ClipWordStore& wordStore, const uint16_t* wordOrder, int fromOrder, int toOrder,
                     int startPageInSection, int sectionPageCount, const SelectionBounds* selectionBounds = nullptr,
                     uint16_t tableSelection = UINT16_MAX);

}  // namespace ClipTextBuilder
