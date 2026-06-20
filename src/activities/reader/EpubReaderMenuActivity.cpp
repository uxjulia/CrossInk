#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>

#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "EpubReaderClippingListActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/icons/settings2.h"
#include "fontIds.h"

namespace {

constexpr uint8_t MenuIcon24[] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf,
    0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf,
    0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf,
    0xf3, 0xe7, 0xcf, 0xf3, 0xe7, 0xcf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static_assert(sizeof(MenuIcon24) == 24 * ((24 + 7) / 8), "MenuIcon24 must contain 24 rows of 1-bit icon data");

constexpr int tabIconSize = 24;
constexpr int selectedTabBoxWidth = 50;
constexpr int selectedTabBoxHeight = 34;
constexpr int selectedTabBoxRadius = 2;

struct ReaderLayoutSettingsSnapshot {
  uint8_t fontFamily;
  uint8_t fontSize;
  uint8_t lineHeightPercent;
  uint8_t orientation;
  uint8_t screenMargin;
  uint8_t publisherPageNumbers;
  uint8_t paragraphAlignment;
  uint8_t embeddedStyle;
  uint8_t hyphenationEnabled;
  uint8_t textAntiAliasing;
  uint8_t readerDarkMode;
  uint8_t imageRendering;
  uint8_t extraParagraphSpacing;
  uint8_t forceParagraphIndents;
  uint8_t bionicReadingEnabled;
  uint8_t guideReadingEnabled;
  uint8_t epubRenderMode;
  char sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName)] = {};

  bool operator==(const ReaderLayoutSettingsSnapshot& other) const {
    return fontFamily == other.fontFamily && fontSize == other.fontSize &&
           lineHeightPercent == other.lineHeightPercent && orientation == other.orientation &&
           screenMargin == other.screenMargin && publisherPageNumbers == other.publisherPageNumbers &&
           paragraphAlignment == other.paragraphAlignment && embeddedStyle == other.embeddedStyle &&
           hyphenationEnabled == other.hyphenationEnabled && textAntiAliasing == other.textAntiAliasing &&
           readerDarkMode == other.readerDarkMode && imageRendering == other.imageRendering &&
           extraParagraphSpacing == other.extraParagraphSpacing &&
           forceParagraphIndents == other.forceParagraphIndents && bionicReadingEnabled == other.bionicReadingEnabled &&
           guideReadingEnabled == other.guideReadingEnabled && epubRenderMode == other.epubRenderMode &&
           std::strncmp(sdFontFamilyName, other.sdFontFamilyName, sizeof(sdFontFamilyName)) == 0;
  }
  bool operator!=(const ReaderLayoutSettingsSnapshot& other) const { return !(*this == other); }
};

ReaderLayoutSettingsSnapshot captureReaderLayoutSettings() {
  ReaderLayoutSettingsSnapshot snapshot{
      SETTINGS.fontFamily,
      SETTINGS.fontSize,
      SETTINGS.lineHeightPercent,
      SETTINGS.orientation,
      SETTINGS.screenMargin,
      SETTINGS.publisherPageNumbers,
      SETTINGS.paragraphAlignment,
      SETTINGS.embeddedStyle,
      SETTINGS.hyphenationEnabled,
      SETTINGS.textAntiAliasing,
      SETTINGS.readerDarkMode,
      SETTINGS.imageRendering,
      SETTINGS.extraParagraphSpacing,
      SETTINGS.forceParagraphIndents,
      SETTINGS.bionicReadingEnabled,
      SETTINGS.guideReadingEnabled,
      SETTINGS.epubRenderMode,
  };
  std::strncpy(snapshot.sdFontFamilyName, SETTINGS.sdFontFamilyName, sizeof(snapshot.sdFontFamilyName) - 1);
  snapshot.sdFontFamilyName[sizeof(snapshot.sdFontFamilyName) - 1] = '\0';
  return snapshot;
}

bool haveReaderLayoutSettingsChanged(const ReaderLayoutSettingsSnapshot& before) {
  return before != captureReaderLayoutSettings();
}

