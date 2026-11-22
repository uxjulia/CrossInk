#pragma once

#include <cstddef>
#include <limits>

namespace PageDeserializationBudget {

// Keep a small cushion for allocator metadata and the next deserialization
// object. This is deliberately local to the requested vector allocation so a
// valid dense page is not rejected by a chapter-wide aggregate estimate.
constexpr size_t MIN_FREE_AFTER_RESERVE = 64;
constexpr size_t LARGE_VECTOR_RESERVE = 1024;
constexpr size_t MAX_CACHED_RESERVES = 8;

inline bool canReserveVector(const size_t count, const size_t itemSize, const size_t freeHeap,
                             const size_t largestFreeBlock) {
  if (count == 0) return true;
  if (itemSize == 0 || count > std::numeric_limits<size_t>::max() / itemSize) return false;

  const size_t bytes = count * itemSize;
  return bytes <= largestFreeBlock && bytes <= freeHeap && MIN_FREE_AFTER_RESERVE <= freeHeap - bytes;
}

inline bool hasMinimumSerializedBytes(const size_t available, const size_t count, const size_t minimumItemBytes,
                                      const size_t trailingBytes = 0) {
  if (minimumItemBytes != 0 && count > std::numeric_limits<size_t>::max() / minimumItemBytes) return false;
  const size_t itemBytes = count * minimumItemBytes;
  return itemBytes <= available && trailingBytes <= available - itemBytes;
}

// Reuse one heap shape across small sibling vectors. Heap queries walk allocator
// metadata, so refresh before a large allocation and after a short batch of
// small ones instead of doing two heap queries for every table cell.
class HeapBudget {
 public:
  bool needsRefresh(const size_t count, const size_t itemSize) const {
    if (!initialized_ || reservesSinceRefresh_ >= MAX_CACHED_RESERVES) return true;
    if (itemSize == 0 || count > std::numeric_limits<size_t>::max() / itemSize) return true;
    return count * itemSize >= LARGE_VECTOR_RESERVE;
  }

  void refresh(const size_t freeHeap, const size_t largestFreeBlock) {
    freeHeap_ = freeHeap;
    largestFreeBlock_ = largestFreeBlock;
    reservedSinceRefresh_ = 0;
    reservesSinceRefresh_ = 0;
    initialized_ = true;
  }

  bool canReserve(const size_t count, const size_t itemSize) const {
    if (itemSize == 0 || count > std::numeric_limits<size_t>::max() / itemSize) return false;
    const size_t alreadyReserved = reservedSinceRefresh_;
    if (alreadyReserved > freeHeap_) return false;
    return canReserveVector(count, itemSize, freeHeap_ - alreadyReserved, largestFreeBlock_);
  }

  void recordReserve(const size_t count, const size_t itemSize) {
    if (count == 0 || itemSize == 0 || count > std::numeric_limits<size_t>::max() / itemSize) return;
    const size_t bytes = count * itemSize;
    if (bytes <= std::numeric_limits<size_t>::max() - reservedSinceRefresh_) reservedSinceRefresh_ += bytes;
    ++reservesSinceRefresh_;
  }

  size_t availableAfterReserves() const {
    return reservedSinceRefresh_ <= freeHeap_ ? freeHeap_ - reservedSinceRefresh_ : 0;
  }
  size_t largestFreeBlock() const { return largestFreeBlock_; }

 private:
  size_t freeHeap_ = 0;
  size_t largestFreeBlock_ = 0;
  size_t reservedSinceRefresh_ = 0;
  size_t reservesSinceRefresh_ = 0;
  bool initialized_ = false;
};

}  // namespace PageDeserializationBudget
