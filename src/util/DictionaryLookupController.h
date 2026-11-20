#pragma once
#if CROSSINK_APP_CAP_TOUCH
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#endif

#include <atomic>
#include <cstdint>
#include <string>

#include "Dictionary.h"
#include "LookupHistory.h"
#include "WordSelectNavigator.h"

class Activity;
class DictionaryLookupWorker;
class GfxRenderer;
class MappedInputManager;

// Shared controller for dictionary lookup flow used by DictionaryWordSelectActivity
// and DictionaryDefinitionActivity.  Owns the background lookup task, the
// stems/alt-form fallback chain, and all overlay rendering (looking-up popup,
// alt-form prompt, not-found popup).  The calling activity delegates input and
// render to this class whenever isActive() returns true.
class DictionaryLookupController {
  friend class DictionaryLookupWorker;

 public:
  enum class LookupState { Idle, LookingUp, AltFormPrompt, NotFound, ReadError };
  enum class LookupEvent {
    None,
    FoundDefinition,
    CreateClipping,
    NotFoundDismissedBack,
    NotFoundDismissedDone,
    SwitchDictionary,
    Cancelled
  };

  // How the word was ultimately resolved when FoundDefinition fires.
  enum class FoundStatus { Direct, Stem, AltForm, Suggestion };

  // Convert FoundStatus to LookupHistory::Status for history recording.
  static LookupHistory::Status toHistStatus(FoundStatus fs) {
    switch (fs) {
      case FoundStatus::Direct:
        return LookupHistory::Status::Direct;
      case FoundStatus::Stem:
        return LookupHistory::Status::Stem;
      case FoundStatus::AltForm:
        return LookupHistory::Status::AltForm;
      case FoundStatus::Suggestion:
        return LookupHistory::Status::Suggestion;
      default:
        return LookupHistory::Status::NotFound;
    }
  }

  // The owning activity keeps its cache path alive longer than the controller.
  // Borrow it here so constructing a low-memory dictionary activity does not
  // duplicate the path through a throwing std::string allocation.
  DictionaryLookupController(GfxRenderer& renderer, MappedInputManager& mappedInput, Activity& owner,
                             const std::string& cachePath, bool allowCreateClipping = false);
  ~DictionaryLookupController();

  // Start a lookup. Transitions Idle → LookingUp and notifies the shared
  // background worker.
  // If recordHistory is true (default), adds the word to lookup history on success.
  void startLookup(const std::string& word, bool recordHistory = true, bool allowFallback = true);

  // Like startLookup but marks the result as Suggestion (word came from fuzzy suggestions list).
  void startLookupAsSuggestion(const std::string& word);

  // Called by the activity after the suggestions path has been exhausted.
  // Transitions to NotFound state.
  void setNotFound();

  // Must be called from the activity's onExit() to cancel and drain any
  // running shared-worker request before this controller is destroyed.
  void onExit();

  // True when the controller owns input/render (LookingUp, AltFormPrompt, NotFound, ReadError).
  bool isActive() const { return state != LookupState::Idle; }

  // Process input for the current state.  Returns an event the activity must handle.
  LookupEvent handleInput();

  // Draw the appropriate overlay and call displayBuffer.  Returns true when fully
  // handled (activity must return immediately from render()).
  bool render();

  // Inform the activity's skipLoopDelay() override.
  bool skipLoopDelay() const { return state == LookupState::LookingUp; }

  // Lets a definition view avoid redrawing stale content while a replacement
  // dictionary lookup is resolving in the background.
  bool isLookingUp() const { return state == LookupState::LookingUp; }
  // Treat an external Home-key press like Back without starting another lookup.
  void requestBack();

