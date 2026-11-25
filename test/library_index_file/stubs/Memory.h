#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

inline bool failNextObjectAllocation = false;
inline size_t objectAllocationAttempts = 0;

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  ++objectAllocationAttempts;
  if (std::exchange(failNextObjectAllocation, false)) return nullptr;
  return std::make_unique<T>(std::forward<Args>(args)...);
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  return std::make_unique<T>(count);
}
