#include <HalStorage.h>
#include <SecureHttpClient.h>
#include <gtest/gtest.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "SyncJsonAllocator.h"

bool KOReaderCredentialStore::hasCredentials() const { return true; }
std::string KOReaderCredentialStore::getBaseUrl() const { return "https://sync.test"; }
std::string KOReaderCredentialStore::getMd5Password() const { return "hash"; }
SyncServerSupport KOReaderCredentialStore::getServerSupport() const { return SyncServerSupport::SUPPORTED; }

namespace {
using Client = KOReaderSyncClient;
constexpr const char* PATH = "/.crosspoint/progress-sync.tmp";
const std::string DOCUMENT(32, 'a');
std::string page(size_t textSize = 5, uint32_t cursor = 101) {
  return "{\"cursor\":" + std::to_string(cursor) + ",\"more\":false,\"changes\":[{\"document\":\"" + DOCUMENT +
         "\",\"percentage\":0.5,\"progress\":\"" + std::string(textSize, 'x') + "\",\"device\":\"reader\"}]}";
}
bool collect(const KOReaderProgressChange& change, void* context) {
  static_cast<std::vector<KOReaderProgressChange>*>(context)->push_back(change);
  return true;
}
class ProgressSync : public ::testing::Test {
 protected:
  std::vector<KOReaderProgressChange> changes;
  Client::ChangesPage result;
  void SetUp() override {
    testHttp = {};
    ESP = {};
    Storage.files.clear();
    failBookmarkWriteOpen = failBookmarkSync = failBookmarkClose = false;
    maxBookmarkWriteBytes = shortReadAfter = -1;
    // Any JSON file read must happen after TLS has released its connection.
    bookmarkReadHook = [] { EXPECT_FALSE(testHttp.connected); };
  }
  void TearDown() override {
    bookmarkReadHook = nullptr;
    EXPECT_FALSE(testHttp.connected);
    EXPECT_FALSE(bookmarkMockWriting);
    EXPECT_EQ(openBookmarkReaders, 0);
    EXPECT_FALSE(Storage.files.contains(PATH));
  }
  Client::Error pull(uint8_t limit = 10) { return Client::getProgressChanges(100, limit, result, collect, &changes); }
};

TEST_F(ProgressSync, ParsesAfterTlsClosesAndAcceptsLargerThanSoftServerBudget) {
  testHttp.replies = {{page(9000)}};
  ASSERT_EQ(pull(), Client::OK);
  ASSERT_EQ(changes.size(), 1U);
  EXPECT_EQ(changes[0].progress.size(), 9000U);
  EXPECT_EQ(result.cursor, 101U);
}

TEST_F(ProgressSync, FragmentedHeapAdmissionMatchesRsaWorkspace) {
  // Reproduced after the first RSA-3072 feed page on C3: ample total heap,
  // but no 20 KB block. SP RSA can still connect with the 18.4 KB block.
  ESP.free = 42868;
  ESP.largest = 18420;
  testHttp.replies = {{page()}};
#if defined(WOLFSSL_HAVE_SP_RSA)
  EXPECT_EQ(pull(), Client::OK);
  EXPECT_EQ(changes.size(), 1U);
#else
  EXPECT_EQ(pull(), Client::LOW_MEMORY);
  EXPECT_TRUE(testHttp.urls.empty());
#endif
}

TEST_F(ProgressSync, RetriesOversizeAtSameCursorWithoutDeliveringPartialChanges) {
  testHttp.replies = {{page(17000)}, {page(17000)}, {page(17000)}, {page()}};
  ASSERT_EQ(pull(), Client::OK);
  ASSERT_EQ(testHttp.urls.size(), 4U);
  for (size_t i = 0; const int limit : {10, 5, 2, 1}) {
    EXPECT_NE(testHttp.urls[i].find("since=100&limit=" + std::to_string(limit) + "&"), std::string::npos);
    ++i;
  }
  EXPECT_EQ(changes.size(), 1U);
  EXPECT_EQ(result.cursor, 101U);
}
TEST_F(ProgressSync, NeverSkipsAnOversizedSingleChange) {
  testHttp.replies = {{page(17000)}};
  EXPECT_EQ(pull(), Client::RESPONSE_TOO_LARGE);
  EXPECT_EQ(testHttp.urls.size(), 4U);
  EXPECT_TRUE(changes.empty());
  EXPECT_EQ(result.cursor, 0U);
  EXPECT_LT(testHttp.streamedBytes, 4U * 17000);
}
TEST_F(ProgressSync, RejectsIncompleteButParseableResponseWithoutRetryOrCursor) {
  testHttp.replies = {{page(), 200, false}};
  EXPECT_EQ(pull(), Client::NETWORK_ERROR);
  EXPECT_EQ(testHttp.urls.size(), 1U);
  EXPECT_TRUE(changes.empty());
  EXPECT_EQ(result.cursor, 0U);
}
TEST_F(ProgressSync, BoundsJsonNodesAndRetriesMemoryFailuresWithoutCallingItUnsupported) {
  // Many tiny alias nodes fit on the wire but exceed the parser's live budget.
  std::string body = page();
  body.insert(
      body.find("\"percentage\""), "\"aliases\":[" + std::string("\"a\"") + [&] {
        std::string aliases;
        for (unsigned i = 0; i < 2000; ++i) aliases += ",\"a\"";
        return aliases;
      }() + "],");
  ASSERT_LT(body.size(), 16U * 1024);
  testHttp.replies = {{body}};
  EXPECT_EQ(pull(), Client::LOW_MEMORY);
  EXPECT_EQ(testHttp.urls.size(), 4U);
  EXPECT_EQ(result.cursor, 0U);
  EXPECT_TRUE(changes.empty());
}
TEST_F(ProgressSync, CursorProbeDoesNotRetryAsAPage) {
  testHttp.replies = {{page(17000)}};
  EXPECT_EQ(pull(0), Client::RESPONSE_TOO_LARGE);
  EXPECT_EQ(testHttp.urls.size(), 1U);
}
TEST_F(ProgressSync, PreservesLegacyAndHttpResults) {
  for (const auto [status, expected] : {std::pair{401, Client::AUTH_FAILED},
                                        {404, Client::FEED_UNSUPPORTED},
                                        {500, Client::SERVER_ERROR},
                                        {-1, Client::NETWORK_ERROR}}) {
    testHttp = {};
    testHttp.replies = {{"", status}};
    EXPECT_EQ(pull(), expected);
    EXPECT_EQ(result.cursor, 0U);
  }
  testHttp = {};
  testHttp.replies = {{"{\"devices\":[]}"}};
  EXPECT_EQ(pull(), Client::FEED_UNSUPPORTED);
}
TEST_F(ProgressSync, StorageFailuresCannotAdvanceCursorOrBecomeLegacyFallback) {
  testHttp.replies = {{page()}};
  failBookmarkWriteOpen = true;
  EXPECT_EQ(pull(), Client::STORAGE_ERROR);
  failBookmarkWriteOpen = false;
  maxBookmarkWriteBytes = 1;
  EXPECT_EQ(pull(), Client::STORAGE_ERROR);
  maxBookmarkWriteBytes = -1;
  failBookmarkSync = true;
  EXPECT_EQ(pull(), Client::STORAGE_ERROR);
  failBookmarkSync = false;
  failBookmarkClose = true;
  EXPECT_EQ(pull(), Client::STORAGE_ERROR);
  failBookmarkClose = false;
  shortReadAfter = 10;
  EXPECT_EQ(pull(), Client::STORAGE_ERROR);
  EXPECT_TRUE(changes.empty());
  EXPECT_EQ(result.cursor, 0U);
}
TEST_F(ProgressSync, PreservesTlsFloors) {
  ESP.free = 34999;
  EXPECT_EQ(pull(), Client::LOW_MEMORY);
  ESP.free = 45000;
  ESP.largest = Client::MIN_MAX_ALLOC_HEAP_FOR_TLS - 1;
  EXPECT_EQ(pull(), Client::LOW_MEMORY);
  EXPECT_TRUE(testHttp.urls.empty());
}
TEST_F(ProgressSync, SingleProgressUsesSameStagedAndCompleteResponseRules) {
  KOReaderProgress progress;
  testHttp.replies = {{"{\"percentage\":0.5,\"progress\":\"/body/p[1]\",\"position\":{\"pctQ\":500000,\"pages\":2}}"}};
  ASSERT_EQ(Client::getProgress(DOCUMENT, progress), Client::OK);
  EXPECT_EQ(progress.document, DOCUMENT);
  ASSERT_TRUE(progress.position.has_value());
  EXPECT_EQ(progress.position->totalPages, 2);
  testHttp = {};
  testHttp.replies = {{"{\"percentage\":0.5}", 200, false}};
  EXPECT_EQ(Client::getProgress(DOCUMENT, progress), Client::NETWORK_ERROR);
}
TEST_F(ProgressSync, BoundsIgnoredPutReplyAndRejectsIncompleteAcknowledgement) {
  KOReaderProgress progress{};
  progress.document = DOCUMENT;
  progress.progress = "/body/p[1]";
  testHttp.replies = {{"{}"}};
  EXPECT_EQ(Client::updateProgress(progress), Client::OK);
  testHttp = {};
  testHttp.replies = {{std::string(10000, 'x')}};
  EXPECT_EQ(Client::updateProgress(progress), Client::NETWORK_ERROR);
  EXPECT_LT(testHttp.streamedBytes, 4300U);
  testHttp = {};
  testHttp.replies = {{"{}", 200, false}};
  std::vector<KOReaderProgress> batch{progress};
  EXPECT_EQ(Client::updateProgressBatch(batch), Client::NETWORK_ERROR);
}
TEST_F(ProgressSync, AcceptsEmpty204AcknowledgementsWithoutContentLength) {
  KOReaderProgress progress{};
  progress.document = DOCUMENT;
  progress.progress = "/body/p[1]";
  testHttp.replies = {{"", 204, false}};
  EXPECT_EQ(Client::updateProgress(progress), Client::OK);
  testHttp = {};
  testHttp.replies = {{"", 204, false}};
  std::vector<KOReaderProgress> batch{progress};
  EXPECT_EQ(Client::updateProgressBatch(batch), Client::OK);
}
TEST(SyncJsonBudget, LimitsLiveBytesAndPreservesBlocksAfterFailedGrowth) {
  SyncJsonAllocator allocator;
  auto* first = static_cast<char*>(allocator.allocate(SyncJsonAllocator::CAPACITY / 2 - 64));
  ASSERT_NE(first, nullptr);
  first[0] = 'x';
  auto* second = allocator.allocate(SyncJsonAllocator::CAPACITY / 2 - 64);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(allocator.allocate(1000), nullptr);
  EXPECT_EQ(allocator.reallocate(first, SyncJsonAllocator::CAPACITY / 2 + 1000), nullptr);
  EXPECT_EQ(first[0], 'x');
  allocator.deallocate(second);
  first = static_cast<char*>(allocator.reallocate(first, SyncJsonAllocator::CAPACITY / 2 + 1000));
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first[0], 'x');
  allocator.deallocate(first);
  EXPECT_EQ(allocator.allocate(SIZE_MAX), nullptr);
}
}  // namespace
