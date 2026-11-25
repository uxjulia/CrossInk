#pragma once

#include <KOReaderCredentialStore.h>

// Changing Document Matching gives every book a new document id, so
// everything keyed by the old ids is dropped: the id map, kept positions,
// and each sync scope's cursor, first walk and catch-ups. The next sync of
// the card or a folder checks every book again under the new ids. Progress
// already on the server stays under the old ids (merging books in the web
// app reconnects it).
namespace DocumentMatching {
// Saves `method`. Returns false (and changes nothing) when it is already set.
bool set(DocumentMatchMethod method);
}  // namespace DocumentMatching
