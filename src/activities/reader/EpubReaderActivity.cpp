#include "EpubReaderActivity.h"

#include <Arduino.h>
#include <Epub/Page.h>
#include <Epub/PageCountEstimator.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <Utf8.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>

#include "../settings/DictionarySelectActivity.h"
#include "../settings/KOReaderSettingsActivity.h"
#include "BookStatsActivity.h"
#include "ClipSelectionActivity.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubReaderBookmarkListActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderClippingListActivity.h"
#include "EpubReaderFootnotesActivity.h"
#if CROSSINK_APP_CAP_TOUCH
#include "EpubReaderTouchMenuActivity.h"
#endif
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "GlobalActions.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "LookedUpWordsActivity.h"
#include "MappedInputManager.h"
#include "NearbyBookPositionSyncActivity.h"
#include "PageWordGeometry.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "QuickActions.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "WordRef.h"
#include "activities/home/RecentBookProgress.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "clippings/ClippingHighlightGeometry.h"
#include "clippings/ClippingMatchTracker.h"
#include "clippings/ClippingTextMatcher.h"
#include "clippings/ClippingsManager.h"
#include "components/UITheme.h"
#if CROSSINK_APP_CAP_TOUCH
#include "components/TouchHeaderBackButton.h"
#endif
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/BookMoveUtils.h"
#include "util/Dictionary.h"
#include "util/ScreenshotUtil.h"

namespace {
constexpr unsigned long TOUCH_DICTIONARY_LOOKUP_HOLD_MS = 1000;
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long longPressMenuMs = 600;
constexpr uint16_t DEFAULT_AUTO_PAGE_TURN_INTERVAL_S = 30;
constexpr uint16_t MIN_AUTO_PAGE_TURN_INTERVAL_S = 5;
constexpr uint16_t MAX_AUTO_PAGE_TURN_INTERVAL_S = 120;
constexpr int MAX_PAGE_LOAD_RETRIES = 3;
constexpr uint8_t LEGACY_READER_SETTINGS_FILE_VERSION = 1;
constexpr uint8_t PRE_WORD_SPACING_READER_SETTINGS_FILE_VERSION = 2;
constexpr uint8_t PRE_INDEXING_METHOD_READER_SETTINGS_FILE_VERSION = 3;
constexpr uint8_t PRE_DICTIONARY_FONT_READER_SETTINGS_FILE_VERSION = 4;
constexpr uint8_t PRE_POINT_SIZE_READER_SETTINGS_FILE_VERSION = 5;
constexpr uint8_t PRE_DICTIONARY_FONT_SIZE_READER_SETTINGS_FILE_VERSION = 6;
constexpr uint8_t PRE_SPLIT_SCREEN_MARGIN_READER_SETTINGS_FILE_VERSION = 7;
constexpr uint8_t PRE_GLOBAL_DARK_MODE_READER_SETTINGS_FILE_VERSION = 8;
constexpr uint8_t READER_SETTINGS_FILE_VERSION = 9;
constexpr uint8_t READER_SETTINGS_FLAG_CUSTOM = 1 << 0;
constexpr uint8_t READER_SETTINGS_FLAG_AUTO_PAGE_TURN = 1 << 1;
constexpr uint8_t READER_SETTINGS_FLAG_RENDER_MODE = 1 << 2;
constexpr uint8_t READER_SETTINGS_FLAG_DICTIONARY_FONT = 1 << 3;
constexpr char READER_SETTINGS_FILE_NAME[] = "/reader_settings.bin";
constexpr char BALANCED_SECTION_CACHE_SUFFIX[] = "_balanced";
constexpr char LIGHT_SECTION_CACHE_SUFFIX[] = "_light";
constexpr unsigned long RENDER_MODE_TOAST_MS = 1500UL;
// Shared dwell time for the transient bookmark/completed/tilt confirmations.
constexpr unsigned long TRANSIENT_FEEDBACK_MS = 1000UL;
constexpr unsigned long IDLE_SD_FONT_PREWARM_DELAY_MS = 400UL;
constexpr uint32_t IDLE_SD_FONT_PREWARM_MIN_FREE = 64U * 1024U;
constexpr uint32_t IDLE_SD_FONT_PREWARM_MIN_MAX_ALLOC = 40U * 1024U;
constexpr unsigned long MIN_READING_STATS_PAGE_MS = 2000UL;
constexpr uint32_t MIN_READING_PACE_SAMPLE_SECONDS = 2;
constexpr uint16_t MIN_STORED_TIME_LEFT_PACE_SAMPLE_COUNT = 3;
constexpr uint16_t MIN_SESSION_TIME_LEFT_PACE_SAMPLE_COUNT = 10;
constexpr uint16_t MIN_STORED_PACE_SLOWER_RECOVERY_SESSION_SAMPLES = 10;
constexpr uint8_t STORED_PACE_SLOWER_RECOVERY_PERCENT = 110;
constexpr uint16_t MIN_STORED_PACE_FASTER_RECOVERY_SESSION_SAMPLES = 15;
constexpr uint8_t STORED_PACE_FASTER_RECOVERY_PERCENT = 90;
constexpr uint8_t BOOK_PROGRESS_ESTIMATE_FLOOR_PERCENT = 90;
constexpr uint16_t FOOTNOTE_PREVIEW_MAX_PAGES = 3;
#if CROSSINK_APP_CAP_TOUCH
constexpr int TOUCH_FOOTNOTE_TARGET_SIZE = 48;
#endif
constexpr uint8_t PUBLISHER_PAGE_NUMBER_LEFT_MARGIN_MIN = 15;
constexpr int PUBLISHER_PAGE_NUMBER_X = 5;
constexpr uint16_t CLIP_ADVANCE_CODEPOINT_CAPACITY = 256;

int publisherPageNumberX(const GfxRenderer& renderer) {
#if (defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO) || \
    (defined(FREEINK_DEVICE_X4CLASSIC) && FREEINK_DEVICE_X4CLASSIC)
  if (renderer.getOrientation() == GfxRenderer::Orientation::LandscapeCounterClockwise) {
    return PUBLISHER_PAGE_NUMBER_X + 5;
  }
#endif

  return PUBLISHER_PAGE_NUMBER_X;
}

struct ClipAdvanceCollector {
  static constexpr uint8_t STYLE_COUNT = 4;
  uint32_t codepoints[STYLE_COUNT][CLIP_ADVANCE_CODEPOINT_CAPACITY]{};
  uint16_t counts[STYLE_COUNT]{};
  uint8_t truncatedStyles = 0;
  std::string rtlTokenScratch;
  std::string rtlVisualScratch;

  void reset() {
    std::fill(std::begin(counts), std::end(counts), 0);
    truncatedStyles = 0;
    rtlTokenScratch.clear();
    rtlVisualScratch.clear();
  }

  bool addCodepoint(const uint8_t style, const uint32_t codepoint) {
    auto& count = counts[style];
    auto* bucket = codepoints[style];
    if (std::find(bucket, bucket + count, codepoint) != bucket + count) return false;
    if (count >= CLIP_ADVANCE_CODEPOINT_CAPACITY) {
      truncatedStyles |= static_cast<uint8_t>(1U << style);
      return true;
    }
    bucket[count++] = codepoint;
    return false;
  }

  bool addLogicalText(const uint8_t style, const char* text) {
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    uint32_t codepoint = 0;
    while ((codepoint = utf8NextCodepoint(&p))) {
      if (addCodepoint(style, codepoint)) return true;
    }
    return false;
  }
};

uint32_t pagesCentipages(const float pages) {
  if (pages <= 0.0f) {
    return 0;
  }
  if (pages >= static_cast<float>(UINT32_MAX) / 100.0f) {
    return UINT32_MAX;
  }
  return static_cast<uint32_t>(pages * 100.0f + 0.5f);
}

bool hasEnoughPaceSamplesForTimeLeft(const BookReadingStats& stats) {
  return stats.avgSecondsPerForwardPage > 0 && stats.paceSampleCount >= MIN_STORED_TIME_LEFT_PACE_SAMPLE_COUNT;
}

std::string confirmationHeading(const StrId actionLabelId) {
  return std::string(tr(STR_CONFIRM)) + ": " + std::string(I18N.get(actionLabelId));
}

EpubRenderMode normalizeRenderMode(const uint8_t rawMode) {
  return isValidEpubRenderMode(rawMode) ? static_cast<EpubRenderMode>(rawMode) : EpubRenderMode::CrossInkDefault;
}

uint8_t normalizeRenderModeRaw(const uint8_t rawMode) { return static_cast<uint8_t>(normalizeRenderMode(rawMode)); }

const char* sectionCacheSuffixForRenderMode(const EpubRenderMode renderMode) {
  switch (renderMode) {
    case EpubRenderMode::Balanced:
      return BALANCED_SECTION_CACHE_SUFFIX;
    case EpubRenderMode::Light:
      return LIGHT_SECTION_CACHE_SUFFIX;
    case EpubRenderMode::CrossInkDefault:
    default:
      return "";
  }
}

void getSyncPageAnchors(const Section& section, const int page, std::optional<uint16_t>& paragraphIndex,
                        std::optional<uint16_t>& listItemIndex) {
  if (page < 0 || page >= section.pageCount) {
    return;
  }

  const uint16_t sourcePage = static_cast<uint16_t>(page);
  if (const auto pIdx = section.getParagraphIndexForPage(sourcePage)) {
    paragraphIndex = *pIdx;
  }
  if (const auto liIdx = section.getListItemIndexForPage(sourcePage); liIdx.has_value() && *liIdx > 0) {
    listItemIndex = *liIdx;
  }
}

uint64_t hashFootnotePreviewAnchor(const std::string& anchor) {
  uint64_t hash = 1469598103934665603ULL;
  for (const char c : anchor) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ULL;
  }
  return hash;
}

const char* labelForRenderModeToast(const EpubRenderMode renderMode) {
  switch (renderMode) {
    case EpubRenderMode::Balanced:
      return tr(STR_BALANCED_MODE);
    case EpubRenderMode::Light:
      return tr(STR_LIGHT_MODE);
    case EpubRenderMode::CrossInkDefault:
    default:
      return "";
  }
}

std::array<EpubRenderMode, 3> fallbackModesForSelection(const EpubRenderMode selectedMode, uint8_t& count) {
  switch (selectedMode) {
    case EpubRenderMode::Balanced:
      count = 2;
      return {EpubRenderMode::Balanced, EpubRenderMode::Light, EpubRenderMode::Light};
    case EpubRenderMode::Light:
      count = 1;
      return {EpubRenderMode::Light, EpubRenderMode::Light, EpubRenderMode::Light};
    case EpubRenderMode::CrossInkDefault:
    default:
      count = 3;
      return {EpubRenderMode::CrossInkDefault, EpubRenderMode::Balanced, EpubRenderMode::Light};
  }
}

struct SectionBuildProfile {
  EpubRenderMode renderMode;
  bool embeddedStyle;
  bool bionicReadingEnabled;
  bool guideReadingEnabled;
  const char* label;
  bool safeMode;
};

const char* sectionBuildLabelForRenderMode(const EpubRenderMode renderMode) {
  switch (renderMode) {
    case EpubRenderMode::Balanced:
      return "balanced";
    case EpubRenderMode::Light:
      return "light";
    case EpubRenderMode::CrossInkDefault:
    default:
      return "primary";
  }
}

SectionBuildProfile buildProfileForRenderMode(const EpubRenderMode renderMode) {
  return SectionBuildProfile{renderMode,
                             SETTINGS.embeddedStyle != 0,
                             SETTINGS.bionicReadingEnabled != 0,
                             SETTINGS.guideReadingEnabled != 0,
                             sectionBuildLabelForRenderMode(renderMode),
                             false};
}

bool shouldAttemptSafeModeFallback() {
  return SETTINGS.embeddedStyle != 0 || SETTINGS.bionicReadingEnabled != 0 || SETTINGS.guideReadingEnabled != 0;
}

SectionBuildProfile safeModeBuildProfile() {
  return SectionBuildProfile{EpubRenderMode::Light, false, false, false, "safe", true};
}

struct SectionBuildAttempt {
  bool succeeded;
  bool lowMemory;
};

struct SectionFallbackResult {
  bool succeeded = false;
  bool lastAttemptLowMemory = false;
  bool usedSafeMode = false;
};

template <typename BuildFn, typename BeforeRetryFn>
SectionFallbackResult runSectionBuildFallbacks(const EpubRenderMode selectedMode, const bool allowSafeMode,
                                               BuildFn& build, BeforeRetryFn& beforeRetry,
                                               const SectionBuildAttempt* initialAttempt = nullptr) {
  SectionFallbackResult result;
  uint8_t fallbackCount = 0;
  const auto fallbackModes = fallbackModesForSelection(selectedMode, fallbackCount);
  uint8_t firstModeIndex = 0;
  if (initialAttempt) {
    result.succeeded = initialAttempt->succeeded;
    result.lastAttemptLowMemory = initialAttempt->lowMemory;
    firstModeIndex = 1;
  }
  for (uint8_t i = firstModeIndex; i < fallbackCount && !result.succeeded; ++i) {
    const SectionBuildProfile profile = buildProfileForRenderMode(fallbackModes[i]);
    if (i > 0) {
      if (!result.lastAttemptLowMemory) break;
      beforeRetry(profile);
    }
    const SectionBuildAttempt attempt = build(profile);
    result.succeeded = attempt.succeeded;
    result.lastAttemptLowMemory = attempt.lowMemory;
  }

  if (!result.succeeded && result.lastAttemptLowMemory && allowSafeMode) {
    const SectionBuildProfile profile = safeModeBuildProfile();
    beforeRetry(profile);
    const SectionBuildAttempt attempt = build(profile);
    result.succeeded = attempt.succeeded;
    result.lastAttemptLowMemory = attempt.lowMemory;
    result.usedSafeMode = attempt.succeeded;
  }
  return result;
}

ReaderRenderSpec readerRenderSpecForProfile(const int fontId, const uint16_t viewportWidth,
                                            const uint16_t viewportHeight, const SectionBuildProfile& profile) {
  ReaderRenderSpec spec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight, profile.renderMode);
  spec.fontId = fontId;
  spec.embeddedStyle = profile.embeddedStyle;
  spec.bionicReadingEnabled = profile.bionicReadingEnabled;
  spec.guideReadingEnabled = profile.guideReadingEnabled;
  return spec;
}

void ensureReaderSdFontLoaded(GfxRenderer& renderer) {
  sdFontSystem.ensureLoaded(renderer);
  // Layout only needs the active font. Release the settings-only family metadata
  // before building a section so it does not consume reader heap headroom.
  sdFontSystem.releaseRegistry();
}

void applySafeModeReaderSettings() {
  SETTINGS.epubRenderMode = static_cast<uint8_t>(EpubRenderMode::Light);
  SETTINGS.embeddedStyle = 0;
  SETTINGS.bionicReadingEnabled = 0;
  SETTINGS.guideReadingEnabled = 0;
}

bool hasEmSpacePrefix(const char* text) {
  return text && static_cast<unsigned char>(text[0]) == 0xE2 && static_cast<unsigned char>(text[1]) == 0x80 &&
         static_cast<unsigned char>(text[2]) == 0x83;
}

bool hasEmSpacePrefix(const std::string& text) { return text.size() >= 3 && hasEmSpacePrefix(text.c_str()); }

bool hasVisibleWordText(const char* text) {
  const char* cursor = text + (hasEmSpacePrefix(text) ? 3 : 0);
  while (*cursor) {
    if (*cursor != ' ' && *cursor != '\t' && *cursor != '\r' && *cursor != '\n') return true;
    cursor++;
  }
  return false;
}

bool hasVisibleWordText(const std::string& text) { return hasVisibleWordText(text.c_str()); }

struct ClippingPageMatch {
  uint16_t startWord = 0;
  uint16_t endWord = 0;
  uint16_t tableSelection = UINT16_MAX;
  bool startsAtClipStart = false;
  bool reachesClipEnd = false;
};

bool isUtf8SpaceAt(const char* cursor, size_t& advance) {
  const auto c = static_cast<unsigned char>(cursor[0]);
  if (c == 0xC2 && cursor[1] != '\0' && static_cast<unsigned char>(cursor[1]) == 0xA0) {
    advance = 2;
    return true;
  }
  if (c == 0xE2 && cursor[1] != '\0' && cursor[2] != '\0' && static_cast<unsigned char>(cursor[1]) == 0x80) {
    const auto c2 = static_cast<unsigned char>(cursor[2]);
    if (c2 == 0x83 || c2 == 0xAF) {
      advance = 3;
      return true;
    }
  }
  return false;
}

bool nextClipToken(const char*& cursor, const char*& tokenStart, size_t& tokenLen) {
  while (*cursor != '\0') {
    size_t advance = 0;
    if (std::isspace(static_cast<unsigned char>(*cursor)) || isUtf8SpaceAt(cursor, advance)) {
      cursor += advance > 0 ? advance : 1;
      continue;
    }
    break;
  }
  if (*cursor == '\0') {
    tokenStart = nullptr;
    tokenLen = 0;
    return false;
  }

  tokenStart = cursor;
  while (*cursor != '\0') {
    size_t advance = 0;
    if (std::isspace(static_cast<unsigned char>(*cursor)) || isUtf8SpaceAt(cursor, advance)) {
      break;
    }
    cursor++;
  }
  tokenLen = static_cast<size_t>(cursor - tokenStart);
  return true;
}

uint16_t countClipTokens(const std::string& text) {
  uint16_t count = 0;
  const char* cursor = text.c_str();
  const char* token = nullptr;
  size_t len = 0;
  while (nextClipToken(cursor, token, len) && count < UINT16_MAX) {
    count++;
  }
  return count;
}

bool advanceClipCursorToToken(const std::string& text, const uint16_t targetIndex, const char*& cursor,
                              const char*& tokenStart, size_t& tokenLen) {
  cursor = text.c_str();
  uint16_t index = 0;
  while (nextClipToken(cursor, tokenStart, tokenLen)) {
    if (index == targetIndex) {
      return true;
    }
    index++;
  }
  tokenStart = nullptr;
  tokenLen = 0;
  return false;
}

ClippingTextMatcher::TokenFragmentResult matchPageWordToToken(const TextBlock& block, const uint16_t wordIndex,
                                                              const char* token, const size_t tokenLen,
                                                              const size_t tokenOffset = 0) {
  const char* word = block.wordText(wordIndex);
  const char* visibleWord = word + (hasEmSpacePrefix(word) ? 3 : 0);
  return ClippingTextMatcher::matchTokenFragmentWithLength(visibleWord, block.wordEndsWithInsertedHyphen(wordIndex),
                                                           token, tokenLen, tokenOffset);
}

template <typename Callback>
bool forEachPageTextLine(const Page& page, Callback&& callback) {
  using CallbackType = std::remove_reference_t<Callback>;
  return page.forEachTextLine(
      [](const PageTextLine& line, void* context) { return (*static_cast<CallbackType*>(context))(line); }, &callback);
}

template <typename Callback>
bool forEachVisiblePageWord(const Page& page, Callback&& callback) {
  uint16_t wordIndex = 0;
  return forEachPageTextLine(page, [&](const PageTextLine& line) {
    const auto& block = *line.block;
    const uint16_t count = block.wordCount();
    for (uint16_t i = 0; i < count; ++i) {
      const char* word = block.wordText(i);
      const char* visibleWord = word + (hasEmSpacePrefix(word) ? 3 : 0);
      bool hasVisibleText = false;
      for (const char* p = visibleWord; *p != '\0'; ++p) {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
          hasVisibleText = true;
          break;
        }
      }
      if (!hasVisibleText) continue;

      if (!callback(wordIndex, line, block, i)) {
        return false;
      }
      wordIndex++;
    }
    return true;
  });
}

bool matchClipRunFromPageWord(const Page& page, const std::string& clippingText, const uint16_t startPageWord,
                              const uint16_t startClipToken, const uint16_t minPartialMatch,
                              const uint8_t expectedTableColumn, ClippingPageMatch& match) {
  const char* cursor = nullptr;
  const char* token = nullptr;
  size_t tokenLen = 0;
  if (!advanceClipCursorToToken(clippingText, startClipToken, cursor, token, tokenLen)) {
    return false;
  }

  uint16_t matchedTokens = 0;
  uint16_t lastWord = startPageWord;
  size_t tokenOffset = 0;
  bool reachedClipEnd = false;
  bool stoppedByMismatch = false;
  uint16_t tableSelection = UINT16_MAX;
  const bool tableColumnOnly = expectedTableColumn != UINT8_MAX;

  forEachVisiblePageWord(
      page, [&](const uint16_t wordIndex, const PageTextLine& line, const TextBlock& block, const size_t i) {
        if (wordIndex < startPageWord) {
          return true;
        }
        if (tableColumnOnly && wordIndex == startPageWord) {
          tableSelection = line.tableSelection;
          if (!ClippingHighlightGeometry::matchesTableColumn(
                  expectedTableColumn, static_cast<uint8_t>(tableSelection % TableFragmentRow::MAX_SERIALIZED_CELLS),
                  TableFragmentRow::MAX_SERIALIZED_CELLS)) {
            return false;
          }
        }
        if (tableColumnOnly && !ClippingHighlightGeometry::matchesTableSelection(tableSelection, line.tableSelection)) {
          return true;
        }

        const auto fragmentResult = matchPageWordToToken(block, static_cast<uint16_t>(i), token, tokenLen, tokenOffset);
        if (fragmentResult.match == ClippingTextMatcher::TokenFragmentMatch::MISMATCH) {
          stoppedByMismatch = true;
          return false;
        }

        lastWord = wordIndex;
        if (fragmentResult.match == ClippingTextMatcher::TokenFragmentMatch::CONTINUES_TOKEN) {
          tokenOffset += fragmentResult.tokenBytes;
          return true;
        }

        matchedTokens++;
        tokenOffset = 0;
        if (!nextClipToken(cursor, token, tokenLen)) {
          reachedClipEnd = true;
          return false;
        }
        return true;
      });

  if (matchedTokens == 0) {
    return false;
  }

  if (stoppedByMismatch) {
    return false;
  }

  // A relayout can split a saved clipping so this page starts mid-clipping.
  // Accept complete runs (the whole clipping matched, start to end) or
  // page-boundary partial runs of a meaningful length. Reaching the end of the
  // clip text is not enough on its own: starting the search at a late token
  // (e.g. the clipping's last word) trivially "reaches the end" after matching
  // a single coincidental word, which must not be treated as a full match.
  const bool completeClipMatch = startClipToken == 0 && reachedClipEnd;
  if (!completeClipMatch && matchedTokens < minPartialMatch) {
    const bool startsAtClipBoundary = startClipToken == 0;
    const bool startsAtPageBoundary = startPageWord == 0;
    if (!startsAtClipBoundary && !startsAtPageBoundary) {
      return false;
    }
  }

  match.startWord = startPageWord;
  match.endWord = lastWord;
  match.tableSelection = tableSelection;
  match.startsAtClipStart = startClipToken == 0;
  match.reachesClipEnd = reachedClipEnd;
  return true;
}

bool findClippingTextOnPage(const Page& page, const std::string& clippingText, ClippingPageMatch& match,
                            bool* uniqueMatch = nullptr, const uint8_t expectedTableColumn = UINT8_MAX) {
  if (clippingText.empty()) return false;

  const uint16_t tokenCount = countClipTokens(clippingText);
  if (tokenCount == 0) return false;
  const uint16_t minPartialMatch = std::min<uint16_t>(tokenCount, 3);
  const bool tableColumnOnly = expectedTableColumn != UINT8_MAX;

  ClippingMatchTracker matches;

  forEachVisiblePageWord(
      page, [&](const uint16_t wordIndex, const PageTextLine& line, const TextBlock& block, const size_t i) {
        if (tableColumnOnly && (!ClippingHighlightGeometry::isTableColumnCandidate(line.tableSelection) ||
                                line.tableSelection % TableFragmentRow::MAX_SERIALIZED_CELLS != expectedTableColumn)) {
          return true;
        }
        const char* cursor = clippingText.c_str();
        const char* token = nullptr;
        size_t tokenLen = 0;
        uint16_t tokenIndex = 0;
        while (nextClipToken(cursor, token, tokenLen)) {
          if (tokenIndex >= tokenCount) {
            break;
          }
          ClippingPageMatch candidate;
          if (matchPageWordToToken(block, static_cast<uint16_t>(i), token, tokenLen).match !=
                  ClippingTextMatcher::TokenFragmentMatch::MISMATCH &&
              matchClipRunFromPageWord(page, clippingText, wordIndex, tokenIndex, minPartialMatch, expectedTableColumn,
                                       candidate)) {
            if (matches.record(candidate.startWord, candidate.endWord)) {
              match = candidate;
            }
            if (!uniqueMatch || !matches.unique()) {
              return false;
            }
          }
          tokenIndex++;
        }
        return true;
      });

  if (uniqueMatch) {
    *uniqueMatch = matches.unique();
  }
  return matches.found();
}

uint16_t countVisiblePageWords(const Page& page) {
  uint16_t count = 0;
  forEachVisiblePageWord(page, [&](const uint16_t, const PageTextLine&, const TextBlock&, const size_t) {
    if (count == UINT16_MAX) return false;
    count++;
    return true;
  });
  return count;
}

bool findClippingStoredRangeOnPage(const Page& page, const Clipping& clipping, const uint16_t currentPage,
                                   const uint16_t currentPageCount, ClippingPageMatch& match) {
  if (clipping.wordCount == 0 || currentPageCount == 0 || clipping.pageCount != currentPageCount) {
    return false;
  }
  if (clipping.startPage > clipping.endPage || currentPage < clipping.startPage || currentPage > clipping.endPage) {
    return false;
  }

  const uint16_t pageWordCount = countVisiblePageWords(page);
  if (pageWordCount == 0) return false;

  uint16_t startWord = 0;
  uint16_t endWord = static_cast<uint16_t>(pageWordCount - 1);
  if (currentPage == clipping.startPage) {
    if (clipping.startWordIndex >= pageWordCount) return false;
    startWord = clipping.startWordIndex;
  }
  if (currentPage == clipping.endPage) {
    if (clipping.endWordIndex >= pageWordCount) return false;
    endWord = clipping.endWordIndex;
  }
  if (startWord > endWord) return false;

  match.startWord = startWord;
  match.endWord = endWord;
  match.tableSelection = clipping.tableSelection;
  return true;
}

uint16_t clampSectionPage(const uint32_t page, const uint16_t pageCount) {
  if (pageCount == 0) return 0;
  return static_cast<uint16_t>(std::min<uint32_t>(page, pageCount - 1));
}

uint16_t pageFromStoredProgress(const float progress, const uint16_t pageCount) {
  if (pageCount == 0 || progress <= 0.0f) return 0;
  if (progress >= 1.0f) return static_cast<uint16_t>(pageCount - 1);
  return clampSectionPage(static_cast<uint32_t>(progress * static_cast<float>(pageCount) + 0.001f), pageCount);
}

uint16_t approximateRelayoutPage(const Clipping& clipping, const uint16_t currentPageCount) {
  if (currentPageCount == 0) return 0;
  if (clipping.pageCount <= 1) return 0;

  const uint32_t oldLastPage = static_cast<uint32_t>(clipping.pageCount - 1);
  const uint32_t newLastPage = static_cast<uint32_t>(currentPageCount - 1);
  const uint32_t scaledPage =
      (static_cast<uint32_t>(clipping.startPage) * newLastPage + oldLastPage / 2U) / oldLastPage;
  return clampSectionPage(scaledPage, currentPageCount);
}

uint16_t resolveParagraphJumpPage(const Section& section, const uint16_t paragraphIndex, const uint16_t fallbackPage) {
  if (section.pageCount == 0 || paragraphIndex == UINT16_MAX) return fallbackPage;

  const uint16_t pageCount = static_cast<uint16_t>(section.pageCount);
  const uint16_t clampedFallback = clampSectionPage(fallbackPage, pageCount);
  const auto paragraphPage = section.getPageForParagraphIndex(paragraphIndex);
  if (!paragraphPage.has_value()) return clampedFallback;

  const uint16_t startPage = clampSectionPage(*paragraphPage, pageCount);
  if (clampedFallback < startPage) return startPage;

  if (paragraphIndex < UINT16_MAX - 1) {
    const auto nextParagraphPage = section.getPageForParagraphIndex(static_cast<uint16_t>(paragraphIndex + 1));
    if (nextParagraphPage.has_value() && *nextParagraphPage > startPage && clampedFallback >= *nextParagraphPage) {
      return static_cast<uint16_t>(*nextParagraphPage - 1);
    }
  }

  return clampedFallback;
}

uint8_t tableColumnForSelection(const uint16_t tableSelection) {
  return tableSelection == UINT16_MAX ? UINT8_MAX
                                      : static_cast<uint8_t>(tableSelection % TableFragmentRow::MAX_SERIALIZED_CELLS);
}

bool pageContainsClippingText(Section& section, const std::string& clippingText, const uint16_t page,
                              const uint8_t expectedTableColumn = UINT8_MAX) {
  section.currentPage = page;
  auto loadedPage = section.loadPage(page);
  if (!loadedPage) return false;

  ClippingPageMatch match;
  return findClippingTextOnPage(*loadedPage, clippingText, match, nullptr, expectedTableColumn);
}

bool findClippingPageNear(Section& section, const std::string& clippingText, const uint16_t center,
                          const uint16_t radius, uint16_t& outPage, const uint8_t expectedTableColumn = UINT8_MAX) {
  if (section.pageCount == 0) return false;

  const uint16_t pageCount = static_cast<uint16_t>(section.pageCount);
  const uint16_t clampedCenter = clampSectionPage(center, pageCount);
  if (pageContainsClippingText(section, clippingText, clampedCenter, expectedTableColumn)) {
    outPage = clampedCenter;
    return true;
  }

  for (uint16_t distance = 1; distance <= radius; ++distance) {
    if (clampedCenter >= distance) {
      const uint16_t before = static_cast<uint16_t>(clampedCenter - distance);
      if (pageContainsClippingText(section, clippingText, before, expectedTableColumn)) {
        outPage = before;
        return true;
      }
    }
    const uint32_t after = static_cast<uint32_t>(clampedCenter) + distance;
    if (after < pageCount &&
        pageContainsClippingText(section, clippingText, static_cast<uint16_t>(after), expectedTableColumn)) {
      outPage = static_cast<uint16_t>(after);
      return true;
    }
  }
  return false;
}

uint16_t resolveClippingJumpPage(Section& section, const Clipping& clipping, const std::string& clippingText,
                                 const uint16_t fallbackPage) {
  constexpr uint16_t SEARCH_RADIUS = 8;
  if (section.pageCount == 0) return fallbackPage;

  const uint16_t pageCount = static_cast<uint16_t>(section.pageCount);
  const uint8_t expectedTableColumn = tableColumnForSelection(clipping.tableSelection);
  uint16_t resolvedPage = clampSectionPage(fallbackPage, pageCount);
  const uint16_t approximatePage = approximateRelayoutPage(clipping, pageCount);
  if (!clippingText.empty() &&
      findClippingPageNear(section, clippingText, approximatePage, SEARCH_RADIUS, resolvedPage, expectedTableColumn)) {
    return resolvedPage;
  }

  if (clipping.paragraphIndex != UINT16_MAX) {
    const auto paragraphPage = section.getPageForParagraphIndex(clipping.paragraphIndex);
    if (paragraphPage.has_value() && !clippingText.empty() &&
        findClippingPageNear(section, clippingText, clampSectionPage(*paragraphPage, pageCount), SEARCH_RADIUS,
                             resolvedPage, expectedTableColumn)) {
      return resolvedPage;
    }
  }

  if (!clippingText.empty()) {
    findClippingPageNear(section, clippingText, resolvedPage, SEARCH_RADIUS, resolvedPage, expectedTableColumn);
  }
  return resolvedPage;
}

constexpr int GRAYSCALE_STRIP_ROWS = 80;

bool runTiledGrayscalePass(GfxRenderer& renderer, const Page& page, const int fontId, const int marginLeft,
                           const int marginTop, const bool foregroundBlack, const bool needsTextGrayscale,
                           const bool needsImageGrayscale, uint8_t* scratch, const size_t scratchSize,
                           const bool asyncRefreshPending) {
  if ((!needsTextGrayscale && !needsImageGrayscale) || !renderer.supportsStripGrayscale()) {
    return false;
  }

  const int displayHeight = renderer.getDisplayHeight();
  const int displayWidthBytes = renderer.getDisplayWidthBytes();
  const size_t planeBytes = static_cast<size_t>(displayWidthBytes) * displayHeight;

  const auto renderPlaneToBuffer = [&](const GfxRenderer::RenderMode mode, uint8_t* buffer) {
    renderer.setRenderMode(mode);
    for (int y = 0; y < displayHeight; y += GRAYSCALE_STRIP_ROWS) {
      const int rows = std::min(GRAYSCALE_STRIP_ROWS, displayHeight - y);
      renderer.beginStripTarget(buffer + static_cast<size_t>(y) * displayWidthBytes, y, rows);
      renderer.clearScreen(0x00);
      if (needsTextGrayscale) {
        page.render(renderer, fontId, marginLeft, marginTop, foregroundBlack);
      } else {
        page.renderImages(renderer, fontId, marginLeft, marginTop);
      }
      renderer.endStripTarget();
    }
  };

  // Whole-plane buffers are about 48 KB each, so they are unsuitable for the
  // task stack or permanent activity storage. Each transient allocation must
  // leave enough total and contiguous heap for the next render allocations.
  constexpr size_t PLANE_BUFFER_FREE_HEAP_RESERVE = 60000;
  constexpr size_t PLANE_BUFFER_MAX_ALLOC_RESERVE = 16 * 1024;
  const bool usePsramPlanes = psramHeapAvailable();
  const auto planeBufferFits = [planeBytes, usePsramPlanes] {
    if (usePsramPlanes) {
      constexpr size_t PSRAM_PLANE_RESERVE = 128 * 1024;
      const auto psram = MemoryBudget::psramSnapshot();
      return psram.freeHeap >= planeBytes + PSRAM_PLANE_RESERVE && psram.maxAllocHeap >= planeBytes;
    }
    return ESP.getFreeHeap() >= planeBytes + PLANE_BUFFER_FREE_HEAP_RESERVE &&
           ESP.getMaxAllocHeap() >= planeBytes + PLANE_BUFFER_MAX_ALLOC_RESERVE;
  };
  const auto allocatePlane = [planeBytes, usePsramPlanes] {
    return usePsramPlanes ? makePsramByteBufferNoThrow(planeBytes) : makeHeapByteBufferNoThrow(planeBytes);
  };
  auto lsbPlaneBuf = (asyncRefreshPending && planeBufferFits()) ? allocatePlane() : HeapByteBuffer{};
  auto msbPlaneBuf = (lsbPlaneBuf && planeBufferFits()) ? allocatePlane() : HeapByteBuffer{};

  if (lsbPlaneBuf) {
    if (usePsramPlanes) {
      LOG_INF("EPS", "Using PSRAM grayscale planes: bytes=%u count=%u", static_cast<unsigned>(planeBytes),
              msbPlaneBuf ? 2U : 1U);
    }
    renderPlaneToBuffer(GfxRenderer::GRAYSCALE_LSB, lsbPlaneBuf.get());
    if (msbPlaneBuf) {
      renderPlaneToBuffer(GfxRenderer::GRAYSCALE_MSB, msbPlaneBuf.get());
    }

    renderer.waitRefreshComplete();
    renderer.writeGrayscalePlaneStrip(true, lsbPlaneBuf.get(), 0, displayHeight);
    if (msbPlaneBuf) {
      renderer.writeGrayscalePlaneStrip(false, msbPlaneBuf.get(), 0, displayHeight);
    } else {
      renderPlaneToBuffer(GfxRenderer::GRAYSCALE_MSB, lsbPlaneBuf.get());
      renderer.writeGrayscalePlaneStrip(false, lsbPlaneBuf.get(), 0, displayHeight);
    }

    renderer.setRenderMode(GfxRenderer::BW);
    renderer.displayGrayBuffer();
    renderer.cleanupGrayscaleWithFrameBuffer();
    return true;
  }

  if (asyncRefreshPending) {
    // Controller writes and the BW snapshot fallback both need the refresh to
    // be complete when the whole-plane allocation cannot be satisfied.
    renderer.waitRefreshComplete();
  }

  const size_t requiredScratchSize = static_cast<size_t>(displayWidthBytes) * GRAYSCALE_STRIP_ROWS;
  if (!scratch || scratchSize < requiredScratchSize) {
    if (asyncRefreshPending) {
      // The shadow-free async update does not rebuild the controller's
      // differential baseline. Re-sync it even when grayscale is skipped.
      renderer.cleanupGrayscaleWithFrameBuffer();
    }
    return false;
  }

  // Keep the live BW framebuffer intact, stream grayscale planes by row-band,
  // then re-sync the controller BW state from the framebuffer.
  const auto renderPlane = [&](const GfxRenderer::RenderMode mode, const bool lsbPlane) {
    renderer.setRenderMode(mode);
    for (int y = 0; y < displayHeight; y += GRAYSCALE_STRIP_ROWS) {
      const int rows = std::min(GRAYSCALE_STRIP_ROWS, displayHeight - y);
      renderer.beginStripTarget(scratch, y, rows);
      renderer.clearScreen(0x00);
      if (needsTextGrayscale) {
        page.render(renderer, fontId, marginLeft, marginTop, foregroundBlack);
      } else {
        page.renderImages(renderer, fontId, marginLeft, marginTop);
      }
      renderer.endStripTarget();
      renderer.writeGrayscalePlaneStrip(lsbPlane, scratch, y, rows);
    }
  };

  renderPlane(GfxRenderer::GRAYSCALE_LSB, true);

  renderPlane(GfxRenderer::GRAYSCALE_MSB, false);

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayGrayBuffer();
  renderer.cleanupGrayscaleWithFrameBuffer();
  return true;
}

