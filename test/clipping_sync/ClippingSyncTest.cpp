#include <ArduinoJson.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <gtest/gtest.h>
#include <uzlib.h>

#include <map>
#include <string>

#include "ArrayHeap.h"
#include "ClippingStore.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "clippings/ClippingIdentity.h"
#include "network/ClippingsUpload.h"

namespace {
constexpr const char* BOOK = "/books/test.epub";
const std::string DOCUMENT(32, 'a');
using Result = StatsUploadClient::Result;
std::map<std::string, std::string> remote;
uint64_t revision = 0;
unsigned putCount = 0;
unsigned getCount = 0;
uint64_t lastCursor = 0;
bool offline = false;
std::string forcedReply;

std::string storePath(const std::string& book = BOOK) {
  return "/.crosspoint/clippings/epub_" + std::to_string(uzlib_crc32(book.data(), book.size(), 0)) + ".bin";
}

std::string scope() {
  char scope[17];
  EXPECT_TRUE(clippingSyncIdentity(0,
                                   std::to_string(testCredentials.server.size()) + ":" + testCredentials.server +
                                       testCredentials.user + ":" + DOCUMENT,
                                   scope));
  return scope;
}

std::string savedKey(size_t ordinal) {
  Clipping clip;
  std::string text, key;
  bool done = false;
  EXPECT_TRUE(ClippingStore::readForUpload(BOOK, ordinal, clip, text, done, &key));
  EXPECT_FALSE(done);
  return key;
}

void addRemote(const std::string& id, const std::string& text, bool deleted = false) {
  JsonDocument json;
  json["id"] = id;
  json["revision"] = ++revision;
  json["deleted"] = deleted ? 1 : 0;
  json["spine"] = 2;
  json["start_page"] = 3;
  json["end_page"] = 4;
  json["pages"] = 20;
  json["start_word"] = 5;
  json["end_word"] = 6;
  json["words"] = 7;
  json["para"] = 12;
  json["created_at"] = 1790000000U;
  json["layout_signature"] = 123456;
  json["chapter"] = "Chapter";
  json["text"] = text;
  std::string row;
  serializeJson(json, row);
  remote[id] = row;
}

class ClippingSync : public ::testing::Test {
 protected:
  void SetUp() override {
    CLIPPINGS.unload();
    ASSERT_EQ(openBookmarkReaders, 0);
    Storage.files.clear();
    Storage.writes = 0;
    failBookmarkWriteOpen = failBookmarkSync = failBookmarkClose = false;
    failBookmarkRenameFrom.clear();
    maxBookmarkWriteBytes = shortReadAfter = -1;
    remote.clear();
    revision = putCount = getCount = 0;
    lastCursor = 0;
    testCredentials = {};
    maxClippingTestArrayAllocation = SIZE_MAX;
    offline = false;
    forcedReply.clear();
    Storage.files["/My Clippings.txt"] = "local export\n";
  }
  void TearDown() override {
    CLIPPINGS.unload();
    maxClippingTestArrayAllocation = SIZE_MAX;
    EXPECT_EQ(openBookmarkReaders, 0);
    EXPECT_FALSE(Storage.files.contains("/.crosspoint/clipping-sync/download.tmp"));
    EXPECT_FALSE(Storage.files.contains("/.crosspoint/clipping-sync/upload.tmp"));
    EXPECT_EQ(Storage.files.at("/My Clippings.txt"), "local export\n");
  }
  void load() { ASSERT_TRUE(CLIPPINGS.loadForBook(BOOK, "Title", "Author", "epub")); }
  void addLocal() {
    load();
    ASSERT_EQ(CLIPPINGS.addClipping(2, 3, 4, 20, 5, 6, 7, "Chapter", 12, "local selection", UINT16_MAX, 42),
              ClippingStore::AddResult::Added);
    CLIPPINGS.unload();
  }
};
}  // namespace

bool ReadingStatsDate::isValid() const { return true; }
uint32_t readingStatsDayIndex(const ReadingStatsDate&) { return 0; }
void SyncPending::SessionMarker::mark(const std::string&, uint8_t) {}

