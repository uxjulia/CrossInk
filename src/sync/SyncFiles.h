#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Crash-safe sync files under /.crosspoint, shared by the pending-push list,
// the sync state, and each book's cache sync file. Whole-file rewrites go
// through "<name>.tmp", keep the previous file as "<name>.bak", and end with a
// CRC32 of everything before it, so a torn or corrupted file is detected and
// the backup is used instead (docs/file-formats.md, "Sync files").
namespace SyncFiles {
// Running CRC32 over a file's bytes, matching uzlib_crc32 chaining.
uint32_t crc(const void* data, size_t length, uint32_t running);

// Writer for "<path>.tmp". Every byte written is folded into the trailing CRC.
class Writer {
 public:
  explicit Writer(const char* path);
  ~Writer();
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  bool open();
  bool write(const void* data, size_t length);
  template <typename T>
  bool pod(const T& value) {
    return write(&value, sizeof(value));
  }
  bool string16(const std::string& value);  // u16 length + bytes
  // Appends the CRC (unless the format checks each record itself), closes,
  // and swaps the file into place (old file -> .bak).
  bool commit(bool trailingChecksum = true);

 private:
  std::string path;
  std::string tmpPath;
  FsFile file;
  uint32_t running = 0;
  bool ok = true;
};

// Reader over a validated file. open() checks the trailing CRC first and
// falls back to "<path>.bak"; read calls then never cross into the CRC.
class Reader {
 public:
  enum class Source : uint8_t { None, Main, Backup };
  explicit Reader(const char* path);
  ~Reader();
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;
  // Missing and corrupt are both None; `missing` tells them apart.
  Source open(bool* missing = nullptr);
  // For a file this code validated moments ago (a snapshot): skips the CRC pass.
  Source openUnchecked();
  bool read(void* data, size_t length);
  template <typename T>
  bool pod(T& value) {
    return read(&value, sizeof(value));
  }
  bool string16(std::string& value, size_t maxLength);
  bool atEnd() const { return position >= payloadEnd; }
  uint32_t offset() const { return position; }
  bool seek(uint32_t offset);
  // Release the handle before the same path is rewritten (one open handle per file on SdFat).
  void close();

 private:
  bool openOne(const std::string& candidate, bool validate = true);
  std::string path;
  FsFile file;
  uint32_t position = 0;
  uint32_t payloadEnd = 0;
};

// Removes "<path>", "<path>.bak" and "<path>.tmp".
void removeAll(const char* path);
}  // namespace SyncFiles
