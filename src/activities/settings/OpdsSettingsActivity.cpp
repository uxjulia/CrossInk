#include "OpdsSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/OptionSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/OpdsDownloadFolder.h"

namespace fui = freeink::ui;

namespace {
// Editable fields: Name, URL, Username, Password, Filename, Use Server Filename, Download Folder.
// Existing servers also show a Delete option (BASE_ITEMS + 1).
constexpr int BASE_ITEMS = 7;
constexpr fui::ActionId ACTION_ROW = 1;
}  // namespace

OpdsSettingsActivity::OpdsSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const int serverIndex)
    : Activity("OpdsSettings", renderer, mappedInput),
      serverIndex(serverIndex),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void OpdsSettingsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<OpdsSettingsActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->getMenuItemCount())) return;
  self->selectedIndex = static_cast<size_t>(event.value);
  // Activation opens a keyboard or leaves the screen; a lingering flash would
  // gray an unrelated row.
  self->app.clearTapFlash();
  self->handleSelection();
}

int OpdsSettingsActivity::getMenuItemCount() const {
  return isNewServer ? BASE_ITEMS : BASE_ITEMS + 1;  // +1 for Delete
}

void OpdsSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  isNewServer = (serverIndex < 0);
  showSaveError = false;
  showFolderError = false;

  if (!isNewServer) {
    // Edit flow: copy the selected server into local editable state.
    // Changes are persisted field-by-field through saveServer().
    const auto* server = OPDS_STORE.getServer(static_cast<size_t>(serverIndex));
    if (server) {
      editServer = *server;
    } else {
      // Server was deleted between navigation and entering this screen — treat as new
      isNewServer = true;
      serverIndex = -1;
    }
  }

  updateDownloadFolderLabel();
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &OpdsSettingsActivity::onRowEvent, this);
  app.setScreen(&OpdsSettingsActivity::listScreen, this);
  requestUpdate();
}

void OpdsSettingsActivity::onExit() { Activity::onExit(); }

void OpdsSettingsActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finish();
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

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int menuItems = getMenuItemCount();
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    topIndex = scrollListBy(topIndex, swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows,
                            visibleRows, menuItems);
    requestUpdate();
    return;
  }
  buttonNavigator.onNext([this, menuItems] {
    selectedIndex = (selectedIndex + 1) % menuItems;
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, menuItems);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuItems] {
    selectedIndex = (selectedIndex + menuItems - 1) % menuItems;
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, menuItems);
    requestUpdate();
  });
}

bool OpdsSettingsActivity::saveServer() {
  bool success = false;

  if (isNewServer) {
    // Create flow: first save inserts a new server record into the multi-server store.
    success = OPDS_STORE.addServer(editServer);
    if (success) {
      // After the first successful save, promote to an existing server so
      // subsequent field edits update in-place rather than creating duplicates.
      isNewServer = false;
      serverIndex = static_cast<int>(OPDS_STORE.getCount()) - 1;
    } else {
      LOG_ERR("OPS", "Failed to add OPDS server");
    }
  } else {
    // Edit flow: update the same server entry in-place.
    success = OPDS_STORE.updateServer(static_cast<size_t>(serverIndex), editServer);
    if (!success) {
      LOG_ERR("OPS", "Failed to update OPDS server at index %d", serverIndex);
    }
  }

  showSaveError = !success;
  if (showSaveError) {
    requestUpdate();
  }

  return success;
}

