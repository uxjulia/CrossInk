#include "HalFlashPartition.h"

#include <Logging.h>

#if defined(SIMULATOR)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#else
#include <esp_partition.h>
#endif

bool HalFlashPartition::begin(bool create) {
  incompatibleLayout_ = false;
#if defined(SIMULATOR)
  if (size_ && (!create || descriptor_ >= 0)) return true;
  // A host flash image is separate from the simulated SD card. Tests can
  // select a disposable image without touching normal simulator data.
  const char* path = std::getenv("CROSSINK_LANGUAGE_FLASH");
  if (!path || !*path) {
    if (create) ::mkdir(".cache", 0700);
    path = ".cache/language-flash.bin";
  }
  descriptor_ = ::open(path, O_RDWR);
  // An absent host image represents erased device flash. Read-only validation
  // can inspect its capacity without provisioning or modifying a backing file.
  if (descriptor_ < 0 && !create && errno == ENOENT) {
    size_ = 0x360000;
    return true;
  }
  if (descriptor_ < 0 && create) {
    descriptor_ = ::open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (descriptor_ >= 0) {
      size_ = 0x360000;
      if (!erase(0, size_)) {
        ::close(descriptor_);
        descriptor_ = -1;
        size_ = 0;
        return false;
      }
    }
  }
  if (descriptor_ < 0) return false;
  struct stat status{};
  if (::fstat(descriptor_, &status) != 0 || status.st_size < 0) {
    ::close(descriptor_);
    descriptor_ = -1;
    size_ = 0;
    return false;
  }
  size_ = static_cast<size_t>(status.st_size);
#else
  if (size_) return true;
  (void)create;
  const auto* partition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!partition) {
    incompatibleLayout_ = true;
    LOG_ERR("LANG", "Missing spiffs partition");
    return false;
  }
  LOG_INF("LANG", "spiffs offset=0x%x size=0x%x", unsigned(partition->address), unsigned(partition->size));
  if (partition->encrypted || partition->readonly) {
    LOG_ERR("LANG", "Language data partition unavailable");
    return false;
  }
  partition_ = partition;
  size_ = partition->size;
#endif
  if (size_ < 2 * 65536 || size_ % 65536 != 0) {
    incompatibleLayout_ = true;
    LOG_ERR("LANG", "Unexpected language partition size: %u", static_cast<unsigned>(size_));
    size_ = 0;
#if defined(SIMULATOR)
    ::close(descriptor_);
    descriptor_ = -1;
#endif
    return false;
  }
  return true;
}
bool HalFlashPartition::read(size_t offset, void* bytes, size_t length) const {
  if (!contains(offset, length)) return false;
#if defined(SIMULATOR)
  if (descriptor_ < 0) {
    std::memset(bytes, 0xff, length);
    return true;
  }
  return descriptor_ >= 0 && ::pread(descriptor_, bytes, length, offset) == static_cast<ssize_t>(length);
#else
  return partition_ &&
         esp_partition_read(static_cast<const esp_partition_t*>(partition_), offset, bytes, length) == ESP_OK;
#endif
}
bool HalFlashPartition::write(size_t offset, const void* bytes, size_t length) {
  if (!contains(offset, length)) return false;
#if defined(SIMULATOR)
  uint8_t previous[128];
  const auto* data = static_cast<const uint8_t*>(bytes);
  for (size_t i = 0; i < length; i += sizeof(previous)) {
    const size_t n = std::min(sizeof(previous), length - i);
    if (!read(offset + i, previous, n)) return false;
    for (size_t j = 0; j < n; ++j)
      if ((previous[j] & data[i + j]) != data[i + j]) return false;
  }
  return ::pwrite(descriptor_, bytes, length, offset) == static_cast<ssize_t>(length) && ::fsync(descriptor_) == 0;
#else
  return partition_ &&
         esp_partition_write(static_cast<const esp_partition_t*>(partition_), offset, bytes, length) == ESP_OK;
#endif
}
bool HalFlashPartition::erase(size_t offset, size_t length) {
  if (!contains(offset, length) || offset % 4096 || length % 4096) return false;
#if defined(SIMULATOR)
  uint8_t erased[256];
  std::memset(erased, 0xff, sizeof(erased));
  for (size_t i = 0; i < length; i += sizeof(erased)) {
    const size_t n = std::min(sizeof(erased), length - i);
    if (::pwrite(descriptor_, erased, n, offset + i) != static_cast<ssize_t>(n)) return false;
  }
  return ::fsync(descriptor_) == 0;
#else
  return partition_ &&
         esp_partition_erase_range(static_cast<const esp_partition_t*>(partition_), offset, length) == ESP_OK;
#endif
}
bool HalFlashPartition::map(size_t offset, size_t length, Mapping& mapping) const {
  if (mapping.data || !contains(offset, length)) return false;
#if defined(SIMULATOR)
  void* data = ::mmap(nullptr, length, PROT_READ, MAP_SHARED, descriptor_, offset);
  if (data == MAP_FAILED) return false;
  mapping.data = static_cast<const uint8_t*>(data);
  mapping.handle = reinterpret_cast<uintptr_t>(data);
#else
  const void* data = nullptr;
  esp_partition_mmap_handle_t handle;
  if (!partition_ || esp_partition_mmap(static_cast<const esp_partition_t*>(partition_), offset, length,
                                        ESP_PARTITION_MMAP_DATA, &data, &handle) != ESP_OK)
    return false;
  mapping.data = static_cast<const uint8_t*>(data);
  mapping.handle = handle;
#endif
  mapping.size = length;
  return true;
}
void HalFlashPartition::unmap(Mapping& mapping) {
  if (!mapping.data) return;
#if defined(SIMULATOR)
  ::munmap(const_cast<uint8_t*>(mapping.data), mapping.size);
#else
  esp_partition_munmap(static_cast<esp_partition_mmap_handle_t>(mapping.handle));
#endif
  mapping = {};
}
HalFlashPartition::~HalFlashPartition() {
#if defined(SIMULATOR)
  if (descriptor_ >= 0) ::close(descriptor_);
#endif
}
