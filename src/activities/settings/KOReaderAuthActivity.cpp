#include "KOReaderAuthActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/StatsUploadClient.h"
#include "network/WifiUtils.h"

void KOReaderAuthActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = FAILED;
      errorMessage = tr(STR_CONNECTION_FAILED);
    }
    requestUpdate();
    return;
  }

  WiFi.setSleep(false);
  LOG_DBG("KOAuth", "WiFi sleep disabled for authentication");

  sdFontSystem.releaseForNetwork(renderer);

  {
    RenderLock lock(*this);
    state = AUTHENTICATING;
    statusMessage = mode == Mode::SIGN_UP ? tr(STR_CREATING_ACCOUNT) : tr(STR_AUTHENTICATING);
  }
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("KOSync", "Authentication screen could not be rendered before request");
    requestUpdate(true);
  }

  performAuthentication();
}

void KOReaderAuthActivity::performAuthentication() {
  const auto result = mode == Mode::SIGN_UP ? KOReaderSyncClient::createUser() : KOReaderSyncClient::authenticate();
  if (result == KOReaderSyncClient::OK && !KOREADER_STORE.usesCrossPointSyncServer()) {
    // Re-check on every sign-in so a server upgrade or URL change is noticed.
    StatsUploadClient client;
    client.probe(true);  // Credentials were just accepted, so this is the real server.
  }

  {
    RenderLock lock(*this);
    if (result == KOReaderSyncClient::OK) {
      state = SUCCESS;
      statusMessage = mode == Mode::SIGN_UP ? tr(STR_ACCOUNT_CREATED) : tr(STR_AUTH_SUCCESS);
    } else {
      state = FAILED;
      errorMessage =
          result == KOReaderSyncClient::USER_EXISTS ? tr(STR_USERNAME_TAKEN) : KOReaderSyncClient::errorString(result);
    }
  }
  requestUpdate();
}

void KOReaderAuthActivity::onEnter() {
  Activity::onEnter();
  sdFontSystem.releaseLoadedFont(renderer);

  // Check if already connected
  if (hasActiveStationWifiConnection()) {
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderAuthActivity::onExit() {
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  // Authentication launches from minimal network boot, so restore the full
  // app state even if setup failed before WiFi was started.
  silentRestart();
}

void KOReaderAuthActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageHeight = renderer.getScreenHeight();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  const char* title = mode == Mode::SIGN_UP ? tr(STR_SIGN_UP) : tr(STR_AUTHENTICATE);
  if ((state == SUCCESS || state == FAILED) && mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, title, false);
  } else {
    GUI.drawHeader(renderer, header, title);
  }
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == AUTHENTICATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
  } else if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top,
                              mode == Mode::SIGN_UP ? tr(STR_ACCOUNT_CREATED) : tr(STR_AUTH_SUCCESS), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, tr(STR_SYNC_READY));
    // Tell the user up front what this server can receive besides progress.
    const auto support = KOREADER_STORE.getServerSupport();
    if (support != SyncServerSupport::UNKNOWN) {
      const Rect textArea{screen.x + metrics.contentSidePadding, screen.y,
                          screen.width - metrics.contentSidePadding * 2, screen.height};
      UITheme::drawCenteredWrappedText(
          renderer, textArea, UI_10_FONT_ID, top + (height + 10) * 2,
          support == SyncServerSupport::SUPPORTED ? tr(STR_SERVER_EXTRAS_AVAILABLE) : tr(STR_SERVER_PROGRESS_ONLY), 2,
          true, EpdFontFamily::REGULAR, 4);
    }
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, mode == Mode::SIGN_UP ? tr(STR_SIGNUP_FAILED) : tr(STR_AUTH_FAILED),
                              true, EpdFontFamily::BOLD);
    const Rect textArea{screen.x + metrics.contentSidePadding, screen.y, screen.width - metrics.contentSidePadding * 2,
                        screen.height};
    UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, top + height + 10, errorMessage.c_str(), 3,
                                     true, EpdFontFamily::REGULAR, 4);
  }

  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
}

void KOReaderAuthActivity::loop() {
  if (state == SUCCESS || state == FAILED) {
    const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
    if (TouchHeaderBackButton::wasTapped(mappedInput, header) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finishAfterBackPress();
      return;
    }

    int x = 0;
    int y = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
  }
}
