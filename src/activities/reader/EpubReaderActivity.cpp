#include "EpubReaderActivity.h"

#include <Arduino.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <MemoryBudget.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>

#include "../settings/KOReaderSettingsActivity.h"
#include "BookStatsActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderBookmarkListActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "GlobalActions.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/boot_sleep/SleepCoverAssets.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long longPressMenuMs = 600;
constexpr uint16_t DEFAULT_AUTO_PAGE_TURN_INTERVAL_S = 30;
constexpr uint16_t MIN_AUTO_PAGE_TURN_INTERVAL_S = 5;
constexpr uint16_t MAX_AUTO_PAGE_TURN_INTERVAL_S = 120;
constexpr int MAX_PAGE_LOAD_RETRIES = 3;
constexpr uint8_t READER_SETTINGS_FILE_VERSION = 1;
constexpr char READER_SETTINGS_FILE_NAME[] = "/reader_settings.bin";
constexpr char FALLBACK_FONT_SECTION_CACHE_SUFFIX[] = "_fallback_font";

void drawToastBuffer(const GfxRenderer& renderer, const char* msg) {
  constexpr int toastPadX = 20;
  constexpr int toastPadY = 12;
  const int msgW = renderer.getTextWidth(UI_10_FONT_ID, msg);
  const int msgH = renderer.getLineHeight(UI_10_FONT_ID);
  const int toastW = msgW + toastPadX * 2;
  const int toastH = msgH + toastPadY * 2;
  const int toastX = (renderer.getScreenWidth() - toastW) / 2;
  const int toastY = (renderer.getScreenHeight() - toastH) / 2;
  renderer.fillRect(toastX, toastY, toastW, toastH, true);
  renderer.drawText(UI_10_FONT_ID, toastX + toastPadX, toastY + toastPadY, msg, false);
}

void drawToast(const GfxRenderer& renderer, const char* msg) {
  drawToastBuffer(renderer, msg);
  renderer.displayBuffer();
}

bool releaseReaderSdFontCachesForLowMemory(GfxRenderer& renderer, const char* tag, const char* reason) {
  const int fontId = SETTINGS.getReaderFontId();
  if (!renderer.isSdCardFont(fontId)) {
    return false;
  }

  const auto before = MemoryBudget::snapshot();
  if (!renderer.releaseSdCardFontForLowMemory(fontId)) {
    return false;
  }
  const auto after = MemoryBudget::snapshot();
  LOG_DBG(tag, "Released SD font caches after %s: free=%u->%u maxAlloc=%u->%u", reason, before.freeHeap, after.freeHeap,
          before.maxAllocHeap, after.maxAllocHeap);
  return true;
}

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

bool isSnippetWhitespace(const std::string& word) {
  if (word.empty()) return true;
  return std::all_of(word.begin(), word.end(),
                     [](const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; });
}

void buildBookmarkSnippet(const Page& page, char* out, const size_t outSize) {
  if (!out || outSize == 0) return;
  out[0] = '\0';
  size_t len = 0;

  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*el);
    if (!line.getBlock()) continue;
    const auto& words = line.getBlock()->getWords();
    for (const auto& word : words) {
      if (isSnippetWhitespace(word)) continue;
      const size_t separatorLen = len > 0 ? 1 : 0;
      const size_t wordLen = word.size();
      if (len + separatorLen + wordLen >= outSize) return;
      if (separatorLen > 0) out[len++] = ' ';
      memcpy(out + len, word.c_str(), wordLen);
      len += wordLen;
      out[len] = '\0';
    }
  }
}

uint16_t clampAutoPageTurnIntervalSeconds(const uint16_t seconds) {
  return std::clamp(seconds, MIN_AUTO_PAGE_TURN_INTERVAL_S, MAX_AUTO_PAGE_TURN_INTERVAL_S);
}

uint16_t loadAutoPageTurnIntervalSeconds(const std::string& cachePath) {
  FsFile f;
  if (!Storage.openFileForRead("ERS", cachePath + READER_SETTINGS_FILE_NAME, f)) {
    return DEFAULT_AUTO_PAGE_TURN_INTERVAL_S;
  }

  uint8_t data[3] = {};
  const int n = f.read(data, sizeof(data));
  f.close();

  if (n != static_cast<int>(sizeof(data)) || data[0] != READER_SETTINGS_FILE_VERSION) {
    LOG_DBG("ERS", "Reader settings missing or version mismatch, using defaults");
    return DEFAULT_AUTO_PAGE_TURN_INTERVAL_S;
  }

  const uint16_t seconds = static_cast<uint16_t>(data[1]) | (static_cast<uint16_t>(data[2]) << 8);
  if (seconds == 0) {
    return DEFAULT_AUTO_PAGE_TURN_INTERVAL_S;
  }
  return clampAutoPageTurnIntervalSeconds(seconds);
}

bool saveAutoPageTurnIntervalSeconds(const std::string& cachePath, const uint16_t seconds) {
  FsFile f;
  if (!Storage.openFileForWrite("ERS", cachePath + READER_SETTINGS_FILE_NAME, f)) {
    LOG_ERR("ERS", "Could not open reader settings file for write");
    return false;
  }

  const uint16_t clampedSeconds = clampAutoPageTurnIntervalSeconds(seconds);
  uint8_t data[3];
  data[0] = READER_SETTINGS_FILE_VERSION;
  data[1] = clampedSeconds & 0xFF;
  data[2] = (clampedSeconds >> 8) & 0xFF;
  const size_t written = f.write(data, sizeof(data));
  f.close();
  if (written != sizeof(data)) {
    LOG_ERR("ERS", "Short write saving reader settings: %u/%u bytes", (unsigned)written, (unsigned)sizeof(data));
    return false;
  }
  return true;
}

// SD card folder finished books are moved into. Single source of truth for the path.
constexpr char READ_FOLDER[] = "/Read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // excludes NUL
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

// Pick a non-colliding destination path inside /Read/ for a finished book.
// Mirrors the suffixing scheme used elsewhere: "name.epub" -> "name (2).epub", etc.
std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

// Relocate a finished book and its cache dir into /Read/, keep it in recents by
// repointing its entry to the new path, and repoint the resume pointer too.
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath, const std::string& title) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s", tr(STR_MOVE_TO_READ_FAILED_TITLE));
    snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), tr(STR_MOVE_TO_READ_FAILED_BODY),
             title.c_str());
    APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
    APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
    return;
  }

  // Cache dir is keyed by hash of the epub path (see Epub ctor), so it must be re-keyed.
  const std::string newCachePath = Epub::cachePathForFilePath(dstPath, "/.crosspoint");
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  // Keep the book in recents (crossink behavior): repoint the entry to its new
  // location instead of dropping it. updatePath persists on success.
  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

float EpubReaderActivity::getCurrentBookProgressPercent() const {
  if (!epub || !section || section->pageCount == 0 || epub->getBookSize() == 0) {
    return 0.0f;
  }

  const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
  return epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
}