// In-memory transport exercises the production serializer, pagination parser,
// store and retry flow; no test code substitutes the merge implementation.
struct StatsUploadClient::Impl {};
StatsUploadClient::StatsUploadClient() = default;
StatsUploadClient::~StatsUploadClient() = default;
void StatsUploadClient::close() {}
Result StatsUploadClient::put(const char*, const char* payload, unsigned, bool, const char*) {
  if (offline) return Result::Network;
  ++putCount;
  JsonDocument request;
  if (deserializeJson(request, payload)) return Result::InvalidResponse;
  auto item = request["items"][0];
  const std::string id = item["id"].as<std::string>();
  JsonDocument previous;
  if (remote.contains(id)) deserializeJson(previous, remote[id]);
  if (previous["deleted"] == 1 && item["deleted"] != 1) return Result::Ok;
  item["deleted"] = item["deleted"] == 1 ? 1 : 0;
  item["revision"] = ++revision;
  std::string row;
  serializeJson(item, row);
  remote[id] = row;
  return Result::Ok;
}
Result StatsUploadClient::putFile(const char* endpoint, const char* path, unsigned count) {
  auto found = Storage.files.find(path);
  if (found == Storage.files.end()) return Result::InvalidResponse;
  EXPECT_EQ(found->second.find('\x01'), std::string::npos);
  return put(endpoint, found->second.c_str(), count);
}
Result StatsUploadClient::get(const char* endpoint, const char* responsePath, size_t maxBytes) {
  ++getCount;
  if (offline) return Result::Network;
  std::string body = forcedReply;
  if (body.empty()) {
    const char* query = strstr(endpoint, "cursor=");
    const uint64_t cursor = query ? strtoull(query + 7, nullptr, 10) : 0;
    lastCursor = cursor;
    uint64_t firstRevision = UINT64_MAX;
    std::string first;
    unsigned matches = 0;
    for (const auto& [id, row] : remote) {
      JsonDocument json;
      deserializeJson(json, row);
      const uint64_t rev = json["revision"];
      if (rev > cursor) {
        ++matches;
        if (rev < firstRevision) {
          firstRevision = rev;
          first = row;
        }
      }
    }
    body = "{\"document\":\"" + DOCUMENT +
           "\",\"sync_version\":2,\"cursor\":" + std::to_string(matches ? firstRevision : cursor) +
           ",\"more\":" + (matches > 1 ? "true" : "false") + ",\"items\":[" + first + "]}";
  }
  if (body.size() > maxBytes) return Result::InvalidResponse;
  FsFile file;
  if (!Storage.openFileForWrite("test", responsePath, file)) return Result::InvalidResponse;
  const bool written = file.write(body.data(), body.size()) == body.size() && file.sync();
  const bool closed = file.close();
  return written && closed ? Result::Ok : Result::InvalidResponse;
}

TEST_F(ClippingSync, ImportsFullTextWithoutDuplicatesOrReuploadingForeignAnchors) {
  std::string text;
  for (unsigned i = 0; i < 2048; ++i) text += "é";
  addRemote("provider:long-id", text);
  addRemote("device-A", "second selection");
  ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Skipped);
  EXPECT_EQ(putCount, 0U);
  load();
  ASSERT_EQ(CLIPPINGS.clippingCount(), 2U);
  std::string saved;
  ASSERT_TRUE(CLIPPINGS.readClippingText(0, saved));
  EXPECT_EQ(saved, text);
  EXPECT_EQ(savedKey(0), scope() + "provider:long-id");
  EXPECT_EQ(CLIPPINGS.clippingAt(0)->layoutSignature, 0U);
  EXPECT_EQ(CLIPPINGS.clippingAt(0)->paragraphIndex, 12);
}

TEST_F(ClippingSync, LocalSelectionEchoKeepsItsLayoutAndNeverDuplicates) {
  addLocal();
  ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  EXPECT_EQ(remote.size(), 1U);
  load();
  ASSERT_EQ(CLIPPINGS.clippingCount(), 1U);
  EXPECT_EQ(CLIPPINGS.clippingAt(0)->layoutSignature, 42U);
}

