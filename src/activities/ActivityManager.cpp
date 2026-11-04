#include "ActivityManager.h"

#include "activities/plugins/PluginCatalogActivity.h"

#if CROSSINK_SCALABLE_FONTS
#include <HalScalableFont.h>
#endif

#include <CrossInkHalFrontlight.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#if defined(CROSSINK_LANGUAGE_BENCHMARK)
#include <LanguageBenchmark.h>
#endif
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FilenameFontSystem.h"
#include "GlobalActions.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "ReleaseNotes.h"
#include "SilentRestart.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "components/TouchRegistry.h"
#include "home/AlertActivity.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/HomeActivity.h"
#include "home/RecentBookProgress.h"
#include "library/LibraryActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "network/NearbyBookTransferActivity.h"
#include "network/NearbyStatsSyncActivity.h"
#include "network/UsbDriveActivity.h"
#include "reader/BookReadingStats.h"
#include "reader/BookStatsActivity.h"
#include "reader/BookStatsTracking.h"
#include "reader/GlobalReadingStats.h"
#include "reader/ReaderActivity.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/ReleaseNotesActivity.h"
#include "settings/SettingsActivity.h"
#include "util/FrontlightPanelActivity.h"
#include "util/FullScreenMessageActivity.h"
#include "util/SwipeAdjustment.h"
#include "util/TwoFingerSwipe.h"

namespace {
constexpr uint32_t FILE_TRANSFER_MODE_MASK = 0xFF;
constexpr uint32_t FILE_TRANSFER_RETURN_TO_READER = 1U << 8;

constexpr bool hasStickyReaderDetailsPanel() {
#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
  return true;
#else
  return false;
#endif
}

uint32_t fileTransferBootPayload(const NetworkMode mode, const bool returnToReader) {
  return static_cast<uint32_t>(mode) | (returnToReader ? FILE_TRANSFER_RETURN_TO_READER : 0);
}

void restartToFileTransfer(const NetworkMode mode, const std::string& returnBookPath) {
  silentRestartToNetwork(NetworkBootTarget::FILE_TRANSFER, fileTransferBootPayload(mode, !returnBookPath.empty()));
}

std::string fileNameFromPath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool isNearbyTransferFile(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path) || FsHelpers::hasTxtExtension(path);
}

FrontlightPanelContext buildFrontlightPanelContext(Activity& activity, GfxRenderer& renderer,
                                                   MappedInputManager& mappedInput) {
  FrontlightPanelContext context;
  context.sourceActivity = &activity;
  const std::string currentPath = activity.getCurrentBookPath();
  const bool currentBookValid = isNearbyTransferFile(currentPath) && Storage.exists(currentPath.c_str());
  const bool currentEpubValid = FsHelpers::hasEpubExtension(currentPath) && currentBookValid;
  const bool lastValid = !APP_STATE.openEpubPath.empty() && FsHelpers::hasEpubExtension(APP_STATE.openEpubPath) &&
                         Storage.exists(APP_STATE.openEpubPath.c_str());
  context.activeReaderBook = hasFrontlightActiveReaderBook(activity.isReaderActivity(), currentBookValid);
  context.showReadingStatsAction = SETTINGS.shouldTrackReadingStats();
  if (context.activeReaderBook) {
    if (FsHelpers::hasEpubExtension(currentPath)) {
      context.showReadingStatsAction =
          BookStatsTracking::isEnabled(Epub::cachePathForFilePath(currentPath, "/.crosspoint"));
    } else if (FsHelpers::hasXtcExtension(currentPath)) {
      context.showReadingStatsAction = BookStatsTracking::isEnabled(Xtc(currentPath, "/.crosspoint").getCachePath());
    }
    context.bookTitle = activity.getCurrentBookTitle();
    context.bookPath = currentPath;
    context.activeEpub = activity.isEpubReaderActivity() && currentEpubValid;
    if (activity.getFrontlightPanelBookDetails(context.bookDetails)) {
      context.showReaderDetails =
          shouldShowStickyReaderDetails(hasStickyReaderDetailsPanel(), Frontlight.present(), context.activeReaderBook);
      context.bookTitle = context.bookDetails.title;
    }
    if (context.showReadingStatsAction) context.readingStatsActivity = activity.createFrontlightReadingStatsActivity();
    if (context.activeEpub) {
      context.bookPath = currentPath;
      return context;
    }
    if (context.readingStatsActivity) return context;
  }

  const FrontlightBookSource source = chooseFrontlightBookSource(false, false, lastValid && !context.activeReaderBook);

  // Transfer actions use the last EPUB even when reading stats are disabled.
  if (source == FrontlightBookSource::LastBook) {
    context.bookPath = APP_STATE.openEpubPath;
    context.bookTitle = fileNameFromPath(context.bookPath);
  }

  if (!SETTINGS.shouldTrackReadingStats()) return context;
  const GlobalReadingStats global = GlobalReadingStats::load();
  std::string cachePath;
  std::string statsTitle;
  BookReadingStats bookStats;
  float progress = -1.0f;
  if (source == FrontlightBookSource::LastBook) {
    statsTitle = context.bookTitle;
    cachePath = Epub::cachePathForFilePath(context.bookPath, "/.crosspoint");
    if (BookStatsTracking::isBookEnabled(cachePath))
      bookStats = BookReadingStats::load(cachePath);
    else {
      cachePath.clear();
      statsTitle = tr(STR_READING_STATS);
    }
    const RecentBook book{context.bookPath, context.bookTitle, {}, {}};
    progress = RecentBookProgress::loadCachedEpubPercent(book);
  } else {
    statsTitle = tr(STR_READING_STATS);
    if (!context.activeReaderBook) context.bookTitle = statsTitle;
  }
  auto stats =
      GlobalReadingStats::hasSyncedStats()
          ? makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, statsTitle, cachePath, bookStats, progress,
                                                 false, 0, global, GlobalReadingStats::loadAggregated(global))
          : makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, statsTitle, cachePath, bookStats, progress,
                                                 false, 0, global);
  if (stats && !cachePath.empty()) stats->setSyncBookPath(context.bookPath);
  context.readingStatsActivity = std::move(stats);
  return context;
}

bool openFrontlightPanel(Activity& activity, GfxRenderer& renderer, MappedInputManager& mappedInput,
                         const FrontlightDrawerState* restoredState = nullptr) {
  FrontlightPanelContext context = buildFrontlightPanelContext(activity, renderer, mappedInput);
  if (restoredState) context.drawerState = *restoredState;
  auto panel = makeUniqueNoThrow<FrontlightPanelActivity>(renderer, mappedInput, std::move(context));
  if (!panel) {
    LOG_ERR("ACT", "OOM opening frontlight panel");
    return false;
  }
  activity.onFrontlightPanelOpened();
  activity.startActivityForResult(std::move(panel), [&activity](const ActivityResult& result) {
    const auto* panelResult = std::get_if<FrontlightPanelResult>(&result.data);
    if (panelResult) activity.handleFrontlightPanelResult(*panelResult);
  });
  return true;
}