ToastRect computeToastRect(const GfxRenderer& renderer, const char* msg) {
  constexpr int toastPadX = 20;
  constexpr int toastPadY = 12;
  const int msgW = renderer.getTextWidth(UI_10_FONT_ID, msg);
  const int msgH = renderer.getLineHeight(UI_10_FONT_ID);
  const int toastW = msgW + toastPadX * 2;
  const int toastH = msgH + toastPadY * 2;
  const int toastX = (renderer.getScreenWidth() - toastW) / 2;
  const int toastY = (renderer.getScreenHeight() - toastH) / 2;
  return {toastX, toastY, toastW, toastH};
}

void drawToastBuffer(const GfxRenderer& renderer, const char* msg) {
  constexpr int toastPadX = 20;
  constexpr int toastPadY = 12;
  const bool toastBackgroundBlack = ReaderUtils::readerForegroundBlack();
  const ToastRect toast = computeToastRect(renderer, msg);
  renderer.fillRect(toast.x, toast.y, toast.w, toast.h, toastBackgroundBlack);
  renderer.drawRect(toast.x, toast.y, toast.w, toast.h, !toastBackgroundBlack);
  renderer.drawText(UI_10_FONT_ID, toast.x + toastPadX, toast.y + toastPadY, msg, !toastBackgroundBlack);
}

void drawToast(const GfxRenderer& renderer, const char* msg) {
  drawToastBuffer(renderer, msg);
  renderer.displayBuffer();
}

void drawPublisherPageMarkers(const GfxRenderer& renderer, const Page& page, const int contentTop,
                              const int contentBottom, const bool foregroundBlack = true) {
  if (!SETTINGS.publisherPageNumbers || page.publisherPageMarkers.empty()) {
    return;
  }

  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int lineStep = std::max(1, lineHeight - 2);
  const int availableHeight = contentBottom - contentTop;
  if (availableHeight <= lineHeight) {
    return;
  }

  for (const auto& marker : page.publisherPageMarkers) {
    const char* label = marker.label;
    if (!label || label[0] == '\0') {
      continue;
    }

    bool hasNonAscii = false;
    int labelLen = 0;
    int maxCharWidth = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(label); *p != '\0'; p++) {
      if (*p >= 0x80) {
        hasNonAscii = true;
        break;
      }
      if (*p <= ' ') {
        continue;
      }
      const char ch[2] = {static_cast<char>(*p), '\0'};
      maxCharWidth = std::max(maxCharWidth, renderer.getTextWidth(SMALL_FONT_ID, ch));
      labelLen++;
    }

    if (labelLen == 0) {
      continue;
    }

    const int x = publisherPageNumberX(renderer);
    if (hasNonAscii) {
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, label);
      const int maxY = contentBottom - lineHeight;
      const int y = maxY < contentTop ? contentTop : std::min(std::max(contentTop + marker.yPos, contentTop), maxY);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, x, y + textWidth, label, foregroundBlack);
      continue;
    }

    const int markerHeight = lineHeight + (labelLen - 1) * lineStep;
    int y = contentTop + marker.yPos - lineHeight / 2;
    const int maxY = contentBottom - markerHeight;
    y = maxY < contentTop ? contentTop : std::min(std::max(y, contentTop), maxY);

    int row = 0;
    for (const char* p = label; *p != '\0'; p++) {
      if (static_cast<unsigned char>(*p) <= ' ') {
        continue;
      }
      const char ch[2] = {*p, '\0'};
      const int charWidth = renderer.getTextWidth(SMALL_FONT_ID, ch);
      renderer.drawText(SMALL_FONT_ID, x + (maxCharWidth - charWidth) / 2, y + row * lineStep, ch, foregroundBlack);
      row++;
    }
  }
}

uint8_t effectiveReaderLeftMargin() {
  return SETTINGS.publisherPageNumbers
             ? std::max<uint8_t>(SETTINGS.screenMarginHorizontal, PUBLISHER_PAGE_NUMBER_LEFT_MARGIN_MIN)
             : SETTINGS.screenMarginHorizontal;
}

struct ReaderViewportLayout {
  int marginTop;
  int marginRight;
  int marginBottom;
  int marginLeft;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
};

ReaderViewportLayout computeReaderViewportLayout(GfxRenderer& renderer, const bool automaticPageTurnActive,
                                                 const bool showFootnoteHeader = false) {
  ReaderViewportLayout layout{};
  renderer.getOrientedViewableTRBL(&layout.marginTop, &layout.marginRight, &layout.marginBottom, &layout.marginLeft);
  layout.marginLeft += effectiveReaderLeftMargin();
  layout.marginRight += SETTINGS.screenMarginHorizontal;

  const int topStatusBarReservedHeight = ReaderUtils::getTopClockStatusBarReservedHeight(renderer);
  if (topStatusBarReservedHeight > 0) {
    layout.marginTop += std::max(static_cast<int>(SETTINGS.screenMarginVertical),
                                 topStatusBarReservedHeight + ReaderUtils::TOP_CLOCK_TEXT_PADDING);
  } else {
    layout.marginTop += SETTINGS.screenMarginVertical;
  }

#if CROSSINK_APP_CAP_TOUCH
  if (showFootnoteHeader) {
    const Rect header = TouchHeaderBackButton::compactHeaderRect(renderer);
    layout.marginTop =
        std::max(layout.marginTop, header.y + header.height + static_cast<int>(SETTINGS.screenMarginVertical));
  }
#else
  (void)showFootnoteHeader;
#endif

  layout.marginBottom += ReaderUtils::getReaderFooterReservedHeight(automaticPageTurnActive);

  layout.viewportWidth = renderer.getScreenWidth() - layout.marginLeft - layout.marginRight;
  layout.viewportHeight = renderer.getScreenHeight() - layout.marginTop - layout.marginBottom;
  return layout;
}

bool releaseReaderSdFontCachesForLowMemory(const GfxRenderer& renderer, const char* tag, const char* reason) {
  const int fontId = SETTINGS.getReaderFontId();
  if (!renderer.isSdCardFont(fontId)) {
    return false;
  }

#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  const auto before = MemoryBudget::snapshot();
#endif
  if (!renderer.releaseSdCardFontForLowMemory(fontId)) {
    return false;
  }
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  const auto after = MemoryBudget::snapshot();
  LOG_DBG(tag, "Released SD font caches after %s: free=%u->%u maxAlloc=%u->%u", reason, before.freeHeap, after.freeHeap,
          before.maxAllocHeap, after.maxAllocHeap);
#endif
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

bool isSnippetWhitespace(const char* word) {
  if (!word || *word == '\0') return true;
  for (const char* cursor = word; *cursor != '\0'; ++cursor) {
    if (*cursor != ' ' && *cursor != '\r' && *cursor != '\n' && *cursor != '\t') {
      return false;
    }
  }
  return true;
}

void buildBookmarkSnippet(const Page& page, char* out, const size_t outSize) {
  if (!out || outSize == 0) return;
  out[0] = '\0';
  size_t len = 0;

  for (const auto& el : page.elements) {
    if (el->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*el);
    if (!line.getBlock()) continue;
    const auto& block = *line.getBlock();
    for (uint16_t i = 0; i < block.wordCount(); ++i) {
      const char* word = block.wordText(i);
      if (isSnippetWhitespace(word)) continue;
      const size_t separatorLen = len > 0 ? 1 : 0;
      const size_t wordLen = std::strlen(word);
      if (len + separatorLen + wordLen >= outSize) return;
      if (separatorLen > 0) out[len++] = ' ';
      memcpy(out + len, word, wordLen);
      len += wordLen;
      out[len] = '\0';
    }
  }
}

uint16_t clampAutoPageTurnIntervalSeconds(const uint16_t seconds) {
  return std::clamp(seconds, MIN_AUTO_PAGE_TURN_INTERVAL_S, MAX_AUTO_PAGE_TURN_INTERVAL_S);
}

bool readExact(FsFile& file, void* data, const size_t size) { return file.read(data, size) == static_cast<int>(size); }

bool writeExact(FsFile& file, const void* data, const size_t size) { return file.write(data, size) == size; }

bool readU8(FsFile& file, uint8_t& value) { return readExact(file, &value, sizeof(value)); }

bool writeU8(FsFile& file, const uint8_t value) { return writeExact(file, &value, sizeof(value)); }

bool readU16(FsFile& file, uint16_t& value) {
  uint8_t data[2] = {};
  if (!readExact(file, data, sizeof(data))) return false;
  value = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
  return true;
}

bool writeU16(FsFile& file, const uint16_t value) {
  const uint8_t data[2] = {static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF)};
  return writeExact(file, data, sizeof(data));
}

void captureReaderSettings(EpubReaderActivity::ReaderSettingsSnapshot& out) {
  out.fontFamily = SETTINGS.fontFamily;
  out.readerFontPointSize = SETTINGS.readerFontPointSize;
  out.lineHeightPercent = SETTINGS.lineHeightPercent;
  out.wordSpacing = SETTINGS.wordSpacing;
  out.orientation = SETTINGS.orientation;
  out.screenMarginVertical = SETTINGS.screenMarginVertical;
  out.screenMarginHorizontal = SETTINGS.screenMarginHorizontal;
  out.publisherPageNumbers = SETTINGS.publisherPageNumbers;
  out.paragraphAlignment = SETTINGS.paragraphAlignment;
  out.embeddedStyle = SETTINGS.embeddedStyle;
  out.hyphenationEnabled = SETTINGS.hyphenationEnabled;
  out.textAntiAliasing = SETTINGS.textAntiAliasing;
  out.imageRendering = SETTINGS.imageRendering;
  out.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  out.forceParagraphIndents = SETTINGS.forceParagraphIndents;
  out.bionicReadingEnabled = SETTINGS.bionicReadingEnabled;
  out.guideReadingEnabled = SETTINGS.guideReadingEnabled;
  out.epubRenderMode = normalizeRenderModeRaw(SETTINGS.epubRenderMode);
  out.indexingMethod = SETTINGS.indexingMethod;
  std::strncpy(out.sdFontFamilyName, SETTINGS.sdFontFamilyName, sizeof(out.sdFontFamilyName) - 1);
  out.sdFontFamilyName[sizeof(out.sdFontFamilyName) - 1] = '\0';
}

void applyReaderSettings(const EpubReaderActivity::ReaderSettingsSnapshot& in) {
  SETTINGS.fontFamily = in.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? in.fontFamily : SETTINGS.fontFamily;
  std::strncpy(SETTINGS.sdFontFamilyName, in.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName) - 1);
  SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
  if (in.readerFontPointSize < CrossPointSettings::MIN_READER_FONT_POINT_SIZE) {
    if (in.sdFontFamilyName[0] != '\0') {
      SETTINGS.readerFontPointSize = sdFontSystem.resolveLegacySizeStep(in.sdFontFamilyName, in.readerFontPointSize);
    } else {
      SETTINGS.readerFontPointSize = CrossPointSettings::getReaderFontPointSize(
          static_cast<CrossPointSettings::FONT_SIZE>(in.readerFontPointSize));
    }
  } else {
    SETTINGS.readerFontPointSize = in.readerFontPointSize;
  }
  SETTINGS.lineHeightPercent = CrossPointSettings::clampedLineHeightPercent(in.lineHeightPercent);
  SETTINGS.wordSpacing = std::min<uint8_t>(in.wordSpacing, CrossPointSettings::MAX_WORD_SPACING);
  SETTINGS.orientation = in.orientation < CrossPointSettings::ORIENTATION_COUNT ? in.orientation : SETTINGS.orientation;
  SETTINGS.screenMarginVertical = std::clamp<uint8_t>(in.screenMarginVertical, CrossPointSettings::MIN_SCREEN_MARGIN,
                                                      CrossPointSettings::MAX_SCREEN_MARGIN);
  SETTINGS.screenMarginHorizontal = std::clamp<uint8_t>(
      in.screenMarginHorizontal, CrossPointSettings::MIN_SCREEN_MARGIN, CrossPointSettings::MAX_SCREEN_MARGIN);
  SETTINGS.publisherPageNumbers = in.publisherPageNumbers ? 1 : 0;
  SETTINGS.paragraphAlignment = in.paragraphAlignment < CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT
                                    ? in.paragraphAlignment
                                    : SETTINGS.paragraphAlignment;
  SETTINGS.embeddedStyle = in.embeddedStyle ? 1 : 0;
  SETTINGS.hyphenationEnabled = in.hyphenationEnabled ? 1 : 0;
  SETTINGS.textAntiAliasing = in.textAntiAliasing ? 1 : 0;
  SETTINGS.imageRendering =
      in.imageRendering < CrossPointSettings::IMAGE_RENDERING_COUNT ? in.imageRendering : SETTINGS.imageRendering;
  SETTINGS.extraParagraphSpacing = in.extraParagraphSpacing ? 1 : 0;
  SETTINGS.forceParagraphIndents = in.forceParagraphIndents ? 1 : 0;
  SETTINGS.bionicReadingEnabled = in.bionicReadingEnabled ? 1 : 0;
  SETTINGS.guideReadingEnabled = in.guideReadingEnabled ? 1 : 0;
  SETTINGS.epubRenderMode = normalizeRenderModeRaw(in.epubRenderMode);
  SETTINGS.indexingMethod = in.indexingMethod < CrossPointSettings::INDEXING_METHOD_COUNT
                                ? in.indexingMethod
                                : CrossPointSettings::INDEXING_FULL_SECTION;
}

using BookReaderSettingsData = EpubReaderActivity::BookReaderSettingsData;

bool readReaderSettingsSnapshot(FsFile& file, EpubReaderActivity::ReaderSettingsSnapshot& out,
                                const bool includesWordSpacing, const bool includesIndexingMethod,
                                const bool includesSplitScreenMargins, const bool includesLegacyReaderDarkMode) {
  if (!(readU8(file, out.fontFamily) && readU8(file, out.readerFontPointSize) && readU8(file, out.lineHeightPercent) &&
        (!includesWordSpacing || readU8(file, out.wordSpacing)) && readU8(file, out.orientation))) {
    return false;
  }

  uint8_t legacyScreenMargin = CrossPointSettings::MIN_SCREEN_MARGIN;
  const bool marginsRead = includesSplitScreenMargins
                               ? readU8(file, out.screenMarginVertical) && readU8(file, out.screenMarginHorizontal)
                               : readU8(file, legacyScreenMargin);
  if (!includesSplitScreenMargins) {
    out.screenMarginVertical = legacyScreenMargin;
    out.screenMarginHorizontal = legacyScreenMargin;
  }
  // Versions 2-8 wrote a per-book Dark Mode byte. Consume it to preserve
  // their remaining reader settings, but never let one book change the
  // global display inversion setting.
  uint8_t discardedLegacyReaderDarkMode = 0;
  if (!(marginsRead && readU8(file, out.publisherPageNumbers) && readU8(file, out.paragraphAlignment) &&
        readU8(file, out.embeddedStyle) && readU8(file, out.hyphenationEnabled) && readU8(file, out.textAntiAliasing) &&
        (!includesLegacyReaderDarkMode || readU8(file, discardedLegacyReaderDarkMode)) &&
        readU8(file, out.imageRendering) && readU8(file, out.extraParagraphSpacing) &&
        readU8(file, out.forceParagraphIndents) && readU8(file, out.bionicReadingEnabled) &&
        readU8(file, out.guideReadingEnabled))) {
    return false;
  }
  if (!readU8(file, out.epubRenderMode)) {
    return false;
  }
  out.epubRenderMode = normalizeRenderModeRaw(out.epubRenderMode);
  if (includesIndexingMethod && !readU8(file, out.indexingMethod)) {
    return false;
  }
  return readExact(file, out.sdFontFamilyName, sizeof(out.sdFontFamilyName));
}

bool writeReaderSettingsSnapshot(FsFile& file, const EpubReaderActivity::ReaderSettingsSnapshot& in) {
  return writeU8(file, in.fontFamily) && writeU8(file, in.readerFontPointSize) && writeU8(file, in.lineHeightPercent) &&
         writeU8(file, std::min<uint8_t>(in.wordSpacing, CrossPointSettings::MAX_WORD_SPACING)) &&
         writeU8(file, in.orientation) && writeU8(file, in.screenMarginVertical) &&
         writeU8(file, in.screenMarginHorizontal) && writeU8(file, in.publisherPageNumbers) &&
         writeU8(file, in.paragraphAlignment) && writeU8(file, in.embeddedStyle) &&
         writeU8(file, in.hyphenationEnabled) && writeU8(file, in.textAntiAliasing) &&
         writeU8(file, in.imageRendering) && writeU8(file, in.extraParagraphSpacing) &&
         writeU8(file, in.forceParagraphIndents) && writeU8(file, in.bionicReadingEnabled) &&
         writeU8(file, in.guideReadingEnabled) && writeU8(file, normalizeRenderModeRaw(in.epubRenderMode)) &&
         writeU8(file, in.indexingMethod < CrossPointSettings::INDEXING_METHOD_COUNT
                           ? in.indexingMethod
                           : CrossPointSettings::INDEXING_FULL_SECTION) &&
         writeExact(file, in.sdFontFamilyName, sizeof(in.sdFontFamilyName));
}

BookReaderSettingsData loadBookReaderSettingsFile(const std::string& cachePath) {
  BookReaderSettingsData data;
  captureReaderSettings(data.readerSettings);
  std::strncpy(data.dictionarySdFontFamilyName, SETTINGS.dictionarySdFontFamilyName,
               sizeof(data.dictionarySdFontFamilyName) - 1);
  data.dictionarySdFontFamilyName[sizeof(data.dictionarySdFontFamilyName) - 1] = '\0';
  data.dictionaryFontPointSize = SETTINGS.dictionaryFontPointSize;

  FsFile file;
  if (!Storage.openFileForRead("ERS", cachePath + READER_SETTINGS_FILE_NAME, file)) {
    return data;
  }

  uint8_t version = 0;
  if (!readU8(file, version)) {
    file.close();
    LOG_DBG("ERS", "Reader settings missing version, using defaults");
    return data;
  }

  if (version == LEGACY_READER_SETTINGS_FILE_VERSION) {
    uint16_t seconds = 0;
    if (readU16(file, seconds) && seconds != 0) {
      data.hasAutoPageTurnInterval = true;
      data.autoPageTurnSeconds = clampAutoPageTurnIntervalSeconds(seconds);
    }
    file.close();
    return data;
  }

  if (version != PRE_WORD_SPACING_READER_SETTINGS_FILE_VERSION &&
      version != PRE_INDEXING_METHOD_READER_SETTINGS_FILE_VERSION &&
      version != PRE_DICTIONARY_FONT_READER_SETTINGS_FILE_VERSION &&
      version != PRE_POINT_SIZE_READER_SETTINGS_FILE_VERSION &&
      version != PRE_DICTIONARY_FONT_SIZE_READER_SETTINGS_FILE_VERSION &&
      version != PRE_SPLIT_SCREEN_MARGIN_READER_SETTINGS_FILE_VERSION &&
      version != PRE_GLOBAL_DARK_MODE_READER_SETTINGS_FILE_VERSION && version != READER_SETTINGS_FILE_VERSION) {
    file.close();
    LOG_DBG("ERS", "Reader settings version mismatch, using defaults");
    return data;
  }

  uint8_t flags = 0;
  uint16_t seconds = 0;
  uint8_t renderMode = static_cast<uint8_t>(EpubRenderMode::CrossInkDefault);
  EpubReaderActivity::ReaderSettingsSnapshot snapshot;
  // Version 2 books inherit the current global indexing method instead of
  // silently changing modes when their older custom settings are loaded.
  snapshot.indexingMethod = data.readerSettings.indexingMethod;
  bool ok = readU8(file, flags) && readU16(file, seconds);
  if (ok) {
    ok = readU8(file, renderMode);
  }
  if (ok) {
    ok = readReaderSettingsSnapshot(file, snapshot, version >= PRE_INDEXING_METHOD_READER_SETTINGS_FILE_VERSION,
                                    version >= PRE_DICTIONARY_FONT_READER_SETTINGS_FILE_VERSION,
                                    version >= PRE_GLOBAL_DARK_MODE_READER_SETTINGS_FILE_VERSION,
                                    version <= PRE_GLOBAL_DARK_MODE_READER_SETTINGS_FILE_VERSION);
  }
  if (ok && version >= PRE_POINT_SIZE_READER_SETTINGS_FILE_VERSION) {
    ok = readExact(file, data.dictionarySdFontFamilyName, sizeof(data.dictionarySdFontFamilyName));
  }
  if (ok && version >= PRE_SPLIT_SCREEN_MARGIN_READER_SETTINGS_FILE_VERSION) {
    ok = readU8(file, data.dictionaryFontPointSize);
  }
  file.close();
  if (!ok) {
    LOG_ERR("ERS", "Reader settings file is truncated, using defaults");
    return data;
  }

  if ((flags & READER_SETTINGS_FLAG_AUTO_PAGE_TURN) && seconds != 0) {
    data.hasAutoPageTurnInterval = true;
    data.autoPageTurnSeconds = clampAutoPageTurnIntervalSeconds(seconds);
  }
  if (flags & READER_SETTINGS_FLAG_CUSTOM) {
    data.hasCustomReaderSettings = true;
    data.readerSettings = snapshot;
  }
  if (flags & READER_SETTINGS_FLAG_RENDER_MODE) {
    data.hasRenderModeOverride = true;
    data.renderMode = normalizeRenderModeRaw(renderMode);
  }
  if (flags & READER_SETTINGS_FLAG_DICTIONARY_FONT) {
    data.dictionarySdFontFamilyName[sizeof(data.dictionarySdFontFamilyName) - 1] = '\0';
    data.hasDictionaryFontOverride = data.dictionarySdFontFamilyName[0] != '\0';
  }
  if (!data.hasDictionaryFontOverride) {
    std::strncpy(data.dictionarySdFontFamilyName, SETTINGS.dictionarySdFontFamilyName,
                 sizeof(data.dictionarySdFontFamilyName) - 1);
    data.dictionarySdFontFamilyName[sizeof(data.dictionarySdFontFamilyName) - 1] = '\0';
    data.dictionaryFontPointSize = SETTINGS.dictionaryFontPointSize;
  }
  return data;
}

bool saveBookReaderSettingsFile(const std::string& cachePath, const BookReaderSettingsData& data) {
  FsFile file;
  if (!Storage.openFileForWrite("ERS", cachePath + READER_SETTINGS_FILE_NAME, file)) {
    LOG_ERR("ERS", "Could not open reader settings file for write");
    return false;
  }

  uint8_t flags = 0;
  if (data.hasCustomReaderSettings) flags |= READER_SETTINGS_FLAG_CUSTOM;
  if (data.hasAutoPageTurnInterval) flags |= READER_SETTINGS_FLAG_AUTO_PAGE_TURN;
  if (data.hasRenderModeOverride) flags |= READER_SETTINGS_FLAG_RENDER_MODE;
  if (data.hasDictionaryFontOverride && data.dictionarySdFontFamilyName[0] != '\0') {
    flags |= READER_SETTINGS_FLAG_DICTIONARY_FONT;
  }
  const uint16_t clampedSeconds = clampAutoPageTurnIntervalSeconds(data.autoPageTurnSeconds);
  EpubReaderActivity::ReaderSettingsSnapshot normalizedReaderSettings = data.readerSettings;
  normalizedReaderSettings.epubRenderMode = normalizeRenderModeRaw(data.renderMode);
  const bool ok = writeU8(file, READER_SETTINGS_FILE_VERSION) && writeU8(file, flags) &&
                  writeU16(file, clampedSeconds) && writeU8(file, normalizeRenderModeRaw(data.renderMode)) &&
                  writeReaderSettingsSnapshot(file, normalizedReaderSettings) &&
                  writeExact(file, data.dictionarySdFontFamilyName, sizeof(data.dictionarySdFontFamilyName)) &&
                  writeU8(file, data.dictionaryFontPointSize);
  file.close();
  if (!ok) {
    LOG_ERR("ERS", "Short write saving reader settings");
  }
  return ok;
}

bool saveBookRenderModeForCache(const std::string& cachePath, const uint8_t renderMode) {
  BookReaderSettingsData data = loadBookReaderSettingsFile(cachePath);
  data.hasRenderModeOverride = true;
  data.renderMode = normalizeRenderModeRaw(renderMode);
  data.readerSettings.epubRenderMode = data.renderMode;
  return saveBookReaderSettingsFile(cachePath, data);
}

bool saveRuntimeReaderSettingsForCache(const std::string& cachePath) {
  BookReaderSettingsData data = loadBookReaderSettingsFile(cachePath);
  EpubReaderActivity::ReaderSettingsSnapshot snapshot;
  captureReaderSettings(snapshot);
  data.hasCustomReaderSettings = true;
  data.hasRenderModeOverride = true;
  data.renderMode = normalizeRenderModeRaw(SETTINGS.epubRenderMode);
  data.readerSettings = snapshot;
  return saveBookReaderSettingsFile(cachePath, data);
}

class ScopedReaderSettingsRestore {
 public:
  ScopedReaderSettingsRestore() { captureReaderSettings(snapshot); }
  ~ScopedReaderSettingsRestore() { applyReaderSettings(snapshot); }

 private:
  EpubReaderActivity::ReaderSettingsSnapshot snapshot;
};

// SD card folder finished books are moved into. Single source of truth for the path.
constexpr char READ_FOLDER[] = "/Read";

// True if path is inside READ_FOLDER (starts with "<READ_FOLDER>/"). Non-allocating so
// it is cheap to call from loop(), and avoids reintroducing a separate "/Read/" literal.
bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;  // excludes NUL
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

// Relocate a finished book into /Read/, then migrate path-keyed state such as
// cache files, bookmarks, recents, and resume path.
void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath, const std::string& title,
                                  const std::string& author) {
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

  BookMoveUtils::migrateMovedEpubState(srcPath, dstPath, oldCachePath, title, author,
                                       !SETTINGS.removeReadBooksFromRecents);
}

}  // namespace

EpubReaderActivity::BookReaderSettingsData EpubReaderActivity::readBookReaderSettings(const Epub& epub) {
  return loadBookReaderSettingsFile(epub.getCachePath());
}

uint8_t EpubReaderActivity::loadBookRenderMode(const std::string& filePath) {
  Epub epub(filePath, "/.crosspoint");
  epub.setupCacheDir();
  const BookReaderSettingsData data = loadBookReaderSettingsFile(epub.getCachePath());
  return data.hasRenderModeOverride ? normalizeRenderModeRaw(data.renderMode)
                                    : static_cast<uint8_t>(EpubRenderMode::CrossInkDefault);
}

bool EpubReaderActivity::saveBookRenderMode(const std::string& filePath, const uint8_t renderMode) {
  Epub epub(filePath, "/.crosspoint");
  epub.setupCacheDir();
  return saveBookRenderModeForCache(epub.getCachePath(), renderMode);
}

bool EpubReaderActivity::resetBookReaderSettings(const std::string& filePath) {
  Epub epub(filePath, "/.crosspoint");
  const std::string settingsPath = epub.getCachePath() + READER_SETTINGS_FILE_NAME;
  if (!Storage.exists(settingsPath.c_str())) {
    return true;
  }
  if (!Storage.remove(settingsPath.c_str())) {
    LOG_ERR("ERS", "Failed to reset reader settings: %s", settingsPath.c_str());
    return false;
  }
  LOG_INF("ERS", "Reset reader settings: %s", settingsPath.c_str());
  return true;
}

float EpubReaderActivity::getCurrentBookProgressPercent() const {
  const int totalPages = section ? section->estimatedTotalPages() : 0;
  if (activeFootnotePreview || !epub || !section || totalPages == 0 || epub->getBookSize() == 0) {
    return 0.0f;
  }

  const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(totalPages);
  return epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
}

void EpubReaderActivity::pauseReadingPaceTimer(const char* reason) {
  if (!activeFootnotePreview) {
    recordCurrentPageReadingTime(reason);
  }
  pageShownAtMs = 0UL;
  paceSampleWarmupPending = true;
}

void EpubReaderActivity::resumeReadingPaceTimer(const char*) {
  if (activeFootnotePreview) {
    pageShownAtMs = 0UL;
    return;
  }
  if (section && section->pageCount > 0 && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    pageShownAtMs = millis();
  } else {
    pageShownAtMs = 0UL;
  }
}

void EpubReaderActivity::onInputLockChanged(const bool locked) {
  if (locked) {
    pauseReadingPaceTimer("quick_lock");
  } else {
    resumeReadingPaceTimer("quick_lock");
  }
}

bool EpubReaderActivity::handleQuickLockUnlock(const QuickLockTrigger trigger) {
  if (trigger == QuickLockTrigger::LongMenu) {
    if (longPressMenuHandled) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
          !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
        longPressMenuHandled = false;
      }
      return true;
    }
    if (SETTINGS.longPressMenuAction == CrossPointSettings::LONG_MENU_QUICK_LOCK &&
        mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= longPressMenuMs) {
      longPressMenuHandled = true;
      handleGlobalPowerButtonAction(CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK, QuickLockTrigger::LongMenu);
      return true;
    }
    return false;
  }

  if (trigger == QuickLockTrigger::LongBack) {
    if (longPressBackHandled) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          !mappedInput.isPressed(MappedInputManager::Button::Back)) {
        longPressBackHandled = false;
      }
      return true;
    }
    if (SETTINGS.longPressBackAction == CrossPointSettings::LONG_MENU_QUICK_LOCK &&
        mappedInput.isPressed(MappedInputManager::Button::Back) &&
        mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
      longPressBackHandled = true;
      mappedInput.suppressNextBackRelease();
      handleGlobalPowerButtonAction(CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK, QuickLockTrigger::LongBack);
      return true;
    }
    return false;
  }

  return false;
}

void EpubReaderActivity::armReadingPaceWarmup(const char*) { paceSampleWarmupPending = true; }

bool EpubReaderActivity::forwardPageReadElapsed(uint32_t& seconds, const char*) const {
  seconds = 0;
  if (activeFootnotePreview || !SETTINGS.shouldTrackReadingStats() || pageShownAtMs == 0UL) {
    return false;
  }

  const unsigned long elapsedMs = millis() - pageShownAtMs;
  if (elapsedMs < MIN_READING_STATS_PAGE_MS) {
    return false;
  }

  seconds = static_cast<uint32_t>(elapsedMs / 1000UL);
  return true;
}

bool EpubReaderActivity::currentPageReadingSecondsForStats(uint32_t& seconds, const char* source) const {
  seconds = 0;
  if (activeFootnotePreview || !SETTINGS.shouldTrackReadingStats() || pageShownAtMs == 0UL) {
    return false;
  }

  const unsigned long elapsedMs = millis() - pageShownAtMs;
  const uint32_t elapsedSeconds = static_cast<uint32_t>(elapsedMs / 1000UL);
  if (elapsedSeconds == 0) {
    return false;
  }

  const uint32_t thresholdSeconds = SETTINGS.getReadingIdleTimeThresholdSeconds();
  if (elapsedSeconds > thresholdSeconds) {
    return false;
  }

  seconds = elapsedSeconds;
  return true;
}

void EpubReaderActivity::recordCurrentPageReadingTime(const char* source) {
  if (activeFootnotePreview) {
    pageShownAtMs = 0UL;
    return;
  }
  uint32_t seconds = 0;
  if (currentPageReadingSecondsForStats(seconds, source)) {
    sessionReadingSeconds = sessionReadingSeconds > UINT32_MAX - seconds ? UINT32_MAX : sessionReadingSeconds + seconds;
  }
  pageShownAtMs = 0UL;
}

void EpubReaderActivity::recordForwardPagePaceSample(uint32_t seconds, const char* source) {
  if (paceSampleWarmupPending) {
    paceSampleWarmupPending = false;
    return;
  }

  if (seconds < MIN_READING_PACE_SAMPLE_SECONDS) {
    return;
  }

  const uint32_t maxReadingPaceSampleSeconds = SETTINGS.getReadingIdleTimeThresholdSeconds();
  if (seconds > maxReadingPaceSampleSeconds) {
    return;
  }

  if (sessionPaceSampleCount < UINT16_MAX && sessionPaceSampleSeconds <= UINT32_MAX - static_cast<uint32_t>(seconds)) {
    sessionPaceSampleSeconds += seconds;
    sessionPaceSampleCount++;
  }

  stats.recordForwardPageRead(seconds);
  recoverStoredPaceFromSession("pace_sample");
}

bool EpubReaderActivity::getSessionAveragePaceSeconds(uint16_t& avgSeconds) const {
  avgSeconds = 0;
  if (sessionPaceSampleCount < MIN_SESSION_TIME_LEFT_PACE_SAMPLE_COUNT || sessionPaceSampleSeconds == 0) {
    return false;
  }
  const uint32_t roundedAvg =
      (sessionPaceSampleSeconds + static_cast<uint32_t>(sessionPaceSampleCount / 2)) / sessionPaceSampleCount;
  avgSeconds = static_cast<uint16_t>(std::min<uint32_t>(roundedAvg, UINT16_MAX));
  return avgSeconds > 0;
}

void EpubReaderActivity::recoverStoredPaceFromSession(const char* reason) {
  if (stats.avgSecondsPerForwardPage == 0) {
    return;
  }

  uint16_t sessionAvg = 0;
  if (!getSessionAveragePaceSeconds(sessionAvg)) {
    return;
  }

  const uint32_t slowerRecoveryThreshold =
      (static_cast<uint32_t>(stats.avgSecondsPerForwardPage) * STORED_PACE_SLOWER_RECOVERY_PERCENT + 99U) / 100U;
  if (sessionPaceSampleCount >= MIN_STORED_PACE_SLOWER_RECOVERY_SESSION_SAMPLES &&
      sessionAvg >= slowerRecoveryThreshold) {
    LOG_DBG("ERS",
            "Time-left stored pace recovered: reason=%s direction=slower avg=%u->%u samples=%u sessionSamples=%u "
            "threshold=%lu",
            reason ? reason : "unknown", stats.avgSecondsPerForwardPage, sessionAvg, stats.paceSampleCount,
            sessionPaceSampleCount, static_cast<unsigned long>(slowerRecoveryThreshold));
    stats.avgSecondsPerForwardPage = sessionAvg;
    return;
  }

  const uint32_t fasterRecoveryThreshold =
      (static_cast<uint32_t>(stats.avgSecondsPerForwardPage) * STORED_PACE_FASTER_RECOVERY_PERCENT) / 100U;
  if (sessionPaceSampleCount >= MIN_STORED_PACE_FASTER_RECOVERY_SESSION_SAMPLES &&
      sessionAvg <= fasterRecoveryThreshold) {
    LOG_DBG("ERS",
            "Time-left stored pace recovered: reason=%s direction=faster avg=%u->%u samples=%u sessionSamples=%u "
            "threshold=%lu",
            reason ? reason : "unknown", stats.avgSecondsPerForwardPage, sessionAvg, stats.paceSampleCount,
            sessionPaceSampleCount, static_cast<unsigned long>(fasterRecoveryThreshold));
    stats.avgSecondsPerForwardPage = sessionAvg;
  }
}

bool EpubReaderActivity::getTimeLeftPaceSeconds(uint16_t& avgSeconds, const char*& source,
                                                uint16_t& sampleCount) const {
  if (getSessionAveragePaceSeconds(avgSeconds)) {
    source = "session_pace";
    sampleCount = sessionPaceSampleCount;
    return true;
  }
  if (hasEnoughPaceSamplesForTimeLeft(stats)) {
    avgSeconds = stats.avgSecondsPerForwardPage;
    source = "stored_pace";
    sampleCount = stats.paceSampleCount;
    return true;
  }
  avgSeconds = 0;
  source = "none";
  sampleCount = 0;
  return false;
}

bool EpubReaderActivity::estimateRemainingTimeLeftPages(const bool bookEstimate, float& remainingPages) const {
  remainingPages = 0.0f;
  const int totalPages = section ? section->estimatedTotalPages() : 0;
  if (!epub || !section || totalPages == 0) {
    return false;
  }

  if (!bookEstimate) {
    int groupedCurrentPage = section->currentPage + 1;
    int groupedPageCount = totalPages;
    float groupedProgress = 0.0f;
    bool groupedEstimated = false;
    resolveChapterGroupPageProgress(groupedCurrentPage, groupedPageCount, groupedProgress, groupedEstimated);
    const int remainingChapterPages = groupedPageCount - groupedCurrentPage;
    if (remainingChapterPages <= 0) {
      return false;
    }
    remainingPages = static_cast<float>(remainingChapterPages);
  } else {
    const size_t bookSize = epub->getBookSize();
    if (bookSize == 0) {
      return false;
    }

    const size_t prevChapterSize = currentSpineIndex >= 1 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
    const size_t cumulativeSize = epub->getCumulativeSpineItemSize(currentSpineIndex);
    if (cumulativeSize <= prevChapterSize) {
      return false;
    }

    const float chapterSize = static_cast<float>(cumulativeSize - prevChapterSize);
    const float completedCurrentChapter =
        (static_cast<float>(section->currentPage) / static_cast<float>(totalPages)) * chapterSize;
    const float completedBookSize = static_cast<float>(prevChapterSize) + completedCurrentChapter;
    if (completedBookSize >= static_cast<float>(bookSize)) {
      return false;
    }

    const float bytesPerPage = chapterSize / static_cast<float>(totalPages);
    if (bytesPerPage <= 0.0f) {
      return false;
    }
    remainingPages = (static_cast<float>(bookSize) - completedBookSize) / bytesPerPage;
  }

  return remainingPages > 0.0f;
}

