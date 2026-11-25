#include "OpdsServerListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "OpdsSettingsActivity.h"
#include "activities/ActivityManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/OpdsDownloadFolder.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;

}  // namespace

int OpdsServerListActivity::getItemCount() const {
  int count = static_cast<int>(OPDS_STORE.getCount());
  // Both modes append "Add Server"; Settings also includes Download Folder.
  count += pickerMode ? 1 : 2;
  return count;
}

OpdsServerListActivity::OpdsServerListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const bool pickerMode)
    : Activity("OpdsServerList", renderer, mappedInput),
      pickerMode(pickerMode),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void OpdsServerListActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<OpdsServerListActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->getItemCount())) return;
  self->selectedIndex = event.value;
  // Activation opens an editor/browser or repaints a new value; a lingering
  // flash would gray an unrelated row.
  self->app.clearTapFlash();
  self->handleSelection();
  self->requestUpdate();
}

void OpdsServerListActivity::onEnter() {
  Activity::onEnter();

  // Reload from disk in case servers were added/removed by a subactivity or the web UI
  OPDS_STORE.loadFromFile();
  selectedIndex = 0;
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &OpdsServerListActivity::onRowEvent, this);
  app.setScreen(&OpdsServerListActivity::listScreen, this);
  requestUpdate();
}

void OpdsServerListActivity::onExit() { Activity::onExit(); }

void OpdsServerListActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  auto activateSelected = [this] { handleSelection(); };

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (pickerMode) {
      activityManager.goToFileTransfer();
    } else {
      finish();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }

  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent dispatch.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;  // dispatched to onRowEvent
    }
  }

  const int itemCount = getItemCount();
  if (itemCount > 0) {
    // Swipes scroll the viewport; the selection stays put and button
    // navigation pulls the view back to it.
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
      const int next = scrollListBy(topIndex, delta, visibleRows, itemCount);
      if (next != topIndex) {
        topIndex = next;
        requestUpdate();
      }
      return;
    }
    const auto moveSelection = [this, itemCount](const int index) {
      selectedIndex = index;
      topIndex = followListSelection(selectedIndex, topIndex, visibleRows, itemCount);
      requestUpdate();
    };
    buttonNavigator.onNext(
        [this, itemCount, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectedIndex, itemCount)); });
    buttonNavigator.onPrevious(
        [this, itemCount, &moveSelection] { moveSelection(ButtonNavigator::previousIndex(selectedIndex, itemCount)); });
  }
}

void OpdsServerListActivity::handleSelection() {
  const auto serverCount = static_cast<int>(OPDS_STORE.getCount());

  if (pickerMode) {
    // Picker mode: select a server or add the first one without leaving the flow.
    if (selectedIndex < serverCount) {
      activityManager.goToOpdsServer(static_cast<uint32_t>(selectedIndex));
    } else {
      auto editor = makeUniqueNoThrow<OpdsSettingsActivity>(renderer, mappedInput, -1);
      if (!editor) {
        LOG_ERR("OPDS", "OOM: OPDS settings activity");
        return;
      }
      startActivityForResult(std::move(editor), [this](const ActivityResult&) {
        OPDS_STORE.loadFromFile();
        selectedIndex = 0;
        topIndex = 0;
        requestUpdate();
      });
    }
    return;
  }

  // Item layout: configured servers, Add Server, Download Folder.
  if (selectedIndex == serverCount + 1) {
    showFolderError = showSaveError = false;
    auto resultHandler = [this](const ActivityResult& result) {
      if (result.isCancelled) return;

      const auto& keyboardResult = std::get<KeyboardResult>(result.data);
      std::string folder;
      if (!OpdsDownloadFolder::normalize(keyboardResult.text, folder)) {
        showFolderError = true;
        requestUpdate();
        return;
      }
      if (folder == "/") folder.clear();
      const std::string previous = SETTINGS.opdsDownloadFolder;
      strncpy(SETTINGS.opdsDownloadFolder, folder.c_str(), sizeof(SETTINGS.opdsDownloadFolder) - 1);
      SETTINGS.opdsDownloadFolder[sizeof(SETTINGS.opdsDownloadFolder) - 1] = '\0';
      if (!SETTINGS.saveToFile()) {
        LOG_ERR("OPDS", "Could not save download folder setting");
        strcpy(SETTINGS.opdsDownloadFolder, previous.c_str());
        showSaveError = true;
      }
      requestUpdate();
    };
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(
                               renderer, mappedInput, tr(STR_OPDS_DEFAULT_FOLDER), SETTINGS.opdsDownloadFolder,
                               sizeof(SETTINGS.opdsDownloadFolder) - 1, InputType::Text),
                           resultHandler);
    return;
  }

  // Settings mode: open editor for selected server, or create a new one
  auto resultHandler = [this](const ActivityResult&) {
    // Reload server list when returning from editor
    OPDS_STORE.loadFromFile();
    selectedIndex = 0;
  };

  if (selectedIndex < serverCount) {
    startActivityForResult(std::make_unique<OpdsSettingsActivity>(renderer, mappedInput, selectedIndex), resultHandler);
  } else {
    startActivityForResult(std::make_unique<OpdsSettingsActivity>(renderer, mappedInput, -1), resultHandler);
  }
}

void OpdsServerListActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<OpdsServerListActivity*>(user)->buildListScreen(screen);
}

void OpdsServerListActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  setUiContentMargin(screen, renderer,
                     fui::Insets{static_cast<int16_t>(TouchHeaderBackButton::contentTop(renderer, mappedInput)), 0,
                                 static_cast<int16_t>(UITheme::getButtonHintsReserve(renderer)), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int itemCount = getItemCount();
  if (itemCount == 0) {
    screen.centeredText(tr(STR_NO_SERVERS), screen.theme().bodyText);
    return;
  }

  const auto& servers = OPDS_STORE.getServers();
  const auto serverCount = static_cast<int>(servers.size());

  // Primary label: server name (falling back to URL if unnamed); subtitle is
  // the URL when a name is set, or the current folder/format values.
  std::vector<fui::ListItem> items;
  items.reserve(itemCount);
  for (int i = 0; i < serverCount; i++) {
    fui::ListItem item;
    item.label = servers[i].name.empty() ? servers[i].url.c_str() : servers[i].name.c_str();
    if (!servers[i].name.empty()) item.subtitle = servers[i].url.c_str();
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }
  fui::ListItem addServer;
  addServer.label = tr(STR_ADD_SERVER);
  addServer.actionValue = static_cast<int16_t>(serverCount);
  items.push_back(addServer);

  if (!pickerMode) {
    fui::ListItem folder;
    folder.label = tr(STR_OPDS_DEFAULT_FOLDER);
    folder.subtitle = SETTINGS.opdsDownloadFolder[0] ? SETTINGS.opdsDownloadFolder : tr(STR_OPDS_SD_ROOT);
    folder.actionValue = static_cast<int16_t>(serverCount + 1);
    items.push_back(folder);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  const auto rows = configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, itemCount);  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void OpdsServerListActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_OPDS_SERVERS), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_OPDS_SERVERS));
  }

  uiReady = false;
  renderUiApp(app, uiTarget);
  uiReady = true;

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (showFolderError)
    GUI.drawPopup(renderer, tr(STR_OPDS_INVALID_FOLDER));
  else if (showSaveError)
    GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
  renderer.displayBuffer();
}
