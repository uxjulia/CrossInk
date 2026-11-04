#include <gtest/gtest.h>

#include <map>

#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "SyncFiles.h"
#include "SyncPending.h"
#include "SyncState.h"

namespace {
using Entries = std::map<std::string, uint8_t>;

Entries entries() {
  Entries out;
  SyncPending::Cursor cursor;
  if (!cursor.begin()) return out;
  std::string path;
  uint8_t mask = 0;
  while (cursor.next(path, mask) == SyncPending::Cursor::Step::Entry) out[path] = mask;
  return out;
}

void corrupt(const std::string& path) {
  if (auto* bytes = Storage.bytes(path)) (*bytes)[0] ^= 0xFF;
}

class SyncPendingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    KOREADER_STORE = {};
    SETTINGS = {};
  }
};
}  // namespace

TEST_F(SyncPendingTest, MarksOnlyRecordableBitsAndMergesThem) {
  ASSERT_TRUE(SyncPending::mark("/a.epub", SyncPending::PROGRESS_BIT));
  ASSERT_TRUE(SyncPending::mark("/a.epub", SyncPending::STATS_BIT));
  KOREADER_STORE.clippings = false;
  ASSERT_TRUE(SyncPending::mark("/b.epub", SyncPending::CLIPPINGS_BIT | SyncPending::PROGRESS_BIT));
  EXPECT_EQ(entries(), (Entries{{"/a.epub", SyncPending::PROGRESS_BIT | SyncPending::STATS_BIT},
                                {"/b.epub", SyncPending::PROGRESS_BIT}}));
}

TEST_F(SyncPendingTest, NothingIsQueuedWithoutAnAccount) {
  KOREADER_STORE.credentials = false;
  ASSERT_TRUE(SyncPending::mark("/a.epub", SyncPending::PROGRESS_BIT | SyncPending::STATS_BIT));
  EXPECT_FALSE(Storage.exists(SyncPending::PATH));
}

TEST_F(SyncPendingTest, ClearDropsEmptyEntriesAndKeepsOthers) {
  ASSERT_TRUE(SyncPending::mark("/a.epub", SyncPending::PROGRESS_BIT | SyncPending::STATS_BIT));
  ASSERT_TRUE(SyncPending::mark("/b.epub", SyncPending::STATS_BIT));
  ASSERT_TRUE(SyncPending::clear("/a.epub", SyncPending::STATS_BIT));
  ASSERT_TRUE(SyncPending::clearPaths({"/b.epub"}, SyncPending::STATS_BIT));
  EXPECT_EQ(entries(), (Entries{{"/a.epub", SyncPending::PROGRESS_BIT}}));
  ASSERT_TRUE(SyncPending::mark("/c.epub", SyncPending::CLIPPINGS_BIT));
  ASSERT_TRUE(SyncPending::clearEverywhere(SyncPending::PROGRESS_BIT));
  EXPECT_EQ(entries(), (Entries{{"/c.epub", SyncPending::CLIPPINGS_BIT}}));
}

TEST_F(SyncPendingTest, RenameAndRemoveFollowTheBook) {
  ASSERT_TRUE(SyncPending::mark("/a.epub", SyncPending::STATS_BIT));
  ASSERT_TRUE(SyncPending::mark("/b.epub", SyncPending::STATS_BIT));
  ASSERT_TRUE(SyncPending::renamePath("/a.epub", "/Read/a.epub"));
  ASSERT_TRUE(SyncPending::removePath("/b.epub"));
  EXPECT_EQ(entries(), (Entries{{"/Read/a.epub", SyncPending::STATS_BIT}}));
}

