#pragma once
inline constexpr int STR_DONE = 0, STR_NONE_OPT = 1, STR_NOT_SUPPORTED = 2, STR_AUTH_FAILED = 3,
                     STR_KOREADER_SYNC_HTTP_404 = 4, STR_KOREADER_SYNC_LOW_MEMORY = 5, STR_CONNECTION_FAILED = 6,
                     STR_SYNC_FAILED_MSG = 7, STR_SYNC_SERVER_ERROR_SHORT = 8, STR_SYNC_INVALID_DATA_SHORT = 9;
inline const char* tr(int key) {
  return key == STR_SYNC_SERVER_ERROR_SHORT   ? "Server error"
         : key == STR_SYNC_INVALID_DATA_SHORT ? "Invalid sync data"
                                              : "test";
}
