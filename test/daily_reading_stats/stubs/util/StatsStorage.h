#pragma once
#include <string>

#include "activities/reader/StatsCounterId.h"
namespace StatsStorage {
inline std::string localPath(const std::string& path, bool = false, bool = false) { return path; }
inline bool directoryCounterId(const std::string&, StatsCounterId& id) {
  id = {};
  return true;
}
}  // namespace StatsStorage
