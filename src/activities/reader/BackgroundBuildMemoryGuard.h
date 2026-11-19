#pragma once

#include <MemoryBudget.h>

// One reclaim attempt per continuous pressure episode, not once per idle tick.
// A healthy observation on a later tick re-arms the guard; a successful reclaim
// itself must not re-arm it, since the next build may immediately consume it.
class BackgroundBuildMemoryGuard {
 public:
  static bool hasHeap(const MemoryBudget::HeapSnapshot heap) {
    return MemoryBudget::hasHeap(heap, MemoryBudget::EPUB_TEXT_LAYOUT_MIN_FREE,
                                 MemoryBudget::EPUB_TEXT_LAYOUT_MIN_MAX_ALLOC);
  }

  bool shouldReclaim(const MemoryBudget::HeapSnapshot heap) {
    if (hasHeap(heap)) {
      reclaimAttempted = false;
      return false;
    }
    if (reclaimAttempted) return false;
    reclaimAttempted = true;
    return true;
  }

 private:
  bool reclaimAttempted = false;
};
