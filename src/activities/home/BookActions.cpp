#include "BookActions.h"

#include <BookKey.h>
#include <Epub.h>
#include <Epub/EpubRenderMode.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>

#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DeviceCapabilities.h"
#include "GlobalActions.h"
#include "RecentBookProgress.h"
#include "RecentBooksStore.h"
#include "activities/RenderLock.h"
#include "activities/network/StatsUploadActivity.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/BookStatsActivity.h"
#include "activities/reader/BookStatsTracking.h"
#include "activities/reader/EpubReaderActivity.h"
#include "activities/reader/GlobalReadingStats.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "sync/BookSyncFile.h"
#include "sync/SyncPending.h"
#include "util/BookCacheUtils.h"
#include "util/BookMoveUtils.h"

namespace BookActions {
namespace {

bool hasReadingStats(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path);
}

std::string bookStatsCachePath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return Epub(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, "/.crosspoint").getCachePath();
  }
  return "";
}

}  // namespace

std::vector<FileBrowserActionActivity::MenuItem> buildBookActionItems(const std::string& fullPath,
                                                                      const bool includeRemoveFromRecents) {
  std::vector<FileBrowserActionActivity::MenuItem> items;
  items.reserve(includeRemoveFromRecents ? 10 : 9);
  items.push_back({FileBrowserAction::Delete, StrId::STR_DELETE});
  if (hasClearableBookCache(fullPath)) {
    items.push_back({FileBrowserAction::DeleteCache, StrId::STR_DELETE_CACHE});
  }
  // XTC books have no KOReader position, but their reading stats still sync.
  if (FsHelpers::hasEpubExtension(fullPath) || FsHelpers::hasXtcExtension(fullPath)) {
    items.push_back({FileBrowserAction::SyncProgress, StrId::STR_SYNC_BOOK});
  }
  if (FsHelpers::hasEpubExtension(fullPath)) {
    items.push_back({FileBrowserAction::EpubRenderMode, StrId::STR_EPUB_RENDER_MODE});
    items.push_back({FileBrowserAction::ResetReaderSettings, StrId::STR_RESET_BOOK_READER_SETTINGS});
  }
  if (hasReadingStats(fullPath)) {
    if (SETTINGS.shouldTrackReadingStats()) {
      const bool bookEnabled = isBookStatsTrackingEnabled(fullPath);
      items.push_back({FileBrowserAction::ToggleBookStatsTracking, StrId::STR_TRACK_READING_STATS,
                       bookEnabled ? StrId::STR_ON : StrId::STR_OFF});
      if (bookEnabled) {
        items.push_back({FileBrowserAction::ReadingStats, StrId::STR_READING_STATS});
        items.push_back({FileBrowserAction::DeleteStats, StrId::STR_DELETE_BOOK_STATS});
      }
    }
    items.push_back({FileBrowserAction::ToggleCompleted,
                     isBookCompleted(fullPath) ? StrId::STR_MARK_UNFINISHED : StrId::STR_MARK_FINISHED});
  }
  if (includeRemoveFromRecents) {
    items.push_back({FileBrowserAction::RemoveFromRecents, StrId::STR_REMOVE_FROM_RECENTS_ACTION});
  }
  return items;
}

void syncProgress(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& fullPath,
                  PendingOverlayResume returnResume) {
  if (FsHelpers::hasXtcExtension(fullPath) && Storage.exists(fullPath.c_str())) {
    if (!KOREADER_STORE.hasCredentials()) {
      startGlobalSyncProgress();  // Opens account settings, same as for EPUB.
      return;
    }
    // No position to map and no reader to resume: send this book's stats in place.
    auto sync =
        makeUniqueNoThrow<StatsUploadActivity>(renderer, mappedInput, fullPath, StatsUploadActivity::Scope::Book);
    if (!sync) {
      LOG_ERR("BookActions", "Cannot allocate XTC sync: %s", fullPath.c_str());
      drawToast(renderer, tr(STR_SYNC_FAILED_MSG));
      return;
    }
    sync->setReturnTo(std::move(returnResume));
    activityManager.replaceActivity(std::move(sync));
    return;
  }
  if (!FsHelpers::hasEpubExtension(fullPath) || !Storage.exists(fullPath.c_str())) {
    LOG_ERR("BookActions", "Cannot sync missing or unsupported book: %s", fullPath.c_str());
    drawToast(renderer, tr(STR_SYNC_FAILED_MSG));
    return;
  }
  if (!KOREADER_STORE.hasCredentials()) {
    startGlobalSyncProgress();  // Opens account settings without changing the current book.
    return;
  }

  drawToast(renderer, tr(STR_LOADING));
  {
    // A never-opened book needs its spine/TOC cache for progress mapping.
    // Keep parsing off the network boot and release this fallible allocation
    // before TLS; no CSS, page layout, or resident location tables are needed.
    auto epub = makeUniqueNoThrow<Epub>(fullPath, "/.crosspoint");
    if (!epub || !epub->load(true, true, Epub::XLocationLoadMode::Skip, true)) {
      LOG_ERR("BookActions", "Could not prepare sync metadata: %s", fullPath.c_str());
      drawToast(renderer, tr(STR_SYNC_FAILED_MSG));
      return;
    }
  }

  // The existing network reboot resumes from this saved path. Persist the
  // selected book before rebooting so a menu action never syncs the previous book.
  std::string previousPath = APP_STATE.openEpubPath;
  auto previousOverlay = std::move(APP_STATE.pendingOverlayResume);
  APP_STATE.openEpubPath = fullPath;
  APP_STATE.pendingOverlayResume = std::move(returnResume);
  if (!APP_STATE.saveToFile()) {
    APP_STATE.openEpubPath = std::move(previousPath);
    APP_STATE.pendingOverlayResume = std::move(previousOverlay);
    LOG_ERR("BookActions", "Could not save sync target: %s", fullPath.c_str());
    drawToast(renderer, tr(STR_SAVE_PROGRESS_FAILED));
    return;
  }
  startGlobalSyncProgress();
}

