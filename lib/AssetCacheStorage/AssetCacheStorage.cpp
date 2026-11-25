#include "AssetCacheStorage.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include "AssetCacheReadSession.h"

#ifndef SIMULATOR
#include <Arduino.h>
#endif

#include <algorithm>
#include <cstring>
#include <mutex>

namespace {
bool hasPsram() {
#if defined(SIMULATOR) && (defined(SIMULATOR_DEVICE_X4_PRO) || defined(SIMULATOR_DEVICE_X4_CLASSIC) || \
                           defined(SIMULATOR_DEVICE_STICKY) || defined(CROSSINK_ASSET_CACHE_TEST_PSRAM))
  return true;
#else
  return psramHeapAvailable();
#endif
}
HeapByteBuffer allocateSnapshot(size_t length) {
#if defined(SIMULATOR)
  // Host backing only for PSRAM-capable profiles (gated by begin).
  return makeHeapByteBufferNoThrow(length);
#else
  return makePsramByteBufferNoThrow(length);
#endif
}
}  // namespace

AssetCacheReadSession::AssetCacheReadSession(AssetCacheStorage& storage) : storage_(storage) {
  if (storage_.reader_) {
    LOG_ERR("ASSET", "Asset cache read session already active");
    return;
  }
  if (!storage_.sd_) {
    ready_ = true;
    return;
  }
  const bool absent = !Storage.exists(AssetCacheStorage::SD_PATH);
  if (!absent) {
    file_ = Storage.open(AssetCacheStorage::SD_PATH);
    if (!file_ || file_.fileSize() != storage_.size()) {
      LOG_ERR("ASSET", "Cannot open SD asset cache read session");
      return;
    }
  }
  storage_.reader_ = &file_;
  storage_.erasedRead_ = absent;
  attached_ = ready_ = true;
}
AssetCacheReadSession::~AssetCacheReadSession() { close(); }
bool AssetCacheReadSession::close() {
  if (attached_) {
    storage_.reader_ = nullptr;
    storage_.erasedRead_ = false;
    attached_ = false;
  }
  ready_ = false;
  if (!file_.close()) {
    LOG_ERR("ASSET", "Cannot close SD asset cache read session");
    return false;
  }
  return true;
}

