#include <gtest/gtest.h>

#include <string>

#include "../../src/activities/reader/BookReadingStats.h"
#include "../../src/activities/reader/BookStatsTracking.h"
#include "../../src/sync/BookSyncFile.h"
#include "../../src/sync/SyncIds.h"
#include "../../src/sync/SyncPending.h"
#include "../../src/util/BookCacheUtils.h"
#include "CrossPointSettings.h"
#include "Epub.h"
#include "HalStorage.h"
#include "KOReaderDocumentId.h"

// Stub only the content hash and completion peek; persistence and sync tracking
// use the production implementations below.
unsigned documentHashCalls = 0;
std::string KOReaderDocumentId::calculate(const std::string& path, uint32_t* sampledSize) {
  ++documentHashCalls;
  const auto& bytes = fake::files.at(path)->bytes;
  if (sampledSize) *sampledSize = bytes.size();
  return std::string(32, bytes.empty() ? '0' : static_cast<char>(bytes[0]));
}
std::string KOReaderDocumentId::calculateFromFilename(const std::string&) { return std::string(32, 'f'); }
bool BookReadingStats::peekCompleted(const std::string&) { return false; }

namespace {

class BookCacheUtilsTest : public ::testing::Test {
 protected:
  std::string cachePath;

  void SetUp() override {
    fake::reset();
    KOREADER_STORE = {};
    documentHashCalls = 0;
    SETTINGS.trackReadingStats = 1;
    cachePath = Epub("/a.epub", "/.crosspoint").getCachePath();
    fake::add(cachePath + "/progress.bin", "progress");
    fake::add(cachePath + "/stats_v6.bin", "stats");
    fake::add(cachePath + "/reader_settings.bin", "settings");
    fake::add(cachePath + "/reading_stats_off", "off");
    fake::add(cachePath + "/book.bin", "derived");
    fake::add("/a.epub", "aaaa");
  }

  void seedSyncHistory() {
    BookSyncFile record;
    record.hasId = true;
    record.method = DocumentMatchMethod::BINARY;
    std::fill(std::begin(record.id), std::end(record.id), 0xaa);
    record.fileSize = 4;
    record.statsStamp = 11;
    record.clippingsStamp = 12;
    record.walkStamp = 13;
    record.idsGeneration = SyncIds::record("/a.epub", record.id);
    ASSERT_NE(record.idsGeneration, 0u);
    ASSERT_TRUE(BookSyncFile::write(cachePath, record));
    ASSERT_TRUE(BookSyncFile::write(cachePath, record));
  }

  BookSyncFile expectSyncHistoryIntact() {
    BookSyncFile record;
    EXPECT_TRUE(BookSyncFile::read(cachePath, record));
    EXPECT_EQ(record.statsStamp, 11u);
    EXPECT_EQ(record.clippingsStamp, 12u);
    EXPECT_EQ(record.walkStamp, 13u);
    return record;
  }

