#include "TxtReaderActivity.h"

#include <BidiUtils.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <Serialization.h>
#include <Utf8.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GlobalActions.h"
#include "MappedInputManager.h"
#include "QuickActions.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/boot_sleep/SleepCoverAssets.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t CHUNK_SIZE = 8 * 1024;  // 8KB chunk for reading
constexpr unsigned long LONG_PRESS_MENU_MS = 600;
// Cache file magic and version
constexpr uint32_t CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t CACHE_VERSION = 5;          // Increment when cache format changes
constexpr uint32_t MAX_CACHE_PAGES = 65535;   // Sanity cap to prevent unbounded reserve()

// Parses and word-wraps lines from a file chunk into outLines.
// Returns the number of bytes consumed from the start of buffer.
size_t parseAndWrapLines(const uint8_t* buffer, size_t chunkSize, size_t fileOffset, size_t fileSize, int linesPerPage,
                         GfxRenderer& renderer, int fontId, int vw, std::vector<std::string>& outLines,
                         int8_t characterSpacing) {
  size_t pos = 0;
  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    size_t lineEnd = pos;
    while (lineEnd < chunkSize && buffer[lineEnd] != '\n') lineEnd++;
    bool lineComplete = (lineEnd < chunkSize) || (fileOffset + lineEnd >= fileSize);
    if (!lineComplete && !outLines.empty()) break;

    size_t lineContentLen = lineEnd - pos;
    bool hasCR = (lineContentLen > 0 && buffer[pos + lineContentLen - 1] == '\r');
    size_t displayLen = hasCR ? lineContentLen - 1 : lineContentLen;
    std::string line(reinterpret_cast<const char*>(buffer + pos), displayLen);
    size_t lineBytePos = 0;

    do {
      if (line.empty()) {
        outLines.emplace_back();
        break;
      }

      if (renderer.getTextWidth(fontId, line.c_str(), EpdFontFamily::REGULAR, BidiUtils::BidiBaseDir::AUTO,
                                characterSpacing) <= vw) {
        outLines.push_back(line);
        lineBytePos = displayLen;
        line.clear();
        break;
      }
      size_t breakPos = line.length();
      while (breakPos > 0 && renderer.getTextWidth(fontId, line.substr(0, breakPos).c_str(), EpdFontFamily::REGULAR,
                                                   BidiUtils::BidiBaseDir::AUTO, characterSpacing) > vw) {
        size_t spacePos = line.rfind(' ', breakPos - 1);
        if (spacePos != std::string::npos && spacePos > 0) {
          breakPos = spacePos;
        } else {
          breakPos--;
          while (breakPos > 0 && (line[breakPos] & 0xC0) == 0x80) breakPos--;
        }
      }
      if (breakPos == 0) {
        breakPos = 1;
        while (breakPos < line.length() && (line[breakPos] & 0xC0) == 0x80) breakPos++;
      }
      outLines.push_back(line.substr(0, breakPos));
      size_t skipChars = breakPos;
      if (breakPos < line.length() && line[breakPos] == ' ') skipChars++;
      lineBytePos += skipChars;
      line = line.substr(skipChars);
    } while (!line.empty() && static_cast<int>(outLines.size()) < linesPerPage);

    if (line.empty()) {
      pos = lineEnd + 1;
    } else {
      pos = pos + lineBytePos;
      break;
    }
  }
  if (pos == 0 && !outLines.empty()) {
    pos = 1;
  }
  return pos;
}

int getReaderLineHeight(const GfxRenderer& renderer, const int fontId) {
  return std::max(1, static_cast<int>(renderer.getLineHeight(fontId) * SETTINGS.getReaderLineCompression() + 0.5f));
}

void drawToast(const GfxRenderer& renderer, const char* msg) {
  constexpr int toastPadX = 20;
  constexpr int toastPadY = 12;
  const bool toastBackgroundBlack = ReaderUtils::readerForegroundBlack();
  const int msgW = renderer.getTextWidth(UI_10_FONT_ID, msg);
  const int msgH = renderer.getLineHeight(UI_10_FONT_ID);
  const int toastW = msgW + toastPadX * 2;
  const int toastH = msgH + toastPadY * 2;
  const int toastX = (renderer.getScreenWidth() - toastW) / 2;
  const int toastY = (renderer.getScreenHeight() - toastH) / 2;
  renderer.fillRect(toastX, toastY, toastW, toastH, toastBackgroundBlack);
  renderer.drawRect(toastX, toastY, toastW, toastH, !toastBackgroundBlack);
  renderer.drawText(UI_10_FONT_ID, toastX + toastPadX, toastY + toastPadY, msg, !toastBackgroundBlack);
  renderer.displayBuffer();
}
}  // namespace

void TxtReaderActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  showOpeningFeedback();

  if (!txt) {
    return;
  }

  sdFontSystem.ensureLoaded(renderer);
  showOpeningFeedback();

  // Activate reader-specific front button mapping (if configured).
  mappedInput.setReaderMode(true);

  txt->setupCacheDir();

  // Save current txt as last opened file and add to recent books
  auto filePath = txt->getPath();
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  showOpeningFeedback();
  SleepCoverAssets::prepareTxt(*txt);
  const std::string coverBmpPath = Storage.exists(txt->getCoverBmpPath().c_str()) ? txt->getCoverBmpPath() : "";
  if (!skipRecentBookUpdateOnEntry) {
    RECENT_BOOKS.addOrUpdateBook(filePath, fileName, "", coverBmpPath);
  }

  showOpeningFeedback();
  // Trigger first update
  requestUpdate();
}

void TxtReaderActivity::onExit() {
  mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();
  if (auto* fontCache = renderer.getFontCacheManager()) {
    fontCache->releaseSdFontCaches();
  }

  // Deactivate reader-specific front button mapping.
  mappedInput.setReaderMode(false);

  if (!flushQueuedProgress()) {
    LOG_ERR("TRS", "Failed to flush debounced reader progress on exit");
  }
  pluginEvents.exit(getCurrentBookPath(), getScreenshotInfo().progressPercent);

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  pageOffsets.clear();
  currentPageLines.clear();
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  txt.reset();
}

