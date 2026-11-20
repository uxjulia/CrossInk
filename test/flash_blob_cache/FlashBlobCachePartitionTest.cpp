#include <AssetFlashLayout.h>
#include <FlashBlobCache.h>
#include <Fnv1a.h>
#include <HalFlashPartition.h>
#include <HyphenationPack.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
void put32(uint8_t* out, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) out[i] = value >> (8 * i);
}
std::vector<uint8_t> pack(const char* code, size_t size) {
  std::vector<uint8_t> bytes(hyphenation_pack::PACK_HEADER_SIZE + size, 0x56);
  std::fill_n(bytes.begin(), hyphenation_pack::PACK_HEADER_SIZE, 0);
  std::memcpy(bytes.data(), "CPHY\1", 5);
  std::memcpy(bytes.data() + 5, code, 2);
  bytes[7] = bytes[8] = 2;
  put32(bytes.data() + 12, size - 1);
  put32(bytes.data() + 16, size);
  put32(bytes.data() + 20, hyphenation_pack::crc32(bytes.data() + hyphenation_pack::PACK_HEADER_SIZE, size));
  return bytes;
}
bool source(void* ctx, size_t offset, void* out, size_t size) {
  const auto& bytes = *static_cast<std::vector<uint8_t>*>(ctx);
  if (offset > bytes.size() || size > bytes.size() - offset) return false;
  std::memcpy(out, bytes.data() + offset, size);
  return true;
}
bool blobSource(void* ctx, uint32_t offset, uint8_t* out, uint32_t size) { return source(ctx, offset, out, size); }
}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  char path[] = "/tmp/crossink-shaping-flash-XXXXXX";
  const int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);
  unlink(path);
  assert(setenv("CROSSINK_LANGUAGE_FLASH", path, 1) == 0);
#if !CROSSINK_SHAPING_FLASH_CACHE
  assert(asset_flash_layout::shapingReserve(0x340000) == 0);
  assert(!FlashBlobCache::canHold(1, 100));
  assert(!FlashBlobCache::acquire(1, 100, blobSource, nullptr));
  assert(access(path, F_OK) != 0);
  std::puts("PASS PSRAM profile leaves shaping flash unused");
  return 0;