void drawBookmarkTabIcon(const GfxRenderer& renderer, int x, int y, const bool foregroundBlack = true) {
  constexpr int ribbonWidth = 16;
  constexpr int ribbonHeight = 22;
  constexpr int notchSize = 6;
  const int iconX = x + (tabIconSize - ribbonWidth) / 2;
  const int iconY = y + 1;
  const int centerX = iconX + ribbonWidth / 2;

  const int polyX[5] = {iconX, iconX + ribbonWidth, iconX + ribbonWidth, centerX, iconX};
  const int polyY[5] = {iconY, iconY, iconY + ribbonHeight, iconY + ribbonHeight - notchSize, iconY + ribbonHeight};
  renderer.fillPolygon(polyX, polyY, 5, foregroundBlack);
}

}  // namespace

EpubReaderMenuActivity::EpubReaderMenuActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title, const int currentPage,
    const int totalPages, const int bookProgressPercent, const uint8_t currentOrientation, const bool hasFootnotes,
    const bool hasBookmarks, const bool hasClippings, const bool isCurrentPageBookmarked, const bool isBookCompleted,
    const bool autoPageTurnActive, const uint16_t autoPageTurnIntervalSeconds, const bool showReadingPaceReset,
    ReaderOptionsActivity::SaveSettingsCallback saveReaderSettingsCallback, void* saveReaderSettingsContext,
    ReaderOptionsActivity::SaveGlobalSettingsCallback saveGlobalSettingsCallback, void* saveGlobalSettingsContext,
    ReaderOptionsActivity::GlobalSettingsEditCallback beginGlobalSettingsEditCallback,
    void* beginGlobalSettingsEditContext, const bool stablePageNumbersAvailable,
    ReaderOptionsActivity::GlobalSettingsEditCallback endGlobalSettingsEditCallback, void* endGlobalSettingsEditContext)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasFootnotes, hasBookmarks, hasClippings, isCurrentPageBookmarked, isBookCompleted,
                               showReadingPaceReset)),
      title(title),
      pendingOrientation(currentOrientation),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent),
      autoPageTurnActive(autoPageTurnActive),
      autoPageTurnIntervalSeconds(autoPageTurnIntervalSeconds),
      saveReaderSettingsCallback(saveReaderSettingsCallback),
      saveReaderSettingsContext(saveReaderSettingsContext),
      saveGlobalSettingsCallback(saveGlobalSettingsCallback),
      saveGlobalSettingsContext(saveGlobalSettingsContext),
      beginGlobalSettingsEditCallback(beginGlobalSettingsEditCallback),
      beginGlobalSettingsEditContext(beginGlobalSettingsEditContext),
      stablePageNumbersAvailable(stablePageNumbersAvailable),
      endGlobalSettingsEditCallback(endGlobalSettingsEditCallback),
      endGlobalSettingsEditContext(endGlobalSettingsEditContext) {}

EpubReaderMenuActivity::TabMenuItems EpubReaderMenuActivity::buildMenuItems(bool hasFootnotes, bool hasBookmarks,
                                                                            bool hasClippings,
                                                                            bool isCurrentPageBookmarked,
                                                                            bool isBookCompleted,
                                                                            bool showReadingPaceReset) {
  TabMenuItems items;
  auto& mainItems = items[MAIN_TAB_INDEX];
  auto& bookmarkItems = items[BOOKMARKS_TAB_INDEX];
  auto& settingsItems = items[SETTINGS_TAB_INDEX];

  mainItems.reserve(8 + (hasFootnotes ? 1u : 0u));
  bookmarkItems.reserve(7 + (hasBookmarks ? 2u : 0u) + (hasClippings ? 1u : 0u));
  settingsItems.reserve(2 + (showReadingPaceReset ? 1u : 0u));

  if (hasFootnotes) {
    mainItems.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  mainItems.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  mainItems.push_back({MenuAction::READER_OPTIONS, StrId::STR_READER_OPTIONS});
  mainItems.push_back({MenuAction::CONTROLS_OPTIONS, StrId::STR_CAT_CONTROLS});
  mainItems.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  mainItems.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_INTERVAL_SECONDS});
  mainItems.push_back({MenuAction::READING_STATS, StrId::STR_READING_STATS});
  mainItems.push_back(
      {MenuAction::TOGGLE_COMPLETED, isBookCompleted ? StrId::STR_MARK_UNFINISHED : StrId::STR_MARK_FINISHED});

  bookmarkItems.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  bookmarkItems.push_back({MenuAction::SAVE_CLIPPING, StrId::STR_SAVE_CLIPPING});
  if (hasClippings) {
    bookmarkItems.push_back({MenuAction::VIEW_CLIPPINGS, StrId::STR_VIEW_CLIPPINGS});
  }
  bookmarkItems.push_back(
      {MenuAction::BOOKMARK_TOGGLE, isCurrentPageBookmarked ? StrId::STR_REMOVE_BOOKMARK : StrId::STR_ADD_BOOKMARK});
  if (hasBookmarks) {
    bookmarkItems.push_back({MenuAction::VIEW_BOOKMARKS, StrId::STR_VIEW_BOOKMARKS});
    bookmarkItems.push_back({MenuAction::DELETE_BOOKMARKS, StrId::STR_DELETE_BOOKMARKS});
  }
  bookmarkItems.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  bookmarkItems.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});

  settingsItems.push_back({MenuAction::DELETE_STATS, StrId::STR_DELETE_BOOK_STATS});
  settingsItems.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  if (showReadingPaceReset) {
    settingsItems.push_back({MenuAction::RESET_READING_PACE, StrId::STR_RESET_READING_PACE});
  }
  return items;
}