TEST_F(ClippingSync, OfflineDeletionOfFinalSelectionSurvivesRetryAndBlocksResurrection) {
  addRemote("other-device", "selection");
  ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  load();
  ASSERT_TRUE(CLIPPINGS.removeClippingAt(0));
  CLIPPINGS.unload();
  offline = true;
  EXPECT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Network);
  offline = false;
  // A download before the pending delete is sent must not resurrect it.
  ASSERT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Skipped);
  load();
  EXPECT_EQ(CLIPPINGS.clippingCount(), 0U);
  CLIPPINGS.unload();
  ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  JsonDocument row;
  deserializeJson(row, remote.at("other-device"));
  EXPECT_EQ(row["deleted"], 1);
  std::string id;
  bool done = false;
  ASSERT_TRUE(ClippingStore::readDeletionForUpload(BOOK, 0, id, done));
  EXPECT_TRUE(done);
}

TEST_F(ClippingSync, RemoteDeletionWinsOverStaleLocalUpload) {
  addLocal();
  ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  const auto id = remote.begin()->first;
  addRemote(id, "local selection", true);
  ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  load();
  EXPECT_FALSE(CLIPPINGS.hasClippings());
}

TEST_F(ClippingSync, FailedDeleteAndImportLeaveThePreviousFileReadable) {
  addLocal();
  load();
  const auto original = Storage.files.at(storePath());
  failBookmarkRenameFrom = storePath() + ".tmp";
  EXPECT_FALSE(CLIPPINGS.removeClippingAt(0));
  EXPECT_EQ(CLIPPINGS.clippingCount(), 1U);
  EXPECT_EQ(Storage.files.at(storePath()), original);
  CLIPPINGS.unload();
  addRemote("new", "remote selection");
  EXPECT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::InvalidResponse);
  EXPECT_EQ(Storage.files.at(storePath()), original);
  failBookmarkRenameFrom.clear();
  ASSERT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Ok);
  load();
  EXPECT_EQ(CLIPPINGS.clippingCount(), 2U);
}

TEST_F(ClippingSync, ClearAllAndRenamePreservePendingDeletes) {
  addRemote("one", "first");
  addRemote("two", "second");
  ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  load();
  CLIPPINGS.clearAll();
  EXPECT_FALSE(CLIPPINGS.hasClippings());
  CLIPPINGS.unload();
  ASSERT_TRUE(ClippingStore::migrateForFilePath(BOOK, "/moved.epub", "Title", "Author", "epub"));
  std::string id;
  bool done;
  ASSERT_TRUE(ClippingStore::readDeletionForUpload("/moved.epub", 1, id, done));
  EXPECT_FALSE(done);
  ASSERT_EQ(ClippingsUpload::sync("/moved.epub", DOCUMENT), Result::Ok);
  ASSERT_TRUE(CLIPPINGS.loadForBook("/moved.epub", "Title", "Author", "epub"));
  EXPECT_FALSE(CLIPPINGS.hasClippings());
}

TEST_F(ClippingSync, BackupRecoveryAndReplayKeepImportedIds) {
  addRemote("stable", "selection");
  ASSERT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Ok);
  const auto file = storePath();
  ASSERT_TRUE(Storage.rename(file.c_str(), (file + ".bak").c_str()));
  ASSERT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Skipped);
  load();
  ASSERT_EQ(CLIPPINGS.clippingCount(), 1U);
  EXPECT_EQ(savedKey(0), scope() + "stable");
}

TEST_F(ClippingSync, RejectsMalformedOrNonAdvancingPagesWithoutChangingSavedData) {
  addLocal();
  const auto original = Storage.files.at(storePath());
  for (const char* bad :
       {"{}", "<html>portal</html>", "{\"sync_version\":2,\"cursor\":0,\"more\":true,\"items\":[]}"}) {
    forcedReply = bad;
    EXPECT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::InvalidResponse);
    EXPECT_EQ(Storage.files.at(storePath()), original);
  }
}

TEST_F(ClippingSync, BookLimitFailsWithoutDroppingSavedSelectionsAndCanRetry) {
  Clipping clip;
  std::string id;
  for (unsigned i = 0; i < CLIPPING_MAX_PER_BOOK; ++i) {
    id = "id-" + std::to_string(i);
    ASSERT_TRUE(ClippingStore::mergeFromSync(BOOK, clip, "selection", scope() + id, false));
  }
  id = "extra";
  EXPECT_FALSE(ClippingStore::mergeFromSync(BOOK, clip, "extra selection", scope() + id, false));
  load();
  EXPECT_EQ(CLIPPINGS.clippingCount(), CLIPPING_MAX_PER_BOOK);
  CLIPPINGS.unload();
  id = "id-0";
  EXPECT_TRUE(ClippingStore::mergeFromSync(BOOK, clip, "selection", scope() + id, false));
  EXPECT_TRUE(ClippingStore::mergeFromSync(BOOK, clip, "", scope() + id, true));
  id = "extra";
  EXPECT_TRUE(ClippingStore::mergeFromSync(BOOK, clip, "extra selection", scope() + id, false));
}

