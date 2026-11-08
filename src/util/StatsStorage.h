#pragma once

#include <string>

#include "activities/reader/StatsCounterId.h"

// Device-qualified paths keep copied counters and pending uploads with their
// original reader. Empty means identity/migration failed; never use the legacy
// path as a fallback in that case.
namespace StatsStorage {
std::string localPath(const std::string& legacyPath, bool directory = false, bool readOnly = false);
// Immutable ID for an independent directory of daily counters. Existing data
// without a marker retains the legacy identity; uncertain markers fail closed.
bool directoryCounterId(const std::string& directory, StatsCounterId& id);
}  // namespace StatsStorage
