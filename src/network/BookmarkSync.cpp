#include "BookmarkSync.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <JsonFileReader.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>
#include <Memory.h>
#include <ProgressMapper.h>
#include <mbedtls/sha256.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "BookmarkStore.h"
#include "sync/SyncFiles.h"

namespace {
using Result = StatsUploadClient::Result;
constexpr size_t MAX_TRACKED = 128;
constexpr size_t PAGE_SIZE = 4;
constexpr size_t REQUEST_LIMIT = 12;
constexpr size_t MAX_XPATH = 512;
constexpr char DIRECTORY[] = "/.crosspoint/bookmark-sync";
constexpr char RESPONSE[] = "/.crosspoint/bookmark-sync/reply.tmp";

struct Key {
  uint16_t spine = 0;
  uint16_t paragraph = UINT16_MAX;
  uint32_t progressQ = 0;
  bool operator==(const Key&) const = default;
};
Key key(const Bookmark& bookmark) {
  return {bookmark.spineIndex, bookmark.paragraphIndex,
          static_cast<uint32_t>(
              std::isfinite(bookmark.progress) ? std::clamp(bookmark.progress, 0.0f, 1.0f) * 1000000.0f + 0.5f : 0.0f)};
}
constexpr size_t MAX_ID = 64;  // Official server: printable ASCII, 1–64 bytes.
enum class EntryStatus : uint8_t { Settled, PendingPush, PendingImport, PendingDelete, PendingRestore };
struct Entry {
  char id[MAX_ID + 1]{};
  Key local;
  EntryStatus status = EntryStatus::Settled;
  uint64_t deleteAfter = 0;
};
bool validId(const char* value) {
  if (!value) return false;
  const size_t length = strlen(value);
  if (!length || length > MAX_ID) return false;
  for (size_t i = 0; i < length; ++i)
    if (value[i] < 0x21 || value[i] > 0x7e) return false;
  return true;
}
struct State {
  std::unique_ptr<Entry[]> items = makeUniqueNoThrow<Entry[]>(MAX_TRACKED);
  size_t count = 0;
  uint64_t cursor = 0;
  uint64_t tombstoneScanCursor = 0;
  Entry* index(const char* id) {
    for (size_t i = 0; i < count; ++i)
      if (strcmp(items[i].id, id) == 0) return &items[i];
    return nullptr;
  }
  Entry* track(const Key& value) {
    for (size_t i = 0; i < count; ++i)
      if (items[i].local == value) return &items[i];
    return nullptr;
  }
  bool append(const char* id, const Key& value, EntryStatus status) {
    if (count == MAX_TRACKED) return false;
    auto& e = items[count++];
    snprintf(e.id, sizeof(e.id), "%s", id);
    e.local = value;
    e.status = status;
    e.deleteAfter = 0;
    return true;
  }
  void erase(Entry* entry) { *entry = items[--count]; }
};
bool digestId(const std::string_view text, uint64_t& id) {
  uint8_t digest[32];
  if (mbedtls_sha256(reinterpret_cast<const uint8_t*>(text.data()), text.size(), digest, 0) != 0) return false;
  id = 0;
  for (size_t i = 0; i < 8; ++i) id = (id << 8) | digest[i];
  return true;
}
void printId(uint64_t id, char (&out)[17]) {
  snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(id));
}