bool EpubReaderActivity::estimateProgressTimeLeftSeconds(uint32_t& seconds) const {
  seconds = 0;
  const int totalPages = section ? section->estimatedTotalPages() : 0;
  if (!epub || !section || totalPages == 0 || epub->getBookSize() == 0) {
    return false;
  }
  const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(totalPages);
  const float progressPercent = epub->calculateSizeProgress(currentSpineIndex, chapterProgress) * 100.0f;
  uint32_t currentPageSeconds = 0;
  uint32_t sessionSeconds = sessionReadingSeconds;
  if (SETTINGS.shouldTrackReadingStats() &&
      currentPageReadingSecondsForStats(currentPageSeconds, "time_left_preview")) {
    sessionSeconds =
        sessionSeconds > UINT32_MAX - currentPageSeconds ? UINT32_MAX : sessionSeconds + currentPageSeconds;
  }
  const uint32_t elapsedReadingSeconds =
      stats.totalReadingSeconds > UINT32_MAX - sessionSeconds ? UINT32_MAX : stats.totalReadingSeconds + sessionSeconds;

  if (progressPercent <= 0.0f || progressPercent >= 100.0f || elapsedReadingSeconds < 120) {
    return false;
  }

  const float progress = progressPercent / 100.0f;
  const float estimate = (static_cast<float>(elapsedReadingSeconds) * (1.0f - progress)) / progress;
  if (estimate <= 0.0f) {
    return false;
  }

  seconds = static_cast<uint32_t>(std::min(estimate + 0.5f, static_cast<float>(UINT32_MAX)));
  return seconds > 0;
}

bool EpubReaderActivity::estimateTimeLeftSeconds(const bool bookEstimate, uint32_t& seconds) const {
  seconds = 0;
  uint16_t paceSeconds = 0;
  const char* paceSource = "none";
  uint16_t paceSampleCount = 0;
  const bool hasPace = getTimeLeftPaceSeconds(paceSeconds, paceSource, paceSampleCount);

  uint32_t paceEstimateSeconds = 0;
  bool hasPaceEstimate = false;
  float remainingPages = 0.0f;
  if (hasPace && estimateRemainingTimeLeftPages(bookEstimate, remainingPages)) {
    const float estimatedSeconds = remainingPages * static_cast<float>(paceSeconds);
    if (estimatedSeconds > 0.0f) {
      paceEstimateSeconds = static_cast<uint32_t>(std::min(estimatedSeconds + 0.5f, static_cast<float>(UINT32_MAX)));
      hasPaceEstimate = paceEstimateSeconds > 0;
    }
  }

  uint32_t progressEstimateSeconds = 0;
  bool hasProgressEstimate = false;
  if (bookEstimate && hasPace) {
    hasProgressEstimate = estimateProgressTimeLeftSeconds(progressEstimateSeconds);
  }
  if (!hasPaceEstimate && !hasProgressEstimate) {
    return false;
  }

  if (!hasPaceEstimate) {
    seconds = progressEstimateSeconds;
  } else if (hasProgressEstimate) {
    const uint32_t progressFloorSeconds = static_cast<uint32_t>(std::min<uint64_t>(
        (static_cast<uint64_t>(progressEstimateSeconds) * BOOK_PROGRESS_ESTIMATE_FLOOR_PERCENT + 99ULL) / 100ULL,
        UINT32_MAX));
    if (paceEstimateSeconds < progressFloorSeconds) {
      seconds = progressFloorSeconds;
    } else {
      seconds = paceEstimateSeconds;
    }
  } else {
    seconds = paceEstimateSeconds;
  }
  return seconds > 0;
}

bool EpubReaderActivity::formatTimeLeftLabel(char* buf, const size_t len) const {
  if (!buf || len == 0 || SETTINGS.statusBarTimeLeft == CrossPointSettings::STATUS_BAR_TIME_LEFT::TIME_LEFT_HIDE) {
    return false;
  }

  const bool bookEstimate = SETTINGS.statusBarTimeLeft == CrossPointSettings::STATUS_BAR_TIME_LEFT::TIME_LEFT_BOOK;
  uint32_t seconds = 0;
  if (estimateTimeLeftSeconds(bookEstimate, seconds)) {
    formatCompactReadingDuration(seconds, buf, len);
    return true;
  }

  uint16_t paceSeconds = 0;
  const char* paceSource = "none";
  uint16_t paceSampleCount = 0;
  if (!getTimeLeftPaceSeconds(paceSeconds, paceSource, paceSampleCount)) {
    float remainingPages = 0.0f;
    if (!estimateRemainingTimeLeftPages(bookEstimate, remainingPages)) {
      return false;
    }
    snprintf(buf, len, "%s", tr(STR_TIME_LEFT_CALCULATING));
    return true;
  }

  return false;
}

void EpubReaderActivity::refreshCachedTimeLeftEstimate() {
  uint32_t seconds = 0;
  stats.estimatedTimeLeftSeconds = (!stats.isCompleted && estimateTimeLeftSeconds(true, seconds)) ? seconds : 0;
}

// The stats screen can edit dates/completion using a preview copy that includes
// current-session time, so keep live counters in memory and import only edits.
void EpubReaderActivity::applyBookStatsEditsFromDisk() {
  if (epub) {
    const BookReadingStats diskStats = BookReadingStats::load(epub->getCachePath());
    stats.isCompleted = diskStats.isCompleted;
    stats.startDateManual = diskStats.startDateManual;
    stats.finishedDateManual = diskStats.finishedDateManual;
    stats.startDate = diskStats.startDate;
    stats.finishedDate = diskStats.finishedDate;
  }

  const GlobalReadingStats diskGlobalStats = GlobalReadingStats::load();
  globalStats.completedBooks = diskGlobalStats.completedBooks;
}

void EpubReaderActivity::handleBookStatsReturn(const bool returnToReaderMenu) {
  applyBookStatsEditsFromDisk();
  completionPromptShown = stats.isCompleted;
  if (stats.isCompleted && SETTINGS.moveFinishedToReadFolder && epub && !isInReadFolder(epub->getPath())) {
    pendingReadFolderMove = true;
  } else if (!stats.isCompleted) {
    pendingReadFolderMove = false;
  }
  resumeReadingPaceTimer("book_stats_return");
  if (returnToReaderMenu) {
    openReaderMenu();
  } else {
    requestUpdate();
  }
}

void EpubReaderActivity::initializeCompletionPromptTrigger() {
  completionTriggerSpineIndex = -1;
  completionTriggerSpineProgress = 1.0f;
  completionPromptQueued = false;
  completionPromptShown = stats.isCompleted;
  completionTriggerSeenBelow = false;
  completionTriggerCrossed = false;
  lastAtOrPastCompletionTrigger = false;

  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  const int spineCount = epub->getSpineItemsCount();
  if (bookSize == 0 || spineCount <= 0) {
    return;
  }

  int locationSpineIndex = 0;
  float locationSpineProgress = 0.0f;
  if (epub->resolveLocationPercentToSpineProgress(99, locationSpineIndex, locationSpineProgress)) {
    completionTriggerSpineIndex = locationSpineIndex;
    completionTriggerSpineProgress = locationSpineProgress;
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
  const int totalPages = section ? section->estimatedTotalPages() : 0;
  if (!epub || !section || totalPages == 0 || completionTriggerSpineIndex < 0) {
    return false;
  }

  if (currentSpineIndex > completionTriggerSpineIndex) {
    return true;
  }
  if (currentSpineIndex < completionTriggerSpineIndex) {
    return false;
  }

  const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(totalPages);
  return chapterProgress >= completionTriggerSpineProgress;
}

bool EpubReaderActivity::shouldQueueCompletionPromptOnChapterExit() const {
  if (completionPromptShown || completionPromptQueued || stats.isCompleted || footnoteDepth > 0 ||
      !completionTriggerCrossed || !epub || !section || section->pageCount == 0 || completionTriggerSpineIndex < 0 ||
      section->isBuilding() || section->isPartial()) {
    return false;
  }

  if (currentSpineIndex != completionTriggerSpineIndex) {
    return false;
  }

  return section->currentPage >= section->pageCount - 1;
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
    completionTriggerCrossed = true;
  }

  lastAtOrPastCompletionTrigger = atOrPastTrigger;
}

void EpubReaderActivity::captureGlobalReaderSettings() {
  captureReaderSettings(globalReaderSettingsBeforeBook);
  restoreGlobalReaderSettingsOnExit = true;
}

void EpubReaderActivity::restoreGlobalReaderSettings() {
  if (!restoreGlobalReaderSettingsOnExit) {
    return;
  }
  applyReaderSettings(globalReaderSettingsBeforeBook);
  restoreGlobalReaderSettingsOnExit = false;
}

void EpubReaderActivity::loadBookReaderSettings() {
  bookHasCustomReaderSettings = false;
  bookHasAutoPageTurnInterval = false;
  lastAutoPageTurnIntervalSeconds = DEFAULT_AUTO_PAGE_TURN_INTERVAL_S;

  if (!epub) {
    return;
  }

  const auto& data = initialBookReaderSettings;
  bookHasCustomReaderSettings = data.hasCustomReaderSettings;
  bookHasAutoPageTurnInterval = data.hasAutoPageTurnInterval;
  bookHasRenderModeOverride = data.hasRenderModeOverride;
  lastAutoPageTurnIntervalSeconds =
      data.hasAutoPageTurnInterval ? data.autoPageTurnSeconds : DEFAULT_AUTO_PAGE_TURN_INTERVAL_S;
  if (data.hasCustomReaderSettings) {
    applyReaderSettings(data.readerSettings);
  }
  SETTINGS.epubRenderMode = data.hasRenderModeOverride ? normalizeRenderModeRaw(data.renderMode)
                                                       : static_cast<uint8_t>(EpubRenderMode::CrossInkDefault);
}

void EpubReaderActivity::saveCurrentBookReaderSettings() {
  if (!epub) {
    return;
  }

  if (section && section->isBuilding()) {
    // The incremental parser and section writer keep SD handles open. Release
    // them before reader_settings.bin is read/written; the build reopens both
    // lazily on its next chunk.
    section->releaseBuildFile();
  }

  BookReaderSettingsData data = loadBookReaderSettingsFile(epub->getCachePath());
  captureReaderSettings(data.readerSettings);
  bookHasCustomReaderSettings = true;
  bookHasRenderModeOverride = true;
  initialBookReaderSettings.hasCustomReaderSettings = true;
  initialBookReaderSettings.hasAutoPageTurnInterval = bookHasAutoPageTurnInterval;
  initialBookReaderSettings.autoPageTurnSeconds = lastAutoPageTurnIntervalSeconds;
  initialBookReaderSettings.hasRenderModeOverride = true;
  initialBookReaderSettings.renderMode = SETTINGS.epubRenderMode;
  data.hasCustomReaderSettings = true;
  data.hasAutoPageTurnInterval = bookHasAutoPageTurnInterval;
  data.autoPageTurnSeconds = lastAutoPageTurnIntervalSeconds;
  data.hasRenderModeOverride = true;
  data.renderMode = SETTINGS.epubRenderMode;
  saveBookReaderSettingsFile(epub->getCachePath(), data);
}

void EpubReaderActivity::saveDictionaryFontForBook(const char* familyName, const uint8_t pointSize) {
  if (!epub) return;

  if (section && section->isBuilding()) {
    section->releaseBuildFile();
  }

  BookReaderSettingsData data = loadBookReaderSettingsFile(epub->getCachePath());
  if (familyName && familyName[0] != '\0') {
    std::strncpy(data.dictionarySdFontFamilyName, familyName, sizeof(data.dictionarySdFontFamilyName) - 1);
    data.dictionarySdFontFamilyName[sizeof(data.dictionarySdFontFamilyName) - 1] = '\0';
    data.hasDictionaryFontOverride = true;
  } else {
    data.dictionarySdFontFamilyName[0] = '\0';
    data.hasDictionaryFontOverride = false;
    data.dictionaryFontPointSize = 0;
  }
  if (data.hasDictionaryFontOverride) data.dictionaryFontPointSize = pointSize;
  saveBookReaderSettingsFile(epub->getCachePath(), data);
}

void EpubReaderActivity::saveGlobalSettingsPreservingBookOverrides() {
  if (!restoreGlobalReaderSettingsOnExit) {
    SETTINGS.saveToFile();
    return;
  }

  ReaderSettingsSnapshot activeReaderSettings;
  captureReaderSettings(activeReaderSettings);
  applyReaderSettings(globalReaderSettingsBeforeBook);
  SETTINGS.saveToFile();
  applyReaderSettings(activeReaderSettings);
}

void EpubReaderActivity::beginGlobalSettingsEdit() {
  if (bookReaderSettingsSuspendedForGlobalEdit || !restoreGlobalReaderSettingsOnExit) {
    return;
  }
  captureReaderSettings(suspendedBookReaderSettings);
  applyReaderSettings(globalReaderSettingsBeforeBook);
  bookReaderSettingsSuspendedForGlobalEdit = true;
}

void EpubReaderActivity::endGlobalSettingsEdit() {
  if (!bookReaderSettingsSuspendedForGlobalEdit) {
    return;
  }

  // Global Settings is editing SETTINGS while the book-specific reader values
  // are suspended. Retain every edited reader default before restoring this
  // book, otherwise the stale snapshot is written back on a later save or
  // when the reader exits.
  captureReaderSettings(globalReaderSettingsBeforeBook);
  applyReaderSettings(suspendedBookReaderSettings);
  bookReaderSettingsSuspendedForGlobalEdit = false;
}

void EpubReaderActivity::saveReaderOptionsForBook(void* ctx) {
  if (!ctx) {
    return;
  }
  static_cast<EpubReaderActivity*>(ctx)->saveCurrentBookReaderSettings();
}

void EpubReaderActivity::saveDictionaryFontForBookReader(void* ctx, const char* familyName, const uint8_t pointSize) {
  if (!ctx) return;
  static_cast<EpubReaderActivity*>(ctx)->saveDictionaryFontForBook(familyName, pointSize);
}

void EpubReaderActivity::saveGlobalSettingsForBookReader(void* ctx) {
  if (!ctx) {
    return;
  }
  static_cast<EpubReaderActivity*>(ctx)->saveGlobalSettingsPreservingBookOverrides();
}

void EpubReaderActivity::beginGlobalSettingsEditForBookReader(void* ctx) {
  if (!ctx) {
    return;
  }
  static_cast<EpubReaderActivity*>(ctx)->beginGlobalSettingsEdit();
}

void EpubReaderActivity::endGlobalSettingsEditForBookReader(void* ctx) {
  if (!ctx) {
    return;
  }
  static_cast<EpubReaderActivity*>(ctx)->endGlobalSettingsEdit();
}

void EpubReaderActivity::onEnter() {
  Activity::onEnter();
  pageLoadRetryCount = 0;

  MemoryBudget::logEpubHeapPools("reader enter");

  // epub is a required collaborator: ReaderActivity dereferences it before handing it
  // over, and onExit() unconditionally tears down the setup below. Returning early here
  // instead would leave reader mode and the bookmark/clipping stores unbalanced.
  captureGlobalReaderSettings();
  epub->setupCacheDir();
  loadBookReaderSettings();
  ensureReaderSdFontLoaded(renderer);
  ImageBlock::clearSessionRenderFailures();
  ImageBlock::setExtractor(epub.get(), [](void* context, const char* source, const char* destination) {
    return static_cast<Epub*>(context)->extractItemToFile(source, destination);
  });

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Activate reader-specific front button mapping (if configured).
  mappedInput.setReaderMode(true);

  BOOKMARKS.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), "epub");
  CLIPPINGS.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), "epub");

  uint16_t restartPageBuildSpine = UINT16_MAX;
  uint16_t restartPageBuildTarget = 0;
  bool resumeAutoPageTurnAfterRestart = false;
  const bool resumePageBuildAfterRestart = consumeSilentRestartReaderPageBuild(
      epub->getPath(), restartPageBuildSpine, restartPageBuildTarget, resumeAutoPageTurnAfterRestart);

  if (APP_STATE.pendingBookmarkSpine != UINT16_MAX && APP_STATE.pendingBookmarkProgress >= 0.0f) {
    // Resume from a bookmark selected on the Home screen
    currentSpineIndex = APP_STATE.pendingBookmarkSpine;
    pendingSpineProgress = APP_STATE.pendingBookmarkProgress;
    pendingParagraphIndex = APP_STATE.pendingBookmarkParagraphIndex;
    pendingClippingIndex = APP_STATE.pendingClippingIndex;
    pendingPercentJump = true;
    cachedSpineIndex = currentSpineIndex;

    // Clear the pending jump
    APP_STATE.pendingBookmarkSpine = UINT16_MAX;
    APP_STATE.pendingBookmarkProgress = -1.0f;
    APP_STATE.pendingBookmarkParagraphIndex = UINT16_MAX;
    APP_STATE.pendingClippingIndex = UINT16_MAX;
    APP_STATE.saveToFile();
  } else {
    EpubReaderUtils::Progress progress;
    if (EpubReaderUtils::loadProgress(*epub, progress)) {
      currentSpineIndex = progress.spineIndex;
      nextPageNumber = progress.pageNumber;
      cachedSpineIndex = currentSpineIndex;
      if (progress.hasPageCount) {
        cachedChapterPageNumber = progress.pageNumber;
        cachedChapterTotalPageCount = progress.pageCount;
      }
      if (progress.hasVisibleTextOffset) {
        cachedVisibleTextOffset = progress.visibleTextOffset;
        // A content position may come from another device whose page count does not
        // match this layout.  Let the section builder resolve it before showing a page.
        pendingRelayoutReposition = true;
      }
    }
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex == 0 && !pendingPercentJump) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
    }
  }

  if (resumePageBuildAfterRestart) {
    if (static_cast<int>(restartPageBuildSpine) < epub->getSpineItemsCount()) {
      currentSpineIndex = restartPageBuildSpine;
      nextPageNumber = restartPageBuildTarget;
      cachedSpineIndex = currentSpineIndex;
      pendingPageJump = restartPageBuildTarget;
      pendingPercentJump = false;
      pendingParagraphIndex = UINT16_MAX;
      pendingClippingIndex = UINT16_MAX;
      lowMemoryPartialRestartAttempted = true;
      LOG_INF("ERS", "Resuming low-memory partial build after silent restart: spine=%u target=%u",
              restartPageBuildSpine, restartPageBuildTarget);
      if (resumeAutoPageTurnAfterRestart) {
        const uint16_t seconds = getAutoPageTurnIntervalSeconds();
        lastPageTurnTime = millis();
        pageTurnDuration = static_cast<unsigned long>(seconds) * 1000UL;
        automaticPageTurnActive = true;
        LOG_INF("ERS", "Restored Auto Page Turn after silent restart: interval=%u", seconds);
      }
    } else {
      LOG_ERR("ERS", "Ignoring invalid low-memory partial restart target: spine=%u target=%u", restartPageBuildSpine,
              restartPageBuildTarget);
    }
  }

  // Load reading stats and record session start time.
  // Session count and reading time are committed on exit once thresholds are met.
  stats = BookReadingStats::load(epub->getCachePath());
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  const uint32_t cumulativeAvgSeconds =
      stats.totalPagesTurned > 0 ? stats.totalReadingSeconds / stats.totalPagesTurned : 0;
  LOG_DBG("ERS",
          "Reading stats loaded: totalReadingSeconds=%lu totalPagesTurned=%lu avg=%u samples=%u cumulativeAvg=%lu",
          static_cast<unsigned long>(stats.totalReadingSeconds), static_cast<unsigned long>(stats.totalPagesTurned),
          stats.avgSecondsPerForwardPage, stats.paceSampleCount, static_cast<unsigned long>(cumulativeAvgSeconds));
#endif
  armReadingPaceWarmup("reader_open");
  sessionReadingSeconds = 0;
  hasSessionStartLocalDateTime = getCurrentLocalReadingStatsDateTime(sessionStartLocalDateTime);

  globalStats = GlobalReadingStats::load();

  initializeCompletionPromptTrigger();

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  const RecentBook::CoverState coverState =
      epub->hasCoverImage() ? RecentBook::CoverState::Unknown : RecentBook::CoverState::Missing;
  if (!skipRecentBookUpdateOnEntry) {
    RECENT_BOOKS.addOrUpdateBook(epub->getPath(), epub->getTitle(), epub->getAuthor(),
                                 coverState == RecentBook::CoverState::Missing ? "" : epub->getThumbBmpPath(),
                                 coverState);
  }

  // Trigger first update
  requestUpdate();
}

void EpubReaderActivity::onExit() {
  mappedInput.setReaderTouchscreenOverride(false);

  // The extraction callback holds the Epub as a raw context pointer.
  ImageBlock::setExtractor(nullptr, nullptr);
  releaseGrayscaleStripScratch(true);
  ImageBlock::releaseSessionPixelCache();

  // SD-font caches live in the renderer singleton, so leaving them resident after
  // the reader exits can fragment the contiguous heap needed for Home cover images.
  releaseReaderSdFontCachesForLowMemory(renderer, "ERS", "reader exit");
  MemoryBudget::logEpubHeapPools("reader exit after caches");
  Activity::onExit();

  // Deactivate reader-specific front button mapping.
  mappedInput.setReaderMode(false);

  if (footnoteDepth == 0 && !flushQueuedProgress()) {
    LOG_ERR("ERS", "Failed to flush debounced reader progress on exit");
  }

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  if (SETTINGS.shouldTrackReadingStats()) {
    recordCurrentPageReadingTime("reader_exit");

    // Commit session stats based on active reading time. Page intervals longer
    // than the idle threshold are rejected before they reach sessionReadingSeconds.
    // Sessions under 1 minute don't count toward session count or reading time.
    // Sessions under 10 seconds don't add to reading time.
    const uint32_t elapsedSecs = sessionReadingSeconds;
    if (elapsedSecs >= 60) {
      stats.sessionCount++;
      globalStats.totalSessions++;
    }
    if (elapsedSecs >= 10) {
      stats.totalReadingSeconds += elapsedSecs;
      globalStats.totalReadingSeconds += elapsedSecs;
      if (hasSessionStartLocalDateTime) {
        stats.recordReadingSpan(sessionStartLocalDateTime, elapsedSecs);
        globalStats.recordReadingSpan(sessionStartLocalDateTime, elapsedSecs);
      }
      if (elapsedSecs >= 120 && !stats.startDateManual && !stats.startDate.isValid() && hasSessionStartLocalDateTime) {
        stats.startDate = sessionStartLocalDateTime.date;
      }
    }
    if (epub) {
      recoverStoredPaceFromSession("reader_exit");
      refreshCachedTimeLeftEstimate();
      stats.save(epub->getCachePath());
    }
    globalStats.save();
  }

  // Leaving mid-footnote loses the in-RAM return stack on deep sleep; persist the
  // pre-footnote position so the book reopens at the link origin, not the footnote.
  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    // Record the origin chapter's real page count. A zero reads back as a valid
    // record with hasPageCount set, which breaks the percent math on reopen.
    // A preview section describes the note rather than the origin chapter, so its page
    // count is not usable here; fall back to the last count persisted for that spine.
    int originPageCount = 0;
    if (!activeFootnotePreview && section && origin.spineIndex == currentSpineIndex) {
      originPageCount = section->estimatedTotalPages();
    } else if (lastSavedSpineIndex == origin.spineIndex) {
      originPageCount = std::max(0, lastSavedPageCount);
    }
    // Forced past the footnote-preview suppression: this origin position is exactly what
    // that suppression protects, so it is the one save that must go through.
    saveProgress(origin.spineIndex, origin.pageNumber, originPageCount, /*allowDuringFootnotePreview=*/true);
  }

  BOOKMARKS.unload();
  CLIPPINGS.unload();
  section.reset();

  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string title = epub->getTitle();
    const std::string author = epub->getAuthor();
    const std::string dstPath = BookMoveUtils::buildReadFolderDestination(srcPath);
    epub.reset();  // release the Epub (and any open handles) before renaming on the SD card
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath, title, author);
  } else {
    epub.reset();
  }

  restoreGlobalReaderSettings();
}

void EpubReaderActivity::openReaderMenu() {
  int currentPage = 0;
  int totalPages = 0;
  float bookProgress = 0.0f;
  uint16_t bmSpine;
  float bmProgress = 0.0f;
  int bookmarkPageCount = 1;
  bool isBookCompleted;
  bool previewActive = false;
  {
    // Serialize EPUB metadata/file access with the render task.
    RenderLock lock(*this);
    previewActive = activeFootnotePreview;
    currentPage = section ? section->currentPage + 1 : 0;
    totalPages = section ? section->estimatedTotalPages() : 0;
    bmSpine = static_cast<uint16_t>(currentSpineIndex);
    bmProgress = (section && totalPages > 0) ? static_cast<float>(section->currentPage) / totalPages : 0.0f;
    bookmarkPageCount = totalPages > 0 ? totalPages : 1;
    isBookCompleted = stats.isCompleted;
    bookProgress = getCurrentBookProgressPercent();
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));

  pauseReadingPaceTimer("reader_menu");
  const BookReaderSettingsData bookSettings = loadBookReaderSettingsFile(epub->getCachePath());
  std::unique_ptr<Activity> menuActivity;
#if CROSSINK_APP_CAP_TOUCH
  if (mappedInput.hasTouchHardware()) {
    menuActivity = makeUniqueNoThrow<EpubReaderTouchMenuActivity>(
        renderer, mappedInput, epub, touchReaderPreviewModel.get(), bookProgressPercent,
        !previewActive && !currentPageFootnotes.empty(),
        !previewActive && epub && Dictionary::exists(epub->getCachePath().c_str()), !BOOKMARKS.getBookmarks().empty(),
        CLIPPINGS.hasClippings(),
        !previewActive && BOOKMARKS.hasBookmarkForPage(bmSpine, bmProgress, bookmarkPageCount), isBookCompleted,
        SETTINGS.statusBarTimeLeft != CrossPointSettings::STATUS_BAR_TIME_LEFT::TIME_LEFT_HIDE,
        !previewActive && epub && epub->hasStablePageNumbers(), getAutoPageTurnIntervalSeconds(),
        automaticPageTurnActive, saveReaderOptionsForBook, this, saveGlobalSettingsForBookReader, this,
        beginGlobalSettingsEditForBookReader, this, endGlobalSettingsEditForBookReader, this,
        bookSettings.dictionarySdFontFamilyName, bookSettings.dictionaryFontPointSize,
        bookSettings.hasDictionaryFontOverride, saveDictionaryFontForBookReader, this, touchReaderDrawerState);
    if (!menuActivity) {
      LOG_ERR("ERS", "Could not allocate touch reader menu");
      resumeReadingPaceTimer("reader_menu_oom");
      requestUpdate();
      return;
    }
  }
#endif
  if (!menuActivity) {
    menuActivity = makeUniqueNoThrow<EpubReaderMenuActivity>(
        renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent, SETTINGS.orientation,
        !previewActive && !currentPageFootnotes.empty(),
        !previewActive && epub && Dictionary::exists(epub->getCachePath().c_str()), !BOOKMARKS.getBookmarks().empty(),
        CLIPPINGS.hasClippings(),
        !previewActive && BOOKMARKS.hasBookmarkForPage(bmSpine, bmProgress, bookmarkPageCount), isBookCompleted,
        automaticPageTurnActive, getAutoPageTurnIntervalSeconds(),
        SETTINGS.statusBarTimeLeft != CrossPointSettings::STATUS_BAR_TIME_LEFT::TIME_LEFT_HIDE,
        saveReaderOptionsForBook, this, saveGlobalSettingsForBookReader, this, beginGlobalSettingsEditForBookReader,
        this, !previewActive && epub && epub->hasStablePageNumbers(), endGlobalSettingsEditForBookReader, this,
        bookSettings.dictionarySdFontFamilyName, bookSettings.dictionaryFontPointSize,
        bookSettings.hasDictionaryFontOverride, saveDictionaryFontForBookReader, this);
    if (!menuActivity) {
      LOG_ERR("ERS", "Could not allocate reader menu");
      resumeReadingPaceTimer("reader_menu_oom");
      requestUpdate();
      return;
    }
  }
  startActivityForResult(std::move(menuActivity), [this](const ActivityResult& result) {
    if (const auto* chapter = std::get_if<ChapterResult>(&result.data)) {
#if CROSSINK_APP_CAP_TOUCH
      touchReaderDrawerState = chapter->drawerState;
#endif
      applyOrientation(chapter->orientation);
      if (chapter->settingsChanged) {
        ensureReaderSdFontLoaded(renderer);
        RenderLock lock(*this);
        prepareCurrentSectionForRelayout();
        section.reset();
      }
      {
        RenderLock lock(*this);
        clearFootnotePreviewState();
        currentSpineIndex = chapter->spineIndex;
        pendingAnchor = chapter->anchor;
        nextPageNumber = 0;
        section.reset();
      }
      armReadingPaceWarmup("chapter_jump");
      pauseReadingPaceTimer("chapter_jump");
      requestUpdate();
      return;
    }
    if (const auto* clipping = std::get_if<ClippingJumpResult>(&result.data)) {
      applyOrientation(clipping->orientation);
      if (clipping->settingsChanged) {
        ensureReaderSdFontLoaded(renderer);
        RenderLock lock(*this);
        prepareCurrentSectionForRelayout();
        section.reset();  // Force re-layout with changed reader settings
      }
      handleClippingJump(*clipping);
      requestUpdate();
      return;
    }

    // Always apply orientation change even if the menu was cancelled
    const auto* menu = std::get_if<MenuResult>(&result.data);
    if (menu == nullptr) {
      resumeReadingPaceTimer("reader_menu_return");
      requestUpdate();
      return;
    }
#if CROSSINK_APP_CAP_TOUCH
    touchReaderDrawerState = menu->reopenDrawer ? menu->drawerState : ReaderDrawerState{};
#endif
    applyOrientation(menu->orientation);
    if (menu->settingsChanged && hasReaderSettingsChange(menu->changeMask, ReaderSettingsChangeMask::Relayout)) {
      ensureReaderSdFontLoaded(renderer);
      RenderLock lock(*this);
      prepareCurrentSectionForRelayout();
      section.reset();  // Force re-layout with changed reader settings
    }
    resumeReadingPaceTimer("reader_menu_return");
    if (!result.isCancelled) {
      if (menu->action == static_cast<int>(EpubReaderMenuAction::GO_TO_PERCENT) && menu->drawerValue >= 0) {
        jumpToPercent(menu->drawerValue);
        return;
      }
      if (menu->action == static_cast<int>(EpubReaderMenuAction::AUTO_PAGE_TURN) && menu->drawerValue >= 0) {
        setAutoPageTurnIntervalSeconds(static_cast<uint16_t>(menu->drawerValue));
        return;
      }
      const auto action = static_cast<EpubReaderMenuActivity::MenuAction>(menu->action);
      const bool returnToReaderMenu =
          shouldReopenTouchReaderDrawer(menu->reopenDrawer, mappedInput.hasTouchHardware()) ||
          !mappedInput.hasTouchHardware();
      onReaderMenuConfirm(action, returnToReaderMenu);
#if CROSSINK_APP_CAP_TOUCH
      if (shouldReopenTouchReaderDrawer(menu->reopenDrawer, mappedInput.hasTouchHardware()) &&
          (action == EpubReaderMenuAction::BOOKMARK_TOGGLE || action == EpubReaderMenuAction::TOGGLE_COMPLETED ||
           action == EpubReaderMenuAction::RESET_READING_PACE)) {
        openReaderMenu();
      }
#endif
    }
  });
}

void EpubReaderActivity::showBuildPopup() {
  if (!buildPopupPending || !renderer.hasFrameBuffer()) return;
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  pagesUntilFullRefresh = 1;
  buildPopupPending = false;
}

bool EpubReaderActivity::backgroundSectionBuildHasHeap() {
  const auto heap = MemoryBudget::snapshot();
  if (MemoryBudget::hasHeap(heap, MemoryBudget::EPUB_TEXT_LAYOUT_MIN_FREE,
                            MemoryBudget::EPUB_TEXT_LAYOUT_MIN_MAX_ALLOC)) {
    backgroundBuildPausedForLowMemory = false;
    return true;
  }

  if (!backgroundBuildPausedForLowMemory) {
    LOG_DBG("ERS", "Pausing background section build: low heap (free=%u, maxAlloc=%u, need %u/%u)", heap.freeHeap,
            heap.maxAllocHeap, MemoryBudget::EPUB_TEXT_LAYOUT_MIN_FREE, MemoryBudget::EPUB_TEXT_LAYOUT_MIN_MAX_ALLOC);
  }
  backgroundBuildPausedForLowMemory = true;
  return false;
}

void EpubReaderActivity::idlePrewarmNextPage() {
  if (!section || section->isBuilding() || activeFootnotePreview || automaticPageTurnActive ||
      !renderer.hasFrameBuffer() || RenderLock::peek() || lastRenderCompleteMs == 0 ||
      (millis() - lastRenderCompleteMs) < IDLE_SD_FONT_PREWARM_DELAY_MS) {
    return;
  }

  const int renderFontId = activeSectionFontId != 0 ? activeSectionFontId : SETTINGS.getReaderFontId();
  if (!renderer.isSdCardFont(renderFontId) ||
      (idlePrewarmSpine == currentSpineIndex && idlePrewarmPage == section->currentPage &&
       idlePrewarmFontId == renderFontId)) {
    return;
  }

  const int nextPage = section->currentPage + 1;
  if (nextPage >= section->pageCount) {
    return;
  }

  const auto heap = MemoryBudget::snapshot();
  if (!MemoryBudget::hasHeap(heap, IDLE_SD_FONT_PREWARM_MIN_FREE, IDLE_SD_FONT_PREWARM_MIN_MAX_ALLOC)) {
    return;
  }

  // The scan loads one serialized page and may grow persistent SD-font mini buffers. Keep it
  // behind both free-heap and contiguous-block gates, and run it once per visible page/font.
  idlePrewarmSpine = currentSpineIndex;
  idlePrewarmPage = section->currentPage;
  idlePrewarmFontId = renderFontId;

  RenderLock lock(*this);
  auto page = section->loadPage(nextPage);
  if (!page) {
    LOG_DBG("ERS", "Idle SD font prewarm skipped: failed to load spine=%d page=%d", currentSpineIndex, nextPage);
    return;
  }

  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) {
    LOG_DBG("ERS", "Idle SD font prewarm skipped: no font cache manager");
    return;
  }

  const unsigned long startedAt = millis();
  auto scope = fcm->createPrewarmScope();
  page->renderText(renderer, renderFontId, 0, 0);
  scope.endScanAndPrewarm();
  LOG_DBG("ERS", "Idle SD font prewarm: spine=%d page=%d in %lums", currentSpineIndex, nextPage, millis() - startedAt);
}

// One dismissal rule for every transient reader confirmation: it clears when its
// dwell time elapses or the reader presses a navigation button.
bool EpubReaderActivity::transientFeedbackDismissed(const unsigned long showTimeMs) const {
  if ((millis() - showTimeMs) >= TRANSIENT_FEEDBACK_MS) {
    return true;
  }
  return mappedInput.wasReleased(MappedInputManager::Button::Left) ||
         mappedInput.wasReleased(MappedInputManager::Button::Right) ||
         mappedInput.wasReleased(MappedInputManager::Button::Up) ||
         mappedInput.wasReleased(MappedInputManager::Button::Down);
}

