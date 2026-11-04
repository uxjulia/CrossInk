#include "LookedUpWordsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "Memory.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"
#include "util/LookupHistory.h"

namespace fui = freeink::ui;

LookedUpWordsActivity::LookedUpWordsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::string bookCachePath, const char* dictionaryFontFamilyName,
                                             const uint8_t dictionaryFontPointSize)
    : Activity("LookedUpWords", renderer, mappedInput),
      cachePath(std::move(bookCachePath)),
      controller(renderer, mappedInput, *this, cachePath),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {
  this->dictionaryFontPointSize = dictionaryFontPointSize;
  if (dictionaryFontFamilyName) {
    std::strncpy(this->dictionaryFontFamilyName, dictionaryFontFamilyName, sizeof(this->dictionaryFontFamilyName) - 1);
  }
}

const char* LookedUpWordsActivity::glyphFor(LookupHistory::Status s) {
  switch (s) {
    case LookupHistory::Status::Direct:
      return "\xe2\x88\x9a";  // √ U+221A
    case LookupHistory::Status::Stem:
      return "~";
    case LookupHistory::Status::AltForm:
      return "~";
    case LookupHistory::Status::Suggestion:
      return "?";
    case LookupHistory::Status::NotFound:
      return "\xc3\x97";  // × U+00D7
    default:
      return "?";
  }
}

void LookedUpWordsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  topIndex = 0;
  visibleRows = 1;
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &LookedUpWordsActivity::onRowEvent, this);
  app.setScreen(&LookedUpWordsActivity::historyScreen, this);
  reloadEntries();
  requestUpdate();
}

void LookedUpWordsActivity::reloadEntries() {
  entries = LookupHistory::load(cachePath);
  labels.clear();
  labels.reserve(entries.size());
  uiItems.clear();
  uiItems.reserve(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    labels.push_back(std::string(glyphFor(entries[i].status)) + " " + entries[i].word);
    fui::ListItem item;
    item.label = labels.back().c_str();
    item.actionValue = static_cast<int16_t>(i);
    uiItems.push_back(item);
  }
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(entries.size()));
}

void LookedUpWordsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<LookedUpWordsActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->entries.size())) return;
  self->selectedIndex = event.value;
  if (event.longPress) {
    self->app.clearTapFlash();
    self->showDeleteConfirmation(false);
    return;
  }
  self->app.clearTapFlash();
  self->controller.startLookup(self->entries[self->selectedIndex].word);
}

void LookedUpWordsActivity::historyScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<LookedUpWordsActivity*>(user)->buildHistoryScreen(screen);
}

void LookedUpWordsActivity::buildHistoryScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  setUiContentMargin(
      screen, renderer,
      fui::Insets{
          static_cast<int16_t>(TouchHeaderBackButton::contentTop(renderer, mappedInput) + metrics.verticalSpacing), 0,
          static_cast<int16_t>(UITheme::getButtonHintsReserve(renderer) + metrics.verticalSpacing), 0});

  fui::ListProps props;
  props.items = uiItems.data();
  props.count = static_cast<uint16_t>(uiItems.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.topIndex = static_cast<uint16_t>(topIndex);
  props.action = ACTION_ROW;
  props.inputMask = static_cast<uint16_t>(fui::InputTouch | fui::InputLongPress);
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(entries.size()));
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void LookedUpWordsActivity::onExit() {
  controller.onExit();
  Activity::onExit();
}

void LookedUpWordsActivity::showDeleteConfirmation(const bool ignoreInitialConfirmRelease) {
  if (entries.empty() || selectedIndex < 0 || selectedIndex >= static_cast<int>(entries.size())) return;

  const std::string word = entries[selectedIndex].word;
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, std::string(tr(STR_DELETE)) + "?",
                                                              word, ignoreInitialConfirmRelease, true);
  if (!confirmation) {
    LOG_ERR("LOOKUP", "OOM: ConfirmationActivity");
    return;
  }
  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      LookupHistory::removeRecentAt(cachePath, selectedIndex);
      reloadEntries();
      if (selectedIndex >= static_cast<int>(entries.size())) {
        selectedIndex = std::max(0, static_cast<int>(entries.size()) - 1);
      }
    }
    requestUpdate();
  });
}

void LookedUpWordsActivity::loop() {
  if (controller.isActive()) {
    switch (controller.handleInput()) {
      case DictionaryLookupController::LookupEvent::FoundDefinition: {
        auto definition = makeUniqueNoThrow<DictionaryDefinitionActivity>(
            renderer, mappedInput, controller.getFoundWord(), controller.getFoundLocation(), true, cachePath,
            controller.getRecordHistory(), controller.getLookupWord(),
            DictionaryLookupController::toHistStatus(controller.getFoundStatus()), nullptr, nullptr,
            dictionaryFontFamilyName, dictionaryFontPointSize);
        if (!definition) {
          LOG_ERR("LOOKUP", "OOM allocating DictionaryDefinitionActivity (%u bytes)",
                  static_cast<unsigned>(sizeof(DictionaryDefinitionActivity)));
          requestUpdate();
          break;
        }
        startActivityForResult(std::move(definition), [this](const ActivityResult& result) {
          reloadEntries();
          if (!result.isCancelled) {
            setResult(ActivityResult{});
            finish();
          } else {
            requestUpdate();
          }
        });
        break;
      }
      case DictionaryLookupController::LookupEvent::NotFoundDismissedBack:
        reloadEntries();
        requestUpdate();
        break;
      case DictionaryLookupController::LookupEvent::NotFoundDismissedDone:
        setResult(ActivityResult{});
        finish();
        break;
      case DictionaryLookupController::LookupEvent::Cancelled:
        requestUpdate();
        break;
      default:
        break;
    }
    return;
  }

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }

  if (entries.empty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      DictUtils::cancelAndFinish(*this);
    }
    return;
  }

  // Long press Confirm: open the delete confirmation at the hold threshold.
  if (mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= Dictionary::LONG_PRESS_MS) {
    showDeleteConfirmation(true);
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
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
    const int next = scrollListBy(topIndex, delta, visibleRows, static_cast<int>(entries.size()));
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  const int totalItems = static_cast<int>(entries.size());
  const int pageItems = visibleRows;

  buttonNavigator.onNextRelease([this, totalItems] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, totalItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, totalItems] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, totalItems);
    requestUpdate();
  });
  buttonNavigator.onNextContinuous([this, totalItems, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, totalItems, pageItems);
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, totalItems);
    requestUpdate();
  });
  buttonNavigator.onPreviousContinuous([this, totalItems, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, totalItems, pageItems);
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, totalItems);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    controller.startLookup(entries[selectedIndex].word);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }
}

void LookedUpWordsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (controller.render()) return;

  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_LOOKUP_HISTORY), true);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_LOOKUP_HISTORY));
  }

  const int contentTop = TouchHeaderBackButton::contentTop(renderer, mappedInput) + metrics.verticalSpacing;

  if (entries.empty()) {
    const int midY = contentTop + (pageHeight - contentTop - UITheme::getButtonHintsReserve(renderer)) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_LOOKUP_HISTORY_EMPTY));
    const auto buttonLabels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
    GUI.drawButtonHints(renderer, buttonLabels.btn1, buttonLabels.btn2, buttonLabels.btn3, buttonLabels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  uiReady = false;
  renderUiApp(app, uiTarget);
  uiReady = true;

  const auto buttonLabels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, buttonLabels.btn1, buttonLabels.btn2, buttonLabels.btn3, buttonLabels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
