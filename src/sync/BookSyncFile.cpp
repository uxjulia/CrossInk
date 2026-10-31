#include "BookSyncFile.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <KOReaderDocumentId.h>
#include <Logging.h>
#include <Xtc.h>

#include <cstdio>
#include <cstring>

#include "SyncFiles.h"
#include "SyncIds.h"
#include "SyncPending.h"
#include "activities/reader/BookReadingStats.h"

namespace {
// Version 2 adds walkStamp and idsGeneration after the version 1 fields.
constexpr uint8_t VERSION = 2;
constexpr uint8_t VERSION_1 = 1;
constexpr const char* FILE_NAME = "/sync.bin";

bool parseHex(const std::string& hex, uint8_t (&out)[16]) {
  if (hex.size() != 32) return false;
  for (size_t i = 0; i < 16; ++i) {
    unsigned value = 0;
    if (std::sscanf(hex.c_str() + i * 2, "%2x", &value) != 1) return false;
    out[i] = static_cast<uint8_t>(value);
  }
  return true;
}

}  // namespace

std::string BookSyncFile::cachePathFor(const std::string& bookPath) {
  if (FsHelpers::hasEpubExtension(bookPath)) return Epub::resolveCachePathForFilePath(bookPath, "/.crosspoint");
  if (FsHelpers::hasXtcExtension(bookPath)) return Xtc(bookPath, "/.crosspoint").getCachePath();
  return {};
}

bool BookSyncFile::read(const std::string& cachePath, BookSyncFile& out) {
  out = {};
  if (cachePath.empty()) return false;
  const std::string path = cachePath + FILE_NAME;
  SyncFiles::Reader reader(path.c_str());
  if (reader.open() == SyncFiles::Reader::Source::None) return false;
  uint8_t version = 0;
  uint8_t hasId = 0;
  uint8_t method = 0;
  if (!reader.pod(version) || (version != VERSION && version != VERSION_1) || !reader.pod(hasId) ||
      !reader.pod(method) || !reader.read(out.id, sizeof(out.id)) || !reader.pod(out.fileSize) ||
      !reader.pod(out.statsStamp) || !reader.pod(out.clippingsStamp) ||
      (version == VERSION && !(reader.pod(out.walkStamp) && reader.pod(out.idsGeneration)))) {
    out = {};
    return false;
  }
  out.hasId = hasId != 0;
  out.method = static_cast<DocumentMatchMethod>(method);
  return true;
}

bool BookSyncFile::write(const std::string& cachePath, const BookSyncFile& record) {
  if (cachePath.empty() || !Storage.exists(cachePath.c_str())) return false;
  const std::string path = cachePath + FILE_NAME;
  SyncFiles::Writer writer(path.c_str());
  const uint8_t hasId = record.hasId ? 1 : 0;
  const auto method = static_cast<uint8_t>(record.method);
  const bool ok = writer.open() && writer.pod(VERSION) && writer.pod(hasId) && writer.pod(method) &&
                  writer.write(record.id, sizeof(record.id)) && writer.pod(record.fileSize) &&
                  writer.pod(record.statsStamp) && writer.pod(record.clippingsStamp) && writer.pod(record.walkStamp) &&
                  writer.pod(record.idsGeneration) && writer.commit();
  if (!ok) LOG_ERR("SYNC", "Cannot save book sync record: %s", path.c_str());
  return ok;
}

std::string BookSyncFile::documentId(const std::string& bookPath) {
  const auto method = KOREADER_STORE.getMatchMethod();
  const std::string cachePath = cachePathFor(bookPath);
  BookSyncFile record;
  const bool known = read(cachePath, record);
  bool changed = false;
  bool idChanged = false;
  std::string id;
  if (method == DocumentMatchMethod::FILENAME) {
    // Filename matching hashes the name only: nothing to read or cache.
    id = KOReaderDocumentId::calculateFromFilename(bookPath);
  } else {
    // An SD card can be edited outside CrossInk, even preserving size and timestamps.
    uint32_t size = 0;
    id = KOReaderDocumentId::calculate(bookPath, &size);
    uint8_t fresh[16];
    if (!parseHex(id, fresh)) return {};
    idChanged = !known || !record.hasId || record.method != method || memcmp(record.id, fresh, sizeof(fresh)) != 0;
    if (idChanged || record.fileSize != size) {
      memcpy(record.id, fresh, sizeof(fresh));
      record.hasId = true;
      record.method = method;
      record.fileSize = size;
      changed = true;
    }
  }
  if (id.empty()) return id;

  // Only the pull reads sync_ids.bin, and XTC has no position to pull. A
  // finished book is left out so the map stays close to books being read.
  uint8_t raw[16];
  if (FsHelpers::hasEpubExtension(bookPath) && parseHex(id, raw) && !BookReadingStats::peekCompleted(cachePath)) {
    const uint32_t current = SyncIds::generation();
    if (!current || record.idsGeneration != current || idChanged) {
      record.idsGeneration = SyncIds::record(bookPath, raw);
      changed = true;
    }
  }
  if (changed) write(cachePath, record);
  return id;
}

