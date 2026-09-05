#include "CrossPointState.h"

#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>
#include <Serialization.h>

#include <algorithm>
#include <mutex>

namespace {
constexpr uint8_t STATE_FILE_VERSION = 5;
constexpr char STATE_FILE_BIN[] = "/.crosspoint/state.bin";
constexpr char STATE_FILE_JSON[] = "/.crosspoint/state.json";
constexpr char STATE_FILE_BAK[] = "/.crosspoint/state.bin.bak";
}  // namespace

bool CrossPointState::isRecentSleep(uint16_t idx, uint8_t checkCount) const {
  const uint8_t effectiveCount = std::min(checkCount, recentSleepFill);
  for (uint8_t i = 0; i < effectiveCount; i++) {
    const uint8_t slot = (recentSleepPos + SLEEP_RECENT_COUNT - 1 - i) % SLEEP_RECENT_COUNT;
    if (recentSleepImages[slot] == idx) return true;
  }
  return false;
}

void CrossPointState::pushRecentSleep(uint16_t idx) {
  recentSleepImages[recentSleepPos] = idx;
  recentSleepPos = (recentSleepPos + 1) % SLEEP_RECENT_COUNT;
  if (recentSleepFill < SLEEP_RECENT_COUNT) recentSleepFill++;
}

void CrossPointState::clearRecentSleepHistory() {
  std::fill_n(recentSleepImages, SLEEP_RECENT_COUNT, static_cast<uint16_t>(0));
  recentSleepPos = 0;
  recentSleepFill = 0;
}

void CrossPointState::setPendingOverlayResume(PendingOverlayResume value) {
  pendingOverlayResume = std::move(value);
  saveToFile();
}

bool CrossPointState::consumePendingOverlayResume(PendingOverlayResume& value) {
  if (!consumePendingOverlayResumeOnce(pendingOverlayResume, value)) return false;
  saveToFile();
  return true;
}

void CrossPointState::pushRecentBoot(uint16_t idx) {
  recentBootImages[recentBootPos] = idx;
  recentBootPos = (recentBootPos + 1) % BOOT_RECENT_COUNT;
  if (recentBootFill < BOOT_RECENT_COUNT) recentBootFill++;
}

bool CrossPointState::saveToFile() const {
  std::lock_guard<std::mutex> storeLock(storeMutex);
  std::lock_guard<std::mutex> stateLock(_mutex);
  JsonDocument doc;
  toJson(doc);
  return PersistableStoreBase::writeDocToFile(STATE_FILE_JSON, doc);
}

bool CrossPointState::loadFromFile() {
  // Try JSON first
  if (Storage.exists(STATE_FILE_JSON)) {
    std::lock_guard<std::mutex> storeLock(storeMutex);
    JsonDocument doc;
    if (PersistableStoreBase::readDocFromFile(STATE_FILE_JSON, doc)) {
      std::lock_guard<std::mutex> stateLock(_mutex);
      return fromJson(doc.as<JsonVariantConst>());
    }
  }

  // Fall back to binary migration
  if (Storage.exists(STATE_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      if (saveToFile()) {
        Storage.rename(STATE_FILE_BIN, STATE_FILE_BAK);
        LOG_DBG("CPS", "Migrated state.bin to state.json");
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save state during migration");
        return false;
      }
    }
  }

  return false;
}

