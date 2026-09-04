#include "FontDownloadActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_rom_crc.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "Memory.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;

namespace {

constexpr fui::ActionId ACTION_ROW = 1;

Rect downloadCancelButtonRect(const GfxRenderer& renderer, const ThemeMetrics& metrics,
                              const int downloadAttemptTotal) {
  constexpr int kButtonHeight = 44;
  constexpr int kMaxButtonWidth = 240;
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int centerY = (pageHeight - lineHeight) / 2;
  const int barY = centerY + (downloadAttemptTotal > 1 ? lineHeight : metrics.verticalSpacing);
  const int width = std::min(kMaxButtonWidth, pageWidth - metrics.contentSidePadding * 2);
  const int y = std::min(barY + metrics.progressBarHeight + lineHeight + metrics.verticalSpacing,
                         pageHeight - metrics.buttonHintsHeight - kButtonHeight - metrics.verticalSpacing);
  return Rect{(pageWidth - width) / 2, y, width, kButtonHeight};
}

constexpr int FONT_DOWNLOAD_MAX_ATTEMPTS = 3;
constexpr int FONT_MANIFEST_MAX_ATTEMPTS = 5;
constexpr uint32_t FONT_DOWNLOAD_RETRY_DELAY_MS = 500;

bool isGitHubReleaseAssetBaseUrl(const std::string& baseUrl) {
  return baseUrl.rfind("https://github.com/", 0) == 0 && baseUrl.find("/releases/download/") != std::string::npos;
}

std::string urlEncodePathSegment(const std::string& segment) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(segment.size());
  for (const unsigned char c : segment) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded.push_back(static_cast<char>(c));
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[c >> 4]);
      encoded.push_back(kHex[c & 0x0F]);
    }
  }
  return encoded;
}

std::string buildFontDownloadUrl(const std::string& baseUrl, const std::string& manifestFileName) {
  std::string assetName = manifestFileName;
  if (isGitHubReleaseAssetBaseUrl(baseUrl)) {
    // GitHub release uploads expose spaces in asset names as dots. Keep the
    // manifest/local filename untouched, but request the actual asset URL.
    std::replace(assetName.begin(), assetName.end(), ' ', '.');
  }
  return baseUrl + urlEncodePathSegment(assetName);
}

std::string normalizedFontFamilyName(const std::string& familyName) {
  std::string normalized;
  normalized.reserve(familyName.size());
  for (const unsigned char c : familyName) {
    if (std::isalnum(c)) {
      normalized.push_back(static_cast<char>(std::tolower(c)));
    }
  }
  return normalized;
}

bool parseManifestPointSize(const char* filename, uint8_t& outPointSize) {
  if (!filename) return false;
  static constexpr char kExt[] = ".cpfont";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  const size_t nameLen = strlen(filename);
  if (nameLen <= kExtLen) return false;
  if (strcmp(filename + nameLen - kExtLen, kExt) != 0) return false;

  char base[128];
  const size_t baseLen = nameLen - kExtLen;
  if (baseLen == 0 || baseLen >= sizeof(base)) return false;
  memcpy(base, filename, baseLen);
  base[baseLen] = '\0';

  const char* sizeStr = strrchr(base, '_');
  if (!sizeStr || sizeStr[1] == '\0') return false;
  sizeStr++;

  char* endPtr = nullptr;
  const long parsed = strtol(sizeStr, &endPtr, 10);
  if (endPtr == sizeStr || *endPtr != '\0' || parsed < 1 || parsed > 255) return false;

  outPointSize = static_cast<uint8_t>(parsed);
  return true;
}

int fontListRowHeight(const GfxRenderer& renderer, const ThemeMetrics& metrics) {
  constexpr int kLineGap = 2;
  constexpr int kVerticalPadding = 10;
  const int requiredHeight = renderer.getLineHeight(UI_10_FONT_ID) + renderer.getLineHeight(SMALL_FONT_ID) * 2 +
                             kLineGap * 2 + kVerticalPadding;
  return std::max(metrics.listWithSubtitleRowHeight, requiredHeight);
}

}  // namespace

FontDownloadActivity::FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FontDownload", renderer, mappedInput),
      fontInstaller_(sdFontSystem.registry()),
      uiTarget_(makeUiTarget(renderer)),
      app_(uiTarget_, uiTarget_.deviceContext()) {}

void FontDownloadActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FontDownloadActivity*>(user);
  if (self->state_ != FAMILY_LIST) return;
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->listItemCount())) return;
  self->selectedIndex_ = event.value;
  // Activation starts a download or opens the delete prompt; a lingering
  // flash would gray an unrelated row.
  self->app_.clearTapFlash();
  self->activateSelected();  // ends with requestUpdateAndWait itself
}

