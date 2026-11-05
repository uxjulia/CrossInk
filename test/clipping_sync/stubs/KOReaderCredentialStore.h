#pragma once
#include <string>
enum class SyncServerSupport { UNSUPPORTED, SUPPORTED };
struct TestCredentials {
  std::string server = "https://sync.test", user = "reader";
  const std::string& getBaseUrl() const { return server; }
  const std::string& getUsername() const { return user; }
  const std::string& getMd5Password() const { return user; }
  const std::string& getCustomHeaders() const { return user; }
  void setServerSupport(SyncServerSupport) {}
};
inline TestCredentials testCredentials;
#define KOREADER_STORE testCredentials
