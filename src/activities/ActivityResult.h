#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "reader/EpubReaderMenuModel.h"
#include "util/FrontlightPanelModel.h"

struct WifiResult {
  bool connected = false;
  std::string ssid;
  std::string ip;
};

struct KeyboardResult {
  std::string text;
};

struct MenuResult {
  int action = -1;
  uint8_t orientation = 0;
  bool settingsChanged = false;
  ReaderDrawerState drawerState{};
  ReaderSettingsChangeMask changeMask = ReaderSettingsChangeMask::None;
  bool reopenDrawer = false;
  int16_t drawerValue = -1;
};

struct ChapterResult {
  int spineIndex = 0;
  std::string anchor;
  uint8_t orientation = 0;
  bool settingsChanged = false;
  ReaderDrawerState drawerState{};
};

struct PercentResult {
  int percent = 0;
};

struct IntervalResult {
  uint32_t value = 0;
};

struct OptionSelectionResult {
  uint8_t index = 0;
};

struct PageResult {
  uint32_t page = 0;
};

enum class NetworkMode;

struct NetworkModeResult {
  NetworkMode mode;
};

struct FootnoteResult {
  std::string href;
};

struct BookmarkResult {
  uint16_t spineIndex = 0;
  float progress = 0.0f;
  uint16_t paragraphIndex = UINT16_MAX;
};

struct FileBrowserActionResult {
  int action = -1;
};

struct FilePathResult {
  std::string path;
};

struct WordResult {
  std::string word;
};

struct ReadingStatsResult {
  bool changed = false;
};

struct ClippingResult {
  std::string text;
  uint16_t sectionPage = 0;
  uint16_t endSectionPage = 0;
  uint16_t sectionPageCount = 1;
  uint16_t startPageWordIndex = 0;
  uint16_t endPageWordIndex = 0;
  uint16_t paragraphIndex = UINT16_MAX;
  uint16_t tableSelection = UINT16_MAX;
  uint16_t wordCount = 0;
};

// A dictionary lookup retains the selected page-word range and exact byte
// boundaries within its outer words. The reader resolves it through its
// canonical ClipWordStore before creating a clipping.
struct DictionaryClippingRequest {
  // Page offsets are relative to the reader page that opened dictionary lookup.
  // A touch drag may continue from that page onto the next one.
  uint8_t firstPageOffset = 0;
  uint16_t firstPageWordOrdinal = 0;
  uint8_t lastPageOffset = 0;
  uint16_t lastPageWordOrdinal = 0;
  uint16_t firstWordByteOffset = 0;
  uint16_t lastWordByteEndOffset = 0;
};

struct ClippingJumpResult {
  uint16_t spineIndex = 0;
  uint16_t page = 0;
  uint16_t pageCount = 1;
  uint16_t paragraphIndex = UINT16_MAX;
  uint16_t clippingIndex = UINT16_MAX;
  uint8_t orientation = 0;
  bool settingsChanged = false;
};

using ResultVariant =
    std::variant<std::monostate, WifiResult, KeyboardResult, MenuResult, ChapterResult, PercentResult, IntervalResult,
                 OptionSelectionResult, PageResult, NetworkModeResult, FootnoteResult, BookmarkResult,
                 FileBrowserActionResult, FilePathResult, WordResult, ReadingStatsResult, ClippingResult,
                 DictionaryClippingRequest, ClippingJumpResult, FrontlightPanelResult>;

struct ActivityResult {
  bool isCancelled = false;
  ResultVariant data;

  explicit ActivityResult() = default;

  template <typename ResultType>
    requires std::is_constructible_v<ResultVariant, ResultType&&>
  // cppcheck-suppress noExplicitConstructor
  ActivityResult(ResultType&& result) : data{std::forward<ResultType>(result)} {}
};

using ActivityResultHandler = std::function<void(const ActivityResult&)>;
