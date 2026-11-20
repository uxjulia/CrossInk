#include "FlashBlobCache.h"

#include <AssetFlashLayout.h>

#if defined(FLASH_BLOB_CACHE_EXTERNAL_BACKEND) || CROSSINK_SHAPING_FLASH_CACHE
#define CROSSPOINT_FLASH_BLOBS 1
#else
#define CROSSPOINT_FLASH_BLOBS 0
#endif

#if CROSSPOINT_FLASH_BLOBS

#include <Logging.h>
#include <Memory.h>

#include <cstring>
#include <memory>
#include <new>

#include "Fnv1a.h"

// Partition access. The device build maps these onto esp_partition; a host
// test can supply its own by defining FLASH_BLOB_CACHE_EXTERNAL_BACKEND.
namespace FlashBlobCache::backend {
bool open(uint32_t* size);
bool read(uint32_t offset, void* buf, uint32_t length);
bool erase(uint32_t offset, uint32_t length);
bool write(uint32_t offset, const void* buf, uint32_t length);
const uint8_t* map(uint32_t offset, uint32_t length, uint32_t* handle);
void unmap(uint32_t handle);
}  // namespace FlashBlobCache::backend

#ifndef FLASH_BLOB_CACHE_EXTERNAL_BACKEND
#include <HalFlashPartition.h>
#include <HyphenationPack.h>

namespace FlashBlobCache::backend {
namespace {
HalFlashPartition partition;
size_t base = 0;
size_t capacity = 0;
HalFlashPartition::Mapping mappings[8];
bool contains(uint32_t offset, uint32_t length) { return offset <= capacity && length <= capacity - offset; }
}  // namespace

bool open(uint32_t* size) {
  if (!asset_flash_layout::hasShapingReservation()) return false;
  if (!partition.begin(true) || partition.size() < asset_flash_layout::LanguageBytes) return false;
  const size_t hyphenationBytes = partition.size() - asset_flash_layout::LanguageBytes;
  capacity = asset_flash_layout::shapingReserve(hyphenationBytes);
  if (!capacity) return false;
  const hyphenation_pack::Flash flash{&partition,
                                      hyphenationBytes,
                                      [](void* ctx, size_t offset, void* bytes, size_t length) {
                                        return static_cast<HalFlashPartition*>(ctx)->read(offset, bytes, length);
                                      },
                                      nullptr,
                                      nullptr,
                                      nullptr};
  // Old firmware could fill this tail with installed patterns. Preserve those
  // bytes and fall back until an install/removal and reboot frees the region.
  if (!hyphenation_pack::tailAvailable(flash, hyphenationBytes / 2, hyphenationBytes / 2, capacity)) {
    LOG_ERR("FBC", "Hyphenation bank tail occupied or invalid; shaping flash unavailable");
    capacity = 0;
    return false;
  }
  base = hyphenationBytes - capacity;
  *size = capacity;
  return true;
}
bool read(uint32_t offset, void* buf, uint32_t length) {
  return contains(offset, length) && partition.read(base + offset, buf, length);
}
bool erase(uint32_t offset, uint32_t length) {
  return contains(offset, length) && partition.erase(base + offset, length);
}
bool write(uint32_t offset, const void* buf, uint32_t length) {
  return contains(offset, length) && partition.write(base + offset, buf, length);
}
const uint8_t* map(uint32_t offset, uint32_t length, uint32_t* handle) {
  if (!contains(offset, length)) return nullptr;
  for (uint32_t i = 0; i < 8; ++i) {
    if (mappings[i].data) continue;
    if (!partition.map(base + offset, length, mappings[i])) return nullptr;
    *handle = i;
    return mappings[i].data;
  }
  return nullptr;
}
void unmap(uint32_t handle) {
  if (handle < 8) HalFlashPartition::unmap(mappings[handle]);
}
}  // namespace FlashBlobCache::backend
#endif

