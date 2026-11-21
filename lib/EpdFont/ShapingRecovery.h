#pragma once

#include <cstdint>

namespace shaping_recovery {

// Diagnostic identities for the first two failed faces (normally regular and
// bold on C3). Retry policy is section-wide, including any additional faces.
struct Failures {
  struct Face {
    uint32_t key = 0;
    uint16_t scripts = 0;
  };
  Face faces[2]{};
  bool degraded = false;
  bool sourceUnavailable = false;
  bool blockedFlash = false;
  uint32_t failureFree = 0;
  uint32_t failureLargest = 0;
  // Only recovery builds request positive witnesses. Ordinary layout does
  // not accumulate successful faces or touch the additional tracking state.
  const Failures* expected = nullptr;
  uint16_t witnessed[2]{};
  bool anyShaped = false;

  void recordSuccess(uint32_t key, unsigned script) {
    if (!expected) return;
    anyShaped = true;
    for (unsigned i = 0; i < 2; ++i) {
      if (expected->faces[i].key == key) witnessed[i] |= static_cast<uint16_t>(1u << script);
    }
  }

  bool recovered() const {
    if (!expected) return true;
    if (degraded || !anyShaped) return false;
    for (unsigned i = 0; i < 2; ++i) {
      if (expected->faces[i].scripts & ~witnessed[i]) return false;
    }
    return true;
  }

  void record(uint32_t key, unsigned script, uint32_t free, uint32_t largest, bool sourceFailure = false,
              bool flashBlocked = false) {
    if (!degraded) {
      failureFree = free;
      failureLargest = largest;
    }
    degraded = true;
    sourceUnavailable |= sourceFailure;
    blockedFlash |= flashBlocked;
    for (auto& face : faces) {
      if (face.scripts != 0 && face.key != key) continue;
      face.key = key;
      face.scripts |= static_cast<uint16_t>(1u << script);
      break;
    }
  }
};

struct Readiness {
  uint32_t free = 0;
  uint32_t largest = 0;
  bool flash = false;

  bool improvedFrom(const Readiness& previous) const {
    // Ignore small differences caused by entry path, allocator bookkeeping,
    // and scheduled tasks. Never retry simply because a new boot occurred.
    constexpr uint32_t improvement = 16 * 1024;
    const auto increased = [](uint32_t now, uint32_t old) { return now > old && now - old >= improvement; };
    return (flash && !previous.flash) || (free >= previous.free && largest >= previous.largest &&
                                          (increased(free, previous.free) || increased(largest, previous.largest)));
  }
};

}  // namespace shaping_recovery