bool FontDownloadActivity::pollCancelInput(const bool includeDownloadScreenButton) {
  if (cancelRequested_) return true;

  mappedInput.update();
  if (mappedInput.wasHomeGesture()) {
    goHomeRequested_ = true;
    cancelRequested_ = true;
  }
  if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancelRequested_ = true;
  }
  if (mappedInput.hasTouchHardware()) {
    if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
      cancelRequested_ = true;
    }
    if (includeDownloadScreenButton) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const Rect cancelButton = downloadCancelButtonRect(renderer, metrics, downloadAttemptTotal_);
      if (mappedInput.wasTapInRect(cancelButton.x, cancelButton.y, cancelButton.width, cancelButton.height)) {
        cancelRequested_ = true;
      }
    }
  }
  return cancelRequested_;
}

// --- Lifecycle ---

void FontDownloadActivity::onEnter() {
  Activity::onEnter();
  // Free the whole SD font registry, not just the active glyph font, before the
  // network + manifest work. With many families installed the registry, the
  // parsed manifest, and the families_ list would otherwise all be resident at
  // once and exhaust the heap, aborting during manifest parse. Matches the
  // pre-network release done by the KOReader sync/auth activities.
  sdFontSystem.releaseForNetwork(renderer);
  uiReady_ = false;
  visibleRows_ = 1;
  topIndex_ = 0;
  applySharedUiTheme(app_, uiTarget_);
  app_.on(ACTION_ROW, &FontDownloadActivity::onRowEvent, this);
  app_.setScreen(&FontDownloadActivity::listScreen, this);
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onExit() {
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    if (fontsChanged_) {
      silentRestartAfterNetwork();
    } else {
      WiFi.mode(WIFI_OFF);
    }
  }

  sdFontSystem.ensureLoaded(renderer);
  sdFontSystem.releaseRegistry();
}

void FontDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state_ = LOADING_MANIFEST;
  }
  requestUpdateAndWait();

  if (!fetchAndParseManifest()) {
    if (goHomeRequested_) {
      onGoHome();
      return;
    }
    if (cancelRequested_) {
      finishAfterBackPress();
      return;
    }
    {
      RenderLock lock(*this);
      state_ = ERROR;
    }
    return;
  }

  {
    RenderLock lock(*this);
    state_ = FAMILY_LIST;
    selectedIndex_ = 0;
  }
}

// --- Manifest fetching ---