void EpubReaderActivity::initializeCompletionPromptTrigger() {
  completionTriggerSpineIndex = -1;
  completionTriggerSpineProgress = 1.0f;
  completionPromptQueued = false;
  completionPromptShown = stats.isCompleted;
  completionTriggerSeenBelow = false;
  lastAtOrPastCompletionTrigger = false;

  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  const int spineCount = epub->getSpineItemsCount();
  if (bookSize == 0 || spineCount <= 0) {
    return;
  }

  size_t targetSize = (bookSize / 100) * 99 + (bookSize % 100) * 99 / 100;
  if (targetSize >= bookSize) {
    targetSize = bookSize - 1;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;

  completionTriggerSpineIndex = targetSpineIndex;
  completionTriggerSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);

  if (completionTriggerSpineProgress < 0.0f) {
    completionTriggerSpineProgress = 0.0f;
  } else if (completionTriggerSpineProgress > 1.0f) {
    completionTriggerSpineProgress = 1.0f;
  }
}

bool EpubReaderActivity::isAtOrPastCompletionTrigger() const {
  if (!epub || !section || section->pageCount == 0 || completionTriggerSpineIndex < 0) {
    return false;
  }

  if (currentSpineIndex > completionTriggerSpineIndex) {
    return true;
  }
  if (currentSpineIndex < completionTriggerSpineIndex) {
    return false;
  }

  const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
  return chapterProgress >= completionTriggerSpineProgress;
}

void EpubReaderActivity::queueCompletionPromptIfNeeded() {
  if (completionPromptShown || completionPromptQueued || stats.isCompleted || footnoteDepth > 0) {
    return;
  }

  const bool atOrPastTrigger = isAtOrPastCompletionTrigger();

  if (!atOrPastTrigger) {
    completionTriggerSeenBelow = true;
  }

  if (completionTriggerSeenBelow && !lastAtOrPastCompletionTrigger && atOrPastTrigger) {
    completionPromptQueued = true;
  }

  lastAtOrPastCompletionTrigger = atOrPastTrigger;
}

