#include "EpubReaderMenuActivity.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>

#include "ClippingStore.h"
#include "CrossInkHalFrontlight.h"
#include "CrossPointSettings.h"
#include "EpubReaderClippingListActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "MappedInputManager.h"
#include "Memory.h"
#include "ReaderUtils.h"
#include "components/TouchHeaderBackButton.h"
#include "components/TouchRegistry.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/touchscreenStateIcons.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {

constexpr fui::ActionId ACTION_ROW = 1;

constexpr int tabIconSize = 24;
constexpr int selectedTabBoxWidth = 50;
constexpr int selectedTabBoxHeight = 34;
constexpr int selectedTabBoxRadius = 2;
constexpr int headerActionHitSize = 44;
constexpr int headerActionTouchSize = 60;
constexpr int headerActionGap = 10;
constexpr int headerActionRightPadding = 10;
constexpr int inlineBatteryReserve = 72;
constexpr int touchReaderMenuRowHeightScale = 2;
constexpr int touchReaderMenuTabBarHeightScale = 2;
int readerMenuRowHeightScale(const bool hasTouch) { return hasTouch ? touchReaderMenuRowHeightScale : 1; }

int readerMenuTabBarHeight(const int baseTabBarHeight, const bool hasTouch) {
  return baseTabBarHeight * (hasTouch ? touchReaderMenuTabBarHeightScale : 1);
}

#if CROSSINK_APP_CAP_TOUCH
bool readerMenuTabsAtBottom(const MappedInputManager& mappedInput) {
  // Frontlight boards reserve the top-edge down-swipe for the quick panel, so
  // the reader menu opens from the bottom and its tabs should stay thumb-close.
  return mappedInput.hasTouch() && Frontlight.present();
}
#endif

Rect readerMenuHeaderRect(const GfxRenderer& renderer, const MappedInputManager& mappedInput) {
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouch(), false);
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{screen.x, screen.y + metrics.topPadding, screen.width,
              TouchHeaderBackButton::height(metrics, mappedInput)};
}

bool rectContains(const Rect& rect, const int x, const int y) {
  return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

struct ReaderLayoutSettingsSnapshot {
  uint8_t fontFamily;
  uint8_t readerFontPointSize;
  uint8_t lineHeightPercent;
  uint8_t wordSpacing;
  uint8_t orientation;
  uint8_t screenMarginVertical;
  uint8_t screenMarginHorizontal;
  uint8_t publisherPageNumbers;
  uint8_t paragraphAlignment;
  uint8_t embeddedStyle;
  uint8_t hyphenationEnabled;
  uint8_t textAntiAliasing;
  uint8_t imageRendering;
  uint8_t extraParagraphSpacing;
  uint8_t forceParagraphIndents;
  uint8_t focusReadingEnabled;
  uint8_t guideReadingEnabled;
  uint8_t epubRenderMode;
  // Indexing method is a build policy, not a layout input. A mode-only change
  // keeps the live section/parser and takes effect when the next chapter opens.
  char sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName)] = {};
};

ReaderLayoutSettingsSnapshot captureReaderLayoutSettings() {
  ReaderLayoutSettingsSnapshot snapshot{
      SETTINGS.fontFamily,
      SETTINGS.readerFontPointSize,
      SETTINGS.lineHeightPercent,
      SETTINGS.wordSpacing,
      SETTINGS.orientation,
      SETTINGS.screenMarginVertical,
      SETTINGS.screenMarginHorizontal,
      SETTINGS.publisherPageNumbers,
      SETTINGS.paragraphAlignment,
      SETTINGS.embeddedStyle,
      SETTINGS.hyphenationEnabled,
      SETTINGS.textAntiAliasing,
      SETTINGS.imageRendering,
      SETTINGS.extraParagraphSpacing,
      SETTINGS.forceParagraphIndents,
      SETTINGS.focusReadingEnabled,
      SETTINGS.guideReadingEnabled,
      SETTINGS.epubRenderMode,
  };
  std::strncpy(snapshot.sdFontFamilyName, SETTINGS.sdFontFamilyName, sizeof(snapshot.sdFontFamilyName) - 1);
  snapshot.sdFontFamilyName[sizeof(snapshot.sdFontFamilyName) - 1] = '\0';
  return snapshot;
}

