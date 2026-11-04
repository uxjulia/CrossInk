#pragma once
#include <HalStorage.h>

#include <string>
namespace FsHelpers {
inline bool directoryIterationFailed(FsFile&) { return false; }
inline bool hasEpubExtension(const std::string& s) { return s.ends_with(".epub"); }
}  // namespace FsHelpers