bool FontDownloadActivity::fetchAndParseManifest() {
  // Download manifest to a temp file on SD card to avoid holding both
  // TLS buffers and the full JSON string in RAM simultaneously.
  static constexpr const char* MANIFEST_TMP = "/fonts_manifest.tmp";

  baseUrl_.clear();
  clearManifestFamilies();
  cancelRequested_ = false;
  goHomeRequested_ = false;

  // Poll the Cancel (Back) button while the manifest downloads so a slow or
  // failing network can be backed out of. HttpDownloader checks shouldCancel on
  // every read-loop iteration, and we re-check it between retry attempts so the
  // retry delays do not swallow the press.
  HttpDownloader::DownloadOptions manifestOptions;
  manifestOptions.shouldCancel = [this]() { return pollCancelInput(false); };
  // The font list is fetched again after individual updates. Release registry
  // memory on every manifest load, not only when entering Manage Fonts.
  sdFontSystem.releaseForNetwork(renderer);

  Storage.remove(MANIFEST_TMP);
  HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
  for (int attempt = 1; attempt <= FONT_MANIFEST_MAX_ATTEMPTS; ++attempt) {
    if (attempt > 1) {
      LOG_DBG("FONT", "Retrying font manifest download (%d/%d)", attempt, FONT_MANIFEST_MAX_ATTEMPTS);
      delay(FONT_DOWNLOAD_RETRY_DELAY_MS);
    }
    result = HttpDownloader::downloadToFile(FONT_MANIFEST_URL, MANIFEST_TMP, nullptr, &cancelRequested_, "", "",
                                            manifestOptions);
    if (result == HttpDownloader::OK || result == HttpDownloader::ABORTED) break;
    if (manifestOptions.shouldCancel()) {
      result = HttpDownloader::ABORTED;
      break;
    }
    LOG_ERR("FONT", "Font manifest download attempt failed (%d/%d, error=%d)", attempt, FONT_MANIFEST_MAX_ATTEMPTS,
            result);
  }
  if (result != HttpDownloader::OK) {
    Storage.remove(MANIFEST_TMP);
    if (result == HttpDownloader::ABORTED) {
      LOG_INF("FONT", "Font list loading cancelled");
      return false;
    }
    LOG_ERR("FONT", "Failed to fetch manifest from %s", FONT_MANIFEST_URL);
    errorMessage_ = "Failed to fetch font list";
    return false;
  }

  // HTTP client is now closed — TLS buffers freed. Parse JSON from file.
  FsFile manifestFile;
  if (!Storage.openFileForRead("FONT", MANIFEST_TMP, manifestFile)) {
    LOG_ERR("FONT", "Failed to open temp manifest");
    Storage.remove(MANIFEST_TMP);
    errorMessage_ = "Failed to read font list";
    return false;
  }

  // The parsed manifest and the installed-font registry are each large when many
  // families are installed. Keep the JsonDocument in its own scope and defer
  // loading the registry until after it is freed (see second pass below), so the
  // two are never resident at the same time. Their coexistence here is what
  // aborted during parse on low-heap devices with many SD fonts installed.
  {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, manifestFile);
    manifestFile.close();
    Storage.remove(MANIFEST_TMP);

    if (err) {
      LOG_ERR("FONT", "Manifest parse error: %s", err.c_str());
      errorMessage_ = "Invalid font manifest";
      return false;
    }

    int version = doc["version"] | 0;
    if (version != FONTS_MANIFEST_VERSION) {
      LOG_ERR("FONT", "Unsupported manifest version: %d", version);
      errorMessage_ = "Unsupported manifest version";
      return false;
    }

    baseUrl_ = doc["baseUrl"] | "";
    families_.clear();

    std::vector<ManifestFamily> parsedFamilies;
    JsonArray familiesArr = doc["families"].as<JsonArray>();
    parsedFamilies.reserve(familiesArr.size());

    // ArduinoJson owns a second copy of every manifest string. Consume the
    // array from the front and remove each family after copying it so those
    // strings are released before the next family's vectors grow. Keeping the
    // whole JSON tree alive here can exhaust the heap partway through the list.
    while (!familiesArr.isNull() && familiesArr.size() > 0) {
      JsonObject fObj = familiesArr[0];
      ManifestFamily family;
      family.name = fObj["name"] | "";
      family.description = fObj["description"] | "";
      family.languages = fObj["languages"] | "";

      family.totalSize = 0;
      JsonArray filesArr = fObj["files"].as<JsonArray>();
      family.files.reserve(filesArr.size());
      for (JsonObject fileObj : filesArr) {
        ManifestFile file;
        file.name = fileObj["name"] | "";
        file.size = fileObj["size"] | 0;
        if (!parseManifestPointSize(file.name.c_str(), file.pointSize)) {
          LOG_ERR("FONT", "Malformed manifest file entry: invalid filename %s", file.name.c_str());
          errorMessage_ = "Invalid font manifest";
          return false;
        }

        if (!CrossPointSettings::isSdFontPointSizeAllowedForRange(file.pointSize, SETTINGS.sdFontSizeRange)) {
          continue;
        }

        if (!fileObj["crc32"].is<uint32_t>()) {
          LOG_ERR("FONT", "Malformed manifest file entry: missing or invalid crc32 for %s", file.name.c_str());
          errorMessage_ = "Invalid font manifest";
          return false;
        }
        file.crc32 = fileObj["crc32"].as<uint32_t>();

        family.totalSize += file.size;
        family.files.push_back(std::move(file));
      }

      if (family.files.empty()) {
        familiesArr.remove(0);
        continue;
      }

      parsedFamilies.push_back(std::move(family));
      familiesArr.remove(0);
    }

    families_.swap(parsedFamilies);
  }  // JsonDocument freed here, before the registry is loaded below.

  // Second pass: load the installed-font registry and resolve installed/update
  // state now that the manifest JsonDocument has been released, keeping peak
  // heap usage down on devices with many SD fonts installed.
  if (!fontInstaller_.refreshRegistry()) {
    LOG_ERR("FONT", "Not enough contiguous heap to scan installed fonts (free=%u maxAlloc=%u)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    errorMessage_ = tr(STR_MEMORY_ERROR);
    return false;
  }
  for (auto& family : families_) {
    resolveInstalledFamilyName(family);
  }

  if (!rebuildListItems()) {
    // Do not leave FAMILY_LIST with a populated manifest but no renderable
    // rows: button navigation would still act on families the UI cannot show.
    clearManifestFamilies();
    errorMessage_ = tr(STR_MEMORY_ERROR);
    return false;
  }

  LOG_DBG("FONT", "Manifest loaded: %zu families", families_.size());
  return true;
}

const SdCardFontFamilyInfo* FontDownloadActivity::findInstalledFamilyCandidate(const char* familyName) const {
  const auto& registry = sdFontSystem.registry();
  const SdCardFontFamilyInfo* exact = registry.findFamily(familyName);
  if (exact) return exact;

  const std::string target = normalizedFontFamilyName(familyName);
  for (const auto& family : registry.getFamilies()) {
    if (normalizedFontFamilyName(family.name) == target) {
      return &family;
    }
  }
  return nullptr;
}

bool FontDownloadActivity::installedFilesMatch(const char* familyName, const std::vector<ManifestFile>& files,
                                               bool& hasUpdate, std::string* resolvedFamilyName) const {
  hasUpdate = false;
  const SdCardFontFamilyInfo* installedFamily = findInstalledFamilyCandidate(familyName);
  if (!installedFamily) return false;

  for (const auto& file : files) {
    const SdCardFontFileInfo* installedFile = installedFamily->findFile(file.pointSize);
    if (!installedFile) {
      hasUpdate = true;
      continue;
    }

    FsFile f;
    if (!Storage.openFileForRead("FONT", installedFile->path, f)) {
      hasUpdate = true;
      continue;
    }

    const size_t actual = f.fileSize();
    f.close();
    if (actual != file.size) {
      hasUpdate = true;
    }
  }
  if (resolvedFamilyName) {
    *resolvedFamilyName = installedFamily->name;
  }
  return true;
}

