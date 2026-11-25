#pragma once

#include <cstdint>
#include <string>

// One folder (or "/" for the whole card) a bulk sync has prepared: its change
// feed cursor, its resumable first walk, and its own catch-up bits.
struct SyncScope {
  static constexpr size_t MAX_PATH_LENGTH = 255;
  std::string path;
  bool prepared = false;  // first walk finished: later syncs only pull and push
  bool statsCaughtUp = false;
  bool clippingsCaughtUp = false;
  uint32_t cursor = 0;      // change feed position for this scope
  uint32_t lastSynced = 0;  // eviction order: a stamp id, not a clock
  uint32_t walkId = 0;      // non-zero while a first walk is unfinished
  uint32_t statsCatchupId = 0;
  uint32_t clippingsCatchupId = 0;

  // True when `bookPath` is inside this scope.
  bool covers(const std::string& bookPath) const;
};

// What the bulk sync actions have already finished for the current server
// account: /.crosspoint/sync_state.bin. Keyed to base URL plus username; a
// different server or user starts fresh, so its catch-ups and walks run again.
// Scopes are streamed from the file one at a time, so RAM use does not grow
// with the scope list.
struct SyncState {
  static constexpr const char* PATH = "/.crosspoint/sync_state.bin";
  static constexpr size_t MAX_SCOPES = 32;

  std::string baseUrl;
  std::string username;
  // Whole-card catch-ups: every book's saved stats / clippings were sent at
  // least once with that data's What to Sync toggle on.
  bool statsCaughtUp = false;
  bool clippingsCaughtUp = false;
  // Non-zero while a catch-up is under way; books it finished carry this id in
  // their cache sync file, so an interrupted run resumes by skipping them.
  uint32_t statsCatchupId = 0;
  uint32_t clippingsCatchupId = 0;
  uint32_t nextStampId = 0;

  // Loads the state for the configured account. A missing or corrupt file (and
  // backup), or another account's file, gives a fresh state.
  static SyncState load();
  // Saves these fields; scopes already stored for this account are kept.
  bool save() const;
  // Something completed that a lost pending list would have to redo.
  bool anythingCompleted() const { return statsCaughtUp || clippingsCaughtUp; }
  // Stamp ids are never reused, even across resets, so an old stamp can't skip a book.
  uint32_t newStampId();

  // The pending list was lost: re-run every catch-up and first walk.
  static void resetAfterPendingLoss();
  // A What to Sync toggle was turned on: changes made while it was off were
  // never marked pending, so the next sync re-sends that data.
  static void restartCatchup(bool stats);
  // A change of that type could not be queued (toggle off, signed out, server
  // not accepting it): the catch-up must run again. Writes only if needed.
  static void invalidateCatchup(bool stats);
  // Document Matching changed: every book has a new id, so cursors, prepared
  // scopes and catch-ups start over.
  static void resetForNewDocumentIds();

  // Scope list for the current account.
  static bool findScope(const std::string& path, SyncScope& out);
  // The prepared scope nearest to `path` that covers it ("/" covers all).
  static bool findPreparedCovering(const std::string& path, SyncScope& out);
  // Adds or replaces `scope` (by path) and stamps it as the most recently
  // synced. Beyond MAX_SCOPES the least recently synced other scope is dropped.
  static bool putScope(const SyncScope& scope);
  // A whole-card catch-up covered every prepared scope too.
  static bool markScopesCaughtUp(bool stats);

 private:
  // The stored state whatever account it belongs to; false when none is readable.
  static bool readStored(SyncState& state);
};