const std::vector<EpubReaderMenuActivity::MenuItem>& EpubReaderMenuActivity::activeMenuItems() const {
  return menuItems[activeTabIndex()];
}

void EpubReaderMenuActivity::focusTabRow() { selectedIndex = -1; }

void EpubReaderMenuActivity::cycleActiveTab() {
  const auto nextTabIndex = ButtonNavigator::nextIndex(static_cast<int>(activeTabIndex()), MENU_TAB_COUNT);
  activeTab = static_cast<MenuTab>(nextTabIndex);
  focusTabRow();
  requestUpdate();
}

void EpubReaderMenuActivity::finishCancelled() {
  ActivityResult result;
  result.isCancelled = true;
  result.data = MenuResult{-1, pendingOrientation, settingsChanged};
  setResult(std::move(result));
  finish();
}

void EpubReaderMenuActivity::drawIconTabBar(const Rect rect) const {
  renderer.drawLine(rect.x, rect.y, rect.x + rect.width - 1, rect.y, true);
  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);

  for (size_t i = 0; i < MENU_TAB_COUNT; i++) {
    const int slotX = rect.x + static_cast<int>((i * rect.width) / MENU_TAB_COUNT);
    const int nextSlotX = rect.x + static_cast<int>(((i + 1) * rect.width) / MENU_TAB_COUNT);
    const int slotWidth = nextSlotX - slotX;
    const int centerX = slotX + slotWidth / 2;
    const bool selected = i == activeTabIndex();
    const bool tabFocused = selected && selectedIndex < 0;
    const int boxX = centerX - selectedTabBoxWidth / 2;
    const int boxY = rect.y + (rect.height - selectedTabBoxHeight) / 2;
    const int iconX = centerX - tabIconSize / 2;
    const int iconY = rect.y + (rect.height - tabIconSize) / 2;

    if (tabFocused) {
      renderer.fillRoundedRect(boxX, boxY, selectedTabBoxWidth, selectedTabBoxHeight, selectedTabBoxRadius,
                               Color::Black);
    } else if (selected) {
      renderer.drawRoundedRect(boxX, boxY, selectedTabBoxWidth, selectedTabBoxHeight, 1, selectedTabBoxRadius, true);
    }

    if (i == static_cast<size_t>(MenuTab::Main)) {
      if (tabFocused) {
        renderer.drawIconInverted(MenuIcon24, iconX, iconY, tabIconSize, tabIconSize);
      } else {
        renderer.drawIcon(MenuIcon24, iconX, iconY, tabIconSize, tabIconSize);
      }
    } else if (i == static_cast<size_t>(MenuTab::Bookmarks)) {
      drawBookmarkTabIcon(renderer, iconX, iconY, !tabFocused);
    } else {
      if (tabFocused) {
        renderer.drawIconInverted(Settings2Icon24, iconX, iconY, tabIconSize, tabIconSize);
      } else {
        renderer.drawIcon(Settings2Icon24, iconX, iconY, tabIconSize, tabIconSize);
      }
    }
  }
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() { Activity::onExit(); }