#endif
  HalFlashPartition flash;
  assert(flash.begin(true));
  const size_t capacity = flash.size() - asset_flash_layout::LanguageBytes;
  const size_t reserve = asset_flash_layout::shapingReserve(capacity);
  assert(reserve == 384 * 1024);
  const size_t base = capacity - reserve;
  const hyphenation_pack::Flash region{
      &flash,
      capacity,
      [](void* p, size_t off, void* out, size_t n) { return static_cast<HalFlashPartition*>(p)->read(off, out, n); },
      [](void* p, size_t off, const void* in, size_t n) {
        return static_cast<HalFlashPartition*>(p)->write(off, in, n);
      },
      [](void* p, size_t off, size_t n) { return static_cast<HalFlashPartition*>(p)->erase(off, n); },
      nullptr};
  const auto supported = [](const char*, uint8_t, uint8_t) { return true; };
  const auto install = [&](const char* code, size_t bytes, size_t tail) {
    hyphenation_pack::Store store;
    assert(store.begin(region, supported, tail) == hyphenation_pack::Result::Ok);
    auto input = pack(code, bytes);
    assert(store.install({&input, input.size(), source}) == hyphenation_pack::Result::Ok);
  };
  std::vector<uint8_t> language(asset_flash_layout::LanguageBytes, 0xa5);
  assert(flash.write(capacity, language.data(), language.size()));
  std::vector<uint8_t> blob(79000);
  for (size_t i = 0; i < blob.size(); ++i) blob[i] = uint8_t(i * 17 + 5);
  const uint32_t key = fnv1a::hash(blob.data(), blob.size()) | 1u;

  if (!std::strcmp(argv[1], "occupied")) {
    // An older six-pack bank occupies the future cache tail. Both banks and
    // language slots must survive the attempted cache claim byte for byte.
    for (const char* code : {"de", "fr", "es", "ru", "pl", "it"}) install(code, 240 * 1024, 0);
    std::vector<uint8_t> before(flash.size()), after(flash.size());
    assert(flash.read(0, before.data(), before.size()));
    assert(!hyphenation_pack::tailAvailable(region, capacity / 2, capacity / 2, reserve));
    asset_flash_layout::confirmShapingReservation(reserve);
    assert(!FlashBlobCache::canHold(key, blob.size()));
    assert(!FlashBlobCache::acquire(key, blob.size(), blobSource, &blob));
    assert(flash.read(0, after.data(), after.size()));
    assert(before == after);
    hyphenation_pack::Store old;
    assert(old.begin(region, supported, reserve) == hyphenation_pack::Result::Ok);
    assert(old.count() == 6);
  } else if (!std::strcmp(argv[1], "corrupt")) {
    const uint8_t invalid = 0;
    assert(flash.write(capacity / 2, &invalid, 1));
    asset_flash_layout::confirmShapingReservation(reserve);
    assert(!FlashBlobCache::canHold(key, blob.size()));
    assert(!FlashBlobCache::acquire(key, blob.size(), blobSource, &blob));
  } else if (!std::strcmp(argv[1], "transient")) {
    auto unavailable = region;
    unavailable.read = [](void*, size_t, void*, size_t) { return false; };
    assert(!hyphenation_pack::tailAvailable(unavailable, capacity / 2, capacity / 2, reserve));
    hyphenation_pack::Store live;
    assert(live.begin(region, supported, 0) == hyphenation_pack::Result::Ok);
    asset_flash_layout::confirmShapingReservation(live.reservedTailBytes());
    assert(hyphenation_pack::tailAvailable(region, capacity / 2, capacity / 2, reserve));
    assert(live.begin(region, supported, reserve) == hyphenation_pack::Result::Unchanged);
    asset_flash_layout::confirmShapingReservation(live.reservedTailBytes());
    // The read recovered, but this session's writer still owns the full bank.
    assert(!FlashBlobCache::canHold(key, blob.size()));
    assert(!FlashBlobCache::acquire(key, blob.size(), blobSource, &blob));
  } else if (!std::strcmp(argv[1], "shrink")) {
    for (const char* code : {"de", "fr", "es", "ru", "pl", "it"}) install(code, 240 * 1024, 0);
    assert(!hyphenation_pack::tailAvailable(region, capacity / 2, capacity / 2, reserve));
    for (const char* code : {"de", "fr"}) {
      hyphenation_pack::Store live;
      assert(live.begin(region, supported, 0) == hyphenation_pack::Result::Ok);
      asset_flash_layout::confirmShapingReservation(live.reservedTailBytes());
      assert(live.remove(code) == hyphenation_pack::Result::Ok);
    }
    assert(hyphenation_pack::tailAvailable(region, capacity / 2, capacity / 2, reserve));
    // Removing the last overlap cannot authorize a cache mapping mid-session.
    assert(!FlashBlobCache::canHold(key, blob.size()));
    assert(!FlashBlobCache::acquire(key, blob.size(), blobSource, &blob));
  } else {
    // A font can be loaded early in boot. Refusing it before confirmation must
    // not permanently disable the later, correctly reserved cache.
    assert(!FlashBlobCache::canHold(key, blob.size()));
    hyphenation_pack::Store live;
    assert(live.begin(region, supported, reserve) == hyphenation_pack::Result::Ok);
    asset_flash_layout::confirmShapingReservation(live.reservedTailBytes());
    install("de", 90000, reserve);
    install("fr", 80000, reserve);
    std::vector<uint8_t> before(base), after(base);
    assert(flash.read(0, before.data(), before.size()));
    assert(FlashBlobCache::canHold(key, blob.size()));
    const auto* mapped = FlashBlobCache::acquire(key, blob.size(), blobSource, &blob);
    assert(mapped && !std::memcmp(mapped, blob.data(), blob.size()));
    assert(flash.read(0, after.data(), after.size()));
    assert(before == after);
    // An install into each bank must leave the live mapping and both slots intact.
    std::vector<uint8_t> cached(reserve), updated(reserve);
    assert(flash.read(base, cached.data(), cached.size()));
    install("es", 60000, reserve);
    install("ru", 50000, reserve);
    assert(!std::memcmp(mapped, blob.data(), blob.size()));
    assert(flash.read(base, updated.data(), updated.size()));
    assert(cached == updated);
    FlashBlobCache::release(const_cast<uint8_t*>(mapped));
    const auto* reused = FlashBlobCache::acquire(key, blob.size(), blobSource, &blob);
    assert(reused && !std::memcmp(reused, blob.data(), blob.size()));
    FlashBlobCache::release(const_cast<uint8_t*>(reused));
    hyphenation_pack::Store store;
    assert(store.begin(region, supported, reserve) == hyphenation_pack::Result::Ok);
    assert(store.count() == 4);
  }
  std::vector<uint8_t> check(language.size());
  assert(flash.read(capacity, check.data(), check.size()));
  assert(check == language);
  unlink(path);
  std::printf("PASS shaping flash coexistence (%s)\n", argv[1]);
}
