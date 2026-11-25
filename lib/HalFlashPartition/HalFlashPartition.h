#pragma once

#include <cstddef>
#include <cstdint>

// Cold-path access to the firmware-owned spiffs data partition. Existing
// filesystem contents may be replaced. Application slots and the partition
// table are outside this adapter; the caller restricts writes to cache slots.
class HalFlashPartition {
 public:
  struct Mapping {
    const uint8_t* data = nullptr;
    uintptr_t handle = 0;
    size_t size = 0;
  };
  bool begin(bool create = false);
  bool incompatibleLayout() const { return incompatibleLayout_; }
  size_t size() const { return size_; }
  bool read(size_t offset, void* bytes, size_t length) const;
  bool write(size_t offset, const void* bytes, size_t length);
  bool erase(size_t offset, size_t length);
  bool map(size_t offset, size_t length, Mapping& mapping) const;
  static void unmap(Mapping& mapping);
  ~HalFlashPartition();
  HalFlashPartition() = default;
  HalFlashPartition(const HalFlashPartition&) = delete;
  HalFlashPartition& operator=(const HalFlashPartition&) = delete;

 private:
  bool contains(size_t offset, size_t length) const { return offset <= size_ && length <= size_ - offset; }
  bool incompatibleLayout_ = false;
  size_t size_ = 0;
#if defined(SIMULATOR)
  int descriptor_ = -1;
#else
  const void* partition_ = nullptr;
#endif
};
