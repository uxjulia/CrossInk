#include "StatsUploadActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <I18n.h>
#include <KOReaderCredentialStore.h>
#include <Memory.h>
#include <TrustedTime.h>
#include <WiFi.h>
#include <Xtc.h>

#include <cstdio>
#include <cstring>

#include "ClippingStore.h"
#include "CrossPointState.h"
#include "HalClock.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/BookStatsTracking.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/KOReaderSyncActivity.h"
#include "components/TouchActionButtons.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/ClippingsUpload.h"
#include "network/ReadingSyncUpload.h"
#include "network/WifiUtils.h"
#include "sync/BookSyncFile.h"
#include "sync/SyncIds.h"
#include "sync/SyncParked.h"
#include "sync/SyncState.h"
#include "util/InputReleaseGuard.h"

namespace {
// Fits about a dozen books per stats request; allocated only for bulk syncs.
constexpr size_t STATS_BATCH_CAPACITY = 4096;
// Changes per feed page: each parsed change stays in RAM until its page is
// applied, so keep pages small on the C3.
constexpr uint8_t PAGE_LIMIT = 10;

std::string documentIdFor(const std::string& bookPath) { return BookSyncFile::documentId(bookPath); }
}  // namespace

void StatsUploadActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseForNetwork(renderer);
  initialConfirm = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  if (!KOREADER_STORE.hasCredentials()) fail(tr(STR_SYNC_SERVER_SETUP_FIRST));
  requestUpdate();
}

void StatsUploadActivity::closeTransfer() {
  folderBooks.reset();
  // Unsent positions keep their pending bits for the next sync.
  std::vector<KOReaderProgress>().swap(progressBatch);
  std::vector<std::string>().swap(progressBatchPaths);
  batch = StatsUploadPayload::BookBatch{nullptr, 0};
  batchBuffer.reset();
  batchPaths.clear();
  pending.end();
  if (ownsWifi) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    ownsWifi = false;
  }
}

void StatsUploadActivity::onExit() {
  closeTransfer();
  Activity::onExit();
#ifndef SIMULATOR
  // Started from File Transfer in the minimal network boot: every way out
  // (Back, Done, the Home gesture, shortcuts) reboots into the normal boot so
  // reader resources come back, as OpdsBookBrowserActivity does.
  if (restartOnLeave) {
    if (returnBookPath.empty())
      silentRestart();
    else
      silentRestartToReader();
  }
#endif
}

void StatsUploadActivity::leave() {
  // A single book was synced from its Library or File Browser menu: go back there,
  // keeping the browsing position, the same way EPUB Sync Book returns.
  if (scope == Scope::Book && returnTo.valid() &&
      (returnTo.origin == PendingOverlayOrigin::FileBrowser || returnTo.origin == PendingOverlayOrigin::Library)) {
    // The list restores its folder and selection from this on entry.
    const std::string folderPath = returnTo.fileBrowserPath;
    const bool fileBrowser = returnTo.origin == PendingOverlayOrigin::FileBrowser;
    APP_STATE.setPendingOverlayResume(std::move(returnTo));
    if (fileBrowser)
      activityManager.goToFileBrowser(folderPath);
    else
      activityManager.goToLibrary();
    return;
  }
  if (!restartOnLeave && !returnBookPath.empty() && Storage.exists(returnBookPath.c_str())) {
    // The EPUB reader reopens the starting screen too; other readers resume the page.
    if (runReturnMenuAction && FsHelpers::hasEpubExtension(returnBookPath))
      activityManager.goToReaderAndRunMenuAction(std::move(returnBookPath), returnMenuAction);
    else
      activityManager.goToReader(std::move(returnBookPath), true);
    return;
  }
  if (fromFileTransfer) {
    activityManager.goHome(HomeMenuItem::FILE_TRANSFER);
    return;
  }
  finish();
}

const char* StatsUploadActivity::title() const {
  switch (scope) {
    case Scope::Library:
      return tr(STR_SYNC_BOOKS);
    case Scope::Folder:
      return tr(STR_FOLDER_SYNC);
    case Scope::Book:
      return tr(STR_SYNC_BOOK);
    case Scope::ReadingStats:
      return tr(STR_SYNC_READING_STATS);
    case Scope::Clippings:
      return tr(STR_SYNC_CLIPPINGS);
  }
  return tr(STR_SYNC);
}

void StatsUploadActivity::fail(const char* text) {
  LOG_ERR("StatsSync", "Upload stopped: %s", text);
  {
    RenderLock lock(*this);
    message = text;
    state = State::Failed;
  }
  closeTransfer();
  requestUpdate();
}

void StatsUploadActivity::failBook(const std::string& bookPath, const uint8_t failedParts, std::string reason,
                                   std::string title) {
  LOG_ERR("StatsSync", "Book sync failed (parts 0x%x): %s %s", failedParts, bookPath.c_str(), reason.c_str());
  RenderLock lock(*this);
  ++failed;
  // Reserved at start; past the cap the book is only counted.
  if (failures.size() >= MAX_FAILURE_DETAILS) return;
  failures.push_back(
      {title.empty() ? ReadingSyncUpload::displayName(bookPath) : std::move(title), failedParts, std::move(reason)});
}

void StatsUploadActivity::setCurrentBook(const std::string& bookPath) {
  RenderLock lock(*this);
  currentBook = ReadingSyncUpload::displayName(bookPath);
}

uint32_t StatsUploadActivity::countFolderBooks(const std::string& root) {
  // File names only: no book is opened, so this is one pass over the folders.
  auto books = makeUniqueNoThrow<FolderBookIterator>(root);
  if (!books) return 0;
  uint32_t total = 0;
  std::string bookPath;
  for (;;) {
    const auto step = books->next(bookPath);
    if (step == FolderBookIterator::Result::Book)
      ++total;
    else if (step != FolderBookIterator::Result::Entry)
      break;
  }
  return total;
}

uint32_t StatsUploadActivity::countPendingInScope() const {
  return SyncPending::count([](const std::string& bookPath,
                               void* context) { return static_cast<const SyncScope*>(context)->covers(bookPath); },
                            const_cast<SyncScope*>(&syncScope));
}

bool StatsUploadActivity::startTargeted(const bool stats) {
  SyncState syncState = SyncState::load();
  if (stats ? syncState.statsCaughtUp : syncState.clippingsCaughtUp) {
    phase = Phase::Pending;
    return pending.begin();
  }
  // Data saved before the pending list existed, or while this toggle was off,
  // was never marked: send everything once. Books this catch-up already sent
  // carry its id, so an interrupted run resumes where it stopped.
  uint32_t& id = stats ? syncState.statsCatchupId : syncState.clippingsCatchupId;
  if (!id) {
    id = syncState.newStampId();
    if (!syncState.save()) return false;
  }
  catchupStamp = id;
  phase = Phase::CatchUp;
  LOG_INF("StatsSync", "%s catch-up %lu", stats ? "Stats" : "Clippings", static_cast<unsigned long>(id));
  if (!stats) return true;  // the clippings folder is listed by nextBook()
  // Stats live in each book's path-hashed cache folder, which does not record
  // its book, so walk the card and check each book for a stats file.
  folderBooks = makeUniqueNoThrow<FolderBookIterator>("/");
  return folderBooks != nullptr;
}