void FontDownloadActivity::resolveInstalledFamilyName(ManifestFamily& family) const {
  bool hasUpdate = false;
  std::string resolvedName;
  if (installedFilesMatch(family.name.c_str(), family.files, hasUpdate, &resolvedName)) {
    family.installName = resolvedName;
    family.installed = true;
    family.hasUpdate = hasUpdate;
    return;
  }

  family.installName = family.name;
  family.installed = false;
  family.hasUpdate = false;
}

// --- Download ---

void FontDownloadActivity::clearManifestFamilies() {
  // Storage remains allocated for the activity lifetime, but none of its
  // borrowed family-string pointers may be used until the next manifest fills
  // it again.
  listItemCount_ = 0;
  std::vector<ManifestFamily>().swap(families_);
}

void FontDownloadActivity::updateAll() {
  cancelRequested_ = false;
  hasRetryFamily_ = false;
  retryFamily_ = ManifestFamily();
  manifestReloadNeeded_ = true;

  while (true) {
    int nextFamilyIndex = -1;
    for (int i = 0; i < static_cast<int>(families_.size()); i++) {
      const auto& family = families_[i];
      if (family.hasUpdate) {
        nextFamilyIndex = i;
        break;
      }
    }

    if (nextFamilyIndex < 0) {
      RenderLock lock(*this);
      state_ = COMPLETE;
      manifestReloadNeeded_ = false;
      return;
    }

    ManifestFamily family = families_[nextFamilyIndex];
    activeDownloadFamilyName_ = family.name;
    selectedIndex_ = 0;
    clearManifestFamilies();

    downloadFamily(family);
    if (state_ == ERROR) {
      retryFamily_ = family;
      hasRetryFamily_ = true;
      return;
    }
    if (cancelRequested_ || state_ == FAMILY_LIST) {
      return;
    }

    family = ManifestFamily();
    {
      RenderLock lock(*this);
      state_ = LOADING_MANIFEST;
      errorMessage_.clear();
      errorHint_.clear();
    }
    requestUpdateAndWait();

    if (!fetchAndParseManifest()) {
      if (cancelRequested_) {
        finishAfterBackPress();
        return;
      }
      RenderLock lock(*this);
      state_ = ERROR;
      return;
    }
  }
}

bool FontDownloadActivity::showUpdateAllRow() const {
  for (const auto& f : families_) {
    if (f.hasUpdate) return true;
  }
  return false;
}

int FontDownloadActivity::specialRowCount() const { return showUpdateAllRow() ? 1 : 0; }

bool FontDownloadActivity::isUpdateAllRow(int index) const { return showUpdateAllRow() && index == 0; }

int FontDownloadActivity::listItemCount() const {
  return families_.empty() ? 0 : static_cast<int>(families_.size()) + specialRowCount();
}

size_t FontDownloadActivity::totalUpdateSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (f.hasUpdate) total += f.totalSize;
  }
  return total;
}

