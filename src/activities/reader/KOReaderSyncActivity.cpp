#include "KOReaderSyncActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cassert>
#include <cmath>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub/EpubRenderMode.h"
#include "Epub/Section.h"
#include "EpubReaderActivity.h"
#include "EpubReaderUtils.h"
#include "HalClock.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/ProgressSyncComparison.h"
#include "components/TouchActionButtons.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/ClippingsUpload.h"
#include "network/ReadingSyncUpload.h"
#include "network/WifiUtils.h"
#include "sync/BookSyncFile.h"
#include "sync/SyncParked.h"
#include "sync/SyncPending.h"

namespace {
TouchActionButtons::Layout bottomActionLayout(const Rect& screen, const ThemeMetrics& metrics,
                                              const uint8_t buttonCount) {
  const int totalHeight =
      TouchActionButtons::kDefaultHeight * buttonCount + TouchActionButtons::kDefaultGap * (buttonCount - 1);
  const Rect container{screen.x + metrics.contentSidePadding,
                       screen.y + screen.height - metrics.verticalSpacing - totalHeight,
                       std::max(1, screen.width - metrics.contentSidePadding * 2), totalHeight};
  return TouchActionButtons::vertical(container, buttonCount);
}

std::string calculateDocumentHashForMethod(const std::string& path, const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                 : KOReaderDocumentId::calculate(path);
}

const char* matchMethodName(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? "filename" : "binary";
}

void syncTimeWithNTP() {
#ifndef SIMULATOR
  if (!halClock.syncSystemTimeFromNTP()) {
    LOG_DBG("KOSync", "NTP sync unavailable, using fallback");
  }
#endif
}

void wifiOff() {
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
}
}  // namespace

void KOReaderSyncActivity::ensureEpubLoaded() {
  if (!epub) {
    epub = makeUniqueNoThrow<Epub>(epubPath, "/.crosspoint");
    if (!epub) {
      LOG_ERR("KOSync", "Cannot allocate EPUB for sync");
      return;
    }
    epub->setupCacheDir();
    // Load metadata only (no CSS needed for progress mapping, don't rebuild if cache is missing).
    if (!epub->load(false, true, Epub::XLocationLoadMode::Immediate, true)) {
      LOG_ERR("KOSync", "Failed to load epub for progress mapping");
      epub.reset();
      return;
    }
    // Kept for failure screens, which may show after the EPUB is released.
    if (bookTitle.empty()) bookTitle = epub->getTitle();
  }
}

bool KOReaderSyncActivity::ensureLocalProgressLoaded() {
  if (!localProgressDeferred) return localProgress.valid;

  ensureEpubLoaded();
  if (!epub) return false;

  EpubReaderUtils::Progress progress;
  if (EpubReaderUtils::loadProgress(*epub, progress, "KOSync")) {
    currentSpineIndex = progress.spineIndex;
    currentPage = progress.pageNumber;
    if (progress.hasPageCount) totalPagesInSpine = std::max(1, progress.pageCount);
  }

  if (currentSpineIndex < 0 || currentSpineIndex >= epub->getSpineItemsCount()) currentSpineIndex = 0;
  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPagesInSpine};
  if (progress.hasVisibleTextOffset) {
    localPos.visibleTextOffset = progress.visibleTextOffset;
    localPos.hasVisibleTextOffset = true;
  }
  const PositionCoordinateSpace coordinateSpace = primaryMatchMethod == DocumentMatchMethod::FILENAME
                                                      ? PositionCoordinateSpace::SourceDocument
                                                      : PositionCoordinateSpace::CurrentDocument;
  localProgress = ProgressMapper::toKOReader(epub, localPos, coordinateSpace);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  localChapterName = tocIdx >= 0 ? epub->getTocItem(tocIdx).title : "";
  localProgressDeferred = false;
  return localProgress.valid;
}

void KOReaderSyncActivity::saveProgressAndReturn(const CrossPointPosition& position) {
  // epub is guaranteed non-null here: ensureEpubLoaded() was called in performSync() before
  // SHOWING_RESULT state is entered, and this method is only called from that state.
  assert(epub);
  // A bulk sync's Exit must not still change this book's saved position.
  if (batchExitRequested()) {
    exitBatch();
    return;
  }
  const int pageCount = std::max(position.totalPages, position.pageNumber + 1);
  if (pageCount != position.totalPages) {
    LOG_DBG("KOSync", "Adjusted remote page count before save: page=%d count=%d -> %d", position.pageNumber,
            position.totalPages, pageCount);
  }
  const std::optional<uint32_t> visibleTextOffset =
      position.hasVisibleTextOffset ? std::optional<uint32_t>(position.visibleTextOffset) : std::nullopt;
  if (!EpubReaderUtils::saveProgress(*epub, position.spineIndex, position.pageNumber, pageCount, visibleTextOffset)) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      progressFailed = true;
      statusMessage = tr(STR_SAVE_PROGRESS_FAILED);
    }
    requestUpdate(true);
    return;
  }
  RecentBookProgress::saveCachedEpubPercent(*epub, position.spineIndex, position.pageNumber, pageCount);
  progressSucceeded = true;
  if (!uploadExtras()) return;
  syncSucceeded = true;
  returnToSource();
}

