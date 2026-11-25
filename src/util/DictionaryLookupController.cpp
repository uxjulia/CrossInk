#include "DictionaryLookupController.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>

#include "../activities/Activity.h"
#include "../activities/reader/DictionarySuggestionsActivity.h"
#include "CrossPointSettings.h"
#include "DictionaryLookupWorker.h"
#include "DictionaryRegistry.h"
#include "MappedInputManager.h"
#include "Memory.h"
#include "MemoryBudget.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/Dictionary.h"

DictionaryLookupController::DictionaryLookupController(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                       Activity& owner, const std::string& cachePath,
                                                       const bool allowCreateClipping)
    : renderer(renderer),
      mappedInput(mappedInput),
      owner(owner),
      cachePath(cachePath),
      allowCreateClipping_(allowCreateClipping)
#if CROSSINK_APP_CAP_TOUCH
      ,
      altFormUiTarget(makeUiTarget(renderer)),
      altFormUiApp(altFormUiTarget, altFormUiTarget.deviceContext())
#endif
{
}

DictionaryLookupController::~DictionaryLookupController() { onExit(); }

namespace {
constexpr int kDictionarySwitchTouchHeight = 56;
constexpr size_t kDictionaryNotFoundTitleCapacity = 96;

void dictionaryNotFoundTitle(char (&title)[kDictionaryNotFoundTitleCapacity]) {
  snprintf(title, sizeof(title), "%s: %s", tr(STR_DICTIONARY), tr(STR_NOT_FOUND));
}

Rect dictionarySwitchTouchRect(const GfxRenderer& renderer) {
  const int buttonHintsHeight = UITheme::getButtonHintsReserve(renderer);
  return Rect{0, renderer.getScreenHeight() - buttonHintsHeight - kDictionarySwitchTouchHeight,
              renderer.getScreenWidth(), kDictionarySwitchTouchHeight};
}

void logDictionaryLookupTaskEnd() {
  MemoryBudget::logHeapShape("dict.lookup_done");
#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2
  LOG_DBG("HEAP", "stage=dict.lookup_stack highWater=%u", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
}

}  // namespace

void DictionaryLookupController::startLookup(const std::string& word, bool recordHistory, bool allowFallback) {
  allowFallback_ = allowFallback;
  searchingAltForms_ = false;
  hasAltForms_ = false;
  MemoryBudget::logHeapShape("dict.lookup_start");
  lookupWord = word;
  foundWord.clear();
  foundLocation = DictLocation{};
  lookupProgress = 0;
  lookupDone = false;
  lookupCancelled = false;
  lookupCancelRequested = false;
  lookupReadError = false;
  lookupMatchedStem = false;
  recordHistory_ = recordHistory;
  state = LookupState::LookingUp;
  // CLEANUP: on Auto-only commit, delete only this line (gate below stays — it's the Auto check)
  if (lookupToastEnabled_ && shouldShowPopup()) {
    // Toast overlay: draw popup directly over whatever the user is currently viewing.
    // RenderLock serializes against the render task — without it, a prior requestUpdate()
    // (e.g. from navigation) may still be mid-refresh, and concurrent framebuffer / SPI
    // access from two tasks crashes the e-ink driver.
    RenderLock lock;
    GUI.drawPopup(renderer, tr(STR_DICT_LOOKING_UP));  // refreshes the panel itself
  } else if (!lookupToastEnabled_) {
    owner.requestUpdate();
  }
  if (!DictionaryLookupWorker::instance().start(*this)) {
    showMemoryErrorAndReset();
  }
}

void DictionaryLookupController::startLookupAsSuggestion(const std::string& word) {
  nextIsSuggestion = true;
  startLookup(word);
}

void DictionaryLookupController::setNotFound() {
  state = LookupState::NotFound;
#if CROSSINK_APP_CAP_TOUCH
  if (mappedInput.hasTouch()) {
    altFormUiReady = false;
    applySharedUiTheme(altFormUiApp, altFormUiTarget);
    if (allowCreateClipping_) {
      altFormUiApp.setScreen(&DictionaryLookupController::altFormPromptScreen, this);
    }
  }
#endif
  owner.requestUpdate();
}

void DictionaryLookupController::onExit() {
  lookupCancelRequested = true;
  DictionaryLookupWorker::instance().waitForOwner(*this);
}

void DictionaryLookupController::requestBack() {
  if (state == LookupState::LookingUp) {
    lookupCancelRequested = true;
  } else if (state != LookupState::Idle) {
    state = LookupState::Idle;
    nextIsSuggestion = false;
  }
  owner.requestUpdate();
}

DictionaryLookupController::LookupEvent DictionaryLookupController::handleInput() {
  if (state == LookupState::LookingUp) {
    if (lookupDone) {
      state = LookupState::Idle;
      if (lookupCancelRequested || lookupCancelled) {
        nextIsSuggestion = false;
        return LookupEvent::Cancelled;
      }

      if (foundLocation.found) {
        foundWord = std::move(foundLocation.headword);
        if (nextIsSuggestion)
          foundStatus = FoundStatus::Suggestion;
        else if (searchingAltForms_)
          foundStatus = FoundStatus::AltForm;
        else
          foundStatus = lookupMatchedStem ? FoundStatus::Stem : FoundStatus::Direct;
        nextIsSuggestion = false;
        return LookupEvent::FoundDefinition;
      }

      // Try alt forms
      if (!searchingAltForms_ && hasAltForms_) {
        state = LookupState::AltFormPrompt;
#if CROSSINK_APP_CAP_TOUCH
        altFormUiReady = false;
        applySharedUiTheme(altFormUiApp, altFormUiTarget);
        altFormUiApp.setScreen(&DictionaryLookupController::altFormPromptScreen, this);
#endif
        owner.requestUpdate();
        return LookupEvent::None;
      }

      if (lookupReadError) {
        showReadError();
        return LookupEvent::None;
      }

      handleLookupFailed();
      return LookupEvent::None;
    }

    // Task still running — check for cancel
    if (!lookupCancelRequested && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      lookupCancelRequested = true;
      owner.requestUpdate();
    }
    return LookupEvent::None;
  }

  if (state == LookupState::AltFormPrompt) {
#if CROSSINK_APP_CAP_TOUCH
    freeink::ui::ActionId touchAction = freeink::ui::NO_ACTION;
    const bool headerTapped = TouchHeaderBackButton::wasTapped(mappedInput, renderer);
    if (altFormUiReady && mappedInput.hasTouch()) {
      const auto event = altFormUiApp.route(touchSnapshotFrom(mappedInput));
      if (altFormUiApp.invalidated()) owner.requestUpdate();
      touchAction = event.action;
    }
#endif
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)
#if CROSSINK_APP_CAP_TOUCH
        || touchAction == ACTION_ALT_FORM_YES
#endif
    ) {
      searchingAltForms_ = true;
      lookupDone = false;
      state = LookupState::LookingUp;
      if (!DictionaryLookupWorker::instance().start(*this))
        showMemoryErrorAndReset();
      else
        owner.requestUpdate();
      return LookupEvent::None;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)
#if CROSSINK_APP_CAP_TOUCH
        || headerTapped || touchAction == ACTION_ALT_FORM_NO
#endif
    ) {
      state = LookupState::Idle;
      nextIsSuggestion = false;
      // Declining the optional alternate-form lookup completes this lookup.
      // The word-selection activity treats this the same as dismissing a
      // not-found result, so it returns to the reader instead of restoring
      // the selected word underneath the prompt.
      return LookupEvent::NotFoundDismissedBack;
    }