void EpubReaderMenuActivity::loop() {
  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    const int menuCount = static_cast<int>(activeMenuItems().size());
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex + 1, menuCount + 1) - 1;
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    const int menuCount = static_cast<int>(activeMenuItems().size());
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex + 1, menuCount + 1) - 1;
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex < 0) {
      cycleActiveTab();
      return;
    }

    const auto& items = activeMenuItems();
    if (selectedIndex >= static_cast<int>(items.size())) {
      focusTabRow();
      requestUpdate();
      return;
    }

    const auto selectedAction = items[selectedIndex].action;
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      // Cycle orientation preview locally; actual rotation happens on menu exit.
      pendingOrientation = (pendingOrientation + 1) % orientationLabels.size();
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::READER_OPTIONS) {
      const auto before = captureReaderLayoutSettings();
      startActivityForResult(
          std::make_unique<ReaderOptionsActivity>(
              renderer, mappedInput, saveReaderSettingsCallback, saveReaderSettingsContext, saveGlobalSettingsCallback,
              saveGlobalSettingsContext, beginGlobalSettingsEditCallback, beginGlobalSettingsEditContext,
              endGlobalSettingsEditCallback, endGlobalSettingsEditContext, stablePageNumbersAvailable),
          [this, before](const ActivityResult&) {
            settingsChanged = settingsChanged || haveReaderLayoutSettingsChanged(before);
            pendingOrientation = SETTINGS.orientation;  // sync in case orientation changed
            requestUpdate();
          });
      return;
    }

    if (selectedAction == MenuAction::CONTROLS_OPTIONS) {
      startActivityForResult(std::make_unique<ControlsOptionsActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) {
                               ActivityResult result;
                               result.isCancelled = true;
                               result.data = MenuResult{-1, pendingOrientation, settingsChanged};
                               setResult(std::move(result));
                               finish();
                             });
      return;
    }

    if (selectedAction == MenuAction::VIEW_CLIPPINGS) {
      startActivityForResult(
          std::make_unique<EpubReaderClippingListActivity>(renderer, mappedInput, CLIPPINGS.getClippings()),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              requestUpdate();
              return;
            }

            const auto* clipping = std::get_if<ClippingJumpResult>(&result.data);
            if (clipping == nullptr) {
              requestUpdate();
              return;
            }

            ClippingJumpResult menuResult = *clipping;
            menuResult.orientation = pendingOrientation;
            menuResult.settingsChanged = settingsChanged;
            setResult(std::move(menuResult));
            finish();
          });
      return;
    }

    setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, settingsChanged});
    finish();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (selectedIndex >= 0) {
      focusTabRow();
      requestUpdate();
      return;
    }
    finishCancelled();
    return;
  }
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str(), nullptr, true);

  // Progress summary
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  GUI.drawSubHeader(
      renderer,
      Rect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight, screen.width, metrics.tabBarHeight},
      progressLine.c_str());

  const Rect tabRect{screen.x, screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight,
                     screen.width, metrics.tabBarHeight};
  drawIconTabBar(tabRect);

  const int contentTop =
      screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight * 2 + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;
  const auto& items = activeMenuItems();

  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, items.size(), selectedIndex,
      [&items](int index) { return I18N.get(items[index].labelId); }, nullptr, nullptr,
      [this](int index) -> std::string {
        const auto& items = activeMenuItems();
        if (index < 0 || index >= static_cast<int>(items.size())) {
          return "";
        }
        const auto value = items[index].action;
        if (value == MenuAction::ROTATE_SCREEN) {
          // Render current orientation value on the right edge of the content area.
          return I18N.get(orientationLabels[pendingOrientation]);
        } else if (value == MenuAction::AUTO_PAGE_TURN) {
          // Render current page turn value on the right edge of the content area.
          return autoPageTurnActive ? std::to_string(autoPageTurnIntervalSeconds) : "";
        } else {
          return "";
        }
      },
      true);

  // Footer / Hints
  const auto confirmLabel = selectedIndex < 0 ? tr(STR_NEXT_FIELD) : tr(STR_SELECT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
