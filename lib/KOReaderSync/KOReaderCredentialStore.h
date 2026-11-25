#pragma once
#include <ArduinoJson.h>
#include <HttpHeader.h>
#include <PersistableStore.h>

#include <array>
#include <cstdint>
#include <string>

// Document matching method for KOReader sync
enum class DocumentMatchMethod : uint8_t {
  FILENAME = 0,  // Match by filename (simpler, works across different file sources)
  BINARY = 1,    // Match by partial MD5 of file content (more accurate, but files must be identical)
};

// How manual "Sync Progress" resolves differences after fetching remote progress.
enum class KOReaderSyncBehavior : uint8_t {
  ASK_EVERY_TIME = 0,  // Preserve legacy behavior: always show Apply/Upload choices.
  SMART = 1,           // Auto-resolve simple cases using furthest progress.
};

// Whether the configured server accepts the CrossPoint stats and clippings API.
enum class SyncServerSupport : uint8_t {
  UNKNOWN = 0,      // Not probed yet for the current server URL.
  SUPPORTED = 1,    // CrossPoint Sync (or compatible) extensions are available.
  UNSUPPORTED = 2,  // KOSync-only server: progress sync only.
};

/**
 * Singleton class for storing KOReader sync credentials on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON (not cryptographically secure,
 * but prevents casual reading and ties credentials to the specific device).
 */

enum class KOReaderServerType : uint8_t { CROSSPOINT = 0, KOSYNC = 1, OTHER = 2 };

class KOReaderCredentialStore : public PersistableStore<KOReaderCredentialStore> {
 private:
  KOReaderServerType serverType = KOReaderServerType::CROSSPOINT;
  std::array<HttpHeader, 2> customHeaders;
  std::string username;
  std::string password;
  std::string serverUrl;                                            // Custom sync server URL (empty = default)
  DocumentMatchMethod matchMethod = DocumentMatchMethod::FILENAME;  // Default to filename for compatibility
  // Off by default: syncing stats and clippings is always an explicit opt-in.
  bool syncStats = false;
  bool syncClippings = false;
  SyncServerSupport serverSupport = SyncServerSupport::UNKNOWN;
  std::string serverSupportUrl;  // Base URL serverSupport was learned for.
  bool sendMetadata = true;      // Send document metadata with progress sync
  KOReaderSyncBehavior syncBehavior = KOReaderSyncBehavior::SMART;

  // Private constructor for singleton
  KOReaderCredentialStore() = default;
  ~KOReaderCredentialStore() = default;

  friend class PersistableStore<KOReaderCredentialStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/koreader.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Credential management
  void setCredentials(const std::string& user, const std::string& pass);
  const std::string& getUsername() const {
    ensureLoaded();
    return username;
  }
  const std::string& getPassword() const {
    ensureLoaded();
    return password;
  }

  // Get MD5 hash of password for API authentication
  std::string getMd5Password() const;

  // Check if credentials are set
  bool hasCredentials() const;

  // Server URL management
  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const {
    ensureLoaded();
    return serverUrl;
  }

  // Get base URL for API calls (with http:// normalization if no protocol, falls back to default)
  std::string getBaseUrl() const;

  // Whether API calls target the CrossPoint sync server that supports protocol extensions.
  bool usesCrossPointSyncServer() const;

  void setServerType(KOReaderServerType type);
  KOReaderServerType getServerType() const {
    ensureLoaded();
    return serverType;
  }
  bool supportsRichProgress() const { return getServerType() != KOReaderServerType::KOSYNC; }
  bool supportsExtendedMetadata() const { return getServerType() == KOReaderServerType::OTHER; }
  bool setCustomHeader(size_t index, const std::string& name, const std::string& value);
  const std::array<HttpHeader, 2>& getCustomHeaders() const {
    ensureLoaded();
    return customHeaders;
  }

  // Document matching method
  void setMatchMethod(DocumentMatchMethod method);
  DocumentMatchMethod getMatchMethod() const {
    ensureLoaded();
    return matchMethod;
  }

  // Send metadata setting
  void setSendMetadata(bool enabled);
  bool getSendMetadata() const {
    ensureLoaded();
    return sendMetadata;
  }

  // Effective "include in sync" values: the user's choice, except that an
  // unsupported server always reads as off.
  void setSyncStats(bool enabled);
  bool getSyncStats() const;
  void setSyncClippings(bool enabled);
  bool getSyncClippings() const;

  // Server capability for the current base URL. The default server is always
  // CrossPoint Sync; other servers stay UNKNOWN until probed or an upload answers.
  SyncServerSupport getServerSupport() const;
  // Persists only when the value changes for the current server.
  void setServerSupport(SyncServerSupport support);

  // Sync behavior
  void setSyncBehavior(KOReaderSyncBehavior behavior);
  KOReaderSyncBehavior getSyncBehavior() const {
    ensureLoaded();
    return syncBehavior;
  }
};

// Helper macro to access credential store
#define KOREADER_STORE KOReaderCredentialStore::getInstance()
