#include "NetworkModeSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/icons/listIcons.h"
#include "components/icons/readingStatsIcons.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;

#if CROSSINK_APP_CAP_USB_DRIVE
constexpr NetworkMode menuModes[] = {NetworkMode::JOIN_NETWORK,        NetworkMode::CONNECT_CALIBRE,
                                     NetworkMode::CREATE_HOTSPOT,      NetworkMode::USB_DRIVE,
                                     NetworkMode::NEARBY_BOOK_RECEIVE, NetworkMode::NEARBY_STATS_SYNC};
constexpr StrId menuItems[] = {StrId::STR_JOIN_NETWORK, StrId::STR_CALIBRE_WIRELESS,    StrId::STR_CREATE_HOTSPOT,
                               StrId::STR_USB_DRIVE,    StrId::STR_RECEIVE_NEARBY_BOOK, StrId::STR_NEARBY_STATS_SYNC};
constexpr StrId menuDescs[] = {StrId::STR_JOIN_DESC,
                               StrId::STR_CALIBRE_DESC,
                               StrId::STR_HOTSPOT_DESC,
                               StrId::STR_USB_DRIVE_DESC,
                               StrId::STR_RECEIVE_NEARBY_BOOK_DESC,
                               StrId::STR_NEARBY_STATS_SYNC_DESC};
constexpr UIIcon menuIcons[] = {UIIcon::Wifi,     UIIcon::Library,  UIIcon::Hotspot,
                                UIIcon::Transfer, UIIcon::Transfer, UIIcon::Transfer};
#else
constexpr NetworkMode menuModes[] = {NetworkMode::JOIN_NETWORK, NetworkMode::CONNECT_CALIBRE,
                                     NetworkMode::CREATE_HOTSPOT, NetworkMode::NEARBY_BOOK_RECEIVE,
                                     NetworkMode::NEARBY_STATS_SYNC};
constexpr StrId menuItems[] = {StrId::STR_JOIN_NETWORK, StrId::STR_CALIBRE_WIRELESS, StrId::STR_CREATE_HOTSPOT,
                               StrId::STR_RECEIVE_NEARBY_BOOK, StrId::STR_NEARBY_STATS_SYNC};
constexpr StrId menuDescs[] = {StrId::STR_JOIN_DESC, StrId::STR_CALIBRE_DESC, StrId::STR_HOTSPOT_DESC,
                               StrId::STR_RECEIVE_NEARBY_BOOK_DESC, StrId::STR_NEARBY_STATS_SYNC_DESC};
constexpr UIIcon menuIcons[] = {UIIcon::Wifi, UIIcon::Library, UIIcon::Hotspot, UIIcon::Transfer, UIIcon::Transfer};
#endif

constexpr int MENU_ITEM_COUNT = sizeof(menuModes) / sizeof(menuModes[0]);
constexpr int LIST_ITEM_COUNT = MENU_ITEM_COUNT + 1;
constexpr int NEARBY_SECTION_INDEX = CROSSINK_APP_CAP_USB_DRIVE ? 4 : 3;

int listIndexForMenuIndex(const int menuIndex) { return menuIndex < NEARBY_SECTION_INDEX ? menuIndex : menuIndex + 1; }
}  // namespace

NetworkModeSelectionActivity::NetworkModeSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("NetworkModeSelection", renderer, mappedInput), ui(renderer) {}

void NetworkModeSelectionActivity::selectCurrent() {
  if (selectedIndex >= 0 && selectedIndex < MENU_ITEM_COUNT) onModeSelected(menuModes[selectedIndex]);
}

void NetworkModeSelectionActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<NetworkModeSelectionActivity*>(user);
  if (event.value < 0 || event.value >= MENU_ITEM_COUNT) return;
  self->selectedIndex = event.value;
  // Selection leaves this screen; a lingering flash would gray an unrelated
  // element on the next render.
  self->ui.app.clearTapFlash();
  self->selectCurrent();
}

void NetworkModeSelectionActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  ui.closeRouting();
  visibleRows = 1;
  topIndex = 0;
  listNav.reset(listIndexForMenuIndex(selectedIndex));
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
      listNav.selected = listIndexForMenuIndex(selectedIndex);
      listNav.top = topIndex;
      listNav.visibleRows = visibleRows;
      // Page by the rows the last layout measured, not the fixed-height
      // estimate: wrapped subtitles fit fewer rows, and paging by the larger
      // estimate would skip the ones in between.
      const int page = listNav.pageRowsFor(LIST_ITEM_COUNT);
      moved = listNav.scrollBy(swipe == MappedInputManager::SwipeDir::Up ? page : -page, LIST_ITEM_COUNT);
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
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, MENU_ITEM_COUNT);
      listNav.selected = listIndexForMenuIndex(selectedIndex);
      listNav.top = topIndex;
      listNav.visibleRows = visibleRows;
      listNav.follow(LIST_ITEM_COUNT);
      topIndex = listNav.top;
    }
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    {
      RenderLock lock(*this);
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, MENU_ITEM_COUNT);
      listNav.selected = listIndexForMenuIndex(selectedIndex);
      listNav.top = topIndex;
      listNav.visibleRows = visibleRows;
      listNav.follow(LIST_ITEM_COUNT);
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
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  std::vector<fui::ListItem> items;
  items.reserve(LIST_ITEM_COUNT);
  for (int i = 0; i < MENU_ITEM_COUNT; i++) {
    if (i == NEARBY_SECTION_INDEX) {
      fui::ListItem header;
      header.label = I18N.get(StrId::STR_NEARBY_DEVICE);
      header.isHeader = true;
      items.push_back(header);
    }

    fui::ListItem item;
    item.label = I18N.get(menuItems[i]);
    item.subtitle = I18N.get(menuDescs[i]);
    if (menuModes[i] == NetworkMode::USB_DRIVE) {
      item.icon = fui::bitmapFromIcon(icon_usb_32);
    } else if (menuModes[i] == NetworkMode::NEARBY_BOOK_RECEIVE) {
      item.icon = fui::bitmapFromIcon(icon_chevrons_left_right_ellipsis_32);
    } else if (menuModes[i] == NetworkMode::NEARBY_STATS_SYNC) {
      item.icon = fui::bitmapFromIcon(icon_reading_stats_32);
    } else {
      item.icon = listIconFor(menuIcons[i], 32);  // subtitle rows carry the larger icon
    }
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(listIndexForMenuIndex(selectedIndex));
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
  // The normal 10px row gap plus this extra 10px separates Nearby Device
  // from the hotspot group by about 20px.
  props.sectionGap = 10;
  const auto rows = configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  listNav.selected = listIndexForMenuIndex(selectedIndex);
  listNav.top = topIndex;
  listNav.visibleRows = visibleRows;
  listNav.syncToProps(screen.body(), props.rowHeight, props.rowGap, LIST_ITEM_COUNT, props);
  topIndex = listNav.top;
  screen.list(props);
  topIndex = listNav.top;
}

void NetworkModeSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, ui.target, header, tr(STR_FILE_TRANSFER), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_FILE_TRANSFER));
  }

  ui.closeRouting();
  for (int pass = 0; pass < 8; ++pass) {
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
