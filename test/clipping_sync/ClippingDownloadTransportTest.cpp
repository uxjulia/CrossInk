#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <SyncLog.h>
#include <gtest/gtest.h>

#include "StatsUploadClient.h"

namespace {
using Result = StatsUploadClient::Result;
constexpr const char* PATH = "/download.tmp";
constexpr size_t LIMIT = 26624;
class ClippingDownloadTransport : public ::testing::Test {
 protected:
  void SetUp() override {
    failBookmarkRenameFrom.clear();
    testCredentials.support = SyncServerSupport::UNSUPPORTED;
    testHttp = {};
    ESP = {};
    Storage.files.clear();
    failBookmarkWriteOpen = failBookmarkSync = failBookmarkClose = false;
    maxBookmarkWriteBytes = -1;
    shortReadAfter = -1;
    clippingTestPsramAvailable = false;
    maxClippingTestByteAllocation = SIZE_MAX;
  }
  void TearDown() override {
    EXPECT_FALSE(testHttp.connected);
    EXPECT_FALSE(bookmarkMockWriting);
  }
};

TEST_F(ClippingDownloadTransport, UploadsAndRequiresCompleteAcknowledgment) {
#ifdef CROSSINK_TEST_UPSTREAM_SDK
  ESP.free = 100000;
  ESP.largest = 60000;
#endif
  const std::string body(25000, 'x');
  Storage.files["/upload.tmp"] = body;
  testHttp.body = R"({"until":1,"accepted":1})";
  StatsUploadClient client;
  EXPECT_EQ(client.putFile("/clippings", "/upload.tmp", 1), Result::Ok);
  EXPECT_EQ(testHttp.uploaded, body);
  testHttp.complete = false;
  EXPECT_EQ(client.putFile("/clippings", "/upload.tmp", 1), Result::InvalidResponse);
  testHttp.complete = true;
  shortReadAfter = 512;
  EXPECT_EQ(client.putFile("/clippings", "/upload.tmp", 1), Result::Network);
  shortReadAfter = -1;
}

#ifdef CROSSINK_TEST_UPSTREAM_SDK
TEST_F(ClippingDownloadTransport, BufferedUploadPreservesTlsHeadroom) {
  Storage.files["/upload.tmp"].assign(25000, 'x');
  testHttp.body = R"({"until":1,"accepted":1})";
  StatsUploadClient client;
  EXPECT_EQ(client.putFile("/clippings", "/upload.tmp", 1), Result::LowMemory);
  EXPECT_EQ(testHttp.streamedRequests, 0U);
  EXPECT_EQ(Storage.files["/upload.tmp"].size(), 25000U);
}

TEST_F(ClippingDownloadTransport, BufferedUploadRejectsFragmentedHeap) {
  ESP.free = 100000;
  ESP.largest = 20000;
  Storage.files["/upload.tmp"].assign(25000, 'x');
  StatsUploadClient client;
  EXPECT_EQ(client.putFile("/clippings", "/upload.tmp", 1), Result::LowMemory);
  EXPECT_EQ(testHttp.streamedRequests, 0U);
}

TEST_F(ClippingDownloadTransport, BufferedUploadUsesPsramWithoutSpendingTlsHeadroom) {
  clippingTestPsramAvailable = true;
  const std::string body(25000, 'x');
  Storage.files["/upload.tmp"] = body;
  testHttp.body = R"({"until":1,"accepted":1})";
  StatsUploadClient client;
  EXPECT_EQ(client.putFile("/clippings", "/upload.tmp", 1), Result::Ok);
  EXPECT_EQ(testHttp.uploaded, body);
}

TEST_F(ClippingDownloadTransport, BufferedUploadHandlesAllocationFailure) {
  ESP.free = 100000;
  ESP.largest = 60000;
  maxClippingTestByteAllocation = 0;
  Storage.files["/upload.tmp"].assign(25000, 'x');
  StatsUploadClient client;
  EXPECT_EQ(client.putFile("/clippings", "/upload.tmp", 1), Result::LowMemory);
  EXPECT_EQ(testHttp.streamedRequests, 0U);
  EXPECT_EQ(Storage.files["/upload.tmp"].size(), 25000U);
}
#endif

TEST_F(ClippingDownloadTransport, CounterUploadsRequireExplicitProtocolAndBatchAcknowledgment) {
  StatsUploadClient client;
  for (const char* body : {R"({"until":1,"accepted":1})", R"({"until":1,"accepted":1,"counter_ids":0})",
                           R"({"until":1,"accepted":2,"counter_ids":1})"}) {
    testHttp.body = body;
    EXPECT_EQ(client.put("/api/v1/stats/counters/books", "{}", 1), Result::InvalidResponse);
  }
  testHttp.body = R"({"until":1,"accepted":1,"counter_ids":1})";
  EXPECT_EQ(client.put("/api/v1/stats/counters/books", "{}", 1), Result::Ok);
  client.close();
  for (const int status : {404, 405}) {
    testHttp.status = status;
    KOREADER_STORE.setServerSupport(SyncServerSupport::SUPPORTED);
    EXPECT_EQ(client.put("/api/v1/stats/counters/global", "{}", 0), Result::Server);
    EXPECT_EQ(KOREADER_STORE.getServerSupport(), SyncServerSupport::SUPPORTED);
    EXPECT_EQ(StatsUploadClient::lastHttpCode, status);
    KOREADER_STORE.setServerSupport(SyncServerSupport::UNSUPPORTED);
    EXPECT_EQ(client.put("/api/v1/stats/counters/global", "{}", 0), Result::Unsupported);
  }
}

TEST_F(ClippingDownloadTransport, StreamsMaximumResponseWithC3MemoryHeadroom) {
  // Enough for TLS, but not for TLS plus the former 26 KiB response buffer.
  testHttp.body.assign(LIMIT, 'x');
  StatsUploadClient client;
  EXPECT_EQ(client.get("/clippings", PATH, LIMIT), Result::Ok);
  EXPECT_EQ(testHttp.streamedRequests, 1U);
  EXPECT_EQ(Storage.files.at(PATH), testHttp.body);
}

TEST_F(ClippingDownloadTransport, RejectsOversizeAndIncompleteBodies) {
  StatsUploadClient client;
  testHttp.body.assign(LIMIT + 1, 'x');
  EXPECT_EQ(client.get("/clippings", PATH, LIMIT), Result::InvalidResponse);
  EXPECT_LE(Storage.files.at(PATH).size(), LIMIT);
  testHttp.body = "{partial";
  testHttp.complete = false;
  EXPECT_EQ(client.get("/clippings", PATH, LIMIT), Result::InvalidResponse);
}

TEST_F(ClippingDownloadTransport, RejectsStorageFailuresAndReleasesFile) {
  StatsUploadClient client;
  testHttp.body = "reply";
  failBookmarkWriteOpen = true;
  EXPECT_EQ(client.get("/clippings", PATH, LIMIT), Result::InvalidResponse);
  EXPECT_EQ(StatsUploadClient::lastHttpCode, 0);
  char detail[96];
  EXPECT_STREQ(
      StatsUploadClient::formatResult(detail, sizeof(detail), Result::InvalidResponse, StatsUploadClient::lastHttpCode),
      "Invalid sync data");
  failBookmarkWriteOpen = false;
  maxBookmarkWriteBytes = 2;
  EXPECT_EQ(client.get("/clippings", PATH, LIMIT), Result::InvalidResponse);
  maxBookmarkWriteBytes = -1;
  failBookmarkSync = true;
  EXPECT_EQ(client.get("/clippings", PATH, LIMIT), Result::InvalidResponse);
  failBookmarkSync = false;
  failBookmarkClose = true;
  EXPECT_EQ(client.get("/clippings", PATH, LIMIT), Result::InvalidResponse);
}

TEST_F(ClippingDownloadTransport, PreservesHttpFailureResults) {
  StatsUploadClient client;
  for (const auto [status, expected] : {std::pair{-1, Result::Network},
                                        {401, Result::Auth},
                                        {404, Result::Unsupported},
                                        {405, Result::Unsupported},
                                        {500, Result::Server}}) {
    testHttp.status = status;
    EXPECT_EQ(client.get("/clippings", PATH, LIMIT), expected);
  }
}

TEST_F(ClippingDownloadTransport, RetainsTlsMemoryFloors) {
  StatsUploadClient client;
  ESP.free = 34999;
  EXPECT_EQ(client.get("/clippings", PATH, LIMIT), Result::LowMemory);
  ESP.free = 45000;
  ESP.largest = 19999;
  EXPECT_EQ(client.get("/clippings", PATH, LIMIT), Result::LowMemory);
  EXPECT_EQ(testHttp.streamedRequests, 0U);
}

TEST_F(ClippingDownloadTransport, RejectsInvalidServerUrl) {
  testHttp.validUrl = false;
  StatsUploadClient client;
  EXPECT_EQ(client.get("/clippings", PATH, LIMIT), Result::Network);
  EXPECT_EQ(testHttp.streamedRequests, 0U);
}
}  // namespace

