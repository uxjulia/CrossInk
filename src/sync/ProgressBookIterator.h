#pragma once

#include <bitset>
#include <memory>
#include <string>

#include "util/FolderBookIterator.h"

// Discovers EPUBs with usable progress, using their caches before walking the card.
class ProgressBookIterator {
 public:
  using Result = FolderBookIterator::Result;

  explicit ProgressBookIterator(std::string root);
  Result next(std::string& bookPath);

  // The association is recoverable. A write failure never invalidates saved progress.
  static void rememberPath(const std::string& bookPath, const std::string& cachePath);
  static void updateMovedPath(const std::string& bookPath, const std::string& cachePath);
  // Cold boot may follow an SD-card edit outside the firmware.
  static void resetLegacyLookups();

 private:
  Result nextCache(std::string& bookPath);
  Result nextLegacy(std::string& bookPath);
  bool mightNeedPath(uint64_t hash) const;

  std::string root;
  DirectoryPosition cachePosition{};
  bool cacheStarted = false;
  const uint32_t pathRevision;
  uint32_t unlinkedCaches = 0;
  uint64_t unlinkedDigest = 0;
  // A fixed filter keeps the legacy walk from probing the SD cache for every unread book.
  // False positives only cost a lookup. The iterator and its filter live on the heap.
  std::bitset<2048> unlinkedHashes;
  std::unique_ptr<FolderBookIterator> legacy;
  bool done = false;
  bool pathRecoveryFailed = false;
};
