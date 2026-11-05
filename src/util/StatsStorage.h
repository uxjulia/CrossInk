#pragma once

#include <string>

// Device-qualified paths keep copied counters and pending uploads with their
// original reader. Empty means identity/migration failed; never use the legacy
// path as a fallback in that case.
namespace StatsStorage {
std::string localPath(const std::string& legacyPath, bool directory = false, bool readOnly = false);
}