bool TxtReaderActivity::handleFrontlightPanelResult(const FrontlightPanelResult& result) {
  if (result.action != FrontlightPanelAction::SendNearbyBook || !txt) return false;
  saveProgress(currentPage);
  return activityManager.goToNearbyBookSend(txt->getPath(), true);
}

void TxtReaderActivity::loop() {
  pluginEvents.pollWhenIdle(getCurrentBookPath());
  if (quickActionsPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
#if CROSSINK_APP_CAP_TOUCH
  // TXT has no reader drawer: the bottom edge exits directly on every touch device.
  if (mappedInput.wasBottomEdgeUpSwipe()) {
    onGoHome();
    return;
  }
  if (handlePinchFontResize()) return;
#endif
  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);
  if (touch.tapped &&
      (ReaderUtils::isBottomStatusBarTap(renderer, touch.y, UITheme::getInstance().getStatusBarHeight(renderer)) ||
       ReaderUtils::isTopStatusBarTap(renderer, touch.y,
                                      UITheme::getReaderStatusBarHeight(ReaderStatusBarPosition::Top, renderer)))) {
    if (SETTINGS.tapToHideStatusBar) {
      statusBarVisible = !statusBarVisible;
      requestUpdate();
    }
    return;
  }
  if (consumeLongPowerButtonRelease()) {
    return;
  }
  if (executePowerButtonAction()) {
    return;
  }

  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (longPressMenuHandled) {
    if (confirmReleased || !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      longPressMenuHandled = false;
    }
    return;
  }

  if (SETTINGS.longPressMenuAction == CrossPointSettings::LONG_MENU_ROTATE_FLIP &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MENU_MS) {
    longPressMenuHandled = true;
    applyReaderOrientation(ReaderUtils::flippedOrientation(SETTINGS.orientation));
    return;
  }

  if (SETTINGS.longPressMenuAction == CrossPointSettings::LONG_MENU_CHANGE_FONT &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= LONG_PRESS_MENU_MS) {
    longPressMenuHandled = true;
    cycleReaderFont();
    return;
  }

  if ((SETTINGS.longPressMenuAction == CrossPointSettings::LONG_MENU_LIBRARY ||
       ReaderUtils::isNavigationLongPressAction(SETTINGS.longPressMenuAction)) &&
      mappedInput.getHeldTime() >= LONG_PRESS_MENU_MS &&
      (mappedInput.isPressed(MappedInputManager::Button::Confirm) || confirmReleased)) {
    longPressMenuHandled = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    mappedInput.suppressNextConfirmRelease();
    if (!ReaderUtils::dispatchNavigationLongPressAction(SETTINGS.longPressMenuAction)) activityManager.goToLibrary();
    return;
  }

  if (longPressBackHandled) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        !mappedInput.isPressed(MappedInputManager::Button::Back)) {
      longPressBackHandled = false;
    }
    return;
  }

  if (!longPressBackHandled && mappedInput.isPressed(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    longPressBackHandled = true;
    mappedInput.suppressNextBackRelease();
    executeLongPressBackAction();
    return;
  }

  // Short press BACK goes directly to home
  if (!touch.prev && !touch.next && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  const auto side = readerButtonShortcuts.update(mappedInput, mappedInput.buttonSampleTime());
  if (ReaderButtonShortcuts::shouldConsume(side, mappedInput)) {
    if (side.triggered) {
      if (side.longPress) {
        if (side.front)
          mappedInput.suppressNextFrontRelease(side.up ? MappedInputManager::Button::Left
                                                       : MappedInputManager::Button::Right);
        else
          mappedInput.suppressNextSideRelease(side.up ? MappedInputManager::Button::Up
                                                      : MappedInputManager::Button::Down);
      }
      switch (side.action) {
        case CrossPointSettings::PAGE_TURN:
          requestPageTurn(false, true);
          break;
        case CrossPointSettings::SIDE_INCREASE_FONT:
        case CrossPointSettings::SIDE_DECREASE_FONT:
          changeReaderFontSize(side.action == CrossPointSettings::SIDE_INCREASE_FONT);
          break;
        case CrossPointSettings::SIDE_ROTATE_COUNTERCLOCKWISE:
        case CrossPointSettings::SIDE_ROTATE_CLOCKWISE:
          handleTwoFingerRotation(side.action == CrossPointSettings::SIDE_ROTATE_CLOCKWISE);
          break;
        case CrossPointSettings::SIDE_ROTATE_FLIP:
          applyReaderOrientation(ReaderUtils::flippedOrientation(SETTINGS.orientation));
          break;
        case CrossPointSettings::SIDE_PREVIOUS_CHAPTER:
        case CrossPointSettings::SIDE_NEXT_CHAPTER:
        case CrossPointSettings::IGNORE:
          // Plain text has no chapter model.
          break;
        default: {
          const auto action = static_cast<CrossPointSettings::SHORT_PWRBTN>(side.action);
          if (action == CrossPointSettings::QUICK_LOCK)
            handleGlobalPowerButtonAction(action, ReaderButtonShortcuts::quickLockTrigger(side));
          else if (!handleShortcutAction(action))
            handleGlobalPowerButtonAction(action);
          break;
        }
      }
    }
    return;
  }

  auto [prevTriggered, nextTriggered, fromSideBtn, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  (void)fromSideBtn;
  (void)fromTilt;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  requestPageTurn(prevTriggered, nextTriggered);
}

void TxtReaderActivity::requestPageTurn(const bool previous, const bool next) {
  {
    // Indexing publishes the page count on the render task. Retain this turn
    // until that work finishes, then check bounds while holding its lock.
    RenderLock lock(*this);
    if (previous && currentPage > 0) {
      --currentPage;
      pluginEvents.noteTurn(false);
    } else if (next && currentPage < totalPages - 1) {
      ++currentPage;
      pluginEvents.noteTurn(true);
    } else {
      return;
    }
  }
  requestUpdate();
}

bool TxtReaderActivity::changeReaderFontSize(const bool larger, const FontSizeStepMode mode) {
  RenderLock lock(*this);
  if (!sdFontSystem.changeReaderFontSize(larger, mode)) return false;
  rebuildTextLayout();
  return true;
}

void TxtReaderActivity::cycleReaderFont() {
  RenderLock lock(*this);
  const CrossPointSettings::FONT_SIZE effectiveSize = SETTINGS.getEffectiveReaderFontSize();
  SETTINGS.fontFamily = (SETTINGS.fontFamily + 1) % CrossPointSettings::FONT_FAMILY_COUNT;
  SETTINGS.sdFontFamilyName[0] = '\0';
  SETTINGS.readerFontPointSize = CrossPointSettings::getReaderFontPointSize(effectiveSize);
  rebuildTextLayout();
}

void TxtReaderActivity::rebuildTextLayout() {
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("TXT", "Failed to save reader font setting");
  }
  sdFontSystem.ensureLoaded(renderer);
  pageOffsets.clear();
  currentPageLines.clear();
  statusBarRelayoutOffset.reset();
  statusBarRelayoutPage = -1;
  initialized = false;
  requestUpdate();
}

#if CROSSINK_APP_CAP_TOUCH
bool TxtReaderActivity::handlePinchFontResize() {
  if (!SETTINGS.pinchFontResizeEnabled || !mappedInput.supportsMultiTouch()) {
    resetPinchFontGesture();
    return false;
  }

  int x1 = 0;
  int y1 = 0;
  int x2 = 0;
  int y2 = 0;
  if (!mappedInput.getTwoFingerTouch(x1, y1, x2, y2)) {
    resetPinchFontGesture();
    return false;
  }

  const auto action = pinchFontGesture.update(x1, y1, x2, y2);
  if (action == ReaderPinchGesture::Action::None) return true;

  mappedInput.suppressCurrentTouchContact();
  changeReaderFontSize(action == ReaderPinchGesture::Action::Increase, FontSizeStepMode::Clamp);
  return true;
}

void TxtReaderActivity::resetPinchFontGesture() { pinchFontGesture.reset(); }
#endif

bool TxtReaderActivity::handleTwoFingerSwipeAction(const CrossPointSettings::TWO_FINGER_SWIPE_ACTION action) {
  if (action != CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_FONT_SIZE &&
      action != CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_FONT_SIZE) {
    // TXT has no chapter model; ActivityManager still consumes configured
    // chapter swipes so they cannot fall through as one-finger navigation.
    return true;
  }

  changeReaderFontSize(action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_FONT_SIZE, FontSizeStepMode::Clamp);
  return true;
}

bool TxtReaderActivity::handleTwoFingerRotation(const bool clockwise) {
  return applyReaderOrientation(ReaderUtils::rotatedOrientation(SETTINGS.orientation, clockwise));
}

bool TxtReaderActivity::applyReaderOrientation(const uint8_t orientation) {
  {
    RenderLock lock(*this);
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    pageOffsets.clear();
    currentPageLines.clear();
    initialized = false;
  }
  requestUpdate();
  return true;
}

void TxtReaderActivity::toggleDarkMode() {
  SETTINGS.screenInverted = !SETTINGS.screenInverted;
  SETTINGS.saveToFile();
  requestUpdate();
}

bool TxtReaderActivity::consumeLongPowerButtonRelease() {
  if (!longPowerButtonHandled) {
    return false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Power) ||
      !mappedInput.isPressed(MappedInputManager::Button::Power)) {
    longPowerButtonHandled = false;
    return true;
  }

  return false;
}