bool FontDownloadActivity::rebuildListItems() {
  const int count = listItemCount();
  if (count <= 0) {
    listItemCount_ = 0;
    return true;
  }

  const size_t required = static_cast<size_t>(count);
  if (required > listItemCapacity_) {
    // This activity-lifetime buffer avoids redraw-time allocation and has an
    // explicit low-memory fallback. The log reports its exact byte size.
    auto items = makeUniqueNoThrow<fui::ListItem[]>(required);
    if (!items) {
      LOG_ERR("FONT", "Failed to allocate %zu-byte font list (heap=%u maxAlloc=%u)", required * sizeof(fui::ListItem),
              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      listItemCount_ = 0;
      return false;
    }
    listItems_ = std::move(items);
    listItemCapacity_ = required;
  }

  size_t itemIndex = 0;
  if (showUpdateAllRow()) {
    char sizeLabel[32];
    formatSize(totalUpdateSize(), sizeLabel, sizeof(sizeLabel));
    snprintf(updateAllLabel_, sizeof(updateAllLabel_), "%s (%s)", tr(STR_UPDATE_ALL), sizeLabel);
    listItems_[itemIndex] = fui::ListItem{};
    listItems_[itemIndex].label = updateAllLabel_;
    listItems_[itemIndex].actionValue = static_cast<int16_t>(itemIndex);
    ++itemIndex;
  }

  for (const auto& family : families_) {
    fui::ListItem& item = listItems_[itemIndex];
    item = fui::ListItem{};
    item.label = family.name.c_str();
    if (!family.description.empty()) item.subtitle = family.description.c_str();
    if (family.hasUpdate) {
      item.value = tr(STR_UPDATE_AVAILABLE);
    } else if (family.installed) {
      item.value = tr(STR_INSTALLED);
      // Dimmed but still tappable (opens the delete prompt): visual-only
      // disabled state, the row stays enabled for hit registration.
      item.state = fui::StateDisabled;
    }
    item.actionValue = static_cast<int16_t>(itemIndex);
    ++itemIndex;
  }

  listItemCount_ = itemIndex;
  return true;
}

// Standard CRC32 matching zlib/Python zlib.crc32().
bool FontDownloadActivity::computeFileCrc32(const char* path, uint32_t& outCrc) {
  FsFile f;
  if (!Storage.openFileForRead("FONT", path, f)) {
    return false;
  }
  constexpr size_t BUF_SIZE = 128;
  uint8_t buf[BUF_SIZE];
  uint32_t crc = 0;
  while (f.available()) {
    const int n = f.read(buf, BUF_SIZE);
    if (n <= 0) break;
    crc = esp_rom_crc32_le(crc, buf, static_cast<uint32_t>(n));
  }
  outCrc = crc;
  return true;
}

void FontDownloadActivity::returnToFamilyList() {
  hasRetryFamily_ = false;
  retryFamily_ = ManifestFamily();
  activeDownloadFamilyName_.clear();

  if (manifestReloadNeeded_) {
    {
      RenderLock lock(*this);
      state_ = LOADING_MANIFEST;
      errorMessage_.clear();
      errorHint_.clear();
    }
    requestUpdateAndWait();

    if (!fetchAndParseManifest()) {
      if (cancelRequested_) {
        finishAfterBackPress();
        return;
      }
      RenderLock lock(*this);
      state_ = ERROR;
      return;
    }

    manifestReloadNeeded_ = false;
  }

  {
    RenderLock lock(*this);
    state_ = FAMILY_LIST;
    selectedIndex_ = 0;
  }
}

void FontDownloadActivity::downloadFamily(ManifestFamily& family) {
  const auto failDownload = [this, &family](const std::string& message, const std::string& hint) {
    fontInstaller_.refreshRegistry();
    bool hasUpdate = false;
    std::string resolvedName;
    family.installed = installedFilesMatch(family.installName.c_str(), family.files, hasUpdate, &resolvedName);
    family.hasUpdate = true;
    if (!resolvedName.empty()) {
      family.installName = resolvedName;
    }
    {
      RenderLock lock(*this);
      state_ = ERROR;
      errorMessage_ = message;
      errorHint_ = hint;
      downloadAttempt_ = 0;
      downloadAttemptTotal_ = 0;
    }
    requestUpdate(true);
  };

  activeDownloadFamilyName_ = family.name;
  downloadingFamilyIndex_ = -1;
  for (size_t i = 0; i < families_.size(); ++i) {
    if (&families_[i] == &family) {
      downloadingFamilyIndex_ = static_cast<int>(i);
      break;
    }
  }

  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    currentFileIndex_ = 0;
    currentFileTotal_ = family.files.size();
    fileProgress_ = 0;
    fileTotal_ = 0;
    downloadAttempt_ = 0;
    downloadAttemptTotal_ = 0;
    cancelRequested_ = false;
    goHomeRequested_ = false;
  }
  requestUpdateAndWait();

  if (!fontInstaller_.ensureFamilyDir(family.installName.c_str())) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to create font directory";
    return;
  }

  for (size_t i = 0; i < family.files.size(); i++) {
    const auto& file = family.files[i];

    {
      RenderLock lock(*this);
      currentFileIndex_ = i;
      fileProgress_ = 0;
      fileTotal_ = file.size;
    }
    requestUpdateAndWait();

    char destPath[192];
    FontInstaller::buildFontPath(family.installName.c_str(), file.name.c_str(), destPath, sizeof(destPath));
    char tempPath[208];
    snprintf(tempPath, sizeof(tempPath), "%s.tmp", destPath);

    if (Storage.exists(destPath)) {
      uint32_t existingCrc = 0;
      if (computeFileCrc32(destPath, existingCrc) && existingCrc == file.crc32 &&
          fontInstaller_.validateCpfontFile(destPath)) {
        {
          RenderLock lock(*this);
          fileProgress_ = file.size;
          fileTotal_ = file.size;
          downloadAttempt_ = 0;
          downloadAttemptTotal_ = 0;
        }
        requestUpdate(true);
        continue;
      }
    }

    std::string url = buildFontDownloadUrl(baseUrl_, file.name);

    HttpDownloader::DownloadOptions downloadOptions;
    downloadOptions.preservePartial = true;
    downloadOptions.resumePartial = true;
    // Poll Back and the touch Cancel controls from shouldCancel, which
    // HttpDownloader checks at the top of every read-loop iteration. The
    // progress callback is throttled to every 64KB / 250ms, so polling input
    // there dropped quick taps and forced the user to press Cancel repeatedly.
    downloadOptions.shouldCancel = [this]() { return pollCancelInput(true); };
    HttpDownloader::DownloadError result = HttpDownloader::HTTP_ERROR;
    for (int attempt = 1; attempt <= FONT_DOWNLOAD_MAX_ATTEMPTS; ++attempt) {
      {
        RenderLock lock(*this);
        downloadAttempt_ = attempt;
        downloadAttemptTotal_ = FONT_DOWNLOAD_MAX_ATTEMPTS;
      }
      if (attempt > 1) {
        LOG_DBG("FONT", "Retrying %s (%d/%d)", file.name.c_str(), attempt, FONT_DOWNLOAD_MAX_ATTEMPTS);
      }
      requestUpdateAndWait();
      if (attempt > 1) delay(FONT_DOWNLOAD_RETRY_DELAY_MS);

      result = HttpDownloader::downloadToFile(
          url, tempPath,
          [this](size_t downloaded, size_t total) {
            fileProgress_ = downloaded;
            fileTotal_ = total;
            requestUpdate(true);
          },
          &cancelRequested_, "", "", downloadOptions);
      if (result == HttpDownloader::ABORTED) {
        LOG_INF("FONT", "Download cancelled: %s", file.name.c_str());
        Storage.remove(tempPath);
        if (goHomeRequested_) {
          onGoHome();
          return;
        }
        // The Back release that confirmed the cancel would otherwise be seen by
        // the family list and treated as a request to leave the screen.
        mappedInput.suppressNextBackRelease();
        {
          RenderLock lock(*this);
          state_ = FAMILY_LIST;
          downloadAttempt_ = 0;
          downloadAttemptTotal_ = 0;
        }
        requestUpdate(true);
        return;
      }
      if (result == HttpDownloader::OK) {
        break;
      }
      LOG_ERR("FONT", "Download attempt failed: %s (%d/%d, error=%d)", file.name.c_str(), attempt,
              FONT_DOWNLOAD_MAX_ATTEMPTS, result);
    }

    if (result != HttpDownloader::OK) {
      LOG_ERR("FONT", "Download failed: %s (%d)", file.name.c_str(), result);
      failDownload(std::string(tr(STR_FONT_DOWNLOAD_INTERRUPTED)) + ": " + file.name, tr(STR_FONT_DOWNLOAD_RETRY_HINT));
      return;
    }

    uint32_t actualCrc = 0;
    if (!computeFileCrc32(tempPath, actualCrc)) {
      LOG_ERR("FONT", "Failed to open file for CRC check: %s", tempPath);
      Storage.remove(tempPath);
      failDownload("Could not verify downloaded file: " + file.name, tr(STR_FONT_DOWNLOAD_CHECKSUM_HINT));
      return;
    }
    if (actualCrc != file.crc32) {
      LOG_ERR("FONT", "CRC32 mismatch for %s: got %08x expected %08x", file.name.c_str(), actualCrc, file.crc32);
      Storage.remove(tempPath);
      failDownload("Downloaded file did not match: " + file.name, tr(STR_FONT_DOWNLOAD_CHECKSUM_HINT));
      return;
    }

    if (!fontInstaller_.validateCpfontFile(tempPath)) {
      LOG_ERR("FONT", "Invalid .cpfont: %s", tempPath);
      Storage.remove(tempPath);
      failDownload("Downloaded font file was invalid: " + file.name, tr(STR_FONT_DOWNLOAD_CHECKSUM_HINT));
      return;
    }

    char backupPath[208];
    snprintf(backupPath, sizeof(backupPath), "%s.bak", destPath);
    const bool hadExistingFile = Storage.exists(destPath);
    if (Storage.exists(backupPath)) {
      Storage.remove(backupPath);
    }
    if (hadExistingFile && !Storage.rename(destPath, backupPath)) {
      LOG_ERR("FONT", "Failed to back up existing font file: %s", destPath);
      Storage.remove(tempPath);
      failDownload("Could not replace existing font file: " + file.name, "");
      return;
    }
    if (!Storage.rename(tempPath, destPath)) {
      LOG_ERR("FONT", "Failed to install downloaded font file: %s", destPath);
      Storage.remove(tempPath);
      if (hadExistingFile) {
        Storage.rename(backupPath, destPath);
      }
      failDownload("Could not save downloaded font file: " + file.name, "");
      return;
    }
    if (!fontInstaller_.validateCpfontFile(destPath)) {
      LOG_ERR("FONT", "Installed .cpfont failed validation: %s", destPath);
      Storage.remove(destPath);
      if (hadExistingFile) {
        Storage.rename(backupPath, destPath);
      }
      failDownload("Downloaded font file was invalid: " + file.name, tr(STR_FONT_DOWNLOAD_CHECKSUM_HINT));
      return;
    }
    if (hadExistingFile) {
      Storage.remove(backupPath);
    }
    fontsChanged_ = true;
    currentFileIndex_++;
  }

  fontInstaller_.refreshRegistry();
  family.installed = true;
  family.hasUpdate = false;

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
    downloadAttempt_ = 0;
    downloadAttemptTotal_ = 0;
  }
}