bool AssetCacheStorage::begin(bool create) {
  if (reader_) {
    LOG_ERR("ASSET", "Cannot reopen asset cache during a read session");
    return false;
  }
  if (sd_) return beginSd(create);
  if (flash_.begin(create)) {
    if (flash_.size() >= SD_SIZE || !hasPsram()) return true;
    // A stock Picco can hold both language slots but its hyphenation banks
    // cannot hold a German pack. Keep the old banks readable until install.
  } else if (!flash_.incompatibleLayout() || !hasPsram()) {
    return false;  // Never mask a flash I/O failure.
  }
  sd_ = true;
  LOG_INF("ASSET", "Small/missing flash partition: using SD cache with PSRAM; partition table unchanged");
  return beginSd(create);
}
bool AssetCacheStorage::beginSd(bool create) {
  // Language and hyphenation adapters share a single staging file.
  static std::mutex provisionLock;
  std::lock_guard<std::mutex> guard(provisionLock);
  if (Storage.exists(SD_PATH)) {
    auto file = Storage.open(SD_PATH);
    const bool valid = file && file.fileSize() == SD_SIZE;
    const bool closed = file.close();
    if (!valid || !closed) LOG_ERR("ASSET", "Cannot read SD asset cache or unexpected size");
    return valid && closed;
  }
  // Read the original flash layout until an explicit install creates the image.
  if (!create) return true;
  if (!Storage.ensureDirectoryExists("/.crosspoint")) {
    LOG_ERR("ASSET", "Cannot create asset cache directory");
    return false;
  }
  constexpr char staging[] = "/.crosspoint/asset-cache.bin.tmp";
  auto file = Storage.open(staging, O_RDWR | O_CREAT | O_TRUNC);
  if (!file) {
    LOG_ERR("ASSET", "Cannot create SD asset cache");
    file.close();
    return false;
  }
  uint8_t chunk[256];
  bool ok = true;
  for (size_t offset = 0; ok && offset < SD_SIZE; offset += sizeof(chunk)) {
    ok = readInitial(offset, chunk, sizeof(chunk)) && file.write(chunk, sizeof(chunk)) == sizeof(chunk);
#ifndef SIMULATOR
    if (offset % 4096 == 0) delay(1);
#endif
  }
  ok = ok && file.sync();
  ok = file.close() && ok;
  ok = ok && Storage.rename(staging, SD_PATH);
  if (!ok) {
    LOG_ERR("ASSET", "Cannot initialize SD asset cache");
    if (!Storage.remove(staging)) LOG_ERR("ASSET", "Cannot remove incomplete SD asset cache");
  }
  return ok;
}
// Project the compact flash layout into the larger SD image. Bank entries use
// offsets relative to their bank and the two 64 KiB language slots sit at the end.
bool AssetCacheStorage::readInitial(size_t offset, void* bytes, size_t length) const {
  auto* out = static_cast<uint8_t*>(bytes);
  std::memset(out, 0xff, length);
  if (!flash_.size()) return true;
  constexpr size_t languageBytes = 2 * 65536;
  constexpr size_t bank = (SD_SIZE - languageBytes) / 2;
  const size_t oldBank = (flash_.size() - languageBytes) / 2;
  const auto copyRegion = [&](size_t start, size_t oldStart, size_t size) {
    const size_t first = std::max(offset, start);
    const size_t last = std::min(offset + length, start + size);
    return first >= last || flash_.read(oldStart + first - start, out + (first - offset), last - first);
  };
  return copyRegion(0, 0, oldBank) && copyRegion(bank, oldBank, oldBank) &&
         copyRegion(2 * bank, 2 * oldBank, languageBytes);
}
bool AssetCacheStorage::read(size_t offset, void* bytes, size_t length) const {
  if (!sd_) return flash_.read(offset, bytes, length);
  if (!contains(offset, length)) return false;
  if (reader_) {
    if (erasedRead_) {
      return readInitial(offset, bytes, length);
    }
    const bool ok = *reader_ && (reader_->position() == offset || reader_->seek(offset)) &&
                    reader_->read(bytes, length) == static_cast<int>(length);
    if (!ok) LOG_ERR("ASSET", "Cannot read SD asset cache session");
    return ok;
  }
  if (!Storage.exists(SD_PATH)) {
    return readInitial(offset, bytes, length);
  }
  auto file = Storage.open(SD_PATH);
  const bool ok = file && file.seek(offset) && file.read(bytes, length) == static_cast<int>(length);
  const bool closed = file.close();
  if (!ok || !closed) LOG_ERR("ASSET", "Cannot read SD asset cache");
  return ok && closed;
}
bool AssetCacheStorage::write(size_t offset, const void* bytes, size_t length) {
  if (reader_) {
    LOG_ERR("ASSET", "Cannot write asset cache during a read session");
    return false;
  }
  if (!sd_) return flash_.write(offset, bytes, length);
  if (!contains(offset, length)) return false;
  auto file = Storage.open(SD_PATH, O_RDWR);
  const bool ok = file && file.seek(offset) && file.write(bytes, length) == length && file.sync();
  const bool closed = file.close();
  if (!ok || !closed) LOG_ERR("ASSET", "Cannot write SD asset cache");
  return ok && closed;
}
bool AssetCacheStorage::erase(size_t offset, size_t length) {
  if (reader_) {
    LOG_ERR("ASSET", "Cannot erase asset cache during a read session");
    return false;
  }
  if (!sd_) return flash_.erase(offset, length);
  if (!contains(offset, length) || offset % 4096 || length % 4096) return false;
  auto file = Storage.open(SD_PATH, O_RDWR);
  bool ok = file && file.seek(offset);
  uint8_t erased[256];
  std::memset(erased, 0xff, sizeof(erased));
  for (size_t done = 0; ok && done < length; done += sizeof(erased))
    ok = file.write(erased, sizeof(erased)) == sizeof(erased);
  ok = ok && file.sync();
  ok = file.close() && ok;
  if (!ok) LOG_ERR("ASSET", "Cannot erase SD asset cache region");
  return ok;
}
bool AssetCacheStorage::verify(size_t offset, const void* bytes, size_t length) {
  if (!contains(offset, length)) return false;
  // Keep one SD reader open for the whole readback; each read otherwise reopens
  // and seeks. The preceding write has already synced and closed its handle.
  AssetCacheReadSession reads(*this);
  if (!reads) return false;
  uint8_t check[256];
  const auto* expected = static_cast<const uint8_t*>(bytes);
  for (size_t done = 0; done < length; done += sizeof(check)) {
    const size_t count = std::min(sizeof(check), length - done);
    if (!read(offset + done, check, count) || std::memcmp(check, expected + done, count)) {
      LOG_ERR("ASSET", "Asset cache readback failed");
      return false;
    }
  }
  return reads.close();
}
bool AssetCacheStorage::map(size_t offset, size_t length, Mapping& mapping) const {
  if (!sd_) return flash_.map(offset, length, mapping);
  if (mapping.data || !length || !contains(offset, length)) return false;
  // Boot-only snapshots outlive all users until restart. Up to 64 KiB per
  // language slot and 1664 KiB for the active hyphenation bank; never C3 RAM.
  auto bytes = allocateSnapshot(length);
  if (!bytes) {
    LOG_ERR("ASSET", "Cannot allocate %u bytes of PSRAM for SD cache", unsigned(length));
    return false;
  }
  if (!read(offset, bytes.get(), length)) return false;
  // Mapping takes ownership, matching the flash mmap/unmap lifetime contract.
  mapping.data = bytes.release();
  mapping.size = length;
  mapping.psram = true;
  return true;
}
void AssetCacheStorage::unmap(Mapping& mapping) {
  if (mapping.psram)
    HeapByteBufferDeleter{}(const_cast<uint8_t*>(mapping.data));
  else
    HalFlashPartition::unmap(mapping);
  mapping = {};
}