bool TxtReaderActivity::consumeLongPowerButtonHold() {
  if (longPowerButtonHandled || !mappedInput.isPressed(MappedInputManager::Button::Power) ||
      mappedInput.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()) {
    return false;
  }

  longPowerButtonHandled = true;
  return true;
}

bool TxtReaderActivity::supportsQuickAction(const CrossPointSettings::SHORT_PWRBTN action) {
  switch (action) {
    case CrossPointSettings::SHORT_PWRBTN::ROTATE_FLIP:
    case CrossPointSettings::SHORT_PWRBTN::PREVIOUS_PAGE:
    case CrossPointSettings::SHORT_PWRBTN::SLEEP:
    case CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH:
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_DARK_MODE:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FONT:
    case CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER:
    case CrossPointSettings::SHORT_PWRBTN::LIBRARY:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FRONTLIGHT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_PAGE_TURN_GESTURES:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TOUCHSCREEN:
    case CrossPointSettings::SHORT_PWRBTN::BACK_HOME:
    case CrossPointSettings::SHORT_PWRBTN::HOME_READER:
      return true;
    default:
      return false;
  }
}

bool TxtReaderActivity::executeReaderShortcutAction(const CrossPointSettings::SHORT_PWRBTN action) {
  switch (action) {
    case CrossPointSettings::SHORT_PWRBTN::ROTATE_FLIP:
      return applyReaderOrientation(ReaderUtils::flippedOrientation(SETTINGS.orientation));
    case CrossPointSettings::SHORT_PWRBTN::PREVIOUS_PAGE:
      requestPageTurn(true, false);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FONT:
      cycleReaderFont();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
      activityManager.goToFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
      saveProgressBeforeRestart();
      activityManager.goToCalibreWireless(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
      saveProgressBeforeRestart();
      activityManager.goToJoinNetworkFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
      saveProgressBeforeRestart();
      activityManager.goToHotspotFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_DARK_MODE:
      toggleDarkMode();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FILE_BROWSER:
      activityManager.goToFileBrowser(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::SHORT_PWRBTN::LIBRARY:
      activityManager.goToLibrary();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_HOME_BUTTON_IN_READER:
      toggleHomeButtonInReader();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FRONTLIGHT:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_PAGE_TURN_GESTURES:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TOUCHSCREEN:
      return handleGlobalPowerButtonAction(action);
    default:
      return false;
  }
}

bool TxtReaderActivity::executePowerButtonAction() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      mappedInput.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()) {
    return executeReaderShortcutAction(static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn));
  }

  const auto longPowerAction = static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn);
  if (longPowerAction == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN || !consumeLongPowerButtonHold()) {
    return false;
  }

  if (executeReaderShortcutAction(longPowerAction)) {
    // Reader long-press actions execute while Power is still held. Consume its
    // later release so the app-wide shortcut dispatcher cannot run it again.
    mappedInput.suppressNextPowerRelease();
    return true;
  }

  return false;
}

