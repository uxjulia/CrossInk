#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/listIcons.h"
#include "components/icons/readingStatsIcons.h"
#include "components/icons/syncIcons.h"
#include "fontIds.h"
#include "network/ReadingSyncUpload.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;

enum class MenuSection : uint8_t { None, Nearby, SyncServer };

struct MenuEntry {
  NetworkMode mode;
  StrId label;
  StrId description;
  UIIcon icon;
  MenuSection section;
};

// Display order. A section heading precedes the first visible row of its section.
constexpr MenuEntry MENU_ENTRIES[] = {
    {NetworkMode::OPDS_BROWSER, StrId::STR_OPDS_BROWSER, StrId::STR_OPDS_SERVERS, UIIcon::Opds, MenuSection::None},
    {NetworkMode::JOIN_NETWORK, StrId::STR_JOIN_NETWORK, StrId::STR_JOIN_DESC, UIIcon::Wifi, MenuSection::None},
    {NetworkMode::CONNECT_CALIBRE, StrId::STR_CALIBRE_WIRELESS, StrId::STR_CALIBRE_DESC, UIIcon::Library,
     MenuSection::None},
    {NetworkMode::CREATE_HOTSPOT, StrId::STR_CREATE_HOTSPOT, StrId::STR_HOTSPOT_DESC, UIIcon::Hotspot,
     MenuSection::None},
#if CROSSINK_APP_CAP_USB_DRIVE
    {NetworkMode::USB_DRIVE, StrId::STR_USB_DRIVE, StrId::STR_USB_DRIVE_DESC, UIIcon::Transfer, MenuSection::None},
#endif
    {NetworkMode::SYNC_BOOKS, StrId::STR_SYNC_BOOKS, StrId::STR_SYNC_BOOKS_DESC, UIIcon::Transfer,
     MenuSection::SyncServer},
    {NetworkMode::SYNC_READING_STATS, StrId::STR_SYNC_READING_STATS, StrId::STR_SYNC_READING_STATS_DESC, UIIcon::Chart,
     MenuSection::SyncServer},
    {NetworkMode::SYNC_CLIPPINGS, StrId::STR_SYNC_CLIPPINGS, StrId::STR_SYNC_CLIPPINGS_DESC, UIIcon::BookmarkIcon,
     MenuSection::SyncServer},
    {NetworkMode::NEARBY_BOOK_RECEIVE, StrId::STR_RECEIVE_NEARBY_BOOK, StrId::STR_RECEIVE_NEARBY_BOOK_DESC,
     UIIcon::Transfer, MenuSection::Nearby},
    {NetworkMode::NEARBY_STATS_SYNC, StrId::STR_NEARBY_STATS_SYNC, StrId::STR_NEARBY_STATS_SYNC_DESC, UIIcon::Transfer,
     MenuSection::Nearby},
};
constexpr int MENU_ENTRY_COUNT = sizeof(MENU_ENTRIES) / sizeof(MENU_ENTRIES[0]);
static_assert(MENU_ENTRY_COUNT <= NetworkModeSelectionActivity::MAX_MENU_ROWS, "Raise MAX_MENU_ROWS");

bool entryVisible(const NetworkMode mode) {
  switch (mode) {
    case NetworkMode::NEARBY_STATS_SYNC:
      return SETTINGS.shouldTrackReadingStats();
    case NetworkMode::SYNC_READING_STATS:
      return ReadingSyncUpload::canSyncReadingStats();
    case NetworkMode::SYNC_CLIPPINGS:
      return ReadingSyncUpload::canSyncClippings();
    default:
      return true;
  }
}

StrId sectionHeading(const MenuSection section) {
  return section == MenuSection::Nearby ? StrId::STR_NEARBY_DEVICE : StrId::STR_SYNC_SERVER;
}
}  // namespace

NetworkModeSelectionActivity::NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("NetworkModeSelection", renderer, mappedInput), ui(renderer) {}

