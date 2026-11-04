#include "OpdsServerStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "CrossPointSettings.h"
#include "util/OpdsDownloadFolder.h"

namespace {
constexpr char FILENAME_FORMAT_AUTHOR_TITLE[] = "author_title";
constexpr char FILENAME_FORMAT_TITLE_AUTHOR[] = "title_author";
constexpr char FILENAME_FORMAT_SERVER[] = "server_filename";
}  // namespace

const char* opdsFilenameFormatToJson(const OpdsFilenameFormat format) {
  switch (format) {
    case OpdsFilenameFormat::TITLE_AUTHOR:
      return FILENAME_FORMAT_TITLE_AUTHOR;
    case OpdsFilenameFormat::AUTHOR_TITLE:
    default:
      return FILENAME_FORMAT_AUTHOR_TITLE;
  }
}

OpdsFilenameFormat opdsFilenameFormatFromJson(const char* value) {
  if (value && strcmp(value, FILENAME_FORMAT_TITLE_AUTHOR) == 0) {
    return OpdsFilenameFormat::TITLE_AUTHOR;
  }
  return OpdsFilenameFormat::AUTHOR_TITLE;
}

bool applyOpdsFilenameSettings(JsonVariantConst object, OpdsServer& server) {
  const auto useServerFilename = object["useServerFilename"];
  if (!useServerFilename.isUnbound() && !useServerFilename.is<bool>()) return false;
  if (const char* format = object["filenameFormat"]) {
    server.filenameFormat = opdsFilenameFormatFromJson(format);
    if (useServerFilename.isUnbound() && strcmp(format, FILENAME_FORMAT_SERVER) == 0) {
      server.useServerFilename = true;
    }
  }
  if (!useServerFilename.isUnbound()) server.useServerFilename = useServerFilename.as<bool>();
  return true;
}

bool applyOpdsDownloadFolder(JsonVariantConst object, std::string& folder) {
  const auto value = object["downloadFolder"];
  if (value.isUnbound()) return true;
  if (!value.is<const char*>()) return false;
  const auto input = value.as<JsonString>();
  return OpdsDownloadFolder::normalize(std::string_view(input.c_str(), input.size()), folder);
}

void OpdsServerStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["servers"].to<JsonArray>();
  for (const auto& server : servers) {
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = server.name;
    obj["url"] = server.url;
    obj["username"] = server.username;
    obj["password_obf"] = obfuscation::obfuscateToBase64(server.password);
    obj["filenameFormat"] = opdsFilenameFormatToJson(server.filenameFormat);
    obj["useServerFilename"] = server.useServerFilename;
    obj["downloadFolder"] = server.downloadFolder;
  }
}

bool OpdsServerStore::fromJson(JsonVariantConst doc) {
  // Tolerate a missing/invalid 'servers' key (a null array iterates zero
  // times). Reject invalid folder overrides rather than silently rerouting.
  servers.clear();
  JsonArrayConst arr = doc["servers"].as<JsonArrayConst>();
  servers.reserve(std::min(arr.size(), MAX_SERVERS));
  bool needsResave = false;

  for (JsonObjectConst obj : arr) {
    if (servers.size() >= OpdsServerStore::MAX_SERVERS) break;
    OpdsServer server;
    server.name = obj["name"] | "";
    server.url = obj["url"] | "";
    server.username = obj["username"] | "";
    if (!applyOpdsFilenameSettings(obj, server)) {
      LOG_ERR("OPS", "Invalid OPDS server filename option");
      servers.clear();
      return false;
    }
    if (!applyOpdsDownloadFolder(obj, server.downloadFolder)) {
      LOG_ERR("OPS", "Invalid OPDS download folder");
      servers.clear();
      return false;
    }
    obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
    server.password = obfuscation::deobfuscateFromBase64(obj["password_obf"] | "", &status);
    if (status == obfuscation::DecodeStatus::LEGACY && !server.password.empty()) {
      needsResave = true;
    }
    if (status == obfuscation::DecodeStatus::INVALID || status == obfuscation::DecodeStatus::EMPTY ||
        server.password.empty()) {
      server.password = obj["password"] | "";
      if (!server.password.empty()) needsResave = true;
    }
    if (status == obfuscation::DecodeStatus::INVALID && server.password.empty()) {
      LOG_ERR("OPS", "Ignoring unreadable password for OPDS server: %s", server.name.c_str());
    }
    servers.push_back(std::move(server));
  }

  if (needsResave) {
    LOG_DBG("OPS", "Resaving JSON with obfuscated passwords");
    requestResave();
  }

  return true;
}

