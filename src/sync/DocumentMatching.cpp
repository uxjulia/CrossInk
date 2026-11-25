#include "DocumentMatching.h"

#include <Logging.h>

#include "SyncIds.h"
#include "SyncState.h"

namespace DocumentMatching {
bool set(const DocumentMatchMethod method) {
  if (KOREADER_STORE.getMatchMethod() == method) return false;
  KOREADER_STORE.setMatchMethod(method);
  if (!KOREADER_STORE.saveToFile()) LOG_ERR("SYNC", "Cannot save Document Matching");
  SyncIds::reset();
  SyncState::resetForNewDocumentIds();  // also drops kept positions
  LOG_INF("SYNC", "Document Matching changed; sync ids, kept positions and scopes reset");
  return true;
}
}  // namespace DocumentMatching