void EpubReaderActivity::onEnter() {
  Activity::onEnter();
  pageLoadRetryCount = 0;

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Activate reader-specific front button mapping (if configured).
  mappedInput.setReaderMode(true);

  epub->setupCacheDir();
  lastAutoPageTurnIntervalSeconds = loadAutoPageTurnIntervalSeconds(epub->getCachePath());
  BOOKMARKS.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), "epub");

  if (APP_STATE.pendingBookmarkSpine != UINT16_MAX && APP_STATE.pendingBookmarkProgress >= 0.0f) {
    // Resume from a bookmark selected on the Home screen
    currentSpineIndex = APP_STATE.pendingBookmarkSpine;
    pendingSpineProgress = APP_STATE.pendingBookmarkProgress;
    pendingBookmarkParagraphIndex = APP_STATE.pendingBookmarkParagraphIndex;
    pendingPercentJump = true;
    cachedSpineIndex = currentSpineIndex;

    // Clear the pending jump
    APP_STATE.pendingBookmarkSpine = UINT16_MAX;
    APP_STATE.pendingBookmarkProgress = -1.0f;
    APP_STATE.pendingBookmarkParagraphIndex = UINT16_MAX;
    APP_STATE.saveToFile();
  } else {
    EpubReaderUtils::Progress progress;
    if (EpubReaderUtils::loadProgress(*epub, progress)) {
      currentSpineIndex = progress.spineIndex;
      nextPageNumber = progress.pageNumber;
      cachedSpineIndex = currentSpineIndex;
      if (progress.hasPageCount) {
        cachedChapterTotalPageCount = progress.pageCount;
      }
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0 && !pendingPercentJump) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  // Load reading stats and record session start time.
  // Session count and reading time are committed on exit once thresholds are met.
  stats = BookReadingStats::load(epub->getCachePath());
  sessionStartMs = millis();

  globalStats = GlobalReadingStats::load();

  initializeCompletionPromptTrigger();

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addOrUpdateBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
  SleepCoverAssets::prepareEpub(*epub);

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  Activity::onExit();

  // Deactivate reader-specific front button mapping.
  mappedInput.setReaderMode(false);

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  // Commit session stats based on how long the session lasted.
  // Sessions under 1 minute don't count toward session count or reading time.
  // Sessions under 10 seconds don't add to reading time.
  const unsigned long elapsedMs = millis() - sessionStartMs;
  if (elapsedMs >= 60000UL) {
    stats.sessionCount++;
    globalStats.totalSessions++;
  }
  if (elapsedMs >= 10000UL) {
    const uint32_t elapsedSecs = static_cast<uint32_t>(elapsedMs / 1000UL);
    stats.totalReadingSeconds += elapsedSecs;
    globalStats.totalReadingSeconds += elapsedSecs;
  }
  if (epub) {
    stats.save(epub->getCachePath());
  }
  globalStats.save();

  BOOKMARKS.unload();
  section.reset();

  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string title = epub->getTitle();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath, title);
  } else {
    epub.reset();
  }
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  if (completionPromptQueued) {
    completionPromptQueued = false;
    completionPromptShown = true;
    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_MARK_FINISHED_PROMPT_TITLE),
                                               tr(STR_MARK_FINISHED_PROMPT_BODY)),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            setBookCompleted(true);
            showCompletedFeedback(true);
          }
          requestUpdate();
        });
    return;
  }

  if (pendingBookmarkFeedback) {
    const bool timedOut = (millis() - bookmarkFeedbackShowTime) >= 1000UL;
    const bool navPressed = mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (timedOut || navPressed) {
      pendingBookmarkFeedback = false;
      requestUpdate();
      return;
    }
  }

  if (pendingCompletedFeedback) {
    const bool timedOut = (millis() - completedFeedbackShowTime) >= 1000UL;
    const bool navPressed = mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (timedOut || navPressed) {
      pendingCompletedFeedback = false;
      requestUpdate();
      return;
    }
  }
  if (pendingTiltPageTurnFeedback) {
    const bool timedOut = (millis() - tiltPageTurnFeedbackShowTime) >= 1000UL;
    const bool navPressed = mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (timedOut || navPressed) {
      pendingTiltPageTurnFeedback = false;
      requestUpdate();
      return;
    }
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved) — no per-frame writes.
  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      RECENT_BOOKS.addOrUpdateBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  // Arm the move here so any exit path relocates the book into /Read/.
  // setBookCompleted() also arms this when the user marks a book finished before
  // the End-of-Book screen.
  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else if (!stats.isCompleted) {
    pendingReadFolderMove = false;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      automaticPageTurnActive = false;
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  // Long-press Confirm: execute the configured reader action without opening the menu
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (longPressMenuHandled) {
      longPressMenuHandled = false;
      return;
    }
    if (SETTINGS.longPressMenuAction != CrossPointSettings::LONG_MENU_OFF &&
        mappedInput.getHeldTime() >= longPressMenuMs) {
      executeLongPressMenuAction();
      return;
    }
  }
  if (SETTINGS.longPressMenuAction != CrossPointSettings::LONG_MENU_OFF && !longPressMenuHandled &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= longPressMenuMs) {
    longPressMenuHandled = true;
    executeLongPressMenuAction();
    return;
  }

  // Enter reader menu activity.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    int currentPage = 0;
    int totalPages = 0;
    float bookProgress = 0.0f;
    uint16_t bmSpine = static_cast<uint16_t>(currentSpineIndex);
    float bmProgress = 0.0f;
    int bookmarkPageCount = 1;
    bool isBookCompleted = stats.isCompleted;
    {
      // Serialize EPUB metadata/file access with the render task.
      RenderLock lock(*this);
      currentPage = section ? section->currentPage + 1 : 0;
      totalPages = section ? section->pageCount : 0;
      bmSpine = static_cast<uint16_t>(currentSpineIndex);
      bmProgress =
          (section && section->pageCount > 0) ? static_cast<float>(section->currentPage) / section->pageCount : 0.0f;
      bookmarkPageCount = (section && section->pageCount > 0) ? section->pageCount : 1;
      isBookCompleted = stats.isCompleted;
      bookProgress = getCurrentBookProgressPercent();
    }
    const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));

    startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                               renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                               SETTINGS.orientation, !currentPageFootnotes.empty(), !BOOKMARKS.getBookmarks().empty(),
                               BOOKMARKS.hasBookmarkForPage(bmSpine, bmProgress, bookmarkPageCount), isBookCompleted,
                               automaticPageTurnActive, getAutoPageTurnIntervalSeconds()),
                           [this](const ActivityResult& result) {
                             // Always apply orientation change even if the menu was cancelled
                             const auto& menu = std::get<MenuResult>(result.data);
                             applyOrientation(menu.orientation);
                             if (menu.settingsChanged) {
                               sdFontSystem.ensureLoaded(renderer);
                               RenderLock lock(*this);
                               if (section) {
                                 cachedSpineIndex = currentSpineIndex;
                                 cachedChapterTotalPageCount = section->pageCount;
                                 nextPageNumber = section->currentPage;
                               }
                               section.reset();  // Force re-layout with changed reader settings
                             }
                             if (!result.isCancelled) {
                               onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                             }
                           });
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(epub ? epub->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
      return;
    }
    onGoHome();
    return;
  }

  // Side button long-press actions use raw Up/Down so the direction stays
  // physical regardless of the Prev/Next side layout setting.
  const bool sideLongPressChangesFont =
      SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_FONT_SIZE;
  const bool sideLongPressChangesOrientation =
      SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_ORIENTATION_CHANGE;
  if (sideLongPressChangesFont || sideLongPressChangesOrientation) {
    const bool topReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
    const bool bottomReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (sideButtonLongPressHandled && (topReleased || bottomReleased)) {
      sideButtonLongPressHandled = false;
      return;
    }

    const bool longPressReady = mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
    const bool topLongPressed =
        longPressReady && (mappedInput.isPressed(MappedInputManager::Button::Up) || topReleased);
    const bool bottomLongPressed =
        longPressReady && (mappedInput.isPressed(MappedInputManager::Button::Down) || bottomReleased);

    if (!sideButtonLongPressHandled && topLongPressed) {
      sideButtonLongPressHandled = !topReleased;
      if (sideLongPressChangesFont) {
        if (sdFontSystem.changeReaderFontSize(/*larger=*/true)) {
          reindexCurrentSection();
        }
      } else {
        applyOrientation(ReaderUtils::rotatedOrientation(SETTINGS.orientation, /*clockwise=*/false));
        requestUpdate();
      }
      return;
    }
    if (!sideButtonLongPressHandled && bottomLongPressed) {
      sideButtonLongPressHandled = !bottomReleased;
      if (sideLongPressChangesFont) {
        if (sdFontSystem.changeReaderFontSize(/*larger=*/false)) {
          reindexCurrentSection();
        }
      } else {
        applyOrientation(ReaderUtils::rotatedOrientation(SETTINGS.orientation, /*clockwise=*/true));
        requestUpdate();
      }
      return;
    }
  }

  if (consumeLongPowerButtonRelease()) {
    return;
  }
  if (executeShortPowerButtonAction()) {
    return;
  }
  if (executeLongPowerButtonAction()) {
    return;
  }

  const bool frontLongPressAction = SETTINGS.longPressButtonBehavior == CrossPointSettings::CHAPTER_SKIP ||
                                    SETTINGS.longPressButtonBehavior == CrossPointSettings::ORIENTATION_CHANGE;
  if (frontLongPressAction) {
    const bool leftReleased = mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool rightReleased = mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (frontButtonLongPressHandled && (leftReleased || rightReleased)) {
      frontButtonLongPressHandled = false;
      return;
    }

    const bool longPressReady = mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
    const bool prevLongPressed = longPressReady && mappedInput.isPressed(MappedInputManager::Button::Left);
    const bool nextLongPressed = longPressReady && mappedInput.isPressed(MappedInputManager::Button::Right);
    if (!frontButtonLongPressHandled && (prevLongPressed || nextLongPressed)) {
      frontButtonLongPressHandled = true;
      if (SETTINGS.longPressButtonBehavior == CrossPointSettings::CHAPTER_SKIP) {
        if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
          if (nextLongPressed) {
            onGoHome();
          } else {
            currentSpineIndex = epub->getSpineItemsCount() - 1;
            nextPageNumber = 0;
            pendingPageJump = std::numeric_limits<uint16_t>::max();
            requestUpdate();
          }
          return;
        }

        {
          RenderLock lock(*this);
          nextPageNumber = 0;
          currentSpineIndex = nextLongPressed ? currentSpineIndex + 1 : currentSpineIndex - 1;
          section.reset();
        }
        requestUpdate();
        return;
      }

      const uint8_t newOrientation = nextLongPressed
                                         ? ReaderUtils::rotatedOrientation(SETTINGS.orientation, /*clockwise=*/false)
                                         : ReaderUtils::rotatedOrientation(SETTINGS.orientation, /*clockwise=*/true);
      applyOrientation(newOrientation);
      requestUpdate();
      return;
    }
  }

  auto [prevTriggered, nextTriggered, fromSideBtn, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && consumeLongPowerButtonHold()) {
    nextTriggered = true;
    fromSideBtn = false;
    fromTilt = false;
  }
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      requestUpdate();
    }
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
  const bool skipChapter =
      longPress &&
      (fromSideBtn ? SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_CHAPTER_SKIP
                   : SETTINGS.longPressButtonBehavior == CrossPointSettings::CHAPTER_SKIP);

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (skipChapter) {
    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      currentSpineIndex = nextTriggered ? currentSpineIndex + 1 : currentSpineIndex - 1;
      section.reset();
    }
    requestUpdate();
    return;
  }

  if (longPress && !fromSideBtn && SETTINGS.longPressButtonBehavior == CrossPointSettings::ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  pageLoadRetryCount = 0;
  if (!epub) {
    return;
  }

  // BookMetadataCache uses a shared seek-based FsFile for spine metadata lookups.
  // Hold the render/file mutex for the full jump calculation so menu-driven jumps
  // cannot race render/status-bar reads of the same cache file.
  RenderLock lock(*this);

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  // Store a normalized position within the spine so it can be applied once loaded.
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (pendingSpineProgress < 0.0f) {
    pendingSpineProgress = 0.0f;
  } else if (pendingSpineProgress > 1.0f) {
    pendingSpineProgress = 1.0f;
  }

  // Reset state so render() reloads and repositions on the target spine.
  currentSpineIndex = targetSpineIndex;
  nextPageNumber = 0;
  pendingPercentJump = true;
  section.reset();
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this](const ActivityResult& result) {
            if (!result.isCancelled && currentSpineIndex != std::get<ChapterResult>(result.data).spineIndex) {
              RenderLock lock(*this);
              currentSpineIndex = std::get<ChapterResult>(result.data).spineIndex;
              nextPageNumber = 0;
              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      {
        // Serialize EPUB metadata/file access with the render task.
        RenderLock lock(*this);
        bookProgress = getCurrentBookProgressPercent();
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        auto p = section->loadPageFromSectionFile();
        if (p) {
          std::string fullText;
          for (const auto& el : p->elements) {
            if (el->getTag() == TAG_PageLine) {
              const auto& line = static_cast<const PageLine&>(*el);
              if (line.getBlock()) {
                const auto& words = line.getBlock()->getWords();
                for (const auto& w : words) {
                  if (!fullText.empty()) fullText += " ";
                  fullText += w;
                }
              }
            }
          }
          if (!fullText.empty()) {
            startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                   [this](const ActivityResult& result) {});
            break;
          }
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      bool cacheDeleted = false;
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          cacheDeleted = epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
          if (cacheDeleted) {
            drawToast(renderer, tr(STR_BOOK_CACHE_DELETED));
          }
        }
      }
      if (cacheDeleted) {
        delay(1000);
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::READING_STATS: {
      // Include elapsed time from the current session in the display stats.
      BookReadingStats displayStats = stats;
      displayStats.totalReadingSeconds += static_cast<uint32_t>((millis() - sessionStartMs) / 1000UL);
      const bool hasSyncedStats = GlobalReadingStats::hasSyncedStats();
      const GlobalReadingStats displayAllDevicesStats =
          hasSyncedStats ? GlobalReadingStats::loadAggregated(globalStats) : GlobalReadingStats{};
      if (hasSyncedStats) {
        startActivityForResult(std::make_unique<BookStatsActivity>(renderer, mappedInput, epub->getTitle(),
                                                                   displayStats, globalStats, displayAllDevicesStats),
                               [this](const ActivityResult&) { requestUpdate(); });
      } else {
        startActivityForResult(
            std::make_unique<BookStatsActivity>(renderer, mappedInput, epub->getTitle(), displayStats, globalStats),
            [this](const ActivityResult&) { requestUpdate(); });
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_COMPLETED: {
      const bool markCompleted = !stats.isCompleted;
      setBookCompleted(markCompleted);
      showCompletedFeedback(markCompleted);
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      if (KOREADER_STORE.hasCredentials()) {
        const int currentPage = section ? section->currentPage : nextPageNumber;
        const int totalPages = section ? section->pageCount : cachedChapterTotalPageCount;
        std::optional<uint16_t> paragraphIndex;
        if (section && currentPage >= 0 && currentPage < section->pageCount) {
          const uint16_t paragraphPage =
              currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
          if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
            paragraphIndex = *pIdx;
          }
        }

        // Pre-compute local KO position and chapter name while Epub is still in RAM.
        CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
        if (paragraphIndex.has_value()) {
          localPos.paragraphIndex = *paragraphIndex;
          localPos.hasParagraphIndex = true;
        }
        KOReaderPosition localKoPos = ProgressMapper::toKOReader(epub, localPos);
        const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
        std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
        const std::string savedEpubPath = epub->getPath();

        // Persist current position so the reader resumes at the right page on return.
        // goToReader() depends on this file, so abort the sync if the write fails.
        if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
          LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
          pendingSyncSaveError = true;
          requestUpdate();
          return;
        }

        // Release the heavy Section now. Keep Epub alive until onExit(), which still
        // needs it for stats/cache cleanup before the sync activity starts.
        LOG_DBG("KOSync", "Releasing section for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
        {
          RenderLock lock(*this);
          if (section) {
            nextPageNumber = section->currentPage;
          }
          section.reset();
        }
        LOG_DBG("KOSync", "Section released for sync (heap after: %u)", (unsigned)ESP.getFreeHeap());

        activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
            renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
            std::move(localChapterName), paragraphIndex));
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARK_TOGGLE: {
      if (!section || section->pageCount == 0) break;
      const uint16_t spine = static_cast<uint16_t>(currentSpineIndex);
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);

      if (BOOKMARKS.hasBookmarkForPage(spine, progress, section->pageCount)) {
        BOOKMARKS.removeBookmarkForPage(spine, progress, section->pageCount);
        bookmarkFeedbackType = BookmarkFeedbackType::Removed;
      } else {
        const char* chapterTitle = nullptr;
        std::string titleStr;
        const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
        if (tocIndex != -1) {
          titleStr = epub->getTocItem(tocIndex).title;
          chapterTitle = titleStr.c_str();
        }
        uint16_t paragraphIndex = UINT16_MAX;
        if (const auto pIdx = section->getParagraphIndexForPage(static_cast<uint16_t>(section->currentPage))) {
          paragraphIndex = *pIdx;
        }
        char snippet[BOOKMARK_SNIPPET_MAX] = {};
        if (auto page = section->loadPageFromSectionFile()) {
          buildBookmarkSnippet(*page, snippet, sizeof(snippet));
        }
        const auto addResult =
            BOOKMARKS.addBookmark(spine, progress, section->pageCount, chapterTitle, paragraphIndex, snippet);
        bookmarkFeedbackType = (addResult == BookmarkStore::AddResult::Added) ? BookmarkFeedbackType::Added
                                                                              : BookmarkFeedbackType::LimitReached;
      }
      pendingBookmarkFeedback = true;
      bookmarkFeedbackShowTime = millis();
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::VIEW_BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarkListActivity>(renderer, mappedInput, BOOKMARKS.getBookmarks()),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& bm = std::get<BookmarkResult>(result.data);
              RenderLock lock(*this);
              currentSpineIndex = bm.spineIndex;
              pendingSpineProgress = bm.progress;
              pendingBookmarkParagraphIndex = bm.paragraphIndex;
              pendingPercentJump = true;
              section.reset();
            }
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_BOOKMARKS: {
      BOOKMARKS.clearAll();
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::AUTO_PAGE_TURN:
      openAutoPageTurnIntervalPicker();
      break;
    case EpubReaderMenuActivity::MenuAction::ROTATE_SCREEN:
    case EpubReaderMenuActivity::MenuAction::READER_OPTIONS:
    case EpubReaderMenuActivity::MenuAction::CONTROLS_OPTIONS:
      break;
  }
}

void EpubReaderActivity::reindexCurrentSection() {
  SETTINGS.saveToFile();
  sdFontSystem.ensureLoaded(renderer);
  {
    RenderLock lock(*this);
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::openFileTransfer() {
  if (epub && section) {
    saveProgress(currentSpineIndex, section->currentPage, section->pageCount);
  }

  activityManager.goToFileTransfer(epub ? epub->getPath() : std::string{});
}

void EpubReaderActivity::openAutoPageTurnIntervalPicker(const bool ignoreInitialConfirmRelease) {
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "EpubReaderAutoPageTurnInterval", StrId::STR_AUTO_TURN_INTERVAL_SECONDS,
          StrId::STR_AUTO_TURN_STEP_HINT, getAutoPageTurnIntervalSeconds(), MIN_AUTO_PAGE_TURN_INTERVAL_S,
          MAX_AUTO_PAGE_TURN_INTERVAL_S, 1, 5, StrId::STR_NONE_OPT, /*readerActivity=*/true,
          /*allowPowerAsConfirm=*/true, ignoreInitialConfirmRelease),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          setAutoPageTurnIntervalSeconds(static_cast<uint16_t>(std::get<IntervalResult>(result.data).value));
        }
        requestUpdate();
      });
}

