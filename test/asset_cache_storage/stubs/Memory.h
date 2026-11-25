#pragma once
#include <cstdint>
#include <cstdlib>
#include <memory>
inline bool failSnapshotAllocation = false;
struct HeapByteBufferDeleter {
  void operator()(uint8_t* data) const { std::free(data); }
};
using HeapByteBuffer = std::unique_ptr<uint8_t[], HeapByteBufferDeleter>;
inline bool psramHeapAvailable() { return false; }
inline HeapByteBuffer makeHeapByteBufferNoThrow(size_t length) {
  return HeapByteBuffer(failSnapshotAllocation ? nullptr : static_cast<uint8_t*>(std::malloc(length)));
}