bool applyConfiguredSwipeAction(Activity& activity, ActivityManager& activityManager, const uint8_t action,
                                const int lightAmount = 5, const bool persist = true) {
  switch (static_cast<CrossPointSettings::TWO_FINGER_SWIPE_ACTION>(action)) {
    case CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS:
    case CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_BRIGHTNESS: {
      if (!Frontlight.present()) return true;
      const uint8_t previousBrightness = Frontlight.brightness();
      const bool previousOn = Frontlight.isOn();
      const int delta = action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS ? lightAmount : -lightAmount;
      const uint8_t brightness =
          static_cast<uint8_t>(std::clamp(static_cast<int>(Frontlight.brightness()) + delta, 0, 100));
      Frontlight.setBrightness(brightness);
      Frontlight.setOn(true);
      SETTINGS.frontlightBrightness = brightness;
      SETTINGS.frontlightOn = 1;
      activity.onExternalFrontlightChange();
      if (persist && (brightness != previousBrightness || !previousOn)) activityManager.persistGlobalSettings();
      return true;
    }
    case CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_WARMTH:
    case CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_WARMTH: {
      if (!Frontlight.present() || !Frontlight.hasColorTemperature()) return true;
      const uint8_t previousWarmth = Frontlight.warmth();
      const bool previousOn = Frontlight.isOn();
      const int delta = action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_WARMTH ? lightAmount : -lightAmount;
      const uint8_t warmth = static_cast<uint8_t>(std::clamp(static_cast<int>(Frontlight.warmth()) + delta, 0, 100));
      Frontlight.setWarmth(warmth);
      SETTINGS.frontlightWarmth = warmth;
      SETTINGS.frontlightOn = Frontlight.isOn() ? 1 : 0;
      activity.onExternalFrontlightChange();
      if (persist && (warmth != previousWarmth || Frontlight.isOn() != previousOn))
        activityManager.persistGlobalSettings();
      return true;
    }
    case CrossPointSettings::TWO_FINGER_SWIPE_BACK_HOME:
      return activityManager.handleShortcutAction(CrossPointSettings::BACK_HOME);
    case CrossPointSettings::TWO_FINGER_SWIPE_HOME_READER:
      return activityManager.handleShortcutAction(CrossPointSettings::HOME_READER);
    case CrossPointSettings::TWO_FINGER_SWIPE_TOGGLE_PAGE_TURN_GESTURES:
      return handleGlobalPowerButtonAction(CrossPointSettings::TOGGLE_PAGE_TURN_GESTURES);
    case CrossPointSettings::TWO_FINGER_SWIPE_SELECT_CHAPTER:
    case CrossPointSettings::TWO_FINGER_SWIPE_NEXT_CHAPTER:
    case CrossPointSettings::TWO_FINGER_SWIPE_PREVIOUS_CHAPTER:
    case CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_FONT_SIZE:
    case CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_FONT_SIZE:
      activity.handleTwoFingerSwipeAction(static_cast<CrossPointSettings::TWO_FINGER_SWIPE_ACTION>(action));
      return true;
    case CrossPointSettings::TWO_FINGER_SWIPE_NOT_SET:
    case CrossPointSettings::TWO_FINGER_SWIPE_ACTION_COUNT:
      return false;
  }
  return true;
}

bool isLightSwipeAction(const uint8_t action) {
  return action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS ||
         action == CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_BRIGHTNESS ||
         action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_WARMTH ||
         action == CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_WARMTH;
}

#if CROSSINK_APP_CAP_TOUCH
void finishLiveLightSwipe(LiveLightSwipeState& state, ActivityManager& activityManager) {
  if (state.changed) activityManager.persistGlobalSettings();
  state = {};
}

void updateLiveLightSwipe(Activity& activity, ActivityManager& activityManager, LiveLightSwipeState& state,
                          const int amount) {
  const bool brightness = state.action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS ||
                          state.action == CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_BRIGHTNESS;
  const int sign = state.action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS ||
                           state.action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_WARMTH
                       ? 1
                       : -1;
  const int target = SwipeAdjustment::targetValue(state.initialValue, sign > 0, amount);
  const int current = brightness ? Frontlight.brightness() : Frontlight.warmth();
  const int difference = sign * (target - current);
  if (difference != 0 || (brightness && amount > 0 && !Frontlight.isOn()))
    applyConfiguredSwipeAction(activity, activityManager, state.action, difference, false);
  if (brightness && amount == 0 && Frontlight.isOn() != state.initialOn) {
    Frontlight.setOn(state.initialOn);
    SETTINGS.frontlightOn = state.initialOn ? 1 : 0;
    activity.onExternalFrontlightChange();
  }
  state.changed = (brightness ? Frontlight.brightness() : Frontlight.warmth()) != state.initialValue ||
                  Frontlight.isOn() != state.initialOn;
}

void cancelLiveLightSwipe(Activity& activity, ActivityManager& activityManager, LiveLightSwipeState& state) {
  updateLiveLightSwipe(activity, activityManager, state, 0);
  if (Frontlight.isOn() != state.initialOn) {
    Frontlight.setOn(state.initialOn);
    SETTINGS.frontlightOn = state.initialOn ? 1 : 0;
    activity.onExternalFrontlightChange();
  }
  state.active = false;
  state.blocked = true;
  state.changed = false;
}
#endif

uint8_t actionForTwoFingerDirection(const TwoFingerSwipe::Direction direction) {
  switch (direction) {
    case TwoFingerSwipe::Direction::Up:
      return SETTINGS.twoFingerSwipeUp;
    case TwoFingerSwipe::Direction::Down:
      return SETTINGS.twoFingerSwipeDown;
    case TwoFingerSwipe::Direction::Left:
      return SETTINGS.twoFingerSwipeLeft;
    case TwoFingerSwipe::Direction::Right:
      return SETTINGS.twoFingerSwipeRight;
    case TwoFingerSwipe::Direction::None:
      return CrossPointSettings::TWO_FINGER_SWIPE_NOT_SET;
  }
  return CrossPointSettings::TWO_FINGER_SWIPE_NOT_SET;
}

