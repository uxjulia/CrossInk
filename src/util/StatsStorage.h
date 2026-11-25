#pragma once

#include <string>

#include "activities/reader/StatsCounterId.h"

// Device-qualified paths keep copied counters and pending uploads with their
// original reader. Empty means identity/migration failed; never use the legacy
// path as a fallback in that case.
namespace StatsStorage {
std::string localPath(const std::string& legacyPath, bool directory = false, bool readOnly = false);
// A partially moved family can still have owned recovery slots under its legacy name.
std::string bookReadPath(const std::string& legacyPath, bool& splitRecovery);
// Presence hints for the completion check. Versions 1-3 used stats.bin (index 0).
// A bit can refer to another reader: always resolve ownership before reading.
struct BookFiles {
  enum Slot : uint8_t { Primary = 1, Backup = 2, Temp = 4, All = Primary | Backup | Temp };
  uint8_t slots[8]{};
};
BookFiles bookFiles(const std::string& directory);
// Immutable ID for an independent directory of daily counters. Existing data
// without a marker retains the legacy identity; uncertain markers fail closed.
bool directoryCounterId(const std::string& directory, StatsCounterId& id);
}  // namespace StatsStorage
