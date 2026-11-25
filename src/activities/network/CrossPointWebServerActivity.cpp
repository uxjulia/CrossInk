#include "CrossPointWebServerActivity.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Memory.h>
#include <WiFi.h>

#include <cstddef>

#include "CrossPointState.h"
#include "FilenameFontSystem.h"
#include "MappedInputManager.h"
#include "NetworkModeSelectionActivity.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "WifiSelectionActivity.h"
#include "activities/ActivityManager.h"
#include "activities/network/CalibreConnectActivity.h"
#include "activities/network/StatsUploadActivity.h"
#include "components/CompactHeader.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/PluginEvents.h"
#include "util/QrUtils.h"

namespace {
// AP Mode configuration
constexpr const char* AP_SSID = "CrossPoint-Reader";
constexpr const char* AP_PASSWORD = nullptr;  // Open network for ease of use
constexpr const char* AP_HOSTNAME = "crosspoint";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr int QR_CODE_WIDTH = 198;
constexpr int QR_CODE_HEIGHT = 198;

// DNS server for captive portal (redirects all DNS queries to our IP)
DNSServer* dnsServer = nullptr;
constexpr uint16_t DNS_PORT = 53;

void stopDnsServer() {
  if (!dnsServer) return;

  dnsServer->stop();
  delete dnsServer;
  dnsServer = nullptr;
}

void restartMdns(const char* hostname, const char* tag) {
  MDNS.end();
  if (MDNS.begin(hostname)) {
    LOG_DBG(tag, "mDNS started: http://%s.local/", hostname);
  } else {
    LOG_DBG(tag, "WARNING: mDNS failed to start");
  }
}

// 0..4 bars from RSSI (dBm), with 3 dBm hysteresis on currentBars to suppress flicker.
int barsForRssi(int rssi, int currentBars) {
  static constexpr int RISE_DBM[] = {-85, -75, -65, -55};
  static constexpr int FALL_DBM[] = {-88, -78, -68, -58};
  int bars = std::clamp(currentBars, 0, 4);
  while (bars < 4 && rssi >= RISE_DBM[bars]) bars++;
  while (bars > 0 && rssi < FALL_DBM[bars - 1]) bars--;
  return bars;
}
}  // namespace

void CrossPointWebServerActivity::onEnter() {
  // The server can replace the selected font through HTTP/WebDAV. Keep it
  // closed throughout this activity and its Wi-Fi/password child screens.
  filenameFontSystem.suspend(renderer);
  Activity::onEnter();
  // Build or refresh the compact on-disk font index before Wi-Fi starts. The
  // C3 has substantially more contiguous heap here than while serving HTTP.
  sdFontSystem.ensureRegistry();
  sdFontSystem.releaseForNetwork(renderer);

  LOG_DBG("WEBACT", "Free heap at onEnter: %d bytes", ESP.getFreeHeap());

  // Reset state
  state = WebServerActivityState::MODE_SELECTION;
  networkMode = NetworkMode::JOIN_NETWORK;
  isApMode = false;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  requestUpdate();

  if (hasInitialNetworkMode) {
    onNetworkModeSelected(initialNetworkMode);
    return;
  }

  // Launch network mode selection subactivity
  startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             exitToOrigin();
                           } else {
                             onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                           }
                         });
}

void CrossPointWebServerActivity::onExit() {
  pluginevents::refreshSubscriptions();
  library::invalidateLibraryIndex();
  Activity::onExit();

  state = WebServerActivityState::SHUTTING_DOWN;

  // Every active WiFi exit already reboots to clear network heap
  // fragmentation. Restart before graceful socket teardown: a stalled browser
  // can otherwise keep WebSocketsServer::close() retrying writes for seconds.
  // silentRestart() returns only when deep sleep is already in progress; that
  // path still needs the explicit cleanup below.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    if (returnBookPath.empty()) {
      silentRestart();
    } else {
      silentRestartToReader();
    }
  }

  stopDnsServer();
  MDNS.end();

  // Stop local services before disconnecting/restarting WiFi.
  stopWebServer();
  MDNS.end();
  if (dnsServer) {
    dnsServer->stop();
    delete dnsServer;
    dnsServer = nullptr;
  }
  delay(50);

  // On the deep-sleep path silentRestart() returns without rebooting, so shut
  // WiFi down after local services have released their sockets.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    if (isApMode) {
      WiFi.softAPdisconnect(true);
    } else {
      WiFi.disconnect(false);
    }
    delay(30);
  }

  LOG_DBG("WEBACT", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
  filenameFontSystem.resume();
}