void TxtReaderActivity::toggleHomeButtonInReader() {
  if (!mappedInput.hasHomeKey()) return;
  SETTINGS.homeButtonInReaderEnabled = SETTINGS.homeButtonInReaderEnabled ? 0 : 1;
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("TXT", "Failed to save Home button reader setting");
  }
  mappedInput.clearDeferredHomeGesture();
  drawToast(renderer, SETTINGS.homeButtonInReaderEnabled ? tr(STR_HOME_BUTTON_ENABLED) : tr(STR_HOME_BUTTON_DISABLED));
  delay(1000);
  requestUpdate();
}

bool TxtReaderActivity::executeLongPressBackAction() {
  if (ReaderUtils::dispatchNavigationLongPressAction(SETTINGS.longPressBackAction)) return true;
  switch (static_cast<CrossPointSettings::LONG_PRESS_MENU_ACTION>(SETTINGS.longPressBackAction)) {
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_ROTATE_FLIP:
      return applyReaderOrientation(ReaderUtils::flippedOrientation(SETTINGS.orientation));
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_SLEEP:
      enterDeepSleep();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_CHANGE_FONT:
      cycleReaderFont();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_REFRESH_SCREEN:
      prepareManualRefresh();
      requestUpdate();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_FILE_TRANSFER:
      activityManager.goToFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_CALIBRE_WIRELESS:
      saveProgressBeforeRestart();
      activityManager.goToCalibreWireless(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_JOIN_NETWORK:
      saveProgressBeforeRestart();
      activityManager.goToJoinNetworkFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_CREATE_HOTSPOT:
      saveProgressBeforeRestart();
      activityManager.goToHotspotFileTransfer(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_TOGGLE_DARK_MODE:
      toggleDarkMode();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_FILE_BROWSER:
      activityManager.goToFileBrowser(txt ? txt->getPath() : "");
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_LIBRARY:
      activityManager.goToLibrary();
      return true;
    case CrossPointSettings::LONG_PRESS_MENU_ACTION::LONG_MENU_CREATE_CLIPPING:
      return false;
    default:
      return false;
  }
}

bool TxtReaderActivity::handleShortcutAction(const uint8_t action) {
  return executeReaderShortcutAction(static_cast<CrossPointSettings::SHORT_PWRBTN>(action));
}

bool TxtReaderActivity::handleShortcutAction(const CrossPointSettings::SHORT_PWRBTN action) {
  if (action == CrossPointSettings::SHORT_PWRBTN::HOME_READER || action == CrossPointSettings::SHORT_PWRBTN::BACK_HOME)
    return dispatchShortcutAction(action);
  if (action == CrossPointSettings::SHORT_PWRBTN::QUICK_ACTIONS) {
    QuickActions::showConfiguredPopup(
        quickActionsPopup, [this] { requestUpdate(); },
        [this](const auto quickAction) {
          mappedInput.setReaderTouchscreenOverride(false);
          dispatchShortcutAction(quickAction);
        },
        [](const auto quickAction) { return supportsQuickAction(quickAction); });
    if (quickActionsPopup.isActive()) {
      mappedInput.setReaderTouchscreenOverride(true);
      quickActionsPopup.setCancelCallback([this] { mappedInput.setReaderTouchscreenOverride(false); });
    }
    return true;
  }
  return executeReaderShortcutAction(action);
}

void TxtReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  // Store current settings for cache validation
  cachedFontId = SETTINGS.getReaderFontId();
  cachedCharacterSpacing = SETTINGS.characterSpacingLevel(SETTINGS.characterSpacing);
  cachedVerticalMargin = SETTINGS.screenMarginVertical;
  cachedHorizontalMargin = SETTINGS.screenMarginHorizontal;
  cachedParagraphAlignment = SETTINGS.paragraphAlignment;
  cachedTopStatusBarHeight = ReaderUtils::getTopStatusBarReservedHeight(renderer);
  cachedFooterReservedHeight = ReaderUtils::getReaderFooterReservedHeight(renderer, false);
  cachedViewableInsets = renderer.getViewableInsets();

  // Calculate viewport dimensions
  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginLeft += cachedHorizontalMargin;
  cachedOrientedMarginRight += cachedHorizontalMargin;
  const int topStatusBarReservedHeight = cachedTopStatusBarHeight;
  if (topStatusBarReservedHeight > 0) {
    cachedOrientedMarginTop += std::max(static_cast<int>(cachedVerticalMargin),
                                        topStatusBarReservedHeight + ReaderUtils::TOP_STATUS_BAR_TEXT_PADDING);
  } else {
    cachedOrientedMarginTop += cachedVerticalMargin;
  }
  cachedOrientedMarginBottom += cachedFooterReservedHeight;

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  const int viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  const int lineHeight = getReaderLineHeight(renderer, cachedFontId);

  linesPerPage = viewportHeight / lineHeight;
  if (linesPerPage < 1) linesPerPage = 1;

  // Try to load cached page index first
  if (!loadPageIndexCache()) {
    // Cache not found, build page index
    buildPageIndex();
    // Save to cache for next time
    savePageIndexCache();
  }

  // Load saved progress
  loadProgress();

  initialized = true;
}

void TxtReaderActivity::buildPageIndex() {
  pageOffsets.clear();
  pageOffsets.push_back(0);  // First page starts at offset 0

  size_t offset = 0;
  const size_t fileSize = txt->getFileSize();

  openingFeedback.showIndexing(renderer);

  // At most 8193 bytes, owned only by this indexing call: too large for the
  // task stack, and no permanent C3 RAM cost after indexing finishes.
  auto scratch = makeUniqueNoThrow<uint8_t[]>(std::min(CHUNK_SIZE, fileSize) + 1);
  if (!scratch) {
    LOG_ERR("TRS", "Failed to allocate TXT indexing scratch");
    totalPages = pageOffsets.size();
    return;
  }
  while (offset < fileSize) {
    std::vector<std::string> tempLines;
    size_t nextOffset = offset;

    if (!loadPageAtOffset(offset, tempLines, nextOffset, scratch.get())) {
      break;
    }

    if (nextOffset <= offset) {
      // No progress made, avoid infinite loop
      break;
    }

    offset = nextOffset;
    if (offset < fileSize) {
      pageOffsets.push_back(offset);
    }

    // Yield to other tasks periodically
    if (pageOffsets.size() % 20 == 0) {
      vTaskDelay(1);
    }
  }

  totalPages = pageOffsets.size();
}

bool TxtReaderActivity::loadPageAtOffset(size_t offset, std::vector<std::string>& outLines, size_t& nextOffset,
                                         uint8_t* scratch) {
  outLines.clear();
  const size_t fileSize = txt->getFileSize();

  if (offset >= fileSize) {
    return false;
  }

  // Read a chunk from file
  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto ownedBuffer = scratch ? std::unique_ptr<uint8_t[]>{} : makeUniqueNoThrow<uint8_t[]>(chunkSize + 1);
  uint8_t* buffer = scratch ? scratch : ownedBuffer.get();
  if (!buffer) {
    LOG_ERR("TRS", "Failed to allocate %zu bytes", chunkSize);
    return false;
  }

  if (!txt->readContent(buffer, offset, chunkSize)) {
    return false;
  }
  buffer[chunkSize] = '\0';

  // Prime the SD card font's advance table before the wrap helper starts
  // measuring strings. This avoids on-demand SD glyph lookups for every width
  // check while preserving the shared parseAndWrapLines() implementation.
  if (renderer.isSdCardFont(cachedFontId)) {
    renderer.ensureSdCardFontReady(cachedFontId, reinterpret_cast<const char*>(buffer), /*styleMask=*/0x01);
  }

  size_t pos = parseAndWrapLines(buffer, chunkSize, offset, fileSize, linesPerPage, renderer, cachedFontId,
                                 viewportWidth, outLines, SETTINGS.characterSpacingLevel(SETTINGS.characterSpacing));
  nextOffset = offset + pos;
  if (nextOffset > fileSize) {
    nextOffset = fileSize;
  }

  return !outLines.empty();
}

void TxtReaderActivity::render(RenderLock&&) {
  showOpeningFeedback();
  if (!txt) {
    return;
  }
  if (quickActionsPopup.processRender(renderer, mappedInput)) {
    return;
  }

  bool relayout = false;
  size_t readingOffset = 0;
  if (initialized && currentPage != statusBarRelayoutPage) statusBarRelayoutOffset.reset();
  if (initialized && (cachedViewableInsets != renderer.getViewableInsets() ||
                      cachedTopStatusBarHeight != ReaderUtils::getTopStatusBarReservedHeight(renderer) ||
                      cachedFooterReservedHeight != ReaderUtils::getReaderFooterReservedHeight(renderer, false))) {
    if (currentPage >= 0 && currentPage < static_cast<int>(pageOffsets.size())) {
      if (!statusBarRelayoutOffset) statusBarRelayoutOffset = pageOffsets[currentPage];
      readingOffset = *statusBarRelayoutOffset;
      relayout = true;
    }
    if (!flushQueuedProgress()) LOG_ERR("TRS", "Failed to save progress before status bar relayout");
    initialized = false;
  }

  // Initialize reader if not done
  if (!initialized) {
    initializeReader();
    if (relayout && !pageOffsets.empty()) {
      const auto nextPage = std::upper_bound(pageOffsets.begin(), pageOffsets.end(), readingOffset);
      currentPage = std::max(0, static_cast<int>(nextPage - pageOffsets.begin()) - 1);
      statusBarRelayoutPage = currentPage;
      if (!saveProgress(currentPage)) LOG_ERR("TRS", "Failed to save progress after status bar relayout");
    }
  }

  showOpeningFeedback();
  if (pageOffsets.empty()) {
    openingFeedback.finish();
    renderer.clearScreen(ReaderUtils::readerBackgroundColor());
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), ReaderUtils::readerForegroundBlack(),
                              EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Bounds check
  if (currentPage < 0) currentPage = 0;
  if (currentPage >= totalPages) currentPage = totalPages - 1;

  // Load current page content
  size_t offset = pageOffsets[currentPage];
  size_t nextOffset;
  currentPageLines.clear();
  loadPageAtOffset(offset, currentPageLines, nextOffset);
  showOpeningFeedback();

  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  renderPage();

  if (!queueProgressSave()) {
    LOG_ERR("TRS", "Failed to save debounced reader progress");
  }
}

void TxtReaderActivity::renderPage() {
  const int lineHeight = getReaderLineHeight(renderer, cachedFontId);
  const int contentWidth = viewportWidth;

  // Render text lines with alignment
  auto renderLines = [&]() {
    int y = cachedOrientedMarginTop;
    for (const auto& line : currentPageLines) {
      if (!line.empty()) {
        int x = cachedOrientedMarginLeft;
        const bool lineIsRtl = BidiUtils::startsWithRtl(line.c_str(), BidiUtils::RTL_PARAGRAPH_PROBE_DEPTH);
        uint8_t effectiveAlignment = cachedParagraphAlignment;
        if (lineIsRtl && (effectiveAlignment == CrossPointSettings::LEFT_ALIGN ||
                          effectiveAlignment == CrossPointSettings::JUSTIFIED)) {
          effectiveAlignment = CrossPointSettings::RIGHT_ALIGN;
        }
        const int textWidth =
            renderer.getTextAdvanceX(cachedFontId, line.c_str(), EpdFontFamily::REGULAR, 0, cachedCharacterSpacing);

        // Apply text alignment
        switch (effectiveAlignment) {
          case CrossPointSettings::LEFT_ALIGN:
          default:
            // x already set to left margin
            break;
          case CrossPointSettings::CENTER_ALIGN: {
            x = cachedOrientedMarginLeft + (contentWidth - textWidth) / 2;
            break;
          }
          case CrossPointSettings::RIGHT_ALIGN: {
            x = cachedOrientedMarginLeft + contentWidth - textWidth;
            break;
          }
          case CrossPointSettings::JUSTIFIED:
            // For plain text, justified is treated as left-aligned
            // (true justification would require word spacing adjustments)
            break;
        }

        renderer.drawText(cachedFontId, x, y, line.c_str(), ReaderUtils::readerForegroundBlack(),
                          EpdFontFamily::REGULAR, BidiUtils::BidiBaseDir::AUTO,
                          SETTINGS.characterSpacingLevel(SETTINGS.characterSpacing));
      }
      y += lineHeight;
    }
  };

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  renderLines();  // scan pass — text accumulated, no drawing
  scope.endScanAndPrewarm();
  if (showOpeningFeedback()) renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  openingFeedback.finish();

  // BW rendering
  renderLines();
  renderStatusBar();

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, [&renderLines]() { renderLines(); });
  }
  pluginEvents.rendered(totalPages > 0 ? static_cast<int>((currentPage + 1LL) * 10000 / totalPages) : 0);
  // scope destructor clears font cache via FontCacheManager
}