ReaderSettingsChangeMask classifyReaderSettingsChange(const ReaderLayoutSettingsSnapshot& before,
                                                      const ReaderLayoutSettingsSnapshot& after) {
  ReaderSettingsChangeMask changeMask = ReaderSettingsChangeMask::None;

  if (before.textAntiAliasing != after.textAntiAliasing) {
    changeMask = changeMask | ReaderSettingsChangeMask::NonLayout;
  }
  if (before.orientation != after.orientation) {
    changeMask = changeMask | ReaderSettingsChangeMask::Orientation;
  }
  if (before.fontFamily != after.fontFamily || before.readerFontPointSize != after.readerFontPointSize ||
      before.lineHeightPercent != after.lineHeightPercent || before.wordSpacing != after.wordSpacing ||
      before.screenMarginVertical != after.screenMarginVertical ||
      before.screenMarginHorizontal != after.screenMarginHorizontal ||
      before.publisherPageNumbers != after.publisherPageNumbers ||
      before.paragraphAlignment != after.paragraphAlignment || before.embeddedStyle != after.embeddedStyle ||
      before.hyphenationEnabled != after.hyphenationEnabled ||
      before.extraParagraphSpacing != after.extraParagraphSpacing ||
      before.forceParagraphIndents != after.forceParagraphIndents ||
      before.focusReadingEnabled != after.focusReadingEnabled ||
      before.guideReadingEnabled != after.guideReadingEnabled || before.imageRendering != after.imageRendering ||
      before.epubRenderMode != after.epubRenderMode ||
      std::strncmp(before.sdFontFamilyName, after.sdFontFamilyName, sizeof(before.sdFontFamilyName)) != 0) {
    changeMask = changeMask | ReaderSettingsChangeMask::Relayout;
  }
  return changeMask;
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

Rect readerMenuHeaderActionRect(const Rect& header, const ThemeMetrics& metrics) {
  const int actionHeight = std::min(header.height, headerActionHitSize);
  // Compact headers keep the battery inline at the right edge, so leave its
  // widest percentage-and-glyph band untouched. Tall detached headers place
  // Home in the lower title band, as in the touch-menu design.
  const int rightInset = (metrics.headerBatteryDetached ? 0 : inlineBatteryReserve) + headerActionRightPadding;
  return Rect{header.x + header.width - rightInset - headerActionHitSize, header.y + header.height - actionHeight,
              headerActionHitSize, actionHeight};
}

Rect readerMenuHeaderActionTouchRect(const Rect& header, const Rect& actionRect) {
  const int touchWidth = std::min(headerActionTouchSize, header.width);
  const int touchX = actionRect.x + actionRect.width - touchWidth;
  // The title reserves the space left of touchX. Treat the remaining header
  // corner, including the non-interactive battery area, as Home so the icon is
  // easy to hit without changing its visual placement.
  return Rect{touchX, header.y, header.x + header.width - touchX, header.height};
}

void drawSdkIcon(fui::GfxRendererTarget& target, const freeink::Icon& icon, const int x, const int y,
                 const bool foregroundBlack = true) {
  // FreeInkUI's target maps logical pixels through the renderer, keeping the
  // non-pre-rotated SDK assets upright in every reader orientation.
  target.bitmap(fui::Rect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(icon.w),
                          static_cast<int16_t>(icon.h)},
                fui::bitmapFromIcon(icon), fui::BitmapMode::Center,
                fui::Paint::solid(foregroundBlack ? fui::Color::Black : fui::Color::White));
}

}  // namespace

