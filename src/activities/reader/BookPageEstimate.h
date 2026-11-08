#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

// A small, session-only sample of the book's rendered page density. Weight by
// pages visited, rather than letting a newly indexed chapter describe the book.
class BookPageEstimate {
  struct Sample {
    int spine = -1;
    float pagesPerByte = 0;
    uint16_t weight = 0;
    uint16_t lastPage = 0;
  };
  std::array<Sample, 8> samples{};
  uint8_t next = 0;

 public:
  void observe(int spine, uint32_t bytes, uint16_t estimatedPages, uint16_t availablePages, int currentPage,
               bool complete) {
    if (spine < 0 || bytes == 0 || estimatedPages == 0 || availablePages == 0 || currentPage < 0) return;
    // The first incremental chunk can be mostly a heading, CSS, or images.
    // Wait for a useful prefix before offering a cold-start projection.
    if (!complete && availablePages < 8) return;
    if (currentPage == 0 && pagesPerByte() > 0) return;

    auto target =
        std::find_if(samples.begin(), samples.end(), [spine](const Sample& sample) { return sample.spine == spine; });
    if (target == samples.end()) {
      target = &samples[next];
      *target = {};
      next = (next + 1) % samples.size();
    }
    const uint16_t weight = static_cast<uint16_t>(std::min(std::max(currentPage, 1), 32));
    const float density = static_cast<float>(estimatedPages) / bytes;
    if (target->weight > 0) {
      // Count new reading positions once. Repainting or reindexing the same
      // page must not repeatedly change its contribution. Mature chapters
      // incorporate each new position with at most 1/32 of their weight.
      if (currentPage <= target->lastPage) return;
      const float confidence = std::min(1.0f, static_cast<float>(currentPage - target->lastPage) / weight);
      target->pagesPerByte += confidence * (density - target->pagesPerByte);
    } else {
      target->pagesPerByte = density;
    }
    target->spine = spine;
    target->weight = std::max(target->weight, weight);
    target->lastPage = static_cast<uint16_t>(currentPage);
  }

  float pagesPerByte() const {
    float total = 0;
    uint16_t weight = 0;
    for (const auto& sample : samples) {
      total += sample.pagesPerByte * sample.weight;
      weight += sample.weight;
    }
    return weight > 0 ? total / weight : 0;
  }

  float remainingPages(uint64_t remainingBytes, int currentPage, uint16_t availablePages) const {
    const float density = pagesPerByte();
    if (density <= 0 || currentPage < 0) return 0;
    return std::max(density * remainingBytes - currentPage,
                    static_cast<float>(std::max(0, static_cast<int>(availablePages) - currentPage)));
  }
};
