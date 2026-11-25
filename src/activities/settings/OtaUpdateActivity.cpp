#include "OtaUpdateActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include "AppVersion.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/CompactHeader.h"
#include "components/TouchActionButtons.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/OtaUpdater.h"

namespace {
bool hasActiveWifiConnection() { return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0); }

StrId failureMessageFor(const OtaUpdater::OtaUpdaterError error) {
  if (error == OtaUpdater::HASH_MISMATCH_ERROR) return StrId::STR_UPDATE_HASH_MISMATCH;
  if (error == OtaUpdater::WRONG_DEVICE_ERROR) return StrId::STR_FIRMWARE_WRONG_DEVICE;
  if (error == OtaUpdater::SD_CARD_FULL_ERROR) return StrId::STR_SD_CARD_FULL;
  return StrId::STR_UPDATE_FAILED;
}

TouchActionButtons::Layout getOtaActionLayout(const GfxRenderer& renderer) {
  constexpr int sideMargin = 24;
  constexpr int bottomMargin = 12;
  constexpr int totalHeight = TouchActionButtons::kDefaultHeight * 2 + TouchActionButtons::kDefaultGap;
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  return TouchActionButtons::vertical(Rect{safe.x + sideMargin, safe.y + safe.height - bottomMargin - totalHeight,
                                           std::max(1, safe.width - sideMargin * 2), totalHeight},
                                      2);
}
}  // namespace

void OtaUpdateActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_ERR("OTA", "WiFi connection failed, exiting");
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state = CHECKING_FOR_UPDATE;
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("OTA", "Checking update screen could not be rendered synchronously; aborting update check");
    {
      RenderLock lock(*this);
      state = FAILED;
    }
    requestUpdate(true);
    return;
  }

  const auto res = updater.checkForUpdate();
  if (res != OtaUpdater::OK) {
    LOG_DBG("OTA", "Update check failed: %d", res);
    {
      RenderLock lock(*this);
      failureMessage = failureMessageFor(res);
      state = FAILED;
    }
    requestUpdate(true);
    return;
  }

  if (!updater.isUpdateNewer()) {
    {
      RenderLock lock(*this);
      state = NO_UPDATE;
    }
    requestUpdate(true);
    return;
  }

  {
    RenderLock lock(*this);
    state = WAITING_CONFIRMATION;
  }
  requestUpdate(true);
}

void OtaUpdateActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);

  if (hasActiveWifiConnection()) {
    onWifiSelectionComplete(true);
    return;
  }

  // Turn on WiFi immediately
  WiFi.mode(WIFI_STA);

  // Launch WiFi selection subactivity
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OtaUpdateActivity::onExit() {
  Activity::onExit();

  // Success path reboots via the SHUTTING_DOWN state's plain ESP.restart()
  // (loop() above) so the new firmware boots normally. Back-out paths land
  // here with wifi still active; silent-restart to free the LWIP/mbedTLS
  // fragmentation, same as the other wifi activities.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OtaUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int pageWidth = safe.width;
  const int pageHeight = safe.height;

  renderer.clearScreen();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  const bool canGoBack = state == WAITING_CONFIRMATION || state == FAILED || state == NO_UPDATE;
  if (mappedInput.hasTouchHardware()) {
    if (canGoBack) {
      TouchHeaderBackButton::draw(renderer, header, tr(STR_UPDATE), false);
    } else {
      CompactHeader::drawTitle(renderer, tr(STR_UPDATE));
    }
  } else {
    GUI.drawHeader(renderer, header, tr(STR_UPDATE));
  }
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = safe.y + (pageHeight - height) / 2;

  float updaterProgress = 0;
  if (state == UPDATE_IN_PROGRESS) {
    updaterProgress = static_cast<float>(updater.getProcessedSize()) / static_cast<float>(updater.getTotalSize());
    // Only update every 2% at the most
    if (static_cast<int>(updaterProgress * 50) == lastUpdaterPercentage / 2) {
      return;
    }
    lastUpdaterPercentage = static_cast<int>(updaterProgress * 100);
  }

  if (state == CHECKING_FOR_UPDATE) {
    UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, top, tr(STR_CHECKING_UPDATE));
  } else if (state == WAITING_CONFIRMATION) {
    UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, top, tr(STR_NEW_UPDATE), true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, safe.x + metrics.contentSidePadding, top + height + metrics.verticalSpacing,
                      (std::string(tr(STR_CURRENT_VERSION)) + AppVersion::version()).c_str());
    renderer.drawText(UI_10_FONT_ID, safe.x + metrics.contentSidePadding,
                      top + height * 2 + metrics.verticalSpacing * 2,
                      (std::string(tr(STR_NEW_VERSION)) + updater.getLatestVersion()).c_str());

    if (mappedInput.hasTouch()) {
      const auto actions = getOtaActionLayout(renderer);
      const char* labels[] = {tr(STR_UPDATE), tr(STR_CANCEL)};
      TouchActionButtons::draw(renderer, actions, labels, 0, -1, UI_10_FONT_ID);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_UPDATE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == UPDATE_IN_PROGRESS) {
    UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, top, tr(STR_UPDATING));

    int y = top + height + metrics.verticalSpacing;
    GUI.drawProgressBar(renderer,
                        Rect{safe.x + metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2,
                             metrics.progressBarHeight},
                        static_cast<int>(updaterProgress * 100), 100);

    y += metrics.progressBarHeight + metrics.verticalSpacing;
    // BaseTheme::drawProgressBar draws the percentage. This line shows the
    // monotonic combined staging-and-flashing work instead.
    y += height + metrics.verticalSpacing;
    UITheme::drawCenteredText(
        renderer, safe, UI_10_FONT_ID, y,
        (std::to_string(updater.getProcessedSize()) + " / " + std::to_string(updater.getTotalSize())).c_str());
  } else if (state == NO_UPDATE) {
    UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, top, tr(STR_NO_UPDATE), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FAILED) {
    UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, top, I18n::getInstance().get(failureMessage), true,
                              EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FINISHED) {
    UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    const int hintY = top + height + metrics.verticalSpacing;
    const Rect hintBounds{safe.x + metrics.contentSidePadding, hintY, pageWidth - metrics.contentSidePadding * 2,
                          safe.y + pageHeight - hintY};
    UITheme::drawCenteredWrappedText(renderer, hintBounds, UI_10_FONT_ID, hintY, tr(STR_POWER_ON_HINT), 3);
  }

  renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
}

void OtaUpdateActivity::runUpdateInstall() {
  {
    RenderLock lock(*this);
    failureMessage = StrId::STR_UPDATE_FAILED;
    state = UPDATE_IN_PROGRESS;
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("OTA", "Update progress screen could not be rendered synchronously; aborting OTA install");
    {
      RenderLock lock(*this);
      state = FAILED;
    }
    requestUpdate(true);
    return;
  }
  const auto res = updater.installUpdate(
      [](void* ctx) {
        // immediate=true notifies the render task directly. The default deferred path only
        // sets a flag consumed at the end of ActivityManager::loop(), which never runs while
        // installUpdate() blocks this task.
        static_cast<OtaUpdateActivity*>(ctx)->requestUpdate(true);
      },
      this);

  if (res != OtaUpdater::OK) {
    LOG_DBG("OTA", "Update failed: %d", res);
    {
      RenderLock lock(*this);
      failureMessage = failureMessageFor(res);
      state = FAILED;
    }
    requestUpdate();
    return;
  }

  {
    RenderLock lock(*this);
    state = FINISHED;
  }
  const auto renderResult = requestUpdateAndWait();
  if (renderResult == RequestUpdateResult::Rendered) {
    // Hold the completion screen briefly so the user sees it, then restart.
    delay(3000);
  } else {
    LOG_ERR("OTA", "Completion screen could not be rendered synchronously; restarting without sync confirmation");
  }
  {
    RenderLock lock(*this);
    state = SHUTTING_DOWN;
  }
}

void OtaUpdateActivity::loop() {
  if ((state == WAITING_CONFIRMATION || state == FAILED || state == NO_UPDATE) &&
      TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finish();
    return;
  }

  if (state == WAITING_CONFIRMATION) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasScreenTapped(x, y)) {
      const auto actions = getOtaActionLayout(renderer);
      const int action = TouchActionButtons::indexAt(actions, x, y);
      if (action == 0) {
        runUpdateInstall();
        return;
      }
      if (action == 1) {
        finish();
        return;
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      runUpdateInstall();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }

    return;
  }

  if (state == FAILED) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
    return;
  }

  if (state == NO_UPDATE) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
    return;
  }

  if (state == SHUTTING_DOWN) {
    ESP.restart();
  }
}