EpubReaderMenuActivity::EpubReaderMenuActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title, const int currentPage,
    const int totalPages, const int bookProgressPercent, const uint8_t currentOrientation, const bool hasFootnotes,
    const bool hasDictionary, const bool hasBookmarks, const bool hasClippings, const bool isCurrentPageBookmarked,
    const bool isBookCompleted, const bool autoPageTurnActive, const uint16_t autoPageTurnIntervalSeconds,
    const bool showReadingPaceReset, ReaderOptionsActivity::SaveSettingsCallback saveReaderSettingsCallback,
    void* saveReaderSettingsContext, ReaderOptionsActivity::SaveGlobalSettingsCallback saveGlobalSettingsCallback,
    void* saveGlobalSettingsContext, ReaderOptionsActivity::GlobalSettingsEditCallback beginGlobalSettingsEditCallback,
    void* beginGlobalSettingsEditContext, const uint32_t stableCurrentPage, const uint32_t stablePageCount,
    ReaderOptionsActivity::GlobalSettingsEditCallback endGlobalSettingsEditCallback, void* endGlobalSettingsEditContext,
    const char* dictionaryFontFamilyName, const uint8_t dictionaryFontPointSize, const bool hasDictionaryFontOverride,
    ReaderOptionsActivity::DictionaryFontChangedCallback dictionaryFontChangedCallback,
    void* dictionaryFontChangedContext)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasFootnotes, hasBookmarks, hasClippings, isCurrentPageBookmarked, isBookCompleted,
                               showReadingPaceReset, hasDictionary, stablePageCount > 0)),
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
      stableCurrentPage(stableCurrentPage),
      stablePageCount(stablePageCount),
      endGlobalSettingsEditCallback(endGlobalSettingsEditCallback),
      endGlobalSettingsEditContext(endGlobalSettingsEditContext),
      dictionaryFontPointSize(dictionaryFontPointSize),
      hasDictionaryFontOverride(hasDictionaryFontOverride),
      dictionaryFontChangedCallback(dictionaryFontChangedCallback),
      dictionaryFontChangedContext(dictionaryFontChangedContext),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {
  if (dictionaryFontFamilyName) {
    std::strncpy(this->dictionaryFontFamilyName, dictionaryFontFamilyName, sizeof(this->dictionaryFontFamilyName) - 1);
  }
}

EpubReaderMenuActivity::TabMenuItems EpubReaderMenuActivity::buildMenuItems(
    bool hasFootnotes, bool hasBookmarks, bool hasClippings, bool isCurrentPageBookmarked, bool isBookCompleted,
    bool showReadingPaceReset, bool hasDictionary, bool hasStablePageNumbers) {
  TabMenuItems items;
  auto& mainItems = items[MAIN_TAB_INDEX];
  auto& bookmarkItems = items[BOOKMARKS_TAB_INDEX];
  auto& settingsItems = items[SETTINGS_TAB_INDEX];

  mainItems.reserve(9 + (hasFootnotes ? 1u : 0u) + (hasDictionary ? 2u : 0u));
  bookmarkItems.reserve(9 + (hasBookmarks ? 2u : 0u) + (hasClippings ? 1u : 0u));
  settingsItems.reserve(6);

  if (hasFootnotes) {
    mainItems.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  if (hasDictionary) {
    mainItems.push_back({MenuAction::LOOKUP, StrId::STR_LOOKUP});
    mainItems.push_back({MenuAction::LOOKUP_HISTORY, StrId::STR_LOOKUP_HISTORY});
  }
  mainItems.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  mainItems.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  if (hasStablePageNumbers) {
    mainItems.push_back({MenuAction::GO_TO_STABLE_PAGE, StrId::STR_GO_TO_STABLE_PAGE});
  }
  mainItems.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_INTERVAL_SECONDS});
  mainItems.push_back({MenuAction::READING_STATS, StrId::STR_READING_STATS});
  mainItems.push_back({MenuAction::READER_OPTIONS, StrId::STR_READER_OPTIONS});
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
  bookmarkItems.push_back({MenuAction::SYNC, StrId::STR_SYNC_PROGRESS});
  bookmarkItems.push_back({MenuAction::NEARBY_POSITION_SYNC, StrId::STR_NEARBY_POSITION_SYNC});
  bookmarkItems.push_back({MenuAction::SEND_NEARBY_BOOK, StrId::STR_SEND_NEARBY_BOOK});
  bookmarkItems.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  bookmarkItems.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});

  settingsItems.push_back({MenuAction::SET_BOOK_DICTIONARY, StrId::STR_BOOK_DICTIONARY});
  settingsItems.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  settingsItems.push_back({MenuAction::DELETE_STATS, StrId::STR_DELETE_BOOK_STATS});
  if (showReadingPaceReset) {
    settingsItems.push_back({MenuAction::RESET_READING_PACE, StrId::STR_RESET_READING_PACE});
  }
  settingsItems.push_back({MenuAction::CONTROLS_OPTIONS, StrId::STR_CAT_CONTROLS});
  settingsItems.push_back(
      {MenuAction::TOGGLE_COMPLETED, isBookCompleted ? StrId::STR_MARK_UNFINISHED : StrId::STR_MARK_FINISHED});
  return items;
}

