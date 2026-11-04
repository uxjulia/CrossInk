#pragma once

#include <KOReaderSyncClient.h>

#include <memory>
#include <string>
#include <vector>

#include "PendingOverlayResume.h"
#include "activities/Activity.h"
#include "activities/reader/StatsUploadPayload.h"
#include "network/ReadingSyncUpload.h"
#include "sync/SyncPending.h"
#include "sync/SyncState.h"
#include "util/FolderBookIterator.h"

// Bulk "Sync" for several books: progress, reading stats, and clippings for each
// book, plus overall stats once. Against a CrossPoint Sync server with the
// progress change feed, Sync Books and Sync Folder are incremental: a scope's
// first sync walks it once, later syncs pull only changed positions and push
// only books on the pending list. Stock KOSync servers keep the per-book walk.
// ReadingStats uploads saved stats; Clippings exchanges per-book highlights
// across all EPUBs, including books without local selections. Neither changes
// reading progress. Every upload
// clears the pending bits it settled. Only entered from an explicit user
// action; no reader/background hooks.
class StatsUploadActivity final : public Activity {
 public:
  enum class Scope : uint8_t {
    Library,       // Every book on the SD card ("Sync Books"); does not need the Library index.
    Folder,        // A File Browser folder and its subfolders ("Sync Folder").
    Book,          // One book without a KOReader position (XTC): stats only.
    ReadingStats,  // Overall stats plus every book's saved stats, found by walking the card.
    Clippings,     // Two-way clippings for every EPUB on the card.
  };
  // Sync Books.
  StatsUploadActivity(GfxRenderer& renderer, MappedInputManager& input)
      : Activity("StatsUpload", renderer, input), scope(Scope::Library) {}
  // `confirmed`: the caller already asked, so start without the confirmation screen.
  StatsUploadActivity(GfxRenderer& renderer, MappedInputManager& input, std::string path, Scope scope = Scope::Folder,
                      bool confirmed = false)
      : Activity("StatsUpload", renderer, input),
        scope(scope),
        autoStart(confirmed || scope == Scope::Book),
        path(std::move(path)) {}
  // Book scope: the Library/File Browser view to return to. Kept here, not in
  // global state, so leaving another way (Home, sleep) leaves nothing stale.
  void setReturnTo(PendingOverlayResume resume) { returnTo = std::move(resume); }
  // Started from a screen over a reader: reopen that book afterwards, the way a
  // reader-started sync resumes after its silent restart. The EPUB reader then
  // runs `readerMenuAction` (an EpubReaderMenuAction) to reopen that screen.
  void setReturnToBook(std::string bookPath, uint8_t readerMenuAction) {
    returnBookPath = std::move(bookPath);
    returnMenuAction = readerMenuAction;
    runReturnMenuAction = true;
  }
  // Started from File Transfer: go back to the book it was opened from (if any),
  // else Home with File Transfer focused. `restart`: File Transfer was running in
  // the minimal network boot, which must reboot to restore reader resources.
  void setReturnToFileTransfer(std::string bookPath, bool restart) {
    returnBookPath = std::move(bookPath);
    fromFileTransfer = true;
    restartOnLeave = restart;
  }
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::Uploading; }

 private:
  enum class State { Ready, Wifi, Uploading, SyncingBook, Done, Failed };
  enum class NextBook { Book, Skip, Done, Error };
  // Reading Stats / Clippings: catch-up walk first when needed, then pending books.
  enum class Phase : uint8_t { CatchUp, Pending, Done };
  // Sync Books / Sync Folder. Legacy: the per-book walk (stock KOSync, or no
  // change feed). Otherwise a scope's first walk, or catch-ups, pull, push.
  enum class Flow : uint8_t { Unplanned, Legacy, FirstWalk, CatchUpStats, CatchUpClippings, Pull, Push, Done };
  struct PullItem {
    uint8_t change;  // index into pageChanges
    std::string path;
  };
  State state = State::Ready;
  const Scope scope;
  const bool autoStart = false;  // A single book or a pre-confirmed bulk sync skips Ready.
  std::string path;              // Folder root or single book; empty for Library.
  PendingOverlayResume returnTo;
  std::string returnBookPath;
  uint8_t returnMenuAction = 0;
  bool runReturnMenuAction = false;
  bool fromFileTransfer = false;
  bool restartOnLeave = false;
  uint32_t uploaded = 0;
  uint32_t skipped = 0;
  uint32_t failed = 0;
  // A book that failed, for the summary: what failed (SyncPending bits) and why.
  struct BookFailure {
    std::string name;
    uint8_t parts;
    std::string reason;
  };
  // Enough to scroll through on the summary; the rest are only counted.
  static constexpr size_t MAX_FAILURE_DETAILS = 20;
  std::vector<BookFailure> failures;
  // The summary's failure list: the first failure on screen, and whether the
  // last render had more below it (set by render, read by loop to scroll).
  size_t failureScroll = 0;
  bool failuresBelow = false;
  // Ask on conflicts: books whose choice waits for the end of the run (their
  // remote position is parked meanwhile), then shown one after another.
  static constexpr size_t MAX_DEFERRED = 32;
  std::vector<std::string> deferredBooks;
  size_t reviewIndex = 0;
  bool reviewStarted = false;
  // Progress bar for the walk and the push; 0 total = no bar (the pull's size is unknown).
  uint32_t progressDone = 0;
  uint32_t progressTotal = 0;
  std::string currentBook;  // shown under the progress text
  // What the last uploadBookExtras() call failed to send (SyncPending bits).
  uint8_t extrasFailedParts = 0;
  const char* extrasFailureReason = nullptr;  // and why (a translated message)
  uint32_t statsUploaded = 0;
  uint32_t clippingsUploaded = 0;
  uint32_t statsFailed = 0;
  uint32_t clippingsFailed = 0;
  // ReadingStats scope: one request carries several books (fixed buffer, this action only).
  std::unique_ptr<char[]> batchBuffer;
  StatsUploadPayload::BookBatch batch{nullptr, 0};
  std::vector<std::string> batchPaths;  // books in the queued request, cleared/stamped on success
  Phase phase = Phase::Done;
  uint32_t catchupStamp = 0;  // this catch-up's id while phase is CatchUp
  SyncPending::Cursor pending;
  char deviceId[32]{};
  // Clippings scope: position in the clippings folder listing.
  uint64_t clippingsPosition = 0;
  // Incremental Sync Books / Sync Folder.
  Flow flow = Flow::Unplanned;
  SyncScope syncScope;
  std::vector<KOReaderProgressChange> pageChanges;  // one feed page, at most PAGE_LIMIT
  std::vector<PullItem> pullItems;                  // matched books in scope for that page
  size_t pullIndex = 0;
  uint32_t pageCursor = 0;
  bool pageMore = false;
  bool pageLoaded = false;
  bool anyPageLoaded = false;
  bool pageOverflow = false;            // the server sent more than a page
  bool walkResumed = false;             // this run continues an earlier, unfinished first walk
  KOReaderProgressChange parkedChange;  // push: the parked remote being compared
  // Push: positions queued for one batch request (at most
  // KOReaderSyncClient::MAX_PROGRESS_BATCH, a few hundred bytes each), with
  // their books. Sent when full and when the push ends.
  std::vector<KOReaderProgress> progressBatch;
  std::vector<std::string> progressBatchPaths;
  bool progressBatchUnsupported = false;  // the server has no batch route: one PUT per book
  uint32_t pulledApplied = 0;
  uint32_t pulledParked = 0;
  ReadingSyncUpload::GlobalStatsStep globalFailedStep = ReadingSyncUpload::GlobalStatsStep::None;
  StatsUploadClient::Result globalResult = StatsUploadClient::Result::Skipped;
  std::unique_ptr<FolderBookIterator> folderBooks;
  bool globalAttempted = false;
  bool clippingPullStarted = false;
  bool clippingPullFinished = false;
  bool clippingCancelled = false;
  static bool cancelClippings(void* context);
  bool singleBookTaken = false;
  bool ownsWifi = false;
  bool initialConfirm = false;
  std::string message;
  NextBook nextBook(std::string& bookPath);
  const char* title() const;
  bool singleBook() const { return scope == Scope::Book; }
  bool targeted() const { return scope == Scope::ReadingStats || scope == Scope::Clippings; }
  void start();
  void uploadNext();
  // `remote`: a pulled change to apply instead of fetching (incremental pull).
  void syncEpub(std::string&& bookPath, const KOReaderProgressChange* remote = nullptr);
  void addBookStats(const std::string& bookPath);
  bool flushBookStats();
  void uploadClippings(const std::string& bookPath);
  bool startTargeted(bool stats);
  void finishCatchup();
  NextBook nextPending(std::string& bookPath, uint8_t bit);
  std::string scopePath() const;
  void planIncremental();
  void startLegacyWalk();
  void advanceFlow();
  void finishScopeCatchup(bool stats);
  void finishFirstWalk();
  void walkStep();
  void pullStep();
  void loadPage();
  bool matchPage();
  // Parks a pulled change; on a write failure, fails the run before its cursor moves.
  bool keepRemote(const KOReaderProgressChange& change);
  void pushStep();
  // True when the book's progress went to a per-book comparison (child activity).
  bool pushProgress(const std::string& bookPath);
  // Sends the queued positions; marks each book uploaded or failed.
  void flushProgressBatch();
  void bookDone(const std::string& bookPath);
  bool hasSyncableExtras(const std::string& bookPath) const;
  // False only when the book definitely has no usable saved position.
  static bool hasUsableSavedPosition(const std::string& bookPath);
  static bool collectChange(const KOReaderProgressChange& change, void* context);
  bool uploadBookExtras(const std::string& bookPath, bool* sentAny = nullptr);
  void leave();
  void recordExtras(bool statsOk, bool clippingsOk, bool statsError, bool clippingsError);
  void fail(const char* text);
  // Counts a failed book and keeps its details for the summary; the run goes on.
  void failBook(const std::string& bookPath, uint8_t failedParts, std::string reason = {}, std::string title = {});
  void setCurrentBook(const std::string& bookPath);
  void reviewNext();
  static uint32_t countFolderBooks(const std::string& root);
  uint32_t countPendingInScope() const;
  void finishUpload();
  void closeTransfer();
};