#if CROSSINK_APP_CAP_TOUCH
bool applyLiveTwoFingerLightSwipe(Activity& activity, MappedInputManager& mappedInput, GfxRenderer& renderer,
                                  ActivityManager& activityManager, LiveLightSwipeState& state,
                                  LiveLightSwipeState& edgeState) {
  int x1 = 0;
  int y1 = 0;
  int x2 = 0;
  int y2 = 0;
  if (!mappedInput.getTwoFingerTouch(x1, y1, x2, y2)) {
    if (!state.tracking) return false;
    const bool consumed = state.active;
    if (consumed) {
      MappedInputManager::CompletedRotation rotation;
      if (mappedInput.wasCompletedMultiTouchRotation(rotation)) {
        cancelLiveLightSwipe(activity, activityManager, state);
        state = {};
        return false;  // Let the SDK's completed rotation keep its priority.
      }
      finishLiveLightSwipe(state, activityManager);
    } else {
      state = {};
    }
    return consumed;
  }

  mappedInput.resetEdgeSlide();
  if (edgeState.active) finishLiveLightSwipe(edgeState, activityManager);
  const int centerX = (x1 + x2) / 2;
  const int centerY = (y1 + y2) / 2;
  const TwoFingerSwipe::FingerPair rawCurrent = {x1, y1, x2, y2};
  if (!state.tracking || state.owner != &activity) {
    state = {};
    state.owner = &activity;
    state.tracking = true;
    state.startX = centerX;
    state.startY = centerY;
    state.firstX = x1;
    state.firstY = y1;
    state.secondX = x2;
    state.secondY = y2;
    return false;
  }

  if (state.blocked) return false;
  const TwoFingerSwipe::FingerPair start = {state.firstX, state.firstY, state.secondX, state.secondY};
  const TwoFingerSwipe::FingerPair current = TwoFingerSwipe::alignedPair(start, rawCurrent);
  if (!TwoFingerSwipe::hasTranslationGeometry(start, current)) {
    if (state.active)
      cancelLiveLightSwipe(activity, activityManager, state);
    else
      state.blocked = true;
    return false;
  }

  if (!state.active) {
    const TwoFingerSwipe::CompletedSwipe swipe = {2, state.startX, state.startY, centerX, centerY, 0};
    const auto direction = TwoFingerSwipe::directionFor(swipe, renderer.getScreenWidth(), renderer.getScreenHeight());
    const uint8_t action = actionForTwoFingerDirection(direction);
    if (!isLightSwipeAction(action) || !TwoFingerSwipe::fingersMovedTogether(start, current, direction)) {
      return false;
    }
    state.active = true;
    state.action = action;
    state.direction = static_cast<int>(direction);
    state.vertical = direction == TwoFingerSwipe::Direction::Up || direction == TwoFingerSwipe::Direction::Down;
    state.movementSign =
        direction == TwoFingerSwipe::Direction::Up || direction == TwoFingerSwipe::Direction::Left ? -1 : 1;
    state.initialOn = Frontlight.isOn();
    state.initialValue = action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS ||
                                 action == CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_BRIGHTNESS
                             ? Frontlight.brightness()
                             : Frontlight.warmth();
  }

  const int displacement = state.movementSign * (state.vertical ? centerY - state.startY : centerX - state.startX);
  const int axisSize = state.vertical ? renderer.getScreenHeight() : renderer.getScreenWidth();
  updateLiveLightSwipe(activity, activityManager, state, SwipeAdjustment::amount(std::max(0, displacement), axisSize));
  return true;
}
#endif

bool applyTwoFingerSwipeAction(Activity& activity, MappedInputManager& mappedInput, const GfxRenderer& renderer,
                               ActivityManager& activityManager) {
  MappedInputManager::CompletedSwipe completed;
  if (!mappedInput.wasCompletedMultiTouchSwipe(completed)) return false;

  const TwoFingerSwipe::CompletedSwipe swipe = {completed.contactCount, completed.startX, completed.startY,
                                                completed.endX,         completed.endY,   completed.durationMs};
  const auto direction = TwoFingerSwipe::directionFor(swipe, renderer.getScreenWidth(), renderer.getScreenHeight());
  const uint8_t action = actionForTwoFingerDirection(direction);
  if (action == CrossPointSettings::TWO_FINGER_SWIPE_NOT_SET) return false;
  const bool vertical = direction == TwoFingerSwipe::Direction::Up || direction == TwoFingerSwipe::Direction::Down;
  const int distance =
      vertical ? std::abs(completed.endY - completed.startY) : std::abs(completed.endX - completed.startX);
  const int axisSize = vertical ? renderer.getScreenHeight() : renderer.getScreenWidth();
  return applyConfiguredSwipeAction(activity, activityManager, action, SwipeAdjustment::amount(distance, axisSize));
}

#if CROSSINK_APP_CAP_TOUCH
bool applyEdgeSlideAction(Activity& activity, MappedInputManager& mappedInput, ActivityManager& activityManager,
                          LiveLightSwipeState& state) {
  MappedInputManager::EdgeSlideProgress progress;
  if (!mappedInput.getEdgeSlideProgress(progress)) return false;
  uint8_t action = CrossPointSettings::TWO_FINGER_SWIPE_NOT_SET;
  switch (progress.direction) {
    case MappedInputManager::EdgeSlide::LeftUp:
      action = SETTINGS.leftEdgeUp;
      break;
    case MappedInputManager::EdgeSlide::LeftDown:
      action = SETTINGS.leftEdgeDown;
      break;
    case MappedInputManager::EdgeSlide::RightUp:
      action = SETTINGS.rightEdgeUp;
      break;
    case MappedInputManager::EdgeSlide::RightDown:
      action = SETTINGS.rightEdgeDown;
      break;
    case MappedInputManager::EdgeSlide::None:
      break;
  }
  if (state.active) {
    // Drifting inward ends the slide at its last applied value.
    if (!progress.leftEdgeBand) {
      const int amount = state.direction == static_cast<int>(progress.direction)
                             ? SwipeAdjustment::amount(progress.distance, mappedInput.getRenderer().getScreenHeight())
                             : 0;
      updateLiveLightSwipe(activity, activityManager, state, amount);
    }
    if (progress.finished) {
      mappedInput.suppressCurrentTouchContact();
      finishLiveLightSwipe(state, activityManager);
    }
    return true;
  }
  if (action == CrossPointSettings::TWO_FINGER_SWIPE_NOT_SET) return false;
  if (isLightSwipeAction(action)) {
    state = {};
    state.owner = &activity;
    state.active = true;
    state.action = action;
    state.direction = static_cast<int>(progress.direction);
    state.initialOn = Frontlight.isOn();
    state.initialValue = action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS ||
                                 action == CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_BRIGHTNESS
                             ? Frontlight.brightness()
                             : Frontlight.warmth();
    updateLiveLightSwipe(activity, activityManager, state,
                         SwipeAdjustment::amount(progress.distance, mappedInput.getRenderer().getScreenHeight()));
    if (progress.finished) {
      mappedInput.suppressCurrentTouchContact();
      finishLiveLightSwipe(state, activityManager);
    }
    return true;
  }
  if (!progress.finished) return false;
  mappedInput.suppressCurrentTouchContact();
  return applyConfiguredSwipeAction(activity, activityManager, action);
}
#endif

bool applyTwoFingerRotation(Activity& activity, MappedInputManager& mappedInput) {
  if (!SETTINGS.twoFingerRotationEnabled) return false;
  MappedInputManager::CompletedRotation completed;
  if (!mappedInput.wasCompletedMultiTouchRotation(completed)) return false;
  // Rotate the content opposite the physical gesture so it feels like the
  // reader is turning the page under the user's fingers.
  return activity.handleTwoFingerRotation(completed.degrees < 0.0f);
}
}  // namespace