bool hasClearableBookCache(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path);
}

bool canSendNearby(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasTxtExtension(path) || FsHelpers::hasXtcExtension(path) ||
         FsHelpers::hasPngExtension(path) || FsHelpers::hasBmpExtension(path);
}

void clearFileMetadata(const std::string& fullPath) {
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crosspoint").clearCache();
    BookmarkStore::deleteForFilePath(fullPath, "epub");
    ClippingStore::deleteForFilePath(fullPath, "epub");
  } else if (FsHelpers::hasXtcExtension(fullPath)) {
    BookmarkStore::deleteForFilePath(fullPath, "xtc");
  } else if (FsHelpers::hasTxtExtension(fullPath) || FsHelpers::hasMarkdownExtension(fullPath)) {
    BookmarkStore::deleteForFilePath(fullPath, "txt");
  }
}

bool clearBookCache(const std::string& fullPath) {
  if (FsHelpers::hasEpubExtension(fullPath) || FsHelpers::hasXtcExtension(fullPath)) {
    return clearBookCachePreservingUserState(fullPath);
  }
  return false;
}

bool deleteBookStats(const std::string& fullPath) {
  const std::string cachePath = bookStatsCachePath(fullPath);
  if (cachePath.empty()) {
    return false;
  }
  return BookReadingStats::remove(cachePath);
}

std::unique_ptr<Activity> createReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const std::string& fullPath, const std::string& title) {
  const std::string cachePath = bookStatsCachePath(fullPath);
  if (cachePath.empty() || !BookStatsTracking::isEnabled(cachePath)) {
    LOG_ERR("BookActions", "No reading stats for: %s", fullPath.c_str());
    return {};
  }
  if (!Storage.exists(cachePath.c_str()) && !Storage.mkdir(cachePath.c_str())) {
    LOG_ERR("BookActions", "Could not create stats cache for: %s", fullPath.c_str());
    return {};
  }

  const RecentBook book{fullPath, title, {}, {}};
  const float progress = FsHelpers::hasEpubExtension(fullPath) ? RecentBookProgress::loadCachedEpubPercent(book)
                                                               : RecentBookProgress::loadPercent(book);
  const BookReadingStats stats = BookReadingStats::load(cachePath);
  const GlobalReadingStats global = GlobalReadingStats::load();
  auto activity =
      GlobalReadingStats::hasSyncedStats()
          ? makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, title, cachePath, stats, progress, false, 0,
                                                 global, GlobalReadingStats::loadAggregated(global))
          : makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, title, cachePath, stats, progress, false, 0,
                                                 global);
  if (activity) activity->setSyncBookPath(fullPath);
  return activity;
}

bool resetBookReaderSettings(const std::string& fullPath) {
  if (!FsHelpers::hasEpubExtension(fullPath)) {
    return false;
  }
  return EpubReaderActivity::resetBookReaderSettings(fullPath);
}

std::vector<std::string> epubRenderModeOptions() {
  return {I18N.get(StrId::STR_RENDER_MODE_CROSSINK_DEFAULT), I18N.get(StrId::STR_RENDER_MODE_BALANCED),
          I18N.get(StrId::STR_RENDER_MODE_LIGHT)};
}