void TxtReaderActivity::renderStatusBar() const {
  if (!statusBarVisible) {
    return;
  }

  const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
  const auto needsTitle = [](const ReaderStatusBarConfig& bar) {
    return bar.contains(ReaderStatusBarItem::TitleBook) || bar.contains(ReaderStatusBarItem::TitleChapter);
  };
  std::string title;
  if (needsTitle(SETTINGS.readerStatusBar(ReaderStatusBarPosition::Top)) ||
      needsTitle(SETTINGS.readerStatusBar(ReaderStatusBarPosition::Bottom))) {
    title = txt->getTitle();
  }
  ReaderStatusBarContent content;
  if (SETTINGS.readerStatusBar(ReaderStatusBarPosition::Top).contains(ReaderStatusBarItem::Battery) ||
      SETTINGS.readerStatusBar(ReaderStatusBarPosition::Bottom).contains(ReaderStatusBarItem::Battery)) {
    content.batteryPercentage = powerManager.getBatteryPercentage();
  }
  content.bookProgress = progress;
  content.chapterPage = currentPage + 1;
  content.chapterPageCount = totalPages;
  content.bookTitle = title.c_str();
  // TXT has no chapter metadata; preserve its existing title fallback.
  content.chapterTitle = content.bookTitle;
  content.darkMode = ReaderUtils::readerDarkModeEnabled();
  GUI.drawReaderStatusBar(renderer, ReaderStatusBarPosition::Top, content);
  GUI.drawReaderStatusBar(renderer, ReaderStatusBarPosition::Bottom, content);
}

