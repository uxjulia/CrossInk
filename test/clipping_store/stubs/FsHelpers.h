#pragma once
#include <HalStorage.h>
namespace FsHelpers {
inline bool directoryIterationFailed(const HalFile& f) { return f.iterationFailed(); }
}  // namespace FsHelpers
