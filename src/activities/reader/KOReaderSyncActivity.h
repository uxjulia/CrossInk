#pragma once
#include <Epub.h>

#include <functional>
#include <memory>
#include <optional>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "network/ReadingSyncUpload.h"

struct Rect;

/**
 * Activity for syncing reading progress with KOReader sync server.
 *
 * Flow:
 * 1. Connect to WiFi (if not connected)
 * 2. Calculate document hash
 * 3. Fetch remote progress
 * 4. Show comparison and options (Apply/Upload)
 * 5. Apply or upload progress
 */
class KOReaderSyncActivity final : public Activity {
 public:
  static constexpr const char* NAME = "KOReaderSync";

  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const uint8_t readerOrientation)
      : Activity(NAME, renderer, mappedInput),
        currentSpineIndex(0),
        currentPage(0),
        totalPagesInSpine(1),
        primaryMatchMethod(DocumentMatchMethod::FILENAME),
        remoteProgress{},
        remotePosition{},
        localProgress{},
        restartBeforeNetwork(true),
        readerOrientation(readerOrientation) {}

  explicit KOReaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath,
                                DocumentMatchMethod matchMethod, const uint8_t readerOrientation,
                                bool folderSync = false, bool includeGlobalStats = true)
      : Activity(NAME, renderer, mappedInput),
        epubPath(std::move(epubPath)),
        currentSpineIndex(0),
        currentPage(0),
        totalPagesInSpine(1),
        primaryMatchMethod(matchMethod),
        remoteProgress{},
        remotePosition{},
        localProgress{},
        localProgressDeferred(true),
        folderSync(folderSync),
        includeGlobalStats(includeGlobalStats),
        readerOrientation(readerOrientation) {}

  // Bulk sync behind its own progress screen: draw nothing and never wait for
  // input. An Ask on conflicts choice is kept (parked) for the end of the run,
  // and a failure is reported in the result instead of a Skip book screen.
  void setBackground() { background = true; }

  // Incremental bulk sync: apply a position the change feed already delivered
  // (no progress request), and leave stats and clippings to the push.
  void setRemoteProgress(KOReaderProgress remote) {
    injectedRemote = std::move(remote);
    progressOnly = true;
  }

  // A server position in this reader's layout, refined against the section
  // cache. `valid` is false when it cannot be mapped (re-optimize the EPUB).
  // Reads section caches, so the caller must keep the render task off the book.
  static CrossPointPosition mapRemotePosition(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer,
                                              const KOReaderProgress& remote, DocumentMatchMethod method,
                                              int localSpineIndex, int localSpinePages);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == SYNCING || state == UPLOADING; }
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    CONNECTING,
    SYNCING,
    SHOWING_RESULT,
    UPLOADING,
    UPLOAD_COMPLETE,
    SYNC_COMPLETE,
    SYNC_FAILED,
    NO_CREDENTIALS
  };

  std::shared_ptr<Epub> epub;  // released between pre-network mapping and TLS
  std::string epubPath;
  std::string localChapterName;
  int currentSpineIndex;
  int currentPage;
  int totalPagesInSpine;
  std::optional<uint16_t> currentParagraphIndex;
  DocumentMatchMethod primaryMatchMethod;

  State state = WIFI_SELECTION;
  ScreenTransitionRefresh screenTransitionRefresh;
  std::string statusMessage;
  std::string documentHash;

  // Remote progress data
  KOReaderProgress remoteProgress;
  CrossPointPosition remotePosition;

  // Resolved before Wi-Fi on a fresh single-book network boot; deferred in batches.
  KOReaderPosition localProgress;
  bool localProgressDeferred = false;
  bool restartBeforeNetwork = false;
  bool folderSync = false;
  bool includeGlobalStats = true;
  std::optional<KOReaderProgress> injectedRemote;
  bool progressOnly = false;
  bool syncSucceeded = false;
  bool progressSucceeded = false;
  bool progressFailed = false;
  bool background = false;  // the progress step failed (as opposed to stats or clippings)
  std::string bookTitle;    // from the EPUB once loaded; failure screens fall back to the file name
  bool extrasAttempted = false;
  bool exitingBatch = false;
  ReadingSyncUpload::ExtrasResult extrasResult;
  ReadingSyncUpload::GlobalStatsStep globalStatsFailedStep = ReadingSyncUpload::GlobalStatsStep::None;
  StatsUploadClient::Result globalStatsResult = StatsUploadClient::Result::Skipped;
  ProgressSyncResult syncResult() const;
  // The reader can use a book-specific orientation that its teardown restores
  // before this activity gets control. Keep that one value through the
  // lightweight network reboot so every sync screen matches the book.
  uint8_t readerOrientation = CrossPointSettings::ORIENTATION_COUNT;

  // Selection in result screen (0=Apply, 1=Upload, 2=Skip in bulk sync);
  // in a bulk sync's no-remote screen (0=Upload, 1=Skip).
  int selectedOption = 0;

  // Timed return for successful smart-sync terminal states.
  unsigned long autoReturnAt = 0;
  static constexpr unsigned long AUTO_RETURN_DELAY_MS = 1200;

  // Tracks whether this session activated WiFi. Set in onEnter past the credentials
  // check; checked in onExit to decide whether to silent-reboot. Can't rely on
  // WiFi.getMode() because performUpload() calls esp_wifi_stop() on the way out,
  // which makes WiFi.getMode() return WIFI_MODE_NULL.
  bool wifiActivated = false;
  bool lockInitialConfirmRelease = false;
  bool touchOverrideActive = false;

  void onWifiSelectionComplete(bool success);
  void performSync();
  void finishComparison(KOReaderSyncClient::Error result);
  std::optional<KOReaderMetadata> preparedMetadata;
  void performUpload();
  bool uploadExtras();
  // Per-data result rows; `withProgress` adds a Progress row (failure screen).
  void drawExtrasResults(Rect textArea, int y, bool withProgress = false) const;
  bool consumeInitialConfirmRelease();
  bool smartSyncEnabled() const;
  void markAutoReturn();
  void completeAlreadySynced();
  void ensureEpubLoaded();
  bool ensureLocalProgressLoaded();
  void saveProgressAndReturn(const CrossPointPosition& position);
  void returnToSource();
  // Bulk sync only: Skip book keeps the batch going, Exit stops all of it.
  void skipBook();
  // Background bulk sync: keep a conflict for the end of the run.
  void deferChoice();
  // Background bulk sync: report a failure and let the run continue.
  void finishFailed();
  void parkRemote();
  bool batchExitRequested();
  void exitBatch();
};
