#pragma once
#include <cstdint>
namespace MemoryBudget {
struct HeapSnapshot {
  uint32_t freeHeap, maxAllocHeap;
};
inline uint32_t available = 1024 * 1024;
inline HeapSnapshot snapshot() { return {available, available}; }
}  // namespace MemoryBudget