void StatsUploadActivity::start() {
  // Created before any catch-up can be recorded as done (see SyncPending::ensureExists).
  SyncPending::ensureExists();
  // Small and bounded, reserved now so a failure late in a low-memory run
  // records without growing the vectors.
  failures.reserve(MAX_FAILURE_DETAILS);
  if (scope == Scope::Folder || scope == Scope::Library) deferredBooks.reserve(4);
  if (scope == Scope::ReadingStats && KOREADER_STORE.getSyncStats()) {
    batchBuffer = makeUniqueNoThrow<char[]>(STATS_BATCH_CAPACITY);
    if (!batchBuffer) {
      fail(tr(STR_KOREADER_SYNC_LOW_MEMORY));
      return;
    }
    batchPaths.reserve(StatsUploadPayload::BookBatch::MAX_ITEMS);
    batch = StatsUploadPayload::BookBatch{batchBuffer.get(), STATS_BATCH_CAPACITY};
    if (!StatsUploadClient::deviceId(deviceId, sizeof(deviceId)) || !batch.begin(deviceId) || !startTargeted(true)) {
      fail(tr(STR_SYNC_FAILED_MSG));
      return;
    }
  } else if (scope == Scope::Clippings && KOREADER_STORE.getSyncClippings()) {
    if (!startTargeted(false)) {
      fail(tr(STR_SYNC_FAILED_MSG));
      return;
    }
  } else if (scope == Scope::Folder || scope == Scope::Library) {
    // Planned once connected (reading a scope's starting cursor is a request).
    // Stats batching is shared by scope catch-ups and the push.
    batchBuffer = makeUniqueNoThrow<char[]>(STATS_BATCH_CAPACITY);
    if (!batchBuffer) {
      fail(tr(STR_KOREADER_SYNC_LOW_MEMORY));
      return;
    }
    batchPaths.reserve(StatsUploadPayload::BookBatch::MAX_ITEMS);
    batch = StatsUploadPayload::BookBatch{batchBuffer.get(), STATS_BATCH_CAPACITY};
    if (!StatsUploadClient::deviceId(deviceId, sizeof(deviceId)) || !batch.begin(deviceId)) {
      fail(tr(STR_SYNC_FAILED_MSG));
      return;
    }
    flow = Flow::Unplanned;
  }
  if (!hasActiveStationWifiConnection()) {
    ownsWifi = true;
    {
      RenderLock lock(*this);
      state = State::Wifi;
    }
    auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
    if (!wifi) {
      fail(tr(STR_KOREADER_SYNC_LOW_MEMORY));
      return;
    }
    startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
      if (result.isCancelled) {
        leave();
        return;
      }
      if (!hasActiveStationWifiConnection()) {
        fail(tr(STR_CONNECTION_FAILED));
        return;
      }
      {
        RenderLock lock(*this);
        state = State::Uploading;
      }
      requestUpdate();
    });
  } else {
    {
      RenderLock lock(*this);
      state = State::Uploading;
    }
    requestUpdate();
  }
}

void StatsUploadActivity::finishUpload() {
  if (!reviewStarted && !deferredBooks.empty()) {
    // Every book is done; now ask about the kept conflicts (connection still up).
    reviewStarted = true;
    LOG_INF("StatsSync", "Reviewing %u kept conflict(s)", static_cast<unsigned>(deferredBooks.size()));
    reviewNext();
    return;
  }
  LOG_INF("StatsSync", "Finished: synced=%u skipped=%u failed=%u", static_cast<unsigned>(uploaded),
          static_cast<unsigned>(skipped), static_cast<unsigned>(failed));
  LOG_INF("StatsSync", "Extras: global=%d stats=%u statsFailed=%u clippings=%u clippingsFailed=%u",
          static_cast<int>(globalResult), static_cast<unsigned>(statsUploaded), static_cast<unsigned>(statsFailed),
          static_cast<unsigned>(clippingsUploaded), static_cast<unsigned>(clippingsFailed));
  if (scope == Scope::Library && flow == Flow::Legacy) {
    // A completed whole-card walk sent every book's stats and clippings for
    // each toggle that was on, so their one-time catch-ups are not needed.
    // A failed book may never have attempted its extras, and an
    // "Unsupported" answer sent nothing: neither counts as caught up.
    SyncState syncState = SyncState::load();
    const bool complete = failed == 0 && KOREADER_STORE.getServerSupport() == SyncServerSupport::SUPPORTED;
    const bool stats = complete && KOREADER_STORE.getSyncStats() && statsFailed == 0;
    const bool clippings = complete && KOREADER_STORE.getSyncClippings() && clippingsFailed == 0;
    if (stats) {
      syncState.statsCaughtUp = true;
      syncState.statsCatchupId = 0;
    }
    if (clippings) {
      syncState.clippingsCaughtUp = true;
      syncState.clippingsCatchupId = 0;
    }
    if (stats || clippings) syncState.save();
  }
  closeTransfer();
  {
    RenderLock lock(*this);
    state = State::Done;
    message = tr(STR_DONE);
  }
  requestUpdate();
}

void StatsUploadActivity::finishCatchup() {
  const bool stats = scope == Scope::ReadingStats;
  if (stats) flushBookStats();
  // Books whose upload failed are not stamped; leaving the catch-up open lets
  // the next run retry just those. An "Unsupported" answer marks the server
  // progress-only and sent nothing, so that is not done either.
  if ((stats ? statsFailed : clippingsFailed) == 0 &&
      KOREADER_STORE.getServerSupport() == SyncServerSupport::SUPPORTED) {
    SyncState syncState = SyncState::load();
    if (stats) {
      syncState.statsCaughtUp = true;
      syncState.statsCatchupId = 0;
    } else {
      syncState.clippingsCaughtUp = true;
      syncState.clippingsCatchupId = 0;
    }
    syncState.save();
    // The whole card covers every prepared folder too.
    SyncState::markScopesCaughtUp(stats);
    LOG_INF("StatsSync", "%s catch-up complete", stats ? "Stats" : "Clippings");
  }
  folderBooks.reset();
  catchupStamp = 0;
  phase = pending.begin() ? Phase::Pending : Phase::Done;
}

std::string StatsUploadActivity::scopePath() const {
  if (scope == Scope::Library) return "/";
  std::string folder = path;
  while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
  return folder.empty() ? std::string("/") : folder;
}

void StatsUploadActivity::startLegacyWalk() {
  // Sync Books walks the card itself, so books never shown in the Library
  // (File Browser-only users, books copied since the last refresh) are included.
  flow = Flow::Legacy;
  progressDone = 0;
  progressTotal = countFolderBooks(scopePath());
  folderBooks = makeUniqueNoThrow<FolderBookIterator>(scopePath());
  if (!folderBooks) fail(tr(STR_KOREADER_SYNC_LOW_MEMORY));
}

void StatsUploadActivity::planIncremental() {
  const std::string scopeRoot = scopePath();
  // Only CrossPoint Sync has the change feed; stock KOSync keeps the walk.
  if (KOREADER_STORE.getServerSupport() != SyncServerSupport::SUPPORTED ||
      scopeRoot.size() > SyncScope::MAX_PATH_LENGTH) {
    startLegacyWalk();
    return;
  }
  SyncScope stored;
  const bool known = SyncState::findScope(scopeRoot, stored);
  if (known && stored.prepared) {
    syncScope = stored;
  } else {
    SyncScope ancestor;
    if (SyncState::findPreparedCovering(scopeRoot, ancestor) && ancestor.path != scopeRoot) {
      // A folder inside a prepared scope needs no walk: it starts from a copy of
      // that scope's cursor, and the ancestor's catch-ups covered it.
      syncScope = SyncScope{};
      syncScope.path = scopeRoot;
      syncScope.prepared = true;
      syncScope.cursor = ancestor.cursor;
      syncScope.statsCaughtUp = ancestor.statsCaughtUp;
      syncScope.clippingsCaughtUp = ancestor.clippingsCaughtUp;
      SyncState::putScope(syncScope);
    } else {
      // First sync of this scope: walk it once. Read the feed cursor before the
      // walk, so positions changed during it are replayed later (harmless);
      // reading it afterwards would skip them. A resumed walk keeps its cursor.
      syncScope = known ? stored : SyncScope{};
      syncScope.path = scopeRoot;
      walkResumed = syncScope.walkId != 0;
      if (!syncScope.walkId) {
        KOReaderSyncClient::ChangesPage page;
        const auto result = KOReaderSyncClient::getProgressChanges(0, 0, page, nullptr, nullptr);
        if (result == KOReaderSyncClient::FEED_UNSUPPORTED || result == KOReaderSyncClient::SERVER_ERROR ||
            result == KOReaderSyncClient::JSON_ERROR) {
          LOG_INF("StatsSync", "No change feed (%d); using the per-book walk", static_cast<int>(result));
          startLegacyWalk();
          return;
        }
        if (result != KOReaderSyncClient::OK) {
          fail(KOReaderSyncClient::errorString(result).c_str());
          return;
        }
        SyncState syncState = SyncState::load();
        syncScope.walkId = syncState.newStampId();
        syncScope.cursor = page.cursor;
        if (!syncState.save() || !SyncState::putScope(syncScope)) {
          fail(tr(STR_SYNC_FAILED_MSG));
          return;
        }
      }
      LOG_INF("StatsSync", "First walk of %s (walk %lu, cursor %lu)", scopeRoot.c_str(),
              static_cast<unsigned long>(syncScope.walkId), static_cast<unsigned long>(syncScope.cursor));
      flow = Flow::FirstWalk;
      progressDone = 0;
      progressTotal = countFolderBooks(scopeRoot);
      folderBooks = makeUniqueNoThrow<FolderBookIterator>(scopeRoot);
      if (!folderBooks) fail(tr(STR_KOREADER_SYNC_LOW_MEMORY));
      requestUpdate();
      return;
    }
  }
  flow = Flow::Unplanned;  // advanceFlow() starts from the first prepared step
  advanceFlow();
}

