#include "WifiCredentialStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>
#include <Serialization.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace {
constexpr uint8_t WIFI_FILE_VERSION = 2;
constexpr char WIFI_FILE_BIN[] = "/.crosspoint/wifi.bin";
constexpr char WIFI_FILE_BAK[] = "/.crosspoint/wifi.bin.bak";
constexpr uint8_t LEGACY_OBFUSCATION_KEY[] = {0x43, 0x72, 0x6F, 0x73, 0x73, 0x50, 0x6F, 0x69, 0x6E, 0x74};
constexpr size_t LEGACY_KEY_LENGTH = sizeof(LEGACY_OBFUSCATION_KEY);

void legacyDeobfuscate(std::string& data) {
  for (size_t i = 0; i < data.size(); i++) {
    data[i] ^= LEGACY_OBFUSCATION_KEY[i % LEGACY_KEY_LENGTH];
  }
}
}  // namespace

void WifiCredentialStore::toJson(JsonDocument& doc) const {
  std::lock_guard<std::mutex> lock(credentialMutex);
  doc["lastConnectedSsid"] = lastConnectedSsid;

  JsonArray arr = doc["credentials"].to<JsonArray>();
  for (const auto& cred : credentials) {
    JsonObject obj = arr.add<JsonObject>();
    obj["ssid"] = cred.ssid;
    obj["password_obf"] = obfuscation::obfuscateToBase64(cred.password);
  }
}

bool WifiCredentialStore::fromJson(JsonVariantConst doc) {
  std::lock_guard<std::mutex> lock(credentialMutex);
  lastConnectedSsid = doc["lastConnectedSsid"] | "";

  // Tolerate a missing/invalid 'credentials' key (treat as empty list); only
  // a JSON parse error is fatal. A null JsonArray iterates zero times.
  credentials.clear();
  JsonArrayConst arr = doc["credentials"].as<JsonArrayConst>();
  credentials.reserve(std::min(arr.size(), MAX_NETWORKS));
  bool needsResave = false;

  for (JsonObjectConst obj : arr) {
    if (credentials.size() >= MAX_NETWORKS) break;
    WifiCredential cred;
    cred.ssid = obj["ssid"] | "";
    if (cred.ssid.empty()) {
      LOG_ERR("WCS", "Skipping WiFi credential with empty SSID");
      continue;
    }

    obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
    cred.password = obfuscation::deobfuscateFromBase64(obj["password_obf"] | "", MAX_PASSWORD_LENGTH, &status);
    if (status == obfuscation::DecodeStatus::TOO_LONG) {
      LOG_ERR("WCS", "Skipping WiFi credential with oversized password: %s", cred.ssid.c_str());
      needsResave = true;
      continue;
    }
    if (status == obfuscation::DecodeStatus::LEGACY && !cred.password.empty()) {
      needsResave = true;
    }
    if (status == obfuscation::DecodeStatus::INVALID || status == obfuscation::DecodeStatus::EMPTY ||
        cred.password.empty()) {
      const char* legacyPassword = obj["password"] | "";
      if (strlen(legacyPassword) > MAX_PASSWORD_LENGTH) {
        LOG_ERR("WCS", "Skipping WiFi credential with oversized plaintext password: %s", cred.ssid.c_str());
        needsResave = true;
        continue;
      }
      cred.password = legacyPassword;
      if (!cred.password.empty()) needsResave = true;
    }
    if (status == obfuscation::DecodeStatus::INVALID && cred.password.empty()) {
      LOG_ERR("WCS", "Skipping WiFi credential with unreadable password: %s", cred.ssid.c_str());
      continue;
    }
    credentials.push_back(std::move(cred));
  }

  if (needsResave) {
    LOG_DBG("WCS", "Resaving JSON with obfuscated passwords");
    requestResave();
  }

  return true;
}

bool WifiCredentialStore::loadFromFile() {
  LOG_INF("WCS", "load start");
  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    credentials.clear();
    lastConnectedSsid.clear();
  }

  const bool hasStoreFile = Storage.exists(getFilePath());
  if (PersistableStore<WifiCredentialStore>::loadFromFile()) {
    LOG_INF("WCS", "load JSON complete");
    return true;
  }
  if (hasStoreFile) {
    LOG_INF("WCS", "load JSON present but unreadable");
    return false;
  }

  if (Storage.exists(WIFI_FILE_BIN)) {
    LOG_INF("WCS", "load legacy migration attempted");
    if (!loadFromBinaryFile()) {
      LOG_INF("WCS", "load legacy migration failed: read");
      return false;
    }
    if (saveToFile()) {
      Storage.rename(WIFI_FILE_BIN, WIFI_FILE_BAK);
      LOG_INF("WCS", "load legacy migration complete");
      return true;
    }
    LOG_INF("WCS", "load legacy migration failed: save");
    return false;
  }

  LOG_INF("WCS", "load no store");
  return false;
}