bool TxtReaderActivity::getFrontlightPanelBookDetails(FrontlightPanelBookDetails& details) {
  RenderLock lock(*this);
  if (!txt) return false;

  details.title = txt->getTitle();
  details.author.clear();
  details.chapter.clear();
  if (!initialized || totalPages <= 0) {
    details.progressPercent = 0;
    return true;
  }

  const int page = std::clamp(currentPage, 0, totalPages - 1);
  details.progressPercent = (page + 1) * 100 / totalPages;
  return true;
}

bool TxtReaderActivity::saveProgress(const int page) {
  if (!txt) {
    return false;
  }
  HalFile f;
  if (!Storage.openFileForWrite("TRS", txt->getCachePath() + "/progress.bin", f)) {
    return false;
  }
  // 6-byte format: page(2 bytes LE) + file offset(4 bytes LE)
  // The offset lets drawCurrentPageToBuffer render without requiring index.bin.
  const bool offsetKnown = page >= 0 && page < static_cast<int>(pageOffsets.size());
  const size_t offset = offsetKnown ? pageOffsets[page] : UINT32_MAX;
  uint8_t data[6];
  data[0] = page & 0xFF;
  data[1] = (page >> 8) & 0xFF;
  data[2] = offset & 0xFF;
  data[3] = (offset >> 8) & 0xFF;
  data[4] = (offset >> 16) & 0xFF;
  data[5] = (offset >> 24) & 0xFF;
  // Retain the existing six-byte format even while a font rebuild leaves only
  // the page known. An out-of-range offset lets old and new readers use that page.
  const bool written = f.write(data, sizeof(data)) == sizeof(data);
  f.close();
  if (!written) {
    LOG_ERR("TRS", "Short write saving reader progress");
    return false;
  }
  progressSaveDebouncer.markPersisted(static_cast<uint32_t>(page));
  return true;
}

bool TxtReaderActivity::queueProgressSave() {
  if (!progressSaveDebouncer.observe(static_cast<uint32_t>(currentPage))) {
    return true;
  }
  return saveProgress(currentPage);
}

bool TxtReaderActivity::flushQueuedProgress() {
  return !progressSaveDebouncer.hasPending() ||
         saveProgress(static_cast<int>(progressSaveDebouncer.lastObservedPosition()));
}

void TxtReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("TRS", txt->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6]{};
    const int bytes = f.read(data, sizeof(data));
    if (bytes >= 2) {
      currentPage = data[0] + (data[1] << 8);
      // Page numbers change when status bars change while the book is closed.
      // Prefer the existing saved byte offset; retain legacy short records.
      if (bytes == 6 && !pageOffsets.empty()) {
        const uint32_t offset = static_cast<uint32_t>(data[2]) | (static_cast<uint32_t>(data[3]) << 8) |
                                (static_cast<uint32_t>(data[4]) << 16) | (static_cast<uint32_t>(data[5]) << 24);
        if (offset < txt->getFileSize() && (offset != 0 || currentPage == 0)) {
          const auto nextPage = std::upper_bound(pageOffsets.begin(), pageOffsets.end(), offset);
          currentPage = std::max(0, static_cast<int>(nextPage - pageOffsets.begin()) - 1);
        }
      }
      if (currentPage >= totalPages) {
        currentPage = totalPages - 1;
      }
      if (currentPage < 0) {
        currentPage = 0;
      }
    }
    f.close();
  }
}