void CrossPointState::toJson(JsonDocument& doc) const {
  doc["openEpubPath"] = openEpubPath;
  doc["favoriteSleepImagePath"] = favoriteSleepImagePath;
  doc["preferredSleepFolderPath"] = preferredSleepFolderPath;
  JsonArray recentArr = doc["recentSleepImages"].to<JsonArray>();
  for (int i = 0; i < SLEEP_RECENT_COUNT; i++) recentArr.add(recentSleepImages[i]);
  doc["recentSleepPos"] = recentSleepPos;
  doc["recentSleepFill"] = recentSleepFill;
  doc["favoriteBootImagePath"] = favoriteBootImagePath;
  JsonArray recentBootArr = doc["recentBootImages"].to<JsonArray>();
  for (int i = 0; i < BOOT_RECENT_COUNT; i++) recentBootArr.add(recentBootImages[i]);
  doc["recentBootPos"] = recentBootPos;
  doc["recentBootFill"] = recentBootFill;
  doc["readerActivityLoadCount"] = readerActivityLoadCount;
  doc["lastSleepFromReader"] = lastSleepFromReader;
  doc["pendingBookmarkSpine"] = pendingBookmarkSpine;
  doc["pendingBookmarkProgress"] = pendingBookmarkProgress;
  doc["pendingBookmarkParagraphIndex"] = pendingBookmarkParagraphIndex;
  doc["pendingClippingIndex"] = pendingClippingIndex;
  doc["showBootScreen"] = showBootScreen;
  doc["quickLockResumePending"] = quickLockResumePending;
  doc["quickLockResumeTrigger"] = quickLockResumeTrigger;
  doc["quickLockRestoreFrontlight"] = quickLockRestoreFrontlight;
  doc["pendingOverlayOrigin"] = static_cast<uint8_t>(pendingOverlayResume.origin);
  doc["pendingOverlayType"] = static_cast<uint8_t>(pendingOverlayResume.overlay);
  doc["pendingOverlayTab"] = pendingOverlayResume.tab;
  doc["pendingOverlayPane"] = pendingOverlayResume.pane;
  doc["pendingOverlaySelection"] = pendingOverlayResume.selectedIndex;
  doc["pendingOverlayScroll"] = pendingOverlayResume.scrollPosition;
  doc["pendingOverlayBookPath"] = pendingOverlayResume.bookPath;
  doc["pendingOverlayReturnHome"] = pendingOverlayResume.returnHomeAfterReaderFlow;
  doc["pendingOverlayReaderOrientation"] = pendingOverlayResume.readerOrientation;
  doc["pendingOverlayPreserveReaderOrientation"] = pendingOverlayResume.preserveReaderOrientation;
}