bool OpdsServerStore::loadFromFile() {
  servers.clear();
  loaded_ = true;
  const bool hasStoreFile = Storage.exists(getFilePath());
  if (PersistableStore<OpdsServerStore>::loadFromFile()) {
    return true;
  }
  if (hasStoreFile) {
    return false;
  }

  if (migrateFromSettings()) {
    LOG_DBG("OPS", "Migrated legacy OPDS settings");
    return true;
  }

  return false;
}

void OpdsServerStore::ensureLoaded() const {
  if (loaded_) return;
  const_cast<OpdsServerStore*>(this)->loadFromFile();
}

void OpdsServerStore::release() {
  std::vector<OpdsServer>().swap(servers);
  loaded_ = false;
}

bool OpdsServerStore::migrateFromSettings() {
  if (strlen(SETTINGS.opdsServerUrl) == 0) {
    return false;
  }

  OpdsServer server;
  server.name = "OPDS Server";
  server.url = SETTINGS.opdsServerUrl;
  server.username = SETTINGS.opdsUsername;
  server.password = SETTINGS.opdsPassword;
  servers.push_back(std::move(server));

  if (saveToFile()) {
    // Clear legacy fields so migration won't run again on next boot.
    SETTINGS.opdsServerUrl[0] = '\0';
    SETTINGS.opdsUsername[0] = '\0';
    SETTINGS.opdsPassword[0] = '\0';
    SETTINGS.saveToFile();
    LOG_DBG("OPS", "Migrated single-server OPDS config to opds.json");
    return true;
  }

  // Save failed; roll back in-memory state so callers don't see a partial migration.
  servers.clear();
  return false;
}

bool OpdsServerStore::addServer(const OpdsServer& server) {
  ensureLoaded();
  if (servers.size() >= MAX_SERVERS) {
    LOG_DBG("OPS", "Cannot add more servers, limit of %zu reached", MAX_SERVERS);
    return false;
  }

  servers.push_back(server);
  if (!OpdsDownloadFolder::normalize(servers.back().downloadFolder, servers.back().downloadFolder)) {
    LOG_ERR("OPS", "Invalid OPDS download folder");
    servers.pop_back();
    return false;
  }
  if (saveToFile()) return true;
  servers.pop_back();
  return false;
}

bool OpdsServerStore::updateServer(size_t index, const OpdsServer& server) {
  ensureLoaded();
  if (index >= servers.size()) {
    return false;
  }

  OpdsServer replacement = server;
  if (!OpdsDownloadFolder::normalize(replacement.downloadFolder, replacement.downloadFolder)) {
    LOG_ERR("OPS", "Invalid OPDS download folder");
    return false;
  }
  std::swap(servers[index], replacement);
  if (saveToFile()) return true;
  std::swap(servers[index], replacement);
  return false;
}

bool OpdsServerStore::removeServer(size_t index) {
  ensureLoaded();
  if (index >= servers.size()) {
    return false;
  }

  servers.erase(servers.begin() + static_cast<ptrdiff_t>(index));
  return saveToFile();
}

const OpdsServer* OpdsServerStore::getServer(size_t index) const {
  ensureLoaded();
  if (index >= servers.size()) {
    return nullptr;
  }
  return &servers[index];
}
