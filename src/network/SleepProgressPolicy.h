#pragma once

#include <cmath>

namespace SleepProgressPolicy {
enum class Remote { Absent, Present, Unavailable };

// Unattended sync only moves the server forward. Equivalent positions, malformed
// values, or an unreadable remote are left to the normal interactive Sync Book flow.
inline bool mayUpload(const float local, const Remote remote, const float server) {
  constexpr float MIN_ADVANCE = 0.001f;  // Same 0.1 percentage-point tolerance as manual sync.
  if (!std::isfinite(local) || local < 0.0f || local > 1.0f) return false;
  if (remote == Remote::Absent) return true;
  return remote == Remote::Present && std::isfinite(server) && server >= 0.0f && server <= 1.0f &&
         local - server > MIN_ADVANCE;
}
}  // namespace SleepProgressPolicy
