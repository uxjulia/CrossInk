#pragma once
#include "HalStorage.h"
namespace FsHelpers {
inline bool directoryIterationFailed(HalFile& file) { return file.allocationFailed() || file.iterationFailed(); }
inline bool resolveRootDirectoryIgnoreCase(const char* path, char* out, size_t length) {
  if (!Storage.exists(path)) return false;
  std::snprintf(out, length, "%s", path);
  return true;
}
}  // namespace FsHelpers
