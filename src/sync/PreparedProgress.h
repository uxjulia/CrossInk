#pragma once
#include <KOReaderSyncClient.h>

#include <cstdint>
#include <string>

// Disposable, CRC-protected records for one explicit bulk sync. Never a durable
// outbox: begin discards any interrupted run, and pending bits remain authoritative.
namespace PreparedProgress {
struct Saved {
  uint16_t spine = 0, page = 0, pages = 0;
  uint32_t offset = 0;
  bool hasOffset = false;
  bool operator==(const Saved& other) const {
    return spine == other.spine && page == other.page && pages == other.pages && hasOffset == other.hasOffset &&
           (!hasOffset || offset == other.offset);
  }
};
bool begin();
void clear();
bool write(const std::string& path, const Saved& saved, const KOReaderProgress& progress);
bool read(const std::string& path, const Saved& saved, KOReaderProgress& progress);
}  // namespace PreparedProgress