#if CROSSINK_APP_CAP_TOUCH
    if (allowCreateClipping_ && touchAction == ACTION_CREATE_CLIPPING) {
      state = LookupState::Idle;
      return LookupEvent::CreateClipping;
    }
#endif
    return LookupEvent::None;
  }

  if (state == LookupState::NotFound || state == LookupState::ReadError) {
#if CROSSINK_APP_CAP_TOUCH
    freeink::ui::ActionId touchAction = freeink::ui::NO_ACTION;
    const bool headerTapped = state == LookupState::NotFound && TouchHeaderBackButton::wasTapped(mappedInput, renderer);
    if (state == LookupState::NotFound && altFormUiReady && mappedInput.hasTouch()) {
      const auto event = altFormUiApp.route(touchSnapshotFrom(mappedInput));
      if (altFormUiApp.invalidated()) owner.requestUpdate();
      touchAction = event.action;
    }
    if (state == LookupState::NotFound && allowCreateClipping_ && touchAction == ACTION_CREATE_CLIPPING) {
      state = LookupState::Idle;
      return LookupEvent::CreateClipping;
    }
    if (state == LookupState::NotFound && touchAction == ACTION_SWITCH_DICTIONARY) {
      state = LookupState::Idle;
      return LookupEvent::SwitchDictionary;
    }
    if (state == LookupState::ReadError) {
      int touchX = 0;
      int touchY = 0;
      const Rect switchRect = dictionarySwitchTouchRect(renderer);
      if (mappedInput.hasTouch() && mappedInput.wasScreenTapped(touchX, touchY) && touchX >= switchRect.x &&
          touchX < switchRect.x + switchRect.width && touchY >= switchRect.y &&
          touchY < switchRect.y + switchRect.height) {
        state = LookupState::Idle;
        return LookupEvent::SwitchDictionary;
      }
    }
#endif
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      state = LookupState::Idle;
      return LookupEvent::SwitchDictionary;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = LookupState::Idle;
      return LookupEvent::NotFoundDismissedDone;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)