void CrossPointWebServerActivity::onNetworkModeSelected(const NetworkMode mode) {
  const char* modeName = "Join Network";
  if (mode == NetworkMode::CONNECT_CALIBRE) {
    modeName = "Connect to Calibre";
  } else if (mode == NetworkMode::OPDS_BROWSER) {
    modeName = "OPDS Browser";
  } else if (mode == NetworkMode::CREATE_HOTSPOT) {
    modeName = "Create Hotspot";
  } else if (mode == NetworkMode::NEARBY_STATS_SYNC) {
    modeName = "Nearby Stats Sync";
  } else if (mode == NetworkMode::NEARBY_BOOK_RECEIVE) {
    modeName = "Receive File";
  } else if (mode == NetworkMode::SYNC_BOOKS) {
    modeName = "Sync Books";
  } else if (mode == NetworkMode::SYNC_READING_STATS) {
    modeName = "Sync Reading Stats";
  } else if (mode == NetworkMode::SYNC_CLIPPINGS) {
    modeName = "Sync Clippings";
  }
  LOG_DBG("WEBACT", "Network mode selected: %s", modeName);

  if (mode == NetworkMode::USB_DRIVE) {
    activityManager.goToUsbDrive();
    return;
  }

  if (mode == NetworkMode::SYNC_BOOKS || mode == NetworkMode::SYNC_READING_STATS ||
      mode == NetworkMode::SYNC_CLIPPINGS) {
    // Sync Books asks first; the targeted uploads start straight away. Each
    // brings up Wi-Fi itself, like Sync Folder.
    const auto scope = mode == NetworkMode::SYNC_BOOKS           ? StatsUploadActivity::Scope::Library
                       : mode == NetworkMode::SYNC_READING_STATS ? StatsUploadActivity::Scope::ReadingStats
                                                                 : StatsUploadActivity::Scope::Clippings;
    auto sync = makeUniqueNoThrow<StatsUploadActivity>(renderer, mappedInput, std::string{}, scope,
                                                       scope != StatsUploadActivity::Scope::Library);
    if (!sync) {
      LOG_ERR("WEBACT", "Cannot allocate sync activity");
      exitToOrigin();
      return;
    }
    sync->setReturnToFileTransfer(returnBookPath, networkBootReady);
    // Back here after a cancelled Join Network, the radio can still be on; drop
    // it so leaving this screen does not reboot before the sync runs. The sync
    // reboots on leave itself when this is the minimal network boot.
    if (WiFi.getMode() != WIFI_MODE_NULL) {
      WiFi.disconnect(false);
      WiFi.mode(WIFI_OFF);
    }
    activityManager.replaceActivity(std::move(sync));
    return;
  }
  if (mode == NetworkMode::OPDS_BROWSER) {
    activityManager.goToBrowser();
    return;
  }

  networkMode = mode;
  isApMode = (mode == NetworkMode::CREATE_HOTSPOT);

  if (mode == NetworkMode::NEARBY_STATS_SYNC) {
    activityManager.goToNearbyStatsSync();
    return;
  }
  if (mode == NetworkMode::NEARBY_BOOK_RECEIVE) {
    activityManager.goToNearbyBookReceive();
    return;
  }

  if (!networkBootReady) {
    switch (mode) {
      case NetworkMode::JOIN_NETWORK:
        activityManager.goToJoinNetworkFileTransfer(returnBookPath);
        break;
      case NetworkMode::CONNECT_CALIBRE:
        activityManager.goToCalibreWireless(returnBookPath);
        break;
      case NetworkMode::CREATE_HOTSPOT:
        activityManager.goToHotspotFileTransfer(returnBookPath);
        break;
      case NetworkMode::USB_DRIVE:
        activityManager.goToUsbDrive();
        break;
      case NetworkMode::OPDS_BROWSER:
      case NetworkMode::NEARBY_STATS_SYNC:
      case NetworkMode::NEARBY_BOOK_RECEIVE:
      case NetworkMode::SYNC_BOOKS:
      case NetworkMode::SYNC_READING_STATS:
      case NetworkMode::SYNC_CLIPPINGS:
        break;
    }
    return;
  }

  if (mode == NetworkMode::CONNECT_CALIBRE) {
    // The child activity must survive this callback; allocate only its small control object on the heap.
    auto calibreActivity = makeUniqueNoThrow<CalibreConnectActivity>(renderer, mappedInput, !returnBookPath.empty());
    if (!calibreActivity) {
      LOG_ERR("WEBACT", "OOM: Calibre activity (size=%u free=%u maxAlloc=%u)",
              static_cast<unsigned>(sizeof(CalibreConnectActivity)), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      exitToOrigin();
      return;
    }

    startActivityForResult(std::move(calibreActivity), [this](const ActivityResult& result) {
      state = WebServerActivityState::MODE_SELECTION;

      if (networkBootReady) {
        exitToOrigin();
        return;
      }

      startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                             [this](const ActivityResult& result) {
                               if (result.isCancelled) {
                                 exitToOrigin();
                               } else {
                                 onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                               }
                             });
    });
    return;
  }

  if (mode == NetworkMode::JOIN_NETWORK) {
    // STA mode - launch WiFi selection
    WiFi.mode(WIFI_STA);

    state = WebServerActivityState::WIFI_SELECTION;
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& wifi = std::get<WifiResult>(result.data);
                               connectedIP = wifi.ip;
                               connectedSSID = wifi.ssid;
                             }
                             onWifiSelectionComplete(!result.isCancelled);
                           });
  } else {
    // AP mode - start access point
    state = WebServerActivityState::AP_STARTING;
    requestUpdate();
    startAccessPoint();
  }
}

void CrossPointWebServerActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    // Get connection info before exiting subactivity
    isApMode = false;

    // Start mDNS for hostname resolution
    restartMdns(AP_HOSTNAME, "WEBACT");

    // Start the web server
    startWebServer();
  } else {
    // User cancelled - go back to mode selection
    state = WebServerActivityState::MODE_SELECTION;

    startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               exitToOrigin();
                             } else {
                               onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                             }
                           });
  }
}

void CrossPointWebServerActivity::startAccessPoint() {
  LOG_DBG("WEBACT", "Free heap before AP start: %d bytes", ESP.getFreeHeap());

  // Configure and start the AP
  WiFi.mode(WIFI_AP);
  delay(100);

  // Start soft AP
  bool apStarted;
  if (AP_PASSWORD && strlen(AP_PASSWORD) >= 8) {
    apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  } else {
    // Open network (no password)
    apStarted = WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  }

  if (!apStarted) {
    LOG_ERR("WEBACT", "ERROR: Failed to start Access Point!");
    exitToOrigin();
    return;
  }

  delay(100);  // Wait for AP to fully initialize

  // Get AP IP address
  const IPAddress apIP = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", apIP[0], apIP[1], apIP[2], apIP[3]);
  connectedIP = ipStr;
  connectedSSID = AP_SSID;

  // Start mDNS for hostname resolution
  restartMdns(AP_HOSTNAME, "WEBACT");

  // Start DNS server for captive portal behavior
  // This redirects all DNS queries to our IP, making any domain typed resolve to us
  stopDnsServer();
  dnsServer = new DNSServer();
  dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer->start(DNS_PORT, "*", apIP);

  LOG_DBG("WEBACT", "Free heap after AP start: %d bytes", ESP.getFreeHeap());

  // Start the web server
  startWebServer();
}

