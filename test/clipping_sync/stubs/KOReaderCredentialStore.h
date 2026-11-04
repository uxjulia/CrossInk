#pragma once
#include <string>
struct TestCredentials {
  std::string server = "https://sync.test", user = "reader";
  const std::string& getBaseUrl() const { return server; }
  const std::string& getUsername() const { return user; }
};
inline TestCredentials testCredentials;
#define KOREADER_STORE testCredentials
