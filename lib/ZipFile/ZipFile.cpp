#include "ZipFile.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

struct ZipInflateCtx {
  HalFile* file = nullptr;
  size_t fileRemaining = 0;
  uint8_t* readBuf = nullptr;
  size_t readBufSize = 0;
};

namespace {
constexpr uint16_t ZIP_METHOD_STORED = 0;
constexpr uint16_t ZIP_METHOD_DEFLATED = 8;
constexpr size_t ONE_SHOT_DEFLATE_MAX_COMPRESSED_BYTES = 32768;

// RAII zip: opens the zip if not already open, closes on destruction only if
// it performed the open.  Removes the wasOpen/close boilerplate from every method.
class ScopedOpenClose final {
 public:
  [[nodiscard]] explicit ScopedOpenClose(ZipFile& zf) : zf(zf), needsClose(!zf.isOpen()) {
    if (needsClose) ok = zf.open();
  }
  ~ScopedOpenClose() {
    if (needsClose && ok) zf.close();
  }
  ScopedOpenClose(const ScopedOpenClose&) = delete;
  ScopedOpenClose& operator=(const ScopedOpenClose&) = delete;
  ScopedOpenClose(ScopedOpenClose&&) = delete;
  ScopedOpenClose& operator=(ScopedOpenClose&&) = delete;
  explicit operator bool() const { return ok || !needsClose; }

 private:
  ZipFile& zf;
  bool needsClose = false;
  bool ok = true;  // true when zip was already open (no open() call needed)
};

constexpr uint32_t ZIP_CENTRAL_DIR_SIGNATURE = 0x02014b50;
constexpr size_t ZIP_CENTRAL_DIR_HEADER_SIZE = 46;
constexpr size_t ZIP_CENTRAL_DIR_CHUNK_SIZE = 1024;
constexpr uint16_t ZIP_MAX_STORED_NAME_LEN = 255;  // callers pass a 256-byte name buffer

struct CentralDirEntry {
  uint32_t entryStart = 0;  // file offset of this entry's signature
  uint16_t method = 0;
  uint32_t crc32 = 0;
  uint32_t compressedSize = 0;
  uint32_t uncompressedSize = 0;
  uint16_t nameLen = 0;
  uint32_t localHeaderOffset = 0;
  bool nameStored = false;  // false when the name was too long and skipped
};

// Sequential reader for the ZIP central directory. The previous loops made
// ~13 locked SD calls per entry (nine 2-4 byte reads plus seeks); every chapter,
// image and stylesheet lookup scans this directory from the start, so large
// EPUBs paid that thousands of times per lookup. This reads 1 KiB chunks and
// parses entries from RAM; if the buffer cannot be allocated it falls back to
// one header read, one name read and one seek per entry.
class CentralDirCursor final {
 public:
  CentralDirCursor(HalFile& file, const uint32_t offset)
      : file(file), buf(makeUniqueNoThrow<uint8_t[]>(ZIP_CENTRAL_DIR_CHUNK_SIZE)) {
    if (!buf) LOG_ERR("ZIP", "Central directory buffer alloc failed; using unbuffered scan");
    restart(offset);
  }

  void restart(const uint32_t offset) {
    ok = file.seek(offset);
    filePos = offset;
    fill = 0;
    off = 0;
  }

  bool valid() const { return ok; }

  // File offset of the next unread entry.
  uint32_t position() const { return filePos - static_cast<uint32_t>(fill - off); }

  // Reads the next entry. `name` must hold ZIP_MAX_STORED_NAME_LEN + 1 bytes.
  // Returns false at the end of the directory or on a read/seek failure.
  bool next(CentralDirEntry& entry, char* name) {
    if (!ok) return false;
    entry.entryStart = position();
    uint8_t direct[ZIP_CENTRAL_DIR_HEADER_SIZE];
    const uint8_t* header = direct;
    if (buf) {
      if (!ensure(ZIP_CENTRAL_DIR_HEADER_SIZE)) return false;
      header = buf.get() + off;
    } else if (!readDirect(direct, ZIP_CENTRAL_DIR_HEADER_SIZE)) {
      return false;
    }

    uint32_t signature = 0;
    uint16_t extraLen = 0;
    uint16_t commentLen = 0;
    memcpy(&signature, header, 4);
    if (signature != ZIP_CENTRAL_DIR_SIGNATURE) return false;
    memcpy(&entry.method, header + 10, 2);
    memcpy(&entry.crc32, header + 16, 4);
    memcpy(&entry.compressedSize, header + 20, 4);
    memcpy(&entry.uncompressedSize, header + 24, 4);
    memcpy(&entry.nameLen, header + 28, 2);
    memcpy(&extraLen, header + 30, 2);
    memcpy(&commentLen, header + 32, 2);
    memcpy(&entry.localHeaderOffset, header + 42, 4);
    if (buf) off += ZIP_CENTRAL_DIR_HEADER_SIZE;

    entry.nameStored = entry.nameLen <= ZIP_MAX_STORED_NAME_LEN;
    if (entry.nameStored) {
      if (buf) {
        if (!ensure(entry.nameLen)) return false;
        memcpy(name, buf.get() + off, entry.nameLen);
        off += entry.nameLen;
      } else if (!readDirect(reinterpret_cast<uint8_t*>(name), entry.nameLen)) {
        return false;
      }
      name[entry.nameLen] = '\0';
    } else if (!skip(entry.nameLen)) {
      return false;
    }
    // Like the old scan, report this entry even if its trailing fields cannot
    // be skipped; the failed seek ends the scan at the next call.
    skip(static_cast<size_t>(extraLen) + commentLen);
    return true;
  }

