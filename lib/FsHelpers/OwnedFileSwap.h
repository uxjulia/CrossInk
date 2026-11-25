#pragma once

#include <mutex>

#include "DownloadFileSwap.h"

// Only for firmware-owned filenames whose .tmp and .bak siblings are reserved.
namespace OwnedFileSwap {
inline std::mutex swapMutex;

inline bool recover(const std::string& path) {
  std::lock_guard<std::mutex> lock(swapMutex);
  return DownloadFileSwap::recover(path.c_str(), (path + ".bak").c_str());
}

inline bool publish(const std::string& path, const std::string& staged) {
  std::lock_guard<std::mutex> lock(swapMutex);
  return DownloadFileSwap::publishWithBackup(path.c_str(), staged.c_str(), (path + ".bak").c_str());
}

inline bool save(const char* tag, const std::string& path, const uint8_t* data, size_t length) {
  std::lock_guard<std::mutex> lock(swapMutex);
  const std::string backup = path + ".bak";
  if (!DownloadFileSwap::recover(path.c_str(), backup.c_str())) return false;
  const std::string stage = path + ".tmp";
  HalFile file;
  if (!Storage.openFileForWrite(tag, stage.c_str(), file)) return false;
  const bool written = file.write(data, length) == length && file.sync();
  const bool closed = file.close();
  if (written && closed && DownloadFileSwap::publishWithBackup(path.c_str(), stage.c_str(), backup.c_str()))
    return true;
  LOG_ERR(tag, "Could not save %s", path.c_str());
  Storage.remove(stage.c_str());
  return false;
}
}  // namespace OwnedFileSwap