void EpubReaderActivity::executeReaderQuickAction(CrossPointSettings::LONG_PRESS_MENU_ACTION action) {
  switch (action) {
    case CrossPointSettings::LONG_MENU_SLEEP:
      enterDeepSleep();
      break;
    case CrossPointSettings::LONG_MENU_CHANGE_FONT:
      SETTINGS.fontFamily = (SETTINGS.fontFamily + 1) % CrossPointSettings::FONT_FAMILY_COUNT;
      reindexCurrentSection();
      break;
    case CrossPointSettings::LONG_MENU_TOGGLE_GUIDE_DOTS:
      SETTINGS.guideReadingEnabled = !SETTINGS.guideReadingEnabled;
      reindexCurrentSection();
      break;
    case CrossPointSettings::LONG_MENU_TOGGLE_BIONIC:
      SETTINGS.bionicReadingEnabled = !SETTINGS.bionicReadingEnabled;
      reindexCurrentSection();
      break;
    case CrossPointSettings::LONG_MENU_TOGGLE_BOOKMARK:
      onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::BOOKMARK_TOGGLE);
      break;
    case CrossPointSettings::LONG_MENU_REFRESH_SCREEN:
      pagesUntilFullRefresh = 1;  // Forces HALF_REFRESH on next render
      requestUpdate();
      break;
    case CrossPointSettings::LONG_MENU_SYNC_PROGRESS:
      if (KOREADER_STORE.hasCredentials()) {
        onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::SYNC);
      } else {
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { SETTINGS.saveToFile(); });
      }
      break;
    case CrossPointSettings::LONG_MENU_MARK_FINISHED: {
      const bool newCompleted = !stats.isCompleted;
      setBookCompleted(newCompleted);
      showCompletedFeedback(newCompleted);
    }
      requestUpdate();
      break;
    case CrossPointSettings::LONG_MENU_READING_STATS:
      onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::READING_STATS);
      break;
    case CrossPointSettings::LONG_MENU_SCREENSHOT:
      onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::SCREENSHOT);
      break;
    case CrossPointSettings::LONG_MENU_CYCLE_PAGE_TURN:
      openAutoPageTurnIntervalPicker(/*ignoreInitialConfirmRelease=*/true);
      break;
    case CrossPointSettings::LONG_MENU_FILE_TRANSFER:
      openFileTransfer();
      break;
    case CrossPointSettings::LONG_MENU_TOGGLE_TILT_PAGE_TURN:
      if (halTiltSensor.isAvailable()) {
        SETTINGS.tiltPageTurn = SETTINGS.tiltPageTurn == CrossPointSettings::TILT_OFF ? CrossPointSettings::TILT_ON
                                                                                      : CrossPointSettings::TILT_OFF;
        SETTINGS.saveToFile();
        halTiltSensor.clearPendingEvents();
        showTiltPageTurnFeedback(SETTINGS.tiltPageTurn != CrossPointSettings::TILT_OFF);
        requestUpdate();
      }
      break;
    case CrossPointSettings::LONG_MENU_OFF:
    default:
      break;
  }
}

