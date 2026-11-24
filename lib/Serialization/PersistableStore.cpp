#include "PersistableStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cctype>

#include "AtomicFile.h"

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  return writeDocToFileAtomic(path, doc);
}

bool PersistableStoreBase::writeDocToFileAtomically(const char* path, const JsonDocument& doc) {
  return writeDocToFileAtomic(path, doc);
}

namespace {
const atomic_file::Operations ATOMIC_OPS{
    nullptr,
    [](void*, const char* path) { return Storage.exists(path); },
    [](void*, const char* path) { return Storage.remove(path); },
    [](void*, const char* from, const char* to) { return Storage.rename(from, to); },
};

enum class OrphanTemporaryStatus { None, Recovered, DiscardedTorn, ValidNotPromoted, Unreadable };

bool isCompleteJsonContainer(const std::string& json, JsonDocument& doc) {
  if (!doc.is<JsonObject>() && !doc.is<JsonArray>()) return false;
  size_t start = 0;
  while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) ++start;
  if (start == json.size() || (json[start] != '{' && json[start] != '[')) return false;

  size_t depth = 0;
  bool inString = false;
  bool escaped = false;
  for (size_t i = start; i < json.size(); ++i) {
    const char byte = json[i];
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (byte == '\\') {
        escaped = true;
      } else if (byte == '"') {
        inString = false;
      }
      continue;
    }
    if (byte == '"') {
      inString = true;
    } else if (byte == '{' || byte == '[') {
      ++depth;
    } else if (byte == '}' || byte == ']') {
      if (depth == 0) return false;
      if (--depth == 0) {
        for (++i; i < json.size(); ++i) {
          if (!std::isspace(static_cast<unsigned char>(json[i]))) return false;
        }
        return true;
      }
    }
  }
  return false;
}

OrphanTemporaryStatus recoverOrphanTemporary(const char* path) {
  const std::string temporary = std::string(path) + ".tmp";
  const std::string backup = std::string(path) + ".bak";
  if (Storage.exists(path) || Storage.exists(backup.c_str()) || !Storage.exists(temporary.c_str())) {
    return OrphanTemporaryStatus::None;
  }

  constexpr size_t MAX_STORE_BYTES = 50000;
  std::string json;
  if (!Storage.readFileToString("PERSIST", temporary, MAX_STORE_BYTES, json)) {
    LOG_ERR("PERSIST", "Could not read interrupted temporary file %s", temporary.c_str());
    return OrphanTemporaryStatus::Unreadable;
  }

  JsonDocument parsed;
  const auto error = json.empty() ? DeserializationError::EmptyInput : deserializeJson(parsed, json);
  if (!error && !isCompleteJsonContainer(json, parsed)) {
    LOG_ERR("PERSIST", "Interrupted temporary file is not a complete store document: %s", temporary.c_str());
    return OrphanTemporaryStatus::Unreadable;
  }
  if (error) {
    // A prefix cut off during serialization is incomplete. Other parse failures
    // or resource errors leave the orphan untouched because it may hold data
    // from another format or a complete file we could not validate.
    if (error != DeserializationError::EmptyInput && error != DeserializationError::IncompleteInput) {
      LOG_ERR("PERSIST", "Could not validate interrupted temporary file %s: %s", temporary.c_str(), error.c_str());
      return OrphanTemporaryStatus::Unreadable;
    }
    if (!Storage.remove(temporary.c_str())) {
      LOG_ERR("PERSIST", "Could not discard incomplete temporary file %s", temporary.c_str());
      return OrphanTemporaryStatus::Unreadable;
    }
    return OrphanTemporaryStatus::DiscardedTorn;
  }

  if (!Storage.rename(temporary.c_str(), path)) {
    LOG_ERR("PERSIST", "Could not restore interrupted temporary file %s", temporary.c_str());
    return OrphanTemporaryStatus::ValidNotPromoted;
  }
  LOG_INF("PERSIST", "Recovered interrupted first save for %s", path);
  return OrphanTemporaryStatus::Recovered;
}

