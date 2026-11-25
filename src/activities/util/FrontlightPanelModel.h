#pragma once

#include <cstdint>
#include <memory>
#include <string>

class Activity;

enum class FrontlightPanelAction : uint8_t {
  None = 0,
  SyncProgress,
  NearbyPositionSync,
  SendNearbyBook,
};

enum class FrontlightBookSource : uint8_t { CurrentBook = 0, LastBook, DeviceOnly };

constexpr FrontlightBookSource chooseFrontlightBookSource(const bool activeEpub, const bool currentBookValid,
                                                          const bool lastBookValid) {
  if (activeEpub && currentBookValid) return FrontlightBookSource::CurrentBook;
  if (lastBookValid) return FrontlightBookSource::LastBook;
  return FrontlightBookSource::DeviceOnly;
}

constexpr bool hasFrontlightActiveReaderBook(const bool isReaderActivity, const bool currentBookValid) {
  return isReaderActivity && currentBookValid;
}

// Every touch reader can show book details when there are no light controls.
constexpr bool shouldShowReaderDetails(const bool hasFrontlight, const bool activeReaderBook) {
  return !hasFrontlight && activeReaderBook;
}

constexpr bool supportsReaderTopDrawer(const bool hasTouchHardware, const bool hasFrontlight,
                                       const bool isReaderActivity) {
  return hasTouchHardware && (hasFrontlight || isReaderActivity);
}

struct FrontlightPanelBookDetails {
  std::string title;
  std::string author;
  std::string chapter;
  uint32_t chapterPage = 0;
  uint32_t chapterPageCount = 0;
  bool chapterPageCountEstimated = false;
  int progressPercent = 0;
};

struct FrontlightDrawerState {
  int8_t selectedAction = 0;
  bool syncDialogOpen = false;
};

struct FrontlightPanelResult {
  FrontlightPanelAction action = FrontlightPanelAction::None;
  FrontlightDrawerState state{};
  bool activeEpub = false;
  bool ttfRenderingChanged = false;
  std::string bookPath;
};

struct FrontlightPanelContext {
  Activity* sourceActivity = nullptr;
  // An open reader of any supported format. This controls reader header chrome
  // and Home navigation; activeEpub identifies the reader save/handoff path.
  bool activeReaderBook = false;
  bool activeEpub = false;
  bool showReaderDetails = false;
  bool showReadingStatsAction = true;
  std::string bookTitle;
  std::string bookPath;
  FrontlightPanelBookDetails bookDetails;
  std::unique_ptr<Activity> readingStatsActivity;
  FrontlightDrawerState drawerState{};
};