TEST_F(ClippingSync, ReadsLegacyVersionsAndKeepsExistingServerIdentity) {
  const auto pod = []<typename T>(std::string& bytes, const T& value) {
    bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
  };
  const auto string = [&](std::string& bytes, const std::string& value) {
    pod(bytes, static_cast<uint32_t>(value.size()));
    bytes += value;
  };
  for (uint8_t version = 1; version <= 5; ++version) {
    remote.clear();
    std::string bytes;
    pod(bytes, version);
    pod(bytes, uint16_t{1});
    string(bytes, "Title");
    string(bytes, "Author");
    string(bytes, BOOK);
    if (version == 5) {
      pod(bytes, uint16_t{1});
      string(bytes, "unscoped-deletion");
    }
    const size_t recordOffset = bytes.size();
    for (uint16_t field : {2, 3, 4, 20, 5, 6, 7, 12}) pod(bytes, field);
    pod(bytes, uint32_t{1752300000});
    if (version >= 3) pod(bytes, uint32_t{42});
    if (version >= 4) pod(bytes, uint16_t{7});
    if (version == 5) {
      string(bytes, "original-id");
      pod(bytes, uint8_t{1});
    }
    char title[48] = "Chapter";
    bytes.append(title, sizeof(title));
    if (version == 1)
      pod(bytes, uint32_t{5});
    else
      pod(bytes, uint16_t{5});
    bytes += "hello";
    for (bool pullFirst : {false, true}) {
      remote.clear();
      Storage.files[storePath()] = bytes;
      const std::string id = version == 5 ? "original-id" : "00dd471ed33eb756";
      if (pullFirst) {
        addRemote(id, "hello");
        ASSERT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Ok);
        EXPECT_EQ(savedKey(0), scope() + id);
      }
      ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
      ASSERT_EQ(remote.size(), 1U);
      EXPECT_EQ(remote.begin()->first, version == 5 ? "original-id" : "00dd471ed33eb756");
      load();
      ASSERT_EQ(CLIPPINGS.clippingCount(), 1U);
      EXPECT_EQ(CLIPPINGS.clippingAt(0)->layoutSignature, version >= 3 ? 42U : 0U);
      EXPECT_EQ(CLIPPINGS.clippingAt(0)->tableSelection, version >= 4 ? 7 : UINT16_MAX);
      ASSERT_TRUE(CLIPPINGS.removeClippingAt(0));
      CLIPPINGS.unload();
      EXPECT_EQ(static_cast<uint8_t>(Storage.files.at(storePath())[0]), 6);
      ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
      load();
      EXPECT_FALSE(CLIPPINGS.hasClippings());
      CLIPPINGS.unload();
    }
    if (version == 5) {
      // A download-only sync binds the first duplicate. Deleting it before the
      // first upload must not let the remaining selection reuse its tombstoned ID.
      bytes[1] = 2;
      bytes += bytes.substr(recordOffset);
      Storage.files[storePath()] = bytes;
      remote.clear();
      addRemote("original-id", "hello");
      ASSERT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Ok);
      load();
      ASSERT_TRUE(CLIPPINGS.removeClippingAt(0));
      CLIPPINGS.unload();
      ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
      ASSERT_EQ(remote.size(), 2U);
      load();
      ASSERT_EQ(CLIPPINGS.clippingCount(), 1U);
      EXPECT_NE(savedKey(0), scope() + "original-id");
      CLIPPINGS.unload();
    }
  }
}

TEST_F(ClippingSync, FullBookConvergesWhenRemoteAdditionPrecedesDeletion) {
  Clipping clip;
  std::string id;
  for (unsigned i = 0; i < CLIPPING_MAX_PER_BOOK; ++i) {
    id = "id-" + std::to_string(i);
    ASSERT_TRUE(ClippingStore::mergeFromSync(BOOK, clip, "selection", scope() + id, false));
  }
  addRemote("extra", "extra selection");
  addRemote("id-0", "selection", true);
  ASSERT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Ok);
  load();
  ASSERT_EQ(CLIPPINGS.clippingCount(), CLIPPING_MAX_PER_BOOK);
  EXPECT_EQ(savedKey(CLIPPING_MAX_PER_BOOK - 1), scope() + "extra");
}