void FontDownloadActivity::promptDeleteSelectedFamily() {
  const int pendingDeleteFamilyIndex = familyIndexFromList(selectedIndex_);
  if (pendingDeleteFamilyIndex < 0 || pendingDeleteFamilyIndex >= static_cast<int>(families_.size())) {
    return;
  }

  std::string heading = tr(STR_DELETE);
  const auto& family = families_[pendingDeleteFamilyIndex];
  std::string body = family.installName;
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, body),
                         [this](const ActivityResult& result) { onDeleteConfirmationResult(result); });
}

void FontDownloadActivity::onDeleteConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    requestUpdate();
    return;
  }

  auto& family = families_[familyIndexFromList(selectedIndex_)];

  if (fontInstaller_.deleteFamily(family.installName.c_str()) != FontInstaller::Error::OK) {
    RenderLock lock(*this);
    state_ = ERROR;
    errorMessage_ = "Failed to delete font";
  } else {
    fontsChanged_ = true;
    fontInstaller_.refreshRegistry();
    family.installed = false;
    family.hasUpdate = false;
    // Deletion changes the row's visual state. Reuse the existing storage;
    // the row count cannot grow on this path.
    rebuildListItems();
  }

  requestUpdate();
}

bool FontDownloadActivity::isSelectedFamilyDeletable() const {
  if (isUpdateAllRow(selectedIndex_)) return false;
  if (selectedIndex_ < specialRowCount() || selectedIndex_ >= listItemCount()) return false;
  const auto& family = families_[familyIndexFromList(selectedIndex_)];
  return family.installed && !family.hasUpdate;
}