bool canProceedAfterOrphanRecovery(const OrphanTemporaryStatus status) {
  return status != OrphanTemporaryStatus::Unreadable && status != OrphanTemporaryStatus::ValidNotPromoted;
}
}  // namespace

bool PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc) {
  const std::string temporaryPath = std::string(path) + ".tmp";
  std::string recoveryPath;
  const char* readPath = path;
  if (!Storage.exists(path)) {
    recoveryPath = std::string(path) + ".bak";
    if (Storage.exists(recoveryPath.c_str())) {
      if (Storage.rename(recoveryPath.c_str(), path)) {
        LOG_INF("PERSIST", "Recovered interrupted write for %s", path);
      } else {
        LOG_ERR("PERSIST", "Could not restore backup for %s; reading backup directly", path);
        readPath = recoveryPath.c_str();
      }
    } else {
      const auto tempStatus = recoverOrphanTemporary(path);
      if (tempStatus == OrphanTemporaryStatus::Unreadable) return false;
      if (tempStatus == OrphanTemporaryStatus::ValidNotPromoted) {
        readPath = temporaryPath.c_str();
      } else if (!Storage.exists(path)) {
        return false;  // Expected on first boot or after discarding a torn save.
      }
    }
  }
  // Preserve the legacy 50,000-byte limit, but reject an oversized/short read
  // instead of parsing a truncated prefix. The HAL sizes once and reads in bulk,
  // avoiding one String append and SD call per byte. This cold-path buffer is
  // bounded; a stack buffer of this size is unsafe on device tasks.
  constexpr size_t MAX_STORE_BYTES = 50000;
  std::string json;
  if (!Storage.readFileToString("PERSIST", readPath, MAX_STORE_BYTES, json) || json.empty()) {
    LOG_ERR("PERSIST", "Failed to read %s (empty or incomplete)", path);
    return false;
  }
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("PERSIST", "JSON parse error in %s: %s", path, error.c_str());
    return false;
  }
  return true;
}

bool PersistableStoreBase::hasStoreFile(const char* path) {
  const std::string backup = std::string(path) + ".bak";
  const std::string temporary = std::string(path) + ".tmp";
  return Storage.exists(path) || Storage.exists(backup.c_str()) || Storage.exists(temporary.c_str());
}

bool PersistableStoreBase::recoverAtomicFile(const char* path) {
  const std::string backup = std::string(path) + ".bak";
  if (!atomic_file::recover(ATOMIC_OPS, path, backup.c_str())) {
    LOG_ERR("PERSIST", "Could not recover %s from backup", path);
    return false;
  }
  const auto temporary = recoverOrphanTemporary(path);
  if (!canProceedAfterOrphanRecovery(temporary)) {
    LOG_ERR("PERSIST", "Could not recover %s from an interrupted save", path);
    return false;
  }
  return true;
}

bool PersistableStoreBase::writeDocToFileAtomic(const char* path, const JsonDocument& doc) {
  if (doc.overflowed()) {
    LOG_ERR("PERSIST", "Incomplete JSON document for %s", path);
    return false;
  }
  if (!recoverAtomicFile(path)) return false;
  Storage.mkdir("/.crosspoint");
  // Two small cold-path names; stream the JSON instead of allocating its full
  // serialized representation alongside the document on C3.
  const std::string temporary = std::string(path) + ".tmp";
  const std::string backup = std::string(path) + ".bak";
  HalFile file;
  if (!Storage.openFileForWrite("PERSIST", temporary.c_str(), file)) return false;
  const size_t expected = measureJson(doc);
  const size_t written = serializeJson(doc, file);
  const bool synced = written == expected && file.sync();
  const bool closed = file.close();
  if (!synced || !closed) {
    LOG_ERR("PERSIST", "Could not write/sync %s", temporary.c_str());
    return false;
  }
  if (!atomic_file::publish(ATOMIC_OPS, path, temporary.c_str(), backup.c_str())) {
    Storage.remove(temporary.c_str());
    LOG_ERR("PERSIST", "Could not publish %s; previous settings retained for recovery", path);
    return false;
  }
  return true;
}