uint8_t epubRenderModeDisplayIndex(const uint8_t renderMode) {
  switch (static_cast<EpubRenderMode>(renderMode)) {
    case EpubRenderMode::Balanced:
      return 1;
    case EpubRenderMode::Light:
      return 2;
    case EpubRenderMode::CrossInkDefault:
    default:
      return 0;
  }
}

uint8_t epubRenderModeForDisplayIndex(const uint8_t displayIndex) {
  switch (displayIndex) {
    case 1:
      return static_cast<uint8_t>(EpubRenderMode::Balanced);
    case 2:
      return static_cast<uint8_t>(EpubRenderMode::Light);
    case 0:
    default:
      return static_cast<uint8_t>(EpubRenderMode::CrossInkDefault);
  }
}

std::string confirmationHeading(const StrId actionLabelId) {
  return std::string(tr(STR_CONFIRM)) + ": " + std::string(I18N.get(actionLabelId));
}

bool isBookCompleted(const std::string& fullPath) {
  const std::string cachePath = bookStatsCachePath(fullPath);
  return !cachePath.empty() && BookReadingStats::load(cachePath).isCompleted;
}

bool isBookCompletedForList(const std::string& fullPath) {
  std::string cachePath;
  if (FsHelpers::hasEpubExtension(fullPath)) {
    // Epub's constructor may rename a legacy cache folder; list rendering
    // should only read, so probe the stable path and then the legacy one.
    cachePath = Epub::cachePathForFilePath(fullPath, "/.crosspoint");
    if (!Storage.exists(cachePath.c_str())) {
      cachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(fullPath));
      if (!Storage.exists(cachePath.c_str())) return false;
    }
  } else if (FsHelpers::hasXtcExtension(fullPath)) {
    cachePath = Xtc(fullPath, "/.crosspoint").getCachePath();
    if (!Storage.exists(cachePath.c_str())) return false;
  } else {
    return false;
  }
  return BookReadingStats::peekCompleted(cachePath);
}

bool isBookStatsTrackingEnabled(const std::string& fullPath) {
  return BookStatsTracking::isBookEnabled(bookStatsCachePath(fullPath));
}

bool toggleBookStatsTracking(const std::string& fullPath, bool& enabled) {
  if (!hasReadingStats(fullPath) || !SETTINGS.shouldTrackReadingStats()) return false;
  const std::string cachePath = bookStatsCachePath(fullPath);
  enabled = !BookStatsTracking::isBookEnabled(cachePath);
  if (BookStatsTracking::setBookEnabled(cachePath, enabled)) return true;
  enabled = BookStatsTracking::isBookEnabled(cachePath);
  return false;
}

bool toggleBookCompleted(const std::string& fullPath, const std::string& displayName, bool& completed) {
  const bool isEpub = FsHelpers::hasEpubExtension(fullPath);
  const bool isXtc = FsHelpers::hasXtcExtension(fullPath);
  if (!isEpub && !isXtc) {
    return false;
  }

  Epub epub(fullPath, "/.crosspoint");
  Xtc xtc(fullPath, "/.crosspoint");
  std::string cachePath;
  if (isEpub) {
    epub.setupCacheDir();
    cachePath = epub.getCachePath();
  } else {
    if (!xtc.load()) {
      return false;
    }
    xtc.setupCacheDir();
    cachePath = xtc.getCachePath();
  }

  BookReadingStats stats = BookReadingStats::load(cachePath);
  completed = !stats.isCompleted;
  stats.isCompleted = completed;
  const bool trackStats = BookStatsTracking::isEnabled(cachePath);
  if (completed && trackStats && !stats.finishedDateManual) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) {
      stats.finishedDate = now.date;
    }
  }

  if (!stats.save(cachePath)) {
    LOG_ERR("BookActions", "Could not save completion for: %s", fullPath.c_str());
    return false;
  }
  SyncPending::mark(fullPath, SyncPending::STATS_BIT);
  BookSyncFile::onFinishedChanged(fullPath, completed);
  if (trackStats) {
    GlobalReadingStats globalStats = GlobalReadingStats::load();
    if (completed) {
      globalStats.completedBooks++;
    } else if (globalStats.completedBooks > 0) {
      globalStats.completedBooks--;
    }
    globalStats.save();
  }

  // Changing completion status does not open a book. The reader adds it to
  // recents if it is opened again after being marked unfinished.
  if (SETTINGS.removeReadBooksFromRecents && completed) RECENT_BOOKS.removeByPath(fullPath);

  if (isEpub && completed && SETTINGS.moveFinishedToReadFolder && fullPath.rfind("/Read/", 0) != 0) {
    // This EPUB has not been opened by the reader, so getTitle()/getAuthor()
    // would be empty. Load only metadata before migrating the saved items.
    std::string title;
    std::string author;
    if (!epub.loadMetadata(title, author)) {
      LOG_ERR("BookActions", "Could not load metadata for completed book: %s", fullPath.c_str());
    }
    if (title.empty()) title = displayName.empty() ? fullPath.substr(fullPath.find_last_of('/') + 1) : displayName;
    const std::string oldCachePath = epub.getCachePath();
    const std::string dstPath = BookMoveUtils::buildReadFolderDestination(fullPath);
    LOG_INF("BookActions", "Moving completed epub: %s -> %s", fullPath.c_str(), dstPath.c_str());
    if (!bookkey::moveFile(fullPath, dstPath)) {
      LOG_ERR("BookActions", "Failed to move book to 'Read' folder");
      snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s",
               tr(STR_MOVE_TO_READ_FAILED_TITLE));
      snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), tr(STR_MOVE_TO_READ_FAILED_BODY),
               displayName.c_str());
      APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
      APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
      return true;
    }

    BookMoveUtils::migrateMovedEpubState(fullPath, dstPath, oldCachePath, title, author,
                                         !SETTINGS.removeReadBooksFromRecents);
  }

  return true;
}