TEST_F(ClippingSync, NeverUploadedSelectionsCanBeDeletedIndefinitelyWithoutSync) {
  for (unsigned i = 0; i < 300; ++i) {
    addLocal();
    load();
    ASSERT_TRUE(CLIPPINGS.removeClippingAt(0));
    CLIPPINGS.unload();
  }
  std::string id;
  bool done = false;
  ASSERT_TRUE(ClippingStore::readDeletionForUpload(BOOK, 0, id, done));
  EXPECT_TRUE(done);
}
TEST_F(ClippingSync, LegacyServerRetainsSuccessfulUploadOnlyBehavior) {
  addLocal();
  forcedReply = "{\"document\":\"" + DOCUMENT + "\",\"until\":123,\"more\":false,\"items\":[]}";
  EXPECT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  EXPECT_EQ(putCount, 1U);
}

TEST_F(ClippingSync, CursorResumesAfterCancellationAndSeparatesAccounts) {
  addRemote("one", "one", true);
  addRemote("two", "two", true);
  addRemote("three", "three", true);
  unsigned polls = 0;
  const auto cancel = [](void* context) { return ++*static_cast<unsigned*>(context) > 1; };
  EXPECT_EQ(ClippingsUpload::download(BOOK, DOCUMENT, cancel, &polls), Result::Cancelled);
  EXPECT_EQ(getCount, 1U);
  EXPECT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Ok);
  EXPECT_EQ(getCount, 3U);  // Starts at revision one, rather than replaying it.
  EXPECT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Skipped);
  EXPECT_EQ(getCount, 4U);  // Entire history is not replayed on the next sync.
  EXPECT_EQ(lastCursor, 3U);
  testCredentials.user = "another account";
  EXPECT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Ok);
  EXPECT_EQ(getCount, 7U);
}
TEST_F(ClippingSync, FailedCursorCheckpointSafelyReplaysTheAppliedItem) {
  addRemote("one", "selection");
  failBookmarkRenameFrom = ClippingStore::syncStatePath(BOOK) + ".tmp";
  EXPECT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::InvalidResponse);
  failBookmarkRenameFrom.clear();
  EXPECT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Ok);
  load();
  EXPECT_EQ(CLIPPINGS.clippingCount(), 1U);
}

TEST_F(ClippingSync, DeletingBookOrLosingStoreResetsItsDownloadCursor) {
  addRemote("one", "selection");
  ASSERT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Ok);
  ClippingStore::deleteForFilePath(BOOK, "epub");
  ASSERT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Ok);
  ASSERT_TRUE(Storage.remove(storePath().c_str()));
  ASSERT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Ok);
  load();
  EXPECT_EQ(CLIPPINGS.clippingCount(), 1U);
}

TEST_F(ClippingSync, PreparesAllLocalIdsInOneRewriteBeforeUpload) {
  load();
  for (unsigned i = 0; i < 4; ++i) {
    ASSERT_EQ(
        CLIPPINGS.addClipping(0, 0, 0, 1, 0, 0, 1, "Chapter", 0, "selection " + std::to_string(i), UINT16_MAX, 42),
        ClippingStore::AddResult::Added);
  }
  CLIPPINGS.unload();
  const auto before = Storage.writes;
  ASSERT_TRUE(ClippingStore::prepareUploads(BOOK, scope()));
  EXPECT_EQ(Storage.writes - before, 1U);
  ASSERT_TRUE(ClippingStore::prepareUploads(BOOK, scope()));
  EXPECT_EQ(Storage.writes - before, 1U);
}
TEST_F(ClippingSync, ReplacingDestinationDuringMoveResetsItsCursor) {
  addLocal();
  addRemote("remote", "selection");
  ASSERT_EQ(ClippingsUpload::download("/moved.epub", DOCUMENT), Result::Ok);
  ASSERT_TRUE(ClippingStore::migrateForFilePath(BOOK, "/moved.epub", "Title", "Author", "epub"));
  ASSERT_EQ(ClippingsUpload::download("/moved.epub", DOCUMENT), Result::Ok);
  ASSERT_TRUE(CLIPPINGS.loadForBook("/moved.epub", "Title", "Author", "epub"));
  EXPECT_EQ(CLIPPINGS.clippingCount(), 2U);
}

