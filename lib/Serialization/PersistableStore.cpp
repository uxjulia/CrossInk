#include "PersistableStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include "AtomicFile.h"

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  Storage.mkdir("/.crosspoint");
  String json;
  serializeJson(doc, json);
  if (!Storage.writeFile(path, json)) {
    LOG_ERR("PERSIST", "Failed to write %s", path);
    return false;
  }
  return true;
}

bool PersistableStoreBase::writeDocToFileAtomically(const char* path, const JsonDocument& doc) {
  Storage.mkdir("/.crosspoint");
  String json;
  serializeJson(doc, json);

  const std::string tempPath = std::string(path) + ".tmp";
  const std::string backupPath = std::string(path) + ".bak";
  if (Storage.exists(tempPath.c_str()) && !Storage.remove(tempPath.c_str())) {
    LOG_ERR("PERSIST", "Failed to remove stale temporary file %s", tempPath.c_str());
    return false;
  }
  if (!Storage.writeFile(tempPath.c_str(), json)) {
    LOG_ERR("PERSIST", "Failed to write temporary file %s", tempPath.c_str());
    return false;
  }

  const bool hadOriginal = Storage.exists(path);
  if (hadOriginal) {
    if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
      LOG_ERR("PERSIST", "Failed to remove stale backup %s", backupPath.c_str());
      Storage.remove(tempPath.c_str());
      return false;
    }
    if (!Storage.rename(path, backupPath.c_str())) {
      LOG_ERR("PERSIST", "Failed to back up %s", path);
      Storage.remove(tempPath.c_str());
      return false;
    }
  }

  if (!Storage.rename(tempPath.c_str(), path)) {
    LOG_ERR("PERSIST", "Failed to replace %s", path);
    if (hadOriginal && !Storage.rename(backupPath.c_str(), path)) {
      LOG_ERR("PERSIST", "Failed to restore backup %s", backupPath.c_str());
    }
    Storage.remove(tempPath.c_str());
    return false;
  }

  if (hadOriginal && Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
    LOG_ERR("PERSIST", "Failed to remove completed backup %s", backupPath.c_str());
  }
  return true;
}

bool PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc) {
  std::string recoveryPath;
  const char* readPath = path;
  if (!Storage.exists(path)) {
    recoveryPath = std::string(path) + ".bak";
    if (!Storage.exists(recoveryPath.c_str())) {
      return false;  // Expected on first boot — not an error.
    }
    if (Storage.rename(recoveryPath.c_str(), path)) {
      LOG_INF("PERSIST", "Recovered interrupted write for %s", path);
    } else {
      LOG_ERR("PERSIST", "Could not restore backup for %s; reading backup directly", path);
      readPath = recoveryPath.c_str();
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

namespace {
const atomic_file::Operations ATOMIC_OPS{
    nullptr,
    [](void*, const char* path) { return Storage.exists(path); },
    [](void*, const char* path) { return Storage.remove(path); },
    [](void*, const char* from, const char* to) { return Storage.rename(from, to); },
};
}  // namespace

bool PersistableStoreBase::recoverAtomicFile(const char* path) {
  const std::string backup = std::string(path) + ".bak";
  if (atomic_file::recover(ATOMIC_OPS, path, backup.c_str())) return true;
  LOG_ERR("PERSIST", "Could not recover %s from backup", path);
  return false;
}

bool PersistableStoreBase::writeDocToFileAtomic(const char* path, const JsonDocument& doc) {
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
    LOG_ERR("PERSIST", "Could not publish %s; previous settings retained for recovery", path);
    return false;
  }
  return true;
}
