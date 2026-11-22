#include <ArduinoJson.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <SecureHttpClient.h>
#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "BookmarkStore.h"
#include "BookmarkSync.h"

namespace {
constexpr const char* BOOK = "/book.epub";
constexpr const char* DOC = "0123456789abcdef0123456789abcdef";
using Result = StatsUploadClient::Result;
unsigned pushes, deletes;
bool rejectAck;
std::string remotePage;
std::vector<std::pair<uint64_t, std::string>> feed;
std::vector<uint64_t> getCursors;
std::string pageReply(uint64_t cursor, const std::string& items, bool more = false) {
  return std::string("{\"document\":\"") + DOC + "\",\"sync_version\":2,\"items\":[" + items +
         "],\"cursor\":" + std::to_string(cursor) + ",\"more\":" + (more ? "true" : "false") + "}";
}
std::string liveRow(const std::string& id, uint64_t revision) {
  return "{\"id\":\"" + id + "\",\"revision\":" + std::to_string(revision) +
         R"(,"xpath":"/test/incoming","percentage":0.4,"summary":"server row"})";
}
std::string deadRow(const std::string& id, uint64_t revision) {
  return "{\"id\":\"" + id + "\",\"revision\":" + std::to_string(revision) + R"(,"deleted":1})";
}

class BookmarkSyncExchange : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.files.clear();
    failBookmarkRenameFrom.clear();
    failBookmarkRemove.clear();
    testHttp = {};
    testCredentials = {};
    ESP = {};
    pushes = deletes = 0;
    rejectAck = false;
    remotePage.clear();
    feed.clear();
    getCursors.clear();
    testHttp.onRequest = [](const std::string& url, const char* method) {
      if (!strcmp(method, "GET")) {
        const auto from = url.find("&cursor=");
        const auto cursor = std::stoull(url.substr(from + 8));
        getCursors.push_back(cursor);
        if (!remotePage.empty()) {
          JsonDocument reply;
          if (deserializeJson(reply, remotePage) || !reply["cursor"].is<uint64_t>() ||
              reply["cursor"].as<uint64_t>() > cursor) {
            testHttp.body = remotePage;
            return;
          }
        }
        size_t count = 0;
        uint64_t next = cursor;
        bool more = false;
        std::string rows;
        for (const auto& [rev, row] : feed) {
          if (rev <= cursor) continue;
          if (count == 4) {
            more = true;
            break;
          }
          if (count++) rows += ',';
          rows += row;
          next = rev;
        }
        testHttp.body = pageReply(next, rows, more);
        return;
      }
      ++pushes;
      JsonDocument body;
      EXPECT_FALSE(deserializeJson(body, testHttp.uploaded));
      if (body["items"][0]["deleted"] == 1) ++deletes;
      testHttp.body = rejectAck ? R"({"until":1,"accepted":0})" : R"({"until":1,"accepted":1})";
    };
    BookmarkStore book;
    ASSERT_TRUE(book.loadForBook(BOOK, "Book", "Author", "epub"));
    ASSERT_EQ(book.addBookmark(3, 0.4f, 11, "Chapter", 2, "actual snippet"), BookmarkStore::AddResult::Added);
  }
  void eraseBookmark(size_t index = 0) {
    BookmarkStore book;
    ASSERT_TRUE(book.loadForBook(BOOK, "Book", "Author", "epub"));
    ASSERT_TRUE(book.removeBookmarkAt(index));
    ASSERT_TRUE(book.flushSyncChanges());
  }
};

TEST_F(BookmarkSyncExchange, NeverSendsWhenOffUnsupportedOrMalformed) {
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::Skipped);
  testCredentials.bookmarks = true;
  testCredentials.type = KOReaderServerType::KOSYNC;
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::Unsupported);
  testCredentials.type = KOReaderServerType::CROSSPOINT;
  testHttp.status = 404;
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::Unsupported);
  testHttp.status = 200;
  remotePage = "{}";
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::InvalidResponse);
  EXPECT_EQ(pushes, 0U);
}