void EpubReaderMenuActivity::dictionaryFontChangedForMenu(void* ctx, const char* familyName, const uint8_t pointSize) {
  auto* self = static_cast<EpubReaderMenuActivity*>(ctx);
  if (!self) return;

  if (familyName && familyName[0] != '\0') {
    self->hasDictionaryFontOverride = true;
    std::strncpy(self->dictionaryFontFamilyName, familyName, sizeof(self->dictionaryFontFamilyName) - 1);
    self->dictionaryFontFamilyName[sizeof(self->dictionaryFontFamilyName) - 1] = '\0';
  } else {
    self->hasDictionaryFontOverride = false;
    std::strncpy(self->dictionaryFontFamilyName, SETTINGS.dictionarySdFontFamilyName,
                 sizeof(self->dictionaryFontFamilyName) - 1);
    self->dictionaryFontFamilyName[sizeof(self->dictionaryFontFamilyName) - 1] = '\0';
    self->dictionaryFontPointSize = SETTINGS.dictionaryFontPointSize;
  }
  if (self->hasDictionaryFontOverride) {
    self->dictionaryFontPointSize = pointSize;
  }
  if (self->dictionaryFontChangedCallback) {
    self->dictionaryFontChangedCallback(self->dictionaryFontChangedContext,
                                        self->hasDictionaryFontOverride ? self->dictionaryFontFamilyName : nullptr,
                                        self->dictionaryFontPointSize);
  }
}

const std::vector<EpubReaderMenuActivity::MenuItem>& EpubReaderMenuActivity::activeMenuItems() const {
  return menuItems[activeTabIndex()];
}

void EpubReaderMenuActivity::focusTabRow() {
  selectedIndex = -1;
  topIndex = 0;
}

void EpubReaderMenuActivity::cycleActiveTab() { moveActiveTab(true); }

void EpubReaderMenuActivity::moveActiveTab(const bool forward) {
  const int nextTabIndex = forward ? ButtonNavigator::nextIndex(static_cast<int>(activeTabIndex()), MENU_TAB_COUNT)
                                   : ButtonNavigator::previousIndex(static_cast<int>(activeTabIndex()), MENU_TAB_COUNT);
  activeTab = static_cast<MenuTab>(nextTabIndex);
  focusTabRow();
  requestUpdate();
}

void EpubReaderMenuActivity::finishCancelled() {
  ActivityResult result;
  result.isCancelled = true;
  result.data = makeMenuResult(-1);
  setResult(std::move(result));
  finish();
}

MenuResult EpubReaderMenuActivity::makeMenuResult(const int action) const {
  MenuResult result{action, pendingOrientation, settingsChanged};
  result.changeMask = changeMask;
  return result;
}

