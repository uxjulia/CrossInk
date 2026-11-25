#pragma once

#include <string>

// Just what the sync stores read: the account identity and What to Sync toggles.
struct TestCredentialStore {
  bool credentials = true;
  bool stats = true;
  bool clippings = true;
  std::string baseUrl = "https://sync.example";
  std::string username = "reader";
  bool hasCredentials() const { return credentials; }
  bool getSyncStats() const { return stats; }
  bool getSyncClippings() const { return clippings; }
  std::string getBaseUrl() const { return baseUrl; }
  const std::string& getUsername() const { return username; }
};
inline TestCredentialStore KOREADER_STORE;