TEST_F(SyncPendingTest, NoOpClearWritesNothing) {
  ASSERT_TRUE(SyncPending::mark("/a.epub", SyncPending::STATS_BIT));
  ASSERT_FALSE(Storage.exists("/.crosspoint/sync_pending.bin.bak"));
  ASSERT_TRUE(SyncPending::clear("/not-pending.epub", SyncPending::STATS_BIT));
  EXPECT_FALSE(Storage.exists("/.crosspoint/sync_pending.bin.bak"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/sync_pending.bin.tmp"));
}

TEST_F(SyncPendingTest, SnapshotWalkIgnoresClearsMadeDuringIt) {
  ASSERT_TRUE(SyncPending::mark("/a.epub", SyncPending::STATS_BIT));
  ASSERT_TRUE(SyncPending::mark("/b.epub", SyncPending::STATS_BIT));
  SyncPending::Cursor cursor;
  ASSERT_TRUE(cursor.begin());
  std::string path;
  uint8_t mask = 0;
  ASSERT_EQ(cursor.next(path, mask), SyncPending::Cursor::Step::Entry);
  ASSERT_TRUE(SyncPending::clear(path, SyncPending::STATS_BIT));
  ASSERT_EQ(cursor.next(path, mask), SyncPending::Cursor::Step::Entry);
  EXPECT_EQ(cursor.next(path, mask), SyncPending::Cursor::Step::Done);
  cursor.end();
  EXPECT_EQ(entries().size(), 1u);
}

TEST_F(SyncPendingTest, SessionMarkerRetriesAfterFailedSave) {
  SyncPending::SessionMarker marker;
  Storage.failNextRenameFrom(std::string(SyncPending::PATH) + ".tmp");
  marker.mark("/a.epub", SyncPending::STATS_BIT);
  EXPECT_TRUE(entries().empty());
  marker.mark("/a.epub", SyncPending::STATS_BIT);
  EXPECT_EQ(entries(), (Entries{{"/a.epub", SyncPending::STATS_BIT}}));
}

TEST_F(SyncPendingTest, SessionMarkerWritesOnceUntilASyncClearsTheBook) {
  SyncPending::SessionMarker marker;
  marker.mark("/a.epub", SyncPending::CLIPPINGS_BIT);
  ASSERT_TRUE(SyncPending::clear("/a.epub", SyncPending::CLIPPINGS_BIT));
  EXPECT_TRUE(entries().empty());
  // The same session saves again after a sync uploaded it: it must be queued again.
  marker.mark("/a.epub", SyncPending::CLIPPINGS_BIT);
  EXPECT_EQ(entries(), (Entries{{"/a.epub", SyncPending::CLIPPINGS_BIT}}));
}

TEST_F(SyncPendingTest, DroppedChangesRestartThatCatchup) {
  ASSERT_TRUE(SyncPending::ensureExists());  // a sync created the list before finishing its catch-ups
  SyncState state = SyncState::load();
  state.statsCaughtUp = state.clippingsCaughtUp = true;
  ASSERT_TRUE(state.save());
  KOREADER_STORE.stats = false;  // e.g. the server stopped accepting stats
  ASSERT_TRUE(SyncPending::mark("/a.epub", SyncPending::STATS_BIT | SyncPending::PROGRESS_BIT));
  const SyncState after = SyncState::load();
  EXPECT_FALSE(after.statsCaughtUp);
  EXPECT_TRUE(after.clippingsCaughtUp);
}

TEST_F(SyncPendingTest, MissingListCountsAsLostButAnEmptyOneDoesNot) {
  SyncState state = SyncState::load();
  state.statsCaughtUp = true;
  ASSERT_TRUE(state.save());
  // ensureExists runs before any catch-up completes in real use; here the flag
  // already exists, so this documents that a *missing* list counts as lost too.
  SyncPending::ensureExists();
  EXPECT_FALSE(SyncState::load().statsCaughtUp);
  SyncState again = SyncState::load();
  again.statsCaughtUp = true;
  ASSERT_TRUE(again.save());
  SyncPending::ensureExists();
  EXPECT_TRUE(SyncState::load().statsCaughtUp);  // an existing empty list is fine
}

TEST_F(SyncPendingTest, LostListAndBackupResetCatchups) {
  ASSERT_TRUE(SyncPending::mark("/a.epub", SyncPending::STATS_BIT));
  ASSERT_TRUE(SyncPending::mark("/b.epub", SyncPending::STATS_BIT));
  SyncState state = SyncState::load();
  state.statsCaughtUp = state.clippingsCaughtUp = true;
  ASSERT_TRUE(state.save());
  corrupt(SyncPending::PATH);
  corrupt("/.crosspoint/sync_pending.bin.bak");
  ASSERT_TRUE(SyncPending::mark("/c.epub", SyncPending::STATS_BIT));
  const SyncState after = SyncState::load();
  EXPECT_FALSE(after.statsCaughtUp);
  EXPECT_FALSE(after.clippingsCaughtUp);
  EXPECT_EQ(entries(), (Entries{{"/c.epub", SyncPending::STATS_BIT}}));
}

TEST_F(SyncPendingTest, CorruptListFallsBackToBackup) {
  ASSERT_TRUE(SyncPending::mark("/a.epub", SyncPending::STATS_BIT));
  ASSERT_TRUE(SyncPending::mark("/b.epub", SyncPending::STATS_BIT));
  corrupt(SyncPending::PATH);
  EXPECT_EQ(entries(), (Entries{{"/a.epub", SyncPending::STATS_BIT}}));
}

TEST_F(SyncPendingTest, StateBelongsToOneAccount) {
  SyncState state = SyncState::load();
  state.statsCaughtUp = true;
  const uint32_t stamp = state.newStampId();
  ASSERT_TRUE(state.save());
  KOREADER_STORE.username = "someone-else";
  const SyncState other = SyncState::load();
  EXPECT_FALSE(other.statsCaughtUp);
  EXPECT_GT(other.nextStampId, stamp);  // stamps are never reused across accounts
  KOREADER_STORE.username = "reader";
  EXPECT_TRUE(SyncState::load().statsCaughtUp);
}

TEST_F(SyncPendingTest, RestartCatchupClearsOnlyThatType) {
  SyncState state = SyncState::load();
  state.statsCaughtUp = state.clippingsCaughtUp = true;
  state.clippingsCatchupId = 7;
  ASSERT_TRUE(state.save());
  SyncState::restartCatchup(false);
  const SyncState after = SyncState::load();
  EXPECT_TRUE(after.statsCaughtUp);
  EXPECT_FALSE(after.clippingsCaughtUp);
  EXPECT_EQ(after.clippingsCatchupId, 0u);
}

TEST_F(SyncPendingTest, ChangesWhileSignedOutRestartTheCatchup) {
  ASSERT_TRUE(SyncPending::ensureExists());
  SyncState state = SyncState::load();
  state.statsCaughtUp = state.clippingsCaughtUp = true;
  ASSERT_TRUE(state.save());
  // Clearing the username signs out: nothing can be queued, and the identity
  // no longer matches the stored state.
  KOREADER_STORE.credentials = false;
  KOREADER_STORE.username.clear();
  ASSERT_TRUE(SyncPending::mark("/a.epub", SyncPending::STATS_BIT));
  KOREADER_STORE.credentials = true;
  KOREADER_STORE.username = "reader";
  const SyncState after = SyncState::load();
  EXPECT_FALSE(after.statsCaughtUp);
  EXPECT_TRUE(after.clippingsCaughtUp);
}

TEST_F(SyncPendingTest, ScopesAreStoredFoundAndCoveredByTheirNearestPreparedAncestor) {
  SyncScope read;
  read.path = "/Read";
  read.prepared = true;
  read.cursor = 40;
  ASSERT_TRUE(SyncState::putScope(read));
  SyncScope card;
  card.path = "/";
  card.prepared = true;
  card.cursor = 10;
  ASSERT_TRUE(SyncState::putScope(card));
  SyncScope found;
  ASSERT_TRUE(SyncState::findScope("/Read", found));
  EXPECT_EQ(found.cursor, 40u);
  ASSERT_TRUE(SyncState::findPreparedCovering("/Read/Series", found));
  EXPECT_EQ(found.path, "/Read");
  ASSERT_TRUE(SyncState::findPreparedCovering("/Other", found));
  EXPECT_EQ(found.path, "/");
  EXPECT_FALSE(SyncState::findScope("/Other", found));
  // A save of the fixed fields keeps the scopes.
  SyncState state = SyncState::load();
  state.statsCaughtUp = true;
  ASSERT_TRUE(state.save());
  EXPECT_TRUE(SyncState::findScope("/Read", found));
}

TEST_F(SyncPendingTest, CoversOnlyRealSubfolders) {
  SyncScope scope;
  scope.path = "/Read";
  EXPECT_TRUE(scope.covers("/Read/a.epub"));
  EXPECT_FALSE(scope.covers("/Reading/a.epub"));
  EXPECT_FALSE(scope.covers("/Read"));
  scope.path = "/";
  EXPECT_TRUE(scope.covers("/a.epub"));
}

TEST_F(SyncPendingTest, ReplacingAScopeKeepsOneEntryAndTheCapDropsTheLeastRecent) {
  SyncScope scope;
  for (size_t i = 0; i < SyncState::MAX_SCOPES; ++i) {
    scope.path = "/folder" + std::to_string(i);
    ASSERT_TRUE(SyncState::putScope(scope));
  }
  scope.path = "/folder0";
  scope.cursor = 99;
  ASSERT_TRUE(SyncState::putScope(scope));  // replace: still 32, folder0 now newest
  SyncScope found;
  ASSERT_TRUE(SyncState::findScope("/folder0", found));
  EXPECT_EQ(found.cursor, 99u);
  scope.path = "/one-more";
  ASSERT_TRUE(SyncState::putScope(scope));
  EXPECT_FALSE(SyncState::findScope("/folder1", found));  // least recently synced
  EXPECT_TRUE(SyncState::findScope("/folder0", found));
  EXPECT_TRUE(SyncState::findScope("/one-more", found));
}

TEST_F(SyncPendingTest, CountsEntriesForTheProgressBar) {
  EXPECT_EQ(SyncPending::count(), 0u);  // no list yet
  ASSERT_TRUE(SyncPending::mark("/Read/a.epub", SyncPending::PROGRESS_BIT));
  ASSERT_TRUE(SyncPending::mark("/Read/b.epub", SyncPending::STATS_BIT));
  ASSERT_TRUE(SyncPending::mark("/Other/c.epub", SyncPending::PROGRESS_BIT));
  EXPECT_EQ(SyncPending::count(), 3u);
  SyncScope scope;
  scope.path = "/Read";
  EXPECT_EQ(SyncPending::count([](const std::string& path,
                                  void* context) { return static_cast<const SyncScope*>(context)->covers(path); },
                               &scope),
            2u);
  // A running cursor's snapshot does not change the count of the live list.
  SyncPending::Cursor cursor;
  ASSERT_TRUE(cursor.begin());
  EXPECT_EQ(SyncPending::count(), 3u);
  cursor.end();
}

TEST_F(SyncPendingTest, ResetsReachScopes) {
  ASSERT_TRUE(SyncPending::ensureExists());
  SyncScope scope;
  scope.path = "/Read";
  scope.prepared = scope.statsCaughtUp = scope.clippingsCaughtUp = true;
  scope.cursor = 7;
  ASSERT_TRUE(SyncState::putScope(scope));
  SyncState::restartCatchup(true);
  SyncScope found;
  ASSERT_TRUE(SyncState::findScope("/Read", found));
  EXPECT_FALSE(found.statsCaughtUp);
  EXPECT_TRUE(found.clippingsCaughtUp);
  EXPECT_TRUE(found.prepared);
  SyncState::resetForNewDocumentIds();
  ASSERT_TRUE(SyncState::findScope("/Read", found));
  EXPECT_FALSE(found.prepared);
  EXPECT_EQ(found.cursor, 0u);
  EXPECT_FALSE(found.clippingsCaughtUp);
}

TEST_F(SyncPendingTest, LosingThePendingListMakesPreparedScopesWalkAgain) {
  ASSERT_TRUE(SyncPending::mark("/Read/a.epub", SyncPending::PROGRESS_BIT));
  SyncScope scope;
  scope.path = "/Read";
  scope.prepared = true;
  ASSERT_TRUE(SyncState::putScope(scope));
  SyncFiles::removeAll(SyncPending::PATH);
  ASSERT_TRUE(SyncPending::ensureExists());
  SyncScope found;
  ASSERT_TRUE(SyncState::findScope("/Read", found));
  EXPECT_FALSE(found.prepared);
}

TEST_F(SyncPendingTest, ScopesBelongToTheirAccount) {
  SyncScope scope;
  scope.path = "/Read";
  scope.prepared = true;
  ASSERT_TRUE(SyncState::putScope(scope));
  KOREADER_STORE.username = "someone-else";
  SyncScope found;
  EXPECT_FALSE(SyncState::findScope("/Read", found));
  ASSERT_TRUE(SyncState::load().save());  // the new account's first save drops the old scopes
  KOREADER_STORE.username = "reader";
  EXPECT_FALSE(SyncState::findScope("/Read", found));
}

TEST_F(SyncPendingTest, TheWholeCardScopeIsNeverEvicted) {
  SyncScope scope;
  scope.path = "/";
  scope.prepared = true;
  ASSERT_TRUE(SyncState::putScope(scope));  // oldest of all
  for (size_t i = 0; i < SyncState::MAX_SCOPES; ++i) {
    scope.path = "/folder" + std::to_string(i);
    ASSERT_TRUE(SyncState::putScope(scope));
  }
  SyncScope found;
  EXPECT_TRUE(SyncState::findScope("/", found));
  EXPECT_FALSE(SyncState::findScope("/folder0", found));
}

TEST_F(SyncPendingTest, VersionOneStateIsReadAndUpgraded) {
  {
    SyncFiles::Writer writer(SyncState::PATH);
    const uint8_t version = 1;
    const uint8_t flags = 1;  // stats caught up
    const uint32_t statsId = 0, clippingsId = 5, next = 77;
    ASSERT_TRUE(writer.open() && writer.pod(version) && writer.string16(KOREADER_STORE.baseUrl) &&
                writer.string16(KOREADER_STORE.username) && writer.pod(flags) && writer.pod(statsId) &&
                writer.pod(clippingsId) && writer.pod(next) && writer.commit());
  }
  SyncState state = SyncState::load();
  EXPECT_TRUE(state.statsCaughtUp);
  EXPECT_EQ(state.clippingsCatchupId, 5u);
  EXPECT_EQ(state.nextStampId, 77u);
  SyncScope scope;
  scope.path = "/Read";
  ASSERT_TRUE(SyncState::putScope(scope));
  SyncScope found;
  EXPECT_TRUE(SyncState::findScope("/Read", found));
  EXPECT_TRUE(SyncState::load().statsCaughtUp);
}