void EpubReaderActivity::loop() {
  if (quickActionsPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

#if CROSSINK_APP_CAP_TOUCH
  if (handlePinchFontResize()) {
    // A live two-finger gesture is reader input, so background indexing yields
    // just as it does for a page turn or normal tap.
    backgroundBuildYieldForInput.store(true, std::memory_order_relaxed);
    return;
  }
#endif

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  if (touch.tapped &&
      ReaderUtils::isBottomStatusBarTap(renderer, touch.y, UITheme::getInstance().getStatusBarHeight())) {
    statusBarVisible = !statusBarVisible;
    requestUpdate();
    return;
  }
  // A popup selection suppresses the Confirm release that follows its press.
  // Read it once: wasReleased() consumes that suppression, and a second read
  // in this loop would otherwise turn the same release into a reader-menu open.
  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  const bool userInputPending = mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased() || touch.tapped ||
                                touch.prev || touch.next || mappedInput.wasScreenTouchReleased();
  if (userInputPending) {
    // Do not tear down the parser: suspending here would write a partial cache and
    // make the next build replay from page 0. Yield until the requested render starts.
    backgroundBuildYieldForInput.store(true, std::memory_order_relaxed);
  }

  if (goHomeAfterBuildCancel.load(std::memory_order_relaxed) && !RenderLock::peek()) {
    goHomeAfterBuildCancel.store(false, std::memory_order_relaxed);
    sectionBuildCancelRequested.store(false, std::memory_order_relaxed);
    onGoHome();
    return;
  }

  if (RenderLock::peek() && !touch.prev && !touch.next && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    sectionBuildCancelRequested.store(true, std::memory_order_relaxed);
    goHomeAfterBuildCancel.store(true, std::memory_order_relaxed);
    automaticPageTurnActive = false;
    LOG_DBG("ERS", "Back requested while EPUB indexing is busy; cancelling build");
    return;
  }

#if CROSSINK_APP_CAP_TOUCH
  if (activeFootnotePreview && touch.tapped && !RenderLock::peek() &&
      TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    restoreSavedPosition();
    return;
  }
#endif

  // Lazily resume a partial's extension build once the reader nears its watermark. Far from it the
  // rebuild is all cost (whole-chapter re-layout from page 0) and no benefit this session.
  if (!backgroundBuildYieldForInput.load(std::memory_order_relaxed) && section && !section->isBuilding() &&
      section->isPartial() && !RenderLock::peek() && buildViewportWidth > 0 && !partialRebuildStartFailed &&
      !partialRebuildAbortedForLowMemory &&
      section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount)) {
    RenderLock lock(*this);
    releaseGrayscaleStripScratch();
    if (section && !section->isBuilding() && section->isPartial() && backgroundSectionBuildHasHeap()) {
      const int renderFontId = activeSectionFontId != 0 ? activeSectionFontId : SETTINGS.getReaderFontId();
      const SectionBuildProfile profile = buildProfileForRenderMode(normalizeRenderMode(SETTINGS.epubRenderMode));
      if (!section->startBuild(
              readerRenderSpecForProfile(renderFontId, buildViewportWidth, buildViewportHeight, profile))) {
        partialRebuildStartFailed = true;
        LOG_ERR("ERS", "Failed to start deferred partial extension build");
      } else {
        LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
                section->pageCount);
      }
    }
  }

  // Drive any in-progress incremental section build forward, off the page-turn critical path,
  // but only within a small window ahead of the reader: an unbounded build monopolized the
  // RenderLock and locked out page turns. The build follows the reader instead, and instant
  // reopen comes from suspendBuild() persisting the laid-out pages as a partial on exit.
  // Skip while the render mutex is busy so we never delay a pending render; re-check
  // isBuilding() under the lock since render() may have just finished it.
  // While extending a partial, pageCount is pinned at the partial watermark until the rebuild
  // catches up, so keep ticking it even before activeBuildHasCaughtReadablePages() turns true;
  // the window check below would compare against the pinned watermark and stall the catch-up.
  // Once the extension has caught up, the window applies again: without that, a resumed partial
  // rebuilt its whole chapter in one hot-loop burst instead of following the reader.
  // sectionBuildWantsTick() holds the catch-up/window logic and is shared with
  // skipLoopDelay(), so the loop only runs hot while a tick can actually happen.
  if (!backgroundBuildYieldForInput.load(std::memory_order_relaxed) && sectionBuildWantsTick() && !RenderLock::peek() &&
      (section->isPartial() || section->activeBuildHasCaughtReadablePages())) {
    RenderLock lock(*this);
    releaseGrayscaleStripScratch();
    // Re-check under the lock: render() may have finalized the build between the outer
    // isBuilding() check and acquiring the lock here.
    if (section && section->isBuilding() && (section->isPartial() || section->activeBuildHasCaughtReadablePages()) &&
        backgroundSectionBuildHasHeap()) {
      if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
        LOG_ERR("ERS", "Background section build failed");
        if (section->lastBuildLayoutAbortedForLowMemory() && section->pageCount > 0) {
          partialRebuildAbortedForLowMemory = true;
          LOG_ERR("ERS", "Background section build suspended for low heap; not retrying for this section");
          return;
        }
        section.reset();
        requestUpdate();
        return;
      }
      if (section->isBuildComplete()) {
        const bool repositioned = applyDeferredReposition();
        if (repositioned || progressSaveRequiredAfterRelayout) {
          requestUpdate();
        }
      }
    }
  }

  if (completionPromptQueued) {
    completionPromptQueued = false;
    completionPromptShown = true;
    pauseReadingPaceTimer("completion_prompt");
    startActivityForResult(
        std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_MARK_FINISHED_PROMPT_TITLE),
                                               tr(STR_MARK_FINISHED_PROMPT_BODY)),
        [this](const ActivityResult& result) {
          resumeReadingPaceTimer("completion_prompt_return");
          if (!result.isCancelled) {
            setBookCompleted(true);
            showCompletedFeedback(true);
          }
          requestUpdate();
        });
    return;
  }

  if (pendingBookmarkFeedback && transientFeedbackDismissed(bookmarkFeedbackShowTime)) {
    pendingBookmarkFeedback = false;
    requestUpdate();
    return;
  }

  if (pendingCompletedFeedback && transientFeedbackDismissed(completedFeedbackShowTime)) {
    pendingCompletedFeedback = false;
    requestUpdate();
    return;
  }
  if (pendingTiltPageTurnFeedback && transientFeedbackDismissed(tiltPageTurnFeedbackShowTime)) {
    pendingTiltPageTurnFeedback = false;
    homeButtonInReaderFeedback = false;
    requestUpdate();
    return;
  }
  if ((pendingRenderModeToast || pendingSafeModeToast) &&
      (millis() - renderModeToastShowTime) >= RENDER_MODE_TOAST_MS) {
    bool toastRegionRestored = false;
    if (renderModeToastRegionSaved) {
      if (RenderLock::peek()) {
        return;
      }
      RenderLock lock(*this);
      toastRegionRestored = restoreRenderModeToastRegion();
    }
    pendingRenderModeToast = false;
    pendingSafeModeToast = false;
    if (!toastRegionRestored) {
      requestUpdate();
    }
    return;
  }

  // End-of-Book screen reached (currentSpineIndex == spine count) means the book is
  // finished. Two independent finished-book features key off this same condition.
  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();

  // Paged back into the book: release the end screen app and its theme tokens.
  if (!atEndOfBook && endOfBookOptions) {
    RenderLock lock(*this);
    endOfBookOptions.reset();
  }

  // The render task is asynchronous. Prepare suggestions before an input can
  // leave the reader and move this EPUB into /Read/, while still allocating
  // this UI state only when the end screen is reached.
  if (atEndOfBook) {
    RenderLock lock(*this);
    if (!endOfBookOptions) {
      endOfBookOptions = makeUniqueNoThrow<EndOfBookOptions>(renderer);
      if (!endOfBookOptions) {
        LOG_ERR("ERS", "OOM: EndOfBookOptions (%u bytes)", static_cast<unsigned>(sizeof(EndOfBookOptions)));
      }
    }
    if (endOfBookOptions) {
      endOfBookOptions->loadOnce(epub->getPath());
    }
  }

  // Drop this book from the Recent Books list; if the reader then pages back into the book,
  // re-add it. So removal only sticks if the reader leaves while still on the End-of-Book
  // screen. Acts only on the transition (guarded by recentsEntryRemoved), no per-frame writes.
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

  // The suggestion menu owns Confirm/Back/navigation before automatic page
  // turning and reader shortcuts. This keeps a hold from placing a bookmark
  // or opening dictionary selection behind the menu.
  const bool endOfBookMenuOpen = atEndOfBook && endOfBookOptions && endOfBookOptions->menuActive();
  if (endOfBookMenuOpen) {
    longPressMenuHandled = false;
    lastPageTurnTime = millis();
    std::string openPath;
    switch (endOfBookOptions->handleMenuInput(mappedInput, &openPath)) {
      case EndOfBookOptions::Action::OpenBook:
        activityManager.goToReader(openPath);
        return;
      case EndOfBookOptions::Action::GoHome:
        onGoHome();
        return;
      case EndOfBookOptions::Action::LastPage:
        currentSpineIndex = std::max(epub->getSpineItemsCount() - 1, 0);
        nextPageNumber = 0;
        pendingPageJump = std::numeric_limits<uint16_t>::max();
        requestUpdate();
        return;
      case EndOfBookOptions::Action::Redraw:
        requestUpdate();
        return;
      case EndOfBookOptions::Action::None:
        break;
    }
  }

  if (automaticPageTurnActive && !endOfBookMenuOpen) {
    if (confirmReleased || (!touch.prev && !touch.next && mappedInput.wasReleased(MappedInputManager::Button::Back)) ||
        ReaderUtils::isTouchMenuGesture(mappedInput)) {
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
      pageTurn(true, "auto");
      return;
    }
  }

#if CROSSINK_APP_CAP_TOUCH
  if (!atEndOfBook && touch.tapped && handleTouchFootnoteLink(touch.x, touch.y)) {
    return;
  }