bool EpubReaderActivity::executeShortPowerButtonAction() {
  if (!mappedInput.wasReleased(MappedInputManager::Button::Power) ||
      mappedInput.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration()) {
    return false;
  }

  switch (SETTINGS.shortPwrBtn) {
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FONT:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CHANGE_FONT);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_GUIDE_DOTS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_GUIDE_DOTS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_BIONIC_READING:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_BIONIC);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_BOOKMARK:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_BOOKMARK);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::SYNC_PROGRESS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_SYNC_PROGRESS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::MARK_FINISHED:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_MARK_FINISHED);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::READING_STATS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_READING_STATS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::SCREENSHOT:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_SCREENSHOT);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CYCLE_PAGE_TURN:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CYCLE_PAGE_TURN);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_FILE_TRANSFER);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TILT_PAGE_TURN:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_TILT_PAGE_TURN);
      return true;
    default:
      return false;
  }
}

bool EpubReaderActivity::consumeLongPowerButtonRelease() {
  if (!mappedInput.wasReleased(MappedInputManager::Button::Power) || !longPowerButtonHandled) {
    return false;
  }

  longPowerButtonHandled = false;
  return true;
}

bool EpubReaderActivity::consumeLongPowerButtonHold() {
  if (longPowerButtonHandled || !mappedInput.isPressed(MappedInputManager::Button::Power) ||
      mappedInput.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()) {
    return false;
  }

  longPowerButtonHandled = true;
  return true;
}

bool EpubReaderActivity::executeLongPowerButtonAction() {
  if (SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN || !consumeLongPowerButtonHold()) {
    return false;
  }

  switch (SETTINGS.longPwrBtn) {
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FONT:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CHANGE_FONT);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_GUIDE_DOTS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_GUIDE_DOTS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_BIONIC_READING:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_BIONIC);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_BOOKMARK:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_BOOKMARK);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::SYNC_PROGRESS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_SYNC_PROGRESS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::MARK_FINISHED:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_MARK_FINISHED);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::READING_STATS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_READING_STATS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::SCREENSHOT:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_SCREENSHOT);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CYCLE_PAGE_TURN:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CYCLE_PAGE_TURN);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_FILE_TRANSFER);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TILT_PAGE_TURN:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_TILT_PAGE_TURN);
      return true;
    default:
      return false;
  }
}

void EpubReaderActivity::executeLongPressMenuAction() {
  executeReaderQuickAction(static_cast<CrossPointSettings::LONG_PRESS_MENU_ACTION>(SETTINGS.longPressMenuAction));
}

void EpubReaderActivity::setBookCompleted(bool isCompleted) {
  if (stats.isCompleted == isCompleted) {
    return;
  }

  stats.isCompleted = isCompleted;
  if (isCompleted) {
    completionPromptShown = true;
    if (SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath())) {
      pendingReadFolderMove = true;
    }
  } else {
    pendingReadFolderMove = false;
  }
  if (isCompleted) {
    globalStats.completedBooks++;
  } else if (globalStats.completedBooks > 0) {
    globalStats.completedBooks--;
  }

  stats.save(epub->getCachePath());
  globalStats.save();
}

void EpubReaderActivity::showCompletedFeedback(bool isCompleted) {
  completedFeedbackIsFinished = isCompleted;
  pendingCompletedFeedback = true;
  completedFeedbackShowTime = millis();
}

void EpubReaderActivity::showTiltPageTurnFeedback(bool enabled) {
  tiltPageTurnFeedbackEnabled = enabled;
  pendingTiltPageTurnFeedback = true;
  tiltPageTurnFeedbackShowTime = millis();
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  const auto targetOrientation = ReaderUtils::toRendererOrientation(orientation);
  const bool settingsChanged = SETTINGS.orientation != orientation;
  const bool rendererChanged = renderer.getOrientation() != targetOrientation;

  // No-op only when both the persisted setting and the live renderer already match.
  if (!settingsChanged && !rendererChanged) {
    return;
  }

  {
    RenderLock lock(*this);

    // Preserve current reading position only when we need a live re-layout.
    if (rendererChanged && section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }

    if (settingsChanged) {
      // Persist the selection so the reader keeps the new orientation on next launch.
      SETTINGS.orientation = orientation;
      SETTINGS.saveToFile();
    }

    if (rendererChanged) {
      // Update renderer orientation to match the new logical coordinate system.
      renderer.setOrientation(targetOrientation);

      // Reset section to force re-layout in the new orientation.
      section.reset();
    }
  }
}

uint16_t EpubReaderActivity::getAutoPageTurnIntervalSeconds() const {
  if (lastAutoPageTurnIntervalSeconds == 0) {
    return DEFAULT_AUTO_PAGE_TURN_INTERVAL_S;
  }
  return clampAutoPageTurnIntervalSeconds(lastAutoPageTurnIntervalSeconds);
}

