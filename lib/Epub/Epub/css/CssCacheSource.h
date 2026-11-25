#pragma once

#include <HalStorage.h>

#include <array>
#include <cstdint>
#include <string>

// Only terminal (fixed rule-cap) caches use this marker. No ZIP scan or source
// extraction on reopen. Like ZipDirectoryCache, identity is size + FAT mtime:
// replacing bytes while preserving both requires explicitly clearing book cache.
namespace CssCacheSource {
using Identity = std::array<uint32_t, 6>;
inline std::string markerPath(const std::string& cache) { return cache + "/css_terminal.source"; }
inline bool read(const std::string& source, uint8_t version, uint32_t sourceLimit, Identity& identity) {
  FsFile file;
  if (!Storage.openFileForRead("CSS", source, file)) return false;
  const uint64_t size = file.size();
  const uint32_t modified = file.modificationTime();
  file.close();
  if (size == 0 || modified == 0) return false;
  identity = {0x43535401, version, uint32_t(size), uint32_t(size >> 32), modified, sourceLimit};
  return true;
}
inline bool matches(const std::string& cache, const Identity& identity) {
  FsFile file;
  if (!Storage.openFileForRead("CSS", markerPath(cache), file)) return false;
  Identity stored{};
  const bool valid = file.size() == sizeof(stored) && file.read(stored.data(), sizeof(stored)) == sizeof(stored);
  file.close();
  return valid && stored == identity;
}
inline bool invalidate(const std::string& cache) {
  const auto path = markerPath(cache);
  return !Storage.exists(path.c_str()) || Storage.remove(path.c_str());
}
inline bool save(const std::string& cache, const Identity& identity) {
  FsFile file;
  if (!Storage.openFileForWrite("CSS", markerPath(cache), file)) return false;
  const bool written = file.write(identity.data(), sizeof(identity)) == sizeof(identity) && file.sync();
  file.close();
  if (!written) invalidate(cache);
  return written;
}
}  // namespace CssCacheSource