void StatsUploadActivity::advanceFlow() {
  folderBooks.reset();
  catchupStamp = 0;
  progressDone = progressTotal = 0;
  currentBook.clear();
  const SyncState wholeCard = SyncState::load();
  const bool statsDone = syncScope.statsCaughtUp || wholeCard.statsCaughtUp;
  const bool clippingsDone = syncScope.clippingsCaughtUp || wholeCard.clippingsCaughtUp;
  // Steps in order; each one that has nothing to do is skipped.
  if (flow < Flow::CatchUpStats && KOREADER_STORE.getSyncStats() && !statsDone) {
    if (!syncScope.statsCatchupId) {
      SyncState syncState = SyncState::load();
      syncScope.statsCatchupId = syncState.newStampId();
      syncState.save();
      SyncState::putScope(syncScope);
    }
    flow = Flow::CatchUpStats;
    phase = Phase::CatchUp;
    catchupStamp = syncScope.statsCatchupId;
    folderBooks = makeUniqueNoThrow<FolderBookIterator>(syncScope.path);
    if (!folderBooks) fail(tr(STR_KOREADER_SYNC_LOW_MEMORY));
  } else if (flow < Flow::CatchUpClippings && KOREADER_STORE.getSyncClippings() && !clippingsDone) {
    if (!syncScope.clippingsCatchupId) {
      SyncState syncState = SyncState::load();
      syncScope.clippingsCatchupId = syncState.newStampId();
      syncState.save();
      SyncState::putScope(syncScope);
    }
    flow = Flow::CatchUpClippings;
    phase = Phase::CatchUp;
    catchupStamp = syncScope.clippingsCatchupId;
    clippingsPosition = 0;
  } else if (flow < Flow::Pull) {
    flow = Flow::Pull;
    phase = Phase::Pending;
    pageLoaded = false;
    // One block for the whole pull (about 3 KB), instead of regrowing per page
    // while the response and TLS buffers are alive.
    pageChanges.reserve(PAGE_LIMIT);
  } else if (flow < Flow::Push) {
    flow = Flow::Push;
    phase = Phase::Pending;
    progressTotal = countPendingInScope();
    if (!pending.begin()) fail(tr(STR_SYNC_FAILED_MSG));
  } else {
    flushBookStats();
    flow = Flow::Done;
  }
  requestUpdate();
}

void StatsUploadActivity::finishScopeCatchup(const bool stats) {
  if (stats) flushBookStats();
  if ((stats ? statsFailed : clippingsFailed) == 0 &&
      KOREADER_STORE.getServerSupport() == SyncServerSupport::SUPPORTED) {
    if (stats) {
      syncScope.statsCaughtUp = true;
      syncScope.statsCatchupId = 0;
    } else {
      syncScope.clippingsCaughtUp = true;
      syncScope.clippingsCatchupId = 0;
    }
    SyncState::putScope(syncScope);
    if (syncScope.path == "/") {
      // The whole card's catch-up covers every prepared folder too.
      SyncState syncState = SyncState::load();
      (stats ? syncState.statsCaughtUp : syncState.clippingsCaughtUp) = true;
      (stats ? syncState.statsCatchupId : syncState.clippingsCatchupId) = 0;
      syncState.save();
      SyncState::markScopesCaughtUp(stats);
    }
    LOG_INF("StatsSync", "%s catch-up of %s complete", stats ? "Stats" : "Clippings", syncScope.path.c_str());
  }
  advanceFlow();
}

void StatsUploadActivity::finishFirstWalk() {
  // A completed walk reconciled progress, stats and clippings for its books:
  // the scope is prepared, and each toggle that was on counts as caught up
  // unless an upload failed (or the server stopped accepting it).
  if (failed > 0 && !walkResumed) {
    // A failed book may never have got its remote position, and the feed
    // cursor (read before the walk) won't send it again. Keep the walk open
    // once: the next run skips the books this one finished and retries only
    // the rest. A book that fails again (corrupt, needs re-optimizing) must
    // not hold the scope back forever; one whose remote position was fetched
    // has it parked, so the push still compares instead of overwriting.
    LOG_INF("StatsSync", "First walk of %s left %u book(s) unsynced; resuming next time", syncScope.path.c_str(),
            static_cast<unsigned>(failed));
    return;
  }
  const bool complete = failed == 0 && KOREADER_STORE.getServerSupport() == SyncServerSupport::SUPPORTED;
  const bool stats = complete && KOREADER_STORE.getSyncStats() && statsFailed == 0;
  const bool clippings = complete && KOREADER_STORE.getSyncClippings() && clippingsFailed == 0;
  syncScope.prepared = true;
  syncScope.walkId = 0;
  if (stats) {
    syncScope.statsCaughtUp = true;
    syncScope.statsCatchupId = 0;
  }
  if (clippings) {
    syncScope.clippingsCaughtUp = true;
    syncScope.clippingsCatchupId = 0;
  }
  SyncState::putScope(syncScope);
  if (syncScope.path == "/" && (stats || clippings)) {
    SyncState syncState = SyncState::load();
    if (stats) {
      syncState.statsCaughtUp = true;
      syncState.statsCatchupId = 0;
    }
    if (clippings) {
      syncState.clippingsCaughtUp = true;
      syncState.clippingsCatchupId = 0;
    }
    syncState.save();
    if (stats) SyncState::markScopesCaughtUp(true);
    if (clippings) SyncState::markScopesCaughtUp(false);
  }
  LOG_INF("StatsSync", "First walk of %s complete; scope prepared", syncScope.path.c_str());
}

void StatsUploadActivity::bookDone(const std::string& bookPath) {
  // An interrupted first walk resumes by skipping books stamped with its id.
  if (flow == Flow::FirstWalk) BookSyncFile::stampWalk(bookPath, syncScope.walkId);
}

bool StatsUploadActivity::hasUsableSavedPosition(const std::string& bookPath) {
  auto epub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
  if (!epub) return true;  // can't tell: keep the pending bit
  EpubReaderUtils::Progress saved;
  return EpubReaderUtils::loadProgress(*epub, saved) && saved.hasPageCount && saved.pageCount >= 1 &&
         saved.pageNumber < saved.pageCount;
}

bool StatsUploadActivity::hasSyncableExtras(const std::string& bookPath) const {
  BookReadingStats stats;
  if (KOREADER_STORE.getSyncStats() && ReadingSyncUpload::loadBookStats(bookPath, stats)) return true;
  return KOREADER_STORE.getSyncClippings() && FsHelpers::hasEpubExtension(bookPath) &&
         ClippingStore::hasStoredStateForFilePath(bookPath, "epub");
}

void StatsUploadActivity::walkStep() {
  std::string bookPath;
  const auto next = nextBook(bookPath);
  if (next == NextBook::Done) {
    if (flow == Flow::FirstWalk) finishFirstWalk();
    finishUpload();
    return;
  }
  if (next == NextBook::Error) {
    fail(tr(STR_FOLDER_SYNC_READ_FAILED));
    return;
  }
  if (next == NextBook::Skip) return;
  {
    RenderLock lock(*this);
    ++progressDone;
  }
  // Resuming an interrupted first walk: one small read per book, no request.
  if (flow == Flow::FirstWalk && BookSyncFile::walkStamped(bookPath, syncScope.walkId)) return;
  setCurrentBook(bookPath);
  if (FsHelpers::hasXtcExtension(bookPath)) {
    // XTC has no KOReader position: its saved stats are the whole sync.
    bool sent = false;
    if (!uploadBookExtras(bookPath, &sent)) {
      failBook(bookPath, extrasFailedParts, extrasFailureReason ? extrasFailureReason : "");
      requestUpdate();
      return;
    }
    bookDone(bookPath);
    {
      RenderLock lock(*this);
      if (sent)
        ++uploaded;
      else
        ++skipped;
    }
    requestUpdate();
    return;
  }
  syncEpub(std::move(bookPath));
}

bool StatsUploadActivity::collectChange(const KOReaderProgressChange& change, void* context) {
  auto* self = static_cast<StatsUploadActivity*>(context);
  if (self->pageChanges.size() >= PAGE_LIMIT) {
    // The cursor covers the whole page, so a truncated page would lose changes.
    self->pageOverflow = true;
    return false;
  }
  self->pageChanges.push_back(change);
  return true;
}

