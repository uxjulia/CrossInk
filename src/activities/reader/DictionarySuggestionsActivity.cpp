#include "DictionarySuggestionsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "util/DictionaryActivityUtils.h"

namespace fui = freeink::ui;

DictionarySuggestionsActivity::DictionarySuggestionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                             std::vector<std::string> suggestions)
    : Activity("DictionarySuggestions", renderer, mappedInput),
      suggestions(std::move(suggestions)),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void DictionarySuggestionsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  topIndex = 0;
  visibleRows = 1;
  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &DictionarySuggestionsActivity::onRowEvent, this);
  app.setScreen(&DictionarySuggestionsActivity::suggestionsScreen, this);
  uiItems.clear();
  uiItems.reserve(suggestions.size());
  for (size_t i = 0; i < suggestions.size(); ++i) {
    fui::ListItem item;
    item.label = suggestions[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    uiItems.push_back(item);
  }
  requestUpdate();
}

void DictionarySuggestionsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<DictionarySuggestionsActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->suggestions.size())) return;
  self->selectedIndex = event.value;
  self->app.clearTapFlash();
  self->setResult(WordResult{self->suggestions[self->selectedIndex]});
  self->finish();
}

void DictionarySuggestionsActivity::suggestionsScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<DictionarySuggestionsActivity*>(user)->buildSuggestionsScreen(screen);
}

void DictionarySuggestionsActivity::buildSuggestionsScreen(UiApp::ScreenType& screen) {
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
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(suggestions.size()));
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void DictionarySuggestionsActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    DictUtils::cancelAndFinish(*this);
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
    const int next = scrollListBy(topIndex, delta, visibleRows, static_cast<int>(suggestions.size()));
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(WordResult{suggestions[selectedIndex]});
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }
  const bool prevItem = DictUtils::dictionaryPageButtonTriggered(mappedInput, true) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextItem = DictUtils::dictionaryPageButtonTriggered(mappedInput, false) ||
                        mappedInput.wasReleased(MappedInputManager::Button::Right);
  if (prevItem && selectedIndex > 0) {
    selectedIndex--;
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, static_cast<int>(suggestions.size()));
    requestUpdate();
  }
  if (nextItem && selectedIndex < static_cast<int>(suggestions.size()) - 1) {
    selectedIndex++;
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, static_cast<int>(suggestions.size()));
    requestUpdate();
  }
}

void DictionarySuggestionsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_DICT_DID_YOU_MEAN), true);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_DICT_DID_YOU_MEAN));
  }
  uiReady = false;
  renderUiApp(app, uiTarget);
  uiReady = true;
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