bool TxtReaderActivity::loadPageIndexCache() {
  // Cache file format (using serialization module):
  // - uint32_t: magic "TXTI"
  // - uint8_t: cache version
  // - uint32_t: file size (to validate cache)
  // - int32_t: viewport width
  // - int32_t: lines per page
  // - int32_t: font ID (to invalidate cache on font change)
  // - int32_t: vertical and horizontal screen margins (to invalidate cache on margin changes)
  // - uint8_t: paragraph alignment (to invalidate cache on alignment change)
  // - uint32_t: total pages count
  // - N * uint32_t: page offsets

  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForRead("TRS", cachePath, f)) {
    LOG_DBG("TRS", "No page index cache found");
    return false;
  }

  // Read and validate header using serialization module
  uint32_t magic;
  serialization::readPod(f, magic);
  if (magic != CACHE_MAGIC) {
    LOG_DBG("TRS", "Cache magic mismatch, rebuilding");
    return false;
  }

  uint8_t version;
  serialization::readPod(f, version);
  if (version != CACHE_VERSION) {
    LOG_DBG("TRS", "Cache version mismatch (%d != %d), rebuilding", version, CACHE_VERSION);
    return false;
  }

  uint32_t fileSize;
  serialization::readPod(f, fileSize);
  if (fileSize != txt->getFileSize()) {
    LOG_DBG("TRS", "Cache file size mismatch, rebuilding");
    return false;
  }

  int8_t spacing;
  if (!serialization::tryReadPod(f, spacing) || spacing != cachedCharacterSpacing) {
    f.close();
    return false;
  }
  int32_t cachedWidth;
  serialization::readPod(f, cachedWidth);
  if (cachedWidth != viewportWidth) {
    LOG_DBG("TRS", "Cache viewport width mismatch, rebuilding");
    return false;
  }

  int32_t cachedLines;
  serialization::readPod(f, cachedLines);
  if (cachedLines != linesPerPage) {
    LOG_DBG("TRS", "Cache lines per page mismatch, rebuilding");
    return false;
  }

  int32_t fontId;
  serialization::readPod(f, fontId);
  if (fontId != cachedFontId) {
    LOG_DBG("TRS", "Cache font ID mismatch (%d != %d), rebuilding", fontId, cachedFontId);
    return false;
  }

  int32_t verticalMargin;
  int32_t horizontalMargin;
  serialization::readPod(f, verticalMargin);
  serialization::readPod(f, horizontalMargin);
  if (verticalMargin != cachedVerticalMargin || horizontalMargin != cachedHorizontalMargin) {
    LOG_DBG("TRS", "Cache screen margins mismatch, rebuilding");
    return false;
  }

  uint8_t alignment;
  serialization::readPod(f, alignment);
  if (alignment != cachedParagraphAlignment) {
    LOG_DBG("TRS", "Cache paragraph alignment mismatch, rebuilding");
    return false;
  }

  uint32_t numPages;
  serialization::readPod(f, numPages);
  if (numPages > MAX_CACHE_PAGES) {
    LOG_ERR("TRS", "Cache numPages %u exceeds cap %u, cache invalid", numPages, MAX_CACHE_PAGES);
    f.close();
    return false;
  }

  // Read page offsets
  pageOffsets.clear();
  pageOffsets.reserve(numPages);

  for (uint32_t i = 0; i < numPages; i++) {
    uint32_t offset;
    serialization::readPod(f, offset);
    pageOffsets.push_back(offset);
  }

  totalPages = pageOffsets.size();
  return true;
}

void TxtReaderActivity::savePageIndexCache() const {
  std::string cachePath = txt->getCachePath() + "/index.bin";
  HalFile f;
  if (!Storage.openFileForWrite("TRS", cachePath, f)) {
    LOG_ERR("TRS", "Failed to save page index cache");
    return;
  }

  // Write header using serialization module
  serialization::writePod(f, CACHE_MAGIC);
  serialization::writePod(f, CACHE_VERSION);
  serialization::writePod(f, static_cast<uint32_t>(txt->getFileSize()));
  serialization::writePod(f, cachedCharacterSpacing);
  serialization::writePod(f, static_cast<int32_t>(viewportWidth));
  serialization::writePod(f, static_cast<int32_t>(linesPerPage));
  serialization::writePod(f, static_cast<int32_t>(cachedFontId));
  serialization::writePod(f, static_cast<int32_t>(cachedVerticalMargin));
  serialization::writePod(f, static_cast<int32_t>(cachedHorizontalMargin));
  serialization::writePod(f, cachedParagraphAlignment);
  serialization::writePod(f, static_cast<uint32_t>(pageOffsets.size()));

  // Write page offsets
  for (size_t offset : pageOffsets) {
    serialization::writePod(f, static_cast<uint32_t>(offset));
  }
}

