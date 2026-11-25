#include "PluginHubInstallActivity.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include <cstdio>
#include <utility>

#include "activities/util/ConfirmationActivity.h"
#include "components/CatalogScreens.h"
#include "util/PluginBundle.h"
#include "util/PluginLocations.h"

namespace {
constexpr const char* PLUGIN_HUB_BASE_URL = "https://pluginhub.crosspointreader.com/";
constexpr const char* PLUGIN_HUB_DIR = "/.crosspoint/plugins/pluginhub";
constexpr const char* PLUGIN_HUB_FILES[] = {"manifest.json", "device.json", "plugin.js", "README.md"};

}  // namespace

PluginHubInstallActivity::PluginHubInstallActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   const bool showConfirmation)
    : CatalogActivity("PluginHubInstall", renderer, mappedInput), showConfirmation(showConfirmation) {}

bool PluginHubInstallActivity::isAvailable() {
  const std::string dir = PluginLocations::findPluginDirOnDisk("pluginhub");
  return !dir.empty() && !PluginLocations::isPluginQuarantined(dir) && Storage.exists((dir + "/device.json").c_str());
}

void PluginHubInstallActivity::onEnter() {
  RenderLock lock;
  CatalogActivity::onEnter();

  if (!showConfirmation) {
    state = State::CHECK_WIFI;
    statusMessage = tr(STR_CHECKING_WIFI);
    requestUpdate();
    checkAndConnectWifi();
    return;
  }

  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_PLUGIN_HUB),
                                                              tr(STR_PLUGIN_HUB_DESCRIPTION));
  if (!confirmation) {
    LOG_ERR("PHUB", "OOM: install confirmation");
    fail(StrId::STR_MEMORY_ERROR);
    return;
  }

  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    RenderLock lock;
    if (result.isCancelled) {
      finish();
      return;
    }
    state = State::CHECK_WIFI;
    statusMessage = tr(STR_CHECKING_WIFI);
    requestUpdate();
    checkAndConnectWifi();
  });
}

void PluginHubInstallActivity::startBrowse() {
  beginDownload(tr(STR_PLUGIN_HUB));
  pendingInstall = true;
}

HttpDownloader::DownloadError PluginHubInstallActivity::installPluginHub() {
  if ((!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) ||
      (!Storage.exists(PluginLocations::ROOTS[0]) && !Storage.mkdir(PluginLocations::ROOTS[0]))) {
    LOG_ERR("PHUB", "Unable to create plugin root");
    return HttpDownloader::FILE_ERROR;
  }

  const auto cancelled = [](void* context) {
    auto* self = static_cast<PluginHubInstallActivity*>(context);
    self->onDownloadProgress(0, 0);
    return self->cancelDownload;
  };
  if (!PluginBundle::prepare(PLUGIN_HUB_DIR, cancelled, this)) {
    LOG_ERR("PHUB", "Unable to stage Plugin Hub");
    return cancelDownload ? HttpDownloader::ABORTED : HttpDownloader::FILE_ERROR;
  }
  const std::string stage = PluginBundle::stagingPath(PLUGIN_HUB_DIR);
  constexpr unsigned fileCount = sizeof(PLUGIN_HUB_FILES) / sizeof(PLUGIN_HUB_FILES[0]);
  unsigned fileIndex = 0;
  for (const char* filename : PLUGIN_HUB_FILES) {
    {
      RenderLock lock;
      char step[64];
      std::snprintf(step, sizeof(step), tr(STR_ASSET_STEP_FORMAT), ++fileIndex, fileCount);
      statusMessage = step;
      statusMessage += " - ";
      statusMessage += filename;
      downloadProgress = downloadTotal = 0;
    }
    // Show every file transition, even when the next transfer finishes quickly.
    requestUpdateAndWait();
    const std::string url = std::string(PLUGIN_HUB_BASE_URL) + filename;
    const auto result = downloadFile(url, stage + "/" + filename, {}, {}, {}, HttpDownloader::Transport::ESP_HTTP);
    if (result != HttpDownloader::OK) {
      PluginBundle::discard(PLUGIN_HUB_DIR);
      return result;
    }
  }
  if (!PluginBundle::publish(PLUGIN_HUB_DIR)) {
    LOG_ERR("PHUB", "Unable to publish Plugin Hub");
    return HttpDownloader::FILE_ERROR;
  }
  return HttpDownloader::OK;
}

void PluginHubInstallActivity::loop() {
  {
    RenderLock lock;
    UiListActivity::loop();
    if (!pendingInstall) return;
    pendingInstall = false;
  }
  requestUpdateAndWait();
  const auto result = installPluginHub();
  RenderLock lock;
  finishDownload(result);
}

void PluginHubInstallActivity::wifiSelectionCancelled() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void PluginHubInstallActivity::downloadFinished(const bool cancelled) {
  if (cancelled) {
    finish();
    return;
  }
  state = State::DONE;
  requestUpdate();
}

bool PluginHubInstallActivity::handleCustomInput() {
  if (CatalogActivity::handleCustomInput()) return true;
  if (state != State::DONE) return false;

  int x = 0, y = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
  return true;
}

void PluginHubInstallActivity::onBackButton() { finish(); }

void PluginHubInstallActivity::buildScreen(UiScreen& screen) {
  screenHeader(screen, tr(STR_PLUGIN_HUB));
  if (buildStatusScreen(screen, true, true)) return;

  if (state == State::DONE) {
    catalogCenteredBlock(screen, {{tr(STR_DOWNLOAD_COMPLETE), true}, {tr(STR_PLUGIN_HUB_DESCRIPTION)}});
  }
}