void ActivityManager::begin(const uint32_t renderTaskStackBytes) {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess::configure(renderingMutex);
#endif
#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t renderTaskCore = 1;
#else
  constexpr BaseType_t renderTaskCore = 0;
#endif
  xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender", renderTaskStackBytes,
                          this,               // Parameters
                          1,                  // Priority
                          &renderTaskHandle,  // Task handle
                          renderTaskCore  // Keep long renders/cover decodes off CPU 0's idle watchdog when available
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
  LOG_DBG("ACT", "Render task started with %lu-byte stack", static_cast<unsigned long>(renderTaskStackBytes));
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock;
    if (enteringActivity && !entryRenderRequested) {
      requestedUpdate = true;
      continue;
    }
    entryRenderRequested = false;
    TouchRegistry::getInstance().setEnabled(mappedInput.hasTouch());
    TouchRegistry::getInstance().beginFrame();
    if (currentActivity) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      // Apply Night Mode to each activity's normal-polarity frame. SleepActivity
      // preserves it only for Quick Resume and clears it for other sleep screens.
      display.setInverted(SETTINGS.screenInverted != 0);
#if defined(CROSSINK_LANGUAGE_BENCHMARK)
      language_benchmark::beginFrame(currentActivity->name.c_str());
#endif
      mappedInput.setMenuMode(currentActivity->usesMenuButtonOrientation());
      currentActivity->render(std::move(lock));
#if defined(CROSSINK_LANGUAGE_BENCHMARK)
      language_benchmark::endFrame();
#endif
      restoredActivityNeedsRender = false;
    }
    TouchRegistry::getInstance().publish();
    // Notify any task blocked in requestUpdateAndWait() that the render is done.
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(&renderStateMux);
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    taskEXIT_CRITICAL(&renderStateMux);
    if (waiter) {
      xTaskNotify(waiter, 1, eIncrement);
    }
  }
}