bool EpubReaderMenuActivity::activateSelectedItem() {
  if (selectedIndex < 0) {
    cycleActiveTab();
    return true;
  }

  const auto& items = activeMenuItems();
  if (selectedIndex >= static_cast<int>(items.size())) {
    focusTabRow();
    requestUpdate();
    return true;
  }

  const auto selectedAction = items[selectedIndex].action;
  if (selectedAction == MenuAction::ROTATE_SCREEN) {
    optionPopup.show(StrId::STR_ORIENTATION, orientationLabels.data(), static_cast<int>(orientationLabels.size()),
                     pendingOrientation, [this](int idx) {
                       pendingOrientation = idx;
                       // Rotate the menu immediately while leaving the saved
                       // reader orientation for the result handler to apply.
                       ReaderUtils::applyOrientation(renderer, pendingOrientation);
                       app.setDevice(uiTarget.deviceContext());
                       requestUpdate(true);
                     });
    requestUpdate();
    return true;
  }

  if (selectedAction == MenuAction::READER_OPTIONS) {
    const auto before = captureReaderLayoutSettings();
    startActivityForResult(
        std::make_unique<ReaderOptionsActivity>(
            renderer, mappedInput, saveReaderSettingsCallback, saveReaderSettingsContext, saveGlobalSettingsCallback,
            saveGlobalSettingsContext, beginGlobalSettingsEditCallback, beginGlobalSettingsEditContext,
            endGlobalSettingsEditCallback, endGlobalSettingsEditContext, stablePageCount > 0, dictionaryFontFamilyName,
            dictionaryFontPointSize, hasDictionaryFontOverride, dictionaryFontChangedForMenu, this),
        [this, before](const ActivityResult& result) {
          const ReaderSettingsChangeMask changed = classifyReaderSettingsChange(before, captureReaderLayoutSettings());
          if (changed != ReaderSettingsChangeMask::None) {
            settingsChanged = true;
            changeMask = changeMask | changed;
          }
          pendingOrientation = SETTINGS.orientation;  // sync in case orientation changed
          if (result.isCancelled) {
            finishCancelled();
            return;
          }
          requestUpdate();
        });
    return true;
  }

  if (selectedAction == MenuAction::CONTROLS_OPTIONS) {
    if (beginGlobalSettingsEditCallback) beginGlobalSettingsEditCallback(beginGlobalSettingsEditContext);
    startActivityForResult(std::make_unique<ControlsOptionsActivity>(renderer, mappedInput),
                           [this](const ActivityResult&) {
                             if (endGlobalSettingsEditCallback)
                               endGlobalSettingsEditCallback(endGlobalSettingsEditContext);
                             ActivityResult result;
                             result.isCancelled = true;
                             result.data = makeMenuResult(-1);
                             setResult(std::move(result));
                             finish();
                           });
    return true;
  }

  if (selectedAction == MenuAction::GO_TO_STABLE_PAGE) {
    auto selector = makeUniqueNoThrow<EpubReaderPercentSelectionActivity>(renderer, mappedInput, stableCurrentPage,
                                                                          stablePageCount);
    if (!selector) {
      LOG_ERR("ERM", "Could not allocate stable page selector");
      requestUpdate();
      return true;
    }
    startActivityForResult(std::move(selector), [this](const ActivityResult& result) {
      if (result.isCancelled) {
        requestUpdate();
        return;
      }
      MenuResult menu = makeMenuResult(static_cast<int>(MenuAction::GO_TO_STABLE_PAGE));
      menu.drawerPage = std::get<PageResult>(result.data).page;
      setResult(std::move(menu));
      finish();
    });
    return true;
  }

  if (selectedAction == MenuAction::VIEW_CLIPPINGS) {
    startActivityForResult(std::make_unique<EpubReaderClippingListActivity>(renderer, mappedInput),
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
    return true;
  }

  setResult(makeMenuResult(static_cast<int>(selectedAction)));
  finish();
  return true;
}

bool EpubReaderMenuActivity::handleTouchInput() {
  int tabIndex = -1;
  if (mappedInput.wasTabTapped(tabIndex) && tabIndex >= 0) {
    if (mappedInput.hasTouchHardware() && tabIndex == static_cast<int>(TOUCH_LOCK_ICON_INDEX)) {
      SETTINGS.disableReaderTouchscreen = SETTINGS.disableReaderTouchscreen ? 0 : 1;
      if (saveGlobalSettingsCallback) {
        saveGlobalSettingsCallback(saveGlobalSettingsContext);
      } else {
        SETTINGS.saveToFile();
      }
      requestUpdate();
      return true;
    }
    if (mappedInput.hasTouchHardware() && tabIndex == static_cast<int>(TOUCH_HOME_ICON_INDEX)) {
      setResult(makeMenuResult(static_cast<int>(MenuAction::GO_HOME)));
      finish();
      return true;
    }
    if (tabIndex < static_cast<int>(MENU_TAB_COUNT)) {
      activeTab = static_cast<MenuTab>(tabIndex);
      focusTabRow();
      requestUpdate();
      return true;
    }
  }
  return false;
}

void EpubReaderMenuActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EpubReaderMenuActivity*>(user);
  const auto& items = self->activeMenuItems();
  if (self->optionPopup.isActive() || event.value < 0 || event.value >= static_cast<int16_t>(items.size())) return;
  self->selectedIndex = event.value;
  self->app.clearTapFlash();
  self->activateSelectedItem();
}

void EpubReaderMenuActivity::drawIconTabBar(const Rect rect, const bool drawBottomBorder) {
  renderer.drawLine(rect.x, rect.y, rect.x + rect.width - 1, rect.y, true);
  if (drawBottomBorder) {
    renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width - 1, rect.y + rect.height - 1, true);
  }

#if CROSSINK_APP_CAP_TOUCH
  const size_t iconCount = mappedInput.hasTouchHardware() ? TOUCH_ICON_COUNT : MENU_TAB_COUNT;
#else
  constexpr size_t iconCount = MENU_TAB_COUNT;
