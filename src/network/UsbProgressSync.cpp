#include "UsbProgressSync.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <KOReaderSyncClient.h>
#include <Logging.h>
#include <Memory.h>
#include <ProgressMapper.h>
#include <Utf8.h>

#include <cmath>
#include <memory>
#include <string_view>

#include "activities/home/RecentBookProgress.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/KOReaderSyncActivity.h"
#include "network/ClippingsUpload.h"
#include "network/ReadingSyncUpload.h"
#include "network/UsbSerialFileTransfer.h"
#include "sync/SyncParked.h"
#include "sync/SyncPending.h"
#include "util/FolderBookIterator.h"

namespace UsbProgressSync {
bool readerId(char* out, const size_t capacity) { return KOReaderSyncClient::readerId(out, capacity); }

namespace {
void fail(JsonDocument& response, const char* error) {
  LOG_ERR("USBSync", "%s", error);
  response.clear();
  response["error"] = error;
}

bool matchesConfiguredAccount(JsonVariantConst request) {
  std::string server = KOREADER_STORE.getBaseUrl();
  while (!server.empty() && server.back() == '/') server.pop_back();
  return server == (request["server"] | "") && KOREADER_STORE.getUsername() == (request["username"] | "");
}

// USB can use a different account from the reader. Only retire the configured
// account's queued/parked state when both server and username agree.
void settle(JsonVariantConst request, JsonDocument& response, const std::string& path, const std::string& id) {
  response["settled"] = false;
  if (!KOREADER_STORE.hasCredentials() || !matchesConfiguredAccount(request)) return;
  if (!SyncParked::remove(id) || !SyncPending::clearExisting(path, SyncPending::PROGRESS_BIT)) {
    LOG_ERR("USBSync", "Cannot settle previous sync state");
    response["settlement_error"] = true;
    return;
  }
  response["settled"] = true;
}

// A logical snapshot, independent of which crash-safe progress slot won.
uint32_t revision(const EpubReaderUtils::Progress& progress) {
  progress_record::Fields fields{static_cast<uint16_t>(progress.spineIndex),
                                 static_cast<uint16_t>(progress.pageNumber),
                                 static_cast<uint16_t>(progress.pageCount),
                                 progress.hasPageCount,
                                 progress.visibleTextOffset,
                                 progress.hasVisibleTextOffset};
  uint8_t bytes[progress_record::RECORD_BYTES];
  progress_record::encode(fields, 0, bytes);
  return progress_record::crc32(bytes, 16);
}

bool validPath(const std::string& path, bool extras = false) {
  if (path.empty() || path.size() > 255 || path.front() != '/' ||
      (!FsHelpers::hasEpubExtension(path) && !(extras && FsHelpers::hasXtcExtension(path))))
    return false;
  // Do not expose internal/hidden files through the progress API.
  for (size_t at = 0; at < path.size(); ++at) {
    if (static_cast<unsigned char>(path[at]) < 32 || path[at] == '\\' ||
        (path[at] == '/' && at + 1 < path.size() && path[at + 1] == '.'))
      return false;
  }
  // normalisePath returns a relative path, even when its input starts with '/'.
  return FsHelpers::normalisePath(path) == std::string_view(path).substr(1);
}

bool hasUsablePagination(const EpubReaderUtils::Progress& saved) {
  return saved.hasPageCount && saved.pageCount > 0 && saved.pageNumber >= 0 && saved.pageNumber < saved.pageCount &&
         saved.spineIndex >= 0;
}

// The iterator closes its SD handles between steps.
std::unique_ptr<FolderBookIterator> cardBooks;
uint32_t nextPage = 0;
uint32_t lastPageAt = 0;
bool listingExtras = false;

void resetBookList() {
  cardBooks.reset();
  nextPage = 0;
}

void books(JsonVariantConst request, JsonDocument& response, bool extras) {
  if (!request["cursor"].is<uint32_t>()) return fail(response, "invalid_cursor");
  const auto cursor = request["cursor"].as<uint32_t>();
  if (cursor == 0) {
    resetBookList();
    listingExtras = extras;
    cardBooks = makeUniqueNoThrow<FolderBookIterator>("/");
    if (!cardBooks) return fail(response, "low_memory");
  } else if (cursor != nextPage || extras != listingExtras) {
    return fail(response, "invalid_cursor");
  }

  auto entries = response["books"].to<JsonArray>();
  std::string path;
  bool done = false;
  for (unsigned step = 0; step < 32 && entries.size() < 4 && !done; ++step) {
    const auto result = cardBooks->next(path);
    if (result == FolderBookIterator::Result::Error) {
      resetBookList();
      return fail(response, "book_scan_failed");
    }
    done = result == FolderBookIterator::Result::Done;
    if (result != FolderBookIterator::Result::Book || !validPath(path, extras)) continue;
    if (!extras) {
      EpubReaderUtils::Progress progress;
      const auto cache = Epub::resolveCachePathForFilePath(path, "/.crosspoint");
      if (!EpubReaderUtils::loadProgressPath(cache + "/progress.bin", progress, "USBSync") ||
          !hasUsablePagination(progress))
        continue;
    }
    entries.add(path);
  }
  response["next"] = cursor + 1;
  response["done"] = done;
  if (done) {
    resetBookList();
  } else {
    nextPage = cursor + 1;
    lastPageAt = millis();
  }
}

const char* resultName(StatsUploadClient::Result result) {
  using Result = StatsUploadClient::Result;
  switch (result) {
    case Result::Ok:
      return "ok";
    case Result::Skipped:
      return "skipped";
    case Result::Unsupported:
      return "unsupported";
    case Result::Network:
      return "network";
    case Result::Auth:
      return "auth";
    case Result::LowMemory:
      return "low_memory";
    case Result::Server:
      return "server";
    case Result::Cancelled:
      return "cancelled";
    default:
      return "invalid_response";
  }
}
void partResult(JsonDocument& response, const char* part, StatsUploadClient::Result result) {
  response[part]["result"] = resultName(result);
  response[part]["http_code"] = StatsUploadClient::lastHttpCode;
}
void extras(JsonVariantConst request, JsonDocument& response) {
  const char* server = request["server"] | "";
  const char* username = request["username"] | "";
  if (!server[0] || strlen(server) > 2048 || !username[0] || strlen(username) > 255 ||
      strlen(server) != request["server"].as<JsonString>().size() ||
      strlen(username) != request["username"].as<JsonString>().size() || !request["stats"].is<bool>() ||
      !request["clippings"].is<bool>())
    return fail(response, "invalid_account");
  char reader[32];
  if (!KOReaderSyncClient::readerId(reader, sizeof(reader))) return fail(response, "device_id_failed");
  if (!request["device_id"].isNull() && strcmp(reader, request["device_id"] | "") != 0)
    return fail(response, "reader_changed");
  // One ~300-byte relay, reused for this command; keep its response buffer off
  // the loop task stack and avoid allocating any HTTP/TLS client on the reader.
  auto relay = makeUniqueNoThrow<StatsUploadClient::Relay>();
  if (!relay) return fail(response, "low_memory");
  bool interrupted = false;
  relay->server = server;
  relay->username = username;
  relay->context = &interrupted;
  relay->request = UsbSerialFileTransfer::relaySyncHttp;
  response["device_id"] = reader;
  const bool stats = request["stats"].as<bool>();
  // Daily/session acknowledgments consume the reader's shared outbox. A
  // different USB account must not retire data intended for its Wi-Fi account.
  const bool sameAccount = KOREADER_STORE.hasCredentials() && matchesConfiguredAccount(request);
  const bool statsAccount = !KOREADER_STORE.hasCredentials() || sameAccount;
  if (stats && !statsAccount) {
    response["stats"]["result"] = "account_mismatch";
    response["stats"]["http_code"] = 0;
  }
  const bool clippings = request["clippings"].as<bool>();
  if (strcmp(request["kind"] | "", "global") == 0) {
    if (stats && statsAccount) partResult(response, "stats", ReadingSyncUpload::globalStats(nullptr, relay.get()));
    return;
  }
  if (strcmp(request["kind"] | "", "book") != 0) return fail(response, "unknown_operation");
  const std::string path = request["path"] | "";
  if (!validPath(path, true) || !Storage.exists(path.c_str())) return fail(response, "invalid_book");
  const auto method = KOREADER_STORE.getMatchMethod();
  const auto id = method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                          : KOReaderDocumentId::calculate(path);
  if (id.empty()) return fail(response, "document_id_failed");
  uint8_t settled = 0;
  if (stats && statsAccount) {
    const auto result = ReadingSyncUpload::stats(path, id, relay.get());
    partResult(response, "stats", result);
    if (StatsUploadClient::settled(result)) settled |= SyncPending::STATS_BIT;
  }
  if (clippings && FsHelpers::hasEpubExtension(path)) {
    const auto result = ClippingsUpload::sync(
        path, id, [](void* state) { return *static_cast<bool*>(state); }, &interrupted, relay.get());
    partResult(response, "clippings", result);
    if (StatsUploadClient::settled(result)) settled |= SyncPending::CLIPPINGS_BIT;
  }
  if (sameAccount && settled && !SyncPending::clearExisting(path, settled)) response["settlement_error"] = true;
}
}  // namespace

void expireBookList(bool onHome) {
  if (nextPage && (!onHome || millis() - lastPageAt >= 30000)) resetBookList();
}

void handle(JsonVariantConst request, JsonDocument& response, GfxRenderer& renderer) {
  const char* op = request["op"] | "";
  if (strcmp(op, "books") == 0) return books(request, response, false);
  if (strcmp(op, "extras_books") == 0) return books(request, response, true);
  if (strcmp(op, "extras") == 0) return extras(request, response);
  const bool exporting = strcmp(op, "export") == 0;
  const bool comparing = strcmp(op, "compare") == 0;
  if (!exporting && !comparing && strcmp(op, "apply") != 0 && strcmp(op, "settle") != 0)
    return fail(response, "unknown_operation");
  const std::string path = request["path"] | "";
  if (!validPath(path) || !Storage.exists(path.c_str())) return fail(response, "invalid_book");
  EpubReaderUtils::Progress saved;
  const std::string cache = Epub::resolveCachePathForFilePath(path, "/.crosspoint");
  const bool hasSaved = EpubReaderUtils::loadProgressPath(cache + "/progress.bin", saved, "USBSync");
  const bool allowRecovery = request["allow_recovery"] | false;
  if (!hasSaved && !allowRecovery) return fail(response, "no_saved_progress");
  const bool usableSaved = hasSaved && hasUsablePagination(saved);
  // Same matching preference as Wi-Fi sync, without exporting any credentials.
  const auto method = KOREADER_STORE.getMatchMethod();
  const auto id = method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                          : KOReaderDocumentId::calculate(path);
  if (id.empty()) return fail(response, "document_id_failed");
  char reader[32];
  if (!readerId(reader, sizeof(reader))) return fail(response, "device_id_failed");
  const uint32_t snapshot = hasSaved ? revision(saved) : 0;
  if (!exporting &&
      (!request["revision"].is<uint32_t>() || request["revision"].as<uint32_t>() != snapshot ||
       (allowRecovery && (!request["has_saved"].is<bool>() || request["has_saved"].as<bool>() != hasSaved)) ||
       id != (request["document"] | "") || strcmp(reader, request["device_id"] | "") != 0))
    return fail(response, "reader_changed");

  if (strcmp(op, "settle") == 0) {
    if (!usableSaved) return fail(response, "invalid_saved_progress");
    settle(request, response, path, id);
    return;
  }

  if (exporting) {
    response["document"] = id;
    response["revision"] = snapshot;
    response["device_id"] = reader;
    response["match_method"] = method == DocumentMatchMethod::FILENAME ? "filename" : "binary";
    if (allowRecovery) response["has_saved"] = hasSaved;
    if (allowRecovery && !usableSaved) {
      response["local_unavailable"] = true;
      return;
    }
    KOReaderProgress progress{};
    bool localUnavailable = false;
    if (!ReadingSyncUpload::prepareProgress(path, progress, &localUnavailable)) {
      if (allowRecovery && localUnavailable) {
        response["local_unavailable"] = true;
        return;
      }
      return fail(response, "position_mapping_failed");
    }
    if (progress.progress.size() > MAX_LOCATOR_BYTES) return fail(response, "position_mapping_failed");
    response["document"] = progress.document;
    response["progress"] = progress.progress;
    response["percentage"] = progress.percentage;
    response["device"] = progress.device;
    if (progress.position) {
      const auto& rich = *progress.position;
      auto pos = response["position"].to<JsonObject>();
      pos["pctQ"] = rich.pctQ;
      pos["spine"] = rich.spineIndex;
      pos["page"] = rich.pageNumber;
      pos["pages"] = rich.totalPages;
      if (rich.paragraphIndex) pos["para"] = *rich.paragraphIndex;
      if (!rich.xpath.empty() && rich.xpath.size() <= 120) pos["xpath"] = rich.xpath;
    }
    if (progress.metadata) {
      for (auto* text : {&progress.metadata->title, &progress.metadata->authors}) {
        text->resize(utf8SafeTruncateBuffer(text->c_str(), std::min<size_t>(text->size(), 255)));
      }
      auto metadata = response["metadata"].to<JsonObject>();
      metadata["filename"] = progress.metadata->filename;
      metadata["title"] = progress.metadata->title;
      metadata["authors"] = progress.metadata->authors;
    }
    return;
  }

  const auto local = request["local_percentage"];
  if (comparing && !local.isNull() &&
      (!local.is<float>() || !std::isfinite(local.as<float>()) || local.as<float>() < 0 || local.as<float>() > 1))
    return fail(response, "invalid_saved_progress");
  const char* xpath = request["progress"] | "";
  const float percentage = request["percentage"].as<float>();
  if (strlen(xpath) > MAX_LOCATOR_BYTES || !request["percentage"].is<float>() || !std::isfinite(percentage) ||
      percentage < 0 || percentage > 1)
    return fail(response, "invalid_remote_progress");
  if (comparing && usableSaved && !local.isNull() && (request["same_locator"] | false) &&
      KOReaderSyncActivity::sameReadingProgress(local.as<float>(), percentage)) {
    response["choice"] = "match";
    return;
  }

  // Mapping requires shared ownership, as in ReadingSyncUpload. One EPUB lives
  // for this explicit request; its existing loader owns the metadata buffers.
  std::shared_ptr<Epub> epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!epub) return fail(response, "low_memory");
  if (allowRecovery) epub->setupCacheDir();
  if (!epub->load(true, true, Epub::XLocationLoadMode::Immediate, true)) return fail(response, "book_load_failed");
  const bool recovering = !usableSaved || saved.spineIndex >= epub->getSpineItemsCount();
  if (recovering && !allowRecovery) return fail(response, "invalid_saved_progress");

