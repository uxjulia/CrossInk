#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace EpubNavigation {

template <typename Entry>
bool hasResolvableReferencePageRanges(const uint32_t totalUnits, const Entry* entries, const size_t entryCount) {
  if (!entries || entryCount == 0 || totalUnits == 0) return false;
  for (size_t i = 0; i < entryCount; ++i) {
    if (entries[i].wordCount > 0 && entries[i].wordStart < totalUnits) return true;
  }
  return false;
}

template <typename Entry>
bool resolveReferencePageToSpineProgress(const uint32_t requestedPage, const uint32_t totalPages,
                                         const uint32_t totalUnits, const uint32_t unitsPerPage, const Entry* entries,
                                         const size_t entryCount, int& spineIndex, float& spineProgress,
                                         uint32_t* spineUnitOffset = nullptr, uint32_t* spineUnitCount = nullptr) {
  if (!entries || entryCount == 0 || totalPages == 0 || totalUnits == 0 || unitsPerPage == 0) return false;

  const uint32_t page = std::clamp<uint32_t>(requestedPage, 1, totalPages);
  const uint64_t requestedUnit = static_cast<uint64_t>(page - 1) * unitsPerPage;
  const uint32_t targetUnit = static_cast<uint32_t>(std::min<uint64_t>(requestedUnit, totalUnits - 1));
  int lastValidSpine = -1;

  for (size_t i = 0; i < entryCount; ++i) {
    const Entry& entry = entries[i];
    if (entry.wordCount == 0 || entry.wordStart >= totalUnits) continue;

    const uint32_t availableUnits = totalUnits - entry.wordStart;
    const uint32_t entryUnits = std::min(entry.wordCount, availableUnits);
    const uint64_t entryEnd = static_cast<uint64_t>(entry.wordStart) + entryUnits;
    lastValidSpine = static_cast<int>(i);

    if (targetUnit < entry.wordStart) {
      // A malformed manifest left a gap. Land at the first readable content
      // after the requested boundary instead of producing an invalid target.
      spineIndex = static_cast<int>(i);
      spineProgress = 0.0f;
      if (spineUnitOffset) *spineUnitOffset = 0;
      if (spineUnitCount) *spineUnitCount = entryUnits;
      return true;
    }
    if (targetUnit < entryEnd) {
      spineIndex = static_cast<int>(i);
      const uint32_t relativeUnit = targetUnit - entry.wordStart;
      spineProgress = static_cast<float>(relativeUnit) / static_cast<float>(entryUnits);
      if (spineUnitOffset) *spineUnitOffset = relativeUnit;
      if (spineUnitCount) *spineUnitCount = entryUnits;
      return true;
    }
  }

  if (lastValidSpine < 0) return false;
  // An inconsistent explicit page total may extend past the available units.
  // The final readable position is still a safe fallback.
  spineIndex = lastValidSpine;
  spineProgress = 1.0f;
  const Entry& lastEntry = entries[static_cast<size_t>(lastValidSpine)];
  const uint32_t lastUnitCount = std::min(lastEntry.wordCount, totalUnits - lastEntry.wordStart);
  if (spineUnitOffset) *spineUnitOffset = lastUnitCount;
  if (spineUnitCount) *spineUnitCount = lastUnitCount;
  return true;
}

template <typename PageIndex>
uint16_t resolveReferenceTargetToRenderedPage(const uint32_t spineUnitOffset, const uint32_t spineUnitCount,
                                              const uint32_t visibleTextLength, const PageIndex& pageIndex) {
  if (pageIndex.empty() || spineUnitCount == 0 || visibleTextLength == 0) return 0;

  const uint32_t clampedUnitOffset = std::min(spineUnitOffset, spineUnitCount);
  const uint32_t targetVisibleOffset =
      static_cast<uint32_t>((static_cast<uint64_t>(clampedUnitOffset) * visibleTextLength) / spineUnitCount);
  uint16_t page = 0;
  for (size_t i = 1; i < pageIndex.size(); ++i) {
    if (pageIndex[i].visibleTextOffset > targetVisibleOffset) break;
    page = static_cast<uint16_t>(i);
  }
  return page;
}

}  // namespace EpubNavigation