TEST_F(ClippingSync, DeletingOneOfTwoIdenticalSelectionsPreservesTheOther) {
  load();
  for (const int page : {1, 8})
    ASSERT_EQ(CLIPPINGS.addClipping(0, page, page, 20, 0, 0, 1, "Chapter", 0, "same", UINT16_MAX, 42),
              ClippingStore::AddResult::Added);
  CLIPPINGS.unload();
  ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  ASSERT_EQ(remote.size(), 2U);
  load();
  ASSERT_TRUE(CLIPPINGS.removeClippingAt(0));
  CLIPPINGS.unload();
  ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  load();
  ASSERT_EQ(CLIPPINGS.clippingCount(), 1U);
  EXPECT_EQ(CLIPPINGS.clippingAt(0)->startPage, 8);
}

TEST_F(ClippingSync, DeletionsWaitForTheirOriginalAccountEvenWhenIdsMatch) {
  addRemote("shared", "A's passage");
  ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  load();
  ASSERT_TRUE(CLIPPINGS.removeClippingAt(0));
  CLIPPINGS.unload();
  auto firstAccount = remote;
  remote.clear();
  testCredentials.user = "B";
  addRemote("shared", "B's passage");
  addRemote("other", "B's other passage");
  ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  EXPECT_EQ(putCount, 0U);
  load();
  ASSERT_TRUE(CLIPPINGS.removeClippingAt(1));
  CLIPPINGS.unload();
  ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  EXPECT_EQ(putCount, 1U);
  testCredentials.user = "reader";
  remote.swap(firstAccount);
  ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  EXPECT_EQ(putCount, 2U);
  load();
  ASSERT_EQ(CLIPPINGS.clippingCount(), 1U);
  std::string passage;
  ASSERT_TRUE(CLIPPINGS.readClippingText(0, passage));
  EXPECT_EQ(passage, "B's passage");
}

TEST_F(ClippingSync, DownloadsWorstCaseEscapedSelectionWithFragmentedHeap) {
  // A full passage can expand to six times its size on the wire.
  const std::string text(CLIPPING_TEXT_MAX, '\x01');
  addRemote("escaped", text);
  // Match a server serializer that emits control bytes as Unicode escapes.
  std::string wire;
  wire.reserve(CLIPPING_TEXT_MAX * 6 + 1024);
  for (const char value : remote.at("escaped")) {
    if (value == '\x01')
      wire += "\\u0001";
    else
      wire += value;
  }
  remote["escaped"] = std::move(wire);
  ASSERT_GT(remote.at("escaped").size(), CLIPPING_TEXT_MAX * 6);
  maxClippingTestArrayAllocation = 16 * 1024;
  ASSERT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  load();
  std::string saved;
  ASSERT_TRUE(CLIPPINGS.readClippingText(0, saved));
  EXPECT_EQ(saved, text);
  EXPECT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Skipped);
}

TEST_F(ClippingSync, DownloadFileFailuresDoNotAdvanceCursor) {
  addRemote("remote", "selection");
  failBookmarkWriteOpen = true;
  EXPECT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::InvalidResponse);
  failBookmarkWriteOpen = false;
  maxBookmarkWriteBytes = 8;
  EXPECT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::InvalidResponse);
  maxBookmarkWriteBytes = -1;
  failBookmarkSync = true;
  EXPECT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::InvalidResponse);
  failBookmarkSync = false;
  failBookmarkClose = true;
  EXPECT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::InvalidResponse);
  failBookmarkClose = false;
  ASSERT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Ok);
  EXPECT_EQ(lastCursor, 0U);
  EXPECT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Skipped);
}

TEST_F(ClippingSync, FailedPullAfterSuccessfulUploadStillReportsFailureAndRetries) {
  addLocal();
  forcedReply = "{broken";
  EXPECT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::InvalidResponse);
  EXPECT_EQ(remote.size(), 1U);
  forcedReply.clear();
  EXPECT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  load();
  EXPECT_EQ(CLIPPINGS.clippingCount(), 1U);
}