#if CROSSINK_APP_CAP_TOUCH
        || headerTapped
#endif
    ) {
      state = LookupState::Idle;
      return LookupEvent::NotFoundDismissedBack;
    }
    return LookupEvent::None;
  }

  return LookupEvent::None;
}

#if CROSSINK_APP_CAP_TOUCH
void DictionaryLookupController::altFormPromptScreen(AltFormUiApp::ScreenType& screen, void* user) {
  static_cast<DictionaryLookupController*>(user)->buildAltFormPromptScreen(screen);
}

void DictionaryLookupController::buildAltFormPromptScreen(AltFormUiApp::ScreenType& screen) {
  if (!mappedInput.hasTouch()) return;

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  const bool isAltForm = state == LookupState::AltFormPrompt;
  const uint8_t actionCount = isAltForm ? (allowCreateClipping_ ? 3 : 2) : (allowCreateClipping_ ? 2 : 0);
  const int16_t actionHeight = std::max<int16_t>(theme.rowHeight, kDictionarySwitchTouchHeight);
  const int16_t actionGap = theme.spaceMd;
  const int16_t actionBandHeight =
      actionCount == 0
          ? 0
          : static_cast<int16_t>(actionHeight * actionCount + actionGap * static_cast<int16_t>(actionCount - 1));
  // Keep the final action clear of the e-ink panel's bottom edge. The visible
  // button-hint strip is not enough margin on every touch device.
  screen.setContentMargin(
      freeink::ui::Insets{0, 0, static_cast<int16_t>(UITheme::getButtonHintsReserve(renderer) + theme.spaceLg), 0});
  const freeink::ui::Rect band = screen.takeBottom(actionBandHeight, theme.spaceLg);
  const int16_t inset = static_cast<int16_t>(metrics.contentSidePadding);
  const freeink::ui::Rect area{static_cast<int16_t>(band.x + inset), band.y,
                               std::max<int16_t>(1, static_cast<int16_t>(band.width - inset * 2)), band.height};

  freeink::ui::ButtonProps button;
  button.inputMask = freeink::ui::InputTouch;
  button.styles = freeink::ui::outlinedButtonStyles();
  button.text = theme.bodyText;
  button.text.align = freeink::ui::TextAlign::Center;
  button.minTouchSize = actionHeight;
  auto addButton = [&](const char* label, const freeink::ui::ActionId action, const bool bold, const int index) {
    button.label = label;
    button.action = action;
    button.text.bold = bold;
    screen.button(button, freeink::ui::Rect{area.x, static_cast<int16_t>(area.y + index * (actionHeight + actionGap)),
                                            area.width, actionHeight});
  };
  if (isAltForm) {
    addButton(tr(STR_YES), ACTION_ALT_FORM_YES, false, 0);
    addButton(tr(STR_NO), ACTION_ALT_FORM_NO, false, 1);
    if (allowCreateClipping_) addButton(tr(STR_SAVE_CLIPPING), ACTION_CREATE_CLIPPING, true, 2);
  } else if (allowCreateClipping_) {
    addButton(tr(STR_SWITCH_DICTIONARY), ACTION_SWITCH_DICTIONARY, false, 0);
    addButton(tr(STR_SAVE_CLIPPING), ACTION_CREATE_CLIPPING, false, 1);
  }
}
#endif