#endif

  // Long-press Confirm: execute the configured reader action without opening the menu.
  // The end-of-book menu receives the same input first and keeps this shortcut inert.
  if (!endOfBookMenuOpen && longPressMenuHandled) {
    if (confirmReleased || !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressMenuHandled = false;
    }
    return;
  }

  if (!endOfBookMenuOpen && confirmReleased) {
    if (SETTINGS.longPressMenuAction != CrossPointSettings::LONG_MENU_OFF &&
        mappedInput.getHeldTime() >= longPressMenuMs) {
      const auto action = static_cast<CrossPointSettings::LONG_PRESS_MENU_ACTION>(SETTINGS.longPressMenuAction);
      executeReaderQuickAction(action);
      return;
    }
  }

  if (!endOfBookMenuOpen && SETTINGS.longPressMenuAction != CrossPointSettings::LONG_MENU_OFF &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= longPressMenuMs) {
    longPressMenuHandled = true;
    const auto action = static_cast<CrossPointSettings::LONG_PRESS_MENU_ACTION>(SETTINGS.longPressMenuAction);
    executeReaderQuickAction(action);
    return;
  }

  // Enter reader menu activity.
  if (!endOfBookMenuOpen && (confirmReleased || ReaderUtils::isTouchMenuGesture(mappedInput))) {
    openReaderMenu();
  }

  if (!endOfBookMenuOpen && handleTouchDictionaryLookup()) {
    return;
  }

  if (longPressBackHandled) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        !mappedInput.isPressed(MappedInputManager::Button::Back)) {
      longPressBackHandled = false;
    }
    return;
  }

  if (!longPressBackHandled && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    longPressBackHandled = true;
    const auto action = static_cast<CrossPointSettings::LONG_PRESS_MENU_ACTION>(SETTINGS.longPressBackAction);
    if (action == CrossPointSettings::LONG_MENU_CREATE_CLIPPING) {
      startClipSelection(nullptr, /*ignoreInitialBackRelease=*/true);
    } else {
      mappedInput.suppressNextBackRelease();
      executeReaderQuickAction(action, true, QuickLockTrigger::LongBack);
    }
    return;
  }

  // Short press BACK goes directly to home (or restores position if viewing footnote)
  if (!touch.prev && !touch.next && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
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
  const bool sideLongPressSkipsChapter =
      SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_CHAPTER_SKIP;
  const bool sideLongPressChangesFont =
      SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_FONT_SIZE;
  const bool sideLongPressChangesOrientation =
      SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_ORIENTATION_CHANGE;
  if (sideLongPressSkipsChapter || sideLongPressChangesFont || sideLongPressChangesOrientation) {
    const bool topReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
    const bool bottomReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
    const bool sidePrevReleased = mappedInput.wasReleased(MappedInputManager::Button::PageBack);
    const bool sideNextReleased = mappedInput.wasReleased(MappedInputManager::Button::PageForward);
    const bool sideLongPressReleased =
        sideLongPressSkipsChapter ? (sidePrevReleased || sideNextReleased) : (topReleased || bottomReleased);
    if (sideButtonLongPressHandled && sideLongPressReleased) {
      sideButtonLongPressHandled = false;
      return;
    }

    const bool longPressReady = mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
    const bool prevLongPressed = longPressReady && mappedInput.isPressed(MappedInputManager::Button::PageBack);
    const bool nextLongPressed = longPressReady && mappedInput.isPressed(MappedInputManager::Button::PageForward);
    const bool topLongPressed =
        longPressReady && (mappedInput.isPressed(MappedInputManager::Button::Up) || topReleased);
    const bool bottomLongPressed =
        longPressReady && (mappedInput.isPressed(MappedInputManager::Button::Down) || bottomReleased);

    if (sideLongPressSkipsChapter && !sideButtonLongPressHandled && (prevLongPressed || nextLongPressed)) {
      sideButtonLongPressHandled = true;
      if (!nextLongPressed && section && section->currentPage > 0) {
        section->currentPage = 0;
        requestUpdate();
        return;
      }

      // We don't want to delete the section mid-render, so grab the semaphore.
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        if (nextLongPressed) {
          currentSpineIndex++;
        } else if (currentSpineIndex > 0) {
          currentSpineIndex--;
        }
        section.reset();
      }
      requestUpdate();
      return;
    }

    if ((sideLongPressChangesFont || sideLongPressChangesOrientation) && !sideButtonLongPressHandled &&
        (topLongPressed || bottomLongPressed)) {
      // Top grows the font and rotates counter-clockwise; bottom is its inverse.
      // Top wins when both are held, matching the previous ordered checks.
      const bool isTop = topLongPressed;
      sideButtonLongPressHandled = !(isTop ? topReleased : bottomReleased);
      if (sideLongPressChangesFont) {
        if (sdFontSystem.changeReaderFontSize(/*larger=*/isTop)) {
          reindexCurrentSection();
        }
      } else {
        applyOrientation(ReaderUtils::rotatedOrientation(SETTINGS.orientation, /*clockwise=*/!isTop));
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

  const bool frontLongPressChangesFont = SETTINGS.longPressButtonBehavior == CrossPointSettings::FONT_SIZE_CHANGE;
  const bool frontLongPressAction = SETTINGS.longPressButtonBehavior == CrossPointSettings::CHAPTER_SKIP ||
                                    SETTINGS.longPressButtonBehavior == CrossPointSettings::ORIENTATION_CHANGE ||
                                    frontLongPressChangesFont;
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

      if (frontLongPressChangesFont) {
        if (sdFontSystem.changeReaderFontSize(/*larger=*/nextLongPressed)) {
          reindexCurrentSection();
        }
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
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  const bool powerReleased = mappedInput.wasReleased(MappedInputManager::Button::Power);
  const bool shortPowerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && powerReleased &&
                              mappedInput.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration();
  const bool shortPowerPrevious = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PREVIOUS_PAGE &&
                                  powerReleased &&
                                  mappedInput.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration();
  const bool releasedLongPowerTurn = SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                                     powerReleased &&
                                     mappedInput.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
  const bool releasedLongPowerPrevious = SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PREVIOUS_PAGE &&
                                         powerReleased &&
                                         mappedInput.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
  bool heldLongPowerTurn = false;
  if (SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && consumeLongPowerButtonHold()) {
    nextTriggered = true;
    fromSideBtn = false;
    fromTilt = false;
    heldLongPowerTurn = true;
  }
  if (SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PREVIOUS_PAGE && consumeLongPowerButtonHold()) {
    prevTriggered = true;
    fromSideBtn = false;
    fromTilt = false;
    heldLongPowerTurn = true;
  }
  prevTriggered = prevTriggered || shortPowerPrevious || releasedLongPowerPrevious;
  if (!prevTriggered && !nextTriggered) {
    idlePrewarmNextPage();
    return;
  }

  if (nextTriggered && silentPrefetchBuildActive.load(std::memory_order_relaxed)) {
    // This turn still advances to the visible next page. The speculative build
    // sees this at its next parser checkpoint and leaves no partial .bin behind.
    silentPrefetchCancelRequested.store(true, std::memory_order_relaxed);
    LOG_DBG("ERS", "Forward page turn requested while silent next-chapter indexing is busy; cancelling prefetch");
  }

  // At end of the book with no suggestion menu, forward button goes home and back
  // button returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (endOfBookOptions && endOfBookOptions->menuActive()) {
      // Selection movement was handled above; absorb leftover page-turn triggers so
      // e.g. "previous" at the top of the list doesn't jump back into the book
      return;
    }
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

  // Touch page turns deliberately ignore the physical-button long-press
  // settings. Keep those saved settings intact for a later move back to a
  // button device, but never let a held screen tap skip a chapter or rotate.
  const bool fromTouch = touch.prev || touch.next;
  const unsigned long heldMs = fromTouch ? touch.heldMs : mappedInput.getHeldTime();
  const bool longPress = !fromTilt && heldMs > ReaderUtils::SKIP_HOLD_MS;
  const bool skipChapter =
      !fromTouch && longPress &&
      (fromSideBtn ? SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_CHAPTER_SKIP
                   : SETTINGS.longPressButtonBehavior == CrossPointSettings::CHAPTER_SKIP);

  // Don't skip chapter after screenshot
  if (gpio.wasReleased(HalGPIO::BTN_POWER) && gpio.wasReleased(HalGPIO::BTN_DOWN)) {
    return;
  }

  if (skipChapter) {
    if (!nextTriggered && section && section->currentPage > 0) {
      section->currentPage = 0;
      requestUpdate();
      return;
    }

    // We don't want to delete the section mid-render, so grab the semaphore
    {
      RenderLock lock(*this);
      nextPageNumber = 0;
      if (nextTriggered) {
        currentSpineIndex++;
      } else if (currentSpineIndex > 0) {
        currentSpineIndex--;
      }
      section.reset();
    }
    requestUpdate();
    return;
  }

  if (!fromTouch && longPress && !fromSideBtn &&
      SETTINGS.longPressButtonBehavior == CrossPointSettings::ORIENTATION_CHANGE) {
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

  const char* pageTurnSource = fromTouch ? "touch" : (fromTilt ? "tilt" : (fromSideBtn ? "side" : "front"));
  if (shortPowerTurn || shortPowerPrevious || releasedLongPowerTurn || releasedLongPowerPrevious || heldLongPowerTurn) {
    pageTurnSource = "power";
  }
  // Drop the manual turn if a render is still in flight, OR within a short window of the
  // last turn. render() runs on its own task (renderTaskLoop) concurrently with
  // input; a slow AA/image page display lags behind fast taps, and a second turn
  // firing before the first commits its differential baseline writes the panel
  // twice -> two overlapping page segments. RenderLock::peek() catches a render
  // that has already taken the lock (mirrors the automatic-turn guard), but there
  // is a brief window between requesting a turn and the render task acquiring the
  // lock where peek() is still false, a mashed second tap slips through there,
  // which is what still triggered after slow image pages. The lastPageTurnTime
  // gap bridges that startup latency; after it, peek() takes over for the rest of
  // the (variable-length) render. You can't turn faster than the panel refreshes,
  // so dropping the extra tap (vs corrupting the frame) is correct; the next tap
  // after the render finishes turns normally.
  constexpr unsigned long kMinManualTurnGapMs = 200;
  if (RenderLock::peek() || (millis() - lastPageTurnTime) < kMinManualTurnGapMs) {
    return;
  }
  if (prevTriggered) {
    pageTurn(false, pageTurnSource);
  } else {
    pageTurn(true, pageTurnSource);
  }
}

bool EpubReaderActivity::handleTwoFingerSwipeAction(const CrossPointSettings::TWO_FINGER_SWIPE_ACTION action) {
  if (!epub) return false;

  switch (action) {
    case CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_FONT_SIZE:
      if (sdFontSystem.changeReaderFontSize(/*larger=*/true, FontSizeStepMode::Clamp)) reindexCurrentSection();
      return true;
    case CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_FONT_SIZE:
      if (sdFontSystem.changeReaderFontSize(/*larger=*/false, FontSizeStepMode::Clamp)) reindexCurrentSection();
      return true;
    case CrossPointSettings::TWO_FINGER_SWIPE_NEXT_CHAPTER:
    case CrossPointSettings::TWO_FINGER_SWIPE_PREVIOUS_CHAPTER: {
      const int direction = action == CrossPointSettings::TWO_FINGER_SWIPE_NEXT_CHAPTER ? 1 : -1;
      int firstSpineIndex = currentSpineIndex;
      int lastSpineIndex = currentSpineIndex;
      epub->resolveChapterGroupRange(currentSpineIndex, firstSpineIndex, lastSpineIndex);
      int targetSpine = direction > 0 ? lastSpineIndex + 1 : firstSpineIndex - 1;
      if (direction < 0 && targetSpine >= 0) {
        int previousChapterFirstSpineIndex = targetSpine;
        int previousChapterLastSpineIndex = targetSpine;
        epub->resolveChapterGroupRange(targetSpine, previousChapterFirstSpineIndex, previousChapterLastSpineIndex);
        targetSpine = previousChapterFirstSpineIndex;
      }
      if (activeFootnotePreview || targetSpine < 0 || targetSpine >= epub->getSpineItemsCount()) return true;
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex = targetSpine;
        section.reset();
      }
      armReadingPaceWarmup("two_finger_chapter");
      requestUpdate();
      return true;
    }
    default:
      return false;
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

  int locationSpineIndex = 0;
  float locationSpineProgress = 0.0f;
  if (epub->resolveLocationPercentToSpineProgress(percent, locationSpineIndex, locationSpineProgress)) {
    clearFootnotePreviewState();
    currentSpineIndex = locationSpineIndex;
    pendingSpineProgress = locationSpineProgress;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
    armReadingPaceWarmup("percent_jump");
    return;
  }

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
  clearFootnotePreviewState();
  currentSpineIndex = targetSpineIndex;
  nextPageNumber = 0;
  pendingPercentJump = true;
  section.reset();
  armReadingPaceWarmup("percent_jump");
}

void EpubReaderActivity::handleClippingJump(const ClippingJumpResult& clipping) {
  RenderLock lock(*this);
  clearFootnotePreviewState();
  currentSpineIndex = clipping.spineIndex;
  pendingPageJump = clipping.page;
  pendingParagraphIndex = clipping.paragraphIndex;
  pendingClippingIndex = clipping.clippingIndex;
  section.reset();
  armReadingPaceWarmup("clipping_jump");
  pauseReadingPaceTimer("clipping_jump");
}

#if CROSSINK_APP_CAP_TOUCH
bool EpubReaderActivity::handlePinchFontResize() {
  if (!SETTINGS.pinchFontResizeEnabled || !SETTINGS.touchReaderControls || !mappedInput.supportsMultiTouch()) {
    resetPinchFontGesture();
    return false;
  }

  int x1 = 0;
  int y1 = 0;
  int x2 = 0;
  int y2 = 0;
  if (!mappedInput.getTwoFingerTouch(x1, y1, x2, y2)) {
    resetPinchFontGesture();
    return false;
  }

  const auto action = pinchFontGesture.update(x1, y1, x2, y2);

  if (action == ReaderPinchGesture::Action::Increase) {
    mappedInput.suppressCurrentTouchContact();
    if (sdFontSystem.changeReaderFontSize(/*larger=*/true, FontSizeStepMode::Clamp)) reindexCurrentSection();
  } else if (action == ReaderPinchGesture::Action::Decrease) {
    mappedInput.suppressCurrentTouchContact();
    if (sdFontSystem.changeReaderFontSize(/*larger=*/false, FontSizeStepMode::Clamp)) reindexCurrentSection();
  }
  return true;
}

void EpubReaderActivity::resetPinchFontGesture() { pinchFontGesture.reset(); }
#endif

bool EpubReaderActivity::handleTwoFingerRotation(const bool clockwise) {
  applyOrientation(ReaderUtils::rotatedOrientation(SETTINGS.orientation, clockwise));
  requestUpdate();
  return true;
}

bool EpubReaderActivity::handleTouchDictionaryLookup() {
  if (!SETTINGS.touchReaderControls || !mappedInput.hasTouch() || RenderLock::peek() || activeFootnotePreview ||
      !epub) {
    return false;
  }

  int touchX = 0;
  int touchY = 0;
  unsigned long heldMs = 0;
  if (!mappedInput.isScreenTouchTapCandidate(touchX, touchY, heldMs)) {
    touchDictionaryLookupHandled = false;
    return false;
  }
  if (touchDictionaryLookupHandled || heldMs < TOUCH_DICTIONARY_LOOKUP_HOLD_MS) {
    return false;
  }

  touchDictionaryLookupHandled = true;
  if (!Dictionary::exists(epub->getCachePath().c_str())) {
    return false;
  }

  openWordSelect(/*framebufferContainsPage=*/true, touchX, touchY, /*autoLookupInitialWord=*/true);
  return true;
}

std::unique_ptr<Page> EpubReaderActivity::reloadDictionaryLookupPage(const int pageOffset) {
  if (!section || pageOffset < 0 || section->currentPage + pageOffset >= section->pageCount) return nullptr;
  // A Page is variable-sized and can reach tens of KB, so it must remain a
  // fallible heap object. It exists only while rebuilding the parent selection
  // or a rare full-screen modal background, then is released immediately.
  const int savedPage = section->currentPage;
  section->currentPage = savedPage + pageOffset;
  auto page = section->loadPageFromSectionFile();
  section->currentPage = savedPage;
  return page;
}

std::unique_ptr<Page> EpubReaderActivity::reloadDictionaryLookupPageCallback(void* context, const int pageOffset) {
  return static_cast<EpubReaderActivity*>(context)->reloadDictionaryLookupPage(pageOffset);
}

void EpubReaderActivity::renderDictionaryLookupBackground() {
  auto backgroundPage = reloadDictionaryLookupPage();
  if (!backgroundPage) {
    LOG_ERR("DICT", "Failed to reload reader page for dictionary modal background");
    renderer.clearScreen(ReaderUtils::readerBackgroundColor());
    return;
  }

  const ReaderViewportLayout layout = computeReaderViewportLayout(renderer, automaticPageTurnActive);
  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  const bool foregroundBlack = ReaderUtils::readerForegroundBlack();
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) {
    backgroundPage->render(renderer, SETTINGS.getReaderFontId(), layout.marginLeft, layout.marginTop, foregroundBlack);
    return;
  }
  auto scope = fcm->createPrewarmScope();
  backgroundPage->render(renderer, SETTINGS.getReaderFontId(), layout.marginLeft, layout.marginTop, foregroundBlack);
  scope.endScanAndPrewarm();
  backgroundPage->render(renderer, SETTINGS.getReaderFontId(), layout.marginLeft, layout.marginTop, foregroundBlack);
}

void EpubReaderActivity::openWordSelect(bool framebufferContainsPage, int initialTouchX, int initialTouchY,
                                        bool autoLookupInitialWord) {
  std::unique_ptr<Page> pageForLookup;
  ReaderViewportLayout layout{};
  std::string bookCachePath;
  std::string nextPageFirstWord;
  bool hasNextPageForLookup = false;

  {
    RenderLock lock(*this);
    if (!section || !epub) {
      requestUpdate();
      return;
    }

    pageForLookup = section->loadPageFromSectionFile();
    if (!pageForLookup) {
      requestUpdate();
      return;
    }

    layout = computeReaderViewportLayout(renderer, automaticPageTurnActive);
    bookCachePath = epub->getCachePath();

    if (section->currentPage < section->pageCount - 1) {
      const int savedPage = section->currentPage;
      section->currentPage = savedPage + 1;
      auto nextPage = section->loadPageFromSectionFile();
      section->currentPage = savedPage;
      if (nextPage) {
        hasNextPageForLookup = true;
        const auto it = std::find_if(nextPage->elements.begin(), nextPage->elements.end(),
                                     [](const auto& element) { return element->getTag() == TAG_PageLine; });
        if (it != nextPage->elements.end()) {
          const auto* firstLine = static_cast<const PageLine*>(it->get());
          if (firstLine->getBlock() && !firstLine->getBlock()->isEmpty()) {
            nextPageFirstWord = firstLine->getBlock()->wordText(0);
          }
        }
      }
    }
  }

  pauseReadingPaceTimer("dictionary_lookup");
  const BookReaderSettingsData bookSettings = loadBookReaderSettingsFile(bookCachePath);
  // The activity outlives this call, so it must be heap-owned; make the fixed-size
  // object allocation fallible instead of aborting the firmware when memory is tight.
  auto wordSelect = makeUniqueNoThrow<DictionaryWordSelectActivity>(
      renderer, mappedInput, std::move(pageForLookup), layout.marginLeft, layout.marginTop, std::move(bookCachePath),
      std::move(nextPageFirstWord), hasNextPageForLookup, framebufferContainsPage, layout.marginBottom, initialTouchX,
      initialTouchY, autoLookupInitialWord, bookSettings.dictionarySdFontFamilyName,
      bookSettings.dictionaryFontPointSize, this, &EpubReaderActivity::reloadDictionaryLookupPageCallback);
  if (!wordSelect) {
    LOG_ERR("DICT", "OOM allocating DictionaryWordSelectActivity (%u bytes)",
            static_cast<unsigned>(sizeof(DictionaryWordSelectActivity)));
    resumeReadingPaceTimer("dictionary_lookup_alloc_failed");
    drawToast(renderer, tr(STR_MEMORY_ERROR));
    delay(1000);
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(wordSelect), [this](const ActivityResult& result) {
    if (const auto* request = std::get_if<DictionaryClippingRequest>(&result.data)) {
      resumeReadingPaceTimer("dictionary_lookup_to_clip");
      startClipSelection(request);
      return;
    }
    resumeReadingPaceTimer("dictionary_lookup_return");
    MemoryBudget::logHeapShape("dict.child_destroyed");
    // Dictionary lookup warms multiple SD-font styles and large definition glyph
    // sets. The child activity has been destroyed before this callback runs, so
    // release those renderer-owned caches before the reader rebuilds its page cache.
    releaseReaderSdFontCachesForLowMemory(renderer, "DICT", "dictionary lookup exit");
    MemoryBudget::logHeapShape("dict.after_font_release");
    pendingHeapShapeReaderRedrawStages.fetch_or(HEAP_SHAPE_REDRAW_DICT, std::memory_order_relaxed);
    requestUpdate();
  });
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action, const bool returnToReaderMenu,
                                             const PendingOverlayResume* replacementResume) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::STATUS_BAR_SETTINGS:
      break;
    case EpubReaderMenuActivity::MenuAction::SEND_NEARBY_BOOK: {
      if (!epub) break;
      const int page = section ? section->currentPage : nextPageNumber;
      const int pageCount = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
      if (!saveProgress(currentSpineIndex, page, pageCount)) {
        LOG_ERR("NBOOK", "Could not save EPUB progress before transfer");
        drawToast(renderer, tr(STR_NEARBY_TRANSFER_PROGRESS_SAVE_FAILED));
        delay(1200);
        requestUpdate();
        break;
      }
      activityManager.goToNearbyBookSend(epub->getPath(), true);
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const std::string path = epub->getPath();
      pauseReadingPaceTimer("chapter_selection");
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx),
          [this, returnToReaderMenu](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& chapterResult = std::get<ChapterResult>(result.data);
              RenderLock lock(*this);

              clearFootnotePreviewState();
              currentSpineIndex = chapterResult.spineIndex;

              // If anchor is not empty, it will be used later to calculate the page number.
              pendingAnchor = chapterResult.anchor;

              // Otherwise page 0 will be used.
              nextPageNumber = 0;

              section.reset();
              armReadingPaceWarmup("chapter_jump");
              pauseReadingPaceTimer("chapter_jump");
            } else if (returnToReaderMenu) {
              openReaderMenu();
            } else {
              requestUpdate();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      pauseReadingPaceTimer("footnotes");
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this, returnToReaderMenu](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               } else if (returnToReaderMenu) {
                                 openReaderMenu();
                                 return;
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
      pauseReadingPaceTimer("percent_selection");
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this, returnToReaderMenu](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            } else if (returnToReaderMenu) {
              openReaderMenu();
            } else {
              requestUpdate();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        auto p = section->loadPage(section->currentPage);
        if (p) {
          std::string fullText;
          for (const auto& el : p->elements) {
            if (el->getTag() == TAG_PageLine) {
              const auto& line = static_cast<const PageLine&>(*el);
              if (line.getBlock()) {
                const auto& block = *line.getBlock();
                for (uint16_t i = 0; i < block.wordCount(); ++i) {
                  if (!fullText.empty()) fullText += " ";
                  fullText += block.wordText(i);
                }
              }
            }
          }
          if (!fullText.empty()) {
            pauseReadingPaceTimer("qr_display");
            startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                   [this, returnToReaderMenu](const ActivityResult&) {
                                     if (returnToReaderMenu)
                                       openReaderMenu();
                                     else
                                       requestUpdate();
                                   });
            break;
          }
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SAVE_CLIPPING: {
      startClipSelection();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::LOOKUP: {
      openWordSelect(/*framebufferContainsPage=*/false);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::LOOKUP_HISTORY: {
      pauseReadingPaceTimer("lookup_history");
      const BookReaderSettingsData bookSettings = loadBookReaderSettingsFile(epub->getCachePath());
      startActivityForResult(std::make_unique<LookedUpWordsActivity>(renderer, mappedInput, epub->getCachePath(),
                                                                     bookSettings.dictionarySdFontFamilyName,
                                                                     bookSettings.dictionaryFontPointSize),
                             [this, returnToReaderMenu](const ActivityResult&) {
                               resumeReadingPaceTimer("lookup_history_return");
                               if (returnToReaderMenu && mappedInput.hasTouchHardware())
                                 openReaderMenu();
                               else
                                 requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SET_BOOK_DICTIONARY: {
      pauseReadingPaceTimer("dictionary_select");
      startActivityForResult(std::make_unique<DictionarySelectActivity>(renderer, mappedInput, epub->getCachePath()),
                             [this, returnToReaderMenu](const ActivityResult&) {
                               resumeReadingPaceTimer("dictionary_select_return");
                               if (returnToReaderMenu && mappedInput.hasTouchHardware())
                                 openReaderMenu();
                               else
                                 requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_STATS: {
      pauseReadingPaceTimer("delete_stats_confirm");
      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput,
                                                                    confirmationHeading(StrId::STR_DELETE_BOOK_STATS),
                                                                    epub ? epub->getTitle() : std::string{}),
                             [this, returnToReaderMenu](const ActivityResult& result) {
                               bool statsDeleted = false;
                               if (!result.isCancelled) {
                                 {
                                   RenderLock lock(*this);
                                   if (epub) {
                                     statsDeleted = BookReadingStats::remove(epub->getCachePath());
                                     if (statsDeleted) {
                                       resetCurrentBookStatsAfterDelete();
                                     }
                                   }
                                 }
                                 if (statsDeleted) {
                                   drawToast(renderer, tr(STR_BOOK_STATS_DELETED));
                                   delay(1000);
                                 } else {
                                   LOG_ERR("ERS", "Failed to delete book stats");
                                 }
                               }
                               resumeReadingPaceTimer("delete_stats_return");
                               if (returnToReaderMenu && mappedInput.hasTouchHardware())
                                 openReaderMenu();
                               else
                                 requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      pauseReadingPaceTimer("delete_cache_confirm");
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput, confirmationHeading(StrId::STR_DELETE_CACHE),
                                                 epub ? epub->getTitle() : std::string{}, false, true),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              resumeReadingPaceTimer("delete_cache_cancel");
              requestUpdate();
              return;
            }

            bool cacheDeleted = false;
            {
              RenderLock lock(*this);
              if (epub && section) {
                uint16_t backupSpine = currentSpineIndex;
                uint16_t backupPage = section->currentPage;
                const int backupPageCount = section->estimatedTotalPages();
                if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
                  LOG_ERR("ERS", "Failed to save progress before cache clear");
                }
                stats.save(epub->getCachePath());
                section.reset();
                cacheDeleted = clearBookCachePreservingUserState(epub->getPath());
                epub->setupCacheDir();
                if (cacheDeleted) {
                  drawToast(renderer, tr(STR_BOOK_CACHE_DELETED));
                }
              }
            }
            if (cacheDeleted) {
              delay(1000);
            } else {
              LOG_ERR("ERS", "Failed to delete book cache");
            }
            onGoHome();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::RESET_READING_PACE: {
      resetReadingPaceData();
      drawToast(renderer, tr(STR_READING_PACE_RESET));
      delay(1000);
      requestUpdate();
      break;
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
      if (SETTINGS.shouldTrackReadingStats()) {
        uint32_t currentPageSeconds = 0;
        displayStats.totalReadingSeconds = displayStats.totalReadingSeconds > UINT32_MAX - sessionReadingSeconds
                                               ? UINT32_MAX
                                               : displayStats.totalReadingSeconds + sessionReadingSeconds;
        if (currentPageReadingSecondsForStats(currentPageSeconds, "book_stats_preview")) {
          displayStats.totalReadingSeconds = displayStats.totalReadingSeconds > UINT32_MAX - currentPageSeconds
                                                 ? UINT32_MAX
                                                 : displayStats.totalReadingSeconds + currentPageSeconds;
        }
      }
      uint32_t estimatedTimeLeftSeconds = 0;
      const bool hasEstimatedTimeLeft = estimateTimeLeftSeconds(true, estimatedTimeLeftSeconds);
      displayStats.estimatedTimeLeftSeconds = hasEstimatedTimeLeft ? estimatedTimeLeftSeconds : 0;
      const bool hasSyncedStats = GlobalReadingStats::hasSyncedStats();
      const GlobalReadingStats displayAllDevicesStats =
          hasSyncedStats ? GlobalReadingStats::loadAggregated(globalStats) : GlobalReadingStats{};
      pauseReadingPaceTimer("book_stats");
      if (hasSyncedStats) {
        startActivityForResult(
            std::make_unique<BookStatsActivity>(renderer, mappedInput, epub->getTitle(), epub->getCachePath(),
                                                displayStats, getCurrentBookProgressPercent(), hasEstimatedTimeLeft,
                                                estimatedTimeLeftSeconds, globalStats, displayAllDevicesStats),
            [this, returnToReaderMenu](const ActivityResult&) { handleBookStatsReturn(returnToReaderMenu); });
      } else {
        startActivityForResult(
            std::make_unique<BookStatsActivity>(renderer, mappedInput, epub->getTitle(), epub->getCachePath(),
                                                displayStats, getCurrentBookProgressPercent(), hasEstimatedTimeLeft,
                                                estimatedTimeLeftSeconds, globalStats),
            [this, returnToReaderMenu](const ActivityResult&) { handleBookStatsReturn(returnToReaderMenu); });
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
      if (activeFootnotePreview) {
        requestUpdate();
        break;
      }
      if (KOREADER_STORE.hasCredentials()) {
        const int currentPage = section ? section->currentPage : nextPageNumber;
        const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;

        // Persist current position so the reader resumes at the right page on return.
        // goToReader() depends on this file, so abort the sync if the write fails.
        if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
          LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
          pendingSyncSaveError = true;
          requestUpdate();
          return;
        }

        // ActivityManager owns this one-shot handoff across deferred reader teardown.
        auto restartActivity = makeUniqueNoThrow<KOReaderSyncActivity>(renderer, mappedInput, SETTINGS.orientation);
        if (!restartActivity) {
          LOG_ERR("KOSync", "OOM: restart handoff (free=%u maxAlloc=%u)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
          drawToast(renderer, tr(STR_KOREADER_SYNC_LOW_MEMORY));
          delay(1200);
          requestUpdate();
          break;
        }

        pauseReadingPaceTimer("sync_progress");
        activityManager.replaceActivity(std::move(restartActivity));
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::NEARBY_POSITION_SYNC: {
      const int currentPage = section ? section->currentPage : nextPageNumber;
      const int totalPages = section ? section->estimatedTotalPages() : std::max(1, cachedChapterTotalPageCount);
      std::optional<uint16_t> paragraphIndex;
      std::optional<uint16_t> listItemIndex;
      if (section) {
        getSyncPageAnchors(*section, currentPage, paragraphIndex, listItemIndex);
      }

      CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
      if (section && currentPage >= 0 && currentPage < section->pageCount) {
        if (const auto offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))) {
          localPos.visibleTextOffset = *offset;
          localPos.hasVisibleTextOffset = true;
        }
      }
      if (paragraphIndex.has_value()) {
        localPos.paragraphIndex = *paragraphIndex;
        localPos.hasParagraphIndex = true;
      }
      if (listItemIndex.has_value()) {
        localPos.liIndex = *listItemIndex;
        localPos.hasLiIndex = true;
      }
      const DocumentMatchMethod matchMethod = KOREADER_STORE.getMatchMethod();
      const PositionCoordinateSpace coordinateSpace = matchMethod == DocumentMatchMethod::FILENAME
                                                          ? PositionCoordinateSpace::SourceDocument
                                                          : PositionCoordinateSpace::CurrentDocument;
      KOReaderPosition localKoPos = ProgressMapper::toKOReader(epub, localPos, coordinateSpace);
      if (!localKoPos.valid) {
        LOG_ERR("NBPS", "Exact filename sync needs a source map; re-optimize this split EPUB");
        drawToast(renderer, tr(STR_SYNC_REOPTIMIZE_REQUIRED));
        delay(1200);
        requestUpdate();
        break;
      }
      const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
      std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
      const std::string savedEpubPath = epub->getPath();

      if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
        LOG_ERR("NBPS", "Aborting nearby position sync because current progress could not be saved");
        pendingSyncSaveError = true;
        requestUpdate();
        return;
      }

      LOG_DBG("NBPS", "Releasing section for nearby position sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
      {
        RenderLock lock(*this);
        if (section) {
          nextPageNumber = section->currentPage;
        }
        section.reset();
      }
      LOG_DBG("NBPS", "Section released for nearby position sync (heap after: %u)", (unsigned)ESP.getFreeHeap());

      auto syncActivity = makeUniqueNoThrow<NearbyBookPositionSyncActivity>(
          renderer, mappedInput, epub, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
          std::move(localChapterName), matchMethod, paragraphIndex, listItemIndex);
      if (!syncActivity) {
        LOG_ERR("NBPS", "OOM opening nearby position sync");
        requestUpdate();
        break;
      }
      if (replacementResume) APP_STATE.setPendingOverlayResume(*replacementResume);
      pauseReadingPaceTimer("nearby_position_sync");
      activityManager.replaceActivity(std::move(syncActivity));
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARK_TOGGLE: {
      const int bookmarkPageCount = section ? section->estimatedTotalPages() : 0;
      if (activeFootnotePreview || !section || bookmarkPageCount == 0 || section->pageCount == 0) break;
      const uint16_t spine = static_cast<uint16_t>(currentSpineIndex);
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(bookmarkPageCount);

      if (BOOKMARKS.hasBookmarkForPage(spine, progress, bookmarkPageCount)) {
        BOOKMARKS.removeBookmarkForPage(spine, progress, bookmarkPageCount);
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
        if (auto page = section->loadPage(section->currentPage)) {
          buildBookmarkSnippet(*page, snippet, sizeof(snippet));
        }
        const auto addResult =
            BOOKMARKS.addBookmark(spine, progress, bookmarkPageCount, chapterTitle, paragraphIndex, snippet);
        bookmarkFeedbackType = (addResult == BookmarkStore::AddResult::Added) ? BookmarkFeedbackType::Added
                                                                              : BookmarkFeedbackType::LimitReached;
      }
      pendingBookmarkFeedback = true;
      bookmarkFeedbackShowTime = millis();
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::VIEW_BOOKMARKS: {
      pauseReadingPaceTimer("bookmark_list");
      startActivityForResult(
          std::make_unique<EpubReaderBookmarkListActivity>(renderer, mappedInput, BOOKMARKS.getBookmarks()),
          [this, returnToReaderMenu](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& bm = std::get<BookmarkResult>(result.data);
              RenderLock lock(*this);
              clearFootnotePreviewState();
              if (section && currentSpineIndex == bm.spineIndex) {
                bool resolved = false;
                const uint16_t fallbackPage =
                    pageFromStoredProgress(bm.progress, static_cast<uint16_t>(section->estimatedTotalPages()));
                if (bm.paragraphIndex != UINT16_MAX) {
                  section->currentPage = resolveParagraphJumpPage(*section, bm.paragraphIndex, fallbackPage);
                  resolved = true;
                  LOG_DBG("ERS", "Resolved bookmark paragraph %u to page %d", bm.paragraphIndex, section->currentPage);
                }
                if (!resolved) {
                  section->currentPage = fallbackPage;
                }
                nextPageNumber = section->currentPage;
                pendingPercentJump = false;
                pendingParagraphIndex = UINT16_MAX;
              } else {
                currentSpineIndex = bm.spineIndex;
                pendingSpineProgress = bm.progress;
                pendingParagraphIndex = bm.paragraphIndex;
                pendingPercentJump = true;
                section.reset();
              }
              armReadingPaceWarmup("bookmark_jump");
              pauseReadingPaceTimer("bookmark_jump");
            } else {
              resumeReadingPaceTimer("bookmark_list_cancel");
              if (returnToReaderMenu && mappedInput.hasTouchHardware()) {
                openReaderMenu();
                return;
              }
            }
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::VIEW_CLIPPINGS: {
      pauseReadingPaceTimer("clipping_list");
      startActivityForResult(std::make_unique<EpubReaderClippingListActivity>(renderer, mappedInput),
                             [this, returnToReaderMenu](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& clipping = std::get<ClippingJumpResult>(result.data);
                                 handleClippingJump(clipping);
                               } else {
                                 resumeReadingPaceTimer("clipping_list_cancel");
                                 if (returnToReaderMenu && mappedInput.hasTouchHardware()) {
                                   openReaderMenu();
                                   return;
                                 }
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_BOOKMARKS: {
      pauseReadingPaceTimer("delete_bookmarks_confirm");
      startActivityForResult(
          std::make_unique<ConfirmationActivity>(renderer, mappedInput,
                                                 confirmationHeading(StrId::STR_DELETE_BOOKMARKS),
                                                 epub ? epub->getTitle() : std::string{}),
          [this, returnToReaderMenu](const ActivityResult& result) {
            if (!result.isCancelled) {
              BOOKMARKS.clearAll();
            }
            resumeReadingPaceTimer(result.isCancelled ? "delete_bookmarks_cancel" : "delete_bookmarks_return");
            if (returnToReaderMenu && mappedInput.hasTouchHardware())
              openReaderMenu();
            else
              requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::AUTO_PAGE_TURN:
      openAutoPageTurnIntervalPicker(false, returnToReaderMenu);
      break;
    case EpubReaderMenuActivity::MenuAction::ROTATE_SCREEN:
    case EpubReaderMenuActivity::MenuAction::READER_OPTIONS:
    case EpubReaderMenuActivity::MenuAction::CONTROLS_OPTIONS:
      break;
  }
}

std::unique_ptr<Activity> EpubReaderActivity::createFrontlightReadingStatsActivity() {
  if (!epub) return {};

  BookReadingStats displayStats = stats;
  if (SETTINGS.shouldTrackReadingStats()) {
    displayStats.totalReadingSeconds = displayStats.totalReadingSeconds > UINT32_MAX - sessionReadingSeconds
                                           ? UINT32_MAX
                                           : displayStats.totalReadingSeconds + sessionReadingSeconds;
    uint32_t currentPageSeconds = 0;
    if (currentPageReadingSecondsForStats(currentPageSeconds, "frontlight_stats_preview")) {
      displayStats.totalReadingSeconds = displayStats.totalReadingSeconds > UINT32_MAX - currentPageSeconds
                                             ? UINT32_MAX
                                             : displayStats.totalReadingSeconds + currentPageSeconds;
    }
  }

  uint32_t estimatedTimeLeftSeconds = 0;
  const bool hasEstimatedTimeLeft = estimateTimeLeftSeconds(true, estimatedTimeLeftSeconds);
  displayStats.estimatedTimeLeftSeconds = hasEstimatedTimeLeft ? estimatedTimeLeftSeconds : 0;
  const bool hasSyncedStats = GlobalReadingStats::hasSyncedStats();
  if (hasSyncedStats) {
    return makeUniqueNoThrow<BookStatsActivity>(
        renderer, mappedInput, epub->getTitle(), epub->getCachePath(), displayStats, getCurrentBookProgressPercent(),
        hasEstimatedTimeLeft, estimatedTimeLeftSeconds, globalStats, GlobalReadingStats::loadAggregated(globalStats));
  }
  return makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, epub->getTitle(), epub->getCachePath(),
                                              displayStats, getCurrentBookProgressPercent(), hasEstimatedTimeLeft,
                                              estimatedTimeLeftSeconds, globalStats);
}

bool EpubReaderActivity::getFrontlightPanelBookDetails(FrontlightPanelBookDetails& details) {
  RenderLock lock(*this);
  if (!epub) return false;

  details.title = epub->getTitle();
  details.author = epub->getAuthor();
  details.chapter.clear();
  const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (tocIndex >= 0 && tocIndex < epub->getTocItemsCount()) {
    details.chapter = epub->getTocItem(tocIndex).title;
  }
  details.progressPercent = clampPercent(static_cast<int>(getCurrentBookProgressPercent() + 0.5f));
  return true;
}

void EpubReaderActivity::onFrontlightPanelClosed() {
  globalStats = GlobalReadingStats::load();
  stats = epub ? BookReadingStats::load(epub->getCachePath()) : stats;
  resumeReadingPaceTimer("frontlight_panel_return");
  requestUpdate();
}

bool EpubReaderActivity::handleFrontlightPanelResult(const FrontlightPanelResult& result) {
  if (!epub || result.action == FrontlightPanelAction::None) return false;
  if (result.action != FrontlightPanelAction::SyncProgress &&
      result.action != FrontlightPanelAction::NearbyPositionSync &&
      result.action != FrontlightPanelAction::SendNearbyBook) {
    return false;
  }

  PendingOverlayResume resume;
  resume.origin = PendingOverlayOrigin::Reader;
  resume.overlay = PendingOverlayType::FrontlightDrawer;
  resume.selectedIndex = result.state.selectedAction;
  resume.bookPath = epub->getPath();
  if (result.action == FrontlightPanelAction::SyncProgress) {
    resume.readerOrientation = SETTINGS.orientation;
    resume.preserveReaderOrientation = true;
    if (KOREADER_STORE.hasCredentials()) APP_STATE.setPendingOverlayResume(resume);
    return startGlobalSyncProgress();
  }
  if (result.action == FrontlightPanelAction::NearbyPositionSync) {
    onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::NEARBY_POSITION_SYNC, false, &resume);
    return true;
  }
  if (!activityManager.goToNearbyBookSend(epub->getPath(), true)) return false;
  APP_STATE.setPendingOverlayResume(std::move(resume));
  return true;
}

bool EpubReaderActivity::handleExternalReaderMenuAction(const uint8_t action) {
  if (action != static_cast<uint8_t>(EpubReaderMenuActivity::MenuAction::NEARBY_POSITION_SYNC)) return false;
  onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::NEARBY_POSITION_SYNC);
  return true;
}

bool EpubReaderActivity::restorePendingOverlay(const PendingOverlayResume& resume) {
#if CROSSINK_APP_CAP_TOUCH
  if (!epub || resume.overlay != PendingOverlayType::ReaderDrawer || resume.bookPath != epub->getPath()) return false;
  touchReaderDrawerState.tab =
      static_cast<ReaderDrawerTab>(std::min<uint8_t>(resume.tab, static_cast<uint8_t>(ReaderDrawerTab::Count) - 1));
  touchReaderDrawerState.pane = static_cast<ReaderDrawerPane>(resume.pane);
  touchReaderDrawerState.selectedIndex = resume.selectedIndex;
  restoreReaderDrawerScroll(touchReaderDrawerState, resume.scrollPosition);
  openReaderMenu();
  return true;
#else
  (void)resume;
  return false;
#endif
}

void EpubReaderActivity::reindexCurrentSection() {
  const bool restorePreviewPosition = activeFootnotePreview;
  {
    RenderLock lock(*this);
    // Saving releases the incremental parser's SD handle. Serialize that close
    // with parseStep() so the render task cannot read a just-closed HalFile.
    saveCurrentBookReaderSettings();
    ensureReaderSdFontLoaded(renderer);
    if (!restorePreviewPosition) {
      GUI.drawPopup(renderer, tr(STR_INDEXING));
      prepareCurrentSectionForRelayout();
      section.reset();
      // The newly selected SD font can still hold glyph and advance-table caches from
      // the previous page. Release them before the relayout so the parser gets a
      // contiguous allocation window instead of reporting a memory failure as an
      // invalid book. The renderer reloads the active font lazily while parsing.
      releaseReaderSdFontCachesForLowMemory(renderer, "ERS", "reader setting reindex");
    }
  }
  if (restorePreviewPosition) {
    restoreSavedPosition();
    return;
  }
  requestUpdate();
}

void EpubReaderActivity::openFileTransfer() {
  pauseReadingPaceTimer("file_transfer");
  if (epub && section) {
    saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages());
  }

  activityManager.goToFileTransfer(epub ? epub->getPath() : std::string{});
}

void EpubReaderActivity::openAutoPageTurnIntervalPicker(const bool ignoreInitialConfirmRelease,
                                                        const bool returnToReaderMenu) {
  pauseReadingPaceTimer("auto_turn_interval");
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "EpubReaderAutoPageTurnInterval", StrId::STR_AUTO_TURN_INTERVAL_SECONDS,
          getAutoPageTurnIntervalSeconds(), MIN_AUTO_PAGE_TURN_INTERVAL_S, MAX_AUTO_PAGE_TURN_INTERVAL_S, 1, 5,
          StrId::STR_NONE_OPT, /*readerActivity=*/true,
          /*allowPowerAsConfirm=*/true, ignoreInitialConfirmRelease,
          /*showPercentValue=*/false, StrId::STR_NONE_OPT,
          /*overrideDisabledReaderTouchscreen=*/true,
          /*showTouchHeaderBackButton=*/false, /*valueFormatter=*/nullptr, /*tapStep=*/5),
      [this, returnToReaderMenu](const ActivityResult& result) {
        if (!result.isCancelled) {
          setAutoPageTurnIntervalSeconds(static_cast<uint16_t>(std::get<IntervalResult>(result.data).value));
        } else {
          resumeReadingPaceTimer("auto_turn_interval_cancel");
        }
        if (returnToReaderMenu && mappedInput.hasTouchHardware())
          openReaderMenu();
        else
          requestUpdate();
      });
}

void EpubReaderActivity::startClipSelection(const DictionaryClippingRequest* dictionaryRequest,
                                            const bool ignoreInitialBackRelease) {
  if (!section || !epub) {
    requestUpdate();
    return;
  }

  ReaderViewportLayout layout{};
  ClipWordStore wordStore;
  int readerFontId = 0;
  int startPage = 0;
  std::string bookTitle;
  std::string author;
  std::string chapterTitle;
  std::unique_ptr<ClipAdvanceCollector> advanceCollector;
  uint8_t clipAdvanceCapLoggedStyles = 0;
  uint32_t clippingLayoutSignature = clippingWordLayoutSignature(activeSectionLayoutSignature);

  MemoryBudget::logHeapShape("clip.before");

  {
    RenderLock lock(*this);
    if (!section || !epub) {
      requestUpdate();
      return;
    }

    layout = computeReaderViewportLayout(renderer, automaticPageTurnActive);
    readerFontId = activeSectionFontId > 0 ? activeSectionFontId : SETTINGS.getReaderFontId();
    startPage = section->currentPage;
    if (renderer.isSdCardFont(readerFontId)) {
      // Four 256-entry buckets (~4.2 KB plus reusable RTL strings) cannot fit
      // on the reader task stack. They live only while clipping pages are scanned.
      advanceCollector = makeUniqueNoThrow<ClipAdvanceCollector>();
      if (!advanceCollector) {
        LOG_ERR("CLIP", "OOM: clipping advance collector (%u bytes)",
                static_cast<unsigned>(sizeof(ClipAdvanceCollector)));
        section->currentPage = startPage;
        drawToast(renderer, tr(STR_MEMORY_ERROR));
        requestUpdate();
        return;
      }
    }
    const int lineHeight = renderer.getLineHeight(readerFontId);
    const int pagesToLoad = std::min(3, section->pageCount - startPage);
    static constexpr size_t MAX_CLIP_SELECTION_WORDS = 240;
    static constexpr uint32_t CLIP_SELECTION_WORD_RESERVE_HEADROOM = 16U * 1024U;
    static constexpr size_t CLIP_SELECTION_INITIAL_TEXT_RESERVE = 4U * 1024U;
    // Page density varies with font size and layout. Keep the fixed memory cap,
    // but do not mistake the old 80-words-per-page reserve estimate for a limit.
    const size_t maxSelectableWords = pagesToLoad > 0 ? MAX_CLIP_SELECTION_WORDS : 0;
    const uint32_t wordReserveBytes = static_cast<uint32_t>(maxSelectableWords * sizeof(WordRef));
    const auto heapBeforeWords = MemoryBudget::snapshot();
    if (heapBeforeWords.maxAllocHeap < wordReserveBytes + CLIP_SELECTION_WORD_RESERVE_HEADROOM) {
      LOG_ERR("CLIP", "Low heap for clipping selection (%u free, %u max alloc, need block %u); skipping",
              heapBeforeWords.freeHeap, heapBeforeWords.maxAllocHeap,
              wordReserveBytes + CLIP_SELECTION_WORD_RESERVE_HEADROOM);
      section->currentPage = startPage;
      requestUpdate();
      return;
    }
    wordStore.words.reserve(maxSelectableWords);
    wordStore.textPool.reserve(CLIP_SELECTION_INITIAL_TEXT_RESERVE);
    bool wordLimitLogged = false;
    bool textPoolLimitLogged = false;
    bool textPoolFull = false;

    for (int pageIdx = 0; pageIdx < pagesToLoad; ++pageIdx) {
      if (textPoolFull) break;
      section->currentPage = startPage + pageIdx;
      auto page = section->loadPage(section->currentPage);
      if (!page) break;

      if (advanceCollector) {
        advanceCollector->reset();
        forEachPageTextLine(*page, [&](const PageTextLine& line) {
          const auto& block = *line.block;
          for (uint16_t i = 0; i < block.wordCount(); ++i) {
            const uint8_t style = static_cast<uint8_t>(block.wordStyle(i)) & 0x03;
            const char* wordText = block.wordText(i);
            advanceCollector->addLogicalText(style, wordText);
            if ((advanceCollector->truncatedStyles & static_cast<uint8_t>(1U << style)) == 0) {
              if (renderer.collectSdCardFontShapedRtlCodepoints(
                      wordText, advanceCollector->codepoints[style], advanceCollector->counts[style],
                      CLIP_ADVANCE_CODEPOINT_CAPACITY, advanceCollector->rtlTokenScratch,
                      advanceCollector->rtlVisualScratch)) {
                advanceCollector->truncatedStyles |= static_cast<uint8_t>(1U << style);
              }
            }
          }
          return true;
        });
        for (uint8_t style = 0; style < ClipAdvanceCollector::STYLE_COUNT; ++style) {
          if ((advanceCollector->truncatedStyles & static_cast<uint8_t>(1U << style)) &&
              (clipAdvanceCapLoggedStyles & static_cast<uint8_t>(1U << style)) == 0) {
            LOG_ERR("CLIP", "SD-font advance bucket cap hit for style %u; using glyph-miss fallback", style);
            clipAdvanceCapLoggedStyles |= static_cast<uint8_t>(1U << style);
          }
          if (advanceCollector->counts[style] > 0) {
            renderer.ensureSdCardFontReady(readerFontId, advanceCollector->codepoints[style],
                                           advanceCollector->counts[style], false, false,
                                           static_cast<uint8_t>(1U << style));
          }
        }
        MemoryBudget::logHeapShape("clip.after_advance_batch");
      }

      // The pool keeps each word NUL-terminated, so estimate the page once
      // and grow once before collecting it instead of reallocating per word.
      // If the current page alone exceeds the selection cap, retain a bounded
      // window around its middle instead of the leading words. That keeps the
      // initial cursor aligned with button-driven dictionary selection.
      size_t pageTextBytes = 0;
      const size_t remainingWords = maxSelectableWords - wordStore.words.size();
      size_t pageSelectableWords = 0;
      forEachPageTextLine(*page, [&](const PageTextLine& line) {
        const auto& block = *line.block;
        for (uint16_t i = 0; i < block.wordCount(); ++i) {
          const char* wordText = block.wordText(i);
          if (!hasVisibleWordText(wordText)) continue;
          if (pageWordGeometry(renderer, readerFontId, line, block, i).width > 0) ++pageSelectableWords;
        }
        return true;
      });
      size_t firstWordToKeep = 0;
      size_t wordsToKeep = 0;
      if (dictionaryRequest) {
        // Dictionary requests name stable page-local word ordinals. Retain the
        // requested interval instead of the leading page words, so a dense
        // first page cannot evict a valid endpoint on the following page.
        const bool pageIsInRequestedRange =
            pageIdx >= dictionaryRequest->firstPageOffset && pageIdx <= dictionaryRequest->lastPageOffset;
        if (pageIsInRequestedRange) {
          firstWordToKeep = pageIdx == dictionaryRequest->firstPageOffset ? dictionaryRequest->firstPageWordOrdinal : 0;
          const size_t endWordExclusive = pageIdx == dictionaryRequest->lastPageOffset
                                              ? static_cast<size_t>(dictionaryRequest->lastPageWordOrdinal) + 1U
                                              : pageSelectableWords;
          if (firstWordToKeep < pageSelectableWords && firstWordToKeep < endWordExclusive) {
            const size_t requestedWords = std::min(endWordExclusive, pageSelectableWords) - firstWordToKeep;
            wordsToKeep = std::min(remainingWords, requestedWords);
          }
        }
      } else {
        firstWordToKeep =
            pageIdx == 0 && pageSelectableWords > remainingWords ? (pageSelectableWords - remainingWords) / 2 : 0;
        wordsToKeep = std::min(remainingWords, pageSelectableWords - firstWordToKeep);
      }
      size_t selectableWordIndex = 0;
      size_t estimatedWords = 0;
      forEachPageTextLine(*page, [&](const PageTextLine& line) {
        if (estimatedWords >= wordsToKeep) return false;
        const auto& block = *line.block;
        for (uint16_t i = 0; i < block.wordCount() && estimatedWords < wordsToKeep; ++i) {
          const char* wordText = block.wordText(i);
          if (!hasVisibleWordText(wordText)) continue;
          if (pageWordGeometry(renderer, readerFontId, line, block, i).width <= 0) continue;
          if (selectableWordIndex++ < firstWordToKeep) continue;

          const size_t wordBytes = strlen(wordText) + 1;
          if (wordBytes > ClipWordStore::MAX_TEXT_POOL_BYTES - pageTextBytes) {
            pageTextBytes = ClipWordStore::MAX_TEXT_POOL_BYTES;
            break;
          }
          pageTextBytes += wordBytes;
          estimatedWords++;
        }
        return estimatedWords < wordsToKeep;
      });
      const size_t remainingTextBytes = ClipWordStore::MAX_TEXT_POOL_BYTES - wordStore.textPool.size();
      wordStore.textPool.reserve(wordStore.textPool.size() + std::min(pageTextBytes, remainingTextBytes));

      size_t pageWordIndex = 0;
      forEachPageTextLine(*page, [&](const PageTextLine& line) {
        if (textPoolFull) return false;
        const auto& block = *line.block;
        const uint16_t count = block.wordCount();
        for (uint16_t i = 0; i < count; ++i) {
          const char* wordText = block.wordText(i);
          if (!hasVisibleWordText(wordText)) continue;

          const auto textStyle = static_cast<EpdFontFamily::Style>(block.wordStyle(i) & ~EpdFontFamily::UNDERLINE);
          const PageWordGeometry geometry = pageWordGeometry(renderer, readerFontId, line, block, i);
          if (geometry.width <= 0) continue;
          const size_t wordIndexOnPage = pageWordIndex++;
          if (wordIndexOnPage < firstWordToKeep) continue;
          if (wordIndexOnPage >= firstWordToKeep + wordsToKeep) break;
          if (wordStore.words.size() >= maxSelectableWords) {
            if (!wordLimitLogged) {
              LOG_ERR("CLIP", "Selectable word cap hit (%u words); clipping range truncated",
                      static_cast<unsigned>(maxSelectableWords));
              wordLimitLogged = true;
            }
            break;
          }

          WordRef word;
          word.x = layout.marginLeft + line.xPos + geometry.xOffset;
          word.y = layout.marginTop + line.yPos;
          word.w = geometry.width;
          word.h = line.lineHeight > 0 ? line.lineHeight : lineHeight;
          word.pageIdx = pageIdx;
          word.pageWordIndex = static_cast<uint16_t>(wordIndexOnPage);
          word.tableSelection = line.tableSelection;
          if (!wordStore.appendText(word, wordText)) {
            if (!textPoolLimitLogged) {
              LOG_ERR("CLIP", "Selectable text pool reached its 64 KB limit; clipping range truncated");
              textPoolLimitLogged = true;
            }
            textPoolFull = true;
            break;
          }
          word.style = textStyle;
          word.endsWithInsertedHyphen = block.wordEndsWithInsertedHyphen(i);
          word.lineIsRtl = block.getBlockStyle().isRtl;
          wordStore.words.push_back(word);
        }
        return !textPoolFull && wordStore.words.size() < maxSelectableWords;
      });
      MemoryBudget::logHeapShape("clip.after_word_store");
    }

    section->currentPage = startPage;

    auto endsWithHyphen = [&wordStore](const WordRef& word) {
      const char* text = wordStore.text(word);
      return word.textLength > 0 && text[word.textLength - 1] == '-';
    };
    const int indentThreshold = renderer.getLineHeight(readerFontId) / 2;
    int previousLineFirstIdx = -1;
    for (int i = 0; i < static_cast<int>(wordStore.words.size()); ++i) {
      const bool newLine = i == 0 || wordStore.words[i].pageIdx != wordStore.words[i - 1].pageIdx ||
                           wordStore.words[i].y != wordStore.words[i - 1].y;
      if (!newLine) continue;

      const bool byEmSpace = hasEmSpacePrefix(wordStore.text(wordStore.words[i]));
      const bool byIndent = !byEmSpace && previousLineFirstIdx >= 0 &&
                            wordStore.words[i].x > wordStore.words[previousLineFirstIdx].x + indentThreshold &&
                            !endsWithHyphen(wordStore.words[i - 1]);
      if (byEmSpace || byIndent) {
        wordStore.words[i].paragraphStart = true;
      }
      previousLineFirstIdx = i;
    }

    const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex >= 0) {
      chapterTitle = epub->getTocItem(tocIndex).title;
    }
    bookTitle = epub->getTitle();
    author = epub->getAuthor();
  }

  if (wordStore.words.empty()) {
    LOG_ERR("CLIP", "No selectable words on current EPUB page");
    requestUpdate();
    return;
  }
  if (dictionaryRequest) {
    const auto hasRequestedWord = [&wordStore](const uint8_t pageOffset, const uint16_t wordOrdinal) {
      return std::any_of(wordStore.words.begin(), wordStore.words.end(),
                         [pageOffset, wordOrdinal](const WordRef& word) {
                           return word.pageIdx == pageOffset && word.pageWordIndex == wordOrdinal;
                         });
    };
    if (!hasRequestedWord(dictionaryRequest->firstPageOffset, dictionaryRequest->firstPageWordOrdinal) ||
        !hasRequestedWord(dictionaryRequest->lastPageOffset, dictionaryRequest->lastPageWordOrdinal)) {
      LOG_ERR("CLIP", "Dictionary clipping range exceeded selection storage (%u:%u..%u:%u)",
              static_cast<unsigned>(dictionaryRequest->firstPageOffset),
              static_cast<unsigned>(dictionaryRequest->firstPageWordOrdinal),
              static_cast<unsigned>(dictionaryRequest->lastPageOffset),
              static_cast<unsigned>(dictionaryRequest->lastPageWordOrdinal));
      drawToast(renderer, tr(STR_CLIPPING_FAILED));
      requestUpdate();
      return;
    }
  }

  advanceCollector.reset();
  pauseReadingPaceTimer("clip_selection");
  auto clipSelection = makeUniqueNoThrow<ClipSelectionActivity>(
      renderer, mappedInput, std::move(wordStore), readerFontId, *section, startPage, layout.marginTop,
      layout.marginLeft, dictionaryRequest, ignoreInitialBackRelease);
  if (!clipSelection) {
    LOG_ERR("CLIP", "OOM: failed to allocate clip selection activity");
    resumeReadingPaceTimer("clip_selection_alloc_failed");
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(clipSelection), [this, bookTitle = std::move(bookTitle), author = std::move(author),
                                                    chapterTitle = std::move(chapterTitle),
                                                    clippingLayoutSignature](const ActivityResult& result) {
    MemoryBudget::logHeapShape("clip.child_destroyed");
    const char* clippingFeedback = nullptr;
    bool saved = false;
    if (!result.isCancelled) {
      const auto& clip = std::get<ClippingResult>(result.data);
      if (!clip.text.empty()) {
        const size_t clippingIndex = CLIPPINGS.clippingCount();
        const auto addResult = CLIPPINGS.addClipping(
            static_cast<uint16_t>(currentSpineIndex), clip.sectionPage, clip.endSectionPage, clip.sectionPageCount,
            clip.startPageWordIndex, clip.endPageWordIndex, clip.wordCount, chapterTitle.c_str(), clip.paragraphIndex,
            clip.text, clip.tableSelection, clippingLayoutSignature);
        bool exported = false;
        if (addResult == ClippingStore::AddResult::Added) {
          exported = ClippingsManager::saveClipping(bookTitle, author, chapterTitle,
                                                    static_cast<int>(clip.sectionPage) + 1, clip.text);
          if (!exported && !CLIPPINGS.removeClippingAt(clippingIndex)) {
            LOG_ERR("CLIP", "Failed to roll back clipping after export failure");
          }
        }
        saved = addResult == ClippingStore::AddResult::Added && exported;
        clippingFeedback = addResult == ClippingStore::AddResult::LimitReached ? tr(STR_CLIPPING_LIMIT_REACHED)
                           : saved                                             ? tr(STR_CLIPPING_SAVED)
                                                                               : tr(STR_CLIPPING_FAILED);
      }
    }
    resumeReadingPaceTimer("clip_selection_return");
    releaseReaderSdFontCachesForLowMemory(renderer, "CLIP", "clipping selection exit");
    MemoryBudget::logHeapShape("clip.after_font_release");
    pendingHeapShapeReaderRedrawStages.fetch_or(HEAP_SHAPE_REDRAW_CLIP, std::memory_order_relaxed);
    if (clippingFeedback) {
#if CROSSINK_APP_CAP_TOUCH
      if (saved && mappedInput.hasTouchHardware() && requestUpdateAndWait() != RequestUpdateResult::Rendered) {
        LOG_ERR("CLIP", "Could not render saved highlight before clipping toast");
      }
#endif
      {
        RenderLock lock(*this);
        drawToast(renderer, clippingFeedback);
      }
      delay(1000);
    }
    requestUpdate();
  });
}

void EpubReaderActivity::resetReadingPaceData() {
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  const uint16_t oldAvg = stats.avgSecondsPerForwardPage;
  const uint16_t oldCount = stats.paceSampleCount;
#endif
  stats.avgSecondsPerForwardPage = 0;
  stats.paceSampleCount = 0;
  stats.estimatedTimeLeftSeconds = 0;
  sessionPaceSampleSeconds = 0;
  sessionPaceSampleCount = 0;
  armReadingPaceWarmup("reading_pace_reset");
  if (epub) {
    epub->setupCacheDir();
    stats.save(epub->getCachePath());
  }
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  LOG_DBG("ERS", "Reading pace reset: avg=%u->%u samples=%u->%u totalReadingSeconds=%lu totalPagesTurned=%lu", oldAvg,
          stats.avgSecondsPerForwardPage, oldCount, stats.paceSampleCount,
          static_cast<unsigned long>(stats.totalReadingSeconds), static_cast<unsigned long>(stats.totalPagesTurned));
#endif
}

void EpubReaderActivity::resetCurrentBookStatsAfterDelete() {
  stats = BookReadingStats{};
  sessionReadingSeconds = 0;
  sessionPaceSampleSeconds = 0;
  sessionPaceSampleCount = 0;
  pendingReadFolderMove = false;
  hasSessionStartLocalDateTime = getCurrentLocalReadingStatsDateTime(sessionStartLocalDateTime);
  armReadingPaceWarmup("book_stats_delete");
  initializeCompletionPromptTrigger();
}

void EpubReaderActivity::executeReaderQuickAction(CrossPointSettings::LONG_PRESS_MENU_ACTION action,
                                                  const bool dictionaryLookupFramebufferContainsPage,
                                                  const QuickLockTrigger quickLockTrigger) {
  switch (action) {
    case CrossPointSettings::LONG_MENU_SLEEP:
      enterDeepSleep();
      break;
    case CrossPointSettings::LONG_MENU_CHANGE_FONT: {
      const CrossPointSettings::FONT_SIZE effectiveSize = SETTINGS.getEffectiveReaderFontSize();
      SETTINGS.fontFamily = (SETTINGS.fontFamily + 1) % CrossPointSettings::FONT_FAMILY_COUNT;
      SETTINGS.sdFontFamilyName[0] = '\0';
      SETTINGS.readerFontPointSize = CrossPointSettings::getReaderFontPointSize(effectiveSize);
      reindexCurrentSection();
      break;
    }
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
      prepareManualRefresh();
      requestUpdate();
      break;
    case CrossPointSettings::LONG_MENU_SYNC_PROGRESS:
      if (KOREADER_STORE.hasCredentials()) {
        onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::SYNC);
      } else {
        pauseReadingPaceTimer("koreader_settings");
        startActivityForResult(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) {
                                 resumeReadingPaceTimer("koreader_settings_return");
                                 saveGlobalSettingsPreservingBookOverrides();
                               });
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
      openAutoPageTurnIntervalPicker(mappedInput.isPressed(MappedInputManager::Button::Confirm));
      break;
    case CrossPointSettings::LONG_MENU_FILE_TRANSFER:
      openFileTransfer();
      break;
    case CrossPointSettings::LONG_MENU_CALIBRE_WIRELESS:
      activityManager.goToCalibreWireless(epub ? epub->getPath() : "");
      break;
    case CrossPointSettings::LONG_MENU_JOIN_NETWORK:
      activityManager.goToJoinNetworkFileTransfer(epub ? epub->getPath() : "");
      break;
    case CrossPointSettings::LONG_MENU_CREATE_HOTSPOT:
      activityManager.goToHotspotFileTransfer(epub ? epub->getPath() : "");
      break;
    case CrossPointSettings::LONG_MENU_TOGGLE_TILT_PAGE_TURN:
      if (halTiltSensor.isAvailable()) {
        SETTINGS.tiltPageTurn = SETTINGS.tiltPageTurn == CrossPointSettings::TILT_OFF ? CrossPointSettings::TILT_ON
                                                                                      : CrossPointSettings::TILT_OFF;
        saveGlobalSettingsPreservingBookOverrides();
        halTiltSensor.clearPendingEvents();
        showTiltPageTurnFeedback(SETTINGS.tiltPageTurn != CrossPointSettings::TILT_OFF);
        requestUpdate();
      }
      break;
    case CrossPointSettings::LONG_MENU_TOGGLE_DARK_MODE: {
      {
        RenderLock lock(*this);
        SETTINGS.screenInverted = !SETTINGS.screenInverted;
        saveGlobalSettingsPreservingBookOverrides();
      }
      requestUpdate();
      break;
    }
    case CrossPointSettings::LONG_MENU_FOOTNOTES:
      executeFootnoteQuickAction();
      break;
    case CrossPointSettings::LONG_MENU_FILE_BROWSER:
      activityManager.goToFileBrowser(epub ? epub->getPath() : "");
      break;
    case CrossPointSettings::LONG_MENU_CREATE_CLIPPING:
      startClipSelection();
      break;
    case CrossPointSettings::LONG_MENU_LOOKUP_WORD:
      if (epub && Dictionary::exists(epub->getCachePath().c_str())) {
        openWordSelect(dictionaryLookupFramebufferContainsPage);
      } else {
        drawToast(renderer, tr(STR_DICT_NO_DICT_SET));
        delay(1000);
        requestUpdate();
      }
      break;
    case CrossPointSettings::LONG_MENU_QUICK_ACTIONS:
      openQuickActionsPopup();
      break;
    case CrossPointSettings::LONG_MENU_QUICK_LOCK:
      handleGlobalPowerButtonAction(CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK, quickLockTrigger);
      break;
    case CrossPointSettings::LONG_MENU_OFF:
    default:
      break;
  }
}

bool EpubReaderActivity::handleShortcutAction(const uint8_t rawAction) {
  const auto action = static_cast<CrossPointSettings::SHORT_PWRBTN>(rawAction);
  switch (action) {
    case CrossPointSettings::SHORT_PWRBTN::IGNORE:
      return true;
    case CrossPointSettings::SHORT_PWRBTN::PAGE_TURN:
      pageTurn(true, "home_button");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::PREVIOUS_PAGE:
      pageTurn(false, "home_button");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::NEARBY_POSITION_SYNC:
      onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::NEARBY_POSITION_SYNC);
      return true;
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
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CALIBRE_WIRELESS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_JOIN_NETWORK);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CREATE_HOTSPOT);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TILT_PAGE_TURN:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_TILT_PAGE_TURN);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_DARK_MODE:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_DARK_MODE);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FOOTNOTES:
      executeFootnoteQuickAction();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_FILE_BROWSER);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CREATE_CLIPPING:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CREATE_CLIPPING);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::LOOKUP_WORD:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_LOOKUP_WORD);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_HOME_BUTTON_IN_READER:
      toggleHomeButtonInReader();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS:
      openQuickActionsPopup();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FRONTLIGHT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TOUCHSCREEN:
      return handleGlobalPowerButtonAction(action);
    case CrossPointSettings::SHORT_PWRBTN::SLEEP:
    case CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH:
    case CrossPointSettings::SHORT_PWRBTN::SHORT_PWRBTN_COUNT:
      return false;
  }
  return false;
}

bool EpubReaderActivity::handleShortcutAction(const CrossPointSettings::SHORT_PWRBTN action) {
  switch (action) {
    case CrossPointSettings::SHORT_PWRBTN::PAGE_TURN:
      pageTurn(true, "shortcut");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::PREVIOUS_PAGE:
      pageTurn(false, "shortcut");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::NEARBY_POSITION_SYNC:
      onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::NEARBY_POSITION_SYNC);
      return true;
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
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_DARK_MODE:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_DARK_MODE);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_FILE_BROWSER);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CALIBRE_WIRELESS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_JOIN_NETWORK);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CREATE_HOTSPOT);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CREATE_CLIPPING:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CREATE_CLIPPING);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::LOOKUP_WORD:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_LOOKUP_WORD);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FOOTNOTES:
      executeFootnoteQuickAction();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_HOME_BUTTON_IN_READER:
      toggleHomeButtonInReader();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS:
      openQuickActionsPopup();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FRONTLIGHT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TOUCHSCREEN:
      return handleGlobalPowerButtonAction(action);
    default:
      return false;
  }
}

