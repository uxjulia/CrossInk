#pragma once
#include <atomic>
#include <cstdint>
#include <string>

class CrossPointState {
  // Static instance
  static CrossPointState instance;

 public:
  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  std::string favoriteSleepImagePath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};  // circular buffer of recent wallpaper indices
  uint8_t recentSleepPos = 0;                           // next write slot
  uint8_t recentSleepFill = 0;                          // valid entries (0..SLEEP_RECENT_COUNT)
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
  ~CrossPointState() = default;

  // Get singleton instance
  static CrossPointState& getInstance() { return instance; }

  bool saveToFile() const;

  bool loadFromFile();
  uint16_t pendingBookmarkSpine = UINT16_MAX;
  float pendingBookmarkProgress = -1.0f;

  // Set by background move task on failure; read and cleared by ActivityManager to show AlertActivity.
  // Title/body are written before the flag is set to ensure they are visible when flag is read.
  std::atomic<bool> hasPendingAlert{false};
  char pendingAlertTitle[64] = {};
  char pendingAlertBody[256] = {};

 private:
  bool loadFromBinaryFile();
};

// Helper macro to access settings
#define APP_STATE CrossPointState::getInstance()
