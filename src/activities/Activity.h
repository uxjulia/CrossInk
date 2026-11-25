#pragma once
#include <Logging.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "ActivityManager.h"  // for using the ActivityManager singleton
#include "ActivityResult.h"
#include "CrossPointSettings.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "RenderLock.h"
#include "util/QuickLockTrigger.h"
#include "util/ScreenshotInfo.h"

struct PendingOverlayResume;

class Activity {
  friend class ActivityManager;

 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  ActivityResultHandler resultHandler;
  ActivityResult result;

  // Use when a screen exits on Back press instead of Back release so the
  // parent screen does not also receive the held button's release.
  void finishAfterBackPress();

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter();
  virtual void onExit();
  virtual void prepareForSleep() {}
  virtual void loop() {}

  virtual void render(RenderLock&&) {}

  // Let activities that need more than a framebuffer refresh prepare a full redraw.
  virtual bool prepareManualRefresh() { return false; }

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  virtual void requestUpdate(bool immediate = false);

  // Request an immediate render and block until it completes.
  virtual RequestUpdateResult requestUpdateAndWait();

  virtual bool skipLoopDelay() { return false; }
  // Read on the main task even while render() holds the render lock.
  // Overrides must return a fixed value that does not depend on render state.
  virtual uint8_t inputPollDelayMs() const { return 10; }
  virtual bool preventAutoSleep() { return false; }
  // While true, main-loop global controls and activity replacement are
  // suspended so an exclusive storage owner cannot race the filesystem.
  virtual bool requiresExclusiveStorageLoop() const { return false; }
  // Called by the app-wide Quick Lock. Reader activities use it to exclude
  // locked time from reading statistics; other activities have no state to change.
  virtual void onInputLockChanged(bool) {}
  // Called from the main loop as soon as raw input arrives, before a global
  // shortcut or gesture can consume it instead of entering activity loop().
  virtual void onUserInput() {}
  // Quick Lock may permit one reader-only long-press trigger to unlock. The
  // activity validates that trigger without routing any other normal input.
  virtual bool handleQuickLockUnlock(QuickLockTrigger) { return false; }
  virtual bool isReaderActivity() const { return false; }
  // Reader-mode child screens may also report isReaderActivity(); only the
  // activity owning the open book should be the Home/Reader unwind target.
  virtual bool isBookReaderActivity() const { return false; }
  // Lists use spatial menu directions; text-selection/keyboard screens keep their own navigation rules.
  virtual bool usesMenuButtonOrientation() const { return !isBookReaderActivity(); }
  virtual bool isHomeActivity() const { return false; }
  // The open book uses its vertical swipe actions across the entire page;
  // dialogs and lists retain their normal edge or scroll gestures.
  virtual bool usesFullScreenReaderVerticalSwipes() const { return false; }
  // Overlays can reserve the frontlight gesture for their own dismissal.
  virtual bool allowFrontlightPanelGesture() const { return true; }
  // Partial-screen overlays preserve pixels from the activity beneath them.
  // After a full-screen child pops, ActivityManager must redraw that source
  // activity before pushing one of these overlays again.
  virtual bool requiresFreshBackdrop() const { return false; }
  // A backdrop-only render must not make a paused reader count overlay time as
  // reading time. Readers clear that transient render timestamp here.
  virtual void onBackdropRenderedForOverlay() {}
  virtual bool allowPowerAsConfirmInReaderMode() const { return false; }
  virtual bool allowGlobalHomeGesture() const { return true; }
  // Activities with a modal can keep global gestures from acting behind it.
  virtual bool blocksGlobalInput() const { return false; }
  // Lists that own vertical swipes can opt out of the global bottom-edge
  // Home gesture while retaining the capacitive Home key on X4 Pro.
  virtual bool allowGlobalHomeSwipeGesture() const { return true; }
  // Let overlays consume the global Home gesture as a dismiss action.
  virtual bool handleHomeGesture() { return false; }
  virtual bool canSnapshotForSleepOverlay() const { return false; }
  // Activity-specific swipe actions (chapter and font commands). Global
  // frontlight commands are handled by ActivityManager before this callback.
  virtual bool handleTwoFingerSwipeAction(CrossPointSettings::TWO_FINGER_SWIPE_ACTION) { return false; }
  // An overlay may cache the live frontlight value for its controls.
  virtual void onExternalFrontlightChange() {}
  // Completed two-finger rotations are routed only to activities that can
  // safely rebuild their content for a new screen orientation.
  virtual bool handleTwoFingerRotation(bool clockwise) { return false; }
  virtual bool openReaderSettingsMenu() { return false; }
  virtual bool handleShortcutAction(uint8_t) { return false; }
  virtual bool handleShortcutAction(CrossPointSettings::SHORT_PWRBTN) { return false; }
  virtual std::string getCurrentBookPath() const { return {}; }
  virtual std::string getCurrentBookTitle() const { return {}; }
  virtual bool getFrontlightPanelBookDetails(FrontlightPanelBookDetails&) { return false; }
  virtual bool isEpubReaderActivity() const { return false; }
  virtual std::unique_ptr<Activity> createFrontlightReadingStatsActivity() { return {}; }
  virtual void onFrontlightPanelOpened() {}
  virtual void onFrontlightPanelClosed() { requestUpdate(); }
  // Readers with per-book overrides can temporarily restore their global
  // defaults around this write. Other activities persist normally.
  virtual void persistGlobalSettings() { SETTINGS.saveToFile(); }
  virtual bool onFrontlightGlobalSettingsOpened() { return false; }
  virtual void onFrontlightGlobalSettingsClosed() {}
  virtual bool handleFrontlightPanelResult(const FrontlightPanelResult& result);
  virtual bool handleExternalReaderMenuAction(uint8_t) { return false; }
  virtual bool restorePendingOverlay(const PendingOverlayResume&) { return false; }
  virtual ScreenshotInfo getScreenshotInfo() const { return {}; }

  // Start a new activity without destroying the current one
  // Note: requestUpdate() will be invoked automatically once resultHandler finishes
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler);

  // Set the result to be passed back to the previous activity when this activity finishes
  void setResult(ActivityResult&& result);

  // Finish this activity and return to the previous one on the stack (if any)
  void finish();

  // Convenience method to facilitate API transition to ActivityManager
  // TODO: remove this in near future
  void onGoHome(HomeMenuItem item = HomeMenuItem::NONE);
  void onSelectBook(const std::string& path);
};
