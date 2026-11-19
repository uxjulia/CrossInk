#include "CalibreConnectActivity.h"

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "WifiSelectionActivity.h"
#include "components/CompactHeader.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* HOSTNAME = "crosspoint";
}  // namespace

void CalibreConnectActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);

  requestUpdate();
  state = CalibreConnectState::WIFI_SELECTION;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  progress = {};
  lastCompleteName.clear();
  lastCompleteAt = 0;
  lastProcessedCompleteAt = 0;
  exitRequested = false;

  if (WiFi.status() != WL_CONNECTED) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               RenderLock lock;
                               const auto& wifi = std::get<WifiResult>(result.data);
                               connectedIP = wifi.ip;
                               connectedSSID = wifi.ssid;
                             }
                             onWifiSelectionComplete(!result.isCancelled);
                           });
  } else {
    connectedIP = WiFi.localIP().toString().c_str();
    connectedSSID = WiFi.SSID().c_str();
    startWebServer();
  }
}

void CalibreConnectActivity::onExit() {
  library::invalidateLibraryIndex();
  Activity::onExit();

  MDNS.end();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    if (returnToReader) {
      silentRestartToReader();
    } else {
      silentRestart();
    }
  }
}

void CalibreConnectActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    finish();
    return;
  }

  startWebServer();
}

void CalibreConnectActivity::startWebServer() {
  {
    RenderLock lock;
    state = CalibreConnectState::SERVER_STARTING;
    requestUpdate();
  }

  MDNS.end();
  if (MDNS.begin(HOSTNAME)) {
    // mDNS is optional for the Calibre plugin but still helpful for users.
    LOG_DBG("CAL", "mDNS started: http://%s.local/", HOSTNAME);
  }

  webServer.reset(new CrossPointWebServer());
  webServer->begin();

  RenderLock lock;
  if (webServer->isRunning()) {
    state = CalibreConnectState::SERVER_RUNNING;
    requestUpdate();
  } else {
    state = CalibreConnectState::ERROR;
    requestUpdate();
  }
}

void CalibreConnectActivity::stopWebServer() {
  if (webServer) {
    webServer->stop();
    webServer.reset();
  }
}

void CalibreConnectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    exitRequested = true;
  }

  if (webServer && webServer->isRunning()) {
    const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;
    if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
      LOG_DBG("CAL", "WARNING: %lu ms gap since last handleClient", timeSinceLastHandleClient);
    }

    esp_task_wdt_reset();
    constexpr int MAX_ITERATIONS = 80;
    for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); i++) {
      webServer->handleClient();
      if ((i & 0x07) == 0x07) {
        esp_task_wdt_reset();
      }
      if ((i & 0x0F) == 0x0F) {
        yield();
        if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
          exitRequested = true;
          break;
        }
      }
    }
    lastHandleClientTime = millis();

    const auto status = webServer->getWsUploadStatus();
    RenderLock lock;  // Publish upload strings and their counters together with respect to render().
    bool changed = progress.update(status.inProgress, status.filename, status.received, status.total, millis());
    // Only update lastCompleteAt if the server has a NEW value (not one we already processed)
    // This prevents restoring an old value after the 6s timeout clears it
    if (status.lastCompleteAt != 0 && status.lastCompleteAt != lastProcessedCompleteAt) {
      lastCompleteAt = status.lastCompleteAt;
      lastCompleteName = status.lastCompleteName;
      lastProcessedCompleteAt = status.lastCompleteAt;  // Mark this value as processed
      changed = true;
    }
    if (lastCompleteAt > 0 && (millis() - lastCompleteAt) >= 6000) {
      lastCompleteAt = 0;
      lastCompleteName.clear();
      // Note: we DON'T reset lastProcessedCompleteAt here, so we won't re-process the old server value
      changed = true;
    }
    if (changed) {
      requestUpdate();
    }
  }

  if (exitRequested) {
    finish();
    return;
  }
}

void CalibreConnectActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int pageWidth = safe.width;
  const int pageHeight = safe.height;

  renderer.clearScreen();

  CompactHeader::drawTitle(renderer, tr(STR_CALIBRE_WIRELESS));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = safe.y + (pageHeight - height) / 2;

  if (state == CalibreConnectState::SERVER_STARTING) {
    renderer.drawCenteredText(UI_12_FONT_ID, top, tr(STR_CALIBRE_STARTING));
  } else if (state == CalibreConnectState::ERROR) {
    renderer.drawCenteredText(UI_12_FONT_ID, top, tr(STR_CONNECTION_FAILED), true, EpdFontFamily::BOLD);
  } else if (state == CalibreConnectState::SERVER_RUNNING) {
    const int subHeaderTop = CompactHeader::contentTop(renderer);
    GUI.drawSubHeader(renderer, Rect{safe.x, subHeaderTop, pageWidth, metrics.tabBarHeight}, connectedSSID.c_str());

    // Keep the network name and full address independently readable on narrow
    // screens. Sharing one subheader row forces one of them to be truncated.
    const std::string ipLabel = std::string(tr(STR_IP_ADDRESS_PREFIX)) + connectedIP;
    const int ipTop = subHeaderTop + metrics.tabBarHeight + metrics.verticalSpacing;
    renderer.drawText(SMALL_FONT_ID, safe.x + metrics.contentSidePadding, ipTop, ipLabel.c_str());

    int y = ipTop + height + metrics.verticalSpacing * 3;
    const auto heightText12 = renderer.getTextHeight(UI_12_FONT_ID);
    renderer.drawText(UI_12_FONT_ID, safe.x + metrics.contentSidePadding, y, tr(STR_CALIBRE_SETUP), true,
                      EpdFontFamily::BOLD);
    y += heightText12 + metrics.verticalSpacing * 2;

    renderer.drawText(SMALL_FONT_ID, safe.x + metrics.contentSidePadding, y, tr(STR_CALIBRE_INSTRUCTION_1));
    renderer.drawText(SMALL_FONT_ID, safe.x + metrics.contentSidePadding, y + height, tr(STR_CALIBRE_INSTRUCTION_2));
    renderer.drawText(SMALL_FONT_ID, safe.x + metrics.contentSidePadding, y + height * 2,
                      tr(STR_CALIBRE_INSTRUCTION_3));
    renderer.drawText(SMALL_FONT_ID, safe.x + metrics.contentSidePadding, y + height * 3,
                      tr(STR_CALIBRE_INSTRUCTION_4));

    y += height * 3 + metrics.verticalSpacing * 4;
    renderer.drawText(UI_12_FONT_ID, safe.x + metrics.contentSidePadding, y, tr(STR_CALIBRE_STATUS), true,
                      EpdFontFamily::BOLD);
    y += heightText12 + metrics.verticalSpacing * 2;

    const bool showUploadProgress = progress.total > 0 && progress.received <= progress.total;
    if (showUploadProgress) {
      std::string label = tr(STR_CALIBRE_RECEIVING);
      if (!progress.filename.empty()) {
        label += ": " + progress.filename;
        label = renderer.truncatedText(SMALL_FONT_ID, label.c_str(), pageWidth - metrics.contentSidePadding * 2,
                                       EpdFontFamily::REGULAR);
      }
      renderer.drawText(SMALL_FONT_ID, safe.x + metrics.contentSidePadding, y, label.c_str());
      GUI.drawProgressBar(renderer,
                          Rect{safe.x + metrics.contentSidePadding, y + height + metrics.verticalSpacing,
                               pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
                          progress.received, progress.total);
      y += height + metrics.verticalSpacing * 2 + metrics.progressBarHeight;
    }

    if (!showUploadProgress && lastCompleteAt > 0 && (millis() - lastCompleteAt) < 6000) {
      std::string msg = std::string(tr(STR_CALIBRE_RECEIVED)) + lastCompleteName;
      msg = renderer.truncatedText(SMALL_FONT_ID, msg.c_str(), pageWidth - metrics.contentSidePadding * 2,
                                   EpdFontFamily::REGULAR);
      renderer.drawText(SMALL_FONT_ID, safe.x + metrics.contentSidePadding, y, msg.c_str());
    }

    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_EXIT)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
}