void StatsUploadActivity::loadPage() {
  pageChanges.clear();
  pullItems.clear();
  pullIndex = 0;
  pageOverflow = false;
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("StatsSync", "Cannot render pull screen");
  }
  KOReaderSyncClient::ChangesPage page;
  const auto result = KOReaderSyncClient::getProgressChanges(syncScope.cursor, PAGE_LIMIT, page,
                                                             &StatsUploadActivity::collectChange, this);
  if (!anyPageLoaded && (result == KOReaderSyncClient::FEED_UNSUPPORTED || result == KOReaderSyncClient::SERVER_ERROR ||
                         result == KOReaderSyncClient::JSON_ERROR)) {
    // The feed went away (older server, or it failed on its first request):
    // this run falls back to the per-book walk, which reaches every book.
    LOG_INF("StatsSync", "Change feed unavailable (%d); using the per-book walk", static_cast<int>(result));
    startLegacyWalk();
    return;
  }
  if (result != KOReaderSyncClient::OK) {
    fail(KOReaderSyncClient::errorString(result).c_str());
    return;
  }
  if (pageOverflow || (page.more && page.cursor <= syncScope.cursor)) {
    // A page larger than asked for, or one that never moves the cursor, would
    // lose changes or loop forever.
    LOG_ERR("StatsSync", "Unusable change feed page (overflow=%d, cursor %lu -> %lu)", pageOverflow ? 1 : 0,
            static_cast<unsigned long>(syncScope.cursor), static_cast<unsigned long>(page.cursor));
    fail(tr(STR_SYNC_FAILED_MSG));
    return;
  }
  anyPageLoaded = true;
  pageLoaded = true;
  pageCursor = page.cursor;
  pageMore = page.more;
  LOG_INF("StatsSync", "Feed page: %u changes, cursor %lu, more=%d", static_cast<unsigned>(pageChanges.size()),
          static_cast<unsigned long>(pageCursor), pageMore ? 1 : 0);
  if (!matchPage()) pageLoaded = false;
}

namespace {
bool parseId(const std::string& hex, uint8_t (&out)[16]) {
  if (hex.size() != 32) return false;
  for (size_t i = 0; i < 16; ++i) {
    unsigned value = 0;
    if (std::sscanf(hex.c_str() + i * 2, "%2x", &value) != 1) return false;
    out[i] = static_cast<uint8_t>(value);
  }
  return true;
}

bool changeMatches(const KOReaderProgressChange& change, const uint8_t (&id)[16]) {
  uint8_t candidate[16];
  if (parseId(change.document, candidate) && memcmp(candidate, id, 16) == 0) return true;
  for (uint8_t i = 0; i < change.aliasCount; ++i)
    if (parseId(change.aliases[i], candidate) && memcmp(candidate, id, 16) == 0) return true;
  return false;
}

struct MatchContext {
  const std::vector<KOReaderProgressChange>* changes;
  std::vector<uint8_t>* found;  // per change: some existing book has this id
  std::vector<std::pair<uint8_t, std::string>>* inScope;
  const SyncScope* scope;
};
}  // namespace

bool StatsUploadActivity::keepRemote(const KOReaderProgressChange& change) {
  if (SyncParked::park(change)) {
    ++pulledParked;
    return true;
  }
  fail(tr(STR_SYNC_FAILED_MSG));
  return false;
}

bool StatsUploadActivity::matchPage() {
  // One pass over sync_ids.bin for the whole page; the matches are applied
  // afterwards, so nothing reopens the map while it is being read.
  std::vector<uint8_t> found(pageChanges.size(), 0);
  std::vector<std::pair<uint8_t, std::string>> inScope;
  MatchContext context{&pageChanges, &found, &inScope, &syncScope};
  const bool mapRead = SyncIds::forEach(
      [](const uint8_t (&id)[16], const std::string& bookPath, void* raw) {
        auto& ctx = *static_cast<MatchContext*>(raw);
        for (size_t i = 0; i < ctx.changes->size(); ++i) {
          if (!changeMatches((*ctx.changes)[i], id)) continue;
          if (!Storage.exists(bookPath.c_str())) continue;  // moved on a computer: parked below
          (*ctx.found)[i] = 1;
          // Books outside this scope are left alone; a sync covering them gets
          // the change through its own cursor.
          if (ctx.scope->covers(bookPath)) ctx.inScope->emplace_back(static_cast<uint8_t>(i), bookPath);
        }
        return true;
      },
      &context);
  if (!mapRead && Storage.exists(SyncIds::PATH)) {
    // The id map is there but unreadable: matching would park changes for
    // books on this reader. The per-book walk reaches every book (and rebuilds
    // the map); the cursor stays, so this page is pulled again next time.
    LOG_ERR("StatsSync", "Cannot read the id map; using the per-book walk");
    pageChanges.clear();
    startLegacyWalk();
    return false;
  }
  pullItems.reserve(inScope.size());
  for (auto& item : inScope) pullItems.push_back({item.first, std::move(item.second)});
  for (size_t i = 0; i < pageChanges.size(); ++i) {
    if (found[i]) continue;
    // Not a book this reader knows (yet): kept, so a later push of that book
    // compares with it instead of overwriting it.
    // Never advance the cursor past a change that was neither applied nor kept.
    if (!keepRemote(pageChanges[i])) return false;
  }
  return true;
}

void StatsUploadActivity::pullStep() {
  if (pageLoaded && pullIndex < pullItems.size()) {
    PullItem& item = pullItems[pullIndex++];
    syncEpub(std::move(item.path), &pageChanges[item.change]);
    return;
  }
  if (pageLoaded) {
    // The whole page is handled (applied, marked pending, or parked): a crash
    // after this point never replays it, one before replays it harmlessly.
    syncScope.cursor = pageCursor;
    SyncState::putScope(syncScope);
    pageLoaded = false;
    if (!pageMore) {
      LOG_INF("StatsSync", "Pull done: applied=%u parked=%u", static_cast<unsigned>(pulledApplied),
              static_cast<unsigned>(pulledParked));
      // Release the page buffer (about 3 KB) before the push loads books over TLS.
      std::vector<KOReaderProgressChange>().swap(pageChanges);
      std::vector<PullItem>().swap(pullItems);
      advanceFlow();
      return;
    }
  }
  loadPage();
}

bool StatsUploadActivity::pushProgress(const std::string& bookPath) {
  // The pull already compared every book with a newer remote position, so the
  // push needs no per-book request. A book with a parked remote position (the
  // pull couldn't apply it, or Skip book) is compared with that position
  // instead, exactly as the pull would have, so it is never overwritten blindly.
  const std::string document = documentIdFor(bookPath);
  if (document.empty()) {
    failBook(bookPath, SyncPending::PROGRESS_BIT, tr(STR_HASH_FAILED));
    return false;
  }
  SyncParked::Entry parked;
  if (SyncParked::find(document, parked)) {
    LOG_INF("StatsSync", "Comparing with a parked remote position: %s", bookPath.c_str());
    const KOReaderProgress remote = parked.toProgress(document);
    parkedChange = KOReaderProgressChange{};
    parkedChange.document = document;
    parkedChange.percentage = remote.percentage;
    parkedChange.progress = remote.progress;
    parkedChange.position = remote.position;
    parkedChange.device = remote.device;
    parkedChange.timestamp = remote.timestamp;
    syncEpub(std::string(bookPath), &parkedChange);
    return true;
  }
  KOReaderProgress progress;
  if (!ReadingSyncUpload::prepareProgress(bookPath, progress)) {
    if (hasUsableSavedPosition(bookPath)) {
      // Allocation, load or mapping failed: keep the bit for the next sync.
      failBook(bookPath, SyncPending::PROGRESS_BIT);
      return false;
    }
    // No usable saved position (unread, or old progress): nothing to push.
    SyncPending::clear(bookPath, SyncPending::PROGRESS_BIT);
    return false;
  }
  if (progressBatch.empty()) {
    progressBatch.reserve(KOReaderSyncClient::MAX_PROGRESS_BATCH);
    progressBatchPaths.reserve(KOReaderSyncClient::MAX_PROGRESS_BATCH);
  }
  progressBatch.push_back(std::move(progress));
  progressBatchPaths.push_back(bookPath);
  // Without the batch route there is nothing to gain by holding positions.
  if (progressBatchUnsupported || progressBatch.size() >= KOReaderSyncClient::MAX_PROGRESS_BATCH) flushProgressBatch();
  return false;
}

