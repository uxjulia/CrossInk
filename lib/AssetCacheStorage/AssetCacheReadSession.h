#pragma once

#include <HalStorage.h>

#include "AssetCacheStorage.h"

// Cold-path, stack-scoped reader. Keeps the SD file's FAT traversal position
// during validation/mapping, then closes before normal reading or USB access.
// The storage object must outlive the session; sessions cannot be nested.
class AssetCacheReadSession {
 public:
  explicit AssetCacheReadSession(AssetCacheStorage& storage);
  ~AssetCacheReadSession();
  bool close();
  explicit operator bool() const { return ready_; }
  AssetCacheReadSession(const AssetCacheReadSession&) = delete;
  AssetCacheReadSession& operator=(const AssetCacheReadSession&) = delete;

 private:
  AssetCacheStorage& storage_;
  HalFile file_;
  bool attached_ = false;
  bool ready_ = false;
};
