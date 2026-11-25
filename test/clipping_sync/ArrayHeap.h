#pragma once
#include <cstddef>
// Count simultaneously live new[] bytes during an import, including index
// replacement overlap and rewrite scratch (not STL or ArduinoJson allocations).
namespace arrayHeap {
inline bool tracking = false;
inline size_t live = 0, peak = 0, limit = static_cast<size_t>(-1);
}  // namespace arrayHeap
