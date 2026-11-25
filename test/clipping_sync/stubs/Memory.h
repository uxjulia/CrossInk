#pragma once

#include "../../../lib/Memory/Memory.h"

inline bool clippingTestPsramAvailable = false;
inline size_t maxClippingTestByteAllocation = SIZE_MAX;
inline HeapByteBuffer clippingTestMakePsramByteBuffer(size_t count) {
  if (!clippingTestPsramAvailable || count > maxClippingTestByteAllocation) return {};
  return makeInternalByteBufferNoThrow(count);
}
inline HeapByteBuffer clippingTestMakeInternalByteBuffer(size_t count) {
  if (count > maxClippingTestByteAllocation) return {};
  return makeInternalByteBufferNoThrow(count);
}
#define makePsramByteBufferNoThrow clippingTestMakePsramByteBuffer
#define makeInternalByteBufferNoThrow clippingTestMakeInternalByteBuffer

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