 private:
  // Makes `count` (<= chunk size) bytes available at buf[off].
  bool ensure(const size_t count) {
    if (fill - off >= count) return true;
    memmove(buf.get(), buf.get() + off, fill - off);
    fill -= off;
    off = 0;
    while (fill < count) {
      const int got = file.read(buf.get() + fill, ZIP_CENTRAL_DIR_CHUNK_SIZE - fill);
      if (got <= 0) return false;
      fill += static_cast<size_t>(got);
      filePos += static_cast<uint32_t>(got);
    }
    return true;
  }

  bool readDirect(uint8_t* out, const size_t count) {
    if (count == 0) return true;
    const int got = file.read(out, count);
    if (got < 0 || static_cast<size_t>(got) != count) return false;
    filePos += static_cast<uint32_t>(count);
    return true;
  }

  bool skip(const size_t count) {
    const size_t buffered = fill - off;
    if (count <= buffered) {
      off += count;
      return true;
    }
    const size_t rest = count - buffered;
    fill = 0;
    off = 0;
    if (!file.seekCur(static_cast<int64_t>(rest))) {
      ok = false;
      return false;
    }
    filePos += static_cast<uint32_t>(rest);
    return true;
  }

  HalFile& file;
  std::unique_ptr<uint8_t[]> buf;
  uint32_t filePos = 0;  // file offset just past the bytes read so far
  size_t fill = 0;
  size_t off = 0;
  bool ok = true;
};

size_t zipFillCallback(void* vctx, const uint8_t** data) {
  auto* ctx = static_cast<ZipInflateCtx*>(vctx);
  if (ctx->fileRemaining == 0) return 0;

  const size_t toRead = ctx->fileRemaining < ctx->readBufSize ? ctx->fileRemaining : ctx->readBufSize;
  const int result = ctx->file->read(ctx->readBuf, toRead);
  if (result < 0) {
    LOG_ERR("ZIP", "Failed to read compressed data: %d", result);
    return 0;
  }
  const size_t bytesRead = static_cast<size_t>(result);
  ctx->fileRemaining -= bytesRead;

  *data = ctx->readBuf;
  return bytesRead;
}

size_t zipStreamFillCallback(void* vctx, const uint8_t** data) {
  auto* ctx = static_cast<ZipStreamInflateCtx*>(vctx);
  if (!ctx->file || ctx->fileRemaining == 0) return 0;

  const size_t toRead = ctx->fileRemaining < ctx->readBufSize ? ctx->fileRemaining : ctx->readBufSize;
  const int result = ctx->file->read(ctx->readBuf, toRead);
  if (result < 0) {
    LOG_ERR("ZIP", "Failed to read cooperative compressed data: %d", result);
    return 0;
  }
  const size_t bytesRead = static_cast<size_t>(result);
  ctx->fileRemaining -= bytesRead;

  *data = ctx->readBuf;
  return bytesRead;
}
}  // namespace

ZipFileStreamReader::~ZipFileStreamReader() { abort(); }