  KOReaderProgress remote{};
  remote.progress = xpath;
  remote.percentage = percentage;
  const auto pos = request["position"].as<JsonObjectConst>();
  if (KOREADER_STORE.supportsRichProgress() && pos["pctQ"].is<uint32_t>() && pos["pctQ"].as<uint32_t>() <= 1000000 &&
      pos["spine"].is<uint16_t>() && pos["page"].is<uint16_t>() && pos["pages"].is<uint16_t>() &&
      pos["pages"].as<uint16_t>() > 0 && pos["page"].as<uint16_t>() < pos["pages"].as<uint16_t>()) {
    auto& rich = remote.position.emplace();
    rich.pctQ = pos["pctQ"].as<uint32_t>();
    rich.spineIndex = pos["spine"].as<uint16_t>();
    rich.pageNumber = pos["page"].as<uint16_t>();
    rich.totalPages = pos["pages"].as<uint16_t>();
    if (pos["para"].is<uint16_t>()) rich.paragraphIndex = pos["para"].as<uint16_t>();
    const char* richXPath = pos["xpath"] | "";
    if (strlen(richXPath) <= 120) rich.xpath = richXPath;
  }
  // Reuse Wi-Fi's mapping and section-cache refinement, including optimized EPUBs.
  // A book without usable local pagination can only be restored from a content coordinate.
  const auto mapped = KOReaderSyncActivity::mapRemotePosition(
      epub, renderer, remote, method, recovering ? 0 : saved.spineIndex, recovering ? 1 : saved.pageCount, recovering);
  if (!mapped.valid || mapped.spineIndex < 0 || mapped.spineIndex >= epub->getSpineItemsCount()) {
    return fail(response, "position_mapping_failed");
  }
  if (comparing) {
    if (recovering || local.isNull()) {
      response["choice"] = "ask";
    } else if (KOReaderSyncActivity::sameReadingProgress(local.as<float>(), remote.percentage)) {
      response["choice"] = "match";
    } else if (KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART) {
      response["choice"] = local.as<float>() > remote.percentage ? "reader" : "server";
    } else {
      response["choice"] = "ask";
    }
    return;
  }
  const int pages = std::max(mapped.totalPages, mapped.pageNumber + 1);
  const auto offset = mapped.hasVisibleTextOffset ? std::optional<uint32_t>(mapped.visibleTextOffset) : std::nullopt;
  if (!EpubReaderUtils::saveProgress(*epub, mapped.spineIndex, mapped.pageNumber, pages, offset)) {
    return fail(response, "save_failed");
  }
  RecentBookProgress::saveCachedEpubPercent(*epub, mapped.spineIndex, mapped.pageNumber, pages);
  response["applied"] = true;
  settle(request, response, path, id);
}
}  // namespace UsbProgressSync