void StatsUploadActivity::flushProgressBatch() {
  if (progressBatchPaths.empty()) return;
  const size_t count = progressBatchPaths.size();
  KOReaderSyncClient::Error result = KOReaderSyncClient::BATCH_UNSUPPORTED;
  if (!progressBatchUnsupported) {
    // One request (one TLS handshake) for the whole batch; consumes progressBatch.
    result = KOReaderSyncClient::updateProgressBatch(progressBatch);
    if (result == KOReaderSyncClient::BATCH_UNSUPPORTED) {
      LOG_INF("StatsSync", "No batch progress route; sending one book at a time");
      progressBatchUnsupported = true;
    }
  }
  if (result == KOReaderSyncClient::BATCH_UNSUPPORTED) {
    // Positions still queued are sent as they are; a batch the server just
    // turned down was consumed by the request, so those are rebuilt (once per run).
    for (size_t i = 0; i < progressBatchPaths.size(); ++i) {
      const std::string& bookPath = progressBatchPaths[i];
      KOReaderProgress progress;
      if (i < progressBatch.size()) {
        progress = std::move(progressBatch[i]);
      } else if (!ReadingSyncUpload::prepareProgress(bookPath, progress)) {
        failBook(bookPath, SyncPending::PROGRESS_BIT);
        continue;
      }
      const auto single = KOReaderSyncClient::updateProgress(progress);
      if (single != KOReaderSyncClient::OK) {
        failBook(bookPath, SyncPending::PROGRESS_BIT, KOReaderSyncClient::errorString(single));
        continue;
      }
      SyncPending::clear(bookPath, SyncPending::PROGRESS_BIT);
      RenderLock lock(*this);
      ++uploaded;
    }
  } else if (result == KOReaderSyncClient::OK) {
    LOG_INF("StatsSync", "Batch progress upload: %u books", static_cast<unsigned>(count));
    SyncPending::clearPaths(progressBatchPaths, SyncPending::PROGRESS_BIT);
    RenderLock lock(*this);
    uploaded += count;
  } else {
    // All or nothing: every book in the request failed and keeps its pending bit.
    const std::string reason = KOReaderSyncClient::errorString(result);
    for (const auto& bookPath : progressBatchPaths) failBook(bookPath, SyncPending::PROGRESS_BIT, reason);
  }
  std::vector<KOReaderProgress>().swap(progressBatch);
  progressBatchPaths.clear();
  requestUpdate();
}

void StatsUploadActivity::pushStep() {
  std::string bookPath;
  uint8_t mask = 0;
  switch (pending.next(bookPath, mask)) {
    case SyncPending::Cursor::Step::Error:
      flushBookStats();
      flushProgressBatch();
      fail(tr(STR_SYNC_FAILED_MSG));
      return;
    case SyncPending::Cursor::Step::Done:
      pending.end();
      flushProgressBatch();
      advanceFlow();
      return;
    case SyncPending::Cursor::Step::Entry:
      break;
  }
  if (!syncScope.covers(bookPath)) return;
  {
    RenderLock lock(*this);
    ++progressDone;
  }
  if (!Storage.exists(bookPath.c_str())) {
    // Renamed or deleted on a computer. The book's next local change re-adds it.
    LOG_INF("StatsSync", "Dropping pending entry for missing book: %s", bookPath.c_str());
    SyncPending::removePath(bookPath);
    return;
  }
  setCurrentBook(bookPath);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) LOG_ERR("StatsSync", "Cannot render push screen");
  // Stats and clippings first: a parked comparison continues in a child activity.
  if ((mask & SyncPending::STATS_BIT) && KOREADER_STORE.getSyncStats()) addBookStats(bookPath);
  if ((mask & SyncPending::CLIPPINGS_BIT) && KOREADER_STORE.getSyncClippings()) uploadClippings(bookPath);
  if ((mask & SyncPending::PROGRESS_BIT) && FsHelpers::hasEpubExtension(bookPath) && pushProgress(bookPath)) return;
  requestUpdate();
}

StatsUploadActivity::NextBook StatsUploadActivity::nextPending(std::string& bookPath, const uint8_t bit) {
  uint8_t mask = 0;
  switch (pending.next(bookPath, mask)) {
    case SyncPending::Cursor::Step::Error:
      return NextBook::Error;
    case SyncPending::Cursor::Step::Done:
      phase = Phase::Done;
      return NextBook::Done;
    case SyncPending::Cursor::Step::Entry:
      break;
  }
  if (!(mask & bit)) return NextBook::Skip;
  if (!Storage.exists(bookPath.c_str())) {
    // Renamed or deleted on a computer. The book's next local change re-adds it.
    LOG_INF("StatsSync", "Dropping pending entry for missing book: %s", bookPath.c_str());
    SyncPending::removePath(bookPath);
    return NextBook::Skip;
  }
  return NextBook::Book;
}

StatsUploadActivity::NextBook StatsUploadActivity::nextBook(std::string& bookPath) {
  switch (scope) {
    case Scope::ReadingStats:
    case Scope::Clippings: {
      const bool stats = scope == Scope::ReadingStats;
      if (phase == Phase::Pending)
        return nextPending(bookPath, stats ? SyncPending::STATS_BIT : SyncPending::CLIPPINGS_BIT);
      if (phase != Phase::CatchUp) return NextBook::Done;  // toggle off: overall stats only
      NextBook next = NextBook::Done;
      if (stats) {
        const auto step = folderBooks->next(bookPath);
        next = step == FolderBookIterator::Result::Book    ? NextBook::Book
               : step == FolderBookIterator::Result::Entry ? NextBook::Skip
               : step == FolderBookIterator::Result::Error ? NextBook::Error
                                                           : NextBook::Done;
      } else {
        const auto step = ClippingStore::nextClippedEpub(clippingsPosition, bookPath);
        next = step == ClippingStore::ClippedBookStep::Book    ? NextBook::Book
               : step == ClippingStore::ClippedBookStep::Skip  ? NextBook::Skip
               : step == ClippingStore::ClippedBookStep::Error ? NextBook::Error
                                                               : NextBook::Done;
      }
      if (next != NextBook::Done) return next;
      finishCatchup();
      return NextBook::Skip;  // continue with the pending list
    }
    case Scope::Folder:
    case Scope::Library: {
      const auto next = folderBooks->next(bookPath);
      if (next == FolderBookIterator::Result::Done) return NextBook::Done;
      if (next == FolderBookIterator::Result::Error) return NextBook::Error;
      return next == FolderBookIterator::Result::Book ? NextBook::Book : NextBook::Skip;
    }
    case Scope::Book:
      if (singleBookTaken) return NextBook::Done;
      singleBookTaken = true;
      bookPath = path;
      return NextBook::Book;
  }
  return NextBook::Done;
}

void StatsUploadActivity::uploadNext() {
  if (!globalAttempted && scope == Scope::Clippings) {
    // Sync Clippings sends clippings only.
    globalAttempted = true;
  }
  if (!globalAttempted) {
    // Batch ownership prevents books that skip/fail from starving overall stats,
    // and prevents retries of accepted global snapshots after a per-book failure.
    globalAttempted = true;
    if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
      LOG_ERR("StatsSync", "Cannot render overall stats upload screen");
      {
        RenderLock lock(*this);
        globalResult = StatsUploadClient::Result::InvalidResponse;
      }
      requestUpdate();
      return;
    }
#ifndef SIMULATOR
    halClock.syncSystemTimeFromNTP();
    trustedtime::note();
    trustedtime::startSync();
#endif
    const auto result =
        KOREADER_STORE.getSyncStats() ? ReadingSyncUpload::globalStats() : StatsUploadClient::Result::Skipped;
    {
      RenderLock lock(*this);
      globalResult = result;
    }
    requestUpdate();
    return;
  }

  if (scope == Scope::Library || scope == Scope::Folder) {
    switch (flow) {
      case Flow::Unplanned:
        planIncremental();
        return;
      case Flow::Legacy:
      case Flow::FirstWalk:
        walkStep();
        return;
      case Flow::CatchUpStats:
      case Flow::CatchUpClippings: {
        const bool stats = flow == Flow::CatchUpStats;
        std::string bookPath;
        NextBook next = NextBook::Done;
        if (stats) {
          const auto step = folderBooks->next(bookPath);
          next = step == FolderBookIterator::Result::Book    ? NextBook::Book
                 : step == FolderBookIterator::Result::Entry ? NextBook::Skip
                 : step == FolderBookIterator::Result::Error ? NextBook::Error
                                                             : NextBook::Done;
        } else {
          const auto step = ClippingStore::nextClippedEpub(clippingsPosition, bookPath);
          next = step == ClippingStore::ClippedBookStep::Book    ? NextBook::Book
                 : step == ClippingStore::ClippedBookStep::Skip  ? NextBook::Skip
                 : step == ClippingStore::ClippedBookStep::Error ? NextBook::Error
                                                                 : NextBook::Done;
          // Folder catch-ups stay in the folder.
          if (next == NextBook::Book && !syncScope.covers(bookPath)) next = NextBook::Skip;
        }
        if (next == NextBook::Error) {
          if (stats) flushBookStats();
          fail(tr(STR_FOLDER_SYNC_READ_FAILED));
        } else if (next == NextBook::Done) {
          finishScopeCatchup(stats);
        } else if (next == NextBook::Book) {
          if (stats)
            addBookStats(bookPath);
          else
            uploadClippings(bookPath);
        }
        return;
      }
      case Flow::Pull:
        pullStep();
        return;
      case Flow::Push:
        pushStep();
        return;
      case Flow::Done:
        finishUpload();
        return;
    }
    return;
  }

  std::string bookPath;
  const auto next = nextBook(bookPath);
  if (next == NextBook::Done) {
    if (scope == Scope::ReadingStats) flushBookStats();
    finishUpload();
    return;
  }
  if (next == NextBook::Error) {
    // Send the books already queued before the walk stops.
    if (scope == Scope::ReadingStats) flushBookStats();
    fail(tr(STR_FOLDER_SYNC_READ_FAILED));
    return;
  }
  if (next == NextBook::Skip) return;
  if (scope == Scope::ReadingStats) {
    addBookStats(bookPath);
    return;
  }
  if (scope == Scope::Clippings) {
    uploadClippings(bookPath);
    return;
  }
  if (FsHelpers::hasXtcExtension(bookPath)) {
    // XTC has no KOReader position: its saved stats are the whole sync.
    bool sent = false;
    if (!uploadBookExtras(bookPath, &sent)) {
      failBook(bookPath, extrasFailedParts, extrasFailureReason ? extrasFailureReason : "");
      requestUpdate();
      return;
    }
    {
      RenderLock lock(*this);
      if (sent)
        ++uploaded;
      else
        ++skipped;
    }
    requestUpdate();
    return;
  }
  syncEpub(std::move(bookPath));
}