void BookSyncFile::stamp(const std::string& bookPath, const bool stats, const uint32_t value) {
  const std::string cachePath = cachePathFor(bookPath);
  BookSyncFile record;
  read(cachePath, record);
  if (stats)
    record.statsStamp = value;
  else
    record.clippingsStamp = value;
  write(cachePath, record);
}

namespace {
void forgetIdsRecord(const std::string& bookPath) {
  const std::string cachePath = BookSyncFile::cachePathFor(bookPath);
  BookSyncFile record;
  if (!BookSyncFile::read(cachePath, record) || !record.idsGeneration) return;
  record.idsGeneration = 0;
  BookSyncFile::write(cachePath, record);
}
}  // namespace

void BookSyncFile::onFinishedChanged(const std::string& bookPath, const bool finished) {
  if (finished) SyncIds::removePath(bookPath);
  forgetIdsRecord(bookPath);
}

void BookSyncFile::onRenamed(const std::string& oldPath, const std::string& newPath) {
  if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME &&
      oldPath.substr(oldPath.find_last_of('/') + 1) != newPath.substr(newPath.find_last_of('/') + 1)) {
    SyncIds::removePath(oldPath);
    forgetIdsRecord(newPath);
    return;
  }
  SyncIds::renamePath(oldPath, newPath);
}

void BookSyncFile::onDeleted(const std::string& bookPath) { SyncIds::removePath(bookPath); }

bool BookSyncFile::onFileChanged(const std::string& bookPath) {
  const std::string cachePath = cachePathFor(bookPath);
  if (cachePath.empty() || !Storage.exists(cachePath.c_str())) return true;

  BookSyncFile record;
  if (read(cachePath, record)) {
    record.hasId = false;
    memset(record.id, 0, sizeof(record.id));
    record.fileSize = 0;
    record.idsGeneration = 0;
    // Refresh both the main record and its backup so recovery cannot bring
    // back the old file's id. Keep the catch-up and first-walk stamps intact.
    if (!write(cachePath, record) || !write(cachePath, record)) return false;
  } else {
    const std::string path = cachePath + FILE_NAME;
    if (Storage.exists(path.c_str()) || Storage.exists((path + ".bak").c_str()) ||
        Storage.exists((path + ".tmp").c_str())) {
      LOG_ERR("SYNC", "Cannot preserve unreadable book sync history: %s", bookPath.c_str());
      return false;
    }
  }

  // A deleted/reuploaded book may have lost its pending entry. Requeue its
  // saved data under the fresh id, respecting the configured sync toggles.
  const uint8_t pendingBits = FsHelpers::hasEpubExtension(bookPath)
                                  ? SyncPending::PROGRESS_BIT | SyncPending::STATS_BIT | SyncPending::CLIPPINGS_BIT
                                  : SyncPending::STATS_BIT;
  if (!SyncIds::removePath(bookPath) || !SyncPending::mark(bookPath, pendingBits)) {
    LOG_ERR("SYNC", "Cannot refresh book sync tracking: %s", bookPath.c_str());
    return false;
  }
  return true;
}

void BookSyncFile::stampWalk(const std::string& bookPath, const uint32_t walkId) {
  const std::string cachePath = cachePathFor(bookPath);
  BookSyncFile record;
  read(cachePath, record);
  record.walkStamp = walkId;
  write(cachePath, record);
}

bool BookSyncFile::walkStamped(const std::string& bookPath, const uint32_t walkId) {
  if (!walkId) return false;
  BookSyncFile record;
  return read(cachePathFor(bookPath), record) && record.walkStamp == walkId;
}

bool BookSyncFile::stamped(const std::string& bookPath, const bool stats, const uint32_t value) {
  if (!value) return false;
  BookSyncFile record;
  if (!read(cachePathFor(bookPath), record)) return false;
  return (stats ? record.statsStamp : record.clippingsStamp) == value;
}
