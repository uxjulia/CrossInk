#include "ReadingSessionJournal.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#ifndef SIMULATOR
#include <esp_system.h>
#else
#include <random>
#endif
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "sync/BookSyncFile.h"
#include "sync/SyncFiles.h"
#include "util/StatsStorage.h"

namespace ReadingSessionJournal {
namespace {
constexpr const char* DIRECTORY = "/.crosspoint/reading_sessions";
constexpr uint8_t VERSION = 1;

bool hexId(const char* id) {
  if (strlen(id) != 32) return false;
  for (size_t i = 0; i < 32; ++i)
    if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
  return true;
}
bool valid(const Record& r) {
  return r.id[32] == 0 && r.document[32] == 0 && hexId(r.id) && hexId(r.document) && r.startedAt >= 946684800 &&
         r.endedAt >= r.startedAt && r.seconds >= 10 && r.endedAt - r.startedAt <= 7 * 86400 &&
         r.seconds <= r.endedAt - r.startedAt && r.utcOffsetMinutes >= -720 && r.utcOffsetMinutes <= 840;
}
void pathFor(char* out, size_t capacity, const std::string& directory, const char* id) {
  snprintf(out, capacity, "%s/%s.bin", directory.c_str(), id);
}
uint64_t localSeconds(const ReadingStatsDateTime& t) {
  return 946684800ULL + uint64_t(readingStatsDayIndex(t.date)) * 86400 + t.hour * 3600u + t.minute * 60u + t.second;
}
uint32_t randomWord() {
#ifndef SIMULATOR
  return esp_random();
#else
  static std::random_device random;
  return random();
#endif
}
}  // namespace

bool save(const Record& record) {
  if (!valid(record)) return false;
  const std::string directory = StatsStorage::localPath(DIRECTORY, true);
  if (directory.empty()) return false;
  if (!Storage.exists(directory.c_str()) && !Storage.mkdir(directory.c_str())) {
    LOG_ERR("SessionStats", "Cannot create reading session outbox");
    return false;
  }
  char path[96];
  pathFor(path, sizeof(path), directory, record.id);
  SyncFiles::Writer writer(path);
  // Field-by-field serialization avoids ABI-dependent padding in persisted data.
  return writer.open() && writer.pod(VERSION) && writer.write(record.id, 32) && writer.write(record.document, 32) &&
         writer.pod(record.startedAt) && writer.pod(record.endedAt) && writer.pod(record.seconds) &&
         writer.pod(record.utcOffsetMinutes) && writer.commit();
}

bool Session::checkpoint() {
  if (pending.seconds < 10 || pending.seconds == checkpointSeconds) return true;
  if (!save(pending)) {
    LOG_ERR("SessionStats", "Cannot save reading session; retaining for retry");
    return false;
  }
  checkpointSeconds = pending.seconds;
  ++checkpointRevision;
  return true;
}

bool Session::checkpointIfDue() {
  return pending.seconds - checkpointSeconds < READING_STATS_CHECKPOINT_SECONDS || checkpoint();
}

bool Session::flush() {
  if (!checkpoint()) return false;
  pending = {};
  checkpointSeconds = 0;
  return true;
}

bool Session::accept(const char* bookPath, const ReadingStatsDateTime& end, const uint8_t offset,
                     const uint32_t seconds, const bool allowCheckpoint) {
  if (!isValidReadingStatsInterval(intervalStart, end, intervalOffset, offset, seconds)) return true;
  const uint64_t start = localSeconds(intervalStart);
  const int16_t minutes = (int(offset) - 48) * 15;
  const uint64_t utcStart = start - int64_t(minutes) * 60;
  // Normalize subsecond RTC rounding to measured seconds, never add idle time.
  const uint64_t utcEnd = utcStart + seconds;
  if (pending.seconds && (pending.utcOffsetMinutes != minutes || utcStart < pending.endedAt ||
                          utcStart > pending.endedAt + 2 || utcEnd - pending.startedAt > 86400)) {
    if (!flush()) return false;
    pending = {};  // An isolated fragment below the provider's ten-second minimum.
  }
  if (!pending.seconds) {
    const std::string document = BookSyncFile::documentId(bookPath);
    if (!hexId(document.c_str())) {
      LOG_ERR("SessionStats", "Cannot identify session book");
      return false;
    }
    memcpy(pending.document, document.c_str(), 33);
    for (size_t i = 0; i < 4; ++i) snprintf(pending.id + i * 8, 9, "%08lx", static_cast<unsigned long>(randomWord()));
    pending.startedAt = utcStart;
    pending.utcOffsetMinutes = minutes;
  }
  pending.endedAt = utcEnd;
  pending.seconds += seconds;
  return !allowCheckpoint || checkpointIfDue();
}

bool uploadPending(const Upload upload, void* context) {
  const std::string directory = StatsStorage::localPath(DIRECTORY, true);
  if (directory.empty()) return false;
  if (!Storage.exists(directory.c_str())) return true;
  FsFile dir = Storage.open(directory.c_str());
  if (!dir || !dir.isDirectory()) {
    dir.close();
    LOG_ERR("SessionStats", "Cannot open reading session outbox");
    return false;
  }
  bool ok = true;
  // Drain the queue one record at a time; a backlog is not an upload failure.
  for (FsFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    char name[48]{};
    const bool regular = !file.isDirectory();
    const size_t length = file.getName(name, sizeof(name));
    file.close();
    // Include recoverable .tmp/.bak after an interrupted first write. Any
    // repeated entry keeps its original ID, so even a lost deletion is safe.
    if (!regular || length < 36 || strncmp(name + 32, ".bin", 4) != 0) continue;
    name[32] = 0;
    if (!hexId(name)) continue;
    char path[96];
    pathFor(path, sizeof(path), directory, name);
    if (!Storage.exists(path) && !Storage.exists((std::string(path) + ".bak").c_str()) &&
        !Storage.exists((std::string(path) + ".tmp").c_str()))
      continue;
    Record record;
    bool loaded = false;
    {
      SyncFiles::Reader reader(path);
      uint8_t version = 0;
      loaded = reader.open() != SyncFiles::Reader::Source::None && reader.pod(version) && version == VERSION &&
               reader.read(record.id, 32) && reader.read(record.document, 32) && reader.pod(record.startedAt) &&
               reader.pod(record.endedAt) && reader.pod(record.seconds) && reader.pod(record.utcOffsetMinutes) &&
               reader.atEnd() && valid(record) && strcmp(record.id, name) == 0;
      reader.close();
    }
    if (!loaded) {
      LOG_ERR("SessionStats", "Retaining unreadable session %s", name);
      ok = false;
      continue;
    }
    ReadingStatsDateTime now;
    if (getCurrentLocalDailyReadingDateTime(now) && SETTINGS.clockUtcOffsetQ <= 104) {
      const uint64_t utcNow = localSeconds(now) - int64_t(int(SETTINGS.clockUtcOffsetQ) - 48) * 900;
      if (record.endedAt > utcNow + 300) {
        LOG_ERR("SessionStats", "Retaining future-dated session after clock correction");
        ok = false;
        continue;
      }
    }
    if (!upload(context, record)) {
      ok = false;
      break;
    }
    SyncFiles::removeAll(path);
    if (Storage.exists(path) || Storage.exists((std::string(path) + ".bak").c_str()) ||
        Storage.exists((std::string(path) + ".tmp").c_str())) {
      LOG_ERR("SessionStats", "Cannot remove acknowledged session");
      ok = false;
      break;
    }
  }
  ok = ok && !FsHelpers::directoryIterationFailed(dir);
  dir.close();
  return ok;
}
}  // namespace ReadingSessionJournal