namespace FlashBlobCache {
namespace {

constexpr uint32_t kMagic = 0x32465043;  // "CPF2": 128 KB slots (a "CPFB" directory had 64 KB slots)
constexpr uint32_t kSlotSize = MAX_BLOB_BYTES;
constexpr uint32_t kSlots = 8;  // at most this many slots; the directory always has this many entries
// ComplexShaper keeps two faces live (regular and bold) without PSRAM; fewer
// slots would rewrite flash on every style change.
constexpr uint32_t kMinSlots = 2;
constexpr uint32_t kDirSector = 4096;  // directory at offset 0; slot i at (i + 1) * kSlotSize
constexpr uint32_t kCopyChunk = 4096;

struct DirEntry {
  uint32_t magic;
  uint32_t key;
  uint32_t length;
  uint32_t sequence;  // write order; the lowest idle slot is recycled first
};

struct Mapping {
  const uint8_t* data;
  uint32_t handle;
  uint8_t slot;
};

Mapping gMappings[kSlots] = {};
uint8_t gMappingCount = 0;
int8_t gUsable = -1;      // -1 unchecked, 0 no, 1 yes
uint32_t gSlotCount = 0;  // slots the partition holds, up to kSlots

// Keys whose copy failed verification: the source does not hash to its key
// (a corrupt file, or a bad hash in its header), so copying it again would
// only erase and rewrite a slot on every face build. Oldest entry recycled.
uint32_t gRejected[kSlots] = {};
uint8_t gRejectedCount = 0;
uint8_t gRejectedNext = 0;

bool isRejected(const uint32_t key) {
  for (uint8_t i = 0; i < gRejectedCount; i++) {
    if (gRejected[i] == key) return true;
  }
  return false;
}

void rememberRejected(const uint32_t key) {
  gRejected[gRejectedNext] = key;
  gRejectedNext = static_cast<uint8_t>((gRejectedNext + 1) % kSlots);
  if (gRejectedCount < kSlots) gRejectedCount++;
}

uint32_t slotOffset(const uint32_t slot) { return (slot + 1) * kSlotSize; }

bool usable() {
#ifndef FLASH_BLOB_CACHE_EXTERNAL_BACKEND
  // Boot can load a font before the hyphenation store initializes. Do not cache
  // this refusal: later calls may use its confirmed reservation.
  if (!asset_flash_layout::hasShapingReservation()) return false;
#endif
  if (gUsable < 0) {
    uint32_t size = 0;
    const uint32_t regions = backend::open(&size) ? size / kSlotSize : 0;  // the first holds the directory
    gSlotCount = regions > kSlots ? kSlots : (regions > 0 ? regions - 1 : 0);
    gUsable = gSlotCount >= kMinSlots ? 1 : 0;
    if (gUsable) {
      LOG_DBG("FBC", "Flash partition of %u bytes: %u layout font slots", size, gSlotCount);
    } else {
      LOG_ERR("FBC", "No usable flash partition (%u bytes, needs %u); flash cache disabled", size,
              slotOffset(kMinSlots));
    }
  }
  return gUsable == 1;
}

uint32_t contentKey(const uint8_t* data, const uint32_t length) { return fnv1a::hash(data, length) | 1u; }

bool slotMapped(const uint32_t slot) {
  for (uint8_t i = 0; i < gMappingCount; i++) {
    if (gMappings[i].slot == slot) return true;
  }
  return false;
}

// Maps a slot and checks it still holds the blob; unmaps on mismatch.
const uint8_t* mapVerified(const uint32_t slot, const uint32_t key, const uint32_t length) {
  if (gMappingCount >= kSlots) return nullptr;
  uint32_t handle = 0;
  const uint8_t* data = backend::map(slotOffset(slot), length, &handle);
  if (data == nullptr) return nullptr;
  if (contentKey(data, length) != key) {
    backend::unmap(handle);
    return nullptr;
  }
  gMappings[gMappingCount++] = Mapping{data, handle, static_cast<uint8_t>(slot)};
  return data;
}

// `chunk` holds kCopyChunk bytes.
bool copyIn(const uint32_t slot, const uint32_t length, const Reader read, void* ctx, uint8_t* chunk) {
  if (!backend::erase(slotOffset(slot), kSlotSize)) return false;
  for (uint32_t done = 0; done < length;) {
    const uint32_t n = length - done < kCopyChunk ? length - done : kCopyChunk;
    if (!read(ctx, done, chunk, n) || !backend::write(slotOffset(slot) + done, chunk, n)) return false;
    done += n;
  }
  return true;
}

}  // namespace

const uint8_t* acquire(const uint32_t key, const uint32_t length, const Reader read, void* ctx) {
  if (!canHold(key, length)) return nullptr;

  DirEntry dir[kSlots];
  if (!backend::read(0, dir, sizeof(dir))) return nullptr;

  uint32_t newest = 0;
  for (uint32_t slot = 0; slot < gSlotCount; slot++) {
    if (dir[slot].magic != kMagic) continue;
    if (dir[slot].sequence > newest) newest = dir[slot].sequence;
    if (dir[slot].key == key && dir[slot].length == length && !slotMapped(slot)) {
      if (const uint8_t* data = mapVerified(slot, key, length)) return data;
    }
  }

  // Not cached (or the copy went bad): recycle an empty slot, else the oldest idle one.
  int32_t victim = -1;
  for (uint32_t slot = 0; slot < gSlotCount; slot++) {
    if (slotMapped(slot)) continue;
    if (dir[slot].magic != kMagic) {
      victim = static_cast<int32_t>(slot);
      break;
    }
    if (victim < 0 || dir[slot].sequence < dir[victim].sequence) victim = static_cast<int32_t>(slot);
  }
  if (victim < 0) return nullptr;

  const auto chunk = makeUniqueNoThrow<uint8_t[]>(kCopyChunk);
  if (!chunk) {
    LOG_ERR("FBC", "OOM: %u-byte copy buffer", kCopyChunk);
    return nullptr;
  }
  const auto slot = static_cast<uint32_t>(victim);
  LOG_DBG("FBC", "Copying %u-byte layout font to flash slot %u", length, slot);
  if (!copyIn(slot, length, read, ctx, chunk.get())) {
    // Not retried this session: every attempt erases a slot first.
    LOG_ERR("FBC", "Flash copy to slot %u failed", slot);
    rememberRejected(key);
    return nullptr;
  }
  const uint8_t* data = mapVerified(slot, key, length);
  if (data == nullptr) {
    LOG_ERR("FBC", "Slot %u failed verification after copy", slot);
    rememberRejected(key);
    return nullptr;
  }
  // The entry is recorded only after the data verified, so an interrupted
  // copy leaves the slot unlisted and it is simply rewritten next time.
  dir[slot] = DirEntry{kMagic, key, length, newest + 1};
  if (!backend::erase(0, kDirSector) || !backend::write(0, dir, sizeof(dir))) {
    LOG_ERR("FBC", "Flash directory update failed");
  }
  return data;
}

bool canHold(const uint32_t key, const uint32_t length) {
  return key != 0 && length != 0 && length <= kSlotSize && !isRejected(key) && usable();
}

void release(void* data) {
  for (uint8_t i = 0; i < gMappingCount; i++) {
    if (gMappings[i].data != data) continue;
    backend::unmap(gMappings[i].handle);
    gMappings[i] = gMappings[--gMappingCount];
    return;
  }
}

}  // namespace FlashBlobCache

#else

namespace FlashBlobCache {
const uint8_t* acquire(uint32_t, uint32_t, Reader, void*) { return nullptr; }
void release(void*) {}
bool canHold(uint32_t, uint32_t) { return false; }
}  // namespace FlashBlobCache

#endif