bool DictionaryLookupController::render() {
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (state == LookupState::LookingUp) {
    // Popup is drawn inline as a toast in startLookup(); nothing to do from the render task.
    // Returning false lets the activity's normal render run (e.g. on cancel, the page repaints
    // which naturally wipes the toast overlay).
    return false;
  }

  if (state == LookupState::AltFormPrompt) {
    renderer.clearScreen();
    const int pageWidth = renderer.getScreenWidth();
    const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
#if CROSSINK_APP_CAP_TOUCH
    if (mappedInput.hasTouchHardware()) {
      TouchHeaderBackButton::draw(renderer, altFormUiTarget, header, tr(STR_DICT_SEARCH_ALT_FORMS), true);
    } else
#endif
    {
      GUI.drawHeader(renderer, header, tr(STR_DICT_SEARCH_ALT_FORMS));
    }
    const int promptTop = TouchHeaderBackButton::contentTop(renderer, mappedInput) + metrics.verticalSpacing;
#if CROSSINK_APP_CAP_TOUCH
    if (mappedInput.hasTouch()) {
      const int actionRows = allowCreateClipping_ ? 3 : 2;
      const auto& theme = altFormUiApp.theme();
      const int actionHeight = std::max<int>(theme.rowHeight, kDictionarySwitchTouchHeight);
      const int actionBandHeight = actionHeight * actionRows + theme.spaceMd * (actionRows - 1);
      const int bottom = renderer.getScreenHeight() - UITheme::getButtonHintsReserve(renderer) - theme.spaceLg -
                         actionBandHeight - theme.spaceLg;
      freeink::ui::TextStyle phraseStyle = altFormUiApp.theme().bodyText;
      phraseStyle.align = freeink::ui::TextAlign::Center;
      phraseStyle.maxLines =
          static_cast<uint8_t>(std::max(1, (bottom - promptTop) / renderer.getLineHeight(UI_10_FONT_ID)));
      altFormUiTarget.text(
          freeink::ui::Rect{static_cast<int16_t>(metrics.contentSidePadding), static_cast<int16_t>(promptTop),
                            static_cast<int16_t>(std::max(1, pageWidth - metrics.contentSidePadding * 2)),
                            static_cast<int16_t>(std::max(1, bottom - promptTop))},
          lookupWord.c_str(), phraseStyle);
    } else
#endif
    {
      // Keep the original alternate-form baseline on button-only devices while
      // giving multi-word phrases the same wrapped full-screen treatment.
      const int y = promptTop + renderer.getLineHeight(UI_10_FONT_ID);
      const int bottom =
          renderer.getScreenHeight() - UITheme::getButtonHintsReserve(renderer) - metrics.verticalSpacing;
      const Rect textArea{metrics.contentSidePadding, y, std::max(1, pageWidth - metrics.contentSidePadding * 2),
                          std::max(1, bottom - y)};
      const int maxLines = std::max(1, textArea.height / renderer.getLineHeight(UI_10_FONT_ID));
      UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, y, lookupWord.c_str(), maxLines);
    }
#if CROSSINK_APP_CAP_TOUCH
    if (mappedInput.hasTouch()) {
      altFormUiReady = false;
      renderUiApp(altFormUiApp, altFormUiTarget);
      altFormUiReady = true;
    } else
#endif
    {
      const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_CONFIRM), "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return true;
  }

  if (state == LookupState::NotFound) {
    renderer.clearScreen();
    const int pageWidth = renderer.getScreenWidth();
    const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    char title[kDictionaryNotFoundTitleCapacity];
    dictionaryNotFoundTitle(title);
#if CROSSINK_APP_CAP_TOUCH
    if (mappedInput.hasTouchHardware()) {
      TouchHeaderBackButton::draw(renderer, altFormUiTarget, header, title, true);
      const int y = TouchHeaderBackButton::contentTop(renderer, mappedInput) + metrics.verticalSpacing;
      const auto& theme = altFormUiApp.theme();
      const int actionHeight = std::max<int>(theme.rowHeight, kDictionarySwitchTouchHeight);
      const int actionBandHeight = allowCreateClipping_ ? actionHeight * 2 + theme.spaceMd : 0;
      const int bottom = renderer.getScreenHeight() - UITheme::getButtonHintsReserve(renderer) - theme.spaceLg -
                         actionBandHeight - (allowCreateClipping_ ? theme.spaceLg : 0);
      freeink::ui::TextStyle phraseStyle = altFormUiApp.theme().bodyText;
      phraseStyle.align = freeink::ui::TextAlign::Center;
      phraseStyle.maxLines = static_cast<uint8_t>(std::max(1, (bottom - y) / renderer.getLineHeight(UI_10_FONT_ID)));
      altFormUiTarget.text(
          freeink::ui::Rect{static_cast<int16_t>(metrics.contentSidePadding), static_cast<int16_t>(y),
                            static_cast<int16_t>(std::max(1, pageWidth - metrics.contentSidePadding * 2)),
                            static_cast<int16_t>(std::max(1, bottom - y))},
          lookupWord.c_str(), phraseStyle);
      if (allowCreateClipping_) {
        altFormUiReady = false;
        renderUiApp(altFormUiApp, altFormUiTarget);
        altFormUiReady = true;
      }
      const auto labels =
          mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_DONE), tr(STR_DICT_SWITCH), "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return true;
    }