void FontDownloadActivity::activateSelected() {
  if (families_.empty()) return;
  if (isUpdateAllRow(selectedIndex_)) {
    currentFileIndex_ = 0;
    currentFileTotal_ = 0;
    for (const auto& f : families_) {
      if (f.hasUpdate) currentFileTotal_ += f.files.size();
    }
    updateAll();
  } else {
    auto& family = families_[familyIndexFromList(selectedIndex_)];
    if (!family.installed || family.hasUpdate) {
      currentFileIndex_ = 0;
      currentFileTotal_ = family.files.size();
      downloadFamily(family);
    } else {
      promptDeleteSelectedFamily();
      return;
    }
  }
  requestUpdateAndWait();
}

void FontDownloadActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<FontDownloadActivity*>(user)->buildListScreen(screen);
}

void FontDownloadActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (families_.empty()) {
    screen.centeredText(tr(STR_NO_FONTS_AVAILABLE), screen.theme().bodyText);
    return;
  }

  const int listSize = static_cast<int>(listItemCount_);
  if (listSize <= 0 || !listItems_) {
    screen.centeredText(tr(STR_NO_FONTS_AVAILABLE), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = listItems_.get();
  props.count = static_cast<uint16_t>(listItemCount_);
  props.selectedIndex = static_cast<int16_t>(selectedIndex_);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the status and the row edge
  const auto rows = configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle);
  visibleRows_ = rows > 0 ? rows : 1;
  topIndex_ = scrollListBy(topIndex_, 0, visibleRows_, listSize);  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex_);
  screen.list(props);
}

// --- Input handling ---

void FontDownloadActivity::loop() {
  if (state_ == FAMILY_LIST) {
    if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finishAfterBackPress();
      return;
    }

    const int listSize = listItemCount();

    // Touch goes through the FreeInkApp: render() registered the row hit
    // rects; route the snapshot and let onRowEvent dispatch.
    if (uiReady_) {
      const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
      if (snap.touchPressed || snap.touchReleased) {
        const auto event = app_.route(snap);
        if (app_.invalidated()) requestUpdate();
        if (event) return;  // dispatched to onRowEvent
      }
    }

    if (!families_.empty()) {
      // Swipes scroll the viewport; the selection stays put and button
      // navigation pulls the view back to it.
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
        const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows_ : -visibleRows_;
        const int next = scrollListBy(topIndex_, delta, visibleRows_, listSize);
        if (next != topIndex_) {
          topIndex_ = next;
          requestUpdate();
        }
        return;
      }
    }

    const auto moveSelection = [this, listSize](const int index) {
      selectedIndex_ = index;
      topIndex_ = followListSelection(selectedIndex_, topIndex_, visibleRows_, listSize);
      requestUpdate();
    };
    buttonNavigator_.onNextRelease(
        [this, listSize, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectedIndex_, listSize)); });
    buttonNavigator_.onPreviousRelease(
        [this, listSize, &moveSelection] { moveSelection(ButtonNavigator::previousIndex(selectedIndex_, listSize)); });
    buttonNavigator_.onNextContinuous([this, listSize, &moveSelection] {
      moveSelection(ButtonNavigator::nextPageIndex(selectedIndex_, listSize, visibleRows_));
    });
    buttonNavigator_.onPreviousContinuous([this, listSize, &moveSelection] {
      moveSelection(ButtonNavigator::previousPageIndex(selectedIndex_, listSize, visibleRows_));
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      activateSelected();
      return;
    }
  } else if (state_ == COMPLETE) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      if (manifestReloadNeeded_) {
        returnToFamilyList();
        requestUpdate();
        return;
      }
      hasRetryFamily_ = false;
      retryFamily_ = ManifestFamily();
      manifestReloadNeeded_ = false;
      activeDownloadFamilyName_.clear();
      errorMessage_.clear();
      errorHint_.clear();
      if (families_.empty()) {
        finishAfterBackPress();
      } else {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (hasRetryFamily_) {
        currentFileIndex_ = 0;
        currentFileTotal_ = retryFamily_.files.size();
        downloadFamily(retryFamily_);
        if (state_ == ERROR) {
          hasRetryFamily_ = true;
        } else if (state_ == FAMILY_LIST && manifestReloadNeeded_) {
          returnToFamilyList();
        } else {
          hasRetryFamily_ = false;
          retryFamily_ = ManifestFamily();
        }
        requestUpdateAndWait();
        return;
      } else if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.size())) {
        downloadFamily(families_[downloadingFamilyIndex_]);
        requestUpdateAndWait();
        return;
      } else {
        returnToFamilyList();
        requestUpdate();
      }
    } else {
      int x = 0;
      int y = 0;
      if (mappedInput.wasScreenTapped(x, y)) {
        if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.size())) {
          downloadFamily(families_[downloadingFamilyIndex_]);
          requestUpdateAndWait();
          return;
        }
        {
          RenderLock lock(*this);
          state_ = FAMILY_LIST;
        }
        requestUpdate();
      }
    }
  }
}

