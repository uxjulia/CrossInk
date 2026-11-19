#pragma once
#include <cstdint>

#include "ReadingStatsUtils.h"

// An outbox alongside existing stats. Cumulative checkpoints keep one session
// identity across retries; only explicit acknowledgment permits removal.
namespace ReadingSessionJournal {
constexpr int16_t UNKNOWN_PROGRESS = -1;
struct Record {
  char id[33]{};
  char document[33]{};
  uint64_t startedAt = 0;
  uint64_t endedAt = 0;
  uint32_t seconds = 0;
  int16_t utcOffsetMinutes = 0;
  int16_t startProgressPermille = UNKNOWN_PROGRESS;
  int16_t endProgressPermille = UNKNOWN_PROGRESS;
};
using Upload = bool (*)(void*, const Record&);
bool uploadPending(Upload upload, void* context);
bool save(const Record& record);

class Session {
  Record pending{};
  ReadingStatsDateTime intervalStart{};
  int16_t intervalProgress = UNKNOWN_PROGRESS;
  uint8_t intervalOffset = 255;
  uint32_t checkpointSeconds = 0;
  uint32_t checkpointRevision = 0;

 public:
  void start(const ReadingStatsDateTime& local, uint8_t offset, int16_t progressPermille = UNKNOWN_PROGRESS) {
    intervalProgress = progressPermille;
    intervalStart = local;
    intervalOffset = offset;
  }
  uint32_t revision() const { return checkpointRevision; }
  bool checkpoint();
  bool checkpointIfDue();
  // Uses the same accepted page intervals as the existing daily counters.
  // Suppresses periodic writes only; ending a discontinuous session still
  // preserves its record before reusing the bounded in-memory slot.
  bool accept(const char* bookPath, const ReadingStatsDateTime& end, uint8_t offset, uint32_t seconds,
              bool allowCheckpoint = true);
  bool flush();
};
}  // namespace ReadingSessionJournal
