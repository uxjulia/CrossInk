#pragma once
#include <HalStorage.h>
namespace FsHelpers {
inline bool directoryIterationFailed(const HalFile& file) { return file.allocationFailed() || file.iterationFailed(); }
}  // namespace FsHelpers