void KOReaderSyncActivity::returnToSource() {
  if (folderSync) {
    if (syncSucceeded) {
      setResult(syncResult());
    } else {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
    }
    finish();
    return;
  }
  const PendingOverlayResume& resume = APP_STATE.pendingOverlayResume;
  if (resume.origin == PendingOverlayOrigin::FileBrowser && resume.valid()) {
    activityManager.goToFileBrowser(resume.fileBrowserPath);
  } else if (resume.origin == PendingOverlayOrigin::Library && resume.valid()) {
    activityManager.goToLibrary();
  } else {
    activityManager.goToReader(epubPath, false, false, true);
  }
}

void KOReaderSyncActivity::skipBook() {
  LOG_INF("KOSync", "Skipping bulk sync book: %s", epubPath.c_str());
  // Stats and clippings do not depend on the progress choice.
  if (!uploadExtras()) return;
  parkRemote();
  ProgressSyncResult result = syncResult();
  result.skipped = true;
  setResult(result);
  finish();
}

void KOReaderSyncActivity::deferChoice() {
  LOG_INF("KOSync", "Conflict kept for the end of the bulk sync: %s", epubPath.c_str());
  // Stats and clippings do not depend on the choice.
  if (!uploadExtras()) return;
  parkRemote();
  ProgressSyncResult result = syncResult();
  result.deferred = true;
  setResult(std::move(result));
  finish();
}

void KOReaderSyncActivity::finishFailed() {
  LOG_INF("KOSync", "Bulk sync book failed: %s (%s)", epubPath.c_str(), statusMessage.c_str());
  parkRemote();
  ProgressSyncResult result = syncResult();
  result.progressFailed = progressFailed;
  result.reason = std::move(statusMessage);
  setResult(std::move(result));
  finish();
}

void KOReaderSyncActivity::parkRemote() {
  // Keep the remote position this book was not reconciled with, so a later
  // push compares with it instead of overwriting it. (A pulled change is
  // parked by the bulk sync itself.)
  if (progressOnly || documentHash.empty() || remoteProgress.progress.empty()) return;
  KOReaderProgressChange kept;
  kept.document = documentHash;
  kept.percentage = remoteProgress.percentage;
  kept.progress = remoteProgress.progress;
  kept.position = remoteProgress.position;
  kept.device = remoteProgress.device;
  kept.timestamp = remoteProgress.timestamp;
  if (!SyncParked::park(kept)) LOG_ERR("KOSync", "Cannot keep the remote position: %s", epubPath.c_str());
}

bool KOReaderSyncActivity::batchExitRequested() {
  if (!folderSync || exitingBatch) return exitingBatch;
  // Bulk work runs synchronously between frames, so sample input here. A held
  // Exit counts too: a quick press during a request is otherwise missed.
  mappedInput.update();
  return mappedInput.isPressed(MappedInputManager::Button::Back) ||
         mappedInput.wasPressed(MappedInputManager::Button::Back);
}

void KOReaderSyncActivity::exitBatch() {
  if (exitingBatch) return;  // Already finishing; never set a second result.
  LOG_INF("KOSync", "Bulk sync exited at: %s", epubPath.c_str());
  exitingBatch = true;
  mappedInput.suppressNextBackRelease();
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

bool KOReaderSyncActivity::consumeInitialConfirmRelease() {
  if (!lockInitialConfirmRelease) {
    return false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
    lockInitialConfirmRelease = false;
  }
  return true;
}

bool KOReaderSyncActivity::smartSyncEnabled() const {
  return KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART;
}

void KOReaderSyncActivity::markAutoReturn() { autoReturnAt = millis() + AUTO_RETURN_DELAY_MS; }

void KOReaderSyncActivity::completeAlreadySynced() {
  progressSucceeded = true;
  if (!uploadExtras()) return;
  syncSucceeded = true;
  if (folderSync) {
    returnToSource();
    return;
  }
  {
    RenderLock lock(*this);
    state = SYNC_COMPLETE;
  }
  markAutoReturn();
  requestUpdate(true);
}

void KOReaderSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_DBG("KOSync", "WiFi connection failed, exiting");
    returnToSource();
    return;
  }

  WiFi.setSleep(false);
  LOG_DBG("KOSync", "WiFi sleep disabled for sync");

  sdFontSystem.releaseForNetwork(renderer);

  {
    RenderLock lock(*this);
    state = SYNCING;
    statusMessage = tr(STR_SYNCING_TIME);
  }
  requestUpdate(true);

  // The folder batch synchronizes time once, with its first book.
  if (!folderSync || includeGlobalStats) syncTimeWithNTP();

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_CALC_HASH);
  }
  requestUpdate(true);

  performSync();
}