void CrossPointWebServerActivity::startWebServer() {
  // Create the web server instance
  webServer.reset(new CrossPointWebServer());
  leaveRequested = false;
  webServer->setUploadCancelCheck(
      [](void* context) { return static_cast<CrossPointWebServerActivity*>(context)->checkUploadCancellation(); },
      this);
  webServer->begin();

  if (webServer->isRunning()) {
    state = WebServerActivityState::SERVER_RUNNING;
    lastWifiBars = isApMode ? 0 : barsForRssi(WiFi.RSSI(), 0);

    // Force an immediate render since we're transitioning from a subactivity
    // that had its own rendering task. We need to make sure our display is shown.
    requestUpdate();
  } else {
    LOG_ERR("WEBACT", "ERROR: Failed to start web server!");
    webServer.reset();
    // Go back on error
    exitToOrigin();
  }
}

bool CrossPointWebServerActivity::checkUploadCancellation() {
  if (leaveRequested) return true;
  // Only poll here while a multipart body blocks the main loop. HalGPIO::update
  // already re-samples pending physical-button debounce; no extra delay needed.
  mappedInput.update();
  leaveRequested = exitRequested() || mappedInput.isPressed(MappedInputManager::Button::Back);
  return leaveRequested;
}

void CrossPointWebServerActivity::exitToOrigin() {
  if (networkBootReady) {
    if (returnBookPath.empty()) {
      silentRestart();
    } else {
      silentRestartToReader();
    }
    return;
  }

  if (returnBookPath.empty()) {
    onGoHome();
    return;
  }

  // A portal rename updates the persisted resume path while this activity's
  // original return path still contains the old filename.
  if (!Storage.exists(returnBookPath.c_str()) && !APP_STATE.openEpubPath.empty() &&
      Storage.exists(APP_STATE.openEpubPath.c_str())) {
    returnBookPath = APP_STATE.openEpubPath;
  }
  activityManager.goToReader(returnBookPath, true);
}

void CrossPointWebServerActivity::stopWebServer() {
  if (webServer && webServer->isRunning()) {
    webServer->stop();
  }
  webServer.reset();
}