TEST_F(BookmarkSyncExchange, AckIdempotenceAndAccountScopedTombstones) {
  testCredentials.bookmarks = true;
  rejectAck = true;
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::InvalidResponse);
  EXPECT_EQ(pushes, 1U);
  rejectAck = false;
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  EXPECT_NE(testHttp.uploaded.find("actual snippet"), std::string::npos);
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  EXPECT_EQ(pushes, 2U);
  eraseBookmark();
  testCredentials.user = "another";
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::InvalidResponse);
  EXPECT_EQ(deletes, 0U);
  testCredentials.user = "reader";
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  EXPECT_EQ(deletes, 1U);
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  EXPECT_EQ(deletes, 1U);
}

TEST_F(BookmarkSyncExchange, MissingFileIsNotAnIntentionalDeleteAll) {
  testCredentials.bookmarks = true;
  ASSERT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  for (const auto& name : Storage.listFiles("/.crosspoint/bookmarks"))
    Storage.remove(("/.crosspoint/bookmarks/" + name).c_str());
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::InvalidResponse);
  EXPECT_EQ(deletes, 0U);
}
TEST_F(BookmarkSyncExchange, ImportsNormalizedIntraChapterAnchorAndRemoteDeletion) {
  testCredentials.bookmarks = true;
  remotePage =
      R"({"document":"0123456789abcdef0123456789abcdef","sync_version":2,"items":[{"id":"ko-abc123","revision":1,"xpath":"/test/incoming","percentage":0.4,"summary":"remote bookmark"}],"cursor":1,"more":false})";
  ASSERT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  ASSERT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  BookmarkStore book;
  ASSERT_TRUE(book.loadForBook(BOOK, "Book", "Author", "epub"));
  ASSERT_EQ(book.getBookmarks().size(), 2U);
  const auto& imported = book.getBookmarks()[1];
  EXPECT_EQ(imported.spineIndex, 3U);
  EXPECT_EQ(imported.paragraphIndex, 42U);
  EXPECT_NEAR(imported.progress, 0.4f, 0.002f);
  EXPECT_STREQ(imported.snippet, "remote bookmark");
  book.unload();
  remotePage =
      R"({"document":"0123456789abcdef0123456789abcdef","sync_version":2,"items":[{"id":"ko-abc123","revision":2,"deleted":1}],"cursor":2,"more":false})";
  ASSERT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  ASSERT_TRUE(book.loadForBook(BOOK, "Book", "Author", "epub"));
  ASSERT_EQ(book.getBookmarks().size(), 1U);
  EXPECT_EQ(deletes, 0U);
}
TEST_F(BookmarkSyncExchange, RemoteTombstoneWinsWhenLivePutLostItsAcknowledgment) {
  testCredentials.bookmarks = true;
  rejectAck = true;
  ASSERT_EQ(BookmarkSync::run(BOOK, DOC), Result::InvalidResponse);
  JsonDocument sent;
  ASSERT_FALSE(deserializeJson(sent, testHttp.uploaded));
  const std::string id = sent["items"][0]["id"].as<std::string>();
  remotePage = std::string("{\"document\":\"") + DOC + "\",\"sync_version\":2,\"items\":[{\"id\":\"" + id +
               "\",\"revision\":2,\"deleted\":1}],\"cursor\":2,\"more\":false}";
  rejectAck = false;
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  EXPECT_EQ(pushes, 1U);
  BookmarkStore book;
  ASSERT_TRUE(book.loadForBook(BOOK, "Book", "Author", "epub"));
  ASSERT_TRUE(book.getBookmarks().empty());
}