#endif
  for (size_t i = 0; i < iconCount; i++) {
    const int slotX = rect.x + static_cast<int>((i * rect.width) / iconCount);
    const int nextSlotX = rect.x + static_cast<int>(((i + 1) * rect.width) / iconCount);
    const int slotWidth = nextSlotX - slotX;
    const int centerX = slotX + slotWidth / 2;
    const bool selected = i < MENU_TAB_COUNT && i == activeTabIndex();
    const bool tabFocused = selected && selectedIndex < 0;
    TouchRegistry::getInstance().add(Rect{slotX, rect.y, slotWidth, rect.height}, static_cast<int>(i),
                                     TouchRegistry::Tab);
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
      drawSdkIcon(uiTarget, icon_menu_24, iconX, iconY, !tabFocused);
    } else if (i == static_cast<size_t>(MenuTab::Bookmarks)) {
      drawBookmarkTabIcon(renderer, iconX, iconY, !tabFocused);
    } else if (i == static_cast<size_t>(MenuTab::Settings)) {
      drawSdkIcon(uiTarget, icon_cog_24, iconX, iconY, !tabFocused);
    }
#if CROSSINK_APP_CAP_TOUCH
    else {
      drawSdkIcon(uiTarget, SETTINGS.disableReaderTouchscreen ? icon_device_tablet_off_24 : icon_device_tablet_24,
                  iconX, iconY);
    }
#endif
  }
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  mappedInput.setReaderTouchscreenOverride(true);
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &EpubReaderMenuActivity::onRowEvent, this);
  app.setScreen(&EpubReaderMenuActivity::menuScreen, this);
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() {
  mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();
}

void EpubReaderMenuActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (TouchHeaderBackButton::wasTapped(mappedInput, readerMenuHeaderRect(renderer, mappedInput))) {
    finishCancelled();
    return;
  }
  if (handleTouchInput()) return;
  if (mappedInput.wasHomeGesture()) {
    finishCancelled();
    return;
  }

  // On X4 Pro, the top-edge down-swipe is the opposite of the reader menu's
  // upward opening gesture. It dismisses before generic swipe scrolling.
  if (ReaderUtils::isTouchMenuDismissGesture(mappedInput)) {
    finishCancelled();
    return;
  }

  // A home-key long press toggles the reader menu: the same hold that opens it
  // closes it. The SDK fires the long event once per hold, so the opening hold
  // (still down as the menu appears) does not immediately re-close it, only a
  // fresh press-and-hold does.
  if (mappedInput.wasReaderMenuHold()) {
    finishCancelled();
    return;
  }

  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent dispatch.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      // No pressed-state repaint: the render it triggers would drop a slow
      // tap's release inside the uiReady window (tap-to-activate needed two
      // taps), and it costs a second e-ink refresh per tap.
      if (app.invalidated()) requestUpdate();
      if (event) return;  // dispatched to onRowEvent
    }
  }

  // Swipes scroll the viewport; the selection stays put (it may scroll
  // off-screen) and button navigation pulls the view back to it.
  const int menuCount = static_cast<int>(activeMenuItems().size());
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
    const int next = scrollListBy(topIndex, delta, visibleRows, menuCount);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  const auto moveSelection = [this, menuCount](const int index) {
    selectedIndex = index - 1;
    if (selectedIndex >= 0) topIndex = followListSelection(selectedIndex, topIndex, visibleRows, menuCount);
    requestUpdate();
  };
  buttonNavigator.onNextRelease([this, menuCount, &moveSelection] {
    moveSelection(ButtonNavigator::nextIndex(selectedIndex + 1, menuCount + 1));
  });
  buttonNavigator.onPreviousRelease([this, menuCount, &moveSelection] {
    moveSelection(ButtonNavigator::previousIndex(selectedIndex + 1, menuCount + 1));
  });
  buttonNavigator.onNextContinuous([this] { moveActiveTab(true); });
  buttonNavigator.onPreviousContinuous([this] { moveActiveTab(false); });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelectedItem();
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

void EpubReaderMenuActivity::menuScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<EpubReaderMenuActivity*>(user)->buildMenuScreen(screen);
}

void EpubReaderMenuActivity::buildMenuScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouch(), false);
  const int tabBarHeight = readerMenuTabBarHeight(metrics.tabBarHeight, mappedInput.hasTouch());
