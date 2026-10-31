#include "SyncFiles.h"

#include <Logging.h>
#include <uzlib.h>

#include <cstring>

namespace SyncFiles {
uint32_t crc(const void* data, const size_t length, const uint32_t running) {
  return uzlib_crc32(data, static_cast<unsigned int>(length), running);
}

Writer::Writer(const char* path) : path(path), tmpPath(std::string(path) + ".tmp") {}

Writer::~Writer() {
  // An abandoned write leaves the current file untouched.
  if (file) {
    file.close();
    Storage.remove(tmpPath.c_str());
  }
}

bool Writer::open() {
  if (Storage.exists(tmpPath.c_str())) Storage.remove(tmpPath.c_str());
  ok = Storage.openFileForWrite("SYNC", tmpPath, file);
  running = 0;
  if (!ok) LOG_ERR("SYNC", "Cannot create %s", tmpPath.c_str());
  return ok;
}

bool Writer::write(const void* data, const size_t length) {
  if (!ok || !file) return false;
  if (length && file.write(data, length) != length) {
    LOG_ERR("SYNC", "Short write: %s", tmpPath.c_str());
    ok = false;
    return false;
  }
  running = crc(data, length, running);
  return true;
}

bool Writer::string16(const std::string& value) {
  if (value.size() > UINT16_MAX) {
    ok = false;
    return false;
  }
  const auto length = static_cast<uint16_t>(value.size());
  return pod(length) && write(value.data(), value.size());
}

bool Writer::commit(const bool trailingChecksum) {
  if (!ok || !file) {
    LOG_ERR("SYNC", "Discarding failed write: %s", tmpPath.c_str());
    return false;
  }
  const uint32_t checksum = running;
  if ((trailingChecksum && file.write(&checksum, sizeof(checksum)) != sizeof(checksum)) || !file.close()) {
    LOG_ERR("SYNC", "Cannot finish %s", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  // Same rotation as GlobalReadingStats: the previous file becomes the backup
  // before the new one takes its name. With no current file (first write, or
  // recovering from the backup) the backup is kept, and a crash before the
  // final rename leaves a complete .tmp that Reader::open() also accepts.
  const std::string backupPath = path + ".bak";
  if (trailingChecksum && Storage.exists(path.c_str()) && Storage.exists(backupPath.c_str())) {
    Reader reader(path.c_str());
    const auto source = reader.open();
    reader.close();
    // A corrupt primary must never displace the backup we recovered from.
    if (source == Reader::Source::Backup && !Storage.remove(path.c_str())) {
      LOG_ERR("SYNC", "Cannot remove corrupt primary: %s", path.c_str());
      return false;
    }
  }
  if (!Storage.exists(path.c_str())) {
    if (Storage.rename(tmpPath.c_str(), path.c_str())) return true;
    LOG_ERR("SYNC", "Cannot place %s", path.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
    LOG_ERR("SYNC", "Cannot remove old backup: %s", backupPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(path.c_str(), backupPath.c_str())) {
    LOG_ERR("SYNC", "Cannot rotate backup: %s", path.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), path.c_str())) {
    LOG_ERR("SYNC", "Cannot replace %s", path.c_str());
    if (Storage.exists(backupPath.c_str()) && !Storage.exists(path.c_str()))
      Storage.rename(backupPath.c_str(), path.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  return true;
}

Reader::Reader(const char* path) : path(path) {}

Reader::~Reader() {
  if (file) file.close();
}

bool Reader::openOne(const std::string& candidate, const bool validate) {
  if (file) file.close();
  if (!Storage.exists(candidate.c_str()) || !Storage.openFileForRead("SYNC", candidate, file)) return false;
  const size_t size = file.fileSize();
  if (size < sizeof(uint32_t)) {
    file.close();
    return false;
  }
  if (!validate) {
    payloadEnd = static_cast<uint32_t>(size - sizeof(uint32_t));
    position = 0;
    return true;
  }
  // Validate the whole file before trusting any of it; sync files are small.
  uint8_t buffer[128];
  uint32_t running = 0;
  size_t remaining = size - sizeof(uint32_t);
  while (remaining > 0) {
    const size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    if (file.read(buffer, chunk) != static_cast<int>(chunk)) {
      file.close();
      return false;
    }
    running = crc(buffer, chunk, running);
    remaining -= chunk;
  }
  uint32_t stored = 0;
  if (file.read(&stored, sizeof(stored)) != static_cast<int>(sizeof(stored)) || stored != running || !file.seekSet(0)) {
    file.close();
    return false;
  }
  payloadEnd = static_cast<uint32_t>(size - sizeof(uint32_t));
  position = 0;
  return true;
}

Reader::Source Reader::open(bool* missing) {
  const std::string backupPath = path + ".bak";
  const std::string tmpPath = path + ".tmp";
  const bool mainExists = Storage.exists(path.c_str());
  if (missing) *missing = !mainExists && !Storage.exists(backupPath.c_str()) && !Storage.exists(tmpPath.c_str());
  if (openOne(path)) return Source::Main;
  if (mainExists) LOG_ERR("SYNC", "Corrupt sync file, trying backup: %s", path.c_str());
  // A crash between rotating the old file to .bak and renaming the new one into
  // place leaves the newest complete copy as .tmp; its CRC proves it finished.
  if (!mainExists && openOne(tmpPath)) {
    // Promote it before anything else touches the path: the next write starts
    // by clearing .tmp, and SdFat allows one handle per file.
    close();
    if (Storage.rename(tmpPath.c_str(), path.c_str()) && openOne(path)) return Source::Main;
    LOG_ERR("SYNC", "Cannot restore %s from its temp file", path.c_str());
  }
  if (openOne(backupPath)) return Source::Backup;
  return Source::None;
}

Reader::Source Reader::openUnchecked() {
  if (!openOne(path, false)) return Source::None;
  return Source::Main;
}

bool Reader::read(void* data, const size_t length) {
  if (!file || length > payloadEnd - position) return false;
  if (length && file.read(data, length) != static_cast<int>(length)) return false;
  position += static_cast<uint32_t>(length);
  return true;
}

bool Reader::string16(std::string& value, const size_t maxLength) {
  uint16_t length = 0;
  if (!pod(length) || length > maxLength) return false;
  value.resize(length);
  return read(&value[0], length);
}

bool Reader::seek(const uint32_t offset) {
  if (!file || offset > payloadEnd || !file.seekSet(offset)) return false;
  position = offset;
  return true;
}

void Reader::close() {
  if (file) file.close();
  position = payloadEnd = 0;
}

void removeAll(const char* path) {
  const std::string base(path);
  for (const char* suffix : {"", ".bak", ".tmp"}) {
    const std::string candidate = base + suffix;
    if (Storage.exists(candidate.c_str())) Storage.remove(candidate.c_str());
  }
}
}  // namespace SyncFiles