TEST_F(BookmarkSyncExchange, RemovingOneOfTwoLocalAliasesDoesNotDeleteRemoteItem) {
  testCredentials.bookmarks = true;
  BookmarkStore book;
  ASSERT_TRUE(book.loadForBook(BOOK, "Book", "Author", "epub"));
  ASSERT_EQ(book.addBookmark(3, 0.8f, 11, "Chapter", 8), BookmarkStore::AddResult::Added);
  book.unload();
  ASSERT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  ASSERT_EQ(pushes, 1U);
  eraseBookmark();
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  EXPECT_EQ(deletes, 0U);
  eraseBookmark();
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  EXPECT_EQ(deletes, 1U);
}
TEST_F(BookmarkSyncExchange, NewerServerReAddWinsWhenLocalTombstoneLostItsAcknowledgment) {
  testCredentials.bookmarks = true;
  ASSERT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  JsonDocument sent;
  ASSERT_FALSE(deserializeJson(sent, testHttp.uploaded));
  const std::string id = sent["items"][0]["id"].as<std::string>();
  remotePage = pageReply(1, liveRow(id, 1));
  ASSERT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  eraseBookmark();
  rejectAck = true;
  ASSERT_EQ(BookmarkSync::run(BOOK, DOC), Result::InvalidResponse);
  ASSERT_EQ(deletes, 1U);
  remotePage = pageReply(3, liveRow(id, 3));
  rejectAck = false;
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  EXPECT_EQ(deletes, 1U);
  BookmarkStore book;
  ASSERT_TRUE(book.loadForBook(BOOK, "Book", "Author", "epub"));
  ASSERT_EQ(book.getBookmarks().size(), 1U);
  EXPECT_STREQ(book.getBookmarks()[0].snippet, "server row");
}

TEST_F(BookmarkSyncExchange, DeliberateLocalReAddSurvivesLostDeleteReply) {
  testCredentials.bookmarks = true;
  ASSERT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  JsonDocument sent;
  ASSERT_FALSE(deserializeJson(sent, testHttp.uploaded));
  const std::string id = sent["items"][0]["id"].as<std::string>();
  eraseBookmark();
  rejectAck = true;
  ASSERT_EQ(BookmarkSync::run(BOOK, DOC), Result::InvalidResponse);
  BookmarkStore book;
  ASSERT_TRUE(book.loadForBook(BOOK, "Book", "Author", "epub"));
  ASSERT_EQ(book.addBookmark(3, 0.4f, 11, "Chapter", 2, "deliberately re-added"), BookmarkStore::AddResult::Added);
  book.unload();
  rejectAck = false;
  remotePage = pageReply(2, deadRow(id, 2));
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  ASSERT_FALSE(deserializeJson(sent, testHttp.uploaded));
  EXPECT_EQ(sent["items"][0]["id"].as<std::string>(), id);
  EXPECT_STREQ(sent["items"][0]["summary"].as<const char*>(), "deliberately re-added");
  ASSERT_TRUE(book.loadForBook(BOOK, "Book", "Author", "epub"));
  EXPECT_EQ(book.getBookmarks().size(), 1U);
}

TEST_F(BookmarkSyncExchange, AtCapacityReachesLaterTombstonesWithoutSkippingBlockedImports) {
  testCredentials.bookmarks = true;
  // Start without a local edit so the bounded ledger contains only remote IDs.
  BookmarkStore::deleteForFilePath(BOOK, "epub");
  for (uint64_t i = 1; i <= 129; ++i) feed.emplace_back(i, liveRow("ko-" + std::to_string(i), i));
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::Cancelled);
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::Cancelled);
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::LowMemory);
  EXPECT_EQ(pushes, 0U);
  getCursors.clear();
  // The server lists only the latest row per ID, so replace ID 1 with its tombstone.
  feed.erase(feed.begin());
  feed.emplace_back(130, deadRow("ko-1", 130));
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  ASSERT_GE(getCursors.size(), 2U);
  EXPECT_EQ(getCursors[0], 129U);  // Read later tombstone without advancing main cursor.
  EXPECT_EQ(getCursors[1], 128U);  // Replay the blocked live record now space is available.
  EXPECT_EQ(pushes, 0U);
}
TEST_F(BookmarkSyncExchange, InterruptedBookmarkMoveCannotExportToAnotherAccount) {
  testCredentials.bookmarks = true;
  ASSERT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  constexpr const char* moved = "/moved.epub";
  const auto oldLedger = BookmarkStore::syncStatePath(BOOK);
  const auto newLedger = BookmarkStore::syncStatePath(moved);
  BookmarkStore::RenameMigration migration;
  ASSERT_TRUE(BookmarkStore::beginRenameMigration(BOOK, moved, "Book", "Author", "epub", migration));
  ASSERT_TRUE(BookmarkStore::syncBlockedByMove(moved));
  testCredentials.user = "second-account";
  const auto requestsBefore = testHttp.streamedRequests;
  const auto putsBefore = pushes;
  // Simulates waking after the new bookmark file is durable but before its ledger is moved.
  EXPECT_EQ(BookmarkSync::run(moved, DOC), Result::InvalidResponse);
  EXPECT_EQ(testHttp.streamedRequests, requestsBefore);
  ASSERT_TRUE(BookmarkStore::rollbackRenameMigration(migration));
  ASSERT_FALSE(BookmarkStore::syncBlockedByMove(moved));

  ASSERT_TRUE(BookmarkStore::beginRenameMigration(BOOK, moved, "Book", "Author", "epub", migration));
  // Simulate failure publishing main after successfully moving the backup.
  Storage.files[oldLedger + ".bak"] = Storage.files.at(oldLedger);
  failBookmarkRenameFrom = oldLedger;
  EXPECT_FALSE(BookmarkStore::commitRenameMigration(migration));
  EXPECT_TRUE(BookmarkStore::syncBlockedByMove(moved));
  EXPECT_TRUE(Storage.exists(oldLedger.c_str()));
  EXPECT_TRUE(Storage.exists((oldLedger + ".bak").c_str()));
  EXPECT_FALSE(Storage.exists(newLedger.c_str()));
  EXPECT_EQ(BookmarkSync::run(moved, DOC), Result::InvalidResponse);
  EXPECT_EQ(testHttp.streamedRequests, requestsBefore);
  EXPECT_EQ(pushes, putsBefore);
  BookmarkStore local;
  ASSERT_TRUE(local.loadForBook(moved, "Book", "Author", "epub"));
  EXPECT_EQ(local.getBookmarks().size(), 1U);  // Only sync, not local reading, is blocked.
  failBookmarkRenameFrom.clear();
}

