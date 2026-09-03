#pragma once

#include <Print.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

class HalFile : public Print {
 public:
  explicit HalFile(std::vector<uint8_t> data) : data_(std::move(data)) {}

  int read() {
    if (position_ >= data_.size()) return -1;
    return data_[position_++];
  }

  int read(void* buffer, size_t size) {
    const size_t available = data_.size() - std::min(position_, data_.size());
    const size_t count = std::min(size, available);
    std::memcpy(buffer, data_.data() + position_, count);
    position_ += count;
    return static_cast<int>(count);
  }

  bool seek(size_t position) {
    if (position > data_.size()) return false;
    position_ = position;
    return true;
  }

  bool seekCur(int64_t offset) {
    if (offset < 0 && static_cast<size_t>(-offset) > position_) return false;
    const size_t next = offset < 0 ? position_ - static_cast<size_t>(-offset) : position_ + static_cast<size_t>(offset);
    return seek(next);
  }

  explicit operator bool() const { return true; }

  size_t write(uint8_t) override { return 0; }

 private:
  std::vector<uint8_t> data_;
  size_t position_ = 0;
};