TEST_F(ClippingDownloadTransport, KeepsHttpCodesAndReportsInvalidSuccessAcknowledgments) {
  StatsUploadClient client;
  char detail[128];
  testHttp.status = 503;
  auto result = client.put("/api/v1/stats/counters/global", "{}", 0);
  EXPECT_EQ(result, Result::Server);
  EXPECT_EQ(StatsUploadClient::lastHttpCode, 503);
  EXPECT_STREQ(StatsUploadClient::formatResult(detail, sizeof(detail), result, 503), "Server error (HTTP 503)");
  testHttp.status = 200;
  testHttp.body = "{}";
  EXPECT_EQ(client.put("/api/v1/stats/counters/global", "{}", 0), Result::InvalidResponse);
  EXPECT_EQ(StatsUploadClient::lastHttpCode, 200);
  EXPECT_STREQ(StatsUploadClient::formatResult(detail, sizeof(detail), Result::InvalidResponse, 200),
               "Invalid sync data (HTTP 200)");
  client.close();
  ESP.free = 1;
  EXPECT_EQ(client.put("/api/v1/stats/counters/global", "{}", 0), Result::LowMemory);
  EXPECT_EQ(StatsUploadClient::lastHttpCode, 0);  // Never borrow an earlier response.
  const auto& log = Storage.files.at(SyncLog::PATH);
  EXPECT_NE(log.find("http=503 result=server-error"), std::string::npos);
  EXPECT_NE(log.find("http=200 result=invalid-response"), std::string::npos);
  EXPECT_NE(log.find("http=0 result=low-memory"), std::string::npos);
}