bool loadState(const char* path, uint64_t account, uint64_t document, State& state) {
  SyncFiles::Reader file(path);
  bool missing = false;
  if (file.open(&missing) == SyncFiles::Reader::Source::None) return missing;
  uint8_t version;
  uint64_t savedAccount, savedDocument;
  uint16_t count;
  if (!file.pod(version) || version != 2 || !file.pod(savedAccount) || !file.pod(savedDocument) ||
      !file.pod(state.cursor) || !file.pod(state.tombstoneScanCursor) || !file.pod(count) || count > MAX_TRACKED)
    return false;
  if (account != savedAccount) {
    LOG_ERR("BookmarkSync", "Bookmark account changed; retaining local bookmarks without exporting them");
    return false;
  }
  if (document != savedDocument) {
    state.cursor = state.tombstoneScanCursor = 0;
    return true;  // Same account, but rename/match change must not send old-document tombstones.
  }
  for (size_t i = 0; i < count; ++i) {
    auto& e = state.items[i];
    uint8_t length;
    if (!file.pod(length) || !length || length > MAX_ID || !file.read(e.id, length) || !validId(e.id) ||
        !file.pod(e.local.spine) || !file.pod(e.local.paragraph) || !file.pod(e.local.progressQ) ||
        e.local.progressQ > 1000000 || !file.pod(e.status) || e.status > EntryStatus::PendingRestore ||
        !file.pod(e.deleteAfter))
      return false;
    ++state.count;
  }
  return file.atEnd();
}
bool saveState(const char* path, uint64_t account, uint64_t document, const State& state) {
  SyncFiles::Writer file(path);
  if (!file.open() || !file.pod(uint8_t{2}) || !file.pod(account) || !file.pod(document) || !file.pod(state.cursor) ||
      !file.pod(state.tombstoneScanCursor) || !file.pod(static_cast<uint16_t>(state.count)))
    return false;
  for (size_t i = 0; i < state.count; ++i) {
    const auto& e = state.items[i];
    const auto length = static_cast<uint8_t>(strlen(e.id));
    if (!file.pod(length) || !file.write(e.id, length) || !file.pod(e.local.spine) || !file.pod(e.local.paragraph) ||
        !file.pod(e.local.progressQ) || !file.pod(e.status) || !file.pod(e.deleteAfter))
      return false;
  }
  return file.commit();
}
size_t findLocal(const BookmarkRecords& items, const Key& test) {
  for (size_t i = 0; i < items.size(); ++i)
    if (key(items[i]) == test) return i;
  return items.size();
}

Result push(StatsUploadClient& client, const std::string& endpoint, const char* id, const KOReaderPosition* mapped,
            const Bookmark* bookmark) {
  JsonDocument json;
  JsonObject item = json["items"].to<JsonArray>().add<JsonObject>();
  char summary[BOOKMARK_SNIPPET_MAX];
  item["id"] = id;
  if (!bookmark) {
    item["deleted"] = 1;
  } else {
    item["xpath"] = mapped->xpath;
    item["percentage"] = mapped->percentage;
    // Leave name absent: CrossInk does not expose CrossPoint's editable label.
    item["si"] = bookmark->spineIndex;
    item["pc"] = 1000;
    item["pp"] = (key(*bookmark).progressQ * 999 + 500000) / 1000000;
    for (size_t i = 0; i < sizeof(summary); ++i) {
      const auto c = static_cast<unsigned char>(bookmark->snippet[i]);
      summary[i] = c && c < 32 ? ' ' : static_cast<char>(c);
    }
    summary[sizeof(summary) - 1] = 0;
    item["summary"] = static_cast<const char*>(summary);
  }
  const size_t bytes = measureJson(json);
  if (json.overflowed() || !bytes || bytes > 4096) return Result::LowMemory;
  auto body = makeUniqueNoThrow<char[]>(bytes + 1);
  if (!body || serializeJson(json, body.get(), bytes + 1) != bytes) return Result::LowMemory;
  json.clear();
  return client.put(endpoint.c_str(), body.get(), 1);
}

