#include "SleepBookSync.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>

#include "BookmarkSync.h"
#include "ClippingsUpload.h"
#include "ReadingSyncUpload.h"
#include "SleepProgressPolicy.h"
#include "sync/BookSyncFile.h"

namespace {
constexpr uint32_t SYNC_BUDGET_MS = 15000;
constexpr size_t SESSIONS_PER_SLEEP = 4;

bool budgetExpired(void* const context) {
  const auto started = *static_cast<const uint32_t*>(context);
  return static_cast<uint32_t>(millis() - started) >= SYNC_BUDGET_MS;
}

void syncProgress(const KOReaderProgress& local, uint32_t& started) {
  if (budgetExpired(&started)) return;
  KOReaderProgress remote{};
  const auto result = KOReaderSyncClient::getProgress(local.document, remote, budgetExpired, &started);
  using Remote = SleepProgressPolicy::Remote;
  const auto remoteState = result == KOReaderSyncClient::OK          ? Remote::Present
                           : result == KOReaderSyncClient::NOT_FOUND ? Remote::Absent
                                                                     : Remote::Unavailable;
  if (!SleepProgressPolicy::mayUpload(local.percentage, remoteState, remote.percentage) || budgetExpired(&started)) {
    LOG_INF("SleepSync", "Position left unchanged (read result=%d)", static_cast<int>(result));
    return;
  }
  const auto uploaded = KOReaderSyncClient::updateProgress(local, budgetExpired, &started);
  LOG_INF("SleepSync", "Position result=%d", static_cast<int>(uploaded));
}
}  // namespace

void SleepBookSync::run(const std::string& path, const bool progressSafe, bool (*ensureConnected)()) {
  if (!ensureConnected || !KOREADER_STORE.getSyncOnSleep() || !KOREADER_STORE.hasCredentials() ||
      !FsHelpers::hasEpubExtension(path)) {
    return;
  }
  uint32_t started = millis();
  const bool bookmarks =
      KOREADER_STORE.getSyncBookmarks() && KOREADER_STORE.getServerType() != KOReaderServerType::KOSYNC;
  const bool clippings = ReadingSyncUpload::canSyncClippings();
  const bool sessions = ReadingSyncUpload::canSyncReadingStats();
  KOReaderProgress local{};
  const bool havePosition = progressSafe && ReadingSyncUpload::prepareProgress(path, local, nullptr, true);
  if (!havePosition && !bookmarks && !clippings && !sessions) return;
  const std::string document = havePosition             ? local.document
                               : bookmarks || clippings ? BookSyncFile::documentId(path)
                                                        : std::string{};
  if (!havePosition && document.empty() && !sessions) return;
  if (budgetExpired(&started) || !ensureConnected() || budgetExpired(&started)) return;
  // Do not gate healthy progress on any optional stream or use stale local
  // progress if the last save failed. Each enabled stream can try independently.
  if (havePosition) {
    syncProgress(local, started);
    local = {};
  }
  if (bookmarks && !document.empty() && !budgetExpired(&started)) {
    const auto result = BookmarkSync::run(path, document, budgetExpired, &started);
    LOG_INF("SleepSync", "Bookmarks result=%d", static_cast<int>(result));
  }
  if (clippings && !document.empty() && !budgetExpired(&started)) {
    const auto result =
        ClippingsUpload::sync(path, document, budgetExpired, &started, nullptr, StatsUploadClient::SLEEP_TIMEOUT_MS);
    LOG_INF("SleepSync", "Clippings result=%d", static_cast<int>(result));
  }
  if (sessions && !budgetExpired(&started)) {
    const auto result = ReadingSyncUpload::sessions(SESSIONS_PER_SLEEP, budgetExpired, &started);
    LOG_INF("SleepSync", "Sessions result=%d", static_cast<int>(result));
  }
}