TEST_F(ClippingDownloadTransport, LogRedactsRoutesAndRotatesWithinStorageBudget) {
  ASSERT_TRUE(SyncLog::record("GET", "/api/v1/clippings/private-book?token=secret", 401, "authentication-failed"));
  ASSERT_TRUE(
      SyncLog::record("GET", "https://user:password@private-server/path?token=secret", -1, "connection-failed"));
  const auto log = Storage.files.at(SyncLog::PATH);
  EXPECT_NE(log.find("/api/v1/clippings/:document"), std::string::npos);
  for (const char* secret : {"private-book", "token", "secret", "password", "private-server"})
    EXPECT_EQ(log.find(secret), std::string::npos);
  Storage.files[SyncLog::PATH].assign(SyncLog::MAX_BYTES, 'x');
  ASSERT_TRUE(SyncLog::record("PUT", "/syncs/progress", 500, "server-error"));
  EXPECT_EQ(Storage.files.at(SyncLog::PREVIOUS_PATH).size(), SyncLog::MAX_BYTES);
  EXPECT_LT(Storage.files.at(SyncLog::PATH).size(), 224U);
  Storage.files[SyncLog::PATH].assign(SyncLog::MAX_BYTES, 'y');
  ASSERT_TRUE(SyncLog::record("GET", "/users/auth", 200, "ok"));
  EXPECT_EQ(Storage.files.at(SyncLog::PREVIOUS_PATH).front(), 'y');
}

TEST_F(ClippingDownloadTransport, UnwritableLogDoesNotChangeSuccessfulSync) {
  StatsUploadClient client;
  testHttp.body = R"({"until":1})";
  failBookmarkWriteOpen = true;
  EXPECT_EQ(client.put("/api/v1/stats/global", "{}", 0), Result::Ok);
  client.close();
  EXPECT_EQ(StatsUploadClient::lastHttpCode, 200);
}
