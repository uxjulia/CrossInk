#include "ClippingsUpload.h"

#include <ArduinoJson.h>
#include <JsonFileReader.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include "ClippingStore.h"
#include "clippings/ClippingIdentity.h"
#include "sync/SyncFiles.h"

namespace {
// ArduinoJson escapes common whitespace but writes other control bytes verbatim.
// Keep every SD-staged body valid JSON without an expanded in-memory string.
class JsonUploadWriter {
 public:
  explicit JsonUploadWriter(FsFile& file) : file(file) {}
  size_t write(uint8_t value) {
    if (value < 0x20) {
      static constexpr char HEX_DIGITS[] = "0123456789abcdef";
      const char escaped[] = {'\\', 'u', '0', '0', HEX_DIGITS[value >> 4], HEX_DIGITS[value & 15]};
      return emit(reinterpret_cast<const uint8_t*>(escaped), sizeof(escaped)) ? 1 : 0;
    }
    return emit(&value, 1) ? 1 : 0;
  }
  size_t write(const uint8_t* data, size_t length) {
    size_t count = 0;
    while (count < length && write(data[count])) ++count;
    return count;
  }
  bool finish() { return flush() && ok; }

 private:
  bool emit(const uint8_t* data, size_t length) {
    if (!ok) return false;
    for (size_t i = 0; i < length; ++i) {
      if (used == sizeof(buffer) && !flush()) return false;
      buffer[used++] = data[i];
    }
    return true;
  }
  bool flush() {
    if (used) ok = ok && file.write(buffer, used) == used;
    used = 0;
    return ok;
  }
  FsFile& file;
  uint8_t buffer[128];
  size_t used = 0;
  bool ok = true;
};

StatsUploadClient::Result readResponse(const char* path, JsonDocument& json) {
  using Result = StatsUploadClient::Result;
  FsFile file;
  if (!Storage.openFileForRead("ClippingsSync", path, file)) {
    LOG_ERR("ClippingsSync", "Cannot open download response");
    return Result::InvalidResponse;
  }
  serialization::JsonFileReader reader(file);
  const auto error = deserializeJson(json, reader);
  // Release the sole SD reader before merging clippings or updating the cursor.
  const bool closed = file.close();
  if (error || reader.failed() || !closed) {
    LOG_ERR("ClippingsSync", "Cannot parse download response: %s", error.c_str());
    return error == DeserializationError::NoMemory ? Result::LowMemory : Result::InvalidResponse;
  }
  return Result::Ok;
}

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
  // One clipping at a time; never hold the up-to-256-record index across TLS.
  constexpr const char* UPLOAD_PATH = "/.crosspoint/clipping-sync/upload.tmp";
  if ((!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) ||
      (!Storage.exists("/.crosspoint/clipping-sync") && !Storage.mkdir("/.crosspoint/clipping-sync"))) {
    LOG_ERR("ClippingsSync", "Cannot create upload directory");
    return Result::InvalidResponse;
  }
  ScopedCleanup cleanup{[&] { Storage.remove(UPLOAD_PATH); }};
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
    if (clippingHasSourceRange(clipping)) {
      item["start_offset"] = clipping.sourceStart;
      item["end_offset"] = clipping.sourceEnd;
    }
    // Omit note/color: preserve server annotations.
    if (json.overflowed()) return Result::LowMemory;
    const size_t needed = measureJson(json);
    if (needed > CLIPPING_TEXT_MAX * 6 + 1024) return Result::InvalidResponse;
    // Serialize before TLS, then release both decoded text and the JSON tree.
    client.close();
    FsFile file;
    if (!Storage.openFileForWrite("ClippingsSync", UPLOAD_PATH, file)) {
      LOG_ERR("ClippingsSync", "Cannot open upload staging file");
      return Result::InvalidResponse;
    }
    JsonUploadWriter writer(file);
    const bool written = serializeJson(json, writer) == needed && writer.finish() &&
                         file.size() <= CLIPPING_TEXT_MAX * 6 + 1024 && file.sync();
    const bool closed = file.close();
    if (!written || !closed) {
      LOG_ERR("ClippingsSync", "Cannot stage complete clipping");
      return Result::InvalidResponse;
    }
    json.clear();
    std::string().swap(text);
    const auto result = client.putFile(endpoint.c_str(), UPLOAD_PATH, 1);
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
  // Keep worst-case escaped text off the heap during TLS. Each bounded page
  // is staged on SD and parsed only after the transport releases its connection.
  constexpr size_t MAX_RESPONSE_BYTES = CLIPPING_TEXT_MAX * 6 + 2048;
  constexpr const char* RESPONSE_PATH = "/.crosspoint/clipping-sync/download.tmp";
  if ((!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) ||
      (!Storage.exists("/.crosspoint/clipping-sync") && !Storage.mkdir("/.crosspoint/clipping-sync"))) {
    LOG_ERR("ClippingsSync", "Cannot create download directory");
    return Result::InvalidResponse;
  }
  ScopedCleanup cleanup{[&] { Storage.remove(RESPONSE_PATH); }};
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
    const auto result = client.get(query, RESPONSE_PATH, MAX_RESPONSE_BYTES);
    if (result != Result::Ok) return result;
    JsonDocument json;
    const auto parsed = readResponse(RESPONSE_PATH, json);
    if (parsed != Result::Ok) return parsed;
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
        const bool hasStart = !item["start_offset"].isNull();
        const bool hasEnd = !item["end_offset"].isNull();
        if (hasStart != hasEnd ||
            (hasStart && (!item["start_offset"].is<uint32_t>() || !item["end_offset"].is<uint32_t>())))
          return Result::InvalidResponse;
        if (hasStart) {
          clip.sourceStart = item["start_offset"];
          clip.sourceEnd = item["end_offset"];
          if (!clippingHasSourceRange(clip)) return Result::InvalidResponse;
        }
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
      // All fields are owned above; release JSON before loading the book index.
      json.clear();
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