Result readPage(JsonDocument& page) {
  FsFile file;
  if (!Storage.openFileForRead("BookmarkSync", RESPONSE, file)) return Result::InvalidResponse;
  serialization::JsonFileReader input(file);
  JsonDocument filter;
  filter["document"] = true;
  filter["sync_version"] = true;
  filter["cursor"] = true;
  filter["more"] = true;
  filter["until"] = true;
  auto fields = filter["items"].to<JsonArray>().add<JsonObject>();
  for (const char* name : {"id", "revision", "deleted", "xpath", "percentage", "summary"}) fields[name] = true;
  if (filter.overflowed()) {
    file.close();
    return Result::LowMemory;
  }
  const auto error = deserializeJson(page, input, DeserializationOption::Filter(filter));
  const bool closed = file.close();
  if (error == DeserializationError::NoMemory) return Result::LowMemory;
  return error || input.failed() || !closed ? Result::InvalidResponse : Result::Ok;
}
Result downloadPage(StatsUploadClient& client, const std::string& endpoint, const std::string& document,
                    uint64_t cursor, JsonDocument& page) {
  const auto url = endpoint + "?limit=" + std::to_string(PAGE_SIZE) + "&cursor=" + std::to_string(cursor);
  auto result = client.get(url.c_str(), RESPONSE, 16 * 1024);
  if (result == Result::Ok) result = readPage(page);
  Storage.remove(RESPONSE);
  if (result != Result::Ok) return result;
  if (!page["sync_version"].is<unsigned>() || page["sync_version"].as<unsigned>() != 2)
    return page["until"].is<uint64_t>() ? Result::Unsupported : Result::InvalidResponse;
  if ((page["document"] | "") != document || !page["cursor"].is<uint64_t>() || !page["more"].is<bool>() ||
      !page["items"].is<JsonArrayConst>())
    return Result::InvalidResponse;
  const auto next = page["cursor"].as<uint64_t>();
  const auto more = page["more"].as<bool>();
  const auto items = page["items"].as<JsonArrayConst>();
  if (items.size() > PAGE_SIZE || next < cursor || ((more || items.size()) && next == cursor) ||
      (more && items.size() == 0))
    return Result::InvalidResponse;
  uint64_t previous = cursor;
  for (const auto item : items) {
    if (!item.is<JsonObjectConst>() || !validId(item["id"] | "") || !item["revision"].is<uint64_t>() ||
        item["revision"].as<uint64_t>() <= previous)
      return Result::InvalidResponse;
    previous = item["revision"].as<uint64_t>();
  }
  if (previous != next) return Result::InvalidResponse;
  return Result::Ok;
}
}  // namespace