bool CrossPointState::fromJson(JsonVariantConst doc) {
  openEpubPath = doc["openEpubPath"] | "";
  favoriteSleepImagePath = doc["favoriteSleepImagePath"] | "";
  preferredSleepFolderPath = doc["preferredSleepFolderPath"] | "";
  std::fill_n(recentSleepImages, SLEEP_RECENT_COUNT, static_cast<uint16_t>(0));
  JsonArrayConst recentArr = doc["recentSleepImages"];
  const int actualCount =
      recentArr.isNull() ? 0 : std::min(static_cast<int>(recentArr.size()), static_cast<int>(SLEEP_RECENT_COUNT));
  for (int i = 0; i < actualCount; i++) recentSleepImages[i] = recentArr[i] | static_cast<uint16_t>(0);
  recentSleepPos = doc["recentSleepPos"] | static_cast<uint8_t>(0);
  if (recentSleepPos >= SLEEP_RECENT_COUNT) {
    recentSleepPos = actualCount > 0 ? recentSleepPos % SLEEP_RECENT_COUNT : 0;
  }
  recentSleepFill = doc["recentSleepFill"] | static_cast<uint8_t>(0);
  recentSleepFill = static_cast<uint8_t>(std::min(static_cast<int>(recentSleepFill), actualCount));
  if (recentSleepFill == 0 && !doc["lastSleepImage"].isNull()) {
    const uint8_t legacy = doc["lastSleepImage"] | static_cast<uint8_t>(UINT8_MAX);
    if (legacy != UINT8_MAX) pushRecentSleep(static_cast<uint16_t>(legacy));
  }
  favoriteBootImagePath = doc["favoriteBootImagePath"] | "";
  std::fill_n(recentBootImages, BOOT_RECENT_COUNT, static_cast<uint16_t>(0));
  JsonArrayConst recentBootArr = doc["recentBootImages"];
  const int actualBootCount =
      recentBootArr.isNull() ? 0
                             : std::min(static_cast<int>(recentBootArr.size()), static_cast<int>(BOOT_RECENT_COUNT));
  for (int i = 0; i < actualBootCount; i++) recentBootImages[i] = recentBootArr[i] | static_cast<uint16_t>(0);
  recentBootPos = doc["recentBootPos"] | static_cast<uint8_t>(0);
  if (recentBootPos >= BOOT_RECENT_COUNT) {
    recentBootPos = actualBootCount > 0 ? recentBootPos % BOOT_RECENT_COUNT : 0;
  }
  recentBootFill = doc["recentBootFill"] | static_cast<uint8_t>(0);
  recentBootFill = static_cast<uint8_t>(std::min(static_cast<int>(recentBootFill), actualBootCount));
  readerActivityLoadCount = doc["readerActivityLoadCount"] | static_cast<uint8_t>(0);
  lastSleepFromReader = doc["lastSleepFromReader"] | false;
  pendingBookmarkSpine = doc["pendingBookmarkSpine"] | static_cast<uint16_t>(UINT16_MAX);
  pendingBookmarkProgress = doc["pendingBookmarkProgress"] | static_cast<float>(-1.0f);
  pendingBookmarkParagraphIndex = doc["pendingBookmarkParagraphIndex"] | static_cast<uint16_t>(UINT16_MAX);
  pendingClippingIndex = doc["pendingClippingIndex"] | static_cast<uint16_t>(UINT16_MAX);
  showBootScreen = doc["showBootScreen"] | true;
  quickLockResumePending = doc["quickLockResumePending"] | false;
  quickLockResumeTrigger = doc["quickLockResumeTrigger"] | static_cast<uint8_t>(0);
  quickLockRestoreFrontlight = doc["quickLockRestoreFrontlight"] | false;
  pendingOverlayResume.origin =
      static_cast<PendingOverlayOrigin>(doc["pendingOverlayOrigin"] | static_cast<uint8_t>(0));
  pendingOverlayResume.overlay = static_cast<PendingOverlayType>(doc["pendingOverlayType"] | static_cast<uint8_t>(0));
  pendingOverlayResume.tab = doc["pendingOverlayTab"] | static_cast<uint8_t>(0);
  pendingOverlayResume.pane = doc["pendingOverlayPane"] | static_cast<uint8_t>(0);
  pendingOverlayResume.selectedIndex = doc["pendingOverlaySelection"] | static_cast<int16_t>(0);
  pendingOverlayResume.scrollPosition = doc["pendingOverlayScroll"] | static_cast<int16_t>(0);
  pendingOverlayResume.bookPath = doc["pendingOverlayBookPath"] | "";
  pendingOverlayResume.returnHomeAfterReaderFlow = doc["pendingOverlayReturnHome"] | false;
  pendingOverlayResume.readerOrientation = doc["pendingOverlayReaderOrientation"] | static_cast<uint8_t>(0);
  pendingOverlayResume.preserveReaderOrientation = doc["pendingOverlayPreserveReaderOrientation"] | false;
  return true;
}

bool CrossPointState::loadFromBinaryFile() {
  HalFile inputFile;
  if (!Storage.openFileForRead("CPS", STATE_FILE_BIN, inputFile)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(_mutex);

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version > STATE_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  serialization::readString(inputFile, openEpubPath);
  if (version >= 2) {
    uint8_t legacyLastSleep = UINT8_MAX;
    serialization::readPod(inputFile, legacyLastSleep);
    if (legacyLastSleep != UINT8_MAX) {
      pushRecentSleep(static_cast<uint16_t>(legacyLastSleep));
    }
  }

  if (version >= 3) {
    serialization::readPod(inputFile, readerActivityLoadCount);
  }

  if (version >= 4) {
    serialization::readPod(inputFile, lastSleepFromReader);
  } else {
    lastSleepFromReader = false;
  }

  if (version >= 5) {
    serialization::readPod(inputFile, pendingBookmarkSpine);
    serialization::readPod(inputFile, pendingBookmarkProgress);
    pendingBookmarkParagraphIndex = UINT16_MAX;
    pendingClippingIndex = UINT16_MAX;
  } else {
    pendingBookmarkSpine = UINT16_MAX;
    pendingBookmarkProgress = -1.0f;
    pendingBookmarkParagraphIndex = UINT16_MAX;
    pendingClippingIndex = UINT16_MAX;
  }

  return true;
}