void WifiCredentialStore::ensureLoaded() const {
  std::call_once(loadOnce, [this] { const_cast<WifiCredentialStore*>(this)->loadFromFile(); });
}

bool WifiCredentialStore::loadFromBinaryFile() {
  HalFile file;
  if (!Storage.openFileForRead("WCS", WIFI_FILE_BIN, file)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(file, version);
  if (version > WIFI_FILE_VERSION) {
    LOG_DBG("WCS", "Unknown file version: %u", version);
    return false;
  }

  if (version >= 2) {
    serialization::readString(file, lastConnectedSsid);
  } else {
    lastConnectedSsid.clear();
  }

  uint8_t count;
  serialization::readPod(file, count);

  credentials.clear();
  credentials.reserve(std::min<size_t>(count, MAX_NETWORKS));
  for (uint8_t i = 0; i < count && i < MAX_NETWORKS; i++) {
    WifiCredential cred;
    serialization::readString(file, cred.ssid);
    serialization::readString(file, cred.password);
    legacyDeobfuscate(cred.password);
    credentials.push_back(std::move(cred));
  }

  return true;
}

bool WifiCredentialStore::addCredential(const std::string& ssid, const std::string& password) {
  ensureLoaded();
  if (password.size() > MAX_PASSWORD_LENGTH) {
    LOG_ERR("WCS", "Refusing oversized WiFi password for %s", ssid.c_str());
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    const auto cred = find_if(credentials.begin(), credentials.end(),
                              [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });
    if (cred != credentials.end()) {
      cred->password = password;
    } else {
      if (credentials.size() >= MAX_NETWORKS) {
        LOG_DBG("WCS", "Cannot add more networks, limit of %zu reached", MAX_NETWORKS);
        return false;
      }

      credentials.push_back({ssid, password});
    }
  }
  return saveToFile();
}

bool WifiCredentialStore::removeCredential(const std::string& ssid) {
  ensureLoaded();

  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    const auto cred = find_if(credentials.begin(), credentials.end(),
                              [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });
    if (cred == credentials.end()) return false;
    credentials.erase(cred);
    if (ssid == lastConnectedSsid) lastConnectedSsid.clear();
  }
  return saveToFile();
}

std::optional<WifiCredential> WifiCredentialStore::findCredential(const std::string& ssid) const {
  ensureLoaded();
  std::lock_guard<std::mutex> lock(credentialMutex);

  const auto cred = find_if(credentials.begin(), credentials.end(),
                            [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; });
  return cred != credentials.end() ? std::optional<WifiCredential>(*cred) : std::nullopt;
}

std::optional<WifiCredential> WifiCredentialStore::getCredentialAt(const size_t index) const {
  ensureLoaded();
  std::lock_guard<std::mutex> lock(credentialMutex);
  return index < credentials.size() ? std::optional<WifiCredential>(credentials[index]) : std::nullopt;
}

std::optional<std::string> WifiCredentialStore::getSsidAt(const size_t index) const {
  ensureLoaded();
  std::lock_guard<std::mutex> lock(credentialMutex);
  return index < credentials.size() ? std::optional<std::string>(credentials[index].ssid) : std::nullopt;
}

size_t WifiCredentialStore::getCredentialCount() const {
  ensureLoaded();
  std::lock_guard<std::mutex> lock(credentialMutex);
  return credentials.size();
}

std::vector<WifiCredentialSummary> WifiCredentialStore::getCredentialSummaries() const {
  ensureLoaded();
  std::lock_guard<std::mutex> lock(credentialMutex);
  std::vector<WifiCredentialSummary> summaries;
  summaries.reserve(credentials.size());
  for (const auto& credential : credentials) {
    summaries.push_back({credential.ssid, !credential.password.empty(), credential.ssid == lastConnectedSsid});
  }
  return summaries;
}

bool WifiCredentialStore::hasSavedCredential(const std::string& ssid) const {
  ensureLoaded();
  std::lock_guard<std::mutex> lock(credentialMutex);
  return find_if(credentials.begin(), credentials.end(),
                 [&ssid](const WifiCredential& cred) { return cred.ssid == ssid; }) != credentials.end();
}

void WifiCredentialStore::setLastConnectedSsid(const std::string& ssid) {
  ensureLoaded();

  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    if (lastConnectedSsid == ssid) return;
    lastConnectedSsid = ssid;
  }
  saveToFile();
}

std::string WifiCredentialStore::getLastConnectedSsid() const {
  ensureLoaded();
  std::lock_guard<std::mutex> lock(credentialMutex);
  return lastConnectedSsid;
}

void WifiCredentialStore::clearAll() {
  ensureLoaded();

  {
    std::lock_guard<std::mutex> lock(credentialMutex);
    credentials.clear();
    lastConnectedSsid.clear();
  }
  saveToFile();
}
