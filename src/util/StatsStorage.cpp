#include "StatsStorage.h"

#include <HalStorage.h>
#include <KOReaderSyncClient.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "sync/SyncFiles.h"

namespace {
constexpr const char* ROOT = "/.crosspoint";
constexpr const char* OWNER = "/stats_origin.bin";

bool validId(const char* id) {
  if (strcmp(id, "crossink-simulator") == 0) return true;
  if (strncmp(id, "crossink-", 9) != 0 || strlen(id) != 21) return false;
  for (size_t i = 9; i < 21; ++i)
    if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
  return true;
}

bool readOwner(const std::string& path, char* out, bool& unreadable) {
  FsFile file;
  if (!Storage.openFileForRead("StatsOrigin", path, file)) {
    unreadable = true;
    return false;
  }
  uint8_t data[37]{};
  const size_t size = file.fileSize();
  const int bytes = file.read(data, sizeof(data));
  const bool closed = file.close();
  if (bytes < 0 || static_cast<size_t>(bytes) != (size < sizeof(data) ? size : sizeof(data)) || !closed) {
    unreadable = true;
    return false;
  }
  const bool complete = size == sizeof(data);
  uint32_t checksum = 0;
  for (size_t i = 0; i < 4; ++i) checksum |= uint32_t(data[33 + i]) << (8 * i);
  // A newer format is uncertain data, even in .tmp; older firmware must not
  // replace a complete claim it does not understand. An empty temp is a known
  // interruption between opening the first claim and writing its version.
  if (size > sizeof(data) || (size && data[0] != 1)) {
    unreadable = true;
    return false;
  }
  if (!complete || SyncFiles::crc(data, 33, 0) != checksum) return false;
  if (data[32] != 0) {
    unreadable = true;
    return false;
  }
  memcpy(out, data + 1, 32);
  if (validId(out)) return true;
  unreadable = true;
  return false;
}

bool owner(const std::string& path, const char* fallback, char* out, bool create) {
  bool exists = false;
  bool unreadable = false;
  // Origin claims are immutable. Validate recovery files without promoting
  // them: library completion peeks must never rename or write any files.
  for (const char* suffix : {"", ".tmp", ".bak"}) {
    const std::string candidate = path + suffix;
    if (!Storage.exists(candidate.c_str())) continue;
    exists = true;
    if (readOwner(candidate, out, unreadable)) return true;
  }
  // With neither a published claim nor a backup, an invalid temp can only
  // precede the first commit. No legacy move has started, so retry that claim.
  // A read error is not proof of a torn write and must never permit deletion.
  const bool interrupted =
      exists && !unreadable && !Storage.exists(path.c_str()) && !Storage.exists((path + ".bak").c_str());
  if (exists && !interrupted) {
    LOG_ERR("StatsOrigin", "Cannot read stats origin: %s", path.c_str());
    return false;
  }
  snprintf(out, 32, "%s", fallback);
  if (!create) return true;
  if (interrupted && !Storage.remove((path + ".tmp").c_str())) {
    LOG_ERR("StatsOrigin", "Cannot retry interrupted stats origin: %s", path.c_str());
    return false;
  }
  SyncFiles::Writer writer(path.c_str());
  // Fixed-size, zero-padded ID; the existing CRC/transaction protects the claim
  // before any legacy file is moved. An interrupted migration can safely retry.
  char stored[32]{};
  snprintf(stored, sizeof(stored), "%s", fallback);
  if (writer.open() && writer.pod(uint8_t{1}) && writer.write(stored, sizeof(stored)) && writer.commit()) return true;
  LOG_ERR("StatsOrigin", "Cannot preserve stats origin: %s", path.c_str());
  return false;
}
}  // namespace

static std::string resolvePath(const std::string& legacyPath, bool directory, bool readOnly) {
  char id[32]{};
  if (!KOReaderSyncClient::readerId(id, sizeof(id)) || !validId(id)) {
    LOG_ERR("StatsOrigin", "Cannot read device identity");
    return {};
  }
  const size_t extension = directory ? legacyPath.size() : legacyPath.rfind(".bin");
  if (extension == std::string::npos) return {};
  // Paths are cold storage/checkpoint work, not render/page-turn allocations.
  const std::string local = legacyPath.substr(0, extension) + "_" + id + legacyPath.substr(extension);
  if (!readOnly && !Storage.exists(ROOT) && !Storage.mkdir(ROOT)) {
    LOG_ERR("StatsOrigin", "Cannot create stats root");
    return {};
  }
  char rootOwner[32];
  if (!owner(std::string(ROOT) + OWNER, id, rootOwner, !readOnly)) return {};
  bool legacyExists = Storage.exists(legacyPath.c_str());
  if (!directory)
    for (const char* suffix : {".bak", ".tmp"}) legacyExists |= Storage.exists((legacyPath + suffix).c_str());
  if (!legacyExists) return local;

  // A per-book claim also travels with a partial cache-folder copy. The root
  // claim covers old book files not opened yet when a whole card is copied.
  const std::string parent = legacyPath.substr(0, legacyPath.find_last_of('/'));
  char legacyOwner[32];
  if (!owner(parent + OWNER, rootOwner, legacyOwner, !readOnly)) return {};
  if (strcmp(legacyOwner, id) != 0) return local;
  // Older firmware can recreate legacy stats after migration. Keep using the
  // established local family without mixing its slots or totals with that copy.
  // A remaining legacy primary identifies a separate family: migration moves
  // it first. If only legacy recovery slots remain, finish that interrupted move.
  bool localExists = Storage.exists(local.c_str());
  if (!directory)
    for (const char* suffix : {".bak", ".tmp"}) localExists |= Storage.exists((local + suffix).c_str());
  if (localExists && Storage.exists(legacyPath.c_str())) return local;
  if (readOnly) return localExists ? local : legacyPath;
  for (const char* suffix : {"", ".bak", ".tmp"}) {
    if (directory && *suffix) break;
    const std::string from = legacyPath + suffix;
    const std::string to = local + suffix;
    if (!Storage.exists(from.c_str())) continue;
    if (Storage.exists(to.c_str())) continue;
    if (!Storage.rename(from.c_str(), to.c_str())) {
      LOG_ERR("StatsOrigin", "Cannot migrate stats: %s", from.c_str());
      return {};
    }
  }
  return local;
}

namespace {
bool blocked = false;
}
std::string StatsStorage::localPath(const std::string& path, bool directory, bool readOnly) {
  if (blocked) return {};
  std::string local = resolvePath(path, directory, readOnly);
  // load() returns empty counters on failure. Do not later save those empty
  // counters over recovered history after a transient migration failure.
  if (local.empty()) blocked = true;
  return local;
}
#ifdef STATS_STORAGE_TEST
void resetStatsStorageForTest() { blocked = false; }
#endif
