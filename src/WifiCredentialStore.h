#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct WifiCredential {
  std::string ssid;
  std::string password;  // Plaintext in memory; obfuscated with hardware key on disk
};

struct WifiCredentialSummary {
  std::string ssid;
  bool hasPassword = false;
  bool isLastConnected = false;
};

/**
 * Singleton class for storing WiFi credentials on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON (not cryptographically secure,
 * but prevents casual reading and ties credentials to the specific device).
 */
class WifiCredentialStore : public PersistableStore<WifiCredentialStore> {
 private:
  std::vector<WifiCredential> credentials;
  std::string lastConnectedSsid;
  mutable std::mutex credentialMutex;
  mutable std::once_flag loadOnce;

  static constexpr size_t MAX_NETWORKS = 8;
  static constexpr size_t MAX_PASSWORD_LENGTH = 64;

  // Private constructor for singleton
  WifiCredentialStore() = default;
  bool loadFromFile();
  bool loadFromBinaryFile();
  void ensureLoaded() const;

  friend class PersistableStore<WifiCredentialStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/wifi.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Credential management
  bool addCredential(const std::string& ssid, const std::string& password);
  bool removeCredential(const std::string& ssid);
  std::optional<WifiCredential> findCredential(const std::string& ssid) const;
  std::optional<WifiCredential> getCredentialAt(size_t index) const;
  std::optional<std::string> getSsidAt(size_t index) const;
  size_t getCredentialCount() const;
  // Bounded to MAX_NETWORKS (8): copy SSIDs so the web task never holds the
  // credential mutex across blocking network writes.
  std::vector<WifiCredentialSummary> getCredentialSummaries() const;

  // Check if a network is saved
  bool hasSavedCredential(const std::string& ssid) const;

  // Last connected network
  void setLastConnectedSsid(const std::string& ssid);
  std::string getLastConnectedSsid() const;

  // Clear all credentials
  void clearAll();
};

// Helper macro to access credentials store
#define WIFI_STORE WifiCredentialStore::getInstance()
