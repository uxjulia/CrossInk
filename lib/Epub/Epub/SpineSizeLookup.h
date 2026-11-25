#pragma once

#include <cstddef>

// Cumulative spine sizes are nondecreasing, including empty chapters. Find the
// first end offset >= the target, matching the reader's historical boundary
// choice without reading every preceding chapter from SD. No scratch storage.
template <typename ReadCumulativeSize>
int findSpineForSize(const int count, const size_t target, ReadCumulativeSize&& readSize) {
  if (count <= 0) return -1;
  int first = 0;
  int last = count;
  while (first < last) {
    const int middle = first + (last - first) / 2;
    if (readSize(middle) < target) {
      first = middle + 1;
    } else {
      last = middle;
    }
  }
  return first < count ? first : count - 1;
}