  void expectUserStateIntact() {
    const auto expectFile = [](const std::string& path, const std::string& expected) {
      ASSERT_TRUE(Storage.exists(path.c_str())) << path;
      const auto& bytes = fake::files.at(path)->bytes;
      EXPECT_EQ(std::string(bytes.begin(), bytes.end()), expected);
    };
    expectFile(cachePath + "/progress.bin", "progress");
    expectFile(cachePath + "/stats_v6.bin", "stats");
    expectFile(cachePath + "/reader_settings.bin", "settings");
    expectFile(cachePath + "/reading_stats_off", "off");
  }
};

TEST_F(BookCacheUtilsTest, ClearsDerivedFilesWhileKeepingReadingState) {
  ASSERT_TRUE(clearBookCachePreservingUserState("/a.epub"));
  expectUserStateIntact();
  EXPECT_FALSE(Storage.exists((cachePath + "/book.bin").c_str()));
  EXPECT_EQ(fake::directoryEntriesByPath[cachePath], 0u);
}

TEST_F(BookCacheUtilsTest, RetryRecoversStatsAfterFailedRestore) {
  fake::failRenameToPath = cachePath + "/stats_v6.bin";
  ASSERT_FALSE(clearBookCachePreservingUserState("/a.epub"));
  ASSERT_TRUE(Storage.exists((cachePath + ".upload_preserve_stats_v6.bin").c_str()));
  ASSERT_TRUE(Storage.exists((cachePath + ".upload_preserve_stats_pending").c_str()));

  ASSERT_TRUE(clearBookCachePreservingUserState("/a.epub"));
  expectUserStateIntact();
  EXPECT_FALSE(Storage.exists((cachePath + ".upload_preserve_stats_v6.bin").c_str()));
  EXPECT_FALSE(Storage.exists((cachePath + ".upload_preserve_stats_pending").c_str()));
}

TEST_F(BookCacheUtilsTest, RetryRecoversProgressAfterFailedRestore) {
  fake::failRenameToPath = cachePath + "/progress.bin";
  ASSERT_FALSE(clearBookCachePreservingUserState("/a.epub"));
  ASSERT_TRUE(Storage.exists((cachePath + ".upload_preserve_progress.bin").c_str()));

  ASSERT_TRUE(clearBookCachePreservingUserState("/a.epub"));
  expectUserStateIntact();
  EXPECT_FALSE(Storage.exists((cachePath + ".upload_preserve_progress.bin").c_str()));
}

TEST_F(BookCacheUtilsTest, BookTrackingDefaultsOnAndGlobalOffTakesPrecedence) {
  fake::files.erase(cachePath + "/reading_stats_off");
  EXPECT_TRUE(BookStatsTracking::isEnabled(cachePath));
  SETTINGS.trackReadingStats = 0;
  EXPECT_FALSE(BookStatsTracking::isEnabled(cachePath));
  SETTINGS.trackReadingStats = 1;
  EXPECT_TRUE(BookStatsTracking::isEnabled(cachePath));
}

TEST_F(BookCacheUtilsTest, BookChoiceSurvivesCacheClearAndStatsDeletion) {
  ASSERT_FALSE(BookStatsTracking::isBookEnabled(cachePath));
  ASSERT_TRUE(clearBookCachePreservingUserState("/a.epub"));
  ASSERT_FALSE(BookStatsTracking::isBookEnabled(cachePath));
  ASSERT_TRUE(Storage.remove((cachePath + "/stats_v6.bin").c_str()));
  EXPECT_FALSE(BookStatsTracking::isBookEnabled(cachePath));
  ASSERT_TRUE(BookStatsTracking::setBookEnabled(cachePath, true));
  EXPECT_TRUE(BookStatsTracking::isEnabled(cachePath));
}

TEST_F(BookCacheUtilsTest, FailedBookToggleKeepsPreviousChoice) {
  ASSERT_TRUE(BookStatsTracking::setBookEnabled(cachePath, true));
  fake::failRenameToPath = cachePath + "/reading_stats_off";
  EXPECT_FALSE(BookStatsTracking::setBookEnabled(cachePath, false));
  EXPECT_TRUE(BookStatsTracking::isBookEnabled(cachePath));
  EXPECT_FALSE(Storage.exists((cachePath + "/reading_stats_off.tmp").c_str()));
  ASSERT_TRUE(BookStatsTracking::setBookEnabled(cachePath, false));
  EXPECT_FALSE(BookStatsTracking::isEnabled(cachePath));
}

TEST_F(BookCacheUtilsTest, CacheClearsPreserveSyncMainBackupAndInterruptedWrite) {
  seedSyncHistory();
  const auto main = fake::files.at(cachePath + "/sync.bin")->bytes;
  const auto backup = fake::files.at(cachePath + "/sync.bin.bak")->bytes;
  fake::add(cachePath + "/sync.bin.tmp", "interrupted");
  ASSERT_TRUE(clearBookCachePreservingUserState("/a.epub"));
  EXPECT_EQ(fake::files.at(cachePath + "/sync.bin")->bytes, main);
  EXPECT_EQ(fake::files.at(cachePath + "/sync.bin.bak")->bytes, backup);
  EXPECT_TRUE(Storage.exists((cachePath + "/sync.bin.tmp").c_str()));
  EXPECT_TRUE(expectSyncHistoryIntact().hasId);
  ASSERT_TRUE(clearBookCacheDirectoryPreservingStats(cachePath));
  EXPECT_EQ(fake::files.at(cachePath + "/sync.bin")->bytes, main);
  EXPECT_EQ(fake::files.at(cachePath + "/sync.bin.bak")->bytes, backup);
  EXPECT_TRUE(Storage.exists((cachePath + "/sync.bin.tmp").c_str()));
}

TEST_F(BookCacheUtilsTest, CacheClearKeepsRecoverableSyncBackupWithoutMain) {
  seedSyncHistory();
  ASSERT_TRUE(Storage.remove((cachePath + "/sync.bin").c_str()));
  ASSERT_TRUE(clearBookCachePreservingUserState("/a.epub"));
  EXPECT_TRUE(expectSyncHistoryIntact().hasId);
}

TEST_F(BookCacheUtilsTest, RetryRecoversSyncAfterFailedRestore) {
  seedSyncHistory();
  fake::failRenameToPath = cachePath + "/sync.bin";
  ASSERT_FALSE(clearBookCachePreservingUserState("/a.epub"));
  EXPECT_TRUE(Storage.exists((cachePath + ".upload_preserve_sync.bin").c_str()));
  ASSERT_TRUE(clearBookCachePreservingUserState("/a.epub"));
  EXPECT_TRUE(expectSyncHistoryIntact().hasId);
}

TEST_F(BookCacheUtilsTest, SameSizeReplacementRecalculatesBinaryIdAndQueuesSavedData) {
  KOREADER_STORE.method = DocumentMatchMethod::BINARY;
  seedSyncHistory();
  ASSERT_EQ(BookSyncFile::documentId("/a.epub"), std::string(32, 'a'));
  ASSERT_EQ(documentHashCalls, 1u);
  // Web deletion removes the file and its pending/map entries, keeping user state.
  ASSERT_TRUE(SyncPending::mark("/a.epub", SyncPending::STATS_BIT));
  ASSERT_TRUE(Storage.remove("/a.epub"));
  clearBookCache("/a.epub");
  ASSERT_TRUE(SyncPending::removePath("/a.epub"));
  BookSyncFile::onDeleted("/a.epub");
  fake::add("/a.epub", "bbbb");
  ASSERT_TRUE(clearBookCachePreservingUserState("/a.epub", true));
  const auto invalidated = expectSyncHistoryIntact();
  EXPECT_FALSE(invalidated.hasId);
  EXPECT_EQ(invalidated.idsGeneration, 0u);
  unsigned mapped = 0;
  ASSERT_TRUE(SyncIds::forEach(
      [](const uint8_t (&)[16], const std::string&, void* context) {
        ++*static_cast<unsigned*>(context);
        return true;
      },
      &mapped));
  EXPECT_EQ(mapped, 0u);
  SyncPending::Cursor pending;
  ASSERT_TRUE(pending.begin());
  std::string path;
  uint8_t bits = 0;
  ASSERT_EQ(pending.next(path, bits), SyncPending::Cursor::Step::Entry);
  EXPECT_EQ(path, "/a.epub");
  EXPECT_EQ(bits, SyncPending::PROGRESS_BIT | SyncPending::STATS_BIT | SyncPending::CLIPPINGS_BIT);
  pending.end();
  // The fallback must also carry the invalidated id, not the old same-size id.
  const auto main = fake::files.at(cachePath + "/sync.bin")->bytes;
  ASSERT_TRUE(Storage.remove((cachePath + "/sync.bin").c_str()));
  EXPECT_FALSE(expectSyncHistoryIntact().hasId);
  fake::add(cachePath + "/sync.bin");
  fake::files.at(cachePath + "/sync.bin")->bytes = main;
  EXPECT_EQ(BookSyncFile::documentId("/a.epub"), std::string(32, 'b'));
  EXPECT_EQ(documentHashCalls, 2u);
  EXPECT_TRUE(expectSyncHistoryIntact().hasId);
  expectUserStateIntact();
}

TEST_F(BookCacheUtilsTest, FilenameReplacementKeepsDocumentIdentityAndHistory) {
  seedSyncHistory();
  const auto before = BookSyncFile::documentId("/a.epub");
  fake::add("/a.epub", "different length");
  ASSERT_TRUE(clearBookCachePreservingUserState("/a.epub", true));
  EXPECT_EQ(BookSyncFile::documentId("/a.epub"), before);
  EXPECT_EQ(documentHashCalls, 0u);
  expectSyncHistoryIntact();
}

TEST_F(BookCacheUtilsTest, FailedSyncInvalidationAbortsBeforeRemovingDerivedCache) {
  seedSyncHistory();
  fake::failRenameToPath = cachePath + "/sync.bin";
  EXPECT_FALSE(clearBookCachePreservingUserState("/a.epub", true));
  EXPECT_TRUE(Storage.exists((cachePath + "/book.bin").c_str()));
  expectSyncHistoryIntact();
  expectUserStateIntact();
}

TEST_F(BookCacheUtilsTest, UnreadableSyncHistoryIsKeptAndReported) {
  fake::add(cachePath + "/sync.bin", "corrupt");
  EXPECT_FALSE(clearBookCachePreservingUserState("/a.epub", true));
  EXPECT_TRUE(Storage.exists((cachePath + "/book.bin").c_str()));
  EXPECT_TRUE(Storage.exists((cachePath + "/sync.bin").c_str()));
}

TEST_F(BookCacheUtilsTest, FreshUploadDoesNotCreateBookSyncStateOrPendingEntry) {
  fake::reset();
  fake::add("/a.epub", "new book");
  ASSERT_TRUE(clearBookCachePreservingUserState("/a.epub", true));
  EXPECT_FALSE(Storage.exists(cachePath.c_str()));
  EXPECT_EQ(SyncPending::count(), 0u);
}

TEST_F(BookCacheUtilsTest, DirectSameSizeReplacementDropsOldIdMapBeforeRehashing) {
  KOREADER_STORE.method = DocumentMatchMethod::BINARY;
  seedSyncHistory();
  fake::add("/a.epub", "bbbb");
  ASSERT_TRUE(clearBookCachePreservingUserState("/a.epub", true));
  unsigned mapped = 0;
  ASSERT_TRUE(SyncIds::forEach(
      [](const uint8_t (&)[16], const std::string&, void* context) {
        ++*static_cast<unsigned*>(context);
        return true;
      },
      &mapped));
  EXPECT_EQ(mapped, 0u);
  EXPECT_EQ(BookSyncFile::documentId("/a.epub"), std::string(32, 'b'));
  EXPECT_EQ(documentHashCalls, 1u);
  ASSERT_TRUE(clearBookCachePreservingUserState("/a.epub"));
  EXPECT_EQ(BookSyncFile::documentId("/a.epub"), std::string(32, 'b'));
  EXPECT_EQ(documentHashCalls, 2u);
  EXPECT_TRUE(expectSyncHistoryIntact().hasId);
}

TEST_F(BookCacheUtilsTest, ReplacementRecoversCompletedSyncTempBeforeInvalidatingId) {
  seedSyncHistory();
  ASSERT_TRUE(Storage.rename((cachePath + "/sync.bin").c_str(), (cachePath + "/sync.bin.tmp").c_str()));
  ASSERT_TRUE(clearBookCachePreservingUserState("/a.epub", true));
  EXPECT_FALSE(expectSyncHistoryIntact().hasId);
}

TEST_F(BookCacheUtilsTest, ReplacementHonorsDisabledSyncTypes) {
  seedSyncHistory();
  KOREADER_STORE.stats = false;
  KOREADER_STORE.clippings = false;
  ASSERT_TRUE(clearBookCachePreservingUserState("/a.epub", true));
  SyncPending::Cursor pending;
  ASSERT_TRUE(pending.begin());
  std::string path;
  uint8_t bits = 0;
  ASSERT_EQ(pending.next(path, bits), SyncPending::Cursor::Step::Entry);
  EXPECT_EQ(bits, SyncPending::PROGRESS_BIT);
  expectSyncHistoryIntact();
}

TEST_F(BookCacheUtilsTest, ReplacementWithoutAccountKeepsHistoryWithoutQueuingUploads) {
  seedSyncHistory();
  KOREADER_STORE.credentials = false;
  ASSERT_TRUE(clearBookCachePreservingUserState("/a.epub", true));
  EXPECT_EQ(SyncPending::count(), 0u);
  expectSyncHistoryIntact();
}

TEST_F(BookCacheUtilsTest, NonBookUploadHasNoCacheToRefresh) {
  ASSERT_TRUE(clearBookCachePreservingUserState("/cover.jpg", true));
  EXPECT_EQ(SyncPending::count(), 0u);
}

TEST_F(BookCacheUtilsTest, XtcReplacementKeepsSyncHistoryAndQueuesOnlyStats) {
  const std::string path = "/a.xtc";
  const std::string xtcCache = BookSyncFile::cachePathFor(path);
  fake::add(path, "xtc");
  fake::add(xtcCache + "/progress.bin", "progress");
  BookSyncFile record;
  record.statsStamp = 42;
  ASSERT_TRUE(BookSyncFile::write(xtcCache, record));
  ASSERT_TRUE(clearBookCachePreservingUserState(path));
  ASSERT_TRUE(BookSyncFile::read(xtcCache, record));
  EXPECT_EQ(record.statsStamp, 42u);
  ASSERT_TRUE(clearBookCachePreservingUserState(path, true));
  ASSERT_TRUE(BookSyncFile::read(xtcCache, record));
  EXPECT_EQ(record.statsStamp, 42u);
  SyncPending::Cursor pending;
  ASSERT_TRUE(pending.begin());
  std::string queuedPath;
  uint8_t bits = 0;
  ASSERT_EQ(pending.next(queuedPath, bits), SyncPending::Cursor::Step::Entry);
  EXPECT_EQ(queuedPath, path);
  EXPECT_EQ(bits, SyncPending::STATS_BIT);
}

TEST_F(BookCacheUtilsTest, MovingNewBytesOntoDeletedPathPreservesDestinationHistoryWithFreshId) {
  KOREADER_STORE.method = DocumentMatchMethod::BINARY;
  seedSyncHistory();
  ASSERT_TRUE(Storage.remove("/a.epub"));
  clearBookCache("/a.epub");
  BookSyncFile::onDeleted("/a.epub");
  fake::add("/Inbox/a.epub", "bbbb");
  ASSERT_TRUE(Storage.rename("/Inbox/a.epub", "/a.epub"));
  ASSERT_TRUE(clearBookCachePreservingUserState("/a.epub", true));
  EXPECT_EQ(BookSyncFile::documentId("/a.epub"), std::string(32, 'b'));
  EXPECT_EQ(documentHashCalls, 1u);
  expectUserStateIntact();
  expectSyncHistoryIntact();
}

}  // namespace

TEST_F(BookCacheUtilsTest, ExternalSameSizeReplacementUpdatesTheMappedId) {
  KOREADER_STORE.method = DocumentMatchMethod::BINARY;
  seedSyncHistory();
  fake::add("/a.epub", "bbbb");
  ASSERT_EQ(BookSyncFile::documentId("/a.epub"), std::string(32, 'b'));
  unsigned count = 0;
  ASSERT_TRUE(SyncIds::forEach(
      [](const uint8_t (&id)[16], const std::string&, void* context) {
        EXPECT_TRUE(std::all_of(std::begin(id), std::end(id), [](uint8_t byte) { return byte == 0xbb; }));
        ++*static_cast<unsigned*>(context);
        return true;
      },
      &count));
  EXPECT_EQ(count, 1u);
}