void EpubReaderActivity::openQuickActionsPopup() {
  QuickActions::showConfiguredPopup(
      quickActionsPopup, [this] { requestUpdate(); },
      [this](const auto action) {
        mappedInput.setReaderTouchscreenOverride(false);
        if (action == CrossPointSettings::SHORT_PWRBTN::LOOKUP_WORD) {
          // The popup was the most recent render, so word selection must redraw
          // the reader page instead of reusing the popup framebuffer.
          executeReaderQuickAction(CrossPointSettings::LONG_MENU_LOOKUP_WORD,
                                   /*dictionaryLookupFramebufferContainsPage=*/false);
          return;
        }
        if (action == CrossPointSettings::SHORT_PWRBTN::SYNC_PROGRESS && KOREADER_STORE.hasCredentials()) {
          onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::SYNC);
          return;
        }
        dispatchShortcutAction(action);
      });
  if (quickActionsPopup.isActive()) {
    mappedInput.setReaderTouchscreenOverride(true);
    quickActionsPopup.setCancelCallback([this] { mappedInput.setReaderTouchscreenOverride(false); });
  }
}

void EpubReaderActivity::executeFootnoteQuickAction(const bool suppressInitialPowerRelease) {
  if (footnoteDepth > 0 && SETTINGS.pwrBtnFootnoteBack) {
    restoreSavedPosition();
    return;
  }

  if (currentPageFootnotes.size() == 1) {
    navigateToHref(currentPageFootnotes[0].href, true);
    return;
  }

  if (currentPageFootnotes.size() > 1) {
    if (suppressInitialPowerRelease) {
      suppressPowerShortcutRelease();
    }
    pauseReadingPaceTimer("footnotes");
    startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                               navigateToHref(footnoteResult.href, true);
                             } else {
                               resumeReadingPaceTimer("footnotes_cancel");
                             }
                             requestUpdate();
                           });
  }
}

bool EpubReaderActivity::executeShortPowerButtonAction() {
  if (!mappedInput.wasReleased(MappedInputManager::Button::Power) ||
      mappedInput.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration()) {
    return false;
  }

  switch (SETTINGS.shortPwrBtn) {
    case CrossPointSettings::SHORT_PWRBTN::NEARBY_POSITION_SYNC:
      onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::NEARBY_POSITION_SYNC);
      return true;
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
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CALIBRE_WIRELESS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_JOIN_NETWORK);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CREATE_HOTSPOT);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TILT_PAGE_TURN:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_TILT_PAGE_TURN);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_DARK_MODE:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_DARK_MODE);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FOOTNOTES:
      executeFootnoteQuickAction();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_FILE_BROWSER);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CREATE_CLIPPING:
      mappedInput.suppressNextPowerConfirmRelease();
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CREATE_CLIPPING);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::LOOKUP_WORD:
      mappedInput.suppressNextPowerConfirmRelease();
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_LOOKUP_WORD);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_HOME_BUTTON_IN_READER:
      toggleHomeButtonInReader();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS:
      openQuickActionsPopup();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FRONTLIGHT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TOUCHSCREEN:
      return handleGlobalPowerButtonAction(static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn));
    default:
      return false;
  }
}

bool EpubReaderActivity::consumeLongPowerButtonRelease() {
  if (!longPowerButtonHandled) {
    return false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Power) ||
      !mappedInput.isPressed(MappedInputManager::Button::Power)) {
    longPowerButtonHandled = false;
    return true;
  }

  return false;
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
  if ((SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN ||
       SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PREVIOUS_PAGE) ||
      !consumeLongPowerButtonHold()) {
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
      mappedInput.suppressNextPowerConfirmRelease();
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_SYNC_PROGRESS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::NEARBY_POSITION_SYNC:
      mappedInput.suppressNextPowerConfirmRelease();
      onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction::NEARBY_POSITION_SYNC);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::MARK_FINISHED:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_MARK_FINISHED);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::READING_STATS:
      mappedInput.suppressNextPowerConfirmRelease();
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_READING_STATS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::SCREENSHOT:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_SCREENSHOT);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CYCLE_PAGE_TURN:
      mappedInput.suppressNextPowerConfirmRelease();
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CYCLE_PAGE_TURN);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_FILE_TRANSFER);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CALIBRE_WIRELESS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_JOIN_NETWORK);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CREATE_HOTSPOT);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TILT_PAGE_TURN:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_TILT_PAGE_TURN);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_DARK_MODE:
      mappedInput.suppressNextPowerRelease();
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_DARK_MODE);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FOOTNOTES:
      executeFootnoteQuickAction(/*suppressInitialPowerRelease=*/true);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_FILE_BROWSER);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CREATE_CLIPPING:
      mappedInput.suppressNextPowerConfirmRelease();
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CREATE_CLIPPING);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::LOOKUP_WORD:
      mappedInput.suppressNextPowerConfirmRelease();
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_LOOKUP_WORD);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_HOME_BUTTON_IN_READER:
      toggleHomeButtonInReader();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS:
      openQuickActionsPopup();
      return true;
    default:
      return false;
  }
}

void EpubReaderActivity::suppressPowerShortcutRelease() {
  mappedInput.suppressNextPowerRelease();
  mappedInput.suppressNextPowerConfirmRelease();
}

void EpubReaderActivity::setBookCompleted(bool isCompleted) {
  if (stats.isCompleted == isCompleted) {
    return;
  }

  stats.isCompleted = isCompleted;
  if (isCompleted && !stats.finishedDateManual) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) {
      stats.finishedDate = now.date;
    }
  }
  if (isCompleted) {
    completionPromptShown = true;
    if (SETTINGS.removeReadBooksFromRecents) {
      RECENT_BOOKS.removeByPath(epub->getPath());
    }
    if (SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath())) {
      pendingReadFolderMove = true;
    }
  } else {
    if (SETTINGS.removeReadBooksFromRecents) {
      RECENT_BOOKS.addOrUpdateBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
    }
    recentsEntryRemoved = false;
    pendingReadFolderMove = false;
  }
  if (isCompleted) {
    globalStats.completedBooks++;
  } else if (globalStats.completedBooks > 0) {
    globalStats.completedBooks--;
  }

  refreshCachedTimeLeftEstimate();
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
  homeButtonInReaderFeedback = false;
  pendingTiltPageTurnFeedback = true;
  tiltPageTurnFeedbackShowTime = millis();
}

void EpubReaderActivity::toggleHomeButtonInReader() {
  if (!gpio.hasHomeKey()) return;
  SETTINGS.homeButtonInReaderEnabled = SETTINGS.homeButtonInReaderEnabled ? 0 : 1;
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("ERS", "Failed to save Home button reader setting");
  }
  mappedInput.clearDeferredHomeGesture();
  tiltPageTurnFeedbackEnabled = SETTINGS.homeButtonInReaderEnabled;
  homeButtonInReaderFeedback = true;
  pendingTiltPageTurnFeedback = true;
  tiltPageTurnFeedbackShowTime = millis();
  requestUpdate();
}

void EpubReaderActivity::showRenderModeToast(const uint8_t renderMode) {
  if (normalizeRenderMode(renderMode) == EpubRenderMode::CrossInkDefault) {
    return;
  }
  renderModeToastMode = normalizeRenderModeRaw(renderMode);
  pendingRenderModeToast = true;
  pendingSafeModeToast = false;
  renderModeToastShown = true;
  renderModeToastShowTime = millis();
}

void EpubReaderActivity::showSafeModeToast() {
  pendingSafeModeToast = true;
  pendingRenderModeToast = false;
  safeModeToastShown = true;
  renderModeToastShown = true;
  renderModeToastShowTime = millis();
}

bool EpubReaderActivity::storeRenderModeToastRegion(const char* msg) {
  renderModeToastRegionSaved = false;
  const ToastRect toast = computeToastRect(renderer, msg);
  const size_t needed = renderer.getRegionByteSize(toast.x, toast.y, toast.w, toast.h);
  if (needed == 0) {
    return false;
  }
  if (!renderModeToastRegionBuffer || renderModeToastRegionBufferSize < needed) {
    renderModeToastRegionBuffer = makeUniqueNoThrow<uint8_t[]>(needed);
    if (!renderModeToastRegionBuffer) {
      renderModeToastRegionBufferSize = 0;
      LOG_ERR("ERS", "OOM: render-mode toast region backup (%u bytes)", static_cast<unsigned>(needed));
      return false;
    }
    renderModeToastRegionBufferSize = needed;
  }
  if (!renderer.copyRegionToBuffer(toast.x, toast.y, toast.w, toast.h, renderModeToastRegionBuffer.get(),
                                   renderModeToastRegionBufferSize)) {
    return false;
  }
  renderModeToastRegion = toast;
  renderModeToastRegionSaved = true;
  return true;
}

void EpubReaderActivity::drawRenderModeToastBuffer(const char* msg) {
  storeRenderModeToastRegion(msg);
  drawToastBuffer(renderer, msg);
}

bool EpubReaderActivity::restoreRenderModeToastRegion() {
  if (!renderModeToastRegionSaved || !renderModeToastRegionBuffer) {
    return false;
  }
  const bool restored = renderer.copyBufferToRegion(renderModeToastRegion.x, renderModeToastRegion.y,
                                                    renderModeToastRegion.w, renderModeToastRegion.h,
                                                    renderModeToastRegionBuffer.get(), renderModeToastRegionBufferSize);
  renderModeToastRegionSaved = false;
  renderModeToastRegionBuffer.reset();
  renderModeToastRegionBufferSize = 0;
  if (!restored) {
    return false;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  return true;
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
      prepareCurrentSectionForRelayout();
    }

    if (settingsChanged) {
      // Persist the selection for this book without changing global reader defaults.
      SETTINGS.orientation = orientation;
      saveCurrentBookReaderSettings();
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
  bookHasAutoPageTurnInterval = true;
  if (epub) {
    BookReaderSettingsData data = loadBookReaderSettingsFile(epub->getCachePath());
    captureReaderSettings(data.readerSettings);
    initialBookReaderSettings.hasAutoPageTurnInterval = true;
    initialBookReaderSettings.autoPageTurnSeconds = seconds;
    initialBookReaderSettings.hasCustomReaderSettings = bookHasCustomReaderSettings;
    initialBookReaderSettings.hasRenderModeOverride = bookHasRenderModeOverride;
    initialBookReaderSettings.renderMode = SETTINGS.epubRenderMode;
    data.hasAutoPageTurnInterval = true;
    data.autoPageTurnSeconds = seconds;
    data.hasCustomReaderSettings = bookHasCustomReaderSettings;
    data.hasRenderModeOverride = bookHasRenderModeOverride;
    data.renderMode = SETTINGS.epubRenderMode;
    saveBookReaderSettingsFile(epub->getCachePath(), data);
  }
  lastPageTurnTime = millis();
  pageTurnDuration = static_cast<unsigned long>(seconds) * 1000UL;
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  // resets cached section so that space is reserved for auto page turn indicator when None or progress bar only
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    prepareCurrentSectionForRelayout();
    section.reset();
  }
}

void EpubReaderActivity::pageTurn(bool isForwardTurn, const char* source) {
  pageLoadRetryCount = 0;
  if (activeFootnotePreview) {
    if (isForwardTurn) {
      if (section && section->currentPage < section->pageCount - 1) {
        section->currentPage++;
      }
    } else {
      armReadingPaceWarmup("preview_back_page");
      if (section && section->currentPage > 0) {
        section->currentPage--;
      } else if (source && strcmp(source, "touch") == 0) {
        restoreSavedPosition();
        return;
      }
    }
    lastPageTurnTime = millis();
    requestUpdate();
    return;
  }
  if (isForwardTurn) {
    uint32_t forwardReadSeconds = 0;
    const bool shouldRecordForwardRead = forwardPageReadElapsed(forwardReadSeconds, source);
    recordCurrentPageReadingTime(source);
    const bool exitingChapter =
        section && !section->isBuilding() && section->pageCount > 0 && section->currentPage >= section->pageCount - 1;
    // Advance within the section while there are (or may still be) more pages: either a built
    // page ahead, or the section is still building (windowed), in which case more pages exist
    // beyond the current watermark and render()'s ensure-built pump will lay them out. Only when
    // the section is fully built AND we're on its last page do we move to the next spine -- using
    // the live pageCount alone would mistake the build watermark for the end of a giant spine.
    if (section->currentPage < section->pageCount - 1 || section->isBuilding() || section->isPartial()) {
      section->currentPage++;
    } else {
      if (shouldQueueCompletionPromptOnChapterExit()) {
        completionPromptQueued = true;
        requestUpdate();
        return;
      }

      // We don't want to delete the section mid-render, so grab the semaphore
      {
        RenderLock lock(*this);
        nextPageNumber = 0;
        currentSpineIndex++;
        section.reset();
      }
    }
    if (shouldRecordForwardRead) {
      if (!exitingChapter) {
        recordForwardPagePaceSample(forwardReadSeconds, source);
      }
      stats.totalPagesTurned++;
      globalStats.totalPagesTurned++;
    }
  } else {
    recordCurrentPageReadingTime(source);
    armReadingPaceWarmup("back_page");
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
  lastPageTurnTime = millis();
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  // The render task now owns the mutex requested by the input action. Background
  // indexing may resume only after this render releases it.
  backgroundBuildYieldForInput.store(false, std::memory_order_relaxed);
  if (!epub) {
    return;
  }
  if (quickActionsPopup.processRender(renderer, mappedInput)) {
    return;
  }

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  const auto queueLowMemoryLayoutAlert = [this](const bool goHomeOnBack) {
    snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s", tr(STR_EPUB_LAYOUT_MEMORY_TITLE));
    snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), "%s", tr(STR_EPUB_LAYOUT_MEMORY_BODY));
    APP_STATE.pendingAlertGoHomeOnBack.store(goHomeOnBack, std::memory_order_relaxed);
    APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
  };

  const auto showLowMemoryLayoutError = [this, &queueLowMemoryLayoutAlert]() {
    queueLowMemoryLayoutAlert(true);
    GUI.drawPopup(renderer, tr(STR_EPUB_LAYOUT_MEMORY_TITLE));
  };

  // A section build failure (e.g. an invalid/corrupt EPUB that fails XML parsing) leaves the
  // "Indexing" popup on screen with no way forward. Surface an explicit error instead of hanging.
  // clearScreen first so the error popup doesn't overlay the stale "Indexing" popup.
  const auto showBuildError = [this]() {
    renderer.clearScreen(ReaderUtils::readerBackgroundColor());
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
  };

  const auto showIndexingPopup = [this]() {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    pagesUntilFullRefresh = 1;
  };

  bool buildCancelledForBack = false;
  const auto cancelBuildForBack = [this, &buildCancelledForBack]() {
    if (!sectionBuildCancelRequested.load(std::memory_order_relaxed)) {
      return false;
    }
    if (section && section->isBuilding()) {
      section->suspendBuild();
    }
    buildCancelledForBack = true;
    automaticPageTurnActive = false;
    LOG_DBG("ERS", "EPUB section build cancelled by Back");
    return true;
  };

  // A freshly restarted reader has a less fragmented heap than one that has just
  // built and displayed several pages. Try that once before changing layout modes;
  // the RTC token marks the resumed attempt so it proceeds through the fallbacks.
  const auto restartForLowMemoryLayout = [this](const int targetPage, const int savedPage, const int pageCount,
                                                const char* stage) {
    if (lowMemoryPartialRestartAttempted || targetPage < 0 || targetPage >= std::numeric_limits<uint16_t>::max() ||
        currentSpineIndex < 0 || currentSpineIndex > std::numeric_limits<uint16_t>::max()) {
      return false;
    }
    LOG_ERR("ERS", "Low heap during %s; silent restarting to retry page %d (spine=%d)", stage, targetPage,
            currentSpineIndex);
    if (!saveProgress(currentSpineIndex, std::max(0, savedPage), std::max(0, pageCount))) {
      LOG_ERR("ERS", "Skipping silent restart because progress save failed");
      return false;
    }
    armSilentRestartReaderPageBuild(epub->getPath(), static_cast<uint16_t>(currentSpineIndex),
                                    static_cast<uint16_t>(targetPage), automaticPageTurnActive);
    lowMemoryPartialRestartAttempted = true;
    silentRestartToReader();
    return true;
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
    if (!endOfBookOptions) {
      endOfBookOptions = makeUniqueNoThrow<EndOfBookOptions>(renderer);
      if (!endOfBookOptions) {
        LOG_ERR("ERS", "OOM: EndOfBookOptions (%u bytes)", static_cast<unsigned>(sizeof(EndOfBookOptions)));
      }
    }
    renderer.clearScreen();
    if (endOfBookOptions) {
      endOfBookOptions->loadOnce(epub->getPath());
      endOfBookOptions->render(renderer, mappedInput);
    }
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  const ReaderViewportLayout layout = computeReaderViewportLayout(
      renderer, automaticPageTurnActive, activeFootnotePreview || !pendingFootnotePreviewAnchor.empty());
  const uint16_t viewportWidth = layout.viewportWidth;
  const uint16_t viewportHeight = layout.viewportHeight;
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  if (!section) {
    preparedNextSpineIndex = -1;
    // Section loading/indexing can need a large contiguous block. Return the
    // render-only strip before starting that work.
    releaseGrayscaleStripScratch();
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d (free=%u, maxAlloc=%u)", filepath.c_str(), currentSpineIndex,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    const int readerFontId = SETTINGS.getReaderFontId();
    const EpubRenderMode selectedRenderMode = normalizeRenderMode(SETTINGS.epubRenderMode);
    const bool fullSectionIndexing = SETTINGS.indexingMethod == CrossPointSettings::INDEXING_FULL_SECTION;
    EpubRenderMode usedRenderMode = selectedRenderMode;
    const bool buildingFootnotePreview = !pendingFootnotePreviewAnchor.empty();
    bool loadedSection = false;
    bool safeModeBuildSucceeded = false;
    partialRebuildStartFailed = false;
    partialRebuildAbortedForLowMemory = false;
    auto loadSectionWithFont = [&](const int fontId, const EpubRenderMode renderMode) {
      const std::string cacheSuffix = buildingFootnotePreview
                                          ? footnotePreviewCacheSuffix(renderMode, pendingFootnotePreviewAnchor)
                                          : std::string(sectionCacheSuffixForRenderMode(renderMode));
      section = makeUniqueNoThrow<Section>(epub, currentSpineIndex, renderer, cacheSuffix.c_str());
      if (!section) {
        LOG_ERR("ERS", "Failed to allocate section for spine %d (font=%d, free=%u, maxAlloc=%u)", currentSpineIndex,
                fontId, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        return false;
      }
      const ReaderRenderSpec spec =
          readerRenderSpecForProfile(fontId, viewportWidth, viewportHeight, buildProfileForRenderMode(renderMode));
      if (!section->loadSectionFile(spec)) {
        section.reset();
        return false;
      }
      activeSectionFontId = fontId;
      activeSectionLayoutSignature = readerRenderSpecSignature(spec);
      usedRenderMode = renderMode;
      return true;
    };

    loadedSection = loadSectionWithFont(readerFontId, selectedRenderMode);
    if (loadedSection && !pendingRelayoutReposition) {
      cachedChapterTotalPageCount = 0;
    }
    const bool partialCacheLoaded = loadedSection && section && section->isPartial();

    if (!loadedSection || partialCacheLoaded) {
      if (!loadedSection) {
        // Font selection previews can leave glyph and advance-table caches resident.
        // Drop them before allocating a replacement layout after a render-spec mismatch.
        releaseReaderSdFontCachesForLowMemory(renderer, "ERS", "uncached section build");
      }
      if (partialCacheLoaded) {
        LOG_DBG("ERS", "Partial cache found (%u pages), resuming build... (free=%u, maxAlloc=%u)", section->pageCount,
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      } else {
        LOG_DBG("ERS", "Cache not found, building... (free=%u, maxAlloc=%u)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      }

      const auto popupFn = [this]() {
        if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
      };

      bool imagesWereSuppressed = false;
      bool layoutAbortedForLowMemory = false;
      bool fallbackBuildSucceeded = false;
      bool usedReadablePartialFallback = false;
      EpubRenderMode lastAttemptedRenderMode = selectedRenderMode;
      bool lastAttemptUsedSafeMode = false;
      std::unique_ptr<Section> readablePartialFallback;
      if (fullSectionIndexing && partialCacheLoaded) {
        // Keep the known-readable partial alive until its full replacement has
        // been built and atomically promoted. A failed full build must not turn
        // a mode change or reopen into an invalid-book dead end.
        readablePartialFallback = std::move(section);
      }
      auto buildSectionWithProfile = [&](const int fontId, const SectionBuildProfile& profile) {
        lastAttemptedRenderMode = profile.renderMode;
        lastAttemptUsedSafeMode = profile.safeMode;
        const std::string cacheSuffix =
            buildingFootnotePreview ? footnotePreviewCacheSuffix(profile.renderMode, pendingFootnotePreviewAnchor)
                                    : std::string(sectionCacheSuffixForRenderMode(profile.renderMode));
        const bool reuseLoadedPartial = !fullSectionIndexing && section && section->isPartial() &&
                                        fontId == activeSectionFontId && profile.renderMode == usedRenderMode;
        if (!reuseLoadedPartial) {
          section.reset();
          section = makeUniqueNoThrow<Section>(epub, currentSpineIndex, renderer, cacheSuffix.c_str());
          if (!section) {
            LOG_ERR("ERS", "Failed to allocate %s section builder for spine %d (free=%u, maxAlloc=%u)", profile.label,
                    currentSpineIndex, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
            layoutAbortedForLowMemory = true;
            return false;
          }
        }

        bool attemptImagesWereSuppressed = false;
        bool attemptLayoutAbortedForLowMemory = false;
        const SectionBuildOptions buildOptions{
            buildingFootnotePreview ? pendingFootnotePreviewAnchor.c_str() : nullptr,
            static_cast<uint16_t>(buildingFootnotePreview ? FOOTNOTE_PREVIEW_MAX_PAGES : 0)};
        const ReaderRenderSpec spec = readerRenderSpecForProfile(fontId, viewportWidth, viewportHeight, profile);
        const bool needsFullBuild = fullSectionIndexing || buildingFootnotePreview || pendingPercentJump ||
                                    pendingClippingIndex != UINT16_MAX || pendingParagraphIndex != UINT16_MAX;
        bool buildSucceeded = false;
        if (needsFullBuild) {
          showIndexingPopup();
          {
            GfxRenderer::FrameBufferLoan loan(renderer);
            buildSucceeded = section->createSectionFile(spec, popupFn, &attemptImagesWereSuppressed,
                                                        &attemptLayoutAbortedForLowMemory, buildOptions);
          }
        } else {
          const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          const bool willInflate = !section->hasHtmlCache();
          const bool anchorJump = !pendingAnchor.empty();
          const auto anchorPageReady = [&]() {
            const auto page = section->findAnchor(pendingAnchor);
            return page.has_value() &&
                   (static_cast<int>(*page) < static_cast<int>(section->pageCount) || section->isBuildComplete());
          };
          const bool deferPartialBuild =
              section->isPartial() &&
              (anchorJump ? anchorPageReady()
                          : target + PARTIAL_REBUILD_START_MARGIN < static_cast<int>(section->pageCount));
          if (deferPartialBuild) {
            LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
            buildSucceeded = true;
          } else {
            bool showPopup = false;
            if (anchorJump) {
              showPopup = !anchorPageReady() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
            } else {
              const bool targetAvailable = target < static_cast<int>(section->pageCount);
              showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                               target > BUILD_POPUP_PAGE_THRESHOLD);
            }
            if (showPopup) {
              showIndexingPopup();
            }
            buildPopupPending = !showPopup;
            const unsigned long buildStartMs = millis();
            bool started;
            {
              GfxRenderer::FrameBufferLoan loan(renderer);
              started = section->startBuild(spec, buildOptions, [this] { showBuildPopup(); });
            }
            if (started) {
              bool buildFailed = false;
              while (!section->isBuildComplete() &&
                     (anchorJump                  ? !anchorPageReady()
                      : pendingRelayoutReposition ? !isRelayoutCatchUpComplete()
                                                  : static_cast<int>(section->pageCount) <= target)) {
                if (cancelBuildForBack()) {
                  break;
                }
                if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
                  showBuildPopup();
                }
                if (!section->buildSomeMore(INTERACTIVE_BUILD_PAGES_PER_CHUNK)) {
                  LOG_ERR("ERS", "Failed during incremental section build");
                  buildFailed = true;
                  break;
                }
              }
              if (!buildFailed && pendingRelayoutReposition && section->isBuilding() && isRelayoutCatchUpComplete()) {
                LOG_DBG("ERS", "Incremental relayout reached prior watermark: pages=%u target=%d", section->pageCount,
                        cachedChapterPageWatermark);
              }
              attemptImagesWereSuppressed = attemptImagesWereSuppressed || section->lastBuildImagesWereSuppressed();
              attemptLayoutAbortedForLowMemory =
                  attemptLayoutAbortedForLowMemory || section->lastBuildLayoutAbortedForLowMemory();
              const bool requestedPageAvailable = anchorJump ? anchorPageReady()
                                                  : pendingRelayoutReposition
                                                      ? isRelayoutCatchUpComplete()
                                                      : target >= 0 && target < static_cast<int>(section->pageCount);
              if (buildCancelledForBack) {
                buildFailed = false;
              }
              if (buildFailed && attemptLayoutAbortedForLowMemory && requestedPageAvailable) {
                LOG_ERR("ERS", "Incremental section build paused for low heap after reaching requested page");
                attemptLayoutAbortedForLowMemory = false;
                buildFailed = false;
              }
              buildSucceeded =
                  buildCancelledForBack || (!buildFailed && (section->pageCount > 0 || section->isBuildComplete()));
            } else {
              attemptImagesWereSuppressed = attemptImagesWereSuppressed || section->lastBuildImagesWereSuppressed();
              attemptLayoutAbortedForLowMemory =
                  attemptLayoutAbortedForLowMemory || section->lastBuildLayoutAbortedForLowMemory();
            }
            buildPopupPending = false;
          }
        }
        imagesWereSuppressed = imagesWereSuppressed || attemptImagesWereSuppressed;
        layoutAbortedForLowMemory = attemptLayoutAbortedForLowMemory;
        if (buildSucceeded) {
          activeSectionFontId = fontId;
          activeSectionLayoutSignature = readerRenderSpecSignature(spec);
          usedRenderMode = profile.renderMode;
          safeModeBuildSucceeded = profile.safeMode;
          LOG_DBG("ERS",
                  "%s section cache built: spine=%d font=%d mode=%u embedded=%u bionic=%u guide=%u pages=%u free=%u "
                  "maxAlloc=%u building=%u",
                  profile.label, currentSpineIndex, fontId, static_cast<unsigned>(profile.renderMode),
                  static_cast<unsigned>(profile.embeddedStyle), static_cast<unsigned>(profile.bionicReadingEnabled),
                  static_cast<unsigned>(profile.guideReadingEnabled), section->pageCount, ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap(), section->isBuilding() ? 1U : 0U);
        }
        return buildSucceeded;
      };

      auto buildWithFallback = [&](const SectionBuildProfile& profile) {
        layoutAbortedForLowMemory = false;
        const bool succeeded = buildSectionWithProfile(readerFontId, profile);
        return SectionBuildAttempt{succeeded, layoutAbortedForLowMemory};
      };
      auto beforeFallbackRetry = [&](const SectionBuildProfile& profile) {
        if (profile.safeMode) {
          LOG_ERR("ERS", "EPUB section layout aborted for low heap; retrying Safe Mode");
          releaseReaderSdFontCachesForLowMemory(renderer, "ERS", "safe mode section rebuild");
          return;
        }
        LOG_ERR("ERS", "EPUB section layout aborted for low heap; retrying mode %u",
                static_cast<unsigned>(profile.renderMode));
        releaseReaderSdFontCachesForLowMemory(renderer, "ERS", "fallback section rebuild");
      };
      const SectionBuildAttempt initialAttempt = buildWithFallback(buildProfileForRenderMode(selectedRenderMode));
      const bool canRestartInitialLayout = !buildingFootnotePreview && !pendingPercentJump && pendingAnchor.empty() &&
                                           pendingClippingIndex == UINT16_MAX && pendingParagraphIndex == UINT16_MAX;
      if (!initialAttempt.succeeded && initialAttempt.lowMemory && canRestartInitialLayout) {
        const int target = pendingPageJump.has_value() ? *pendingPageJump : std::max(0, nextPageNumber);
        const int savedPage =
            section && section->pageCount > 0 ? std::min(target, static_cast<int>(section->pageCount) - 1) : target;
        const int estimatedPages = section ? section->estimatedTotalPages() : 0;
        if (restartForLowMemoryLayout(target, savedPage, estimatedPages, "initial EPUB layout")) {
          return;
        }
      }
      const SectionFallbackResult fallbackResult = runSectionBuildFallbacks(
          selectedRenderMode, shouldAttemptSafeModeFallback(), buildWithFallback, beforeFallbackRetry, &initialAttempt);
      fallbackBuildSucceeded = fallbackResult.succeeded;
      layoutAbortedForLowMemory = fallbackResult.lastAttemptLowMemory;

      if (buildCancelledForBack) {
        return;
      }

      if (!fallbackBuildSucceeded && readablePartialFallback && !buildingFootnotePreview && !pendingPercentJump &&
          pendingClippingIndex == UINT16_MAX && pendingParagraphIndex == UINT16_MAX && !pendingRelayoutReposition) {
        const int target = pendingPageJump.has_value() ? *pendingPageJump : std::max(0, nextPageNumber);
        bool targetAvailable = target < static_cast<int>(readablePartialFallback->pageCount);
        if (!pendingAnchor.empty()) {
          const auto anchorPage = readablePartialFallback->findAnchor(pendingAnchor);
          targetAvailable = anchorPage.has_value() && *anchorPage < readablePartialFallback->pageCount;
        }
        if (targetAvailable) {
          LOG_ERR("ERS", "Full-section build failed; retaining readable partial cache (%u pages)",
                  readablePartialFallback->pageCount);
          section.reset();
          section = std::move(readablePartialFallback);
          fallbackBuildSucceeded = true;
          usedReadablePartialFallback = true;
          layoutAbortedForLowMemory = false;
        }
      }

      if (!fallbackBuildSucceeded && layoutAbortedForLowMemory && section && section->isPartial() &&
          section->pageCount > 0 && !buildingFootnotePreview && !pendingPercentJump && pendingAnchor.empty() &&
          pendingClippingIndex == UINT16_MAX && pendingParagraphIndex == UINT16_MAX && !pendingRelayoutReposition) {
        LOG_ERR("ERS", "Incremental build stopped for low heap; retaining readable partial cache (%u pages)",
                section->pageCount);
        fallbackBuildSucceeded = true;
        usedReadablePartialFallback = true;
        partialRebuildAbortedForLowMemory = true;
        activeSectionFontId = readerFontId;
        usedRenderMode = lastAttemptedRenderMode;
        safeModeBuildSucceeded = lastAttemptUsedSafeMode;
        queueLowMemoryLayoutAlert(false);
      }

      if (!fallbackBuildSucceeded) {
        if (layoutAbortedForLowMemory) {
          LOG_ERR("ERS", "EPUB section layout aborted for low heap; chapter exceeds safe layout memory");
        }
        if (!layoutAbortedForLowMemory) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
        }
        const bool shouldRestoreFootnoteOrigin = buildingFootnotePreview && footnoteDepth > 0;
        section.reset();
        if (buildingFootnotePreview) {
          pendingFootnotePreviewAnchor.clear();
          activeFootnotePreview = false;
        }
        if (shouldRestoreFootnoteOrigin) {
          footnoteDepth--;
          const SavedPosition& origin = savedPositions[footnoteDepth];
          LOG_ERR("ERS", "Footnote preview build failed; restoring origin spine %d page %d", origin.spineIndex,
                  origin.pageNumber);
          pendingAnchor.clear();
          pendingPageJump.reset();
          currentSpineIndex = origin.spineIndex;
          nextPageNumber = origin.pageNumber;
          armReadingPaceWarmup("footnote_preview_failed_restore");
          requestUpdate();
          return;
        }
        if (layoutAbortedForLowMemory) {
          showLowMemoryLayoutError();
        } else {
          showBuildError();
        }
        return;
      }
      if (!section->isBuilding()) {
        releaseReaderSdFontCachesForLowMemory(renderer, "ERS", "section cache build");
      }
      if (usedReadablePartialFallback) {
        LOG_DBG("ERS", "Continuing from readable partial cache after failed build: pages=%u free=%u maxAlloc=%u",
                section->pageCount, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      } else {
        LOG_DBG("ERS", "Cache build complete: pages=%u font=%d mode=%u free=%u maxAlloc=%u", section->pageCount,
                activeSectionFontId, static_cast<unsigned>(usedRenderMode), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      }

      if (!buildingFootnotePreview && safeModeBuildSucceeded) {
        applySafeModeReaderSettings();
        bookHasCustomReaderSettings = true;
        bookHasRenderModeOverride = true;
        if (!saveRuntimeReaderSettingsForCache(epub->getCachePath())) {
          LOG_ERR("ERS", "Failed to save Safe Mode reader settings");
        }
      }

      if (!buildingFootnotePreview && imagesWereSuppressed) {
        snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s",
                 tr(STR_LOW_MEMORY_IMAGES_TITLE));
        snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), "%s", tr(STR_LOW_MEMORY_IMAGES_BODY));
        APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
        APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build... (pages=%u, font=%d mode=%u free=%u, maxAlloc=%u)",
              section->pageCount, activeSectionFontId, static_cast<unsigned>(usedRenderMode), ESP.getFreeHeap(),
              ESP.getMaxAllocHeap());
    }

    activeFootnotePreview = buildingFootnotePreview;

    if (!buildingFootnotePreview && usedRenderMode != selectedRenderMode && !safeModeBuildSucceeded) {
      SETTINGS.epubRenderMode = static_cast<uint8_t>(usedRenderMode);
      bookHasRenderModeOverride = true;
      saveBookRenderModeForCache(epub->getCachePath(), SETTINGS.epubRenderMode);
    }
    const bool renderModeChangedDuringLoad = usedRenderMode != selectedRenderMode;
    if (!buildingFootnotePreview && safeModeBuildSucceeded && !safeModeToastShown) {
      showSafeModeToast();
    } else if (!buildingFootnotePreview && renderModeChangedDuringLoad &&
               usedRenderMode != EpubRenderMode::CrossInkDefault && !renderModeToastShown) {
      showRenderModeToast(static_cast<uint8_t>(usedRenderMode));
    }

    if (!section) {
      LOG_ERR("ERS", "Section load/build did not produce a section");
      showPendingSyncSaveError();
      return;
    }

    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump;
      if (pendingClippingIndex != UINT16_MAX && pendingClippingIndex < CLIPPINGS.clippingCount()) {
        const Clipping* clipping = CLIPPINGS.clippingAt(pendingClippingIndex);
        const uint16_t fallbackPage = static_cast<uint16_t>(std::max(0, section->currentPage));
        std::string clippingText;
        clippingText.reserve(CLIPPING_TEXT_MAX);
        if (clipping) CLIPPINGS.readClippingText(*clipping, clippingText);
        section->currentPage =
            clipping ? resolveClippingJumpPage(*section, *clipping, clippingText, fallbackPage) : fallbackPage;
        LOG_DBG("ERS", "Resolved clipping %u to page %d", pendingClippingIndex, section->currentPage);
      } else if (pendingParagraphIndex != UINT16_MAX) {
        const uint16_t fallbackPage = static_cast<uint16_t>(std::max(0, section->currentPage));
        section->currentPage = resolveParagraphJumpPage(*section, pendingParagraphIndex, fallbackPage);
        LOG_DBG("ERS", "Resolved paragraph %u to page %d", pendingParagraphIndex, section->currentPage);
      }
      pendingClippingIndex = UINT16_MAX;
      pendingParagraphIndex = UINT16_MAX;
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) {
        section->currentPage = 0;
      }
    }

    if (!pendingAnchor.empty()) {
      // Resolve from the pages laid out so far and/or the on-disk map (finalized or partial).
      const auto page = section->findAnchor(pendingAnchor);
      if (page && static_cast<int>(*page) < static_cast<int>(section->pageCount)) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      } else if (page) {
        LOG_DBG("ERS", "Anchor '%s' resolved to unavailable page %d in section %d (pages=%u)", pendingAnchor.c_str(),
                *page, currentSpineIndex, section->pageCount);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found in section %d", pendingAnchor.c_str(), currentSpineIndex);
      }
      pendingAnchor.clear();
      pendingFootnotePreviewAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      // Apply the pending percent jump now that we know the new section's page count.
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) {
        newPage = section->pageCount - 1;
      }
      section->currentPage = newPage;
      if (pendingClippingIndex != UINT16_MAX && pendingClippingIndex < CLIPPINGS.clippingCount()) {
        const Clipping* clipping = CLIPPINGS.clippingAt(pendingClippingIndex);
        const uint16_t fallbackPage = static_cast<uint16_t>(std::max(0, section->currentPage));
        std::string clippingText;
        clippingText.reserve(CLIPPING_TEXT_MAX);
        if (clipping) CLIPPINGS.readClippingText(*clipping, clippingText);
        section->currentPage =
            clipping ? resolveClippingJumpPage(*section, *clipping, clippingText, fallbackPage) : fallbackPage;
        LOG_DBG("ERS", "Resolved clipping %u to page %d", pendingClippingIndex, section->currentPage);
      } else if (pendingParagraphIndex != UINT16_MAX) {
        const uint16_t fallbackPage = static_cast<uint16_t>(std::max(0, section->currentPage));
        section->currentPage = resolveParagraphJumpPage(*section, pendingParagraphIndex, fallbackPage);
        LOG_DBG("ERS", "Resolved paragraph %u to page %d", pendingParagraphIndex, section->currentPage);
      }
      pendingClippingIndex = UINT16_MAX;
      pendingParagraphIndex = UINT16_MAX;
      pendingPercentJump = false;
    }

    // Keep negative page numbers in bounds now. Upper-bound clamping waits until after
    // lazy build catch-up because pageCount may still be only a partial-build watermark.
    if (section->currentPage < 0) {
      section->currentPage = 0;
    }
  }

  // Extend the build to the requested page if needed. This covers a partial cache that is
  // already loaded but not actively building; pages already available do no work here.
  const int requestedPageBeforeCatchUp = section->currentPage;
  if (!activeFootnotePreview && partialRebuildAbortedForLowMemory && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    const bool shouldSilentRestartForPartialLowMemory =
        !lowMemoryPartialRestartAttempted && section->lastBuildLayoutAbortedForLowMemory() &&
        section->currentPage == static_cast<int>(section->pageCount) &&
        section->pageCount < std::numeric_limits<uint16_t>::max() && currentSpineIndex >= 0 &&
        currentSpineIndex <= std::numeric_limits<uint16_t>::max();
    if (shouldSilentRestartForPartialLowMemory) {
      const uint16_t lastReadablePage = section->pageCount - 1;
      const uint16_t targetPage = section->pageCount;
      const int estimatedPages = section->estimatedTotalPages();
      if (restartForLowMemoryLayout(targetPage, lastReadablePage, estimatedPages, "partial EPUB layout")) {
        return;
      }
    }
    LOG_ERR("ERS", "Requested page %d exceeds low-memory partial watermark %u; showing last readable page",
            section->currentPage, section->pageCount);
    section->currentPage = section->pageCount - 1;
  }
  if (!activeFootnotePreview && section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    showIndexingPopup();
  }
  const bool needsBlockingCatchUp = section->currentPage >= static_cast<int>(section->pageCount) &&
                                    ((!activeFootnotePreview && section->isPartial()) || section->isBuilding());
  bool catchUpCancelled = false;
  bool catchUpFailed = false;
  const auto runBlockingCatchUp = [&]() {
    while (!activeFootnotePreview && section->isPartial() &&
           section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->isBuilding()) {
        const int renderFontId = activeSectionFontId != 0 ? activeSectionFontId : SETTINGS.getReaderFontId();
        const SectionBuildProfile profile = buildProfileForRenderMode(normalizeRenderMode(SETTINGS.epubRenderMode));
        if (!section->startBuild(readerRenderSpecForProfile(renderFontId, viewportWidth, viewportHeight, profile))) {
          LOG_ERR("ERS", "Failed to start partial extension build");
          catchUpFailed = true;
          return;
        }
      }
      while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
        if (cancelBuildForBack()) {
          catchUpCancelled = true;
          return;
        }
        if (!section->buildSomeMore(INTERACTIVE_BUILD_PAGES_PER_CHUNK)) {
          LOG_ERR("ERS", "Failed during incremental section build");
          catchUpFailed = true;
          return;
        }
      }
    }

    if (section->isBuilding()) {
      while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
        if (cancelBuildForBack()) {
          catchUpCancelled = true;
          return;
        }
        if (!section->buildSomeMore(INTERACTIVE_BUILD_PAGES_PER_CHUNK)) {
          LOG_ERR("ERS", "Failed during incremental section build");
          catchUpFailed = true;
          return;
        }
      }
    }
  };

  if (needsBlockingCatchUp) {
    // The panel keeps its current image, and the page below is redrawn from
    // scratch. Let miniz use the framebuffer while this catch-up blocks.
    releaseGrayscaleStripScratch();
    GfxRenderer::FrameBufferLoan loan(renderer);
    runBlockingCatchUp();
  }
  if (catchUpCancelled) {
    return;
  }
  if (catchUpFailed) {
    if (section->lastBuildLayoutAbortedForLowMemory() && section->pageCount > 0) {
      partialRebuildAbortedForLowMemory = true;
      const int partialWatermark = static_cast<int>(section->pageCount);
      if (requestedPageBeforeCatchUp == partialWatermark &&
          restartForLowMemoryLayout(partialWatermark, partialWatermark - 1, section->estimatedTotalPages(),
                                    "partial EPUB catch-up")) {
        return;
      }
      section->currentPage = std::min(section->currentPage, static_cast<int>(section->pageCount) - 1);
      LOG_ERR("ERS", "Blocking build stopped for low heap; retaining %u readable partial pages", section->pageCount);
      queueLowMemoryLayoutAlert(false);
    } else {
      section.reset();
      showBuildError();
      return;
    }
  }

  // The requested page is now as built as it will get. Clamp any past-the-end target
  // against the final count, including the UINT16_MAX "last page" sentinel.
  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  applyDeferredReposition();

  if (!activeFootnotePreview) {
    refreshChapterGroupEstimate(viewportWidth, viewportHeight);
  }

  renderer.clearScreen(ReaderUtils::readerBackgroundColor());

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, ReaderUtils::messageCenterY(renderer), tr(STR_EMPTY_CHAPTER),
                              ReaderUtils::readerForegroundBlack(), EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, ReaderUtils::messageCenterY(renderer), tr(STR_OUT_OF_BOUNDS),
                              ReaderUtils::readerForegroundBlack(), EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  {
    // Unified page read: the in-progress build's in-RAM table if it has reached the page,
    // otherwise the on-disk file (finalized section, or a partial from a previous session).
    auto p = section->loadPage(section->currentPage);
    if (!p) {
      pageLoadRetryCount++;
      if (pageLoadRetryCount <= MAX_PAGE_LOAD_RETRIES) {
        LOG_ERR("ERS", "Failed to load page from SD (retry %d) - clearing section cache", pageLoadRetryCount);
        section->abandonBuild();
        section->clearCache();
        section.reset();
        requestUpdate();
        automaticPageTurnActive = false;
        showPendingSyncSaveError();
        return;
      }

      LOG_ERR("ERS", "Failed to load page from SD after %d retries", pageLoadRetryCount);
      pageLoadRetryCount = 0;
      renderer.clearScreen(ReaderUtils::readerBackgroundColor());
      renderer.drawCenteredText(UI_12_FONT_ID, ReaderUtils::messageCenterY(renderer), tr(STR_PAGE_LOAD_ERROR),
                                ReaderUtils::readerForegroundBlack(), EpdFontFamily::BOLD);
      renderStatusBar();
      renderer.displayBuffer();
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;

    // Preview pages are transient note windows, not full chapter pages with reusable footnote metadata.
    if (activeFootnotePreview) {
      currentPageFootnotes.clear();
    } else {
      currentPageFootnotes = std::move(p->footnotes);
    }

    const int renderFontId = activeSectionFontId != 0 ? activeSectionFontId : SETTINGS.getReaderFontId();
    if (!renderContents(std::move(p), renderFontId, layout.marginTop, layout.marginRight, layout.marginBottom,
                        layout.marginLeft, /*updatePanel=*/true)) {
      currentPageFootnotes.clear();
#if CROSSINK_APP_CAP_TOUCH
      currentPageFootnoteTouchTargets.fill({});
#endif
      renderer.clearScreen(ReaderUtils::readerBackgroundColor());
      GUI.drawPopup(renderer, renderer.isSdCardFont(renderFontId) ? tr(STR_MEMORY_ERROR) : tr(STR_PAGE_LOAD_ERROR));
      renderer.displayBuffer();
      automaticPageTurnActive = false;
      showPendingSyncSaveError();
      return;
    }
    lastRenderCompleteMs = millis();
    const uint8_t heapShapeRedrawStages = pendingHeapShapeReaderRedrawStages.exchange(0, std::memory_order_relaxed);
    if (heapShapeRedrawStages & HEAP_SHAPE_REDRAW_CLIP) {
      MemoryBudget::logHeapShape("clip.reader_redrawn");
    }
    if (heapShapeRedrawStages & HEAP_SHAPE_REDRAW_DICT) {
      MemoryBudget::logHeapShape("dict.reader_redrawn");
    }
    pageShownAtMs = activeFootnotePreview ? 0UL : millis();
  }
  if (!activeFootnotePreview) {
    const int totalPages = section->estimatedTotalPages();
    // render() also runs on menu/bookmark/screenshot re-renders. Avoid repeating
    // the same progress.bin write unless the rendered position or estimated total changed.
    if (progressSaveRequiredAfterRelayout || currentSpineIndex != lastSavedSpineIndex ||
        section->currentPage != lastSavedPage || totalPages != lastSavedPageCount) {
      if (!queueProgressSave(currentSpineIndex, section->currentPage, totalPages, progressSaveRequiredAfterRelayout)) {
        pendingSyncSaveError = true;
      } else if (progressSaveRequiredAfterRelayout) {
        progressSaveRequiredAfterRelayout = false;
      }
    }
    silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
    queueCompletionPromptIfNeeded();
  }

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (SETTINGS.indexingMethod != CrossPointSettings::INDEXING_FULL_SECTION || activeFootnotePreview || !epub ||
      !section || section->isBuilding() || section->isPartial() || section->pageCount == 0) {
    return;
  }

  // Start on the penultimate page, including one-page chapters, and catch up
  // after a direct jump to the last page.
  const int triggerPage = section->pageCount > 1 ? section->pageCount - 2 : 0;
  if (section->currentPage < triggerPage) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }
  if (preparedNextSpineIndex == nextSpineIndex && preparedNextViewportWidth == viewportWidth &&
      preparedNextViewportHeight == viewportHeight) {
    return;
  }

  const int readerFontId = SETTINGS.getReaderFontId();
  const EpubRenderMode selectedRenderMode = normalizeRenderMode(SETTINGS.epubRenderMode);
  auto nextSection =
      makeUniqueNoThrow<Section>(epub, nextSpineIndex, renderer, sectionCacheSuffixForRenderMode(selectedRenderMode));
  if (!nextSection) {
    LOG_ERR("ERS", "Failed to allocate next-chapter cache reader for spine %d", nextSpineIndex);
    return;
  }

  if (nextSection->loadSectionFile(readerRenderSpecForProfile(readerFontId, viewportWidth, viewportHeight,
                                                              buildProfileForRenderMode(selectedRenderMode))) &&
      !nextSection->isPartial()) {
    preparedNextSpineIndex = nextSpineIndex;
    preparedNextViewportWidth = viewportWidth;
    preparedNextViewportHeight = viewportHeight;
    return;
  }
  // Close any loaded partial before rebuilding the same cache path. Real SdFat
  // hardware permits only one reader for a file path at a time.
  nextSection.reset();

  releaseGrayscaleStripScratch();
  const bool releasedSdFontCaches =
      releaseReaderSdFontCachesForLowMemory(renderer, "ERS", "preparing silent next-chapter indexing");
  const uint32_t minFreeForPrefetch = releasedSdFontCaches
                                          ? MemoryBudget::OPTIONAL_EPUB_PREFETCH_AFTER_SD_FONT_RELEASE_MIN_FREE
                                          : MemoryBudget::OPTIONAL_EPUB_REBUILD_MIN_FREE;
  if (!MemoryBudget::hasHeapForOptionalEpubRebuild("ERS", "silent next-chapter indexing", nextSpineIndex,
                                                   minFreeForPrefetch,
                                                   MemoryBudget::OPTIONAL_EPUB_REBUILD_MIN_MAX_ALLOC)) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d (free=%u, maxAlloc=%u)", nextSpineIndex, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  silentPrefetchCancelRequested.store(false, std::memory_order_relaxed);
  silentPrefetchBuildActive.store(true, std::memory_order_release);
  struct ClearSilentPrefetchBuildActive {
    std::atomic<bool>& active;
    ~ClearSilentPrefetchBuildActive() { active.store(false, std::memory_order_release); }
  } clearSilentPrefetchBuildActive{silentPrefetchBuildActive};

  bool layoutAbortedForLowMemory = false;
  bool buildSucceeded = false;
  bool safeModeBuildSucceeded = false;
  bool prefetchCancelled = false;
  EpubRenderMode usedRenderMode = selectedRenderMode;

  const auto buildNextSection = [&](const SectionBuildProfile& profile) {
    auto attemptSection =
        makeUniqueNoThrow<Section>(epub, nextSpineIndex, renderer, sectionCacheSuffixForRenderMode(profile.renderMode));
    if (!attemptSection) {
      LOG_ERR("ERS", "Failed to allocate next-chapter section builder for spine %d", nextSpineIndex);
      layoutAbortedForLowMemory = true;
      return false;
    }

    bool attemptAbortedForLowMemory = false;
    bool attemptCancelled = false;
    SectionBuildOptions buildOptions;
    buildOptions.shouldCancel = [](void* context) {
      return static_cast<EpubReaderActivity*>(context)->silentPrefetchCancelRequested.load(std::memory_order_relaxed);
    };
    buildOptions.cancelContext = this;
    buildOptions.cancellationObserved = &attemptCancelled;
    // The page is already on the panel, so lend its framebuffer to miniz while
    // preparing the next chapter. Without this, a large EPUB entry makes the
    // inflater take its workspace from the same constrained heap as layout.
    GfxRenderer::FrameBufferLoan loan(renderer);
    const bool succeeded = attemptSection->createSectionFile(
        readerRenderSpecForProfile(readerFontId, viewportWidth, viewportHeight, profile), nullptr, nullptr,
        &attemptAbortedForLowMemory, buildOptions);
    if (attemptCancelled) {
      prefetchCancelled = true;
      LOG_DBG("ERS", "Silent next-chapter indexing cancelled: chapter=%d", nextSpineIndex);
      return false;
    }
    layoutAbortedForLowMemory = attemptAbortedForLowMemory;
    if (succeeded) {
      usedRenderMode = profile.renderMode;
      LOG_DBG("ERS", "Silent indexing complete: chapter=%d pages=%u mode=%u free=%u maxAlloc=%u", nextSpineIndex,
              attemptSection->pageCount, static_cast<unsigned>(usedRenderMode), ESP.getFreeHeap(),
              ESP.getMaxAllocHeap());
    }
    return succeeded;
  };

  auto buildWithFallback = [&](const SectionBuildProfile& profile) {
    layoutAbortedForLowMemory = false;
    const bool succeeded = buildNextSection(profile);
    return SectionBuildAttempt{succeeded, layoutAbortedForLowMemory};
  };
  auto beforeFallbackRetry = [&](const SectionBuildProfile& profile) {
    releaseReaderSdFontCachesForLowMemory(
        renderer, "ERS",
        profile.safeMode ? "silent next-chapter safe mode indexing" : "silent next-chapter fallback indexing");
  };
  SectionFallbackResult fallbackResult;
  const SectionBuildAttempt initialAttempt = buildWithFallback(buildProfileForRenderMode(selectedRenderMode));
  if (!prefetchCancelled) {
    fallbackResult = runSectionBuildFallbacks(selectedRenderMode, shouldAttemptSafeModeFallback(), buildWithFallback,
                                              beforeFallbackRetry, &initialAttempt);
    buildSucceeded = fallbackResult.succeeded;
    layoutAbortedForLowMemory = fallbackResult.lastAttemptLowMemory;
    safeModeBuildSucceeded = fallbackResult.usedSafeMode;
  }

  releaseReaderSdFontCachesForLowMemory(renderer, "ERS", "silent next-chapter indexing");
  // FrameBufferLoan restores the storage as a blank frame. Rebuild the visible
  // page in RAM without refreshing the panel; the panel still holds the page.
  if (!restoreCurrentPageBufferAfterSilentIndex()) {
    renderer.clearScreen(ReaderUtils::readerBackgroundColor());
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_PAGE_LOAD_ERROR),
                              ReaderUtils::readerForegroundBlack(), EpdFontFamily::BOLD);
    renderer.displayBuffer();
  }

  if (prefetchCancelled) {
    return;
  }

  if (!buildSucceeded) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
    return;
  }

  // A grouped sibling just changed from missing/partial to exact. Re-scan on the
  // next reader render so the tilde can disappear without doing file I/O in the
  // status-bar drawing function itself.
  chapterGroupEstimate.valid = false;

  if (safeModeBuildSucceeded) {
    applySafeModeReaderSettings();
    bookHasCustomReaderSettings = true;
    bookHasRenderModeOverride = true;
    if (!saveRuntimeReaderSettingsForCache(epub->getCachePath())) {
      LOG_ERR("ERS", "Failed to save Safe Mode reader settings after silent indexing");
    }
  } else if (usedRenderMode != selectedRenderMode) {
    SETTINGS.epubRenderMode = static_cast<uint8_t>(usedRenderMode);
    bookHasRenderModeOverride = true;
    if (!saveBookRenderModeForCache(epub->getCachePath(), SETTINGS.epubRenderMode)) {
      LOG_ERR("ERS", "Failed to save render mode after silent indexing");
    }
  }
  preparedNextSpineIndex = nextSpineIndex;
  preparedNextViewportWidth = viewportWidth;
  preparedNextViewportHeight = viewportHeight;
}