CrossPointPosition KOReaderSyncActivity::mapRemotePosition(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer,
                                                           const KOReaderProgress& remote,
                                                           const DocumentMatchMethod method, const int localSpineIndex,
                                                           const int localSpinePages) {
  const PositionCoordinateSpace remoteCoordinateSpace = method == DocumentMatchMethod::FILENAME
                                                            ? PositionCoordinateSpace::SourceDocument
                                                            : PositionCoordinateSpace::CurrentDocument;
  const auto renderMode = static_cast<EpubRenderMode>(EpubReaderActivity::loadBookRenderMode(*epub));
  const char* cacheSuffix = sectionCacheSuffixForRenderMode(renderMode);
  CrossPointPosition remotePosition{};
  bool usedRichPosition = false;
  // The client accepts rich positions only for profiles that support them.
  // Filename matching still needs source-document mapping because optimized books can diverge.
  if (remoteCoordinateSpace == PositionCoordinateSpace::CurrentDocument && remote.position.has_value()) {
    const auto richMapped = ProgressMapper::fromRichPosition(epub, *remote.position, renderer, cacheSuffix);
    if (richMapped.has_value()) {
      remotePosition = *richMapped;
      usedRichPosition = true;
    }
  }
  if (!usedRichPosition) {
    const KOReaderPosition koPos = {remote.progress, remote.percentage};
    remotePosition = ProgressMapper::toCrossPoint(epub, koPos, localSpineIndex, localSpinePages, remoteCoordinateSpace);
  }
  if (!remotePosition.valid) return remotePosition;

  // Refine page using the content-offset LUT first, then structural anchors.
  // A partial cache deliberately returns no page for an offset outside its
  // watermark; preserving that offset lets the reader index through to it.
  if (!usedRichPosition && (remotePosition.hasVisibleTextOffset || remotePosition.hasLiIndex ||
                            remotePosition.xpathAnchorId[0] != '\0' || remotePosition.hasParagraphIndex)) {
    Section tempSection(epub, remotePosition.spineIndex, renderer, cacheSuffix);
    bool refined = false;
    if (remotePosition.hasVisibleTextOffset) {
      const auto contentPage = tempSection.getPageForVisibleTextOffset(remotePosition.visibleTextOffset, true);
      if (contentPage.has_value()) {
        LOG_DBG("KOSync", "Visible offset %lu -> page %d (was %d)",
                static_cast<unsigned long>(remotePosition.visibleTextOffset), *contentPage, remotePosition.pageNumber);
        remotePosition.pageNumber = *contentPage;
        refined = true;
      } else {
        LOG_DBG("KOSync", "Visible offset %lu is beyond the cached section watermark",
                static_cast<unsigned long>(remotePosition.visibleTextOffset));
      }
    }
    if (!refined && remotePosition.hasLiIndex) {
      const auto liPage = tempSection.getPageForListItemIndex(remotePosition.liIndex);
      if (liPage.has_value()) {
        LOG_DBG("KOSync", "Li index %u -> page %d (was %d)", remotePosition.liIndex, *liPage,
                remotePosition.pageNumber);
        remotePosition.pageNumber = *liPage;
        refined = true;
      } else {
        LOG_DBG("KOSync", "Li index %u not found in section LUT", remotePosition.liIndex);
      }
    }
    if (!refined && remotePosition.xpathAnchorId[0] != '\0') {
      const auto anchorPage = tempSection.getPageForAnchor(std::string(remotePosition.xpathAnchorId));
      if (anchorPage.has_value()) {
        LOG_DBG("KOSync", "Anchor '%s' -> page %d (was %d)", remotePosition.xpathAnchorId, *anchorPage,
                remotePosition.pageNumber);
        remotePosition.pageNumber = *anchorPage;
        refined = true;
      } else {
        LOG_DBG("KOSync", "Anchor '%s' not found in section cache", remotePosition.xpathAnchorId);
      }
    }
    if (!refined && remotePosition.hasParagraphIndex) {
      const auto paragraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex);
      const auto nextParagraphPage = tempSection.getPageForParagraphIndex(remotePosition.paragraphIndex + 1);
      if (paragraphPage.has_value()) {
        int refinedPage = std::max(remotePosition.pageNumber, static_cast<int>(*paragraphPage));
        if (nextParagraphPage.has_value()) {
          const int lutSpan = static_cast<int>(*nextParagraphPage) - static_cast<int>(*paragraphPage);
          // Keep the percentage-derived page inside the paragraph's cached page range.
          // A one-page paragraph should not allow byte-percentage drift to jump to later paragraphs.
          if (lutSpan > 0 && refinedPage >= static_cast<int>(*nextParagraphPage)) {
            refinedPage = static_cast<int>(*nextParagraphPage) - 1;
          }
        }
        char nextParaBuf[8];
        if (nextParagraphPage.has_value())
          snprintf(nextParaBuf, sizeof(nextParaBuf), "%d", *nextParagraphPage);
        else
          snprintf(nextParaBuf, sizeof(nextParaBuf), "none");
        LOG_DBG("KOSync", "Paragraph %u -> LUT page %d, nextPara page %s, intra page %d, using %d",
                remotePosition.paragraphIndex, *paragraphPage, nextParaBuf, remotePosition.pageNumber, refinedPage);
        remotePosition.pageNumber = refinedPage;
      } else {
        LOG_DBG("KOSync", "Paragraph %u not found in section LUT", remotePosition.paragraphIndex);
      }
    }
  }
  return remotePosition;
}