void EpubReaderActivity::setAutoPageTurnIntervalSeconds(uint16_t seconds) {
  if (seconds == 0) {
    automaticPageTurnActive = false;
    return;
  }

  seconds = clampAutoPageTurnIntervalSeconds(seconds);
  lastAutoPageTurnIntervalSeconds = seconds;
  if (epub) {
    saveAutoPageTurnIntervalSeconds(epub->getCachePath(), seconds);
  }
  lastPageTurnTime = millis();
  pageTurnDuration = static_cast<unsigned long>(seconds) * 1000UL;
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  pageLoadRetryCount = 0;
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        currentSpineIndex--;
        section.reset();
      }
    }
  }
  stats.totalPagesTurned++;
  globalStats.totalPagesTurned++;
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  const auto showLowMemoryLayoutError = [this]() {
    snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s", tr(STR_EPUB_LAYOUT_MEMORY_TITLE));
    snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), "%s", tr(STR_EPUB_LAYOUT_MEMORY_BODY));
    APP_STATE.pendingAlertGoHomeOnBack.store(true, std::memory_order_relaxed);
    APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
    GUI.drawPopup(renderer, tr(STR_EPUB_LAYOUT_MEMORY_TITLE));
  };

  const auto queueFallbackFontAlert = []() {
    snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s", tr(STR_EPUB_FALLBACK_FONT_TITLE));
    snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), "%s", tr(STR_EPUB_FALLBACK_FONT_BODY));
    APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
    APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
  };

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > epub->getSpineItemsCount()) {
    currentSpineIndex = epub->getSpineItemsCount();
  }

  // Show end of book screen
  if (currentSpineIndex == epub->getSpineItemsCount()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  // Minimum padding between last line of text and the status bar
  static constexpr uint8_t STATUS_BAR_TEXT_PADDING = 3;

  // reserves space for automatic page turn indicator when no status bar or progress bar only
  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin +
                                      STATUS_BAR_TEXT_PADDING));
  } else {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin, static_cast<uint8_t>(statusBarHeight + STATUS_BAR_TEXT_PADDING));
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d (free=%u, maxAlloc=%u)", filepath.c_str(), currentSpineIndex,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    const int readerFontId = SETTINGS.getReaderFontId();
    const int fallbackFontId = SETTINGS.getBuiltInReaderFontId();
    const bool canUseFallbackFont = renderer.isSdCardFont(readerFontId) && fallbackFontId != readerFontId;
    bool loadedSection = false;
    bool usedFallbackFont = false;
    auto loadSectionWithFont = [&](const int fontId, const char* cacheSuffix) {
      section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer, cacheSuffix));
      if (!section->loadSectionFile(fontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                                    SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, viewportWidth,
                                    viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                    SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled,
                                    SETTINGS.guideReadingEnabled)) {
        section.reset();
        return false;
      }
      activeSectionFontId = fontId;
      activeSectionUsesFallbackFont = cacheSuffix && cacheSuffix[0] != '\0';
      return true;
    };

    loadedSection = loadSectionWithFont(readerFontId, "");
    if (!loadedSection && canUseFallbackFont) {
      loadedSection = loadSectionWithFont(fallbackFontId, FALLBACK_FONT_SECTION_CACHE_SUFFIX);
      if (loadedSection) {
        usedFallbackFont = true;
        LOG_DBG("ERS", "Using fallback built-in font cache for section %d (font=%d, free=%u, maxAlloc=%u)",
                currentSpineIndex, fallbackFontId, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      }
    }

    if (!loadedSection) {
      LOG_DBG("ERS", "Cache not found, building... (free=%u, maxAlloc=%u)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

      GUI.drawPopup(renderer, tr(STR_INDEXING));

      const auto popupFn = [this]() { GUI.drawPopup(renderer, tr(STR_INDEXING)); };

      bool imagesWereSuppressed = false;
      bool layoutAbortedForLowMemory = false;
      section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
      if (!section->createSectionFile(readerFontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                                      SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                      SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled,
                                      SETTINGS.guideReadingEnabled, popupFn, &imagesWereSuppressed,
                                      &layoutAbortedForLowMemory)) {
        bool fallbackBuildSucceeded = false;
        if (layoutAbortedForLowMemory && canUseFallbackFont) {
          LOG_ERR("ERS", "EPUB section layout aborted for low heap with SD font; retrying built-in fallback font");
          section.reset();
          releaseReaderSdFontCachesForLowMemory(renderer, "ERS", "fallback font section rebuild");
          GUI.drawPopup(renderer, tr(STR_INDEXING));
          section = std::unique_ptr<Section>(
              new Section(epub, currentSpineIndex, renderer, FALLBACK_FONT_SECTION_CACHE_SUFFIX));
          bool fallbackImagesWereSuppressed = false;
          bool fallbackLayoutAbortedForLowMemory = false;
          fallbackBuildSucceeded = section->createSectionFile(
              fallbackFontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
              SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
              SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
              SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled, popupFn, &fallbackImagesWereSuppressed,
              &fallbackLayoutAbortedForLowMemory);
          imagesWereSuppressed = imagesWereSuppressed || fallbackImagesWereSuppressed;
          layoutAbortedForLowMemory = fallbackLayoutAbortedForLowMemory;
          if (fallbackBuildSucceeded) {
            activeSectionFontId = fallbackFontId;
            activeSectionUsesFallbackFont = true;
            usedFallbackFont = true;
            LOG_DBG("ERS", "Fallback built-in font section cache built: spine=%d font=%d pages=%u free=%u maxAlloc=%u",
                    currentSpineIndex, fallbackFontId, section->pageCount, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
          }
        }

        if (!fallbackBuildSucceeded) {
          if (layoutAbortedForLowMemory) {
            LOG_ERR("ERS", "EPUB section layout aborted for low heap; chapter exceeds safe layout memory");
          }
          if (!layoutAbortedForLowMemory) {
            LOG_ERR("ERS", "Failed to persist page data to SD");
          }
          section.reset();
          if (layoutAbortedForLowMemory) {
            showLowMemoryLayoutError();
          } else {
            showPendingSyncSaveError();
          }
          return;
        }
      } else {
        activeSectionFontId = readerFontId;
        activeSectionUsesFallbackFont = false;
      }
      releaseReaderSdFontCachesForLowMemory(renderer, "ERS", "section cache build");
      LOG_DBG("ERS", "Cache build complete: pages=%u font=%d fallback=%u free=%u maxAlloc=%u", section->pageCount,
              activeSectionFontId, activeSectionUsesFallbackFont ? 1U : 0U, ESP.getFreeHeap(), ESP.getMaxAllocHeap());

      if (usedFallbackFont) {
        queueFallbackFontAlert();
      } else if (imagesWereSuppressed) {
        snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s",
                 tr(STR_LOW_MEMORY_IMAGES_TITLE));
        snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), "%s", tr(STR_LOW_MEMORY_IMAGES_BODY));
        APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
        APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build... (pages=%u, font=%d fallback=%u free=%u, maxAlloc=%u)",
              section->pageCount, activeSectionFontId, activeSectionUsesFallbackFont ? 1U : 0U, ESP.getFreeHeap(),
              ESP.getMaxAllocHeap());
      if (usedFallbackFont) {
        queueFallbackFontAlert();
      }
    }

    if (!section) {
      LOG_ERR("ERS", "Section load/build did not produce a section");
      showPendingSyncSaveError();
      return;
    }

    if (pendingPageJump.has_value()) {
      if (*pendingPageJump >= section->pageCount && section->pageCount > 0) {
        section->currentPage = section->pageCount - 1;
      } else {
        section->currentPage = *pendingPageJump;
      }
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      } else if (section->currentPage >= section->pageCount && section->pageCount > 0) {
        LOG_DBG("ERS", "Clamping cached page %d to %d", section->currentPage, section->pageCount - 1);
        section->currentPage = section->pageCount - 1;
      }
    }

    if (!pendingAnchor.empty()) {
      if (const auto page = section->getPageForAnchor(pendingAnchor)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
    }

    // handles changes in reader settings and reset to approximate position based on cached progress
    if (cachedChapterTotalPageCount > 0) {
      // only goes to relative position if spine index matches cached value
      if (currentSpineIndex == cachedSpineIndex && section->pageCount != cachedChapterTotalPageCount) {
        float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
        int newPage = static_cast<int>(progress * section->pageCount);
        section->currentPage = newPage;
      }
      cachedChapterTotalPageCount = 0;  // resets to 0 to prevent reading cached progress again
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      if (pendingBookmarkParagraphIndex != UINT16_MAX) {
        if (const auto paragraphPage = section->getPageForParagraphIndex(pendingBookmarkParagraphIndex)) {
          section->currentPage = *paragraphPage;
          LOG_DBG("ERS", "Resolved bookmark paragraph %u to page %u", pendingBookmarkParagraphIndex, *paragraphPage);
        } else {
          LOG_DBG("ERS", "Bookmark paragraph %u not found; using saved chapter progress",
                  pendingBookmarkParagraphIndex);
        }
      }
      pendingBookmarkParagraphIndex = UINT16_MAX;
      pendingPercentJump = false;
    }

    // Clamp the current page to ensure we stay within bounds if reader settings have
    // changed since the page number (e.g., via a bookmark) was saved.
    if (section->pageCount > 0) {
      if (section->currentPage >= section->pageCount) {
        section->currentPage = section->pageCount - 1;
      } else if (section->currentPage < 0) {
        section->currentPage = 0;
      }
    }
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  {
    auto p = section->loadPageFromSectionFile();
    if (!p) {
      pageLoadRetryCount++;
      if (pageLoadRetryCount < MAX_PAGE_LOAD_RETRIES) {
        LOG_ERR("ERS", "Failed to load page from SD (retry %d) - clearing section cache", pageLoadRetryCount);
        section->clearCache();
        section.reset();
        requestUpdate();
        automaticPageTurnActive = false;
        showPendingSyncSaveError();
        return;
      }

      LOG_ERR("ERS", "Failed to load page from SD after %d retries", pageLoadRetryCount);
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
      renderStatusBar();
      renderer.displayBuffer();
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }

    pageLoadRetryCount = 0;

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    const int renderFontId = activeSectionFontId != 0 ? activeSectionFontId : SETTINGS.getReaderFontId();
    renderContents(std::move(p), renderFontId, orientedMarginTop, orientedMarginRight, orientedMarginBottom,
                   orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  if (!saveProgress(currentSpineIndex, section->currentPage, section->pageCount)) {
    pendingSyncSaveError = true;
  }
  queueCompletionPromptIfNeeded();

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
                                  SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                  SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                                  SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled)) {
    return;
  }

  if (renderer.isSdCardFont(SETTINGS.getReaderFontId())) {
    LOG_DBG("ERS", "Skipping silent next-chapter indexing for SD font: chapter=%d free=%u maxAlloc=%u", nextSpineIndex,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return;
  }

  if (!MemoryBudget::hasHeapForOptionalEpubRebuild("ERS", "silent next-chapter indexing", nextSpineIndex)) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d (free=%u, maxAlloc=%u)", nextSpineIndex, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  if (!nextSection.createSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                                     SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
                                     SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                     SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                                     SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  } else {
    LOG_DBG("ERS", "Silent indexing complete: chapter=%d pages=%u free=%u maxAlloc=%u", nextSpineIndex,
            nextSection.pageCount, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount);
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int fontId, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  fcm->resetStats();
  const uint32_t heapBefore = esp_get_free_heap_size();
  auto scope = fcm->createPrewarmScope();
  page->renderText(renderer, fontId, orientedMarginLeft, orientedMarginTop);  // scan pass
  scope.endScanAndPrewarm();
  const uint32_t heapAfter = esp_get_free_heap_size();
  fcm->logStats("prewarm");
  const auto tPrewarm = millis();

  LOG_DBG("ERS", "Heap: before=%lu after=%lu delta=%ld", heapBefore, heapAfter,
          (int32_t)heapAfter - (int32_t)heapBefore);
  (void)heapBefore;
  (void)heapAfter;

  const bool pageHasImages = page->hasImages();
  const bool needsImageGrayscale = pageHasImages;
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || needsImageGrayscale;

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  if (pendingBookmarkFeedback) {
    const char* msg = tr(STR_BOOKMARK_ADDED);
    switch (bookmarkFeedbackType) {
      case BookmarkFeedbackType::Added:
        msg = tr(STR_BOOKMARK_ADDED);
        break;
      case BookmarkFeedbackType::Removed:
        msg = tr(STR_BOOKMARK_REMOVED);
        break;
      case BookmarkFeedbackType::LimitReached:
        msg = tr(STR_BOOKMARK_LIMIT_REACHED);
        break;
    }
    drawToastBuffer(renderer, msg);
  }
  if (pendingCompletedFeedback) {
    const char* msg = completedFeedbackIsFinished ? tr(STR_MARKED_FINISHED) : tr(STR_MARKED_UNFINISHED);
    drawToastBuffer(renderer, msg);
  }
  if (pendingTiltPageTurnFeedback) {
    const char* msg = tiltPageTurnFeedbackEnabled ? tr(STR_TILT_TO_TURN_ON) : tr(STR_TILT_TO_TURN_OFF);
    drawToastBuffer(renderer, msg);
  }
  fcm->logStats("bw_render");
  const auto tBwRender = millis();
  const auto logImagePageProfile = [](const uint32_t imageBlankDisplayMs, const uint32_t imageRestoreRenderMs,
                                      const uint32_t imageFinalDisplayMs) {
    LOG_DBG("ERS", "Image page profile: blank_display=%lums restore_render=%lums final_display=%lums",
            imageBlankDisplayMs, imageRestoreRenderMs, imageFinalDisplayMs);
  };

  if (pageHasImages) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      const auto tImageBlankDisplay = millis();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      const uint32_t imageBlankDisplayMs = millis() - tImageBlankDisplay;

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      const auto tImageRestoreRender = millis();
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      const uint32_t imageRestoreRenderMs = millis() - tImageRestoreRender;
      const auto tImageFinalDisplay = millis();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      const uint32_t imageFinalDisplayMs = millis() - tImageFinalDisplay;
      logImagePageProfile(imageBlankDisplayMs, imageRestoreRenderMs, imageFinalDisplayMs);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // Double FAST_REFRESH handles ghosting for image pages; don't count toward full refresh cadence
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Save bw buffer to reset buffer state after grayscale data sync
  const uint32_t bwStoreHeapBefore = esp_get_free_heap_size();
  const bool storedBwBuffer = renderer.storeBwBuffer();
  const uint32_t bwStoreHeapAfter = esp_get_free_heap_size();
  const auto tBwStore = millis();
  (void)bwStoreHeapBefore;
  (void)bwStoreHeapAfter;
  const bool canApplyGrayscale = needsAnyGrayscale && storedBwBuffer;
  if (needsAnyGrayscale && !storedBwBuffer) {
    LOG_ERR("ERS", "Skipping grayscale enhancement: failed to store BW backup");
  }

  // grayscale rendering
  if (canApplyGrayscale) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
    renderer.copyGrayscaleLsbBuffers();
    const auto tGrayLsb = millis();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
    renderer.copyGrayscaleMsbBuffers();
    const auto tGrayMsb = millis();

    // display grayscale part
    renderer.displayGrayBuffer();
    const auto tGrayDisplay = millis();
    renderer.setRenderMode(GfxRenderer::BW);
    // restore the bw data
    renderer.restoreBwBuffer();
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums bw_store_ok=%d "
            "bw_store_heap_before=%lu bw_store_heap_after=%lu bw_store_heap_delta=%ld "
            "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, storedBwBuffer,
            bwStoreHeapBefore, bwStoreHeapAfter, (int32_t)bwStoreHeapAfter - (int32_t)bwStoreHeapBefore,
            tGrayLsb - tBwStore, tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
  } else {
    if (storedBwBuffer) {
      // Restore the BW data when we skipped grayscale entirely.
      renderer.restoreBwBuffer();
    }
    const auto tBwRestore = millis();

    const auto tEnd = millis();
    LOG_DBG("ERS",
            "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums bw_store_ok=%d "
            "bw_store_heap_before=%lu bw_store_heap_after=%lu bw_store_heap_delta=%ld "
            "bw_restore=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, storedBwBuffer,
            bwStoreHeapBefore, bwStoreHeapAfter, (int32_t)bwStoreHeapAfter - (int32_t)bwStoreHeapBefore,
            tBwRestore - tBwStore, tEnd - t0);
  }
}

