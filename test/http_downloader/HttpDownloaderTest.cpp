#include <OpdsParser.h>
#include <gtest/gtest.h>

#include "DownloadFileSwap.h"
#include "HttpDownloadFilename.h"
#include "HttpDownloadResume.h"
#include "HttpDownloader.h"
#include "TestPlatform.h"
#include "util/OpdsFeedError.h"

class HttpDownloaderTest : public testing::TestWithParam<HttpDownloader::Transport> {
  void SetUp() override {
    files.clear();
    replies.clear();
    requests.clear();
    requestUrls.clear();
    recordReservations.clear();
    nextReply = 0;
    failWrite = failSync = false;
    cardCapacity = cardUsedBytes = 0;
    fileWriteOpens = capacityChecks = 0;
    failRemovePaths.clear();
    failRenameSources.clear();
  }

 protected:
  std::string resolved;
  HttpDownloader::DownloadError serverDownload(const std::string& approved = "",
                                               HttpDownloader::CancelCallback cancel = nullptr,
                                               bool (*validate)(const std::string&) = nullptr) {
    HttpDownloader::DownloadOptions options;
    options.transport = GetParam();
    options.stageAsPart = true;
    options.useServerFilename = true;
    options.overwriteApprovedPath = approved;
    options.resolvedPath = &resolved;
    options.shouldCancel = std::move(cancel);
    options.validate = validate;
    return HttpDownloader::downloadToFile("https://example.com/book", "/Books/Author - Title.epub", nullptr, nullptr,
                                          "user", "pass", options);
  }
  static Reply named(std::string name = "Server_Name.epub", std::string body = "abc", int status = 200,
                     size_t length = 3, bool complete = true, std::string range = "") {
    Reply reply{status, std::move(body), length, std::move(range), complete};
    reply.disposition = "attachment; filename=\"" + name + "\"";
    return reply;
  }
  HttpDownloader::DownloadError download(HttpDownloader::CancelCallback cancel = nullptr, bool strictCleanup = false) {
    HttpDownloader::DownloadOptions options;
    options.transport = GetParam();
    options.stageAsPart = true;
    options.shouldCancel = cancel;
    options.requireBackupCleanup = strictCleanup;
    return HttpDownloader::downloadToFile("https://example.com/book", "/book", nullptr, nullptr, "user", "pass",
                                          options);
  }
};
TEST_P(HttpDownloaderTest, ReconnectsAtLastWrittenByte) {
  replies = {{200, "abc", 6, "", false}, {206, "def", 3, "bytes 3-5/6", true}};
  ASSERT_EQ(download(), HttpDownloader::OK);
  EXPECT_EQ(files["/book"], "abcdef");
  EXPECT_EQ(requests[1]["Range"], "bytes=3-");
}
TEST_P(HttpDownloaderTest, IgnoredRangeRestartsWithoutDuplicateBytes) {
  replies = {{200, "abc", 6, "", false}, {200, "abcdef", 6}, {200, "abcdef", 6}};
  ASSERT_EQ(download(), HttpDownloader::OK);
  EXPECT_EQ(files["/book"], "abcdef");
  EXPECT_EQ(requests.size(), 3u);
  EXPECT_EQ(requests[2].count("Range"), 0u);
}
TEST_P(HttpDownloaderTest, RejectsWrongOffsetsAndChangedSizes) {
  for (const auto* range : {"bytes 2-4/6", "bytes 3-5/7", "bytes 3-9/6", "bytes 3-5/*"}) {
    replies = {{200, "abc", 6, "", false}, {206, "def", 3, range, true}};
    nextReply = 0;
    requests.clear();
    files.clear();
    files["/book"] = "old";
    EXPECT_EQ(download(), HttpDownloader::HTTP_ERROR);
    EXPECT_EQ(files["/book"], "old");
    EXPECT_FALSE(files.count("/book.part"));
  }
}
TEST_P(HttpDownloaderTest, PartialRangeRequiresAnotherRequest) {
  replies = {{200, "ab", 6, "", false}, {206, "cd", 2, "bytes 2-3/6", true}, {206, "ef", 2, "bytes 4-5/6", true}};
  EXPECT_EQ(download(), HttpDownloader::OK);
  EXPECT_EQ(files["/book"], "abcdef");
}
TEST_P(HttpDownloaderTest, EmptyIgnoredRangeStillRestarts) {
  replies = {{200, "ab", 6, "", false}, {200, "", 6}, {200, "abcdef", 6}};
  EXPECT_EQ(download(), HttpDownloader::OK);
  EXPECT_EQ(files["/book"], "abcdef");
}
TEST_P(HttpDownloaderTest, StopsAfterThreeStalls) {
  replies = {{200, "", 6, "", false}, {200, "", 6, "", false}, {200, "", 6, "", false}, {200, "abcdef", 6}};
  EXPECT_EQ(download(), HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(nextReply, 3u);
}
TEST_P(HttpDownloaderTest, CancellationAndSdFailureNeverRetry) {
  replies = {{200, "abc", 6, "", false}, {206, "def", 3, "bytes 3-5/6", true}};
  EXPECT_EQ(download([] { return true; }), HttpDownloader::ABORTED);
  EXPECT_EQ(nextReply, 0u);
  failWrite = true;
  EXPECT_EQ(download(), HttpDownloader::FILE_ERROR);
  EXPECT_EQ(nextReply, 1u);
}
TEST_P(HttpDownloaderTest, FlushFailureKeepsOriginal) {
  files["/book"] = "old";
  replies = {{200, "abcdef", 6}};
  failSync = true;
  EXPECT_EQ(download(), HttpDownloader::FILE_ERROR);
  EXPECT_EQ(files["/book"], "old");
}
TEST_P(HttpDownloaderTest, RejectsHttpsDowngradeAndOmitsCrossOriginCredentials) {
  replies = {{302, "", 0, "", true, "http://example.com/book"}};
  EXPECT_EQ(download(), HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(nextReply, 1u);
  replies = {{302, "", 0, "", true, "https://other.example/book"}, {200, "abc", 3}};
  nextReply = 0;
  requests.clear();
  EXPECT_EQ(download(), HttpDownloader::OK);
  EXPECT_TRUE(requests[0].count("Authorization"));
  EXPECT_FALSE(requests[1].count("Authorization"));
}
TEST_P(HttpDownloaderTest, DoesNotRetryHttpErrorsOrOversizedBody) {
  replies = {{404, "missing", 7}};
  EXPECT_EQ(download(), HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(nextReply, 1u);
  replies = {{200, "abcdef", 3}};
  nextReply = 0;
  EXPECT_EQ(download(), HttpDownloader::HTTP_ERROR);
}

TEST_P(HttpDownloaderTest, MissingValidatorRestartsFromZero) {
  replies = {{200, "abc", 6, "", false, "", ""}, {200, "UVWXYZ", 6, "", true, "", ""}};
  EXPECT_EQ(download(), HttpDownloader::OK);
  EXPECT_EQ(files["/book"], "UVWXYZ");
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_EQ(requests[1].count("Range"), 0u);
}

TEST_P(HttpDownloaderTest, SendsIfRangeAndRejectsChangedRepresentation) {
  replies = {{200, "abc", 6, "", false, "", "\"v1\""}, {206, "XYZ", 3, "bytes 3-5/6", true, "", "\"v2\""}};
  EXPECT_EQ(download(), HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(requests[1]["If-Range"], "\"v1\"");
  EXPECT_FALSE(files.count("/book"));
}
TEST_P(HttpDownloaderTest, CancelsBetweenRetryAttempts) {
  files["/book"] = "old";
  replies = {{200, "abc", 6, "", false}, {206, "def", 3, "bytes 3-5/6", true}};
  EXPECT_EQ(download([] { return files["/book.part"].size() == 3; }), HttpDownloader::ABORTED);
  EXPECT_EQ(nextReply, 1u);
  EXPECT_EQ(files["/book"], "old");
  EXPECT_FALSE(files.count("/book.part"));
}

TEST_P(HttpDownloaderTest, RetriesTransportFailureWithoutLosingPartialBytes) {
  replies = {{200, "abc", 6, "", false}, {-1, "", 0}, {206, "def", 3, "bytes 3-5/6", true}};
  EXPECT_EQ(download(), HttpDownloader::OK);
  EXPECT_EQ(files["/book"], "abcdef");
  EXPECT_EQ(requests[2]["Range"], "bytes=3-");
}

TEST_P(HttpDownloaderTest, ServerFilenameUsesFinalResponseAndKeepsGeneratedFile) {
  files["/Books/Author - Title.epub"] = "unrelated";
  Reply redirect = named("Wrong.epub", "", 302, 0);
  redirect.location = "https://other.example/download";
  replies = {redirect, named()};
  ASSERT_EQ(serverDownload(), HttpDownloader::OK);
  EXPECT_EQ(resolved, "/Books/Server_Name.epub");
  EXPECT_EQ(files[resolved], "abc");
  EXPECT_EQ(files["/Books/Author - Title.epub"], "unrelated");
  EXPECT_FALSE(files.count("/Books/Wrong.epub"));
  EXPECT_FALSE(requests[1].count("Authorization"));
}
TEST_P(HttpDownloaderTest, ServerFilenameToggleKeepsSelectedTemplateAsFallback) {
  const std::string fallback = "/Books/Title - Author.epub";
  for (const bool enabled : {false, true}) {
    for (const char* name : {"From_Server.epub", "../unsafe.epub", ""}) {
      files.clear();
      requests.clear();
      nextReply = 0;
      replies = {named(name)};
      HttpDownloader::DownloadOptions options;
      options.transport = GetParam();
      options.stageAsPart = true;
      options.useServerFilename = enabled;
      options.resolvedPath = &resolved;
      ASSERT_EQ(HttpDownloader::downloadToFile("https://example.com/book", fallback, nullptr, nullptr, "", "", options),
                HttpDownloader::OK);
      EXPECT_EQ(resolved, enabled && std::string(name) == "From_Server.epub" ? "/Books/From_Server.epub" : fallback);
      EXPECT_EQ(files[resolved], "abc");
    }
  }
}

TEST_P(HttpDownloaderTest, ServerNameFallbacksAndCollisionApproval) {
  for (const auto* header : {"", "attachment; filename=../escape.epub", "attachment; filename*=UTF-8''%00.epub"}) {
    files.clear();
    requests.clear();
    nextReply = 0;
    auto reply = named();
    reply.disposition = header;
    replies = {reply};
    EXPECT_EQ(serverDownload(), HttpDownloader::OK);
    EXPECT_EQ(resolved, "/Books/Author - Title.epub");
    EXPECT_EQ(files[resolved], "abc");
  }
  files.clear();
  nextReply = 0;
  replies = {named()};
  files["/Books/Server_Name.epub"] = "old";
  files["/Books/Server_Name.epub.part"] = "pending";
  EXPECT_EQ(serverDownload(), HttpDownloader::FILE_EXISTS);
  EXPECT_EQ(files[resolved], "old");
  EXPECT_EQ(files[resolved + ".part"], "pending");
  nextReply = 0;
  EXPECT_EQ(serverDownload("/Books/Other.epub"), HttpDownloader::FILE_EXISTS);
  nextReply = 0;
  EXPECT_EQ(serverDownload("/Books/Server_Name.epub"), HttpDownloader::OK);
  EXPECT_EQ(files[resolved], "abc");
  EXPECT_FALSE(files.count(resolved + ".part"));
  EXPECT_FALSE(files.count(resolved + ".old"));
}
TEST_P(HttpDownloaderTest, ServerNameResumesSameIdentityWhenHeaderIsOmitted) {
  auto first = named("Exact.epub", "abc", 200, 6, false);
  Reply second{206, "def", 3, "bytes 3-5/6"};
  replies = {first, second};
  EXPECT_EQ(serverDownload(), HttpDownloader::OK);
  EXPECT_EQ(resolved, "/Books/Exact.epub");
  EXPECT_EQ(files[resolved], "abcdef");
  EXPECT_EQ(requests[1]["Range"], "bytes=3-");
  EXPECT_EQ(requests[1]["If-Range"], "\"v1\"");
}
TEST_P(HttpDownloaderTest, ServerNameChangeDuringResumeNeverWritesAnotherBook) {
  replies = {named("Exact.epub", "abc", 200, 6, false), named("Other.epub", "def", 206, 3, true, "bytes 3-5/6")};
  EXPECT_EQ(serverDownload(), HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(resolved, "/Books/Exact.epub");
  EXPECT_TRUE(files.empty());
}
TEST_P(HttpDownloaderTest, ApprovedReplacementPreservesOriginalOnSdOrValidationFailure) {
  files["/Books/Server_Name.epub"] = "old";
  replies = {named()};
  failWrite = true;
  EXPECT_EQ(serverDownload("/Books/Server_Name.epub"), HttpDownloader::FILE_ERROR);
  EXPECT_EQ(files["/Books/Server_Name.epub"], "old");
  failWrite = false;
  nextReply = 0;
  EXPECT_EQ(serverDownload("/Books/Server_Name.epub", nullptr, [](const std::string&) { return false; }),
            HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(files["/Books/Server_Name.epub"], "old");
  EXPECT_FALSE(files.count("/Books/Server_Name.epub.part"));
}
TEST_P(HttpDownloaderTest, ServerNameCancellationDeletesOnlyItsPartial) {
  files["/Books/Server_Name.epub"] = "old";
  replies = {named("Server_Name.epub", "abc", 200, 6, false)};
  EXPECT_EQ(serverDownload("/Books/Server_Name.epub",
                           [] {
                             return files.count("/Books/Server_Name.epub.part") &&
                                    files["/Books/Server_Name.epub.part"].size() == 3;
                           }),
            HttpDownloader::ABORTED);
  EXPECT_EQ(files["/Books/Server_Name.epub"], "old");
  EXPECT_FALSE(files.count("/Books/Server_Name.epub.part"));
}
TEST_P(HttpDownloaderTest, Utf8ExtendedNameAndExtensionCaseArePreserved) {
  auto reply = named();
  reply.disposition = "attachment; filename=Fallback.epub; filename*=UTF-8'en'%E6%97%A5%E6%9C%AC%20%F0%9F%93%9A.EPUB";
  replies = {reply};
  EXPECT_EQ(serverDownload(), HttpDownloader::OK);
  EXPECT_EQ(resolved, "/Books/日本 📚.EPUB");
  EXPECT_EQ(files[resolved], "abc");
}
TEST_P(HttpDownloaderTest, ServerErrorsAndEmptyBodiesDoNotReplaceOrRemoveExistingBooks) {
  files["/Books/Author - Title.epub"] = "fallback";
  files["/Books/Server_Name.epub"] = "old";
  replies = {named("Server_Name.epub", "error", 401, 5)};
  EXPECT_EQ(serverDownload(), HttpDownloader::UNAUTHORIZED);
  nextReply = 0;
  replies = {named("Server_Name.epub", "", 200, 0)};
  EXPECT_EQ(serverDownload(), HttpDownloader::FILE_EXISTS);
  nextReply = 0;
  EXPECT_EQ(serverDownload("/Books/Server_Name.epub"), HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(files["/Books/Server_Name.epub"], "old");
  EXPECT_EQ(files["/Books/Author - Title.epub"], "fallback");
}
TEST(HttpDownloadFilenameTest, QuotedSemicolonsAndUtf8AreExact) {
  std::string name;
  ASSERT_TRUE(HttpDownloadFilename::parse("attachment; FILENAME = \"The Guest List_ A Novel; Lucy.epub\"", name));
  EXPECT_EQ(name, "The Guest List_ A Novel; Lucy.epub");
  ASSERT_TRUE(HttpDownloadFilename::parse("inline; filename=Literal%20Space.epub", name));
  EXPECT_EQ(name, "Literal%20Space.epub");
  ASSERT_TRUE(HttpDownloadFilename::parse("attachment; filename*=utf-8''Cafe%CC%81.epub", name));
  EXPECT_EQ(name, "Café.epub");
  ASSERT_TRUE(HttpDownloadFilename::parse("attachment; filename=Safe.epub; filename*=UNKNOWN''bad", name));
  EXPECT_EQ(name, "Safe.epub");
}
TEST(HttpDownloadFilenameTest, RejectsUnsafeAmbiguousAndMalformedNames) {
  std::string name;
  for (const auto* header :
       {"attachment; filename=../escape.epub", "attachment; filename=folder\\escape.epub",
        "attachment; filename=.hidden.epub", "attachment; filename=Book:Title.epub", "attachment; filename=Book.epub.",
        "attachment; filename=Book.exe", "attachment; filename=\"unfinished.epub",
        "attachment; filename=a.epub; FILENAME=b.epub", "attachment; filename*=UTF-8''bad%GG.epub",
        "attachment; filename*=UTF-8''bad%00.epub", "attachment; filename*=UTF-8''%C0%AF.epub",
        "attachment; filename*=UTF-8''%ED%A0%80.epub", "attachment; filename*=UTF-8''%F4%90%80%80.epub",
        "attachment; filename*=UTF-8''%2Fescape.epub", "attachment; filename=bad\r\n.epub"}) {
    EXPECT_FALSE(HttpDownloadFilename::parse(header, name)) << header;
    EXPECT_TRUE(name.empty());
  }
}
TEST(HttpDownloadFilenameTest, RejectsDesktopReservedNames) {
  std::string name;
  for (const auto* value : {"CON.epub", "prn.epub", "NUL .epub", "COM1.epub", "Lpt9.epub"})
    EXPECT_FALSE(HttpDownloadFilename::parse(std::string("attachment; filename=") + value, name));
  EXPECT_TRUE(HttpDownloadFilename::parse("attachment; filename=COM10.epub", name));
}

TEST(HttpDownloadFilenameTest, LengthLimitsDoNotSilentlyTruncateIdentity) {
  std::string name;
  std::string maximum(235, 'a');
  maximum += ".epub";
  EXPECT_TRUE(HttpDownloadFilename::parse("attachment; filename=" + maximum, name));
  EXPECT_EQ(name, maximum);
  EXPECT_FALSE(HttpDownloadFilename::parse("attachment; filename=a" + maximum, name));
  EXPECT_FALSE(HttpDownloadFilename::parse(std::string(1025, 'a'), name));
}

INSTANTIATE_TEST_SUITE_P(Transports, HttpDownloaderTest,
                         testing::Values(HttpDownloader::Transport::ESP_HTTP, HttpDownloader::Transport::WOLFSSL));
TEST(HttpDownloadResumeTest, OverflowAndRetryBackstop) {
  size_t total = 0, end = 0;
  EXPECT_FALSE(HttpDownloadResume::range("bytes 0-999999999999999999999999/9999999999999999999999999", 0, 0, false, 0,
                                         total, end));
  EXPECT_FALSE(HttpDownloadResume::accepts(SIZE_MAX, 0, 1));
  HttpDownloadResume::RetryBudget budget;
  for (size_t i = 0; i < 19; ++i) EXPECT_TRUE(budget.again(i, i + 1));
  EXPECT_FALSE(budget.again(19, 20));
}

TEST_P(HttpDownloaderTest, DuplicateDispositionHeadersUseGeneratedFallback) {
  auto reply = named("First.epub");
  reply.extraDispositions = {"attachment; filename=Second.epub"};
  replies = {reply};
  ASSERT_EQ(serverDownload(), HttpDownloader::OK);
  EXPECT_EQ(resolved, "/Books/Author - Title.epub");
  EXPECT_EQ(files[resolved], "abc");
  EXPECT_FALSE(files.count("/Books/First.epub"));
  EXPECT_FALSE(files.count("/Books/Second.epub"));
}

TEST_P(HttpDownloaderTest, DeclaredLimitRejectsBeforeOpeningFileOrCallingSink) {
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  options.stageAsPart = true;
  options.maxBytes = 3;
  files["/book"] = "original";
  replies = {{200, "abcd", 4}};
  EXPECT_EQ(HttpDownloader::downloadToFile("https://example.com/book", "/book", nullptr, nullptr, "", "", options),
            HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(files["/book"], "original");
  EXPECT_FALSE(files.count("/book.part"));
  nextReply = 0;
  size_t received = 0;
  EXPECT_EQ(HttpDownloader::streamUrl(
                "https://example.com/book",
                [&](const uint8_t*, size_t n) {
                  received += n;
                  return true;
                },
                nullptr, "", "", options),
            HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(received, 0u);
}

TEST_P(HttpDownloaderTest, UnknownLengthLimitChecksEveryChunkBeforeDelivery) {
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  options.maxBytes = 3;
  options.stageAsPart = true;
  options.preservePartial = true;
  Reply reply{200, "abcd", 0};  // Chunked or close-delimited body.
  reply.chunkSize = 2;
  replies = {reply};
  size_t received = 0;
  EXPECT_EQ(HttpDownloader::streamUrl(
                "https://example.com/book",
                [&](const uint8_t*, size_t n) {
                  received += n;
                  return true;
                },
                nullptr, "", "", options),
            HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(received, 2u);  // The chunk that would cross the cap never reaches the callback.
  EXPECT_EQ(nextReply, 1u);
  nextReply = 0;
  files["/book"] = "original";
  EXPECT_EQ(HttpDownloader::downloadToFile("https://example.com/book", "/book", nullptr, nullptr, "", "", options),
            HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(files["/book.part"], "ab");
  EXPECT_EQ(files["/book"], "original");
  EXPECT_EQ(nextReply, 1u);
}

TEST_P(HttpDownloaderTest, ExpectedSizeAcceptsExactAndRejectsShortOrLongBodies) {
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  options.expectedBytes = 3;
  for (size_t declared : {0u, 2u, 3u, 4u}) {
    for (const std::string body : {"ab", "abc", "abcd"}) {
      nextReply = 0;
      replies = {{200, body, declared}};
      size_t received = 0;
      const auto result = HttpDownloader::streamUrl(
          "https://example.com/book",
          [&](const uint8_t*, size_t n) {
            received += n;
            return true;
          },
          nullptr, "", "", options);
      EXPECT_EQ(result,
                body.size() == 3 && (declared == 0 || declared == 3) ? HttpDownloader::OK : HttpDownloader::HTTP_ERROR);
      EXPECT_LE(received, 3u);
      if (declared != 0 && declared != 3) EXPECT_EQ(received, 0u);
    }
  }
}

TEST_P(HttpDownloaderTest, LimitsIncludeResumedBytesAndTotalFromContentRange) {
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  options.maxBytes = 6;
  options.expectedBytes = 6;
  options.stageAsPart = true;
  replies = {{200, "abc", 6, "", false}, {206, "def", 3, "bytes 3-5/6", true}};
  EXPECT_EQ(HttpDownloader::downloadToFile("https://example.com/book", "/book", nullptr, nullptr, "", "", options),
            HttpDownloader::OK);
  EXPECT_EQ(files["/book"], "abcdef");
  files["/book.part"] = "abc";
  options.resumePartial = true;
  options.maxBytes = 5;
  nextReply = 0;
  replies = {{206, "def", 3, "bytes 3-5/6", true}};
  EXPECT_EQ(HttpDownloader::downloadToFile("https://example.com/book", "/book", nullptr, nullptr, "", "", options),
            HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(files["/book"], "abcdef");
  EXPECT_FALSE(files.count("/book.part"));
}

TEST_P(HttpDownloaderTest, UrlPolicyAppliesBeforeInitialAndEachRedirectConnection) {
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  std::vector<std::string> checked;
  options.urlContext = &checked;
  options.allowUrl = [](std::string_view url, void* context) {
    static_cast<std::vector<std::string>*>(context)->emplace_back(url);
    return url == "https://example.com/book" || url == "https://example.com/next";
  };
  const auto sink = [](const uint8_t*, size_t) { return true; };
  replies = {{200, "abc", 3}};
  EXPECT_EQ(HttpDownloader::streamUrl("https://unapproved.example/book", sink, nullptr, "", "", options),
            HttpDownloader::HTTP_ERROR);
  EXPECT_TRUE(requestUrls.empty());
  ASSERT_EQ(checked.size(), 1u);
  checked.clear();
  replies = {
      {302, "", 0, "", true, "/next"}, {302, "", 0, "", true, "https://unapproved.example/book"}, {200, "abc", 3}};
  EXPECT_EQ(HttpDownloader::streamUrl("https://example.com/book", sink, nullptr, "", "", options),
            HttpDownloader::HTTP_ERROR);
  ASSERT_EQ(requestUrls.size(), 2u);
  EXPECT_EQ(requestUrls[1], "https://example.com/next");
  ASSERT_EQ(checked.size(), 3u);
  EXPECT_EQ(checked[2], "https://unapproved.example/book");
}

TEST_P(HttpDownloaderTest, RedirectBudgetCountsRedirectsAndAllowsZero) {
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  const auto sink = [](const uint8_t*, size_t) { return true; };
  options.maxRedirects = 0;
  replies = {{200, "abc", 3}};
  EXPECT_EQ(HttpDownloader::streamUrl("https://example.com/book", sink, nullptr, "", "", options), HttpDownloader::OK);
  nextReply = 0;
  replies = {{302, "", 0, "", true, "/next"}, {200, "abc", 3}};
  EXPECT_EQ(HttpDownloader::streamUrl("https://example.com/book", sink, nullptr, "", "", options),
            HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(nextReply, 1u);
  options.maxRedirects = 1;
  nextReply = 0;
  EXPECT_EQ(HttpDownloader::streamUrl("https://example.com/book", sink, nullptr, "", "", options), HttpDownloader::OK);
  EXPECT_EQ(nextReply, 2u);
}

TEST_P(HttpDownloaderTest, ContextValidatorChecksStagedFileBeforePublication) {
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  options.stageAsPart = true;
  std::string expected = "wrong";
  options.validationContext = &expected;
  options.validateWithContext = [](const std::string& path, void* context) {
    EXPECT_EQ(path, "/book.part");
    EXPECT_EQ(files["/book"], "original");
    return files[path] == *static_cast<std::string*>(context);
  };
  files["/book"] = "original";
  replies = {{200, "abc", 3}};
  EXPECT_EQ(HttpDownloader::downloadToFile("https://example.com/book", "/book", nullptr, nullptr, "", "", options),
            HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(files["/book"], "original");
  EXPECT_FALSE(files.count("/book.part"));
  expected = "abc";
  nextReply = 0;
  EXPECT_EQ(HttpDownloader::downloadToFile("https://example.com/book", "/book", nullptr, nullptr, "", "", options),
            HttpDownloader::OK);
  EXPECT_EQ(files["/book"], "abc");
}

TEST_P(HttpDownloaderTest, PublicationCleanupFailureReportsFailureAndRecoversWithoutLosingSource) {
  files["/book"] = "original";
  replies = {{200, "abc", 3}};
  failRemovePaths.insert("/book.old");
  EXPECT_EQ(download(nullptr, true), HttpDownloader::FILE_ERROR);
  EXPECT_EQ(files["/book"], "abc");
  EXPECT_EQ(files["/book.old"], "original");
  EXPECT_FALSE(DownloadFileSwap::recover("/book"));
  EXPECT_EQ(files["/book"], "abc");
  failRemovePaths.clear();
  EXPECT_TRUE(DownloadFileSwap::recover("/book"));
  EXPECT_EQ(files["/book"], "abc");
  EXPECT_FALSE(files.count("/book.old"));
}

TEST_P(HttpDownloaderTest, FailedPublishAndRollbackLeaveRecoverableOriginal) {
  files["/book"] = "original";
  replies = {{200, "abc", 3}};
  failRenameSources.insert("/book.part");
  failRenameSources.insert("/book.old");
  EXPECT_EQ(download(), HttpDownloader::FILE_ERROR);
  EXPECT_FALSE(files.count("/book"));
  EXPECT_EQ(files["/book.old"], "original");
  failRenameSources.clear();
  EXPECT_TRUE(DownloadFileSwap::recover("/book"));
  EXPECT_EQ(files["/book"], "original");
  EXPECT_FALSE(files.count("/book.old"));
}

TEST_P(HttpDownloaderTest, ExactSizeMismatchNeverValidatesOrPublishesStagedFile) {
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  options.stageAsPart = true;
  options.expectedBytes = 3;
  bool validated = false;
  options.validationContext = &validated;
  options.validateWithContext = [](const std::string&, void* context) {
    *static_cast<bool*>(context) = true;
    return true;
  };
  files["/book"] = "original";
  replies = {{200, "ab", 0}};
  EXPECT_EQ(HttpDownloader::downloadToFile("https://example.com/book", "/book", nullptr, nullptr, "", "", options),
            HttpDownloader::HTTP_ERROR);
  EXPECT_FALSE(validated);
  EXPECT_EQ(files["/book"], "original");
  EXPECT_FALSE(files.count("/book.part"));
  EXPECT_EQ(nextReply, 1u);
}

TEST_P(HttpDownloaderTest, UnknownLengthBodyAtMaximumIsAccepted) {
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  options.maxBytes = 3;
  Reply reply{200, "abc", 0};
  reply.chunkSize = 1;
  replies = {reply};
  size_t received = 0;
  EXPECT_EQ(HttpDownloader::streamUrl(
                "https://example.com/book",
                [&](const uint8_t*, size_t count) {
                  received += count;
                  return true;
                },
                nullptr, "", "", options),
            HttpDownloader::OK);
  EXPECT_EQ(received, 3u);
}

TEST_P(HttpDownloaderTest, LegacyPublicationCleanupFailureStillReportsSuccess) {
  files["/book"] = "original";
  replies = {{200, "abc", 3}};
  failRemovePaths.insert("/book.old");
  EXPECT_EQ(download(), HttpDownloader::OK);
  EXPECT_EQ(files["/book"], "abc");
  EXPECT_EQ(files["/book.old"], "original");
  failRemovePaths.clear();
  EXPECT_TRUE(DownloadFileSwap::recover("/book"));
  EXPECT_EQ(files["/book"], "abc");
  EXPECT_FALSE(files.count("/book.old"));
}

TEST_P(HttpDownloaderTest, FontStyleUnstagedDownloadKeepsResumablePartialOnTransportFailure) {
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  options.preservePartial = options.resumePartial = true;
  replies = {{200, "abc", 9, "", false}, {206, "def", 3, "bytes 3-5/9", false}, {500, "", 0}};
  EXPECT_EQ(HttpDownloader::downloadToFile("https://example.com/font", "/font.tmp", nullptr, nullptr, "", "", options),
            HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(files["/font.tmp"], "abcdef");
  EXPECT_EQ(requests[0].count("Range"), 0u);
  EXPECT_EQ(requests[1]["Range"], "bytes=3-");
  EXPECT_EQ(requests[2]["Range"], "bytes=6-");
  EXPECT_FALSE(files.count("/font.tmp.old"));
  EXPECT_EQ(capacityChecks, 0u);  // Existing fonts do not opt into capacity preflight.
}

#ifndef SIMULATOR
TEST_P(HttpDownloaderTest, ExpectedSizePreflightsUnknownLengthBeforeOpeningOrWriting) {
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  options.stageAsPart = options.checkFreeSpace = true;
  options.expectedBytes = 6;
  files["/book"] = "original";
  cardCapacity = 10;
  cardUsedBytes = 8;  // Includes the existing source; never reclaim it up front.
  replies = {{200, "abcdef", 0}};
  EXPECT_EQ(HttpDownloader::downloadToFile("https://example.com/book", "/book", nullptr, nullptr, "", "", options),
            HttpDownloader::INSUFFICIENT_SPACE);
  EXPECT_EQ(fileWriteOpens, 0u);
  EXPECT_EQ(capacityChecks, 1u);
  EXPECT_EQ(files["/book"], "original");
  EXPECT_FALSE(files.count("/book.part"));
  EXPECT_EQ(nextReply, 1u);
}

TEST_P(HttpDownloaderTest, ExactFreeSpaceAllowsUnknownLengthReplacementWithOldSourceRetained) {
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  options.stageAsPart = options.checkFreeSpace = true;
  options.expectedBytes = 6;
  files["/book"] = "original";
  cardCapacity = 14;
  cardUsedBytes = 8;
  Reply reply{200, "abcdef", 0};
  reply.chunkSize = 2;
  replies = {reply};
  options.validate = [](const std::string&) { return files["/book"] == "original"; };
  EXPECT_EQ(HttpDownloader::downloadToFile("https://example.com/book", "/book", nullptr, nullptr, "", "", options),
            HttpDownloader::OK);
  EXPECT_EQ(files["/book"], "abcdef");
  EXPECT_FALSE(files.count("/book.old"));
  EXPECT_EQ(capacityChecks, 1u);
}

TEST_P(HttpDownloaderTest, ResumedPreflightRequiresOnlyRemainingBytes) {
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  options.stageAsPart = options.checkFreeSpace = options.resumePartial = true;
  options.expectedBytes = 6;
  files["/book"] = "original";
  files["/book.part"] = "abc";
  cardCapacity = 14;
  cardUsedBytes = 11;  // Existing source and partial are both already on SD.
  replies = {{206, "def", 3, "bytes 3-5/6"}};
  EXPECT_EQ(HttpDownloader::downloadToFile("https://example.com/book", "/book", nullptr, nullptr, "", "", options),
            HttpDownloader::OK);
  EXPECT_EQ(files["/book"], "abcdef");
  EXPECT_EQ(capacityChecks, 1u);
}

TEST_P(HttpDownloaderTest, DeclaredLengthStillPreflightsLegacyBookDownloads) {
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  options.stageAsPart = options.checkFreeSpace = true;
  files["/book"] = "original";
  cardCapacity = cardUsedBytes = 8;
  replies = {{200, "abc", 3}};
  EXPECT_EQ(HttpDownloader::downloadToFile("https://example.com/book", "/book", nullptr, nullptr, "", "", options),
            HttpDownloader::INSUFFICIENT_SPACE);
  EXPECT_EQ(fileWriteOpens, 0u);
  EXPECT_EQ(files["/book"], "original");
}

TEST_P(HttpDownloaderTest, UnknownCapacityFallsBackToSdWriteError) {
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  options.stageAsPart = options.checkFreeSpace = true;
  options.expectedBytes = 3;
  files["/book"] = "original";
  cardCapacity = 0;
  failWrite = true;
  replies = {{200, "abc", 0}};
  EXPECT_EQ(HttpDownloader::downloadToFile("https://example.com/book", "/book", nullptr, nullptr, "", "", options),
            HttpDownloader::FILE_ERROR);
  EXPECT_EQ(fileWriteOpens, 1u);
  EXPECT_EQ(files["/book"], "original");
  EXPECT_FALSE(files.count("/book.part"));
}

TEST_P(HttpDownloaderTest, UsedCapacityAboveTotalSaturatesWithoutUnsignedWrap) {
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  options.stageAsPart = options.checkFreeSpace = true;
  options.expectedBytes = 3;
  cardCapacity = 4;
  cardUsedBytes = 5;
  replies = {{200, "abc", 0}};
  EXPECT_EQ(HttpDownloader::downloadToFile("https://example.com/book", "/book", nullptr, nullptr, "", "", options),
            HttpDownloader::INSUFFICIENT_SPACE);
  EXPECT_EQ(fileWriteOpens, 0u);
}
#endif
TEST_P(HttpDownloaderTest, FeedDiagnosticsFollowFinalResponseAndAuthorization) {
  for (const auto* destination : {"https://example.com/feed", "https://other.example/feed"}) {
    for (const bool credentials : {false, true}) {
      replies = {{302, "", 0, "", true, destination}, {401, "", 0}};
      nextReply = 0;
      requests.clear();
      HttpRequestDiagnostics diagnostic;
      HttpDownloader::DownloadOptions options;
      options.transport = GetParam();
      auto result = HttpDownloader::streamUrl(
          "https://example.com/start", [](const uint8_t*, size_t) { return true; }, nullptr, credentials ? "user" : "",
          credentials ? "pass" : "", options, &diagnostic);
      const bool withheld = credentials && std::string(destination).find("other") != std::string::npos;
      EXPECT_EQ(result, HttpDownloader::UNAUTHORIZED);
      EXPECT_EQ(diagnostic.status, 401);
      EXPECT_EQ(diagnostic.authorizationWithheld, withheld);
      ASSERT_EQ(requests.size(), 2u);
      EXPECT_EQ(requests[1].count("Authorization") > 0, credentials && !withheld);
      EXPECT_EQ(classifyFeedFailure(diagnostic,
                                    result == HttpDownloader::HTTP_ERROR || result == HttpDownloader::UNAUTHORIZED),
                withheld ? OpdsFeedError::Redirected : OpdsFeedError::Unauthorized);
    }
  }
}

TEST_P(HttpDownloaderTest, FeedDiagnosticsResetAcrossRequestsAndKeep200FailuresGeneric) {
  HttpRequestDiagnostics diagnostic;
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  for (int status : {401, 500, 503, 530, 599, 403, 404, 600, -1, 200}) {
    replies = {{status, "", 4, "", false}};
    nextReply = 0;
    auto result = HttpDownloader::streamUrl(
        "https://example.com/feed", [](const uint8_t*, size_t) { return true; }, nullptr, "", "", options, &diagnostic);
    EXPECT_EQ(result, status == 401 || status == 403 ? HttpDownloader::UNAUTHORIZED : HttpDownloader::HTTP_ERROR);
    EXPECT_EQ(
        classifyFeedFailure(diagnostic, result == HttpDownloader::HTTP_ERROR || result == HttpDownloader::UNAUTHORIZED),
        status == 401                   ? OpdsFeedError::Unauthorized
        : status >= 500 && status < 600 ? OpdsFeedError::ServerError
        : status < 0                    ? OpdsFeedError::Unreachable
                                        : OpdsFeedError::Other);
    EXPECT_FALSE(diagnostic.authorizationWithheld);
  }
  replies = {{200, "abc", 3}};
  nextReply = 0;
  EXPECT_EQ(HttpDownloader::streamUrl(
                "https://example.com/feed", [](const uint8_t*, size_t) { return false; }, nullptr, "", "", options,
                &diagnostic),
            HttpDownloader::FILE_ERROR);
  EXPECT_TRUE(diagnostic.callbackRejected);
  EXPECT_EQ(classifyFeedFailure(diagnostic, true), OpdsFeedError::Other);

  options.shouldCancel = [] { return true; };
  EXPECT_EQ(HttpDownloader::streamUrl(
                "https://example.com/feed", [](const uint8_t*, size_t) { return true; }, nullptr, "", "", options,
                &diagnostic),
            HttpDownloader::ABORTED);
  EXPECT_FALSE(diagnostic.attempted);
  EXPECT_FALSE(diagnostic.callbackRejected);
  EXPECT_EQ(diagnostic.status, 0);
  EXPECT_EQ(classifyFeedFailure(diagnostic, false), OpdsFeedError::Other);
}

TEST_P(HttpDownloaderTest, FeedDiagnosticsDiscardRedirectStatusWhenNextConnectionFails) {
  replies = {{302, "", 0, "", true, "https://other.example/feed"}, {-1, "", 0}};
  HttpRequestDiagnostics diagnostic;
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  EXPECT_EQ(HttpDownloader::streamUrl(
                "https://example.com/feed", [](const uint8_t*, size_t) { return true; }, nullptr, "user", "pass",
                options, &diagnostic),
            HttpDownloader::HTTP_ERROR);
  EXPECT_LE(diagnostic.status, 0);
  EXPECT_TRUE(diagnostic.authorizationWithheld);
  EXPECT_EQ(classifyFeedFailure(diagnostic, true), OpdsFeedError::Unreachable);
}

TEST_P(HttpDownloaderTest, FeedDiagnosticsRespectSavedOriginAndHttpUpgrade) {
  replies = {{302, "", 0, "", true, "https://example.com/feed"}, {401, "", 0}};
  HttpRequestDiagnostics diagnostic;
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  EXPECT_EQ(HttpDownloader::streamUrl(
                "http://example.com/feed", [](const uint8_t*, size_t) { return true; }, nullptr, "user", "pass",
                options, &diagnostic),
            HttpDownloader::UNAUTHORIZED);
  EXPECT_EQ(classifyFeedFailure(diagnostic, true), OpdsFeedError::Redirected);
  EXPECT_FALSE(requests.back().count("Authorization"));
  replies = {{401, "", 0}};
  nextReply = 0;
  options.authorizationOrigin = "https://saved.example";
  EXPECT_EQ(HttpDownloader::streamUrl(
                "https://other.example/feed", [](const uint8_t*, size_t) { return true; }, nullptr, "user", "pass",
                options, &diagnostic),
            HttpDownloader::UNAUTHORIZED);
  EXPECT_EQ(classifyFeedFailure(diagnostic, true), OpdsFeedError::Redirected);
}

TEST_P(HttpDownloaderTest, FeedDiagnosticsMissingCallbackAndSuccessAreGeneric) {
  HttpRequestDiagnostics diagnostic{401, true, true, true};
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  EXPECT_EQ(HttpDownloader::streamUrl("https://example.com/feed", nullptr, nullptr, "", "", options, &diagnostic),
            HttpDownloader::HTTP_ERROR);
  EXPECT_FALSE(diagnostic.attempted);
  EXPECT_EQ(classifyFeedFailure(diagnostic, true), OpdsFeedError::Other);
  replies = {{200, "abc", 3}};
  EXPECT_EQ(HttpDownloader::streamUrl(
                "https://example.com/feed", [](const uint8_t*, size_t) { return true; }, nullptr, "", "", options,
                &diagnostic),
            HttpDownloader::OK);
  EXPECT_EQ(diagnostic.status, 200);
  EXPECT_FALSE(diagnostic.authorizationWithheld);
  EXPECT_EQ(classifyFeedFailure(diagnostic, false), OpdsFeedError::Other);
}

TEST_P(HttpDownloaderTest, FeedInFlightCancellationIsNotConnectivityFailure) {
  replies = {{200, "abc", 3}};
  HttpRequestDiagnostics diagnostic;
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  int checks = 0;
  options.shouldCancel = [&checks] { return ++checks > 1; };
  auto result = HttpDownloader::streamUrl(
      "https://example.com/feed", [](const uint8_t*, size_t) { return true; }, nullptr, "", "", options, &diagnostic);
  EXPECT_EQ(result, HttpDownloader::ABORTED);
  EXPECT_TRUE(diagnostic.attempted);
  EXPECT_EQ(
      classifyFeedFailure(diagnostic, result == HttpDownloader::HTTP_ERROR || result == HttpDownloader::UNAUTHORIZED),
      OpdsFeedError::Other);
}

TEST_P(HttpDownloaderTest, FeedNegativeGetRetainsHeadersAndDoesNotReportConnectivity) {
  if (GetParam() != HttpDownloader::Transport::WOLFSSL) GTEST_SKIP() << "wolfSSL GET contract";
  Reply reply{200, "abc", 6, "", false};
  reply.transportFailureAfterHeaders = true;
  replies = {reply};
  HttpRequestDiagnostics diagnostic;
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  auto result = HttpDownloader::streamUrl(
      "https://example.com/feed", [](const uint8_t*, size_t) { return true; }, nullptr, "", "", options, &diagnostic);
  EXPECT_EQ(result, HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(diagnostic.status, 200);
  EXPECT_EQ(classifyFeedFailure(diagnostic, true), OpdsFeedError::Other);
  nextReply = 0;
  result = HttpDownloader::streamUrl(
      "https://example.com/feed", [](const uint8_t*, size_t) { return false; }, nullptr, "", "", options, &diagnostic);
  EXPECT_TRUE(diagnostic.callbackRejected);
  EXPECT_EQ(
      classifyFeedFailure(diagnostic, result == HttpDownloader::HTTP_ERROR || result == HttpDownloader::UNAUTHORIZED),
      OpdsFeedError::Other);
}

TEST_P(HttpDownloaderTest, MalformedAndUnfinishedFeedsAreNotConnectivityFailures) {
  for (const std::string body : {"<feed></wrong>", "<feed>"}) {
    OpdsEntry entry[1];
    OpdsParser parser(entry);
    replies = {{200, body, body.size()}};
    nextReply = 0;
    HttpRequestDiagnostics diagnostic;
    HttpDownloader::DownloadOptions options;
    options.transport = GetParam();
    auto result = HttpDownloader::streamUrl(
        "https://example.com/feed",
        [&parser](const uint8_t* data, size_t size) { return parser.write(data, size) == size; }, nullptr, "", "",
        options, &diagnostic);
    parser.flush();  // Same finalization performed by OpdsParserStream.
    EXPECT_FALSE(parser);
    EXPECT_EQ(parser.getErrorReason(), OpdsParserError::XML_PARSE);
    EXPECT_EQ(diagnostic.status, 200);
    EXPECT_EQ(
        classifyFeedFailure(diagnostic, result == HttpDownloader::HTTP_ERROR || result == HttpDownloader::UNAUTHORIZED),
        OpdsFeedError::Other);
  }
}
TEST_P(HttpDownloaderTest, PluginHeadersStayOnTheOriginalOrigin) {
  const std::vector<HttpDownloader::Header> headers{{"Authorization", "Bearer test-token"}, {"X-Api-Key", "secret"}};
  HttpDownloader::DownloadOptions options;
  options.transport = GetParam();
  options.stageAsPart = true;
  options.headers = &headers;
  replies = {{302, "", 0, "", true, "https://example.com/next"},
             {302, "", 0, "", true, "https://other.example/file"},
             {200, "abc", 3}};
  EXPECT_EQ(HttpDownloader::downloadToFile("https://example.com/book", "/book", nullptr, nullptr, "", "", options),
            HttpDownloader::OK);
  ASSERT_EQ(requests.size(), 3u);
  EXPECT_EQ(requests[0]["Authorization"], "Bearer test-token");
  EXPECT_EQ(requests[1]["X-Api-Key"], "secret");
  EXPECT_EQ(requests[2].count("Authorization"), 0u);
  EXPECT_EQ(requests[2].count("X-Api-Key"), 0u);
  EXPECT_EQ(files["/book"], "abc");
}

TEST_P(HttpDownloaderTest, BulkReservationFollowsRedirectAndResumeAttempts) {
  replies = {{302, "", 0, "", true, "https://example.com/book"},
             {200, "abc", 6, "", false},
             {206, "def", 3, "bytes 3-5/6", true}};
  replies[1].etag = replies[2].etag = "\"book-v1\"";
  HttpDownloader::DownloadOptions o;
  o.transport = GetParam();
  o.reserveFullRecord = true;
  o.stageAsPart = true;
  EXPECT_EQ(HttpDownloader::downloadToFile("https://example.com/start", "/book.epub", nullptr, nullptr, "", "", o),
            HttpDownloader::OK);
  EXPECT_EQ(files["/book.epub"], "abcdef");
  if (GetParam() == HttpDownloader::Transport::WOLFSSL)
    EXPECT_EQ(recordReservations, (std::vector<bool>{true, true, true}));
  else
    EXPECT_TRUE(recordReservations.empty());
}

TEST_P(HttpDownloaderTest, SmallRequestsDoNotReserveTlsRecordsByDefault) {
  replies = {{200, "abc", 3}};
  HttpDownloader::DownloadOptions o;
  o.transport = GetParam();
  EXPECT_EQ(HttpDownloader::downloadToFile("https://example.com/book", "/book.epub", nullptr, nullptr, "", "", o),
            HttpDownloader::OK);
  if (GetParam() == HttpDownloader::Transport::WOLFSSL) EXPECT_EQ(recordReservations, (std::vector<bool>{false}));
}