void KOReaderSyncActivity::performSync() {
  // The configured method goes through the book's sync record, which caches
  // the id and records the book in sync_ids.bin for the progress pull.
  documentHash = primaryMatchMethod == KOREADER_STORE.getMatchMethod()
                     ? BookSyncFile::documentId(epubPath)
                     : calculateDocumentHashForMethod(epubPath, primaryMatchMethod);
  if (documentHash.empty()) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      progressFailed = true;
      statusMessage = tr(STR_HASH_FAILED);
    }
    requestUpdate(true);
    return;
  }
  if (batchExitRequested()) {
    exitBatch();
    return;
  }

  {
    RenderLock lock(*this);
    statusMessage = tr(STR_FETCH_PROGRESS);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("KOSync", "Fetch progress screen could not be rendered synchronously; aborting sync");
    if (!folderSync) wifiOff();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      progressFailed = true;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

  // Fetch remote progress under the configured matching method only. Another
  // method's hash can belong to a different file version (an optimized EPUB
  // has different bytes), so its position may land far from the right place.
  auto result = KOReaderSyncClient::OK;
  if (injectedRemote) {
    remoteProgress = std::move(*injectedRemote);
    injectedRemote.reset();
  } else {
    result = KOReaderSyncClient::getProgress(documentHash, remoteProgress);
  }
  LOG_DBG("KOSync", "Remote (%s): result=%d http=%d doc=%s remote=%.6f xpath=%s", matchMethodName(primaryMatchMethod),
          result, KOReaderSyncClient::lastHttpCode, documentHash.c_str(), remoteProgress.percentage,
          remoteProgress.progress.c_str());

  // Folder sync resolves here with Wi-Fi already active. Single-book sync keeps
  // only its precomputed pointer through the network request.
  if (!ensureLocalProgressLoaded()) {
    LOG_ERR("KOSync", "Failed to reconstruct local progress after network boot");
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      progressFailed = true;
      statusMessage = localProgress.needsReoptimization ? tr(STR_SYNC_REOPTIMIZE_REQUIRED) : tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

  if (result == KOReaderSyncClient::NOT_FOUND) {
    // Nothing to conflict with, so neither sync behavior asks.
    LOG_DBG("KOSync", "No remote progress found; uploading local %.6f", localProgress.percentage);
    performUpload();
    return;
  }

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      progressFailed = true;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate(true);
    return;
  }

  ensureEpubLoaded();
  if (!epub) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      progressFailed = true;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

  remotePosition =
      mapRemotePosition(epub, renderer, remoteProgress, primaryMatchMethod, currentSpineIndex, totalPagesInSpine);
  if (!remotePosition.valid) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      progressFailed = true;
      statusMessage = tr(STR_SYNC_REOPTIMIZE_REQUIRED);
    }
    requestUpdate(true);
    return;
  }

  static constexpr float SAME_PROGRESS_EPSILON = 0.001f;  // 0.1 percentage points
  const float delta = localProgress.percentage - remoteProgress.percentage;
  LOG_DBG("KOSync", "Sync decision: doc=%s local=%.6f remote=%.6f delta=%.6f remoteXpath=%s mapped=%d/%d",
          documentHash.c_str(), localProgress.percentage, remoteProgress.percentage, delta,
          remoteProgress.progress.c_str(), remotePosition.spineIndex, remotePosition.pageNumber);
  // Matching positions are not a conflict: neither sync behavior asks.
  if (std::fabs(delta) <= SAME_PROGRESS_EPSILON) {
    completeAlreadySynced();
    return;
  }

  if (smartSyncEnabled()) {
    if (delta > 0) {
      performUpload();
      return;
    }

    saveProgressAndReturn(remotePosition);
    return;
  }
  if (background) {
    deferChoice();
    return;
  }
  {
    RenderLock lock(*this);
    state = SHOWING_RESULT;

    // Default to the option that corresponds to the furthest progress
    if (localProgress.percentage > remoteProgress.percentage) {
      selectedOption = 1;  // Upload local progress
    } else {
      selectedOption = 0;  // Apply remote progress
    }
  }
  requestUpdate(true);
}