#endif
    GUI.drawHeader(renderer, header, title);
    const int y = TouchHeaderBackButton::contentTop(renderer, mappedInput) + metrics.verticalSpacing +
                  renderer.getLineHeight(UI_10_FONT_ID);
    const int bottom = renderer.getScreenHeight() - UITheme::getButtonHintsReserve(renderer) - metrics.verticalSpacing;
    const Rect textArea{metrics.contentSidePadding, y, std::max(1, pageWidth - metrics.contentSidePadding * 2),
                        std::max(1, bottom - y)};
    const int maxLines = std::max(1, textArea.height / renderer.getLineHeight(UI_10_FONT_ID));
    UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, y, lookupWord.c_str(), maxLines);
    const auto labels =
        mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_DONE), tr(STR_DICT_SWITCH), "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return true;
  }

  if (state == LookupState::ReadError) {
    GUI.drawPopup(renderer, tr(STR_DICT_READ_FAILED));
#if CROSSINK_APP_CAP_TOUCH
    if (mappedInput.hasTouch()) {
      const Rect switchRect = dictionarySwitchTouchRect(renderer);
      renderer.drawLine(switchRect.x, switchRect.y, switchRect.x + switchRect.width, switchRect.y, true);
      const char* label = tr(STR_SWITCH_DICTIONARY);
      const int labelX =
          switchRect.x + (switchRect.width - renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD)) / 2;
      const int labelY = switchRect.y + (switchRect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
      renderer.drawText(UI_10_FONT_ID, labelX, labelY, label, true, EpdFontFamily::BOLD);
    }
#endif
    const auto labels =
        mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_DONE), tr(STR_DICT_SWITCH), "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return true;
  }

  return false;
}

const char* DictionaryLookupController::getFailureMessage() const {
  if (state == LookupState::ReadError) return tr(STR_DICT_READ_FAILED);
  if (state == LookupState::NotFound) return tr(STR_NOT_FOUND);
  return "";
}

bool DictionaryLookupController::dismissFailureForDictionarySwitch() {
  if (!hasFailureFeedback()) return false;
  state = LookupState::Idle;
  return true;
}

bool DictionaryLookupController::handleMultiSelect(WordSelectNavigator& navigator) {
  std::string msPhrase;
  const auto msAction = navigator.handleMultiSelectInput(mappedInput, msPhrase);
  if (msAction == WordSelectNavigator::MultiSelectAction::None) return false;
  switch (msAction) {
    case WordSelectNavigator::MultiSelectAction::PhraseReady:
      lookupOrPopup(msPhrase, navigator.getLookupSelectionWordCount());
      return true;
    case WordSelectNavigator::MultiSelectAction::ExitedMultiSelect:
    case WordSelectNavigator::MultiSelectAction::EnteredMultiSelect:
      owner.requestUpdate();
      return true;
    default:
      return true;
  }
}

