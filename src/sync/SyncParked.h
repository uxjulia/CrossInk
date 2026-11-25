#pragma once

#include <KOReaderSyncClient.h>

#include <cstdint>
#include <string>

// Remote positions the pull could not apply yet: /.crosspoint/sync_parked.bin.
// A change for a book not on this reader (or not in sync_ids.bin yet), one
// that could not be applied, and one the user skipped in Ask on conflicts is
// parked. A bulk sync's push compares the book with it instead of overwriting
// it, and any reconciled sync of the book removes it.
//
// Fixed 384-byte slots, each with its own CRC32, so replacing or removing an
// entry is an in-place write and a slot torn by a power cut reads as empty.
// At most 32 entries; the oldest (by remote timestamp) is evicted when full.
namespace SyncParked {
constexpr const char* PATH = "/.crosspoint/sync_parked.bin";
constexpr size_t MAX_ENTRIES = 32;
constexpr size_t MAX_HASHES = 4;   // document plus up to 3 aliases
constexpr size_t MAX_DEVICE = 32;  // device name, truncated
constexpr size_t MAX_XPATH = 252;  // longer positions are not parked

struct Entry {
  uint8_t hashes[MAX_HASHES][16]{};
  uint8_t hashCount = 0;
  float percentage = 0;
  int64_t timestamp = 0;
  bool hasRich = false;
  uint32_t pctQ = 0;
  uint16_t spine = 0;
  uint16_t page = 0;
  uint16_t pages = 1;
  uint16_t paragraph = 0;  // 0 = none
  char device[MAX_DEVICE + 1]{};
  std::string xpath;

  bool matches(const uint8_t (&id)[16]) const;
  // The remote position in the shape KOReaderSyncActivity applies.
  KOReaderProgress toProgress(const std::string& document) const;
};

// Parks `change`, replacing any entry that shares one of its hashes. False
// only when the file could not be written; a change that can't be stored
// (malformed id, over-long xpath) is logged and dropped.
bool park(const KOReaderProgressChange& change);
// The parked entry for a book's id, if any.
bool find(const uint8_t (&id)[16], Entry& out);
bool find(const std::string& hexId, Entry& out);
bool remove(const std::string& hexId);
// True when nothing is parked (cheap: lets book open skip hashing).
bool empty();
void clear();
}  // namespace SyncParked
