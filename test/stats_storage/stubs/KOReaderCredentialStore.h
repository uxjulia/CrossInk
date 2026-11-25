#pragma once
#include <string>
struct StatsTestCredentials {
  bool hasCredentials() const { return false; }
  bool getSyncStats() const { return false; }
  const std::string& getBaseUrl() const {
    static std::string s;
    return s;
  }
  const std::string& getUsername() const {
    static std::string s;
    return s;
  }
};
inline StatsTestCredentials KOREADER_STORE;