void EpubReaderActivity::renderStatusBar() const {
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
  const float bookProgress = getCurrentBookProgressPercent();

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(pageTurnDuration / 1000);

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  const float rawProgress = (pageCount > 0) ? (static_cast<float>(section->currentPage) / pageCount) : 0.0f;
  const bool bookmarked = BOOKMARKS.hasBookmarkForPage(static_cast<uint16_t>(currentSpineIndex), rawProgress,
                                                       section->pageCount > 0 ? section->pageCount : 1);
  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, bookmarked);
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  pageLoadRetryCount = 0;
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  pageLoadRetryCount = 0;
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}
bool EpubReaderActivity::drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer) {
  auto epub = std::make_shared<Epub>(filePath, "/.crosspoint");
  // Load CSS when embeddedStyle is enabled, as createSectionFile may need it to rebuild the cache.
  if (!epub->load(true, SETTINGS.embeddedStyle == 0)) {
    LOG_DBG("SLP", "EPUB: failed to load %s", filePath.c_str());
    return false;
  }

  epub->setupCacheDir();

  // Load saved spine index and page number
  int spineIndex = 0, pageNumber = 0;
  EpubReaderUtils::Progress progress;
  if (EpubReaderUtils::loadProgress(*epub, progress, "SLP")) {
    spineIndex = progress.spineIndex;
    pageNumber = progress.pageNumber;
  }
  if (spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) spineIndex = 0;

  // Apply the reader orientation so margins match what the reader would produce
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Compute margins exactly as render() does
  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += SETTINGS.screenMargin;
  marginLeft += SETTINGS.screenMargin;
  marginRight += SETTINGS.screenMargin;
  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  marginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);

  const uint16_t viewportWidth = renderer.getScreenWidth() - marginLeft - marginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - marginTop - marginBottom;

  // Load or rebuild the section cache. Rebuilding is needed when the cache is missing or stale
  // (e.g. after a firmware update). A no-op popup callback avoids any UI during sleep preparation.
  const int readerFontId = SETTINGS.getReaderFontId();
  const int fallbackFontId = SETTINGS.getBuiltInReaderFontId();
  const bool canUseFallbackFont = renderer.isSdCardFont(readerFontId) && fallbackFontId != readerFontId;
  int renderFontId = readerFontId;
  auto section = std::make_unique<Section>(epub, spineIndex, renderer);
  bool loadedSection = section->loadSectionFile(
      readerFontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
      SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
      SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled);
  if (!loadedSection && canUseFallbackFont) {
    section = std::make_unique<Section>(epub, spineIndex, renderer, FALLBACK_FONT_SECTION_CACHE_SUFFIX);
    loadedSection =
        section->loadSectionFile(fallbackFontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
                                 SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, viewportWidth,
                                 viewportHeight, SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                 SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled);
    if (loadedSection) {
      renderFontId = fallbackFontId;
      LOG_DBG("SLP", "EPUB: using fallback built-in font cache for spine %d", spineIndex);
    }
  }

  if (!loadedSection) {
    if (!MemoryBudget::hasHeapForOptionalEpubRebuild("SLP", "EPUB sleep-page cache rebuild", spineIndex)) {
      return false;
    }

    LOG_DBG("SLP", "EPUB: section cache not found for spine %d, rebuilding (free=%u, maxAlloc=%u)", spineIndex,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    section = std::make_unique<Section>(epub, spineIndex, renderer);
    bool layoutAbortedForLowMemory = false;
    if (!section->createSectionFile(
            readerFontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
            SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
            SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled,
            SETTINGS.guideReadingEnabled, []() {}, nullptr, &layoutAbortedForLowMemory)) {
      if (!layoutAbortedForLowMemory || !canUseFallbackFont) {
        LOG_ERR("SLP", "EPUB: failed to rebuild section cache for spine %d", spineIndex);
        return false;
      }

      LOG_DBG("SLP", "EPUB: retrying sleep-page rebuild with fallback built-in font for spine %d", spineIndex);
      releaseReaderSdFontCachesForLowMemory(renderer, "SLP", "sleep-page fallback font rebuild");
      section = std::make_unique<Section>(epub, spineIndex, renderer, FALLBACK_FONT_SECTION_CACHE_SUFFIX);
      if (!section->createSectionFile(fallbackFontId, SETTINGS.getReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
                                      SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                      SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                                      SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled, []() {})) {
        LOG_ERR("SLP", "EPUB: failed to rebuild fallback section cache for spine %d", spineIndex);
        return false;
      }
      renderFontId = fallbackFontId;
    } else {
      renderFontId = readerFontId;
    }
    releaseReaderSdFontCachesForLowMemory(renderer, "SLP", "sleep-page section cache rebuild");
    LOG_DBG("SLP", "EPUB: section cache rebuilt for spine %d (pages=%u, font=%d, free=%u, maxAlloc=%u)", spineIndex,
            section->pageCount, renderFontId, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }

  if (pageNumber < 0 || pageNumber >= section->pageCount) pageNumber = 0;
  section->currentPage = pageNumber;

  auto page = section->loadPageFromSectionFile();
  if (!page) {
    LOG_DBG("SLP", "EPUB: failed to load page %d", pageNumber);
    return false;
  }

  renderer.clearScreen();
  page->render(renderer, renderFontId, marginLeft, marginTop);
  // No displayBuffer call; caller (SleepActivity) handles that after compositing the overlay.
  return true;
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->pageCount;
    if (epub && epub->getBookSize() > 0 && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}
