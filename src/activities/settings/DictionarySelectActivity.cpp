#include "DictionarySelectActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "util/DictionaryRegistry.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId ACTION_ROW = 1;
}  // namespace

DictionarySelectActivity::DictionarySelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   std::string bookCachePath, const bool disableCurrentSelection,
                                                   const bool temporarySelection, std::string displayedDictionary)
    : Activity("DictionarySelect", renderer, mappedInput),
      bookCachePath(std::move(bookCachePath)),
      currentEffectiveDictPath(std::move(displayedDictionary)),
      disableCurrentSelection(disableCurrentSelection),
      temporarySelection(temporarySelection),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void DictionarySelectActivity::onEnter() {
  Activity::onEnter();

  // Suppress Confirm bleed-through only in settings mode: when launched from a list,
  // the Confirm release that opened the picker fires again in the picker's first loop.
  // In per-book mode (launched via reader menu) the menu already consumed the event.
  ignoreNextConfirmRelease = bookCachePath.empty();

  scanDictionaries();

  if (bookCachePath.empty()) {
    // Settings mode: validate global path, pre-select from SETTINGS.
    Dictionary::isValidDictionary();

    selectedIndex = 0;  // default: None
    {
      const std::string activePath = Dictionary::readDictPath();
      if (!activePath.empty()) {
        for (int i = 0; i < static_cast<int>(dictFolders.size()); i++) {
          if (folderForIndex(i + 1) == activePath) {
            selectedIndex = i + 1;
            break;
          }
        }
      }
    }
  } else {
    // Per-book mode: read saved per-book path, pre-select it.
    currentBookDictPath = "";
    HalFile f;
    if (Storage.openFileForRead("DSEL", bookCachePath + "/dictionary.bin", f)) {
      const int sz = static_cast<int>(f.fileSize());
      if (sz > 0) {
        std::string path(sz, '\0');
        const int n = f.read(&path[0], sz);
        if (n > 0) {
          path.resize(n);
          currentBookDictPath = path;
        }
      }
      f.close();
    }

    selectedIndex = 0;  // default: Use Global
    if (!currentBookDictPath.empty()) {
      for (int i = 0; i < static_cast<int>(dictFolders.size()); i++) {
        if (folderForIndex(i + 1) == currentBookDictPath) {
          selectedIndex = i + 1;
          break;
        }
      }
    }

    // Build augmented "Use Global" label showing the active global dictionary name.
    // Path format: <dictRoot>/<folder>/<stem> — extract <folder>.
    const std::string globalPath = Dictionary::readConfiguredDictPath();
    std::string globalFolderName;
    if (globalPath.empty()) {
      globalFolderName = tr(STR_NONE_OPT);
    } else {
      const size_t lastSlash = globalPath.rfind('/');
      if (lastSlash != std::string::npos && lastSlash > 0) {
        const size_t prevSlash = globalPath.rfind('/', lastSlash - 1);
        globalFolderName = (prevSlash != std::string::npos)
                               ? globalPath.substr(prevSlash + 1, lastSlash - prevSlash - 1)
                               : globalPath.substr(0, lastSlash);
      } else {
        globalFolderName = globalPath;
      }
    }
    useGlobalLabel = std::string(tr(STR_DICT_USE_GLOBAL)) + " (" + globalFolderName + ")";
    if (currentEffectiveDictPath.empty()) currentEffectiveDictPath = Dictionary::readDictPath(bookCachePath.c_str());
  }

  totalItems = 1 + static_cast<int>(dictFolders.size());
  if (disableCurrentSelection) {
    selectedIndex = firstSelectableIndexFrom(selectedIndex);
  }
  if (usesPopup()) {
    std::vector<std::string> options;
    options.reserve(totalItems);
    for (int i = 0; i < totalItems; ++i) options.emplace_back(nameForIndex(i));
    optionPopup.show(StrId::STR_BOOK_DICTIONARY, options, selectedIndex, [this](const int index) {
      selectedIndex = index;
      popupSelectionMade = true;
      finishSelection();
    });
    requestUpdate();
    return;
  }
  topIndex = 0;
  visibleRows = 1;
  initialViewportPending = true;
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &DictionarySelectActivity::onRowEvent, this);
  app.setScreen(&DictionarySelectActivity::listScreen, this);
  requestUpdate();
}

void DictionarySelectActivity::onExit() {
  dictionaryRegistry.clear();
  Activity::onExit();
}

// ---------------------------------------------------------------------------
// SD card scan
// ---------------------------------------------------------------------------

