#include "HyphenationPackStore.h"

#include <Arduino.h>
#include <AssetCacheReadSession.h>
#include <AssetCacheStorage.h>
#if defined(ESP_PLATFORM)
#include <BoardConfig.h>
#endif
#include <HalStorage.h>
#include <LanguageCache.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace HyphenationPackStore {
namespace {
AssetCacheStorage flash;
AssetCacheStorage::Mapping mapping;
hyphenation_pack::Store store;
bool initialized = false;

// Same partition, separate ownership: the last two slots belong to UI I18n.
size_t capacity() {
  return flash.size() < 2 * language_cache::SLOT_SIZE ? 0 : language_cache::slotOffset(flash.size(), 0);
}
bool contains(size_t offset, size_t size) { return offset <= capacity() && size <= capacity() - offset; }
bool readFlash(void*, size_t offset, void* bytes, size_t size) {
  return contains(offset, size) && flash.read(offset, bytes, size);
}
bool writeFlash(void*, size_t offset, const void* bytes, size_t size) {
  return contains(offset, size) && flash.write(offset, bytes, size);
}
bool eraseFlash(void*, size_t offset, size_t size) { return contains(offset, size) && flash.erase(offset, size); }
bool supported(const char* code, uint8_t prefix, uint8_t suffix) {
  const auto* language = findLanguageEntry(code);
  return language && !language->hyphenator && prefix == 2 && suffix == 2;
}
bool localSupported(const char* code, uint8_t prefix, uint8_t suffix) {
  return code && code[0] >= 'a' && code[0] <= 'z' && code[1] >= 'a' && code[1] <= 'z' && !code[2] &&
         std::strcmp(code, "en") && prefix == 2 && suffix == 2;
}
bool initialize(bool create) {
  // Simulator read-only initialization can represent an erased, unprovisioned
  // image. Only an explicit install may create its backing storage later.
  if (initialized) return !create || flash.begin(true);
  if (!flash.begin(create)) return false;
  AssetCacheReadSession reads(flash);
  if (!reads) {
    LOG_ERR("HYPH", "Cannot read installed pack cache");
    return false;
  }
  const size_t regionSize = capacity();
  size_t localReservedTail = 0;
#if defined(ESP_PLATFORM) && FREEINK_MCU_C3
  // Development's C3 Indic cache can claim the end of bank 1 when the
  // legacy prefix fits below it. Keep optional data out of that tail on
  // release so switching to existing development can't overwrite it.
  constexpr size_t C3ShapingBytes = 384 * 1024;
  if (!flash.usesSd() && regionSize >= 4 * C3ShapingBytes) localReservedTail = C3ShapingBytes;
#endif
  const auto result = store.begin({nullptr, regionSize, readFlash, writeFlash, eraseFlash, [] { delay(1); }}, supported,
                                  localSupported, localReservedTail);
  if (result != hyphenation_pack::Result::Ok && result != hyphenation_pack::Result::Unchanged) {
    LOG_ERR("HYPH", "%s", hyphenation_pack::resultName(result));
    return false;
  }
  if (store.count() && !flash.map(store.bankOffset(), store.usedBytes(), mapping)) {
    LOG_ERR("HYPH", "Cannot map installed patterns");
    return false;
  }
  initialized = true;
  LOG_INF("HYPH", "Mapped %u packs (%u bytes)", unsigned(store.count()), unsigned(mapping.size));
  return true;
}
bool sourcePath(const char* code, char (&path)[64]) {
  if (!localSupported(code, 2, 2)) return false;
  std::snprintf(path, sizeof(path), "%s/hyph-%s.cphyph", DIRECTORY, code);
  return true;
}
bool readInput(void* context, size_t offset, void* bytes, size_t size) {
  auto* file = static_cast<FsFile*>(context);
  return file->seek(offset) && file->read(bytes, size) == static_cast<int>(size);
}
bool compatibleSource(HalFile& file, const char* code) {
  hyphenation_pack::Entry entry;
  return hyphenation_pack::inspect({&file, file.size(), readInput}, localSupported, code, entry) ==
         hyphenation_pack::Result::Ok;
}
}  // namespace

