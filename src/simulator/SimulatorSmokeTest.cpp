#ifdef SIMULATOR

#include "SimulatorSmokeTest.h"

#include <Epub.h>
#include <Epub/WordSpacing.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <HalScreenCalibration.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <SDL.h>
#include <WiFi.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>

#include "AssetDownloadSmokeTest.h"
#include "BookFontSystem.h"
#include "BookmarkListSmokeTest.h"
#include "ClippingSelectionSmokeTest.h"
#include "OpdsCatalogSmokeTest.h"
#include "PluginSmokeTest.h"
#include "ReadingUploadSmokeTest.h"
#include "SilentRestart.h"
#include "StatsUploadSmokeTest.h"
#include "util/WordSelectNavigator.h"
#if CROSSINK_SCALABLE_FONTS
#include <HalScalableFont.h>

#include <fstream>

#include "FontInstaller.h"
#include "TtfRenderProfileStore.h"
#endif
#include <AppVersion.h>
#include <ArduinoJson.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <SupportInfo.h>

#include <memory>
#include <vector>

#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DeviceCapabilities.h"
#include "FilenameFontSystem.h"
#include "HyphenationPackStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "ReleaseNotes.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "SupportInfoExport.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/home/AlertActivity.h"
#include "activities/home/BookActions.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/home/HomeActivity.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/library/LibraryActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/network/StatsUploadActivity.h"
#include "activities/network/UsbDriveActivity.h"
#include "activities/reader/BookLoadingFeedback.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/BookStatsActivity.h"
#include "activities/reader/BookStatsView.h"
#include "activities/reader/DictionaryDefinitionActivity.h"
#include "activities/reader/EndOfBookOptions.h"
#include "activities/reader/EpubReaderActivity.h"
#include "activities/reader/EpubReaderBookmarkListActivity.h"
#include "activities/reader/EpubReaderChapterSelectionActivity.h"
#include "activities/reader/EpubReaderClippingListActivity.h"
#include "activities/reader/EpubReaderDrawerActivity.h"
#include "activities/reader/EpubReaderFootnotesActivity.h"
#include "activities/reader/EpubReaderPercentSelectionActivity.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/KOReaderSyncActivity.h"
#include "activities/reader/QrDisplayActivity.h"
#include "activities/reader/ReaderButtonShortcuts.h"
#include "activities/reader/ReaderFontLoading.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/reader/TxtReaderActivity.h"
#include "activities/reader/XtcReaderActivity.h"
#include "activities/reader/XtcReaderMenuActivity.h"
#include "activities/settings/AboutActivity.h"
#include "activities/settings/AssetDownloadActivity.h"
#include "activities/settings/FontDownloadActivity.h"
#include "activities/settings/FontSelectionActivity.h"
#include "activities/settings/FrontlightTimePickerActivity.h"
#include "activities/settings/HyphenationManagerActivity.h"
#include "activities/settings/KOReaderSettingsActivity.h"
#include "activities/settings/OtaUpdateActivity.h"
#include "activities/settings/QuickActionsActivity.h"
#include "activities/settings/ReleaseNotesActivity.h"
#include "activities/settings/ScreenCalibrationActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/settings/StatusBarSettingsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/FrontlightPanelActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/CompactHeader.h"
#include "components/HeaderDate.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "simulator/SimulatorHomeKeyInput.h"
#include "sync/BookSyncFile.h"
#include "sync/SyncIds.h"
#include "sync/SyncParked.h"
#include "sync/SyncPending.h"
#include "sync/SyncState.h"
#include "util/ButtonShortcutController.h"
#include "util/Dictionary.h"

extern ActivityManager activityManager;
extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

// Use a detached reader so these boundary checks cannot race its render task.
struct EpubReaderCompletionSmokeTest {
  static std::optional<uint32_t> statusAnchor;
#if !CROSSINK_SCALABLE_FONTS
  static bool fontReload(EpubReaderActivity& reader) {
    RenderLock lock;
    if (!reader.section || reader.section->isBuilding()) return false;
    const int spine = reader.currentSpineIndex;
    const auto anchor = reader.section->getVisibleTextOffsetForPage(reader.section->currentPage);
    // Model a font registration removed by a dictionary/reload handoff. The
    // reader must reconcile its live section before attempting any prewarm.
    reader.activeSectionFontId = std::numeric_limits<int>::min();
    reader.render(std::move(lock));
    if (!reader.section || reader.currentSpineIndex != spine ||
        reader.activeSectionFontId != SETTINGS.getReaderFontId())
      return false;
    const auto page = reader.section->currentPage;
    const auto start = reader.section->getVisibleTextOffsetForPage(page);
    const auto next =
        page + 1 < reader.section->pageCount ? reader.section->getVisibleTextOffsetForPage(page + 1) : std::nullopt;
    return !anchor || (start && *start <= *anchor && (!next || *next > *anchor));
  }
#endif
  static bool openStatusSettings(EpubReaderActivity& reader) {
    Section* original = nullptr;
    const bool firstEdit = !statusAnchor;
    {
      RenderLock lock;
      if (!reader.section || reader.section->isBuilding()) return false;
      original = reader.section.get();
      if (!statusAnchor && reader.section->pageCount > 1) reader.section->currentPage = 1;
      if (!statusAnchor) statusAnchor = reader.section->getVisibleTextOffsetForPage(reader.section->currentPage);
      if (!statusAnchor) return false;
      SETTINGS.displayStatusBarTextSize = 2;
    }
    reader.endGlobalSettingsEdit();
    if (reader.section.get() != original) return false;  // Global font cannot repaginate the book.
    if (firstEdit) {
      // Two settings changes before the chapter can rebuild must retain the
      // first content anchor even while there is no current Section.
      const bool originallyHidden = SETTINGS.bottomReaderStatusBar.hidden;
      for (const bool hidden : {!originallyHidden, originallyHidden}) {
        {
          RenderLock lock;
          SETTINGS.bottomReaderStatusBar.hidden = hidden;
        }
        reader.endGlobalSettingsEdit();
        if (reader.cachedVisibleTextOffset != statusAnchor || reader.statusBarRelayoutOffset != statusAnchor)
          return false;
      }
    }
    reader.onReaderMenuConfirm(EpubReaderMenuAction::STATUS_BAR_SETTINGS, false);
    return true;
  }
  static bool statusSettingsReturned(EpubReaderActivity& reader) {
    RenderLock lock;
    if (!reader.section || reader.pendingRelayoutReposition) return false;
    const auto page = reader.section->currentPage;
    const auto start = reader.section->getVisibleTextOffsetForPage(page);
    const auto next =
        page + 1 < reader.section->pageCount ? reader.section->getVisibleTextOffsetForPage(page + 1) : std::nullopt;
    return !statusAnchor || (start && *start <= *statusAnchor && (!next || *next > *statusAnchor));
  }
  static bool readingPace(EpubReaderActivity& reader) {
    for (const uint32_t sample : {20U, 40U}) {
      reader.stats.setAverageForwardPageMillis(30000);
      reader.stats.paceSampleCount = 100;
      reader.sessionStartPaceMillis = 30000;
      reader.sessionStartPaceSampleCount = 100;
      reader.sessionPaceSampleSeconds = 0;
      reader.sessionPaceSampleCount = 0;
      reader.paceSampleWarmupPending = true;
      reader.recordForwardPagePaceSample(sample, "pace_smoke_warmup");
      reader.recordForwardPagePaceSample(1, "pace_smoke_fast");
      reader.recordForwardPagePaceSample(SETTINGS.getReadingIdleTimeThresholdSeconds() + 1, "pace_smoke_idle");
      if (reader.sessionPaceSampleCount != 0 || reader.stats.paceSampleCount != 100) return false;
      for (uint16_t count = 1; count <= 12; ++count) {
        reader.recordForwardPagePaceSample(sample, "pace_smoke");
        const uint32_t expected = count < 10 ? 30000 + (static_cast<int>(sample) - 30) * 100 * count : sample * 1000;
        float seconds = 0;
        const char* source = nullptr;
        uint16_t samples = 0;
        if (!reader.getTimeLeftPaceSeconds(seconds, source, samples) || samples != count ||
            static_cast<uint32_t>(seconds * 1000.0f + 0.5f) != expected ||
            reader.stats.averageForwardPageMillis() != expected)
          return false;
      }
    }
    reader.resetReadingPaceData();
    float seconds = 0;
    const char* source = nullptr;
    uint16_t samples = 0;
    if (reader.stats.averageForwardPageMillis() != 0 || reader.getTimeLeftPaceSeconds(seconds, source, samples))
      return false;
    reader.recordForwardPagePaceSample(30, "pace_smoke_reset_warmup");
    for (int i = 0; i < 3; ++i) reader.recordForwardPagePaceSample(30, "pace_smoke_reset_sample");
    if (!reader.getTimeLeftPaceSeconds(seconds, source, samples) || seconds != 30.0f) return false;
    reader.recordForwardPagePaceSample(28, "pace_smoke_fraction");
    if (!reader.getTimeLeftPaceSeconds(seconds, source, samples) || seconds != 29.5f ||
        reader.stats.averageForwardPageMillis() != 29500)
      return false;
    LOG_INF("SMOKE", "EPUB reading pace: filters, gradual handover, persisted pace and reset passed");
    return true;
  }
  static bool run(EpubReaderActivity& active, void (*capture)(const char*)) {
    auto reader = std::make_unique<EpubReaderActivity>(renderer, mappedInputManager, nullptr,
                                                       EpubReaderActivity::BookReaderSettingsData{}, 1);
    if (!readingPace(*reader)) return false;
    reader->epub = active.epub;
    const int spineCount = reader->epub->getSpineItemsCount();
    if (spineCount <= 0) return false;

    if (reader->epub->hasStablePageNumbers()) {
      reader->section = std::make_unique<Section>(reader->epub, 0, renderer, "_eta_smoke");
      const auto spec = SETTINGS.readerRenderSpec(160, 200, EpubRenderMode::CrossInkDefault);
      if (!reader->section->startBuild(spec)) return false;
      while (reader->section->pageCount < 5 && !reader->section->isBuildComplete()) {
        if (!reader->section->buildSomeMore(1)) return false;
      }
      if (!reader->section->isBuilding() || reader->section->pageCount < 5) return false;
      reader->section->currentPage = 0;
      reader->statsTrackingActive = false;
      reader->refreshStableTextProgress();
      if (!reader->hasStableTextPosition) return false;
      // An established book estimate survives a new session's text-pace warmup,
      // even before this incremental chapter supplies a usable page density.
      reader->lastBookTimeLeftSeconds = 1800;
      uint32_t retained = 0;
      if (!reader->estimateTimeLeftSeconds(true, retained) || retained != 1800) return false;
      char retainedLabel[24] = {};
      char expectedLabel[24] = {};
      formatCompactReadingDuration(1800, expectedLabel, sizeof(expectedLabel));
      if (!reader->formatTimeLeftLabel(retainedLabel, sizeof(retainedLabel), true) ||
          std::strcmp(retainedLabel, expectedLabel))
        return false;
      reader->refreshCachedTimeLeftEstimate();
      if (reader->stats.estimatedTimeLeftSeconds != 1800) return false;
      const auto captureEstimate = [&](const char* name) {
        RenderLock lock;
        const auto originalBar = SETTINGS.bottomReaderStatusBar;
        const int originalHeight = UITheme::getStatusBarHeight(renderer);
        SETTINGS.bottomReaderStatusBar.hidden = false;
        SETTINGS.bottomReaderStatusBar.slots = {};
        SETTINGS.bottomReaderStatusBar.slots[ReaderStatusBarConfig::CENTER] = ReaderStatusBarItem::TimeLeftBook;
        const int height = std::max(originalHeight, UITheme::getStatusBarHeight(renderer));
        int top, right, bottom, left;
        renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
        renderer.fillRect(left, renderer.getScreenHeight() - bottom - height, renderer.getScreenWidth() - left - right,
                          height, ReaderUtils::readerDarkModeEnabled());
        reader->renderStatusBar();
        capture(name);
        SETTINGS.bottomReaderStatusBar = originalBar;
      };
      captureEstimate("book-time-left-warmup");
      const double start = reader->stableTextPosition;
      reader->paceSampleWarmupPending = false;
      for (int page = 1; page <= 3; ++page) {
        reader->pageShownAtMs = millis() - 10000UL;
        reader->pageTurn(true, "stable_eta_smoke");
        if (reader->pendingStableReadSeconds != 10 || reader->section->currentPage != page) return false;
        reader->refreshStableTextProgress();
        if (page < 3 && (!reader->estimateTimeLeftSeconds(true, retained) || retained != 1800)) return false;
      }
      uint32_t actual = 0;
      const double distance = reader->stableTextPosition - start;
      if (distance <= 0) return false;
      const uint32_t expected =
          static_cast<uint32_t>((reader->stableTextTotal - reader->stableTextPosition) * 30.0 / distance + 0.5);
      if (!reader->estimateTimeLeftSeconds(true, actual) || actual != expected) return false;
      captureEstimate("book-time-left-learned");
      reader->section->suspendBuild();
      reader->refreshStableTextProgress();
      uint32_t suspended = 0;
      if (!reader->hasStableTextPosition || !reader->estimateTimeLeftSeconds(true, suspended) || suspended != actual)
        return false;
      reader->section.reset();
      reader->hasStableTextPosition = false;
      if (!reader->estimateTimeLeftSeconds(true, retained) || retained != actual) return false;
      reader->refreshCachedTimeLeftEstimate();
      if (reader->stats.estimatedTimeLeftSeconds != actual) return false;
      if (reader->estimateTimeLeftSeconds(false, retained)) return false;  // Never use book ETA as chapter ETA.
      reader->resetReadingPaceData();
      if (reader->estimateTimeLeftSeconds(true, retained)) return false;
      reader->stableReadingPace = {};
      reader->statsTrackingActive = true;
      if (!readingPace(*reader)) return false;
      LOG_INF("SMOKE",
              "Stable book ETA: retained warmup, fresh handover, missing chapter, saved fallback and reset passed");
    }

    // Exercise the real ETA path with a chapter count that changes 40 -> 100
    // between its first two pages. Established book density must survive both
    // the chapter switch and refresh; historical time cannot override measured pace.
    if (spineCount > 1) {
      reader->section = std::make_unique<Section>(reader->epub, 0, renderer);
      const uint32_t bytes = reader->epub->getCumulativeSpineItemSize(0);
      if (bytes == 0) return false;
      reader->bookPageEstimate.observe(spineCount, bytes, 100, 100, 32, true);
      reader->section->pageCount = 40;
      reader->section->currentPage = 0;
      reader->refreshBookPageEstimate();
      uint32_t first = 0;
      if (!reader->estimateTimeLeftSeconds(true, first)) return false;
      // Without a retained value, usable page pace bridges text-pace warmup.
      reader->lastBookTimeLeftSeconds = 0;
      reader->hasStableTextPosition = true;
      reader->stableTextSpine = reader->currentSpineIndex;
      reader->stableTextPage = reader->section->currentPage;
      reader->stableTextTotal = 200000;
      reader->stableTextPosition = 100000;
      uint32_t provisional = 0;
      if (!reader->estimateTimeLeftSeconds(true, provisional) || provisional != first) return false;
      reader->hasStableTextPosition = false;
      reader->section->pageCount = 100;
      reader->section->currentPage = 1;
      reader->refreshBookPageEstimate();
      uint32_t second = 0;
      if (!reader->estimateTimeLeftSeconds(true, second) || second > first || first - second > 31) return false;
      char label[24] = {};
      if (!reader->formatTimeLeftLabel(label, sizeof(label), true) || !label[0]) return false;
      // Stable distance and pace must override both chapter estimates and
      // accumulated historical time (which may include rereading).
      reader->hasStableTextPosition = true;
      reader->stableTextSpine = reader->currentSpineIndex;
      reader->stableTextPage = reader->section->currentPage;
      reader->stableTextTotal = 200000;
      reader->stableTextPosition = 100000;
      reader->stableReadingPace.record(30, 1000);
      reader->stableReadingPace.record(30, 1000);
      reader->stableReadingPace.record(30, 1000);
      reader->stats.totalReadingSeconds = 100000;
      uint32_t stableSeconds = 0;
      if (!reader->estimateTimeLeftSeconds(true, stableSeconds) || stableSeconds != 3000) return false;
      reader->section->pageCount = 400;
      reader->refreshBookPageEstimate();
      uint32_t revisedSeconds = 0;
      if (!reader->estimateTimeLeftSeconds(true, revisedSeconds) || revisedSeconds != stableSeconds) return false;
      reader->stableTextPosition += 1000;
      reader->stableReadingPace.record(30, 1000);
      if (!reader->estimateTimeLeftSeconds(true, revisedSeconds) || revisedSeconds != 2970) return false;
      reader->hasStableTextPosition = false;
      // Once the final section is indexed, its known remaining pages take
      // precedence even when there is no session density to extrapolate from.
      reader->statsTrackingActive = false;
      reader->currentSpineIndex = spineCount - 1;
      reader->bookPageEstimate = {};
      reader->section->pageCount = 3;
      reader->section->currentPage = 2;
      uint32_t finalSeconds = 0;
      if (!reader->estimateTimeLeftSeconds(true, finalSeconds) || finalSeconds != 30) return false;
      reader->statsTrackingActive = true;
      reader->stats.totalReadingSeconds = 0;
      reader->currentSpineIndex = 0;
      reader->prepareCurrentSectionForRelayout();
      if (reader->bookPageEstimate.pagesPerByte() != 0) return false;
      reader->section.reset();
      // Deleting book stats must clear both display fallback and text pace.
      reader->resetCurrentBookStatsAfterDelete();
      uint32_t deletedSeconds = 0;
      if (reader->estimateTimeLeftSeconds(true, deletedSeconds) ||
          reader->stableReadingPace.estimate(1000, deletedSeconds))
        return false;
      reader->pendingRelayoutReposition = false;
      reader->nextPageNumber = 0;
      LOG_INF("SMOKE", "Book time-left: chapter opening, revised count, stable pace, final page and relayout passed");
    }

    // Both shortcut entry points must tolerate a chapter that has not loaded yet.
    for (const auto action :
         {CrossPointSettings::SHORT_PWRBTN::PAGE_TURN, CrossPointSettings::SHORT_PWRBTN::PREVIOUS_PAGE}) {
      reader->handleShortcutAction(action);
      reader->handleShortcutAction(static_cast<uint8_t>(action));
      if (reader->section || reader->currentSpineIndex != 0) return false;
    }
    reader->pageTurn(true, "auto");
    reader->pageTurn(false, "test");

    // Previous-page shortcuts on the plain end screen return to the final page.
    for (const bool homeButton : {false, true}) {
      reader->currentSpineIndex = spineCount;
      reader->pendingPageJump = 0;
      if (homeButton) {
        reader->handleShortcutAction(static_cast<uint8_t>(CrossPointSettings::SHORT_PWRBTN::PREVIOUS_PAGE));
      } else {
        reader->handleShortcutAction(CrossPointSettings::SHORT_PWRBTN::PREVIOUS_PAGE);
      }
      if (reader->currentSpineIndex != spineCount - 1 || reader->nextPageNumber != 0 ||
          reader->pendingPageJump != std::numeric_limits<uint16_t>::max())
        return false;
    }
    reader->currentSpineIndex = spineCount;
    reader->handleShortcutAction(CrossPointSettings::SHORT_PWRBTN::PAGE_TURN);
    LOG_INF("SMOKE", "EPUB completion shortcuts: missing chapter and end-screen return passed");
    return true;
  }
};
std::optional<uint32_t> EpubReaderCompletionSmokeTest::statusAnchor;

// Inspect values after dispatching physical key edges through each real slider loop.
struct SliderOrientationSmokeTest {
  static int value(const IntervalSelectionActivity& slider) { return slider.value; }
  static int value(const EpubReaderPercentSelectionActivity& slider) { return slider.value; }
  static void open(EpubReaderDrawerActivity& drawer, const ReaderDrawerPane pane, const uint8_t focus) {
    drawer.openPane(pane);
    drawer.buttonSliderState.focus = focus;
    drawer.draft.lineHeightPercent = 100;
    drawer.draft.wordSpacing = WordSpacing::fromLevel(0);
    drawer.draft.characterSpacing = 5;
    drawer.draft.screenMarginVertical = 20;
    drawer.draft.screenMarginHorizontal = 20;
    drawer.percent = 5000;
    drawer.autoPageTurnIntervalSeconds = 30;
  }
  static int focus(const EpubReaderDrawerActivity& drawer) { return drawer.buttonSliderState.focus; }
  static int value(const EpubReaderDrawerActivity& drawer) {
    switch (drawer.state.pane) {
      case ReaderDrawerPane::Spacing:
        return drawer.buttonSliderState.focus == 0 ? drawer.draft.lineHeightPercent
                                                   : WordSpacing::level(drawer.draft.wordSpacing);
      case ReaderDrawerPane::Margins:
        return drawer.buttonSliderState.focus == 0 ? drawer.draft.screenMarginVertical
                                                   : drawer.draft.screenMarginHorizontal;
      case ReaderDrawerPane::CharacterSpacing:
        return drawer.draft.characterSpacing;
      case ReaderDrawerPane::AutoPageTurn:
        return drawer.autoPageTurnIntervalSeconds;
      case ReaderDrawerPane::Percent:
        return drawer.percent;
      default:
        return -1;
    }
  }
};

struct ReaderDrawerLayoutSmokeTest {
  template <typename Capture>
  static bool verifyPreview(const Capture& capture) {
#if CROSSINK_APP_READER_SAMPLE_PREVIEW
    return true;
#else
    const auto savedInsets = renderer.getViewableInsets();
    const auto savedOrientation = renderer.getOrientation();
    const auto savedMargin = SETTINGS.screenMarginHorizontal;
    const auto savedAlignment = SETTINGS.paragraphAlignment;
    renderer.setViewableInsets(ScreenInsets{{{21, 7, 32, 13}}});
    bool valid = true;
    for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted,
                                   GfxRenderer::LandscapeClockwise, GfxRenderer::LandscapeCounterClockwise}) {
      renderer.setOrientation(orientation);
      for (const uint8_t margin : {12, 48}) {
        SETTINGS.screenMarginHorizontal = margin;
        sdFontSystem.ensureLoaded(renderer);
        const int fontId = SETTINGS.getReaderFontId();
        int top, right, bottom, left;
        renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
        const int clockReservation = ReaderUtils::getTopStatusBarReservedHeight(renderer);
        const int y =
            top + std::max(static_cast<int>(SETTINGS.screenMarginVertical),
                           clockReservation > 0 ? clockReservation + ReaderUtils::TOP_STATUS_BAR_TEXT_PADDING : 0);
        const int width = renderer.getScreenWidth() - left - right - margin * 2;
        for (const auto alignment : {CssTextAlign::Left, CssTextAlign::Right}) {
          SETTINGS.paragraphAlignment = static_cast<uint8_t>(alignment);
          BlockStyle style;
          style.alignment = alignment;
          const int wordX = alignment == CssTextAlign::Left
                                ? 0
                                : width - renderer.getTextAdvanceX(fontId, "Preview", EpdFontFamily::REGULAR);
          auto block = std::make_shared<TextBlock>(
              std::vector<std::string>{"Preview"}, std::vector<int16_t>{static_cast<int16_t>(wordX)},
              std::vector<EpdFontFamily::Style>{EpdFontFamily::REGULAR}, std::vector<uint8_t>{},
              std::vector<uint16_t>{}, std::vector<uint16_t>{}, std::vector<uint8_t>{}, std::vector<bool>{false},
              style);
          Page page;
          page.elements.push_back(std::make_unique<PageLine>(block, 0, 0));
          auto model = std::make_unique<TouchReaderPreviewModel>();
          valid &= model->capture(page, renderer, fontId, SETTINGS.lineHeightPercent, left + margin, y);
          EpubReaderDrawerActivity editor(renderer, mappedInputManager, {}, model.get(), 0.0f, 0, 0, false, false,
                                          false, false, false, false, false, false, true, true, 0, 0, 5, false);
          editor.onEnter();
          const auto bounds = [&]() {
            const auto area = editor.previewBounds();
            std::array<int, 4> result{renderer.getScreenWidth(), renderer.getScreenHeight(), -1, -1};
            for (int py = 0; py < area.bottom(); ++py) {
              for (int px = 0; px < renderer.getScreenWidth(); ++px) {
                if (!renderer.isPixelBlack(px, py)) continue;
                result[0] = std::min(result[0], px);
                result[1] = std::min(result[1], py);
                result[2] = std::max(result[2], px);
                result[3] = std::max(result[3], py);
              }
            }
            return result;
          };
          const auto draw = [&]() {
            renderer.clearScreen();
            editor.previewDirty = true;
            std::optional<FontCacheManager::PrewarmScope> prewarm;
            int previewFontId;
            return editor.renderPreview(previewFontId, prewarm);
          };
          const std::string stem = "reader-preview-o-" + std::to_string(orientation) + "-margin-" +
                                   std::to_string(margin) + "-align-" + std::to_string(static_cast<int>(alignment));
          valid &= draw();
          const auto source = bounds();
          capture(stem + "-source");
          // A rebuilt font uses the reflow path even at the original point size.
          // Its page edges must still match the captured reader layout.
          editor.previewFontMetricsChanged = true;
          valid &= draw();
          const auto reflow = bounds();
          const bool sameBounds = source[2] >= source[0] && source == reflow;
          if (!sameBounds) {
            LOG_ERR("SMOKE", "%s moved from x=%d..%d to x=%d..%d after preview reflow", stem.c_str(), source[0],
                    source[2], reflow[0], reflow[2]);
          }
          valid &= sameBounds;
          capture(stem + "-reflow");
          editor.draft.readerFontPointSize += 2;
          valid &= draw();
          valid &= bounds()[2] >= bounds()[0];
          capture(stem + "-larger-font");
          editor.onExit();
        }
      }
    }
    SETTINGS.screenMarginHorizontal = savedMargin;
    SETTINGS.paragraphAlignment = savedAlignment;
    renderer.setViewableInsets(savedInsets);
    renderer.setOrientation(savedOrientation);
    return valid;
#endif
  }

  template <typename Capture>
  static bool verify(RenderLock& lock, const Capture& capture) {
    namespace fui = freeink::ui;
    const auto savedScale = SETTINGS.uiScale;
    const auto savedOrientation = renderer.getOrientation();
    bool valid = verifyPreview(capture);
    for (const auto orientation :
         {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise, GfxRenderer::LandscapeCounterClockwise}) {
      renderer.setOrientation(orientation);
      for (uint8_t scale = 0; scale < CrossPointSettings::UI_SCALE_COUNT; ++scale) {
        SETTINGS.uiScale = scale;
        EpubReaderDrawerActivity editor(renderer, mappedInputManager, {}, nullptr, 0.0f, 0, 0, false, false, false,
                                        false, false, false, false, false, true, true, 0, 0, 5, false);
        editor.onEnter();
        editor.openPane(ReaderDrawerPane::ReaderFont);
        valid &= editor.paneRows.size() == 4;  // This regression needs an installed TTF family.
        const auto stem = "reader-font-o-" + std::to_string(orientation) + "-scale-" + std::to_string(scale);
        const auto checkRows = [&]() {
          fui::Interaction tab;
          if (!editor.app.hitPublished(renderer.getScreenWidth() / 10, renderer.getScreenHeight() - 28,
                                       editor.ACTION_TAB, tab))
            return false;
          bool found = false;
          for (int y = 0; y < renderer.getScreenHeight(); ++y) {
            fui::Interaction row;
            if (!editor.app.hitPublished(renderer.getScreenWidth() / 2, y, editor.ACTION_ROW, row)) continue;
            found = true;
            if (row.rect.bottom() > tab.rect.y) {
              LOG_ERR("SMOKE", "Reader row %d ends at %d, past navigation at %d", row.value, row.rect.bottom(),
                      tab.rect.y);
              return false;
            }
          }
          return found;
        };
        renderer.clearScreen();
        editor.render(std::move(lock));
        valid &= checkRows();
        capture(stem + "-top");
#if CROSSINK_APP_CAP_TOUCH
        if (editor.visibleRows < static_cast<int>(editor.paneRows.size())) {
          const int x = renderer.getScreenWidth() / 2;
          const int y = renderer.getScreenHeight() - 100;
          mappedInputManager.simulatorInjectTouchDown(x, y);
          editor.loop();
          mappedInputManager.simulatorClearInputFrame();
          mappedInputManager.simulatorInjectTouchRelease(x, y - 100);
          editor.loop();
          mappedInputManager.simulatorClearInputFrame();
          editor.render(std::move(lock));
          valid &= checkRows() && editor.state.paneTopIndex > 0;
          capture(stem + "-swipe");
          editor.scrollBy(-100);
          editor.render(std::move(lock));
        }
#endif
        // Traverse the production button path to bring the final row into view.
        for (int i = 0; i < 3; ++i) {
          mappedInputManager.simulatorInjectRelease(mappedInputManager.menuButton(MappedInputManager::Button::Down));
          editor.loop();
          mappedInputManager.simulatorClearInputFrame();
          editor.render(std::move(lock));
          valid &= checkRows() && editor.simulatorFocusedRowVisible();
        }
        capture(stem + "-last");
        // Find the published TTF target and activate it through touch routing.
        fui::Interaction ttf;
        bool foundTtf = false;
        for (int y = 0; y < renderer.getScreenHeight(); ++y) {
          if (editor.app.hitPublished(renderer.getScreenWidth() / 2, y, editor.ACTION_ROW, ttf) && ttf.value == 3) {
            foundTtf = true;
            break;
          }
        }
        valid &= foundTtf;
#if CROSSINK_APP_CAP_TOUCH
        if (foundTtf) {
          const int x = ttf.rect.x + ttf.rect.width / 2;
          const int y = ttf.rect.y + ttf.rect.height / 2;
          mappedInputManager.simulatorInjectTouchDown(x, y);
          editor.loop();
          mappedInputManager.simulatorClearInputFrame();
          mappedInputManager.simulatorInjectTouchRelease(x, y);
          editor.loop();
          mappedInputManager.simulatorClearInputFrame();
          valid &= editor.state.pane == ReaderDrawerPane::TtfRendering;
          editor.render(std::move(lock));
          valid &= checkRows();
          capture(stem + "-ttf");
          editor.scrollBy(100);
          editor.render(std::move(lock));
          valid &= checkRows() && editor.state.paneTopIndex ==
                                      std::max(0, static_cast<int>(editor.paneRows.size()) - editor.visibleRows);
          capture(stem + "-ttf-last");
        }
#endif
        editor.onExit();
      }
    }
    SETTINGS.uiScale = savedScale;
    renderer.setOrientation(savedOrientation);
    return valid;
  }
};

// Detached activities exercise real popup callbacks and fixed/flowing readers
// while the smoke runner holds the render mutex.
struct StatusBarFeatureSmokeTest {
  using Capture = std::function<void(const std::string&)>;
  // Taps or confirms the selected row; false only when a touch row could not be found.
  static bool press(StatusBarSettingsActivity& editor) {
#if CROSSINK_APP_CAP_TOUCH
    if (mappedInputManager.hasTouchHardware()) {
      freeink::ui::Interaction hit;
      const int x = renderer.getScreenWidth() / 2;
      for (int y = 0; y < renderer.getScreenHeight(); y += 4) {
        if (!editor.app.hitPublished(x, y, StatusBarSettingsActivity::ACTION_ROW, hit) ||
            hit.value != editor.selectedIndex)
          continue;
        const int tapY = hit.rect.y + hit.rect.height / 2;
        mappedInputManager.simulatorInjectTouchDown(x, tapY);
        editor.loop();
        mappedInputManager.simulatorClearInputFrame();
        mappedInputManager.simulatorInjectTouchRelease(x, tapY);
        editor.loop();
        mappedInputManager.simulatorClearInputFrame();
        return true;
      }
      return false;
    }
#endif
    mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Confirm);
    editor.loop();
    mappedInputManager.simulatorClearInputFrame();
    mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Confirm);
    editor.loop();
    mappedInputManager.simulatorClearInputFrame();
    return true;
  }
  static bool open(StatusBarSettingsActivity& editor) { return press(editor) && editor.optionPopup.isActive(); }
  // Toggle rows flip in place without opening a popup.
  static bool flip(StatusBarSettingsActivity& editor) { return press(editor) && !editor.optionPopup.isActive(); }
  static bool choose(StatusBarSettingsActivity& editor, int index, RenderLock& lock) {
    editor.render(std::move(lock));
#if CROSSINK_APP_CAP_TOUCH
    if (mappedInputManager.hasTouchHardware()) {
      const auto hit = editor.optionPopup.simulatorOptionRect(index);
      mappedInputManager.simulatorInjectTouchDown(hit.x + hit.width / 2, hit.y + hit.height / 2);
      editor.loop();
      mappedInputManager.simulatorClearInputFrame();
      mappedInputManager.simulatorInjectTouchRelease(hit.x + hit.width / 2, hit.y + hit.height / 2);
      editor.loop();
      mappedInputManager.simulatorClearInputFrame();
      return !editor.optionPopup.isActive();
    }
#endif
    // Start from the current option, and move through the real menu mapping.
    for (int i = 0; i < index; ++i) {
      mappedInputManager.simulatorInjectRelease(mappedInputManager.menuButton(MappedInputManager::Button::Down));
      editor.loop();
      mappedInputManager.simulatorClearInputFrame();
    }
    mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Confirm);
    editor.loop();
    mappedInputManager.simulatorClearInputFrame();
    mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Confirm);
    editor.loop();
    mappedInputManager.simulatorClearInputFrame();
    return !editor.optionPopup.isActive();
  }
  static bool chapterPercentage(RenderLock& lock, const Capture& capture) {
    const auto framebufferHash = [] {
      uint64_t hash = 14695981039346656037ull;
      const auto* pixels = renderer.getFrameBuffer();
      for (size_t i = 0; i < renderer.getBufferSize(); ++i) hash = (hash ^ pixels[i]) * 1099511628211ull;
      return hash;
    };
    const int chapterIndex = halClock.isAvailable() ? 8 : 6;
    const int emptyIndex = chapterIndex + 3;
    for (const auto position : {ReaderStatusBarPosition::Top, ReaderStatusBarPosition::Bottom}) {
      auto config = ReaderStatusBarConfig{};
      config.slots[0] = ReaderStatusBarItem::BookProgressPercentage;
      SETTINGS.setReaderStatusBar(position, config);
      StatusBarSettingsActivity editor(renderer, mappedInputManager, true);
      editor.onEnter();
      editor.selectedIndex = position == ReaderStatusBarPosition::Top ? 0 : 1;
      editor.handleSelection();
      editor.render(std::move(lock));
      if (!open(editor)) return false;
      editor.render(std::move(lock));
      const auto stem =
          position == ReaderStatusBarPosition::Top ? "chapter-percentage-top" : "chapter-percentage-bottom";
      capture(std::string(stem) + "-picker");
      if (std::strcmp(editor.optionPopup.simulatorOptionLabel(chapterIndex), tr(STR_CHAPTER_PROGRESS_PERCENTAGE)))
        return false;
      if (!choose(editor, mappedInputManager.hasTouchHardware() ? chapterIndex : 1, lock)) return false;
      editor.render(std::move(lock));
      editor.render(std::move(lock));
      capture(stem);
      if (SETTINGS.readerStatusBar(position).slots[0] != ReaderStatusBarItem::ChapterProgressPercentage) return false;
      if (!open(editor) || !choose(editor, mappedInputManager.hasTouchHardware() ? emptyIndex : 3, lock)) return false;
      if (SETTINGS.readerStatusBar(position).slots[0] != ReaderStatusBarItem::Empty) return false;
      editor.render(std::move(lock));
      editor.render(std::move(lock));
      if (!open(editor) ||
          !choose(editor, mappedInputManager.hasTouchHardware() ? chapterIndex : chapterIndex + 1, lock))
        return false;
      editor.onExit();
      if (!SETTINGS.loadFromFile() ||
          SETTINGS.readerStatusBar(position).slots[0] != ReaderStatusBarItem::ChapterProgressPercentage)
        return false;
      // Compare actual glyph pixels against the existing book-percentage formatter.
      // Different book/chapter values catch accidentally rendering the book value.
      config = SETTINGS.readerStatusBar(position);
      ReaderStatusBarContent content;
      content.bookProgress = 75.12f;
      content.chapterProgress = 35.25f;
      for (uint8_t format = 0; format < 3; ++format) {
        config.percentageFormat = format;
        SETTINGS.setReaderStatusBar(position, config);
        renderer.clearScreen();
        GUI.drawReaderStatusBar(renderer, position, content);
        capture(std::string(stem) + "-format-" + std::to_string(format));
        const auto chapterHash = framebufferHash();
        auto reference = config;
        reference.slots[0] = ReaderStatusBarItem::BookProgressPercentage;
        content.bookProgress = content.chapterProgress;
        renderer.clearScreen();
        GUI.drawReaderStatusBar(renderer, position, content, &reference);
        if (chapterHash != framebufferHash()) return false;
        content.bookProgress = 75.12f;
      }
      content.chapterProgress = -1;
      content.chapterPage = 1;
      content.chapterPageCount = 4;
      renderer.clearScreen();
      GUI.drawReaderStatusBar(renderer, position, content);
      const auto fallbackHash = framebufferHash();
      content.chapterProgress = 25;
      renderer.clearScreen();
      GUI.drawReaderStatusBar(renderer, position, content);
      if (fallbackHash != framebufferHash()) return false;
    }
    return true;
  }
  // Rows after the seven slots and five option rows: Center Status Bar, then Separator.
  static bool centeredBar(RenderLock& lock, const Capture& capture) {
    constexpr int centerRow = 12;
    constexpr int separatorRow = 13;
    const auto original = SETTINGS.readerStatusBar(ReaderStatusBarPosition::Bottom);
    auto config = ReaderStatusBarConfig{};
    config.slots = {ReaderStatusBarItem::TimeLeftChapter,
                    ReaderStatusBarItem::Empty,
                    ReaderStatusBarItem::Empty,
                    ReaderStatusBarItem::TitleChapter,
                    ReaderStatusBarItem::ChapterPageCount,
                    ReaderStatusBarItem::BookProgressPercentage,
                    ReaderStatusBarItem::Empty};
    SETTINGS.setReaderStatusBar(ReaderStatusBarPosition::Bottom, config);
    StatusBarSettingsActivity editor(renderer, mappedInputManager, true);
    editor.onEnter();
    editor.selectedIndex = 1;
    editor.handleSelection();
    if (editor.visibleItemCount != centerRow + 1) return false;
    editor.selectedIndex = centerRow;
    editor.render(std::move(lock));
    editor.render(std::move(lock));
    capture("centered-settings-off");
    if (!flip(editor) || !SETTINGS.readerStatusBar(ReaderStatusBarPosition::Bottom).centered) return false;
    if (editor.visibleItemCount != separatorRow + 1) return false;
    editor.selectedIndex = separatorRow;
    editor.render(std::move(lock));
    editor.render(std::move(lock));
    capture("centered-settings-on");
    if (!open(editor)) return false;
    editor.render(std::move(lock));
    capture("centered-separator-picker");
    if (std::strcmp(editor.optionPopup.simulatorOptionLabel(1), "\xE2\x80\x93") != 0) return false;
    if (std::strcmp(editor.optionPopup.simulatorOptionLabel(3), "\xC2\xB7") != 0) return false;
    if (!choose(editor, 3, lock)) return false;
    editor.render(std::move(lock));
    editor.render(std::move(lock));
    capture("centered-settings-middle-dot");
    editor.onExit();
    if (!SETTINGS.loadFromFile()) return false;
    const auto saved = SETTINGS.readerStatusBar(ReaderStatusBarPosition::Bottom);
    SETTINGS.setReaderStatusBar(ReaderStatusBarPosition::Bottom, original);
    SETTINGS.saveToFile();
    return saved.centered && saved.separator == ReaderStatusBarSeparator::MiddleDot;
  }
  static bool settings(RenderLock& lock, const Capture& capture) {
    SETTINGS.legacyXtcTopUsesBottom = 1;
    const auto top = SETTINGS.topReaderStatusBar.slots;
    const auto bottom = SETTINGS.bottomReaderStatusBar.slots;
    const auto language = I18N.getLanguage();
    for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise,
                                   GfxRenderer::PortraitInverted, GfxRenderer::LandscapeCounterClockwise}) {
      renderer.setOrientation(orientation);
      for (int bar = 0; bar < 2; ++bar) {
        StatusBarSettingsActivity editor(renderer, mappedInputManager, true);
        editor.onEnter();
        editor.selectedIndex = bar;
        editor.handleSelection();
        editor.render(std::move(lock));
        // Reach the final Hide row through actual button navigation, including
        // the variable-height list's scrolling and section boundaries.
        for (int i = 0; i < 11; ++i) {
          mappedInputManager.simulatorInjectRelease(mappedInputManager.menuButton(MappedInputManager::Button::Down));
          editor.loop();
          mappedInputManager.simulatorClearInputFrame();
          editor.render(std::move(lock));
        }
        if (editor.selectedIndex != 11 || !editor.simulatorSelectedRowVisible) return false;
        for (int repeat = 0; repeat < 4; ++repeat) {
          const auto position = bar ? ReaderStatusBarPosition::Bottom : ReaderStatusBarPosition::Top;
          const bool before = SETTINGS.readerStatusBar(position).hidden;
          if (!flip(editor)) return false;
          if (SETTINGS.readerStatusBar(position).hidden == before || SETTINGS.topReaderStatusBar.slots != top ||
              SETTINGS.bottomReaderStatusBar.slots != bottom || SETTINGS.legacyXtcTopUsesBottom != 1)
            return false;
          editor.render(std::move(lock));
          if (!editor.simulatorSelectedRowVisible) return false;
          if (repeat == 0)
            capture("hide-row-lang-" + std::to_string(static_cast<int>(language)) + "-o-" +
                    std::to_string(orientation) + "-bar-" + std::to_string(bar));
        }
        editor.onExit();
      }
    }

    for (uint8_t size = 0; size < 3; ++size) {
      SETTINGS.displayStatusBarTextSize = 0;
      const auto readerSize = SETTINGS.statusBarTextSize;
      StatusBarSettingsActivity editor(renderer, mappedInputManager, false, false, true);
      editor.onEnter();
      editor.render(std::move(lock));
      for (int i = 0; i < 4; ++i) {
        mappedInputManager.simulatorInjectRelease(mappedInputManager.menuButton(MappedInputManager::Button::Down));
        editor.loop();
        mappedInputManager.simulatorClearInputFrame();
        editor.render(std::move(lock));
      }
      if (editor.selectedIndex != 4 || !editor.simulatorSelectedRowVisible) return false;
      if (!open(editor)) return false;
      if (!choose(editor, size, lock) || SETTINGS.displayStatusBarTextSize != size ||
          SETTINGS.statusBarTextSize != readerSize)
        return false;
      editor.render(std::move(lock));
      editor.render(std::move(lock));
      capture("global-size-editor-" + std::to_string(size));
      editor.onExit();
    }
    // Battery styles are per bar: changing one must leave the others untouched.
    const auto displayBattery = SETTINGS.displayStatusBar.batteryStyle;
    const auto readerTopBattery = SETTINGS.topReaderStatusBar.batteryStyle;
    const auto readerBottomBattery = SETTINGS.bottomReaderStatusBar.batteryStyle;
    for (int style = 2; style >= 0; --style) {
      SETTINGS.displayStatusBar.batteryStyle = ReaderStatusBarBatteryStyle::IconAndPercent;
      StatusBarSettingsActivity editor(renderer, mappedInputManager, false, false, true);
      editor.onEnter();
      editor.selectedIndex = 3;
      editor.render(std::move(lock));
      if (!open(editor) || !choose(editor, style, lock) ||
          SETTINGS.displayStatusBar.batteryStyle != static_cast<ReaderStatusBarBatteryStyle>(style) ||
          SETTINGS.topReaderStatusBar.batteryStyle != readerTopBattery ||
          SETTINGS.bottomReaderStatusBar.batteryStyle != readerBottomBattery)
        return false;
      editor.render(std::move(lock));
      editor.render(std::move(lock));
      capture("global-battery-editor-" + std::to_string(style));
      editor.onExit();
    }
    for (int style = 2; style >= 0; --style) {
      SETTINGS.bottomReaderStatusBar.batteryStyle = ReaderStatusBarBatteryStyle::IconAndPercent;
      StatusBarSettingsActivity editor(renderer, mappedInputManager, true);
      editor.onEnter();
      editor.selectedIndex = 1;
      editor.handleSelection();
      editor.selectedIndex = 7;
      editor.render(std::move(lock));
      if (!open(editor) || !choose(editor, style, lock) ||
          SETTINGS.bottomReaderStatusBar.batteryStyle != static_cast<ReaderStatusBarBatteryStyle>(style) ||
          SETTINGS.topReaderStatusBar.batteryStyle != readerTopBattery)
        return false;
      editor.render(std::move(lock));
      editor.render(std::move(lock));
      capture("bottom-battery-editor-" + std::to_string(style));
      editor.onExit();
    }
    SETTINGS.displayStatusBar.batteryStyle = displayBattery;
    SETTINGS.bottomReaderStatusBar.batteryStyle = readerBottomBattery;
    return true;
  }
  static bool txt(RenderLock& lock, const Capture& capture) {
    const char* path = "/books/status-feature.txt";
    auto file = Storage.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!file) return false;
    for (int i = 0; i < 1200; ++i) {
      const auto line =
          "Paragraph " + std::to_string(i) + " preserves the reading position through status bar edits.\n";
      if (file.write(reinterpret_cast<const uint8_t*>(line.data()), line.size()) != line.size()) return false;
    }
    file.close();
    auto book = std::make_unique<Txt>(path, "/.crosspoint");
    if (!book->load()) return false;
    book->setupCacheDir();
    TxtReaderActivity reader(renderer, mappedInputManager, std::move(book), 1);
    SETTINGS.topReaderStatusBar.hidden = false;
    SETTINGS.bottomReaderStatusBar.hidden = false;
    reader.render(std::move(lock));
    reader.currentPage = std::min(4, reader.totalPages - 1);
    const auto originalOffset = reader.pageOffsets[reader.currentPage];
    for (const unsigned mask : {0, 1, 3, 2, 0, 3, 0}) {
      const auto offset = originalOffset;
      SETTINGS.topReaderStatusBar.hidden = mask & 1;
      SETTINGS.bottomReaderStatusBar.hidden = mask & 2;
      reader.render(std::move(lock));
      const auto next = reader.currentPage + 1;
      if (reader.pageOffsets[reader.currentPage] > offset ||
          (next < reader.totalPages && reader.pageOffsets[next] <= offset))
        return false;
      if ((mask & 1) && reader.cachedTopStatusBarHeight != 0) return false;
      if ((mask & 2) && reader.cachedFooterReservedHeight != SETTINGS.screenMarginVertical) return false;
      capture("txt-hidden-" + std::to_string(mask));
    }
    if (!reader.saveProgress(reader.currentPage)) return false;
    auto reopened = std::make_unique<Txt>(path, "/.crosspoint");
    if (!reopened->load()) return false;
    TxtReaderActivity second(renderer, mappedInputManager, std::move(reopened), 1);
    second.render(std::move(lock));
    if (second.currentPage != reader.currentPage || second.pageOffsets != reader.pageOffsets) return false;

    // A page turn starts a new anchor; changing bars while the book is closed
    // restores the saved content offset rather than a now-obsolete page number.
    second.currentPage = std::min(second.currentPage + 2, second.totalPages - 1);
    second.render(std::move(lock));
    const auto turnedOffset = second.pageOffsets[second.currentPage];
    if (!second.saveProgress(second.currentPage)) return false;
    SETTINGS.topReaderStatusBar.hidden = true;
    SETTINGS.bottomReaderStatusBar.hidden = true;
    auto closedBook = std::make_unique<Txt>(path, "/.crosspoint");
    if (!closedBook->load()) return false;
    TxtReaderActivity third(renderer, mappedInputManager, std::move(closedBook), 1);
    third.render(std::move(lock));
    const auto contains = [](const TxtReaderActivity& activity, size_t offset) {
      return activity.pageOffsets[activity.currentPage] <= offset &&
             (activity.currentPage + 1 == activity.totalPages ||
              activity.pageOffsets[activity.currentPage + 1] > offset);
    };
    if (!contains(third, turnedOffset)) return false;
    SETTINGS.topReaderStatusBar.hidden = false;
    SETTINGS.bottomReaderStatusBar.hidden = false;
    second.render(std::move(lock));
    if (!contains(second, turnedOffset)) return false;

    // Preserve legacy page-only progress and tolerate an invalid modern offset.
    const auto progressPath = third.txt->getCachePath() + "/progress.bin";
    for (const size_t bytes : {2, 4, 6}) {
      const uint8_t record[6] = {2, 0, 255, 255, 255, 255};
      auto progress = Storage.open(progressPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
      if (!progress || progress.write(record, bytes) != bytes) return false;
      progress.close();
      third.loadProgress();
      if (third.currentPage != 2) return false;
    }
    const uint8_t invalidZero[6] = {2, 0, 0, 0, 0, 0};
    auto zeroProgress = Storage.open(progressPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (!zeroProgress || zeroProgress.write(invalidZero, sizeof(invalidZero)) != sizeof(invalidZero)) return false;
    zeroProgress.close();
    third.loadProgress();
    if (third.currentPage != 2) return false;
    // Exiting during a font rebuild must not publish a made-up byte offset.
    const int pendingPage = third.currentPage;
    third.pageOffsets.clear();
    if (!third.saveProgress(pendingPage)) return false;
    auto progress = Storage.open(progressPath.c_str(), O_RDONLY);
    uint8_t pendingRecord[6]{};
    if (!progress || progress.size() != sizeof(pendingRecord) ||
        progress.read(pendingRecord, sizeof(pendingRecord)) != sizeof(pendingRecord))
      return false;
    progress.close();
    if (pendingRecord[0] != pendingPage || pendingRecord[1] != 0 || pendingRecord[2] != 255 ||
        pendingRecord[3] != 255 || pendingRecord[4] != 255 || pendingRecord[5] != 255)
      return false;
    return true;
  }
  static bool xtc(RenderLock& lock, const Capture& capture) {
    // A real fixed-layout bitmap with black baked-in strips and a central line.
    const uint16_t width = renderer.getScreenWidth(), height = renderer.getScreenHeight();
    const uint32_t bytes = ((width + 7) / 8) * height;
    xtc::XtcHeader header{};
    header.magic = xtc::XTC_MAGIC;
    header.versionMajor = 1;
    header.pageCount = 1;
    header.pageTableOffset = sizeof(header);
    header.dataOffset = sizeof(header) + sizeof(xtc::PageTableEntry);
    const xtc::PageTableEntry entry{header.dataOffset, static_cast<uint32_t>(sizeof(xtc::XtgPageHeader)) + bytes, width,
                                    height};
    const xtc::XtgPageHeader page{xtc::XTG_MAGIC, width, height, 0, 0, bytes, 0};
    auto file = Storage.open("/books/status-feature.xtc", O_WRONLY | O_CREAT | O_TRUNC);
    if (!file) return false;
    file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
    file.write(reinterpret_cast<const uint8_t*>(&entry), sizeof(entry));
    file.write(reinterpret_cast<const uint8_t*>(&page), sizeof(page));
    std::vector<uint8_t> pixels(bytes, 0xff);
    const size_t stride = (width + 7) / 8;
    for (int y = 0; y < height; ++y) {
      if (y < 18 || y >= height - 18 || y == height / 2) std::fill_n(pixels.data() + y * stride, stride, 0);
    }
    if (file.write(pixels.data(), pixels.size()) != pixels.size()) return false;
    file.close();
    auto book = std::make_unique<Xtc>("/books/status-feature.xtc", "/.crosspoint");
    if (!book->load()) return false;
    XtcReaderActivity reader(renderer, mappedInputManager, std::move(book), 1);
    SETTINGS.xtcStatusBarMode = CrossPointSettings::XTC_STATUS_BAR_BOTH;
    SETTINGS.legacyXtcTopUsesBottom = 1;
    for (const unsigned mask : {0, 1, 3, 2, 0}) {
      SETTINGS.topReaderStatusBar.hidden = mask & 1;
      SETTINGS.bottomReaderStatusBar.hidden = mask & 2;
      reader.renderPage(0);
      if (reader.currentPage != 0 || !renderer.isPixelBlack(width / 2, height / 2)) return false;
      int marginTop, marginRight, marginBottom, marginLeft;
      renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
      // Sample the visible strips, beyond the existing physical panel gutter.
      if (mask == 3 && (renderer.isPixelBlack(width / 2, marginTop + 1) ||
                        renderer.isPixelBlack(width / 2, height - marginBottom - 1)))
        return false;
      capture("xtc-legacy-hidden-" + std::to_string(mask));
    }
    return true;
  }
};

struct ScreenCalibrationSmokeTest {
  // Check actual ink, not just layout metadata, across the default/custom boundary.
  static bool safeAreaContract(RenderLock& lock, const StatusBarFeatureSmokeTest::Capture& capture) {
    const auto original = renderer.getViewableInsets();
    const auto orientation = renderer.getOrientation();
    const auto theme = SETTINGS.uiTheme;
    const ScreenInsets cases[] = {ScreenInsets{}, ScreenInsets{{{9, 3, 3, 3}}}, ScreenInsets{{{32, 32, 32, 32}}},
                                  ScreenInsets{{{21, 7, 32, 13}}}};
    std::string definition;
    for (int i = 0; i < 60; ++i) {
      definition +=
          "Calibration keeps long dictionary definitions readable inside the visible screen, "
          "including the final line on each page.\n";
    }
    const char* dictionaryPath = "/.crosspoint/calibration-dictionary";
    if (!Storage.writeFile("/.crosspoint/calibration-dictionary.dict", definition.c_str()) ||
        !Storage.writeFile("/.crosspoint/calibration-dictionary.ifo",
                           "StarDict's dict ifo file\nbookname=Calibration\nsametypesequence=m\n"))
      return false;
    DictLocation dictionaryLocation;
    dictionaryLocation.folderPath = dictionaryPath;
    dictionaryLocation.size = static_cast<uint32_t>(definition.size());
    dictionaryLocation.found = true;
    std::vector<FootnoteEntry> entries(40);
    for (auto& entry : entries) std::strcpy(entry.number, "Calibration boundary row");
    bool valid = true;
    for (int variant = 0; variant < CrossPointSettings::UI_THEME_COUNT; ++variant) {
      SETTINGS.uiTheme = variant;
      UITheme::getInstance().reload();
      for (unsigned index = 0; index < 4; ++index) {
        renderer.setViewableInsets(cases[index]);
        for (unsigned rotation = 0; rotation < 4; ++rotation) {
          renderer.setOrientation(static_cast<GfxRenderer::Orientation>(rotation));
          const auto edges = cases[index].rotated(rotation).edges;
          const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
          valid &= safe.x >= edges[3] && safe.y >= edges[0] &&
                   safe.x + safe.width <= renderer.getScreenWidth() - edges[1] &&
                   safe.y + safe.height <= renderer.getScreenHeight() - edges[2];
          const auto checkInk = [&](const char* screen) {
            for (int y = 0; y < renderer.getScreenHeight(); ++y) {
              for (int x = 0; x < renderer.getScreenWidth(); ++x) {
                if (renderer.isPixelBlack(x, y) &&
                    (x < edges[3] || y < edges[0] || x >= renderer.getScreenWidth() - edges[1] ||
                     y >= renderer.getScreenHeight() - edges[2])) {
                  LOG_ERR("SMOKE", "Unsafe ink screen=%s theme=%d case=%u rotation=%u pixel=%d,%d", screen, variant,
                          index, rotation, x, y);
                  return false;
                }
              }
            }
            return true;
          };
          auto list = std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInputManager, entries);
          list->onEnter();
          list->render(std::move(lock));
          valid &= checkInk("Footnotes");
          const auto deviceSafe = list->app.device().safeRect();
          valid &= deviceSafe.x == edges[3] && deviceSafe.y == edges[0];
          list->onExit();
          renderer.clearScreen();
          const char* options[] = {"One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight"};
          GUI.drawOptionPopup(
              renderer, "Calibration options",
              OptionLabels(options, 8,
                           [](const void* context, size_t i) { return static_cast<const char* const*>(context)[i]; }),
              7, true);
          valid &= checkInk("Options");
          renderer.clearScreen();
          const auto header = TouchHeaderBackButton::headerRect(renderer, mappedInputManager, safe);
          const int menuTop = header.y + header.height;
          GUI.drawButtonMenu(
              renderer, Rect{safe.x, menuTop, safe.width, std::max(0, safe.y + safe.height - menuTop)}, 8, 7,
              [](int) { return "Menu action"; }, nullptr);
          valid &= checkInk("Home menu");
          bool labelsValid = true;
          GUI.drawButtonMenu(
              renderer, Rect{safe.x, menuTop, safe.width, GUI.getMenuRowHeight(renderer) + 4}, 8, -3,
              [&](int i) {
                labelsValid &= i >= 0 && i < 8;
                return "Menu action";
              },
              nullptr);
          valid &= labelsValid;
          // Exercise direct activity renderers that bypass the SDK's normal list layout.
          if ((variant == CrossPointSettings::CLASSIC || variant == CrossPointSettings::ROUNDEDRAFF) && index >= 2) {
            auto ota = std::make_unique<OtaUpdateActivity>(renderer, mappedInputManager);
            ota->state = OtaUpdateActivity::WAITING_CONFIRMATION;
            ota->render(std::move(lock));
            valid &= checkInk("OTA confirmation");
            ota->state = OtaUpdateActivity::FINISHED;
            ota->render(std::move(lock));
            valid &= checkInk("OTA finished");
            auto download = std::make_unique<FontDownloadActivity>(renderer, mappedInputManager);
            download->state_ = FontDownloadActivity::DOWNLOADING;
            download->activeDownloadFamilyName_ = "Long font family name for download feedback";
            download->render(std::move(lock));
            valid &= checkInk("Font download");
            auto qr = std::make_unique<QrDisplayActivity>(renderer, mappedInputManager, std::string(100, 'A'));
            qr->render(std::move(lock));
            valid &= checkInk("QR display");
            auto settings = std::make_unique<SettingsActivity>(renderer, mappedInputManager, false, true);
            settings->onEnter();
            settings->selectedCategoryIndex = 3;
            settings->setCurrentSettingsForCategory();
            settings->render(std::move(lock));
            valid &= checkInk("Settings System footer");
            settings->onExit();
            for (const bool modal : {false, true}) {
              auto dictionary = std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInputManager,
                                                                               "Calibration", dictionaryLocation);
              if (modal) {
                dictionary->backgroundContext_ = &valid;
                dictionary->backgroundRender_ = [](void*) { renderer.clearScreen(); };
              }
              dictionary->wrapText();
              valid &=
                  !dictionary->definitionReadFailed_ && !dictionary->layoutLines.empty() && dictionary->totalPages > 1;
              dictionary->render(std::move(lock));
              valid &= checkInk(modal ? "Dictionary modal" : "Dictionary fullscreen");
              valid &= dictionary->leftPadding >= safe.x &&
                       renderer.getScreenWidth() - dictionary->rightPadding <= safe.x + safe.width;
              dictionary->currentPage = dictionary->totalPages - 1;
              dictionary->loadPage(dictionary->currentPage);
              dictionary->render(std::move(lock));
              valid &= !dictionary->layoutLines.empty() &&
                       checkInk(modal ? "Dictionary modal last page" : "Dictionary fullscreen last page");
            }
            renderer.clearScreen();
            auto panel = std::make_unique<FrontlightPanelActivity>(renderer, mappedInputManager);
            panel->onEnter();
            panel->render(std::move(lock));
            valid &= checkInk("Frontlight panel");
            const auto handle = panel->simulatorHandleRect();
            valid &= handle.x >= safe.x && handle.right() <= safe.x + safe.width && handle.y >= safe.y &&
                     handle.bottom() <= safe.y + safe.height &&
                     panel->simulatorContentBottom <= panel->simulatorActionBarTop;
            panel->onExit();
          }
          if (rotation == 0) {
            capture("calibration-bounds-theme-" + std::to_string(variant) + "-case-" + std::to_string(index));
          }
        }
      }
    }
    renderer.setViewableInsets(original);
    renderer.setOrientation(orientation);
    SETTINGS.uiTheme = theme;
    UITheme::getInstance().reload();
    LOG_INF("SMOKE", "Calibration default/legacy/maximum/asymmetric UI matrix: %s", valid ? "passed" : "failed");
    return valid;
  }
  static bool geometry(RenderLock& lock, const StatusBarFeatureSmokeTest::Capture& capture) {
    const auto original = renderer.getViewableInsets();
    const auto orientation = renderer.getOrientation();
    const auto theme = SETTINGS.uiTheme;
    const auto topStatus = SETTINGS.topReaderStatusBar;
    SETTINGS.topReaderStatusBar = ReaderStatusBarConfig{};
    SETTINGS.topReaderStatusBar.progressBar = CrossPointSettings::BOOK_PROGRESS;
    renderer.setViewableInsets(ScreenInsets{{{32, 32, 32, 32}}});
    // Heap-owned fixture/activity are simulator-only; never add their large
    // activity state to a firmware task stack.
    std::vector<FootnoteEntry> entries(2);
    std::strcpy(entries[0].number, "1. Calibration first footnote");
    std::strcpy(entries[1].number, "2. Calibration second footnote");
    bool valid = true;
    for (const auto variant :
         {CrossPointSettings::CLASSIC, CrossPointSettings::LYRA, CrossPointSettings::ROUNDEDRAFF}) {
      SETTINGS.uiTheme = variant;
      UITheme::getInstance().reload();
      for (unsigned rotation = 0; rotation < 4; ++rotation) {
        renderer.setOrientation(static_cast<GfxRenderer::Orientation>(rotation));
        auto list = std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInputManager, entries);
        list->onEnter();
        list->render(std::move(lock));
        const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
        const auto header = TouchHeaderBackButton::headerRect(renderer, mappedInputManager, safe);
        const auto back = TouchHeaderBackButton::layout(header);
        const int statusY = UITheme::getTopStatusBarY(renderer);
        const auto& metrics = UITheme::getInstance().getMetrics();
        valid &= header.x >= 32 && header.x + header.width <= renderer.getScreenWidth() - 32 && header.y >= 32;
        const int displayY = std::max(32, header.y + UITheme::getTopStatusBarInset(renderer));
        const int iconTop = back.iconRect.y + (back.iconRect.height - TouchHeaderBackButton::ICON_SIZE) / 2 +
                            TouchHeaderBackButton::TITLE_VERTICAL_OFFSET;
        valid &= statusY >= 32 &&
                 displayY + UITheme::getDisplayStatusBarTextHeight(renderer) + ReaderStatusBarConfig::TOP_TEXT_INSET <=
                     iconTop;
        valid &= back.touchRect.x >= 32 && back.touchRect.y >= 32;
        valid &= ReaderUtils::getTopStatusBarReservedHeight(renderer) ==
                 std::max(0, statusY + UITheme::getReaderStatusBarHeight(ReaderStatusBarPosition::Top, renderer) - 32);
        const int rowHeight = uiListRowHeight(list->app.theme(), UiListRowType::SingleLine);
        const int expectedHitY = TouchHeaderBackButton::contentTop(renderer, mappedInputManager, safe.y) +
                                 metrics.verticalSpacing - std::max(0, list->app.device().minTouchSize - rowHeight) / 2;
        freeink::ui::Interaction hit;
        bool found = false;
        for (int y = header.y + header.height; y < renderer.getScreenHeight() - 32; ++y) {
          if (!list->app.hitPublished(renderer.getScreenWidth() / 2, y, 1, hit) || hit.value != 0) continue;
          found = true;
          valid &= hit.rect.x == safe.x + list->app.theme().listInset;
          valid &= hit.rect.y == expectedHitY;
          break;
        }
        valid &= found;
        LOG_INF("SMOKE",
                "Geometry theme=%d rotation=%u header=%d,%d,%d,%d status=%d+%d iconY=%d back=%d,%d first=%d,%d "
                "expected=%d,%d found=%d",
                static_cast<int>(variant), rotation, header.x, header.y, header.width, header.height, statusY,
                metrics.batteryBarHeight,
                back.iconRect.y + (back.iconRect.height - TouchHeaderBackButton::ICON_SIZE) / 2, back.touchRect.x,
                back.touchRect.y, hit.rect.x, hit.rect.y, safe.x + list->app.theme().listInset, expectedHitY, found);
#if CROSSINK_APP_CAP_TOUCH
        if (mappedInputManager.hasTouchHardware()) {
          const int x = back.touchRect.x + back.touchRect.width / 2;
          const int y = back.touchRect.y + back.touchRect.height / 2;
          mappedInputManager.simulatorInjectTouchDown(x, y);
          mappedInputManager.simulatorClearInputFrame();
          mappedInputManager.simulatorInjectTouchRelease(x, y);
          const bool tapped = TouchHeaderBackButton::wasTapped(mappedInputManager, header);
          LOG_INF("SMOKE", "Geometry tap=%d statusBottom=%d iconTop=%d reservation=%d", tapped,
                  displayY + UITheme::getDisplayStatusBarTextHeight(renderer) + ReaderStatusBarConfig::TOP_TEXT_INSET,
                  iconTop, ReaderUtils::getTopStatusBarReservedHeight(renderer));
          valid &= tapped;
          mappedInputManager.simulatorClearInputFrame();
        }
#endif
        capture("calibration-max32-theme-" + std::to_string(static_cast<int>(variant)) + "-rotation-" +
                std::to_string(rotation));
        renderer.clearScreen();
        ReaderStatusBarContent progress;
        progress.bookProgress = 50;
        progress.showProgress = true;
        GUI.drawReaderStatusBar(renderer, ReaderStatusBarPosition::Top, progress);
        valid &= renderer.isPixelBlack(renderer.getScreenWidth() / 4, statusY);
        valid &= !renderer.isPixelBlack(renderer.getScreenWidth() / 4, 31);
        capture("calibration-reader-progress-theme-" + std::to_string(static_cast<int>(variant)) + "-rotation-" +
                std::to_string(rotation));
        if (rotation == 0) {
          renderer.setViewableInsets(ScreenInsets{});
          list->render(std::move(lock));
          const auto restored = list->app.device().safeRect();
          valid &= restored.x == ScreenInsets{}.edges[3] && restored.y == ScreenInsets{}.edges[0];
          capture("calibration-suspended-menu-reset-theme-" + std::to_string(static_cast<int>(variant)));
          renderer.setViewableInsets(ScreenInsets{{{32, 32, 32, 32}}});
        }
        list->onExit();
      }
    }
    renderer.setViewableInsets(original);
    renderer.setOrientation(orientation);
    SETTINGS.uiTheme = theme;
    SETTINGS.topReaderStatusBar = topStatus;
    UITheme::getInstance().reload();
    LOG_INF("SMOKE", "Calibration max32 headers/back targets/list geometry: %s", valid ? "passed" : "failed");
    return valid;
  }
  // Every screen must draw the clock/battery bar on the reader's calibrated row, whichever header rect it passes.
  static bool statusBarOrigin(RenderLock& lock) {
    const auto original = renderer.getViewableInsets();
    const auto orientation = renderer.getOrientation();
    const auto theme = SETTINGS.uiTheme;
    bool valid = true;
    for (const auto& insets :
         {ScreenInsets{}, ScreenInsets{{{0, 0, 0, 0}}}, ScreenInsets{{{15, 6, 3, 3}}}, ScreenInsets{{{4, 3, 3, 3}}}}) {
      renderer.setViewableInsets(insets);
      for (const auto variant :
           {CrossPointSettings::CLASSIC, CrossPointSettings::LYRA, CrossPointSettings::ROUNDEDRAFF}) {
        SETTINGS.uiTheme = variant;
        UITheme::getInstance().reload();
        for (const auto rotation :
             {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise, GfxRenderer::PortraitInverted}) {
          renderer.setOrientation(rotation);
          const int expected = UITheme::getTopStatusBarY(renderer);
          const auto check = [&](const char* screen, Activity& activity) {
            renderer.setOrientation(rotation);  // activity exits restore the saved orientation
            BaseTheme::takeLastDisplayStatusBarOriginY();
            activity.onEnter();
            activity.render(std::move(lock));
            const int actual = BaseTheme::takeLastDisplayStatusBarOriginY();
            activity.onExit();
            LOG_INF("SMOKE", "Status bar origin screen=%s theme=%d rotation=%d top=%d actual=%d expected=%d", screen,
                    static_cast<int>(variant), static_cast<int>(rotation), insets.edges[0], actual, expected);
            if (actual != expected) {
              LOG_ERR("SMOKE", "Status bar origin mismatch on %s: %d, reader row is %d", screen, actual, expected);
              valid = false;
            }
          };
          HomeActivity home(renderer, mappedInputManager);
          check("Home", home);
          SettingsActivity settings(renderer, mappedInputManager);
          check("Settings", settings);
          FileBrowserActivity files(renderer, mappedInputManager, "/books");
          check("FileBrowser", files);
          AlertActivity alert(renderer, mappedInputManager);
          check("Alert", alert);
          OptionSelectionActivity options(renderer, mappedInputManager, "Options", StrId::STR_SCREEN_CALIBRATION,
                                          {"A", "B"}, 0);
          check("OptionSelection", options);
          if (rotation == GfxRenderer::PortraitInverted) {
            // Headers pushed below the top hint gutter keep the bar on their own row.
            renderer.setOrientation(rotation);
            const Rect header = TouchHeaderBackButton::headerRect(
                renderer, mappedInputManager, UITheme::getInstance().getScreenSafeArea(renderer, true, false));
            BaseTheme::takeLastDisplayStatusBarOriginY();
            GUI.drawHeader(renderer, header, "");
            const int actual = BaseTheme::takeLastDisplayStatusBarOriginY();
            const int gutterExpected = header.y + expected - UITheme::getHeaderRect(renderer, 0).y;
            if (actual != gutterExpected || (UITheme::getTopStatusBarInset(renderer) >= 0 && actual < header.y)) {
              LOG_ERR("SMOKE", "Status bar origin below hint gutter: %d, expected %d (header %d)", actual,
                      gutterExpected, header.y);
              valid = false;
            }
          }
        }
      }
    }
    renderer.setViewableInsets(original);
    renderer.setOrientation(orientation);
    SETTINGS.uiTheme = theme;
    UITheme::getInstance().reload();
    LOG_INF("SMOKE", "Calibration status bar origin consistency: %s", valid ? "passed" : "failed");
    return valid;
  }
  static bool keyboardAndHints(RenderLock& lock, const StatusBarFeatureSmokeTest::Capture& capture) {
    const auto original = renderer.getViewableInsets();
    const auto orientation = renderer.getOrientation();
    const auto theme = SETTINGS.uiTheme;
    bool valid = true;
    for (const auto variant : {CrossPointSettings::CLASSIC, CrossPointSettings::LYRA, CrossPointSettings::ROUNDEDRAFF,
                               CrossPointSettings::MINIMAL}) {
      SETTINGS.uiTheme = variant;
      UITheme::getInstance().reload();
      for (const bool custom : {false, true}) {
        renderer.setViewableInsets(custom ? ScreenInsets{{{32, 32, 32, 32}}} : ScreenInsets{});
        const int reserve = UITheme::getButtonHintsReserve(renderer);
        valid &= reserve == (mappedInputManager.hasTouchHardware() ? 0 : custom ? 72 : 43);
        for (unsigned rotation = 0; rotation < 4; ++rotation) {
          renderer.setOrientation(static_cast<GfxRenderer::Orientation>(rotation));
          renderer.clearScreen();
          GUI.drawButtonHints(renderer, "Back", "OK", "Up", "Down", true);
          GUI.drawSideButtonHints(renderer, ">", "<");
          if (custom) {
            const int width = renderer.getScreenWidth(), height = renderer.getScreenHeight();
            // Outline endpoints use renderer's inclusive rectangle convention.
            // Every glyph and outline must be within these physical boundaries.
            for (int y = 0; y < height; ++y)
              for (int x = 0; x < width; ++x)
                if (renderer.isPixelBlack(x, y) && (x < 32 || x > width - 32 || y < 32 || y > height - 32))
                  valid = false;
            const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
            if (!mappedInputManager.hasTouchHardware()) {
              if (rotation == 0) valid &= safe.y + safe.height == height - reserve;
              if (rotation == 1) valid &= safe.x == reserve;
              if (rotation == 2) valid &= safe.y == reserve;
              if (rotation == 3) valid &= safe.x + safe.width == width - reserve;
            }
          }
          capture("calibration-hints-theme-" + std::to_string(static_cast<int>(variant)) +
                  (custom ? "-max32-rotation-" : "-default-rotation-") + std::to_string(rotation));
        }
      }
    }
    SETTINGS.uiTheme = theme;
    UITheme::getInstance().reload();
    renderer.setOrientation(GfxRenderer::Portrait);
    renderer.setViewableInsets(ScreenInsets{{{32, 32, 32, 32}}});
    {
      auto download = makeUniqueNoThrow<FontDownloadActivity>(renderer, mappedInputManager);
      if (!download) return false;
      download->manifestFamilies_ = makeUniqueNoThrow<FontDownloadActivity::ManifestFamily[]>(1);
      if (!download->manifestFamilies_) return false;
      download->manifestFamilyCount_ = 1;
      download->manifestFamilies_[0].name = "Installed font";
      download->manifestFamilies_[0].installed = true;
      download->state_ = FontDownloadActivity::FAMILY_LIST;
      applySharedUiTheme(download->app_, download->uiTarget_);
      download->app_.setScreen(&FontDownloadActivity::listScreen, download.get());
      valid &= download->rebuildListItems();
      if (download->listItemCount_ != 1) return false;
      valid &= !(download->listItems_[0].state & freeink::ui::StateDisabled);
      download->render(std::move(lock));
      capture("upstream-installed-font-selection");
    }
    auto keyboard =
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInputManager, "Password", "", 64, InputType::Password);
    keyboard->onEnter();
    keyboard->text = "café";
    keyboard->cursorPos = keyboard->text.size();
    keyboard->passwordVisible = false;
    keyboard->cursorMode = false;
    valid &= keyboard->displayTextForCurrentState() == "***é";
    keyboard->render(std::move(lock));
    capture("upstream-password-utf8");
    keyboard->text.clear();
    keyboard->cursorPos = 0;
    keyboard->render(std::move(lock));
    const auto keys = keyboard->keyboardRect();
    const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    valid &= keys.x >= 32 && keys.right() <= renderer.getScreenWidth() - 32 && keys.bottom() <= safe.y + safe.height;
    valid &= keyboard->textFieldMargin() >= 32;
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int fieldBottom = TouchHeaderBackButton::contentTop(renderer, mappedInputManager) +
                            metrics.verticalSpacing * 5 + metrics.keyboardVerticalOffset + keyboard->inputLineHeight;
    for (int y = fieldBottom; y < keys.y; ++y)
      for (int x = 0; x < renderer.getScreenWidth(); ++x)
        if ((x < 32 || x > renderer.getScreenWidth() - 32) && renderer.isPixelBlack(x, y)) valid = false;
    bool firstLetter = false, lastLetter = false, mode = false, ok = false;
    for (unsigned i = 0; i < keyboard->interactions.publishedCount(); ++i) {
      const auto& hit = keyboard->interactions.publishedData()[i];
      valid &= hit.rect.x >= 32 && hit.rect.right() <= renderer.getScreenWidth() - 32;
      valid &= hit.rect.bottom() <= safe.y + safe.height;
      const int16_t value = hit.value;
      firstLetter |= value == 'q';
      lastLetter |= value == 'p';
      mode |= value == freeink::ui::QWERTY_KEY_MODE;
      ok |= value == freeink::ui::QWERTY_KEY_ENTER;
#if CROSSINK_APP_CAP_TOUCH
      if (mappedInputManager.hasTouchHardware() && (value == 'q' || value == 'p')) {
        mappedInputManager.simulatorInjectTouchDown(hit.rect.x + hit.rect.width / 2, hit.rect.y + hit.rect.height / 2);
        keyboard->loop();
        mappedInputManager.simulatorClearInputFrame();
        mappedInputManager.simulatorInjectTouchRelease(hit.rect.x + hit.rect.width / 2,
                                                       hit.rect.y + hit.rect.height / 2);
        keyboard->loop();
        mappedInputManager.simulatorClearInputFrame();
      }
#endif
    }
    valid &= firstLetter && lastLetter && mode && ok;
    if (mappedInputManager.hasTouchHardware())
      valid &= keyboard->text.find('q') != std::string::npos && keyboard->text.find('p') != std::string::npos;
    capture("calibration-password-keyboard-max32");
    keyboard->onExit();
    auto fonts = std::make_unique<FontSelectionActivity>(renderer, mappedInputManager, nullptr);
    fonts->updateLayoutMetrics();
    const int customTop = fonts->afterHeader;
    const int customHeight = fonts->usableHeight;
    renderer.setViewableInsets(ScreenInsets{});
    fonts->updateLayoutMetrics();
    valid &= fonts->afterHeader < customTop && fonts->usableHeight > customHeight;
    renderer.setViewableInsets(original);
    renderer.setOrientation(orientation);
    LOG_INF("SMOKE", "Calibration front/side hints, keyboard edge keys, font picker resume: %s",
            valid ? "passed" : "failed");
    return valid;
  }
  static bool timePickerHit(const FrontlightTimePickerActivity& picker, const int16_t value,
                            freeink::ui::Interaction& hit) {
    bool found = false;
    for (unsigned i = 0; i < picker.keyboardInteractions.publishedCount(); ++i) {
      const auto& key = picker.keyboardInteractions.publishedData()[i];
      if (key.rect.x < 32 || key.rect.y < 32 || key.rect.right() > renderer.getScreenWidth() - 32 ||
          key.rect.bottom() > renderer.getScreenHeight() - 32)
        return false;
      if (key.value == value) {
        hit = key;
        found = true;
      }
    }
    // Include the fixed time fields and backspace artwork, which do not all
    // publish active touch targets until a numeric entry is pending.
    for (int y = 0; y < renderer.getScreenHeight(); ++y)
      for (int x = 0; x < renderer.getScreenWidth(); ++x)
        if (renderer.isPixelBlack(x, y) &&
            (x < 32 || y < 32 || x > renderer.getScreenWidth() - 32 || y > renderer.getScreenHeight() - 32))
          return false;
    return found;
  }
  static bool timePickerHourIsTwelve(const FrontlightTimePickerActivity& picker) { return picker.hour12 == 12; }
  static bool txt(RenderLock& lock) {
    const char* path = "/books/calibration.txt";
    auto file = Storage.open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!file) return false;
    constexpr char line[] = "Screen calibration keeps this paragraph visible and preserves its reading position.\n";
    for (int i = 0; i < 600; ++i)
      if (file.write(reinterpret_cast<const uint8_t*>(line), sizeof(line) - 1) != sizeof(line) - 1) return false;
    file.close();
    auto book = std::make_unique<Txt>(path, "/.crosspoint");
    if (!book->load()) return false;
    book->setupCacheDir();
    TxtReaderActivity reader(renderer, mappedInputManager, std::move(book), 1);
    reader.render(std::move(lock));
    if (reader.totalPages < 3) return false;
    reader.currentPage = 2;
    const auto offset = reader.pageOffsets[reader.currentPage];
    const auto original = renderer.getViewableInsets();
    auto changed = original;
    changed.adjust(1, 1);
    changed.adjust(3, 1);
    const int width = reader.viewportWidth;
    renderer.setViewableInsets(changed);
    reader.render(std::move(lock));
    if (reader.cachedViewableInsets != changed || reader.viewportWidth != width - 2) return false;
    const int left = reader.cachedOrientedMarginLeft;
    changed.adjust(3, 1);
    changed.adjust(1, -1);
    renderer.setViewableInsets(changed);
    reader.render(std::move(lock));
    if (reader.viewportWidth != width - 2 || reader.cachedOrientedMarginLeft != left + 1 ||
        reader.pageOffsets[reader.currentPage] > offset ||
        (reader.currentPage + 1 < reader.totalPages && reader.pageOffsets[reader.currentPage + 1] <= offset))
      return false;
    renderer.setViewableInsets(original);
    return true;
  }
  static std::optional<uint32_t> epubOffset;
  static int epubWidth;
  static bool epubReady(const EpubReaderActivity& reader) {
    return reader.section && !reader.section->isBuilding() && !reader.pendingRelayoutReposition;
  }
  static bool reflowEpub(EpubReaderActivity& reader) {
    {
      RenderLock lock;
      reader.section->currentPage = std::min(1, reader.section->pageCount - 1);
      epubOffset = reader.section->getVisibleTextOffsetForPage(reader.section->currentPage);
      epubWidth = reader.buildViewportWidth;
      auto changed = renderer.getViewableInsets();
      changed.adjust(1, 1);
      changed.adjust(3, 1);
      renderer.setViewableInsets(changed);
    }
    reader.endGlobalSettingsEdit();
    RenderLock lock;
    return !reader.section && reader.cachedVisibleTextOffset == epubOffset;
  }
  static bool reflowedEpub(const EpubReaderActivity& reader) {
    const int page = reader.section->currentPage;
    const auto start = reader.section->getVisibleTextOffsetForPage(page);
    const auto next =
        page + 1 < reader.section->pageCount ? reader.section->getVisibleTextOffsetForPage(page + 1) : std::nullopt;
    return reader.buildViewportWidth == epubWidth - 2 && epubOffset && start && *start <= *epubOffset &&
           (!next || *next > *epubOffset);
  }
};
std::optional<uint32_t> ScreenCalibrationSmokeTest::epubOffset;
int ScreenCalibrationSmokeTest::epubWidth = 0;

struct ReaderMenuOrientationSmokeTest {
  template <typename Reader>
  static OptionPopup& popup(Reader& reader) {
    return reader.quickActionsPopup;
  }
};

namespace {

enum class SmokeStep : uint8_t {
  Start,
  HomeReaderReader,
  HomeReaderNested,
  HomeReaderConfirmation,
  HomeReaderTrigger,
  HomeReaderUnwound,
  HomeReaderReturnedHome,
  HomeReaderNonReaderParent,
  HomeReaderNonReaderNested,
  HomeReaderNonReaderHome,
  BackHomeReader,
  BackHomeNested,
  BackHomeReturnedReader,
  BackHomeReturnedHome,
  NavigationLongReader,
  NavigationLongHeld,
  NavigationLongReleased,
  NavigationPicker,
  Home,
  FileBrowser,
  FileBrowserSettings,
  Library,
  StatsUploadEntry,
  SyncServerCaptureBottom,
  SyncServerCaptureUnsupported,
  SyncServerCaptureStats,
  SyncServerCaptureFileTransfer,
  StatsUploadReturn,
  StatsUploadEmptyDone,
  KeptPositionDone,
  MatchingChangeDone,
  RecentLibrary,
  Settings,
  SideButtons,
  ReaderMenu,
  Sleep,
  Reader,
  ReaderInput,
  CompletionReturnedHome,
  CompletionReaderRestored,
  StatusBarReaderSettings,
  StatusBarReaderReturned,
  CarouselHome,
  FrontlightLayout,
  FrontlightLayoutRendered,
  ThemeHome,
  ThemeSettings,
  ThemeReturned,
  ThemeFresh,
  StatusBarEditor,
  StatusBarPicker,
  Done,
};

class HomeReaderSmokeActivity final : public Activity {
 public:
  HomeReaderSmokeActivity(const char* activityName, const bool reader, GfxRenderer& renderer,
                          MappedInputManager& mappedInput, const bool bookReader = false)
      : Activity(activityName, renderer, mappedInput), reader(reader), bookReader(bookReader) {}

  bool isReaderActivity() const override { return reader; }
  bool isBookReaderActivity() const override { return bookReader; }

 private:
  bool reader;
  bool bookReader;
};

// Stands in for a reader hosting Reading Stats: its result handler must run
// before the sync replaces the stack, even when it pushes another screen.
bool statsParentHandlerRan = false;
bool secondSyncStarted = false;

class StatsParentSmokeActivity final : public Activity {
 public:
  StatsParentSmokeActivity(GfxRenderer& renderer, MappedInputManager& input)
      : Activity("StatsParent", renderer, input) {}

  void onEnter() override {
    Activity::onEnter();
    startActivityForResult(
        std::make_unique<BookStatsActivity>(renderer, mappedInput, "Fixture", std::string{}, BookReadingStats{}, -1.0f,
                                            false, 0, GlobalReadingStats{}),
        [this](const ActivityResult&) {
          // Same as EpubReaderActivity importing edits, then reopening its menu.
          statsParentHandlerRan = activityManager.hasDeferredReplace();
          startActivityForResult(
              std::make_unique<ConfirmationActivity>(renderer, mappedInput, "Pushed by parent", "Pushed by parent"),
              [](const ActivityResult&) {});
        });
  }
  void loop() override {}
  void render(RenderLock&&) override {
    renderer.clearScreen();
    renderer.displayBuffer();
  }
};

class EntryRenderSmokeActivity final : public Activity {
  std::atomic<bool> ready{false};
  std::atomic<unsigned> renders{0};

  static void check(bool passed, const char* message) {
    if (passed) return;
    LOG_ERR("SMOKE", "%s", message);
    std::_Exit(1);
  }

 public:
  EntryRenderSmokeActivity(GfxRenderer& renderer, MappedInputManager& input)
      : Activity("EntryRenderSmoke", renderer, input) {}

  void onEnter() override {
    Activity::onEnter();
    requestUpdate(true);
    delay(80);
    check(renders == 0, "Activity rendered before entry state was ready");
    ready = true;
    check(requestUpdateAndWait() == RequestUpdateResult::Rendered, "Entry loading render was rejected");
    check(renders > 0, "Entry loading screen was not rendered");
    ready = false;
    const unsigned previous = renders;
    requestUpdate(true);
    delay(80);
    check(renders == previous, "Asynchronous render ran while entry resumed initialization");
    ready = true;
    requestUpdate();
    LOG_INF("SMOKE", "Entry render isolation and synchronous short-note popup passed");
  }

  void render(RenderLock&&) override {
    check(ready, "Render observed incomplete entry state");
    renderer.clearScreen();
    static const char* const options[] = {"Cancel", "Save"};
    const OptionLabels labels(options, 2,
                              [](const void* owner, size_t i) { return static_cast<const char* const*>(owner)[i]; });
    GUI.drawOptionPopup(renderer, "Popup regression", labels, 0, false, nullptr, nullptr, false, -1,
                        "Note:", "Short note.");
    ++renders;
    renderer.displayBuffer();
  }
};

class NavigationPickerSmokeActivity final : public Activity {
  OptionPopup popup;

 public:
  NavigationPickerSmokeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("NavigationPicker", renderer, mappedInput) {}
  void onEnter() override {
    Activity::onEnter();
    const auto setting = buildShortcutSetting(StrId::STR_SHORT_PWR_BTN, &CrossPointSettings::shortPwrBtn, "shortPwrBtn",
                                              ShortcutOptionCatalog::PowerButton);
    std::vector<std::string> labels;
    labels.reserve(setting.enumValues.size());
    for (const auto label : setting.enumValues) labels.emplace_back(I18N.get(label));
    popup.show(StrId::STR_SHORT_PWR_BTN, labels, labels.size() - 2, [](int) {});
    requestUpdate();
  }
  void render(RenderLock&&) override {
    renderer.clearScreen();
    popup.processRender(renderer, mappedInput);
  }
};

// While armed, network reboot requests are recorded here instead of relaunching the simulator,
// so a scenario can check the boot target and keep driving the same process.
struct CapturedNetworkRestart {
  bool armed = false;
  int count = 0;
  uint32_t target = 0;
  uint32_t payload = 0;
} capturedNetworkRestart;

class SimulatorSmokeTest {
 public:
  void tick() {
    if (!enabled()) return;

    try {
      tickImpl();
    } catch (const std::exception& e) {
      fail("Unhandled exception: %s", e.what());
    } catch (...) {
      fail("Unhandled non-standard exception");
    }
  }

 private:
  enum class ScriptActionType : uint8_t {
    Press,
    Release,
    HomeTap,
    HomeLongPress,
    ConfigureChapterShortcuts,
    RestoreChapterShortcuts,
    ConfigureChapterHomeDoubleTap,
    WaitForChapterSelection,
    WaitForMenuLongPress,
    ConfigureHomeButtonPowerLock,
    WaitForPowerLongPress,
    AssertHomeButtonDisabled,
    AssertHomeButtonEnabled,
    AssertTouchscreenDisabled,
    AssertTouchscreenEnabled,
    AssertTtfProfileNative,
    OpenSmokeBook,
    OpenHomeSwipeScreen,
    OpenFrontlightSettings,
    ClearFrontlightSyncBook,
    PrepareFrontlightSync,
    OpenFrontlightSync,
    CheckFrontlightSync,
    DisableReadingStats,
    EnableReadingStats,
    DisableReaderTouch,
    EnableReaderTouch,
    TouchDown,
    TouchDrawerHandle,
    TouchFrontlightQuickAction,
    TouchMove,
    TouchRelease,
    AssertReaderMenu,
    AssertSettingsNavigation,
    AssertMenuNavigation,
    WaitForSettingsCategory,
    CaptureMenuNavigation,
    AssertAboutTopIndex,
    WaitForNavigationHold,
    AssertActivity,
    Render
  };

  struct ScriptAction {
    ScriptActionType type;
    MappedInputManager::Button button;
    const char* label;
    int settleFrames;
    int x;
    int y;
  };

  uint8_t savedChapterShortcuts[7]{};
  uint32_t lastInjectedHomeAt = 0;
  SmokeStep step = SmokeStep::Start;
  int settleFrames = 0;
  const char* activeStepName = nullptr;
  std::vector<ScriptAction> inputScript;
  size_t scriptIndex = 0;
  unsigned libraryRefreshPass = 0;
  uint16_t libraryBaselineBooks = 0;
  SmokeStep inputCompletionStep = SmokeStep::Done;
  unsigned carouselCachePass = 0;
  std::filesystem::file_time_type carouselCacheWrittenAt;
  std::filesystem::file_time_type carouselSecondWrittenAt;
  uint64_t carouselCacheHash = 0;
  uint64_t carouselScreenHash = 0;
  unsigned frontlightLayoutPass = 0;
  unsigned supportPhase = 0;
  std::string priorSupportExport;
  unsigned filenameFontPhase = 0;
  unsigned hyphenationPhase = 0;
  unsigned aboutPhase = 0;
  unsigned aboutPass = 0;
  unsigned releaseNotesPhase = 0;
  unsigned releaseNotesWait = 0;
  unsigned releaseNotesPass = 0;
  int releaseNotesPortraitPages = 0;
  int releaseNotesPortraitWidth = 0;
  std::vector<uint8_t> releaseNotesHintStrip;
  uint32_t aboutSnapshotUptime = 0;
  unsigned homeThemePass = 0;
  unsigned calibrationPhase = 0;
  unsigned receiveFolderPhase = 0;
  unsigned receiveFolderPass = 0;
  bool receiveFolderReturned = false;
  bool receiveFolderCancelled = false;
  std::string receiveFolderResult;
  unsigned calibrationPickerPass = 0;
  int calibrationPickerResult = -1;
  ScreenInsets calibrationExpected;
  Activity* calibrationResumedHome = nullptr;
  std::string calibrationStoragePath;
  uint64_t homeThemeScreenHash = 0;
  std::string homeThemeBookPath;

  uint8_t homeReaderReaderKind = 0;
  uint8_t homeReaderCancelledMask = 0;
  Activity* homeReaderSmokeReader = nullptr;
  bool homeReaderConfirmationAccepted = false;
  bool backHomeChildCancelled = false;
  uint8_t navigationLongPass = 0;
  uint8_t savedNavigationMenu = 0;
  uint8_t savedNavigationBack = 0;
  unsigned long navigationPressAt = 0;
  Activity* navigationExpected = nullptr;

  void prepareRecentLibrary() {
    SETTINGS.librarySortMethod = 4;
    SETTINGS.librarySortDescending = 1;
    SETTINGS.libraryUseMetadata = 1;
    SETTINGS.libraryShowTxt = 1;
    SETTINGS.libraryHideFinishedBooks = 0;
    for (int i = 0; i < 20; ++i) {
      const std::string path = "/books/recent-smoke-" + std::to_string(i) + ".txt";
      if (!Storage.writeFile(path.c_str(), "Recent Library smoke fixture")) fail("Cannot create recent fixture");
      RECENT_BOOKS.addOrUpdateBook(path, "Title " + std::to_string(i), "Author " + std::to_string(i), "");
    }
    Storage.remove(library::libraryIndexPath());
    library::invalidateLibraryIndex();
  }

  static const char* homeReaderSmokeReaderName(const uint8_t kind) {
    switch (kind) {
      case 0:
        return "EpubReader";
      case 1:
        return "TxtReader";
      default:
        return "XtcReader";
    }
  }

  static void verifyDictionaryElisions() {
    Dictionary::setLookupDictPathOverride("/dictionary-smoke/dict");
    static constexpr const char* cases[][2] = {
        {"l'histoire", "histoire"}, {"l’inspecteur", "inspecteur"},
        {"d'histoire", "histoire"}, {"qu'après", "après"},
        {"QU’Après", "après"},      {"Lʼécole", "école"},
        {"l’école", "école"},       {"j'aime", "aime"},
        {"n’aime", "aime"},         {"m'aime", "aime"},
        {"s’aime", "aime"},         {"t'aime", "aime"},
        {"c’est", "est"},
    };
    for (const auto& entry : cases) {
      bool matchedStem = false;
      const auto result = Dictionary::locateWithStemVariants(Dictionary::cleanWord(entry[0]), &matchedStem);
      if (!result.found || result.readError || result.headword != entry[1] || !matchedStem)
        fail("French dictionary lookup failed: %s -> %s", entry[0], entry[1]);
    }
    bool matchedStem = true;
    const auto exact = Dictionary::locateWithStemVariants("d'accord", &matchedStem);
    if (!exact.found || exact.headword != "d'accord" || matchedStem)
      fail("Exact dictionary headword must take priority over elision");
    for (const char* word : {"don't", "aujourd’hui", "l'", "qu’", "élèves"}) {
      const auto variants = Dictionary::getStemVariants(word);
      if (!variants.empty()) fail("Unexpected dictionary variant for %s", word);
    }
    const auto nameVariants = Dictionary::getStemVariants("O'Brien");
    if (std::find(nameVariants.begin(), nameVariants.end(), "brien") != nameVariants.end() ||
        std::find(nameVariants.begin(), nameVariants.end(), "Brien") != nameVariants.end())
      fail("Unrecognized apostrophe prefix was stripped");
    const auto stems = Dictionary::getStemVariants("running");
    if (std::find(stems.begin(), stems.end(), "run") == stems.end()) fail("English stemming regressed");
    const DictLookupCallbacks cancelled{nullptr, nullptr, [](void*) { return true; }};
    if (Dictionary::locateWithStemVariants("l'histoire", &matchedStem, cancelled).found)
      fail("Cancelled dictionary lookup returned a match");
    Dictionary::clearLookupDictPathOverride();
    LOG_INF("SMOKE", "Simulator smoke test passed: French dictionary elisions and exact-match priority");
    std::_Exit(0);
  }

  static bool enabled() { return std::getenv("CROSSINK_SIMULATOR_SMOKE_TEST") != nullptr; }

  static int pageTurnCount() {
    const char* raw = std::getenv("CROSSINK_SIMULATOR_SMOKE_PAGE_TURNS");
    if (raw == nullptr || raw[0] == '\0') {
      return 2;
    }
    return std::max(0, std::atoi(raw));
  }

  static bool landscapeReaderRequested() {
    const char* raw = std::getenv("CROSSINK_SIMULATOR_SMOKE_LANDSCAPE_READER");
    return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
  }

  static void applyRequestedTheme() {
    const char* raw = std::getenv("CROSSINK_SIMULATOR_SMOKE_THEME");
    if (raw == nullptr || raw[0] == '\0') {
      return;
    }

    const int theme = std::atoi(raw);
    if (theme < 0 || theme >= CrossPointSettings::UI_THEME_COUNT) {
      fail("Invalid smoke test theme index: %d", theme);
    }

    SETTINGS.uiTheme = static_cast<uint8_t>(theme);
    UITheme::getInstance().reload();
    LOG_INF("SMOKE", "Using theme index %d", theme);
  }

  static void verifyGestureShortcutChanges() {
    JsonDocument original;
    SETTINGS.toJson(original);
    using C = CrossPointSettings;
    const auto all = getSettingsList();
    for (const auto& setting : getBaseSettingsList()) {
      if (settingKeyIs(setting, "touchReaderControls") || settingKeyIs(setting, "longPressButtonBehavior"))
        fail("Retired setting still in shared device/web catalog");
    }
    for (const uint8_t legacyMode :
         {C::OFF, C::CHAPTER_SKIP, C::FONT_SIZE_CHANGE, C::ORIENTATION_CHANGE, C::ORIENTATION_FLIP}) {
      JsonDocument old;
      old["touchReaderControls"] = 0;
      old["longPressButtonBehavior"] = legacyMode;
      old["pageTurnGesture"] = C::SWIPE_ONLY;
      old["previousPageGesture"] = C::INVERTED_TAP;
      SETTINGS.fromJson(old.as<JsonVariantConst>());
      const uint8_t left = legacyMode == C::CHAPTER_SKIP         ? C::SIDE_PREVIOUS_CHAPTER
                           : legacyMode == C::FONT_SIZE_CHANGE   ? C::SIDE_DECREASE_FONT
                           : legacyMode == C::ORIENTATION_FLIP   ? C::SIDE_ROTATE_FLIP
                           : legacyMode == C::ORIENTATION_CHANGE ? C::SIDE_ROTATE_CLOCKWISE
                                                                 : uint8_t{C::IGNORE};
      const uint8_t right = legacyMode == C::CHAPTER_SKIP         ? C::SIDE_NEXT_CHAPTER
                            : legacyMode == C::FONT_SIZE_CHANGE   ? C::SIDE_INCREASE_FONT
                            : legacyMode == C::ORIENTATION_FLIP   ? C::SIDE_ROTATE_FLIP
                            : legacyMode == C::ORIENTATION_CHANGE ? C::SIDE_ROTATE_COUNTERCLOCKWISE
                                                                  : uint8_t{C::IGNORE};
      if (SETTINGS.frontButtonLeftLong != left || SETTINGS.frontButtonRightLong != right ||
          SETTINGS.disablePageTurnGestures || SETTINGS.pageTurnGesture != C::SWIPE_ONLY ||
          SETTINGS.previousPageGesture != C::INVERTED_TAP)
        fail("Legacy front/touch migration failed");
      old["frontButtonLeftLong"] = C::LIBRARY;
      SETTINGS.fromJson(old.as<JsonVariantConst>());
      if (SETTINGS.frontButtonLeftLong != C::LIBRARY || SETTINGS.frontButtonRightLong != right)
        fail("Partial front migration replaced explicit choice");
      JsonDocument saved;
      SETTINGS.toJson(saved);
      if (!saved["touchReaderControls"].isNull() || !saved["longPressButtonBehavior"].isNull())
        fail("Retired settings exported again");
      SETTINGS.fromJson(saved.as<JsonVariantConst>());
      if (SETTINGS.frontButtonLeftLong != C::LIBRARY || SETTINGS.frontButtonRightLong != right)
        fail("Front assignments did not survive round trip");
    }
    for (const auto catalog :
         {ShortcutOptionCatalog::PowerButton, ShortcutOptionCatalog::ButtonChord, ShortcutOptionCatalog::LongPress,
          ShortcutOptionCatalog::HomeButton, ShortcutOptionCatalog::SideButton}) {
      const auto setting = buildShortcutSetting(StrId::STR_LONG_PRESS_ACTION, &C::shortPwrBtn, "test", catalog);
      const auto raw = shortcutRawValue(catalog, C::TOGGLE_PAGE_TURN_GESTURES);
      if (gpio.hasTouch() &&
          (raw == SHORTCUT_OPTION_UNAVAILABLE ||
           std::find(setting.enumRawValues.begin(), setting.enumRawValues.end(), raw) == setting.enumRawValues.end()))
        fail("Page gesture shortcut missing from catalog");
    }
    SETTINGS.frontButtonLeftLong = C::LIBRARY;
    SETTINGS.frontButtonRightLong = C::FORCE_REFRESH;
    mappedInputManager.setReaderMode(true);
    for (const auto button : {MappedInputManager::Button::Left, MappedInputManager::Button::Right}) {
      ReaderButtonShortcuts tracker;
      mappedInputManager.simulatorInjectPress(button);
      auto result = tracker.update(mappedInputManager, 100);
      if (result.triggered || !result.consumed) fail("Front shortcut fired on press");
      mappedInputManager.simulatorClearInputFrame();
      result = tracker.update(mappedInputManager, 100 + ReaderUtils::SKIP_HOLD_MS);
      if (!result.triggered || !result.longPress || !result.front ||
          result.action != (button == MappedInputManager::Button::Left ? C::LIBRARY : C::FORCE_REFRESH))
        fail("Independent front long action failed");
      if (tracker.update(mappedInputManager, 2000).triggered) fail("Front hold repeated");
      mappedInputManager.suppressNextFrontRelease(button);
      if (mappedInputManager.isPressed(button)) fail("Front hold leaked into shortcut destination");
      mappedInputManager.simulatorClearInputFrame();
      mappedInputManager.simulatorInjectRelease(button);
      if (mappedInputManager.wasReleased(button) || tracker.update(mappedInputManager, 2100).triggered)
        fail("Front hold leaked a release/page turn");
      mappedInputManager.simulatorClearInputFrame();
      mappedInputManager.simulatorInjectPress(button);
      tracker.update(mappedInputManager, 2200);
      mappedInputManager.simulatorClearInputFrame();
      mappedInputManager.simulatorInjectRelease(button);
      result = tracker.update(mappedInputManager, 2300);
      if (!result.triggered || result.longPress ||
          result.action != (button == MappedInputManager::Button::Left ? C::PREVIOUS_PAGE : C::PAGE_TURN))
        fail("Short page turn lost after front long action");
      mappedInputManager.simulatorClearInputFrame();
    }
    for (const auto trigger : {QuickLockTrigger::FrontLeftLong, QuickLockTrigger::FrontRightLong}) {
      ButtonShortcutController lock;
      lock.toggleQuickLock(100, trigger);
      const bool left = trigger == QuickLockTrigger::FrontLeftLong;
      if (lock.tryUnlockFront(200, left, left, false, !left, !left, false, 700)) fail("Front unlocked on press");
      if (!lock.tryUnlockFront(900, left, false, false, !left, false, false, 700) || lock.isQuickLocked())
        fail("Front Quick Lock could not unlock");
    }
    QuickActions::applyTrigger(SETTINGS, QuickActions::Trigger::FrontLeftLong);
    QuickActions::synchronize(SETTINGS);
    if (SETTINGS.frontButtonLeftLong != C::QUICK_ACTIONS) fail("Front Quick Actions trigger lost");
    QuickActions::applyTrigger(SETTINGS, QuickActions::Trigger::FrontRightLong);
    if (SETTINGS.frontButtonLeftLong != C::IGNORE || SETTINGS.frontButtonRightLong != C::QUICK_ACTIONS)
      fail("Front Quick Actions exclusivity failed");
#if CROSSINK_APP_CAP_TOUCH
    SETTINGS.pageTurnGesture = SETTINGS.previousPageGesture = C::TAP_AND_SWIPE;
    SETTINGS.disablePageTurnGestures = 0;
    const int w = renderer.getScreenWidth(), h = renderer.getScreenHeight();
    for (const bool paused : {false, true}) {
      if (paused && !dispatchShortcutAction(C::TOGGLE_PAGE_TURN_GESTURES)) fail("Cannot pause page gestures");
      for (const bool locked : {false, true}) {
        SETTINGS.disableReaderTouchscreen = locked;
        mappedInputManager.simulatorInjectTouchDown(w / 2, h * 3 / 4);
        mappedInputManager.simulatorInjectTouchRelease(w / 2, h / 4);
        if (ReaderUtils::isTouchMenuGesture(mappedInputManager) != !locked) fail("Reader menu gate regression");
        mappedInputManager.simulatorClearInputFrame();
        mappedInputManager.simulatorInjectTouchDown(w / 2, 1);
        mappedInputManager.simulatorInjectTouchRelease(w / 2, h / 2);
        if (mappedInputManager.wasReaderLightPanelGesture() != !locked) fail("Frontlight gate regression");
        mappedInputManager.simulatorClearInputFrame();
        mappedInputManager.simulatorInjectTouchDown(w * 3 / 4, h / 2);
        mappedInputManager.simulatorInjectTouchRelease(w * 3 / 4, h / 2);
        const auto tap = ReaderUtils::detectTouchPageTurn(renderer, mappedInputManager);
        if (tap.tapped != !locked || tap.next != (!locked && !paused)) fail("Page pause blocked other taps");
        mappedInputManager.simulatorClearInputFrame();
        mappedInputManager.simulatorInjectTouchDown(w * 3 / 4, h / 2);
        mappedInputManager.simulatorInjectTouchRelease(w / 4, h / 2);
        const auto swipe = ReaderUtils::detectTouchPageTurn(renderer, mappedInputManager);
        if (swipe.next != (!locked && !paused)) fail("Page swipe pause regression");
        mappedInputManager.simulatorClearInputFrame();
      }
    }
    if (!dispatchShortcutAction(C::TOGGLE_PAGE_TURN_GESTURES) || SETTINGS.disablePageTurnGestures ||
        SETTINGS.pageTurnGesture != C::TAP_AND_SWIPE)
      fail("Cannot restore page gestures");
#endif
    mappedInputManager.setReaderMode(false);
    SETTINGS.fromJson(original.as<JsonVariantConst>());
    LOG_INF("SMOKE", "Gesture shortcut migration, catalogs, gates, front releases and lock checks passed");
  }

  static void verifyMixedPageGestures() {
#if CROSSINK_APP_CAP_TOUCH
    if (!gpio.hasTouch()) return;
    const uint8_t savedNext = SETTINGS.pageTurnGesture;
    const uint8_t savedPrevious = SETTINGS.previousPageGesture;
    const int width = renderer.getScreenWidth();
    const int y = renderer.getScreenHeight() / 2;
    mappedInputManager.setReaderMode(true);
    for (uint8_t next = 0; next < CrossPointSettings::PAGE_TURN_GESTURE_COUNT; ++next) {
      for (uint8_t previous = 0; previous < CrossPointSettings::PAGE_TURN_GESTURE_COUNT; ++previous) {
        SETTINGS.pageTurnGesture = next;
        SETTINGS.previousPageGesture = previous;
        const bool inverted = next == CrossPointSettings::INVERTED_TAP || previous == CrossPointSettings::INVERTED_TAP;
        const bool nextTap = next == CrossPointSettings::TAP_AND_SWIPE || next == CrossPointSettings::TAP_ONLY ||
                             next == CrossPointSettings::INVERTED_TAP;
        const bool previousTap = previous == CrossPointSettings::TAP_AND_SWIPE ||
                                 previous == CrossPointSettings::TAP_ONLY ||
                                 previous == CrossPointSettings::INVERTED_TAP;
        for (const int x : {0, width / 3 - 1, width / 3, width * 2 / 3 - 1, width * 2 / 3, width - 1}) {
          mappedInputManager.simulatorInjectTouchDown(x, y);
          mappedInputManager.simulatorInjectTouchRelease(x, y);
          const auto result = ReaderUtils::detectTouchPageTurn(renderer, mappedInputManager);
          const bool nextZone = inverted ? x < width * 2 / 3 : x >= width / 3;
          const bool expectedNext = nextTap && (!previousTap || nextZone);
          const bool expectedPrevious = previousTap && (!nextTap || !nextZone);
          if (!result.tapped || result.next != expectedNext || result.prev != expectedPrevious) {
            fail("Mixed page tap mismatch: next=%u previous=%u x=%d", next, previous, x);
          }
          mappedInputManager.simulatorClearInputFrame();
        }
        for (const bool right : {false, true}) {
          const int startX = right ? 1 : width - 2;
          const int endX = right ? width - 2 : 1;
          mappedInputManager.simulatorInjectTouchDown(startX, y);
          mappedInputManager.simulatorInjectTouchMove(endX, y);
          mappedInputManager.simulatorInjectTouchRelease(endX, y);
          const auto result = ReaderUtils::detectTouchPageTurn(renderer, mappedInputManager);
          const uint8_t mode = right ? previous : next;
          const bool expected = (mode == CrossPointSettings::TAP_AND_SWIPE || mode == CrossPointSettings::SWIPE_ONLY);
          if (result.next != (!right && expected) || result.prev != (right && expected) ||
              (right && !expected && mappedInputManager.wasReleased(MappedInputManager::Button::Back))) {
            fail("Mixed page swipe mismatch: next=%u previous=%u right=%d", next, previous, right);
          }
          mappedInputManager.simulatorClearInputFrame();
        }
      }
    }
    SETTINGS.pageTurnGesture = savedNext;
    SETTINGS.previousPageGesture = savedPrevious;
    mappedInputManager.setReaderMode(false);
    LOG_INF("SMOKE", "All 25 mixed page gesture combinations passed");
#endif
  }

  static void verifyStatusBarSettings() {
    JsonDocument original;
    SETTINGS.toJson(original);
    const HalClock originalClock = halClock;
    halClock = HalClock{};  // Simulate a failed RTC probe during settings loading.
    for (const int clock : {0, 1}) {
      JsonDocument legacy;
      legacy.set(original);
      legacy.remove("displayStatusBar");
      legacy["showClockOutsideReader"] = clock;
      SETTINGS.fromJson(legacy.as<JsonVariantConst>());
      if (SETTINGS.displayStatusBar.slots[1] != (clock ? ReaderStatusBarItem::Clock : ReaderStatusBarItem::Empty) ||
          SETTINGS.displayStatusBar.slots[2] != ReaderStatusBarItem::Battery)
        fail("Display clock migration failed");
    }
    // Hide Battery % (Never / In Reader / Always) migrates into per-bar battery styles once.
    using Style = ReaderStatusBarBatteryStyle;
    constexpr Style expectedReader[] = {Style::IconAndPercent, Style::IconOnly, Style::IconOnly};
    constexpr Style expectedDisplay[] = {Style::IconAndPercent, Style::IconAndPercent, Style::IconOnly};
    for (const uint8_t hide : {0, 1, 2}) {
      JsonDocument legacy;
      legacy.set(original);
      legacy.remove("displayBatteryStyle");
      legacy["readerStatusBars"]["top"].remove("battery");
      legacy["readerStatusBars"]["bottom"].remove("battery");
      legacy["hideBatteryPercentage"] = hide;
      SETTINGS.fromJson(legacy.as<JsonVariantConst>());
      if (SETTINGS.topReaderStatusBar.batteryStyle != expectedReader[hide] ||
          SETTINGS.bottomReaderStatusBar.batteryStyle != expectedReader[hide] ||
          SETTINGS.displayStatusBar.batteryStyle != expectedDisplay[hide])
        fail("Battery style migration failed for hideBatteryPercentage=%u", hide);
      JsonDocument migrated;
      SETTINGS.toJson(migrated);
      migrated["hideBatteryPercentage"] = 0;  // A stale legacy key must not override saved styles.
      SETTINGS.fromJson(migrated.as<JsonVariantConst>());
      if (SETTINGS.bottomReaderStatusBar.batteryStyle != expectedReader[hide] ||
          SETTINGS.displayStatusBar.batteryStyle != expectedDisplay[hide])
        fail("Battery style did not survive a save round trip for hideBatteryPercentage=%u", hide);
    }
    SETTINGS.fromJson(original.as<JsonVariantConst>());
    SETTINGS.displayStatusBar.slots = {ReaderStatusBarItem::Date, ReaderStatusBarItem::Clock,
                                       ReaderStatusBarItem::Empty};
    SETTINGS.topReaderStatusBar.slots[ReaderStatusBarConfig::CENTER] = ReaderStatusBarItem::Clock;
    SETTINGS.bottomReaderStatusBar.slots[ReaderStatusBarConfig::CENTER] = ReaderStatusBarItem::Date;
    JsonDocument saved;
    SETTINGS.toJson(saved);
    if (!saved["showClockOutsideReader"].isNull()) fail("Obsolete clock setting was saved");
    SETTINGS.displayStatusBar = DisplayStatusBarConfig{};
    SETTINGS.fromJson(saved.as<JsonVariantConst>());
    if (SETTINGS.displayStatusBar.slots[0] != ReaderStatusBarItem::Date ||
        SETTINGS.displayStatusBar.slots[1] != ReaderStatusBarItem::Clock ||
        SETTINGS.displayStatusBar.slots[2] != ReaderStatusBarItem::Empty)
      fail("Display slots did not survive reload");
    if (SETTINGS.topReaderStatusBar.slots[ReaderStatusBarConfig::CENTER] != ReaderStatusBarItem::Clock ||
        SETTINGS.bottomReaderStatusBar.slots[ReaderStatusBarConfig::CENTER] != ReaderStatusBarItem::Date)
      fail("Reader clock/date slots did not survive an unavailable RTC");
    halClock = originalClock;
    const auto display = buildGroupedDisplaySettingsList(getSettingsList());
    if (std::none_of(display.begin(), display.end(),
                     [](const auto& item) { return item.action == SettingAction::DisplayStatusBar; }))
      fail("Display status bar setting is missing");
    SETTINGS.fromJson(original.as<JsonVariantConst>());
    LOG_INF("SMOKE", "Display status bar migration and persistence passed");
  }

  static void captureStatusBarScreen(const char* name) {
    const char* output = std::getenv("CROSSINK_SIMULATOR_SMOKE_STATUS_BAR_CAPTURES");
    if (!output) return;
    std::filesystem::create_directories(output);
    const auto path = std::filesystem::path(output) / (std::string(name) + ".pgm");
    FILE* image = std::fopen(path.c_str(), "wb");
    if (!image) fail("Cannot create status bar capture");
    const int width = renderer.getScreenWidth();
    const int height = renderer.getScreenHeight();
    std::fprintf(image, "P5\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) std::fputc(renderer.isPixelBlack(x, y) ? 0 : 255, image);
    }
    std::fclose(image);
  }

  static void verifyMenuOrientation() {
    RenderLock lock;
    applyRequestedTheme();
    using B = MappedInputManager::Button;
    using C = CrossPointSettings;
    JsonDocument original;
    SETTINGS.toJson(original);
    const auto savedOrientation = renderer.getOrientation();
    MappedInputManager input(gpio, renderer);
    ButtonNavigator::setMappedInputManager(input);
    const auto edge = [&](const uint8_t physical, const bool pressed) {
      constexpr SDL_Scancode keys[] = {SDL_SCANCODE_ESCAPE, SDL_SCANCODE_RETURN, SDL_SCANCODE_LEFT,
                                       SDL_SCANCODE_RIGHT,  SDL_SCANCODE_UP,     SDL_SCANCODE_DOWN};
      gpio.beginFrame();
      SDL_Event event{};
      event.type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
      event.key.keysym.scancode = keys[physical];
      if (SDL_PushEvent(&event) != 1) fail("Cannot inject physical menu button");
      input.update();
    };
    size_t checks = 0;
    // Include custom front slots and reader-specific assignments, not just the defaults.
    for (const bool custom : {false, true}) {
      SETTINGS.frontButtonBack = custom ? HalGPIO::BTN_CONFIRM : HalGPIO::BTN_BACK;
      SETTINGS.frontButtonConfirm = custom ? HalGPIO::BTN_BACK : HalGPIO::BTN_CONFIRM;
      SETTINGS.frontButtonLeft = HalGPIO::BTN_LEFT;
      SETTINGS.frontButtonRight = HalGPIO::BTN_RIGHT;
      SETTINGS.readerFrontButtonBack = HalGPIO::BTN_LEFT;
      SETTINGS.readerFrontButtonConfirm = HalGPIO::BTN_RIGHT;
      SETTINGS.readerFrontButtonLeft = HalGPIO::BTN_BACK;
      SETTINGS.readerFrontButtonRight = HalGPIO::BTN_CONFIRM;
      for (const bool reader : {false, true}) {
        input.setReaderMode(reader);
        for (const bool readerMapping : {false, true}) {
          SETTINGS.readerFrontButtonsEnabled = readerMapping;
          for (const uint8_t aware : {C::FRONT_ORIENTATION_AWARE_OFF, C::FRONT_ORIENTATION_AWARE_NAV_BUTTONS,
                                      C::FRONT_ORIENTATION_AWARE_ALL_BUTTONS}) {
            SETTINGS.frontButtonOrientationAware = aware;
            for (const bool sideAware : {false, true}) {
              SETTINGS.sideButtonOrientationAware = sideAware;
              for (int rotation = 0; rotation < 4; ++rotation) {
                SETTINGS.orientation = (rotation + 1) % 4;  // A menu may rotate independently of the book.
                renderer.setOrientation(static_cast<GfxRenderer::Orientation>(rotation));
                input.setMenuMode(true);
                // Reproduce a theme's temporary portrait hint pass while input is dispatched.
                renderer.setOrientation(GfxRenderer::Portrait);
                const bool swap =
                    rotation == GfxRenderer::LandscapeCounterClockwise || rotation == GfxRenderer::PortraitInverted;
                const bool useReader = reader && readerMapping;
                const uint8_t back = useReader ? SETTINGS.readerFrontButtonBack : SETTINGS.frontButtonBack;
                const uint8_t confirm = useReader ? SETTINGS.readerFrontButtonConfirm : SETTINGS.frontButtonConfirm;
                const uint8_t left = useReader ? SETTINGS.readerFrontButtonLeft : SETTINGS.frontButtonLeft;
                const uint8_t right = useReader ? SETTINGS.readerFrontButtonRight : SETTINGS.frontButtonRight;
                const uint8_t expected[] = {back, confirm, swap ? right : left, swap ? left : right};
                const auto labels = input.mapLabels("Back", "Confirm", "Up", "Down");
                const char* physicalLabels[] = {labels.btn1, labels.btn2, labels.btn3, labels.btn4};
                constexpr const char* names[] = {"Back", "Confirm", "Up", "Down"};
                constexpr B logical[] = {B::Back, B::Confirm, B::Left, B::Right};
                for (size_t i = 0; i < 4; ++i) {
                  if (std::strcmp(physicalLabels[expected[i]], names[i]) != 0) fail("Menu label rotation mismatch");
                  for (const bool pressed : {true, false}) {
                    edge(expected[i], pressed);
                    for (size_t j = 0; j < 4; ++j) {
                      const bool actual = pressed ? input.wasPressed(logical[j]) : input.wasReleased(logical[j]);
                      if (actual != (i == j)) fail("Menu action disagrees with its physical hint");
                      ++checks;
                    }
                  }
                }
                const bool landscape =
                    rotation == GfxRenderer::LandscapeClockwise || rotation == GfxRenderer::LandscapeCounterClockwise;
                const bool horizontalSide = deviceUsesHorizontalSideButtonsForMenus(gpio);
                if ((input.menuButton(B::Up) == B::Left) != (horizontalSide || landscape))
                  fail("Front menu Up/Down roles changed on rotation");
                const bool sideSwap = rotation == GfxRenderer::PortraitInverted ||
                                      rotation == (horizontalSide ? GfxRenderer::LandscapeCounterClockwise
                                                                  : GfxRenderer::LandscapeClockwise);
                for (const B direction : {B::Up, B::Down}) {
                  const bool physicalUp = (direction == B::Up) != sideSwap;
                  edge(physicalUp ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN, true);
                  if (!input.wasPressed(direction)) fail("Side menu press did not rotate");
                  edge(physicalUp ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN, false);
                  if (!input.wasReleased(direction)) fail("Side menu release did not rotate");
                  edge(physicalUp ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN, true);
                  input.suppressNextSideRelease(direction);
                  edge(physicalUp ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN, false);
                  if (input.wasReleased(direction)) fail("Rotated side-button release escaped suppression");
                  checks += 2;
                }
                // Returning from a menu must restore the existing reader preference.
                renderer.setOrientation(static_cast<GfxRenderer::Orientation>(rotation));
                input.setMenuMode(false);
                SETTINGS.orientation = rotation;
                const bool readerSideSwap = reader && sideAware && rotation != GfxRenderer::Portrait;
                edge(readerSideSwap ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP, true);
                if (!input.wasPressed(B::Up)) fail("Reader side-button preference changed");
                edge(readerSideSwap ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP, false);
                if (!input.wasReleased(B::Up)) fail("Reader side-button release changed");
                checks += 2;
                const auto pageLabels = input.mapLabels("Back", "Confirm", "Previous", "Next");
                const char* pagePhysical[] = {pageLabels.btn1, pageLabels.btn2, pageLabels.btn3, pageLabels.btn4};
                uint8_t pageLeft = left;
                if (reader && aware == C::FRONT_ORIENTATION_AWARE_ALL_BUTTONS &&
                    rotation == GfxRenderer::PortraitInverted) {
                  constexpr uint8_t inverted[] = {3, 2, 1, 0};
                  pageLeft = inverted[left];
                } else if (reader && aware != C::FRONT_ORIENTATION_AWARE_OFF &&
                           (landscape || rotation == GfxRenderer::PortraitInverted)) {
                  pageLeft = right;
                }
                if (std::strcmp(pagePhysical[pageLeft], "Previous") != 0) fail("Reader page-turn preference changed");
                ++checks;
              }
            }
          }
        }
      }
    }
    // Quick Actions is embedded in all three readers rather than pushed as an activity.
    SETTINGS.frontButtonOrientationAware = C::FRONT_ORIENTATION_AWARE_OFF;
    SETTINGS.readerFrontButtonsEnabled = false;
    SETTINGS.sideButtonOrientationAware = false;
    SETTINGS.frontButtonBack = HalGPIO::BTN_BACK;
    SETTINGS.frontButtonConfirm = HalGPIO::BTN_CONFIRM;
    SETTINGS.frontButtonLeft = HalGPIO::BTN_LEFT;
    SETTINGS.frontButtonRight = HalGPIO::BTN_RIGHT;
    SETTINGS.orientation = C::LANDSCAPE_CCW;
    renderer.setOrientation(GfxRenderer::LandscapeCounterClockwise);
    SETTINGS.quickActionSlots[0] = C::FORCE_REFRESH;
    SETTINGS.quickActionSlots[1] = C::LIBRARY;
    input.setReaderMode(true);
    const auto popupCheck = [&](auto& reader) {
      auto& popup = ReaderMenuOrientationSmokeTest::popup(reader);
      input.setMenuMode(reader.usesMenuButtonOrientation());
      if (input.isMenuMode()) fail("Reader unexpectedly uses menu mapping before popup");
      if (!reader.handleShortcutAction(C::QUICK_ACTIONS)) fail("Cannot open reader Quick Actions");
      input.setMenuMode(reader.usesMenuButtonOrientation());
      if (!input.isMenuMode()) fail("Embedded reader popup did not request menu mapping");
      popup.processRender(renderer, input);
      const int initial = popup.simulatorSelectedIndex();
      edge(HalGPIO::BTN_LEFT, true);
      popup.handleInput(input, [] {});
      edge(HalGPIO::BTN_LEFT, false);
      const bool nextReleased = input.wasReleased(B::Right);
      popup.handleInput(input, [] {});
      if (popup.simulatorSelectedIndex() != initial + 1)
        fail("Popup lower button did not move down (%d -> %d, mapped release=%d)", initial,
             popup.simulatorSelectedIndex(), nextReleased);
      edge(HalGPIO::BTN_RIGHT, true);
      popup.handleInput(input, [] {});
      edge(HalGPIO::BTN_RIGHT, false);
      popup.handleInput(input, [] {});
      if (popup.simulatorSelectedIndex() != initial) fail("Popup upper button did not move up");
      const bool oppositeEdges = deviceUsesHorizontalSideButtonsForMenus(gpio);
      const uint8_t bottom = oppositeEdges ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN;
      const uint8_t top = oppositeEdges ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
      for (const bool pressed : {true, false}) {
        edge(bottom, pressed);
        popup.handleInput(input, [] {});
      }
      if (popup.simulatorSelectedIndex() != initial + 1) fail("Popup bottom side button did not move down");
      for (const bool pressed : {true, false}) {
        edge(top, pressed);
        popup.handleInput(input, [] {});
      }
      if (popup.simulatorSelectedIndex() != initial) fail("Popup top side button did not move up");
      edge(HalGPIO::BTN_BACK, true);
      popup.handleInput(input, [] {});
      edge(HalGPIO::BTN_BACK, false);
      popup.handleInput(input, [] {});
      input.setMenuMode(reader.usesMenuButtonOrientation());
      if (input.isMenuMode()) fail("Closing reader popup did not restore page mapping");
      input.update();
      checks += 6;
    };
    {
      EpubReaderActivity reader(renderer, input, nullptr, {}, 0);
      popupCheck(reader);
    }
    {
      TxtReaderActivity reader(renderer, input, nullptr, 0);
      popupCheck(reader);
    }
    {
      XtcReaderActivity reader(renderer, input, nullptr, 0);
      popupCheck(reader);
    }
    // Suggestions must ignore page-turn assignments, including swapped or disabled reader keys.
    std::filesystem::create_directories("fs_/menu-end");
    for (const char* name : {"a.epub", "b.epub", "c.epub"}) {
      const auto path = std::filesystem::path("fs_/menu-end") / name;
      FILE* file = std::fopen(path.c_str(), "wb");
      if (!file) fail("Cannot create end-menu fixture");
      std::fclose(file);
    }
    for (const auto orientation : {GfxRenderer::LandscapeClockwise, GfxRenderer::LandscapeCounterClockwise}) {
      renderer.setOrientation(orientation);
      SETTINGS.orientation = orientation;
      input.setMenuMode(true);
      const bool ccw = orientation == GfxRenderer::LandscapeCounterClockwise;
      const bool oppositeEdges = deviceUsesHorizontalSideButtonsForMenus(gpio);
      const bool swap = oppositeEdges ? ccw : !ccw;
      const uint8_t bottom = swap ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN;
      const uint8_t top = swap ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
      for (const bool aware : {false, true}) {
        SETTINGS.sideButtonOrientationAware = aware;
        for (const auto actions : {std::pair{C::PREVIOUS_PAGE, C::PAGE_TURN}, std::pair{C::PAGE_TURN, C::PREVIOUS_PAGE},
                                   std::pair{C::IGNORE, C::IGNORE}, std::pair{C::LIBRARY, C::FORCE_REFRESH}}) {
          SETTINGS.sideButtonUpShort = actions.first;
          SETTINGS.sideButtonDownShort = actions.second;
          EndOfBookOptions suggestions(renderer);
          suggestions.loadOnce("/menu-end/a.epub");
          if (!suggestions.menuActive()) fail("End-menu fixture has no suggestions");
          suggestions.render(renderer, input);
          const auto tap = [&](const uint8_t button, std::string* path = nullptr) {
            edge(button, true);
            suggestions.handleMenuInput(input, path);
            edge(button, false);
            return suggestions.handleMenuInput(input, path);
          };
          if (tap(bottom) != EndOfBookOptions::Action::Redraw) fail("Bottom side key did not move suggestions");
          std::string selected;
          if (tap(SETTINGS.frontButtonConfirm, &selected) != EndOfBookOptions::Action::OpenBook ||
              selected != "/menu-end/c.epub")
            fail("Bottom side key did not select the next suggestion");
          if (tap(top) != EndOfBookOptions::Action::Redraw) fail("Top side key did not move suggestions");
          if (tap(SETTINGS.frontButtonConfirm, &selected) != EndOfBookOptions::Action::OpenBook ||
              selected != "/menu-end/b.epub")
            fail("Top side key did not select the previous suggestion");
          checks += 4;
        }
      }
    }
    SETTINGS.fromJson(original.as<JsonVariantConst>());
    input.setReaderMode(false);
    input.setMenuMode(true);
    SETTINGS.frontButtonOrientationAware = C::FRONT_ORIENTATION_AWARE_OFF;
    for (const auto mode : {C::MENU_NAV_CLASSIC, C::MENU_NAV_DIRECTIONAL}) {
      SETTINGS.menuNavigation = mode;
      for (const auto orientation : {GfxRenderer::LandscapeClockwise, GfxRenderer::LandscapeCounterClockwise}) {
        renderer.setOrientation(orientation);
        SETTINGS.orientation = orientation;
        input.setMenuMode(true);
        SettingsActivity menu(renderer, input);
        menu.onEnter();
        menu.render(std::move(lock));
        const auto tapPhysical = [&](const uint8_t physical) {
          edge(physical, true);
          menu.loop();
          edge(physical, false);
          menu.loop();
        };
        const bool oppositeEdges = deviceUsesHorizontalSideButtonsForMenus(gpio);
        const bool ccw = orientation == GfxRenderer::LandscapeCounterClockwise;
        const uint8_t frontNext = ccw ? SETTINGS.frontButtonLeft : SETTINGS.frontButtonRight;
        const uint8_t frontPrevious = ccw ? SETTINGS.frontButtonRight : SETTINGS.frontButtonLeft;
        const int category = menu.simulatorCategoryIndex();
        tapPhysical(frontNext);
        tapPhysical(frontNext);
        if (menu.simulatorSelectedIndex() != 2 || menu.simulatorCategoryIndex() != category)
          fail("Front Down changed tabs instead of moving rows");
        tapPhysical(frontPrevious);
        if (menu.simulatorSelectedIndex() != 1 || menu.simulatorCategoryIndex() != category)
          fail("Front Up changed tabs instead of moving rows");
        if (oppositeEdges) {
          if (input.menuButton(B::Up) != B::Left) fail("Front hints must remain Up/Down in landscape");
          const uint8_t bottom = ccw ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN;
          const uint8_t top = ccw ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
          tapPhysical(bottom);
          if (mode == C::MENU_NAV_DIRECTIONAL) {
            if (menu.simulatorCategoryIndex() == category) fail("Bottom side button did not advance Settings tabs");
          } else if (menu.simulatorSelectedIndex() != 2 || menu.simulatorCategoryIndex() != category) {
            fail("Classic side tap did not move down a row");
          }
          tapPhysical(top);
          if (menu.simulatorSelectedIndex() != 1 || menu.simulatorCategoryIndex() != category)
            fail("Top side button did not restore Settings selection");
          gpio.beginFrame();
          input.update();
          input.simulatorInjectPress(B::Down);
          menu.loop();
          input.simulatorClearInputFrame();
          delay(510);
          menu.loop();
          const int heldCategory = menu.simulatorCategoryIndex();
          if (heldCategory == category) fail("Held side button did not advance Settings tabs");
          input.simulatorInjectRelease(B::Down);
          menu.loop();
          input.simulatorClearInputFrame();
          if (menu.simulatorCategoryIndex() != heldCategory) fail("Side hold release changed an extra tab");
          tapPhysical(top);
          if (mode == C::MENU_NAV_DIRECTIONAL && menu.simulatorCategoryIndex() != category)
            fail("Previous side key did not restore Settings tab after hold");
          checks += 5;
        }
        checks += 2;
        menu.render(std::move(lock));
        captureStatusBarScreen((std::string("menu-") + (mode == C::MENU_NAV_CLASSIC ? "classic-" : "directional-") +
                                (orientation == GfxRenderer::LandscapeClockwise ? "cw" : "ccw"))
                                   .c_str());
        menu.onExit();
        renderer.setOrientation(orientation);
        input.setMenuMode(true);
        EpubReaderDrawerActivity drawer(renderer, input, {}, nullptr, 0.0f, 0, 0, false, false, false, false, false,
                                        false, false, false, true, true, 0, 0, 5, false);
        drawer.onEnter();
        drawer.render(std::move(lock));
        const auto tapDrawer = [&](const uint8_t physical) {
          edge(physical, true);
          drawer.loop();
          edge(physical, false);
          drawer.loop();
        };
        const auto initialTab = drawer.simulatorState().tab;
        tapDrawer(SETTINGS.frontButtonConfirm);
        if (drawer.simulatorState().tab == initialTab) fail("Confirm can no longer navigate reader tabs");
        const auto tab = drawer.simulatorState().tab;
        tapDrawer(frontNext);  // Focus the first row from the tab band.
        tapDrawer(frontNext);
        if (drawer.simulatorState().selectedIndex != 1 || drawer.simulatorState().tab != tab)
          fail("Reader front Down changed tabs instead of moving rows");
        tapDrawer(frontPrevious);
        if (drawer.simulatorState().selectedIndex != 0 || drawer.simulatorState().tab != tab)
          fail("Reader front Up changed tabs instead of moving rows");
        if (oppositeEdges) {
          tapDrawer(ccw ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN);
          if (mode == C::MENU_NAV_DIRECTIONAL) {
            if (drawer.simulatorState().tab != adjacentReaderDrawerTab(tab, true))
              fail("Reader bottom side button did not advance tabs");
          } else if (drawer.simulatorState().selectedIndex != 1 || drawer.simulatorState().tab != tab) {
            fail("Classic reader side tap did not move down a row");
          }
          tapDrawer(ccw ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP);
          if (drawer.simulatorState().selectedIndex != 0 || drawer.simulatorState().tab != tab)
            fail("Reader top side button did not restore selection");
          if (mode == C::MENU_NAV_DIRECTIONAL) {
            gpio.beginFrame();
            input.update();
            input.simulatorInjectPress(B::Down);
            drawer.loop();
            input.simulatorClearInputFrame();
            delay(510);
            drawer.loop();
            const auto heldTab = drawer.simulatorState().tab;
            if (heldTab != adjacentReaderDrawerTab(tab, true)) fail("Held reader side button did not advance tabs");
            input.simulatorInjectRelease(B::Down);
            drawer.loop();
            input.simulatorClearInputFrame();
            if (drawer.simulatorState().tab != heldTab) fail("Reader side hold release changed an extra tab");
            tapDrawer(ccw ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP);
            if (drawer.simulatorState().tab != tab) fail("Reader previous side key did not restore tab after hold");
            checks += 3;
          }
          checks += 2;
        }
        checks += 3;
        drawer.render(std::move(lock));
        captureStatusBarScreen((std::string("reader-menu-") +
                                (mode == C::MENU_NAV_CLASSIC ? "classic-" : "directional-") + (ccw ? "ccw" : "cw"))
                                   .c_str());
        drawer.onExit();
      }
    }
    // Slider value direction is independent of menu row direction and reader preferences.
    SETTINGS.fromJson(original.as<JsonVariantConst>());
    SETTINGS.frontButtonBack = HalGPIO::BTN_BACK;
    SETTINGS.frontButtonConfirm = HalGPIO::BTN_CONFIRM;
    SETTINGS.frontButtonLeft = HalGPIO::BTN_LEFT;
    SETTINGS.frontButtonRight = HalGPIO::BTN_RIGHT;
    SETTINGS.readerFrontButtonsEnabled = false;
    for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise,
                                   GfxRenderer::PortraitInverted, GfxRenderer::LandscapeCounterClockwise}) {
      renderer.setOrientation(orientation);
      const bool oppositeEdges = deviceUsesHorizontalSideButtonsForMenus(gpio);
      const bool upIncreases =
          oppositeEdges
              ? (orientation == GfxRenderer::LandscapeClockwise || orientation == GfxRenderer::PortraitInverted)
              : (orientation == GfxRenderer::Portrait || orientation == GfxRenderer::LandscapeClockwise);
      const uint8_t increase = upIncreases ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN;
      const uint8_t decrease = upIncreases ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP;
      for (const bool aware : {false, true}) {
        SETTINGS.sideButtonOrientationAware = aware;
        SETTINGS.frontButtonOrientationAware =
            aware ? C::FRONT_ORIENTATION_AWARE_ALL_BUTTONS : C::FRONT_ORIENTATION_AWARE_OFF;
        SETTINGS.orientation = (orientation + 1) % 4;  // The menu's actual orientation owns slider input.
        input.setMenuMode(true);
        const auto tap = [&](auto& slider, const uint8_t physical) {
          edge(physical, true);
          slider.loop();
          edge(physical, false);
          slider.loop();
        };
        const auto checkDirections = [&](auto& slider, const bool checkHold = false) {
          const int before = SliderOrientationSmokeTest::value(slider);
          tap(slider, increase);
          const int raised = SliderOrientationSmokeTest::value(slider);
          if (raised <= before) fail("Upper/right slider button did not increase (orientation=%d)", orientation);
          tap(slider, decrease);
          if (SliderOrientationSmokeTest::value(slider) >= raised)
            fail("Lower/left slider button did not decrease (orientation=%d)", orientation);
          checks += 2;
          if (checkHold) {
            // SDL-pushed edges do not update SDL's held-key state; use the simulator hold API.
            gpio.beginFrame();
            input.update();
            const auto button = input.sliderIncreaseButton();
            input.simulatorInjectPress(button);
            slider.loop();
            input.simulatorClearInputFrame();
            const int pressedValue = SliderOrientationSmokeTest::value(slider);
            delay(510);
            slider.loop();
            const int heldValue = SliderOrientationSmokeTest::value(slider);
            if (heldValue <= pressedValue) fail("Held upper slider button did not repeat upward");
            input.simulatorInjectRelease(button);
            slider.loop();
            input.simulatorClearInputFrame();
            if (SliderOrientationSmokeTest::value(slider) != heldValue) fail("Slider hold release added a step");
            checks += 2;
          }
        };
        input.setReaderMode(false);
        IntervalSelectionActivity interval(renderer, input, "SliderOrientation", StrId::STR_LINE_SPACING, 100, 50, 200,
                                           1, 5);
        interval.onEnter();
        // Theme hint drawing temporarily switches the renderer to portrait.
        renderer.setOrientation(GfxRenderer::Portrait);
        checkDirections(interval, true);
        renderer.setOrientation(orientation);
        interval.render(std::move(lock));
        if (!aware) captureStatusBarScreen(("slider-settings-" + std::to_string(orientation)).c_str());
        interval.onExit();
        // Touch percent selection uses a keypad, and touch reader sliders route their own UI events.
        if (input.hasTouchHardware()) continue;
        input.setReaderMode(true);
        EpubReaderPercentSelectionActivity percentSlider(renderer, input, 50.0f);
        percentSlider.onEnter();
        checkDirections(percentSlider);
        percentSlider.onExit();
        EpubReaderDrawerActivity drawer(renderer, input, {}, nullptr, 50.0f, 0, 0, false, false, false, false, false,
                                        false, false, false, true, true, 0, 0, 30, false);
        drawer.onEnter();
        for (const auto pane :
             {ReaderDrawerPane::Spacing, ReaderDrawerPane::Margins, ReaderDrawerPane::CharacterSpacing,
              ReaderDrawerPane::AutoPageTurn, ReaderDrawerPane::Percent}) {
          for (const uint8_t focus : {0, 1}) {
            if (focus && pane != ReaderDrawerPane::Spacing && pane != ReaderDrawerPane::Margins) continue;
            SliderOrientationSmokeTest::open(drawer, pane, focus);
            if (focus == 0 && (pane == ReaderDrawerPane::Spacing || pane == ReaderDrawerPane::Margins)) {
              const bool swap = orientation == GfxRenderer::PortraitInverted ||
                                orientation == (oppositeEdges ? GfxRenderer::LandscapeCounterClockwise
                                                              : GfxRenderer::LandscapeClockwise);
              tap(drawer, swap ? HalGPIO::BTN_UP : HalGPIO::BTN_DOWN);
              if (SliderOrientationSmokeTest::focus(drawer) != 1) fail("Slider row selection lost its Down control");
              tap(drawer, swap ? HalGPIO::BTN_DOWN : HalGPIO::BTN_UP);
              if (SliderOrientationSmokeTest::focus(drawer) != 0) fail("Slider row selection lost its Up control");
              checks += 2;
            }
            if (pane != ReaderDrawerPane::Percent && pane != ReaderDrawerPane::AutoPageTurn)
              tap(drawer, SETTINGS.frontButtonConfirm);  // Enter value editing through the real loop.
            checkDirections(drawer, pane == ReaderDrawerPane::Spacing && focus == 0);
            if (!aware && pane == ReaderDrawerPane::Spacing && focus == 0) {
              drawer.render(std::move(lock));
              captureStatusBarScreen(("slider-reader-line-height-" + std::to_string(orientation)).c_str());
            }
          }
        }
        drawer.onExit();
      }
    }
    SETTINGS.fromJson(original.as<JsonVariantConst>());
    renderer.setOrientation(savedOrientation);
    ButtonNavigator::setMappedInputManager(mappedInputManager);
    gpio.beginFrame();
    LOG_INF("SMOKE", "Verified %s side-button layout in menus and embedded reader popups",
            deviceUsesHorizontalSideButtonsForMenus(gpio) ? "opposite-edge" : "stacked");
    LOG_INF("SMOKE", "Menu orientation labels/actions and reader preferences passed (%u checks)",
            static_cast<unsigned>(checks));
  }

  static void verifyRotateShortcutPickers() {
    JsonDocument original;
    SETTINGS.toJson(original);
    SETTINGS.menuNavigation = CrossPointSettings::MENU_NAV_DIRECTIONAL;
    {
      std::fill(std::begin(SETTINGS.quickActionSlots), std::end(SETTINGS.quickActionSlots), CrossPointSettings::IGNORE);
      RenderLock lock;
      QuickActionsActivity editor(renderer, mappedInputManager);
      editor.onEnter();
      const auto press = [&](MappedInputManager::Button button) {
        mappedInputManager.simulatorInjectPress(button);
        editor.loop();
        mappedInputManager.simulatorClearInputFrame();
        mappedInputManager.simulatorInjectRelease(button);
        editor.loop();
        mappedInputManager.simulatorClearInputFrame();
        mappedInputManager.update();
      };
      const auto down = mappedInputManager.menuButton(MappedInputManager::Button::Down);
      press(down);
      press(MappedInputManager::Button::Confirm);
      bool found = false;
      for (const auto action : QuickActions::shortcutActionOrder) {
        if (!QuickActions::isQuickActionSlotActionAvailable(action)) continue;
        if (action == CrossPointSettings::ROTATE_FLIP) {
          found = true;
          break;
        }
        press(down);
      }
      if (!found) fail("Rotate Page 180 missing from Quick Action slots");
      editor.render(std::move(lock));
      captureStatusBarScreen("rotate-quick-action-picker");
      press(MappedInputManager::Button::Confirm);
      editor.render(std::move(lock));
      captureStatusBarScreen("rotate-quick-action-assigned");
      // The overview starts on Shortcut; move past all five slots to Save.
      for (int i = 0; i < 6; ++i) press(down);
      press(MappedInputManager::Button::Confirm);
      editor.onExit();
      if (SETTINGS.quickActionSlots[0] != CrossPointSettings::ROTATE_FLIP)
        fail("Quick Action picker did not save Rotate Page 180");
      JsonDocument saved;
      SETTINGS.toJson(saved);
      SETTINGS.quickActionSlots[0] = CrossPointSettings::IGNORE;
      SETTINGS.fromJson(saved.as<JsonVariantConst>());
      if (SETTINGS.quickActionSlots[0] != CrossPointSettings::ROTATE_FLIP)
        fail("Rotate Page 180 Quick Action did not survive reload");
    }
    const auto all = getSettingsList();
    const auto controls = buildControlsSettingsParentList(all);
    const auto verify = [&](const SettingInfo& setting, const uint8_t raw) {
      const auto choice = std::find(setting.enumRawValues.begin(), setting.enumRawValues.end(), raw);
      if (choice == setting.enumRawValues.end() ||
          std::count(setting.enumRawValues.begin(), setting.enumRawValues.end(), raw) != 1 ||
          sideButtonOptionLabel(setting, static_cast<uint8_t>(choice - setting.enumRawValues.begin())) !=
              tr(STR_ROTATE_FLIP))
        fail("Rotate Page 180 missing, duplicated, or mislabeled: %s", setting.key);
      SETTINGS.*setting.valuePtr = raw;
      JsonDocument saved;
      SETTINGS.toJson(saved);
      SETTINGS.*setting.valuePtr = 255;
      SETTINGS.fromJson(saved.as<JsonVariantConst>());
      if (SETTINGS.*setting.valuePtr != raw) fail("Rotate Page 180 did not reload: %s", setting.key);
    };
    // Home uses the same shared catalog on hardware with a Home key.
    const auto home = buildHomeButtonActionSetting(StrId::STR_HOME_BUTTON_TAP, &CrossPointSettings::homeButtonTapAction,
                                                   "homeButtonTapAction");
    const auto homeChoice =
        std::find(home.enumRawValues.begin(), home.enumRawValues.end(), CrossPointSettings::ROTATE_FLIP);
    if (homeChoice == home.enumRawValues.end() ||
        sideButtonOptionLabel(home, homeChoice - home.enumRawValues.begin()) != tr(STR_ROTATE_FLIP))
      fail("Home catalog is missing Rotate Page 180");
    for (const auto submenu : {SettingAction::ControlsPowerButton, SettingAction::ControlsFrontButtons,
                               SettingAction::ControlsSideButtons, SettingAction::ControlsHomeButton}) {
      const auto parent = std::find_if(controls.begin(), controls.end(),
                                       [submenu](const SettingInfo& s) { return s.action == submenu; });
      if (parent == controls.end()) continue;
      const auto rows = submenu == SettingAction::ControlsPowerButton    ? buildControlsPowerSettingsList(all)
                        : submenu == SettingAction::ControlsFrontButtons ? buildControlsFrontButtonSettingsList(all)
                        : submenu == SettingAction::ControlsHomeButton   ? buildControlsHomeButtonSettingsList(all)
                                                                         : buildControlsSideButtonSettingsList(all);
      for (size_t row = 0; row < rows.size(); ++row) {
        const auto& setting = rows[row];
        const auto trigger = QuickActions::triggerForSetting(setting.valuePtr);
        if (trigger == QuickActions::Trigger::None) continue;
        const bool chord = trigger == QuickActions::Trigger::PowerUp || trigger == QuickActions::Trigger::UpDown;
        const bool longPress = trigger == QuickActions::Trigger::LongBack || trigger == QuickActions::Trigger::LongMenu;
        const bool side =
            trigger == QuickActions::Trigger::SideUpShort || trigger == QuickActions::Trigger::SideUpLong ||
            trigger == QuickActions::Trigger::SideDownShort || trigger == QuickActions::Trigger::SideDownLong ||
            trigger == QuickActions::Trigger::FrontLeftLong || trigger == QuickActions::Trigger::FrontRightLong;
        const uint8_t raw = chord       ? CrossPointSettings::CHORD_ROTATE_FLIP
                            : longPress ? CrossPointSettings::LONG_MENU_ROTATE_FLIP
                            : side      ? CrossPointSettings::SIDE_ROTATE_FLIP
                                        : CrossPointSettings::ROTATE_FLIP;
        verify(setting, raw);
        const auto index =
            std::find(setting.enumRawValues.begin(), setting.enumRawValues.end(), raw) - setting.enumRawValues.begin();
        // Start on the preceding option, then select Rotate through the actual popup.
        SETTINGS.*setting.valuePtr = setting.enumRawValues[index - 1];
        RenderLock lock;
        SettingsActivity editor(renderer, mappedInputManager);
        editor.onEnter();
        const auto press = [&](MappedInputManager::Button button) {
          mappedInputManager.simulatorInjectRelease(button);
          editor.loop();
          mappedInputManager.simulatorClearInputFrame();
        };
        press(MappedInputManager::Button::Confirm);
        press(MappedInputManager::Button::Confirm);
        if (editor.simulatorCategoryIndex() != 2) fail("Cannot reach Controls");
        for (size_t i = 0; i <= static_cast<size_t>(parent - controls.begin()); ++i)
          press(mappedInputManager.menuButton(MappedInputManager::Button::Down));
        press(MappedInputManager::Button::Confirm);
        if (editor.simulatorActiveSubmenu() != submenu) fail("Cannot enter shortcut submenu");
        for (size_t i = 0; editor.simulatorSelectedIndex() != static_cast<int>(row + 1) && i <= rows.size(); ++i)
          press(mappedInputManager.menuButton(MappedInputManager::Button::Down));
        if (editor.simulatorSelectedIndex() != static_cast<int>(row + 1)) fail("Cannot reach shortcut row");
        editor.render(std::move(lock));
        press(MappedInputManager::Button::Confirm);
        press(mappedInputManager.menuButton(MappedInputManager::Button::Down));
        editor.render(std::move(lock));
        captureStatusBarScreen((std::string("rotate-") + setting.key).c_str());
        mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Confirm);
        editor.loop();
        mappedInputManager.simulatorClearInputFrame();
        press(MappedInputManager::Button::Confirm);
        if (SETTINGS.*setting.valuePtr != raw) fail("Picker did not select Rotate Page 180: %s", setting.key);
        editor.onExit();
        LOG_INF("SMOKE", "Rotate picker selected and captured: %s", setting.key);
      }
    }
    for (const bool textReader : {false, true}) {
      std::unique_ptr<Activity> reader;
      if (textReader)
        reader = std::make_unique<TxtReaderActivity>(renderer, mappedInputManager, nullptr, 0);
      else
        reader = std::make_unique<EpubReaderActivity>(renderer, mappedInputManager, nullptr,
                                                      EpubReaderActivity::BookReaderSettingsData{}, 0);
      if (!reader->handleShortcutAction(CrossPointSettings::QUICK_ACTIONS) || !reader->blocksGlobalInput())
        fail("Reader filtered out the configured Rotate Page 180 Quick Action");
      mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Back);
      reader->loop();
      mappedInputManager.simulatorClearInputFrame();
      if (reader->blocksGlobalInput()) fail("Could not dismiss Rotate Page 180 Quick Action popup");
      for (uint8_t orientation = 0; orientation < CrossPointSettings::ORIENTATION_COUNT; ++orientation) {
        SETTINGS.orientation = orientation;
        ReaderUtils::applyOrientation(renderer, orientation);
        // Chords use the typed overload; Home uses the raw-value overload.
        if (!reader->handleShortcutAction(CrossPointSettings::ROTATE_FLIP) ||
            SETTINGS.orientation != ReaderUtils::flippedOrientation(orientation) ||
            renderer.getOrientation() != ReaderUtils::toRendererOrientation(SETTINGS.orientation))
          fail("Typed Rotate shortcut failed");
        if (!reader->handleShortcutAction(static_cast<uint8_t>(CrossPointSettings::ROTATE_FLIP)) ||
            SETTINGS.orientation != orientation ||
            renderer.getOrientation() != ReaderUtils::toRendererOrientation(orientation))
          fail("Raw Rotate shortcut failed");
      }
    }
    SETTINGS.fromJson(original.as<JsonVariantConst>());
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    LOG_INF("SMOKE", "Simulator smoke test passed: Rotate Page 180 shortcut pickers and EPUB/TXT handlers");
  }

  static void verifyTiltPageTurnSettings() {
    JsonDocument original;
    SETTINGS.toJson(original);
    const auto all = getSettingsList();
    const auto controls = buildControlsSettingsParentList(all);
    const auto verifyRows = [](const std::vector<SettingInfo>& rows) {
      const auto direction = std::find_if(rows.begin(), rows.end(), [](const SettingInfo& setting) {
        return settingKeyIs(setting, "tiltPageTurnDirection");
      });
      const auto nextOnly = std::find_if(rows.begin(), rows.end(), [](const SettingInfo& setting) {
        return settingKeyIs(setting, "tiltNextPageOnly");
      });
      if (!QuickActions::supportsTiltPageTurn()) {
        if (direction != rows.end() || nextOnly != rows.end()) fail("Tilt settings exposed without a gyroscope");
        return;
      }
      if (direction == rows.end() || nextOnly == rows.end() || nextOnly != direction + 1 ||
          nextOnly->type != SettingType::TOGGLE || nextOnly->nameId != StrId::STR_TILT_NEXT_PAGE_ONLY ||
          nextOnly->valuePtr != &CrossPointSettings::tiltNextPageOnly ||
          std::count_if(rows.begin(), rows.end(),
                        [](const SettingInfo& setting) { return settingKeyIs(setting, "tiltNextPageOnly"); }) != 1)
        fail("Tilt Next Page Only row order, binding, or availability mismatch");
    };
    verifyRows(all);
    verifyRows(controls);
    if (!QuickActions::supportsTiltPageTurn()) {
      SETTINGS.tiltPageTurn = 1;
      SETTINGS.tiltPageTurnDirection = 2;
      SETTINGS.tiltNextPageOnly = 1;
      if (!SETTINGS.saveToFile()) fail("Cannot save tilt settings without a gyroscope");
      SETTINGS.tiltPageTurn = 0;
      SETTINGS.tiltPageTurnDirection = 0;
      SETTINGS.tiltNextPageOnly = 0;
      if (!SETTINGS.loadFromFile() || SETTINGS.tiltPageTurn != 1 || SETTINGS.tiltPageTurnDirection != 2 ||
          SETTINGS.tiltNextPageOnly != 1)
        fail("Failed IMU probe lost saved tilt settings");
      SETTINGS.fromJson(original.as<JsonVariantConst>());
      if (!SETTINGS.saveToFile()) fail("Cannot restore tilt settings after persistence check");
      LOG_INF("SMOKE", "Simulator smoke test passed: tilt settings persist and remain hidden without a gyroscope");
      return;
    }
    if (SETTINGS.tiltNextPageOnly != 0) fail("Tilt Next Page Only must default to Off");

    SETTINGS.tiltNextPageOnly = 1;
    if (!SETTINGS.saveToFile()) fail("Cannot save tilt preference");
    SETTINGS.tiltNextPageOnly = 0;
    if (!SETTINGS.loadFromFile() || SETTINGS.tiltNextPageOnly != 1) fail("Tilt preference did not reload from disk");
    JsonDocument saved;
    SETTINGS.toJson(saved);
    SETTINGS.tiltNextPageOnly = 0;
    SETTINGS.fromJson(saved.as<JsonVariantConst>());
    if (SETTINGS.tiltNextPageOnly != 1) fail("Tilt preference did not survive JSON export/import");
    saved.remove("tiltNextPageOnly");
    SETTINGS.fromJson(saved.as<JsonVariantConst>());
    if (SETTINGS.tiltNextPageOnly != 0) fail("Old settings export must restore bidirectional tilt");
    saved["tiltNextPageOnly"] = 255;
    SETTINGS.fromJson(saved.as<JsonVariantConst>());
    if (SETTINGS.tiltNextPageOnly != 0) fail("Invalid tilt preference did not fall back to Off");

    // Activate the actual Controls row, then reload its saved value.
    SETTINGS.menuNavigation = CrossPointSettings::MENU_NAV_DIRECTIONAL;
    SETTINGS.tiltPageTurn = 1;
    RenderLock lock;
    SettingsActivity editor(renderer, mappedInputManager);
    editor.onEnter();
    const auto press = [&](MappedInputManager::Button button) {
      mappedInputManager.simulatorInjectRelease(button);
      editor.loop();
      mappedInputManager.simulatorClearInputFrame();
    };
    press(MappedInputManager::Button::Confirm);
    press(MappedInputManager::Button::Confirm);
    if (editor.simulatorCategoryIndex() != 2) fail("Cannot reach Controls for tilt preference");
    const auto row = std::find_if(controls.begin(), controls.end(),
                                  [](const SettingInfo& setting) { return settingKeyIs(setting, "tiltNextPageOnly"); });
    for (size_t i = 0; i <= static_cast<size_t>(row - controls.begin()); ++i)
      press(mappedInputManager.menuButton(MappedInputManager::Button::Down));
    editor.render(std::move(lock));
    captureStatusBarScreen("tilt-next-page-only-off");
    press(MappedInputManager::Button::Confirm);
    if (SETTINGS.tiltNextPageOnly != 1) fail("Controls row did not enable Next Page Only");
    editor.render(std::move(lock));
    captureStatusBarScreen("tilt-next-page-only-on");
    SETTINGS.tiltNextPageOnly = 0;
    if (!SETTINGS.loadFromFile() || SETTINGS.tiltNextPageOnly != 1)
      fail("Controls row did not persist tilt preference");
    editor.onExit();
    SETTINGS.fromJson(original.as<JsonVariantConst>());
    if (!SETTINGS.saveToFile()) fail("Cannot restore tilt test settings");
    LOG_INF("SMOKE", "Simulator smoke test passed: tilt settings order, device toggle, disk and JSON persistence");
  }

  static void verifyReaderControlsSettings() {
    const auto all = getSettingsList();
    const auto controls = buildControlsSettingsParentList(all);
    if (!deviceHasFrontButtons()) {
      if (hasSettingByName(all, StrId::STR_MENU_NAVIGATION) || hasSettingByName(controls, StrId::STR_MENU_NAVIGATION))
        fail("Menu navigation must be hidden on devices without front buttons");
    } else if (controls.empty() || controls.back().valuePtr != &CrossPointSettings::menuNavigation) {
      fail("Menu navigation must be the last shared Controls setting on front-button devices");
    }
    JsonDocument navigationOriginal;
    SETTINGS.toJson(navigationOriginal);
    SETTINGS.menuNavigation = CrossPointSettings::MENU_NAV_DIRECTIONAL;
    JsonDocument legacyNavigation;
    SETTINGS.fromJson(legacyNavigation.as<JsonVariantConst>());
    if (SETTINGS.menuNavigation != CrossPointSettings::MENU_NAV_DIRECTIONAL)
      fail("Missing menu navigation must preserve the Directional default");
    for (const uint8_t mode : {CrossPointSettings::MENU_NAV_DIRECTIONAL, CrossPointSettings::MENU_NAV_CLASSIC}) {
      SETTINGS.menuNavigation = mode;
      JsonDocument saved;
      SETTINGS.toJson(saved);
      if (saved["menuNavigation"].as<uint8_t>() != mode) fail("Menu navigation missing from settings JSON");
      SETTINGS.menuNavigation = 255;
      SETTINGS.fromJson(saved.as<JsonVariantConst>());
      if (SETTINGS.menuNavigation != mode) fail("Menu navigation JSON round trip failed");
    }
    SETTINGS.menuNavigation = CrossPointSettings::MENU_NAV_DIRECTIONAL;
    legacyNavigation["menuNavigation"] = 255;
    SETTINGS.fromJson(legacyNavigation.as<JsonVariantConst>());
    if (SETTINGS.menuNavigation != CrossPointSettings::MENU_NAV_DIRECTIONAL)
      fail("Invalid menu navigation must preserve the default");
    SETTINGS.fromJson(navigationOriginal.as<JsonVariantConst>());
    const auto gestures = buildControlsTapsGesturesSettingsList(all);
    if (gpio.hasTouch()) {
      if (hasSettingByName(all, StrId::STR_TOUCH_READER_CONTROLS)) fail("Retired touch setting still visible");
      if (gestures.size() < 3 || gestures[0].nameId != StrId::STR_NEXT_PAGE ||
          gestures[1].nameId != StrId::STR_PREV_PAGE)
        fail("Page gesture settings missing");
      JsonDocument beforeEditor;
      SETTINGS.toJson(beforeEditor);
      SETTINGS.disablePageTurnGestures = 1;
      RenderLock editorLock;
      SettingsActivity editor(renderer, mappedInputManager);
      editor.onEnter();
      const auto press = [&](MappedInputManager::Button button) {
        mappedInputManager.simulatorInjectRelease(button);
        editor.loop();
        mappedInputManager.simulatorClearInputFrame();
      };
      press(MappedInputManager::Button::Confirm);
      press(MappedInputManager::Button::Confirm);
      if (editor.simulatorCategoryIndex() != 2) fail("Cannot reach Controls settings");
      const auto gestureMenu = std::find_if(controls.begin(), controls.end(), [](const SettingInfo& setting) {
        return setting.action == SettingAction::ControlsTapsGestures;
      });
      if (gestureMenu == controls.end()) fail("Taps & Gestures submenu missing");
      for (size_t i = 0; i <= static_cast<size_t>(gestureMenu - controls.begin()); ++i) {
        press(mappedInputManager.menuButton(MappedInputManager::Button::Down));
      }
      press(MappedInputManager::Button::Confirm);
      if (editor.simulatorActiveSubmenu() != SettingAction::ControlsTapsGestures)
        fail("Cannot open Taps & Gestures settings");
      editor.render(std::move(editorLock));
      captureStatusBarScreen("page-gestures-disabled");
      press(MappedInputManager::Button::Confirm);
      if (editor.simulatorOptionPopupActive()) fail("Disabled page gesture opened picker");
      SETTINGS.disablePageTurnGestures = 0;
      editor.render(std::move(editorLock));
      captureStatusBarScreen("taps-and-gestures");
      press(MappedInputManager::Button::Confirm);
      if (!editor.simulatorOptionPopupActive()) fail("Enabled page gesture picker unavailable");
      press(MappedInputManager::Button::Back);
      editor.simulatorOpenFrontButtons();
      editor.render(std::move(editorLock));
      captureStatusBarScreen("front-button-shortcuts");
      editor.onExit();
      SETTINGS.fromJson(beforeEditor.as<JsonVariantConst>());
    } else if (!gestures.empty()) {
      fail("Touch gestures exposed on a button-only device");
    }
    LOG_INF("SMOKE", "Reader touch device settings visibility and activation passed");
    const auto& base = getBaseSettingsList();
    const size_t expectedBaseSettingsCapacity = getBaseSettingsCapacity();
    if (base.size() != expectedBaseSettingsCapacity || base.capacity() < expectedBaseSettingsCapacity) {
      fail("Base settings allocation mismatch: size=%zu expected=%zu capacity=%zu", base.size(),
           expectedBaseSettingsCapacity, base.capacity());
    }
    const auto device = buildSystemDeviceSettingsList(all);
    if (device.size() < 3 || device[1].nameId != StrId::STR_TIME_TO_SLEEP ||
        device[2].nameId != StrId::STR_CUSTOM_BOOTSCREEN) {
      fail("Custom bootscreen setting order mismatch");
    }
    JsonDocument original;
    SETTINGS.toJson(original);
    for (const bool importingCrossPoint : {false, true}) {
      for (uint8_t spacing = 0; spacing <= CrossPointSettings::MAX_WORD_SPACING; ++spacing) {
        JsonDocument savedSpacing;
        savedSpacing["wordSpacing"] = spacing;
        SETTINGS.fromJson(savedSpacing.as<JsonVariantConst>(), importingCrossPoint);
        if (SETTINGS.wordSpacing != spacing) fail("CrossInk word spacing changed during settings load");
      }
    }
    JsonDocument crossPointSpacing;
    crossPointSpacing["wordSpacing"] = 100;
    SETTINGS.fromJson(crossPointSpacing.as<JsonVariantConst>(), true);
    if (SETTINGS.wordSpacing != 0) fail("CrossPoint normal word spacing did not migrate to level 0");
    JsonDocument migratedSpacing;
    SETTINGS.toJson(migratedSpacing);
    SETTINGS.wordSpacing = 4;
    SETTINGS.fromJson(migratedSpacing.as<JsonVariantConst>());
    if (SETTINGS.wordSpacing != 0) fail("Migrated word spacing did not survive reload");
    SETTINGS.fromJson(original.as<JsonVariantConst>());
    for (uint8_t mode = 0; mode <= CrossPointSettings::PAGE_TURN_GESTURE_DISABLED; ++mode) {
      JsonDocument legacy;
      legacy["pageTurnGesture"] = mode;
      SETTINGS.fromJson(legacy.as<JsonVariantConst>());
      if (SETTINGS.pageTurnGesture != mode || SETTINGS.previousPageGesture != mode) {
        fail("Legacy page gesture migration mismatch");
      }
    }
    constexpr uint8_t importedGestures[] = {CrossPointSettings::TAP_AND_SWIPE, CrossPointSettings::TAP_ONLY,
                                            CrossPointSettings::SWIPE_ONLY, CrossPointSettings::INVERTED_TAP};
    for (uint8_t mode = 0; mode < 4; ++mode) {
      JsonDocument crosspoint;
      crosspoint["touchReaderControls"] = mode;
      crosspoint["disableReaderTouchscreen"] = 1;
      SETTINGS.fromJson(crosspoint.as<JsonVariantConst>(), true);
      if (SETTINGS.disableReaderTouchscreen || SETTINGS.disablePageTurnGestures ||
          SETTINGS.pageTurnGesture != importedGestures[mode] ||
          SETTINGS.previousPageGesture != importedGestures[mode]) {
        fail("CrossPoint touch settings migration mismatch");
      }
      JsonDocument migrated;
      SETTINGS.toJson(migrated);
      SETTINGS.disableReaderTouchscreen = 1;
      SETTINGS.pageTurnGesture = CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
      SETTINGS.previousPageGesture = CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
      SETTINGS.fromJson(migrated.as<JsonVariantConst>());
      if (SETTINGS.disableReaderTouchscreen || SETTINGS.disablePageTurnGestures ||
          SETTINGS.pageTurnGesture != importedGestures[mode] ||
          SETTINGS.previousPageGesture != importedGestures[mode]) {
        fail("Migrated CrossPoint touch settings did not survive reload");
      }
    }
    // The namespaced file must preserve intentional locks, including older files without gesture keys.
    JsonDocument locked;
    locked["touchReaderControls"] = 1;
    locked["disableReaderTouchscreen"] = 1;
    SETTINGS.fromJson(locked.as<JsonVariantConst>());
    if (!SETTINGS.disableReaderTouchscreen) fail("CrossInk touch lock was lost");
    locked["pageTurnGesture"] = CrossPointSettings::TAP_ONLY;
    locked["previousPageGesture"] = CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
    SETTINGS.fromJson(locked.as<JsonVariantConst>(), true);
    if (!SETTINGS.disableReaderTouchscreen || SETTINGS.pageTurnGesture != CrossPointSettings::TAP_ONLY ||
        SETTINGS.previousPageGesture != CrossPointSettings::PAGE_TURN_GESTURE_DISABLED) {
      fail("Legacy CrossInk gesture settings were treated as CrossPoint");
    }
    SETTINGS.previousPageGesture = CrossPointSettings::SWIPE_ONLY;
    SETTINGS.pageTurnGesture = CrossPointSettings::TAP_ONLY;
    SETTINGS.customBootscreenEnabled = 0;
    SETTINGS.tapToHideStatusBar = 0;
    JsonDocument saved;
    SETTINGS.toJson(saved);
    SETTINGS.previousPageGesture = CrossPointSettings::TAP_AND_SWIPE;
    SETTINGS.customBootscreenEnabled = 1;
    SETTINGS.tapToHideStatusBar = 1;
    SETTINGS.fromJson(saved.as<JsonVariantConst>());
    if (SETTINGS.previousPageGesture != CrossPointSettings::SWIPE_ONLY ||
        SETTINGS.pageTurnGesture != CrossPointSettings::TAP_ONLY || SETTINGS.customBootscreenEnabled ||
        SETTINGS.tapToHideStatusBar) {
      fail("Reader controls settings round-trip mismatch");
    }
    constexpr char CROSSINK_SETTINGS_FILE_BAK[] = "/.crosspoint/crossink-settings.json.bak";
    constexpr char LEGACY_SETTINGS_FILE_JSON[] = "/.crosspoint/settings.json";
    const char* const crossInkSettingsPath = CrossPointSettings::getFilePath();
    const bool hadCrossInkSettings = Storage.exists(crossInkSettingsPath);
    const String savedCrossInkSettings = hadCrossInkSettings ? Storage.readFile(crossInkSettingsPath) : String();
    const bool hadCrossInkSettingsBackup = Storage.exists(CROSSINK_SETTINGS_FILE_BAK);
    const String savedCrossInkSettingsBackup =
        hadCrossInkSettingsBackup ? Storage.readFile(CROSSINK_SETTINGS_FILE_BAK) : String();
    const bool hadLegacySettings = Storage.exists(LEGACY_SETTINGS_FILE_JSON);
    const String savedLegacySettings = hadLegacySettings ? Storage.readFile(LEGACY_SETTINGS_FILE_JSON) : String();

    JsonDocument crossInkSettings;
    crossInkSettings["touchReaderControls"] = 1;
    crossInkSettings["pageTurnGesture"] = CrossPointSettings::TAP_ONLY;
    crossInkSettings["previousPageGesture"] = CrossPointSettings::SWIPE_ONLY;
    crossInkSettings["disableReaderTouchscreen"] = 0;
    crossInkSettings["wordSpacing"] = 4;
    String crossInkJson;
    serializeJson(crossInkSettings, crossInkJson);

    JsonDocument crossPointSettings;
    crossPointSettings["touchReaderControls"] = 2;
    crossPointSettings["disableReaderTouchscreen"] = 1;
    crossPointSettings["wordSpacing"] = 100;
    String crossPointJson;
    serializeJson(crossPointSettings, crossPointJson);

    if (!Storage.writeFile(crossInkSettingsPath, crossInkJson) ||
        !Storage.writeFile(LEGACY_SETTINGS_FILE_JSON, crossPointJson)) {
      fail("Could not write settings migration test fixture");
    }
    SETTINGS.disableReaderTouchscreen = 1;
    SETTINGS.pageTurnGesture = CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
    SETTINGS.previousPageGesture = CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
    if (!SETTINGS.loadFromFile() || SETTINGS.disableReaderTouchscreen ||
        SETTINGS.pageTurnGesture != CrossPointSettings::TAP_ONLY ||
        SETTINGS.previousPageGesture != CrossPointSettings::SWIPE_ONLY || SETTINGS.wordSpacing != 4) {
      fail("CrossInk settings file did not take precedence over CrossPoint settings");
    }

    // A corrupt CrossInk file still blocks the foreign fallback. It is safer
    // to leave settings unchanged than to silently import CrossPoint values.
    if (!Storage.writeFile(crossInkSettingsPath, "{")) fail("Could not corrupt CrossInk settings test fixture");
    if (SETTINGS.loadFromFile() || SETTINGS.disableReaderTouchscreen ||
        SETTINGS.pageTurnGesture != CrossPointSettings::TAP_ONLY ||
        SETTINGS.previousPageGesture != CrossPointSettings::SWIPE_ONLY) {
      fail("Corrupt CrossInk settings fell through to CrossPoint settings");
    }

    // An interrupted atomic replacement leaves the CrossInk backup as the
    // sole namespaced file. Recover it before considering CrossPoint's file.
    if (!Storage.writeFile(CROSSINK_SETTINGS_FILE_BAK, crossInkJson) || !Storage.remove(crossInkSettingsPath)) {
      fail("Could not create interrupted CrossInk settings fixture");
    }
    SETTINGS.disableReaderTouchscreen = 1;
    SETTINGS.pageTurnGesture = CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
    SETTINGS.previousPageGesture = CrossPointSettings::PAGE_TURN_GESTURE_DISABLED;
    if (!SETTINGS.loadFromFile() || SETTINGS.disableReaderTouchscreen ||
        SETTINGS.pageTurnGesture != CrossPointSettings::TAP_ONLY ||
        SETTINGS.previousPageGesture != CrossPointSettings::SWIPE_ONLY || !Storage.exists(crossInkSettingsPath) ||
        Storage.exists(CROSSINK_SETTINGS_FILE_BAK) || SETTINGS.wordSpacing != 4) {
      fail("Interrupted CrossInk settings save did not recover before CrossPoint import");
    }

    if (!Storage.remove(crossInkSettingsPath)) fail("Could not remove CrossInk settings migration fixture");
    if (!SETTINGS.loadFromFile() || SETTINGS.wordSpacing != 0 || !Storage.exists(crossInkSettingsPath)) {
      fail("CrossPoint word spacing file migration failed");
    }
    SETTINGS.wordSpacing = 4;
    if (!SETTINGS.loadFromFile() || SETTINGS.wordSpacing != 0) {
      fail("Migrated word spacing was not saved to CrossInk settings");
    }
    LOG_INF("SMOKE", "CrossPoint word spacing migration and CrossInk spacing preservation passed");

    if (hadCrossInkSettings) {
      if (!Storage.writeFile(crossInkSettingsPath, savedCrossInkSettings)) fail("Could not restore CrossInk settings");
    } else if (Storage.exists(crossInkSettingsPath) && !Storage.remove(crossInkSettingsPath)) {
      fail("Could not remove CrossInk settings test fixture");
    }
    if (hadCrossInkSettingsBackup) {
      if (!Storage.writeFile(CROSSINK_SETTINGS_FILE_BAK, savedCrossInkSettingsBackup)) {
        fail("Could not restore CrossInk settings backup");
      }
    } else if (Storage.exists(CROSSINK_SETTINGS_FILE_BAK) && !Storage.remove(CROSSINK_SETTINGS_FILE_BAK)) {
      fail("Could not remove CrossInk settings backup fixture");
    }
    if (hadLegacySettings) {
      if (!Storage.writeFile(LEGACY_SETTINGS_FILE_JSON, savedLegacySettings)) fail("Could not restore legacy settings");
    } else if (Storage.exists(LEGACY_SETTINGS_FILE_JSON) && !Storage.remove(LEGACY_SETTINGS_FILE_JSON)) {
      fail("Could not remove legacy settings test fixture");
    }

    SETTINGS.librarySortMethod = 3;
    SETTINGS.librarySortDescending = 0;
    SETTINGS.libraryListExpanded = 1;
    SETTINGS.recentBooksView = CrossPointSettings::RECENT_BOOKS_GRID;
    SETTINGS.libraryShowMarkdown = 0;
    SETTINGS.libraryHideFinishedBooks = 1;
    JsonDocument librarySaved;
    SETTINGS.toJson(librarySaved);
    SETTINGS.librarySortMethod = 0;
    SETTINGS.librarySortDescending = 1;
    SETTINGS.libraryListExpanded = 0;
    SETTINGS.recentBooksView = CrossPointSettings::RECENT_BOOKS_LIST;
    SETTINGS.libraryShowMarkdown = 1;
    SETTINGS.libraryHideFinishedBooks = 0;
    SETTINGS.fromJson(librarySaved.as<JsonVariantConst>());
    if (SETTINGS.librarySortMethod != 3 || SETTINGS.librarySortDescending || !SETTINGS.libraryListExpanded ||
        SETTINGS.recentBooksView != CrossPointSettings::RECENT_BOOKS_GRID || SETTINGS.libraryShowMarkdown ||
        !SETTINGS.libraryHideFinishedBooks) {
      fail("Library settings round-trip mismatch");
    }
    librarySaved["librarySortMethod"] = 99;
    librarySaved["libraryShowTxt"] = 2;
    librarySaved["libraryHideFinishedBooks"] = 2;
    librarySaved["recentBooksView"] = 2;
    SETTINGS.fromJson(librarySaved.as<JsonVariantConst>());
    if (SETTINGS.librarySortMethod != 3 || SETTINGS.libraryShowTxt != 1 || !SETTINGS.libraryHideFinishedBooks ||
        SETTINGS.recentBooksView != CrossPointSettings::RECENT_BOOKS_LIST) {
      fail("Invalid Library settings were not rejected");
    }
    librarySaved.remove("recentBooksView");
    SETTINGS.recentBooksView = CrossPointSettings::RECENT_BOOKS_GRID;
    SETTINGS.fromJson(librarySaved.as<JsonVariantConst>());
    if (SETTINGS.recentBooksView != CrossPointSettings::RECENT_BOOKS_LIST)
      fail("Missing Recently Opened view did not default to List");
    SETTINGS.fromJson(original.as<JsonVariantConst>());
  }

  static void verifyUpDownShortcutAvailability() {
    const auto allSettings = getSettingsList();
    const auto sideButtonSettings = buildControlsSideButtonSettingsList(allSettings);
    const bool hasSideButtonChord =
        std::any_of(sideButtonSettings.begin(), sideButtonSettings.end(),
                    [](const SettingInfo& setting) { return setting.nameId == StrId::STR_SIDE_BUTTON_CHORD; });
    if (hasSideButtonChord != deviceSupportsSideButtonChord(gpio)) {
      fail("Side-button chord availability does not match device controls");
    }

    if (QuickActionsActivityTest::isTriggerAvailable(QuickActions::Trigger::UpDown) !=
        deviceSupportsSideButtonChord(gpio)) {
      fail("Quick Actions Up + Down availability does not match device controls");
    }

    const auto chordSetting = std::find_if(allSettings.begin(), allSettings.end(), [](const SettingInfo& setting) {
      return settingKeyIs(setting, "powerChordAction");
    });
    if (chordSetting == allSettings.end()) fail("Power chord setting is missing");
    if (std::find(chordSetting->enumRawValues.begin(), chordSetting->enumRawValues.end(),
                  CrossPointSettings::CHORD_QUICK_ACTIONS) == chordSetting->enumRawValues.end()) {
      fail("Quick Actions is missing from the Power + Up chord setting");
    }
    if (CrossPointSettings::HOME_READER != 36 || CrossPointSettings::SELECT_CHAPTER != 38 ||
        CrossPointSettings::SHORT_PWRBTN_COUNT != 41 || CrossPointSettings::CHORD_HOME_READER != 32 ||
        CrossPointSettings::CHORD_SELECT_CHAPTER != 34 || CrossPointSettings::POWER_CHORD_ACTION_COUNT != 37) {
      fail("Home/Reader changed persisted shortcut IDs or counts");
    }
    if (QuickActions::actionLabel(CrossPointSettings::HOME_READER) != StrId::STR_HOME_READER) {
      fail("Home/Reader shortcut label mismatch");
    }
    if (!QuickActions::isActionAvailable(CrossPointSettings::HOME_READER)) {
      fail("Home/Reader is unavailable on this device");
    }
    const auto containsShortcut = [](const std::vector<SettingInfo>& settings, const char* key,
                                     const ShortcutOptionCatalog catalog,
                                     const CrossPointSettings::SHORT_PWRBTN action = CrossPointSettings::HOME_READER) {
      const auto setting = std::find_if(settings.begin(), settings.end(),
                                        [key](const SettingInfo& candidate) { return settingKeyIs(candidate, key); });
      if (setting == settings.end()) return false;
      const uint8_t raw = shortcutRawValue(catalog, action);
      const auto choice = std::find(setting->enumRawValues.begin(), setting->enumRawValues.end(), raw);
      return choice != setting->enumRawValues.end() &&
             setting->enumValues[static_cast<size_t>(choice - setting->enumRawValues.begin())] ==
                 QuickActions::actionLabel(action) &&
             std::count(setting->enumRawValues.begin(), setting->enumRawValues.end(), raw) == 1;
    };
    if (!containsShortcut(allSettings, "shortPwrBtn", ShortcutOptionCatalog::PowerButton) ||
        !containsShortcut(sideButtonSettings, "sideButtonUpShort", ShortcutOptionCatalog::SideButton) ||
        !containsShortcut(allSettings, "powerChordAction", ShortcutOptionCatalog::ButtonChord)) {
      fail("Home/Reader shortcut availability or settings mapping mismatch");
    }
    for (const auto action : {CrossPointSettings::BACK_HOME, CrossPointSettings::HOME_READER}) {
      if (!QuickActions::isQuickActionSlotActionAvailable(action)) fail("Navigation missing from Quick Actions");
      for (const char* key : {"shortPwrBtn", "longPwrBtn"}) {
        if (!containsShortcut(allSettings, key, ShortcutOptionCatalog::PowerButton, action))
          fail("Navigation missing from power shortcut %s", key);
      }
      for (const char* key : {"longPressMenuAction", "longPressBackAction"}) {
        if (!containsShortcut(allSettings, key, ShortcutOptionCatalog::LongPress, action))
          fail("Navigation missing from long-press shortcut %s", key);
      }
      if (!containsShortcut(allSettings, "powerChordAction", ShortcutOptionCatalog::ButtonChord, action))
        fail("Navigation missing from power chord");
      if (deviceSupportsSideButtonChord(gpio) &&
          !containsShortcut(allSettings, "sideButtonChordAction", ShortcutOptionCatalog::ButtonChord, action))
        fail("Navigation missing from side chord");
      for (const char* key : {"sideButtonUpShort", "sideButtonUpLong", "sideButtonDownShort", "sideButtonDownLong"}) {
        if (!containsShortcut(sideButtonSettings, key, ShortcutOptionCatalog::SideButton, action))
          fail("Navigation missing from side shortcut %s", key);
      }
      if (gpio.hasHomeKey()) {
        for (const char* key : {"homeButtonTapAction", "homeButtonDoubleTapAction", "homeButtonLongPressAction"}) {
          if (!containsShortcut(allSettings, key, ShortcutOptionCatalog::HomeButton, action))
            fail("Navigation missing or duplicated in Home shortcut %s", key);
        }
      }
    }
    if (CrossPointSettings::BACK_HOME != 37 || CrossPointSettings::CHORD_BACK_HOME != 33 ||
        CrossPointSettings::HOME_BUTTON_BACK_HOME != 23 || CrossPointSettings::LONG_MENU_HOME_READER != 25 ||
        CrossPointSettings::LONG_MENU_BACK_HOME != 26)
      fail("Navigation changed persisted IDs");
    JsonDocument savedNavigationSettings;
    SETTINGS.toJson(savedNavigationSettings);
    for (const auto action : {CrossPointSettings::BACK_HOME, CrossPointSettings::HOME_READER}) {
      const auto chord = shortcutRawValue(ShortcutOptionCatalog::ButtonChord, action);
      const auto longPress = shortcutRawValue(ShortcutOptionCatalog::LongPress, action);
      SETTINGS.shortPwrBtn = action;
      SETTINGS.longPwrBtn = action;
      SETTINGS.powerChordAction = chord;
      SETTINGS.sideButtonUpLong = action;
      SETTINGS.longPressMenuAction = longPress;
      SETTINGS.longPressBackAction = longPress;
      SETTINGS.quickActionSlots[0] = action;
      JsonDocument roundTrip;
      SETTINGS.toJson(roundTrip);
      SETTINGS.fromJson(savedNavigationSettings.as<JsonVariantConst>());
      SETTINGS.fromJson(roundTrip.as<JsonVariantConst>());
      if (SETTINGS.shortPwrBtn != action || SETTINGS.longPwrBtn != action || SETTINGS.powerChordAction != chord ||
          SETTINGS.sideButtonUpLong != action || SETTINGS.longPressMenuAction != longPress ||
          SETTINGS.longPressBackAction != longPress || SETTINGS.quickActionSlots[0] != action)
        fail("Navigation assignments did not survive settings reload");
    }
    SETTINGS.fromJson(savedNavigationSettings.as<JsonVariantConst>());
    for (const auto action :
         {CrossPointSettings::TWO_FINGER_SWIPE_BACK_HOME, CrossPointSettings::TWO_FINGER_SWIPE_HOME_READER}) {
      if (!CrossPointSettings::isTwoFingerSwipeActionAvailable(action, false, false))
        fail("Navigation swipe unexpectedly requires a frontlight");
      if (gpio.hasTouch()) {
        SETTINGS.leftEdgeUp = action;
        if (gpio.supportsMultiTouch()) SETTINGS.twoFingerSwipeUp = action;
        JsonDocument swipeRoundTrip;
        SETTINGS.toJson(swipeRoundTrip);
        SETTINGS.fromJson(savedNavigationSettings.as<JsonVariantConst>());
        SETTINGS.fromJson(swipeRoundTrip.as<JsonVariantConst>());
        if (SETTINGS.leftEdgeUp != action || (gpio.supportsMultiTouch() && SETTINGS.twoFingerSwipeUp != action))
          fail("Navigation swipe did not survive settings reload");
      }
    }
    SETTINGS.fromJson(savedNavigationSettings.as<JsonVariantConst>());
    if (gpio.hasHomeKey()) {
      const auto homeSettings = buildControlsHomeButtonSettingsList(allSettings);
      for (const char* key : {"homeButtonTapAction", "homeButtonDoubleTapAction", "homeButtonLongPressAction"}) {
        if (!containsShortcut(homeSettings, key, ShortcutOptionCatalog::HomeButton)) {
          fail("Home/Reader is missing from a Home-button shortcut picker");
        }
      }
      for (auto field : {&CrossPointSettings::homeButtonTapAction, &CrossPointSettings::homeButtonDoubleTapAction,
                         &CrossPointSettings::homeButtonLongPressAction}) {
        const uint8_t saved = SETTINGS.*field;
        SETTINGS.*field = CrossPointSettings::HOME_READER;
        JsonDocument homeRoundTrip;
        SETTINGS.toJson(homeRoundTrip);
        SETTINGS.*field = CrossPointSettings::IGNORE;
        SETTINGS.fromJson(homeRoundTrip.as<JsonVariantConst>());
        if (SETTINGS.*field != CrossPointSettings::HOME_READER) {
          fail("Home/Reader Home-button assignment did not survive settings reload");
        }
        SETTINGS.*field = saved;
      }
    }
    const uint8_t savedPowerAction = SETTINGS.shortPwrBtn;
    const uint8_t savedChordAction = SETTINGS.powerChordAction;
    const uint8_t savedQuickActionSlot = SETTINGS.quickActionSlots[0];
    SETTINGS.shortPwrBtn = CrossPointSettings::HOME_READER;
    SETTINGS.powerChordAction = CrossPointSettings::CHORD_HOME_READER;
    SETTINGS.quickActionSlots[0] = CrossPointSettings::HOME_READER;
    JsonDocument shortcutRoundTrip;
    SETTINGS.toJson(shortcutRoundTrip);
    SETTINGS.shortPwrBtn = CrossPointSettings::IGNORE;
    SETTINGS.powerChordAction = CrossPointSettings::CHORD_DISABLED;
    SETTINGS.quickActionSlots[0] = CrossPointSettings::IGNORE;
    SETTINGS.fromJson(shortcutRoundTrip.as<JsonVariantConst>());
    if (SETTINGS.shortPwrBtn != CrossPointSettings::HOME_READER ||
        SETTINGS.powerChordAction != CrossPointSettings::CHORD_HOME_READER ||
        SETTINGS.quickActionSlots[0] != CrossPointSettings::HOME_READER) {
      fail("Home/Reader settings round-trip did not match device availability");
    }
    SETTINGS.shortPwrBtn = savedPowerAction;
    SETTINGS.powerChordAction = savedChordAction;
    SETTINGS.quickActionSlots[0] = savedQuickActionSlot;
    if (!gpio.hasHomeKey() &&
        std::find(chordSetting->enumRawValues.begin(), chordSetting->enumRawValues.end(),
                  CrossPointSettings::CHORD_TOGGLE_HOME_BUTTON) != chordSetting->enumRawValues.end()) {
      fail("Toggle Home Button is still offered without a Home key");
    }
    if (std::find(chordSetting->enumRawValues.begin(), chordSetting->enumRawValues.end(),
                  CrossPointSettings::CHORD_PREVIOUS_PAGE) == chordSetting->enumRawValues.end()) {
      fail("Previous Page was removed by an unrelated power-button action ID");
    }
    if (!gpio.hasTouch() &&
        std::find(chordSetting->enumRawValues.begin(), chordSetting->enumRawValues.end(),
                  CrossPointSettings::CHORD_TOGGLE_TOUCHSCREEN) != chordSetting->enumRawValues.end()) {
      fail("Toggle Touchscreen is still offered without touch hardware");
    }
    if (!Frontlight.present() &&
        std::find(chordSetting->enumRawValues.begin(), chordSetting->enumRawValues.end(),
                  CrossPointSettings::CHORD_TOGGLE_FRONTLIGHT) != chordSetting->enumRawValues.end()) {
      fail("Toggle Frontlight is still offered without a frontlight");
    }

    const auto hasLibrary = [](const SettingInfo& setting, const ShortcutOptionCatalog catalog) {
      const auto raw = shortcutRawValue(catalog, CrossPointSettings::LIBRARY);
      const auto choice = std::find(setting.enumRawValues.begin(), setting.enumRawValues.end(), raw);
      return choice != setting.enumRawValues.end() &&
             setting.enumValues[static_cast<size_t>(choice - setting.enumRawValues.begin())] == StrId::STR_LIBRARY;
    };
    const auto verifyLibraryChoice = [&](const char* key, const ShortcutOptionCatalog catalog) {
      const auto setting = std::find_if(allSettings.begin(), allSettings.end(),
                                        [key](const SettingInfo& candidate) { return settingKeyIs(candidate, key); });
      if (setting == allSettings.end() || !hasLibrary(*setting, catalog)) {
        fail("Library shortcut is missing or mislabeled in %s", key);
      }
    };
    verifyLibraryChoice("shortPwrBtn", ShortcutOptionCatalog::PowerButton);
    verifyLibraryChoice("longPwrBtn", ShortcutOptionCatalog::PowerButton);
    for (const char* key : {"sideButtonUpShort", "sideButtonUpLong", "sideButtonDownShort", "sideButtonDownLong"}) {
      verifyLibraryChoice(key, ShortcutOptionCatalog::SideButton);
      const auto setting = std::find_if(allSettings.begin(), allSettings.end(),
                                        [key](const SettingInfo& candidate) { return settingKeyIs(candidate, key); });
      for (const auto [action, suffix] :
           {std::pair{CrossPointSettings::SIDE_ROTATE_COUNTERCLOCKWISE, StrId::STR_ROTATE_CCW},
            std::pair{CrossPointSettings::SIDE_ROTATE_CLOCKWISE, StrId::STR_ROTATE_CW},
            std::pair{CrossPointSettings::SIDE_ROTATE_FLIP, StrId::STR_ROTATE_FLIP}}) {
        const auto choice = std::find(setting->enumRawValues.begin(), setting->enumRawValues.end(), action);
        const std::string expected = I18N.get(suffix);
        if (choice == setting->enumRawValues.end() ||
            sideButtonOptionLabel(*setting, static_cast<uint8_t>(choice - setting->enumRawValues.begin())) !=
                expected) {
          fail("Orientation shortcut is missing or mislabeled in %s", key);
        }
      }
    }
    for (uint8_t orientation = 0; orientation < CrossPointSettings::ORIENTATION_COUNT; ++orientation) {
      if (ReaderUtils::flippedOrientation(ReaderUtils::flippedOrientation(orientation)) != orientation ||
          ReaderUtils::flippedOrientation(orientation) !=
              ReaderUtils::rotatedOrientation(ReaderUtils::rotatedOrientation(orientation, true), true)) {
        fail("Flip must turn the screen 180 degrees from every orientation");
      }
    }
    verifyLibraryChoice("powerChordAction", ShortcutOptionCatalog::ButtonChord);
    verifyLibraryChoice("longPressMenuAction", ShortcutOptionCatalog::LongPress);
    verifyLibraryChoice("longPressBackAction", ShortcutOptionCatalog::LongPress);
    const auto verifySleepChoice = [&](const char* key, const ShortcutOptionCatalog catalog) {
      const auto setting = std::find_if(allSettings.begin(), allSettings.end(),
                                        [key](const SettingInfo& candidate) { return settingKeyIs(candidate, key); });
      const auto raw = shortcutRawValue(catalog, CrossPointSettings::SLEEP);
      if (setting == allSettings.end() || raw == SHORTCUT_OPTION_UNAVAILABLE) {
        fail("Sleep shortcut is missing from %s", key);
      }
      const auto choice = std::find(setting->enumRawValues.begin(), setting->enumRawValues.end(), raw);
      const std::string expected = catalog == ShortcutOptionCatalog::PowerButton
                                       ? std::string(tr(STR_SLEEP)) + "/" + tr(STR_WAKE)
                                       : tr(STR_SLEEP);
      if (choice == setting->enumRawValues.end() ||
          sideButtonOptionLabel(*setting, static_cast<uint8_t>(choice - setting->enumRawValues.begin())) != expected) {
        fail("Sleep shortcut has the wrong label in %s", key);
      }
    };
    verifySleepChoice("shortPwrBtn", ShortcutOptionCatalog::PowerButton);
    verifySleepChoice("longPwrBtn", ShortcutOptionCatalog::PowerButton);
    for (const char* key : {"shortPwrBtn", "longPwrBtn"}) {
      const auto setting = std::find_if(allSettings.begin(), allSettings.end(),
                                        [key](const SettingInfo& candidate) { return settingKeyIs(candidate, key); });
      if (setting == allSettings.end()) fail("Power shortcut setting is missing: %s", key);
      for (const auto [action, label] : {std::pair{CrossPointSettings::SLEEP_ONLY, StrId::STR_SLEEP},
                                         std::pair{CrossPointSettings::WAKE_ONLY, StrId::STR_WAKE}}) {
        const auto choice = std::find(setting->enumRawValues.begin(), setting->enumRawValues.end(), action);
        if (choice == setting->enumRawValues.end() ||
            sideButtonOptionLabel(*setting, static_cast<uint8_t>(choice - setting->enumRawValues.begin())) !=
                I18N.get(label)) {
          fail("Power-only shortcut is missing or mislabeled in %s", key);
        }
      }
    }
    const uint8_t savedShortPowerAction = SETTINGS.shortPwrBtn;
    for (const auto [action, wakes] :
         {std::pair{CrossPointSettings::IGNORE, false}, std::pair{CrossPointSettings::SLEEP_ONLY, false},
          std::pair{CrossPointSettings::WAKE_ONLY, true}, std::pair{CrossPointSettings::SLEEP, true}}) {
      SETTINGS.shortPwrBtn = action;
      if (SETTINGS.shortPowerPressWakes() != wakes) fail("Short Power wake policy does not match its shortcut");
    }
    SETTINGS.shortPwrBtn = savedShortPowerAction;
    verifySleepChoice("powerChordAction", ShortcutOptionCatalog::ButtonChord);
    verifySleepChoice("longPressMenuAction", ShortcutOptionCatalog::LongPress);
    verifySleepChoice("longPressBackAction", ShortcutOptionCatalog::LongPress);
    for (const char* key : {"sideButtonUpShort", "sideButtonUpLong", "sideButtonDownShort", "sideButtonDownLong"}) {
      const auto side = std::find_if(sideButtonSettings.begin(), sideButtonSettings.end(),
                                     [key](const SettingInfo& candidate) { return settingKeyIs(candidate, key); });
      if (side != sideButtonSettings.end()) {
        const auto sleep = std::find(side->enumRawValues.begin(), side->enumRawValues.end(), CrossPointSettings::SLEEP);
        if (sleep == side->enumRawValues.end() ||
            sideButtonOptionLabel(*side, static_cast<uint8_t>(sleep - side->enumRawValues.begin())) != tr(STR_SLEEP)) {
          fail("Sleep shortcut has the wrong label in %s", key);
        }
      }
    }
    if (gpio.hasHomeKey()) {
      verifyLibraryChoice("homeButtonTapAction", ShortcutOptionCatalog::HomeButton);
      verifyLibraryChoice("homeButtonDoubleTapAction", ShortcutOptionCatalog::HomeButton);
      verifyLibraryChoice("homeButtonLongPressAction", ShortcutOptionCatalog::HomeButton);
      verifySleepChoice("homeButtonTapAction", ShortcutOptionCatalog::HomeButton);
      verifySleepChoice("homeButtonDoubleTapAction", ShortcutOptionCatalog::HomeButton);
      verifySleepChoice("homeButtonLongPressAction", ShortcutOptionCatalog::HomeButton);
    }
    if (hasSideButtonChord) {
      const auto side =
          std::find_if(sideButtonSettings.begin(), sideButtonSettings.end(),
                       [](const SettingInfo& setting) { return settingKeyIs(setting, "sideButtonChordAction"); });
      if (side == sideButtonSettings.end() || !hasLibrary(*side, ShortcutOptionCatalog::ButtonChord)) {
        fail("Library shortcut is missing from the filtered Up + Down choices");
      }
      const auto sleep =
          std::find(side->enumRawValues.begin(), side->enumRawValues.end(), CrossPointSettings::CHORD_SLEEP);
      if (sleep == side->enumRawValues.end() ||
          sideButtonOptionLabel(*side, static_cast<uint8_t>(sleep - side->enumRawValues.begin())) != tr(STR_SLEEP)) {
        fail("Sleep shortcut is missing or mislabeled in the filtered Up + Down choices");
      }
    }
    if (!QuickActions::isQuickActionSlotActionAvailable(CrossPointSettings::LIBRARY) ||
        QuickActions::actionLabel(CrossPointSettings::LIBRARY) != StrId::STR_LIBRARY) {
      fail("Library is missing from Quick Actions choices");
    }

    const auto verifyChapterChoice = [&](const char* key, const uint8_t raw) {
      const auto setting = std::find_if(allSettings.begin(), allSettings.end(),
                                        [key](const SettingInfo& candidate) { return settingKeyIs(candidate, key); });
      if (setting == allSettings.end()) fail("Missing shortcut setting: %s", key);
      const auto choice = std::find(setting->enumRawValues.begin(), setting->enumRawValues.end(), raw);
      if (choice == setting->enumRawValues.end() ||
          setting->enumValues[static_cast<size_t>(choice - setting->enumRawValues.begin())] !=
              StrId::STR_SELECT_CHAPTER) {
        fail("Select Chapter is missing or mislabeled in %s", key);
      }
    };
    for (const char* key : {"shortPwrBtn", "longPwrBtn", "sideButtonUpShort", "sideButtonUpLong", "sideButtonDownShort",
                            "sideButtonDownLong"}) {
      verifyChapterChoice(key, CrossPointSettings::SELECT_CHAPTER);
    }
    verifyChapterChoice("powerChordAction", CrossPointSettings::CHORD_SELECT_CHAPTER);
    verifyChapterChoice("longPressMenuAction", CrossPointSettings::LONG_MENU_SELECT_CHAPTER);
    verifyChapterChoice("longPressBackAction", CrossPointSettings::LONG_MENU_SELECT_CHAPTER);
    if (hasSideButtonChord) verifyChapterChoice("sideButtonChordAction", CrossPointSettings::CHORD_SELECT_CHAPTER);
    if (gpio.hasHomeKey()) {
      for (const char* key : {"homeButtonTapAction", "homeButtonLongPressAction", "homeButtonDoubleTapAction"})
        verifyChapterChoice(key, CrossPointSettings::SELECT_CHAPTER);
    }
    if (gpio.hasTouch()) {
      for (const char* key : {"twoFingerSwipeUp", "twoFingerSwipeDown", "twoFingerSwipeLeft", "twoFingerSwipeRight",
                              "leftEdgeUp", "leftEdgeDown", "rightEdgeUp", "rightEdgeDown"})
        verifyChapterChoice(key, CrossPointSettings::TWO_FINGER_SWIPE_SELECT_CHAPTER);
    }
    if (!QuickActions::isQuickActionSlotActionAvailable(CrossPointSettings::SELECT_CHAPTER) ||
        QuickActions::actionLabel(CrossPointSettings::SELECT_CHAPTER) != StrId::STR_SELECT_CHAPTER) {
      fail("Select Chapter is missing from Quick Actions");
    }
    JsonDocument originalChapterSettings;
    SETTINGS.toJson(originalChapterSettings);
    JsonDocument chapterSettings;
    SETTINGS.toJson(chapterSettings);
    chapterSettings["shortPwrBtn"] = CrossPointSettings::SELECT_CHAPTER;
    chapterSettings["longPwrBtn"] = CrossPointSettings::SELECT_CHAPTER;
    chapterSettings["powerChordAction"] = CrossPointSettings::CHORD_SELECT_CHAPTER;
    chapterSettings["longPressMenuAction"] = CrossPointSettings::LONG_MENU_SELECT_CHAPTER;
    chapterSettings["longPressBackAction"] = CrossPointSettings::LONG_MENU_SELECT_CHAPTER;
    chapterSettings["quickActionSlots"][0] = CrossPointSettings::SELECT_CHAPTER;
    SETTINGS.fromJson(chapterSettings.as<JsonVariantConst>());
    if (SETTINGS.shortPwrBtn != CrossPointSettings::SELECT_CHAPTER ||
        SETTINGS.longPwrBtn != CrossPointSettings::SELECT_CHAPTER ||
        SETTINGS.powerChordAction != CrossPointSettings::CHORD_SELECT_CHAPTER ||
        SETTINGS.longPressMenuAction != CrossPointSettings::LONG_MENU_SELECT_CHAPTER ||
        SETTINGS.longPressBackAction != CrossPointSettings::LONG_MENU_SELECT_CHAPTER ||
        SETTINGS.quickActionSlots[0] != CrossPointSettings::SELECT_CHAPTER) {
      fail("Select Chapter settings did not survive reload");
    }
    SETTINGS.fromJson(originalChapterSettings.as<JsonVariantConst>());

    const uint8_t savedTrackReadingStats = SETTINGS.trackReadingStats;
    SETTINGS.trackReadingStats = 0;
    const auto statsDisabledSettings = getSettingsList();
    SETTINGS.trackReadingStats = savedTrackReadingStats;
    for (const char* key : {"sideButtonUpShort", "sideButtonUpLong", "sideButtonDownShort", "sideButtonDownLong"}) {
      const auto setting = std::find_if(statsDisabledSettings.begin(), statsDisabledSettings.end(),
                                        [key](const SettingInfo& candidate) { return settingKeyIs(candidate, key); });
      if (setting == statsDisabledSettings.end() ||
          std::find(setting->enumRawValues.begin(), setting->enumRawValues.end(), CrossPointSettings::READING_STATS) !=
              setting->enumRawValues.end()) {
        fail("Reading Stats remains available in %s while tracking is disabled", key);
      }
    }
  }

  static void verifySideButtonMigrationAndInput() {
    JsonDocument original;
    SETTINGS.toJson(original);
    JsonDocument legacy;
    SETTINGS.toJson(legacy);
    for (const char* key : {"sideButtonUpShort", "sideButtonUpLong", "sideButtonDownShort", "sideButtonDownLong"}) {
      legacy[key] = nullptr;
    }
    legacy["sideButtonLayout"] = CrossPointSettings::NEXT_PREV;
    legacy["sideButtonLongPress"] = CrossPointSettings::SIDE_LONG_CHAPTER_SKIP;
    SETTINGS.fromJson(legacy.as<JsonVariantConst>());
    if (SETTINGS.sideButtonUpShort != CrossPointSettings::PAGE_TURN ||
        SETTINGS.sideButtonDownShort != CrossPointSettings::PREVIOUS_PAGE ||
        SETTINGS.sideButtonUpLong != CrossPointSettings::SIDE_NEXT_CHAPTER ||
        SETTINGS.sideButtonDownLong != CrossPointSettings::SIDE_PREVIOUS_CHAPTER)
      fail("Swapped side-button layout migration failed");

    legacy["sideButtonLayout"] = CrossPointSettings::SIDE_BUTTONS_DISABLED;
    legacy["sideButtonLongPress"] = CrossPointSettings::SIDE_LONG_FONT_SIZE;
    SETTINGS.fromJson(legacy.as<JsonVariantConst>());
    if (SETTINGS.sideButtonUpShort != CrossPointSettings::IGNORE ||
        SETTINGS.sideButtonDownShort != CrossPointSettings::IGNORE ||
        SETTINGS.sideButtonUpLong != CrossPointSettings::SIDE_INCREASE_FONT ||
        SETTINGS.sideButtonDownLong != CrossPointSettings::SIDE_DECREASE_FONT)
      fail("Disabled layout with font hold migration failed");

    legacy["sideButtonUpShort"] = CrossPointSettings::LIBRARY;
    SETTINGS.fromJson(legacy.as<JsonVariantConst>());
    if (SETTINGS.sideButtonUpShort != CrossPointSettings::LIBRARY ||
        SETTINGS.sideButtonDownShort != CrossPointSettings::IGNORE)
      fail("Partially migrated side-button choices were overwritten");

    SETTINGS.fromJson(original.as<JsonVariantConst>());
    const auto sideSettings = buildControlsSideButtonSettingsList(getSettingsList());
    const size_t expected = 7 + (deviceSupportsSideButtonChord(gpio) ? 1u : 0u);
    if (sideSettings.size() != expected) fail("Side-button menu row count mismatch");
    for (const char* key : {"sideButtonUpShort", "sideButtonUpLong", "sideButtonDownShort", "sideButtonDownLong"}) {
      if (std::find_if(sideSettings.begin(), sideSettings.end(),
                       [key](const SettingInfo& setting) { return settingKeyIs(setting, key); }) == sideSettings.end())
        fail("Side-button menu is missing an individual shortcut");
    }

    ReaderButtonShortcuts shortcuts;
    mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Up);
    const auto heldUp = shortcuts.update(mappedInputManager, 1000);
    if (!heldUp.consumed) fail("Side-button press was not captured");
    mappedInputManager.injectRelease(MappedInputManager::Button::Right);
    if (ReaderButtonShortcuts::shouldConsume(heldUp, mappedInputManager))
      fail("Side-button hold swallowed a chord page turn");
    mappedInputManager.clearInjectedReleases();
    mappedInputManager.simulatorClearInputFrame();
    mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Up);
    const auto shortResult = shortcuts.update(mappedInputManager, 1200);
    if (!shortResult.triggered || shortResult.longPress || shortResult.action != SETTINGS.sideButtonUpShort)
      fail("Side-button short action did not dispatch once");
    mappedInputManager.simulatorClearInputFrame();
    if (shortcuts.update(mappedInputManager, 1300).triggered) fail("Side-button release dispatched twice");

    mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Down);
    shortcuts.update(mappedInputManager, 2000);
    mappedInputManager.simulatorClearInputFrame();
    const auto longResult = shortcuts.update(mappedInputManager, 2800);
    if (!longResult.triggered || !longResult.longPress || longResult.action != SETTINGS.sideButtonDownLong)
      fail("Side-button long action did not dispatch");
    mappedInputManager.suppressNextSideRelease(MappedInputManager::Button::Down);
    mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Down);
    if (mappedInputManager.wasReleased(MappedInputManager::Button::Down) ||
        shortcuts.update(mappedInputManager, 2900).triggered)
      fail("Side-button long release leaked into the next activity");
    mappedInputManager.simulatorClearInputFrame();
    if (shortcuts.update(mappedInputManager, 3000).triggered) fail("Side-button long action dispatched twice");

    ButtonShortcutController lock;
    lock.toggleQuickLock(4000, QuickLockTrigger::SideUpShort);
    if (lock.tryUnlockSide(4100, true, true, false, true, false, false, false, true, ReaderUtils::SKIP_HOLD_MS) ||
        !lock.isQuickLocked())
      fail("Side-button Quick Lock unlocked on press instead of release");
    if (!lock.tryUnlockSide(4200, false, false, true, true, false, false, false, true, ReaderUtils::SKIP_HOLD_MS) ||
        lock.isQuickLocked())
      fail("Side-button short Quick Lock did not unlock on a fresh release");
    lock.toggleQuickLock(5000, QuickLockTrigger::SideDownLong);
    if (lock.tryUnlockSide(5100, false, false, false, true, true, false, false, true, ReaderUtils::SKIP_HOLD_MS))
      fail("An inherited side-button hold unlocked Quick Lock");
    lock.tryUnlockSide(5200, false, false, false, true, false, false, true, true, ReaderUtils::SKIP_HOLD_MS);
    lock.tryUnlockSide(5300, false, false, false, true, true, true, false, true, ReaderUtils::SKIP_HOLD_MS);
    if (!lock.tryUnlockSide(6000, false, false, false, true, true, false, false, true, ReaderUtils::SKIP_HOLD_MS) ||
        lock.isQuickLocked())
      fail("Side-button long Quick Lock did not unlock on a fresh hold");
    LOG_INF("SMOKE", "Side-button migration and press/release checks passed");
  }

  // Keeps remote-progress.json (first.epub at page 7 of 10) for `bookPath`,
  // as a pull that could not apply it would.
  static bool parkRemoteFixture(const char* bookPath, const float scale = 1.0f) {
    JsonDocument remote;
    if (deserializeJson(remote, Storage.readFile("/remote-progress.json").c_str())) return false;
    KOReaderProgressChange parked;
    parked.document = BookSyncFile::documentId(bookPath);
    parked.percentage = remote["percentage"].as<float>() * scale;
    parked.progress = remote["progress"].as<const char*>();
    parked.device = "Phone";
    parked.timestamp = 2;
    return !parked.document.empty() && SyncParked::park(parked);
  }

  [[noreturn]] static void fail(const char* message) {
    LOG_ERR("SMOKE", "%s", message);
    std::_Exit(2);
  }

  template <typename... Args>
  [[noreturn]] static void fail(const char* format, Args... args) {
    logPrintf("ERR", "SMOKE", format, args...);
    logPrintf("ERR", "SMOKE", "\n");
    std::_Exit(2);
  }

  static void renderCurrentStep(const char* name) {
    LOG_INF("SMOKE", "Rendering %s", name);
    if (activityManager.requestUpdateAndWait() != RequestUpdateResult::Rendered) {
      fail("Render was rejected for %s", name);
    }
  }

  void queueStep(const char* name, SmokeStep nextStep, int framesToSettle = 3) {
    activeStepName = name;
    settleFrames = framesToSettle;
    step = nextStep;
  }

  void verifyStatusBarFeature() {
    JsonDocument original;
    SETTINGS.toJson(original);
    const auto originalOrientation = renderer.getOrientation();
    SETTINGS.clockDateHasBeenSynced = true;
    SETTINGS.dateFormat = CrossPointSettings::DATE_FORMAT_DAY_MONTH_YEAR_LONG;
    SETTINGS.displayStatusBar.slots = {ReaderStatusBarItem::Clock, ReaderStatusBarItem::Date,
                                       ReaderStatusBarItem::Battery};
    ReaderStatusBarConfig crowded = migrateBottomStatusBar({true, true, true, 1, 2, true, 2, 0, 2});
    crowded.slots[2] = ReaderStatusBarItem::Clock;
    SETTINGS.topReaderStatusBar = crowded;
    SETTINGS.bottomReaderStatusBar = crowded;
    RenderLock lock;
    const auto capture = [&](const std::string& name) { captureStatusBarScreen(name.c_str()); };
    const char* longTitle = "Les paramètres globaux très longs avec descendantes gypsy jumping";
    {
      // Centered bars: every separator, then a long title on a bookmarked page.
      renderer.setOrientation(GfxRenderer::Portrait);
      ReaderStatusBarContent content;
      content.bookProgress = 64;
      content.chapterPage = 4;
      content.chapterPageCount = 12;
      content.chapterTitle = "Chapter 3";
      content.timeLeftChapter = "12m";
      content.previewClock = "10:30";
      ReaderStatusBarConfig centeredBar;
      centeredBar.centered = true;
      centeredBar.slots = {ReaderStatusBarItem::Clock,
                           ReaderStatusBarItem::TimeLeftChapter,
                           ReaderStatusBarItem::Empty,
                           ReaderStatusBarItem::TitleChapter,
                           ReaderStatusBarItem::ChapterPageCount,
                           ReaderStatusBarItem::BookProgressPercentage,
                           ReaderStatusBarItem::Battery};
      std::array<uint64_t, static_cast<size_t>(ReaderStatusBarSeparator::Count)> hashes{};
      for (uint8_t sep = 0; sep < hashes.size(); ++sep) {
        centeredBar.separator = static_cast<ReaderStatusBarSeparator>(sep);
        renderer.clearScreen();
        GUI.drawReaderStatusBar(renderer, ReaderStatusBarPosition::Top, content, &centeredBar);
        GUI.drawReaderStatusBar(renderer, ReaderStatusBarPosition::Bottom, content, &centeredBar);
        hashes[sep] = hashBytes(renderer.getFrameBuffer(), renderer.getBufferSize());
        capture("centered-separator-" + std::to_string(sep));
      }
      if (hashes[0] == hashes[1] || hashes[1] == hashes[2] || hashes[0] == hashes[2])
        fail("Centered status bar separators did not change the rendered bar");
      content.chapterTitle = longTitle;
      content.bookmarked = true;
      centeredBar.separator = ReaderStatusBarSeparator::Bullet;
      renderer.clearScreen();
      GUI.drawReaderStatusBar(renderer, ReaderStatusBarPosition::Top, content, &centeredBar);
      GUI.drawReaderStatusBar(renderer, ReaderStatusBarPosition::Bottom, content, &centeredBar);
      capture("centered-long-title-bookmarked");
    }
    for (uint8_t theme = 0; theme < CrossPointSettings::UI_THEME_COUNT; ++theme) {
      if (theme == CrossPointSettings::COVER_GRID && !UITheme::supportsCoverGrid()) continue;
      SETTINGS.uiTheme = theme;
      UITheme::getInstance().reload();
      for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted,
                                     GfxRenderer::LandscapeClockwise, GfxRenderer::LandscapeCounterClockwise}) {
        renderer.setOrientation(orientation);
        for (uint8_t uiScale = 0; uiScale < CrossPointSettings::UI_SCALE_COUNT; ++uiScale) {
          SETTINGS.uiScale = uiScale;
          const auto stem =
              "theme-" + std::to_string(theme) + "-o-" + std::to_string(orientation) + "-ui-" + std::to_string(uiScale);
          for (uint8_t size = 0; size < 3; ++size) {
            SETTINGS.displayStatusBarTextSize = size;
            const auto& metrics = UITheme::getInstance().getMetrics();
            const auto name = stem + "-global-" + std::to_string(size);
            if (UITheme::getDisplayStatusBarTextHeight(renderer) > 19 + UITheme::getDisplayStatusBarHeightIncrease())
              fail("Global metric enlargement is smaller than the measured font lane");
            const int readerHeight = UITheme::getReaderStatusBarHeight(ReaderStatusBarPosition::Bottom, renderer);
            renderer.clearScreen();
            GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                           longTitle);
            capture(name + "-full");
            renderer.clearScreen();
            if (mappedInputManager.hasTouchHardware())
              TouchHeaderBackButton::drawCompact(renderer, longTitle, false, true);
            else
              CompactHeader::drawTitle(renderer, longTitle, true);
            capture(name + "-compact");
            // The first content row is outlined to make overlap visible in the captures.
            renderer.clearScreen();
            if (mappedInputManager.hasTouchHardware())
              TouchHeaderBackButton::drawCompact(renderer, longTitle, false, true);
            else
              CompactHeader::drawTitle(renderer, longTitle, true);
            const int contentTop = CompactHeader::contentTop(metrics);
            renderer.drawRect(0, contentTop, renderer.getScreenWidth(), 38);
            capture(name + "-content");
            // The clock can change minutes during this large matrix. Keep
            // captures complete, but compare deterministic date/battery pixels.
            const auto clockSlot = SETTINGS.displayStatusBar.slots[0];
            SETTINGS.displayStatusBar.slots[0] = ReaderStatusBarItem::Empty;
            renderer.clearScreen();
            if (mappedInputManager.hasTouchHardware())
              TouchHeaderBackButton::drawCompact(renderer, longTitle, false, true);
            else
              CompactHeader::drawTitle(renderer, longTitle, true);
            const auto globalHash = hashBytes(renderer.getFrameBuffer(), renderer.getBufferSize());
            for (uint8_t readerSize = 0; readerSize < 3; ++readerSize) {
              SETTINGS.statusBarTextSize = readerSize;
              renderer.clearScreen();
              if (mappedInputManager.hasTouchHardware())
                TouchHeaderBackButton::drawCompact(renderer, longTitle, false, true);
              else
                CompactHeader::drawTitle(renderer, longTitle, true);
              if (globalHash != hashBytes(renderer.getFrameBuffer(), renderer.getBufferSize()))
                fail("Reader text size changed global header pixels");
            }
            SETTINGS.displayStatusBar.slots[0] = clockSlot;
            SETTINGS.statusBarTextSize = 0;
            if (readerHeight != UITheme::getReaderStatusBarHeight(ReaderStatusBarPosition::Bottom, renderer))
              fail("Global text size changed the reader's reserved space");
            if (mappedInputManager.hasTouchHardware()) {
              const auto header = TouchHeaderBackButton::compactHeaderRect(renderer);
              const auto target = TouchHeaderBackButton::layout(header);
              if (target.touchRect.height < 68 || target.iconRect.y + target.iconRect.height > header.y + header.height)
                fail("Enlarged global status strip broke the Back/search action target");
            }
            if (uiScale == CrossPointSettings::UI_SCALE_LARGE) {
              renderer.clearScreen();
              const GlobalReadingStats stats{};
              renderGlobalStatsPage(renderer, &mappedInputManager, tr(STR_READING_STATS), stats, true, false);
              capture(name + "-stats");
              const BookReadingStats bookStats{};
              renderPerBookStatsPage(renderer, &mappedInputManager, longTitle, bookStats, 75, false, 0, true, true,
                                     true);
              capture(name + "-book-stats");
              if (!halClock.isAvailable()) {
                renderNoRtcCombinedStatsPage(renderer, &mappedInputManager, longTitle, bookStats, 75, false, 0, stats,
                                             &stats, true);
                capture(name + "-combined-stats");
              }
              renderEditBookDatesPage(renderer, &mappedInputManager, longTitle, bookStats, 0, true);
              capture(name + "-edit-dates");
            }
          }
        }
      }
    }
    // Cached languages stay pinned until reboot. Run the same render matrix
    // for each language through the runner's --language-file boot fixture.
    const auto language = I18N.getLanguage();
    for (uint8_t theme = 0; theme < CrossPointSettings::UI_THEME_COUNT; ++theme) {
      if (theme == CrossPointSettings::COVER_GRID && !UITheme::supportsCoverGrid()) continue;
      SETTINGS.uiTheme = theme;
      UITheme::getInstance().reload();
      for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted,
                                     GfxRenderer::LandscapeClockwise, GfxRenderer::LandscapeCounterClockwise}) {
        renderer.setOrientation(orientation);
        for (const uint8_t size : {0, 2}) {
          SETTINGS.displayStatusBarTextSize = size;
          if (UITheme::getDisplayStatusBarTextHeight(renderer) > 19 + UITheme::getDisplayStatusBarHeightIncrease())
            fail("Localized status text exceeds the global font lane");
          renderer.clearScreen();
          if (mappedInputManager.hasTouchHardware())
            TouchHeaderBackButton::drawCompact(renderer, tr(STR_STATUS_BAR_TEXT_SIZE), false, true);
          else
            CompactHeader::drawTitle(renderer, tr(STR_STATUS_BAR_TEXT_SIZE), true);
          capture("localized-lang-" + std::to_string(static_cast<int>(language)) + "-theme-" + std::to_string(theme) +
                  "-o-" + std::to_string(orientation) + "-global-" + std::to_string(size));
        }
      }
    }

    SETTINGS.uiTheme = CrossPointSettings::LYRA;
    UITheme::getInstance().reload();
    SETTINGS.uiScale = CrossPointSettings::UI_SCALE_LARGE;
    for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted,
                                   GfxRenderer::LandscapeClockwise, GfxRenderer::LandscapeCounterClockwise}) {
      renderer.setOrientation(orientation);
      for (uint8_t size = 0; size < 3; ++size) {
        SETTINGS.statusBarTextSize = size;
        for (unsigned mask = 0; mask < 4; ++mask) {
          SETTINGS.topReaderStatusBar.hidden = mask & 1;
          SETTINGS.bottomReaderStatusBar.hidden = mask & 2;
          const auto topSlots = SETTINGS.topReaderStatusBar.slots;
          const auto bottomSlots = SETTINGS.bottomReaderStatusBar.slots;
          JsonDocument persisted;
          SETTINGS.toJson(persisted);
          if (!SETTINGS.saveToFile()) fail("Cannot persist status bar feature settings");
          SETTINGS.topReaderStatusBar.hidden = false;
          SETTINGS.bottomReaderStatusBar.hidden = false;
          SETTINGS.displayStatusBarTextSize = 0;
          if (!SETTINGS.loadFromFile()) fail("Cannot reload status bar feature settings");
          if (SETTINGS.topReaderStatusBar.hidden != bool(mask & 1) ||
              SETTINGS.bottomReaderStatusBar.hidden != bool(mask & 2) ||
              SETTINGS.topReaderStatusBar.slots != topSlots || SETTINGS.bottomReaderStatusBar.slots != bottomSlots ||
              SETTINGS.statusBarTextSize != size || SETTINGS.displayStatusBarTextSize != 2)
            fail("Independent visibility/sizes/slots failed disk persistence");
          if ((mask & 1) && ReaderUtils::getTopStatusBarReservedHeight(renderer))
            fail("Hidden top still reserves reader space");
          if ((mask & 2) &&
              (UITheme::getStatusBarHeight(renderer) || UITheme::getProgressBarHeight() ||
               ReaderUtils::getReaderFooterReservedHeight(renderer, true) != SETTINGS.screenMarginVertical))
            fail("Hidden bottom still reserves status or auto-turn space");
          renderer.clearScreen();
          ReaderStatusBarContent content;
          content.previewClock = "12:34";
          content.chapterTitle = longTitle;
          content.timeLeftBook = "3h 40m";
          content.chapterPage = 888;
          content.chapterPageCount = 999;
          content.stablePage = 1234;
          content.stablePageCount = 9999;
          content.bookProgress = 75.12f;
          content.bookmarked = true;
          content.autoTurnLabel = "Auto 20s";
          GUI.drawReaderStatusBar(renderer, ReaderStatusBarPosition::Top, content);
          GUI.drawReaderStatusBar(renderer, ReaderStatusBarPosition::Bottom, content);
          capture("reader-o-" + std::to_string(orientation) + "-size-" + std::to_string(size) + "-hidden-" +
                  std::to_string(mask));
          if (mask == 3) {
            const auto* pixels = renderer.getFrameBuffer();
            if (!std::all_of(pixels, pixels + renderer.getBufferSize(), [](uint8_t b) { return b == 0xff; }))
              fail("Hidden bars drew progress, bookmark or auto-turn pixels");
          }
        }
      }
    }
    renderer.setOrientation(GfxRenderer::Portrait);
    if (!StatusBarFeatureSmokeTest::settings(lock, capture)) fail("Status bar popup controls failed");
    renderer.setOrientation(GfxRenderer::Portrait);
    if (!StatusBarFeatureSmokeTest::txt(lock, capture)) fail("TXT hide/show offset or reopen failed");
    if (!StatusBarFeatureSmokeTest::xtc(lock, capture)) fail("XTC bitmap hide/show failed");
    JsonDocument invalid;
    SETTINGS.toJson(invalid);
    invalid["displayStatusBarTextSize"] = 99;
    SETTINGS.fromJson(invalid.as<JsonVariantConst>());
    if (SETTINGS.displayStatusBarTextSize != 0) fail("Invalid global status size accepted");
    invalid.remove("displayStatusBarTextSize");
    SETTINGS.fromJson(invalid.as<JsonVariantConst>());
    if (SETTINGS.displayStatusBarTextSize != 0) fail("Old settings did not default global size to Small");
    SETTINGS.fromJson(original.as<JsonVariantConst>());
    renderer.setOrientation(originalOrientation);
    UITheme::getInstance().reload();
    renderer.clearScreen();
    LOG_INF("SMOKE", "Status bar feature render matrix, independent sizes, hidden geometry and disk reload passed");
  }

  void verifyStatusBarTextSizes() {
    JsonDocument original;
    SETTINGS.toJson(original);
    const auto originalOrientation = renderer.getOrientation();
    const ReaderStatusBarConfig empty{};
    ReaderStatusBarConfig crowded;
    crowded.slots = {ReaderStatusBarItem::Battery,
                     ReaderStatusBarItem::TimeLeftBook,
                     ReaderStatusBarItem::Clock,
                     ReaderStatusBarItem::TitleChapter,
                     ReaderStatusBarItem::ChapterPageCount,
                     ReaderStatusBarItem::StablePageNumber,
                     ReaderStatusBarItem::BookProgressPercentage};
    ReaderStatusBarContent content;
    content.previewOriginY = 100;
    content.previewClock = "12:34";
    content.chapterTitle = "A long chapter title with descenders: gypsy jumping";
    content.timeLeftBook = "3h 40m";
    content.chapterPage = 888;
    content.chapterPageCount = 999;
    content.stablePage = 1234;
    content.stablePageCount = 9999;
    content.bookProgress = 75.12f;
    content.bookmarked = true;
    RenderLock lock;
    for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise}) {
      renderer.setOrientation(orientation);
      int previousHeight = 0;
      uint64_t displayBarHash = 0;
      for (uint8_t size = 0; size < 3; ++size) {
        SETTINGS.statusBarTextSize = size;
        JsonDocument saved;
        SETTINGS.toJson(saved);
        SETTINGS.statusBarTextSize = 0;
        SETTINGS.fromJson(saved.as<JsonVariantConst>());
        if (SETTINGS.statusBarTextSize != size) fail("Status bar text size did not survive settings round trip");
        const int height = UITheme::getReaderStatusBarTextHeight(renderer);
        if (height <= previousHeight) fail("Larger status bar text did not reserve more reading space");
        previousHeight = height;
        // Display headers share the drawing path but keep their existing text size.
        ReaderStatusBarContent displayContent;
        displayContent.outsideReader = true;
        displayContent.previewOriginY = 100;
        displayContent.previewClock = "12:34";
        ReaderStatusBarConfig displayConfig;
        displayConfig.slots[0] = ReaderStatusBarItem::Clock;
        displayConfig.slots[6] = ReaderStatusBarItem::Battery;
        renderer.clearScreen();
        GUI.drawReaderStatusBar(renderer, ReaderStatusBarPosition::Top, displayContent, &displayConfig);
        const uint64_t currentDisplayBarHash = hashBytes(renderer.getFrameBuffer(), renderer.getBufferSize());
        if (size == 0)
          displayBarHash = currentDisplayBarHash;
        else if (currentDisplayBarHash != displayBarHash)
          fail("Reader text size changed the Display status bar");
        SETTINGS.topReaderStatusBar = empty;
        SETTINGS.bottomReaderStatusBar = empty;
        if (UITheme::getStatusBarHeight(renderer) ||
            UITheme::getReaderStatusBarHeight(ReaderStatusBarPosition::Top, renderer))
          fail("Text size reserved space for an empty status bar");
        if (ReaderUtils::getReaderFooterReservedHeight(renderer, true) < height + ReaderUtils::STATUS_BAR_TEXT_PADDING)
          fail("Automatic page turn did not reserve its enlarged label");
        SETTINGS.topReaderStatusBar = crowded;
        SETTINGS.bottomReaderStatusBar = crowded;
        for (const auto position : {ReaderStatusBarPosition::Top, ReaderStatusBarPosition::Bottom}) {
          renderer.clearScreen();
          GUI.drawReaderStatusBar(renderer, position, content);
          // Verify the text stays above the reading area (including its existing padding).
          const int end = content.previewOriginY + UITheme::getReaderStatusBarHeight(position, renderer) +
                          ReaderUtils::STATUS_BAR_TEXT_PADDING;
          // Erase the allowed rectangle in logical coordinates, so partial framebuffer
          // bytes at a rotated boundary cannot look like text outside the bar.
          renderer.fillRect(0, 0, renderer.getScreenWidth(), end, false);
          const uint8_t* pixels = renderer.getFrameBuffer();
          if (!std::all_of(pixels, pixels + renderer.getBufferSize(), [](uint8_t byte) { return byte == 0xff; }))
            fail("Status bar text escaped its reserved reading space: size=%u orientation=%u position=%u end=%d", size,
                 static_cast<unsigned>(orientation), static_cast<unsigned>(position), end);
        }
      }
    }
    JsonDocument invalid;
    SETTINGS.toJson(invalid);
    invalid["statusBarTextSize"] = 99;
    SETTINGS.statusBarTextSize = 0;
    SETTINGS.fromJson(invalid.as<JsonVariantConst>());
    if (SETTINGS.statusBarTextSize != 0) fail("Invalid status bar text size was not rejected");
    invalid.remove("statusBarTextSize");
    SETTINGS.fromJson(invalid.as<JsonVariantConst>());
    if (SETTINGS.statusBarTextSize != 0) fail("Legacy settings did not retain Small status bar text");
    SETTINGS.fromJson(original.as<JsonVariantConst>());
    renderer.setOrientation(originalOrientation);
    renderer.clearScreen();
    LOG_INF("SMOKE", "Status bar text sizes, persistence, empty bars, auto-turn and crowded layout passed");
  }

  void verifyLoadingPopupBackdrop() {
    RenderLock lock;
    const auto originalOrientation = renderer.getOrientation();
    for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise,
                                   GfxRenderer::PortraitInverted, GfxRenderer::LandscapeCounterClockwise}) {
      renderer.setOrientation(orientation);
      const int width = renderer.getScreenWidth();
      const int height = renderer.getScreenHeight();
      renderer.clearScreen();
      for (int y = 0; y < height; y += 7) renderer.drawLine(0, y, width - 1, y);
      const size_t bytes = renderer.getRegionByteSize(0, 0, width, height);
      std::vector<uint8_t> before(bytes), after(bytes);
      if (!renderer.copyRegionToBuffer(0, 0, width, height, before.data(), bytes))
        fail("Could not snapshot loading popup backdrop");
      GUI.drawPopup(renderer, tr(STR_LOADING), true);
      if (!renderer.copyRegionToBuffer(0, 0, width, height, after.data(), bytes) || before != after)
        fail("Loading popup changed the underlying framebuffer");
    }
    renderer.setOrientation(originalOrientation);
    renderer.clearScreen();
    LOG_INF("SMOKE", "Loading popup preserves backdrop in all orientations");
  }

  static void verifyWakePowerReaderShortcut() {
    if (!activityManager.isCurrentActivityNamed("EpubReader")) return;
    auto* reader = activityManager.simulatorCurrentActivity();
    const uint8_t savedShort = SETTINGS.shortPwrBtn;
    const uint8_t savedLong = SETTINGS.longPwrBtn;
    const uint8_t savedSlot = SETTINGS.quickActionSlots[0];
    SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK;
    SETTINGS.longPwrBtn = CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS;
    SETTINGS.quickActionSlots[0] = CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH;

    // Apply the same mapped-input guards as a Power-button wake, then exercise
    // the reader's independent long-press route across the hold and release.
    mappedInputManager.suppressNextPowerRelease();
    mappedInputManager.suppressNextPowerConfirmRelease();
    mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Power);
    mappedInputManager.update();
    delay(SETTINGS.getPowerButtonLongPressDuration() + 10);
    if (!mappedInputManager.isPhysicalPressed(MappedInputManager::Button::Power)) fail("Wake test lost Power hold");
    reader->loop();
    if (reader->blocksGlobalInput()) fail("Wake Power hold opened Quick Actions");

    mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Power);
    mappedInputManager.update();
    reader->loop();
    if (reader->blocksGlobalInput()) fail("Wake Power release opened Quick Actions");
    mappedInputManager.simulatorClearInputFrame();
    mappedInputManager.update();

    mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Power);
    mappedInputManager.update();
    delay(SETTINGS.getPowerButtonLongPressDuration() + 10);
    reader->loop();
    if (!reader->blocksGlobalInput()) fail("Fresh long Power press did not open Quick Actions");
    mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Power);
    mappedInputManager.update();
    reader->loop();
    mappedInputManager.simulatorClearInputFrame();
    mappedInputManager.update();
    mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Back);
    reader->loop();
    mappedInputManager.simulatorClearInputFrame();
    if (reader->blocksGlobalInput()) fail("Wake test could not close Quick Actions");
    SETTINGS.shortPwrBtn = savedShort;
    SETTINGS.longPwrBtn = savedLong;
    SETTINGS.quickActionSlots[0] = savedSlot;
    LOG_INF("SMOKE", "Wake Power hold/release ignored; fresh long press opens Quick Actions");
  }

  static void verifyCachedHomeProgressMigration() {
    const RecentBook book{"/books/legacy-home-smoke.epub", "Legacy Home smoke", {}, {}};
    const std::string legacy = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(book.path));
    const std::string current = Epub::cachePathForFilePath(book.path, "/.crosspoint");
    if (legacy == current || Storage.exists(book.path.c_str())) fail("Invalid legacy Home fixture");
    if (!Storage.mkdir(legacy.c_str())) fail("Cannot create legacy Home cache");
    RecentBookProgress::saveCachedEpubPercent(legacy, 42.5f);
    // There is deliberately no EPUB or book.bin. This must migrate and read the
    // tiny saved percentage without attempting to open, parse or index a book.
    if (RecentBookProgress::loadCachedEpubPercent(book) != 42.5f || Storage.exists(legacy.c_str()) ||
        !Storage.exists(current.c_str()) || BookMetadataCache::exists(current))
      fail("Home did not recover legacy cached progress without opening the EPUB");
    const std::string percentFile = current + "/progress_percent.bin";
    const std::string hostPercentFile = "fs_" + percentFile;
    // Force an old timestamp so this check does not depend on filesystem clock resolution.
    const auto oldTime = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
    std::filesystem::last_write_time(hostPercentFile, oldTime);
    const auto savedTime = std::filesystem::last_write_time(hostPercentFile);
    RecentBookProgress::saveCachedEpubPercent(current, 42.5001f);
    if (std::filesystem::last_write_time(hostPercentFile) != savedTime)
      fail("Saving the same rounded Home percentage rewrote the cache");
    RecentBookProgress::saveCachedEpubPercent(current, 43.5f);
    if (RecentBookProgress::loadCachedEpubPercent(book) != 43.5f) fail("Changed Home percentage was not saved");
    std::filesystem::resize_file(hostPercentFile, 2);
    RecentBookProgress::saveCachedEpubPercent(current, 43.5f);
    if (RecentBookProgress::loadCachedEpubPercent(book) != 43.5f || std::filesystem::file_size(hostPercentFile) != 7)
      fail("Saving Home percentage did not repair a truncated cache");
    if (!Storage.remove(percentFile.c_str())) fail("Cannot remove Home percent fixture");
    RecentBookProgress::saveCachedEpubPercent(current, 42.5f);
    LOG_INF("SMOKE", "Home percentage skips unchanged writes and repairs missing/truncated caches");
    if (!Storage.mkdir(legacy.c_str())) fail("Cannot recreate stale legacy Home cache");
    RecentBookProgress::saveCachedEpubPercent(legacy, 90.0f);
    if (RecentBookProgress::loadCachedEpubPercent(book) != 42.5f || !Storage.exists(legacy.c_str()))
      fail("Stale legacy progress replaced the current Home cache");
    if (!Storage.removeDir(legacy.c_str()) || !Storage.removeDir(current.c_str())) fail("Cannot remove Home fixtures");
    LOG_INF("SMOKE", "Legacy Home progress migration without EPUB loading passed");
  }

  static uint64_t hashBytes(const uint8_t* bytes, size_t size, uint64_t hash = 14695981039346656037ull) {
    for (size_t i = 0; i < size; ++i) {
      hash ^= bytes[i];
      hash *= 1099511628211ull;
    }
    return hash;
  }

  static uint64_t hashCarouselCache() {
    FsFile file;
    if (!Storage.openFileForRead("SMOKE", "/.crosspoint/home_carousel_cache_0.bin", file)) {
      fail("Carousel did not publish an artwork cache");
    }
    if (file.size() < renderer.getBufferSize() || file.size() >= 2 * renderer.getBufferSize()) {
      file.close();
      fail("Carousel snapshot must contain only one viewed artwork frame");
    }
    uint64_t hash = 14695981039346656037ull;
    uint8_t bytes[128];
    int count;
    while ((count = file.read(bytes, sizeof(bytes))) > 0) hash = hashBytes(bytes, count, hash);
    file.close();
    return hash;
  }

  void queueCarouselSwipe(bool next) {
    ++carouselCachePass;
    inputScript.clear();
    scriptIndex = 0;
    inputCompletionStep = SmokeStep::CarouselHome;
#if CROSSINK_APP_CAP_TOUCH
    if (mappedInputManager.hasTouchHardware()) {
      const int startX = renderer.getScreenWidth() * (next ? 3 : 1) / 4;
      const int endX = renderer.getScreenWidth() * (next ? 1 : 3) / 4;
      const int y = renderer.getScreenHeight() / 2;
      inputScript.push_back(touchDown(startX, y));
      inputScript.push_back(touchMove(endX, y));
      inputScript.push_back(touchRelease(endX, y));
    } else
#endif
    {
      addTap(next ? MappedInputManager::Button::Right : MappedInputManager::Button::Left);
    }
    inputScript.push_back(render("Carousel viewed position", 8));
    step = SmokeStep::ReaderInput;
  }

  void verifyCarouselCacheReturn() {
    const auto writtenAt = std::filesystem::last_write_time("fs_/.crosspoint/home_carousel_cache_0.bin");
    const uint64_t cacheHash = hashCarouselCache();
    uint64_t screenHash;
    {
      RenderLock lock;
      screenHash = hashBytes(renderer.getFrameBuffer(), renderer.getBufferSize());
    }
    if (carouselCachePass == 0) {
      if (Storage.exists("/.crosspoint/home_carousel_cache_1.bin") ||
          Storage.exists("/.crosspoint/home_carousel_cache_2.bin")) {
        fail("Carousel eagerly prepared unviewed positions");
      }
      carouselCacheWrittenAt = writtenAt;
      carouselCacheHash = cacheHash;
      const RecentBook& book = RECENT_BOOKS.getBooks().front();
      const std::string cachePath = Epub::resolveCachePathForFilePath(book.path, "/.crosspoint");
      const float oldProgress = RecentBookProgress::loadCachedEpubPercent(book);
      RecentBookProgress::saveCachedEpubPercent(cachePath, oldProgress < 50.0f ? 75.0f : 25.0f);
      BookReadingStats stats = BookReadingStats::load(cachePath);
      stats.sessionCount = 1;
      stats.totalReadingSeconds += 7200;
      if (!stats.save(cachePath)) fail("Cannot save carousel stats fixture");
    } else if (carouselCachePass <= 2) {
      if (writtenAt != carouselCacheWrittenAt || cacheHash != carouselCacheHash) {
        fail("Reading progress/stats or tracking settings rebuilt the carousel artwork");
      }
      if (screenHash == carouselScreenHash) fail("Carousel restored stale progress/stats or menu pixels");
      if (carouselCachePass == 1) {
        SETTINGS.trackReadingStats = 0;
      } else {
        SETTINGS.screenInverted = !SETTINGS.screenInverted;
      }
    } else if (carouselCachePass == 3) {
      if (cacheHash == carouselCacheHash) fail("Dark Mode did not invalidate carousel artwork");
      carouselCacheHash = cacheHash;
      const RecentBook book = RECENT_BOOKS.getBooks().front();
      if (!RECENT_BOOKS.updateBook(book.path, "Changed carousel title", book.author, book.coverBmpPath,
                                   book.coverState))
        fail("Cannot update carousel title fixture");
    } else if (carouselCachePass == 4) {
      if (cacheHash == carouselCacheHash) fail("Title changes did not invalidate carousel artwork");
      carouselCacheHash = cacheHash;
      carouselCacheWrittenAt = writtenAt;
      queueCarouselSwipe(true);
      return;
    } else {
      if (cacheHash != carouselCacheHash || writtenAt != carouselCacheWrittenAt) {
        fail("Navigating the carousel rewrote an already cached position");
      }
      const bool secondExists = Storage.exists("/.crosspoint/home_carousel_cache_1.bin");
      if (!secondExists || Storage.exists("/.crosspoint/home_carousel_cache_2.bin")) {
        fail("Carousel navigation did not cache only the viewed position");
      }
      const auto secondWrittenAt = std::filesystem::last_write_time("fs_/.crosspoint/home_carousel_cache_1.bin");
      const int selected = carouselCachePass == 5 ? 1 : 0;
      if (activityManager.getCurrentBookPath() != RECENT_BOOKS.getBooks()[selected].path) {
        fail("Carousel swipe did not select the expected book");
      }
      if (carouselCachePass == 5) {
        carouselSecondWrittenAt = secondWrittenAt;
        queueCarouselSwipe(false);
        return;
      }
      if (secondWrittenAt != carouselSecondWrittenAt) fail("Carousel rewrote the previous position while leaving it");
      LOG_INF("SMOKE", "Carousel lazy cache, live progress/stats/menu, Dark Mode and title invalidation passed");
      step = SmokeStep::Done;
      return;
    }
    carouselScreenHash = screenHash;
    ++carouselCachePass;
    activityManager.goHome();
    queueStep("Carousel return cache", SmokeStep::CarouselHome, 8);
  }

  void verifyBlockFontSizes(const int readerFontId, const char* captureName = "block-font-sizes") {
    RenderLock lock;
    const int baseFont = renderer.getFontIdForSize(readerFontId, 12);
    if (renderer.getFontPointSize(baseFont) != 12) fail("Missing body size");
    for (const uint8_t points : {8, 12, 24, 32, 44}) {
#if CROSSINK_SCALABLE_FONTS
      if (!renderer.isSdCardFont(baseFont) &&
          renderer.getFontPointSize(renderer.getFontIdForSize(baseFont, points)) != points)
        fail("Missing scalable content size %u", unsigned(points));
#endif
      const int chosen = renderer.getFontIdForSize(baseFont, points);
      if (!renderer.getFontPointSize(chosen)) fail("Bitmap size missing");
    }
    const std::string path = "/block-font-smoke.xhtml";
    if (!Storage.writeFile(path.c_str(),
                           "<html><body><h1>Heading Heading Heading Heading Heading</h1>"
                           "<p style=\"font-size:150%\">Large Large Large</p><p>Body Body Body</p>"
                           "<p>Body <span style=\"font-size:150%\">Large</span> Body</p>"
                           "<p style=\"font-size:50%\">Small Small Small</p></body></html>"))
      fail("Cannot write block font fixture");
    Epub book("/block-font-smoke.epub", "/.crosspoint");
    CssParser css("/.crosspoint/block-font-smoke");
    unsigned checked = 0;
    const int height = std::min(600, renderer.getScreenHeight() - 40);
    ChapterHtmlSlimParser parser(
        book, path, renderer, baseFont, 1.0f, false, false, 1, 320, height, false, false, false, 0,
        [&](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t, uint32_t) {
          FsFile file;
          if (!Storage.openFileForWrite("SMOKE", "/block-font-page", file) || !page->serialize(file))
            fail("Cannot save sized page");
          file.close();
          if (!Storage.openFileForRead("SMOKE", "/block-font-page", file)) fail("Cannot reload sized page");
          auto restored = Page::deserialize(file);
          file.close();
          if (!restored) fail("Sized page cache did not round trip");
          int bottom = 0;
          for (const auto& element : restored->elements) {
            if (element->getTag() != TAG_PageLine) continue;
            const auto& line = static_cast<const PageLine&>(*element);
            const auto& block = *line.getBlock();
            if (!block.wordCount()) continue;
            const char* word = block.wordText(0);
            const int requested = std::strcmp(word, "Heading") == 0 ? 24
                                  : std::strcmp(word, "Large") == 0 ? 18
                                  : std::strcmp(word, "Small") == 0 ? 8
                                                                    : 12;
            const int expected = renderer.getFontPointSize(renderer.getFontIdForSize(baseFont, requested));
            if (requested > 12 && expected <= 12) fail("Heading/CSS enlargement did not apply");
            const int actualFont = block.resolvedFontId(renderer, baseFont);
            if (block.getBlockStyle().fontSize != expected || renderer.getFontPointSize(actualFont) != expected ||
                block.getBlockStyle().lineHeight < renderer.getLineHeight(actualFont))
              fail("Sized page metrics mismatch: %s", word);
            for (uint16_t i = 0; i < block.wordCount(); ++i) {
              const char* text = block.wordText(i);
              const int wanted = std::strcmp(text, "Heading") == 0 ? 24
                                 : std::strcmp(text, "Large") == 0 ? 18
                                 : std::strcmp(text, "Small") == 0 ? 8
                                                                   : 12;
              const int chosen = block.wordFontId(renderer, baseFont, i);
              if (renderer.getFontPointSize(chosen) !=
                  renderer.getFontPointSize(renderer.getFontIdForSize(baseFont, wanted)))
                fail("Inline bitmap size was lost in page cache");
              if (block.getBlockStyle().lineHeight < renderer.getLineHeight(chosen)) fail("Inline size height clipped");
            }
            if (line.yPos < bottom || line.yPos + block.getBlockStyle().lineHeight > height)
              fail("Sized page lines overlap or overflow");
            bottom = line.yPos + block.getBlockStyle().lineHeight;
            ++checked;
          }
          renderer.clearScreen();
          restored->renderText(renderer, baseFont, 20, 20);
          captureStatusBarScreen(captureName);
        },
        true, "", "", 0, {}, nullptr, &css);
    if (!parser.parseAndBuildPages() || checked < 3) fail("Block font parser smoke failed");
    WordSelectNavigator navigator;
    WordSelectNavigator::WordInfo word;
    word.textLen = word.lookupLen = 7;
    word.screenX = 20;
    word.screenY = 20;
    word.width = 100;
    word.setLineHeight(60);
    word.fontId = renderer.getFontIdForSize(baseFont, 24);
    navigator.load({word}, {{20, 0, 1}}, "Heading", false);
    bool hit = false;
    navigator.selectWordAtPoint(50, 75, 20, &hit);
    if (!hit) fail("Heading word selection used body height");
    Storage.remove(path.c_str());
    Storage.remove("/block-font-page");
    LOG_INF("SMOKE", "Block font sizes: layout, cache, rendering and selection passed");
  }

#if CROSSINK_SCALABLE_FONTS
  void verifyPublisherFonts() {
    RenderLock lock;
    const auto savedMode = SETTINGS.publisherFonts;
    if (savedMode != static_cast<uint8_t>(PublisherFonts::Accents))
      fail("Fresh settings did not default to Ignore Body Font");
    Epub book(std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK"), "/.crosspoint");
    if (!book.load()) fail("Cannot open publisher-font fixture");
    const auto settingsPath = book.getCachePath() + "/reader_settings.bin";
    uint8_t record[160]{};
    record[0] = 13;
    record[1] = 1;
    record[155] = 32;  // Publisher-font override bit 21.
    record[159] = 2;
    auto writeSettingsFixture = [&](size_t bytes) {
      auto file = Storage.open(settingsPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
      if (!file || file.write(record, bytes) != bytes || !file.sync()) fail("Publisher settings fixture write failed");
      file.close();
    };
    writeSettingsFixture(sizeof(record));
    if (EpubReaderActivity::readBookReaderSettings(book).readerSettings.publisherFonts != 2 ||
        !EpubReaderActivity::saveBookRenderMode(book.getPath(), 0) ||
        EpubReaderActivity::readBookReaderSettings(book).readerSettings.publisherFonts != 2)
      fail("Publisher book setting did not survive reload/save");
    record[0] = 12;
    record[155] = 16;  // Existing character-spacing override bit 20.
    record[158] = 3;
    SETTINGS.publisherFonts = 1;
    writeSettingsFixture(sizeof(record) - 1);
    const auto legacy = EpubReaderActivity::readBookReaderSettings(book);
    if (legacy.readerSettings.characterSpacing != 3 || legacy.readerSettings.publisherFonts != 1 ||
        (legacy.readerSettingsOverrideMask & (1U << 21)))
      fail("Publisher mode broke version 12 inheritance");
    Storage.remove(settingsPath.c_str());
    const int readerFont = sdFontSystem.ensureBuiltInReaderFont(renderer);
    for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise}) {
      renderer.setOrientation(orientation);
      for (uint8_t mode = 0; mode < 3; ++mode) {
        SETTINGS.publisherFonts = mode;
        bookFontSystem.close();
        for (unsigned i = 0; i < 32; ++i)
          Storage.remove((book.getCachePath() + "/publisher-font-" + std::to_string(i)).c_str());
        bookFontSystem.ensure(book, renderer, mode != 0);
        if (bookFontSystem.requestedFaces()) fail("Opening book eagerly requested fonts");
        auto spec = SETTINGS.readerRenderSpec(renderer.getScreenWidth() - 40, renderer.getScreenHeight() - 40);
        spec.publisherFontSignature = mode ? bookFontSystem.signature() : 0;
        Section section(book, 0, renderer, "-publisher-smoke");
        if (!section.createSectionFile(spec) || !section.loadSectionFile(spec)) fail("Publisher layout/cache failed");
        bool publisherWord = false, readerWord = false;
        for (uint16_t index = 0; index < section.pageCount; ++index) {
          section.currentPage = index;
          auto page = section.loadPageFromSectionFile();
          if (!page) fail("Publisher page cache failed");
          for (const auto& element : page->elements) {
            if (element->getTag() != TAG_PageLine) continue;
            const auto& text = *static_cast<const PageLine&>(*element).getBlock();
            for (uint16_t word = 0; word < text.wordCount(); ++word) {
              const int id = text.wordFontId(renderer, readerFont, word);
              const uint8_t points = renderer.getFontPointSize(id);
              const bool isReader = id == renderer.getFontIdForSize(readerFont, points);
              publisherWord |= !isReader;
              readerWord |= isReader;
            }
          }
          renderer.setRenderMode(GfxRenderer::BW);
          renderer.clearScreen();
          page->renderText(renderer, readerFont, 20, 20);
          char capture[72];
          std::snprintf(capture, sizeof(capture), "publisher-mode-%u-orientation-%u-page-%u", unsigned(mode),
                        unsigned(orientation), unsigned(index));
          captureStatusBarScreen(capture);
        }
        if (mode == 0 && (publisherWord || !readerWord)) fail("Off changed the reader font");
        if (mode == 1 && (!publisherWord || !readerWord)) fail("Ignore Body Font did not retain the reader body font");
        if (mode == 2 && (!publisherWord || readerWord)) fail("All Text did not use publisher fonts");
        if (mode == 1 && Storage.exists((book.getCachePath() + "/publisher-font-0").c_str()))
          fail("Ignore Body Font extracted the ignored body face");
        const uint32_t requested = bookFontSystem.requestedFaces();
        bookFontSystem.close();
        bookFontSystem.ensure(book, renderer, mode != 0);
        if (bookFontSystem.requestedFaces() || !section.loadSectionFile(spec) ||
            bookFontSystem.requestedFaces() != requested)
          fail("Publisher cache did not restore its demanded families");
        auto changed = spec;
        changed.publisherFontSignature ^= 1;
        if (section.loadSectionFile(changed)) fail("Publisher font identity did not invalidate section cache");
      }
    }
    bookFontSystem.close();
    SETTINGS.publisherFonts = 1;
    for (unsigned i = 0; i < 32; ++i)
      Storage.remove((book.getCachePath() + "/publisher-font-" + std::to_string(i)).c_str());
    if (!EpubReaderActivity::drawCurrentPageToBuffer(book.getPath(), renderer))
      fail("Publisher sleep-page reconstruction failed");
    bool extracted = false;
    for (unsigned i = 1; i < 32; ++i)
      extracted |= Storage.exists((book.getCachePath() + "/publisher-font-" + std::to_string(i)).c_str());
    if (!extracted || bookFontSystem.signature() || bookFontSystem.requestedFaces())
      fail("Sleep reconstruction did not load and release publisher fonts");
    const auto sleepPageHash = hashBytes(renderer.getFrameBuffer(), renderer.getBufferSize());
    if (!EpubReaderActivity::drawCurrentPageToBuffer(book.getPath(), renderer) ||
        sleepPageHash != hashBytes(renderer.getFrameBuffer(), renderer.getBufferSize()))
      fail("Publisher sleep page changed when reopening its cache");
    const auto globalRows = buildReaderSettingsParentList(getSettingsList());
    if (std::any_of(globalRows.begin(), globalRows.end(),
                    [](const auto& row) { return row.valuePtr == &CrossPointSettings::publisherFonts; }))
      fail("Publisher fonts remained in the global Reader parent menu");
    const auto fontRows = buildReaderFontSettingsList(getSettingsList());
    const auto publisherRow = std::find_if(fontRows.begin(), fontRows.end(), [](const auto& row) {
      return row.valuePtr == &CrossPointSettings::publisherFonts;
    });
    if (publisherRow == fontRows.end()) fail("Global Publisher fonts setting is missing from Font Options");
    const int publisherIndex = static_cast<int>(std::distance(fontRows.begin(), publisherRow)) + 1;
    const std::string savedFamily = SETTINGS.sdFontFamilyName;
    lock.unlock();  // Opening Font Options takes its own render lock.
    for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise}) {
      for (const bool useTtf : {false, true}) {
        const char* family = std::getenv("CROSSINK_SIMULATOR_SMOKE_FONT_FAMILY");
        if (useTtf && !family) continue;
        std::snprintf(SETTINGS.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName), "%s", useTtf ? family : "");
        renderer.setOrientation(orientation);
        SETTINGS.publisherFonts = 1;
        SettingsActivity globalSettings(renderer, mappedInputManager);
        globalSettings.onEnter();
        const auto release = [&](MappedInputManager::Button button) {
          mappedInputManager.simulatorInjectRelease(button);
          globalSettings.loop();
          mappedInputManager.simulatorClearInputFrame();
        };
        release(MappedInputManager::Button::Confirm);  // Display tab to Reader tab.
        release(mappedInputManager.menuButton(MappedInputManager::Button::Down));
        release(MappedInputManager::Button::Confirm);  // Reader > Font Options.
        RenderLock globalLock;
        globalSettings.render(std::move(globalLock));
        const int expectedIndex = publisherIndex + (useTtf ? 1 : 0);
        for (int i = 1; i < expectedIndex; ++i)
          release(mappedInputManager.menuButton(MappedInputManager::Button::Down));
        if (globalSettings.simulatorCategoryIndex() != 1 || globalSettings.simulatorSelectedIndex() != expectedIndex)
          fail("Global Publisher fonts row was not reachable in Font Options");
        globalSettings.render(std::move(globalLock));
        char globalCapture[72];
        std::snprintf(globalCapture, sizeof(globalCapture), "publisher-global-ttf-%u-orientation-%u", unsigned(useTtf),
                      unsigned(orientation));
        captureStatusBarScreen(globalCapture);
        release(MappedInputManager::Button::Confirm);
        if (!globalSettings.simulatorOptionPopupActive()) fail("Global Publisher fonts picker did not open");
        globalSettings.render(std::move(globalLock));
        std::snprintf(globalCapture, sizeof(globalCapture), "publisher-global-options-ttf-%u-orientation-%u",
                      unsigned(useTtf), unsigned(orientation));
        captureStatusBarScreen(globalCapture);
        release(mappedInputManager.menuButton(MappedInputManager::Button::Down));
        mappedInputManager.simulatorInjectPress(MappedInputManager::Button::Confirm);
        globalSettings.loop();
        mappedInputManager.simulatorClearInputFrame();
        release(MappedInputManager::Button::Confirm);
        if (SETTINGS.publisherFonts != 2) fail("Global Publisher fonts picker did not apply All Text");
        globalSettings.onExit();
      }
      std::snprintf(SETTINGS.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName), "%s", savedFamily.c_str());
      RenderLock menuLock;
      renderer.setOrientation(orientation);
      for (uint8_t mode = 0; mode < 3; ++mode) {
        SETTINGS.publisherFonts = mode;
        ReaderDrawerState state;
        state.tab = ReaderDrawerTab::Font;
        const auto catalog = makeReaderDrawerCatalog({});
        const auto& rows = catalog[static_cast<size_t>(state.tab)];
        state.selectedIndex = 1;
        state.rootTopIndex[static_cast<size_t>(state.tab)] = 0;
        if (rows.items[state.selectedIndex] != ReaderDrawerCatalogItem::PublisherFonts)
          fail("Publisher menu row missing");
        EpubReaderDrawerActivity editor(renderer, mappedInputManager, {}, nullptr, 0.0f, 0, 0, false, false, false,
                                        false, false, false, false, false, true, true, 0, 0, 5, false, nullptr, nullptr,
                                        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, 0, false,
                                        nullptr, nullptr, state);
        editor.onEnter();
        renderer.clearScreen();
        editor.render(std::move(menuLock));
        char capture[64];
        std::snprintf(capture, sizeof(capture), "publisher-menu-%u-orientation-%u", unsigned(mode),
                      unsigned(orientation));
        captureStatusBarScreen(capture);
        mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Confirm);
        editor.loop();
        mappedInputManager.simulatorClearInputFrame();
        editor.render(std::move(menuLock));
        if (editor.simulatorState().pane != ReaderDrawerPane::EnumOptions ||
            editor.simulatorState().selectedIndex != mode)
          fail("Publisher menu did not open the current choice");
        std::snprintf(capture, sizeof(capture), "publisher-options-%u-orientation-%u", unsigned(mode),
                      unsigned(orientation));
        captureStatusBarScreen(capture);
        editor.onExit();
      }
    }
    bookFontSystem.close();
    if (renderer.resolveBookFont(readerFont, CssParser::hashFontFamily("Publisher Body")) != readerFont)
      fail("Publisher fonts survived close");
    SETTINGS.publisherFonts = savedMode;
    renderer.setOrientation(GfxRenderer::Portrait);
    LOG_INF("SMOKE", "Publisher fonts: modes, orientations, cache identity and cleanup passed");
  }
#endif

  void verifyBitmapBook(const int fontId, const char* label) {
    if (!std::getenv("CROSSINK_SIMULATOR_SMOKE_FONT_SIZE_BOOK")) return;
    RenderLock lock;
    Epub book(std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK"), "/.crosspoint");
    if (!book.load()) fail("Cannot load supplied font-size book");
    unsigned dropCaps = 0, pages = 0;
    for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise}) {
      renderer.setOrientation(orientation);
      auto spec = SETTINGS.readerRenderSpec(renderer.getScreenWidth() - 40, renderer.getScreenHeight() - 40);
      spec.fontId = fontId;
      spec.embeddedStyle = true;
      spec.paragraphAlignment = CrossPointSettings::BOOK_STYLE;
      for (int spine = 0; spine < book.getSpineItemsCount(); ++spine) {
        Section section(book, spine, renderer, "-font-size-smoke");
        if (!section.loadSectionFile(spec) && (!section.createSectionFile(spec) || !section.loadSectionFile(spec)))
          fail("Supplied book section failed: %d", spine);
        for (uint16_t index = 0; index < section.pageCount; ++index) {
          section.currentPage = index;
          auto page = section.loadPageFromSectionFile();
          if (!page) fail("Supplied book page cache failed: %d/%u", spine, unsigned(index));
          for (const auto& element : page->elements) {
            if (element->getTag() == TAG_PageDropCap) {
              const auto& cap = static_cast<const PageDropCap&>(*element);
              if (std::strcmp(cap.getText(), "R")) fail("Opening drop cap lost its letter");
              ++dropCaps;
            }
          }
          renderer.setRenderMode(GfxRenderer::BW);
          renderer.clearScreen();
          if (auto* cache = renderer.getFontCacheManager()) {
            FontCacheManager::PrewarmScope scope(*cache, FontCacheManager::PreparationPolicy::Normal);
            page->renderText(renderer, fontId, 20, 20);
            if (!scope.endScanAndPrewarm()) fail("Supplied book page prewarm failed");
            page->renderText(renderer, fontId, 20, 20);
          } else {
            page->renderText(renderer, fontId, 20, 20);
          }
          char name[96];
          std::snprintf(name, sizeof(name), "publisher-%s-o%u-s%d-p%u", label, unsigned(orientation), spine,
                        unsigned(index));
          captureStatusBarScreen(name);
          ++pages;
        }
      }
    }
    if (dropCaps != 2) fail("Supplied book opening drop cap missing after layout/cache");
    LOG_INF("SMOKE", "Supplied book %s: %u pages rendered, opening drop cap preserved in both orientations", label,
            pages);
  }

  void verifyBitmapFontSizes() {
    const auto orientation = renderer.getOrientation();
#if CROSSINK_SCALABLE_FONTS
    verifyBlockFontSizes(sdFontSystem.ensureBuiltInReaderFont(renderer), "s3-scalable");
#else
    for (const int base : {BITTER_12_FONT_ID, LEXENDDECA_12_FONT_ID}) {
      if (renderer.getFontPointSize(base) != 12) fail("Bitmap body size missing");
      for (const auto pair : {std::pair<uint8_t, uint8_t>{8, 10}, {11, 10}, {13, 14}, {15, 16}, {44, 16}}) {
        const int chosen = renderer.getFontIdForSize(base, pair.first);
        if (renderer.getFontPointSize(chosen) != pair.second) fail("Bitmap size snapping failed");
      }
      renderer.setOrientation(GfxRenderer::Portrait);
      verifyBlockFontSizes(base, base == BITTER_12_FONT_ID ? "c3-bitter" : "c3-lexend");
      renderer.setOrientation(GfxRenderer::LandscapeClockwise);
      verifyBlockFontSizes(base, base == BITTER_12_FONT_ID ? "c3-bitter-landscape" : "c3-lexend-landscape");
      verifyBitmapBook(base, base == BITTER_12_FONT_ID ? "bitter12" : "lexend12");
      verifyBitmapBook(renderer.getFontIdForSize(base, 16), base == BITTER_12_FONT_ID ? "bitter16" : "lexend16");
      renderer.setOrientation(orientation);
    }
#endif
    if (renderer.getFontPointSize(UI_12_FONT_ID) != 0 || renderer.getFontIdForSize(UI_12_FONT_ID, 24) != UI_12_FONT_ID)
      fail("Reader sizing changed UI fonts");
    if (const char* family = std::getenv("CROSSINK_SIMULATOR_SMOKE_FONT_FAMILY")) {
      std::snprintf(SETTINGS.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName), "%s", family);
      SETTINGS.readerFontPointSize = 12;
      sdFontSystem.ensureLoaded(renderer);
      const int original = SETTINGS.getReaderFontId();
      if (!renderer.isSdCardFont(original)) fail("Bitmap SD family not loaded");
      renderer.setOrientation(GfxRenderer::Portrait);
      verifyBlockFontSizes(original, "bitmap-sd");
      renderer.setOrientation(GfxRenderer::LandscapeClockwise);
      verifyBlockFontSizes(original, "bitmap-sd-landscape");
      renderer.setOrientation(orientation);
      sdFontSystem.releaseLoadedFont(renderer);
      sdFontSystem.ensureLoaded(renderer);
      if (SETTINGS.getReaderFontId() != original) fail("Bitmap SD size identity changed on reload");
      verifyBlockFontSizes(original, "bitmap-sd-reloaded");
      if (renderer.getFontPointSize(renderer.getFontIdForSize(original, 8)) >= 12)
        fail("Bitmap SD shrinking unavailable");
      const auto dictionary = sdFontSystem.activateDictionaryFont(renderer, family, 8);
      if (!dictionary.usingDictionaryFont || dictionary.fontId == original) fail("Bitmap dictionary font did not swap");
      if (sdFontSystem.restoreReaderFont(renderer) != original) fail("Bitmap dictionary return lost content sizes");
      verifyBlockFontSizes(original, "bitmap-sd-dictionary-return");
      verifyBitmapBook(original, "sd12");
    }
    renderer.setOrientation(orientation);
    LOG_INF("SMOKE", "Bitmap size snapping, page cache and selection passed");
  }

  void tickFileBrowserSyncReturn() {
#if CROSSINK_APP_CAP_TOUCH
    const char* contextMenu = std::getenv("CROSSINK_SIMULATOR_SMOKE_CONTEXT_MENU_SYNC_RETURN");
    const bool fromLibrary = contextMenu && std::string_view(contextMenu) == "library";
    const char* phase = std::getenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_PHASE");
    if (!phase) {
      const char* bookPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
      if (!bookPath || !Storage.exists(bookPath)) fail("Browser sync book fixture missing");
      for (int row = 0; row < 24; ++row) {
        const std::string path = "/books/a-sync-fixture-" + std::to_string(row) + ".txt";
        if (!Storage.writeFile(path.c_str(), "Fixture")) fail("Cannot create browser row fixture");
      }
      APP_STATE.openEpubPath = bookPath;
      if (!APP_STATE.saveToFile()) fail("Cannot save browser sync book path");
      KOREADER_STORE.setCredentials("smoke", "smoke");
      if (!KOREADER_STORE.saveToFile()) fail("Cannot save browser sync credentials");
      if (fromLibrary) {
        RECENT_BOOKS.addOrUpdateBook(bookPath, "Sync EPUB", "", "");
        for (int row = 0; row < 12; ++row) {
          const std::string path = "/books/a-sync-fixture-" + std::to_string(row) + ".txt";
          RECENT_BOOKS.addOrUpdateBook(path, "Sync fixture", "", "");
        }
        if (!RECENT_BOOKS.saveToFile()) fail("Cannot save Library sync fixture");
        activityManager.goToLibrary();
      } else {
        activityManager.goToFileBrowser(bookPath);
      }
      setenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_PHASE", "launch", 1);
      settleFrames = 4;
      return;
    }
    if (std::string_view(phase) == "launch" && fromLibrary) {
      auto* library = dynamic_cast<LibraryActivity*>(activityManager.simulatorCurrentActivity());
      if (!library) fail("Expected Library before menu sync");
      {
        RenderLock lock;
        library->simulatorSetView(4, true, "Sync");
        library->simulatorSelectRow(12);
        // Restore the view even if saved global preferences differ.
        SETTINGS.librarySortMethod = 1;
        SETTINGS.librarySortDescending = false;
        if (!SETTINGS.saveToFile()) fail("Cannot save Library sync settings fixture");
        library->requestUpdate();
      }
      setenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_PHASE", "library-view", 1);
      settleFrames = 4;
      return;
    }
    if (std::string_view(phase) == "library-view") {
      auto* library = dynamic_cast<LibraryActivity*>(activityManager.simulatorCurrentActivity());
      if (!library) fail("Expected settled Library before menu sync");
      {
        RenderLock lock;
        const std::string selection = std::to_string(library->simulatorSelection());
        const std::string scroll = std::to_string(library->simulatorTopIndex());
        setenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_SELECTION", selection.c_str(), 1);
        setenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_SCROLL", scroll.c_str(), 1);
        library->simulatorOpenContextMenu();
      }
      setenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_PHASE", "drawer", 1);
      inputScript = {render("Library context menu opened", 4), assertActivity("FileBrowserAction")};
      addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
      addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
      inputScript.push_back(press(MappedInputManager::Button::Confirm));
      scriptIndex = 0;
      return;
    }
    if (std::string_view(phase) == "launch") {
      auto* browser = dynamic_cast<FileBrowserActivity*>(activityManager.simulatorCurrentActivity());
      if (!browser || browser->simulatorFolderPath() != "/books" || browser->simulatorSelectedIndex() == 0)
        fail("Expected a selected book inside its folder before sync");
      const std::string selection = std::to_string(browser->simulatorSelectedIndex());
      const std::string scroll = std::to_string(browser->simulatorTopIndex());
      setenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_SELECTION", selection.c_str(), 1);
      setenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_SCROLL", scroll.c_str(), 1);
      setenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_PHASE", "drawer", 1);
      if (contextMenu) {
        {
          RenderLock lock;
          browser->simulatorOpenContextMenu();
        }
        inputScript = {render("Browser context menu opened", 4), assertActivity("FileBrowserAction")};
        for (int row = 0; row < 3; ++row) addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
        inputScript.push_back(press(MappedInputManager::Button::Confirm));
        scriptIndex = 0;
        return;
      }
      const int width = renderer.getScreenWidth();
      const int height = renderer.getScreenHeight();
      inputScript = {touchDown(width / 2, 8),
                     touchMove(width / 2, height / 4),
                     touchRelease(width / 2, height / 4),
                     render("Browser sync drawer opened", 4),
                     {ScriptActionType::OpenFrontlightSync, MappedInputManager::Button::Back, nullptr, 0, 0, 0},
                     render("Browser sync choices opened", 4),
                     press(MappedInputManager::Button::Confirm)};
      scriptIndex = 0;
      return;
    }
    if (std::string_view(phase) == "drawer") {
      if (scriptIndex + 1 == inputScript.size())
        setenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_PHASE", "network", 1);
      runReaderInputScript();
      return;
    }
    if (std::string_view(phase) == "network") {
      if (!activityManager.isCurrentActivityNamed("WifiSelection")) fail("Expected Wi-Fi selection after reboot");
      const auto expectedOrigin = fromLibrary ? PendingOverlayOrigin::Library : PendingOverlayOrigin::FileBrowser;
      if (APP_STATE.pendingOverlayResume.origin != expectedOrigin ||
          (!fromLibrary && APP_STATE.pendingOverlayResume.fileBrowserPath != "/books") ||
          (fromLibrary && APP_STATE.pendingOverlayResume.libraryQuery != "Sync"))
        fail("Book list return route was lost across network reboot");
      setenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_PHASE", "returned", 1);
      inputScript = {press(MappedInputManager::Button::Back), release(MappedInputManager::Button::Back)};
      scriptIndex = 0;
      return;
    }
    // The Back input exits Wi-Fi selection and sync, then performs the cleanup
    // reboot. Until that happens, keep feeding the real activity input loop.
    if (!inputScript.empty()) {
      if (scriptIndex < inputScript.size()) runReaderInputScript();
      return;
    }
    if (fromLibrary) {
      auto* library = dynamic_cast<LibraryActivity*>(activityManager.simulatorCurrentActivity());
      if (!library || library->simulatorQuery() != "Sync" || library->simulatorSort() != 4 ||
          !library->simulatorDescending())
        fail("Sync did not restore the Library view");
      if (library->simulatorSelection() !=
              std::atoi(std::getenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_SELECTION")) ||
          library->simulatorTopIndex() != std::atoi(std::getenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_SCROLL"))) {
        LOG_ERR("SMOKE", "Library actual selection=%d scroll=%d expected=%s/%s rows=%d", library->simulatorSelection(),
                library->simulatorTopIndex(), std::getenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_SELECTION"),
                std::getenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_SCROLL"), library->simulatorRowCount());
        fail("Library selection or scroll changed after sync");
      }
      if (APP_STATE.pendingOverlayResume.valid()) fail("Library sync return route was not consumed");
      LOG_INF("SMOKE", "Simulator smoke test passed: Library menu sync returns through both reboots");
      std::_Exit(0);
    }
    auto* browser = dynamic_cast<FileBrowserActivity*>(activityManager.simulatorCurrentActivity());
    if (!browser || browser->simulatorFolderPath() != "/books") fail("Sync did not return to the original folder");
    if (browser->simulatorSelectedIndex() !=
            static_cast<size_t>(std::atoi(std::getenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_SELECTION"))) ||
        browser->simulatorTopIndex() != std::atoi(std::getenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_SCROLL")))
      fail("Browser selection or scroll changed after sync");
    if (APP_STATE.pendingOverlayResume.valid()) fail("Browser sync return route was not consumed");
    LOG_INF("SMOKE", "Simulator smoke test passed: browser sync returns through both reboots");
    std::_Exit(0);
#else
    fail("Browser sync return smoke requires a touch device");
#endif
  }

  void tickFilenameFont() {
#if CROSSINK_SCALABLE_FONTS
    const char* family = std::getenv("CROSSINK_SIMULATOR_SMOKE_FILENAME_FONT");
    if (scriptIndex < inputScript.size()) {
      runReaderInputScript();
      return;
    }
    inputScript.clear();
    scriptIndex = 0;
    switch (filenameFontPhase++) {
      case 0: {
        RenderLock lock;
        SETTINGS.filenameFallbackFont[0] = '\0';
        SETTINGS.uiScale = CrossPointSettings::UI_SCALE_SMALL;
        SETTINGS.uiTheme = CrossPointSettings::LYRA;
        UITheme::getInstance().reload();
        filenameFontSystem.invalidate();
        if (!filenameFontSystem.ensureLoaded(renderer)) fail("None filename font failed");
        const auto device = buildSystemDeviceSettingsList(getSettingsList());
        if (std::none_of(device.begin(), device.end(), [](const SettingInfo& setting) {
              return setting.action == SettingAction::FilenameFallbackFont;
            }))
          fail("Filename setting missing from Device settings");
        std::vector<std::string> names;
        if (!filenameFontSystem.discover(names) || std::find(names.begin(), names.end(), family) == names.end())
          fail("Filename fixture family missing");
        for (const auto& name : names)
          if (name == "Variable Only" || name == "Bitmap Only" || name == "Corrupt Only")
            fail("Unsupported filename font offered");
        activityManager.replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInputManager));
        queueStep("Filename Settings entry", SmokeStep::Start, 4);
        break;
      }
      case 1: {
        const auto device = buildSystemDeviceSettingsList(getSettingsList());
        const auto fontSetting = std::find_if(device.begin(), device.end(), [](const SettingInfo& setting) {
          return setting.action == SettingAction::FilenameFallbackFont;
        });
        for (int i = 0; i < 3; ++i) addTap(MappedInputManager::Button::Confirm);
        addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("Filename Device settings", 4));
        inputScript.push_back(assertSettingsNavigation(3, 1));
        for (int i = 0; i < std::distance(device.begin(), fontSetting); ++i)
          addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
        inputScript.push_back(render("Filename fallback setting", 4));
        break;
      }
      case 2: {
        RenderLock lock;
        captureStatusBarScreen("filename-device-setting");
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("Filename font picker", 4));
        break;
      }
      case 3: {
        RenderLock lock;
        const auto* settings = dynamic_cast<SettingsActivity*>(activityManager.simulatorCurrentActivity());
        if (!settings || !settings->simulatorOptionPopupActive()) fail("Filename picker did not open");
        const auto& names = settings->simulatorFilenameFontNames();
        const auto found = std::find(names.begin(), names.end(), family);
        if (found == names.end()) fail("Filename picker lacks selected family");
        captureStatusBarScreen("filename-font-picker");
        for (int i = 0; i < std::distance(names.begin(), found); ++i) addTap(MappedInputManager::Button::Down);
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("Filename font applied", 5));
        break;
      }
      case 4: {
        RenderLock lock;
        if (std::strcmp(SETTINGS.filenameFallbackFont, family) != 0) fail("Filename selection was not applied");
        const auto& fonts = renderer.getFontMap();
        for (int id : {SMALL_FONT_ID, UI_10_FONT_ID, UI_12_FONT_ID}) {
          const int compositeId = renderer.filenameFontId(id);
          if (compositeId == id) fail("Filename composite missing");
          const auto& primary = fonts.at(id);
          const auto& composite = fonts.at(compositeId);
          for (auto style : {EpdFontFamily::REGULAR, EpdFontFamily::BOLD}) {
            if (primary.getGlyphData('A', style).fontData != primary.getData(style) ||
                primary.getGlyphData('A', style).glyph != composite.getGlyphData('A', style).glyph)
              fail("Filename font replaced built-in Latin");
            const auto cjk = composite.getGlyphData(0x4e00, style);
            if (!composite.hasCodepoint(0x4e00) || !cjk.glyph || cjk.fontData == primary.getData(style))
              fail("Missing CJK fallback glyph");
            const bool covered = primary.hasCodepoint(0x4e00, style);
            const auto sharedCjk = primary.getGlyphData(0x4e00, style);
            const int sharedWidth = renderer.getTextWidth(id, "Menu 一 Հայերեն", style);
            const int aliasWidth = renderer.getTextWidth(compositeId, "Menu 一 Հայերեն", style);
            // Scalable glyph descriptors rotate through scratch slots; compare
            // their owning font and measured text, not transient addresses.
            if (!covered || !sharedCjk.glyph || sharedCjk.fontData != cjk.fontData || sharedWidth != aliasWidth) {
              LOG_ERR("SMOKE", "UI font %d style %d: covered=%d width=%d/%d", id, int(style), covered, sharedWidth,
                      aliasWidth);
              fail("Shared UI font does not measure/draw the fallback");
            }
          }
        }
        JsonDocument saved;
        SETTINGS.toJson(saved);
        saved["filenameFallbackFont"] = std::string(family);  // own bytes before mutating the settings buffer
        SETTINGS.filenameFallbackFont[0] = '\0';
        SETTINGS.fromJson(saved.as<JsonVariantConst>());
        if (std::strcmp(SETTINGS.filenameFallbackFont, family) != 0) fail("Filename JSON round trip failed");
        captureStatusBarScreen("filename-device-selected");
        Storage.mkdir("/books/日本語");
        if (!Storage.writeFile("/books/日本語/Latin 一丁.txt", "Filename font smoke"))
          fail("Cannot create mixed title");
        activityManager.replaceActivity(
            std::make_unique<FileBrowserActivity>(renderer, mappedInputManager, "/books/日本語"));
        queueStep("Filename mixed browser small", SmokeStep::Start, 8);
        break;
      }
      case 5: {
        RenderLock lock;
        captureStatusBarScreen("filename-browser-small");
        SETTINGS.uiScale = CrossPointSettings::UI_SCALE_LARGE;
        UITheme::getInstance().reload();
        activityManager.replaceActivity(
            std::make_unique<FileBrowserActivity>(renderer, mappedInputManager, "/books/日本語"));
        queueStep("Filename mixed browser large", SmokeStep::Start, 8);
        break;
      }
      case 6: {
        RenderLock lock;
        captureStatusBarScreen("filename-browser-large");
        RECENT_BOOKS.addOrUpdateBook("/books/日本語/Latin 一丁.txt", "Latin 一丁 日本語", "Author 日本語", "");
        SETTINGS.uiTheme = CrossPointSettings::LYRA;
        UITheme::getInstance().reload();
        activityManager.replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInputManager));
        queueStep("Filename mixed Home", SmokeStep::Start, 20);
        break;
      }
      case 7: {
        RenderLock lock;
        captureStatusBarScreen("filename-home");
        auto* home = dynamic_cast<HomeActivity*>(activityManager.simulatorCurrentActivity());
        if (!home) fail("Filename Home missing");
        home->onFrontlightPanelOpened();
        SETTINGS.filenameFallbackFont[0] = '\0';
        filenameFontSystem.ensureLoaded(renderer);
        home->onFrontlightPanelClosed();
        queueStep("Filename Home refreshed after None", SmokeStep::Start, 20);
        break;
      }
      case 8: {
        RenderLock lock;
        captureStatusBarScreen("filename-home-none");
        auto* home = dynamic_cast<HomeActivity*>(activityManager.simulatorCurrentActivity());
        if (!home || renderer.filenameFontId(UI_10_FONT_ID) != UI_10_FONT_ID) fail("Filename None Home mismatch");
        home->onFrontlightPanelOpened();
        std::strncpy(SETTINGS.filenameFallbackFont, family, sizeof(SETTINGS.filenameFallbackFont) - 1);
        if (!filenameFontSystem.ensureLoaded(renderer)) fail("Filename Home reenable failed");
        home->onFrontlightPanelClosed();
        queueStep("Filename Home refreshed after selection", SmokeStep::Start, 20);
        break;
      }
      case 9: {
        RenderLock lock;
        captureStatusBarScreen("filename-home-reenabled");
        SETTINGS.librarySortMethod = 4;  // Recently Read includes TXT fixtures
        SETTINGS.libraryUseMetadata = 1;
        SETTINGS.libraryShowTxt = 1;
        SETTINGS.libraryHideFinishedBooks = 0;
        activityManager.replaceActivity(std::make_unique<LibraryActivity>(renderer, mappedInputManager));
        queueStep("Filename mixed Library", SmokeStep::Start, 12);
        break;
      }
      case 10: {
        RenderLock lock;
        captureStatusBarScreen("filename-library");
        activityManager.simulatorCurrentActivity()->startActivityForResult(
            std::make_unique<StatsUploadActivity>(renderer, mappedInputManager), [](const ActivityResult&) {});
        queueStep("UI font remains available to network child", SmokeStep::Start, 5);
        break;
      }
      case 11: {
        RenderLock lock;
        if (!renderer.getFontMap().at(UI_10_FONT_ID).hasCodepoint(0x4e00))
          fail("Network child lost translated UI fallback");
        captureStatusBarScreen("ui-translated-network");
        addTap(MappedInputManager::Button::Back);
        inputScript.push_back(render("Filename Library restored after network child", 6));
        inputScript.push_back(assertActivity("Library"));
        break;
      }
      case 12: {
        RenderLock lock;
        if (renderer.filenameFontId(UI_10_FONT_ID) == UI_10_FONT_ID) fail("Network child return lost filename font");
        captureStatusBarScreen("filename-library-after-network");
        sdFontSystem.releaseForNetwork(renderer);
        if (!renderer.getFontMap().at(UI_10_FONT_ID).hasCodepoint(0x4e00))
          fail("Network release removed shared UI glyphs");
        filenameFontSystem.suspend(renderer);
        filenameFontSystem.suspend(renderer);
        filenameFontSystem.resume();
        if (filenameFontSystem.ensureLoaded(renderer) || renderer.getFontMap().at(UI_10_FONT_ID).hasCodepoint(0x4e00))
          fail("Nested storage suspension reloaded UI font");
        filenameFontSystem.resume();
        if (!filenameFontSystem.ensureLoaded(renderer)) fail("UI font failed after storage release");
        const auto identity = filenameFontSystem.fingerprint();
        sdFontSystem.releaseLoadedFont(renderer);
        if (renderer.filenameFontId(UI_10_FONT_ID) == UI_10_FONT_ID || filenameFontSystem.fingerprint() != identity)
          fail("Reader font release changed filename fallback");
        // Simulate direct entry after a cold resume/network release: reader
        // metadata screens must load their own fallback, without visiting Home.
        SETTINGS.uiScale = CrossPointSettings::UI_SCALE_SMALL;
        UITheme::getInstance().reload();
        filenameFontSystem.release(renderer);
        auto epub = std::make_shared<Epub>(std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK"), "/.crosspoint");
        if (!epub->load()) fail("Metadata EPUB could not load");
        activityManager.replaceActivity(std::make_unique<EpubReaderChapterSelectionActivity>(
            renderer, mappedInputManager, epub, epub->getPath(), 0));
        queueStep("Metadata chapters after cold entry", SmokeStep::Start, 8);
        break;
      }
      case 13: {
        RenderLock lock;
        if (renderer.filenameFontId(UI_10_FONT_ID) == UI_10_FONT_ID) fail("Chapter entry did not load fallback");
        captureStatusBarScreen("metadata-chapters-small");
        SETTINGS.uiScale = CrossPointSettings::UI_SCALE_LARGE;
        UITheme::getInstance().reload();
        auto epub = std::make_shared<Epub>(std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK"), "/.crosspoint");
        if (!epub->load()) fail("Large metadata EPUB could not load");
        activityManager.replaceActivity(std::make_unique<EpubReaderChapterSelectionActivity>(
            renderer, mappedInputManager, epub, epub->getPath(), 0));
        queueStep("Metadata chapters large", SmokeStep::Start, 6);
        break;
      }
      case 14: {
        RenderLock lock;
        captureStatusBarScreen("metadata-chapters-large");
        if (!BOOKMARKS.loadForBook("/books/metadata.epub", "Metadata", "Author", "epub"))
          fail("Metadata bookmark store failed");
        BOOKMARKS.clearAll();
        BOOKMARKS.addBookmark(0, 0.5f, 1, "Chapter 一 Հայերեն", UINT16_MAX, "Latin title 一 Հայերեն");
        activityManager.replaceActivity(std::make_unique<EpubReaderBookmarkListActivity>(renderer, mappedInputManager));
        queueStep("Metadata bookmarks", SmokeStep::Start, 6);
        break;
      }
      case 15: {
        RenderLock lock;
        captureStatusBarScreen("metadata-bookmarks");
        if (!CLIPPINGS.loadForBook("/books/metadata.epub", "Latin 一 Հայերեն", "Author", "epub"))
          fail("Metadata clipping store failed");
        if (CLIPPINGS.addClipping(0, 0, 0, 1, 0, 4, 5, "Chapter 一 Հայերեն", 0,
                                  "Latin excerpt 一 Հայերեն with more words for wrapping.", 0,
                                  0) != ClippingStore::AddResult::Added)
          fail("Metadata clipping fixture failed");
        activityManager.replaceActivity(std::make_unique<EpubReaderClippingListActivity>(renderer, mappedInputManager));
        queueStep("Metadata clippings", SmokeStep::Start, 6);
        break;
      }
      case 16: {
        RenderLock lock;
        captureStatusBarScreen("metadata-clippings");
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("Metadata clipping detail", 6));
        break;
      }
      case 17: {
        RenderLock lock;
        captureStatusBarScreen("metadata-clipping-detail");
        activityManager.replaceActivity(std::make_unique<XtcReaderMenuActivity>(
            renderer, mappedInputManager, "Latin book title 一 Հայերեն", true, false, true, true));
        queueStep("Metadata book menu", SmokeStep::Start, 6);
        break;
      }
      case 18: {
        RenderLock lock;
        captureStatusBarScreen("metadata-book-menu");
        SETTINGS.filenameFallbackFont[0] = '\0';
        filenameFontSystem.ensureLoaded(renderer);
        activityManager.simulatorCurrentActivity()->requestUpdate();
        queueStep("Metadata book menu without fallback", SmokeStep::Start, 6);
        break;
      }
      case 19: {
        RenderLock lock;
        captureStatusBarScreen("metadata-book-menu-none");
        std::strcpy(SETTINGS.filenameFallbackFont, "Missing Family");
        if (filenameFontSystem.ensureLoaded(renderer) || renderer.filenameFontId(UI_10_FONT_ID) != UI_10_FONT_ID ||
            std::strcmp(SETTINGS.filenameFallbackFont, "Missing Family") != 0)
          fail("Missing family did not recover safely");
        std::strncpy(SETTINGS.filenameFallbackFont, family, sizeof(SETTINGS.filenameFallbackFont) - 1);
        if (!filenameFontSystem.ensureLoaded(renderer)) fail("Filename reload failed");
        SETTINGS.filenameFallbackFont[0] = '\0';
        if (!filenameFontSystem.ensureLoaded(renderer) || renderer.filenameFontId(UI_10_FONT_ID) != UI_10_FONT_ID)
          fail("None did not remove filename fallback");
        if (renderer.getFontMap().at(UI_10_FONT_ID).hasCodepoint(0x4e00))
          fail("None left shared UI fallback registered");
        // Settings must load the shared UI font even without a Home/Library
        // visit. --language-file can add missing characters to these labels.
        std::strncpy(SETTINGS.filenameFallbackFont, family, sizeof(SETTINGS.filenameFallbackFont) - 1);
        SETTINGS.uiScale = CrossPointSettings::UI_SCALE_SMALL;
        UITheme::getInstance().reload();
        activityManager.replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInputManager));
        queueStep("Translated Settings cold entry", SmokeStep::Start, 6);
        break;
      }
      case 20: {
        RenderLock lock;
        if (!renderer.getFontMap().at(UI_10_FONT_ID).hasCodepoint(0x4e00))
          fail("Cold Settings entry did not load UI font");
        captureStatusBarScreen("ui-translated-settings-small");
        SETTINGS.uiScale = CrossPointSettings::UI_SCALE_LARGE;
        UITheme::getInstance().reload();
        activityManager.replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInputManager));
        queueStep("Translated Settings large", SmokeStep::Start, 6);
        break;
      }
      case 21: {
        RenderLock lock;
        captureStatusBarScreen("ui-translated-settings-large");
        activityManager.simulatorCurrentActivity()->startActivityForResult(
            std::make_unique<UsbDriveActivity>(renderer, mappedInputManager), [](const ActivityResult&) {});
        queueStep("USB Drive suspends UI font", SmokeStep::Start, 6);
        break;
      }
      case 22: {
        RenderLock lock;
        if (renderer.getFontMap().at(UI_10_FONT_ID).hasCodepoint(0x4e00) || filenameFontSystem.ensureLoaded(renderer))
          fail("USB Drive reopened UI font while storage is handed off");
        activityManager.popActivity();
        queueStep("Translated Settings restored after USB", SmokeStep::Start, 6);
        break;
      }
      case 23: {
        RenderLock lock;
        if (!renderer.getFontMap().at(UI_10_FONT_ID).hasCodepoint(0x4e00))
          fail("Settings return did not restore UI fallback");
        captureStatusBarScreen("ui-translated-settings-after-usb");
        activityManager.simulatorCurrentActivity()->startActivityForResult(
            std::make_unique<CrossPointWebServerActivity>(renderer, mappedInputManager), [](const ActivityResult&) {});
        queueStep("File Transfer child keeps UI font suspended", SmokeStep::Start, 6);
        break;
      }
      case 24: {
        RenderLock lock;
        if (renderer.getFontMap().at(UI_10_FONT_ID).hasCodepoint(0x4e00) || filenameFontSystem.ensureLoaded(renderer))
          fail("File Transfer child reloaded the selected UI font");
        addTap(MappedInputManager::Button::Back);
        inputScript.push_back(render("Home restored after File Transfer cancellation", 8));
        break;
      }
      case 25: {
        RenderLock lock;
        if (!activityManager.simulatorCurrentActivity()->isHomeActivity() ||
            !renderer.getFontMap().at(UI_10_FONT_ID).hasCodepoint(0x4e00))
          fail("File Transfer exit did not restore UI fallback");
        SETTINGS.filenameFallbackFont[0] = '\0';
        filenameFontSystem.ensureLoaded(renderer);
        for (int id : {SMALL_FONT_ID, UI_10_FONT_ID, UI_12_FONT_ID}) {
          if (renderer.getFontMap().at(id).hasCodepoint(0x4e00)) fail("None retained borrowed UI glyphs");
        }
        LOG_INF("SMOKE",
                "Simulator smoke test passed: filename picker, mixed Latin/CJK, both UI sizes, Library, Home refresh, "
                "chapter lists, bookmarks, clippings, reader-menu titles, cold entry, persistence, storage "
                "suspension, network labels, shared UI fonts, Settings return and reader independence");
        std::_Exit(0);
      }
    }
#else
    fail("Filename font smoke requires S3 scalable fonts");
#endif
  }

  void tickHyphenation() {
    if (scriptIndex < inputScript.size()) {
      runReaderInputScript();
      return;
    }
    inputScript.clear();
    scriptIndex = 0;
    const char* savedStage = std::getenv("CROSSINK_HYPHENATION_SMOKE_STAGE");
    const int stage = savedStage ? std::atoi(savedStage) : 0;
    auto* manager = dynamic_cast<HyphenationManagerActivity*>(activityManager.simulatorCurrentActivity());
    if (stage == 2) {
      switch (hyphenationPhase++) {
        case 0: {
          const char* ui = std::getenv("CROSSINK_HYPHENATION_SMOKE_UI");
          if (ui && std::strcmp(I18N.getCode(), ui)) fail("Pack removal changed the UI language");
          if (Hyphenator::patternIdentity("de-DE") || !Hyphenator::patternIdentity("en") ||
              !HyphenationPackStore::hasSource("de"))
            fail("Pack removal changed the wrong resources");
          SETTINGS.hyphenationEnabled = true;
          activityManager.goToReader(std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK"), true);
          inputScript.push_back(render("Missing pack reader", 8));
          break;
        }
        case 1:
        case 4:
          if (!activityManager.isCurrentActivityNamed("Confirmation")) {
            --hyphenationPhase;
            break;  // Wait for the first laid-out page and its missing-pack prompt.
          }
          {
            RenderLock lock;
            captureStatusBarScreen("hyphenation-missing");
          }
          if (hyphenationPhase == 2) {
            addTap(MappedInputManager::Button::Back);
            inputScript.push_back(render("Read without missing pack", 12));
          } else {
            addTap(MappedInputManager::Button::Down);
            addTap(MappedInputManager::Button::Confirm);
            inputScript.push_back(render("Open manager from reader", 8));
          }
          break;
        case 2:
          if (!activityManager.isCurrentActivityNamed("EpubReader")) fail("Missing pack cancel did not resume reading");
          activityManager.goHome();
          inputScript.push_back(render("Home before new reader session", 4));
          break;
        case 3:
          activityManager.goToReader(std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK"), true);
          inputScript.push_back(render("New reader session", 8));
          break;
        case 5:
          if (!manager) fail("Missing pack prompt did not open its manager");
          if (!manager->allowPowerAsConfirmInReaderMode()) fail("Reader manager disabled Power-as-Confirm");
          addTap(MappedInputManager::Button::Back);
          inputScript.push_back(render("Reader after manager cancel", 12));
          break;
        default:
          if (!activityManager.isCurrentActivityNamed("EpubReader")) fail("Manager cancel did not resume reading");
          LOG_INF("SMOKE",
                  "Simulator smoke test passed: hyphenation install/remove reboots, UI language preservation, "
                  "missing-pack prompt and reader return");
          std::_Exit(0);
      }
      return;
    }
    switch (hyphenationPhase++) {
      case 0: {
        const char* ui = std::getenv("CROSSINK_HYPHENATION_SMOKE_UI");
        if (ui && std::strcmp(I18N.getCode(), ui)) fail("Hyphenation update changed the UI language");
        if (HyphenationPackStore::isInstalled("de") != (stage == 1)) fail("Hyphenation boot selection mismatch");
        if (!HyphenationPackStore::hasSource("de")) fail("Hyphenation source was removed");
        if (!Hyphenator::patternIdentity("en")) fail("Built-in English unavailable");
        if (stage == 0 && HyphenationPackStore::install("fr") != hyphenation_pack::Result::Invalid)
          fail("A renamed pack bypassed language identity validation");
        if (stage == 1) {
          Hyphenator::setPreferredLanguage("GER");
          if (Hyphenator::breakOffsets("Satellitensystems", false).empty()) fail("Installed German patterns unused");
        }
        const auto settings = buildReaderPageLayoutSettingsList(getSettingsList());
        auto row = std::find_if(settings.begin(), settings.end(),
                                [](const auto& setting) { return setting.nameId == StrId::STR_HYPHENATION; });
        if (row == settings.end() || ++row == settings.end() || row->action != SettingAction::ManageHyphenation)
          fail("Hyphenation manager is not next to its setting");
        activityManager.replaceActivity(
            std::make_unique<HyphenationManagerActivity>(renderer, mappedInputManager, "de"));
        inputScript.push_back(render("Hyphenation language list", 5));
        break;
      }
      case 1: {
        if (!manager || manager->simulatorOptionDisabled(0)) fail("Built-in English is crossed out");
        const char* expected = stage ? tr(STR_INSTALLED) : tr(STR_HYPHENATION_INSTALL_ACTION);
        if (!std::strstr(manager->simulatorLabel(2), expected) ||
            std::strstr(manager->simulatorLabel(2), tr(STR_ASSET_USE)))
          fail("Hyphenation row status does not match pack state");
        {
          RenderLock lock;
          captureStatusBarScreen(stage ? "hyphenation-installed" : "hyphenation-sd");
        }
        // A pack with an SD source installs directly, with no actions menu.
        if (!stage) setenv("CROSSINK_HYPHENATION_SMOKE_STAGE", "1", 1);
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render(stage ? "Hyphenation actions" : "Install hyphenation from SD", stage ? 4 : 10));
        break;
      }
      case 2:
        if (!stage) fail("Hyphenation install from SD did not restart");
        if (!manager || std::strcmp(manager->simulatorOptionLabel(0), tr(STR_UPDATE)) ||
            std::strcmp(manager->simulatorOptionLabel(1), tr(STR_DELETE)) ||
            std::strcmp(manager->simulatorOptionLabel(2), tr(STR_CANCEL)))
          fail("Installed pack does not offer Update, Delete and Cancel");
        {
          RenderLock lock;
          captureStatusBarScreen("hyphenation-remove");
        }
        addTap(MappedInputManager::Button::Back);
        inputScript.push_back(render("Cancel hyphenation actions", 4));
        break;
      case 3:
        if (!HyphenationPackStore::isInstalled("de")) fail("Cancel changed installed hyphenation");
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("Reopen hyphenation actions", 4));
        break;
      case 4:
        setenv("CROSSINK_HYPHENATION_SMOKE_STAGE", "2", 1);
        addTap(MappedInputManager::Button::Up);  // Cancel -> Delete
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("Delete hyphenation pack", 10));
        break;
      default:
        fail("Hyphenation operation did not restart");
    }
  }

  void tickCalibration() {
    if (scriptIndex < inputScript.size()) {
      runReaderInputScript();
      return;
    }
    inputScript.clear();
    scriptIndex = 0;
    const auto renderCapture = [&](const char* name) {
      RenderLock lock;
      captureStatusBarScreen(name);
    };
    const auto tapControl = [&](ScreenCalibrationActivity& editor, int value) {
#if CROSSINK_APP_CAP_TOUCH
      freeink::ui::Interaction hit;
      bool found = false;
      for (int y = 48; y < renderer.getScreenHeight() - 48 && !found; y += 4) {
        for (int x = 48; x < renderer.getScreenWidth() - 48; x += 4) {
          if (editor.simulatorHit(x, y, hit) && hit.value == value) {
            found = true;
            break;
          }
        }
      }
      if (!found) fail("Calibration touch target not published: %d", value);
      inputScript.push_back(touchDown(hit.rect.x + hit.rect.width / 2, hit.rect.y + hit.rect.height / 2));
      inputScript.push_back(touchRelease(hit.rect.x + hit.rect.width / 2, hit.rect.y + hit.rect.height / 2));
#else
      (void)editor;
      (void)value;
#endif
    };
    const auto openTimePicker = [&] {
      {
        RenderLock lock;
        SETTINGS.uiTheme = calibrationPickerPass == 0 ? CrossPointSettings::CLASSIC : CrossPointSettings::ROUNDEDRAFF;
        SETTINGS.uiScale = CrossPointSettings::UI_SCALE_SMALL;
        UITheme::getInstance().reload();
        renderer.setOrientation(GfxRenderer::Portrait);
        renderer.setViewableInsets(ScreenInsets{{{32, 32, 32, 32}}});
      }
      calibrationPickerResult = -1;
      activityManager.simulatorCurrentActivity()->startActivityForResult(
          std::make_unique<FrontlightTimePickerActivity>(renderer, mappedInputManager, StrId::STR_FRONTLIGHT_SCHEDULE,
                                                         8 * 60 + 3),
          [this](const ActivityResult& result) {
            if (!result.isCancelled && std::holds_alternative<IntervalResult>(result.data))
              calibrationPickerResult = static_cast<int>(std::get<IntervalResult>(result.data).value);
          });
      queueStep("Calibration time picker entry", SmokeStep::Start, 4);
    };
    const auto tapTimePicker = [&](const int16_t value) {
      auto* picker = dynamic_cast<FrontlightTimePickerActivity*>(activityManager.simulatorCurrentActivity());
      RenderLock lock;
      freeink::ui::Interaction hit;
      if (!picker || !ScreenCalibrationSmokeTest::timePickerHit(*picker, value, hit))
        fail("Calibration time picker key %d or field bounds failed", value);
#if CROSSINK_APP_CAP_TOUCH
      inputScript.push_back(touchDown(hit.rect.x + hit.rect.width / 2, hit.rect.y + hit.rect.height / 2));
      inputScript.push_back(touchRelease(hit.rect.x + hit.rect.width / 2, hit.rect.y + hit.rect.height / 2));
#endif
      inputScript.push_back(render("Calibration time picker touch", 4));
    };
    switch (calibrationPhase++) {
      case 0: {
        {
          RenderLock lock;
          renderer.setOrientation(GfxRenderer::LandscapeClockwise);
        }
        activityManager.replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInputManager));
        queueStep("Calibration Settings entry", SmokeStep::Start, 4);
        break;
      }
      case 1: {
        const auto settings = buildGroupedDisplaySettingsList(getSettingsList());
        const auto it = std::find_if(settings.begin(), settings.end(), [](const auto& setting) {
          return setting.action == SettingAction::ScreenCalibration;
        });
        if (it == settings.end()) fail("Calibration missing from Display Settings");
        for (auto i = settings.begin(); i <= it; ++i)
          addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("Calibration opened from Settings", 4));
        inputScript.push_back(assertActivity("ScreenCalibration"));
        break;
      }
      case 2: {
        auto* editor = dynamic_cast<ScreenCalibrationActivity*>(activityManager.simulatorCurrentActivity());
        if (!editor || renderer.getOrientation() != GfxRenderer::Portrait) fail("Calibration portrait entry failed");
        renderCapture("calibration-initial");
        calibrationExpected = renderer.getViewableInsets();
        addTap(MappedInputManager::Button::Confirm);
        for (int i = 0; i < 12; ++i) addTap(MappedInputManager::Button::Up);
        for (int i = 0; i < 20; ++i) addTap(MappedInputManager::Button::Down);
        addTap(MappedInputManager::Button::Confirm);
        addTap(MappedInputManager::Button::Down);
        addTap(MappedInputManager::Button::Confirm);
        for (int i = 0; i < 5; ++i) addTap(MappedInputManager::Button::Down);
        addTap(MappedInputManager::Button::Confirm);
        calibrationExpected.edges[0] = 20;
        calibrationExpected.adjust(1, 5);
        inputScript.push_back(render("Calibration button draft", 4));
        break;
      }
      case 3: {
        auto* editor = dynamic_cast<ScreenCalibrationActivity*>(activityManager.simulatorCurrentActivity());
        if (!editor || editor->simulatorDraft() != calibrationExpected ||
            renderer.getViewableInsets() != ScreenInsets{})
          fail("Calibration draft changed live geometry");
        renderCapture("calibration-edited-buttons");
        for (int i = 0; i < 4; ++i) addTap(MappedInputManager::Button::Down);
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("Calibration saved back to Settings", 4));
        inputScript.push_back(assertActivity("Settings"));
        break;
      }
      case 4:
        if (renderer.getViewableInsets() != calibrationExpected ||
            HalScreenCalibration::load() != calibrationExpected ||
            renderer.getOrientation() != GfxRenderer::LandscapeClockwise)
          fail("Calibration save or orientation restore failed");
        {
          const auto* settings = dynamic_cast<SettingsActivity*>(activityManager.simulatorCurrentActivity());
          if (!settings) fail("Calibration did not return to Settings");
          const auto safe = settings->simulatorSafeRect();
          const auto expected = calibrationExpected.rotated(static_cast<unsigned>(renderer.getOrientation()));
          if (safe.x != expected.edges[3] || safe.y != expected.edges[0] ||
              safe.width != renderer.getScreenWidth() - expected.edges[1] - expected.edges[3] ||
              safe.height != renderer.getScreenHeight() - expected.edges[0] - expected.edges[2])
            fail("Settings kept its old calibration safe area");
        }
        renderCapture("calibration-save-return");
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("Calibration reopened", 4));
        break;
      case 5: {
        auto* editor = dynamic_cast<ScreenCalibrationActivity*>(activityManager.simulatorCurrentActivity());
        if (!editor || editor->simulatorDraft() != calibrationExpected)
          fail("Calibration reopen lost persisted values");
        for (int i = 0; i < 4; ++i) addTap(MappedInputManager::Button::Down);
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("Calibration reset draft", 4));
        break;
      }
      case 6: {
        auto* editor = dynamic_cast<ScreenCalibrationActivity*>(activityManager.simulatorCurrentActivity());
        if (!editor || editor->simulatorDraft() != ScreenInsets{} ||
            renderer.getViewableInsets() != calibrationExpected || HalScreenCalibration::load() != calibrationExpected)
          fail("Calibration Reset was not transactional");
        renderCapture("calibration-reset-draft");
        addTap(MappedInputManager::Button::Down);
        addTap(MappedInputManager::Button::Down);
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("Calibration cancelled", 4));
        inputScript.push_back(assertActivity("Settings"));
        break;
      }
      case 7:
        if (renderer.getViewableInsets() != calibrationExpected ||
            HalScreenCalibration::load() != calibrationExpected ||
            renderer.getOrientation() != GfxRenderer::LandscapeClockwise)
          fail("Calibration Cancel changed saved geometry");
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("Calibration touch entry", 4));
        break;
      case 8: {
        auto* editor = dynamic_cast<ScreenCalibrationActivity*>(activityManager.simulatorCurrentActivity());
        if (!editor) fail("Calibration touch entry failed");
        if (mappedInputManager.hasTouch()) {
          tapControl(*editor, 12);  // Right edge increase.
          calibrationExpected.adjust(1, 1);
        } else {
          addTap(MappedInputManager::Button::Confirm);
          addTap(MappedInputManager::Button::Down);
          addTap(MappedInputManager::Button::Confirm);
          calibrationExpected.adjust(0, 1);
        }
        inputScript.push_back(render("Calibration touch draft", 4));
        break;
      }
      case 9: {
        auto* editor = dynamic_cast<ScreenCalibrationActivity*>(activityManager.simulatorCurrentActivity());
        if (!editor || editor->simulatorDraft() != calibrationExpected) fail("Calibration touch adjustment failed");
        renderCapture("calibration-edited-touch");
        if (mappedInputManager.hasTouch())
          tapControl(*editor, 5);
        else {
          for (int i = 0; i < 5; ++i) addTap(MappedInputManager::Button::Down);
          addTap(MappedInputManager::Button::Confirm);
        }
        inputScript.push_back(render("Calibration touch saved", 4));
        break;
      }
      case 10: {
        if (!activityManager.isCurrentActivityNamed("Settings") ||
            renderer.getViewableInsets() != calibrationExpected || HalScreenCalibration::load() != calibrationExpected)
          fail("Calibration touch save failed");
        {
          RenderLock lock;
          SETTINGS.uiScale = CrossPointSettings::UI_SCALE_LARGE;
        }
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("Calibration current language large UI", 4));
        break;
      }
      case 11: {
        auto* editor = dynamic_cast<ScreenCalibrationActivity*>(activityManager.simulatorCurrentActivity());
        if (!editor) fail("Calibration localized reopen failed");
        renderCapture("calibration-localized-large");
        const char* storage = std::getenv("CROSSINK_CALIBRATION_PATH");
        calibrationStoragePath = storage ? storage : ".screen-calibration.nvs";
        setenv("CROSSINK_CALIBRATION_PATH", "/missing-calibration-directory/settings", 1);
        if (mappedInputManager.hasTouch()) {
          tapControl(*editor, 11);
          inputScript.push_back(render("Calibration unsaved touch edit", 4));
          tapControl(*editor, 5);
        } else {
          addTap(MappedInputManager::Button::Confirm);
          addTap(MappedInputManager::Button::Down);
          addTap(MappedInputManager::Button::Confirm);
          for (int i = 0; i < 5; ++i) addTap(MappedInputManager::Button::Down);
          addTap(MappedInputManager::Button::Confirm);
        }
        inputScript.push_back(render("Calibration save error", 4));
        break;
      }
      case 12: {
        auto* editor = dynamic_cast<ScreenCalibrationActivity*>(activityManager.simulatorCurrentActivity());
        if (!editor || !editor->simulatorSaveFailed() || renderer.getViewableInsets() != calibrationExpected)
          fail("Calibration save error changed live geometry or dismissed the editor");
        renderCapture("calibration-save-failed");
        setenv("CROSSINK_CALIBRATION_PATH", calibrationStoragePath.c_str(), 1);
        if (HalScreenCalibration::load() != calibrationExpected) fail("Failed save changed persisted geometry");
        if (mappedInputManager.hasTouch())
          tapControl(*editor, 6);
        else
          addTap(MappedInputManager::Button::Back);
        inputScript.push_back(render("Calibration save failure cancelled", 4));
        break;
      }
      case 13: {
        if (!activityManager.isCurrentActivityNamed("Settings"))
          fail("Calibration error cancel did not restore Settings");
        RenderLock lock;
        if (!ScreenCalibrationSmokeTest::safeAreaContract(
                lock, [this](const std::string& name) { captureStatusBarScreen(name.c_str()); }))
          fail("Calibration default/custom UI boundary contract failed");
        if (!ScreenCalibrationSmokeTest::geometry(
                lock, [this](const std::string& name) { captureStatusBarScreen(name.c_str()); }))
          fail("Calibration maximum headers, status, touch targets, or list bounds failed");
        if (!ScreenCalibrationSmokeTest::statusBarOrigin(lock))
          fail("Calibration status bar origin differs between screens");
        if (!ScreenCalibrationSmokeTest::keyboardAndHints(
                lock, [this](const std::string& name) { captureStatusBarScreen(name.c_str()); }))
          fail("Calibration hints, keyboard edge keys, or font picker resume failed");
        for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise,
                                       GfxRenderer::PortraitInverted, GfxRenderer::LandscapeCounterClockwise}) {
          renderer.setOrientation(orientation);
          renderer.clearScreen();
          const auto safe = UITheme::getInstance().getScreenSafeArea(renderer);
          const auto expected = calibrationExpected.rotated(static_cast<unsigned>(orientation));
          if (safe.x != expected.edges[3] || safe.y != expected.edges[0] ||
              safe.width != renderer.getScreenWidth() - expected.edges[1] - expected.edges[3] ||
              safe.height != renderer.getScreenHeight() - expected.edges[0] - expected.edges[2])
            fail("Calibration safe area did not rotate");
          renderer.drawRect(safe.x, safe.y, safe.width, safe.height);
          GUI.drawHeader(renderer, Rect{safe.x, safe.y + 40, safe.width, 100}, tr(STR_SCREEN_CALIBRATION));
          captureStatusBarScreen(("calibration-orientation-" + std::to_string(static_cast<int>(orientation))).c_str());
        }
        renderer.setOrientation(GfxRenderer::Portrait);
        if (!ScreenCalibrationSmokeTest::txt(lock))
          fail("TXT calibration reflow or reading-position preservation failed");
        captureStatusBarScreen("calibration-txt-reflow");
        lock.unlock();
        activityManager.goToReader(std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK"));
        queueStep("Calibration EPUB entry", SmokeStep::Start, 5);
        break;
      }
      case 14: {
        auto* reader = dynamic_cast<EpubReaderActivity*>(activityManager.simulatorCurrentActivity());
        if (!reader || !ScreenCalibrationSmokeTest::epubReady(*reader)) {
          --calibrationPhase;
          queueStep("Calibration EPUB ready", SmokeStep::Start, 4);
          break;
        }
        renderCapture("calibration-epub-before-reflow");
        if (!ScreenCalibrationSmokeTest::reflowEpub(*reader))
          fail("EPUB did not invalidate changed calibration dimensions");
        queueStep("Calibration EPUB reflow", SmokeStep::Start, 5);
        break;
      }
      case 15: {
        auto* reader = dynamic_cast<EpubReaderActivity*>(activityManager.simulatorCurrentActivity());
        if (!reader || !ScreenCalibrationSmokeTest::epubReady(*reader)) {
          --calibrationPhase;
          queueStep("Calibration EPUB reflow complete", SmokeStep::Start, 4);
          break;
        }
        RenderLock lock;
        if (!ScreenCalibrationSmokeTest::reflowedEpub(*reader))
          fail("EPUB calibration reflow lost position or viewport");
        captureStatusBarScreen("calibration-epub-after-reflow");
        lock.unlock();
        activityManager.goHome();
        queueStep("Calibration Home resume entry", SmokeStep::Start, 4);
        break;
      }
      case 16: {
        auto* home = dynamic_cast<HomeActivity*>(activityManager.simulatorCurrentActivity());
        if (!home) fail("Calibration Home entry failed");
        calibrationResumedHome = home;
        home->onFrontlightPanelOpened();
        {
          RenderLock lock;
          renderer.setViewableInsets(ScreenInsets{{{32, 32, 32, 32}}});
        }
        home->onFrontlightPanelClosed();
        queueStep("Calibration Home geometry rebuilt", SmokeStep::Start, 4);
        break;
      }
      case 17: {
        if (!activityManager.isCurrentActivityNamed("Home") ||
            activityManager.simulatorCurrentActivity() == calibrationResumedHome)
          fail("Home did not recreate layout after calibration returned from panel");
        renderCapture("calibration-home-resume-max32");
        activityManager.goToFileBrowser("/books");
        queueStep("Calibration browser resume entry", SmokeStep::Start, 4);
        break;
      }
      case 18: {
        auto* browser = dynamic_cast<FileBrowserActivity*>(activityManager.simulatorCurrentActivity());
        if (!browser || browser->simulatorSafeRect().x != 32) fail("Browser initial calibration bounds failed");
        {
          RenderLock lock;
          renderer.setViewableInsets(ScreenInsets{});
        }
        browser->onFrontlightPanelClosed();
        queueStep("Calibration browser defaults restored", SmokeStep::Start, 4);
        break;
      }
      case 19: {
        auto* browser = dynamic_cast<FileBrowserActivity*>(activityManager.simulatorCurrentActivity());
        if (!browser || browser->simulatorSafeRect().x != ScreenInsets{}.edges[3] ||
            browser->simulatorSafeRect().y != ScreenInsets{}.edges[0])
          fail("Browser retained old SDK bounds after calibration reset");
        renderCapture("calibration-browser-resume-reset");
        if (mappedInputManager.hasTouchHardware()) {
          openTimePicker();
          break;
        }
        LOG_INF("SMOKE",
                "Simulator smoke test passed: calibration buttons/touch, persistence/errors, localization, rotations, "
                "headers/list bounds, suspended Home/browser/menu, TXT/EPUB reflow");
        std::_Exit(0);
      }
      case 20:
        renderCapture(calibrationPickerPass == 0 ? "calibration-time-picker-classic-max32"
                                                 : "calibration-time-picker-roundedraff-max32");
        tapTimePicker(1);
        break;
      case 21:
        // The pending first digit enables the adjacent backspace target.
        {
          auto* picker = dynamic_cast<FrontlightTimePickerActivity*>(activityManager.simulatorCurrentActivity());
          RenderLock lock;
          freeink::ui::Interaction hit;
          if (!picker || !ScreenCalibrationSmokeTest::timePickerHit(*picker, -1, hit))
            fail("Calibration time picker backspace bounds failed");
        }
        tapTimePicker(-1);
        break;
      case 22:
        tapTimePicker(0);
        break;
      case 23: {
        const auto* picker = dynamic_cast<FrontlightTimePickerActivity*>(activityManager.simulatorCurrentActivity());
        if (!picker || !ScreenCalibrationSmokeTest::timePickerHourIsTwelve(*picker))
          fail("Calibration time picker 0 did not select 12 AM");
        tapTimePicker(-2);
        break;
      }
      case 24:
        if (!activityManager.isCurrentActivityNamed("FileBrowser") || calibrationPickerResult != 3)
          fail("Calibration time picker OK did not return 12:03 AM");
        if (++calibrationPickerPass < 2) {
          calibrationPhase = 20;
          openTimePicker();
          break;
        }
        LOG_INF("SMOKE", "Simulator smoke test passed: calibration and max32 Classic/RoundedRaff time picker touch");
        std::_Exit(0);
    }
  }

  void tickReceiveFolder() {
    if (scriptIndex < inputScript.size()) {
      runReaderInputScript();
      return;
    }
    inputScript.clear();
    scriptIndex = 0;
    static constexpr const char* paths[] = {"/", "/receive", "/receive/empty"};
    switch (receiveFolderPhase++) {
      case 0: {
        Storage.mkdir("/receive");
        Storage.mkdir("/receive/empty");
        {
          RenderLock lock;
          SETTINGS.uiTheme = receiveFolderPass < 8 ? CrossPointSettings::CLASSIC : CrossPointSettings::LYRA;
          SETTINGS.uiScale =
              receiveFolderPass % 2 ? CrossPointSettings::UI_SCALE_LARGE : CrossPointSettings::UI_SCALE_SMALL;
          renderer.setOrientation(static_cast<GfxRenderer::Orientation>((receiveFolderPass / 2) % 4));
          renderer.setViewableInsets(ScreenInsets{{{32, 32, 32, 32}}});
          UITheme::getInstance().reload();
        }
        receiveFolderReturned = false;
        receiveFolderResult.clear();
        const char* path = receiveFolderPass == 18   ? "/receive"
                           : receiveFolderPass == 19 ? "/receive/empty"
                           : receiveFolderPass >= 16 ? "/"
                                                     : paths[receiveFolderPass % 3];
        activityManager.simulatorCurrentActivity()->startActivityForResult(
            std::make_unique<FileBrowserActivity>(renderer, mappedInputManager, path,
                                                  FileBrowserActivity::Mode::PickDirectory),
            [this](const ActivityResult& result) {
              receiveFolderReturned = true;
              receiveFolderCancelled = result.isCancelled;
              if (const auto* selected = std::get_if<FilePathResult>(&result.data))
                receiveFolderResult = selected->path;
            });
        queueStep("Receive folder picker opened", SmokeStep::Start, 4);
        break;
      }
      case 1: {
        auto* picker = dynamic_cast<FileBrowserActivity*>(activityManager.simulatorCurrentActivity());
        if (!picker) fail("Receive folder picker missing");
        if ((receiveFolderPass == 18 && picker->simulatorFolderPath() == "/receive") ||
            (receiveFolderPass == 19 && picker->simulatorFolderPath() == "/receive/empty")) {
          addTap(receiveFolderPass == 18 ? MappedInputManager::Button::Confirm : MappedInputManager::Button::Back);
          inputScript.push_back(render("Receive folder navigation", 4));
          receiveFolderPhase = 1;
          break;
        }
        {
          RenderLock lock;
          captureStatusBarScreen(("receive-folder-" + std::to_string(receiveFolderPass)).c_str());
        }
        if (receiveFolderPass == 16) {
          addTap(MappedInputManager::Button::Back);
        } else if (receiveFolderPass == 17 || !mappedInputManager.hasTouchHardware()) {
          addTap(MappedInputManager::Button::Right);
        } else {
#if CROSSINK_APP_CAP_TOUCH
          const auto button = picker->simulatorDirectorySelectRect();
          const auto safe = picker->simulatorSafeRect();
          if (button.width <= 0 || button.height < 44 || button.x < safe.x || button.y < safe.y ||
              button.right() > safe.right() || button.bottom() > safe.bottom())
            fail("Receive folder Select button missing or outside safe bounds");
          const int x = button.x + button.width / 2;
          const int y = button.y + button.height / 2;
          inputScript = {touchDown(x, y), touchRelease(x, y)};
#endif
        }
        inputScript.push_back(render("Receive folder picker returned", 4));
        break;
      }
      case 2:
        if (!receiveFolderReturned || receiveFolderCancelled != (receiveFolderPass == 16))
          fail("Receive folder selection/cancellation did not return to caller");
        if (receiveFolderPass != 16 &&
            receiveFolderResult != (receiveFolderPass == 18   ? "/receive/empty"
                                    : receiveFolderPass == 19 ? "/receive"
                                    : receiveFolderPass >= 16 ? "/"
                                                              : paths[receiveFolderPass % 3]))
          fail("Receive folder picker returned wrong directory");
        if (++receiveFolderPass < 20) {
          receiveFolderPhase = 0;
          queueStep("Next receive folder layout", SmokeStep::Start, 2);
        } else {
          LOG_INF("SMOKE",
                  "Simulator smoke test passed: receive folder touch selection, empty/root folders, "
                  "navigation, cancellation and physical selection across themes/scales/rotations");
          std::_Exit(0);
        }
        break;
    }
  }

  // What's New: shown once over Home, Close marks it seen and saves, Settings reopens it without touching that state.
  void tickReleaseNotes() {
    if (scriptIndex < inputScript.size()) {
      runReaderInputScript();
      return;
    }
    inputScript.clear();
    scriptIndex = 0;
    switch (releaseNotesPhase) {
      case 0: {
        // A fresh state file has revision 0, so Home hands over to What's New on its own.
        if (!activityManager.isCurrentActivityNamed("ReleaseNotes")) {
          if (++releaseNotesWait > 600) fail("What's New was not shown over Home on a fresh state");
          return;
        }
        auto* notes = dynamic_cast<ReleaseNotesActivity*>(activityManager.simulatorCurrentActivity());
        if (!notes) fail("What's New activity type mismatch");
        if (APP_STATE.releaseNotesSeen != 0) fail("What's New was marked seen before it was dismissed");
        if (std::strcmp(I18N.getCode(), "EN")) fail("Release notes smoke expects the English UI");
        if (!I18N.hasTranslation(StrId::STR_WHATS_NEW) || releaseNotesNeedLanguageUpdate() ||
            notes->simulatorUpdateOffered())
          fail("English must offer Install Languages, not Update");
        if (notes->simulatorPageCount() < 1) fail("What's New has no pages");
        if (notes->simulatorPageCount() > 1) {
          addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
          addTap(mappedInputManager.menuButton(MappedInputManager::Button::Up));
        }
        inputScript.push_back(render("What's New over Home", 4));
        releaseNotesPhase = 1;
        break;
      }
      case 1:
        captureStatusBarScreen("release-notes-first-run");
        addTap(MappedInputManager::Button::Back);
        inputScript.push_back(render("What's New closed", 6));
        releaseNotesPhase = 2;
        releaseNotesWait = 0;
        break;
      case 2: {
        if (activityManager.isCurrentActivityNamed("ReleaseNotes")) fail("What's New did not close");
        if (APP_STATE.releaseNotesSeen != RELEASE_NOTES_REVISION) fail("Close did not mark What's New seen");
        const String saved = Storage.readFile(CrossPointState::getFilePath());
        JsonDocument doc;
        if (deserializeJson(doc, saved) || doc["releaseNotesSeen"].as<int>() != RELEASE_NOTES_REVISION)
          fail("Close did not save releaseNotesSeen");
        if (++releaseNotesWait < 60) return;  // Stay on Home long enough for a repeat push to show up.
        if (!activityManager.isCurrentActivityNamed("Home")) fail("Expected Home after closing What's New");
        // Reopen from Settings with an unseen revision: it must not touch the stored value.
        APP_STATE.releaseNotesSeen = 0;
        activityManager.goToSettings();
        queueStep("Settings for Release Notes", SmokeStep::Start, 4);
        releaseNotesPhase = 3;
        break;
      }
      case 3: {
        const auto settings = buildSystemSettingsParentList(getSettingsList());
        if (settings.size() < 2 || settings[settings.size() - 2].action != SettingAction::ReleaseNotes ||
            settings.back().action != SettingAction::About)
          fail("Release Notes row is not directly above About");
        for (int i = 0; i < 3; ++i) addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("Release Notes System tab", 3));
        for (size_t i = 0; i < settings.size(); ++i)
          addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
        addTap(mappedInputManager.menuButton(MappedInputManager::Button::Up));
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("Release Notes opened from Settings", 4));
        inputScript.push_back(assertActivity("ReleaseNotes"));
        releaseNotesPhase = 4;
        break;
      }
      case 4:
        if (APP_STATE.releaseNotesSeen != 0) fail("Opening Release Notes from Settings changed the seen revision");
        captureStatusBarScreen("release-notes-from-settings");
        addTap(MappedInputManager::Button::Back);
        inputScript.push_back(render("Release Notes closed to Settings", 4));
        inputScript.push_back(assertActivity("Settings"));
        releaseNotesPhase = 5;
        break;
      case 5:
        if (APP_STATE.releaseNotesSeen != 0) fail("Leaving Release Notes from Settings changed the seen revision");
        // Saved German that is not installed (upgrade from built-in translations) gets one-click Update,
        // until an update was tried for this revision. English and unknown custom codes never do.
        if (!releaseNotesOfferLanguageUpdate("DE", "EN", 0)) fail("Saved but inactive German should offer Update");
        if (releaseNotesOfferLanguageUpdate("DE", "EN", RELEASE_NOTES_REVISION))
          fail("Update must not be offered again after it was tried");
        if (releaseNotesOfferLanguageUpdate("EN", "EN", 0)) fail("English must not offer Update");
        if (releaseNotesOfferLanguageUpdate("XX-CUSTOM", "EN", 0)) fail("Custom languages must not offer Update");
        activityManager.goHome();
        queueStep("Home before What's New Install Languages", SmokeStep::Start, 4);
        releaseNotesPhase = 6;
        break;
      case 6:
        // FirstRun Install Languages: Settings > System > Device with the Download Languages popup, and the notes seen.
        if (!activityManager.isCurrentActivityNamed("Home")) fail("Expected Home before What's New Install Languages");
        saveReleaseNotesState(0, RELEASE_NOTES_REVISION);
        openFirstRunReleaseNotes("What's New for Install Languages");
        releaseNotesPhase = 7;
        break;
      case 7:
        if (currentReleaseNotes().simulatorUpdateOffered()) fail("English What's New must offer Install Languages");
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("What's New Install Languages", 6));
        inputScript.push_back(assertActivity("Settings"));
        releaseNotesPhase = 8;
        break;
      case 8: {
        auto* settings = dynamic_cast<SettingsActivity*>(activityManager.simulatorCurrentActivity());
        if (!settings) fail("Install Languages did not open Settings");
        if (settings->simulatorActiveSubmenu() != SettingAction::SystemDevice)
          fail("Install Languages did not land in System > Device");
        if (!settings->simulatorLanguageDownloadsOpen()) fail("Install Languages did not open Download Languages");
        if (APP_STATE.releaseNotesSeen != RELEASE_NOTES_REVISION ||
            savedStateValue("releaseNotesSeen") != RELEASE_NOTES_REVISION)
          fail("Install Languages did not save releaseNotesSeen");
        {
          RenderLock lock;
          captureStatusBarScreen("release-notes-download-languages");
        }
        activityManager.goHome();
        queueStep("Home before What's New Update", SmokeStep::Start, 4);
        releaseNotesPhase = 9;
        break;
      }
      case 9:
        // Update: a saved German preference that is not active offers the one-click language download.
        if (!activityManager.isCurrentActivityNamed("Home")) fail("Expected Home before What's New Update");
        std::snprintf(SETTINGS.languageCode, sizeof(SETTINGS.languageCode), "%s", "DE");
        saveReleaseNotesState(0, 0);
        capturedNetworkRestart = CapturedNetworkRestart{};
        capturedNetworkRestart.armed = true;
        openFirstRunReleaseNotes("What's New for Update");
        releaseNotesPhase = 10;
        break;
      case 10:
        if (!currentReleaseNotes().simulatorUpdateOffered()) fail("Saved inactive German must offer Update");
        {
          RenderLock lock;
          captureStatusBarScreen("release-notes-update-offered");
        }
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("What's New Update", 4));
        releaseNotesPhase = 11;
        break;
      case 11: {
        const uint32_t payload = AssetDownloadActivity::bootPayload(AssetCatalog::Kind::Translation,
                                                                    static_cast<uint8_t>(I18n::languageFromCode("DE")));
        if (capturedNetworkRestart.count != 1 ||
            capturedNetworkRestart.target != static_cast<uint32_t>(NetworkBootTarget::DOWNLOAD_ASSETS) ||
            capturedNetworkRestart.payload != payload)
          fail("Update did not request one German language download reboot (count=%d target=%u payload=%u)",
               capturedNetworkRestart.count, capturedNetworkRestart.target, capturedNetworkRestart.payload);
        if (APP_STATE.releaseNotesUpdateTried != RELEASE_NOTES_REVISION ||
            savedStateValue("releaseNotesUpdateTried") != RELEASE_NOTES_REVISION)
          fail("Update did not save releaseNotesUpdateTried");
        if (APP_STATE.releaseNotesSeen != 0 || savedStateValue("releaseNotesSeen") != 0)
          fail("Update marked What's New seen before the translated notes were shown");
        // A real device has rebooted by now; leaving must still not mark the notes seen.
        addTap(MappedInputManager::Button::Back);
        inputScript.push_back(render("What's New closed after Update", 4));
        releaseNotesPhase = 12;
        break;
      }
      case 12:
        if (APP_STATE.releaseNotesSeen != 0 || savedStateValue("releaseNotesSeen") != 0)
          fail("Leaving after Update marked What's New seen");
        capturedNetworkRestart.armed = false;
        std::snprintf(SETTINGS.languageCode, sizeof(SETTINGS.languageCode), "%s", "EN");
        if (!SETTINGS.saveToFile()) fail("Cannot restore the English language setting");
#if CROSSINK_APP_CAP_TOUCH
        releaseNotesPhase = 13;
#else
        releaseNotesPhase = 17;
#endif
        activityManager.goHome();
        queueStep("Home after What's New Update", SmokeStep::Start, 4);
        break;
#if CROSSINK_APP_CAP_TOUCH
      case 13:
        // Touch: only a completed tap on Later dismisses; a swipe that starts on the buttons pages.
        if (!activityManager.isCurrentActivityNamed("Home")) fail("Expected Home before What's New touch checks");
        saveReleaseNotesState(0, RELEASE_NOTES_REVISION);
        {
          RenderLock lock;
          const GfxRenderer::Orientation orientations[] = {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise,
                                                           GfxRenderer::LandscapeCounterClockwise,
                                                           GfxRenderer::PortraitInverted};
          renderer.setOrientation(orientations[releaseNotesPass % 4]);
        }
        openFirstRunReleaseNotes("What's New for touch");
        releaseNotesPhase = 14;
        break;
      case 14: {
        const auto& notes = currentReleaseNotes();
        if (notes.simulatorPageCount() < 2) fail("Touch swipe check needs a multi-page What's New");
        char captureName[48];
        std::snprintf(captureName, sizeof(captureName), "release-notes-touch-orientation-%d", releaseNotesPass);
        captureStatusBarScreen(captureName);
        const Rect later = notes.simulatorActionRect(1);
        if (later.width <= 0 || later.height <= 0) fail("What's New has no Later button");
        const int x = later.x + later.width / 2;
        const int y = later.y + later.height / 2;
        // First check a fresh-screen tap in each orientation, then a swipe followed by a tap.
        if (releaseNotesPass < 4) {
          releaseNotesPhase = 15;
          break;
        }
        inputScript = {touchDown(x, y),
                       touchMove(x, y - 120),
                       touchMove(x, y - 240),
                       touchRelease(x, y - 240),
                       render("What's New swipe from Later", 4),
                       assertActivity("ReleaseNotes")};
        releaseNotesPhase = 15;
        break;
      }
      case 15: {
        const auto& notes = currentReleaseNotes();
        const int expectedPage = releaseNotesPass < 4 ? 0 : 1;
        if (notes.simulatorPage() != expectedPage)
          fail("Later touch check expected page %d, got %d", expectedPage, notes.simulatorPage());
        if (APP_STATE.releaseNotesSeen != 0) fail("Swipe from Later marked What's New seen");
        const Rect later = notes.simulatorActionRect(1);
        const int x = later.x + later.width / 2;
        const int y = later.y + later.height / 2;
        inputScript = {touchDown(x, y), touchRelease(x, y), render("What's New Later tapped", 6),
                       assertActivity("Home")};
        releaseNotesPhase = 16;
        break;
      }
      case 16:
        if (APP_STATE.releaseNotesSeen != RELEASE_NOTES_REVISION ||
            savedStateValue("releaseNotesSeen") != RELEASE_NOTES_REVISION)
          fail("Later tap did not save releaseNotesSeen");
        if (++releaseNotesPass < 8) {
          releaseNotesPhase = 13;
          break;
        }
        releaseNotesPass = 0;
        {
          RenderLock lock;
          renderer.setOrientation(GfxRenderer::Portrait);
        }
        releaseNotesPhase = 17;
        break;
#endif
      case 17:
        // Landscape: rotating an open What's New rebuilds its pages, and every page stays clear of the hints.
        if (!activityManager.isCurrentActivityNamed("Home")) fail("Expected Home before What's New landscape checks");
        saveReleaseNotesState(0, RELEASE_NOTES_REVISION);
        openFirstRunReleaseNotes("What's New for landscape");
        releaseNotesPhase = 18;
        break;
      case 18: {
        auto& notes = currentReleaseNotes();
        if (renderer.getOrientation() != GfxRenderer::Orientation::Portrait) fail("Landscape check expects portrait");
        releaseNotesPortraitPages = notes.simulatorPageCount();
        releaseNotesPortraitWidth = notes.simulatorLayoutWidth();
        rotateReleaseNotes(notes, releaseNotesPass == 0 ? GfxRenderer::Orientation::LandscapeClockwise
                                                        : GfxRenderer::Orientation::LandscapeCounterClockwise);
        releaseNotesPhase = 19;
        break;
      }
      case 19:
        checkReleaseNotesLandscapePage(true);
        releaseNotesPhase = 20;
        break;
      case 20: {
        auto& notes = currentReleaseNotes();
        if (notes.simulatorPage() + 1 < notes.simulatorPageCount()) {
          addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
          inputScript.push_back(render("What's New landscape next page", 3));
          releaseNotesPhase = 21;
          break;
        }
        if (++releaseNotesPass < 2) {
          rotateReleaseNotes(notes, GfxRenderer::Orientation::LandscapeCounterClockwise);
          releaseNotesPhase = 19;
          break;
        }
        rotateReleaseNotes(notes, GfxRenderer::Orientation::Portrait);
        releaseNotesPhase = 22;
        break;
      }
      case 21:
        checkReleaseNotesLandscapePage(false);
        releaseNotesPhase = 20;
        break;
      case 22: {
        const auto& notes = currentReleaseNotes();
        if (notes.simulatorLayoutWidth() != releaseNotesPortraitWidth ||
            notes.simulatorPageCount() != releaseNotesPortraitPages)
          fail("Rotating back to portrait did not restore the portrait pages");
        addTap(MappedInputManager::Button::Back);
        inputScript.push_back(render("What's New closed after rotation", 4));
        inputScript.push_back(assertActivity("Home"));
        releaseNotesPhase = 23;
        break;
      }
      case 23:
        LOG_INF(
            "SMOKE",
            "Simulator smoke test passed: release notes shown once over Home, Close saves seen, Settings row "
            "opens notes without changing seen, English offers Install Languages, saved inactive language offers "
            "Update, "
            "Install Languages opens Download Languages, Update reboots to the language download without marking seen"
#if CROSSINK_APP_CAP_TOUCH
            ", Later tap dismisses but a swipe from the buttons pages"
#endif
            ", landscape rebuilds pages clear of the button hints");
        std::_Exit(0);
    }
  }

  static void saveReleaseNotesState(const uint16_t seen, const uint16_t updateTried) {
    APP_STATE.releaseNotesSeen = seen;
    APP_STATE.releaseNotesUpdateTried = updateTried;
    if (!APP_STATE.saveToFile()) fail("Cannot save release notes state");
  }

  static int savedStateValue(const char* key) {
    const String saved = Storage.readFile(CrossPointState::getFilePath());
    JsonDocument doc;
    if (deserializeJson(doc, saved)) fail("Cannot parse saved state");
    return doc[key] | -1;
  }

  static ReleaseNotesActivity& currentReleaseNotes() {
    auto* notes = dynamic_cast<ReleaseNotesActivity*>(activityManager.simulatorCurrentActivity());
    if (!notes) fail("Expected What's New to be the current activity");
    return *notes;
  }

  void openFirstRunReleaseNotes(const char* label) {
    activityManager.pushActivity(
        std::make_unique<ReleaseNotesActivity>(renderer, mappedInputManager, ReleaseNotesActivity::Mode::FirstRun));
    inputScript.push_back(render(label, 4));
    inputScript.push_back(assertActivity("ReleaseNotes"));
  }

  void rotateReleaseNotes(ReleaseNotesActivity& notes, const GfxRenderer::Orientation orientation) {
    {
      RenderLock lock;
      renderer.setOrientation(orientation);
      notes.simulatorSetPage(0);
    }
    inputScript.push_back(render("What's New rotated", 4));
  }

  // Pages must be rebuilt for the rotated screen, the text and header stay inside the area the hints leave free,
  // and the hint strip itself looks the same on every page, so no page draws text under it.
  void checkReleaseNotesLandscapePage(const bool firstPage) {
    const auto& notes = currentReleaseNotes();
    {
      RenderLock lock;
      char name[64];
      std::snprintf(name, sizeof(name), "release-notes-landscape-%s-page-%d", releaseNotesPass == 0 ? "cw" : "ccw",
                    notes.simulatorPage() + 1);
      captureStatusBarScreen(name);
    }
    const int width = renderer.getScreenWidth();
    const int height = renderer.getScreenHeight();
    if (width <= height) fail("What's New landscape check is not in landscape");
    if (notes.simulatorLayoutWidth() != width || notes.simulatorLayoutHeight() != height)
      fail("What's New pages were not rebuilt after rotation");
    if (firstPage && notes.simulatorPage() != 0) fail("What's New landscape check did not start on page 1");
    const bool touch = mappedInputManager.hasTouch();
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !touch, false);
    const Rect body = notes.simulatorBodyRect();
    const Rect header = notes.simulatorHeaderRect();
    for (const Rect& r : {body, header}) {
      if (r.x < safe.x || r.x + r.width > safe.x + safe.width || r.y < safe.y || r.y + r.height > safe.y + safe.height)
        fail("What's New content escaped the hint-safe area (%d,%d %dx%d vs %d,%d %dx%d)", r.x, r.y, r.width, r.height,
             safe.x, safe.y, safe.width, safe.height);
    }
    // Touch devices and, since the Install Languages button, button devices both draw an on-screen action.
    const Rect action = notes.simulatorActionRect(0);
    if (action.width <= 0 || action.x < safe.x || action.x + action.width > safe.x + safe.width ||
        action.y + action.height > safe.y + safe.height)
      fail("What's New action button escaped the hint-safe area in landscape");
    if (body.y + body.height > action.y) fail("What's New text overlaps the action button in landscape");

    std::vector<uint8_t> strip;
    {
      RenderLock lock;
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          const bool inSafe = x >= safe.x && x < safe.x + safe.width && y >= safe.y && y < safe.y + safe.height;
          if (!inSafe) strip.push_back(renderer.isPixelBlack(x, y) ? 1 : 0);
        }
      }
    }
    if (!touch && std::find(strip.begin(), strip.end(), 1) == strip.end())
      fail("What's New landscape drew no button hints");
    if (firstPage) {
      releaseNotesHintStrip = std::move(strip);
    } else if (strip != releaseNotesHintStrip) {
      fail("What's New page %d drew under the side button hints", notes.simulatorPage() + 1);
    }
    LOG_INF("SMOKE", "What's New landscape %s page %d/%d (portrait had %d pages)", releaseNotesPass == 0 ? "cw" : "ccw",
            notes.simulatorPage() + 1, notes.simulatorPageCount(), releaseNotesPortraitPages);
  }

  void tickAbout() {
    if (scriptIndex < inputScript.size()) {
      runReaderInputScript();
      return;
    }
    inputScript.clear();
    scriptIndex = 0;
    switch (aboutPhase++) {
      case 0: {
        // Repeat the actual Settings entry/return route at both scales and rotations.
        {
          RenderLock lock;
          SETTINGS.uiScale = aboutPass % 2 ? CrossPointSettings::UI_SCALE_LARGE : CrossPointSettings::UI_SCALE_SMALL;
          SETTINGS.uiTheme = aboutPass < 2 ? CrossPointSettings::LYRA : CrossPointSettings::CLASSIC;
          static constexpr GfxRenderer::Orientation orientations[] = {
              GfxRenderer::Orientation::Portrait, GfxRenderer::Orientation::LandscapeClockwise,
              GfxRenderer::Orientation::PortraitInverted, GfxRenderer::Orientation::LandscapeCounterClockwise};
          renderer.setOrientation(orientations[aboutPass / 2]);
          UITheme::getInstance().reload();
        }
        activityManager.replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInputManager));
        queueStep("About Settings entry", SmokeStep::Start, 4);
        break;
      }
      case 1: {
        const auto settings = buildSystemSettingsParentList(getSettingsList());
        if (settings.back().action != SettingAction::About) fail("About missing from System");
        for (int i = 0; i < 3; ++i) addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("About System tab", 3));
        for (size_t i = 0; i < settings.size(); ++i)
          addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
        inputScript.push_back(assertSettingsNavigation(3, static_cast<int>(settings.size())));
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("About opened from Settings", 4));
        inputScript.push_back(assertActivity("About"));
        break;
      }
      case 2: {
        auto* about = dynamic_cast<AboutActivity*>(activityManager.simulatorCurrentActivity());
        if (!about || about->simulatorTopIndex() != 0) fail("About initial viewport mismatch");
        if (aboutPass % 2 && !about->simulatorFirstHeading()) {
          RenderLock lock;
          about->simulatorSetFirstHeading(
              "Geräteprofil mit ausführlicher Hardwarebeschreibung und Diagnoseinformationen");
          aboutPhase = 2;
          inputScript.push_back(render("About long localized heading", 3));
          break;
        }
#if CROSSINK_APP_CAP_TOUCH
        const auto action = about->simulatorExportButtonRect();
        const auto header = TouchHeaderBackButton::headerRect(renderer, mappedInputManager);
        if (action.width <= 0 || action.height < 56 || action.x < UITheme::getInstance().getMetrics().listSidePadding ||
            action.y < header.y + header.height ||
            action.x + action.width > renderer.getScreenWidth() - UITheme::getInstance().getMetrics().listSidePadding ||
            action.y + action.height >= renderer.getScreenHeight())
          fail("About full-width export action escaped content bounds");
        {
          RenderLock lock;
          if (!renderer.isPixelBlack(action.x, action.y + action.height / 2) ||
              !renderer.isPixelBlack(action.x + action.width / 2, action.y))
            fail("About export action has no visible outline");
        }
#else
        if (about->simulatorExportButtonRect().width != 0) fail("About export action shown on button device");
#endif
        const auto& snapshot = about->simulatorSnapshot();
        if (!snapshot.simulated || !snapshot.device || snapshot.width != display.getDisplayWidth() ||
            snapshot.height != display.getDisplayHeight() || snapshot.chip || snapshot.sdk || snapshot.internalFree)
          fail("About simulator presented real hardware data");
        if ((snapshot.touch == HalDeviceInfo::Presence::Simulated) != gpio.hasTouch())
          fail("About touch profile mismatch");
        aboutSnapshotUptime = snapshot.uptimeSeconds;
        {
          RenderLock lock;
          captureStatusBarScreen(("about-first-" + std::to_string(aboutPass)).c_str());
        }
        const int nextPage =
            std::min(about->simulatorVisibleRows(), about->simulatorRowCount() - about->simulatorVisibleRows());
        // Even a slow release must page once, with no auto-repeat before release.
        for (const auto button : {MappedInputManager::Button::Down, MappedInputManager::Button::Up}) {
          const bool down = button == MappedInputManager::Button::Down;
          inputScript.push_back(press(button));
          inputScript.push_back({ScriptActionType::WaitForNavigationHold, button, nullptr, 0, 0, 0});
          inputScript.push_back({ScriptActionType::AssertAboutTopIndex, button, nullptr, 0, down ? 0 : nextPage, 0});
          inputScript.push_back(release(button));
          inputScript.push_back(render("About single page after held-button release", 3));
          inputScript.push_back({ScriptActionType::AssertAboutTopIndex, button, nullptr, 0, down ? nextPage : 0, 0});
        }
        addTap(MappedInputManager::Button::Down);
        inputScript.push_back(render("About next page", 3));
        break;
      }
      case 3: {
        const auto* about = dynamic_cast<AboutActivity*>(activityManager.simulatorCurrentActivity());
        if (!about ||
            about->simulatorTopIndex() !=
                std::min(about->simulatorVisibleRows(), about->simulatorRowCount() - about->simulatorVisibleRows()))
          fail("About Down skipped or repeated a page");
        // Repeated paging must reach the last diagnostic and clamp at the end.
        for (int i = 0; i < 30; ++i) addTap(MappedInputManager::Button::Right);
        inputScript.push_back(render("About last page", 3));
        inputScript.push_back(assertActivity("About"));
        break;
      }
      case 4: {
        const auto* about = static_cast<AboutActivity*>(activityManager.simulatorCurrentActivity());
        if (about->simulatorTopIndex() + about->simulatorVisibleRows() != about->simulatorRowCount() ||
            about->simulatorSnapshot().uptimeSeconds != aboutSnapshotUptime)
          fail("About last page or stable snapshot mismatch");
        {
          RenderLock lock;
          captureStatusBarScreen(("about-last-" + std::to_string(aboutPass)).c_str());
        }
#if CROSSINK_APP_CAP_TOUCH
        const int x = renderer.getScreenWidth() / 2;
        const int y = renderer.getScreenHeight() / 2;
        inputScript = {touchDown(x, y), touchMove(x, y + 120), touchRelease(x, y + 120),
                       render("About swipe previous", 3)};
#else
        addTap(MappedInputManager::Button::Up);
        inputScript.push_back(render("About previous page", 3));
#endif
        break;
      }
      case 5: {
        const auto* about = static_cast<AboutActivity*>(activityManager.simulatorCurrentActivity());
        if (about->simulatorTopIndex() != std::max(0, about->simulatorRowCount() - about->simulatorVisibleRows() * 2))
          fail("About Up skipped or repeated a page");
#if CROSSINK_APP_CAP_TOUCH
        // The edge of the full-width outline belongs to the action too.
        const auto hit = about->simulatorExportButtonRect();
        // Wait for the activity's own repaint; a forced render masks broken feedback.
        inputScript = {touchDown(hit.x + 1, hit.y + hit.height / 2), render(nullptr, 6)};
#else
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("About export scope via physical Confirm", 3));
#endif
        break;
      }
      case 6: {
        const auto* about = static_cast<AboutActivity*>(activityManager.simulatorCurrentActivity());
#if CROSSINK_APP_CAP_TOUCH
        if (!about->simulatorExportPressed() || about->simulatorScopePopupActive())
          fail("About export press feedback or release timing mismatch");
        {
          RenderLock lock;
          const auto hit = about->simulatorExportButtonRect();
          if (!renderer.isPixelBlack(hit.x + 3, hit.y + 3)) fail("About export press did not invert the action");
          captureStatusBarScreen(("about-pressed-" + std::to_string(aboutPass)).c_str());
        }
        const auto hit = about->simulatorExportButtonRect();
        inputScript = {touchRelease(hit.x + 1, hit.y + hit.height / 2), render("About export edge release", 3)};
#else
        if (!about->simulatorScopePopupActive()) fail("About Confirm did not open export scope");
#endif
        break;
      }
      case 7: {
        const auto* about = static_cast<AboutActivity*>(activityManager.simulatorCurrentActivity());
        if (!about->simulatorScopePopupActive()) fail("About export full-width hitbox missed its edge");
        {
          RenderLock lock;
          captureStatusBarScreen(("about-scope-" + std::to_string(aboutPass)).c_str());
        }
        // Cancel is still selected by default on scope entry.
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("About default scope cancellation", 3));
        break;
      }
      case 8: {
        const auto* about = static_cast<AboutActivity*>(activityManager.simulatorCurrentActivity());
        if (about->simulatorScopePopupActive() || Storage.exists(SupportInfo::Path))
          fail("About scope cancellation wrote export or stayed open");
#if CROSSINK_APP_CAP_TOUCH
        const auto hit = about->simulatorExportButtonRect();
        // Horizontal drag-off does not page the list, so only the activity's
        // press-state repaint can clear the inversion after cancellation.
        inputScript = {touchDown(hit.x + hit.width / 2, hit.y + hit.height / 2),
                       touchMove(hit.x - 1, hit.y + hit.height / 2), touchRelease(hit.x - 1, hit.y + hit.height / 2),
                       render(nullptr, 6)};
#endif
        break;
      }
      case 9: {
        const auto* about = static_cast<AboutActivity*>(activityManager.simulatorCurrentActivity());
        if (about->simulatorScopePopupActive() || about->simulatorExportPressed())
          fail("About drag-off activated export or left pressed feedback");
#if CROSSINK_APP_CAP_TOUCH
        const auto hit = about->simulatorExportButtonRect();
        {
          RenderLock lock;
          if (renderer.isPixelBlack(hit.x + 3, hit.y + 3)) fail("About drag-off did not repaint cleared feedback");
        }
        inputScript = {touchDown(hit.x + hit.width / 2, hit.y + hit.height + 2),
                       touchRelease(hit.x + hit.width / 2, hit.y + hit.height + 2),
                       render("About tap outside export", 3)};
#endif
        break;
      }
      case 10: {
        const auto* about = static_cast<AboutActivity*>(activityManager.simulatorCurrentActivity());
        if (about->simulatorScopePopupActive()) fail("About export hitbox extends below its outline");
        supportOpenScope();
        break;
      }
      case 11:
        supportSelectScope(1);
        break;
      case 12: {
        RenderLock lock;
        captureStatusBarScreen(("about-confirmation-" + std::to_string(aboutPass)).c_str());
        // Confirm without moving the selection must choose the default Cancel.
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("About default confirmation cancellation", 4));
        inputScript.push_back(assertActivity("About"));
        break;
      }
      case 13: {
        if (Storage.exists(SupportInfo::Path)) fail("About confirmation cancellation wrote export");
#if CROSSINK_APP_CAP_TOUCH
        const auto header = TouchHeaderBackButton::headerRect(renderer, mappedInputManager);
        const auto hit = TouchHeaderBackButton::layout(header).touchRect;
        inputScript = {touchDown(hit.x + hit.width / 2, hit.y + hit.height / 2),
                       touchRelease(hit.x + hit.width / 2, hit.y + hit.height / 2)};
#else
        addTap(MappedInputManager::Button::Back);
#endif
        inputScript.push_back(render("About returns to Settings", 4));
        inputScript.push_back(
            assertSettingsNavigation(3, static_cast<int>(buildSystemSettingsParentList(getSettingsList()).size())));
        break;
      }
      case 14:
        if (++aboutPass < 8) {
          aboutPhase = 0;
          break;
        }
        LOG_INF("SMOKE",
                "Simulator smoke test passed: About navigation, paging, snapshot, bold/long headings, full-width "
                "export outline/press/hitbox, default cancellations, scales and orientations");
        std::_Exit(0);
    }
  }

  void supportOpenScope() {
    auto* about = dynamic_cast<AboutActivity*>(activityManager.simulatorCurrentActivity());
    if (!about) fail("Support export did not return to About");
#if CROSSINK_APP_CAP_TOUCH
    const auto hit = about->simulatorExportButtonRect();
    inputScript = {touchDown(hit.x + hit.width / 2, hit.y + hit.height / 2),
                   touchRelease(hit.x + hit.width / 2, hit.y + hit.height / 2)};
#else
    addTap(MappedInputManager::Button::Confirm);
#endif
    inputScript.push_back(render("Support scope selection", 3));
  }
  void supportSelectScope(int scope) {
    auto* about = dynamic_cast<AboutActivity*>(activityManager.simulatorCurrentActivity());
    if (!about || !about->simulatorScopePopupActive()) fail("Support scope popup missing");
#if CROSSINK_APP_CAP_TOUCH
    const auto hit = about->simulatorScopePopup().simulatorOptionRect(scope);
    inputScript = {touchDown(hit.x + hit.width / 2, hit.y + hit.height / 2),
                   touchRelease(hit.x + hit.width / 2, hit.y + hit.height / 2)};
#else
    for (int i = 0; i < scope; ++i) addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
    addTap(MappedInputManager::Button::Confirm);
#endif
    inputScript.push_back(render("Support scope confirmation", 4));
    inputScript.push_back(assertActivity("Confirmation"));
  }
  void supportConfirmWrite() {
    addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Support export result", 4));
    inputScript.push_back(assertActivity("About"));
  }
  void checkSupportExport(bool book) {
    const std::string contents = Storage.readFile(SupportInfo::Path).c_str();
    if (contents.find("PRIVATE_SUPPORT") != std::string::npos) fail("Support export leaked private data");
    JsonDocument doc;  // Host-only regression inspection, never part of firmware export.
    if (deserializeJson(doc, contents)) fail("Support export invalid JSON");
    if (doc["version"].as<int>() != 1 || !doc["about"]["runtimeData"].is<const char*>())
      fail("Support export schema missing");
    if (std::strcmp(doc["about"]["chipAndMemoryStatus"], "unsupported") || !doc["about"]["internalFreeBytes"].isNull())
      fail("Support export invented simulator heap");
    if (std::strcmp(doc["firmware"]["freeinkSdkSha"], AppVersion::sdkSha()))
      fail("Support export SDK provenance mismatch");
    if (!book) {
      if (std::strcmp(doc["bookContext"]["status"], "excluded") ||
          !doc["bookContext"]["effectiveReaderPreferences"].isNull())
        fail("Device scope included book settings");
    } else {
      const auto prefs = doc["bookContext"]["effectiveReaderPreferences"];
      if (std::strcmp(doc["bookContext"]["status"], "loaded") ||
          prefs["readerFontPointSize"]["value"].as<int>() != 27 ||
          std::strcmp(prefs["readerFontPointSize"]["source"], "book_override") ||
          std::strcmp(prefs["orientation"]["source"], "global_default") ||
          !doc["bookContext"]["fontSelectionOverride"].as<bool>() ||
          !doc["bookContext"]["dictionaryOverride"].as<bool>())
        fail("Support book inheritance mismatch");
    }
  }
  void checkSupportLegacyAndMissingRecords() {
    const auto cache = Epub::cachePathForFilePath(APP_STATE.openEpubPath, "/.crosspoint");
    const auto path = cache + "/reader_settings.bin";
    uint8_t legacy[88]{};  // Actual v4 format: single margin, legacy dark byte, no dictionary or mask.
    legacy[0] = 4;
    legacy[1] = 1;
    legacy[6] = 2;
    legacy[8] = 255;
    legacy[9] = 99;
    legacy[10] = 255;
    legacy[12] = 99;
    legacy[13] = 2;
    legacy[17] = 99;
    auto writeRecord = [&] {
      auto file = Storage.open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
      if (!file || file.write(legacy, sizeof(legacy)) != sizeof(legacy) || !file.sync())
        fail("Legacy support fixture write failed");
      file.close();
    };
    auto exportDocument = [&] {
      if (SupportInfoExport::save(true) != SupportInfo::Result::Saved) fail("Support context export failed");
      JsonDocument document;
      const auto contents = Storage.readFile(SupportInfo::Path);
      if (deserializeJson(document, contents)) fail("Support context invalid JSON");
      if (std::strstr(contents.c_str(), "PRIVATE_SUPPORT")) fail("Support context leaked private data");
      return document;
    };
    // A v11 image override must not be mistaken for a custom-font override.
    uint8_t grayscale[158]{};
    grayscale[0] = 11;
    grayscale[1] = 1;
    grayscale[155] = 4;  // Image grayscale is bit 18; custom font moved to bit 19.
    grayscale[157] = SETTINGS.imageGrayscale ? 0 : 1;
    {
      auto file = Storage.open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
      if (!file || file.write(grayscale, sizeof(grayscale)) != sizeof(grayscale) || !file.sync())
        fail("Grayscale support fixture write failed");
      file.close();
      auto d = exportDocument();
      auto field = d["bookContext"]["effectiveReaderPreferences"]["imageGrayscale"];
      if (!field["value"].is<int>() || field["value"].as<int>() != grayscale[157] ||
          std::strcmp(field["source"], "book_override") || d["bookContext"]["fontSelectionOverride"].as<bool>())
        fail("Support grayscale override confused with custom font");
    }
    writeRecord();
    {
      auto d = exportDocument();
      auto prefs = d["bookContext"]["effectiveReaderPreferences"];
      if (prefs["readerFontPointSize"]["value"].as<int>() != 14 ||
          prefs["lineHeightPercent"]["value"].as<int>() != 70 || prefs["wordSpacing"]["value"].as<int>() != 8 ||
          prefs["screenMarginHorizontal"]["value"].as<int>() != 150 ||
          prefs["orientation"]["value"].as<int>() != SETTINGS.orientation ||
          prefs["imageRendering"]["value"].as<int>() != SETTINGS.imageRendering ||
          prefs["embeddedStyle"]["value"].as<int>() != 1)
        fail("Legacy support normalization mismatch");
    }
    std::memcpy(legacy + 24, "PRIVATE_SUPPORT_SENTINEL_FONT", 29);
    writeRecord();
    {
      auto d = exportDocument();
      auto field = d["bookContext"]["effectiveReaderPreferences"]["readerFontPointSize"];
      if (!field["value"].isNull() || std::strcmp(field["status"], "unavailable_legacy_custom_font_size"))
        fail("Legacy custom font size misrepresented");
    }
    legacy[0] = 99;
    writeRecord();
    {
      auto d = exportDocument();
      if (std::strcmp(d["bookContext"]["status"], "invalid_record") ||
          !d["bookContext"]["effectiveReaderPreferences"].isNull())
        fail("Invalid support context misrepresented");
    }
    if (!Storage.remove(path.c_str())) fail("Support missing-record fixture failed");
    {
      auto d = exportDocument();
      if (std::strcmp(d["bookContext"]["status"], "inherited_no_record") ||
          std::strcmp(d["bookContext"]["effectiveReaderPreferences"]["readerFontPointSize"]["source"],
                      "global_default"))
        fail("Missing support record did not inherit");
    }
    if (!Storage.rmdir(cache.c_str())) fail("Support unavailable-cache fixture failed");
    {
      auto d = exportDocument();
      if (std::strcmp(d["bookContext"]["status"], "unavailable")) fail("Absent support cache misrepresented");
    }
  }

  void tickSupportExport() {
    if (scriptIndex < inputScript.size()) {
      runReaderInputScript();
      return;
    }
    inputScript.clear();
    scriptIndex = 0;
    switch (supportPhase++) {
      case 0: {
        {
          RenderLock lock;
          std::strcpy(SETTINGS.deviceName, "PRIVATE_SUPPORT_SENT");
          std::strcpy(SETTINGS.opdsServerUrl, "PRIVATE_SUPPORT_SENTINEL_URL");
          std::strcpy(SETTINGS.opdsUsername, "PRIVATE_SUPPORT_SENTINEL_USER");
          std::strcpy(SETTINGS.opdsPassword, "PRIVATE_SUPPORT_SENTINEL_PASSWORD");
          std::strcpy(SETTINGS.sdFontFamilyName, "PRIVATE_SUPPORT_SENTINEL_FONT");
          std::strcpy(SETTINGS.dictionarySdFontFamilyName, "PRIVATE_SUPPORT_SENTINEL_DICT");
          std::strcpy(SETTINGS.opdsDownloadFolder, "PRIVATE_SUPPORT_SENTINEL_FOLDER");
          std::strcpy(SETTINGS.nearbyReceiveFolder, "PRIVATE_SUPPORT_SENTINEL_NEARBY");
        }
        APP_STATE.openEpubPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
        const auto cache = Epub::cachePathForFilePath(APP_STATE.openEpubPath, "/.crosspoint");
        Storage.mkdir(cache.c_str());
        uint8_t record[157]{};  // Fixed v10 fixture; includes poisoned font names, no metadata parse.
        record[0] = 10;
        record[1] = 9;
        record[6] = 27;
        record[7] = 100;
        record[10] = 5;
        record[11] = 5;
        std::memcpy(record + 24, "PRIVATE_SUPPORT_SENTINEL_BOOK_FONT", 33);
        std::memcpy(record + 88, "PRIVATE_SUPPORT_SENTINEL_BOOK_DICT", 33);
        record[152] = 18;
        record[153] = 2;
        record[155] = 4;  // font point-size bit1, font name bit18.
        auto f = Storage.open((cache + "/reader_settings.bin").c_str(), O_WRONLY | O_CREAT | O_TRUNC);
        if (!f || f.write(record, sizeof(record)) != sizeof(record) || !f.sync()) fail("Support fixture write failed");
        f.close();
        for (const char* name : {"wifi.json", "opds.json", "koreader.json", "ttf-rendering.json"})
          Storage.writeFile((std::string("/.crosspoint/") + name).c_str(), "PRIVATE_SUPPORT_SENTINEL_CONFIG");
        activityManager.replaceActivity(std::make_unique<AboutActivity>(renderer, mappedInputManager));
        queueStep("Support About entry", SmokeStep::Start, 4);
        break;
      }
      case 1:
        supportOpenScope();
        break;
      case 2:  // Default selection cancels; it must never generate a file.
      {
        RenderLock lock;
        captureStatusBarScreen("support-scope");
      }
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("Support scope cancelled", 3));
        break;
      case 3:
        if (Storage.exists(SupportInfo::Path)) fail("Scope cancellation wrote export");
        supportOpenScope();
        break;
      case 4:
        supportSelectScope(1);
        break;
      case 5: {
        RenderLock lock;
        captureStatusBarScreen("support-confirmation");
      }
        addTap(MappedInputManager::Button::Back);
        inputScript.push_back(render("Support confirmation cancelled", 4));
        inputScript.push_back(assertActivity("About"));
        break;
      case 6:
        if (Storage.exists(SupportInfo::Path)) fail("Confirmation cancellation wrote export");
        supportOpenScope();
        break;
      case 7:
        supportSelectScope(1);
        break;
      case 8:
        supportConfirmWrite();
        break;
      case 9: {
        RenderLock lock;
        captureStatusBarScreen("support-saved");
      }
        checkSupportExport(false);
        priorSupportExport = Storage.readFile(SupportInfo::Path).c_str();
        // A directory at the temp filename deterministically rejects the HAL write.
        if (!Storage.mkdir(SupportInfo::TempPath) || !Storage.writeFile("/crossink-support.json.tmp/blocker", "test"))
          fail("Support failure fixture failed");
        supportOpenScope();
        break;
      case 10:
        supportSelectScope(1);
        break;
      case 11:
        supportConfirmWrite();
        break;
      case 12: {
        RenderLock lock;
        captureStatusBarScreen("support-failed");
      }
        if (std::string(Storage.readFile(SupportInfo::Path).c_str()) != priorSupportExport)
          fail("Failed export destroyed prior file");
        if (!Storage.remove("/crossink-support.json.tmp/blocker") || !Storage.rmdir(SupportInfo::TempPath))
          fail("Support failure fixture cleanup failed");
        if (static_cast<AboutActivity*>(activityManager.simulatorCurrentActivity())->simulatorExportStatus() !=
            StrId::STR_SUPPORT_FAILED)
          fail("Support failure was not shown");
        supportOpenScope();
        break;
      case 13:
        supportSelectScope(2);
        break;
      case 14:
        supportConfirmWrite();
        break;
      case 15:
        checkSupportExport(true);
        checkSupportLegacyAndMissingRecords();
        if (Storage.exists(SupportInfo::TempPath) || Storage.exists(SupportInfo::BackupPath))
          fail("Support transaction files remained");
        if (static_cast<AboutActivity*>(activityManager.simulatorCurrentActivity())->simulatorExportStatus() !=
            StrId::STR_SUPPORT_SAVED)
          fail("Support success was not shown");
        addTap(MappedInputManager::Button::Back);
        inputScript.push_back(render("Support About exit", 4));
        break;
      case 16:
        LOG_INF("SMOKE",
                "Simulator smoke test passed: support export privacy, explicit scopes, cancellation, failure "
                "preservation and EPUB inheritance");
        std::_Exit(0);
    }
  }

  void tickImpl() {
    mappedInputManager.simulatorClearInputFrame();
    if (std::getenv("CROSSINK_SIMULATOR_SMOKE_PLUGINS")) {
      runPluginSmokeTestTick();
      return;
    }

    if (settleFrames > 0) {
      --settleFrames;
      if (settleFrames == 0 && activeStepName != nullptr) {
        renderCurrentStep(activeStepName);
        activeStepName = nullptr;
      }
      return;
    }

    if (std::getenv("CROSSINK_SIMULATOR_SMOKE_HYPHENATION")) {
      tickHyphenation();
      return;
    }
    if (std::getenv("CROSSINK_SIMULATOR_SMOKE_FILENAME_FONT")) {
      tickFilenameFont();
      return;
    }
    if (tickAssetDownloadSmokeTest()) return;
    if (tickOpdsCatalogSmokeTest()) return;
    if (tickBookmarkListSmokeTest()) return;

    if (std::getenv("CROSSINK_SIMULATOR_SMOKE_SUPPORT_EXPORT")) {
      tickSupportExport();
      return;
    }
    if (std::getenv("CROSSINK_SIMULATOR_SMOKE_CALIBRATION")) {
      tickCalibration();
      return;
    }
    if (std::getenv("CROSSINK_SIMULATOR_SMOKE_RECEIVE_FOLDER")) {
      tickReceiveFolder();
      return;
    }
    if (std::getenv("CROSSINK_SIMULATOR_SMOKE_ABOUT")) {
      tickAbout();
      return;
    }
    if (std::getenv("CROSSINK_SIMULATOR_SMOKE_RELEASE_NOTES")) {
      tickReleaseNotes();
      return;
    }
    if (std::getenv("CROSSINK_SIMULATOR_SMOKE_FILE_BROWSER_SYNC_RETURN")) {
      tickFileBrowserSyncReturn();
      return;
    }

    switch (step) {
      case SmokeStep::Start:
        LOG_INF("SMOKE", "Starting simulator smoke test");
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_HOME_SWIPE")) {
          buildHomeSwipeScript();
          step = SmokeStep::ReaderInput;
          break;
        }
        if (std::getenv("CROSSINK_KOSYNC_SETTINGS_UI")) {
          KOREADER_STORE.setCustomHeader(0, "CF-Access-Client-Id", "fixture-id");
          activityManager.replaceActivity(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInputManager));
          inputScript.clear();
          scriptIndex = 0;
          inputCompletionStep = SmokeStep::Done;
          inputScript.push_back(render("Sync settings", 100));
          for (int i = 0; i < 3; ++i) addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
          addTap(MappedInputManager::Button::Confirm);
          inputScript.push_back(render("Sync server types", 200));
          step = SmokeStep::ReaderInput;
          break;
        }
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_APP_NOTES")) {
          activityManager.replaceActivity(std::make_unique<EntryRenderSmokeActivity>(renderer, mappedInputManager));
          queueStep("Entry render isolation and short note", SmokeStep::Done, 8);
          break;
        }
        if (std::getenv("CROSSINK_READING_TEST_MENU") && Storage.exists("/expected-progress.json")) {
          if (Storage.exists("/menu-network-checked")) {
            if (!activityManager.isCurrentActivityNamed("Library") || APP_STATE.pendingOverlayResume.valid())
              fail("Completed menu sync did not return to Library");
            // Sync Book reconciled the book, so the position kept for it is gone.
            if (std::getenv("CROSSINK_READING_TEST_MENU_KEPT") && !SyncParked::empty())
              fail("Sync Book left the book's kept position");
            LOG_INF("SMOKE", "Stats upload transport smoke passed");
            std::_Exit(0);
          }
          if (!Storage.writeFile("/menu-network-checked", "checked")) fail("Cannot mark menu sync reboot");
          const char* selectedBook =
              std::getenv("CROSSINK_READING_TEST_COLD") ? "/read/unread.epub" : "/read/first.epub";
          if (APP_STATE.openEpubPath != selectedBook || !activityManager.hasActivityNamed(KOReaderSyncActivity::NAME))
            fail("Book menu did not reboot into selected book sync");
          inputScript.clear();
          scriptIndex = 0;
          inputCompletionStep = SmokeStep::StatsUploadEmptyDone;
          inputScript.push_back(render("Selected book combined sync", 100));
          step = SmokeStep::ReaderInput;
          break;
        }
        // Applying remote progress intentionally reboots into the reader. The
        // isolated fixture marks re-entry so this test cannot repeat uploads.
        if (std::getenv("CROSSINK_READING_TEST_CURRENT") && Storage.exists("/expected-progress.json")) {
          LOG_INF("SMOKE", "Stats upload transport smoke passed");
          std::_Exit(0);
        }
        verifyStatsUploadContract();
        if (prepareReadingUploadSmokeTest()) {
          WiFi.mode(WIFI_STA);
          WiFi.begin("reading-smoke");
          if (const char* matching = std::getenv("CROSSINK_READING_TEST_MATCHING")) {
            // Sync state keyed by Filename ids: a recorded book, a prepared
            // folder and a kept position, all dropped by a confirmed change.
            if (!SyncState::load().save()) fail("Cannot save sync state fixture");
            SyncScope scope;
            scope.path = "/read";
            scope.prepared = true;
            scope.cursor = 5;
            if (!SyncState::putScope(scope)) fail("Cannot prepare the sync scope fixture");
            if (!parkRemoteFixture("/read/first.epub") || SyncIds::generation() == 0)
              fail("Cannot record sync ids and a kept position");
            inputScript.clear();
            scriptIndex = 0;
            inputCompletionStep = SmokeStep::MatchingChangeDone;
            if (std::strcmp(matching, "web") == 0) {
              // The web settings page saves through the settings list (after its own warning).
              for (const auto& setting : getSettingsList())
                if (setting.key && std::strcmp(setting.key, "koMatchMethod") == 0)
                  setting.valueSetter(static_cast<uint8_t>(DocumentMatchMethod::BINARY));
              inputScript.push_back(render("Web Document Matching change", 2));
              step = SmokeStep::ReaderInput;
              break;
            }
            activityManager.replaceActivity(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInputManager));
            inputScript.push_back(render("Sync Server settings", 10));
            // Up wraps to Send Metadata, then Document Matching.
            addTap(mappedInputManager.menuButton(MappedInputManager::Button::Up));
            addTap(mappedInputManager.menuButton(MappedInputManager::Button::Up));
            addTap(MappedInputManager::Button::Confirm);
            inputScript.push_back(render("Document Matching warning", 10));
            inputScript.push_back(assertActivity("Confirmation"));
            // Cancel is selected first; Confirm is the second option.
            if (std::strcmp(matching, "confirm") == 0)
              addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
            addTap(MappedInputManager::Button::Confirm);
            inputScript.push_back(render("Sync Server after the warning", 10));
            inputScript.push_back(assertActivity("KOReaderSettings"));
            step = SmokeStep::ReaderInput;
            break;
          }
          if (const char* kept = std::getenv("CROSSINK_READING_TEST_OPEN_KEPT")) {
            // Another reader left first.epub further ahead (page 7 of 10 vs 2), or
            // well behind it.
            const bool prepared = std::strcmp(kept, "unprepared") != 0;
            const bool behind = std::strcmp(kept, "behind") == 0;
            if (!SyncState::load().save()) fail("Cannot save sync state fixture");
            SyncScope scope;
            scope.path = prepared ? "/read" : "/read-sibling";
            scope.prepared = true;
            if (!SyncState::putScope(scope)) fail("Cannot prepare the sync scope fixture");
            if (behind) {
              Epub book("/read/first.epub", "/.crosspoint");
              if (!EpubReaderUtils::saveProgress(book, 1, 0, 4)) fail("Cannot move the local position to chapter 2");
            }
            if (!parkRemoteFixture("/read/first.epub", behind ? 0.1f : 1.0f)) fail("Cannot keep a remote position");
            activityManager.goToReader("/read/first.epub");
            inputScript.clear();
            scriptIndex = 0;
            inputCompletionStep = SmokeStep::KeptPositionDone;
            inputScript.push_back(render("Reader with a kept position", 80));
            if (prepared && !behind) {
              inputScript.push_back(assertActivity("Confirmation"));
              // Stay here is selected first; Continue is the second option.
              if (std::strcmp(kept, "accept") == 0)
                addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
              addTap(MappedInputManager::Button::Confirm);
              inputScript.push_back(render("Reader after the kept position choice", 80));
            }
            inputScript.push_back(assertActivity("EpubReader"));
            step = SmokeStep::ReaderInput;
            break;
          }
          if (std::getenv("CROSSINK_READING_TEST_MENU")) {
            for (const auto* path : {"/read/first.epub", "/read/notes.txt", "/read/book.xtc"}) {
              const auto actions = BookActions::buildBookActionItems(path, true);
              const auto count = std::count_if(actions.begin(), actions.end(), [](const auto& item) {
                return item.action == FileBrowserAction::SyncProgress;
              });
              if (count != (std::string(path) == "/read/notes.txt" ? 0 : 1))
                fail("Book menu sync availability mismatch");
            }
            if (!WIFI_STORE.addCredential("reading-smoke", "")) fail("Cannot save test Wi-Fi");
            WIFI_STORE.setLastConnectedSsid("reading-smoke");
            APP_STATE.openEpubPath = "/read/sub/second.EPUB";
            if (!APP_STATE.saveToFile() || !KOREADER_STORE.saveToFile()) fail("Cannot save menu sync fixture");
            PendingOverlayResume resume;
            resume.origin = PendingOverlayOrigin::Library;
            resume.tab = 4;
            resume.pane = true;
            if (std::getenv("CROSSINK_READING_TEST_MENU_KEPT") && !parkRemoteFixture("/read/first.epub"))
              fail("Cannot keep a remote position");
            BookActions::syncProgress(
                renderer, mappedInputManager,
                std::getenv("CROSSINK_READING_TEST_COLD") ? "/read/unread.epub" : "/read/first.epub",
                std::move(resume));
            break;
          }
          const bool currentBook = std::getenv("CROSSINK_READING_TEST_CURRENT");
          if (currentBook)
            activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
                renderer, mappedInputManager, "/read/first.epub", DocumentMatchMethod::FILENAME, SETTINGS.orientation));
          else if (std::getenv("CROSSINK_READING_TEST_XTC_BOOK")) {
            // The real Library menu path: stats sync in place, then back to Library.
            PendingOverlayResume resume;
            resume.origin = PendingOverlayOrigin::Library;
            BookActions::syncProgress(renderer, mappedInputManager, "/read/sub/comic.xtc", std::move(resume));
            inputScript.clear();
            scriptIndex = 0;
            inputCompletionStep = SmokeStep::StatsUploadEmptyDone;
            inputScript.push_back(render("XTC Sync Book result", 100));
            inputScript.push_back(assertActivity("StatsUpload"));
            addTap(MappedInputManager::Button::Back);
            inputScript.push_back(assertActivity("Library"));
            step = SmokeStep::ReaderInput;
            break;
          } else if (std::getenv("CROSSINK_READING_TEST_CLIPPINGS_ONLY")) {
            // Sync Clippings lists the clippings folder; no overall stats, stats, or progress.
            // Started as if from File Transfer opened inside a book: leaving reopens it.
            auto sync = std::make_unique<StatsUploadActivity>(renderer, mappedInputManager, std::string{},
                                                              StatsUploadActivity::Scope::Clippings, true);
            sync->setReturnToFileTransfer("/read/first.epub", false);
            activityManager.replaceActivity(std::move(sync));
            inputScript.clear();
            scriptIndex = 0;
            inputCompletionStep = SmokeStep::StatsUploadEmptyDone;
            inputScript.push_back(render("Sync Clippings result", 100));
            inputScript.push_back(assertActivity("StatsUpload"));
            addTap(MappedInputManager::Button::Back);
            inputScript.push_back(render("Back to the book", 60));
            inputScript.push_back(assertActivity("EpubReader"));
            step = SmokeStep::ReaderInput;
            break;
          } else if (std::getenv("CROSSINK_READING_TEST_ALL")) {
            // Sync Books walks the Library index, not a folder.
            library::BuildStats stats;
            if (!library::buildLibraryIndex("/", stats)) fail("Cannot build Sync Books Library fixture");
            // An indexed book deleted afterwards is skipped, not a failure.
            if (!Storage.remove("/read-sibling/outside.epub")) fail("Cannot remove indexed fixture book");
            if (std::getenv("CROSSINK_READING_TEST_STATS_ENTRY")) {
              // Reading Stats asks first, closes, then Sync Reading Stats replaces the stack.
              activityManager.pushActivity(std::make_unique<StatsParentSmokeActivity>(renderer, mappedInputManager));
              inputScript.clear();
              scriptIndex = 0;
              inputCompletionStep = SmokeStep::StatsUploadEmptyDone;
              inputScript.push_back(render("Reading stats before Sync Reading Stats", 5));
              addTap(MappedInputManager::Button::Confirm);
              inputScript.push_back(render("Sync Reading Stats confirmation", 5));
              inputScript.push_back(assertActivity("Confirmation"));
              addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
              addTap(MappedInputManager::Button::Confirm);
              inputScript.push_back(render("Sync Reading Stats from Reading Stats", 100));
              inputScript.push_back(assertActivity("StatsUpload"));
              addTap(MappedInputManager::Button::Back);
              inputScript.push_back(assertActivity("Home"));
              step = SmokeStep::ReaderInput;
              break;
            }
            activityManager.replaceActivity(std::make_unique<StatsUploadActivity>(renderer, mappedInputManager));
          } else {
            // Bulk sync screens follow the reader orientation setting.
            if (std::getenv("CROSSINK_READING_TEST_LANDSCAPE"))
              SETTINGS.orientation = CrossPointSettings::LANDSCAPE_CCW;
            activityManager.replaceActivity(
                std::make_unique<StatsUploadActivity>(renderer, mappedInputManager, "/read"));
          }
          inputScript.clear();
          scriptIndex = 0;
          inputCompletionStep = SmokeStep::StatsUploadEmptyDone;
          if (currentBook) {
            inputScript.push_back(render("Current book combined sync", 40));
            step = SmokeStep::ReaderInput;
            break;
          }
          inputScript.push_back(render("Folder upload confirmation", 5));
          if (std::getenv("CROSSINK_READING_TEST_EXIT_HELD")) {
            // Smart sync with no prompts: holding Exit once the run starts stops it.
            addTap(MappedInputManager::Button::Confirm);
            inputScript.push_back(press(MappedInputManager::Button::Back));
            inputScript.push_back(render("Folder sync held Exit while running", 25));
            inputScript.push_back(release(MappedInputManager::Button::Back));
            inputScript.push_back(render("After held Exit", 5));
            inputScript.push_back(assertActivity("Home"));
            step = SmokeStep::ReaderInput;
            break;
          }
          if (!std::getenv("CROSSINK_READING_TEST_CANCEL")) {
            addTap(MappedInputManager::Button::Confirm);
            if (std::getenv("CROSSINK_READING_TEST_ASK")) {
              // Ask on conflicts: only first.epub's differing remote position asks, after
              // the run; second.EPUB has no remote position and uploads without asking.
              for (int book = 0; book < 1; ++book) {
                inputScript.push_back(render("Folder sync choice", 25));
                inputScript.push_back(assertActivity("KOReaderSync"));
                if (std::getenv("CROSSINK_READING_TEST_ASK_CANCEL")) break;
                if (std::getenv("CROSSINK_READING_TEST_ASK_SKIP")) {
                  // Skip book is one step up from the default.
                  addTap(MappedInputManager::Button::Up);
                  inputScript.push_back(render("Folder sync Skip book selected", 5));
                  addTap(MappedInputManager::Button::Confirm);
                  continue;
                }
                if (std::getenv("CROSSINK_READING_TEST_ASK_EXIT_HELD")) {
                  // Upload local while Exit is held: the upload checkpoint stops the batch.
                  addTap(MappedInputManager::Button::Down);
                  inputScript.push_back(press(MappedInputManager::Button::Back));
                  addTap(MappedInputManager::Button::Confirm);
                  inputScript.push_back(render("Folder sync held Exit", 25));
                  inputScript.push_back(release(MappedInputManager::Button::Back));
                  break;
                }
                addTap(MappedInputManager::Button::Confirm);
              }
            }
            inputScript.push_back(render("Folder upload result", 100));
            if (std::getenv("CROSSINK_READING_TEST_SKIP_FAILED") || std::getenv("CROSSINK_READING_TEST_ERROR")) {
              // Failed books never stop the run: it ends on the summary, which lists them.
              inputScript.push_back(assertActivity("StatsUpload"));
            }
          }
          if (std::getenv("CROSSINK_READING_TEST_DONE_TOUCH")) {
#if CROSSINK_APP_CAP_TOUCH
            if (!mappedInputManager.hasTouchHardware()) fail("Touch Back test requires a touch simulator");
            const auto area = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
            const int x = area.x + area.width / 2;
            const int y = area.y + area.height - 52;
            inputScript.push_back(touchDown(x, y));
            inputScript.push_back(touchRelease(x, y));
#else
            fail("Touch Back test requires a touch simulator");
#endif
          } else {
            addTap(std::getenv("CROSSINK_READING_TEST_DONE_CONFIRM") ? MappedInputManager::Button::Confirm
                                                                     : MappedInputManager::Button::Back);
          }
          inputScript.push_back(assertActivity("Home"));
          step = SmokeStep::ReaderInput;
          break;
        }
        if (std::getenv("CROSSINK_STATS_TEST_EMPTY_LIBRARY")) {
          library::BuildStats stats;
          if (!Storage.mkdir("/empty-stats-library") || !library::buildLibraryIndex("/empty-stats-library", stats) ||
              stats.books || stats.folders)
            fail("Cannot build empty Library fixture");
          SETTINGS.trackReadingStats = 1;
          WiFi.mode(WIFI_STA);
          WiFi.begin("stats-smoke");
          activityManager.replaceActivity(std::make_unique<StatsUploadActivity>(renderer, mappedInputManager));
          inputScript.clear();
          scriptIndex = 0;
          inputCompletionStep = SmokeStep::StatsUploadEmptyDone;
          inputScript.push_back(render("Empty Library stats upload confirmation", 5));
          addTap(MappedInputManager::Button::Confirm);
          inputScript.push_back(render("Empty Library stats upload result", 100));
          addTap(MappedInputManager::Button::Back);
          inputScript.push_back(assertActivity("Home"));
          step = SmokeStep::ReaderInput;
          break;
        }
#if CROSSINK_SCALABLE_FONTS
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_READER_FONT_LAYOUT")) {
          sdFontSystem.ensureRegistry();
          const char* family = std::getenv("CROSSINK_SIMULATOR_SMOKE_FONT_FAMILY");
          if (!family) fail("Reader Font layout requires a TTF family");
          std::snprintf(SETTINGS.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName), "%s", family);
          RenderLock lock;
          if (!ReaderDrawerLayoutSmokeTest::verify(
                  lock, [&](const std::string& name) { captureStatusBarScreen(name.c_str()); }))
            fail("Reader Font rows overlapped navigation or became unreachable");
          LOG_INF("SMOKE", "Simulator smoke test passed: reader font layout");
          std::_Exit(0);
        }
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_PUBLISHER_FONTS")) {
          verifyPublisherFonts();
          LOG_INF("SMOKE", "Simulator smoke test passed: publisher fonts");
          std::_Exit(0);
        }
#endif
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_FONT_RECOVERY")) {
          // A catalog-only fixture: the detail allocation fails before the
          // deliberately invalid font bytes are opened.
          std::filesystem::create_directories("fs_/fonts/Recovery");
          if (!Storage.writeFile("/fonts/Recovery/Recovery_12.cpfont", "catalog fixture"))
            fail("Cannot create font recovery fixture");
          std::strcpy(SETTINGS.sdFontFamilyName, "Recovery");
          SETTINGS.legacySdFontSizeStep = UINT8_MAX;
          SETTINGS.readerFontPointSize = 12;
          int saves = 0;
          sdFontSystem.setSettingsPersistenceCallback([](void* ctx) { ++*static_cast<int*>(ctx); }, &saves);
          sdFontSystem.releaseForNetwork(renderer);
          // Model FontInstaller's direct catalog discovery and subsequent exit.
          sdFontSystem.registry().discover();
          sdFontSystem.releaseRegistry();
          if (sdFontSystem.registry().getFamilyCount() != 0) fail("Installer catalog survived release");
          sdFontSystem.ensureRegistry();
          const auto* summary = sdFontSystem.registry().findSummary("Recovery");
          if (!summary || !summary->files.empty()) fail("Recovery summary not lazy");
          const size_t available = ESP.getFreeHeap();
          if (available <= 4096) fail("Insufficient initial heap for recovery check");
          void* pressure = heap_caps_malloc(available - 4096, MALLOC_CAP_INTERNAL);
          if (!pressure) fail("Cannot inject font detail memory pressure");
          sdFontSystem.ensureLoaded(renderer);
          heap_caps_free(pressure);
          if (std::strcmp(SETTINGS.sdFontFamilyName, "Recovery") != 0 || saves != 0)
            fail("Temporary detail failure erased the reader selection");
          if (!summary->ensureDetails()) fail("Font details did not recover after memory pressure");
          sdFontSystem.ensureLoaded(renderer);
          if (std::strcmp(SETTINGS.sdFontFamilyName, "Recovery") != 0 || saves != 0)
            fail("Unreadable font file erased the reader selection");
          std::strcpy(SETTINGS.sdFontFamilyName, "Missing Recovery Family");
          sdFontSystem.ensureLoaded(renderer);
          if (SETTINGS.sdFontFamilyName[0] || saves != 1) fail("Truly missing family was not cleared");
          sdFontSystem.setSettingsPersistenceCallback(nullptr, nullptr);
          sdFontSystem.releaseRegistry();
          LOG_INF("SMOKE", "Simulator smoke test passed: font recovery");
          std::_Exit(0);
        }
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_MENU_ORIENTATION")) {
          verifyMenuOrientation();
          LOG_INF("SMOKE", "Simulator smoke test passed: menu orientation");
          std::_Exit(0);
        }
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_GESTURE_SHORTCUTS")) {
          verifyGestureShortcutChanges();
          verifyReaderControlsSettings();
          verifyMixedPageGestures();
          verifySideButtonMigrationAndInput();
          LOG_INF("SMOKE", "Simulator smoke test passed: gesture shortcuts");
          std::_Exit(0);
        }
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_C3_FONT_SIZES")) {
          verifyBitmapFontSizes();
          LOG_INF("SMOKE", "Simulator smoke test passed: bitmap font sizes");
          std::_Exit(0);
        }
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_DICTIONARY")) verifyDictionaryElisions();
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_CHAPTER_PERCENTAGE")) {
          RenderLock lock;
          const auto capture = [&](const std::string& name) { captureStatusBarScreen(name.c_str()); };
          if (!StatusBarFeatureSmokeTest::chapterPercentage(lock, capture)) fail("Chapter percentage controls failed");
          LOG_INF("SMOKE", "Simulator smoke test passed: chapter percentage");
          std::_Exit(0);
        }
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_CENTERED_STATUS_BAR")) {
          RenderLock lock;
          const auto capture = [&](const std::string& name) { captureStatusBarScreen(name.c_str()); };
          if (!StatusBarFeatureSmokeTest::centeredBar(lock, capture)) fail("Centered status bar settings failed");
          LOG_INF("SMOKE", "Simulator smoke test passed: centered status bar");
          std::_Exit(0);
        }
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_STATUS_BAR_FEATURE")) {
          verifyStatusBarFeature();
          LOG_INF("SMOKE", "Simulator smoke test passed: status bar feature");
          std::_Exit(0);
        }
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_STATUS_BARS")) {
          verifyStatusBarSettings();
          SETTINGS.clockDateHasBeenSynced = true;
          SETTINGS.dateFormat = CrossPointSettings::DATE_FORMAT_YEAR_MONTH_DAY_NUMERIC;
          SETTINGS.dateSeparator = CrossPointSettings::DATE_SEPARATOR_HYPHEN;
          SETTINGS.displayStatusBar.slots = {ReaderStatusBarItem::Clock, ReaderStatusBarItem::Date,
                                             ReaderStatusBarItem::Battery};
          activityManager.replaceActivity(
              std::make_unique<StatusBarSettingsActivity>(renderer, mappedInputManager, false, false, true));
          queueStep("Display status bar editor", SmokeStep::StatusBarEditor, 4);
          break;
        }

        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_HOME_THEMES")) {
          if (!mappedInputManager.hasHomeKey() || !mappedInputManager.hasTouchHardware())
            fail("Home theme regression requires the X4 Pro simulator");
          for (const char* path : {"/books/theme-first.txt", "/books/theme-second.txt"}) {
            if (!Storage.writeFile(path, "Home theme fixture")) fail("Cannot create Home theme fixture");
            RECENT_BOOKS.addOrUpdateBook(path, path, {}, {}, RecentBook::CoverState::Missing);
          }
          SETTINGS.uiTheme = CrossPointSettings::LYRA_CAROUSEL;
          SETTINGS.uiScale = CrossPointSettings::UI_SCALE_SMALL;
          UITheme::getInstance().reload();
          homeThemeBookPath = "/books/theme-first.txt";
          activityManager.replaceActivity(std::make_unique<HomeActivity>(
              renderer, mappedInputManager, HomeMenuItem::NONE, HalDisplay::FAST_REFRESH, homeThemeBookPath));
          queueStep("Initial carousel Home", SmokeStep::ThemeHome, 8);
          break;
        }
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_FRONTLIGHT_LAYOUT")) {
          if (!mappedInputManager.hasHomeKey() || !mappedInputManager.hasTouchHardware())
            fail("Frontlight layout regression requires the X4 Pro simulator");
          activityManager.replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInputManager));
          queueStep("Frontlight layout Home", SmokeStep::FrontlightLayout, 4);
          break;
        }
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_TILT_SETTINGS")) {
          verifyTiltPageTurnSettings();
          std::_Exit(0);
        }
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_ROTATE_SHORTCUTS")) {
          verifyRotateShortcutPickers();
          verifyUpDownShortcutAvailability();
          if (std::getenv("CROSSINK_SIMULATOR_SMOKE_ROTATE_WEB")) {
            // Serve the production page/API for an external browser round-trip check.
            WiFi.mode(WIFI_AP);
            auto server = std::make_unique<CrossPointWebServer>();
            server->begin();
            if (!server->isRunning()) fail("Shortcut web test server did not start");
            const auto started = millis();
            while (millis() - started < 240000) {
              server->handleClient();
              delay(5);
            }
            server->stop();
          }
          std::_Exit(0);
        }
        verifyStatusBarTextSizes();
        verifyLoadingPopupBackdrop();
        verifyCachedHomeProgressMigration();
        if (!CrossPointSettings::verifySleepTimeoutMigrationContract()) {
          fail("Sleep timeout migration contract failed");
        }
        if (!CrossPointSettings::verifySleepScreenMigrationContract()) {
          fail("Sleep screen migration contract failed");
        }
        if (!SimulatorHomeKeyInput::verifyTimingContract()) {
          fail("Simulator Home key timing contract failed");
        }
        verifyUpDownShortcutAvailability();
        verifySideButtonMigrationAndInput();
        verifyReaderControlsSettings();
        verifyStatusBarSettings();
        verifyMixedPageGestures();
#if CROSSINK_SCALABLE_FONTS
        verifyBlockFontSizes(sdFontSystem.ensureBuiltInReaderFont(renderer));
        if (const char* family = std::getenv("CROSSINK_SIMULATOR_SMOKE_FONT_FAMILY")) {
          // Exercise the production registry, adapter, size cache, and dictionary
          // handoff before the normal reader navigation smoke sequence.
          sdFontSystem.ensureRegistry();
          sdFontSystem.releaseRegistry();
          sdFontSystem.ensureRegistry();
          for (const auto& summary : sdFontSystem.registry().getFamilies()) {
            if (!summary.files.empty()) fail("Font names eagerly loaded detail paths");
          }
          const auto* info = sdFontSystem.registry().findFamily(family);
          if (!info || !info->isScalable()) fail("TTF family not discovered: %s", family);
          const auto picker = buildFontFamilySetting(&sdFontSystem.registry());
          const std::string expectedLabel = std::string(family) + " (8-22pt)";
          const auto item = std::find(picker.enumStringValues.begin(), picker.enumStringValues.end(), expectedLabel);
          if (item == picker.enumStringValues.end()) fail("TTF family missing from picker: %s", family);
          picker.valueSetter(static_cast<uint8_t>(item - picker.enumStringValues.begin()));
          if (std::strcmp(SETTINGS.sdFontFamilyName, family) != 0) fail("TTF picker selected the wrong family");
          SETTINGS.readerFontPointSize = 12;
          sdFontSystem.ensureLoaded(renderer);
          const int original = SETTINGS.getReaderFontId();
          verifyBlockFontSizes(original);
          const TtfRenderProfile initialProfile = TTF_RENDER_PROFILES.profileFor(family);
          TtfRenderProfile nativeProfile = initialProfile;
          nativeProfile.hinting = 1;  // TtfRenderProfile: Native
          nativeProfile.interpreter = 0;
          if (!TTF_RENDER_PROFILES.setProfile(family, nativeProfile)) fail("TTF native profile was not persisted");
          if (!sdFontSystem.reloadActiveScalableFamily(renderer, family)) fail("TTF native profile reload failed");
          const int nativeId = SETTINGS.getReaderFontId();
          if (nativeId == original || nativeId == SETTINGS.getBuiltInReaderFontId())
            fail("TTF native profile did not replace the resident font identity");
          if (!TTF_RENDER_PROFILES.setProfile(family, initialProfile)) fail("TTF initial profile was not restored");
          if (!sdFontSystem.reloadActiveScalableFamily(renderer, family)) fail("TTF initial profile reload failed");
          if (SETTINGS.getReaderFontId() != original) fail("TTF profile round trip changed font identity");
          if (ReaderUtils::shouldShowFontPreviewLoading(family)) {
            fail("Resident TTF family incorrectly requests loading feedback");
          }
          sdFontSystem.releaseLoadedFont(renderer);
          if (!ReaderUtils::shouldShowFontPreviewLoading(family)) {
            fail("Released TTF family suppressed loading feedback");
          }
          sdFontSystem.ensureLoaded(renderer);
          if (ReaderUtils::shouldShowFontPreviewLoading(family)) {
            fail("Reloaded TTF family incorrectly requests loading feedback");
          }
          sdFontSystem.releaseRegistry();
          if (!ReaderUtils::shouldShowFontPreviewLoading(family)) {
            fail("Released TTF catalog suppressed loading feedback");
          }
          sdFontSystem.ensureRegistry();
          if (ReaderUtils::shouldShowFontPreviewLoading(family)) {
            fail("Reloaded TTF catalog incorrectly requests loading feedback");
          }
          sdFontSystem.markRegistryDirty();
          if (!ReaderUtils::shouldShowFontPreviewLoading(family)) {
            fail("Dirty TTF family suppressed loading feedback");
          }
          sdFontSystem.ensureLoaded(renderer);
          if (ReaderUtils::shouldShowFontPreviewLoading(family)) {
            fail("Reloaded dirty TTF family incorrectly requests loading feedback");
          }
          SETTINGS.readerFontPointSize = 22;
          sdFontSystem.releaseRegistry();
          if (ReaderUtils::changeReaderFontSizeWithFeedback(renderer, true, FontSizeStepMode::Clamp) ||
              SETTINGS.readerFontPointSize != 22)
            fail("Cold resize at maximum size did not remain clamped");
          SETTINGS.readerFontPointSize = 12;
          sdFontSystem.releaseRegistry();
          if (!ReaderUtils::changeReaderFontSizeWithFeedback(renderer, true, FontSizeStepMode::Clamp) ||
              SETTINGS.readerFontPointSize != 13)
            fail("Resize did not lazily reload indexed SD sizes");
          sdFontSystem.ensureLoaded(renderer);
          if (!ReaderUtils::changeReaderFontSizeWithFeedback(renderer, false, FontSizeStepMode::Clamp) ||
              SETTINGS.readerFontPointSize != 12)
            fail("Reverse resize did not use indexed sizes");
          sdFontSystem.ensureLoaded(renderer);
          if (original == SETTINGS.getBuiltInReaderFontId()) fail("TTF activation fell back");
          for (uint8_t points : {uint8_t(14), uint8_t(12)}) {
            SETTINGS.readerFontPointSize = points;
            sdFontSystem.ensureLoaded(renderer);
            if (SETTINGS.getReaderFontId() == SETTINGS.getBuiltInReaderFontId()) fail("TTF size activation failed");
          }
          if (SETTINGS.getReaderFontId() != original) fail("TTF size identity changed on reuse");
          if (std::getenv("CROSSINK_SIMULATOR_SMOKE_ISOLATED_FONTS")) {
            // A clean resize must use resident metadata, even with the cache temporarily unavailable.
            namespace fs = std::filesystem;
            fs::rename("fs_/.crosspoint/font-catalog.bin", "fs_/.crosspoint/font-catalog.saved");
            if (!ReaderUtils::changeReaderFontSizeWithFeedback(renderer, true, FontSizeStepMode::Clamp))
              fail("Clean resize failed");
            sdFontSystem.ensureLoaded(renderer);
            if (fs::exists("fs_/.crosspoint/font-catalog.bin")) fail("Clean resize reread/rebuilt the index");
            fs::rename("fs_/.crosspoint/font-catalog.saved", "fs_/.crosspoint/font-catalog.bin");
            if (!ReaderUtils::changeReaderFontSizeWithFeedback(renderer, false, FontSizeStepMode::Clamp))
              fail("Clean reverse resize failed");
            sdFontSystem.ensureLoaded(renderer);
          }
          const auto dictionary = sdFontSystem.activateDictionaryFont(renderer, family, 16);
          if (!dictionary.usingDictionaryFont) fail("TTF dictionary activation failed");
          if (sdFontSystem.restoreReaderFont(renderer) != original) fail("TTF reader restoration changed identity");
          // Cold dictionary activation has no persistent book-layout identity.
          // Restoring that same family must rebuild its normal reader identity.
          sdFontSystem.releaseLoadedFont(renderer);
          const auto temporaryDictionary = sdFontSystem.activateDictionaryFont(renderer, family, 12);
          if (!temporaryDictionary.usingDictionaryFont || temporaryDictionary.fontId == original)
            fail("Cold TTF dictionary did not use a temporary identity");
          const auto reusedDictionary = sdFontSystem.activateDictionaryFont(renderer, family, 12);
          if (reusedDictionary.fontId != temporaryDictionary.fontId)
            fail("Active TTF dictionary family was not reused");
          if (sdFontSystem.restoreReaderFont(renderer) != original)
            fail("Temporary dictionary identity escaped into reader restoration");
          sdFontSystem.releaseLoadedFont(renderer);
          if (!sdFontSystem.activateDictionaryFont(renderer, family, 12).usingDictionaryFont)
            fail("Cold TTF dictionary reload failed");
          sdFontSystem.ensureLoaded(renderer);
          if (SETTINGS.getReaderFontId() != original)
            fail("Reader ensureLoaded reused a temporary dictionary identity");
          sdFontSystem.releaseLoadedFont(renderer);
          sdFontSystem.ensureLoaded(renderer);
          if (SETTINGS.getReaderFontId() != original) fail("TTF reload changed identity");
          if (std::getenv("CROSSINK_SIMULATOR_SMOKE_ISOLATED_FONTS")) {
            namespace fs = std::filesystem;
            // The runner provides disposable copies; never mutate a user's SD tree.
            const auto* current = sdFontSystem.registry().findFamily(family);
            const std::string firstPath = "fs_" + current->files.front().path;
            fs::copy("fs_/fonts", "fs_/font-smoke-backup", fs::copy_options::recursive);
            sdFontSystem.releaseLoadedFont(renderer);
            // Budget failures must preserve selection, release partial faces,
            // and permit a subsequent valid load without stale renderer IDs.
            const auto originalBytes = fs::file_size(firstPath);
            fs::resize_file(firstPath, HalScalableFont::MaxFileBytes + 1);
            sdFontSystem.ensureLoaded(renderer);
            if (SETTINGS.getReaderFontId() != SETTINGS.getBuiltInReaderFontId() ||
                std::strcmp(SETTINGS.sdFontFamilyName, family) != 0)
              fail("Oversized TTF file did not preserve selection and fall back");
            fs::resize_file(firstPath, originalBytes);
            if (current->files.size() == 4) {
              for (const auto& face : current->files) fs::resize_file("fs_" + face.path, HalScalableFont::MaxFileBytes);
              sdFontSystem.ensureLoaded(renderer);
              if (SETTINGS.getReaderFontId() != SETTINGS.getBuiltInReaderFontId() ||
                  std::strcmp(SETTINGS.sdFontFamilyName, family) != 0)
                fail("Oversized TTF family did not preserve selection and fall back");
              for (const auto& face : current->files) {
                const fs::path target = "fs_" + face.path;
                const auto backup = fs::path("fs_/font-smoke-backup") / fs::relative(target, "fs_/fonts");
                fs::copy_file(backup, target, fs::copy_options::overwrite_existing);
              }
            }
            sdFontSystem.ensureLoaded(renderer);
            if (SETTINGS.getReaderFontId() != original) fail("TTF budget failure did not recover");
            sdFontSystem.releaseLoadedFont(renderer);
            {
              std::ofstream changed(firstPath, std::ios::binary | std::ios::app);
              changed.put('X');
            }
            sdFontSystem.markRegistryDirty();
            sdFontSystem.refreshIfDirty();  // A picker must not consume the active-font reload signal.
            sdFontSystem.ensureLoaded(renderer);
            if (SETTINGS.getReaderFontId() == original ||
                SETTINGS.getReaderFontId() == SETTINGS.getBuiltInReaderFontId())
              fail("TTF replacement did not change content identity");
            sdFontSystem.releaseLoadedFont(renderer);
            // Reproduce the source location without interpreting font metadata as a path.
            const fs::path sourceFolder = fs::path(firstPath).parent_path();
            const fs::path duplicateFolder = sourceFolder == fs::path("fs_/fonts")
                                                 ? fs::path("fs_/.fonts")
                                                 : fs::path("fs_/.fonts") / sourceFolder.filename();
            fs::create_directories(duplicateFolder);
            fs::copy_file(firstPath, duplicateFolder / "duplicate.ttf", fs::copy_options::overwrite_existing);
            constexpr const char* separateFamily = "TTF Smoke Separate Family";
            const fs::path separateFolder = fs::path("fs_/fonts") / separateFamily;
            fs::create_directories(separateFolder);
            fs::copy_file(firstPath, separateFolder / "same-metadata.ttf");
            sdFontSystem.markRegistryDirty();
            sdFontSystem.refreshIfDirty();
            if (!sdFontSystem.registry().findFamily(separateFamily)) fail("TTF folder name was ignored");
            FontInstaller installer(sdFontSystem.registry());
            if (installer.deleteFamily(family) != FontInstaller::Error::OK) fail("TTF family deletion failed");
            sdFontSystem.markRegistryDirty();
            sdFontSystem.ensureLoaded(renderer);
            sdFontSystem.refreshIfDirty();
            if (sdFontSystem.registry().findFamily(family) || SETTINGS.sdFontFamilyName[0])
              fail("Deleted TTF family reappeared from duplicate files");
            if (!sdFontSystem.registry().findFamily(separateFamily) ||
                !fs::exists(separateFolder / "same-metadata.ttf"))
              fail("Deleting TTF family removed a different folder with the same metadata");
            fs::remove_all("fs_/fonts");
            fs::rename("fs_/font-smoke-backup", "fs_/fonts");
            sdFontSystem.markRegistryDirty();
            std::snprintf(SETTINGS.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName), "%s", family);
            sdFontSystem.ensureLoaded(renderer);
            if (SETTINGS.getReaderFontId() != original) fail("Restored TTF content changed identity");
            LOG_INF("SMOKE", "TTF replacement, duplicate deletion and restoration passed");
          }
          LOG_INF("SMOKE", "TTF discovery, size reuse, dictionary and reload passed");
        }
#endif
        applyRequestedTheme();
        homeReaderReaderKind = 0;
        homeReaderCancelledMask = 0;
        homeReaderConfirmationAccepted = false;
        activityManager.replaceActivity(std::make_unique<HomeReaderSmokeActivity>(
            homeReaderSmokeReaderName(homeReaderReaderKind), true, renderer, mappedInputManager, true));
        queueStep("Home/Reader reader owner", SmokeStep::HomeReaderReader);
        break;

      case SmokeStep::HomeReaderReader:
        homeReaderSmokeReader = activityManager.simulatorCurrentActivity();
        if (!homeReaderSmokeReader || !homeReaderSmokeReader->isBookReaderActivity()) {
          fail("Home/Reader smoke reader did not become current");
        }
        homeReaderCancelledMask = 0;
        homeReaderSmokeReader->startActivityForResult(
            std::make_unique<HomeReaderSmokeActivity>("ReaderMenu", true, renderer, mappedInputManager),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) fail("Home/Reader confirmed the first nested menu");
              homeReaderCancelledMask |= 1;
            });
        queueStep("Home/Reader nested menu", SmokeStep::HomeReaderNested);
        break;

      case SmokeStep::HomeReaderNested:
        activityManager.simulatorCurrentActivity()->startActivityForResult(
            std::make_unique<HomeReaderSmokeActivity>("NestedSettings", false, renderer, mappedInputManager),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) fail("Home/Reader confirmed nested settings");
              homeReaderCancelledMask |= 2;
            });
        queueStep("Home/Reader destructive confirmation", SmokeStep::HomeReaderConfirmation);
        break;

      case SmokeStep::HomeReaderConfirmation:
        activityManager.simulatorCurrentActivity()->startActivityForResult(
            std::make_unique<HomeReaderSmokeActivity>("Confirmation", false, renderer, mappedInputManager),
            [this](const ActivityResult& result) {
              homeReaderConfirmationAccepted = !result.isCancelled;
              homeReaderCancelledMask |= 4;
            });
        queueStep("Home/Reader trigger", SmokeStep::HomeReaderTrigger);
        break;

      case SmokeStep::HomeReaderTrigger:
        if (!activityManager.handleShortcutAction(CrossPointSettings::HOME_READER) ||
            !activityManager.handleShortcutAction(CrossPointSettings::HOME_READER)) {
          fail("Repeated Home/Reader press was not consumed");
        }
        queueStep("Home/Reader unwound to reader", SmokeStep::HomeReaderUnwound);
        break;

      case SmokeStep::HomeReaderUnwound:
        if (activityManager.simulatorCurrentActivity() != homeReaderSmokeReader || homeReaderCancelledMask != 7 ||
            homeReaderConfirmationAccepted) {
          fail("Home/Reader did not cancel every nested screen back to the existing reader");
        }
        if (!activityManager.handleShortcutAction(CrossPointSettings::HOME_READER)) {
          fail("Home/Reader from the reader was not consumed");
        }
        queueStep("Home/Reader reader to Home", SmokeStep::HomeReaderReturnedHome);
        break;

      case SmokeStep::HomeReaderReturnedHome: {
        if (!activityManager.isHomeActivity()) fail("Home/Reader from the reader did not return Home");
        Activity* const homeBeforeNoOp = activityManager.simulatorCurrentActivity();
        if (!activityManager.handleShortcutAction(CrossPointSettings::HOME_READER) ||
            activityManager.simulatorCurrentActivity() != homeBeforeNoOp) {
          fail("Home/Reader was not a no-op on Home");
        }
        if (++homeReaderReaderKind < 3) {
          activityManager.replaceActivity(std::make_unique<HomeReaderSmokeActivity>(
              homeReaderSmokeReaderName(homeReaderReaderKind), true, renderer, mappedInputManager, true));
          queueStep("Home/Reader next reader type", SmokeStep::HomeReaderReader);
        } else {
          activityManager.simulatorCurrentActivity()->startActivityForResult(
              std::make_unique<HomeReaderSmokeActivity>("Settings", false, renderer, mappedInputManager),
              [](const ActivityResult&) {});
          queueStep("Home/Reader non-reader parent", SmokeStep::HomeReaderNonReaderParent);
        }
        break;
      }

      case SmokeStep::HomeReaderNonReaderParent:
        activityManager.simulatorCurrentActivity()->startActivityForResult(
            std::make_unique<HomeReaderSmokeActivity>("Browser", false, renderer, mappedInputManager),
            [](const ActivityResult&) {});
        queueStep("Home/Reader non-reader nested", SmokeStep::HomeReaderNonReaderNested);
        break;

      case SmokeStep::HomeReaderNonReaderNested:
        if (!activityManager.handleShortcutAction(CrossPointSettings::HOME_READER) ||
            !activityManager.handleShortcutAction(CrossPointSettings::HOME_READER)) {
          fail("Home/Reader outside a reader was not consumed");
        }
        queueStep("Home/Reader non-reader to Home", SmokeStep::HomeReaderNonReaderHome);
        break;

      case SmokeStep::HomeReaderNonReaderHome:
        if (!activityManager.isHomeActivity()) fail("Home/Reader from non-reader menus did not return Home");
        activityManager.replaceActivity(
            std::make_unique<HomeReaderSmokeActivity>("EpubReader", true, renderer, mappedInputManager, true));
        queueStep("Back/Home regression reader", SmokeStep::BackHomeReader);
        break;

      case SmokeStep::BackHomeReader:
        backHomeChildCancelled = false;
        activityManager.simulatorCurrentActivity()->startActivityForResult(
            std::make_unique<HomeReaderSmokeActivity>("ReaderMenu", false, renderer, mappedInputManager),
            [this](const ActivityResult& result) { backHomeChildCancelled = result.isCancelled; });
        queueStep("Back/Home nested menu", SmokeStep::BackHomeNested);
        break;

      case SmokeStep::BackHomeNested:
        if (!activityManager.handleShortcutAction(CrossPointSettings::BACK_HOME))
          fail("Back/Home did not handle nested reader menu");
        queueStep("Back/Home returned one level", SmokeStep::BackHomeReturnedReader);
        break;

      case SmokeStep::BackHomeReturnedReader:
        if (!activityManager.simulatorCurrentActivity()->isReaderActivity() || !backHomeChildCancelled) {
          fail("Back/Home no longer pops exactly one canceled nested activity");
        }
        if (!activityManager.handleShortcutAction(CrossPointSettings::BACK_HOME))
          fail("Back/Home did not return from the reader");
        queueStep("Back/Home returned Home", SmokeStep::BackHomeReturnedHome);
        break;

      case SmokeStep::BackHomeReturnedHome:
        if (!activityManager.isHomeActivity()) fail("Back/Home from the reader did not return Home");
        savedNavigationMenu = SETTINGS.longPressMenuAction;
        savedNavigationBack = SETTINGS.longPressBackAction;
        navigationLongPass = 0;
        activityManager.goToReader(std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK"), true);
        queueStep("Navigation long-press reader", SmokeStep::NavigationLongReader, 8);
        break;

      case SmokeStep::NavigationLongReader: {
        if (!activityManager.isCurrentActivityNamed("EpubReader")) fail("Navigation long-press reader did not open");
        const bool back = navigationLongPass >= 2;
        const bool homeReader = navigationLongPass % 2 != 0;
        SETTINGS.longPressMenuAction = back         ? CrossPointSettings::LONG_MENU_OFF
                                       : homeReader ? CrossPointSettings::LONG_MENU_HOME_READER
                                                    : CrossPointSettings::LONG_MENU_BACK_HOME;
        SETTINGS.longPressBackAction =
            back ? homeReader ? CrossPointSettings::LONG_MENU_HOME_READER : CrossPointSettings::LONG_MENU_BACK_HOME
                 : CrossPointSettings::LONG_MENU_OFF;
        mappedInputManager.simulatorInjectPress(back ? MappedInputManager::Button::Back
                                                     : MappedInputManager::Button::Confirm);
        navigationPressAt = millis();
        step = SmokeStep::NavigationLongHeld;
        break;
      }

      case SmokeStep::NavigationLongHeld:
        if (millis() - navigationPressAt < 1400) break;
        if (!activityManager.isHomeActivity())
          fail("Navigation hold did not return Home (pass %u)", navigationLongPass);
        navigationExpected = activityManager.simulatorCurrentActivity();
        mappedInputManager.simulatorInjectRelease(navigationLongPass >= 2 ? MappedInputManager::Button::Back
                                                                          : MappedInputManager::Button::Confirm);
        queueStep("Navigation release consumed", SmokeStep::NavigationLongReleased);
        break;

      case SmokeStep::NavigationLongReleased: {
        if (activityManager.simulatorCurrentActivity() != navigationExpected)
          fail("Navigation release leaked into the destination screen (pass %u)", navigationLongPass);
        if (++navigationLongPass < 4) {
          activityManager.goToReader(std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK"), true);
          queueStep("Next navigation long-press", SmokeStep::NavigationLongReader, 8);
        } else {
          SETTINGS.longPressMenuAction = savedNavigationMenu;
          SETTINGS.longPressBackAction = savedNavigationBack;
          activityManager.replaceActivity(
              std::make_unique<NavigationPickerSmokeActivity>(renderer, mappedInputManager));
          queueStep("Navigation shortcut picker", SmokeStep::NavigationPicker);
        }
        break;
      }

      case SmokeStep::NavigationPicker:
        if (const char* path = std::getenv("CROSSINK_SIMULATOR_SMOKE_NAVIGATION_CAPTURE")) {
          FILE* image = std::fopen(path, "wb");
          if (!image) fail("Cannot create navigation picker capture");
          RenderLock lock;
          const int width = renderer.getScreenWidth();
          const int height = renderer.getScreenHeight();
          std::fprintf(image, "P5\n%d %d\n255\n", width, height);
          for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) std::fputc(renderer.isPixelBlack(x, y) ? 0 : 255, image);
          }
          std::fclose(image);
        }
        LOG_INF("SMOKE", "Navigation catalogs, persistence, nested menus, holds and releases passed");
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_NAVIGATION_ONLY")) {
          LOG_INF("SMOKE", "Simulator smoke test passed: navigation shortcuts");
          std::_Exit(0);
        }
        activityManager.goHome();
        queueStep("Home", SmokeStep::Home);
        break;

      case SmokeStep::Home:
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK_LOADING")) {
          RenderLock lock;
          BookLoadingFeedback feedback;
          feedback.start();
          if (!feedback.showIfPending(renderer)) fail("Book open omitted immediate loading feedback");
          captureStatusBarScreen("book-loading-immediate");
          if (feedback.showIfPending(renderer)) fail("Book loading feedback refreshed twice");
          const auto frameHash = []() {
            uint32_t hash = 2166136261U;
            for (int y = 0; y < renderer.getScreenHeight(); ++y) {
              for (int x = 0; x < renderer.getScreenWidth(); ++x) {
                hash = (hash ^ static_cast<uint32_t>(renderer.isPixelBlack(x, y))) * 16777619U;
              }
            }
            return hash;
          };
          const auto originalOrientation = renderer.getOrientation();
          for (const auto orientation : {GfxRenderer::Portrait, GfxRenderer::LandscapeClockwise,
                                         GfxRenderer::PortraitInverted, GfxRenderer::LandscapeCounterClockwise}) {
            // Reproduce the old handoff: portrait Loading followed by rotated Indexing.
            renderer.setOrientation(GfxRenderer::Portrait);
            feedback.start();
            feedback.showIfPending(renderer);
            renderer.setOrientation(orientation);
            feedback.showIndexing(renderer);
            char capture[48];
            snprintf(capture, sizeof(capture), "book-indexing-orientation-%d", static_cast<int>(orientation));
            captureStatusBarScreen(capture);
            const uint32_t actualFrame = frameHash();
            renderer.clearScreen();
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            if (actualFrame != frameHash()) fail("Indexing retained pixels from the previous notice");
            if (feedback.showIfPending(renderer)) fail("Loading replaced Indexing");
          }
          renderer.setOrientation(originalOrientation);
          LOG_INF("SMOKE", "Immediate book loading feedback passed");
        }
        activityManager.goToFileBrowser("/books");
        queueStep("File Browser", SmokeStep::FileBrowser);
        break;

      case SmokeStep::FileBrowser:
#if CROSSINK_APP_CAP_TOUCH
        if (mappedInputManager.hasTouchHardware()) {
          buildFileBrowserInputScript();
          step = SmokeStep::ReaderInput;
          break;
        }
#endif
        prepareRecentLibrary();
        activityManager.goToLibrary();
        queueStep("Recent Library", SmokeStep::RecentLibrary);
        break;

      case SmokeStep::FileBrowserSettings:
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_FRONTLIGHT_SYNC")) {
          if (!mappedInputManager.hasHomeKey()) fail("Frontlight sync regression requires X4 Pro simulator");
          LOG_INF("SMOKE", "Frontlight sync: stats on/off menu and dispatch checks passed");
          step = SmokeStep::Done;
          break;
        }
        prepareRecentLibrary();
        activityManager.goToLibrary();
        queueStep("Recent Library", SmokeStep::RecentLibrary);
        break;

      case SmokeStep::RecentLibrary: {
        RenderLock lock;
        auto* activity = static_cast<LibraryActivity*>(activityManager.simulatorCurrentActivity());
        if (Storage.exists(library::libraryIndexPath()) || !library::libraryIndexNeedsRefresh())
          fail("Recently Opened built the missing Library index");
        RecentBook book;
        if (activity->simulatorRowCount() != 18 || !activity->simulatorReadBook(0, book) ||
            book.path != "/books/recent-smoke-19.txt")
          fail("Recent Library did not show bounded history in newest-first order");
        activity->simulatorSetView(4, false);
        if (!activity->simulatorReadBook(0, book) || book.path != "/books/recent-smoke-2.txt")
          fail("Recent Library did not reverse history");
        activity->simulatorSetView(4, true, "Author 19");
        if (activity->simulatorRowCount() != 1 || !activity->simulatorReadBook(0, book) ||
            book.path != "/books/recent-smoke-19.txt")
          fail("Recent Library author search failed");
        SETTINGS.libraryShowTxt = 0;
        activity->simulatorSetView(4, true);
        if (activity->simulatorRowCount() != 0) fail("Recent Library file filter failed");
        SETTINGS.libraryShowTxt = 1;
        SETTINGS.libraryUseMetadata = 0;
        activity->simulatorSetView(4, true, "recent-smoke-19");
        if (activity->simulatorRowCount() != 1 || !activity->simulatorReadBook(0, book) ||
            book.title != "recent-smoke-19" || !book.author.empty())
          fail("Recent Library filename display/search failed");
        SETTINGS.libraryUseMetadata = 1;
        // An unreadable index must also be irrelevant to a history refresh.
        if (!Storage.writeFile(library::libraryIndexPath(), "broken")) fail("Cannot write broken index fixture");
        activity->simulatorSetView(4, true);
        activity->simulatorRefresh();
        FsFile broken;
        if (!Storage.openFileForRead("SMOKE", library::libraryIndexPath(), broken)) fail("Missing broken index");
        const auto brokenSize = broken.size();
        broken.close();
        if (brokenSize != 6 || !library::libraryIndexNeedsRefresh() || activity->simulatorRowCount() != 18)
          fail("Recent Library refreshed the full index");
        activity->simulatorSetView(1, false);
        activity->simulatorSetView(4, true);
        if (activity->simulatorRowCount() != 18 || !activity->simulatorReadBook(0, book))
          fail("Recent Library stayed unavailable after a failed full scan");
        const char* epubPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
        if (epubPath) {
          const std::string cachePath = Epub(epubPath, "/.crosspoint").getCachePath();
          if (!Storage.exists(cachePath.c_str()) && !Storage.mkdir(cachePath.c_str()))
            fail("Cannot create completed-book cache");
          auto stats = BookReadingStats::load(cachePath);
          stats.isCompleted = true;
          if (!stats.save(cachePath)) fail("Cannot save completed-book fixture");
          RECENT_BOOKS.addOrUpdateBook(epubPath, "Completed smoke book", "", "");
          SETTINGS.libraryHideFinishedBooks = 1;
          activity->simulatorSetView(4, true);
          if (activity->simulatorRowCount() != 17) fail("Recent Library did not hide finished EPUB");
          SETTINGS.libraryHideFinishedBooks = 0;
          activity->simulatorSetView(4, true);
          if (activity->simulatorRowCount() != 18 || !activity->simulatorReadBook(0, book) || book.path != epubPath)
            fail("Recent Library did not restore finished EPUB");
          RECENT_BOOKS.removeByPath(epubPath);
          stats.isCompleted = false;
          if (!stats.save(cachePath)) fail("Cannot restore completed-book fixture");
        }
        for (int i = 0; i < 20; ++i) {
          const std::string path = "/books/recent-smoke-" + std::to_string(i) + ".txt";
          if (!Storage.remove(path.c_str())) fail("Cannot remove recent fixture");
        }
        activity->simulatorRefresh();
        if (activity->simulatorRowCount() != 0) fail("Recent Library did not prune missing books");
        // The builder deliberately retains an unreadable previous index. Remove
        // that fixture before checking a deferred build from a missing index.
        if (!Storage.remove(library::libraryIndexPath())) fail("Cannot remove broken index fixture");
        // Switching to a full-library sort must still build the deferred index.
        activity->simulatorSetView(1, false);
        if (library::libraryIndexNeedsRefresh() || activity->simulatorRowCount() == 0)
          fail("Full Library did not build its deferred index");
        SETTINGS.librarySortMethod = 1;
        SETTINGS.librarySortDescending = 0;
        LOG_INF("SMOKE",
                "Recent Library missing/corrupt index, 18-book limit, ordering, search, filters and completion passed");
        queueStep("Library", SmokeStep::Library);
        break;
      }

      case SmokeStep::Library: {
        // Rendering an error screen is not a successful Library smoke test.
        // The script supplies an isolated card with at least one EPUB.
        library::LibraryIndexFile shelf;
        const bool hasFixture = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK") != nullptr;
        const bool readable = shelf.open(library::libraryIndexPath());
        const bool populated = readable && (!hasFixture || shelf.bookCount() > 0);
        const uint16_t books = shelf.bookCount();
        shelf.close();
        if (!populated) fail("Library did not publish a readable populated index");
        constexpr char REFRESH_FIXTURE[] = "/books/library-refresh-smoke.txt";
        if (library::libraryIndexNeedsRefresh()) fail("Successful Library scan stayed dirty");
        if (libraryRefreshPass == 0) {
          libraryBaselineBooks = books;
          auto* libraryActivity = static_cast<LibraryActivity*>(activityManager.simulatorCurrentActivity());
          const int beforeSelection = libraryActivity->simulatorSelection();
          {
            RenderLock busyRender;
            mappedInputManager.simulatorClearInputFrame();
            mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Down);
            libraryActivity->loop();
            mappedInputManager.simulatorClearInputFrame();
            mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Up);
            libraryActivity->loop();
            if (libraryActivity->simulatorPendingInputs() != 2 ||
                libraryActivity->simulatorSelection() != beforeSelection)
              fail("Library did not buffer navigation while rendering");
#if CROSSINK_APP_CAP_TOUCH
            mappedInputManager.simulatorClearInputFrame();
            mappedInputManager.simulatorInjectTouchDown(200, 300);
            libraryActivity->loop();
            mappedInputManager.simulatorClearInputFrame();
            mappedInputManager.simulatorInjectTouchMove(200, 100);
            libraryActivity->loop();
            mappedInputManager.simulatorClearInputFrame();
            mappedInputManager.simulatorInjectTouchRelease(200, 100);
            libraryActivity->loop();
            if (libraryActivity->simulatorPendingInputs() != 5)
              fail("Library did not buffer touch press, cancellation and scroll");
#endif
          }
          mappedInputManager.simulatorClearInputFrame();
          while (libraryActivity->simulatorPendingInputs()) libraryActivity->loop();
          const int selectionBeforeOverflow = libraryActivity->simulatorSelection();
          {
            RenderLock busyRender;
            for (size_t i = 0; i <= LibraryInputBuffer::CAPACITY; ++i) {
              mappedInputManager.simulatorClearInputFrame();
              mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Down);
              libraryActivity->loop();
            }
            if (libraryActivity->simulatorPendingInputs() != 1)
              fail("Library overflow did not cancel the partial input sequence");
          }
          mappedInputManager.simulatorClearInputFrame();
          libraryActivity->loop();
          if (libraryActivity->simulatorSelection() != selectionBeforeOverflow)
            fail("Library replayed navigation after input overflow");
          LOG_INF("SMOKE", "Library input buffering during rendering and overflow passed");
          // Deliberately bypass invalidation to prove that a normal return visit
          // reuses the index instead of walking the card again.
          if (!Storage.writeFile(REFRESH_FIXTURE, "Library refresh smoke fixture"))
            fail("Cannot create Library fixture");
        } else if (libraryRefreshPass == 1) {
          if (books != libraryBaselineBooks) fail("Library rescanned an unchanged session");
          library::invalidateLibraryIndex();
        } else if (libraryRefreshPass == 2) {
          if (books != libraryBaselineBooks + 1) fail("Library missed an invalidated addition");
          if (!Storage.remove(REFRESH_FIXTURE)) fail("Cannot delete Library fixture");
          library::invalidateLibraryIndex();
        } else if (books != libraryBaselineBooks) {
          fail("Library missed an invalidated deletion");
        }
        if (libraryRefreshPass++ < 3) {
          activityManager.goToLibrary();
          queueStep("Library cache reuse and invalidation", SmokeStep::Library);
          break;
        }
        LOG_INF("SMOKE", "Library reuse, addition and deletion refresh passed");
        if (mappedInputManager.hasHomeKey()) {
          renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
        }
        KOREADER_STORE.setCredentials("smoke", "smoke-password");
        activityManager.replaceActivity(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInputManager));
        queueStep("Sync Settings", SmokeStep::StatsUploadEntry);
        break;
      }

      case SmokeStep::StatsUploadEntry:
        inputScript.clear();
        scriptIndex = 0;
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_SYNC_SERVER_CAPTURES")) {
          // Capture-only pass: top of the list, the scrolled sync groups, then a KOSync-only server.
          {
            RenderLock lock;
            captureStatusBarScreen("sync-server-1-top");
          }
          inputCompletionStep = SmokeStep::SyncServerCaptureBottom;
          for (int i = 0; i < 6; ++i) addTap(MappedInputManager::Button::Down);
          inputScript.push_back(render("Sync Server scrolled", 6));
          step = SmokeStep::ReaderInput;
          break;
        }
        inputCompletionStep = SmokeStep::StatsUploadReturn;
        // Sync actions live in File Transfer. With stats and clippings sync off,
        // Sync Books is the last Sync Server row, just above the Nearby rows that
        // end the list.
        activityManager.goToFileTransfer();
        inputScript.push_back(render("File Transfer", 10));
        inputScript.push_back(assertActivity("NetworkModeSelection"));
        for (int i = 0; i < nearbyFileTransferRows() + 1; ++i) {
          addTap(MappedInputManager::Button::Up);
          inputScript.push_back(render("File Transfer row up", 5));
        }
        inputScript.push_back(render("Sync Books selected", 5));
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(assertActivity("StatsUpload"));
        inputScript.push_back(render("Sync Books confirmation", 10));
        addTap(MappedInputManager::Button::Back);  // Opening does not upload.
        inputScript.push_back(assertActivity("Home"));
        step = SmokeStep::ReaderInput;
        break;

      case SmokeStep::SyncServerCaptureBottom: {
        {
          RenderLock lock;
          captureStatusBarScreen("sync-server-2-what-to-sync");
        }
        KOREADER_STORE.setServerUrl("http://kosync.invalid");
        KOREADER_STORE.setServerSupport(SyncServerSupport::UNSUPPORTED);
        activityManager.requestUpdate();
        queueStep("Sync Server progress-only", SmokeStep::SyncServerCaptureUnsupported, 6);
        break;
      }

      case SmokeStep::SyncServerCaptureUnsupported: {
        {
          RenderLock lock;
          captureStatusBarScreen("sync-server-3-progress-only");
        }
        // Reading Stats "This Device" page offers Sync Reading Stats once the server accepts stats.
        GlobalReadingStats device;
        device.totalSessions = 12;
        device.totalReadingSeconds = 3456;
        activityManager.replaceActivity(std::make_unique<BookStatsActivity>(
            renderer, mappedInputManager, "Fixture", std::string{}, BookReadingStats{}, -1.0f, false, 0, device));
        queueStep("Reading stats sync action", SmokeStep::SyncServerCaptureStats, 6);
        break;
      }

      case SmokeStep::SyncServerCaptureStats: {
        {
          RenderLock lock;
          captureStatusBarScreen("sync-server-4-stats-page");
        }
        // File Transfer's Sync Server rows, with stats and clippings sync on.
        KOREADER_STORE.setServerUrl("");
        KOREADER_STORE.setSyncStats(true);
        KOREADER_STORE.setSyncClippings(true);
        SETTINGS.trackReadingStats = 1;
        activityManager.goToFileTransfer();
        inputScript.clear();
        scriptIndex = 0;
        inputCompletionStep = SmokeStep::SyncServerCaptureFileTransfer;
        inputScript.push_back(render("File Transfer", 10));
        // Sync Clippings sits just above the Nearby rows that end the list.
        for (int i = 0; i < nearbyFileTransferRows() + 1; ++i) {
          addTap(MappedInputManager::Button::Up);
          inputScript.push_back(render("File Transfer row up", 5));
        }
        inputScript.push_back(render("File Transfer Sync Server rows", 6));
        step = SmokeStep::ReaderInput;
        break;
      }

      case SmokeStep::SyncServerCaptureFileTransfer: {
        {
          RenderLock lock;
          captureStatusBarScreen("sync-server-5-file-transfer");
        }
        LOG_INF("SMOKE", "Simulator smoke test passed: Sync Server captures");
        std::_Exit(0);
      }

      case SmokeStep::StatsUploadEmptyDone:
        if (std::getenv("CROSSINK_READING_TEST_TWICE") && !secondSyncStarted) {
          // A later sync of the same card: the catch-up is done, so only books
          // changed since (marked pending) are sent.
          secondSyncStarted = true;
          LOG_INF("SMOKE", "Starting second sync");
          if (std::getenv("CROSSINK_READING_TEST_FEED")) {
            // Incremental Sync Folder: the first walk prepared /read, so this run
            // pulls the change feed and pushes only the book marked pending.
            if (!Storage.writeFile("/second-sync-started", "1")) fail("Cannot mark second sync");
            if (!SyncPending::mark("/read/sub/second.EPUB", SyncPending::PROGRESS_BIT))
              fail("Cannot mark book progress pending");
            // first.epub also gets a pulled change: its pending stats must survive the apply.
            if (!SyncPending::mark("/read/first.epub", SyncPending::STATS_BIT)) fail("Cannot mark book stats pending");
            if (std::getenv("CROSSINK_READING_TEST_PARKED")) {
              // A remote position parked for second.EPUB (further ahead): the push
              // must compare with it (Smart applies it) instead of uploading.
              if (!parkRemoteFixture("/read/sub/second.EPUB")) fail("Cannot park a remote position");
            }
            activityManager.replaceActivity(std::make_unique<StatsUploadActivity>(
                renderer, mappedInputManager, "/read", StatsUploadActivity::Scope::Folder, true));
            inputScript.clear();
            scriptIndex = 0;
            inputCompletionStep = SmokeStep::StatsUploadEmptyDone;
            inputScript.push_back(render("Incremental sync result", 100));
            inputScript.push_back(assertActivity("StatsUpload"));
            step = SmokeStep::ReaderInput;
            break;
          }
          const bool clippingsOnly = std::getenv("CROSSINK_READING_TEST_CLIPPINGS_ONLY");
          if (clippingsOnly) {
            // Save a clipping the way the reader does; the store queues the book itself.
            if (!CLIPPINGS.loadForBook("/read/first.epub", "Fixture", "Writer", "epub") ||
                CLIPPINGS.addClipping(0, 3, 3, 10, 1, 3, 4, "Chapter", UINT16_MAX, "A later clipping", UINT16_MAX,
                                      1234) != ClippingStore::AddResult::Added)
              fail("Cannot add a clipping between syncs");
            CLIPPINGS.unload();
          } else if (const char* changed = std::getenv("CROSSINK_READING_TEST_MARK_PENDING")) {
            if (!SyncPending::mark(changed, SyncPending::STATS_BIT)) fail("Cannot mark book pending");
          }
          const auto scope =
              clippingsOnly ? StatsUploadActivity::Scope::Clippings : StatsUploadActivity::Scope::ReadingStats;
          activityManager.replaceActivity(
              std::make_unique<StatsUploadActivity>(renderer, mappedInputManager, std::string{}, scope, true));
          inputScript.clear();
          scriptIndex = 0;
          inputCompletionStep = SmokeStep::StatsUploadEmptyDone;
          inputScript.push_back(render("Second sync result", 100));
          inputScript.push_back(assertActivity("StatsUpload"));
          step = SmokeStep::ReaderInput;
          break;
        }
        if (std::getenv("CROSSINK_READING_TEST_STATS_ENTRY") && !statsParentHandlerRan)
          fail("Reading Stats parent handler did not run before the sync replaced the stack");
        LOG_INF("SMOKE", "Stats upload transport smoke passed");
        std::_Exit(0);

      case SmokeStep::KeptPositionDone: {
        const bool prepared = std::strcmp(std::getenv("CROSSINK_READING_TEST_OPEN_KEPT"), "unprepared") != 0;
        // Offered (and settled either way) only inside a prepared scope.
        if (SyncParked::empty() != prepared) fail("Kept position not settled as expected");
        const char* kept = std::getenv("CROSSINK_READING_TEST_OPEN_KEPT");
        if (std::strcmp(kept, "decline") == 0 || std::strcmp(kept, "behind") == 0) {
          // Staying here, or being further along already, queues this reader's
          // position for the next sync.
          SyncPending::Cursor cursor;
          std::string path;
          uint8_t mask = 0;
          bool queued = false;
          if (!cursor.begin()) fail("Cannot read pending books");
          while (cursor.next(path, mask) == SyncPending::Cursor::Step::Entry)
            queued |= path == "/read/first.epub" && (mask & SyncPending::PROGRESS_BIT);
          if (!queued) fail("Declined kept position did not queue local progress");
        }
        LOG_INF("SMOKE", "Stats upload transport smoke passed");
        std::_Exit(0);
      }

      case SmokeStep::MatchingChangeDone: {
        const bool changed = std::strcmp(std::getenv("CROSSINK_READING_TEST_MATCHING"), "cancel") != 0;
        SyncScope scope;
        const bool prepared = SyncState::findScope("/read", scope) && scope.prepared && scope.cursor == 5;
        if ((KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::BINARY) != changed)
          fail("Document Matching was not saved as chosen");
        // Changed: ids, kept positions and scope progress start over. Cancelled: untouched.
        if (SyncParked::empty() != changed || (SyncIds::generation() == 0) != changed || prepared == changed)
          fail("Sync state does not match the Document Matching choice");
        LOG_INF("SMOKE", "Stats upload transport smoke passed");
        std::_Exit(0);
      }

      case SmokeStep::StatsUploadReturn:
        KOREADER_STORE.setCredentials("", "");
        LOG_INF("SMOKE", "Sync Books entry and cancellation passed");
        activityManager.goToSettings();
        queueStep(mappedInputManager.hasHomeKey() ? "Settings landscape" : "Settings", SmokeStep::Settings);
        break;

      case SmokeStep::StatusBarEditor: {
        {
          RenderLock lock;
          captureStatusBarScreen("display-editor");
        }
        inputScript = {press(MappedInputManager::Button::Confirm), release(MappedInputManager::Button::Confirm),
                       render("Display status bar picker", 4)};
        scriptIndex = 0;
        inputCompletionStep = SmokeStep::StatusBarPicker;
        step = SmokeStep::ReaderInput;
        break;
      }
      case SmokeStep::StatusBarPicker: {
        RenderLock lock;
        captureStatusBarScreen("display-picker");
        for (const uint8_t theme : {CrossPointSettings::CLASSIC, CrossPointSettings::MINIMAL, CrossPointSettings::LYRA,
                                    CrossPointSettings::ROUNDEDRAFF, CrossPointSettings::DASHBOARD}) {
          SETTINGS.uiTheme = theme;
          UITheme::getInstance().reload();
          const auto& metrics = UITheme::getInstance().getMetrics();
          for (unsigned format = 0; format < CrossPointSettings::DATE_FORMAT_COUNT; ++format) {
            SETTINGS.dateFormat = format;
            renderer.clearScreen();
            GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                           tr(STR_SETTINGS_TITLE));
            const auto name = "header-" + std::to_string(theme) + "-date-" + std::to_string(format);
            captureStatusBarScreen(name.c_str());
          }
        }
        LOG_INF("SMOKE", "Simulator smoke test passed: status bar editor, picker and themed date headers");
        std::_Exit(0);
      }

      case SmokeStep::Settings:
        renderer.setOrientation(GfxRenderer::Orientation::Portrait);
        if (!mappedInputManager.hasTouchHardware()) {
          inputScript.clear();
          scriptIndex = 0;
          inputCompletionStep = SmokeStep::SideButtons;
          const auto down = mappedInputManager.menuButton(MappedInputManager::Button::Down);
          const auto up = mappedInputManager.menuButton(MappedInputManager::Button::Up);
          const auto left = mappedInputManager.menuButton(MappedInputManager::Button::Left);
          const auto right = mappedInputManager.menuButton(MappedInputManager::Button::Right);
          addTap(right);
          inputScript.push_back(render("Settings Right selects Reader tab", 3));
          inputScript.push_back(assertSettingsNavigation(1, 0));
          addTap(down);
          inputScript.push_back(render("Settings Down selects first row", 3));
          inputScript.push_back(assertSettingsNavigation(1, 1));
          addTap(left);
          inputScript.push_back(render("Settings Left switches tabs while a row is focused", 3));
          inputScript.push_back(assertSettingsNavigation(0, 1));
          addTap(up);
          inputScript.push_back(render("Settings Up returns to tab band", 3));
          inputScript.push_back(assertSettingsNavigation(0, 0));
          addTap(right);  // Reader tab
          addTap(right);  // Controls tab
          const int controlsCount = static_cast<int>(buildControlsSettingsParentList(getSettingsList()).size());
          for (int i = 0; i < controlsCount; ++i) addTap(down);
          inputScript.push_back(render("Menu navigation last in Controls", 3));
          inputScript.push_back({ScriptActionType::CaptureMenuNavigation, down, "menu-navigation-controls", 0, 0, 0});
          addTap(MappedInputManager::Button::Confirm);
          inputScript.push_back(render("Menu navigation picker", 3));
          inputScript.push_back({ScriptActionType::CaptureMenuNavigation, down, "menu-navigation-picker", 0, 0, 0});
          addTap(MappedInputManager::Button::Right);
          inputScript.push_back(render("Legacy navigation note", 3));
          inputScript.push_back(
              {ScriptActionType::CaptureMenuNavigation, down, "menu-navigation-picker-legacy", 0, 0, 0});
          addTap(MappedInputManager::Button::Confirm);
          inputScript.push_back(render("Classic selected without moving the row", 3));
          inputScript.push_back(
              {ScriptActionType::AssertMenuNavigation, down, nullptr, 0, CrossPointSettings::MENU_NAV_CLASSIC, 0});
          inputScript.push_back(assertSettingsNavigation(2, controlsCount));
          addTap(MappedInputManager::Button::Right);
          inputScript.push_back(assertSettingsNavigation(2, 0));
          addTap(MappedInputManager::Button::Left);
          inputScript.push_back(assertSettingsNavigation(2, controlsCount));
          addTap(MappedInputManager::Button::Down);
          inputScript.push_back(assertSettingsNavigation(2, 0));
          addTap(MappedInputManager::Button::Up);
          inputScript.push_back(assertSettingsNavigation(2, controlsCount));
          addTap(MappedInputManager::Button::Right);
          inputScript.push_back(assertSettingsNavigation(2, 0));
          addTap(MappedInputManager::Button::Right);
          inputScript.push_back(assertSettingsNavigation(2, 1));
          inputScript.push_back(press(MappedInputManager::Button::Right));
          inputScript.push_back({ScriptActionType::WaitForSettingsCategory, down, nullptr, 0, 3, 0});
          inputScript.push_back(release(MappedInputManager::Button::Right));
          inputScript.push_back(assertSettingsNavigation(3, 1));
          inputScript.push_back(press(MappedInputManager::Button::Left));
          inputScript.push_back({ScriptActionType::WaitForSettingsCategory, down, nullptr, 0, 2, 0});
          inputScript.push_back(release(MappedInputManager::Button::Left));
          inputScript.push_back(assertSettingsNavigation(2, 1));
          addTap(MappedInputManager::Button::Left);  // Tab band.
          addTap(MappedInputManager::Button::Left);  // Wrap to Menu navigation.
          addTap(MappedInputManager::Button::Confirm);
          addTap(MappedInputManager::Button::Left);
          addTap(MappedInputManager::Button::Confirm);
          inputScript.push_back(render("Directional restored without moving the row", 3));
          inputScript.push_back(
              {ScriptActionType::AssertMenuNavigation, down, nullptr, 0, CrossPointSettings::MENU_NAV_DIRECTIONAL, 0});
          inputScript.push_back(assertSettingsNavigation(2, controlsCount));
          // Return to the Side Buttons submenu from the last row.
          const auto controls = buildControlsSettingsParentList(getSettingsList());
          const auto sideButtons = std::find_if(controls.begin(), controls.end(), [](const SettingInfo& setting) {
            return setting.action == SettingAction::ControlsSideButtons;
          });
          const int sideButtonsIndex = static_cast<int>(std::distance(controls.begin(), sideButtons));
          for (int i = sideButtonsIndex + 1; i < controlsCount; ++i) addTap(up);
          addTap(MappedInputManager::Button::Confirm);
          inputScript.push_back(render("Side Button Settings", 250));
          step = SmokeStep::ReaderInput;
          break;
        }
        [[fallthrough]];
      case SmokeStep::SideButtons:
        activityManager.replaceActivity(std::make_unique<EpubReaderDrawerActivity>(
            renderer, mappedInputManager, std::shared_ptr<Epub>{}, nullptr, 0.0f, 0, 0, false, false, false, false,
            false, false, false, false, true, true, 0, 0, 5, false));
        queueStep("Reader Menu", SmokeStep::ReaderMenu);
        break;

      case SmokeStep::ReaderMenu:
        activityManager.goToSleep();
        queueStep("Sleep", SmokeStep::Sleep);
        break;

      case SmokeStep::Sleep: {
        const char* bookPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
        if (bookPath == nullptr || bookPath[0] == '\0') {
          LOG_INF("SMOKE", "Skipping Reader step; CROSSINK_SIMULATOR_SMOKE_BOOK is not set");
          step = SmokeStep::Reader;
          break;
        }
        if (!Storage.exists(bookPath)) {
          fail("Smoke test book is missing: %s", bookPath);
        }
        if (landscapeReaderRequested()) {
          SETTINGS.orientation = CrossPointSettings::LANDSCAPE_CCW;
          LOG_INF("SMOKE", "Opening smoke reader in landscape");
        }
        activityManager.goToReader(bookPath, true);
        queueStep("Reader", SmokeStep::Reader, 8);
        break;
      }

      case SmokeStep::Reader:
#if CROSSINK_APP_CAP_TOUCH
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_CLIPPING_SELECTION")) {
          if (ClippingSelectionSmokeTest::tick()) {
            LOG_INF("SMOKE", "Simulator smoke test passed: touch clipping actions");
            std::_Exit(0);
          }
          break;
        }
#endif
        if (!activityManager.isCurrentActivityNamed("EpubReader")) fail("Completion test requires EPUB reader");
#if !CROSSINK_SCALABLE_FONTS
        if (!EpubReaderCompletionSmokeTest::fontReload(
                *static_cast<EpubReaderActivity*>(activityManager.simulatorCurrentActivity())))
          fail("Bitmap font reload lost live section or reading position");
#endif
        if (!EpubReaderCompletionSmokeTest::run(
                *static_cast<EpubReaderActivity*>(activityManager.simulatorCurrentActivity()),
                captureStatusBarScreen)) {
          fail("EPUB completion shortcut regression");
        }
        queueStep("Completion shortcut returned Home", SmokeStep::CompletionReturnedHome);
        break;

      case SmokeStep::CompletionReturnedHome:
        if (!activityManager.isCurrentActivityNamed("Home")) fail("End-screen forward shortcut did not return Home");
        activityManager.goToReader(std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK"), true);
        queueStep("Reader after completion shortcuts", SmokeStep::CompletionReaderRestored, 8);
        break;

      case SmokeStep::CompletionReaderRestored:
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_STATUS_BAR_LIFECYCLE")) {
          if (!EpubReaderCompletionSmokeTest::openStatusSettings(
                  *static_cast<EpubReaderActivity*>(activityManager.simulatorCurrentActivity())))
            fail("Global status size repaginated reader or missing initial chapter");
          queueStep("Reader status settings", SmokeStep::StatusBarReaderSettings, 4);
          break;
        }
        verifyWakePowerReaderShortcut();
        buildReaderInputScript();
        step = SmokeStep::ReaderInput;
        break;

      case SmokeStep::StatusBarReaderSettings: {
        if (!activityManager.isCurrentActivityNamed("StatusBarSettings")) fail("Reader status settings did not open");
        static constexpr unsigned masks[] = {3, 1, 2, 0, 3, 0};
        {
          RenderLock lock;
          auto top = SETTINGS.topReaderStatusBar;
          auto bottom = SETTINGS.bottomReaderStatusBar;
          top.hidden = masks[statusBarReaderPass] & 1;
          bottom.hidden = masks[statusBarReaderPass] & 2;
          SETTINGS.setReaderStatusBar(ReaderStatusBarPosition::Top, top);
          SETTINGS.setReaderStatusBar(ReaderStatusBarPosition::Bottom, bottom);
          SETTINGS.saveToFile();
        }
        inputScript.clear();
        scriptIndex = 0;
        addTap(MappedInputManager::Button::Back);
        inputScript.push_back(render("Reader after status edit", 16));
        inputScript.push_back(assertActivity("EpubReader"));
        inputCompletionStep = SmokeStep::StatusBarReaderReturned;
        step = SmokeStep::ReaderInput;
        break;
      }
      case SmokeStep::StatusBarReaderReturned: {
        auto& reader = *static_cast<EpubReaderActivity*>(activityManager.simulatorCurrentActivity());
        if (!EpubReaderCompletionSmokeTest::statusSettingsReturned(reader)) fail("EPUB status reflow lost text anchor");
        {
          RenderLock lock;
          captureStatusBarScreen(("epub-status-return-" + std::to_string(statusBarReaderPass)).c_str());
        }
        if (++statusBarReaderPass < 6) {
          if (!EpubReaderCompletionSmokeTest::openStatusSettings(reader)) fail("Repeated reader status edit failed");
          queueStep("Repeated reader status settings", SmokeStep::StatusBarReaderSettings, 4);
        } else {
          LOG_INF("SMOKE", "EPUB repeated hide/show, actual Settings return and text-anchor reflow passed");
          buildReaderInputScript();
          step = SmokeStep::ReaderInput;
        }
        break;
      }

      case SmokeStep::ReaderInput:
        runReaderInputScript();
        break;

      case SmokeStep::CarouselHome:
        verifyCarouselCacheReturn();
        break;

      case SmokeStep::FrontlightLayout: {
        // Both scales, all four orientations, and all eight themes, with and
        // without the additional reader progress row.
        if (frontlightLayoutPass == 128) {
          LOG_INF("SMOKE", "Frontlight layout: 128 scale/orientation/theme/context combinations passed");
          step = SmokeStep::Done;
          break;
        }
        {
          RenderLock lock;
          SETTINGS.uiScale =
              (frontlightLayoutPass / 64) ? CrossPointSettings::UI_SCALE_LARGE : CrossPointSettings::UI_SCALE_SMALL;
          SETTINGS.uiTheme = (frontlightLayoutPass / 8) % 8;
          renderer.setOrientation(static_cast<GfxRenderer::Orientation>((frontlightLayoutPass / 2) % 4));
          UITheme::getInstance().reload();
        }
        FrontlightPanelContext context;
        context.activeReaderBook = frontlightLayoutPass % 2;
        if (context.activeReaderBook) {
          context.bookTitle = "Layout fixture";
          context.bookDetails.title = context.bookTitle;
          context.bookDetails.chapter = "Chapter Three: The Storm That Swallowed the Northern Coastline";
          context.bookDetails.chapterPage = 1;
          context.bookDetails.chapterPageCount = 10;
        }
        activityManager.pushActivity(
            std::make_unique<FrontlightPanelActivity>(renderer, mappedInputManager, std::move(context)));
        queueStep("Frontlight layout matrix", SmokeStep::FrontlightLayoutRendered, 4);
        break;
      }

      case SmokeStep::FrontlightLayoutRendered: {
#if CROSSINK_APP_CAP_TOUCH
        auto* panel = dynamic_cast<FrontlightPanelActivity*>(activityManager.simulatorCurrentActivity());
        if (!panel) fail("Layout matrix expected frontlight drawer");
        const auto handle = panel->simulatorHandleRect();
        int top, right, bottom, left;
        renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
        if (handle.y < top || handle.bottom() > renderer.getScreenHeight() - bottom || handle.x < left ||
            handle.right() > renderer.getScreenWidth() - right || handle.height <= 0 ||
            panel->simulatorContentBottom > panel->simulatorActionBarTop) {
          fail("Frontlight controls overflow in layout matrix case %u", frontlightLayoutPass);
        }
        const bool portrait = !(renderer.getOrientation() == GfxRenderer::Orientation::LandscapeClockwise ||
                                renderer.getOrientation() == GfxRenderer::Orientation::LandscapeCounterClockwise);
        if (frontlightLayoutPass % 2 && portrait && !panel->simulatorShowsChapterLine()) {
          fail("Frontlight chapter line dropped in portrait layout matrix case %u", frontlightLayoutPass);
        }
        if (const char* outputDir = std::getenv("CROSSINK_SIMULATOR_SMOKE_FRONTLIGHT_CAPTURES")) {
          const auto path = std::filesystem::path(outputDir) / (std::to_string(frontlightLayoutPass) + ".pgm");
          FILE* image = std::fopen(path.c_str(), "wb");
          if (!image) fail("Cannot create frontlight layout capture");
          RenderLock lock;
          const int width = renderer.getScreenWidth();
          const int height = renderer.getScreenHeight();
          std::fprintf(image, "P5\n%d %d\n255\n", width, height);
          for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) std::fputc(renderer.isPixelBlack(x, y) ? 0 : 255, image);
          }
          std::fclose(image);
        }
        const int x = handle.x + handle.width / 2;
        const int y = handle.y + handle.height / 2;
        inputScript = {touchDown(x, y), touchRelease(x, y), render("Frontlight closed by visible handle", 4),
                       assertActivity("Home")};
        scriptIndex = 0;
        ++frontlightLayoutPass;
        inputCompletionStep = SmokeStep::FrontlightLayout;
        step = SmokeStep::ReaderInput;
#else
        fail("Frontlight layout regression requires the X4 Pro simulator");
#endif
        break;
      }

      case SmokeStep::ThemeHome: {
#if CROSSINK_APP_CAP_TOUCH
        if (activityManager.getCurrentBookPath() != homeThemeBookPath) fail("Theme switch lost the selected book");
        const int width = renderer.getScreenWidth();
        const int height = renderer.getScreenHeight();
        inputScript = {touchDown(width / 2, 8),
                       touchMove(width / 2, height / 4),
                       touchRelease(width / 2, height / 4),
                       render("Home frontlight drawer", 4),
                       assertActivity("FrontlightPanel"),
                       {ScriptActionType::OpenFrontlightSettings, MappedInputManager::Button::Back, nullptr, 0, 0, 0},
                       render("Home drawer Settings", 4),
                       assertActivity("Settings")};
        scriptIndex = 0;
        inputCompletionStep = SmokeStep::ThemeSettings;
        step = SmokeStep::ReaderInput;
#else
        fail("Home theme regression requires the X4 Pro simulator");
#endif
        break;
      }

      case SmokeStep::ThemeSettings: {
        static constexpr uint8_t themes[] = {CrossPointSettings::LYRA_3_COVERS, CrossPointSettings::DASHBOARD,
                                             CrossPointSettings::MINIMAL,       CrossPointSettings::COVER_GRID,
                                             CrossPointSettings::LYRA,          CrossPointSettings::CLASSIC,
                                             CrossPointSettings::ROUNDEDRAFF,   CrossPointSettings::LYRA_CAROUSEL,
                                             CrossPointSettings::LYRA_CAROUSEL, CrossPointSettings::LYRA_CAROUSEL};
        // Change the global values while the real Settings child is open, then
        // return through its real drawer callback. Compare with a fresh Home.
        {
          RenderLock lock;
          SETTINGS.uiTheme = themes[homeThemePass];
          if (homeThemePass == 8) SETTINGS.uiScale = CrossPointSettings::UI_SCALE_LARGE;
          if (homeThemePass == 9) SETTINGS.displayStatusBarTextSize = 2;
          UITheme::getInstance().reload();
        }
        inputScript = {press(MappedInputManager::Button::Back), release(MappedInputManager::Button::Back),
                       render("Home after drawer Settings", 8), assertActivity("Home")};
        scriptIndex = 0;
        inputCompletionStep = SmokeStep::ThemeReturned;
        step = SmokeStep::ReaderInput;
        break;
      }

      case SmokeStep::ThemeReturned: {
        if (activityManager.getCurrentBookPath() != homeThemeBookPath) fail("Theme switch lost the selected book");
        {
          RenderLock lock;
          homeThemeScreenHash = hashBytes(renderer.getFrameBuffer(), renderer.getBufferSize());
          captureStatusBarScreen(("home-theme-size-pass-" + std::to_string(homeThemePass)).c_str());
        }
        activityManager.replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInputManager, HomeMenuItem::NONE,
                                                                       HalDisplay::FAST_REFRESH, homeThemeBookPath));
        queueStep("Fresh Home reference", SmokeStep::ThemeFresh, 8);
        break;
      }

      case SmokeStep::ThemeFresh: {
        {
          RenderLock lock;
          if (hashBytes(renderer.getFrameBuffer(), renderer.getBufferSize()) != homeThemeScreenHash)
            fail("Home after drawer theme change differs from fresh Home (pass %u)", homeThemePass);
        }
        LOG_INF("SMOKE", "Home theme/scale/status-size return matches fresh render (pass %u)", homeThemePass);
        if (++homeThemePass == 10) {
          LOG_INF("SMOKE", "Simulator smoke test passed");
          std::_Exit(0);
        }
        step = SmokeStep::ThemeHome;
        break;
      }

      case SmokeStep::Done:
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_APP_NOTES")) {
          RenderLock lock;
          captureStatusBarScreen("short-note-popup");
        }
        if (SETTINGS.uiTheme == CrossPointSettings::LYRA_CAROUSEL && carouselCachePass == 0 &&
            std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK")) {
          const RecentBook book = RECENT_BOOKS.getBooks().front();
          for (const char* path : {"/books/carousel-second.txt", "/books/carousel-third.txt"}) {
            if (!Storage.writeFile(path, "Carousel side cover fixture")) fail("Cannot create carousel fixture");
            RECENT_BOOKS.addOrUpdateBook(path, path, {}, {}, RecentBook::CoverState::Missing);
          }
          RECENT_BOOKS.addOrUpdateBook(book.path, book.title, book.author, book.coverBmpPath, book.coverState);
          SETTINGS.trackReadingStats = 1;
          activityManager.goHome();
          queueStep("Carousel return cache", SmokeStep::CarouselHome, 8);
          break;
        }
        LOG_INF("SMOKE", "Simulator smoke test passed");
        std::_Exit(0);
    }
  }

  static ScriptAction assertReaderMenu(const ReaderDrawerTab tab, const ReaderDrawerPane pane, const int selected) {
    return {ScriptActionType::AssertReaderMenu,
            MappedInputManager::Button::Back,
            nullptr,
            selected,
            static_cast<int>(tab),
            static_cast<int>(pane)};
  }

  static ScriptAction assertSettingsNavigation(const int category, const int selected) {
    return {
        ScriptActionType::AssertSettingsNavigation, MappedInputManager::Button::Back, nullptr, 0, category, selected};
  }

  static ScriptAction press(MappedInputManager::Button button) {
    return {ScriptActionType::Press, button, nullptr, 0, 0, 0};
  }

  static ScriptAction release(MappedInputManager::Button button) {
    return {ScriptActionType::Release, button, nullptr, 0, 0, 0};
  }

  static ScriptAction homeTap() {
    return {ScriptActionType::HomeTap, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction homeLongPress() {
    return {ScriptActionType::HomeLongPress, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction configureHomeButtonPowerLock() {
    return {ScriptActionType::ConfigureHomeButtonPowerLock, MappedInputManager::Button::Power, nullptr, 0, 0, 0};
  }

  static ScriptAction waitForPowerLongPress() {
    return {ScriptActionType::WaitForPowerLongPress, MappedInputManager::Button::Power, nullptr, 0, 0, 0};
  }

  static ScriptAction assertHomeButtonDisabled() {
    return {ScriptActionType::AssertHomeButtonDisabled, MappedInputManager::Button::Power, nullptr, 0, 0, 0};
  }

  static ScriptAction assertHomeButtonEnabled() {
    return {ScriptActionType::AssertHomeButtonEnabled, MappedInputManager::Button::Power, nullptr, 0, 0, 0};
  }

  static ScriptAction assertTouchscreenDisabled() {
    return {ScriptActionType::AssertTouchscreenDisabled, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction assertTouchscreenEnabled() {
    return {ScriptActionType::AssertTouchscreenEnabled, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction assertTtfProfileNative() {
    return {ScriptActionType::AssertTtfProfileNative, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction openSmokeBook() {
    return {ScriptActionType::OpenSmokeBook, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction disableReaderTouch() {
    return {ScriptActionType::DisableReaderTouch, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction enableReaderTouch() {
    return {ScriptActionType::EnableReaderTouch, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }

  static ScriptAction render(const char* label, int framesToSettle = 3) {
    return {ScriptActionType::Render, MappedInputManager::Button::Back, label, framesToSettle, 0, 0};
  }

  static ScriptAction assertActivity(const char* name) {
    return {ScriptActionType::AssertActivity, MappedInputManager::Button::Back, name, 0, 0, 0};
  }

#if CROSSINK_APP_CAP_TOUCH
  static ScriptAction touchDown(const int x, const int y) {
    return {ScriptActionType::TouchDown, MappedInputManager::Button::Back, nullptr, 0, x, y};
  }
  static ScriptAction touchDrawerHandle() {
    return {ScriptActionType::TouchDrawerHandle, MappedInputManager::Button::Back, nullptr, 0, 0, 0};
  }
  static ScriptAction touchFrontlightQuickAction(const int index) {
    return {ScriptActionType::TouchFrontlightQuickAction, MappedInputManager::Button::Back, nullptr, 0, index, 0};
  }
  static ScriptAction touchMove(const int x, const int y) {
    return {ScriptActionType::TouchMove, MappedInputManager::Button::Back, nullptr, 0, x, y};
  }
  static ScriptAction touchRelease(const int x, const int y) {
    return {ScriptActionType::TouchRelease, MappedInputManager::Button::Back, nullptr, 0, x, y};
  }
#endif

  void addTap(MappedInputManager::Button button) {
    inputScript.push_back(press(button));
    inputScript.push_back(release(button));
  }

  // File Transfer ends with Receive Nearby Book, plus Nearby Stats Sync when reading stats are tracked.
  static int nearbyFileTransferRows() { return SETTINGS.shouldTrackReadingStats() ? 2 : 1; }

  unsigned statusBarReaderPass = 0;

  void buildHomeSwipeScript() {
#if CROSSINK_APP_CAP_TOUCH
    if (!mappedInputManager.hasTouchHardware()) fail("Home swipe checks require a touch simulator");
    inputScript.clear();
    scriptIndex = 0;
    inputCompletionStep = SmokeStep::Done;
    const int width = renderer.getScreenWidth();
    const int height = renderer.getScreenHeight();
    const auto swipe = [&](int startY, int endY) {
      inputScript.push_back(touchDown(width / 2, startY));
      inputScript.push_back(touchMove(width / 2, endY));
      inputScript.push_back(touchRelease(width / 2, endY));
      inputScript.push_back(render("Home swipe result", 4));
    };
    const char* screens[] = {"Settings",     "FileBrowser",       "About",
                             "ReleaseNotes", "StatusBarSettings", "FileBrowserSettings"};
    for (int screen = 0; screen < 6; ++screen) {
      if (screen >= 4) {
        inputScript.push_back(
            {ScriptActionType::OpenHomeSwipeScreen, MappedInputManager::Button::Back, nullptr, 0, screen - 4, 0});
        inputScript.push_back(render("Nested settings parent", 4));
      }
      inputScript.push_back(
          {ScriptActionType::OpenHomeSwipeScreen, MappedInputManager::Button::Back, nullptr, 0, screen, 0});
      inputScript.push_back(render("Non-reader Home swipe screen", 4));
      inputScript.push_back(assertActivity(screens[screen]));
      swipe(height * 3 / 4, height / 3);
      inputScript.push_back(assertActivity(screens[screen]));
      swipe(height / 3, height * 3 / 4);
      inputScript.push_back(assertActivity(screens[screen]));
      swipe(height - 8, height * 2 / 3);
      // Release Notes reserves vertical swipes for paging, including its bottom actions.
      inputScript.push_back(assertActivity(screen == 3 ? "ReleaseNotes" : "Home"));
    }
    if (!Storage.writeFile("/books/home-swipe.txt", "TXT bottom-edge Home swipe regression.\n"))
      fail("Failed to write TXT Home swipe fixture");
    inputScript.push_back({ScriptActionType::OpenHomeSwipeScreen, MappedInputManager::Button::Back, nullptr, 0, 6, 0});
    inputScript.push_back(render("TXT before Home swipe regression", 12));
    inputScript.push_back(assertActivity("TxtReader"));
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("TXT without sharing popup", 4));
    inputScript.push_back(assertActivity("TxtReader"));
    swipe(height * 3 / 4, height / 3);
    inputScript.push_back(assertActivity("TxtReader"));
    swipe(height - 8, height * 2 / 3);
    inputScript.push_back(assertActivity("Home"));
    inputScript.push_back(openSmokeBook());
    inputScript.push_back(render("Reader before Home swipe regression", 12));
    inputScript.push_back(assertActivity("EpubReader"));
    swipe(height - 8, height * 2 / 3);
    inputScript.push_back(assertActivity("EpubReaderDrawer"));
#else
    fail("Home swipe checks require a touch simulator");
#endif
  }

  void buildReaderInputScript() {
    inputScript.clear();
    scriptIndex = 0;
    inputCompletionStep = SmokeStep::Done;

    int currentFontIndex = SETTINGS.fontFamily;
    std::vector<uint8_t> currentFontSizes(std::begin(BUILTIN_READER_FONT_SIZES), std::end(BUILTIN_READER_FONT_SIZES));
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      sdFontSystem.ensureRegistry();
      const auto& families = sdFontSystem.registry().getFamilies();
      for (size_t i = 0; i < families.size(); ++i) {
        if (families[i].name != SETTINGS.sdFontFamilyName) continue;
        currentFontIndex = CrossPointSettings::BUILTIN_FONT_COUNT + static_cast<int>(i);
        currentFontSizes = families[i].availableSizes();
        break;
      }
    }
    const auto sizeIt = std::find(currentFontSizes.begin(), currentFontSizes.end(), SETTINGS.readerFontPointSize);
    const int currentSizeIndex = sizeIt == currentFontSizes.end() ? 0 : std::distance(currentFontSizes.begin(), sizeIt);
    inputScript.push_back(
        {ScriptActionType::ConfigureChapterShortcuts, MappedInputManager::Button::Power, nullptr, 0, 0, 0});
    addTap(MappedInputManager::Button::Power);
    inputScript.push_back(render("Select Chapter from short Power", 4));
    inputScript.push_back(
        {ScriptActionType::AssertActivity, MappedInputManager::Button::Back, "EpubReaderChapterSelection", 0, 0, 0});
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader after cancelling chapter selection", 4));
    inputScript.push_back(press(MappedInputManager::Button::Power));
    inputScript.push_back(waitForPowerLongPress());
    inputScript.push_back(render("Select Chapter from long Power", 4));
    inputScript.push_back(release(MappedInputManager::Button::Power));
    inputScript.push_back(render("Chapter selection after Power release", 4));
    inputScript.push_back(
        {ScriptActionType::AssertActivity, MappedInputManager::Button::Back, "EpubReaderChapterSelection", 0, 0, 0});
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader after long Power chapter shortcut", 4));
    inputScript.push_back(press(MappedInputManager::Button::Confirm));
    inputScript.push_back(
        {ScriptActionType::WaitForMenuLongPress, MappedInputManager::Button::Confirm, nullptr, 0, 0, 0});
    inputScript.push_back(render("Select Chapter from long Menu", 4));
    inputScript.push_back(release(MappedInputManager::Button::Confirm));
    inputScript.push_back(render("Chapter selection after Menu release", 4));
    inputScript.push_back(
        {ScriptActionType::AssertActivity, MappedInputManager::Button::Back, "EpubReaderChapterSelection", 0, 0, 0});
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader after long Menu chapter shortcut", 4));
    if (mappedInputManager.hasHomeKey()) {
      for (const auto action : {ScriptActionType::HomeTap, ScriptActionType::HomeLongPress}) {
        inputScript.push_back({action, MappedInputManager::Button::Power, nullptr, 0, 0, 0});
        inputScript.push_back(
            {ScriptActionType::WaitForChapterSelection, MappedInputManager::Button::Power, nullptr, 0, 0, 0});
        inputScript.push_back(render("Select Chapter from Home key", 8));
        inputScript.push_back(assertActivity("EpubReaderChapterSelection"));
        addTap(MappedInputManager::Button::Back);
        inputScript.push_back(render("Reader after Home chapter shortcut", 4));
        inputScript.push_back(assertActivity("EpubReader"));
      }
      inputScript.push_back(
          {ScriptActionType::ConfigureChapterHomeDoubleTap, MappedInputManager::Button::Power, nullptr, 0, 0, 0});
      inputScript.push_back({ScriptActionType::HomeTap, MappedInputManager::Button::Power, nullptr, 0, 0, 0});
      inputScript.push_back({ScriptActionType::HomeTap, MappedInputManager::Button::Power, nullptr, 0, 0, 0});
      inputScript.push_back(
          {ScriptActionType::WaitForChapterSelection, MappedInputManager::Button::Power, nullptr, 0, 0, 0});
      inputScript.push_back(render("Select Chapter from Home double tap", 4));
      inputScript.push_back(assertActivity("EpubReaderChapterSelection"));
      addTap(MappedInputManager::Button::Back);
      inputScript.push_back(render("Reader after Home double tap chapter shortcut", 4));
      inputScript.push_back(assertActivity("EpubReader"));
    }
    inputScript.push_back(
        {ScriptActionType::RestoreChapterShortcuts, MappedInputManager::Button::Power, nullptr, 0, 0, 0});

    const int turns = pageTurnCount();
#if CROSSINK_APP_CAP_TOUCH
    if (mappedInputManager.hasTouch()) {
      const int width = renderer.getScreenWidth();
      const int height = renderer.getScreenHeight();
      if (width <= 0 || height <= 0) fail("Touch smoke test has invalid screen dimensions");
      LOG_INF("SMOKE", "Running touch reader input script with %d page turn(s)", turns);
      const int tabY = height - 28;
      for (int i = 0; i < turns; ++i) {
        inputScript.push_back(touchDown(width * 5 / 6, height / 2));
        inputScript.push_back(touchRelease(width * 5 / 6, height / 2));
        inputScript.push_back(render("Reader after touch page forward", 4));
      }

      // Exercise the TTF edit path that replaces the active scalable font IDs:
      // Auto -> Native, switch tabs, then return to the current page.
#if CROSSINK_SCALABLE_FONTS
      if (SETTINGS.sdFontFamilyName[0] != '\0' && sdFontSystem.isScalableFamily(SETTINGS.sdFontFamilyName)) {
        inputScript.push_back(touchDown(width / 2, height - 8));
        inputScript.push_back(touchMove(width / 2, height * 3 / 4));
        inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
        inputScript.push_back(render("Reader Menu opened for TTF Native transition", 4));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        inputScript.push_back(touchDown(width / (static_cast<int>(READER_DRAWER_TAB_COUNT) * 2), tabY));
        inputScript.push_back(touchRelease(width / (static_cast<int>(READER_DRAWER_TAB_COUNT) * 2), tabY));
        addTap(MappedInputManager::Button::Confirm);
        addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
        addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("TTF Rendering opened in reader drawer", 4));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("TTF Hinting choices opened in reader drawer", 3));
        const int currentHinting = TTF_RENDER_PROFILES.profileFor(SETTINGS.sdFontFamilyName).hinting;
        inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::EnumOptions, currentHinting));
        const auto hintingDirection = mappedInputManager.menuButton(
            currentHinting > 1 ? MappedInputManager::Button::Up : MappedInputManager::Button::Down);
        for (int i = 0; i < std::abs(currentHinting - 1); ++i) addTap(hintingDirection);
        addTap(MappedInputManager::Button::Confirm);
        inputScript.push_back(render("TTF Native hinting selected", 4));
        inputScript.push_back(assertTtfProfileNative());
        inputScript.push_back(touchDown(width / 2, tabY));
        inputScript.push_back(touchRelease(width / 2, tabY));
        inputScript.push_back(render("Reader Menu tab changed after TTF Native selection", 5));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        addTap(MappedInputManager::Button::Back);
        inputScript.push_back(render("Reader restored after TTF Native selection", 10));
        inputScript.push_back(assertActivity("EpubReader"));
        addTap(MappedInputManager::Button::PageForward);
        inputScript.push_back(render("Reader page turn after TTF Native selection", 5));
      }
#endif

      if (mappedInputManager.hasHomeKey()) {
        // Reader long-Power actions fire at the hold threshold. Their release
        // must not reach main.cpp's global shortcut route and run the same
        // action again. Repeat the gesture to verify the consumed release does
        // not leave the next one latched.
        inputScript.push_back(configureHomeButtonPowerLock());
        inputScript.push_back(press(MappedInputManager::Button::Power));
        inputScript.push_back(waitForPowerLongPress());
        inputScript.push_back(assertHomeButtonDisabled());
        inputScript.push_back(release(MappedInputManager::Button::Power));
        inputScript.push_back(assertHomeButtonDisabled());
        inputScript.push_back(press(MappedInputManager::Button::Power));
        inputScript.push_back(waitForPowerLongPress());
        inputScript.push_back(assertHomeButtonEnabled());
        inputScript.push_back(release(MappedInputManager::Button::Power));
        inputScript.push_back(assertHomeButtonEnabled());

        // X4 Pro reserves the top-edge swipe for its frontlight overlay and
        // moves the reader menu to the bottom edge.
        inputScript.push_back(touchDown(width / 2, 8));
        inputScript.push_back(touchMove(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Frontlight Panel opened from touch gesture", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInputManager);
        inputScript.push_back(touchDown(header.x + header.width - 32, header.y + header.height / 2));
        inputScript.push_back(touchRelease(header.x + header.width - 32, header.y + header.height / 2));
        inputScript.push_back(render("Home opened by Frontlight Panel Home button", 4));
        inputScript.push_back(assertActivity("Home"));
        inputScript.push_back(openSmokeBook());
        inputScript.push_back(render("Reader reopened after Frontlight Panel Home button", 8));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(touchDown(width / 2, 8));
        inputScript.push_back(touchMove(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Frontlight Panel reopened after Home button", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        inputScript.push_back(touchDown(20, height / 3));
        inputScript.push_back(touchMove(20, 8));
        inputScript.push_back(touchRelease(20, 8));
        inputScript.push_back(render("Frontlight Panel remains open after in-drawer swipe up", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        // Use the rendered handle: its position follows the global status text size.
        inputScript.push_back(touchDrawerHandle());
        inputScript.push_back(touchMove(width / 2, 8));
        inputScript.push_back(touchRelease(width / 2, 8));
        inputScript.push_back(render("Reader restored after Frontlight Panel handle drag up", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(touchDown(width / 2, 8));
        inputScript.push_back(touchMove(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Frontlight Panel reopened from touch gesture", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        // The fourth action-bar slot opens Global Settings through the real
        // FrontlightPanelActivity callback path.
        inputScript.push_back(touchFrontlightQuickAction(3));
        inputScript.push_back(render("Global Settings opened from Frontlight Panel", 4));
        inputScript.push_back(assertActivity("Settings"));
        inputScript.push_back(touchDown(width / 2, height * 3 / 4));
        inputScript.push_back(touchMove(width / 2, height / 2));
        inputScript.push_back(touchRelease(width / 2, height / 2));
        inputScript.push_back(render("Global Settings remains open after interior swipe up", 4));
        inputScript.push_back(assertActivity("Settings"));
        inputScript.push_back(touchDown(width / 2, height - 8));
        inputScript.push_back(touchMove(width / 2, height * 3 / 4));
        inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
        inputScript.push_back(render("Reader restored after Settings bottom-edge swipe", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(touchDown(width / 2, 8));
        inputScript.push_back(touchMove(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Frontlight Panel reopened after Global Settings", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        inputScript.push_back(touchFrontlightQuickAction(1));
        inputScript.push_back(render("Sync dialog opened from Frontlight Panel", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        inputScript.push_back(touchDown(width / 2, height - 60));
        inputScript.push_back(touchRelease(width / 2, height - 60));
        inputScript.push_back(render("Reader restored after dismissing Frontlight sync dialog", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(homeLongPress());
        inputScript.push_back(render("Reader Menu opened from simulated Home key hold", 4));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        inputScript.push_back(touchDown(width / 2, height / 2 + 24));
        inputScript.push_back(touchRelease(width / 2, height / 2 + 24));
        inputScript.push_back(render("Reader Font opened from touch reader menu", 4));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        inputScript.push_back(homeTap());
        inputScript.push_back(render("Reader Menu root restored by simulated Home key tap", 8));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        inputScript.push_back(homeTap());
        inputScript.push_back(render("Reader restored by simulated Home key tap at drawer root", 8));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(homeLongPress());
        inputScript.push_back(render("Reader Menu reopened from simulated Home key hold", 4));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        inputScript.push_back(touchDown(width / 2, height * 3 / 4));
        inputScript.push_back(touchMove(width / 2, height - 8));
        inputScript.push_back(touchRelease(width / 2, height - 8));
        inputScript.push_back(render("Reader Menu remains open after in-drawer swipe down", 4));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        inputScript.push_back(touchDrawerHandle());
        inputScript.push_back(touchMove(width / 2, height - 8));
        inputScript.push_back(touchRelease(width / 2, height - 8));
        inputScript.push_back(render("Reader restored after Reader Menu handle drag down", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(disableReaderTouch());
        inputScript.push_back(homeLongPress());
        inputScript.push_back(render("Reader Menu opened from Home key hold with touch disabled", 4));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
        inputScript.push_back(touchDown(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Reader restored after Home key menu with touch disabled", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(homeTap());
        inputScript.push_back(render("Home opened from simulated Home key tap", 8));
        inputScript.push_back(assertActivity("Home"));
        inputScript.push_back(enableReaderTouch());
        inputScript.push_back(openSmokeBook());
        inputScript.push_back(render("Reader reopened after simulated Home key tap", 8));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(touchDown(width / 2, height - 8));
        inputScript.push_back(touchMove(width / 2, height * 3 / 4));
        inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
      } else {
        // Sticky uses the same vertical gesture split as X4 Pro: swipe down
        // opens reader details/actions and swipe up opens the bottom menu.
        inputScript.push_back(touchDown(width / 2, 8));
        inputScript.push_back(touchMove(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Sticky Reader Details opened from touch gesture", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        inputScript.push_back(touchDown(20, height / 3));
        inputScript.push_back(touchMove(20, 8));
        inputScript.push_back(touchRelease(20, 8));
        inputScript.push_back(render("Sticky Reader Details remains open after in-drawer swipe up", 4));
        inputScript.push_back(assertActivity("FrontlightPanel"));
        inputScript.push_back(touchDown(width / 2, height * 3 / 4));
        inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
        inputScript.push_back(render("Reader restored after Sticky details outside tap", 4));
        inputScript.push_back(assertActivity("EpubReader"));
        inputScript.push_back(touchDown(width / 2, height - 8));
        inputScript.push_back(touchMove(width / 2, height * 3 / 4));
        inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
      }
      inputScript.push_back(render("Reader Menu opened from touch gesture", 4));
      inputScript.push_back(assertActivity("EpubReaderDrawer"));

      const int fontTabX = width / (static_cast<int>(READER_DRAWER_TAB_COUNT) * 2);
      inputScript.push_back(touchDown(fontTabX, tabY));
      inputScript.push_back(touchRelease(fontTabX, tabY));
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Touch Reader Font choices", 3));
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Touch Font Family opens on current choice", 4));
      inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::FontFamily, currentFontIndex));
      addTap(MappedInputManager::Button::Back);
      addTap(mappedInputManager.menuButton(MappedInputManager::Button::Down));
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Touch Font Size opens on current choice", 4));
      inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::EnumOptions, currentSizeIndex));
      addTap(MappedInputManager::Button::Back);
      addTap(MappedInputManager::Button::Back);

      // Touch every bottom-drawer tab slot, then dismiss from its handle.
      for (int tab = 0; tab < static_cast<int>(READER_DRAWER_TAB_COUNT); ++tab) {
        const int tabX = width * (tab * 2 + 1) / (static_cast<int>(READER_DRAWER_TAB_COUNT) * 2);
        inputScript.push_back(touchDown(tabX, tabY));
        inputScript.push_back(touchRelease(tabX, tabY));
        inputScript.push_back(render("Touch Reader Menu tab", 3));
        inputScript.push_back(assertActivity("EpubReaderDrawer"));
      }

      const int moreTabX = width / 2;
      const int drawerTop = height / 2;
      constexpr int rootRowStep = 60;
      constexpr int rootRowCenterOffset = 31;
      inputScript.push_back(touchDown(moreTabX, tabY));
      inputScript.push_back(touchRelease(moreTabX, tabY));
      inputScript.push_back(render("Touch Reader Menu More tab", 3));
      inputScript.push_back(touchDown(width / 2, drawerTop + rootRowStep + rootRowCenterOffset));
      inputScript.push_back(touchRelease(width / 2, drawerTop + rootRowStep + rootRowCenterOffset));
      inputScript.push_back(render("Touch Reader Go to Percent pane", 4));
      inputScript.push_back(assertActivity("EpubReaderDrawer"));
      inputScript.push_back(touchDown(20, drawerTop + 26));
      inputScript.push_back(touchRelease(20, drawerTop + 26));
      inputScript.push_back(render("Touch Reader More tab restored", 3));
      inputScript.push_back(touchDown(width / 2, drawerTop + rootRowStep * 2 + rootRowCenterOffset));
      inputScript.push_back(touchRelease(width / 2, drawerTop + rootRowStep * 2 + rootRowCenterOffset));
      inputScript.push_back(render("Touch Reader Auto Page Turn pane", 4));
      inputScript.push_back(assertActivity("EpubReaderDrawer"));
      inputScript.push_back(touchDown(20, drawerTop + 26));
      inputScript.push_back(touchRelease(20, drawerTop + 26));
      inputScript.push_back(render("Touch Reader More tab restored", 3));
      inputScript.push_back(touchDown(width / 2, height * 3 / 4));
      inputScript.push_back(touchMove(width / 2, height - 8));
      inputScript.push_back(touchRelease(width / 2, height - 8));
      inputScript.push_back(render("Reader Menu remains open after in-drawer swipe down", 4));
      inputScript.push_back(assertActivity("EpubReaderDrawer"));
      inputScript.push_back(touchDown(width / 2, drawerTop - 14));
      inputScript.push_back(touchRelease(width / 2, drawerTop - 14));
      inputScript.push_back(render("Reader restored after drawer handle tap", 4));
      inputScript.push_back(assertActivity("EpubReader"));

      inputScript.push_back(touchDown(width / 2, height - 8));
      inputScript.push_back(touchMove(width / 2, height * 3 / 4));
      inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
      inputScript.push_back(render("Reader Menu reopened for bottom-edge Home gesture", 4));
      inputScript.push_back(assertActivity("EpubReaderDrawer"));
      inputScript.push_back(touchDown(width / 2, height * 3 / 4));
      inputScript.push_back(touchMove(width / 2, height / 2 + 8));
      inputScript.push_back(touchRelease(width / 2, height / 2 + 8));
      inputScript.push_back(render("Reader Menu remains open after interior swipe up", 4));
      inputScript.push_back(assertActivity("EpubReaderDrawer"));
      inputScript.push_back(touchDown(width / 2, height - 8));
      inputScript.push_back(touchMove(width / 2, height * 3 / 4));
      inputScript.push_back(touchRelease(width / 2, height * 3 / 4));
      inputScript.push_back(render("Home opened from Reader Menu bottom-edge swipe", 6));
      inputScript.push_back(assertActivity("Home"));
      return;
    }
#endif
    const auto menuDown = mappedInputManager.menuButton(MappedInputManager::Button::Down);
    const auto menuLeft = mappedInputManager.menuButton(MappedInputManager::Button::Left);
    const auto menuRight = mappedInputManager.menuButton(MappedInputManager::Button::Right);
    for (int i = 0; i < turns; i++) {
      addTap(MappedInputManager::Button::PageForward);
      inputScript.push_back(render("Reader after page forward", 4));
    }

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Menu opened from EPUB", 4));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::More, ReaderDrawerPane::Root, 0));
    // Change the shared setting through the real in-reader Controls screen.
    addTap(MappedInputManager::Button::Confirm);  // Location
    addTap(MappedInputManager::Button::Confirm);  // Settings
    addTap(menuDown);                             // Status Bar
    addTap(menuDown);                             // Controls
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("In-reader Controls opened", 5));
    inputScript.push_back(assertActivity("ControlsOptions"));
    const int controlsCount = static_cast<int>(buildControlsSettingsParentList(getSettingsList()).size());
    for (int i = 1; i < controlsCount; ++i) addTap(menuDown);
    inputScript.push_back(
        {ScriptActionType::CaptureMenuNavigation, menuDown, "menu-navigation-reader-controls", 0, 0, 0});
    addTap(MappedInputManager::Button::Confirm);
    addTap(MappedInputManager::Button::Right);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(
        {ScriptActionType::AssertMenuNavigation, menuDown, nullptr, 0, CrossPointSettings::MENU_NAV_CLASSIC, 0});
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader menu after changing Controls", 5));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Settings, ReaderDrawerPane::Root, 1));
    for (int i = 0; i < 3; ++i) addTap(MappedInputManager::Button::Confirm);
    addTap(MappedInputManager::Button::Right);
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::More, ReaderDrawerPane::Root, 0));
    addTap(MappedInputManager::Button::Down);
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::More, ReaderDrawerPane::Root, 1));
    addTap(MappedInputManager::Button::Left);
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::More, ReaderDrawerPane::Root, 0));
    inputScript.push_back(render("Classic reader menu row navigation", 3));
    inputScript.push_back(
        {ScriptActionType::CaptureMenuNavigation, menuDown, "menu-navigation-reader-classic", 0, 0, 0});
    addTap(MappedInputManager::Button::Back);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Location, ReaderDrawerPane::Root, 0));
    // Restore the starting tab via Select, the classic tab-band control.
    for (int i = 0; i < 4; ++i) addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::More, ReaderDrawerPane::Root, 0));
    addTap(MappedInputManager::Button::Confirm);  // Location
    addTap(MappedInputManager::Button::Confirm);  // Settings
    addTap(MappedInputManager::Button::Down);
    addTap(MappedInputManager::Button::Down);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("In-reader Controls reopened in Classic", 5));
    inputScript.push_back(assertActivity("ControlsOptions"));
    for (int i = 1; i < controlsCount; ++i) addTap(MappedInputManager::Button::Down);
    addTap(MappedInputManager::Button::Confirm);
    addTap(MappedInputManager::Button::Left);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(
        {ScriptActionType::AssertMenuNavigation, menuDown, nullptr, 0, CrossPointSettings::MENU_NAV_DIRECTIONAL, 0});
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader menu after restoring Directional", 5));
    for (int i = 0; i < 3; ++i) addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::More, ReaderDrawerPane::Root, 0));
    // Select retains its original tab-band behavior, including wraparound.
    for (const auto tab : {ReaderDrawerTab::Location, ReaderDrawerTab::Settings, ReaderDrawerTab::Font,
                           ReaderDrawerTab::Layout, ReaderDrawerTab::More}) {
      addTap(MappedInputManager::Button::Confirm);
      inputScript.push_back(render("Reader Menu Select advances tab", 3));
      inputScript.push_back(assertReaderMenu(tab, ReaderDrawerPane::Root, 0));
    }
    addTap(menuLeft);
    addTap(menuLeft);
    inputScript.push_back(render("Reader Menu Font tab reached with Left", 3));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::Root, 0));
    addTap(menuLeft);
    inputScript.push_back(render("Reader Menu wraps left to Settings", 3));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Settings, ReaderDrawerPane::Root, 0));
    addTap(menuRight);
    inputScript.push_back(render("Reader Menu wraps right to Font", 3));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::Root, 0));

    addTap(menuDown);
    inputScript.push_back(render("Reader Menu first row focused", 3));
    addTap(menuDown);
    inputScript.push_back(render("Reader Menu Down moves row focus", 3));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::Root, 1));
    addTap(menuRight);
    inputScript.push_back(render("Reader Menu Right switches tab while a row is focused", 3));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Layout, ReaderDrawerPane::Root, 0));
    addTap(menuLeft);
    addTap(menuDown);
    inputScript.push_back(render("Reader Menu first Font row focused again", 3));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::Root, 0));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Font opened from Reader Menu", 4));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::ReaderFont, 0));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Font Family opens on current choice", 4));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::FontFamily, currentFontIndex));
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader Font after closing Font Family", 3));

    addTap(menuDown);
    inputScript.push_back(render("Font Size selected", 3));

    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Font Size choices opened", 3));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Font, ReaderDrawerPane::EnumOptions, currentSizeIndex));

#if CROSSINK_APP_READER_SAMPLE_PREVIEW
    addTap(menuDown);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("C3 font size paragraph preview", 4));
#endif

    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader Font after closing Font Size", 4));

#if CROSSINK_APP_READER_SAMPLE_PREVIEW
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("C3 font family picker", 4));
    addTap(menuDown);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("C3 font family paragraph preview", 4));
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("C3 reader font restored", 3));
#endif

    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader Menu tab focus restored", 4));

#if CROSSINK_APP_READER_SAMPLE_PREVIEW
    addTap(menuDown);
    addTap(menuDown);
    addTap(menuDown);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("C3 spacing paragraph preview", 4));
    addTap(MappedInputManager::Button::Confirm);
    addTap(MappedInputManager::Button::Down);
    inputScript.push_back(render("C3 line spacing adjusted", 4));
    addTap(MappedInputManager::Button::Back);
    addTap(menuDown);
    addTap(MappedInputManager::Button::Confirm);
    addTap(MappedInputManager::Button::Down);
    inputScript.push_back(render("C3 word spacing adjusted", 4));
    addTap(MappedInputManager::Button::Back);
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("C3 font tab restored", 3));
#endif

    addTap(menuRight);
    inputScript.push_back(render("Reader Menu advanced to next tab", 4));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::Layout, ReaderDrawerPane::Root, 0));

#if CROSSINK_APP_READER_SAMPLE_PREVIEW
    addTap(menuDown);
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("C3 margin paragraph preview", 4));
    addTap(MappedInputManager::Button::Confirm);
    addTap(MappedInputManager::Button::Down);
    inputScript.push_back(render("C3 vertical margin adjusted", 4));
    addTap(MappedInputManager::Button::Back);
    addTap(menuDown);
    addTap(MappedInputManager::Button::Confirm);
    addTap(MappedInputManager::Button::Down);
    inputScript.push_back(render("C3 horizontal margin adjusted", 4));
    addTap(MappedInputManager::Button::Back);
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("C3 layout tab restored", 3));
#endif

    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader after closing Reader Menu", 4));
    inputScript.push_back(assertActivity("EpubReader"));
    addTap(MappedInputManager::Button::Confirm);
    inputScript.push_back(render("Reader Menu reopened on More", 4));
    inputScript.push_back(assertReaderMenu(ReaderDrawerTab::More, ReaderDrawerPane::Root, 0));
    addTap(MappedInputManager::Button::Back);
    inputScript.push_back(render("Reader restored after checking default tab", 4));

    LOG_INF("SMOKE", "Running reader input script with %d page turn(s)", turns);
  }

#if CROSSINK_APP_CAP_TOUCH
  void buildFileBrowserInputScript() {
    inputScript.clear();
    scriptIndex = 0;
    inputCompletionStep = SmokeStep::FileBrowserSettings;

    if (mappedInputManager.hasHomeKey()) {
      const int width = renderer.getScreenWidth();
      const int height = renderer.getScreenHeight();
      inputScript.push_back(touchDown(width / 2, 8));
      inputScript.push_back(touchMove(width / 2, height / 4));
      inputScript.push_back(touchRelease(width / 2, height / 4));
      inputScript.push_back(render("Frontlight Panel opened outside Reader", 4));
      inputScript.push_back(assertActivity("FrontlightPanel"));
      inputScript.push_back(touchDown(width * 9 / 10, height / 2));
      inputScript.push_back(touchRelease(width * 9 / 10, height / 2));
      inputScript.push_back(render("Reader touchscreen disabled from Frontlight Panel outside Reader", 4));
      inputScript.push_back(assertActivity("FrontlightPanel"));
      inputScript.push_back(touchDown(width / 2, height - 60));
      inputScript.push_back(touchRelease(width / 2, height - 60));
      inputScript.push_back(render("File Browser restored after disabling reader touchscreen", 4));
      inputScript.push_back(assertActivity("FileBrowser"));
      inputScript.push_back(assertTouchscreenDisabled());
      inputScript.push_back(touchDown(width / 2, 8));
      inputScript.push_back(touchMove(width / 2, height / 4));
      inputScript.push_back(touchRelease(width / 2, height / 4));
      inputScript.push_back(render("Frontlight Panel reopened outside Reader", 4));
      inputScript.push_back(assertActivity("FrontlightPanel"));
      inputScript.push_back(touchDown(width * 9 / 10, height / 2));
      inputScript.push_back(touchRelease(width * 9 / 10, height / 2));
      inputScript.push_back(render("Reader touchscreen enabled from Frontlight Panel outside Reader", 4));
      inputScript.push_back(assertActivity("FrontlightPanel"));
      inputScript.push_back(touchDown(width / 2, height - 60));
      inputScript.push_back(touchRelease(width / 2, height - 60));
      inputScript.push_back(render("File Browser restored after enabling reader touchscreen", 4));
      inputScript.push_back(assertActivity("FileBrowser"));
      inputScript.push_back(assertTouchscreenEnabled());
      inputScript.push_back(
          {ScriptActionType::ClearFrontlightSyncBook, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back(touchDown(width / 2, 8));
      inputScript.push_back(touchMove(width / 2, height / 4));
      inputScript.push_back(touchRelease(width / 2, height / 4));
      inputScript.push_back(render("Frontlight without last-read book", 4));
      inputScript.push_back({ScriptActionType::OpenFrontlightSync, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      inputScript.push_back(render("Unavailable transfer actions rendered", 4));
      inputScript.push_back(
          {ScriptActionType::CheckFrontlightSync, MappedInputManager::Button::Back, nullptr, 0, 1, 0});
      inputScript.push_back(press(MappedInputManager::Button::Confirm));
      inputScript.push_back(release(MappedInputManager::Button::Confirm));
      inputScript.push_back(render("Unavailable transfer action cannot activate", 4));
      inputScript.push_back(assertActivity("FrontlightPanel"));
      inputScript.push_back(press(MappedInputManager::Button::Back));
      inputScript.push_back(release(MappedInputManager::Button::Back));
      inputScript.push_back(render("File Browser restored after unavailable transfer", 4));
      inputScript.push_back(assertActivity("FileBrowser"));
      inputScript.push_back(
          {ScriptActionType::PrepareFrontlightSync, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
      for (const bool statsEnabled : {true, false}) {
        inputScript.push_back(
            {statsEnabled ? ScriptActionType::EnableReadingStats : ScriptActionType::DisableReadingStats,
             MappedInputManager::Button::Back, nullptr, 0, 0, 0});
        inputScript.push_back(touchDown(width / 2, 8));
        inputScript.push_back(touchMove(width / 2, height / 4));
        inputScript.push_back(touchRelease(width / 2, height / 4));
        inputScript.push_back(render("Frontlight sync from File Browser", 4));
        inputScript.push_back(
            {ScriptActionType::OpenFrontlightSync, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
        inputScript.push_back(render("Frontlight sync menu rendered", 4));
        inputScript.push_back(
            {ScriptActionType::CheckFrontlightSync, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
        inputScript.push_back(press(MappedInputManager::Button::Confirm));
        inputScript.push_back(release(MappedInputManager::Button::Confirm));
        inputScript.push_back(render("Frontlight sync opens account settings", 4));
        inputScript.push_back(assertActivity("KOReaderSettings"));
        inputScript.push_back(press(MappedInputManager::Button::Back));
        inputScript.push_back(release(MappedInputManager::Button::Back));
        inputScript.push_back(render("File Browser restored after sync settings", 4));
        inputScript.push_back(assertActivity("FileBrowser"));
      }
      inputScript.push_back({ScriptActionType::EnableReadingStats, MappedInputManager::Button::Back, nullptr, 0, 0, 0});
    }

    const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInputManager);
    const auto backLayout = TouchHeaderBackButton::layout(header);
    const int x = header.x + header.width - backLayout.iconRect.width / 2;
    const int y = backLayout.iconRect.y + backLayout.iconRect.height / 2;
    inputScript.push_back(touchDown(x, y));
    inputScript.push_back(touchRelease(x, y));
    inputScript.push_back(render("File Browser Settings opened from header shortcut", 4));
    inputScript.push_back(assertActivity("FileBrowserSettings"));
    const int rowY = header.y + header.height + 32;
    inputScript.push_back(touchDown(renderer.getScreenWidth() / 2, rowY));
    inputScript.push_back(touchRelease(renderer.getScreenWidth() / 2, rowY));
    inputScript.push_back(render("File Browser Settings toggle without row highlight", 4));
    inputScript.push_back(assertActivity("FileBrowserSettings"));
  }
#endif

  void runReaderInputScript() {
    if (scriptIndex >= inputScript.size()) {
      step = inputCompletionStep;
      return;
    }

    const auto& action = inputScript[scriptIndex++];
    switch (action.type) {
      case ScriptActionType::ClearFrontlightSyncBook:
        APP_STATE.openEpubPath.clear();
        break;
      case ScriptActionType::PrepareFrontlightSync: {
        const char* bookPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
        if (!bookPath || !Storage.exists(bookPath)) fail("Frontlight sync fixture is missing");
        APP_STATE.openEpubPath = bookPath;
        break;
      }
      case ScriptActionType::DisableReadingStats:
        SETTINGS.trackReadingStats = 0;
        break;
      case ScriptActionType::EnableReadingStats:
        SETTINGS.trackReadingStats = 1;
        break;
      case ScriptActionType::OpenFrontlightSync: {
        auto* panel = dynamic_cast<FrontlightPanelActivity*>(activityManager.simulatorCurrentActivity());
        if (!panel) fail("Expected frontlight drawer before opening sync menu");
        panel->simulatorActivateQuickAction(1);
        break;
      }
      case ScriptActionType::CheckFrontlightSync: {
        auto* panel = dynamic_cast<FrontlightPanelActivity*>(activityManager.simulatorCurrentActivity());
        if (!panel) fail("Expected frontlight sync menu");
        const bool unavailable = action.x != 0;
        for (int index = 0; index < 3; ++index) {
          if (panel->simulatorSyncOptionDisabled(index) != unavailable)
            fail("Unexpected transfer availability outside reader");
          const auto row = panel->simulatorSyncOptionRect(index);
          // Sample inside the row background, away from its centered label and rounded corners.
          if (unavailable || index > 0) {
            for (int offset = 0; offset < 8; ++offset)
              if (renderer.isPixelBlack(row.x + row.width / 2 + offset, row.y + 4))
                fail("Unselected sync option is highlighted");
          }
        }
        if (const char* outputDir = std::getenv("CROSSINK_SIMULATOR_SMOKE_FRONTLIGHT_CAPTURES")) {
          const auto path = std::filesystem::path(outputDir) / (unavailable                  ? "sync-no-book.pgm"
                                                                : SETTINGS.trackReadingStats ? "sync-stats-on.pgm"
                                                                                             : "sync-stats-off.pgm");
          FILE* image = std::fopen(path.c_str(), "wb");
          if (!image) fail("Cannot create sync menu capture");
          RenderLock lock;
          const int width = renderer.getScreenWidth();
          const int height = renderer.getScreenHeight();
          std::fprintf(image, "P5\n%d %d\n255\n", width, height);
          for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) std::fputc(renderer.isPixelBlack(x, y) ? 0 : 255, image);
          std::fclose(image);
        }
        break;
      }
      case ScriptActionType::OpenFrontlightSettings: {
        auto* panel = dynamic_cast<FrontlightPanelActivity*>(activityManager.simulatorCurrentActivity());
        if (!panel) fail("Expected frontlight drawer before opening Settings");
        panel->simulatorActivateQuickAction(3);
        break;
      }
      case ScriptActionType::Press:
        mappedInputManager.simulatorInjectPress(action.button);
        break;
      case ScriptActionType::Release:
        mappedInputManager.simulatorInjectRelease(action.button);
        break;
      case ScriptActionType::HomeTap:
        lastInjectedHomeAt = millis();
        simulatorHomeKeyInput.injectTap();
        break;
      case ScriptActionType::HomeLongPress:
        lastInjectedHomeAt = millis();
        simulatorHomeKeyInput.injectLongPress();
        break;
      case ScriptActionType::WaitForChapterSelection:
        if (!activityManager.isCurrentActivityNamed("EpubReaderChapterSelection")) {
          if (millis() - lastInjectedHomeAt > 1000) fail("Home shortcut did not open chapter selection");
          --scriptIndex;
        }
        break;
      case ScriptActionType::ConfigureChapterShortcuts:
        savedChapterShortcuts[0] = SETTINGS.shortPwrBtn;
        savedChapterShortcuts[1] = SETTINGS.longPwrBtn;
        savedChapterShortcuts[2] = SETTINGS.longPressMenuAction;
        savedChapterShortcuts[3] = SETTINGS.longPressBackAction;
        savedChapterShortcuts[4] = SETTINGS.homeButtonTapAction;
        savedChapterShortcuts[5] = SETTINGS.homeButtonLongPressAction;
        savedChapterShortcuts[6] = SETTINGS.homeButtonDoubleTapAction;
        SETTINGS.shortPwrBtn = CrossPointSettings::SELECT_CHAPTER;
        SETTINGS.longPwrBtn = CrossPointSettings::SELECT_CHAPTER;
        SETTINGS.longPressMenuAction = CrossPointSettings::LONG_MENU_SELECT_CHAPTER;
        SETTINGS.longPressBackAction = CrossPointSettings::LONG_MENU_OFF;
        SETTINGS.homeButtonTapAction = CrossPointSettings::SELECT_CHAPTER;
        SETTINGS.homeButtonLongPressAction = CrossPointSettings::SELECT_CHAPTER;
        SETTINGS.homeButtonDoubleTapAction = CrossPointSettings::IGNORE;
        break;
      case ScriptActionType::ConfigureChapterHomeDoubleTap:
        SETTINGS.homeButtonTapAction = CrossPointSettings::IGNORE;
        SETTINGS.homeButtonDoubleTapAction = CrossPointSettings::SELECT_CHAPTER;
        break;
      case ScriptActionType::RestoreChapterShortcuts:
        SETTINGS.shortPwrBtn = savedChapterShortcuts[0];
        SETTINGS.longPwrBtn = savedChapterShortcuts[1];
        SETTINGS.longPressMenuAction = savedChapterShortcuts[2];
        SETTINGS.longPressBackAction = savedChapterShortcuts[3];
        SETTINGS.homeButtonTapAction = savedChapterShortcuts[4];
        SETTINGS.homeButtonLongPressAction = savedChapterShortcuts[5];
        SETTINGS.homeButtonDoubleTapAction = savedChapterShortcuts[6];
        break;
      case ScriptActionType::WaitForMenuLongPress:
        if (mappedInputManager.getHeldTime() < 650) --scriptIndex;
        break;
      case ScriptActionType::ConfigureHomeButtonPowerLock:
        SETTINGS.homeButtonInReaderEnabled = 1;
        SETTINGS.shortPwrBtn = CrossPointSettings::SHORT_PWRBTN::TOGGLE_HOME_BUTTON_IN_READER;
        SETTINGS.longPwrBtn = CrossPointSettings::SHORT_PWRBTN::TOGGLE_HOME_BUTTON_IN_READER;
        break;
      case ScriptActionType::WaitForPowerLongPress:
        if (mappedInputManager.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()) {
          --scriptIndex;
        }
        break;
      case ScriptActionType::AssertHomeButtonDisabled:
        if (SETTINGS.homeButtonInReaderEnabled) fail("Long Power did not disable the Home button");
        break;
      case ScriptActionType::AssertHomeButtonEnabled:
        if (!SETTINGS.homeButtonInReaderEnabled) fail("Long Power did not enable the Home button");
        break;
      case ScriptActionType::AssertTouchscreenDisabled:
        if (!SETTINGS.disableReaderTouchscreen) fail("Expected reader touchscreen to be disabled");
        break;
      case ScriptActionType::AssertTouchscreenEnabled:
        if (SETTINGS.disableReaderTouchscreen) fail("Expected reader touchscreen to be enabled");
        break;
      case ScriptActionType::AssertTtfProfileNative:
#if CROSSINK_SCALABLE_FONTS
        if (TTF_RENDER_PROFILES.profileFor(SETTINGS.sdFontFamilyName).hinting != 1) {
          fail("Expected active TTF profile to use native hinting");
        }
#endif
        break;
      case ScriptActionType::OpenHomeSwipeScreen:
        switch (action.x) {
          case 0:
            activityManager.replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInputManager));
            break;
          case 1:
            activityManager.replaceActivity(
                std::make_unique<FileBrowserActivity>(renderer, mappedInputManager, "/books"));
            break;
          case 2:
            activityManager.replaceActivity(std::make_unique<AboutActivity>(renderer, mappedInputManager));
            break;
          case 3:
            activityManager.replaceActivity(std::make_unique<ReleaseNotesActivity>(
                renderer, mappedInputManager, ReleaseNotesActivity::Mode::FromSettings));
            break;
          case 4:
            activityManager.pushActivity(std::make_unique<StatusBarSettingsActivity>(renderer, mappedInputManager));
            break;
          case 5:
            activityManager.pushActivity(std::make_unique<SettingsActivity>(renderer, mappedInputManager, false, true,
                                                                            SettingsActivity::View::FileBrowser));
            break;
          case 6:
            activityManager.goToReader("/books/home-swipe.txt", true);
            break;
        }
        break;
      case ScriptActionType::OpenSmokeBook: {
        const char* bookPath = std::getenv("CROSSINK_SIMULATOR_SMOKE_BOOK");
        if (bookPath == nullptr || bookPath[0] == '\0') fail("Smoke test book path is missing");
        activityManager.goToReader(bookPath, true);
        break;
      }
      case ScriptActionType::DisableReaderTouch:
        SETTINGS.disableReaderTouchscreen = true;
        break;
      case ScriptActionType::EnableReaderTouch:
        SETTINGS.disableReaderTouchscreen = false;
        break;
      case ScriptActionType::TouchDown:
#if CROSSINK_APP_CAP_TOUCH
        mappedInputManager.simulatorInjectTouchDown(action.x, action.y);
#endif
        break;
      case ScriptActionType::TouchDrawerHandle:
#if CROSSINK_APP_CAP_TOUCH
        if (auto* panel = dynamic_cast<FrontlightPanelActivity*>(activityManager.simulatorCurrentActivity())) {
          const auto handle = panel->simulatorHandleRect();
          mappedInputManager.simulatorInjectTouchDown(handle.x + handle.width / 2, handle.y + handle.height / 2);
        } else if (auto* drawer = dynamic_cast<EpubReaderDrawerActivity*>(activityManager.simulatorCurrentActivity())) {
          const auto handle = drawer->simulatorHandleRect();
          mappedInputManager.simulatorInjectTouchDown(handle.x + handle.width / 2, handle.y + handle.height / 2);
        } else {
          fail("No visible drawer handle for touch gesture");
        }
#endif
        break;
      case ScriptActionType::TouchFrontlightQuickAction:
#if CROSSINK_APP_CAP_TOUCH
        if (auto* panel = dynamic_cast<FrontlightPanelActivity*>(activityManager.simulatorCurrentActivity())) {
          const auto hit = panel->simulatorQuickActionRect(action.x);
          if (hit.width <= 0 || hit.height <= 0) fail("Frontlight quick action has no published target");
          mappedInputManager.simulatorInjectTouchDown(hit.x + hit.width / 2, hit.y + hit.height / 2);
          // Queue the matching release before the next existing render step.
          inputScript.insert(inputScript.begin() + scriptIndex,
                             touchRelease(hit.x + hit.width / 2, hit.y + hit.height / 2));
        } else {
          fail("No Frontlight Panel for quick action");
        }
#endif
        break;
      case ScriptActionType::TouchMove:
#if CROSSINK_APP_CAP_TOUCH
        mappedInputManager.simulatorInjectTouchMove(action.x, action.y);
#endif
        break;
      case ScriptActionType::TouchRelease:
#if CROSSINK_APP_CAP_TOUCH
        mappedInputManager.simulatorInjectTouchRelease(action.x, action.y);
#endif
        break;
      case ScriptActionType::AssertReaderMenu: {
        if (!activityManager.isCurrentActivityNamed("EpubReaderDrawer"))
          fail("Expected reader menu for navigation assertion");
        const auto* drawer = static_cast<EpubReaderDrawerActivity*>(activityManager.simulatorCurrentActivity());
        const auto& state = drawer->simulatorState();
        if ((state.pane == ReaderDrawerPane::FontFamily || state.pane == ReaderDrawerPane::EnumOptions) &&
            !drawer->simulatorFocusedRowVisible())
          fail("Reader picker current choice is not highlighted and visible");
        if (static_cast<int>(state.tab) != action.x || static_cast<int>(state.pane) != action.y ||
            state.selectedIndex != action.settleFrames)
          fail("Reader menu navigation mismatch: tab=%d pane=%d row=%d, expected %d/%d/%d", static_cast<int>(state.tab),
               static_cast<int>(state.pane), state.selectedIndex, action.x, action.y, action.settleFrames);
        break;
      }
      case ScriptActionType::AssertSettingsNavigation: {
        if (!activityManager.isCurrentActivityNamed("Settings")) fail("Expected settings for navigation assertion");
        const auto* settings = static_cast<SettingsActivity*>(activityManager.simulatorCurrentActivity());
        if (settings->simulatorCategoryIndex() != action.x || settings->simulatorSelectedIndex() != action.y)
          fail("Settings navigation mismatch: category=%d row=%d, expected %d/%d", settings->simulatorCategoryIndex(),
               settings->simulatorSelectedIndex(), action.x, action.y);
        break;
      }
      case ScriptActionType::AssertMenuNavigation: {
        RenderLock lock;
        if (SETTINGS.menuNavigation != action.x) fail("Menu navigation selection failed");
        SETTINGS.menuNavigation = 255;
        if (!SETTINGS.loadFromFile() || SETTINGS.menuNavigation != action.x)
          fail("Menu navigation did not survive reloading saved settings");
        break;
      }
      case ScriptActionType::WaitForSettingsCategory: {
        const auto* settings = dynamic_cast<SettingsActivity*>(activityManager.simulatorCurrentActivity());
        if (!settings) fail("Expected Settings during classic hold");
        if (settings->simulatorCategoryIndex() != action.x) --scriptIndex;
        break;
      }
      case ScriptActionType::CaptureMenuNavigation: {
        RenderLock lock;
        captureStatusBarScreen(action.label);
        break;
      }
      case ScriptActionType::WaitForNavigationHold:
        if (mappedInputManager.getHeldTime() < 650) --scriptIndex;
        break;
      case ScriptActionType::AssertAboutTopIndex: {
        const auto* about = dynamic_cast<AboutActivity*>(activityManager.simulatorCurrentActivity());
        if (!about || about->simulatorTopIndex() != action.x)
          fail("About button moved to row %d, expected %d", about ? about->simulatorTopIndex() : -1, action.x);
        break;
      }
      case ScriptActionType::AssertActivity:
        if (!activityManager.isCurrentActivityNamed(action.label)) fail("Expected current activity: %s", action.label);
        if (std::getenv("CROSSINK_SIMULATOR_SMOKE_HOME_SWIPE")) {
          RenderLock lock;
          captureStatusBarScreen(action.label);
        }
        break;
      case ScriptActionType::Render:
        queueStep(action.label, SmokeStep::ReaderInput, action.settleFrames);
        break;
    }
  }
};

SimulatorSmokeTest smokeTest;

}  // namespace

void runSimulatorSmokeTestTick() { smokeTest.tick(); }

bool simulatorInterceptNetworkRestart(const uint32_t target, const uint32_t payload) {
  if (!capturedNetworkRestart.armed) return false;
  ++capturedNetworkRestart.count;
  capturedNetworkRestart.target = target;
  capturedNetworkRestart.payload = payload;
  LOG_INF("SMOKE", "Captured network restart target=%u payload=%u", target, payload);
  return true;
}

// Every scenario except the release notes one starts with What's New already dismissed so it cannot cover Home.
void prepareSimulatorSmokeState() {
  // Smoke tests and scripted-input harnesses (scripts/test_*.py) expect to drive Home directly.
  const bool harness = std::getenv("CROSSINK_SIMULATOR_SMOKE_TEST") || std::getenv("CROSSPOINT_SIM_INPUT_SCRIPT");
  if (harness && !std::getenv("CROSSINK_SIMULATOR_SMOKE_RELEASE_NOTES"))
    APP_STATE.releaseNotesSeen = RELEASE_NOTES_REVISION;
}

#endif
