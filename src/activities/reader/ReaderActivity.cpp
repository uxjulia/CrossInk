#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "ReaderUtils.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isTxtFile(const std::string& path) {
  return FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);  // Treat .md as txt files (until we have a markdown reader)
}

static bool isImagePreviewFile(const std::string& path) {
  return FsHelpers::hasBmpExtension(path) || FsHelpers::hasPngExtension(path);
}

int ReaderActivity::initialRefreshCountdown() const {
  if (!allowFastInitialRefresh) return 0;

  const int refreshFrequency = SETTINGS.getRefreshFrequency();
  return refreshFrequency > 1 ? refreshFrequency : 2;
}

ReaderActivity::EpubOpenResult ReaderActivity::loadEpub(const std::string& path) {
  EpubOpenResult result;
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return result;
  }

  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB object");
    result.failure = Epub::OpenFailure::OutOfMemory;
    return result;
  }
  // Resolve the book orientation before any opening notice, without changing global settings.
  result.readerSettings = EpubReaderActivity::readBookReaderSettings(*epub);
  const auto previousOrientation = renderer.getOrientation();
  const uint8_t bookOrientation = result.readerSettings.readerSettings.orientation;
  ReaderUtils::applyOrientation(
      renderer, bookOrientation < CrossPointSettings::ORIENTATION_COUNT ? bookOrientation : SETTINGS.orientation);

  // First open: building the spine/TOC index (book.bin) takes a couple of seconds. Show the
  // indexing popup so it isn't a silent wait on the home screen. The cachePath/hash is known at
  // construction, so this check is valid before load(); a cached open loads in a blink -> no popup.
  const bool uncached = !Storage.exists((epub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
    // The popup replaces the restored Quick Resume frame, so the reader must clean it.
    allowFastInitialRefresh = false;
    openingFeedback.showIndexing(renderer);
  }
  if (openingFeedback.showIfPending(renderer)) allowFastInitialRefresh = false;
  // Lend the framebuffer's 48 KB for every EPUB load: even a cached book may
  // rebuild stale/missing CSS and need miniz's ~43 KB streaming workspace. The
  // panel keeps showing its last image, and the next activity redraws fully.
  GfxRenderer::FrameBufferLoan loan(renderer);
  const bool loaded = epub->load(
      true, result.readerSettings.hasSafeModeOverride || result.readerSettings.readerSettings.embeddedStyle == 0,
      Epub::XLocationLoadMode::Immediate, true);
  if (loaded) epub->ensureOptimizerImageIndex();
  loan.end();
  if (loaded && openingFeedback.showIfPending(renderer)) allowFastInitialRefresh = false;
  // The reader applies its settings on entry; failed opens return to the original UI.
  renderer.setOrientation(previousOrientation);
  if (loaded) {
    result.epub = std::move(epub);
    result.failure = Epub::OpenFailure::None;
    return result;
  }

  LOG_ERR("READER", "Failed to load epub");
  result.failure = epub->getLastLoadFailure();
  return result;
}

void ReaderActivity::queueEpubOpenAlert(const Epub::OpenFailure failure) {
  const bool outOfMemory = failure == Epub::OpenFailure::OutOfMemory;
  const char* title = outOfMemory ? tr(STR_MEMORY_ERROR) : tr(STR_INDEX_FAILED);
  const char* body = outOfMemory ? tr(STR_EPUB_OPEN_MEMORY_BODY) : tr(STR_EPUB_OPEN_FAILED_BODY);
  if (failure == Epub::OpenFailure::ProtectedUnavailable) body = tr(STR_BOOK_KEY_REQUIRED);
  if (failure == Epub::OpenFailure::LoanExpired) body = tr(STR_BOOK_LOAN_EXPIRED);
  if (failure == Epub::OpenFailure::LoanClockUnavailable) body = tr(STR_BOOK_LOAN_CLOCK);
  snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s", title);
  snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), "%s", body);
  APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
  APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
}

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
  if (!xtc) {
    LOG_ERR("READER", "Failed to allocate XTC object");
    return nullptr;
  }
  if (xtc->load()) {
    return xtc;
  }

  LOG_ERR("READER", "Failed to load XTC");
  return nullptr;
}

std::unique_ptr<Txt> ReaderActivity::loadTxt(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto txt = makeUniqueNoThrow<Txt>(path, "/.crosspoint");
  if (!txt) {
    LOG_ERR("READER", "Failed to allocate TXT object");
    return nullptr;
  }
  if (txt->load()) {
    return txt;
  }

  LOG_ERR("READER", "Failed to load TXT");
  return nullptr;
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  // If coming from a book, start in that book's folder; otherwise start from root
  auto initialPath = fromBookPath.empty() ? "/" : FsHelpers::extractFolderPath(fromBookPath);
  activityManager.goToFileBrowser(std::move(initialPath));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub,
                                      EpubReaderActivity::BookReaderSettingsData readerSettings) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  // A retained-frame fast refresh is only supplied by the direct sleep-wake
  // route. The current book is already first in recents, so avoid loading and
  // rewriting that store solely to record the same entry again. Adapted from
  // Sichroteph/YACP commit 20af8aee8d3e1d560456753b08d1f52e5488621f (MIT).
  const bool skipRecentBookUpdateOnEntry = allowFastInitialRefresh;
  activityManager.replaceActivity(std::make_unique<EpubReaderActivity>(
      renderer, mappedInput, std::move(epub), std::move(readerSettings), initialRefreshCountdown(),
      cleanImageBaseOnEntry, skipRecentBookUpdateOnEntry, openingFeedback));
}

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  activityManager.replaceActivity(std::make_unique<BmpViewerActivity>(renderer, mappedInput, path));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  activityManager.replaceActivity(std::make_unique<XtcReaderActivity>(
      renderer, mappedInput, std::move(xtc), initialRefreshCountdown(), allowFastInitialRefresh, openingFeedback));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  activityManager.replaceActivity(std::make_unique<TxtReaderActivity>(
      renderer, mappedInput, std::move(txt), initialRefreshCountdown(), allowFastInitialRefresh, openingFeedback));
}

void ReaderActivity::onEnter() {
  Activity::onEnter();

  if (suppressInitialBackRelease) {
    mappedInput.suppressNextBackRelease();
  }

  if (initialBookPath.empty()) {
    goToLibrary();  // Start from root when entering via Browse
    return;
  }

  // Quick Resume already shows the book page. A loading notice would blank it.
  if (!keepWakeFrame) openingFeedback.start();

  if (isImagePreviewFile(initialBookPath)) {
    GUI.drawPopup(renderer, tr(STR_LOADING));
    onGoToBmpViewer(initialBookPath);
    return;
  }

  currentBookPath = initialBookPath;
  if (isXtcFile(initialBookPath)) {
    auto xtc = loadXtc(initialBookPath);
    if (!xtc) {
      onGoBack();
      return;
    }
    onGoToXtcReader(std::move(xtc));
  } else if (isTxtFile(initialBookPath)) {
    auto txt = loadTxt(initialBookPath);
    if (!txt) {
      onGoBack();
      return;
    }
    onGoToTxtReader(std::move(txt));
  } else {
    auto result = loadEpub(initialBookPath);
    if (!result.epub) {
      queueEpubOpenAlert(result.failure);
      onGoBack();
      return;
    }
    onGoToEpubReader(std::move(result.epub), std::move(result.readerSettings));
  }
}

void ReaderActivity::onGoBack() { finish(); }
