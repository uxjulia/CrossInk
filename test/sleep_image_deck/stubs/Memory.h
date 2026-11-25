#pragma once
#include <HalStorage.h>

#include <memory>
#include <new>
#include <type_traits>
#include <utility>
template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  return std::unique_ptr<T>(test_storage::failAllocation ? nullptr : new (std::nothrow) T(std::forward<Args>(args)...));
}
template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  using Elem = std::remove_extent_t<T>;
  test_storage::maxAllocation = std::max(test_storage::maxAllocation, count * sizeof(Elem));
  return std::unique_ptr<T>(test_storage::failAllocation ? nullptr : new (std::nothrow) Elem[count]());
}