void ActivityManager::loop() {
  if (currentActivity && currentActivity->requiresExclusiveStorageLoop()) {
    currentActivity->loop();
    // USB Drive normally restarts the device rather than replacing itself. The
    // pending-action fallthrough keeps the simulator's stub lifecycle usable.
    if (pendingAction == PendingAction::None) {
      if (requestedUpdate.exchange(false) && renderTaskHandle) {
        xTaskNotify(renderTaskHandle, 1, eIncrement);
      }
      return;
    }
  }

#if CROSSINK_APP_CAP_TOUCH
  if (edgeLightSwipe.active && edgeLightSwipe.owner != currentActivity.get())
    finishLiveLightSwipe(edgeLightSwipe, *this);
  if (twoFingerLightSwipe.tracking && twoFingerLightSwipe.owner != currentActivity.get())
    finishLiveLightSwipe(twoFingerLightSwipe, *this);
#endif

  if (currentActivity) {
    if (mappedInput.isMenuMode() != currentActivity->usesMenuButtonOrientation()) {
      RenderLock lock;
      mappedInput.setMenuMode(currentActivity->usesMenuButtonOrientation());
    }
    mappedInput.setPowerAsConfirmInReaderMode(currentActivity->allowPowerAsConfirmInReaderMode());

    if (currentActivity->blocksGlobalInput()) {
#if CROSSINK_APP_CAP_TOUCH
      if (edgeLightSwipe.active) finishLiveLightSwipe(edgeLightSwipe, *this);
      if (twoFingerLightSwipe.tracking) finishLiveLightSwipe(twoFingerLightSwipe, *this);
#endif
      mappedInput.resetEdgeSlide();
      currentActivity->loop();
    } else {
#if CROSSINK_APP_CAP_TOUCH
      if (currentActivity->name != "FrontlightPanel" &&
          applyLiveTwoFingerLightSwipe(*currentActivity, mappedInput, renderer, *this, twoFingerLightSwipe,
                                       edgeLightSwipe)) {
        return;
      }
#endif
      // Completed two-finger gestures are recognized before normal one-finger
      // activity gestures. A rotation has priority over translation in the SDK,
      // so a contact sequence can trigger at most one action here.
      if (applyTwoFingerRotation(*currentActivity, mappedInput)) {
        return;
      }

      // The frontlight panel owns its own sliders.
      if (currentActivity->name != "FrontlightPanel" &&
          applyTwoFingerSwipeAction(*currentActivity, mappedInput, renderer, *this)) {
        return;
      }

#if CROSSINK_APP_CAP_TOUCH
      if (applyEdgeSlideAction(*currentActivity, mappedInput, *this, edgeLightSwipe)) {
        return;
      }
#endif

      // Frontlight quick panel: top-edge down-swipe on drawer-layout boards, except
      // that the open EPUB reader exposes the same action across the whole page.
      // Pushed, so it returns to whatever was underneath — including mid-book.
      const bool lightPanelGesture = currentActivity->usesFullScreenReaderVerticalSwipes()
                                         ? mappedInput.wasReaderLightPanelGesture()
                                         : mappedInput.wasLightPanelGesture();
      if (supportsFrontlightDrawer(mappedInput.hasTouchHardware(), Frontlight.present(),
                                   hasStickyReaderDetailsPanel()) &&
          currentActivity->name != "FrontlightPanel" && currentActivity->allowFrontlightPanelGesture() &&
          lightPanelGesture) {
        openFrontlightPanel(*currentActivity, renderer, mappedInput);
        return;
      }
      // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
      if (!handleGlobalHomeGesture()) {
        currentActivity->loop();
      }
    }
  } else {
    mappedInput.setPowerAsConfirmInReaderMode(false);
  }

  // A deferred full-stack replace waits until its requester has popped and every
  // result handler has settled, then runs as an ordinary Replace.
  while (pendingAction != PendingAction::None || (afterReturnActivity && afterReturnOwnerExited)) {
    if (pendingAction == PendingAction::None) {
      afterReturnOwnerExited = false;
      TouchRegistry::getInstance().clear();
      pendingActivity = std::move(afterReturnActivity);
      pendingAction = PendingAction::Replace;
    }
    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      const bool closedFrontlightPanel = currentActivity->name == "FrontlightPanel";
      ActivityResult pendingResult = std::move(currentActivity->result);
      if (afterReturnActivity && currentActivity.get() == afterReturnOwner) {
        afterReturnOwnerExited = true;
        afterReturnOwner = nullptr;
      }

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        mappedInput.setMenuMode(currentActivity->usesMenuButtonOrientation());
        restoredActivityNeedsRender = true;

        // A file-transfer child may have released UI fonts. Restore them for
        // any parent (including Settings) before its first measurement/redraw.
        filenameFontSystem.ensureLoaded(renderer);

        if (closedFrontlightPanel) currentActivity->onFrontlightPanelClosed();

        // Handle result if necessary
        if (currentActivity->resultHandler) {
          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Continue an explicit Home/Reader unwind through each child using
        // normal cancellation results. This lets every parent restore state.
        if (pendingAction == PendingAction::None) continueHomeReaderUnwind();

        // Queue an update to ensure the popped activity gets re-rendered. A
        // partial-screen overlay first restores the full-screen activity below
        // it now that the result handler has finished reconciling settings.
        if (pendingAction == PendingAction::None) {
          lock.unlock();
          if (currentActivity->requiresFreshBackdrop()) restoreBackdropBehindCurrentOverlay();
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      if (pendingAction == PendingAction::Push && pendingActivity->requiresFreshBackdrop()) {
        if (restoredActivityNeedsRender.load()) {
          const RequestUpdateResult redraw = requestUpdateAndWait();
          if (redraw != RequestUpdateResult::Rendered) {
            LOG_ERR("ACT", "Could not restore source backdrop before opening %s", pendingActivity->name.c_str());
          }
        }
        // A queued render may already have refreshed the source and cleared
        // restoredActivityNeedsRender. Preserve the overlay's paused timing
        // state either way before it becomes current.
        if (currentActivity) currentActivity->onBackdropRenderedForOverlay();
      }
      // Current activity has requested a new activity to be launched
      RenderLock lock;

      if (pendingAction == PendingAction::Replace) {
        pendingHomeReaderTarget = nullptr;
        // A full-stack navigation while the requester is still open (Home
        // gesture, sleep) cancels the deferred one. After the requester has
        // exited (e.g. its pop fell back to Home) the deferred one still runs.
        if (afterReturnActivity && !afterReturnOwnerExited) {
          LOG_INF("ACT", "Dropping deferred %s for %s", afterReturnActivity->name.c_str(),
                  pendingActivity->name.c_str());
          afterReturnActivity.reset();
          afterReturnOwner = nullptr;
          afterReturnOwnerExited = false;
        }
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction == PendingAction::Push) {
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
      }
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);
      enteringActivity = true;

      lock.unlock();  // onEnter may acquire its own lock or synchronously render a loading screen.
      enterCurrentActivity();

      if (pendingAction == PendingAction::None && pendingReaderMenuAction >= 0 &&
          currentActivity->isEpubReaderActivity()) {
        const uint8_t action = static_cast<uint8_t>(pendingReaderMenuAction);
        pendingReaderMenuAction = -1;
        currentActivity->handleExternalReaderMenuAction(action);
      }

      if (pendingAction == PendingAction::None && APP_STATE.pendingOverlayResume.valid()) {
        const PendingOverlayResume resume = APP_STATE.pendingOverlayResume;
        if (resume.returnHomeAfterReaderFlow && currentActivity->isEpubReaderActivity() &&
            (resume.bookPath.empty() || resume.bookPath == currentActivity->getCurrentBookPath())) {
          PendingOverlayResume homeResume = resume;
          homeResume.returnHomeAfterReaderFlow = false;
          APP_STATE.setPendingOverlayResume(std::move(homeResume));
          goHome();
          continue;
        }
        const bool readerReady = resume.origin == PendingOverlayOrigin::Reader &&
                                 currentActivity->isEpubReaderActivity() &&
                                 (resume.bookPath.empty() || resume.bookPath == currentActivity->getCurrentBookPath());
        const bool homeReady = resume.origin == PendingOverlayOrigin::Home && currentActivity->isHomeActivity();
        if (readerReady || homeReady) {
          if (resume.overlay == PendingOverlayType::ReaderDrawer && currentActivity->restorePendingOverlay(resume)) {
            PendingOverlayResume consumed;
            APP_STATE.consumePendingOverlayResume(consumed);
          } else if (resume.overlay == PendingOverlayType::FrontlightDrawer &&
                     supportsFrontlightDrawer(mappedInput.hasTouchHardware(), Frontlight.present(),
                                              hasStickyReaderDetailsPanel())) {
            FrontlightDrawerState restoredState;
            restoredState.selectedAction = static_cast<int8_t>(resume.selectedIndex);
            if (openFrontlightPanel(*currentActivity, renderer, mappedInput, &restoredState)) {
              PendingOverlayResume consumed;
              APP_STATE.consumePendingOverlayResume(consumed);
            }
          }
        }
      }

      // Resume a targeted reader unwind only after onEnter has completed.
      if (pendingAction == PendingAction::None) continueHomeReaderUnwind();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  if (APP_STATE.hasPendingAlert.load(std::memory_order_acquire) && pendingAction == PendingAction::None) {
    APP_STATE.hasPendingAlert.store(false, std::memory_order_relaxed);
    pushActivity(std::make_unique<AlertActivity>(renderer, mappedInput));
  }

  // One-time What's New, only over Home so a wake into a book or a silent reboot into a network screen
  // never shows it. The flag is set when the screen actually enters, so a push that Home's own navigation
  // replaces before it applies is retried on the next visit to Home.
  if (!releaseNotesShown && APP_STATE.releaseNotesSeen < RELEASE_NOTES_REVISION &&
      pendingAction == PendingAction::None && currentActivity && currentActivity->isHomeActivity()) {
    if (auto notes =
            makeUniqueNoThrow<ReleaseNotesActivity>(renderer, mappedInput, ReleaseNotesActivity::Mode::FirstRun)) {
      pushActivity(std::move(notes));
    } else {
      releaseNotesShown = true;  // Don't retry every loop; it shows again next boot.
      LOG_ERR("ACT", "OOM: What's New activity; showing it next boot");
    }
  }

  if (currentActivity && mappedInput.isMenuMode() != currentActivity->usesMenuButtonOrientation()) {
    RenderLock lock;
    mappedInput.setMenuMode(currentActivity->usesMenuButtonOrientation());
  }
  if (requestedUpdate.exchange(false)) {
    // Using direct notification to signal the render task to update
    // Increment counter so multiple rapid calls won't be lost
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  }
}

bool ActivityManager::restoreBackdropBehindCurrentOverlay() {
  if (!currentActivity || !currentActivity->requiresFreshBackdrop() || stackActivities.empty()) return false;

  std::unique_ptr<Activity> overlay;
  {
    RenderLock lock;
    overlay = std::move(currentActivity);
    currentActivity = std::move(stackActivities.back());
    stackActivities.pop_back();
  }

  const RequestUpdateResult redraw = requestUpdateAndWait();
  if (redraw == RequestUpdateResult::Rendered && currentActivity) {
    currentActivity->onBackdropRenderedForOverlay();
  } else {
    LOG_ERR("ACT", "Could not restore backdrop behind %s", overlay->name.c_str());
  }

  {
    RenderLock lock;
    stackActivities.push_back(std::move(currentActivity));
    currentActivity = std::move(overlay);
  }
  return redraw == RequestUpdateResult::Rendered;
}

bool ActivityManager::handleGlobalHomeGesture() {
  if (!currentActivity || pendingAction != PendingAction::None || currentActivity->isHomeActivity() ||
      !currentActivity->allowGlobalHomeGesture() || (!mappedInput.hasTouch() && !mappedInput.hasHomeKey())) {
    return false;
  }

  // Outside the open book, the bottom edge remains a Home shortcut even on
  // boards with a Home key. Keep swipe intent separate from the key's Back
  // behavior, and let drawers/dialogs retain their own dismissal handling.
  if (!currentActivity->isBookReaderActivity() && currentActivity->allowGlobalHomeSwipeGesture() &&
      mappedInput.wasBottomEdgeUpSwipe()) {
    if (!currentActivity->handleHomeGesture()) goHome();
    return true;
  }

  const bool homeGesture = currentActivity->usesFullScreenReaderVerticalSwipes()
                               ? mappedInput.wasReaderHomeGesture()
                               : (currentActivity->allowGlobalHomeSwipeGesture() || mappedInput.hasHomeKey()) &&
                                     mappedInput.wasHomeGesture();
  if (!homeGesture) {
    return false;
  }

  // Touch-only devices use an edge swipe as a Home shortcut. Keep that
  // shortcut separate from the X4 Pro's physical Back/Home key.
  if (!mappedInput.hasHomeKey()) {
    if (!currentActivity->handleHomeGesture()) goHome();
    return true;
  }

  return handleHomeButtonBackOrHome();
}

bool ActivityManager::handleHomeButtonBackOrHome() {
  if (!currentActivity || pendingAction != PendingAction::None || currentActivity->isHomeActivity()) {
    return false;
  }

  if (currentActivity->handleHomeGesture()) {
    return true;
  }

  if (!stackActivities.empty()) {
    ActivityResult result;
    result.isCancelled = true;
    currentActivity->setResult(std::move(result));
    popActivity();
    return true;
  }

  goHome();
  return true;
}

bool ActivityManager::openReaderMenuFromShortcut() {
  return currentActivity && pendingAction == PendingAction::None && currentActivity->openReaderSettingsMenu();
}

bool ActivityManager::handleShortcutAction(const uint8_t action) {
  return currentActivity && pendingAction == PendingAction::None && currentActivity->handleShortcutAction(action);
}

bool ActivityManager::handleQuickLockUnlock(const QuickLockTrigger trigger) {
  return currentActivity && pendingAction == PendingAction::None && currentActivity->handleQuickLockUnlock(trigger);
}

void ActivityManager::notifyInputLockChanged(const bool locked) {
  if (currentActivity) currentActivity->onInputLockChanged(locked);
  for (const auto& activity : stackActivities) {
    activity->onInputLockChanged(locked);
  }
}

void ActivityManager::notifyUserInput() {
  if (currentActivity) currentActivity->onUserInput();
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    TouchRegistry::getInstance().clear();
    currentActivity->onExit();
    currentActivity.reset();
  }
}

void ActivityManager::enterCurrentActivity() {
  {
    RenderLock lock;
    mappedInput.setMenuMode(currentActivity->usesMenuButtonOrientation());
  }
  currentActivity->onEnter();
  RenderLock lock;
  mappedInput.setMenuMode(currentActivity->usesMenuButtonOrientation());
  enteringActivity = false;
  entryRenderRequested = false;
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    TouchRegistry::getInstance().clear();
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    TouchRegistry::getInstance().clear();
    {
      RenderLock lock;
      currentActivity = std::move(newActivity);
      enteringActivity = true;
    }
    enterCurrentActivity();
  }
}

