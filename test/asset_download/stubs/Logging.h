#pragma once
#include <cstdio>
#include <string>
#include <vector>

inline std::vector<std::string> assetLogs;
template <typename... Args>
inline void assetLog(const char*, const char* format, Args&&... args) {
  // Firmware's 256-byte entry includes its timestamp, level and origin prefix.
  char text[256 - (sizeof("[4294967295] [ERR] [ASSET] ") - 1)];
  if constexpr (sizeof...(Args) == 0)
    std::snprintf(text, sizeof(text), "%s", format);
  else
    std::snprintf(text, sizeof(text), format, args...);
  assetLogs.emplace_back(text);
}
#define LOG_ERR(...) assetLog(__VA_ARGS__)
#define LOG_INF(...) assetLog(__VA_ARGS__)
#define LOG_DBG(...) assetLog(__VA_ARGS__)
