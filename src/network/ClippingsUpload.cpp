#include "ClippingsUpload.h"

#include <ArduinoJson.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include "ClippingStore.h"
#include "clippings/ClippingIdentity.h"
#include "sync/SyncFiles.h"

namespace {
bool clippingScope(const std::string& document, char (&scope)[17]) {
  const auto& server = KOREADER_STORE.getBaseUrl();
  const auto& user = KOREADER_STORE.getUsername();
  return clippingSyncIdentity(0, std::to_string(server.size()) + ":" + server + user + ":" + document, scope);
}
uint64_t readCursor(const std::string& path, const char* scope) {
  SyncFiles::Reader reader(path.c_str());
  uint8_t version = 0;
  char savedScope[17]{};
  uint64_t cursor = 0;
  if (reader.open() == SyncFiles::Reader::Source::None || !reader.pod(version) || version != 2 ||
      !reader.read(savedScope, 16) || strcmp(savedScope, scope) != 0 || !reader.pod(cursor) || !reader.atEnd())
    return 0;
  return cursor;
}
bool writeCursor(const std::string& path, const char* scope, const uint64_t cursor) {
  Storage.mkdir("/.crosspoint");
  Storage.mkdir("/.crosspoint/clipping-sync");
  SyncFiles::Writer writer(path.c_str());
  return writer.open() && writer.pod(uint8_t{2}) && writer.write(scope, 16) && writer.pod(cursor) && writer.commit();
}
}  // namespace

