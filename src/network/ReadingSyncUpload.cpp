#include "ReadingSyncUpload.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <I18n.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <Memory.h>
#include <ProgressMapper.h>
#include <Xtc.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "ClippingsUpload.h"
#include "CrossPointSettings.h"
#include "activities/reader/BookStatsTracking.h"
#include "activities/reader/DailyReadingStats.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/ReadingSessionJournal.h"
#include "activities/reader/StatsUploadPayload.h"
#include "sync/BookSyncFile.h"

bool ReadingSyncUpload::prepareProgress(const std::string& path, KOReaderProgress& progress) {
  if (!FsHelpers::hasEpubExtension(path)) return false;
  // ProgressMapper requires shared ownership. Release the whole EPUB before TLS.
  std::shared_ptr<Epub> epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!epub) {
    LOG_ERR("ReadingSync", "Cannot allocate EPUB for saved progress");
    return false;
  }
  EpubReaderUtils::Progress saved;
  if (!EpubReaderUtils::loadProgress(*epub, saved) ||
      !epub->load(false, true, Epub::XLocationLoadMode::Immediate, true) || saved.spineIndex < 0 ||
      saved.spineIndex >= epub->getSpineItemsCount() || !saved.hasPageCount || saved.pageCount < 1 ||
      saved.pageNumber >= saved.pageCount) {
    LOG_DBG("ReadingSync", "Skipping book without usable saved progress: %s", path.c_str());
    return false;
  }
  CrossPointPosition position{saved.spineIndex, saved.pageNumber, saved.pageCount};
  position.visibleTextOffset = saved.visibleTextOffset;
  position.hasVisibleTextOffset = saved.hasVisibleTextOffset;
  const auto method = KOREADER_STORE.getMatchMethod();
  const auto mapped =
      ProgressMapper::toKOReader(epub, position,
                                 method == DocumentMatchMethod::FILENAME ? PositionCoordinateSpace::SourceDocument
                                                                         : PositionCoordinateSpace::CurrentDocument);
  if (!mapped.valid || mapped.xpath.empty() || !std::isfinite(mapped.percentage) || mapped.percentage < 0 ||
      mapped.percentage > 1)
    return false;
  progress = {};
  progress.document = BookSyncFile::documentId(path);
  if (progress.document.empty()) return false;
  progress.progress = mapped.xpath;
  progress.percentage = mapped.percentage;
  progress.device = SETTINGS.getEffectiveDeviceName();
  if (KOREADER_STORE.getSendMetadata()) {
    progress.metadata.emplace();
    progress.metadata->filename = path.substr(path.find_last_of('/') + 1);
    progress.metadata->title = epub->getTitle();
    progress.metadata->authors = epub->getAuthor();
    progress.metadata->loadExtended(*epub);
  }
  if (KOREADER_STORE.supportsRichProgress()) {
    KOReaderRichPosition rich;
    rich.pctQ = static_cast<uint32_t>(mapped.percentage * 1000000.0f + 0.5f);
    rich.spineIndex = saved.spineIndex;
    rich.pageNumber = saved.pageNumber;
    rich.totalPages = saved.pageCount;
    rich.xpath = mapped.xpath;
    progress.position = std::move(rich);
  }
  return true;
}

StatsUploadClient::Result ReadingSyncUpload::globalStats() {
  using Result = StatsUploadClient::Result;
  if (!SETTINGS.shouldTrackReadingStats()) return Result::Skipped;
  // Existing fixed 1536-byte upload buffer stays off the small task stack.
  auto payload = makeUniqueNoThrow<char[]>(StatsUploadPayload::CAPACITY);
  char deviceId[32];
  if (!payload || !StatsUploadClient::deviceId(deviceId, sizeof(deviceId))) return Result::LowMemory;
  StatsUploadClient client;
  return globalStats(client, payload.get(), StatsUploadPayload::CAPACITY, deviceId);
}