void CrossPointWebServerActivity::loop() {
  if ((state == WebServerActivityState::SERVER_RUNNING || state == WebServerActivityState::AP_STARTING) &&
      exitRequested()) {
    exitToOrigin();
    return;
  }

  // Handle different states
  if (state == WebServerActivityState::SERVER_RUNNING) {
    // Handle DNS requests for captive portal (AP mode only)
    if (isApMode && dnsServer) {
      dnsServer->processNextRequest();
    }

    // STA mode: Monitor WiFi connection health
    if (!isApMode && webServer && webServer->isRunning()) {
      static unsigned long lastWifiCheck = 0;
      if (millis() - lastWifiCheck > 2000) {  // Check every 2 seconds
        lastWifiCheck = millis();
        const wl_status_t wifiStatus = WiFi.status();
        // Driver auto-reconnect handles retries; abandon (via onGoHome) only
        // after WIFI_ABANDON_MS, otherwise the activity freezes on a blip.
        bool repaint = false;
        if (wifiStatus != WL_CONNECTED) {
          if (consecutiveDisconnects == 0) {
            firstDisconnectAt = millis();
            repaint = true;
          }
          consecutiveDisconnects++;
          LOG_DBG("WEBACT", "WiFi not connected (status=%d, consecutive=%d, total=%lu ms)", wifiStatus,
                  consecutiveDisconnects, millis() - firstDisconnectAt);
          if (millis() - firstDisconnectAt > WIFI_ABANDON_MS) {
            LOG_DBG("WEBACT", "WiFi unavailable for >%lu s; returning to network selection", WIFI_ABANDON_MS / 1000UL);
            state = WebServerActivityState::SHUTTING_DOWN;
            onGoHome();
            return;
          }
        } else {
          if (consecutiveDisconnects > 0) {
            LOG_DBG("WEBACT", "WiFi recovered after %d failed checks (%lu ms)", consecutiveDisconnects,
                    millis() - firstDisconnectAt);
            repaint = true;
          }
          consecutiveDisconnects = 0;
          firstDisconnectAt = 0;
          const int rssi = WiFi.RSSI();
          if (rssi < -75) {
            LOG_DBG("WEBACT", "Warning: Weak WiFi signal: %d dBm", rssi);
          }
          const int bars = barsForRssi(rssi, lastWifiBars);
          if (bars != lastWifiBars) {
            lastWifiBars = bars;
            repaint = true;
          }
        }
        if (repaint) requestUpdate();
      }
    }

    // Handle web server requests while keeping input responsive.
    if (webServer && webServer->isRunning()) {
      const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;

      // Log if there's a significant gap between handleClient calls (>100ms)
      if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
        LOG_DBG("WEBACT", "WARNING: %lu ms gap since last handleClient", timeSinceLastHandleClient);
      }

      // Process a batch of HTTP requests, then return to the main loop, which
      // skips its delay while the server runs. Input is polled only there:
      // every update() clears the previous poll's one-shot touch events
      // (touch-down, release, completed two-finger swipes), so polling here
      // dropped them before the ActivityManager's edge-slide and two-finger
      // gesture handling could see them. The batch matches the old poll
      // interval, so exit buttons stay as responsive as before.
      constexpr int MAX_ITERATIONS = 64;
      for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); i++) {
        webServer->handleClient();
        if (leaveRequested) {
          // The callback only cancels I/O. Navigate after the server unwinds.
          exitToOrigin();
          return;
        }
      }
      lastHandleClientTime = millis();
    }
  }
}

void CrossPointWebServerActivity::render(RenderLock&&) {
  // Only render our own UI when server is running
  // Subactivities handle their own rendering
  if (state == WebServerActivityState::SERVER_RUNNING || state == WebServerActivityState::AP_STARTING) {
    renderer.clearScreen();
    const auto pageHeight = renderer.getScreenHeight();

    if (state == WebServerActivityState::SERVER_RUNNING) {
      renderServerRunning();
    } else {
      renderHeader();
      const auto height = renderer.getLineHeight(UI_10_FONT_ID);
      const auto top = (pageHeight - height) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_STARTING_HOTSPOT));
    }
    renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
  }
}

void CrossPointWebServerActivity::renderHeader() const {
  const char* title = isApMode ? tr(STR_HOTSPOT_MODE) : tr(STR_FILE_TRANSFER);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::drawCompact(renderer, title);
  } else {
    CompactHeader::drawTitle(renderer, title);
  }
}

bool CrossPointWebServerActivity::exitRequested() const {
  return TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
         mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture();
}