bool begin() {
  setExternalHyphenationLookup(lookup);
  return initialize(false);
}
bool lookup(const char* code, ExternalHyphenationPatterns& out) {
  hyphenation_pack::Entry entry;
  if (!initialized || !mapping.data || !store.find(code, entry)) return false;
  out.patterns = {entry.rootOffset, mapping.data + entry.offset, entry.size};
  out.identity = hyphenation_pack::identity(entry);
  return true;
}
bool isInstalled(const char* code) {
  ExternalHyphenationPatterns found{};
  return lookup(code, found);
}
bool hasSource(const char* code) {
  char path[64];
  if (!sourcePath(code, path)) return false;
  if (supported(code, 2, 2)) return Storage.exists(path);
  auto file = Storage.open(path);
  return file && !file.isDirectory() && compatibleSource(file, code);
}
size_t listLocalPacks(LocalPack* out, size_t capacity) {
  if (!out || !capacity) return 0;
  size_t count = 0;
  if (initialized) {
    for (hyphenation_pack::Entry entry; count < capacity && store.entryAt(count, entry); ++count) {
      out[count] = {{entry.code[0], entry.code[1], 0}, false, true};
    }
  }
  auto dir = Storage.open(DIRECTORY);
  if (dir && dir.isDirectory()) {
    // The menu has a fixed-size result. Cap filesystem work as well, even on
    // cards with large unrelated directories.
    constexpr size_t MAX_DIRECTORY_ENTRIES = 512;
    for (size_t seen = 0; seen < MAX_DIRECTORY_ENTRIES; ++seen) {
      if (seen && !(seen % 32)) delay(1);
      auto file = dir.openNextFile();
      if (!file) break;
      char name[32] = {};
      // Require the exact canonical filename including case and extension.
      if (file.isDirectory() || file.getName(name, sizeof(name)) != 14 || std::strncmp(name, "hyph-", 5) ||
          std::strcmp(name + 7, ".cphyph"))
        continue;
      const char code[3] = {name[5], name[6], 0};
      if (!localSupported(code, 2, 2) || !compatibleSource(file, code)) continue;
      auto* const existing =
          std::find_if(out, out + count, [&](const LocalPack& pack) { return !std::strcmp(pack.code, code); });
      if (existing != out + count)
        existing->hasSource = true;
      else if (count < capacity)
        out[count++] = {{code[0], code[1], 0}, true, false};
    }
  }
  std::sort(out, out + count, [](const LocalPack& a, const LocalPack& b) {
    return a.installed != b.installed ? a.installed : std::strcmp(a.code, b.code) < 0;
  });
  return count;
}
hyphenation_pack::Result validateSource(const char* path, const char* code, void* context,
                                        bool (*continueWork)(void*)) {
  using hyphenation_pack::Result;
  if (!supported(code, 2, 2)) return Result::Unsupported;
  if (!initialize(false)) return Result::Unavailable;
  FsFile file;
  if (!Storage.openFileForRead("HYPH", path, file)) return Result::Io;
  hyphenation_pack::ValidationOptions options;
  options.expectedCode = code;
  options.maxPayloadSize = store.availablePayloadBytes(code);
  options.context = context;
  options.continueWork = continueWork;
  hyphenation_pack::Entry entry;
  const auto result = hyphenation_pack::validate({&file, file.size(), readInput}, supported, entry, options);
  file.close();
  if (result != Result::Ok) LOG_ERR("HYPH", "Download validation: %s", hyphenation_pack::resultName(result));
  return result;
}
hyphenation_pack::Result install(const char* code) {
  using hyphenation_pack::Result;
  char path[64];
  if (!sourcePath(code, path)) return Result::Unsupported;
  if (!initialize(true)) return Result::Unavailable;
  FsFile file;
  if (!Storage.openFileForRead("HYPH", path, file)) return Result::Io;
  // Require the header identity to match the selected filename. Never import
  // another language merely because its file was renamed on the SD card.
  char headerCode[2];
  if (!file.seek(5) || file.read(headerCode, sizeof(headerCode)) != sizeof(headerCode) ||
      std::memcmp(headerCode, code, sizeof(headerCode))) {
    file.close();
    LOG_ERR("HYPH", "Pack language does not match %s", path);
    return Result::Invalid;
  }
  const auto result = store.install({&file, file.size(), readInput});
  file.close();
  if (result != Result::Ok && result != Result::Unchanged)
    LOG_ERR("HYPH", "%s: %s", code, hyphenation_pack::resultName(result));
  return result;
}
hyphenation_pack::Result remove(const char* code) {
  // A legacy flash install may still be serving reads before the SD image exists.
  if (isInstalled(code) && !initialize(true)) return hyphenation_pack::Result::Unavailable;
  const auto result = store.remove(code);
  if (result != hyphenation_pack::Result::Ok && result != hyphenation_pack::Result::Unchanged)
    LOG_ERR("HYPH", "%s: %s", code, hyphenation_pack::resultName(result));
  return result;
}
}  // namespace HyphenationPackStore
