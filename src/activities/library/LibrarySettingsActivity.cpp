#include "LibrarySettingsActivity.h"

#include <I18n.h>

#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr int ROW_COUNT = 10;
}  // namespace

LibrarySettingsActivity::LibrarySettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Library Settings", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void LibrarySettingsActivity::onEnter() {
  Activity::onEnter();
  showSelection = !mappedInput.hasTouchHardware();
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &LibrarySettingsActivity::onRow, this);
  app.setScreen(&LibrarySettingsActivity::screen, this);
  ignoreConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  requestUpdate();
}

void LibrarySettingsActivity::toggle(const int row) {
  switch (row) {
    case 0:
      SETTINGS.libraryUseMetadata = !SETTINGS.libraryUseMetadata;
      break;
    case 1:
      SETTINGS.libraryListExpanded = !SETTINGS.libraryListExpanded;
      break;
    case 2:
      SETTINGS.recentBooksView = SETTINGS.recentBooksView == CrossPointSettings::RECENT_BOOKS_GRID
                                     ? CrossPointSettings::RECENT_BOOKS_LIST
                                     : CrossPointSettings::RECENT_BOOKS_GRID;
      break;
    case 3:
      SETTINGS.libraryHideFinishedBooks = !SETTINGS.libraryHideFinishedBooks;
      break;
    case 4:
      SETTINGS.libraryShowSeries = !SETTINGS.libraryShowSeries;
      break;
    case 5:
      SETTINGS.libraryShowGenre = !SETTINGS.libraryShowGenre;
      break;
    case 6:
      SETTINGS.libraryShowEpub = !SETTINGS.libraryShowEpub;
      break;
    case 7:
      SETTINGS.libraryShowXtc = !SETTINGS.libraryShowXtc;
      break;
    case 8:
      SETTINGS.libraryShowTxt = !SETTINGS.libraryShowTxt;
      break;
    case 9:
      SETTINGS.libraryShowMarkdown = !SETTINGS.libraryShowMarkdown;
      break;
    default:
      return;
  }
  if (!SETTINGS.saveToFile()) LOG_ERR("LIB", "Cannot save Library settings");
  requestUpdate();
}

void LibrarySettingsActivity::onRow(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<LibrarySettingsActivity*>(user);
  if (event.value < 0 || event.value >= ROW_COUNT) return;
  self->selection = event.value;
  self->showSelection = false;
  self->topIndex = self->listNav.top;
  self->app.clearTapFlash();
  self->toggle(event.value);
}

void LibrarySettingsActivity::loop() {
  RenderLock lock;
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }
  if (ignoreConfirmRelease) {
    // Consume the menu selection's release even when this screen ignores it.
    // Otherwise its one-shot suppression can swallow the next quick Select.
    (void)mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) ignoreConfirmRelease = false;
    return;
  }
  if (uiReady) {
    const auto snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    toggle(selection);
    return;
  }
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    showSelection = false;
    listNav.top = topIndex;
    listNav.scrollBy(
        swipe == MappedInputManager::SwipeDir::Up ? listNav.pageRowsFor(ROW_COUNT) : -listNav.pageRowsFor(ROW_COUNT),
        ROW_COUNT);
    topIndex = listNav.top;
    requestUpdate();
    return;
  }
  const auto move = [this](int next) {
    if (!showSelection) {
      showSelection = true;
      listNav.selected = selection;
      listNav.top = topIndex;
      listNav.follow(ROW_COUNT);
      topIndex = listNav.top;
      requestUpdate();
      return;
    }
    selection = next;
    listNav.selected = selection;
    listNav.top = topIndex;
    listNav.follow(ROW_COUNT);
    topIndex = listNav.top;
    requestUpdate();
  };
  buttonNavigator.onNextRelease([&] { move(ButtonNavigator::nextIndex(selection, ROW_COUNT)); });
  buttonNavigator.onPreviousRelease([&] { move(ButtonNavigator::previousIndex(selection, ROW_COUNT)); });
}

void LibrarySettingsActivity::screen(UiApp::ScreenType& screen, void* user) {
  static_cast<LibrarySettingsActivity*>(user)->buildScreen(screen);
}