bool EpubReaderActivity::restoreCurrentPageBufferAfterSilentIndex() {
  if (!section || section->pageCount == 0 || section->currentPage < 0 ||
      section->currentPage >= static_cast<int>(section->pageCount)) {
    LOG_ERR("ERS", "Cannot restore reader frame after silent indexing: invalid current page");
    return false;
  }

  auto page = section->loadPage(section->currentPage);
  if (!page) {
    LOG_ERR("ERS", "Failed to load current page for silent-index framebuffer restore");
    return false;
  }

  const ReaderViewportLayout layout = computeReaderViewportLayout(renderer, automaticPageTurnActive);
  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  const int renderFontId = activeSectionFontId != 0 ? activeSectionFontId : SETTINGS.getReaderFontId();
  return renderContents(std::move(page), renderFontId, layout.marginTop, layout.marginRight, layout.marginBottom,
                        layout.marginLeft, /*updatePanel=*/false);
}

bool EpubReaderActivity::isRelayoutCatchUpComplete() const {
  if (!pendingRelayoutReposition || !section) {
    return !pendingRelayoutReposition;
  }

  const bool watermarkReached = static_cast<int>(section->pageCount) >= std::max(1, cachedChapterPageWatermark);
  bool positionResolved = static_cast<int>(section->pageCount) > cachedChapterPageNumber;
  if (cachedVisibleTextOffset) {
    positionResolved = section->getPageForVisibleTextOffset(*cachedVisibleTextOffset).has_value();
  } else if (cachedPageParagraphIndex != UINT16_MAX) {
    positionResolved = section->isBuilding() ? section->findParagraphDuringBuild(cachedPageParagraphIndex).has_value()
                                             : section->getPageForParagraphIndex(cachedPageParagraphIndex).has_value();
  }
  return watermarkReached && positionResolved;
}

bool EpubReaderActivity::applyDeferredReposition() {
  if ((!cachedVisibleTextOffset && cachedChapterTotalPageCount == 0) || !section) {
    return false;
  }

  if (section->isBuilding() && !isRelayoutCatchUpComplete()) {
    return false;
  }

  const bool completedRelayout = pendingRelayoutReposition;
  bool changed = false;
  if (currentSpineIndex == cachedSpineIndex) {
    bool restoredFromContent = false;
    if (cachedVisibleTextOffset) {
      if (const auto contentPage = section->getPageForVisibleTextOffset(*cachedVisibleTextOffset, true)) {
        section->currentPage = *contentPage;
        restoredFromContent = true;
        changed = true;
        LOG_DBG("ERS", "Resolved visible text offset %lu to page %d",
                static_cast<unsigned long>(*cachedVisibleTextOffset), section->currentPage);
      }
    }

    bool restoredFromParagraph = false;
    if (!restoredFromContent && cachedPageParagraphIndex != UINT16_MAX) {
      if (const auto paragraphPage = section->getPageForParagraphIndex(cachedPageParagraphIndex)) {
        uint16_t newStartPage = *paragraphPage;
        if (newStartPage >= section->pageCount) {
          newStartPage = section->pageCount > 0 ? section->pageCount - 1 : 0;
        }

        uint16_t newEndPage = newStartPage + 1;
        while (newEndPage < section->pageCount) {
          const auto pIdx = section->getParagraphIndexForPage(newEndPage);
          if (!pIdx || *pIdx != cachedPageParagraphIndex) {
            break;
          }
          newEndPage++;
        }

        const uint16_t newSpan = std::max<uint16_t>(1, newEndPage - newStartPage);
        const uint16_t oldSpan = std::max<uint16_t>(1, cachedPageParagraphSpan);
        const uint16_t oldOffset = std::min<uint16_t>(cachedPageParagraphOffset, oldSpan - 1);
        uint16_t newOffset = 0;
        if (oldSpan > 1 && newSpan > 1) {
          newOffset = static_cast<uint16_t>((static_cast<uint32_t>(oldOffset) * (newSpan - 1) + (oldSpan - 1) / 2) /
                                            (oldSpan - 1));
        }

        section->currentPage = newStartPage + newOffset;
        restoredFromParagraph = true;
        changed = true;
        LOG_DBG("ERS", "Resolved cached paragraph %u offset %u/%u to page %d (span %u)", cachedPageParagraphIndex,
                oldOffset, oldSpan, section->currentPage, newSpan);
      }
    }

    // A zero page count carries no ratio to scale, so proportional repositioning is
    // skipped rather than dividing by it.
    if (!restoredFromContent && !restoredFromParagraph && !section->isBuilding() && cachedChapterTotalPageCount > 0 &&
        section->pageCount != cachedChapterTotalPageCount) {
      const float progress =
          static_cast<float>(cachedChapterPageNumber) / static_cast<float>(cachedChapterTotalPageCount);
      int newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
      if (newPage < 0) newPage = 0;
      if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
        newPage = section->pageCount - 1;
      }
      if (newPage != section->currentPage) {
        section->currentPage = newPage;
        changed = true;
      }
    }
  }

  cachedChapterPageNumber = 0;
  cachedChapterTotalPageCount = 0;
  cachedChapterPageWatermark = 0;
  cachedVisibleTextOffset.reset();
  pendingRelayoutReposition = false;
  cachedPageParagraphIndex = UINT16_MAX;
  cachedPageParagraphOffset = 0;
  cachedPageParagraphSpan = 0;
  if (completedRelayout) {
    progressSaveRequiredAfterRelayout = true;
  }
  return changed;
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount,
                                      const bool allowDuringFootnotePreview) {
  if (activeFootnotePreview && !allowDuringFootnotePreview) {
    return true;
  }
  if (!epub) {
    return false;
  }
  std::optional<uint32_t> visibleTextOffset;
  // A preview section's page table describes the note, not the chapter being saved, so
  // its offsets must never be attached to the forced origin save above.
  if (!activeFootnotePreview && section && spineIndex == currentSpineIndex && currentPage >= 0 &&
      currentPage < section->pageCount) {
    visibleTextOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage));
  }
  if (section && section->isBuilding() && spineIndex == currentSpineIndex) {
    // Free lazy-build cache files before opening progress.bin; X4/SdFat can reject
    // another file open while a still-building EPUB section keeps files open.
    section->releaseBuildFile();
  }
  const bool saved = EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount, visibleTextOffset);
  if (saved) {
    lastSavedSpineIndex = spineIndex;
    lastSavedPage = currentPage;
    lastSavedPageCount = pageCount;
    // Home reads this lightweight value instead of opening the EPUB, so keep it
    // in sync with the position file written above.
    RecentBookProgress::saveCachedEpubPercent(*epub, spineIndex, currentPage, pageCount);
    const uint32_t positionKey = (static_cast<uint32_t>(spineIndex) << 16) | static_cast<uint16_t>(currentPage);
    progressSaveDebouncer.markPersisted(positionKey, static_cast<uint32_t>(pageCount));
  }
  return saved;
}

bool EpubReaderActivity::queueProgressSave(const int spineIndex, const int currentPage, const int pageCount,
                                           const bool forceSave) {
  if (activeFootnotePreview) {
    return true;
  }
  const uint32_t positionKey = (static_cast<uint32_t>(spineIndex) << 16) | static_cast<uint16_t>(currentPage);
  if (!progressSaveDebouncer.observe(positionKey, static_cast<uint32_t>(pageCount)) && !forceSave) {
    return true;
  }
  return saveProgress(spineIndex, currentPage, pageCount);
}

bool EpubReaderActivity::flushQueuedProgress() {
  if (!progressSaveDebouncer.hasPending()) {
    return true;
  }
  // No section check: saveProgress() only consults it opportunistically for the
  // visible-text offset, and end-of-book leaves it null with a write still pending.
  if (!epub) {
    return false;
  }
  const uint32_t positionKey = progressSaveDebouncer.lastObservedPosition();
  const int spineIndex = static_cast<int>(positionKey >> 16);
  const int pageNumber = static_cast<int>(positionKey & 0xFFFFU);
  const int pageCount = static_cast<int>(progressSaveDebouncer.lastObservedMetadata());
  return saveProgress(spineIndex, pageNumber, pageCount);
}

bool EpubReaderActivity::ensureGrayscaleStripScratch() {
  if (!renderer.supportsStripGrayscale()) {
    return false;
  }

  const size_t requiredSize = static_cast<size_t>(renderer.getDisplayWidthBytes()) * GRAYSCALE_STRIP_ROWS;
  if (grayscaleStripScratch && grayscaleStripScratchSize >= requiredSize) {
    return true;
  }

  releaseGrayscaleStripScratch();
  // About 8 KB at 800-pixel width: too large for the render task stack and
  // runtime-sized, so allocate fallibly once and reuse it while the section is stable.
  grayscaleStripScratchInPsram = psramHeapAvailable();
  grayscaleStripScratch =
      grayscaleStripScratchInPsram ? makePsramByteBufferNoThrow(requiredSize) : makeHeapByteBufferNoThrow(requiredSize);
  if (!grayscaleStripScratch && grayscaleStripScratchInPsram) {
    LOG_ERR("ERS", "OOM: PSRAM grayscale strip scratch (%u bytes); trying default heap",
            static_cast<unsigned>(requiredSize));
    grayscaleStripScratchInPsram = false;
    grayscaleStripScratch = makeHeapByteBufferNoThrow(requiredSize);
  }
  if (!grayscaleStripScratch) {
    LOG_ERR("ERS", "OOM: grayscale strip scratch (%u bytes); falling back to BW snapshot",
            static_cast<unsigned>(requiredSize));
    return false;
  }
  grayscaleStripScratchSize = requiredSize;
  LOG_INF("EPS", "Grayscale strip scratch: bytes=%u pool=%s", static_cast<unsigned>(requiredSize),
          grayscaleStripScratchInPsram ? "psram" : "default");
  return true;
}

void EpubReaderActivity::releaseGrayscaleStripScratch(const bool force) {
  // Indexing needs internal DRAM headroom. A PSRAM-backed strip does not consume
  // that pool, so retain it for the activity lifetime and avoid external churn.
  if (!force && grayscaleStripScratch && grayscaleStripScratchInPsram) return;
  grayscaleStripScratch.reset();
  grayscaleStripScratchSize = 0;
  grayscaleStripScratchInPsram = false;
}

void EpubReaderActivity::cacheCurrentSectionPosition() {
  if (activeFootnotePreview) {
    return;
  }
  cachedSpineIndex = currentSpineIndex;
  cachedChapterPageNumber = section->currentPage;
  cachedChapterTotalPageCount = section->estimatedTotalPages();
  cachedChapterPageWatermark = section->pageCount;
  cachedVisibleTextOffset.reset();
  pendingRelayoutReposition = true;
  cachedPageParagraphIndex = UINT16_MAX;
  cachedPageParagraphOffset = 0;
  cachedPageParagraphSpan = 0;
  nextPageNumber = section->currentPage;

  if (section->currentPage >= 0 && section->currentPage < section->pageCount) {
    const uint16_t currentPage = static_cast<uint16_t>(section->currentPage);
    cachedVisibleTextOffset = section->getVisibleTextOffsetForPage(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(currentPage)) {
      cachedPageParagraphIndex = *pIdx;

      uint16_t paragraphStartPage = currentPage;
      while (paragraphStartPage > 0) {
        const auto prevPIdx = section->getParagraphIndexForPage(paragraphStartPage - 1);
        if (!prevPIdx || *prevPIdx != *pIdx) {
          break;
        }
        paragraphStartPage--;
      }

      uint16_t paragraphEndPage = currentPage + 1;
      while (paragraphEndPage < section->pageCount) {
        const auto nextPIdx = section->getParagraphIndexForPage(paragraphEndPage);
        if (!nextPIdx || *nextPIdx != *pIdx) {
          break;
        }
        paragraphEndPage++;
      }

      cachedPageParagraphOffset = currentPage - paragraphStartPage;
      cachedPageParagraphSpan = std::max<uint16_t>(1, paragraphEndPage - paragraphStartPage);
    }
  }
}

void EpubReaderActivity::prepareCurrentSectionForRelayout() {
  if (!section) return;
  cacheCurrentSectionPosition();
}

bool EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int fontId, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft, const bool updatePanel) {
#if CROSSINK_APP_CAP_TOUCH
  if (mappedInput.hasTouchHardware()) {
    if (!touchReaderPreviewAllocationAttempted) {
      touchReaderPreviewAllocationAttempted = true;
      touchReaderPreviewModel = makeUniqueNoThrow<TouchReaderPreviewModel>();
      if (!touchReaderPreviewModel) {
        LOG_ERR("ERDM", "OOM allocating bounded touch reader preview; keeping the reader background unchanged");
      }
    }
    if (touchReaderPreviewModel &&
        !touchReaderPreviewModel->capture(*page, renderer, fontId, SETTINGS.lineHeightPercent)) {
      LOG_DBG("ERDM", "Skipping touch reader preview outside the 8 KiB/256-run snapshot budget");
    }
  }
#endif
  // Font prewarm: scan pass accumulates text, then prewarm, then real render.
  // SD fonts depend on the prepared bitmap cache; drawing after a failed
  // prewarm would turn every unavailable glyph into a replacement symbol.
  std::optional<FontCacheManager::PrewarmScope> pageRenderScope;
  if (auto* fcm = renderer.getFontCacheManager()) {
    const auto prewarmVisibleText = [&]() {
      pageRenderScope.emplace(*fcm, FontCacheManager::PreparationPolicy::Normal);
      page->renderText(renderer, fontId, orientedMarginLeft, orientedMarginTop);  // scan pass
      // The status-bar title can route to the same SD fallback as the page. Scan
      // it into this batch before rendering so it does not evict page glyphs.
      renderStatusBar();
      if (!pageRenderScope->endScanAndPrewarm()) {
        pageRenderScope.reset();
        return false;
      }
      return true;
    };

    bool prewarmSucceeded = prewarmVisibleText();
    if (!prewarmSucceeded && renderer.isSdCardFont(fontId)) {
      LOG_ERR("ERS", "SD-font page prewarm failed (font=%d free=%u maxAlloc=%u); releasing caches and retrying", fontId,
              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      renderer.releaseSdCardFontForLowMemory(fontId, /*preserveAdvanceTable=*/true);
      prewarmSucceeded = prewarmVisibleText();
    }
    if (!prewarmSucceeded) {
      LOG_ERR("ERS", "Font page prewarm failed after retry (font=%d free=%u maxAlloc=%u)", fontId, ESP.getFreeHeap(),
              ESP.getMaxAllocHeap());
      return false;
    }
  }

#if CROSSINK_APP_CAP_TOUCH
  buildFootnoteTouchTargets(*page, fontId, orientedMarginTop, orientedMarginLeft);
#endif

  const bool pageHasImages = page->hasImages();
  const bool pageHasImagesNeedingDecode = pageHasImages && page->hasImagesNeedingDecode();
  const bool foregroundBlack = ReaderUtils::readerForegroundBlack();
  const bool needsImageGrayscale = pageHasImages;
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing && foregroundBlack;
  const bool needsAnyGrayscale = needsTextGrayscale || needsImageGrayscale;
  const bool tiledGrayscale = needsAnyGrayscale && renderer.supportsStripGrayscale();
  const bool overlapRefresh =
      tiledGrayscale && !pageHasImages && pagesUntilFullRefresh > 1 && renderer.supportsAsyncGrayscaleBase();
  const int contentBottom = renderer.getScreenHeight() - orientedMarginBottom;

  const auto finalizeBufferComposition = [&]() {
    drawClippingHighlights(*page, fontId, orientedMarginTop, orientedMarginLeft);
    drawPublisherPageMarkers(renderer, *page, orientedMarginTop, contentBottom, foregroundBlack);
#if CROSSINK_APP_CAP_TOUCH
    if (activeFootnotePreview) {
      TouchHeaderBackButton::draw(renderer, TouchHeaderBackButton::headerRect(renderer, mappedInput), tr(STR_FOOTNOTES),
                                  /*readerContext=*/true);
    }
#endif
  };

  const auto composePageBuffer = [&]() {
    page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop, foregroundBlack);
    finalizeBufferComposition();
  };

  const auto composeGrayscaleBuffer = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop, foregroundBlack);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
    finalizeBufferComposition();
  };
  if (updatePanel && pageHasImagesNeedingDecode) {
    page->renderWithImagePlaceholders(renderer, fontId, orientedMarginLeft, orientedMarginTop, foregroundBlack);
    finalizeBufferComposition();
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  }
  composePageBuffer();
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
    const char* msg = homeButtonInReaderFeedback
                          ? (tiltPageTurnFeedbackEnabled ? tr(STR_HOME_BUTTON_ENABLED) : tr(STR_HOME_BUTTON_DISABLED))
                          : (tiltPageTurnFeedbackEnabled ? tr(STR_TILT_TO_TURN_ON) : tr(STR_TILT_TO_TURN_OFF));
    drawToastBuffer(renderer, msg);
  }
  if (pendingSafeModeToast) {
    drawRenderModeToastBuffer(tr(STR_SAFE_MODE));
  } else if (pendingRenderModeToast) {
    drawRenderModeToastBuffer(labelForRenderModeToast(normalizeRenderMode(renderModeToastMode)));
  }
  if (!updatePanel) {
    return true;
  }
  if (pageHasImages) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      // Blank the image before any panel update so a pending clean pass does
      // not briefly show the decoded image before the final grayscale pass.
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      // Image pages intentionally bypass the regular refresh cadence. Preserve
      // a pending clean base before their double-FAST grayscale pipeline.
      if (cleanImageBasePending) {
        renderer.displayBuffer(pagesUntilFullRefresh < 0 ? manualScreenRefreshMode() : HalDisplay::HALF_REFRESH);
        cleanImageBasePending = false;
      }
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      composePageBuffer();
      // The restored image frame becomes the base for the grayscale image
      // planes below. On X3, use the same grayscale-aware base waveform as
      // text-only grayscale turns; other panels keep the FAST fallback behavior.
      renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(pagesUntilFullRefresh < 0 ? manualScreenRefreshMode() : HalDisplay::HALF_REFRESH);
    }
    // The image's own page is handled above and doesn't count toward the full
    // refresh cadence. But the grayscale pass below leaves gray charge in the
    // image region that a plain fast diff on the *next* page can't clear, so
    // text there ghosts gray (#2190). Force the next ordinary page onto the
    // HALF ghost-cleanup path, which drives every pixel to its target
    // regardless of residue.
    pagesUntilFullRefresh = 1;
  } else if (needsAnyGrayscale) {
    if (pagesUntilFullRefresh <= 1) {
      // Cleanup turns still need the stronger HALF pass, but X3 grayscale
      // overlays settle better if the OEM precondition step runs before the
      // gray planes are written.
      renderer.displayBuffer(pagesUntilFullRefresh < 0 ? manualScreenRefreshMode() : HalDisplay::HALF_REFRESH);
      renderer.preconditionGrayscale();
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else if (overlapRefresh) {
      ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, /*async=*/true);
    } else {
      // Use the grayscale-aware base waveform so the first visible pass is
      // closer to the final anti-aliased result instead of flashing darker
      // text first and softening after the grayscale overlay.
      renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
      pagesUntilFullRefresh--;
    }
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  if (needsAnyGrayscale) {
    ensureGrayscaleStripScratch();
  }
  if (runTiledGrayscalePass(renderer, *page, fontId, orientedMarginLeft, orientedMarginTop, foregroundBlack,
                            needsTextGrayscale, needsImageGrayscale, grayscaleStripScratch.get(),
                            grayscaleStripScratchSize, overlapRefresh)) {
    return true;
  }

  // Save bw buffer to reset buffer state after grayscale data sync
  const bool storedBwBuffer = needsAnyGrayscale && renderer.storeBwBuffer();
  const bool canApplyGrayscale = needsAnyGrayscale && storedBwBuffer;
  if (needsAnyGrayscale && !storedBwBuffer) {
    LOG_ERR("ERS", "Skipping grayscale enhancement: failed to store BW backup");
  }

  // grayscale rendering
  if (canApplyGrayscale) {
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    composeGrayscaleBuffer();
    renderer.copyGrayscaleLsbBuffers();

    // Render and copy to MSB buffer
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    composeGrayscaleBuffer();
    renderer.copyGrayscaleMsbBuffers();

    // display grayscale part
    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
    // restore the bw data
    renderer.restoreBwBuffer();
  } else {
    if (storedBwBuffer) {
      // Restore the BW data when we skipped grayscale entirely.
      renderer.restoreBwBuffer();
    }
  }
  return true;
}

