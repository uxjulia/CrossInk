#pragma once
#include <Epub.h>
#include <I18n.h>

#include <array>
#include <string>
#include <vector>

#include "ControlsOptionsActivity.h"
#include "ReaderOptionsActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;

class EpubReaderMenuActivity final : public Activity {
 public:
  // Menu actions available from the reader menu.
  enum class MenuAction {
    SELECT_CHAPTER,
    FOOTNOTES,
    GO_TO_PERCENT,
    AUTO_PAGE_TURN,
    ROTATE_SCREEN,
    SCREENSHOT,
    DISPLAY_QR,
    GO_HOME,
    SYNC,
    NEARBY_POSITION_SYNC,
    DELETE_STATS,
    DELETE_CACHE,
    RESET_READING_PACE,
    READING_STATS,
    TOGGLE_COMPLETED,
    READER_OPTIONS,
    CONTROLS_OPTIONS,
    BOOKMARK_TOGGLE,
    VIEW_BOOKMARKS,
    DELETE_BOOKMARKS,
    SAVE_CLIPPING,
    VIEW_CLIPPINGS
  };

  explicit EpubReaderMenuActivity(
      GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title, const int currentPage,
      const int totalPages, const int bookProgressPercent, const uint8_t currentOrientation, const bool hasFootnotes,
      const bool hasBookmarks, const bool hasClippings, const bool isCurrentPageBookmarked, const bool isBookCompleted,
      const bool autoPageTurnActive = false, const uint16_t autoPageTurnIntervalSeconds = 0,
      const bool showReadingPaceReset = false,
      ReaderOptionsActivity::SaveSettingsCallback saveReaderSettingsCallback = nullptr,
      void* saveReaderSettingsContext = nullptr,
      ReaderOptionsActivity::SaveGlobalSettingsCallback saveGlobalSettingsCallback = nullptr,
      void* saveGlobalSettingsContext = nullptr,
      ReaderOptionsActivity::GlobalSettingsEditCallback beginGlobalSettingsEditCallback = nullptr,
      void* beginGlobalSettingsEditContext = nullptr, bool stablePageNumbersAvailable = false,
      ReaderOptionsActivity::GlobalSettingsEditCallback endGlobalSettingsEditCallback = nullptr,
      void* endGlobalSettingsEditContext = nullptr);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  enum class MenuTab : uint8_t { Main = 0, Bookmarks = 1, Settings = 2 };
  static constexpr size_t MAIN_TAB_INDEX = 0;
  static constexpr size_t BOOKMARKS_TAB_INDEX = 1;
  static constexpr size_t SETTINGS_TAB_INDEX = 2;
  static constexpr size_t MENU_TAB_COUNT = 3;
  using TabMenuItems = std::array<std::vector<MenuItem>, MENU_TAB_COUNT>;

  static TabMenuItems buildMenuItems(bool hasFootnotes, bool hasBookmarks, bool hasClippings,
                                     bool isCurrentPageBookmarked, bool isBookCompleted, bool showReadingPaceReset);
  [[nodiscard]] const std::vector<MenuItem>& activeMenuItems() const;
  [[nodiscard]] size_t activeTabIndex() const { return static_cast<size_t>(activeTab); }
  void cycleActiveTab();
  void focusTabRow();
  void finishCancelled();
  void drawIconTabBar(Rect rect) const;

  // Fixed menu layout
  const TabMenuItems menuItems;

  int selectedIndex = -1;
  MenuTab activeTab = MenuTab::Main;

  ButtonNavigator buttonNavigator;
  std::string title = "Reader Menu";
  uint8_t pendingOrientation = 0;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                StrId::STR_LANDSCAPE_CCW};
  int currentPage = 0;
  int totalPages = 0;
  int bookProgressPercent = 0;
  bool autoPageTurnActive = false;
  uint16_t autoPageTurnIntervalSeconds = 0;
  ReaderOptionsActivity::SaveSettingsCallback saveReaderSettingsCallback = nullptr;
  void* saveReaderSettingsContext = nullptr;
  ReaderOptionsActivity::SaveGlobalSettingsCallback saveGlobalSettingsCallback = nullptr;
  void* saveGlobalSettingsContext = nullptr;
  ReaderOptionsActivity::GlobalSettingsEditCallback beginGlobalSettingsEditCallback = nullptr;
  void* beginGlobalSettingsEditContext = nullptr;
  bool stablePageNumbersAvailable = false;
  ReaderOptionsActivity::GlobalSettingsEditCallback endGlobalSettingsEditCallback = nullptr;
  void* endGlobalSettingsEditContext = nullptr;
  bool settingsChanged = false;
};
