#pragma once

#include <algorithm>
#include <cstdint>

// Allocation-free pace math, shared by the reader and native regression tests.
struct ReadingPace {
  static constexpr uint16_t kStoredSamples = 3;
  static constexpr uint16_t kSessionSamples = 10;

  static uint32_t sessionAverageMillis(const uint32_t seconds, const uint16_t count) {
    if (count == 0) return 0;
    return static_cast<uint32_t>(std::min<uint64_t>((static_cast<uint64_t>(seconds) * 1000U + count / 2U) / count,
                                                    static_cast<uint32_t>(UINT16_MAX) * 1000U));
  }

  // The prior is captured at session start, so new samples aren't counted twice.
  // An established prior hands over over ten accepted pages. A prior based on
  // fewer samples has less weight. Preserve the three-sample learning threshold
  // across short sessions, too.
  static uint32_t estimateMillis(const uint32_t priorMillis, const uint16_t priorCount, const uint32_t sessionSeconds,
                                 const uint16_t sessionCount) {
    const bool hasPrior = priorMillis > 0 && priorCount > 0;
    const uint32_t sessionMillis = sessionAverageMillis(sessionSeconds, sessionCount);
    if (!hasPrior) return sessionCount >= kStoredSamples ? sessionMillis : 0;
    if (static_cast<uint32_t>(priorCount) + sessionCount < kStoredSamples) return 0;
    if (sessionCount == 0 || sessionMillis == 0) return priorMillis;
    const uint16_t count = std::min(sessionCount, kSessionSamples);
    const uint16_t priorWeight = std::min(priorCount, kSessionSamples) * (kSessionSamples - count);
    const uint16_t sessionWeight = count * kSessionSamples;
    const uint16_t totalWeight = priorWeight + sessionWeight;
    return static_cast<uint32_t>((static_cast<uint64_t>(priorMillis) * priorWeight +
                                  static_cast<uint64_t>(sessionMillis) * sessionWeight + totalWeight / 2U) /
                                 totalWeight);
  }
};