TEST_F(BookmarkSyncExchange, DirectMoveRetainsGuardOnLedgerTransferFailure) {
  testCredentials.bookmarks = true;
  ASSERT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  constexpr const char* moved = "/moved.epub";
  failBookmarkRenameFrom = BookmarkStore::syncStatePath(BOOK);
  EXPECT_FALSE(BookmarkStore::migrateForFilePath(BOOK, moved, "Book", "Author", "epub"));
  EXPECT_TRUE(BookmarkStore::syncBlockedByMove(moved));
  const auto requestsBefore = testHttp.streamedRequests;
  testCredentials.user = "second-account";
  EXPECT_EQ(BookmarkSync::run(moved, DOC), Result::InvalidResponse);
  EXPECT_EQ(testHttp.streamedRequests, requestsBefore);
  failBookmarkRenameFrom.clear();
}
TEST_F(BookmarkSyncExchange, OldPathCannotExportIfSourceCleanupFailsAfterLedgerMove) {
  testCredentials.bookmarks = true;
  ASSERT_EQ(BookmarkSync::run(BOOK, DOC), Result::Ok);
  constexpr const char* moved = "/moved.epub";
  const auto oldLedger = BookmarkStore::syncStatePath(BOOK);
  const auto newLedger = BookmarkStore::syncStatePath(moved);
  BookmarkStore::RenameMigration migration;
  ASSERT_TRUE(BookmarkStore::beginRenameMigration(BOOK, moved, "Book", "Author", "epub", migration));
  // Fail deletion of the old backup, which an ordinary reopen can recover.
  Storage.files[migration.sourceCurrentPath + ".bak"] = Storage.files.at(migration.sourceCurrentPath);
  failBookmarkRemove = migration.sourceCurrentPath + ".bak";
  EXPECT_FALSE(BookmarkStore::commitRenameMigration(migration));
  EXPECT_FALSE(Storage.exists(oldLedger.c_str()));
  EXPECT_TRUE(Storage.exists(newLedger.c_str()));
  EXPECT_TRUE(BookmarkStore::syncBlockedByMove(BOOK));
  EXPECT_FALSE(BookmarkStore::syncBlockedByMove(moved));
  const auto requestsBefore = testHttp.streamedRequests;
  testCredentials.user = "second-account";
  EXPECT_EQ(BookmarkSync::run(BOOK, DOC), Result::InvalidResponse);
  EXPECT_EQ(testHttp.streamedRequests, requestsBefore);
  BookmarkStore local;
  ASSERT_TRUE(local.loadForBook(BOOK, "Book", "Author", "epub"));
  EXPECT_EQ(local.getBookmarks().size(), 1U);
}
}  // namespace
