#pragma once
#include <string>

#include "../../../../src/util/StatsStorage.h"
namespace StatsStorage {
inline std::string localPath(const std::string& path, bool, bool) { return path; }
inline std::string bookReadPath(const std::string& path, bool& splitRecovery) {
  splitRecovery = false;
  return path;
}
// These payload/recovery tests use a flat file map. Probe all supported names.
inline BookFiles bookFiles(const std::string&) {
  BookFiles files;
  for (auto& slots : files.slots) slots = BookFiles::All;
  return files;
}
inline bool directoryCounterId(const std::string&, StatsCounterId& id) {
  id = {};
  return true;
}
}  // namespace StatsStorage
