#pragma once

#include <Arena.h>

#include <cstddef>
#include <cstring>
#include <type_traits>

// Small vector-like wrapper for trivially copyable scratch data backed by Arena.
// It never frees individual buffers; the owning Arena is cleared/released at the
// end of the operation. Do not use for std::string, std::vector, or owned objects.
template <typename T>
class ArenaVector {
  static_assert(std::is_trivially_copyable_v<T>, "ArenaVector only supports trivially copyable scratch types");

 public:
  explicit ArenaVector(Arena& arena) : arena_(arena) {}

  bool reserve(const size_t newCapacity) {
    if (newCapacity <= capacity_) {
      return true;
    }
    auto* next = static_cast<T*>(arena_.alloc(sizeof(T) * newCapacity, alignof(T)));
    if (!next) {
      return false;
    }
    if (data_ && size_ > 0) {
      std::memcpy(next, data_, sizeof(T) * size_);
    }
    data_ = next;
    capacity_ = newCapacity;
    return true;
  }

  bool resize(const size_t newSize) {
    if (!reserve(newSize)) {
      return false;
    }
    if (newSize > size_) {
      std::memset(data_ + size_, 0, sizeof(T) * (newSize - size_));
    }
    size_ = newSize;
    return true;
  }

  bool push_back(const T& value) {
    if (size_ == capacity_) {
      const size_t nextCapacity = capacity_ == 0 ? 8 : capacity_ * 2;
      if (!reserve(nextCapacity)) {
        return false;
      }
    }
    data_[size_++] = value;
    return true;
  }

  bool insert(const size_t index, const T& value) {
    if (index > size_) {
      return false;
    }
    if (size_ == capacity_) {
      const size_t nextCapacity = capacity_ == 0 ? 8 : capacity_ * 2;
      if (!reserve(nextCapacity)) {
        return false;
      }
    }
    if (index < size_) {
      std::memmove(data_ + index + 1, data_ + index, sizeof(T) * (size_ - index));
    }
    data_[index] = value;
    ++size_;
    return true;
  }

  void clear() { size_ = 0; }
  void resetStorage() {
    data_ = nullptr;
    size_ = 0;
    capacity_ = 0;
  }

  [[nodiscard]] bool empty() const { return size_ == 0; }
  [[nodiscard]] size_t size() const { return size_; }
  [[nodiscard]] size_t capacity() const { return capacity_; }

  T* data() { return data_; }
  const T* data() const { return data_; }
  T* begin() { return data_; }
  T* end() { return data_ ? data_ + size_ : nullptr; }
  const T* begin() const { return data_; }
  const T* end() const { return data_ ? data_ + size_ : nullptr; }

  T& operator[](const size_t index) { return data_[index]; }
  const T& operator[](const size_t index) const { return data_[index]; }

 private:
  Arena& arena_;
  T* data_ = nullptr;
  size_t size_ = 0;
  size_t capacity_ = 0;
};
