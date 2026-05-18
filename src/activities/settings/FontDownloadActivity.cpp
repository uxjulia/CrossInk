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
#include "SdCardFontSystem.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {

constexpr int FONT_DOWNLOAD_MAX_ATTEMPTS = 3;
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

const char* currentSdFontRangeSlug() {
  switch (SETTINGS.sdFontSizeRange) {
    case CrossPointSettings::SD_FONT_RANGE_TEENSY:
      return "teensy";
    case CrossPointSettings::SD_FONT_RANGE_XLARGE:
      return "xlarge";
    case CrossPointSettings::SD_FONT_RANGE_NO_EMOJI:
      return "no_emoji";
    case CrossPointSettings::SD_FONT_RANGE_ALL:
      return "all";
    case CrossPointSettings::SD_FONT_RANGE_TINY:
    default:
      return "tiny";
  }
}

std::string buildRangeScopedFamilyName(const std::string& familyName) {
  return familyName + " (" + currentSdFontRangeSlug() + ")";
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
    : Activity("FontDownload", renderer, mappedInput), fontInstaller_(sdFontSystem.registry()) {}

// --- Lifecycle ---

void FontDownloadActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void FontDownloadActivity::onExit() {
  Activity::onExit();
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
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

  auto result = HttpDownloader::downloadToFile(FONT_MANIFEST_URL, MANIFEST_TMP, nullptr);
  if (result != HttpDownloader::OK) {
    LOG_ERR("FONT", "Failed to fetch manifest from %s", FONT_MANIFEST_URL);
    errorMessage_ = "Failed to fetch font list";
    Storage.remove(MANIFEST_TMP);
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
  fontInstaller_.refreshRegistry();

  JsonArray familiesArr = doc["families"].as<JsonArray>();
  families_.reserve(familiesArr.size());

  for (JsonObject fObj : familiesArr) {
    ManifestFamily family;
    family.name = fObj["name"] | "";
    family.description = fObj["description"] | "";
    family.languages = fObj["languages"] | "";

    for (JsonVariant s : fObj["styles"].as<JsonArray>()) {
      family.styles.push_back(s.as<std::string>());
    }

    family.totalSize = 0;
    for (JsonObject fileObj : fObj["files"].as<JsonArray>()) {
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
      continue;
    }

    resolveInstalledFamilyName(family);

    families_.push_back(std::move(family));
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
  const std::string rangeScopedName = buildRangeScopedFamilyName(family.name);

  bool hasUpdate = false;
  std::string resolvedName;
  if (installedFilesMatch(rangeScopedName.c_str(), family.files, hasUpdate, &resolvedName)) {
    family.installName = resolvedName;
    family.installed = true;
    family.hasUpdate = hasUpdate;
    return;
  }

  // Backward compatibility for fonts installed before range suffixes existed.
  // Reuse the old folder if it has any readable .cpfont files for this family;
  // missing files for the currently selected range are surfaced as an update.
  if (installedFilesMatch(family.name.c_str(), family.files, hasUpdate, &resolvedName)) {
    family.installName = resolvedName;
    family.installed = true;
    family.hasUpdate = hasUpdate;
    return;
  }

  family.installName = rangeScopedName;
  family.installed = false;
  family.hasUpdate = false;
}

// --- Download ---

void FontDownloadActivity::downloadAll() {
  for (size_t i = 0; i < families_.size(); i++) {
    if (families_[i].installed) continue;
    downloadFamily(families_[i]);
    if (state_ == ERROR) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

void FontDownloadActivity::updateAll() {
  for (size_t i = 0; i < families_.size(); i++) {
    if (!families_[i].hasUpdate) continue;
    downloadFamily(families_[i]);
    if (state_ == ERROR) return;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
}

bool FontDownloadActivity::showDownloadAllRow() const {
  for (const auto& f : families_) {
    if (!f.installed) return true;
  }
  return false;
}

bool FontDownloadActivity::showUpdateAllRow() const {
  for (const auto& f : families_) {
    if (f.hasUpdate) return true;
  }
  return false;
}

int FontDownloadActivity::specialRowCount() const {
  return (showDownloadAllRow() ? 1 : 0) + (showUpdateAllRow() ? 1 : 0);
}

bool FontDownloadActivity::isDownloadAllRow(int index) const { return showDownloadAllRow() && index == 0; }

bool FontDownloadActivity::isUpdateAllRow(int index) const {
  return showUpdateAllRow() && index == (showDownloadAllRow() ? 1 : 0);
}

int FontDownloadActivity::listItemCount() const {
  return families_.empty() ? 0 : static_cast<int>(families_.size()) + specialRowCount();
}

size_t FontDownloadActivity::totalDownloadSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (!f.installed) total += f.totalSize;
  }
  return total;
}

size_t FontDownloadActivity::totalUpdateSize() const {
  size_t total = 0;
  for (const auto& f : families_) {
    if (f.hasUpdate) total += f.totalSize;
  }
  return total;
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

  {
    RenderLock lock(*this);
    state_ = DOWNLOADING;
    downloadingFamilyIndex_ = static_cast<int>(&family - families_.data());
    currentFileIndex_ = 0;
    currentFileTotal_ = family.files.size();
    fileProgress_ = 0;
    fileTotal_ = 0;
    downloadAttempt_ = 0;
    downloadAttemptTotal_ = 0;
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
        LOG_DBG("FONT", "Skipping existing %s (crc32=%08x)", file.name.c_str(), existingCrc);
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
          "", "", downloadOptions);
      if (result == HttpDownloader::OK) {
        break;
      }
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
    LOG_DBG("FONT", "Downloaded %s (size=%zu crc32=%08x)", file.name.c_str(), file.size, actualCrc);

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
    fontInstaller_.refreshRegistry();
    family.installed = false;
    family.hasUpdate = false;
  }

  requestUpdate();
}

bool FontDownloadActivity::isSelectedFamilyDeletable() const {
  if (isDownloadAllRow(selectedIndex_) || isUpdateAllRow(selectedIndex_)) return false;
  if (selectedIndex_ < specialRowCount() || selectedIndex_ >= listItemCount()) return false;
  const auto& family = families_[familyIndexFromList(selectedIndex_)];
  return family.installed && !family.hasUpdate;
}

// --- Input handling ---

void FontDownloadActivity::loop() {
  if (state_ == FAMILY_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    const int listSize = listItemCount();
    const int pageItems = fontListPageItems();

    buttonNavigator_.onNextRelease([this, listSize] {
      selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
      requestUpdate();
    });

    buttonNavigator_.onPreviousRelease([this, listSize] {
      selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
      requestUpdate();
    });

    buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
      selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
    });

    buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
      selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (!families_.empty()) {
        if (isDownloadAllRow(selectedIndex_)) {
          downloadAll();
        } else if (isUpdateAllRow(selectedIndex_)) {
          updateAll();
        } else {
          auto& family = families_[familyIndexFromList(selectedIndex_)];
          if (!family.installed || family.hasUpdate) {
            downloadFamily(family);
          } else {
            promptDeleteSelectedFamily();
            return;
          }
        }
        requestUpdateAndWait();
        return;
      }
    }
  } else if (state_ == COMPLETE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    }
  } else if (state_ == ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      {
        RenderLock lock(*this);
        state_ = FAMILY_LIST;
      }
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (downloadingFamilyIndex_ >= 0 && downloadingFamilyIndex_ < static_cast<int>(families_.size())) {
        downloadFamily(families_[downloadingFamilyIndex_]);
        requestUpdateAndWait();
        return;
      } else {
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

std::string FontDownloadActivity::formatSize(size_t bytes) {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

int FontDownloadActivity::fontListPageItems() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int reservedHeight = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing +
                             metrics.buttonHintsHeight + metrics.verticalSpacing;
  const int availableHeight = renderer.getScreenHeight() - reservedHeight;
  return std::max(1, availableHeight / fontListRowHeight(renderer, metrics));
}

void FontDownloadActivity::drawFontList(Rect rect) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  constexpr int kLineGap = 2;
  constexpr int kMinTitleWidth = 40;
  constexpr int kValueGap = 8;
  constexpr int kScrollBarWidth = 3;
  constexpr int kScrollBarGap = 10;

  const int itemCount = listItemCount();
  if (itemCount <= 0) return;

  const int rowHeight = fontListRowHeight(renderer, metrics);
  const int pageItems = std::max(1, rect.height / rowHeight);
  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  const int pageStartIndex = std::max(0, selectedIndex_ / pageItems) * pageItems;
  const int pageEndIndex = std::min(itemCount, pageStartIndex + pageItems);

  const bool showScrollBar = totalPages > 1;
  const int contentWidth = rect.width - (showScrollBar ? (kScrollBarWidth + kScrollBarGap) : 0);
  const int textX = rect.x + metrics.contentSidePadding;
  const int textWidth = contentWidth - metrics.contentSidePadding * 2;
  const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int smallLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int linesHeight = titleLineHeight + smallLineHeight * 2 + kLineGap * 2;
  const int textTopPadding = std::max(0, (rowHeight - linesHeight) / 2);

  if (showScrollBar) {
    const int scrollX = rect.x + rect.width - metrics.contentSidePadding;
    const int scrollBarHeight = std::max(4, (rect.height * pageItems) / itemCount);
    const int currentPage = selectedIndex_ / pageItems;
    const int scrollRange = std::max(0, rect.height - scrollBarHeight);
    const int scrollY = rect.y + (totalPages > 1 ? (scrollRange * currentPage) / (totalPages - 1) : 0);
    renderer.drawLine(scrollX, rect.y, scrollX, rect.y + rect.height, true);
    renderer.fillRect(scrollX - kScrollBarWidth, scrollY, kScrollBarWidth, scrollBarHeight, true);
  }

  for (int index = pageStartIndex; index < pageEndIndex; ++index) {
    const int rowY = rect.y + (index - pageStartIndex) * rowHeight;
    const bool selected = index == selectedIndex_;
    const bool familyRow = !isDownloadAllRow(index) && !isUpdateAllRow(index);
    const bool dimmed = familyRow && families_[familyIndexFromList(index)].installed &&
                        !families_[familyIndexFromList(index)].hasUpdate;

    if (selected) {
      renderer.fillRect(rect.x, rowY, contentWidth, rowHeight, true);
    }

    std::string title;
    std::string description;
    std::string languages;
    std::string value;

    if (isDownloadAllRow(index)) {
      title = std::string(tr(STR_DOWNLOAD_ALL)) + " (" + formatSize(totalDownloadSize()) + ")";
    } else if (isUpdateAllRow(index)) {
      title = std::string(tr(STR_UPDATE_ALL)) + " (" + formatSize(totalUpdateSize()) + ")";
    } else {
      const auto& family = families_[familyIndexFromList(index)];
      title = family.name;
      description = family.description;
      languages = family.languages;
      if (family.hasUpdate) {
        value = tr(STR_UPDATE_AVAILABLE);
      } else if (family.installed) {
        value = tr(STR_INSTALLED);
      }
    }

    const bool hasDetails = !description.empty() || !languages.empty();
    const int titleY = hasDetails ? rowY + textTopPadding : rowY + (rowHeight - titleLineHeight) / 2;
    int titleWidth = textWidth;
    if (!value.empty()) {
      const int maxValueWidth = std::max(0, textWidth - kMinTitleWidth - kValueGap);
      value = renderer.truncatedText(UI_10_FONT_ID, value.c_str(), maxValueWidth);
      const int valueWidth = renderer.getTextWidth(UI_10_FONT_ID, value.c_str());
      renderer.drawText(UI_10_FONT_ID, textX + textWidth - valueWidth, titleY, value.c_str(), !selected);
      titleWidth = std::max(0, textWidth - valueWidth - kValueGap);
    }

    title = renderer.truncatedText(UI_10_FONT_ID, title.c_str(), titleWidth);
    renderer.drawText(UI_10_FONT_ID, textX, titleY, title.c_str(), !selected);

    if (!description.empty()) {
      description = renderer.truncatedText(SMALL_FONT_ID, description.c_str(), textWidth);
      renderer.drawText(SMALL_FONT_ID, textX, rowY + textTopPadding + titleLineHeight + kLineGap, description.c_str(),
                        !selected);
    }

    if (!languages.empty()) {
      languages = renderer.truncatedText(SMALL_FONT_ID, languages.c_str(), textWidth);
      renderer.drawText(SMALL_FONT_ID, textX, rowY + textTopPadding + titleLineHeight + smallLineHeight + kLineGap * 2,
                        languages.c_str(), !selected);
    }

    if (dimmed && !selected) {
      const int ditherWidth = renderer.getTextWidth(UI_10_FONT_ID, title.c_str());
      for (int py = titleY; py < titleY + titleLineHeight; ++py) {
        for (int px = textX; px < textX + ditherWidth; ++px) {
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
        }
      }
    }
  }
}

void FontDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_FONT_BROWSER));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_MANIFEST) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_LOADING_FONT_LIST));
  } else if (state_ == FAMILY_LIST) {
    if (families_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_NO_FONTS_AVAILABLE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      drawFontList(Rect{0, contentTop, pageWidth,
                        pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing});

      const auto labels = mappedInput.mapLabels(tr(STR_BACK),
                                                isSelectedFamilyDeletable()      ? tr(STR_DELETE)
                                                : isUpdateAllRow(selectedIndex_) ? tr(STR_UPDATE)
                                                                                 : tr(STR_DOWNLOAD),
                                                tr(STR_DIR_UP), tr(STR_DIR_DOWN));
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state_ == DOWNLOADING) {
    const auto& family = families_[downloadingFamilyIndex_];

    std::string statusText = std::string(tr(STR_DOWNLOADING)) + " " + family.name + " (" +
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
  } else if (state_ == COMPLETE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_FONT_INSTALLED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_FONT_INSTALL_FAILED), true,
                              EpdFontFamily::BOLD);
    const int messageWidth = pageWidth - metrics.contentSidePadding * 2;
    int messageY = centerY + metrics.verticalSpacing;
    if (!errorMessage_.empty()) {
      const auto messageLines = renderer.wrappedText(SMALL_FONT_ID, errorMessage_.c_str(), messageWidth, 2);
      const int smallLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
      for (const auto& line : messageLines) {
        renderer.drawCenteredText(SMALL_FONT_ID, messageY, line.c_str());
        messageY += smallLineHeight + 2;
      }
    }
    if (!errorHint_.empty()) {
      const auto hintLines = renderer.wrappedText(SMALL_FONT_ID, errorHint_.c_str(), messageWidth, 2);
      const int smallLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
      messageY += metrics.verticalSpacing / 2;
      for (const auto& line : hintLines) {
        renderer.drawCenteredText(SMALL_FONT_ID, messageY, line.c_str());
        messageY += smallLineHeight + 2;
      }
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
