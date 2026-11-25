#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

enum class OpdsFilenameFormat : uint8_t {
  AUTHOR_TITLE = 0,
  TITLE_AUTHOR = 1,
};

const char* opdsFilenameFormatToJson(OpdsFilenameFormat format);
OpdsFilenameFormat opdsFilenameFormatFromJson(const char* value);
// A missing field preserves the caller's current value (older web clients).
bool applyOpdsDownloadFolder(JsonVariantConst object, std::string& folder);

struct OpdsServer {
  std::string name;
  std::string url;
  std::string username;
  std::string password;  // Plaintext in memory; obfuscated with hardware key on disk
  OpdsFilenameFormat filenameFormat = OpdsFilenameFormat::AUTHOR_TITLE;
  std::string downloadFolder;  // Empty: shared default; '/': SD root
  bool useServerFilename = false;
};

// Missing fields preserve current values; legacy server_filename enables the toggle.
bool applyOpdsFilenameSettings(JsonVariantConst object, OpdsServer& server);

/**
 * Singleton class for storing OPDS server configurations on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON.
 */
class OpdsServerStore : public PersistableStore<OpdsServerStore> {
 private:
  std::vector<OpdsServer> servers;
  bool loaded_ = false;

  static constexpr size_t MAX_SERVERS = 8;

  OpdsServerStore() = default;
  bool migrateFromSettings();

  friend class PersistableStore<OpdsServerStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/opds.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
  bool loadFromFile();
  void ensureLoaded() const;
  void release();

  bool addServer(const OpdsServer& server);
  bool updateServer(size_t index, const OpdsServer& server);
  bool removeServer(size_t index);

  const std::vector<OpdsServer>& getServers() const {
    ensureLoaded();
    return servers;
  }
  const OpdsServer* getServer(size_t index) const;
  size_t getCount() const {
    ensureLoaded();
    return servers.size();
  }
  bool hasServers() const {
    ensureLoaded();
    return !servers.empty();
  }
};

#define OPDS_STORE OpdsServerStore::getInstance()