void KOReaderSyncActivity::performUpload() {
  if (batchExitRequested()) {
    exitBatch();
    return;
  }
  {
    RenderLock lock(*this);
    state = UPLOADING;
    statusMessage = tr(STR_UPLOAD_PROGRESS);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("KOSync", "Upload progress screen could not be rendered synchronously; aborting upload");
    if (!folderSync) wifiOff();
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      progressFailed = true;
      statusMessage = tr(STR_SYNC_FAILED_MSG);
    }
    requestUpdate(true);
    return;
  }

  if (epub) {
    epub.reset();
  }

  // localProgress was pre-computed in EpubReaderActivity before the Epub was released.
  KOReaderProgress progress;
  progress.document = documentHash;
  progress.progress = localProgress.xpath;
  progress.percentage = localProgress.percentage;
  progress.device = SETTINGS.getEffectiveDeviceName();

  // Rich CrossPoint position for the default CrossPoint sync server (lossless
  // CrossPoint<->CrossPoint sync). The HTTP client also enforces this boundary
  // before serializing the extension.
  if (KOREADER_STORE.supportsRichProgress()) {
    KOReaderRichPosition pos;
    const float pct = localProgress.percentage < 0.0f   ? 0.0f
                      : localProgress.percentage > 1.0f ? 1.0f
                                                        : localProgress.percentage;
    pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
    pos.spineIndex = static_cast<uint16_t>(currentSpineIndex);
    pos.pageNumber = static_cast<uint16_t>(currentPage);
    pos.totalPages = static_cast<uint16_t>(totalPagesInSpine > 0 ? totalPagesInSpine : 1);
    pos.paragraphIndex = currentParagraphIndex;
    pos.xpath = localProgress.xpath;
    progress.position = std::move(pos);
  }

  // Optionally include document metadata (KOReader PR #15306)
  if (KOREADER_STORE.getSendMetadata()) {
    // The Epub is released before the sync network calls and is only reloaded on the
    // remote-progress path (performSync). When uploading without remote progress the
    // Epub is still null, so reload it here and guard the title/author reads to avoid
    // dereferencing a null Epub. Filename is derived from the path and is always safe.
    ensureEpubLoaded();
    KOReaderMetadata meta;
    const auto lastSlash = epubPath.rfind('/');
    meta.filename = (lastSlash != std::string::npos) ? epubPath.substr(lastSlash + 1) : epubPath;
    if (epub) {
      meta.title = epub->getTitle();
      meta.authors = epub->getAuthor();
      meta.loadExtended(*epub);
    } else {
      LOG_ERR("KOSync", "Epub unavailable for metadata; sending filename only");
    }
    progress.metadata = std::move(meta);
  }

  // Release the Epub before the network call so the TLS handshake has enough free heap
  // (consistent with the release-before-sync pattern in performSync); nothing below needs it.
  epub.reset();

  const auto result = KOReaderSyncClient::updateProgress(progress);

  if (result == KOReaderSyncClient::OK) {
    progressSucceeded = true;
    if (!uploadExtras()) return;
  }

  // Drop the radio while user reads the result; full teardown happens at silent reboot.
  if (!folderSync) wifiOff();

  if (result != KOReaderSyncClient::OK) {
    {
      RenderLock lock(*this);
      state = SYNC_FAILED;
      progressFailed = true;
      statusMessage = KOReaderSyncClient::errorString(result);
    }
    requestUpdate();
    return;
  }

  syncSucceeded = true;
  if (folderSync) {
    returnToSource();
    return;
  }
  {
    RenderLock lock(*this);
    state = UPLOAD_COMPLETE;
  }
  if (smartSyncEnabled()) {
    markAutoReturn();
  }
  requestUpdate(true);
}

ProgressSyncResult KOReaderSyncActivity::syncResult() const {
  ProgressSyncResult result{syncSucceeded,
                            progressSucceeded,
                            extrasResult.stats == StatsUploadClient::Result::Ok,
                            extrasResult.clippings == StatsUploadClient::Result::Ok,
                            StatsUploadClient::failed(extrasResult.stats),
                            StatsUploadClient::failed(extrasResult.clippings)};
  result.title = bookTitle;
  return result;
}

void KOReaderSyncActivity::drawExtrasResults(const Rect textArea, int y, const bool withProgress) const {
  const auto drawRow = [&](const char* title, const char* status) {
    y += UITheme::drawCenteredStatusRow(renderer, textArea, UI_10_FONT_ID, y, title, status) + 8;
  };
  if (withProgress) {
    drawRow(tr(STR_STATS_PROGRESS_LBL), progressSucceeded ? tr(STR_DONE)
                                        : progressFailed  ? tr(STR_SYNC_FAILED_MSG)
                                                          : tr(STR_NONE_OPT));
    // A failure before stats and clippings were tried has no results for them.
    if (!extrasAttempted) return;
  }
  // Folder runs report overall stats once in their own summary, not per book.
  if (includeGlobalStats) {
    char status[128];
    drawRow(tr(STR_ALL_TIME_STATS),
            ReadingSyncUpload::globalStatusLabel(status, sizeof(status), globalStatsResult, globalStatsFailedStep));
  }
  drawRow(tr(STR_READING_STATS), ReadingSyncUpload::statusLabel(extrasResult.stats, false));
  drawRow(tr(STR_CLIPPINGS), ReadingSyncUpload::statusLabel(extrasResult.clippings, true));
}

bool KOReaderSyncActivity::uploadExtras() {
  if (progressOnly) return true;  // the bulk sync's push sends stats and clippings
  if (extrasAttempted) return !StatsUploadClient::failed(globalStatsResult) && extrasResult.success();
  if (batchExitRequested()) {
    exitBatch();
    return false;
  }
  extrasAttempted = true;
  if (!KOREADER_STORE.getSyncStats() && !KOREADER_STORE.getSyncClippings()) return true;
  {
    RenderLock lock(*this);
    state = UPLOADING;
    epub.reset();
    statusMessage = tr(STR_LOADING);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("KOSync", "Cannot render extras upload screen");
  }
  if (includeGlobalStats && KOREADER_STORE.getSyncStats()) {
    ReadingSyncUpload::GlobalStatsStep failedStep = ReadingSyncUpload::GlobalStatsStep::None;
    const auto result = ReadingSyncUpload::globalStats(&failedStep);
    RenderLock lock(*this);
    globalStatsResult = result;
    globalStatsFailedStep = failedStep;
  }
  extrasResult = ReadingSyncUpload::extras(
      epubPath, documentHash,
      [](void* context) {
        auto& self = *static_cast<KOReaderSyncActivity*>(context);
        self.mappedInput.update();
        return self.mappedInput.isPressed(MappedInputManager::Button::Back) ||
               self.mappedInput.wasPressed(MappedInputManager::Button::Back) ||
               TouchHeaderBackButton::wasTapped(self.mappedInput, self.renderer);
      },
      this);
  if (extrasResult.clippings == StatsUploadClient::Result::Cancelled) {
    exitBatch();
    return false;
  }
  if (!StatsUploadClient::failed(globalStatsResult) && extrasResult.success()) return true;
  if (!folderSync) wifiOff();
  {
    RenderLock lock(*this);
    state = SYNC_FAILED;
    statusMessage.clear();  // the heading and result rows say what failed
  }
  requestUpdate(true);
  return false;
}

