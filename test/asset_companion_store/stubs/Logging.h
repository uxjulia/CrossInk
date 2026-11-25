#pragma once
template <typename... Args>
inline void assetLog(const char*, const char*, Args&&...) {}
#define LOG_ERR(...) assetLog(__VA_ARGS__)
#define LOG_INF(...) assetLog(__VA_ARGS__)
#define LOG_DBG(...) assetLog(__VA_ARGS__)