void ActivityManager::goToFileTransfer(std::string returnBookPath) {
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput, std::move(returnBookPath)));
}

bool ActivityManager::goToNearbyBookSend(std::string path, const bool returnToReader) {
  auto activity = makeUniqueNoThrow<NearbyBookTransferActivity>(
      renderer, mappedInput, NearbyBookTransferActivity::Mode::Send, std::move(path), returnToReader);
  if (!activity) {
    LOG_ERR("ACT", "OOM: nearby file sender");
    return false;
  }
  replaceActivity(std::move(activity));
  return true;
}

void ActivityManager::goToNearbyBookReceive() {
  auto activity =
      makeUniqueNoThrow<NearbyBookTransferActivity>(renderer, mappedInput, NearbyBookTransferActivity::Mode::Receive);
  if (!activity) {
    LOG_ERR("ACT", "OOM: nearby file receiver");
    return;
  }
  replaceActivity(std::move(activity));
}

void ActivityManager::goToCalibreWireless(const std::string& returnBookPath) {
  restartToFileTransfer(NetworkMode::CONNECT_CALIBRE, returnBookPath);
}

void ActivityManager::goToJoinNetworkFileTransfer(const std::string& returnBookPath) {
  restartToFileTransfer(NetworkMode::JOIN_NETWORK, returnBookPath);
}

void ActivityManager::goToHotspotFileTransfer(const std::string& returnBookPath) {
  restartToFileTransfer(NetworkMode::CREATE_HOTSPOT, returnBookPath);
}

void ActivityManager::goToUsbDrive() {
#if CROSSINK_APP_CAP_USB_DRIVE
  auto activity = makeUniqueNoThrow<UsbDriveActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("ACT", "OOM: USB Drive activity");
    return;
  }
  replaceActivity(std::move(activity));
#else
  LOG_ERR("ACT", "USB Drive requested in a build without USB Drive capability");
#endif
}

bool ActivityManager::resumeFileTransferFromNetworkBoot(const uint32_t payload) {
  const uint32_t rawMode = payload & FILE_TRANSFER_MODE_MASK;
  if (rawMode > static_cast<uint32_t>(NetworkMode::CREATE_HOTSPOT)) {
    LOG_ERR("ACT", "Invalid file transfer network boot mode: %lu", static_cast<unsigned long>(rawMode));
    return false;
  }

  std::string returnBookPath;
  if ((payload & FILE_TRANSFER_RETURN_TO_READER) != 0) {
    if (!APP_STATE.openEpubPath.empty() && Storage.exists(APP_STATE.openEpubPath.c_str())) {
      returnBookPath = APP_STATE.openEpubPath;
    } else {
      LOG_ERR("ACT", "File transfer cannot return to missing reader path: %s", APP_STATE.openEpubPath.c_str());
    }
  }

  // The activity must outlive this boot function, so allocate its small control object on the heap; web buffers
  // remain owned and released by the activity lifecycle.
  auto activity = makeUniqueNoThrow<CrossPointWebServerActivity>(
      renderer, mappedInput, static_cast<NetworkMode>(rawMode), std::move(returnBookPath), true);
  if (!activity) {
    LOG_ERR("ACT", "OOM: file transfer after minimal boot (free=%u maxAlloc=%u)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }

  replaceActivity(std::move(activity));
  return true;
}

void ActivityManager::goToNearbyStatsSync() {
  if (!SETTINGS.shouldTrackReadingStats()) return;
  replaceActivity(std::make_unique<NearbyStatsSyncActivity>(renderer, mappedInput));
}

void ActivityManager::goToSettings(const bool dismissOnUpSwipe, const bool openLanguageDownloads,
                                   const bool touchscreenRecovery) {
  preferredHomeBookPath.clear();
  returningHomeThroughSettings = false;
  if (currentActivity && currentActivity->isHomeActivity()) {
    preferredHomeBookPath = currentActivity->getCurrentBookPath();
    returningHomeThroughSettings = true;
  } else if (!stackActivities.empty() && stackActivities.back()->isHomeActivity()) {
    preferredHomeBookPath = stackActivities.back()->getCurrentBookPath();
    returningHomeThroughSettings = true;
  }
  auto settings = std::make_unique<SettingsActivity>(renderer, mappedInput, dismissOnUpSwipe);
  if (openLanguageDownloads) settings->openLanguageDownloadsOnEnter();
  if (touchscreenRecovery) settings->enableTouchscreenRecovery();
  replaceActivity(std::move(settings));
}

void ActivityManager::goToFileBrowser(std::string path) {
  replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToLibrary() {
  auto library = makeUniqueNoThrow<LibraryActivity>(renderer, mappedInput);
  if (!library) {
    LOG_ERR("ACT", "Cannot allocate Library activity");
    return;
  }
  replaceActivity(std::move(library));
}

void ActivityManager::goToPlugins() {
  auto plugins = makeUniqueNoThrow<PluginCatalogActivity>(renderer, mappedInput, true);
  if (!plugins) {
    LOG_ERR("ACT", "Cannot allocate Plugins activity");
    return;
  }
  replaceActivity(std::move(plugins));
}

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    goToOpdsServer(0);
  } else {
    replaceActivity(std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true));
  }
}