bool TxtReaderActivity::drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer) {
  Txt txt(filePath, "/.crosspoint");
  if (!txt.load()) {
    LOG_DBG("SLP", "TXT: failed to load %s", filePath.c_str());
    return false;
  }

  // Apply the reader orientation so margins match what the reader would produce
  switch (SETTINGS.orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }

  // Compute layout values that match what initializeReader() produces
  const int fontId = SETTINGS.getReaderFontId();
  const uint8_t verticalMargin = SETTINGS.screenMarginVertical;
  const uint8_t horizontalMargin = SETTINGS.screenMarginHorizontal;
  const uint8_t paragraphAlignment = SETTINGS.paragraphAlignment;

  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginLeft += horizontalMargin;
  marginRight += horizontalMargin;
  const int topStatusBarReservedHeight = ReaderUtils::getTopStatusBarReservedHeight(renderer);
  if (topStatusBarReservedHeight > 0) {
    marginTop += std::max(static_cast<int>(verticalMargin),
                          topStatusBarReservedHeight + ReaderUtils::TOP_STATUS_BAR_TEXT_PADDING);
  } else {
    marginTop += verticalMargin;
  }
  marginBottom += ReaderUtils::getReaderFooterReservedHeight(renderer, false);

  const int vw = renderer.getScreenWidth() - marginLeft - marginRight;
  const int vh = renderer.getScreenHeight() - marginTop - marginBottom;
  const int lineHeight = getReaderLineHeight(renderer, fontId);
  const int linesPerPage = std::max(1, vh / lineHeight);

  // Step 1: Try to read the saved page and its file offset from progress.bin.
  // The 6-byte format (written by saveProgress) stores: page(2) + offset(4).
  // This lets us skip index.bin entirely, so the overlay works even when the
  // page index cache is missing or stale (e.g. after a firmware update).
  int savedPage = 0;
  size_t savedOffset = 0;
  bool offsetKnown = false;
  {
    FsFile progFile;
    if (Storage.openFileForRead("SLP", txt.getCachePath() + "/progress.bin", progFile)) {
      uint8_t data[6] = {0};
      const int n = progFile.read(data, 6);
      progFile.close();
      if (n >= 2) {
        savedPage = (int)((uint32_t)data[0] | ((uint32_t)data[1] << 8));
      }
      if (n >= 6) {
        const uint32_t off =
            (uint32_t)data[2] | ((uint32_t)data[3] << 8) | ((uint32_t)data[4] << 16) | ((uint32_t)data[5] << 24);
        if (off < txt.getFileSize()) {
          savedOffset = off;
          offsetKnown = true;
        }
      }
    }
  }

  // Step 2: If progress.bin didn't provide the offset, fall back to index.bin.
  if (!offsetKnown) {
    std::string cachePath = txt.getCachePath() + "/index.bin";
    FsFile cacheFile;
    if (Storage.openFileForRead("SLP", cachePath, cacheFile)) {
      uint32_t magic;
      serialization::readPod(cacheFile, magic);
      uint8_t version;
      serialization::readPod(cacheFile, version);
      uint32_t cachedFileSize;
      serialization::readPod(cacheFile, cachedFileSize);
      int8_t cachedSpacing = 0;
      if (version == CACHE_VERSION) serialization::readPod(cacheFile, cachedSpacing);
      int32_t cachedVw, cachedLpp, cachedFontId, cachedVerticalMargin, cachedHorizontalMargin;
      serialization::readPod(cacheFile, cachedVw);
      serialization::readPod(cacheFile, cachedLpp);
      serialization::readPod(cacheFile, cachedFontId);
      serialization::readPod(cacheFile, cachedVerticalMargin);
      serialization::readPod(cacheFile, cachedHorizontalMargin);
      uint8_t cachedAlignment;
      serialization::readPod(cacheFile, cachedAlignment);
      uint32_t numPages;
      serialization::readPod(cacheFile, numPages);

      if (magic == CACHE_MAGIC && version == CACHE_VERSION && cachedFileSize == txt.getFileSize() && cachedVw == vw &&
          cachedLpp == linesPerPage && cachedFontId == fontId && cachedVerticalMargin == verticalMargin &&
          cachedHorizontalMargin == horizontalMargin && cachedAlignment == paragraphAlignment &&
          cachedSpacing == SETTINGS.characterSpacingLevel(SETTINGS.characterSpacing) && numPages > 0 &&
          numPages <= MAX_CACHE_PAGES) {
        if (savedPage < 0 || savedPage >= static_cast<int>(numPages)) savedPage = 0;
        for (uint32_t i = 0; i < numPages; i++) {
          uint32_t off;
          serialization::readPod(cacheFile, off);
          if (static_cast<int>(i) == savedPage) {
            if (off < txt.getFileSize()) {
              savedOffset = off;
              offsetKnown = true;
            } else {
              LOG_DBG("SLP", "TXT: index.bin offset %u out of range (fileSize=%u), ignoring", off, txt.getFileSize());
            }
          }
        }
      } else {
        LOG_DBG("SLP", "TXT: index cache invalid or stale");
      }
      cacheFile.close();
    }

    // Step 3: No valid cache at all; render from the start of the file as a last resort.
    // This shows page 1 rather than a blank screen, which is always preferable.
    if (!offsetKnown) {
      LOG_DBG("SLP", "TXT: no valid cache, falling back to start of file");
      savedOffset = 0;
    }
  }

  // Load the page lines from file
  std::vector<std::string> pageLines;
  const size_t fileSize = txt.getFileSize();
  size_t offset = savedOffset;
  if (offset >= fileSize) {
    LOG_DBG("SLP", "TXT: page offset out of bounds");
    return false;
  }

  size_t chunkSize = std::min(CHUNK_SIZE, fileSize - offset);
  auto* buffer = static_cast<uint8_t*>(malloc(chunkSize + 1));
  if (!buffer) return false;

  if (!txt.readContent(buffer, offset, chunkSize)) {
    free(buffer);
    return false;
  }
  buffer[chunkSize] = '\0';

  parseAndWrapLines(buffer, chunkSize, offset, fileSize, linesPerPage, renderer, fontId, vw, pageLines,
                    SETTINGS.characterSpacingLevel(SETTINGS.characterSpacing));
  free(buffer);

  if (pageLines.empty()) return false;

  // Render lines to frame buffer (no displayBuffer call)
  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  int y = marginTop;
  for (const auto& line : pageLines) {
    if (!line.empty()) {
      int x = marginLeft;
      switch (paragraphAlignment) {
        case CrossPointSettings::CENTER_ALIGN:
          x = marginLeft +
              (vw - renderer.getTextWidth(fontId, line.c_str(), EpdFontFamily::REGULAR, BidiUtils::BidiBaseDir::AUTO,
                                          SETTINGS.characterSpacingLevel(SETTINGS.characterSpacing))) /
                  2;
          break;
        case CrossPointSettings::RIGHT_ALIGN:
          x = marginLeft + vw -
              renderer.getTextWidth(fontId, line.c_str(), EpdFontFamily::REGULAR, BidiUtils::BidiBaseDir::AUTO,
                                    SETTINGS.characterSpacingLevel(SETTINGS.characterSpacing));
          break;
        default:
          break;
      }
      renderer.drawText(fontId, x, y, line.c_str(), ReaderUtils::readerForegroundBlack(), EpdFontFamily::REGULAR,
                        BidiUtils::BidiBaseDir::AUTO, SETTINGS.characterSpacingLevel(SETTINGS.characterSpacing));
    }
    y += lineHeight;
  }
  return true;
}

ScreenshotInfo TxtReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Txt;
  if (txt) {
    const std::string t = txt->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
  }
  info.currentPage = currentPage + 1;
  info.totalPages = totalPages;
  info.progressPercent = totalPages > 0 ? static_cast<int>((currentPage + 1) * 100.0f / totalPages + 0.5f) : 0;
  if (info.progressPercent > 100) info.progressPercent = 100;
  return info;
}

void TxtReaderActivity::saveProgressBeforeRestart() {
  // Silent network-mode restarts skip onExit(); coordinate with the render task.
  RenderLock lock(*this);
  if (!(flushQueuedProgress())) {
    LOG_ERR("READER", "Failed to save progress before restart");
  }
}
