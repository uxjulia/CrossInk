#include "KeptPosition.h"

#include <FsHelpers.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>

#include "BookSyncFile.h"
#include "SyncPending.h"
#include "SyncState.h"

namespace KeptPosition {
bool find(const std::string& bookPath, Offer& out) {
  // Cheapest checks first: most opens stop at the empty parked file.
  if (!FsHelpers::hasEpubExtension(bookPath) || SyncParked::empty() || !KOREADER_STORE.hasCredentials()) return false;
  SyncScope scope;
  if (!SyncState::findPreparedCovering(bookPath, scope)) return false;
  out.document = BookSyncFile::documentId(bookPath);
  if (out.document.empty()) {
    LOG_ERR("SYNC", "No document id for kept position check: %s", bookPath.c_str());
    return false;
  }
  if (!SyncParked::find(out.document, out.entry)) return false;
  LOG_INF("SYNC", "Kept position %.4f from %s for %s", out.entry.percentage, out.entry.device, bookPath.c_str());
  return true;
}

void resolve(const std::string& bookPath, const Offer& offer, const bool pushLocal) {
  if (!SyncParked::remove(offer.document)) LOG_ERR("SYNC", "Cannot remove kept position: %s", bookPath.c_str());
  if (pushLocal) SyncPending::mark(bookPath, SyncPending::PROGRESS_BIT);
}
}  // namespace KeptPosition