void drawToast(const GfxRenderer& renderer, const char* msg, const char* detail) {
  constexpr int toastPadX = 20;
  constexpr int toastPadY = 12;
  if (detail) {
    int top = 0, right = 0, bottom = 0, left = 0;
    renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
    const int maxWidth = renderer.getScreenWidth() - left - right - toastPadX * 2;
    // A rare, explicit toggle on touch devices: use the renderer's bounded
    // wrapping so longer translations stay inside calibrated screen edges.
    auto lines = renderer.wrappedText(UI_10_FONT_ID, detail, maxWidth, 4);
    const auto title = renderer.truncatedText(UI_10_FONT_ID, msg, maxWidth);
    int textWidth = renderer.getTextWidth(UI_10_FONT_ID, title.c_str());
    for (const auto& line : lines) textWidth = std::max(textWidth, renderer.getTextWidth(UI_10_FONT_ID, line.c_str()));
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    constexpr int detailGap = 8;
    const int width = textWidth + toastPadX * 2;
    const int height = lineHeight * (1 + static_cast<int>(lines.size())) + detailGap + toastPadY * 2;
    const int x = left + (renderer.getScreenWidth() - left - right - width) / 2;
    const int y = top + (renderer.getScreenHeight() - top - bottom - height) / 2;
    renderer.fillRect(x, y, width, height, true);
    const Rect textArea{x + toastPadX, y + toastPadY, textWidth, height - toastPadY * 2};
    UITheme::drawCenteredText(renderer, textArea, UI_10_FONT_ID, y + toastPadY, title.c_str(), false);
    int lineY = y + toastPadY + lineHeight + detailGap;
    for (const auto& line : lines) {
      UITheme::drawCenteredText(renderer, textArea, UI_10_FONT_ID, lineY, line.c_str(), false);
      lineY += lineHeight;
    }
    renderer.displayBuffer();
    return;
  }
  const int msgW = renderer.getTextWidth(UI_10_FONT_ID, msg);
  const int msgH = renderer.getLineHeight(UI_10_FONT_ID);
  const int toastW = msgW + toastPadX * 2;
  const int toastH = msgH + toastPadY * 2;
  const int toastX = (renderer.getScreenWidth() - toastW) / 2;
  const int toastY = (renderer.getScreenHeight() - toastH) / 2;
  renderer.fillRect(toastX, toastY, toastW, toastH, true);
  renderer.drawText(UI_10_FONT_ID, toastX + toastPadX, toastY + toastPadY, msg, false);
  renderer.displayBuffer();
}

void showTouchscreenToast(const GfxRenderer& renderer, const bool readerContext, const TouchscreenToggleSource source) {
  const bool globalLock = deviceUsesGlobalTouchscreenLock();
  const bool disabled = SETTINGS.disableReaderTouchscreen;
  const bool showRecoveryHint =
      source == TouchscreenToggleSource::FrontlightDrawer && globalLock && disabled && !readerContext;
  {
    RenderLock lock;
    const auto message =
        globalLock ? (disabled ? StrId::STR_TOUCHSCREEN_DISABLED_GLOBAL : StrId::STR_TOUCHSCREEN_ENABLED_GLOBAL)
                   : (disabled ? StrId::STR_TOUCHSCREEN_DISABLED : StrId::STR_TOUCHSCREEN_ENABLED);
    drawToast(renderer, I18N.get(message), showRecoveryHint ? tr(STR_TOUCHSCREEN_REENABLE_HINT) : nullptr);
  }
  delay(showRecoveryHint ? 2500 : 1000);
}

}  // namespace BookActions
