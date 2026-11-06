#include <HalStorage.h>
#include <SecureHttpClient.h>
#include <gtest/gtest.h>

#include "StatsUploadClient.h"

namespace {
using Result = StatsUploadClient::Result;
constexpr const char* PATH = "/download.tmp";
constexpr size_t LIMIT = 26624;
class ClippingDownloadTransport : public ::testing::Test {
 protected:
  void SetUp() override {
    testHttp = {};
    ESP = {};
    Storage.files.clear();
    failBookmarkWriteOpen = failBookmarkSync = failBookmarkClose = false;
    maxBookmarkWriteBytes = -1;
  }
  void TearDown() override {
    EXPECT_FALSE(testHttp.connected);
    EXPECT_FALSE(bookmarkMockWriting);
  }
};

TEST_F(ClippingDownloadTransport, StreamsUploadAndRequiresCompleteAcknowledgment) {
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