#if CROSSINK_APP_CAP_TOUCH
void EpubReaderActivity::buildFootnoteTouchTargets(const Page& page, const int fontId, const int orientedMarginTop,
                                                   const int orientedMarginLeft) {
  currentPageFootnoteTouchTargets.fill({});
  if (activeFootnotePreview || currentPageFootnotes.empty()) {
    return;
  }

  const int lineHeight = renderer.getLineHeight(fontId);
  for (const auto& element : page.elements) {
    if (!element || element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    if (!line.getBlock()) continue;

    const auto& block = *line.getBlock();
    for (uint16_t wordIndex = 0; wordIndex < block.wordCount(); ++wordIndex) {
      const uint8_t linkId = block.wordLinkId(wordIndex);
      if (linkId == 0) continue;

      const auto footnoteIt = std::find_if(currentPageFootnotes.begin(), currentPageFootnotes.end(),
                                           [linkId](const FootnoteEntry& entry) { return entry.linkId == linkId; });
      if (footnoteIt == currentPageFootnotes.end()) continue;
      const size_t footnoteIndex = static_cast<size_t>(footnoteIt - currentPageFootnotes.begin());
      if (footnoteIndex >= currentPageFootnoteTouchTargets.size()) continue;

      const auto style = static_cast<EpdFontFamily::Style>(block.wordStyle(wordIndex) & ~EpdFontFamily::UNDERLINE);
      const int wordX = orientedMarginLeft + line.xPos + block.wordXpos(wordIndex);
      int wordY = orientedMarginTop + line.yPos;
      if ((style & EpdFontFamily::SUP) != 0) {
        wordY -= renderer.getFontAscenderSize(fontId) * 2 / 5;
      } else if ((style & EpdFontFamily::SUB) != 0) {
        wordY += renderer.getFontAscenderSize(fontId) / 4;
      }
      int wordWidth = renderer.getTextAdvanceX(fontId, block.wordText(wordIndex), style);
      if (wordIndex + 1 < block.wordCount() && block.wordXpos(wordIndex + 1) > block.wordXpos(wordIndex)) {
        wordWidth = std::min(wordWidth, static_cast<int>(block.wordXpos(wordIndex + 1) - block.wordXpos(wordIndex)));
      }
      if (wordWidth <= 0) continue;

      auto& target = currentPageFootnoteTouchTargets[footnoteIndex];
      if (target.width <= 0 || target.height <= 0) {
        target = {static_cast<int16_t>(wordX), static_cast<int16_t>(wordY), static_cast<int16_t>(wordWidth),
                  static_cast<int16_t>(lineHeight)};
        continue;
      }

      const int left = std::min<int>(target.x, wordX);
      const int top = std::min<int>(target.y, wordY);
      const int right = std::max<int>(target.x + target.width, wordX + wordWidth);
      const int bottom = std::max<int>(target.y + target.height, wordY + lineHeight);
      target = {static_cast<int16_t>(left), static_cast<int16_t>(top), static_cast<int16_t>(right - left),
                static_cast<int16_t>(bottom - top)};
    }
  }
}

bool EpubReaderActivity::handleTouchFootnoteLink(const int touchX, const int touchY) {
  if (!SETTINGS.touchReaderControls || !mappedInput.hasTouch() || RenderLock::peek() || activeFootnotePreview ||
      currentPageFootnotes.empty()) {
    return false;
  }

  int bestIndex = -1;
  int64_t bestDistance = std::numeric_limits<int64_t>::max();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  for (size_t i = 0; i < currentPageFootnotes.size() && i < currentPageFootnoteTouchTargets.size(); ++i) {
    const auto& target = currentPageFootnoteTouchTargets[i];
    if (target.width <= 0 || target.height <= 0) continue;

    const int expandedWidth = std::max<int>(target.width, TOUCH_FOOTNOTE_TARGET_SIZE);
    const int expandedHeight = std::max<int>(target.height, TOUCH_FOOTNOTE_TARGET_SIZE);
    const int left = std::max(
        0, std::min(target.x - (expandedWidth - target.width) / 2, screenWidth - std::min(screenWidth, expandedWidth)));
    const int top = std::max(0, std::min(target.y - (expandedHeight - target.height) / 2,
                                         screenHeight - std::min(screenHeight, expandedHeight)));
    const int right = std::min(screenWidth, left + expandedWidth);
    const int bottom = std::min(screenHeight, top + expandedHeight);
    if (touchX < left || touchX >= right || touchY < top || touchY >= bottom) continue;

    const int centerX = target.x + target.width / 2;
    const int centerY = target.y + target.height / 2;
    const int64_t deltaX = static_cast<int64_t>(touchX) - centerX;
    const int64_t deltaY = static_cast<int64_t>(touchY) - centerY;
    const int64_t distance = deltaX * deltaX + deltaY * deltaY;
    if (distance < bestDistance) {
      bestDistance = distance;
      bestIndex = static_cast<int>(i);
    }
  }

  if (bestIndex < 0) return false;
  navigateToHref(currentPageFootnotes[bestIndex].href, true, /*preferFootnotePreview=*/true);
  return true;
}
#endif

void EpubReaderActivity::drawClippingHighlights(const Page& page, const int fontId, const int orientedMarginTop,
                                                const int orientedMarginLeft) const {
  if (!section || !CLIPPINGS.hasClippings() || renderer.getRenderMode() != GfxRenderer::BW) {
    return;
  }

  // The BW frame owns the dithered marker and its redrawn black text. Repeating
  // that marker in the grayscale anti-aliasing planes overlays gray pixels on
  // the glyphs and makes a saved clipping look washed out.

  std::array<ClippingPageMatch, CLIPPING_MAX_PAGE_MATCHES> matches;
  uint16_t matchCount = 0;
  const bool canUseStoredRanges = section->pageCount > 0 && section->pageCount <= UINT16_MAX &&
                                  section->currentPage >= 0 && section->currentPage < section->pageCount;
  const uint16_t currentPage = canUseStoredRanges ? static_cast<uint16_t>(section->currentPage) : 0;
  const uint16_t currentPageCount = canUseStoredRanges ? static_cast<uint16_t>(section->pageCount) : 0;
  const uint32_t currentClippingLayoutSignature = clippingWordLayoutSignature(activeSectionLayoutSignature);
  std::string clippingText;
  clippingText.reserve(CLIPPING_TEXT_MAX);
  const auto& clippings = CLIPPINGS.getClippings();
  for (size_t clippingIndex = 0; clippingIndex < clippings.size(); ++clippingIndex) {
    const Clipping& clipping = clippings[clippingIndex];
    if (clipping.spineIndex != static_cast<uint16_t>(currentSpineIndex)) {
      continue;
    }
    ClippingPageMatch match;
    const bool storedLayoutMatches =
        canUseStoredRanges &&
        clippingStoredRangeMatchesLayout(clipping, currentPageCount, currentClippingLayoutSignature);
    const bool legacyWordLayout =
        canUseStoredRanges && clippingUsesLegacyWordLayout(clipping, currentPageCount, activeSectionLayoutSignature);
    const bool legacyRangeReady = legacyWordLayout && clippingCachedRangeReadyOnPage(clipping, currentPage);
    const bool matchedStoredRange = (storedLayoutMatches || legacyRangeReady) &&
                                    findClippingStoredRangeOnPage(page, clipping, currentPage, currentPageCount, match);
    const bool pageInStoredRange = currentPage >= clipping.startPage && currentPage <= clipping.endPage;
    const bool shouldSearchText =
        legacyWordLayout ? (!matchedStoredRange && pageInStoredRange) : (!storedLayoutMatches || pageInStoredRange);
    bool matchedText = false;
    if (!matchedStoredRange && shouldSearchText) {
      clippingText.clear();
      if (CLIPPINGS.readClippingText(clipping, clippingText)) {
        bool uniqueTextMatch = false;
        const uint8_t expectedTableColumn = tableColumnForSelection(clipping.tableSelection);
        matchedText = findClippingTextOnPage(page, clippingText, match, &uniqueTextMatch, expectedTableColumn);
        const bool coversStoredBoundaries = (currentPage != clipping.startPage || match.startsAtClipStart) &&
                                            (currentPage != clipping.endPage || match.reachesClipEnd);
        if (matchedText && uniqueTextMatch && coversStoredBoundaries && legacyWordLayout) {
          CLIPPINGS.cacheResolvedLayoutRange(clippingIndex, currentPage, match.startWord, match.endWord,
                                             currentClippingLayoutSignature);
        }
      }
    }
    if (matchedStoredRange || matchedText) {
      matches[matchCount++] = match;
      if (matchCount >= matches.size()) {
        break;
      }
    }
  }
  if (matchCount == 0) {
    return;
  }

  const auto isHighlightedWord = [&matches, matchCount](const uint16_t pageWordIndex, const PageTextLine& line) {
    for (uint16_t matchIndex = 0; matchIndex < matchCount; ++matchIndex) {
      if (pageWordIndex >= matches[matchIndex].startWord && pageWordIndex <= matches[matchIndex].endWord &&
          ClippingHighlightGeometry::matchesTableSelection(matches[matchIndex].tableSelection, line.tableSelection)) {
        return true;
      }
    }
    return false;
  };

  ClippingHighlightGeometry::WordRect previousHighlight;
  const TextBlock* previousHighlightBlock = nullptr;
  bool hasPreviousHighlight = false;
  forEachVisiblePageWord(page, [&](const uint16_t pageWordIndex, const PageTextLine& line, const TextBlock& block,
                                   const size_t i) {
    if (!isHighlightedWord(pageWordIndex, line)) {
      hasPreviousHighlight = false;
      return true;
    }

    if (i >= block.wordCount()) {
      return true;
    }

    const auto wordIndex = static_cast<uint16_t>(i);
    const char* wordText = block.wordText(wordIndex);
    const bool hasEmSpace = hasEmSpacePrefix(wordText);
    const char* visibleText = wordText + (hasEmSpace ? 3 : 0);
    const auto textStyle = static_cast<EpdFontFamily::Style>(block.wordStyle(wordIndex) & ~EpdFontFamily::UNDERLINE);
    const int skipX = hasEmSpace ? renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", textStyle) : 0;
    const PageWordGeometry geometry = pageWordGeometry(renderer, fontId, line, block, wordIndex);
    const int wordX = orientedMarginLeft + line.xPos + geometry.xOffset + skipX;
    const int wordY = orientedMarginTop + line.yPos;
    int wordW = geometry.width - skipX;
    const int wordH = line.lineHeight > 0 ? line.lineHeight : renderer.getLineHeight(fontId);
    if (wordIndex + 1 < block.wordCount()) {
      const uint16_t nextIndex = static_cast<uint16_t>(wordIndex + 1);
      const char* nextWordText = block.wordText(nextIndex);
      const bool nextHasEmSpace = hasEmSpacePrefix(nextWordText);
      const auto nextTextStyle =
          static_cast<EpdFontFamily::Style>(block.wordStyle(nextIndex) & ~EpdFontFamily::UNDERLINE);
      const int nextSkipX = nextHasEmSpace ? renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", nextTextStyle) : 0;
      const PageWordGeometry nextGeometry = pageWordGeometry(renderer, fontId, line, block, nextIndex);
      const int nextWordX = orientedMarginLeft + line.xPos + nextGeometry.xOffset + nextSkipX;
      if (isHighlightedWord(pageWordIndex + 1, line) && nextWordX > wordX + wordW) {
        wordW = nextWordX - wordX;
      } else if (nextWordX > wordX && wordW > nextWordX - wordX) {
        wordW = nextWordX - wordX;
      }
    }
    if (wordW > 0) {
      const ClippingHighlightGeometry::WordRect currentHighlight{pageWordIndex, wordX, wordY, wordW, wordH};
      ClippingHighlightGeometry::GapRect gap;
      // No-break spaces are stored as hidden layout words. Bridge to the next
      // visible word in the same line even when that hidden token interrupted
      // the immediate-word lookahead above.
      if (hasPreviousHighlight && previousHighlightBlock == &block &&
          ClippingHighlightGeometry::gapBetweenAdjacentWords(previousHighlight, currentHighlight, gap)) {
        renderer.fillRectDither(gap.x, gap.y, gap.width, gap.height, Color::LightGray);
      }
      renderer.fillRectDither(wordX, wordY, wordW, wordH, Color::LightGray);
      // A saved clipping always uses black text on its light-gray marker.
      // The ordinary reader foreground is white in dark mode, which makes the
      // text fade into this marker.
      if (line.clipWidth > 0 && line.clipHeight > 0) {
        renderer.beginTextClip(orientedMarginLeft + line.clipX, orientedMarginTop + line.clipY, line.clipWidth,
                               line.clipHeight);
      }
      renderer.drawText(fontId, wordX, wordY, visibleText, true, textStyle);
      if (line.clipWidth > 0 && line.clipHeight > 0) {
        renderer.endTextClip();
      }
      previousHighlight = currentHighlight;
      previousHighlightBlock = &block;
      hasPreviousHighlight = true;
    } else {
      hasPreviousHighlight = false;
    }
    return true;
  });
}

void EpubReaderActivity::renderStatusBar() const {
  if (!statusBarVisible) {
    return;
  }

  const int estimatedPageCount = section->estimatedTotalPages();
  int currentPage = section->currentPage + 1;
  int pageCount = estimatedPageCount;
  const float bookProgress = activeFootnotePreview ? 0.0f : getCurrentBookProgressPercent();
  const float sectionProgress = (estimatedPageCount > 0)
                                    ? static_cast<float>(section->currentPage) / static_cast<float>(estimatedPageCount)
                                    : 0.0f;
  float chapterProgress = sectionProgress;
  bool pageCountEstimated = section->isBuilding() || section->isPartial();
  if (!activeFootnotePreview) {
    resolveChapterGroupPageProgress(currentPage, pageCount, chapterProgress, pageCountEstimated);
  }

  uint32_t referencePage = 0;
  uint32_t referencePageCount = 0;
  if (activeFootnotePreview || !SETTINGS.stablePageNumbers ||
      !epub->resolveReferencePage(currentSpineIndex, sectionProgress, referencePage, referencePageCount)) {
    referencePage = 0;
    referencePageCount = 0;
  }

  std::string title;

  int textYOffset = 0;

  if (automaticPageTurnActive) {
    // Fixed-shape label on a per-page-render path: format on the stack instead of
    // allocating a std::to_string temporary and a concatenation result each time.
    // Sized for the longest translated prefix (Kazakh, 53 bytes) plus the interval
    // digits, so no locale is cut short or sliced mid-codepoint.
    char autoTurnLabel[96];
    snprintf(autoTurnLabel, sizeof(autoTurnLabel), "%s%lu", tr(STR_AUTO_TURN_ENABLED), pageTurnDuration / 1000);
    title = autoTurnLabel;

    // calculates textYOffset when rendering title in status bar
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

    // offsets text if no status bar or progress bar only
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }

  }
#if CROSSINK_APP_CAP_TOUCH
  else if (activeFootnotePreview) {
    // The touch header owns the preview title; keep the footer from repeating it.
  }
#else
  else if (activeFootnotePreview && SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = tr(STR_FOOTNOTES);
  }
#endif
  else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    int titleSpineIndex = currentSpineIndex;
    int groupLastSpineIndex = currentSpineIndex;
    epub->resolveChapterGroupRange(currentSpineIndex, titleSpineIndex, groupLastSpineIndex);
    const int tocIndex = epub->getTocIndexForSpineIndex(titleSpineIndex);
    if (tocIndex != -1) {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }

  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  const int bookmarkPageCount = estimatedPageCount > 0 ? estimatedPageCount : 1;
  const float rawProgress = (estimatedPageCount > 0)
                                ? (static_cast<float>(section->currentPage) / static_cast<float>(estimatedPageCount))
                                : 0.0f;
  const bool bookmarked =
      !activeFootnotePreview &&
      BOOKMARKS.hasBookmarkForPage(static_cast<uint16_t>(currentSpineIndex), rawProgress, bookmarkPageCount);
  char timeLeftLabel[24] = {};
  const char* timeLeft =
      (!activeFootnotePreview && formatTimeLeftLabel(timeLeftLabel, sizeof(timeLeftLabel))) ? timeLeftLabel : nullptr;
  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title.c_str(), 0, textYOffset, bookmarked, timeLeft,
                    ReaderUtils::readerDarkModeEnabled(), chapterProgress * 100.0f, static_cast<int>(referencePage),
                    static_cast<int>(referencePageCount), !activeFootnotePreview, pageCountEstimated);
  GUI.drawTopStatusBarClock(renderer, UITheme::getInstance().getMetrics().topPadding, nullptr, true, 0,
                            ReaderUtils::readerDarkModeEnabled());
}

void EpubReaderActivity::refreshChapterGroupEstimate(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section) {
    chapterGroupEstimate = {};
    return;
  }

  int firstSpineIndex = currentSpineIndex;
  int lastSpineIndex = currentSpineIndex;
  if (!epub->resolveChapterGroupRange(currentSpineIndex, firstSpineIndex, lastSpineIndex) ||
      firstSpineIndex == lastSpineIndex) {
    chapterGroupEstimate = {};
    return;
  }

  uint32_t signature = 2166136261U;
  const auto mix = [&signature](const uint32_t value) {
    signature ^= value;
    signature *= 16777619U;
  };
  mix(static_cast<uint32_t>(activeSectionFontId != 0 ? activeSectionFontId : SETTINGS.getReaderFontId()));
  mix(viewportWidth);
  mix(viewportHeight);
  mix(static_cast<uint32_t>(SETTINGS.getReaderLineCompression() * 1000.0f));
  mix(SETTINGS.extraParagraphSpacing);
  mix(SETTINGS.forceParagraphIndents);
  mix(SETTINGS.paragraphAlignment);
  mix(SETTINGS.hyphenationEnabled);
  mix(SETTINGS.embeddedStyle);
  mix(SETTINGS.imageRendering);
  mix(SETTINGS.bionicReadingEnabled);
  mix(SETTINGS.guideReadingEnabled);
  mix(SETTINGS.wordSpacing);
  mix(SETTINGS.epubRenderMode);
  if (chapterGroupEstimate.valid && chapterGroupEstimate.currentSpineIndex == currentSpineIndex &&
      chapterGroupEstimate.settingsSignature == signature) {
    return;
  }

  ChapterGroupEstimateCache refreshed;
  refreshed.currentSpineIndex = currentSpineIndex;
  refreshed.firstSpineIndex = firstSpineIndex;
  refreshed.lastSpineIndex = lastSpineIndex;
  refreshed.settingsSignature = signature;
  const int readerFontId = activeSectionFontId != 0 ? activeSectionFontId : SETTINGS.getReaderFontId();
  const EpubRenderMode renderMode = normalizeRenderMode(SETTINGS.epubRenderMode);

  for (int spineIndex = firstSpineIndex; spineIndex <= lastSpineIndex; ++spineIndex) {
    if (spineIndex == currentSpineIndex) continue;
    const size_t previousCumulative = spineIndex > 0 ? epub->getCumulativeSpineItemSize(spineIndex - 1) : 0;
    const size_t cumulative = epub->getCumulativeSpineItemSize(spineIndex);
    const uint32_t spineBytes =
        cumulative > previousCumulative
            ? static_cast<uint32_t>(std::min<size_t>(cumulative - previousCumulative, UINT32_MAX))
            : 0;

    Section sibling(epub, spineIndex, renderer, sectionCacheSuffixForRenderMode(renderMode));
    const bool loaded = sibling.loadSectionFile(
        readerRenderSpecForProfile(readerFontId, viewportWidth, viewportHeight, buildProfileForRenderMode(renderMode)));
    const uint64_t siblingImageUnits = loaded ? sibling.estimatedProtectedImageUnits() : 0;
    const uint64_t siblingNonImageUnits = loaded ? sibling.estimatedNonImageProjectionUnits() : 0;
    if ((siblingImageUnits > 0 || siblingNonImageUnits > 0) && spineBytes > 0) {
      refreshed.knownSiblingImageUnits += siblingImageUnits;
      refreshed.knownSiblingNonImageUnits += siblingNonImageUnits;
      refreshed.knownSiblingBytes += spineBytes;
      refreshed.siblingEstimateUsed = refreshed.siblingEstimateUsed || sibling.isPartial() || sibling.isBuilding();
      if (spineIndex < currentSpineIndex) {
        refreshed.precedingKnownImageUnits += siblingImageUnits;
        refreshed.precedingKnownNonImageUnits += siblingNonImageUnits;
      }
    } else {
      refreshed.unknownSiblingBytes += spineBytes;
      refreshed.unknownSiblingCount++;
      if (spineIndex < currentSpineIndex) {
        refreshed.precedingUnknownBytes += spineBytes;
        refreshed.precedingUnknownCount++;
      }
    }
  }
  refreshed.valid = true;
  chapterGroupEstimate = refreshed;
  LOG_DBG(
      "ERS",
      "Chapter group estimate: spine=%d range=%d-%d known=%llu image/%llu text units/%lu bytes unknown=%u/%lu bytes",
      currentSpineIndex, firstSpineIndex, lastSpineIndex,
      static_cast<unsigned long long>(refreshed.knownSiblingImageUnits),
      static_cast<unsigned long long>(refreshed.knownSiblingNonImageUnits),
      static_cast<unsigned long>(refreshed.knownSiblingBytes), refreshed.unknownSiblingCount,
      static_cast<unsigned long>(refreshed.unknownSiblingBytes));
}

bool EpubReaderActivity::resolveChapterGroupPageProgress(int& currentPage, int& pageCount, float& chapterProgress,
                                                         bool& pageCountEstimated) const {
  if (!chapterGroupEstimate.valid || !section || chapterGroupEstimate.currentSpineIndex != currentSpineIndex) {
    return false;
  }
  const size_t previousCumulative = currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t cumulative = epub->getCumulativeSpineItemSize(currentSpineIndex);
  const uint32_t currentBytes =
      cumulative > previousCumulative
          ? static_cast<uint32_t>(std::min<size_t>(cumulative - previousCumulative, UINT32_MAX))
          : 0;
  const uint64_t currentImageUnits = section->estimatedProtectedImageUnits();
  const uint64_t currentNonImageUnits = section->estimatedNonImageProjectionUnits();
  const uint64_t knownImageUnits = chapterGroupEstimate.knownSiblingImageUnits + currentImageUnits;
  const uint64_t knownNonImageUnits = chapterGroupEstimate.knownSiblingNonImageUnits + currentNonImageUnits;
  const uint64_t knownBytes = static_cast<uint64_t>(chapterGroupEstimate.knownSiblingBytes) + currentBytes;
  if (knownImageUnits + knownNonImageUnits == 0 || knownBytes == 0) return false;

  const auto estimateUnknownUnits = [knownNonImageUnits, knownBytes](const uint32_t bytes, const uint16_t itemCount) {
    if (bytes == 0 || itemCount == 0) return uint64_t{0};
    const uint64_t projected = PageCountEstimator::projectNonImageUnits(knownNonImageUnits, bytes, knownBytes);
    const uint64_t minimum = static_cast<uint64_t>(itemCount) * PageCountEstimator::kUnitsPerPage;
    return std::max(minimum, projected);
  };
  const uint64_t unknownUnits =
      estimateUnknownUnits(chapterGroupEstimate.unknownSiblingBytes, chapterGroupEstimate.unknownSiblingCount);
  const uint64_t precedingUnknownUnits =
      estimateUnknownUnits(chapterGroupEstimate.precedingUnknownBytes, chapterGroupEstimate.precedingUnknownCount);
  const uint64_t groupedTotalUnits = knownImageUnits + knownNonImageUnits + unknownUnits;
  const uint64_t groupedCurrentUnits =
      chapterGroupEstimate.precedingKnownImageUnits + chapterGroupEstimate.precedingKnownNonImageUnits +
      precedingUnknownUnits + (static_cast<uint64_t>(section->currentPage) + 1U) * PageCountEstimator::kUnitsPerPage;
  const uint64_t groupedTotal = groupedTotalUnits / PageCountEstimator::kUnitsPerPage;
  const uint64_t groupedCurrent = groupedCurrentUnits / PageCountEstimator::kUnitsPerPage;
  pageCount = static_cast<int>(std::min<uint64_t>(PageCountEstimator::kMaxPages, std::max<uint64_t>(1, groupedTotal)));
  currentPage =
      static_cast<int>(std::min<uint64_t>(std::max<uint64_t>(1, groupedCurrent), static_cast<uint64_t>(pageCount)));
  chapterProgress = pageCount > 0 ? static_cast<float>(currentPage - 1) / static_cast<float>(pageCount) : 0.0f;
  pageCountEstimated = section->isBuilding() || section->isPartial() || chapterGroupEstimate.siblingEstimateUsed ||
                       chapterGroupEstimate.unknownSiblingCount > 0;
  return true;
}

bool EpubReaderActivity::isTocChapterDestination(const int targetSpineIndex, const std::string& anchor) const {
  if (!epub || targetSpineIndex < 0 || targetSpineIndex >= epub->getSpineItemsCount()) {
    return false;
  }

  const int firstTocIndex = epub->getTocIndexForSpineIndex(targetSpineIndex);
  if (firstTocIndex < 0) {
    return false;
  }

  // A file-level TOC target has no fragment. Otherwise require the matching
  // TOC anchor: a footnote may share a spine item with a chapter entry.
  if (anchor.empty()) {
    return true;
  }
  for (int tocIndex = firstTocIndex; tocIndex < epub->getTocItemsCount(); ++tocIndex) {
    const auto tocItem = epub->getTocItem(tocIndex);
    if (tocItem.spineIndex != targetSpineIndex) {
      break;
    }
    if (tocItem.anchor == anchor) {
      return true;
    }
  }
  return false;
}

bool EpubReaderActivity::shouldUseFootnotePreview(const int targetSpineIndex, const std::string& anchor) const {
  if (!epub || anchor.empty() || targetSpineIndex < 0 || targetSpineIndex >= epub->getSpineItemsCount()) {
    return false;
  }
  return targetSpineIndex != currentSpineIndex && !isTocChapterDestination(targetSpineIndex, anchor);
}

std::string EpubReaderActivity::footnotePreviewCacheSuffix(const EpubRenderMode renderMode,
                                                           const std::string& anchor) const {
  const uint64_t anchorHash = hashFootnotePreviewAnchor(anchor);
  char previewSuffix[32];
  snprintf(previewSuffix, sizeof(previewSuffix), "_fn_%08lx%08lx", static_cast<unsigned long>(anchorHash >> 32),
           static_cast<unsigned long>(anchorHash & 0xffffffffULL));
  return std::string(sectionCacheSuffixForRenderMode(renderMode)) + previewSuffix;
}

void EpubReaderActivity::clearFootnotePreviewState() {
  pendingFootnotePreviewAnchor.clear();
  activeFootnotePreview = false;
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition,
                                        const bool preferFootnotePreview) {
  pageLoadRetryCount = 0;
  if (!epub) return;

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
    return;
  }

  // Internal links are collected with footnotes so their rendered words can
  // be tapped. A destination declared in the book TOC is a chapter jump,
  // though: it must replace the current chapter instead of opening a preview.
  const bool chapterDestination = isTocChapterDestination(targetSpineIndex, anchor);
  // A link clicked in a contents document is a chapter jump, not a footnote.
  // Serialize its source scan with rendering: real SD cards allow only one
  // reader, and ZIP inflation borrows the framebuffer's storage.
  bool sourceScanSucceeded = true;
  bool contentsNavigation = false;
  if (chapterDestination) {
    RenderLock lock(*this);
    GfxRenderer::FrameBufferLoan loan(renderer);
    contentsNavigation = epub->isNavigationDocumentSpine(currentSpineIndex, &sourceScanSucceeded);
  }
  if (!sourceScanSucceeded) {
    LOG_ERR("ERS", "Could not inspect contents source for link: %s", hrefStr.c_str());
  }
  // Keep the return stack for an endnote that happens to point at a TOC
  // chapter.
  const bool saveReturnPosition = savePosition && !contentsNavigation;
  if (saveReturnPosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
  }

  {
    RenderLock lock(*this);
    const bool useFootnotePreview = saveReturnPosition && !chapterDestination && !anchor.empty() &&
                                    (preferFootnotePreview || shouldUseFootnotePreview(targetSpineIndex, anchor));
    pendingAnchor = anchor;
    pendingFootnotePreviewAnchor = useFootnotePreview ? anchor : std::string{};
    activeFootnotePreview = false;
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    section.reset();
  }
  armReadingPaceWarmup(saveReturnPosition ? "href_navigation" : "chapter_link_navigation");
  requestUpdate();
}

void EpubReaderActivity::restoreSavedPosition() {
  pageLoadRetryCount = 0;
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];

  {
    RenderLock lock(*this);
    clearFootnotePreviewState();
    pendingAnchor.clear();
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  armReadingPaceWarmup("saved_position_restore");
  requestUpdate();
}
bool EpubReaderActivity::drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer) {
  auto epub = makeUniqueNoThrow<Epub>(filePath, "/.crosspoint");
  if (!epub) {
    LOG_ERR("SLP", "EPUB: failed to allocate book for sleep-page rendering");
    return false;
  }
  epub->setupCacheDir();

  ScopedReaderSettingsRestore restoreReaderSettings;
  const auto readerSettings = readBookReaderSettings(*epub);
  if (readerSettings.hasCustomReaderSettings) {
    applyReaderSettings(readerSettings.readerSettings);
  }
  SETTINGS.epubRenderMode = readerSettings.hasRenderModeOverride
                                ? normalizeRenderModeRaw(readerSettings.renderMode)
                                : static_cast<uint8_t>(EpubRenderMode::CrossInkDefault);

  // Load CSS when embeddedStyle is enabled, as createSectionFile may need it to rebuild the cache.
  {
    GfxRenderer::FrameBufferLoan loan(renderer);
    if (!epub->load(true, SETTINGS.embeddedStyle == 0, Epub::XLocationLoadMode::Skip)) {
      LOG_DBG("SLP", "EPUB: failed to load %s", filePath.c_str());
      return false;
    }
  }
  ensureReaderSdFontLoaded(renderer);

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

  const ReaderViewportLayout layout = computeReaderViewportLayout(renderer, /*automaticPageTurnActive=*/false);
  const uint16_t viewportWidth = layout.viewportWidth;
  const uint16_t viewportHeight = layout.viewportHeight;

  // Load or rebuild the section cache. Rebuilding is needed when the cache is missing or stale
  // (e.g. after a firmware update). A no-op popup callback avoids any UI during sleep preparation.
  const int readerFontId = SETTINGS.getReaderFontId();
  int renderFontId = readerFontId;
  const EpubRenderMode selectedRenderMode = normalizeRenderMode(SETTINGS.epubRenderMode);
  auto section =
      makeUniqueNoThrow<Section>(*epub, spineIndex, renderer, sectionCacheSuffixForRenderMode(selectedRenderMode));
  bool loadedSection = false;
  if (section) {
    loadedSection = section->loadSectionFile(readerRenderSpecForProfile(readerFontId, viewportWidth, viewportHeight,
                                                                        buildProfileForRenderMode(selectedRenderMode)));
  } else {
    LOG_ERR("SLP", "EPUB: failed to allocate section for spine %d", spineIndex);
  }

  bool sectionRebuilt = false;
  bool safeModeBuildSucceeded = false;
  EpubRenderMode usedRenderMode = selectedRenderMode;
  const auto rebuildSectionWithFallback = [&]() {
    bool layoutAbortedForLowMemory = false;
    auto buildWithFallback = [&](const SectionBuildProfile& profile) {
      layoutAbortedForLowMemory = false;
      section =
          makeUniqueNoThrow<Section>(*epub, spineIndex, renderer, sectionCacheSuffixForRenderMode(profile.renderMode));
      if (!section) {
        LOG_ERR("SLP", "EPUB: failed to allocate section builder for spine %d", spineIndex);
        return SectionBuildAttempt{false, true};
      }
      const bool succeeded = section->createSectionFile(
          readerRenderSpecForProfile(readerFontId, viewportWidth, viewportHeight, profile), []() {}, nullptr,
          &layoutAbortedForLowMemory);
      if (succeeded) usedRenderMode = profile.renderMode;
      return SectionBuildAttempt{succeeded, layoutAbortedForLowMemory};
    };
    auto beforeFallbackRetry = [&](const SectionBuildProfile& profile) {
      if (profile.safeMode) {
        LOG_DBG("SLP", "EPUB: retrying sleep-page rebuild with Safe Mode for spine %d", spineIndex);
        releaseReaderSdFontCachesForLowMemory(renderer, "SLP", "sleep-page safe mode rebuild");
      } else {
        LOG_DBG("SLP", "EPUB: retrying sleep-page rebuild with mode %u for spine %d",
                static_cast<unsigned>(profile.renderMode), spineIndex);
        releaseReaderSdFontCachesForLowMemory(renderer, "SLP", "sleep-page fallback rebuild");
      }
    };
    const SectionFallbackResult result = runSectionBuildFallbacks(selectedRenderMode, shouldAttemptSafeModeFallback(),
                                                                  buildWithFallback, beforeFallbackRetry);
    safeModeBuildSucceeded = result.usedSafeMode;
    return result.succeeded;
  };

  if (!loadedSection) {
    if (!MemoryBudget::hasHeapForOptionalEpubRebuild("SLP", "EPUB sleep-page cache rebuild", spineIndex)) {
      return false;
    }

    LOG_DBG("SLP", "EPUB: section cache not found for spine %d, rebuilding (free=%u, maxAlloc=%u)", spineIndex,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    if (!rebuildSectionWithFallback()) {
      LOG_ERR("SLP", "EPUB: failed to rebuild section cache for spine %d", spineIndex);
      return false;
    }
    sectionRebuilt = true;
  }

  if (pageNumber >= section->pageCount && loadedSection && section->isPartial()) {
    if (!MemoryBudget::hasHeapForOptionalEpubRebuild("SLP", "EPUB sleep-page partial catch-up", spineIndex)) {
      return false;
    }
    bool catchUpSucceeded = section->startBuild(readerRenderSpecForProfile(
        readerFontId, viewportWidth, viewportHeight, buildProfileForRenderMode(selectedRenderMode)));
    while (catchUpSucceeded && !section->isBuildComplete() && pageNumber >= section->pageCount) {
      catchUpSucceeded = section->buildSomeMore(BUILD_PAGES_PER_CHUNK);
    }
    if (!catchUpSucceeded) {
      if (!section->lastBuildLayoutAbortedForLowMemory()) return false;
      LOG_DBG("SLP", "EPUB: partial catch-up hit low heap; rebuilding with fallback modes");
      releaseReaderSdFontCachesForLowMemory(renderer, "SLP", "sleep-page partial fallback rebuild");
      section.reset();
      if (!rebuildSectionWithFallback()) return false;
      sectionRebuilt = true;
    }
  }
  if (sectionRebuilt) {
    if (safeModeBuildSucceeded) {
      applySafeModeReaderSettings();
      if (!saveRuntimeReaderSettingsForCache(epub->getCachePath())) {
        LOG_ERR("SLP", "EPUB: failed to save Safe Mode reader settings");
      }
    } else if (usedRenderMode != selectedRenderMode) {
      SETTINGS.epubRenderMode = static_cast<uint8_t>(usedRenderMode);
      saveBookRenderModeForCache(epub->getCachePath(), SETTINGS.epubRenderMode);
    }
    releaseReaderSdFontCachesForLowMemory(renderer, "SLP", "sleep-page section cache rebuild");
    LOG_DBG("SLP", "EPUB: section cache rebuilt for spine %d (pages=%u, font=%d, mode=%u free=%u, maxAlloc=%u)",
            spineIndex, section->pageCount, renderFontId, static_cast<unsigned>(usedRenderMode), ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
  }
  if (pageNumber < 0 || pageNumber >= section->pageCount) return false;
  section->currentPage = pageNumber;

  auto page = section->loadPage(pageNumber);
  if (!page) {
    LOG_DBG("SLP", "EPUB: failed to load page %d", pageNumber);
    return false;
  }

  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  page->render(renderer, renderFontId, layout.marginLeft, layout.marginTop, ReaderUtils::readerForegroundBlack());
  drawPublisherPageMarkers(renderer, *page, layout.marginTop, renderer.getScreenHeight() - layout.marginBottom,
                           ReaderUtils::readerForegroundBlack());
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
    const int totalPages = section->estimatedTotalPages();
    info.currentPage = section->currentPage + 1;
    info.totalPages = totalPages;
    if (epub && epub->getBookSize() > 0 && totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}