StatsUploadClient::Result ClippingsUpload::upload(const std::string& path, const std::string& document, Cancel cancel,
                                                  void* context) {
  using Result = StatsUploadClient::Result;
  if (document.size() != 32 || document.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    return Result::InvalidResponse;
  char scope[17];
  if (!clippingScope(document, scope) || !ClippingStore::prepareUploads(path, scope)) return Result::InvalidResponse;
  const auto endpoint = "/api/v1/clippings/" + document;
  StatsUploadClient client;
  std::string text;
  text.reserve(CLIPPING_TEXT_MAX);
  // One clipping at a time; never hold the up-to-256-record index across TLS.
  // Reuse the request buffer across records, growing fallibly only when needed.
  std::unique_ptr<char[]> payload;
  size_t capacity = 0;
  // Send durable deletions first. A retry is idempotent on the server.
  bool sent = false;
  for (size_t i = 0; i <= CLIPPING_MAX_PER_BOOK; ++i) {
    if (cancel && cancel(context)) return Result::Cancelled;
    std::string id;
    bool done = false;
    if (!ClippingStore::readDeletionForUpload(path, i, id, done)) return Result::InvalidResponse;
    if (done) break;
    if (!id.starts_with(scope)) continue;
    JsonDocument json;
    auto item = json["items"].to<JsonArray>().add<JsonObject>();
    item["id"] = id.c_str() + 16;
    item["deleted"] = 1;
    // IDs can contain JSON escapes; serialize rather than interpolating them.
    char deletion[192];
    if (json.overflowed() || measureJson(json) >= sizeof(deletion)) return Result::InvalidResponse;
    serializeJson(json, deletion, sizeof(deletion));
    const auto result = client.put(endpoint.c_str(), deletion, true);
    if (result != Result::Ok) return result;
    sent = true;
  }
  if (sent) {
    client.close();
    if (!ClippingStore::acknowledgeDeletions(path, scope)) return Result::InvalidResponse;
  }
  for (size_t i = 0; i <= CLIPPING_MAX_PER_BOOK; ++i) {
    if (cancel && cancel(context)) return Result::Cancelled;
    Clipping clipping;
    std::string key;
    bool done = false;
    if (!ClippingStore::readForUpload(path, i, clipping, text, done, &key)) return Result::InvalidResponse;
    if (done) return sent ? Result::Ok : Result::Skipped;
    // Imported clips retain their server anchors and annotations. There is no
    // local text editor, so only their deletions need sending back.
    if (!clipping.uploaded || !clipping.scoped || !key.starts_with(scope)) continue;
    const uint32_t createdAt = clippingUnixTimestamp(clipping.timestamp);
    if (text.empty() || text.size() > CLIPPING_TEXT_MAX) return Result::InvalidResponse;
    JsonDocument json;
    auto item = json["items"].to<JsonArray>().add<JsonObject>();
    item["id"] = key.c_str() + 16;
    item["spine"] = clipping.spineIndex;
    item["start_page"] = clipping.startPage;
    item["end_page"] = clipping.endPage;
    item["pages"] = clipping.pageCount;
    item["start_word"] = clipping.startWordIndex;
    item["end_word"] = clipping.endWordIndex;
    item["words"] = clipping.wordCount;
    if (clipping.paragraphIndex != UINT16_MAX) item["para"] = clipping.paragraphIndex;
    item["chapter"] = static_cast<const char*>(clipping.chapterTitle);
    item["text"] = text.c_str();
    item["created_at"] = createdAt;
    item["layout_signature"] = clipping.layoutSignature;
    // Omit note/color and source offsets: preserve server annotations.
    if (json.overflowed()) return Result::LowMemory;
    const size_t needed = measureJson(json) + 1;
    if (needed > CLIPPING_TEXT_MAX * 6 + 1024) return Result::InvalidResponse;
    if (needed > capacity) {
      payload.reset();
      payload = makeUniqueNoThrow<char[]>(needed);
      if (!payload) {
        LOG_ERR("ClippingsSync", "Cannot allocate %u-byte request", unsigned(needed));
        return Result::LowMemory;
      }
      capacity = needed;
    }
    serializeJson(json, payload.get(), capacity);
    json.clear();
    const auto result = client.put(endpoint.c_str(), payload.get(), true);
    if (result != Result::Ok) return result;
    sent = true;
  }
  return Result::InvalidResponse;
}

StatsUploadClient::Result ClippingsUpload::download(const std::string& path, const std::string& document, Cancel cancel,
                                                    void* context) {
  using Result = StatsUploadClient::Result;
  if (document.size() != 32 || document.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    return Result::InvalidResponse;
  // One worst-case JSON-escaped 4 KiB passage plus bounded metadata. Reuse on
  // heap (too large for task stack); never retain a book index across TLS.
  constexpr size_t CAPACITY = CLIPPING_TEXT_MAX * 6 + 2048;
  auto response = makeUniqueNoThrow<char[]>(CAPACITY);
  if (!response) {
    LOG_ERR("ClippingsSync", "Cannot allocate download buffer");
    return Result::LowMemory;
  }
  StatsUploadClient client;
  const std::string statePath = ClippingStore::syncStatePath(path);
  char scope[17];
  if (!clippingScope(document, scope)) return Result::InvalidResponse;
  // A small CRC-protected sidecar keeps the cursor tied to this server, user
  // and document. Renames/cache loss safely replay through the same ID merge.
  const uint64_t startingCursor =
      ClippingStore::hasStoredStateForFilePath(path, "epub") ? readCursor(statePath, scope) : 0;
  uint64_t cursor = startingCursor;
  bool received = false;
  bool deferredFull = false;
  bool retryingFull = false;
  // Checkpoint each applied page; interruption resumes without rereading a
  // book's entire deletion history. Capacity deferrals keep the earlier cursor.
  for (;;) {
    if (cancel && cancel(context)) return Result::Cancelled;
    char query[160];
    snprintf(query, sizeof(query), "/api/v1/clippings/%s?cursor=%llu&limit=1&format=reader", document.c_str(),
             static_cast<unsigned long long>(cursor));
    const auto result = client.get(query, response.get(), CAPACITY);
    if (result != Result::Ok) return result;
    JsonDocument json;
    // Mutable input keeps strings in the bounded response buffer.
    if (deserializeJson(json, response.get())) return Result::InvalidResponse;
    if (json["sync_version"].isNull() && json["cursor"].isNull() && json["until"].is<uint64_t>() &&
        json["document"].is<const char*>() && document == json["document"].as<const char*>() &&
        json["more"].is<bool>() && json["items"].is<JsonArray>())
      return Result::Unsupported;
    if (json["sync_version"] != 2 || !json["document"].is<const char*>() ||
        document != json["document"].as<const char*>() || !json["cursor"].is<uint64_t>() || !json["more"].is<bool>() ||
        !json["items"].is<JsonArray>())
      return Result::InvalidResponse;
    const uint64_t next = json["cursor"].as<uint64_t>();
    const bool more = json["more"].as<bool>();
    auto items = json["items"].as<JsonArray>();
    if (items.size() > 1 || (items.size() == 0 && (more || next != cursor)) ||
        (items.size() == 1 && (next <= cursor || next > 9007199254740991ULL)))
      return Result::InvalidResponse;
    if (items.size()) {
      const auto item = items[0].as<JsonObject>();
      if (!item["id"].is<const char*>() || !item["revision"].is<uint64_t>() ||
          item["revision"].as<uint64_t>() != next || !item["deleted"].is<unsigned>() ||
          item["deleted"].as<unsigned>() > 1)
        return Result::InvalidResponse;
      Clipping clip;
      const std::string id = item["id"].as<const char*>();
      if (id.empty() || id.size() > 64 || id.size() != item["id"].as<JsonString>().size())
        return Result::InvalidResponse;
      for (const unsigned char c : id)
        if (c < 0x21 || c > 0x7e) return Result::InvalidResponse;
      const bool deleted = item["deleted"].as<unsigned>() == 1;
      std::string text;
      if (!deleted) {
        const char* fields[] = {"spine", "start_page", "end_page", "pages", "start_word", "end_word", "words"};
        for (const char* field : fields)
          if (!item[field].is<uint16_t>()) return Result::InvalidResponse;
        if (!item["created_at"].is<uint32_t>() || !item["text"].is<const char*>() ||
            !item["chapter"].is<const char*>() || (!item["para"].isNull() && !item["para"].is<uint16_t>()))
          return Result::InvalidResponse;
        text = item["text"].as<const char*>();
        if (text.empty() || text.size() > CLIPPING_TEXT_MAX || text.size() != item["text"].as<JsonString>().size())
          return Result::InvalidResponse;
        clip.spineIndex = item["spine"];
        clip.startPage = item["start_page"];
        clip.endPage = item["end_page"];
        clip.pageCount = item["pages"];
        clip.startWordIndex = item["start_word"];
        clip.endWordIndex = item["end_word"];
        clip.wordCount = item["words"];
        clip.paragraphIndex = item["para"].isNull() ? UINT16_MAX : item["para"].as<uint16_t>();
        clip.timestamp = item["created_at"];
        snprintf(clip.chapterTitle, sizeof(clip.chapterTitle), "%s", item["chapter"].as<const char*>());
        clip.chapterTitle[utf8SafeTruncateBuffer(clip.chapterTitle, strlen(clip.chapterTitle))] = '\0';
      }
      bool full = false;
      if (!ClippingStore::mergeFromSync(path, clip, text, scope + id, deleted, &full)) {
        if (!full) return Result::InvalidResponse;
        deferredFull = true;
      }
      received = true;
    }
    cursor = next;
    if (!deferredFull && cursor != startingCursor && !writeCursor(statePath, scope, cursor))
      return Result::InvalidResponse;
    if (!more) {
      if (deferredFull && !retryingFull) {
        // A later tombstone may have freed room for an earlier addition. Replay
        // once after all deletions, without retaining deferred texts in RAM.
        cursor = startingCursor;
        deferredFull = false;
        retryingFull = true;
        continue;
      }
      if (deferredFull) {
        LOG_ERR("ClippingsSync", "Book clipping limit reached");
        return Result::InvalidResponse;
      }
      return received ? Result::Ok : Result::Skipped;
    }
  }
}

StatsUploadClient::Result ClippingsUpload::sync(const std::string& path, const std::string& document, Cancel cancel,
                                                void* context) {
  using Result = StatsUploadClient::Result;
  const auto sent = upload(path, document, cancel, context);
  if (!StatsUploadClient::settled(sent)) return sent;
  const auto received = download(path, document, cancel, context);
  // Older servers remain upload-compatible; a version-2 download is required
  // before any remote data is applied locally.
  if (received == Result::Unsupported) return sent;
  if (received == Result::Skipped) return sent;
  return received;
}