void StatsUploadActivity::recordExtras(const bool statsOk, const bool clippingsOk, const bool statsError,
                                       const bool clippingsError) {
  RenderLock lock(*this);
  statsUploaded += statsOk;
  clippingsUploaded += clippingsOk;
  statsFailed += statsError;
  clippingsFailed += clippingsError;
}

bool StatsUploadActivity::uploadBookExtras(const std::string& bookPath, bool* sentAny) {
  if (sentAny) *sentAny = false;
  extrasFailedParts = 0;
  extrasFailureReason = nullptr;
  if (!KOREADER_STORE.getSyncStats() && !KOREADER_STORE.getSyncClippings()) return true;
  // Untouched books are skipped before hashing: a walk of a large card only
  // computes ids for (and records in sync_ids.bin) books with saved data.
  if (!hasSyncableExtras(bookPath)) return true;
  const auto document = documentIdFor(bookPath);
  if (document.empty()) {
    // Neither can be sent without the book's id.
    extrasFailedParts = SyncPending::STATS_BIT | SyncPending::CLIPPINGS_BIT;
    extrasFailureReason = tr(STR_HASH_FAILED);
    return false;
  }
  const auto result = ReadingSyncUpload::extras(bookPath, document);
  extrasFailedParts = (StatsUploadClient::failed(result.stats) ? SyncPending::STATS_BIT : 0) |
                      (StatsUploadClient::failed(result.clippings) ? SyncPending::CLIPPINGS_BIT : 0);
  if (extrasFailedParts)
    extrasFailureReason =
        StatsUploadClient::errorString(StatsUploadClient::failed(result.stats) ? result.stats : result.clippings);
  const uint8_t settled = (StatsUploadClient::settled(result.stats) ? SyncPending::STATS_BIT : 0) |
                          (StatsUploadClient::settled(result.clippings) ? SyncPending::CLIPPINGS_BIT : 0);
  if (settled) SyncPending::clear(bookPath, settled);
  if (sentAny)
    *sentAny = result.stats == StatsUploadClient::Result::Ok || result.clippings == StatsUploadClient::Result::Ok;
  recordExtras(result.stats == StatsUploadClient::Result::Ok, result.clippings == StatsUploadClient::Result::Ok,
               StatsUploadClient::failed(result.stats), StatsUploadClient::failed(result.clippings));
  return result.success();
}

void StatsUploadActivity::addBookStats(const std::string& bookPath) {
  BookReadingStats stats;
  if (!ReadingSyncUpload::loadBookStats(bookPath, stats)) {
    // Never read, or stats off for it: nothing to send, so nothing pending.
    if (phase == Phase::Pending) SyncPending::clear(bookPath, SyncPending::STATS_BIT);
    return;
  }
  // Only books with stats pay for the resume check.
  if (phase == Phase::CatchUp && BookSyncFile::stamped(bookPath, true, catchupStamp)) return;
  // Only books with stats need their document id (cached in the book's sync record).
  const auto document = documentIdFor(bookPath);
  if (document.empty()) {
    LOG_ERR("StatsSync", "Cannot compute document id: %s", bookPath.c_str());
    recordExtras(false, false, true, false);
    requestUpdate();
    return;
  }
  if (!batch.add(document.c_str(), stats)) {
    // Full: send what is queued and start a new request with this book.
    if (batch.count() == 0 || !flushBookStats() || !batch.add(document.c_str(), stats)) {
      if (batch.count() == 0) LOG_ERR("StatsSync", "Cannot encode stats: %s", bookPath.c_str());
      recordExtras(false, false, true, false);
      requestUpdate();
      return;
    }
  }
  batchPaths.push_back(bookPath);
  if (batch.count() >= StatsUploadPayload::BookBatch::MAX_ITEMS) flushBookStats();
}

bool StatsUploadActivity::flushBookStats() {
  const auto count = static_cast<uint32_t>(batch.count());
  if (count == 0) return true;
  auto result = StatsUploadClient::Result::InvalidResponse;
  if (batch.finish()) {
    StatsUploadClient client;
    result = client.put("/api/v1/stats/books", batch.payload(), true);
  }
  LOG_INF("StatsSync", "Stats batch of %u books: %d", static_cast<unsigned>(count), static_cast<int>(result));
  if (result == StatsUploadClient::Result::Ok) {
    SyncPending::clearPaths(batchPaths, SyncPending::STATS_BIT);
    if (phase == Phase::CatchUp)
      for (const auto& bookPath : batchPaths) BookSyncFile::stamp(bookPath, true, catchupStamp);
  }
  batchPaths.clear();
  {
    RenderLock lock(*this);
    if (result == StatsUploadClient::Result::Ok)
      statsUploaded += count;
    else if (StatsUploadClient::failed(result))
      statsFailed += count;
  }
  requestUpdate();
  // Start the next request even after a failure; the queued books are counted as failed.
  return batch.begin(deviceId);
}

void StatsUploadActivity::uploadClippings(const std::string& bookPath) {
  if (phase == Phase::CatchUp && BookSyncFile::stamped(bookPath, false, catchupStamp)) return;
  if (!FsHelpers::hasEpubExtension(bookPath)) {  // only EPUBs keep clippings
    SyncPending::clear(bookPath, SyncPending::CLIPPINGS_BIT);
    return;
  }
  const auto document = documentIdFor(bookPath);
  const auto result =
      document.empty() ? StatsUploadClient::Result::InvalidResponse : ClippingsUpload::upload(bookPath, document);
  if (document.empty()) LOG_ERR("StatsSync", "Cannot compute document id: %s", bookPath.c_str());
  if (StatsUploadClient::settled(result)) {
    SyncPending::clear(bookPath, SyncPending::CLIPPINGS_BIT);
    if (phase == Phase::CatchUp) BookSyncFile::stamp(bookPath, false, catchupStamp);
  }
  recordExtras(false, result == StatsUploadClient::Result::Ok, false, StatsUploadClient::failed(result));
  requestUpdate();
}

namespace {
KOReaderProgress progressFromChange(const KOReaderProgressChange& change, const std::string& document) {
  KOReaderProgress progress{};
  progress.document = document;
  progress.progress = change.progress;
  progress.percentage = change.percentage;
  progress.device = change.device;
  progress.timestamp = change.timestamp;
  progress.position = change.position;
  return progress;
}
}  // namespace