bool ZipFileStreamReader::begin(const std::string& zipPathIn, const char* filename, const size_t chunkSizeIn,
                                ZipDirectoryCache* directoryCache) {
  abort();
  if (!filename || filename[0] == '\0' || chunkSizeIn == 0) {
    return false;
  }

  ZipFile zip(zipPathIn, directoryCache);
  const ScopedOpenClose zipOpen{zip};
  if (!zipOpen) return false;

  ZipFile::FileStatSlim fileStat = {};
  if (!zip.loadFileStatSlim(filename, &fileStat)) return false;

  const long offset = zip.getDataOffset(fileStat);
  if (offset < 0) return false;

  zipPath = zipPathIn;
  method = fileStat.method;
  dataOffset = static_cast<uint32_t>(offset);
  compressedSize = fileStat.compressedSize;
  uncompressedSize = fileStat.uncompressedSize;
  chunkSize = chunkSizeIn;
  totalProduced = 0;
  compressedConsumed = 0;

  readBuffer = static_cast<uint8_t*>(malloc(chunkSize));
  if (!readBuffer) {
    LOG_ERR("ZIP", "Failed to allocate cooperative read buffer (free=%u, maxAlloc=%u, chunk=%zu)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap(), chunkSize);
    abort();
    return false;
  }
  outputBuffer = static_cast<uint8_t*>(malloc(chunkSize));
  if (!outputBuffer) {
    LOG_ERR("ZIP", "Failed to allocate cooperative output buffer (free=%u, maxAlloc=%u, chunk=%zu)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap(), chunkSize);
    abort();
    return false;
  }

  if (method == ZIP_METHOD_DEFLATED) {
    inflateCtx.fileRemaining = compressedSize;
    inflateCtx.readBuf = readBuffer;
    inflateCtx.readBufSize = chunkSize;
    if (!inflateCtx.reader.init(true)) {
      LOG_ERR("ZIP", "Failed to init cooperative inflate reader (free=%u, maxAlloc=%u, chunk=%zu)", ESP.getFreeHeap(),
              ESP.getMaxAllocHeap(), chunkSize);
      abort();
      return false;
    }
    inflateCtx.reader.setFill(zipStreamFillCallback, &inflateCtx);
  } else if (method != ZIP_METHOD_STORED) {
    LOG_ERR("ZIP", "Unsupported compression method");
    abort();
    return false;
  }

  active = true;
  return true;
}

ZipStreamStatus ZipFileStreamReader::pump(Print& out, const size_t maxOutputBytes) {
  if (!active) return ZipStreamStatus::Error;

  HalFile zipFile;
  if (!Storage.openFileForRead("ZIP", zipPath, zipFile)) {
    return ZipStreamStatus::Error;
  }

  if (method == ZIP_METHOD_STORED) {
    if (!zipFile.seek(dataOffset + totalProduced)) {
      zipFile.close();
      return ZipStreamStatus::Error;
    }
    const size_t remaining = static_cast<size_t>(uncompressedSize) - totalProduced;
    const size_t budget = maxOutputBytes > 0 ? std::min(remaining, maxOutputBytes) : remaining;
    const size_t toRead = std::min(chunkSize, budget);
    if (toRead == 0) {
      zipFile.close();
      active = false;
      return ZipStreamStatus::Done;
    }
    const int readResult = zipFile.read(outputBuffer, toRead);
    zipFile.close();
    if (readResult <= 0) {
      LOG_ERR("ZIP", "Failed to read stored stream data: %d", readResult);
      return ZipStreamStatus::Error;
    }
    const size_t dataRead = static_cast<size_t>(readResult);
    if (out.write(outputBuffer, dataRead) != dataRead) return ZipStreamStatus::Error;
    totalProduced += dataRead;
    if (totalProduced == static_cast<size_t>(uncompressedSize)) {
      active = false;
      return ZipStreamStatus::Done;
    }
    return ZipStreamStatus::More;
  }

  if (!zipFile.seek(dataOffset + compressedConsumed)) {
    zipFile.close();
    return ZipStreamStatus::Error;
  }

  inflateCtx.file = &zipFile;
  size_t pumped = 0;
  bool success = false;
  ZipStreamStatus result = ZipStreamStatus::More;

  while (true) {
    size_t produced = 0;
    const size_t beforeRemaining = inflateCtx.fileRemaining;
    const size_t outputLimit =
        maxOutputBytes > 0 ? std::min(chunkSize, maxOutputBytes - pumped) : static_cast<size_t>(chunkSize);
    if (outputLimit == 0) {
      success = true;
      break;
    }

    const InflateStream::Status status = inflateCtx.reader.readAtMost(outputBuffer, outputLimit, &produced);
    compressedConsumed += beforeRemaining - inflateCtx.fileRemaining;
    inflateCtx.file = nullptr;

    totalProduced += produced;
    pumped += produced;
    if (totalProduced > static_cast<size_t>(uncompressedSize)) {
      LOG_ERR("ZIP", "Decompressed size exceeds expected (%zu > %zu)", totalProduced,
              static_cast<size_t>(uncompressedSize));
      result = ZipStreamStatus::Error;
      break;
    }

    if (produced > 0 && out.write(outputBuffer, produced) != produced) {
      result = ZipStreamStatus::Error;
      break;
    }

    if (status == InflateStream::Status::Done) {
      if (totalProduced != static_cast<size_t>(uncompressedSize)) {
        LOG_ERR("ZIP", "Decompressed size mismatch (expected %zu, got %zu)", static_cast<size_t>(uncompressedSize),
                totalProduced);
        result = ZipStreamStatus::Error;
      } else {
        active = false;
        result = ZipStreamStatus::Done;
      }
      success = result != ZipStreamStatus::Error;
      break;
    }

    if (status == InflateStream::Status::Error) {
      LOG_ERR("ZIP", "Decompression failed");
      result = ZipStreamStatus::Error;
      break;
    }

    if (maxOutputBytes > 0 && pumped >= maxOutputBytes) {
      success = true;
      result = ZipStreamStatus::More;
      break;
    }

    inflateCtx.file = &zipFile;
  }

  inflateCtx.file = nullptr;
  zipFile.close();
  if (!success && result == ZipStreamStatus::Error) {
    return ZipStreamStatus::Error;
  }
  return result;
}

void ZipFileStreamReader::abort() {
  active = false;
  inflateCtx.reader.deinit();
  inflateCtx.file = nullptr;
  inflateCtx.fileRemaining = 0;
  inflateCtx.readBuf = nullptr;
  inflateCtx.readBufSize = 0;
  if (readBuffer) {
    free(readBuffer);
    readBuffer = nullptr;
  }
  if (outputBuffer) {
    free(outputBuffer);
    outputBuffer = nullptr;
  }
  zipPath.clear();
  method = 0;
  dataOffset = 0;
  compressedSize = 0;
  uncompressedSize = 0;
  chunkSize = 0;
  totalProduced = 0;
  compressedConsumed = 0;
}

bool ZipFile::enumerateFilePathsImpl(void* context, void (*callback)(void*, std::string_view)) {
  if (!fileStatSlimCache.empty()) {
    for (const auto& entry : fileStatSlimCache) callback(context, entry.first);
    return true;
  }
  ScopedOpenClose guard(*this);
  if (!guard || !loadZipDetails()) return false;
  CentralDirCursor cursor(file, zipDetails.centralDirOffset);
  CentralDirEntry entry;
  char name[ZIP_MAX_STORED_NAME_LEN + 1];
  for (uint16_t i = 0; i < zipDetails.totalEntries; ++i) {
    if (!cursor.next(entry, name)) return false;
    if (entry.nameStored) callback(context, std::string_view{name, entry.nameLen});
  }
  return true;
}

bool ZipFile::loadAllFileStatSlims() {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (!loadZipDetails()) return false;

  char itemName[ZIP_MAX_STORED_NAME_LEN + 1];
  fileStatSlimCache.clear();
  fileStatSlimCache.reserve(zipDetails.totalEntries);

  CentralDirCursor cursor(file, zipDetails.centralDirOffset);
  CentralDirEntry entry;
  while (cursor.next(entry, itemName)) {
    if (!entry.nameStored) continue;  // oversized names cannot be looked up
    FileStatSlim fileStat = {};
    fileStat.method = entry.method;
    fileStat.compressedSize = entry.compressedSize;
    fileStat.uncompressedSize = entry.uncompressedSize;
    fileStat.localHeaderOffset = entry.localHeaderOffset;
    fileStatSlimCache.emplace(itemName, fileStat);
  }

  // Set cursor to start of central directory for sequential access
  lastCentralDirPos = zipDetails.centralDirOffset;
  lastCentralDirPosValid = true;

  return true;
}

namespace {
uint16_t directoryU16(const uint8_t* p) { return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8); }
uint32_t directoryU32(const uint8_t* p) {
  return static_cast<uint32_t>(directoryU16(p)) | (static_cast<uint32_t>(directoryU16(p + 2)) << 16);
}
}  // namespace

bool ZipFile::prepareDirectoryCache() {
  if (!directoryCache || !psramHeapAvailable()) return false;
  auto& cache = *directoryCache;
  // Each Epub::load starts a new cache lifetime. Also reject observable changes
  // within that lifetime, including same-size replacements with a new timestamp.
  const uint32_t modified = file.modificationTime();
  if (cache.attempted && (cache.path != filePath || cache.fileSize != file.size() || cache.modified != modified)) {
    cache.clear();
    zipDetails.isSet = false;
  }
  if (cache.attempted) return !!cache.bytes;
  cache.attempted = true;
  cache.path = filePath;
  cache.fileSize = file.size();
  cache.modified = modified;
  if (!loadZipDetails()) return false;
  cache.offset = zipDetails.centralDirOffset;
  cache.length = zipDetails.centralDirSize;
  cache.count = zipDetails.totalEntries;
  constexpr size_t maxDirectoryBytes = 1024 * 1024;
  constexpr size_t reserveBytes = 1024 * 1024;
  const auto heap = byteHeapSnapshot(MemoryPool::Psram);
  if (!cache.count || cache.count == UINT16_MAX || cache.length > maxDirectoryBytes ||
      cache.length < static_cast<size_t>(cache.count) * 46 || cache.offset > cache.fileSize ||
      cache.length > cache.fileSize - cache.offset || heap.free < reserveBytes + cache.length ||
      heap.largest < cache.length)
    return false;
  auto bytes = makePsramByteBufferNoThrow(cache.length);
  if (!bytes) {
    LOG_ERR("ZIP", "Directory cache allocation failed; using SD scan");
    return false;
  }
  if (!file.seek(cache.offset) || file.read(bytes.get(), cache.length) != static_cast<int>(cache.length)) {
    LOG_ERR("ZIP", "Directory cache read failed; using SD scan");
    return false;
  }
  size_t offset = 0;
  for (uint32_t i = 0; i < cache.count; ++i) {
    if (offset > cache.length || cache.length - offset < 46 || directoryU32(bytes.get() + offset) != 0x02014b50) {
      LOG_ERR("ZIP", "Invalid directory cache entry; using SD scan");
      return false;
    }
    const uint8_t* entry = bytes.get() + offset;
    const size_t length = 46u + directoryU16(entry + 28) + directoryU16(entry + 30) + directoryU16(entry + 32);
    if (length > cache.length - offset || directoryU16(entry + 34) != 0 || directoryU32(entry + 20) == UINT32_MAX ||
        directoryU32(entry + 24) == UINT32_MAX || directoryU32(entry + 42) >= cache.offset)
      return false;
    offset += length;
  }
  // Never install a partially parsed directory, or unsupported trailing records.
  if (offset != cache.length) return false;
  LOG_DBG("ZIP", "PSRAM directory: %u entries, %u bytes", cache.count, cache.length);
  cache.bytes = std::move(bytes);
  return true;
}

bool ZipFile::loadFileStatSlim(const char* filename, FileStatSlim* fileStat) {
  if (!fileStatSlimCache.empty()) {
    const auto it = fileStatSlimCache.find(filename);
    if (it != fileStatSlimCache.end()) {
      *fileStat = it->second;
      return true;
    }
    return false;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  if (prepareDirectoryCache()) {
    const auto& cache = *directoryCache;
    const std::string_view wanted(filename);
    size_t offset = 0;
    for (uint32_t i = 0; i < cache.count; ++i) {
      const uint8_t* entry = cache.bytes.get() + offset;
      const uint16_t nameLength = directoryU16(entry + 28);
      if (wanted == std::string_view(reinterpret_cast<const char*>(entry + 46), nameLength)) {
        *fileStat = {directoryU16(entry + 10), directoryU32(entry + 20), directoryU32(entry + 24),
                     directoryU32(entry + 42)};
        return true;
      }
      offset += 46u + nameLength + directoryU16(entry + 30) + directoryU16(entry + 32);
    }
    return false;
  }
  if (!loadZipDetails()) return false;

  // Phase 1: Try scanning from cursor position first
  const uint32_t startPos = lastCentralDirPosValid ? lastCentralDirPos : zipDetails.centralDirOffset;
  bool wrapped = false;
  bool found = false;

  char itemName[ZIP_MAX_STORED_NAME_LEN + 1];
  CentralDirCursor cursor(file, startPos);
  CentralDirEntry entry;

  while (true) {
    if (!cursor.next(entry, itemName)) {
      // End of central directory
      if (!wrapped && lastCentralDirPosValid && startPos != zipDetails.centralDirOffset) {
        // Wrap around to beginning
        cursor.restart(zipDetails.centralDirOffset);
        wrapped = true;
        continue;
      }
      break;
    }

    // If we've wrapped and reached our start position, stop
    if (wrapped && entry.entryStart >= startPos) {
      break;
    }

    if (entry.nameStored && strcmp(itemName, filename) == 0) {
      fileStat->method = entry.method;
      fileStat->compressedSize = entry.compressedSize;
      fileStat->uncompressedSize = entry.uncompressedSize;
      fileStat->localHeaderOffset = entry.localHeaderOffset;
      // Found it! Update cursor to next entry
      lastCentralDirPos = cursor.position();
      lastCentralDirPosValid = true;
      found = true;
      break;
    }
  }

  return found;
}

long ZipFile::getDataOffset(const FileStatSlim& fileStat) {
  const ScopedOpenClose zip{*this};
  if (!zip) return -1;

  constexpr auto localHeaderSize = 30;

  uint8_t pLocalHeader[localHeaderSize];
  const uint64_t fileOffset = fileStat.localHeaderOffset;

  file.seek(fileOffset);
  const int readResult = file.read(pLocalHeader, localHeaderSize);

  if (readResult != localHeaderSize) {
    LOG_ERR("ZIP", "Something went wrong reading the local header: %d", readResult);
    return -1;
  }

  if (pLocalHeader[0] + (pLocalHeader[1] << 8) + (pLocalHeader[2] << 16) + (pLocalHeader[3] << 24) !=
      0x04034b50 /* ZIP local file header signature */) {
    LOG_ERR("ZIP", "Not a valid zip file header");
    return -1;
  }

  const uint16_t filenameLength = pLocalHeader[26] + (pLocalHeader[27] << 8);
  const uint16_t extraOffset = pLocalHeader[28] + (pLocalHeader[29] << 8);
  return fileOffset + localHeaderSize + filenameLength + extraOffset;
}

bool ZipFile::loadZipDetails() {
  if (zipDetails.isSet) {
    return true;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  const size_t fileSize = file.size();
  if (fileSize < 22) {
    LOG_ERR("ZIP", "File too small to be a valid zip");
    return false;  // Minimum EOCD size is 22 bytes
  }

  // We scan the last 1KB (or the whole file if smaller) for the EOCD signature
  // 0x06054b50 is stored as 0x50, 0x4b, 0x05, 0x06 in little-endian
  const int scanRange = fileSize > 1024 ? 1024 : fileSize;
  const auto buffer = static_cast<uint8_t*>(malloc(scanRange));
  if (!buffer) {
    LOG_ERR("ZIP", "Failed to allocate memory for EOCD scan buffer");
    return false;
  }

  if (!file.seek(fileSize - scanRange) || file.read(buffer, scanRange) != scanRange) {
    LOG_ERR("ZIP", "Failed to read ZIP end-of-directory record");
    free(buffer);
    return false;
  }

  // Scan backwards for the signature
  int foundOffset = -1;
  for (int i = scanRange - 22; i >= 0; i--) {
    constexpr uint32_t signature = 0x06054b50;
    uint32_t candidate = 0;
    memcpy(&candidate, buffer + i, sizeof(candidate));
    if (candidate == signature) {
      foundOffset = i;
      break;
    }
  }

  if (foundOffset == -1) {
    LOG_ERR("ZIP", "EOCD signature not found in zip file");
    free(buffer);
    return false;
  }

  // Now extract the values we need from the EOCD record
  // Relative positions within EOCD:
  // Offset 10: Total number of entries (2 bytes)
  // Offset 16: Offset of start of central directory with respect to the starting disk number (4 bytes)
  memcpy(&zipDetails.totalEntries, buffer + foundOffset + 10, sizeof(zipDetails.totalEntries));
  memcpy(&zipDetails.centralDirOffset, buffer + foundOffset + 16, sizeof(zipDetails.centralDirOffset));
  memcpy(&zipDetails.centralDirSize, buffer + foundOffset + 12, sizeof(zipDetails.centralDirSize));
  zipDetails.isSet = true;

  free(buffer);
  return true;
}

bool ZipFile::open() {
  if (!Storage.openFileForRead("ZIP", filePath, file)) {
    return false;
  }
  return true;
}

bool ZipFile::close() {
  if (file) {
    // Explicit close() required: member variable persists beyond function scope
    file.close();
  }
  lastCentralDirPos = 0;
  lastCentralDirPosValid = false;
  return true;
}

bool ZipFile::getInflatedFileSize(const char* filename, size_t* size) {
  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) {
    return false;
  }

  *size = static_cast<size_t>(fileStat.uncompressedSize);
  return true;
}

int ZipFile::fillUncompressedSizes(const SizeTarget* targets, const size_t targetCount, uint32_t* sizes,
                                   const size_t sizeCount) {
  if (targets == nullptr || sizes == nullptr || targetCount == 0) {
    return 0;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  if (!loadZipDetails()) return 0;

  int matched = 0;
  const auto expectedMatches = static_cast<int>(targetCount);
  const SizeTarget* const targetEnd = targets + targetCount;
  char itemName[ZIP_MAX_STORED_NAME_LEN + 1];
  CentralDirCursor cursor(file, zipDetails.centralDirOffset);
  CentralDirEntry entry;

  while (cursor.next(entry, itemName)) {
    if (!entry.nameStored) continue;

    const uint16_t nameLen = entry.nameLen;
    uint64_t hash = fnvHash64(itemName, nameLen);
    SizeTarget key = {hash, nameLen, 0};

    auto it = std::lower_bound(targets, targetEnd, key, [](const SizeTarget& a, const SizeTarget& b) {
      return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
    });

    while (it != targetEnd && it->hash == hash && it->len == nameLen) {
      if (it->index < sizeCount) {
        sizes[it->index] = entry.uncompressedSize;
        matched++;
      }
      ++it;
    }

    if (matched >= expectedMatches) {
      break;
    }
  }

  return matched;
}

int ZipFile::fillEntryIdentities(const EntryTarget* targets, const size_t targetCount, EntryIdentity* identities,
                                 const size_t identityCount) {
  if (targets == nullptr || identities == nullptr || targetCount == 0) {
    return 0;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return -1;

  if (!loadZipDetails()) return -1;

  int matched = 0;
  const auto expectedMatches = static_cast<int>(targetCount);
  const EntryTarget* const targetEnd = targets + targetCount;
  char itemName[ZIP_MAX_STORED_NAME_LEN + 1];
  CentralDirCursor cursor(file, zipDetails.centralDirOffset);
  CentralDirEntry entry;
  if (!cursor.valid()) {
    LOG_ERR("ZIP", "Failed to seek ZIP central directory");
    return -1;
  }

  for (uint32_t i = 0; i < zipDetails.totalEntries; ++i) {
    if (!cursor.next(entry, itemName) || !cursor.valid()) {
      LOG_ERR("ZIP", "Failed to read central directory entry %u", static_cast<unsigned>(i));
      return -1;
    }
    if (!entry.nameStored) continue;

    EntryIdentity identity;
    identity.crc32 = entry.crc32;
    identity.compressedSize = entry.compressedSize;
    identity.uncompressedSize = entry.uncompressedSize;
    const uint16_t nameLen = entry.nameLen;
    const uint64_t hash = fnvHash64(itemName, nameLen);
    const EntryTarget key = {hash, nameLen, 0, nullptr};
    auto it = std::lower_bound(targets, targetEnd, key, [](const EntryTarget& a, const EntryTarget& b) {
      return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
    });

    while (it != targetEnd && it->hash == hash && it->len == nameLen) {
      if (it->index < identityCount && it->path != nullptr && memcmp(it->path, itemName, nameLen) == 0 &&
          it->path[nameLen] == '\0') {
        identity.found = true;
        identities[it->index] = identity;
        ++matched;
      }
      ++it;
    }

    if (matched >= expectedMatches) {
      break;
    }
  }

  return matched;
}

uint8_t* ZipFile::readFileToMemory(const char* filename, size_t* size, const bool trailingNullByte) {
  const ScopedOpenClose zip{*this};
  if (!zip) return nullptr;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return nullptr;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return nullptr;

  file.seek(fileOffset);

  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  const auto dataSize = trailingNullByte ? inflatedDataSize + 1 : inflatedDataSize;
  const auto data = static_cast<uint8_t*>(malloc(dataSize));
  if (data == nullptr) {
    LOG_ERR("ZIP", "Failed to allocate memory for output buffer (%zu bytes)", dataSize);
    return nullptr;
  }

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const int readResult = file.read(data, inflatedDataSize);

    if (readResult < 0 || static_cast<size_t>(readResult) != inflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read stored data: %d", readResult);
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else if (fileStat.method == ZIP_METHOD_DEFLATED) {
    bool inflated = false;
    if (deflatedDataSize <= ONE_SHOT_DEFLATE_MAX_COMPRESSED_BYTES) {
      auto* compressedData = static_cast<uint8_t*>(malloc(deflatedDataSize));
      if (compressedData) {
        const int readResult = file.read(compressedData, deflatedDataSize);
        if (readResult < 0 || static_cast<size_t>(readResult) != deflatedDataSize) {
          LOG_ERR("ZIP", "Failed to read compressed data: %d", readResult);
          free(compressedData);
          free(data);
          return nullptr;
        }

        InflateStream inflate;
        if (!inflate.init(false)) {
          LOG_ERR("ZIP", "Failed to init one-shot inflate stream");
          free(compressedData);
          free(data);
          return nullptr;
        }
        inflate.setSource(compressedData, deflatedDataSize);
        if (!inflate.read(data, inflatedDataSize)) {
          LOG_ERR("ZIP", "Failed to inflate file");
          free(compressedData);
          free(data);
          return nullptr;
        }
        free(compressedData);
        inflated = true;
      } else {
        LOG_DBG("ZIP", "Falling back to streaming inflate; compressed buffer alloc failed (%zu bytes)",
                static_cast<size_t>(deflatedDataSize));
      }
    }

    if (!inflated) {
      file.seek(fileOffset);
      auto* fileReadBuffer = static_cast<uint8_t*>(malloc(1024));
      if (!fileReadBuffer) {
        LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer");
        free(data);
        return nullptr;
      }

      ZipInflateCtx ctx;
      ctx.file = &file;
      ctx.fileRemaining = deflatedDataSize;
      ctx.readBuf = fileReadBuffer;
      ctx.readBufSize = 1024;

      // One-shot mode: `data` holds the entire output, so back-references
      // resolve inside it and no 32KB window is allocated.
      InflateStream inflate;
      if (!inflate.init(false)) {
        LOG_ERR("ZIP", "Failed to init inflate stream for %s", filename);
        free(fileReadBuffer);
        free(data);
        return nullptr;
      }
      inflate.setFill(zipFillCallback, &ctx);

      if (!inflate.read(data, inflatedDataSize)) {
        LOG_ERR("ZIP", "Failed to inflate file");
        free(fileReadBuffer);
        free(data);
        return nullptr;
      }
      free(fileReadBuffer);
    }

    // Continue out of block with data set
  } else {
    LOG_ERR("ZIP", "Unsupported compression method");
    free(data);
    return nullptr;
  }

  if (trailingNullByte) data[inflatedDataSize] = '\0';
  if (size) *size = inflatedDataSize;
  return data;
}

bool ZipFile::readStoredFileToStream(const char* filename, Print& out) {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;
  FileStatSlim stat = {};
  if (!loadFileStatSlim(filename, &stat)) return false;
  if (stat.method != ZIP_METHOD_STORED || stat.compressedSize != stat.uncompressedSize) {
    LOG_ERR("ZIP", "Optimizer entry must use STORE: %s", filename);
    return false;
  }
  const long offset = getDataOffset(stat);
  if (offset < 0 || !file.seek(offset)) return false;
  uint8_t buffer[256];
  size_t remaining = stat.uncompressedSize;
  while (remaining) {
    const size_t n = std::min(remaining, sizeof(buffer));
    if (file.read(buffer, n) != static_cast<int>(n) || out.write(buffer, n) != n) {
      LOG_ERR("ZIP", "Stored optimizer entry read/write failed: %s", filename);
      return false;
    }
    remaining -= n;
  }
  return true;
}

bool ZipFile::readFileToStream(const char* filename, Print& out, const size_t chunkSize, const bool allowEarlyStop) {
  const uint32_t started = micros();
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return false;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return false;

  file.seek(fileOffset);
  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  const uint32_t lookupUs = micros() - started;
  uint32_t readInflateUs = 0;
  uint32_t writeUs = 0;

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const auto buffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!buffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for buffer");
      return false;
    }

    size_t remaining = inflatedDataSize;
    while (remaining > 0) {
      const uint32_t readStarted = micros();
      const int readResult = file.read(buffer, remaining < chunkSize ? remaining : chunkSize);
      readInflateUs += micros() - readStarted;
      if (readResult <= 0) {
        LOG_ERR("ZIP", "Could not read more stored bytes: %d", readResult);
        free(buffer);
        return false;
      }
      const size_t dataRead = static_cast<size_t>(readResult);

      const uint32_t writeStarted = micros();
      const size_t written = out.write(buffer, dataRead);
      writeUs += micros() - writeStarted;
      if (written != dataRead) {
        free(buffer);
        if (allowEarlyStop) return true;
        LOG_ERR("ZIP", "Failed to write all output bytes to stream");
        return false;
      }
      remaining -= dataRead;
    }

    free(buffer);
    LOG_DBG("ZIP", "Stream %s: lookup=%uus read=%uus write=%uus bytes=%u", filename, static_cast<unsigned>(lookupUs),
            static_cast<unsigned>(readInflateUs), static_cast<unsigned>(writeUs),
            static_cast<unsigned>(inflatedDataSize));
    return true;
  }

  if (fileStat.method == ZIP_METHOD_DEFLATED) {
    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = deflatedDataSize;

    auto* fileReadBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!fileReadBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer (free=%u, maxAlloc=%u, chunk=%zu)",
              ESP.getFreeHeap(), ESP.getMaxAllocHeap(), chunkSize);
      return false;
    }

    auto* outputBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!outputBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for output buffer (free=%u, maxAlloc=%u, chunk=%zu)", ESP.getFreeHeap(),
              ESP.getMaxAllocHeap(), chunkSize);
      free(fileReadBuffer);
      return false;
    }

    ctx.readBuf = fileReadBuffer;
    ctx.readBufSize = chunkSize;

    InflateStream inflate;
    if (!inflate.init(true)) {
      LOG_ERR("ZIP", "Failed to init inflate stream for %s (free=%u, maxAlloc=%u, chunk=%zu)", filename,
              ESP.getFreeHeap(), ESP.getMaxAllocHeap(), chunkSize);
      free(outputBuffer);
      free(fileReadBuffer);
      return false;
    }
    inflate.setFill(zipFillCallback, &ctx);

    bool success = false;
    size_t totalProduced = 0;

    while (true) {
      size_t produced;
      const uint32_t inflateStarted = micros();
      const InflateStream::Status status = inflate.readAtMost(outputBuffer, chunkSize, &produced);
      readInflateUs += micros() - inflateStarted;

      totalProduced += produced;
      if (totalProduced > static_cast<size_t>(inflatedDataSize)) {
        LOG_ERR("ZIP", "Decompressed size exceeds expected (%zu > %zu)", totalProduced,
                static_cast<size_t>(inflatedDataSize));
        break;
      }

      if (produced > 0) {
        const uint32_t writeStarted = micros();
        const size_t written = out.write(outputBuffer, produced);
        writeUs += micros() - writeStarted;
        if (written != produced) {
          if (allowEarlyStop) {
            success = true;
          } else {
            LOG_ERR("ZIP", "Failed to write all output bytes to stream");
          }
          break;
        }
      }

      if (status == InflateStream::Status::Done) {
        if (totalProduced != static_cast<size_t>(inflatedDataSize)) {
          LOG_ERR("ZIP", "Decompressed size mismatch (expected %zu, got %zu)", static_cast<size_t>(inflatedDataSize),
                  totalProduced);
          break;
        }
        success = true;
        break;
      }

      if (status == InflateStream::Status::Error) {
        LOG_ERR("ZIP", "Decompression failed");
        break;
      }
      // InflateStream::Status::Ok: output buffer full, continue
    }

    free(outputBuffer);
    free(fileReadBuffer);
    LOG_DBG("ZIP", "Stream %s: ok=%d lookup=%uus read/inflate=%uus write=%uus bytes=%u", filename, success,
            static_cast<unsigned>(lookupUs), static_cast<unsigned>(readInflateUs), static_cast<unsigned>(writeUs),
            static_cast<unsigned>(totalProduced));
    return success;  // inflate destructor frees the decompressor state + window
  }

  LOG_ERR("ZIP", "Unsupported compression method");
  return false;
}

std::unique_ptr<ZipFileStreamReader> ZipFile::openFileStream(const char* filename, const size_t chunkSize) {
  auto reader = makeUniqueNoThrow<ZipFileStreamReader>();
  if (!reader) {
    LOG_ERR("ZIP", "Failed to allocate cooperative stream reader");
    return nullptr;
  }
  if (!reader->begin(filePath, filename, chunkSize, directoryCache)) {
    return nullptr;
  }
  return reader;
}