void LibrarySettingsActivity::provideRow(void*, const uint16_t row, fui::ListItem& item) {
  static constexpr StrId labels[] = {
      StrId::STR_LIBRARY_USE_METADATA,   StrId::STR_LIBRARY_LIST_VIEW, StrId::STR_LIBRARY_RECENTLY_OPENED_VIEW,
      StrId::STR_LIBRARY_FINISHED_BOOKS, StrId::STR_LIBRARY_SERIES,    StrId::STR_LIBRARY_GENRE,
      StrId::STR_LIBRARY_EPUBS,          StrId::STR_LIBRARY_XTC_XTCH,  StrId::STR_LIBRARY_TXT,
      StrId::STR_LIBRARY_MARKDOWN};
  item.label = I18N.get(labels[row]);
  item.actionValue = static_cast<int16_t>(row);
  if (row == 1) {
    item.value = SETTINGS.libraryListExpanded ? tr(STR_LIBRARY_EXPANDED) : tr(STR_COMPACT);
    return;
  }
  if (row == 2) {
    item.value =
        SETTINGS.recentBooksView == CrossPointSettings::RECENT_BOOKS_GRID ? tr(STR_GRID_VIEW) : tr(STR_LIST_VIEW);
    return;
  }
  item.toggle = true;
  switch (row) {
    case 0:
      item.toggleChecked = SETTINGS.libraryUseMetadata;
      break;
    case 3:
      item.sectionHeading = tr(STR_CAT_DISPLAY);
      item.toggleChecked = !SETTINGS.libraryHideFinishedBooks;
      break;
    case 4:
      item.toggleChecked = SETTINGS.libraryShowSeries;
      break;
    case 5:
      item.toggleChecked = SETTINGS.libraryShowGenre;
      break;
    case 6:
      item.sectionHeading = tr(STR_LIBRARY_SHOW_FILES);
      item.toggleChecked = SETTINGS.libraryShowEpub;
      break;
    case 7:
      item.toggleChecked = SETTINGS.libraryShowXtc;
      break;
    case 8:
      item.toggleChecked = SETTINGS.libraryShowTxt;
      break;
    case 9:
      item.toggleChecked = SETTINGS.libraryShowMarkdown;
      break;
  }
}

void LibrarySettingsActivity::buildScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  int bounds[4]{};
  renderer.getOrientedViewableTRBL(&bounds[0], &bounds[1], &bounds[2], &bounds[3]);
  const int16_t headerBottom = static_cast<int16_t>(TouchHeaderBackButton::contentTop(renderer, mappedInput));
  screen.setContentMarginFromScreen(fui::Insets{
      headerBottom, static_cast<int16_t>(bounds[1]),
      static_cast<int16_t>(UITheme::getButtonHintsReserve(renderer) + bounds[2]), static_cast<int16_t>(bounds[3])});
  fui::ListProps props;
  props.rowProvider = &LibrarySettingsActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = ROW_COUNT;
  props.selectedIndex = showSelection ? selection : -1;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.rowHeight = 44;
  props.rowGap = 0;
  props.labelText = props.valueText = props.headerText = screen.theme().bodyText;
  props.headerText.bold = true;
  props.rtl = I18N.isRightToLeft();
  props.rowStyles = screen.theme().listRow;
  props.rowStyles.selected.background = fui::Paint::dither(fui::Color::LightGray);
  props.rowStyles.selected.foreground = fui::Paint::solid(fui::Color::Black);
  props.rowStyles.active = props.rowStyles.selected;
  listNav.selected = showSelection ? selection : -1;
  listNav.top = topIndex;
  listNav.syncToProps(screen.body(), props.rowHeight, props.rowGap, ROW_COUNT, props);
  topIndex = listNav.top;
  screen.list(props);
}

void LibrarySettingsActivity::render(RenderLock&&) {
  uiReady = false;
  for (int pass = 0; pass < 8; ++pass) {
    renderer.clearScreen();
    const auto header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    if (mappedInput.hasTouchHardware())
      TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_LIBRARY_SETTINGS), false);
    else
      GUI.drawHeader(renderer, header, tr(STR_LIBRARY_SETTINGS));
    renderUiApp(app, uiTarget);
    topIndex = listNav.top;
    if (!listNav.consumeRebuildNeeded()) break;
  }
  uiReady = true;
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
