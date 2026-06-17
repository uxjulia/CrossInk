#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

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
  uint8_t pageTurnOption = 0;
};

struct ChapterResult {
  int spineIndex = 0;
  std::string anchor;
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

struct ProgressChangeResult {
  int spineIndex = 0;
  int page = 0;
};

struct SyncResult {
  int spineIndex = 0;
  int page = 0;
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

struct ReadingStatsResult {
  bool changed = false;
};

struct ClippingResult {
  std::string text;
  int fromWordIdx = -1;
  int toWordIdx = -1;
  uint16_t sectionPage = 0;
  uint16_t endSectionPage = 0;
  uint16_t sectionPageCount = 1;
  uint16_t startPageWordIndex = 0;
  uint16_t endPageWordIndex = 0;
  uint16_t paragraphIndex = UINT16_MAX;
  std::string startText;
  std::string endText;
  std::string beforeStartText;
  std::string afterEndText;
  std::string midText;
  uint16_t wordCount = 0;
};

struct ClippingJumpResult {
  uint16_t spineIndex = 0;
  uint16_t page = 0;
  uint16_t pageCount = 1;
  uint16_t paragraphIndex = UINT16_MAX;
  uint16_t clippingIndex = UINT16_MAX;
};

using ResultVariant = std::variant<std::monostate, WifiResult, KeyboardResult, MenuResult, ChapterResult, PercentResult,
                                   IntervalResult, OptionSelectionResult, PageResult, ProgressChangeResult, SyncResult,
                                   NetworkModeResult, FootnoteResult, BookmarkResult, FileBrowserActionResult,
                                   FilePathResult, ReadingStatsResult, ClippingResult, ClippingJumpResult>;

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