void NetworkModeSelectionActivity::buildMenu() {
  menuCount = 0;
  listCount = 0;
  MenuSection previous = MenuSection::None;
  for (int i = 0; i < MENU_ENTRY_COUNT; ++i) {
    const auto& entry = MENU_ENTRIES[i];
    if (!entryVisible(entry.mode)) continue;
    if (entry.section != MenuSection::None && entry.section != previous) ++listCount;  // heading row
    previous = entry.section;
    menuEntries[menuCount] = static_cast<uint8_t>(i);
    menuListIndex[menuCount] = static_cast<uint8_t>(listCount);
    ++menuCount;
    ++listCount;
  }
}

int NetworkModeSelectionActivity::listIndexForMenuIndex(const int menuIndex) const {
  return menuIndex >= 0 && menuIndex < menuCount ? menuListIndex[menuIndex] : -1;
}

void NetworkModeSelectionActivity::selectCurrent() {
  if (selectedIndex >= 0 && selectedIndex < menuCount) onModeSelected(MENU_ENTRIES[menuEntries[selectedIndex]].mode);
}

void NetworkModeSelectionActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<NetworkModeSelectionActivity*>(user);
  if (event.value < 0 || event.value >= self->menuCount) return;
  self->selectedIndex = event.value;
  // Selection leaves this screen; a lingering flash would gray an unrelated
  // element on the next render.
  self->ui.app.clearTapFlash();
  self->selectCurrent();
}

void NetworkModeSelectionActivity::onEnter() {
  Activity::onEnter();

  buildMenu();
  selectedIndex = 0;
  buttonSelectionActive = !mappedInput.hasTouchHardware();
  ui.closeRouting();
  visibleRows = 1;
  topIndex = 0;
  listNav.reset(buttonSelectionActive ? listIndexForMenuIndex(selectedIndex) : -1);
  ui.reset();
  ui.app.on(ACTION_ROW, &NetworkModeSelectionActivity::onRowEvent, this);
  ui.app.setScreen(&NetworkModeSelectionActivity::listScreen, this);
  requestUpdate();
}

void NetworkModeSelectionActivity::onExit() { Activity::onExit(); }

void NetworkModeSelectionActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    onCancel();
    return;
  }
  // Handle back button - cancel
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
    return;
  }

  // Handle confirm button - select current option
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    mappedInput.suppressNextConfirmRelease();
    selectCurrent();
    return;
  }

  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent dispatch.
  if (ui.routingReady()) {
    fui::ActionEvent event{};
    if (ui.routeTouch(mappedInput, event)) {
      if (ui.app.invalidated()) requestUpdate();
      if (event) return;  // dispatched to onRowEvent
    }
  }

  // Swipes scroll the viewport; the selection stays put and button
  // navigation pulls the view back to it.
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    bool moved = false;
    {
      RenderLock lock(*this);
      listNav.selected = buttonSelectionActive ? listIndexForMenuIndex(selectedIndex) : -1;
      listNav.top = topIndex;
      listNav.visibleRows = visibleRows;
      // Page by the rows the last layout measured, not the fixed-height
      // estimate: wrapped subtitles fit fewer rows, and paging by the larger
      // estimate would skip the ones in between.
      const int page = listNav.pageRowsFor(listCount);
      moved = listNav.scrollBy(swipe == MappedInputManager::SwipeDir::Up ? page : -page, listCount);
      topIndex = listNav.top;
    }
    if (moved) {
      requestUpdate();
    }
    return;
  }

  // Handle navigation
  buttonNavigator.onNext([this] {
    {
      RenderLock lock(*this);
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, menuCount);
      buttonSelectionActive = true;
      listNav.selected = listIndexForMenuIndex(selectedIndex);
      listNav.top = topIndex;
      listNav.visibleRows = visibleRows;
      listNav.follow(listCount);
      topIndex = listNav.top;
    }
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    {
      RenderLock lock(*this);
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, menuCount);
      buttonSelectionActive = true;
      listNav.selected = listIndexForMenuIndex(selectedIndex);
      listNav.top = topIndex;
      listNav.visibleRows = visibleRows;
      listNav.follow(listCount);
      topIndex = listNav.top;
    }
    requestUpdate();
  });
}

void NetworkModeSelectionActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<NetworkModeSelectionActivity*>(user)->buildListScreen(screen);
}

void NetworkModeSelectionActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  setUiContentMargin(screen, renderer,
                     fui::Insets{static_cast<int16_t>(TouchHeaderBackButton::contentTop(renderer, mappedInput)), 0,
                                 static_cast<int16_t>(UITheme::getButtonHintsReserve(renderer)), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  std::vector<fui::ListItem> items;
  items.reserve(listCount);
  MenuSection previous = MenuSection::None;
  for (int i = 0; i < menuCount; i++) {
    const auto& entry = MENU_ENTRIES[menuEntries[i]];
    if (entry.section != MenuSection::None && entry.section != previous) {
      fui::ListItem header;
      header.label = I18N.get(sectionHeading(entry.section));
      header.isHeader = true;
      items.push_back(header);
    }
    previous = entry.section;

    fui::ListItem item;
    item.label = I18N.get(entry.label);
    item.subtitle = I18N.get(entry.description);
    if (entry.mode == NetworkMode::USB_DRIVE) {
      item.icon = fui::bitmapFromIcon(icon_usb_32);
    } else if (entry.mode == NetworkMode::NEARBY_BOOK_RECEIVE) {
      item.icon = fui::bitmapFromIcon(icon_chevrons_left_right_ellipsis_32);
    } else if (entry.mode == NetworkMode::NEARBY_STATS_SYNC || entry.mode == NetworkMode::SYNC_READING_STATS) {
      item.icon = fui::bitmapFromIcon(icon_reading_stats_32);
    } else if (entry.mode == NetworkMode::SYNC_BOOKS) {
      item.icon = fui::bitmapFromIcon(icon_cloud_upload_32);
    } else if (entry.mode == NetworkMode::SYNC_CLIPPINGS) {
      item.icon = fui::bitmapFromIcon(icon_blockquote_32);
    } else {
      item.icon = listIconFor(entry.icon, 32);  // subtitle rows carry the larger icon
    }
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.labelText = screen.theme().bodyText;
  props.labelText.bold = true;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.bold = false;
  props.subtitleText.maxLines = 2;
  props.headerText = screen.theme().bodyText;
  props.headerText.bold = true;
  props.rowGap = 10;
  // The normal 10px row gap plus this extra 10px separates each section
  // heading from the group above it by about 20px.
  props.sectionGap = 10;
  const auto rows = configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  listNav.selected = buttonSelectionActive ? listIndexForMenuIndex(selectedIndex) : -1;
  listNav.top = topIndex;
  listNav.visibleRows = visibleRows;
  listNav.syncToProps(screen.body(), props.rowHeight, props.rowGap, listCount, props);
  topIndex = listNav.top;
  screen.list(props);
  topIndex = listNav.top;
}

void NetworkModeSelectionActivity::render(RenderLock&&) {
  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  ui.closeRouting();
  for (int pass = 0; pass < 8; ++pass) {
    // Wrapped rows can move the viewport after layout. Erase the previous pass
    // before drawing the corrected list, including its section headings.
    renderer.clearScreen();
    if (mappedInput.hasTouchHardware()) {
      TouchHeaderBackButton::draw(renderer, ui.target, header, tr(STR_FILE_TRANSFER), false);
    } else {
      GUI.drawHeader(renderer, header, tr(STR_FILE_TRANSFER));
    }
    ui.render();
    if (!listNav.consumeRebuildNeeded()) break;
  }

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(screenTransitionRefresh.modeFor(0));
}

void NetworkModeSelectionActivity::onModeSelected(NetworkMode mode) {
  setResult(NetworkModeResult{mode});
  finish();
}

void NetworkModeSelectionActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
