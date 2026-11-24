#pragma once
#include <HalStorage.h>
#include <Logging.h>

#include <string>

// .part and .old are reserved sidecars for one synchronous download per destination.
namespace DownloadFileSwap {
// Callers in low-heap paths can provide fixed-size sidecar paths.
inline bool recover(const char* destination, const char* backup) {
  if (!Storage.exists(backup)) return true;
  // A committed destination wins. Otherwise restore the last complete book.
  const bool ok = Storage.exists(destination) ? Storage.remove(backup) : Storage.rename(backup, destination);
  if (!ok) LOG_ERR("HTTP", "Could not recover download backup: %s", backup);
  return ok;
}

inline bool recover(const std::string& destination) {
  const std::string backup = destination + ".old";
  return recover(destination.c_str(), backup.c_str());
}

inline bool publishWithBackup(const char* destination, const char* partial, const char* backup,
                              bool requireBackupCleanup = false) {
  if (!recover(destination, backup)) return false;
  const bool replacing = Storage.exists(destination);
  if (replacing && !Storage.rename(destination, backup)) {
    LOG_ERR("HTTP", "Could not preserve existing book: %s", destination);
    return false;
  }
  if (!Storage.rename(partial, destination)) {
    LOG_ERR("HTTP", "Could not publish downloaded book: %s", destination);
    if (replacing && !Storage.rename(backup, destination)) {
      LOG_ERR("HTTP", "Book preserved in backup: %s", backup);
    }
    return false;
  }
  // Asset sources require completed cleanup; legacy book/font callers retain
  // their successful-publication behavior. Both keep the valid new destination.
  if (replacing && !Storage.remove(backup)) {
    LOG_ERR("HTTP", "Could not remove download backup: %s", backup);
    return !requireBackupCleanup;
  }
  return true;
}

inline bool publishFrom(const std::string& destination, const std::string& partial, bool requireBackupCleanup = false) {
  const std::string backup = destination + ".old";
  return publishWithBackup(destination.c_str(), partial.c_str(), backup.c_str(), requireBackupCleanup);
}
inline bool publish(const std::string& destination, bool requireBackupCleanup = false) {
  return publishFrom(destination, destination + ".part", requireBackupCleanup);
}
}  // namespace DownloadFileSwap
