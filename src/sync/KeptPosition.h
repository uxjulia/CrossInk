#pragma once

#include <string>

#include "SyncParked.h"

// A remote position kept in sync_parked.bin, offered when its book is opened:
// "Continue from 45% (Phone)?". Only books inside a folder (or the whole card)
// whose first bulk sync has finished are offered one, so a book deliberately
// kept out of synced folders is never prompted.
namespace KeptPosition {
struct Offer {
  std::string document;  // the book's id under the configured match method
  SyncParked::Entry entry;
};

// The kept position for an EPUB, if any. Nothing kept (the common case) costs
// one failed file open; otherwise the book's id comes from its cache sync
// record, and is only computed when that record is missing or stale.
bool find(const std::string& bookPath, Offer& out);

// The reader settled the offer: forget the kept position. `pushLocal` queues
// this reader's position for the next sync (it is ahead, or the user kept it).
void resolve(const std::string& bookPath, const Offer& offer, bool pushLocal);
}  // namespace KeptPosition