void DictionarySelectActivity::scanDictionaries() {
  // Discovery lives in DictionaryRegistry (shared with the settings list and web UI).
  // Re-scan on every picker open (matches prior behaviour), then mirror the results into
  // the activity's parallel vectors so folderForIndex()/metadata/per-book logic is unchanged.
  dictionaryRegistry.discover();
  dictRoot = dictionaryRegistry.root();
  dictFolders.clear();
  dictStems.clear();
  const auto& entries = dictionaryRegistry.getEntries();
  dictFolders.reserve(entries.size());
  dictStems.reserve(entries.size());
  for (const auto& e : entries) {
    dictFolders.push_back(e.name);
    dictStems.push_back(e.stem);
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string DictionarySelectActivity::folderForIndex(int index) const {
  if (index <= 0 || index > static_cast<int>(dictFolders.size())) return "";
  return dictRoot + "/" + dictFolders[index - 1] + "/" + dictStems[index - 1];
}

std::string DictionarySelectActivity::effectiveFolderForIndex(int index) const {
  if (index == 0 && !bookCachePath.empty()) return Dictionary::readConfiguredDictPath();
  return folderForIndex(index);
}

bool DictionarySelectActivity::rowIsDisabled(int index) const {
  if (!disableCurrentSelection || currentEffectiveDictPath.empty()) return false;
  return effectiveFolderForIndex(index) == currentEffectiveDictPath;
}

int DictionarySelectActivity::firstSelectableIndexFrom(int start) const {
  if (totalItems <= 0) return 0;
  for (int offset = 0; offset < totalItems; offset++) {
    const int idx = (start + offset) % totalItems;
    if (!rowIsDisabled(idx)) return idx;
  }
  return start;
}

const char* DictionarySelectActivity::nameForIndex(int index) const {
  if (index == 0) return bookCachePath.empty() ? tr(STR_NONE_OPT) : useGlobalLabel.c_str();
  if (index <= static_cast<int>(dictFolders.size())) return dictFolders[index - 1].c_str();
  return "";
}

bool DictionarySelectActivity::applySelection() {
  if (rowIsDisabled(selectedIndex)) return false;

  std::string folder = folderForIndex(selectedIndex);

  if (bookCachePath.empty()) {
    // Settings mode: update global dictionary.bin.
    if (Dictionary::readDictPath() == folder) return false;
    Dictionary::saveGlobalDictPath(folder.c_str());
  } else {
    // Per-book mode: save to book cache.
    if (currentBookDictPath == folder) return false;
    HalFile f;
    if (Storage.openFileForWrite("DSEL", bookCachePath + "/dictionary.bin", f)) {
      f.write(reinterpret_cast<const uint8_t*>(folder.c_str()), folder.size());
      f.close();
    } else {
      LOG_ERR("DSEL", "Could not save per-book dictionary");
    }
    currentBookDictPath = folder;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void DictionarySelectActivity::finishSelection() {
  if (temporarySelection) {
    if (rowIsDisabled(selectedIndex)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
    } else {
      setResult(ActivityResult{FilePathResult{effectiveFolderForIndex(selectedIndex)}});
    }
    finish();
    return;
  }

  if (!applySelection()) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
  }
  finish();
}

void DictionarySelectActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<DictionarySelectActivity*>(user);
  if (event.value < 0 || event.value >= self->totalItems || self->rowIsDisabled(event.value)) return;
  self->selectedIndex = event.value;
  self->app.clearTapFlash();
  self->finishSelection();
}

void DictionarySelectActivity::loop() {
  if (usesPopup()) {
    optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
    if (!optionPopup.isActive() && !popupSelectionMade) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (ignoreNextConfirmRelease) {
      ignoreNextConfirmRelease = false;
      return;
    }

    finishSelection();
    return;
  }

  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int next = scrollListBy(topIndex, swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows,
                                  visibleRows, totalItems);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  const auto moveForward = [this](const int next) {
    selectedIndex = firstSelectableIndexFrom(next);
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, totalItems);
    requestUpdate();
  };
  const auto moveBackward = [this](int next) {
    if (totalItems > 0) {
      for (int offset = 0; offset < totalItems && rowIsDisabled(next); offset++) {
        next = ButtonNavigator::previousIndex(next, totalItems);
      }
    }
    selectedIndex = next;
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, totalItems);
    requestUpdate();
  };
  buttonNavigator.onNextRelease(
      [this, &moveForward] { moveForward(ButtonNavigator::nextIndex(selectedIndex, totalItems)); });
  buttonNavigator.onPreviousRelease(
      [this, &moveBackward] { moveBackward(ButtonNavigator::previousIndex(selectedIndex, totalItems)); });
  buttonNavigator.onNextContinuous(
      [this, &moveForward] { moveForward(ButtonNavigator::nextPageIndex(selectedIndex, totalItems, visibleRows)); });
  buttonNavigator.onPreviousContinuous([this, &moveBackward] {
    moveBackward(ButtonNavigator::previousPageIndex(selectedIndex, totalItems, visibleRows));
  });
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void DictionarySelectActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<DictionarySelectActivity*>(user)->buildListScreen(screen);
}

void DictionarySelectActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  setUiContentMargin(screen, renderer,
                     fui::Insets{static_cast<int16_t>(TouchHeaderBackButton::contentTop(renderer, mappedInput)), 0,
                                 static_cast<int16_t>(UITheme::getButtonHintsReserve(renderer)), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  const std::string activePath = disableCurrentSelection
                                     ? currentEffectiveDictPath
                                     : (bookCachePath.empty() ? Dictionary::readDictPath() : currentBookDictPath);
  std::vector<fui::ListItem> items;
  items.reserve(totalItems);
  for (int i = 0; i < totalItems; ++i) {
    const std::string folder = folderForIndex(i);
    fui::ListItem item;
    item.label = nameForIndex(i);
    if ((folder.empty() && activePath.empty()) || (!folder.empty() && folder == activePath))
      item.value = tr(STR_SELECTED);
    if (rowIsDisabled(i)) item.state = fui::StateDisabled;
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }
  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = initialViewportPending ? followListSelection(selectedIndex, 0, visibleRows, totalItems)
                                    : scrollListBy(topIndex, 0, visibleRows, totalItems);
  initialViewportPending = false;
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void DictionarySelectActivity::render(RenderLock&&) {
  if (usesPopup()) {
    optionPopup.processRender(renderer, mappedInput);
    return;
  }
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_DICTIONARY), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_DICTIONARY));
  }
  uiReady = false;
  renderUiApp(app, uiTarget);
  uiReady = true;

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
