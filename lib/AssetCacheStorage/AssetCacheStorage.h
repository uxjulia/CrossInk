#pragma once

#include <HalFlashPartition.h>

class HalFile;
class AssetCacheReadSession;

// App-owned language/hyphenation cache storage. Compatible flash layouts retain
// mmap; small factory layouts use an SD image and PSRAM snapshots instead.
// Neither backend touches the partition table, bootloader, or application slots.
class AssetCacheStorage {
 public:
  struct Mapping : HalFlashPartition::Mapping {
    bool psram = false;
  };
  bool begin(bool create = false);
  size_t size() const { return sd_ ? SD_SIZE : flash_.size(); }
  bool read(size_t offset, void* bytes, size_t length) const;
  bool write(size_t offset, const void* bytes, size_t length);
  bool erase(size_t offset, size_t length);
  bool verify(size_t offset, const void* bytes, size_t length);
  bool map(size_t offset, size_t length, Mapping& mapping) const;
  static void unmap(Mapping& mapping);
  bool usesSd() const { return sd_; }
  static constexpr const char* SD_PATH = "/.crosspoint/asset-cache.bin";

 private:
  friend class AssetCacheReadSession;
  HalFile* reader_ = nullptr;  // Borrowed only for a scoped startup read session.
  bool erasedRead_ = false;
  static constexpr size_t SD_SIZE = 0x360000;
  bool contains(size_t offset, size_t length) const { return offset <= size() && length <= size() - offset; }
  bool beginSd(bool create);
  bool readInitial(size_t offset, void* bytes, size_t length) const;
  HalFlashPartition flash_;
  bool sd_ = false;
};