void KOReaderSyncActivity::onEnter() {
  Activity::onEnter();

  // Sync is a reader-originated activity, but its decision prompts are not
  // reader content. Keep their touch actions available even when the reader's
  // tap controls are disabled.
  if (mappedInput.hasTouchHardware()) {
    mappedInput.setReaderTouchscreenOverride(true);
    touchOverrideActive = true;
  }

  // The reader uses this activity as a tiny handoff so ActivityManager can run
  // reader onExit() before rebooting. Network boot uses the other constructor.
  if (restartBeforeNetwork) {
    const bool hasReaderOrientation = readerOrientation < CrossPointSettings::ORIENTATION_COUNT;
    if (hasReaderOrientation) ReaderUtils::applyOrientation(renderer, readerOrientation);
    // Zero means no reader override; valid orientations are encoded one-based.
    const uint32_t orientationPayload = hasReaderOrientation ? static_cast<uint32_t>(readerOrientation) + 1 : 0;
    silentRestartToNetwork(NetworkBootTarget::KOREADER_SYNC, orientationPayload);
    return;
  }

  LOG_INF("KOSync", "network entry free=%u maxAlloc=%u stack=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  uint8_t syncOrientation =
      readerOrientation < CrossPointSettings::ORIENTATION_COUNT ? readerOrientation : SETTINGS.orientation;
  const PendingOverlayResume& resume = APP_STATE.pendingOverlayResume;
  if (!folderSync && resume.origin == PendingOverlayOrigin::Reader &&
      resume.overlay == PendingOverlayType::FrontlightDrawer && resume.preserveReaderOrientation &&
      resume.readerOrientation < CrossPointSettings::ORIENTATION_COUNT) {
    syncOrientation = resume.readerOrientation;
  }
  ReaderUtils::applyOrientation(renderer, syncOrientation);
  lockInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);

  if (!localProgressDeferred && !localProgress.valid) {
    LOG_ERR("KOSync", "Source position map unavailable; re-optimize the EPUB before filename-based sync");
    state = SYNC_FAILED;
    progressFailed = true;
    statusMessage = tr(STR_SYNC_REOPTIMIZE_REQUIRED);
    requestUpdate();
    return;
  }

  // Check for credentials first
  if (!KOREADER_STORE.hasCredentials()) {
    state = NO_CREDENTIALS;
    requestUpdate();
    return;
  }

  sdFontSystem.releaseForNetwork(renderer);

  // On a fresh network boot, resolve the saved position before the radio takes
  // heap needed by ZIP inflation and XML parsing. Keep only the small result;
  // retaining EPUB metadata would compete with the TLS handshake instead.
  if (!folderSync && !hasActiveStationWifiConnection()) {
    {
      RenderLock lock(*this);
      state = SYNCING;
      statusMessage = tr(STR_LOADING);
    }
    if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
      LOG_ERR("KOSync", "Local mapping screen could not be rendered; aborting sync");
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        progressFailed = true;
        statusMessage = tr(STR_SYNC_FAILED_MSG);
      }
      requestUpdate(true);
      return;
    }
    LOG_INF("KOSync", "local mapping before WiFi free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    const bool loaded = ensureLocalProgressLoaded();
    epub.reset();
    if (!loaded) {
      LOG_ERR("KOSync", "Cannot resolve saved position before WiFi");
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        progressFailed = true;
        statusMessage = localProgress.needsReoptimization ? tr(STR_SYNC_REOPTIMIZE_REQUIRED) : tr(STR_SYNC_FAILED_MSG);
      }
      requestUpdate(true);
      return;
    }
  }

  // Past this point every path uses WiFi.
  wifiActivated = true;

  // Check if already connected (e.g. from settings page auth)
  if (hasActiveStationWifiConnection()) {
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection subactivity
  LOG_INF("KOSync", "launch WiFi selection free=%u maxAlloc=%u stack=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, true, true),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderSyncActivity::onExit() {
  // Every way out (result, Skip book, Exit, failure) clears exactly what this
  // run settled, before the silent restart below. Progress counts as settled
  // when uploaded, applied from the server, or already in sync.
  uint8_t settled = progressSucceeded ? SyncPending::PROGRESS_BIT : 0;
  if (extrasAttempted && StatsUploadClient::settled(extrasResult.stats)) settled |= SyncPending::STATS_BIT;
  if (extrasAttempted && StatsUploadClient::settled(extrasResult.clippings)) settled |= SyncPending::CLIPPINGS_BIT;
  if (settled) SyncPending::clear(epubPath, settled);
  // Reconciled with the server: a remote position parked for this book is settled too.
  if (progressSucceeded && !documentHash.empty()) SyncParked::remove(documentHash);

  if (touchOverrideActive) {
    mappedInput.setReaderTouchscreenOverride(false);
    touchOverrideActive = false;
  }
  Activity::onExit();

  if (wifiActivated && !folderSync) {
    wifiOff();
    if (APP_STATE.pendingOverlayResume.returnsToBookList()) {
      silentRestart();
    } else {
      silentRestartToReader(true);
    }
  }
}

void KOReaderSyncActivity::render(RenderLock&&) {
  // The bulk sync's progress screen stays up; touching the framebuffer here
  // would only cost a refresh.
  if (background) return;
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouchHardware(), false);

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput, screen);
  const std::string heading = folderSync ? epubPath.substr(epubPath.find_last_of('/') + 1) : tr(STR_SYNC_BOOK);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, heading.c_str(), true);
  } else {
    GUI.drawHeader(renderer, header, heading.c_str());
  }

  int top = screen.width > screen.height
                ? TouchHeaderBackButton::contentTop(renderer, mappedInput, screen.y) + metrics.verticalSpacing
                : screen.y + screen.height / 2 - 40;
  const Rect textArea{screen.x + metrics.contentSidePadding, screen.y, screen.width - metrics.contentSidePadding * 2,
                      screen.height};
  if (state == NO_CREDENTIALS) {
    top += UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, top, tr(STR_NO_CREDENTIALS_MSG), 2, true,
                                            EpdFontFamily::BOLD, 4) +
           16;
    UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, top, tr(STR_KOREADER_SETUP_HINT), 3, true,
                                     EpdFontFamily::BOLD, 4);

    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
    return;
  }

  if (state == SYNCING || state == UPLOADING) {
    UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, top, statusMessage.c_str(), 3, true,
                                     EpdFontFamily::BOLD, 4);
    if (folderSync) {
      // Exit is checked between steps; holding it stops after the current request.
      const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_EXIT)), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    }
    renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
    return;
  }

  if (state == SHOWING_RESULT) {
    const bool hasTouch = mappedInput.hasTouchHardware();
    top = TouchHeaderBackButton::contentTop(renderer, mappedInput, screen.y) + metrics.verticalSpacing;
    top += UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, top, tr(STR_PROGRESS_FOUND), 2, true,
                                            EpdFontFamily::BOLD, 4) +
           metrics.verticalSpacing;

    // Remote chapter name requires Epub (loaded lazily in performSync before this state).
    const int remoteTocIndex = epub->getTocIndexForSpineIndex(remotePosition.spineIndex);
    const std::string remoteChapter =
        (remoteTocIndex >= 0) ? epub->getTocItem(remoteTocIndex).title
                              : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(remotePosition.spineIndex + 1));
    // Local chapter name was pre-computed before Epub was released.
    const std::string localChapter =
        !localChapterName.empty() ? localChapterName
                                  : (std::string(tr(STR_SECTION_PREFIX)) + std::to_string(currentSpineIndex + 1));

    char remotePageStr[48];
    snprintf(remotePageStr, sizeof(remotePageStr), tr(STR_SYNC_PAGE_FORMAT), remotePosition.pageNumber + 1);
    char localPageStr[48];
    snprintf(localPageStr, sizeof(localPageStr), tr(STR_SYNC_PAGE_TOTAL_FORMAT), currentPage + 1, totalPagesInSpine);

    const long remoteTenths = ProgressSyncComparison::displayedTenths(remoteProgress.percentage);
    const long localTenths = ProgressSyncComparison::displayedTenths(localProgress.percentage);
    const ProgressSyncComparison::Card cards[] = {
        {tr(STR_SYNC_REMOTE_TITLE), remoteProgress.device.c_str(), remotePageStr, remoteChapter.c_str(),
         remoteProgress.percentage, remoteTenths > localTenths},
        {tr(STR_STATS_THIS_DEVICE_SCREEN), nullptr, localPageStr, localChapter.c_str(), localProgress.percentage,
         localTenths > remoteTenths},
    };

    ProgressSyncComparison::drawCards(renderer, screen, metrics, top, cards[0], cards[1]);

    const auto actions = ProgressSyncComparison::actionLayout(renderer, screen, metrics, hasTouch, folderSync ? 3 : 2);
    const char* actionLabels[] = {tr(STR_APPLY_REMOTE), tr(STR_UPLOAD_LOCAL), tr(STR_SKIP_BOOK)};
    TouchActionButtons::draw(renderer, actions, actionLabels, selectedOption, selectedOption, UI_10_FONT_ID);

    // Bottom button hints. In a bulk sync Back leaves the whole sync, not this book.
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(folderSync ? tr(STR_EXIT) : tr(STR_BACK)),
                                              tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
    return;
  }

  if (state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    top = screen.width > screen.height
              ? TouchHeaderBackButton::contentTop(renderer, mappedInput, screen.y) + metrics.verticalSpacing
              : screen.y + screen.height / 3;
    top += UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, top,
                                            state == UPLOAD_COMPLETE ? tr(STR_UPLOAD_SUCCESS) : tr(STR_ALREADY_SYNCED),
                                            2, true, EpdFontFamily::BOLD, 4) +
           24;
    // Same per-data lines as the failure and bulk result screens.
    drawExtrasResults(textArea, top);

    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_DONE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
    return;
  }

  if (state == SYNC_FAILED) {
    top = screen.width > screen.height
              ? TouchHeaderBackButton::contentTop(renderer, mappedInput, screen.y) + metrics.verticalSpacing
              : screen.y + screen.height / 3;
    // Name what failed and for which book: a bulk sync shows one of these per book.
    const bool statsFailed = extrasAttempted && (StatsUploadClient::failed(extrasResult.stats) ||
                                                 StatsUploadClient::failed(globalStatsResult));
    const bool clippingsFailed = extrasAttempted && StatsUploadClient::failed(extrasResult.clippings);
    top += UITheme::drawCenteredWrappedText(
               renderer, textArea, UI_10_FONT_ID, top,
               ReadingSyncUpload::failureHeading(progressFailed, statsFailed, clippingsFailed), 2, true,
               EpdFontFamily::BOLD, 4) +
           8;
    const std::string book = bookTitle.empty() ? ReadingSyncUpload::displayName(epubPath) : bookTitle;
    top += UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, top, book.c_str(), 2, true,
                                            EpdFontFamily::REGULAR, 4) +
           16;
    // The reason, e.g. "Sync server returned HTTP 500".
    if (!statusMessage.empty()) {
      top += UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, top, statusMessage.c_str(), 3, true,
                                              EpdFontFamily::REGULAR, 4) +
             16;
    }
    top += 8;
    drawExtrasResults(textArea, top, true);
    if (folderSync && mappedInput.hasTouchHardware()) {
      const auto actions = bottomActionLayout(screen, metrics, 1);
      const char* labels[] = {tr(STR_SKIP_BOOK)};
      TouchActionButtons::draw(renderer, actions, labels, 0);
    }
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(folderSync ? tr(STR_EXIT) : tr(STR_BACK)),
                                              folderSync ? tr(STR_SKIP_BOOK) : "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
    renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
    return;
  }
}