// --- Rendering ---

void FontDownloadActivity::formatSize(const size_t bytes, char* const buffer, const size_t bufferSize) {
  if (bufferSize == 0) return;
  if (bytes >= 1024 * 1024) {
    snprintf(buffer, bufferSize, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buffer, bufferSize, "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buffer, bufferSize, "%zu B", bytes);
  }
}

void FontDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget_, header, tr(STR_FONT_BROWSER), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_FONT_BROWSER));
  }

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop =
      metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == FAMILY_LIST) {
    uiReady_ = false;
    app_.render();
    uiReady_ = true;

    const char* confirmLabel = families_.empty()                ? ""
                               : isSelectedFamilyDeletable()    ? tr(STR_DELETE)
                               : isUpdateAllRow(selectedIndex_) ? tr(STR_UPDATE)
                                                                : tr(STR_DOWNLOAD);
    const auto labels =
        mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), confirmLabel,
                              families_.empty() ? "" : tr(STR_DIR_UP), families_.empty() ? "" : tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == DOWNLOADING) {
    const char* familyName = activeDownloadFamilyName_.empty() ? "" : activeDownloadFamilyName_.c_str();

    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + familyName + " (" +
                             std::to_string(currentFileIndex_ + 1) + "/" + std::to_string(currentFileTotal_) + ")";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, statusText.c_str());
    if (downloadAttemptTotal_ > 1) {
      std::string attemptText =
          "Attempt " + std::to_string(downloadAttempt_) + "/" + std::to_string(downloadAttemptTotal_);
      renderer.drawCenteredText(SMALL_FONT_ID, centerY, attemptText.c_str());
    }

    float progress = 0;
    if (fileTotal_ > 0) {
      progress = static_cast<float>(fileProgress_) / static_cast<float>(fileTotal_);
    }

    int barY = centerY + (downloadAttemptTotal_ > 1 ? lineHeight : metrics.verticalSpacing);
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(progress * 100), 100);

    if (mappedInput.hasTouchHardware()) {
      const Rect cancelButton = downloadCancelButtonRect(renderer, metrics, downloadAttemptTotal_);
      renderer.fillRoundedRect(cancelButton.x, cancelButton.y, cancelButton.width, cancelButton.height, 6,
                               Color::White);
      renderer.drawRoundedRect(cancelButton.x, cancelButton.y, cancelButton.width, cancelButton.height, 1, 6, true);
      const int textY = cancelButton.y + (cancelButton.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, textY, tr(STR_CANCEL));
    }

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_FONT_INSTALLED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_FONT_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    const Rect textArea{metrics.contentSidePadding, 0, pageWidth - metrics.contentSidePadding * 2, pageHeight};
    int messageY = centerY + metrics.verticalSpacing;
    if (!errorMessage_.empty()) {
      messageY += UITheme::drawCenteredWrappedText(renderer, textArea, SMALL_FONT_ID, messageY, errorMessage_.c_str(),
                                                   2, true, EpdFontFamily::REGULAR, 2) +
                  2;
    }
    if (!errorHint_.empty()) {
      messageY += metrics.verticalSpacing / 2;
      UITheme::drawCenteredWrappedText(renderer, textArea, SMALL_FONT_ID, messageY, errorHint_.c_str(), 2, true,
                                       EpdFontFamily::REGULAR, 2);
    }
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer(screenTransitionRefresh_.modeFor(static_cast<uint8_t>(state_)));
}
