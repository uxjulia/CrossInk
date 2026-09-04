#pragma once

#include <cstdint>
#include <memory>
#include <string>

class Activity;

enum class FrontlightPanelAction : uint8_t {
  None = 0,
  ReadingStats,
  SyncTransfer,
  DarkMode,
  GlobalSettings,
  ToggleTouchscreen,
  Home,
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

constexpr bool shouldShowStickyReaderDetails(const bool hasStickyReaderDetailsPanel, const bool hasFrontlight,
                                             const bool activeReaderBook) {
  return hasStickyReaderDetailsPanel && !hasFrontlight && activeReaderBook;
}

constexpr bool supportsFrontlightDrawer(const bool hasTouchHardware, const bool hasFrontlight,
                                        const bool hasReaderDetailsPanel = false) {
  return hasTouchHardware && (hasFrontlight || hasReaderDetailsPanel);
}

struct FrontlightPanelBookDetails {
  std::string title;
  std::string author;
  std::string chapter;
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
  std::string bookPath;
  bool inversionChanged = false;
  bool touchscreenChanged = false;
};

struct FrontlightPanelContext {
  Activity* sourceActivity = nullptr;
  // An open reader of any supported format. This controls reader header chrome
  // and Home navigation; EPUB-only actions remain gated by activeEpub.
  bool activeReaderBook = false;
  bool activeEpub = false;
  bool showReaderDetails = false;
  std::string bookTitle;
  std::string bookPath;
  FrontlightPanelBookDetails bookDetails;
  std::unique_ptr<Activity> readingStatsActivity;
  FrontlightDrawerState drawerState{};
};