TEST_F(ClippingSync, OversizedReplyCannotAdvanceCursorOrImportData) {
  forcedReply.assign(CLIPPING_TEXT_MAX * 6 + 2049, ' ');
  EXPECT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::InvalidResponse);
  forcedReply.clear();
  addRemote("remote", "selection");
  EXPECT_EQ(ClippingsUpload::download(BOOK, DOCUMENT), Result::Ok);
  EXPECT_EQ(lastCursor, 0U);
}

TEST_F(ClippingSync, UploadThenDownloadCompletesWithoutWorstCaseResponseAllocation) {
  addLocal();
  maxClippingTestArrayAllocation = 16 * 1024;
  EXPECT_EQ(ClippingsUpload::sync(BOOK, DOCUMENT), Result::Ok);
  EXPECT_EQ(putCount, 1U);
  EXPECT_GT(getCount, 0U);
}

TEST_F(ClippingSync, ImportsNearIndexLimitsWithoutHoldingTwoBookIndexes) {
  for (const unsigned count : {127, 128, 254, 255, 256}) {
    SCOPED_TRACE(count);
    Storage.files.clear();
    Storage.files["/My Clippings.txt"] = "local export\n";
    load();
    for (unsigned i = 0; i < count; ++i) {
      ASSERT_EQ(CLIPPINGS.addClipping(0, i, i, 300, 0, 1, 2, "Chapter", UINT16_MAX, "local-" + std::to_string(i),
                                      UINT16_MAX, 42),
                ClippingStore::AddResult::Added);
    }
    CLIPPINGS.unload();
    arrayHeap::live = arrayHeap::peak = 0;
    const size_t slots = std::min<size_t>(count + 1, CLIPPING_MAX_PER_BOOK);
    arrayHeap::limit = slots * (sizeof(Clipping) + sizeof(uint32_t) + sizeof(uint8_t)) + 256;
    arrayHeap::tracking = true;
    Clipping clip;
    bool full = false;
    const bool merged = ClippingStore::mergeFromSync(BOOK, clip, "incoming", scope() + "incoming", false, &full);
    arrayHeap::tracking = false;
    EXPECT_EQ(merged, count < CLIPPING_MAX_PER_BOOK);
    EXPECT_EQ(full, count == CLIPPING_MAX_PER_BOOK);
    EXPECT_EQ(arrayHeap::live, 0U);
    EXPECT_LE(arrayHeap::peak, arrayHeap::limit);
    load();
    EXPECT_EQ(CLIPPINGS.clippingCount(), slots);
    CLIPPINGS.unload();
  }
}

TEST_F(ClippingSync, FailedImportAllocationPreservesStoreAndRetry) {
  addLocal();
  const auto before = Storage.files.at(storePath());
  arrayHeap::live = arrayHeap::peak = 0;
  arrayHeap::limit = 1;
  arrayHeap::tracking = true;
  Clipping clip;
  bool full = false;
  const bool merged = ClippingStore::mergeFromSync(BOOK, clip, "incoming", scope() + "incoming", false, &full);
  arrayHeap::tracking = false;
  EXPECT_FALSE(merged);
  EXPECT_FALSE(full);
  EXPECT_EQ(arrayHeap::live, 0U);
  EXPECT_EQ(Storage.files.at(storePath()), before);
  EXPECT_TRUE(ClippingStore::mergeFromSync(BOOK, clip, "incoming", scope() + "incoming", false, &full));
}

TEST_F(ClippingSync, UploadsWorstCaseEscapingWithoutLargeArrayAllocation) {
  load();
  const std::string text(4096, '\x01');
  ASSERT_EQ(CLIPPINGS.addClipping(2, 3, 4, 20, 5, 6, 7, "Chapter", 12, text, UINT16_MAX, 42),
            ClippingStore::AddResult::Added);
  CLIPPINGS.unload();
  maxClippingTestArrayAllocation = 8192;
  ASSERT_EQ(ClippingsUpload::upload(BOOK, DOCUMENT), Result::Ok);
  ASSERT_EQ(remote.size(), 1U);
  JsonDocument json;
  ASSERT_FALSE(deserializeJson(json, remote.begin()->second));
  EXPECT_EQ(json["text"].as<std::string>(), text);
}
