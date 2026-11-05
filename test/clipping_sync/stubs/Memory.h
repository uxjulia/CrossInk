#pragma once

#include "../../../lib/Memory/Memory.h"

// Model a fragmented C3 heap: production arrays must fail rather than abort.
inline size_t maxClippingTestArrayAllocation = SIZE_MAX;
template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> clippingTestMakeUniqueNoThrow(Args&&... args) {
  return makeUniqueNoThrow<T>(std::forward<Args>(args)...);
}
template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> clippingTestMakeUniqueNoThrow(size_t count) {
  if (count > maxClippingTestArrayAllocation / sizeof(std::remove_extent_t<T>)) return {};
  return makeUniqueNoThrow<T>(count);
}
#define makeUniqueNoThrow clippingTestMakeUniqueNoThrow