StatsUploadClient::Result ReadingSyncUpload::globalStats(StatsUploadClient& client, char* payload, size_t capacity,
                                                         const char* deviceId) {
  using Result = StatsUploadClient::Result;
  if (!SETTINGS.shouldTrackReadingStats()) return Result::Skipped;
  GlobalReadingStats global;
  if (!GlobalReadingStats::loadForUpload(global)) return client.downloadStreak();
  if (!StatsUploadPayload::global(payload, capacity, deviceId, global)) return Result::InvalidResponse;
  auto result = client.put("/api/v1/stats/global", payload, false);
  if (result != Result::Ok) return result;
  struct UploadContext {
    StatsUploadClient& client;
    char* payload;
    size_t capacity;
    const char* deviceId;
    const GlobalReadingStats& global;
    Result result = Result::Ok;
  } context{client, payload, capacity, deviceId, global};
  const bool complete = DailyReadingStats::uploadPending(
      [](void* opaque, uint32_t day, uint32_t seconds) {
        auto& ctx = *static_cast<UploadContext*>(opaque);
        if (!StatsUploadPayload::global(ctx.payload, ctx.capacity, ctx.deviceId, ctx.global, day, seconds)) {
          ctx.result = Result::InvalidResponse;
          return false;
        }
        ctx.result = ctx.client.put("/api/v1/stats/global", ctx.payload, false, true);
        return ctx.result == Result::Ok;
      },
      &context);
  result = context.result;
  if (!complete && result == Result::Ok) result = Result::InvalidResponse;
  if (!complete) LOG_ERR("ReadingSync", "Daily counter upload incomplete");
  if (complete) {
    ReadingStatsDateTime local;
    if (getCurrentLocalDailyReadingDateTime(local) && !DailyReadingStats::pruneSyncedMonths(local.date))
      LOG_ERR("ReadingSync", "Synced monthly history cleanup deferred");
  }
  if (result != Result::Ok) return result;
  const bool sessionsComplete = ReadingSessionJournal::uploadPending(
      [](void* opaque, const ReadingSessionJournal::Record& record) {
        auto& ctx = *static_cast<UploadContext*>(opaque);
        const int n =
            snprintf(ctx.payload, ctx.capacity,
                     "{\"device_id\":\"%s\",\"items\":[{\"session_id\":\"%s\",\"document\":\"%s\","
                     "\"started_at\":%llu,\"ended_at\":%llu,\"seconds\":%lu,\"utc_offset_minutes\":%d}]}",
                     ctx.deviceId, record.id, record.document, static_cast<unsigned long long>(record.startedAt),
                     static_cast<unsigned long long>(record.endedAt), static_cast<unsigned long>(record.seconds),
                     int(record.utcOffsetMinutes));
        if (n < 0 || size_t(n) >= ctx.capacity) {
          ctx.result = Result::InvalidResponse;
          return false;
        }
        ctx.result = ctx.client.put("/api/v1/stats/sessions", ctx.payload, false, false, record.id);
        return ctx.result == Result::Ok;
      },
      &context);
  if (!sessionsComplete && context.result == Result::Ok) return Result::InvalidResponse;
  if (context.result == Result::Ok) {
    const auto downloaded = client.downloadStreak();
    if (StatsUploadClient::failed(downloaded)) return downloaded;
  }
  return context.result;
}

bool ReadingSyncUpload::loadBookStats(const std::string& path, BookReadingStats& stats) {
  if (!SETTINGS.shouldTrackReadingStats()) return false;
  // XTC books keep stats in their own cache folder; they have no KOReader position.
  const auto cache = FsHelpers::hasXtcExtension(path) ? Xtc(path, "/.crosspoint").getCachePath()
                                                      : Epub::resolveCachePathForFilePath(path, "/.crosspoint");
  return BookStatsTracking::isEnabled(cache) && BookReadingStats::loadForUpload(cache, stats);
}