  // Definition modals render lookup progress inside their existing frame so a
  // chained lookup does not dirty reader pixels outside the modal.
  void setLookupToastEnabled(bool enabled) { lookupToastEnabled_ = enabled; }
  bool hasFailureFeedback() const { return state == LookupState::NotFound || state == LookupState::ReadError; }
  const char* getFailureMessage() const;
  bool dismissFailureForDictionarySwitch();
  bool requiresBackgroundRedrawAfterOverlay() const { return state == LookupState::AltFormPrompt; }
  bool consumeFullScreenChildDisturbance() {
    const bool disturbed = fullScreenChildWasShown_;
    fullScreenChildWasShown_ = false;
    return disturbed;
  }

  // Show the "no word" popup with a 1-second delay, then request update.
  void showNoWordPopup();

  // Clean the word and start lookup; shows no-word popup if cleaning yields empty.
  void lookupOrPopup(const std::string& rawWord, size_t highlightedWordCount = 1);

  // Handle multi-select input from the navigator. Returns true if input was consumed
  // (caller should return from loop). Cleans the phrase and starts lookup or shows popup.
  bool handleMultiSelect(WordSelectNavigator& navigator);

  // Handle single-word confirm lookup from the navigator. Returns true if input was consumed
  // (caller should return from loop). Gets the selected word and starts lookup or shows popup.
  bool handleConfirmLookup(const WordSelectNavigator& navigator);

  const std::string& getLookupWord() const { return lookupWord; }
  const std::string& getFoundWord() const { return foundWord; }
  const DictLocation& getFoundLocation() const { return foundLocation; }
  FoundStatus getFoundStatus() const { return foundStatus; }
  bool getRecordHistory() const { return recordHistory_; }
  bool canCreateClipping() const { return allowCreateClipping_; }

 private:
#if CROSSINK_APP_CAP_TOUCH
  using AltFormUiApp = freeink::ui::FreeInkApp<3, 1>;
  static constexpr freeink::ui::ActionId ACTION_ALT_FORM_NO = 1;
  static constexpr freeink::ui::ActionId ACTION_ALT_FORM_YES = 2;
  static constexpr freeink::ui::ActionId ACTION_CREATE_CLIPPING = 3;
  static constexpr freeink::ui::ActionId ACTION_SWITCH_DICTIONARY = 4;

  static void altFormPromptScreen(AltFormUiApp::ScreenType& screen, void* user);
  void buildAltFormPromptScreen(AltFormUiApp::ScreenType& screen);
#endif

  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  Activity& owner;
  const std::string& cachePath;

#if CROSSINK_APP_CAP_TOUCH
  freeink::ui::GfxRendererTarget altFormUiTarget;
  AltFormUiApp altFormUiApp;
  bool altFormUiReady = false;
#endif

  LookupState state = LookupState::Idle;
  FoundStatus foundStatus = FoundStatus::Direct;
  bool nextIsSuggestion = false;
  bool lookupMatchedStem = false;
  bool allowFallback_ = true;
  bool recordHistory_ = true;
  bool lookupToastEnabled_ = true;
  bool fullScreenChildWasShown_ = false;
  bool allowCreateClipping_ = false;
  bool shouldOfferAltForms_ = true;
  std::string preparedQuickIndexPath;

  std::string lookupWord;
  std::string foundWord;
  DictLocation foundLocation;
  bool searchingAltForms_ = false;
  bool hasAltForms_ = false;

  // CLEANUP: on Auto-only commit, delete only this line (threshold/cache/method below drive Auto mode — keep)
  static constexpr uint32_t AUTO_POPUP_CSPT_ENTRY_THRESHOLD = 50000;
  uint32_t csptEntryCountCached = UINT32_MAX;  // sentinel: not yet read
  bool shouldShowPopup();

  std::atomic<int> lookupProgress = 0;
  std::atomic<bool> lookupDone = false;
  std::atomic<bool> lookupCancelled = false;
  std::atomic<bool> lookupCancelRequested = false;
  std::atomic<bool> lookupReadError = false;

  void runLookup();
  void handleLookupFailed();
  void showReadError();
  void showMemoryErrorAndReset();
  static void progressCallback(void* ctx, int percent);
  static bool cancelCallback(void* ctx);
};