void KOReaderSyncActivity::loop() {
  if (consumeInitialConfirmRelease()) {
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouchHardware(), false);
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput, screen);
  if (TouchHeaderBackButton::wasTapped(mappedInput, header)) {
    returnToSource();
    return;
  }

  if (folderSync && state == SYNC_FAILED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToSource();
      return;
    }
    // A pulled change's book leaves stats and clippings to the bulk sync's push.
    if (!progressOnly && !extrasAttempted && !documentHash.empty()) {
      // A failed progress fetch or coordinate map must not strand independent
      // saved stats/clippings. Back/cancellation above still stops the action.
      std::string progressError = std::move(statusMessage);
      if (!uploadExtras() && exitingBatch) return;
      {
        RenderLock lock(*this);
        state = SYNC_FAILED;
        statusMessage = std::move(progressError);
      }
      requestUpdate();
      return;
    }
    if (background) {
      finishFailed();
      return;
    }
    int x = 0, y = 0;
    const bool skipTapped = mappedInput.wasScreenTapped(x, y) &&
                            TouchActionButtons::indexAt(bottomActionLayout(screen, metrics, 1), x, y) == 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || skipTapped) {
      LOG_INF("KOSync", "Skipping failed folder book: %s", epubPath.c_str());
      parkRemote();
      setResult(syncResult());
      finish();
    }
    return;
  }

  if (state == NO_CREDENTIALS || state == SYNC_FAILED || state == UPLOAD_COMPLETE || state == SYNC_COMPLETE) {
    if (autoReturnAt != 0 && millis() >= autoReturnAt) {
      returnToSource();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      returnToSource();
    }
    return;
  }

  if (state == SHOWING_RESULT) {
    const int optionCount = folderSync ? 3 : 2;
    auto chooseSelected = [this] {
      if (selectedOption == 0) {
        saveProgressAndReturn(remotePosition);
      } else if (selectedOption == 1) {
        performUpload();
      } else if (selectedOption == 2) {
        skipBook();
      }
    };

    {
      const auto actions =
          ProgressSyncComparison::actionLayout(renderer, screen, metrics, mappedInput.hasTouchHardware(), optionCount);
      if (actions.buttons[1].x != actions.buttons[0].x) {
        // The landscape two-row layout is not a single column, so hit-test the tap directly.
        int x = 0, y = 0;
        const int tapped = mappedInput.wasScreenTapped(x, y) ? TouchActionButtons::indexAt(actions, x, y) : -1;
        if (tapped >= 0) {
          selectedOption = tapped;
          chooseSelected();
          return;
        }
      } else {
        const Rect& first = actions.buttons[0];
        int touchedOption = -1;
        const auto touch = mappedInput.rowTouch(touchedOption, first.y, actions.buttons[1].y - first.y, actions.count,
                                                first.x, first.x + first.width, first.height);
        if (touch == MappedInputManager::RowTouch::Down) {
          if (selectedOption != touchedOption) {
            selectedOption = touchedOption;
            requestUpdate();
          }
          return;
        }
        if (touch == MappedInputManager::RowTouch::Tap) {
          selectedOption = touchedOption;
          chooseSelected();
          return;
        }
      }
    }

    // Navigate options
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      selectedOption = (selectedOption + optionCount - 1) % optionCount;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      selectedOption = (selectedOption + 1) % optionCount;
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      chooseSelected();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      returnToSource();
    }
    return;
  }
}