bool ReadingSyncUpload::canSyncReadingStats() {
  return KOREADER_STORE.hasCredentials() && KOREADER_STORE.getServerSupport() == SyncServerSupport::SUPPORTED &&
         KOREADER_STORE.getSyncStats() && SETTINGS.shouldTrackReadingStats();
}

bool ReadingSyncUpload::canSyncClippings() {
  return KOREADER_STORE.hasCredentials() && KOREADER_STORE.getServerSupport() == SyncServerSupport::SUPPORTED &&
         KOREADER_STORE.getSyncClippings();
}

StatsUploadClient::Result ReadingSyncUpload::stats(const std::string& path, const std::string& document) {
  using Result = StatsUploadClient::Result;
  BookReadingStats book;
  if (!loadBookStats(path, book)) return Result::Skipped;
  auto payload = makeUniqueNoThrow<char[]>(StatsUploadPayload::CAPACITY);
  char deviceId[32];
  if (!payload || !StatsUploadClient::deviceId(deviceId, sizeof(deviceId))) return Result::LowMemory;
  if (!StatsUploadPayload::book(payload.get(), StatsUploadPayload::CAPACITY, deviceId, document.c_str(), book))
    return Result::InvalidResponse;
  StatsUploadClient client;
  return client.put("/api/v1/stats/books", payload.get(), true);
}

ReadingSyncUpload::ExtrasResult ReadingSyncUpload::extras(const std::string& path, const std::string& document) {
  ExtrasResult result;
  if (KOREADER_STORE.getSyncStats()) result.stats = stats(path, document);
  // These independent snapshots must still be attempted after a stats failure.
  // Clippings are EPUB-only; XTC books never save any.
  if (KOREADER_STORE.getSyncClippings() && FsHelpers::hasEpubExtension(path))
    result.clippings = ClippingsUpload::upload(path, document);
  LOG_INF("ReadingSync", "Extras: stats=%d clippings=%d", static_cast<int>(result.stats),
          static_cast<int>(result.clippings));
  return result;
}

namespace {
const char* unavailableLabel(const bool clippings) {
  if (KOREADER_STORE.getServerSupport() == SyncServerSupport::UNSUPPORTED) return tr(STR_NOT_SUPPORTED);
  const bool enabled = clippings ? KOREADER_STORE.getSyncClippings() : KOREADER_STORE.getSyncStats();
  return enabled ? nullptr : tr(STR_OFF);
}
}  // namespace

const char* ReadingSyncUpload::statusLabel(const StatsUploadClient::Result result, const bool clippings) {
  const char* unavailable = unavailableLabel(clippings);
  return unavailable ? unavailable : StatsUploadClient::resultString(result);
}

const char* ReadingSyncUpload::countLabel(char* buffer, const size_t capacity, const uint32_t ok, const uint32_t failed,
                                          const bool clippings) {
  const char* unavailable = unavailableLabel(clippings);
  if (unavailable) return unavailable;
  if (failed == 0)
    snprintf(buffer, capacity, "%u", static_cast<unsigned>(ok));
  else
    snprintf(buffer, capacity, "%u (%u %s)", static_cast<unsigned>(ok), static_cast<unsigned>(failed),
             tr(STR_FAILED_LOWER));
  return buffer;
}

const char* ReadingSyncUpload::failureHeading(const bool progress, const bool stats, const bool clippings) {
  if (progress + stats + clippings != 1) return tr(STR_SYNC_FAILED_MSG);
  return progress ? tr(STR_PROGRESS_SYNC_FAILED) : stats ? tr(STR_STATS_SYNC_FAILED) : tr(STR_CLIPPINGS_SYNC_FAILED);
}

std::string ReadingSyncUpload::displayName(const std::string& bookPath) {
  const size_t start = bookPath.find_last_of('/') + 1;
  const size_t dot = bookPath.find_last_of('.');
  return bookPath.substr(start, dot != std::string::npos && dot > start ? dot - start : std::string::npos);
}