#if CROSSINK_APP_CAP_TOUCH
  const bool tabsAtBottom = readerMenuTabsAtBottom(mappedInput);
  // Sticky has touch but no frontlight, so this is compile-time false there;
  // X4 Pro still evaluates the runtime placement check.
  const int contentTop = safe.y + metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) +
                         metrics.tabBarHeight +
                         // cppcheck-suppress knownConditionTrueFalse
                         (tabsAtBottom ? 0 : tabBarHeight) + metrics.verticalSpacing;
  const int contentBottom = renderer.getScreenHeight() - (safe.y + safe.height) +
                            // cppcheck-suppress knownConditionTrueFalse
                            (tabsAtBottom ? tabBarHeight + metrics.verticalSpacing : 0);
#else
  const int contentTop = safe.y + metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) +
                         metrics.tabBarHeight + tabBarHeight + metrics.verticalSpacing;
  const int contentBottom = renderer.getScreenHeight() - (safe.y + safe.height);
#endif
  // The legacy header, progress band, and icon tabs remain outside the app;
  // FreeInkUI owns the scalable list between them.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(contentTop),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(contentBottom), static_cast<int16_t>(safe.x)});

  const auto& activeItems = activeMenuItems();
  std::vector<std::string> values(activeItems.size());
  std::vector<fui::ListItem> items;
  items.reserve(activeItems.size());
  for (size_t i = 0; i < activeItems.size(); i++) {
    const auto& menuItem = activeItems[i];
    fui::ListItem item;
    item.label = I18N.get(menuItem.labelId);
    if (menuItem.action == MenuAction::ROTATE_SCREEN) {
      item.value = I18N.get(orientationLabels[pendingOrientation]);
    } else if (menuItem.action == MenuAction::AUTO_PAGE_TURN) {
      if (autoPageTurnActive) values[i] = std::to_string(autoPageTurnIntervalSeconds);
      item.value = values[i].empty() ? nullptr : values[i].c_str();
    }
    item.actionValue = static_cast<int16_t>(items.size());
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(activeItems.size()));  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  const bool hasTouch = mappedInput.hasTouch();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, !hasTouch, false);
  const int tabBarHeight = readerMenuTabBarHeight(metrics.tabBarHeight, hasTouch);
#if CROSSINK_APP_CAP_TOUCH
  const bool tabsAtBottom = readerMenuTabsAtBottom(mappedInput);
#endif

  // The menu is a system screen, not reading content: its status indicators
  // stay visible unless their setting is Hide Always.
  const Rect headerRect = readerMenuHeaderRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    const Rect homeRect = readerMenuHeaderActionRect(headerRect, metrics);
    const Rect homeTouchRect = readerMenuHeaderActionTouchRect(headerRect, homeRect);
    const int titleRightReserve = headerRect.x + headerRect.width - homeRect.x + headerActionGap;
    TouchHeaderBackButton::draw(renderer, uiTarget, headerRect, title.c_str(), true, titleRightReserve);
    TouchRegistry::getInstance().add(homeTouchRect, static_cast<int>(TOUCH_HOME_ICON_INDEX), TouchRegistry::Tab);
    drawSdkIcon(uiTarget, icon_home_24, homeRect.x + (homeRect.width - tabIconSize) / 2,
                homeRect.y + (homeRect.height - tabIconSize) / 2);
  } else {
    GUI.drawHeader(renderer, headerRect, title.c_str());
  }

  // Progress summary
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  GUI.drawSubHeader(renderer,
                    Rect{screen.x, screen.y + metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput),
                         screen.width, metrics.tabBarHeight},
                    progressLine.c_str());

  const int topTabBarY =
      screen.y + metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) + metrics.tabBarHeight;
#if CROSSINK_APP_CAP_TOUCH
  // cppcheck-suppress knownConditionTrueFalse
  const int tabBarY = tabsAtBottom ? screen.y + screen.height - tabBarHeight : topTabBarY;
#else
  const int tabBarY = topTabBarY;
#endif
  const Rect tabRect{screen.x, tabBarY, screen.width, tabBarHeight};
#if CROSSINK_APP_CAP_TOUCH
  drawIconTabBar(tabRect, !tabsAtBottom);
#else
  drawIconTabBar(tabRect, true);
#endif

  uiReady = false;
  app.render();
  uiReady = true;

  const auto confirmLabel = selectedIndex < 0 ? tr(STR_NEXT_FIELD) : tr(STR_SELECT);
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