bool ActivityManager::goToOpdsServer(const uint32_t serverIndex, const bool networkBootReady) {
#ifndef SIMULATOR
  if (!networkBootReady) {
    silentRestartToNetwork(NetworkBootTarget::OPDS, serverIndex);
    return true;
  }
#else
  // The desktop build has no fragmented WiFi heap to clear; keep the preview in-process.
  (void)networkBootReady;
#endif

  OPDS_STORE.loadFromFile();
  const auto* storedServer = OPDS_STORE.getServer(serverIndex);
  if (!storedServer) {
    LOG_ERR("ACT", "OPDS network boot server index out of range: %lu", static_cast<unsigned long>(serverIndex));
    return false;
  }

  OpdsServer server = *storedServer;
  OPDS_STORE.release();
  auto browser = makeUniqueNoThrow<OpdsBookBrowserActivity>(renderer, mappedInput, std::move(server));
  if (!browser) {
    LOG_ERR("ACT", "OOM: OPDS browser after minimal boot (free=%u maxAlloc=%u)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }
  replaceActivity(std::move(browser));
  return true;
}

void ActivityManager::goToReader(std::string path, const bool suppressBackRelease, const bool allowFastInitialRefresh,
                                 const bool cleanImageBaseOnEntry, const bool keepWakeFrame) {
  // OPDS credentials are unrelated to local reading and may contain several
  // heap-backed strings. Home reloads them lazily when it becomes active.
  OPDS_STORE.release();
  replaceActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path), suppressBackRelease,
                                                   allowFastInitialRefresh, cleanImageBaseOnEntry, keepWakeFrame));
}

void ActivityManager::goToReaderAndRunMenuAction(std::string path, const uint8_t action) {
  pendingReaderMenuAction = action;
  goToReader(std::move(path));
}

void ActivityManager::goToSleep(bool fromTimeout) {
  const bool canSnapshotOverlay = currentActivity && currentActivity->canSnapshotForSleepOverlay();
  const GfxRenderer::Orientation sleepPopupOrientation = renderer.getOrientation();
  std::string currentBookPath = getCurrentBookPath();
  auto sleepActivity = makeUniqueNoThrow<SleepActivity>(renderer, mappedInput, canSnapshotOverlay,
                                                        std::move(currentBookPath), fromTimeout, sleepPopupOrientation);
  const bool renderBeforeExit = currentActivity && sleepActivity && sleepActivity->rendersBeforeExit();
  if (!sleepActivity) {
    LOG_ERR("ACT", "Could not allocate sleep activity; saving outgoing activities before sleep");
  }
  if (renderBeforeExit || !sleepActivity) {
    // Keep the outgoing render task from repainting over the sleep screen while
    // onExit() flushes progress, stats and bookmarks to the card.
    RenderLock lock;
    TouchRegistry::getInstance().clear();
    if (sleepActivity) sleepActivity->onEnter();
    exitActivity(lock);
    while (!stackActivities.empty()) {
      stackActivities.back()->onExit();
      stackActivities.pop_back();
    }
    pendingActivity.reset();
    pendingAction = PendingAction::None;
    currentActivity = std::move(sleepActivity);
    return;
  }
  replaceActivity(std::move(sleepActivity));
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

void ActivityManager::goToBoot() { replaceActivity(std::make_unique<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(std::make_unique<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goHome(HomeMenuItem initialMenuItem, const HalDisplay::RefreshMode initialRefreshMode) {
  std::string initialBookPath;
  if (returningHomeThroughSettings) {
    initialBookPath = std::move(preferredHomeBookPath);
  }
  preferredHomeBookPath.clear();
  returningHomeThroughSettings = false;

  if (initialMenuItem == HomeMenuItem::NONE && currentActivity) {
    const auto& activityName = currentActivity->name;
    if (activityName == "FileBrowser") {
      initialMenuItem = HomeMenuItem::FILE_BROWSER;
    } else if (activityName == "PluginCatalog") {
      initialMenuItem = HomeMenuItem::PLUGINS;
    } else if (activityName == "Library") {
      initialMenuItem = HomeMenuItem::LIBRARY;
    } else if (activityName == "OpdsBookBrowser") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
    } else if (activityName == "CrossPointWebServer") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
    } else if (activityName == "NearbyStatsSync") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
    } else if (activityName == "Settings") {
      initialMenuItem = HomeMenuItem::SETTINGS_MENU;
    }
  }
  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem, initialRefreshMode,
                                                 std::move(initialBookPath)));
}
void ActivityManager::goToCrashReport() { replaceActivity(std::make_unique<CrashActivity>(renderer, mappedInput)); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  TouchRegistry::getInstance().clear();
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::replaceAfterReturn(std::unique_ptr<Activity>&& activity) {
  // Main-loop only, like replaceActivity(): no lock.
  afterReturnActivity = std::move(activity);
  afterReturnOwner = currentActivity.get();
  afterReturnOwnerExited = false;
}

void ActivityManager::popActivity() {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  TouchRegistry::getInstance().clear();
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::requiresExclusiveStorageLoop() const {
  return currentActivity && currentActivity->requiresExclusiveStorageLoop();
}

bool ActivityManager::blocksGlobalInput() const { return currentActivity && currentActivity->blocksGlobalInput(); }

bool ActivityManager::isHomeActivity() const { return currentActivity && currentActivity->name == "Home"; }

bool ActivityManager::isReaderActivity() const {
  if (currentActivity && currentActivity->isReaderActivity()) {
    return true;
  }

  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity && activity->isReaderActivity(); });
}

bool ActivityManager::openReaderSettingsForTouchscreenEscapeHatch() {
  if (!mappedInput.hasTouchHardware() || !SETTINGS.disableReaderTouchscreen || !currentActivity ||
      !currentActivity->isReaderActivity() || pendingAction != PendingAction::None) {
    return false;
  }

  if (!currentActivity->openReaderSettingsMenu()) {
    // TXT has no reader menu. Keep its Settings fallback usable while touch
    // is locked without changing the persisted touchscreen preference.
    goToSettings(false, false, true);
  }
  return true;
}

bool ActivityManager::hasActivityNamed(const char* activityName) const {
  const auto matches = [activityName](const auto& activity) { return activity && activity->name == activityName; };
  if (matches(currentActivity) || matches(pendingActivity)) {
    return true;
  }

  return std::any_of(stackActivities.begin(), stackActivities.end(), matches);
}

#ifdef SIMULATOR
bool ActivityManager::isCurrentActivityNamed(const char* activityName) const {
  return currentActivity && currentActivity->name == activityName;
}
#endif

bool ActivityManager::canSnapshotForSleepOverlay() const {
  return currentActivity && currentActivity->canSnapshotForSleepOverlay();
}

bool ActivityManager::requestManualReaderRefresh() {
  RenderLock lock;
  if (!currentActivity || !currentActivity->isReaderActivity() || !currentActivity->prepareManualRefresh()) {
    return false;
  }

  lock.unlock();
  requestUpdate(true);
  return true;
}

