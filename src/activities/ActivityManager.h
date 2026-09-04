#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cassert>
#include <memory>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "util/QuickLockTrigger.h"
#include "util/ScreenshotInfo.h"

#ifndef portMUX_INITIALIZER_UNLOCKED
struct portMUX_TYPE {};
#define portMUX_INITIALIZER_UNLOCKED \
  {                                  \
  }
#endif

class Activity;    // forward declaration
class RenderLock;  // forward declaration

enum class RequestUpdateResult { Rendered, Rejected };
enum class HomeMenuItem { NONE, FILE_BROWSER, RECENTS, OPDS_BROWSER, FILE_TRANSFER, SETTINGS_MENU };

/**
 * ActivityManager
 *
 * This mirrors the same concept of Activity in Android, where an activity represents a single screen of the UI. The
 * manager is responsible for launching activities, and ensuring that only one activity is active at a time.
 *
 * It also provides a stack mechanism to allow activities to launch sub-activities and get back the results when the
 * sub-activity is done. For example, the WebServer activity can launch a WifiSelect activity to let the user choose a
 * wifi network, and get back the selected network when the user is done.
 *
 * Main differences from Android's ActivityManager:
 * - No onPause/onResume, since we don't have a concept of background activities
 * - onActivityResult is implemented via a callback instead of a separate method, for simplicity
 */
class ActivityManager {
  friend class RenderLock;

 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  std::vector<std::unique_ptr<Activity>> stackActivities;
  std::unique_ptr<Activity> currentActivity;

  void exitActivity(const RenderLock& lock);

  // Pending activity to be launched on next loop iteration
  std::unique_ptr<Activity> pendingActivity;
  enum class PendingAction { None, Push, Pop, Replace };
  PendingAction pendingAction = PendingAction::None;
  // Set when an overlay is closed specifically to hand control back to the
  // reader's menu. It must wait until the reader is current again.
  int16_t pendingReaderMenuAction = -1;

  // A one-shot Home selection to restore after Settings replaces Home. This
  // is intentionally not persisted as recent-book order.
  std::string preferredHomeBookPath;
  bool returningHomeThroughSettings = false;

  // Task to render and display the activity
  TaskHandle_t renderTaskHandle = nullptr;
  static void renderTaskTrampoline(void* param);
  [[noreturn]] virtual void renderTaskLoop();

  // Set by requestUpdateAndWait(); read and cleared by the render task after render completes.
  // Note: only one waiting task is supported at a time
  TaskHandle_t waitingTaskHandle = nullptr;
  portMUX_TYPE renderStateMux = portMUX_INITIALIZER_UNLOCKED;

  // Mutex to protect rendering operations from race conditions
  // Must only be used via RenderLock
  SemaphoreHandle_t renderingMutex = nullptr;

  // Whether to trigger a render after the current loop()
  // This variable must only be set by the main loop, to avoid race conditions
  std::atomic<bool> requestedUpdate{false};
  // A popped full-screen child leaves its pixels in the framebuffer until the
  // restored activity renders. Partial-screen overlays must not preserve that
  // stale child as their backdrop.
  std::atomic<bool> restoredActivityNeedsRender{false};

  bool handleGlobalHomeGesture();
  bool restoreBackdropBehindCurrentOverlay();

 public:
  explicit ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : renderer(renderer), mappedInput(mappedInput), renderingMutex(xSemaphoreCreateMutex()) {
    assert(renderingMutex != nullptr && "Failed to create rendering mutex");
    stackActivities.reserve(10);
  }
  ~ActivityManager() { assert(false); /* should never be called */ };

  void begin(uint32_t renderTaskStackBytes = 16384);
  void loop();

  // Will replace currentActivity and drop all activities on stack
  void replaceActivity(std::unique_ptr<Activity>&& newActivity);

  // goTo... functions are convenient wrapper for replaceActivity()
  void goToFileTransfer(std::string returnBookPath = {});
  void goToCalibreWireless(const std::string& returnBookPath = {});
  void goToJoinNetworkFileTransfer(const std::string& returnBookPath = {});
  void goToHotspotFileTransfer(const std::string& returnBookPath = {});
  void goToUsbDrive();
  bool resumeFileTransferFromNetworkBoot(uint32_t payload);
  void goToNearbyStatsSync();
  bool goToNearbyBookSend(std::string path, bool returnToReader);
  void goToNearbyBookReceive();
  void goToSettings(bool dismissOnUpSwipe = false);
  void goToFileBrowser(std::string path = {});
  void goToRecentBooks();
  void goToBrowser();
  bool goToOpdsServer(uint32_t serverIndex, bool networkBootReady = false);
  void goToReader(std::string path, bool suppressBackRelease = false, bool allowFastInitialRefresh = false,
                  bool cleanImageBaseOnEntry = false);
  void goToReaderAndRunMenuAction(std::string path, uint8_t action);
  void goToSleep(bool fromTimeout = false);
  void goToBoot();
  void goToFullScreenMessage(std::string message, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void goToCrashReport();
  void goHome(HomeMenuItem initialMenuItem = HomeMenuItem::NONE, bool initialFullRefresh = false);

  // This will move current activity to stack instead of deleting it
  void pushActivity(std::unique_ptr<Activity>&& activity);

  // Remove the currentActivity, returning the last one on stack
  // Note: if popActivity() on last activity on the stack, we will goHome()
  void popActivity();

  bool preventAutoSleep() const;
  bool requiresExclusiveStorageLoop() const;
  // The active activity has an open modal that owns all global shortcuts and
  // gestures until it is dismissed.
  bool blocksGlobalInput() const;
  bool isHomeActivity() const;
  bool isReaderActivity() const;
  bool openReaderSettingsForTouchscreenEscapeHatch();
  bool handleHomeButtonBackOrHome();
  bool openReaderMenuFromShortcut();
  bool handleShortcutAction(uint8_t action);
  bool hasActivityNamed(const char* activityName) const;
#ifdef SIMULATOR
  bool isCurrentActivityNamed(const char* activityName) const;
#endif
  bool canSnapshotForSleepOverlay() const;
  bool requestManualReaderRefresh();
  bool handleShortcutAction(CrossPointSettings::SHORT_PWRBTN action);
  bool handleQuickLockUnlock(QuickLockTrigger trigger);
  void notifyInputLockChanged(bool locked);
  bool skipLoopDelay() const;
  std::string getCurrentBookPath() const;
  ScreenshotInfo getScreenshotInfo() const;

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  void requestUpdate(bool immediate = false);

  // Trigger a render and block until it completes.
  // Returns Rejected when a synchronous render would be unsafe, such as from the render task,
  // while another task is already waiting, or while holding a RenderLock.
  RequestUpdateResult requestUpdateAndWait();
};

extern ActivityManager activityManager;  // singleton, to be defined in main.cpp
