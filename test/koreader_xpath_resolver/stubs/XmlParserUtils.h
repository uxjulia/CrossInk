#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "../../../lib/XmlParserUtils/XmlParserUtils.h"

// Exercise the real Expat parser with a bounded allocator, independent of the
// host's plentiful heap. This accounts for Expat only, not ZIP or Wi-Fi memory.
namespace XmlTestHeap {
struct alignas(std::max_align_t) Allocation {
  size_t size;
};
inline size_t live = 0;
inline size_t peak = 0;
inline size_t limit = SIZE_MAX;

inline void* allocate(size_t size) {
  if (size > limit - live) return nullptr;
  auto* block = static_cast<Allocation*>(std::malloc(sizeof(Allocation) + size));
  if (!block) return nullptr;
  block->size = size;
  live += size;
  if (live > peak) peak = live;
  return block + 1;
}

inline void release(void* ptr) {
  if (!ptr) return;
  auto* block = static_cast<Allocation*>(ptr) - 1;
  live -= block->size;
  std::free(block);
}

inline void* reallocate(void* ptr, size_t size) {
  if (!ptr) return allocate(size);
  auto* block = static_cast<Allocation*>(ptr) - 1;
  const size_t retained = live - block->size;
  if (size > limit - retained) return nullptr;
  auto* resized = static_cast<Allocation*>(std::realloc(block, sizeof(Allocation) + size));
  if (!resized) return nullptr;
  resized->size = size;
  live = retained + size;
  if (live > peak) peak = live;
  return resized + 1;
}

inline XML_Parser create(const XML_Char* encoding) {
  static const XML_Memory_Handling_Suite suite = {allocate, reallocate, release};
  return XML_ParserCreate_MM(encoding, &suite, nullptr);
}
}  // namespace XmlTestHeap

#define XML_ParserCreate XmlTestHeap::create