void OpdsSettingsActivity::handleSelection() {
  // Each field edit is saved immediately so partially configured servers
  // survive navigation and power-loss scenarios.
  if (selectedIndex == 0) {
    // Server Name
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.name = kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SERVER_NAME),
                                                                   editServer.name, 63, InputType::Text),
                           handler);
  } else if (selectedIndex == 1) {
    // Server URL
    const std::string prefillUrl = editServer.url.empty() ? "https://" : editServer.url;
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.url = (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_OPDS_SERVER_URL),
                                                                   prefillUrl, 127, InputType::Url),
                           handler);
  } else if (selectedIndex == 2) {
    // Username
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.username = kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_USERNAME),
                                                                   editServer.username, 63, InputType::Text),
                           handler);
  } else if (selectedIndex == 3) {
    // Password
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editServer.password = kb.text;
        saveServer();
        requestUpdate();
      }
    };
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_PASSWORD),
                                                                   editServer.password, 63, InputType::Text),
                           handler);
  } else if (selectedIndex == 4) {
    const auto previous = editServer.filenameFormat;
    editServer.filenameFormat = previous == OpdsFilenameFormat::AUTHOR_TITLE ? OpdsFilenameFormat::TITLE_AUTHOR
                                                                             : OpdsFilenameFormat::AUTHOR_TITLE;
    if (!saveServer()) editServer.filenameFormat = previous;
    requestUpdate();
  } else if (selectedIndex == 5) {
    editServer.useServerFilename = !editServer.useServerFilename;
    if (!saveServer()) editServer.useServerFilename = !editServer.useServerFilename;
    requestUpdate();
  } else if (selectedIndex == 6) {
    editDownloadFolder();
  } else if (selectedIndex == BASE_ITEMS && !isNewServer) {
    // Delete flow is only available for existing servers.
    if (!OPDS_STORE.removeServer(static_cast<size_t>(serverIndex))) {
      LOG_ERR("OPS", "Failed to remove OPDS server at index %d", serverIndex);
      showSaveError = true;
      requestUpdate();
      return;
    }
    finish();
  }
}

void OpdsSettingsActivity::updateDownloadFolderLabel() {
  if (editServer.downloadFolder.empty()) {
    downloadFolderLabel = tr(STR_OPDS_USE_DEFAULT);
    downloadFolderLabel += ": ";
    downloadFolderLabel += SETTINGS.opdsDownloadFolder[0] ? SETTINGS.opdsDownloadFolder : tr(STR_OPDS_SD_ROOT);
  } else {
    downloadFolderLabel = editServer.downloadFolder == "/" ? tr(STR_OPDS_SD_ROOT) : editServer.downloadFolder;
  }
}

void OpdsSettingsActivity::setDownloadFolder(std::string folder) {
  showFolderError = false;
  const std::string previous = editServer.downloadFolder;
  editServer.downloadFolder = std::move(folder);
  if (!saveServer()) editServer.downloadFolder = previous;
  updateDownloadFolderLabel();
  requestUpdate();
}

void OpdsSettingsActivity::editDownloadFolder() {
  showFolderError = false;
  std::vector<std::string> options;
  options.reserve(3);
  options.emplace_back(tr(STR_OPDS_USE_DEFAULT));
  options.back() += ": ";
  options.back() += SETTINGS.opdsDownloadFolder[0] ? SETTINGS.opdsDownloadFolder : tr(STR_OPDS_SD_ROOT);
  options.emplace_back(tr(STR_OPDS_SD_ROOT));
  options.emplace_back(tr(STR_OPDS_CUSTOM_FOLDER));
  const uint8_t selected = editServer.downloadFolder.empty() ? 0 : editServer.downloadFolder == "/" ? 1 : 2;
  auto picker = makeUniqueNoThrow<OptionSelectionActivity>(
      renderer, mappedInput, "OpdsFolder", StrId::STR_OPDS_DOWNLOAD_FOLDER, std::move(options), selected, false, true);
  if (!picker) {
    LOG_ERR("OPS", "Cannot allocate folder picker");
    showSaveError = true;
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(picker), [this](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto choice = std::get<OptionSelectionResult>(result.data).index;
    if (choice < 2) {
      setDownloadFolder(choice == 0 ? "" : "/");
      return;
    }
    auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_OPDS_CUSTOM_FOLDER),
                                                             editServer.downloadFolder, OpdsDownloadFolder::MAX_LENGTH,
                                                             InputType::Text, 1);
    if (!keyboard) {
      LOG_ERR("OPS", "Cannot allocate folder keyboard");
      showSaveError = true;
      requestUpdate();
      return;
    }
    startActivityForResult(std::move(keyboard), [this](const ActivityResult& entry) {
      if (entry.isCancelled) return;
      std::string folder;
      if (!OpdsDownloadFolder::normalize(std::get<KeyboardResult>(entry.data).text, folder) || folder.empty()) {
        showFolderError = true;
        requestUpdate();
        return;
      }
      setDownloadFolder(std::move(folder));
    });
  });
}

void OpdsSettingsActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<OpdsSettingsActivity*>(user)->buildListScreen(screen);
}

void OpdsSettingsActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  setUiContentMargin(screen, renderer,
                     fui::Insets{static_cast<int16_t>(TouchHeaderBackButton::contentTop(renderer, mappedInput)), 0,
                                 static_cast<int16_t>(UITheme::getButtonHintsReserve(renderer)), 0});

  // URL hint where the old sub-header band sat.
  const fui::Rect band = screen.takeTop(static_cast<int16_t>(metrics.tabBarHeight));
  const int16_t pad = screen.theme().headerSidePadding;
  screen.target().text(band.inset(fui::Insets{0, pad, 0, pad}), tr(STR_CALIBRE_URL_HINT), screen.theme().smallText);
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const StrId fieldNames[] = {StrId::STR_SERVER_NAME,
                              StrId::STR_OPDS_SERVER_URL,
                              StrId::STR_USERNAME,
                              StrId::STR_PASSWORD,
                              StrId::STR_FILENAME,
                              StrId::STR_USE_SERVER_FILENAME,
                              StrId::STR_OPDS_DOWNLOAD_FOLDER};
  const int menuItems = getMenuItemCount();
  const char* values[BASE_ITEMS] = {
      editServer.name.empty() ? tr(STR_NOT_SET) : editServer.name.c_str(),
      editServer.url.empty() ? tr(STR_NOT_SET) : editServer.url.c_str(),
      editServer.username.empty() ? tr(STR_NOT_SET) : editServer.username.c_str(),
      editServer.password.empty() ? tr(STR_NOT_SET) : "******",
      editServer.filenameFormat == OpdsFilenameFormat::TITLE_AUTHOR ? tr(STR_TITLE_AUTHOR) : tr(STR_AUTHOR_TITLE),
      nullptr,
      downloadFolderLabel.c_str(),
  };

  std::vector<fui::ListItem> items;
  items.reserve(menuItems);
  for (int i = 0; i < menuItems; i++) {
    fui::ListItem item;
    item.label = i < BASE_ITEMS ? I18N.get(fieldNames[i]) : tr(STR_DELETE_SERVER);
    if (i < BASE_ITEMS) item.value = values[i];
    item.toggle = i == 5;
    item.toggleChecked = editServer.useServerFilename;
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the value and the row edge
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, menuItems);  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void OpdsSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  // Reuse STR_OPDS_BROWSER as the "edit existing server" title.
  // New server creation uses STR_ADD_SERVER.
  const char* header = isNewServer ? tr(STR_ADD_SERVER) : tr(STR_OPDS_BROWSER);
  // Header via GUI.drawHeader (already FreeInkUI-themed) for the battery
  // indicator; the rest of the screen renders through the app.
  const Rect headerRect = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, headerRect, header, false);
  } else {
    GUI.drawHeader(renderer, headerRect, header);
  }
  uiReady = false;
  renderUiApp(app, uiTarget);
  uiReady = true;

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (showFolderError) {
    GUI.drawPopup(renderer, tr(STR_OPDS_INVALID_FOLDER));
  } else if (showSaveError) {
    GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
  }

  renderer.displayBuffer();
}