void CrossPointWebServerActivity::renderServerRunning() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int pageWidth = safe.width;

  renderHeader();
  const int subHeaderTop = CompactHeader::contentTop(renderer);
  GUI.drawSubHeader(renderer, Rect{safe.x, subHeaderTop, pageWidth, metrics.tabBarHeight}, connectedSSID.c_str());

  if (!isApMode) {
    renderWifiIndicator(subHeaderTop);
  }

  int startY = subHeaderTop + metrics.tabBarHeight + metrics.verticalSpacing * 2;
  int height10 = renderer.getLineHeight(UI_10_FONT_ID);
  if (isApMode) {
    // AP mode display
    renderer.drawText(UI_10_FONT_ID, safe.x + metrics.contentSidePadding, startY, tr(STR_CONNECT_WIFI_HINT), true,
                      EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    // Show QR code for Wifi
    // follows spec at https://github.com/zxing/zxing/wiki/Barcode-Contents#wi-fi-network-config-android-ios-11
    const std::string wifiConfig = std::string("WIFI:T:nopass;S:") + connectedSSID + ";;";
    const Rect qrBoundsWifi(safe.x + metrics.contentSidePadding, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBoundsWifi, wifiConfig);

    // Show network name
    renderer.drawText(UI_10_FONT_ID, safe.x + metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing,
                      startY + 80, connectedSSID.c_str());

    startY += QR_CODE_HEIGHT + 2 * metrics.verticalSpacing;

    // Show primary URL (hostname)
    renderer.drawText(UI_10_FONT_ID, safe.x + metrics.contentSidePadding, startY, tr(STR_OPEN_URL_HINT), true,
                      EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    std::string hostnameUrl = std::string("http://") + AP_HOSTNAME + ".local/";
    std::string ipUrl = tr(STR_OR_HTTP_PREFIX) + connectedIP + "/";

    // Show QR code for URL
    const Rect qrBoundsUrl(safe.x + metrics.contentSidePadding, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBoundsUrl, hostnameUrl);

    // Show IP address as fallback
    renderer.drawText(UI_10_FONT_ID, safe.x + metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing,
                      startY + 80, hostnameUrl.c_str());
    renderer.drawText(SMALL_FONT_ID, safe.x + metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing,
                      startY + 100, ipUrl.c_str());
  } else {
    startY += metrics.verticalSpacing * 2;

    // STA mode display (original behavior)
    // std::string ipInfo = "IP Address: " + connectedIP;
    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_OPEN_URL_HINT), true, EpdFontFamily::BOLD);
    startY += height10;
    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_SCAN_QR_HINT), true, EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    // Show QR code for URL
    std::string webInfo = "http://" + connectedIP + "/";
    const Rect qrBounds(safe.x + (pageWidth - QR_CODE_WIDTH) / 2, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBounds, webInfo);
    startY += QR_CODE_HEIGHT + metrics.verticalSpacing * 2;

    // Show web server URL prominently
    renderer.drawCenteredText(UI_10_FONT_ID, startY, webInfo.c_str(), true);
    startY += height10 + 5;

    // Also show hostname URL
    std::string hostnameUrl = std::string(tr(STR_OR_HTTP_PREFIX)) + AP_HOSTNAME + ".local/";
    renderer.drawCenteredText(SMALL_FONT_ID, startY, hostnameUrl.c_str(), true);
  }

  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_EXIT)), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CrossPointWebServerActivity::renderWifiIndicator(int subHeaderTop) const {
  constexpr int BAR_COUNT = 4;
  constexpr int BAR_WIDTH = 4;
  constexpr int BAR_GAP = 2;
  constexpr int ICON_HEIGHT = 14;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int iconWidth = BAR_COUNT * BAR_WIDTH + (BAR_COUNT - 1) * BAR_GAP;
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int iconRight = safe.x + safe.width - metrics.contentSidePadding;
  const int iconLeft = iconRight - iconWidth;
  const int iconBottom = subHeaderTop + metrics.tabBarHeight - metrics.verticalSpacing;

  const bool wifiUp = (WiFi.status() == WL_CONNECTED) && (consecutiveDisconnects == 0);
  if (wifiUp) {
    for (int i = 0; i < BAR_COUNT; i++) {
      const int barHeight = (i + 1) * ICON_HEIGHT / BAR_COUNT;
      const int x = iconLeft + i * (BAR_WIDTH + BAR_GAP);
      const int y = iconBottom - barHeight;
      if (i < lastWifiBars) {
        renderer.fillRect(x, y, BAR_WIDTH, barHeight, true);
      } else {
        renderer.drawRect(x, y, BAR_WIDTH, barHeight, true);
      }
    }
  } else {
    const int xSize = ICON_HEIGHT;
    const int x0 = iconRight - xSize;
    const int y0 = iconBottom - xSize;
    renderer.drawLine(x0, y0, x0 + xSize, y0 + xSize, 2, true);
    renderer.drawLine(x0, y0 + xSize, x0 + xSize, y0, 2, true);
  }
}