bool ActivityManager::handleHomeReaderShortcut() {
  // Consume repeated presses while a pop or replacement is already queued.
  if (pendingAction != PendingAction::None || pendingHomeReaderTarget) return true;
  if (!currentActivity || currentActivity->isHomeActivity()) return true;

  if (currentActivity->isBookReaderActivity()) {
    goHome();
    return true;
  }

  const auto reader = std::find_if(stackActivities.rbegin(), stackActivities.rend(),
                                   [](const auto& activity) { return activity && activity->isBookReaderActivity(); });
  if (reader == stackActivities.rend()) {
    goHome();
    return true;
  }

  pendingHomeReaderTarget = reader->get();
  ActivityResult result;
  result.isCancelled = true;
  currentActivity->setResult(std::move(result));
  popActivity();
  return true;
}

bool ActivityManager::continueHomeReaderUnwind() {
  if (!pendingHomeReaderTarget) return false;
  if (!currentActivity) {
    pendingHomeReaderTarget = nullptr;
    return false;
  }
  if (currentActivity.get() == pendingHomeReaderTarget) {
    pendingHomeReaderTarget = nullptr;
    return false;
  }
  if (pendingAction != PendingAction::None) return false;

  const auto target = std::find_if(stackActivities.begin(), stackActivities.end(),
                                   [this](const auto& activity) { return activity.get() == pendingHomeReaderTarget; });
  if (target == stackActivities.end()) {
    pendingHomeReaderTarget = nullptr;
    return false;
  }

  ActivityResult result;
  result.isCancelled = true;
  currentActivity->setResult(std::move(result));
  popActivity();
  return true;
}

bool ActivityManager::handleShortcutAction(const CrossPointSettings::SHORT_PWRBTN action) {
  if (action == CrossPointSettings::SHORT_PWRBTN::BACK_HOME) {
    handleHomeButtonBackOrHome();
    return true;
  }
  if (action == CrossPointSettings::SHORT_PWRBTN::HOME_READER) return handleHomeReaderShortcut();
  return currentActivity && (currentActivity->isReaderActivity() || currentActivity->isHomeActivity()) &&
         currentActivity->handleShortcutAction(action);
}

Activity* ActivityManager::findEpubReader() const {
  if (currentActivity && currentActivity->isEpubReaderActivity()) return currentActivity.get();
  const auto reader = std::find_if(stackActivities.rbegin(), stackActivities.rend(),
                                   [](const auto& activity) { return activity && activity->isEpubReaderActivity(); });
  return reader != stackActivities.rend() ? reader->get() : nullptr;
}

void ActivityManager::persistGlobalSettings() {
  // A modal may be current while its EPUB reader, which owns the per-book
  // override, remains on the stack. Let that reader protect the global write.
  if (auto* reader = findEpubReader()) {
    reader->persistGlobalSettings();
  } else if (currentActivity) {
    currentActivity->persistGlobalSettings();
  } else {
    SETTINGS.saveToFile();
  }
}

bool ActivityManager::beginGlobalSettingsEdit() {
  auto* reader = findEpubReader();
  return reader && reader->onFrontlightGlobalSettingsOpened();
}

void ActivityManager::endGlobalSettingsEdit() {
  if (auto* reader = findEpubReader()) reader->onFrontlightGlobalSettingsClosed();
}

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

uint8_t ActivityManager::inputPollDelayMs() const { return currentActivity ? currentActivity->inputPollDelayMs() : 10; }

std::string ActivityManager::getCurrentBookPath() const {
  if (currentActivity) {
    const std::string path = currentActivity->getCurrentBookPath();
    if (!path.empty()) {
      return path;
    }
  }

  for (auto it = stackActivities.rbegin(); it != stackActivities.rend(); ++it) {
    if (*it) {
      const std::string path = (*it)->getCurrentBookPath();
      if (!path.empty()) {
        return path;
      }
    }
  }

  return {};
}

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    const ScreenshotInfo info = currentActivity->getScreenshotInfo();
    if (info.readerType != ScreenshotInfo::ReaderType::None) {
      return info;
    }
  }

  // Reader overlays such as the dictionary are pushed above the book activity.
  // Keep their visible framebuffer, but inherit the book's screenshot filename.
  for (auto it = stackActivities.rbegin(); it != stackActivities.rend(); ++it) {
    if (*it) {
      const ScreenshotInfo info = (*it)->getScreenshotInfo();
      if (info.readerType != ScreenshotInfo::ReaderType::None) {
        return info;
      }
    }
  }

  return {};
}

void ActivityManager::requestUpdate(bool immediate) {
  if (immediate) {
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    requestedUpdate = true;
  }
}
RequestUpdateResult ActivityManager::requestUpdateAndWait() {
  if (!renderTaskHandle) {
    return RequestUpdateResult::Rejected;
  }

  // Atomic section to perform checks
  taskENTER_CRITICAL(&renderStateMux);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(&renderStateMux);

  if (isRenderTask) {
    LOG_ERR("ACT", "requestUpdateAndWait() called from render task; rejecting sync update");
    return RequestUpdateResult::Rejected;
  }

  if (alreadyWaiting) {
    LOG_ERR("ACT", "requestUpdateAndWait() called while another task is waiting; rejecting sync update");
    return RequestUpdateResult::Rejected;
  }

  // Cannot call while holding RenderLock or it will cause a deadlock
  if (holdingRenderLock) {
    LOG_ERR("ACT", "requestUpdateAndWait() called while holding RenderLock; rejecting sync update");
    return RequestUpdateResult::Rejected;
  }

  {
    RenderLock lock;
    // onEnter explicitly promises that its state is ready for this one render.
    entryRenderRequested = enteringActivity;
  }
  xTaskNotify(renderTaskHandle, 1, eIncrement);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  return RequestUpdateResult::Rendered;
}

// RenderLock

RenderLock::RenderLock(const Mode mode) {
  isLocked = xSemaphoreTake(activityManager.renderingMutex, mode == Mode::Try ? 0 : portMAX_DELAY) == pdTRUE;
  assert(mode == Mode::Try || isLocked);
}

RenderLock::RenderLock([[maybe_unused]] Activity&, const Mode mode) : RenderLock(mode) {}

RenderLock::~RenderLock() { unlock(); }

void RenderLock::unlock() {
  if (isLocked) {
#if FREEINK_DEVICE_PICCO
    // A drawing transaction includes all async/grayscale passes. Shut down
    // only when its owner releases the lock, including direct toast, screenshot
    // and Quick Lock updates that bypass the activity render task.
    display.powerOffBetweenFrames();
#endif
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 * Checks if renderingMutex is held by any task, including the calling task.
 *
 * @return true if renderingMutex has an owner (any task), false otherwise.
 *
 * @note Must not be called from ISR context — xSemaphoreGetMutexHolder is not ISR-safe.
 */
bool RenderLock::peek() { return xSemaphoreGetMutexHolder(activityManager.renderingMutex) != nullptr; }

bool RenderLock::heldByCurrentTask() {
  return xSemaphoreGetMutexHolder(activityManager.renderingMutex) == xTaskGetCurrentTaskHandle();
}

void ActivityManager::preparePluginSleep() {
  RenderLock lock;
  for (auto& activity : stackActivities) activity->prepareForSleep();
  if (currentActivity) currentActivity->prepareForSleep();
}