void StatsUploadActivity::syncEpub(std::string&& bookPath, const KOReaderProgressChange* remote) {
  setCurrentBook(bookPath);
  if (remote) {
    // The pull map can predate an external SD edit. Check the current book (including aliases)
    // before supplying a remote position to the child activity.
    uint8_t id[16];
    if (!parseId(documentIdFor(bookPath), id) || !changeMatches(*remote, id)) {
      keepRemote(*remote);
      return;
    }
  }
  {
    // Rebuild missing metadata (for example after moving a book into Read),
    // but release it before the child makes TLS requests.
    auto epub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
    if (!epub) {
      fail(tr(STR_KOREADER_SYNC_LOW_MEMORY));
      return;
    }
    EpubReaderUtils::Progress saved;
    if (!EpubReaderUtils::loadProgress(*epub, saved) || !saved.hasPageCount || saved.pageCount < 1 ||
        saved.pageNumber >= saved.pageCount) {
      LOG_INF("StatsSync", "Skipping EPUB progress without usable saved position: %s", bookPath.c_str());
      epub.reset();
      if (remote) {
        // Never opened here (or old progress): kept for a later comparison.
        if (!keepRemote(*remote)) return;
        {
          RenderLock lock(*this);
          ++skipped;
        }
        requestUpdate();
        return;
      }
      if (!uploadBookExtras(bookPath)) {
        failBook(bookPath, extrasFailedParts, extrasFailureReason ? extrasFailureReason : "");
        requestUpdate();
        return;
      }
      {
        RenderLock lock(*this);
        ++skipped;
      }
      bookDone(bookPath);
      requestUpdate();
      return;
    }
    // Refresh the panel only for books that will reach the network, not for
    // every unread EPUB in a large Library.
    if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
      fail(tr(STR_SYNC_FAILED_MSG));
      return;
    }
    if (!epub->load(true, true, Epub::XLocationLoadMode::Skip, true) || saved.spineIndex < 0 ||
        saved.spineIndex >= epub->getSpineItemsCount()) {
      epub.reset();
      extrasFailedParts = 0;
      if (remote) {
        if (!keepRemote(*remote)) return;  // the cursor moves on: keep it for a later comparison
      } else {
        uploadBookExtras(bookPath);
      }
      // The EPUB could not be read, so its position could not sync.
      failBook(bookPath, SyncPending::PROGRESS_BIT | extrasFailedParts, tr(STR_SYNC_REOPTIMIZE_REQUIRED));
      requestUpdate();
      return;
    }
  }
  auto sync = makeUniqueNoThrow<KOReaderSyncActivity>(renderer, mappedInput, bookPath, KOREADER_STORE.getMatchMethod(),
                                                      SETTINGS.orientation, true, false);
  if (!sync) {
    fail(tr(STR_KOREADER_SYNC_LOW_MEMORY));
    return;
  }
  // The book syncs behind this screen: no per-book screen, no prompt mid-run.
  sync->setBackground();
  // The pulled change replaces the per-book GET; it is compared with the local
  // position exactly as a fetched one would be.
  // `remote` points into pageChanges, which stays put until the whole page is
  // handled, so the callback can use it without a copy.
  const KOReaderProgressChange* pulled = remote;
  if (remote) sync->setRemoteProgress(progressFromChange(*remote, remote->document));
  {
    RenderLock lock(*this);
    state = State::SyncingBook;
  }
  startActivityForResult(std::move(sync), [this, bookPath, pulled](const ActivityResult& result) {
    // Exit (held or pressed while the book synced) stops the whole bulk sync.
    if (result.isCancelled) {
      LOG_INF("StatsSync", "Exited: synced=%u skipped=%u failed=%u", static_cast<unsigned>(uploaded),
              static_cast<unsigned>(skipped), static_cast<unsigned>(failed));
      leave();
      return;
    }
    const auto* synced = std::get_if<ProgressSyncResult>(&result.data);
    if (!synced) {
      fail(tr(STR_SYNC_FAILED_MSG));
      return;
    }
    recordExtras(synced->statsUploaded, synced->clippingsUploaded, synced->statsFailed, synced->clippingsFailed);
    if (pulled) {
      if (synced->success) {
        SyncParked::remove(pulled->document);
        ++pulledApplied;
      } else if (!keepRemote(*pulled)) {
        // A conflict kept for the end, or a failure: the cursor still moves
        // past this change, so it must be kept for a later comparison.
        return;
      }
    } else if (synced->success || synced->skipped || synced->deferred) {
      bookDone(bookPath);
    }
    if (synced->deferred) {
      // Its remote position is parked; the choice comes after the run. Past
      // the cap it stays parked for the next sync or the book's next open.
      RenderLock lock(*this);
      if (deferredBooks.size() < MAX_DEFERRED)
        deferredBooks.push_back(bookPath);
      else
        ++skipped;
    } else if (!synced->success && !synced->skipped) {
      const uint8_t parts = (synced->progressFailed ? SyncPending::PROGRESS_BIT : 0) |
                            (synced->statsFailed ? SyncPending::STATS_BIT : 0) |
                            (synced->clippingsFailed ? SyncPending::CLIPPINGS_BIT : 0);
      failBook(bookPath, parts, synced->reason, synced->title);
    }
    {
      RenderLock lock(*this);
      if (synced->progressSucceeded) ++uploaded;
      if (synced->skipped) ++skipped;
      state = State::Uploading;
    }
    requestUpdate();
  });
}

void StatsUploadActivity::reviewNext() {
  // Kept conflicts, one choice after another. A book settled meanwhile (its
  // parked position is gone) needs no choice.
  while (reviewIndex < deferredBooks.size()) {
    const std::string bookPath = deferredBooks[reviewIndex++];
    const std::string document = documentIdFor(bookPath);
    SyncParked::Entry parked;
    if (document.empty() || !SyncParked::find(document, parked)) continue;
    auto sync = makeUniqueNoThrow<KOReaderSyncActivity>(
        renderer, mappedInput, bookPath, KOREADER_STORE.getMatchMethod(), SETTINGS.orientation, true, false);
    if (!sync) {
      fail(tr(STR_KOREADER_SYNC_LOW_MEMORY));
      return;
    }
    // Stats and clippings went with the run; only the position is left.
    sync->setRemoteProgress(parked.toProgress(document));
    startActivityForResult(std::move(sync), [this, bookPath](const ActivityResult& result) {
      if (result.isCancelled) {
        // Exit: the remaining choices stay kept for the next sync or open.
        LOG_INF("StatsSync", "Review exited with %u choice(s) left",
                static_cast<unsigned>(deferredBooks.size() - reviewIndex + 1));
        leave();
        return;
      }
      const auto* synced = std::get_if<ProgressSyncResult>(&result.data);
      if (synced) {
        RenderLock lock(*this);
        if (synced->progressSucceeded) ++uploaded;
        if (synced->skipped) ++skipped;
      }
      if (synced && !synced->success && !synced->skipped)
        failBook(bookPath, SyncPending::PROGRESS_BIT, synced->reason, synced->title);
      reviewNext();
    });
    return;
  }
  finishUpload();
}

void StatsUploadActivity::loop() {
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Confirm, initialConfirm))
    return;
  // A single book, or a bulk sync the caller already confirmed: nothing to ask.
  if (autoStart && state == State::Ready) {
    start();
    return;
  }
  // Each upload step blocks and buttons are polled, so a held Exit also counts
  // while uploading; a quick press during a request would otherwise be missed.
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      (!singleBook() && state == State::Uploading && mappedInput.isPressed(MappedInputManager::Button::Back)) ||
      TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    mappedInput.suppressNextBackRelease();
    leave();
    return;
  }
  if (state == State::Done && !failures.empty()) {
    // Scroll the failure list: buttons, or a swipe like other touch lists.
    const auto swipe = mappedInput.wasSwipe();
    if ((swipe == MappedInputManager::SwipeDir::Up || mappedInput.wasPressed(MappedInputManager::Button::Down) ||
         mappedInput.wasPressed(MappedInputManager::Button::Right)) &&
        failuresBelow) {
      {
        RenderLock lock(*this);
        ++failureScroll;
      }
      requestUpdate();
      return;
    }
    if ((swipe == MappedInputManager::SwipeDir::Down || mappedInput.wasPressed(MappedInputManager::Button::Up) ||
         mappedInput.wasPressed(MappedInputManager::Button::Left)) &&
        failureScroll > 0) {
      {
        RenderLock lock(*this);
        --failureScroll;
      }
      requestUpdate();
      return;
    }
  }
  if (state == State::Ready || state == State::Done) {
    int x = 0, y = 0;
    bool actionTapped = false;
    if (mappedInput.wasScreenTapped(x, y)) {
      const auto area = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
      const auto layout =
          TouchActionButtons::vertical(Rect{area.x + 20, area.y + area.height - 80, area.width - 40, 56}, 1);
      actionTapped = TouchActionButtons::indexAt(layout, x, y) == 0;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || actionTapped) {
      if (state == State::Done) {
        leave();
        return;
      } else {
        start();
      }
    }
  } else if (state == State::Uploading) {
    // One book per loop permits cancellation between books. No background
    // task or persistent upload queue can start networking outside this action.
    uploadNext();
  }
}

void StatsUploadActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  const char* heading = title();
  if (mappedInput.hasTouchHardware())
    TouchHeaderBackButton::draw(renderer, header, heading, false);
  else
    GUI.drawHeader(renderer, header, heading);
  const auto area = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const bool listFailures = state == State::Done && !failures.empty();
  // A failure list needs the room below the results.
  const int y = area.y + (listFailures ? area.height / 6 : area.height / 3);
  const bool asking = state == State::Ready && !autoStart;
  const bool running = !singleBook() && (state == State::Uploading || state == State::SyncingBook);
  const char* confirmText = scope == Scope::Library        ? tr(STR_SYNC_ALL_CONFIRM)
                            : scope == Scope::ReadingStats ? tr(STR_SYNC_READING_STATS_CONFIRM)
                                                           : tr(STR_FOLDER_SYNC_CONFIRM);
  const char* flowText = flow == Flow::FirstWalk || flow == Flow::Legacy
                             ? (scope == Scope::Library ? tr(STR_SYNCING_CARD_BOOKS) : tr(STR_SYNCING_FOLDER_BOOKS))
                         : flow == Flow::CatchUpStats     ? tr(STR_SCANNING_READING_STATS)
                         : flow == Flow::CatchUpClippings ? tr(STR_CHECKING_CLIPPINGS)
                         : flow == Flow::Pull             ? tr(STR_CHECKING_FOR_UPDATES)
                         : flow == Flow::Push             ? tr(STR_UPLOADING_CHANGES)
                                                          : tr(STR_LOADING);
  const char* workingText = scope == Scope::ReadingStats ? tr(STR_SCANNING_READING_STATS)
                            : scope == Scope::Clippings  ? tr(STR_CHECKING_CLIPPINGS)
                            : scope == Scope::Book       ? tr(STR_LOADING)
                                                         : flowText;
  const char* text = asking ? confirmText
                     : state == State::Ready || state == State::Uploading || state == State::SyncingBook
                         ? workingText
                         : message.c_str();
  int detailY =
      y + UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, y, text, 3, true, EpdFontFamily::REGULAR, 4) +
      (running ? 8 : 24);
  if (running) {
    // One screen for the whole run: the book being synced and how far along.
    if (!currentBook.empty()) {
      detailY += UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, detailY, currentBook.c_str(), 1, true,
                                                  EpdFontFamily::BOLD, 4) +
                 16;
    }
    if (progressTotal > 0) {
      const int barHeight = 16;
      GUI.drawProgressBar(renderer, Rect{area.x + 40, detailY, area.width - 80, barHeight},
                          std::min(progressDone, progressTotal), progressTotal);
      detailY += barHeight + 15 + renderer.getLineHeight(UI_10_FONT_ID) + 8;  // the bar's percent line
    }
  }
  if (state == State::Ready && autoStart) {
    // About to connect; there are no results to show yet.
  } else if (asking) {
    if (scope == Scope::Folder) {
      detailY += UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, detailY, path.c_str(), 3, true,
                                                  EpdFontFamily::REGULAR, 4) +
                 8;
    }
    const auto url = KOREADER_STORE.getBaseUrl();
    UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, detailY, url.c_str(), 3, true,
                                     EpdFontFamily::REGULAR, 4);
  } else {
    // Book counts describe progress sync; the targeted uploads only list their data.
    if (!targeted()) {
      char counts[160];
      snprintf(counts, sizeof(counts), tr(STR_FOLDER_SYNC_COUNTS), static_cast<unsigned>(uploaded),
               static_cast<unsigned>(skipped), static_cast<unsigned>(failed));
      detailY += UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, detailY, counts, 3, true,
                                                  EpdFontFamily::REGULAR, 4) +
                 (deferredBooks.empty() || state == State::Done ? 24 : 8);
      if (!deferredBooks.empty() && state != State::Done) {
        char review[64];
        snprintf(review, sizeof(review), tr(STR_SYNC_TO_REVIEW_FORMAT), static_cast<unsigned>(deferredBooks.size()));
        detailY += UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, detailY, review, 1, true,
                                                    EpdFontFamily::REGULAR, 4) +
                   24;
      }
    }
    char statsCount[48];
    char clippingsCount[48];
    const char* statsText =
        ReadingSyncUpload::countLabel(statsCount, sizeof(statsCount), statsUploaded, statsFailed, false);
    const char* clippingsText =
        ReadingSyncUpload::countLabel(clippingsCount, sizeof(clippingsCount), clippingsUploaded, clippingsFailed, true);
    int rowY = detailY;
    if (scope != Scope::Clippings) {
      rowY += UITheme::drawCenteredStatusRow(renderer, area, UI_10_FONT_ID, rowY, tr(STR_ALL_TIME_STATS),
                                             ReadingSyncUpload::statusLabel(globalResult, false)) +
              8;
      rowY += UITheme::drawCenteredStatusRow(renderer, area, UI_10_FONT_ID, rowY, tr(STR_READING_STATS), statsText) + 8;
    }
    if (scope != Scope::ReadingStats)
      rowY += UITheme::drawCenteredStatusRow(renderer, area, UI_10_FONT_ID, rowY, tr(STR_CLIPPINGS), clippingsText) + 8;
    if (listFailures) {
      // Each failed book with what failed and why, as far as the screen allows.
      const int bottom = area.y + area.height - (mappedInput.hasTouchHardware() ? 90 : 60);
      const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
      const int padding = UITheme::getInstance().getMetrics().contentSidePadding;
      const Rect listArea{area.x + padding, area.y, area.width - padding * 2, area.height};
      rowY += 16;
      rowY += UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, rowY, tr(STR_FAILED_BOOKS), 1, true,
                                               EpdFontFamily::BOLD, 4) +
              8;
      // Scrolled one failure at a time; the hints show which way there is more.
      failuresBelow = false;
      for (size_t i = failureScroll; i < failures.size(); ++i) {
        if (rowY + lineHeight * 3 > bottom) {
          failuresBelow = true;
          break;
        }
        const auto& failure = failures[i];
        rowY += UITheme::drawCenteredWrappedText(renderer, listArea, UI_10_FONT_ID, rowY, failure.name.c_str(), 1, true,
                                                 EpdFontFamily::BOLD, 4);
        std::string detail = ReadingSyncUpload::failureHeading(failure.parts & SyncPending::PROGRESS_BIT,
                                                               failure.parts & SyncPending::STATS_BIT,
                                                               failure.parts & SyncPending::CLIPPINGS_BIT);
        if (!failure.reason.empty()) detail += ": " + failure.reason;
        rowY += UITheme::drawCenteredWrappedText(renderer, listArea, UI_10_FONT_ID, rowY, detail.c_str(), 2, true,
                                                 EpdFontFamily::REGULAR, 4) +
                8;
      }
      // Past the detail cap, failed books are only counted.
      if (!failuresBelow && failed > failures.size()) {
        char more[32];
        snprintf(more, sizeof(more), tr(STR_MORE_ITEMS_FORMAT), static_cast<unsigned>(failed - failures.size()));
        UITheme::drawCenteredWrappedText(renderer, area, UI_10_FONT_ID, rowY, more, 1, true, EpdFontFamily::REGULAR, 4);
      }
    }
  }
  const char* action = asking ? tr(STR_SYNC) : state == State::Done ? tr(STR_BACK) : "";
  if (action[0] && mappedInput.hasTouchHardware()) {
    const auto layout =
        TouchActionButtons::vertical(Rect{area.x + 20, area.y + area.height - 80, area.width - 40, 56}, 1);
    const char* labels[] = {action};
    TouchActionButtons::draw(renderer, layout, labels, 0);
  }
  // Mid-run, Back leaves the whole sync rather than one book or screen. When
  // done, Confirm also leaves, but a second Back hint would only repeat the first.
  const auto labels = mappedInput.mapLabels(
      mappedInput.withBackArrow(running ? tr(STR_EXIT) : tr(STR_BACK)), state == State::Done ? "" : action,
      listFailures && failureScroll > 0 ? tr(STR_DIR_UP) : "", listFailures && failuresBelow ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