bool DictionaryLookupController::handleConfirmLookup(const WordSelectNavigator& navigator) {
  if (!mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return false;
  const auto* sel = navigator.getSelected();
  if (!sel) return true;  // consumed input even if nothing selected
  lookupOrPopup(navigator.getLookup(*sel), navigator.getLookupSelectionWordCount());
  return true;
}

void DictionaryLookupController::lookupOrPopup(const std::string& rawWord, const size_t highlightedWordCount) {
  shouldOfferAltForms_ = highlightedWordCount <= 3;
  std::string cleaned = Dictionary::cleanWord(rawWord);
  if (cleaned.empty()) {
    showNoWordPopup();
  } else {
    startLookup(cleaned);
  }
}

void DictionaryLookupController::showMemoryErrorAndReset() {
  {
    RenderLock lock;
    GUI.drawPopup(renderer, tr(STR_MEMORY_ERROR));
  }
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  state = LookupState::Idle;
  owner.requestUpdate();
}

void DictionaryLookupController::showNoWordPopup() {
  {
    // Serialize with render task — see comment in startLookup() for the race this prevents.
    RenderLock lock;
    GUI.drawPopup(renderer, tr(STR_DICT_NO_WORD));
  }
  vTaskDelay(1000 / portTICK_PERIOD_MS);
  owner.requestUpdate();
}

void DictionaryLookupController::handleLookupFailed() {
  auto similar = Dictionary::findSimilar(lookupWord, 6, cachePath.c_str());
  if (!similar.empty()) {
    auto sugActivity = makeUniqueNoThrow<DictionarySuggestionsActivity>(renderer, mappedInput, std::move(similar));
    if (!sugActivity) {
      LOG_ERR("DICT", "OOM: DictionarySuggestionsActivity");
      nextIsSuggestion = false;
      showMemoryErrorAndReset();
      return;
    }
    fullScreenChildWasShown_ = true;
    owner.startActivityForResult(std::move(sugActivity), [this](const ActivityResult& result) {
      if (result.isCancelled) {
        setNotFound();
        return;
      }
      const auto& wr = std::get<WordResult>(result.data);
      startLookupAsSuggestion(wr.word);
    });
    return;
  }
  nextIsSuggestion = false;
  setNotFound();
  // Record after setNotFound() so the popup's requestUpdate() has kicked the render task —
  // the SD write below overlaps the e-ink refresh on the main task.
  LookupHistory::addWordIf(cachePath, lookupWord, LookupHistory::Status::NotFound, recordHistory_);
}

void DictionaryLookupController::showReadError() {
  nextIsSuggestion = false;
  state = LookupState::ReadError;
  owner.requestUpdate();
}

void DictionaryLookupController::progressCallback(void* ctx, int percent) {
  auto* self = static_cast<DictionaryLookupController*>(ctx);
  self->lookupProgress = percent;
  // Intentionally no requestUpdate() here — popup is a single static frame.
}

bool DictionaryLookupController::cancelCallback(void* ctx) {
  return static_cast<DictionaryLookupController*>(ctx)->lookupCancelRequested;
}

void DictionaryLookupController::runLookup() {
  DictLookupCallbacks cbs;
  cbs.ctx = this;
  cbs.onProgress = &DictionaryLookupController::progressCallback;
  cbs.shouldCancel = &DictionaryLookupController::cancelCallback;
  const std::string dictionaryPath = Dictionary::readDictPath(cachePath.c_str());
  const auto search = [&](const std::string& path) {
    if (searchingAltForms_) {
      foundLocation = Dictionary::resolveAltFormIn(path, lookupWord, cbs);
    } else {
      if (path != preparedQuickIndexPath && Dictionary::prepareQuickIndexIn(path, cbs)) preparedQuickIndexPath = path;
      if (lookupCancelRequested) return;
      foundLocation = Dictionary::locateWithStemVariantsIn(path, lookupWord, &lookupMatchedStem, cbs);
      if (!foundLocation.found && !foundLocation.readError && shouldOfferAltForms_ && !hasAltForms_)
        hasAltForms_ = Dictionary::hasAltFormsAt(path);
    }
  };
  search(dictionaryPath);
  if (allowFallback_ && !dictionaryPath.empty() && !foundLocation.found && !foundLocation.readError &&
      !lookupCancelRequested) {
    DictionaryRegistry installed;
    if (!installed.scan(cbs)) lookupReadError = true;
    for (const auto& entry : installed.getEntries()) {
      if (lookupCancelRequested) break;
      if (entry.basePath == dictionaryPath) continue;
      search(entry.basePath);
      if (foundLocation.found) break;
      // A damaged secondary dictionary must not hide a match in a later one.
      if (foundLocation.readError) lookupReadError = true;
    }
  }
  lookupReadError = lookupReadError || foundLocation.readError;
  lookupCancelled = lookupCancelRequested.load();
  lookupDone = true;
  logDictionaryLookupTaskEnd();
  // Don't call requestUpdate(true) here - it triggers an unnecessary e-ink refresh
  // of the word select activity before transitioning to the definition activity.
  // The main loop polls lookupDone every ~10ms, so response time is still fast.
}

bool DictionaryLookupController::shouldShowPopup() {
  if (csptEntryCountCached == UINT32_MAX) {
    csptEntryCountCached = Dictionary::readCsptEntryCount(cachePath.c_str());
  }
  return csptEntryCountCached == 0 || csptEntryCountCached > AUTO_POPUP_CSPT_ENTRY_THRESHOLD;
}