StatsUploadClient::Result BookmarkSync::run(const std::string& path, const std::string& document, bool (*cancel)(void*),
                                            void* context) {
  if (!KOREADER_STORE.getSyncBookmarks() || !KOREADER_STORE.hasCredentials() || !FsHelpers::hasEpubExtension(path))
    return Result::Skipped;
  if (KOREADER_STORE.getServerType() == KOReaderServerType::KOSYNC) return Result::Unsupported;
  if (document.size() != 32 || document.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    return Result::InvalidResponse;
  const auto stop = [&] { return cancel && cancel(context); };
  if (stop()) return Result::Cancelled;
  if (BookmarkStore::syncBlockedByMove(path)) {
    LOG_ERR("BookmarkSync", "Interrupted bookmark move; keeping local bookmarks without exporting them");
    return Result::InvalidResponse;
  }
  if (!Storage.exists(DIRECTORY) && !Storage.mkdir(DIRECTORY)) return Result::InvalidResponse;
  uint64_t account, documentKey;
  const auto server = KOREADER_STORE.getBaseUrl();
  const auto statePath = BookmarkStore::syncStatePath(path);
  if (!digestId(std::to_string(server.size()) + ":" + server + KOREADER_STORE.getUsername(), account) ||
      !digestId(document, documentKey))
    return Result::InvalidResponse;
  State state;
  if (!state.items) return Result::LowMemory;
  const auto commit = [&] { return saveState(statePath.c_str(), account, documentKey, state); };

  const auto openBook = [&]() -> std::shared_ptr<Epub> {
    std::shared_ptr<Epub> epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
    if (!epub || !epub->load(false, true, Epub::XLocationLoadMode::Immediate, true)) return {};
    return epub;
  };
  auto epub = openBook();
  if (!epub) return Result::InvalidResponse;
  BookmarkStore store;
  if (!store.loadForBook(path, epub->getTitle(), epub->getAuthor(), "epub")) return Result::InvalidResponse;
  // Loading can migrate an orphaned bookmark file into /Read along with
  // its ledger. Check the resulting scope, never a pre-migration snapshot.
  if (BookmarkStore::syncBlockedByMove(path) || !loadState(statePath.c_str(), account, documentKey, state))
    return Result::InvalidResponse;
  if (!store.hasDurableSyncSnapshot()) {
    for (size_t i = 0; i < state.count; ++i) {
      if (state.items[i].status == EntryStatus::PendingImport) continue;
      LOG_ERR("BookmarkSync", "Bookmark file missing; not treating the loss as local deletions");
      return Result::InvalidResponse;
    }
  }
  epub.reset();
  const auto coords = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
                          ? PositionCoordinateSpace::SourceDocument
                          : PositionCoordinateSpace::CurrentDocument;
  const std::string endpoint = "/api/v1/bookmarks/" + document;
  StatsUploadClient client(nullptr, cancel, context, 4000);
  const auto& local = store.getBookmarks();
  size_t requests = 0;
  bool drained = false;
  const auto applyTombstone = [&](const char* id, const uint64_t revision) {
    for (size_t j = 0; j < state.count;) {
      auto& e = state.items[j];
      if (strcmp(e.id, id) != 0) {
        ++j;
        continue;
      }
      const auto found = findLocal(local, e.local);
      // A local row now present after we attempted a deletion means the
      // person added it again. Preserve that deliberate intent, exact ID
      // included, while acknowledging the preceding deletion.
      if (e.status == EntryStatus::PendingRestore ||
          (e.status == EntryStatus::PendingDelete && revision > e.deleteAfter && found < local.size())) {
        e.status = EntryStatus::PendingRestore;
        ++j;
        continue;
      }
      const Key removed = e.local;
      state.erase(&e);
      if (!state.track(removed) && found < local.size() && !store.removeBookmarkAt(found)) return false;
    }
    return store.flushSyncChanges();
  };
  if (state.tombstoneScanCursor > state.cursor && state.count == MAX_TRACKED) {
    // Preserve the earliest blocked revision. Separately walk strictly later
    // pages to find known tombstones that free space, even across sleeps.
    for (; requests < REQUEST_LIMIT && state.count == MAX_TRACKED; ++requests) {
      if (stop()) return Result::Cancelled;
      JsonDocument page;
      const auto fetched = downloadPage(client, endpoint, document, state.tombstoneScanCursor, page);
      if (fetched != Result::Ok) return fetched;
      for (const auto item : page["items"].as<JsonArrayConst>()) {
        if ((item["deleted"] | 0u) != 0 || (item["deleted"] | false))
          if (!applyTombstone(item["id"], item["revision"])) return Result::InvalidResponse;
      }
      state.tombstoneScanCursor = state.count == MAX_TRACKED ? page["cursor"].as<uint64_t>() : 0;
      if (!commit()) return Result::InvalidResponse;
      if (!page["more"].as<bool>()) {
        ++requests;
        break;
      }
    }
    if (state.count == MAX_TRACKED) return Result::LowMemory;
  }
  state.tombstoneScanCursor = 0;
  // Pull through the current revision before any PUT. A recorded pending ID
  // lets a tombstone win even when our previous live PUT lost its reply.
  for (; requests < REQUEST_LIMIT; ++requests) {
    if (stop()) return Result::Cancelled;
    JsonDocument page;
    const auto fetched = downloadPage(client, endpoint, document, state.cursor, page);
    if (fetched != Result::Ok) return fetched;
    bool full = false;
    for (const auto item : page["items"].as<JsonArrayConst>()) {
      const char* id = item["id"];
      const uint64_t revision = item["revision"];
      const bool deleted = (item["deleted"] | 0u) != 0 || (item["deleted"] | false);
      if (!deleted) {
        auto* existing = state.index(id);
        if (existing && existing->status == EntryStatus::PendingDelete && revision > existing->deleteAfter) {
          // A newer live server row supersedes the pending deletion. Do not
          // retry the DELETE against this re-add after losing the old ACK.
          while (auto* stale = state.index(id)) state.erase(stale);
          existing = nullptr;
        }
        if (existing && existing->status != EntryStatus::PendingImport) {
          for (size_t i = 0; i < state.count; ++i)
            if (strcmp(state.items[i].id, id) == 0) state.items[i].status = EntryStatus::Settled;
          if (!commit()) return Result::InvalidResponse;
          continue;
        }
        if (!existing && state.count == MAX_TRACKED) {
          full = true;
          continue;  // Still apply any tracked tombstones in this page.
        }
        const char* xpath = item["xpath"] | "";
        const float progress = item["percentage"] | -1.0f;
        if (!*xpath || strlen(xpath) > MAX_XPATH || !std::isfinite(progress) || progress < 0 || progress > 1)
          return Result::InvalidResponse;
        epub = openBook();
        if (!epub) return Result::InvalidResponse;
        const auto mapped = ProgressMapper::toCrossPoint(epub, {xpath, progress}, -1, 0, coords, 1000);
        if (!mapped.valid || mapped.spineIndex < 0 || mapped.spineIndex >= epub->getSpineItemsCount() ||
            mapped.spineIndex >= UINT16_MAX || mapped.totalPages <= 1)
          return Result::InvalidResponse;
        Bookmark bookmark{};
        bookmark.spineIndex = mapped.spineIndex;
        bookmark.paragraphIndex = mapped.hasParagraphIndex ? mapped.paragraphIndex : UINT16_MAX;
        bookmark.progress = float(mapped.pageNumber) / (mapped.totalPages - 1);
        const auto toc = epub->getSpineItem(mapped.spineIndex).tocIndex;
        if (toc >= 0 && toc < epub->getTocItemsCount()) {
          const auto title = epub->getTocItem(toc).title;
          snprintf(bookmark.chapterTitle, sizeof(bookmark.chapterTitle), "%s", title.c_str());
        }
        epub.reset();
        snprintf(bookmark.snippet, sizeof(bookmark.snippet), "%s", item["summary"] | "");
        const Key localKey = key(bookmark);
        // Record account provenance before saving any downloaded text. The
        // pending import makes interruptions retryable without duplicating rows.
        if (!existing) {
          if (!state.append(id, localKey, EntryStatus::PendingImport) || !commit()) return Result::InvalidResponse;
          existing = state.index(id);
        } else if (!(existing->local == localKey)) {
          return Result::InvalidResponse;
        }
        if (findLocal(local, localKey) == local.size() && !store.applySyncedBookmark(bookmark))
          return Result::LowMemory;
        if (!store.flushSyncChanges()) return Result::InvalidResponse;
        existing->status = EntryStatus::Settled;
        if (!commit()) return Result::InvalidResponse;
        continue;
      }

      if (!applyTombstone(id, revision) || !commit()) return Result::InvalidResponse;
    }
    const auto next = page["cursor"].as<uint64_t>();
    if (full) {
      state.tombstoneScanCursor = state.count == MAX_TRACKED ? next : 0;
      if (!commit()) return Result::InvalidResponse;
      return Result::LowMemory;  // The first unimported live revision stays replayable.
    }
    if (next != state.cursor) {
      state.cursor = next;
      if (!commit()) return Result::InvalidResponse;
    }
    if (!page["more"].as<bool>()) {
      ++requests;
      drained = true;
      break;
    }
  }
  if (!drained) return Result::Cancelled;  // Already checkpointed; resume on the next sleep.

  bool allMapped = true;
  for (size_t i = 0; i < local.size(); ++i) {
    const auto& bookmark = local[i];
    const auto localKey = key(bookmark);
    auto* pending = state.track(localKey);
    if (pending && pending->status == EntryStatus::Settled) continue;
    if (stop() || requests >= REQUEST_LIMIT) return Result::Cancelled;
    epub = openBook();
    if (!epub) return Result::InvalidResponse;
    if (bookmark.spineIndex >= epub->getSpineItemsCount()) {
      epub.reset();
      allMapped = false;
      continue;
    }
    CrossPointPosition pos{bookmark.spineIndex, int((localKey.progressQ * 999 + 500000) / 1000000), 1000};
    pos.hasParagraphIndex = bookmark.paragraphIndex != UINT16_MAX && bookmark.paragraphIndex > 0;
    pos.paragraphIndex = bookmark.paragraphIndex;
    const auto mapped = ProgressMapper::toKOReader(epub, pos, coords);
    epub.reset();
    uint64_t hashed;
    if (!mapped.valid || mapped.xpath.empty() || mapped.xpath.size() > MAX_XPATH || !std::isfinite(mapped.percentage) ||
        mapped.percentage < 0 || mapped.percentage > 1 || !digestId(mapped.xpath, hashed)) {
      allMapped = false;
      continue;
    }
    char id[17];
    printId(hashed, id);
    if (pending && pending->status == EntryStatus::PendingDelete) {
      // The previous deletion was never observed; the local re-add cancels it.
      pending->status = EntryStatus::Settled;
      if (!commit()) return Result::InvalidResponse;
      continue;
    }
    const bool restoring = pending && pending->status == EntryStatus::PendingRestore;
    if (pending && !restoring && (pending->status != EntryStatus::PendingPush || strcmp(pending->id, id) != 0))
      return Result::InvalidResponse;
    const char* outgoingId = restoring ? pending->id : id;
    auto* duplicate = state.index(outgoingId);
    const bool alreadyConfirmed = duplicate && duplicate->status == EntryStatus::Settled;
    if (!pending) {
      if (!state.append(id, localKey, alreadyConfirmed ? EntryStatus::Settled : EntryStatus::PendingPush))
        return Result::LowMemory;
      if (!commit()) return Result::InvalidResponse;
      pending = &state.items[state.count - 1];
    }
    if (alreadyConfirmed) continue;
    auto result = push(client, endpoint, outgoingId, &mapped, &bookmark);
    client.close();  // Release TLS before the next EPUB/XPath allocation on C3.
    if (result != Result::Ok) return result;
    ++requests;
    pending->status = EntryStatus::Settled;
    if (!commit()) return Result::InvalidResponse;
  }
  // Resolve all aliases before deletion; a second local bookmark with the
  // same XPath is another live reference to the same remote record.
  if (!allMapped) return Result::InvalidResponse;
  for (size_t i = 0; i < state.count;) {
    auto& e = state.items[i];
    if (findLocal(local, e.local) != local.size()) {
      ++i;
      continue;
    }
    if (stop() || requests >= REQUEST_LIMIT) return Result::Cancelled;
    bool remainingAlias = false;
    for (size_t j = 0; j < state.count; ++j) {
      const auto& alias = state.items[j];
      if (&alias != &e && strcmp(alias.id, e.id) == 0 && findLocal(local, alias.local) != local.size()) {
        remainingAlias = true;
        break;
      }
    }
    if (!remainingAlias) {
      if (e.status != EntryStatus::PendingDelete) {
        e.status = EntryStatus::PendingDelete;
        e.deleteAfter = state.cursor;
        if (!commit()) return Result::InvalidResponse;
      }
      auto result = push(client, endpoint, e.id, nullptr, nullptr);
      if (result != Result::Ok) return result;
      ++requests;
    }
    state.erase(&e);
    if (!commit()) return Result::InvalidResponse;
  }
  return Result::Ok;
}
