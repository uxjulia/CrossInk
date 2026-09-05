#include "ImageFolderIndex.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>

#include "CrossPointState.h"

namespace ImageFolderIndex {
namespace {

// Shared by sleep-image and boot-screen folders: both are small, infrequently
// rebuilt rotations, so one cache directory (keyed by a hash of each indexed
// folder's path) is simpler than splitting per subsystem. The name predates
// the boot-screen use and is kept to avoid orphaning existing sleep caches.
constexpr char INDEX_DIR[] = "/.crosspoint/sleep-image-index";
constexpr char INDEX_MAGIC[] = {'C', 'S', 'I', 'X'};
constexpr uint8_t INDEX_VERSION = 1;
constexpr uint8_t FLAG_INCLUDE_PNG = 1 << 0;
constexpr uint8_t FLAG_VALIDATED = 1 << 1;
constexpr size_t MAX_FILENAME_LENGTH = 255;
constexpr size_t NAME_BUFFER_SIZE = MAX_FILENAME_LENGTH + 1;
constexpr size_t MAX_CACHE_PATH = 128;
constexpr uint16_t MAX_RECORDS = UINT16_MAX;
// Upper bound for a caller's recent-history window; callers pass their own
// buffer/capacity, this only sizes chooseIndex()'s scratch array.
constexpr uint8_t MAX_RECENT_WINDOW = 32;

#pragma pack(push, 1)
struct IndexHeader {
  char magic[sizeof(INDEX_MAGIC)];
  uint8_t version;
  uint8_t flags;
  uint16_t pathLength;
  uint16_t recordCount;
  uint16_t recordSize;
  uint32_t recordsOffset;
};

// A fixed-size record keeps the cache seekable without retaining a directory
// vector in RAM. The build path holds one record on the heap and reuses it.
struct IndexRecord {
  uint16_t nameLength;
  uint8_t flags;
  uint8_t reserved;
  char name[NAME_BUFFER_SIZE];
};
#pragma pack(pop)

static_assert(sizeof(IndexRecord) == sizeof(uint16_t) + 2 + NAME_BUFFER_SIZE, "image index record packing");

uint64_t fnv1a64(std::string_view value) {
  uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

bool pathHasPrefix(std::string_view path, std::string_view folder, const bool ignoreCase = false) {
  if (folder.empty()) return false;
  if (path.size() < folder.size()) return false;
  for (size_t i = 0; i < folder.size(); ++i) {
    const char pathChar = path[i];
    const char folderChar = folder[i];
    if (ignoreCase ? tolower(static_cast<unsigned char>(pathChar)) != tolower(static_cast<unsigned char>(folderChar))
                   : pathChar != folderChar) {
      return false;
    }
  }
  return path.size() == folder.size() || path[folder.size()] == '/';
}

bool pathsIntersect(std::string_view a, std::string_view b) { return pathHasPrefix(a, b) || pathHasPrefix(b, a); }

// Folders this module indexes: the configurable sleep-image folder plus the
// two fixed-name folders (sleep and boot-screen). Boot-screen has no
// configurable folder, unlike sleep's preferredSleepFolderPath.
bool isTrackedImageFolderPath(std::string_view path) {
  if (path.empty()) return false;
  const auto& preferredSleep = APP_STATE.preferredSleepFolderPath;
  return pathsIntersect(path, preferredSleep) || pathHasPrefix(path, "/.sleep", true) ||
         pathHasPrefix("/.sleep", path, true) || pathHasPrefix(path, "/sleep", true) ||
         pathHasPrefix("/sleep", path, true) || pathHasPrefix(path, "/.bootscreen", true) ||
         pathHasPrefix("/.bootscreen", path, true) || pathHasPrefix(path, "/bootscreen", true) ||
         pathHasPrefix("/bootscreen", path, true);
}

bool makeCachePath(const std::string& directory, const bool includePng, char* output, const size_t outputSize) {
  const uint64_t hash = fnv1a64(directory);
  const int written = snprintf(output, outputSize, "%s/%016llx-%s.idx", INDEX_DIR,
                               static_cast<unsigned long long>(hash), includePng ? "all" : "bmp");
  return written > 0 && static_cast<size_t>(written) < outputSize;
}

bool readExact(HalFile& file, void* buffer, const size_t size) {
  return file.read(buffer, size) == static_cast<int>(size);
}

bool writeExact(HalFile& file, const void* buffer, const size_t size) { return file.write(buffer, size) == size; }

bool headerMatches(const IndexHeader& header, const std::string& directory, const bool includePng,
                   const bool requireValidated) {
  return memcmp(header.magic, INDEX_MAGIC, sizeof(INDEX_MAGIC)) == 0 && header.version == INDEX_VERSION &&
         (header.flags & FLAG_INCLUDE_PNG) == (includePng ? FLAG_INCLUDE_PNG : 0) &&
         (!requireValidated || (header.flags & FLAG_VALIDATED) != 0) && header.pathLength == directory.size() &&
         header.recordSize == sizeof(IndexRecord) && header.recordsOffset == sizeof(IndexHeader) + header.pathLength &&
         header.recordCount <= MAX_RECORDS;
}

bool openValidIndex(const std::string& directory, const bool includePng, const bool requireValidated, HalFile& file,
                    IndexHeader& header, char* cachePath, const size_t cachePathSize) {
  if (!makeCachePath(directory, includePng, cachePath, cachePathSize)) return false;
  file = Storage.open(cachePath);
  if (!file) return false;

  if (!readExact(file, &header, sizeof(header)) || !headerMatches(header, directory, includePng, requireValidated)) {
    file.close();
    return false;
  }

  char pathBuffer[NAME_BUFFER_SIZE] = {};
  if (header.pathLength >= sizeof(pathBuffer) || !readExact(file, pathBuffer, header.pathLength) ||
      memcmp(pathBuffer, directory.data(), header.pathLength) != 0) {
    file.close();
    return false;
  }

  const uint64_t expectedSize =
      static_cast<uint64_t>(header.recordsOffset) + static_cast<uint64_t>(header.recordCount) * sizeof(IndexRecord);
  if (file.fileSize64() != expectedSize) {
    file.close();
    return false;
  }
  return true;
}

bool readRecordName(HalFile& file, const IndexHeader& header, const uint16_t index, std::string& name, bool& isPng) {
  if (index >= header.recordCount) return false;
  const uint64_t offset =
      static_cast<uint64_t>(header.recordsOffset) + static_cast<uint64_t>(index) * sizeof(IndexRecord);
  if (!file.seek64(offset)) return false;

  uint16_t nameLength = 0;
  uint8_t flags = 0;
  uint8_t reserved = 0;
  if (!readExact(file, &nameLength, sizeof(nameLength)) || !readExact(file, &flags, sizeof(flags)) ||
      !readExact(file, &reserved, sizeof(reserved)) || nameLength == 0 || nameLength > MAX_FILENAME_LENGTH) {
    return false;
  }

  name.assign(NAME_BUFFER_SIZE, '\0');
  if (!readExact(file, name.data(), NAME_BUFFER_SIZE) || name[nameLength] != '\0') return false;
  name.resize(nameLength);
  isPng = (flags & 1) != 0;
  return (isPng && FsHelpers::hasPngExtension(name)) || (!isPng && FsHelpers::hasBmpExtension(name));
}

bool cacheRecordExists(const std::string& directory, const std::string& name) {
  std::string path = directory;
  if (path.back() != '/') path += '/';
  path += name;
  return Storage.exists(path.c_str());
}

uint16_t chooseIndex(const uint16_t recordCount, const uint16_t* recentIndices, const uint8_t recentCapacity,
                     const uint8_t recentPos, const uint8_t recentFill, const uint8_t recentWindow) {
  if (recordCount == 0) return 0;

  std::array<uint16_t, MAX_RECENT_WINDOW> recent{};
  uint8_t recentCount = 0;
  const uint8_t effectiveWindow =
      static_cast<uint8_t>(std::min({recentWindow, recentFill, recentCapacity, MAX_RECENT_WINDOW}));
  for (uint8_t i = 0; i < effectiveWindow; ++i) {
    const uint8_t slot = static_cast<uint8_t>((recentPos + recentCapacity - 1 - i) % recentCapacity);
    const uint16_t index = recentIndices[slot];
    if (index >= recordCount ||
        std::find(recent.begin(), recent.begin() + recentCount, index) != recent.begin() + recentCount) {
      continue;
    }
    recent[recentCount++] = index;
  }

  std::sort(recent.begin(), recent.begin() + recentCount);
  if (recentCount >= recordCount) return static_cast<uint16_t>(random(recordCount));

  const uint16_t nonRecentCount = static_cast<uint16_t>(recordCount - recentCount);
  uint16_t rank = static_cast<uint16_t>(random(nonRecentCount));
  for (uint8_t i = 0; i < recentCount; ++i) {
    if (recent[i] <= rank) {
      ++rank;
    } else {
      break;
    }
  }
  return rank;
}

bool buildIndex(const std::string& directory, const bool includePng, const bool validateBmpHeaders, char* cachePath,
                const size_t cachePathSize) {
  if (!Storage.ensureDirectoryExists("/.crosspoint") || !Storage.ensureDirectoryExists(INDEX_DIR)) {
    LOG_ERR("IMGIDX", "Cannot create image index directory");
    return false;
  }

  if (!makeCachePath(directory, includePng, cachePath, cachePathSize)) return false;
  char tempPath[MAX_CACHE_PATH] = {};
  const int tempWritten = snprintf(tempPath, sizeof(tempPath), "%s.tmp", cachePath);
  if (tempWritten <= 0 || static_cast<size_t>(tempWritten) >= sizeof(tempPath)) return false;

  auto record = makeUniqueNoThrow<IndexRecord>();
  auto nameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!record || !nameBuffer) {
    LOG_ERR("IMGIDX", "Index scratch allocation failed");
    return false;
  }

  HalFile output;
  if (!Storage.openFileForWrite("IMGIDX", tempPath, output)) {
    LOG_ERR("IMGIDX", "Cannot open cache temp file: %s", tempPath);
    return false;
  }

  IndexHeader header{};
  memcpy(header.magic, INDEX_MAGIC, sizeof(INDEX_MAGIC));
  header.version = INDEX_VERSION;
  header.flags = includePng ? FLAG_INCLUDE_PNG : 0;
  header.flags |= validateBmpHeaders ? FLAG_VALIDATED : 0;
  header.pathLength = static_cast<uint16_t>(directory.size());
  header.recordSize = sizeof(IndexRecord);
  header.recordsOffset = sizeof(IndexHeader) + header.pathLength;
  if (directory.size() > MAX_FILENAME_LENGTH || !writeExact(output, &header, sizeof(header)) ||
      !writeExact(output, directory.data(), directory.size())) {
    output.close();
    Storage.remove(tempPath);
    return false;
  }

  FsFile directoryFile = Storage.open(directory.c_str());
  if (!directoryFile || !directoryFile.isDirectory()) {
    output.close();
    Storage.remove(tempPath);
    return false;
  }

  uint32_t scanned = 0;
  for (auto file = directoryFile.openNextFile(); file; file = directoryFile.openNextFile()) {
    const size_t nameLength = file.getName(nameBuffer.get(), NAME_BUFFER_SIZE);
    const std::string_view name(nameBuffer.get(), std::min(nameLength, MAX_FILENAME_LENGTH));
    const bool isBmp = FsHelpers::hasBmpExtension(name);
    const bool isPng = includePng && FsHelpers::hasPngExtension(name);
    bool valid = nameLength > 0 && nameLength <= MAX_FILENAME_LENGTH && !name.empty() && name.front() != '.' &&
                 !file.isDirectory() && (isBmp || isPng);

    if (valid && validateBmpHeaders && isBmp) {
      Bitmap bitmap(file);
      valid = bitmap.parseHeaders() == BmpReaderError::Ok;
    }

    if (valid && header.recordCount < MAX_RECORDS) {
      memset(record.get(), 0, sizeof(IndexRecord));
      record->nameLength = static_cast<uint16_t>(nameLength);
      record->flags = isPng ? 1 : 0;
      memcpy(record->name, name.data(), nameLength);
      if (!writeExact(output, record.get(), sizeof(IndexRecord))) {
        file.close();
        directoryFile.close();
        output.close();
        Storage.remove(tempPath);
        return false;
      }
      ++header.recordCount;
    }
    file.close();
    if ((++scanned & 0xFF) == 0) delay(1);
  }
  directoryFile.close();

  const bool finalized = output.seek(0) && writeExact(output, &header, sizeof(header)) && output.sync();
  // Close before removing: the earlier error paths do the same, and deleting a
  // still-open file is not safe on SdFat.
  if (!output.close() || !finalized) {
    Storage.remove(tempPath);
    return false;
  }

  Storage.remove(cachePath);
  if (!Storage.rename(tempPath, cachePath)) {
    Storage.remove(tempPath);
    return false;
  }
  return true;
}

bool selectFromCache(const std::string& directory, const bool includePng, const bool validateBmpHeaders,
                     const uint16_t* recentIndices, const uint8_t recentCapacity, const uint8_t recentPos,
                     const uint8_t recentFill, const uint8_t recentWindow, Selection& selection,
                     const bool allowRebuild) {
  char cachePath[MAX_CACHE_PATH] = {};
  IndexHeader header{};
  HalFile cache;
  if (!openValidIndex(directory, includePng, validateBmpHeaders, cache, header, cachePath, sizeof(cachePath))) {
    if (!allowRebuild || !buildIndex(directory, includePng, validateBmpHeaders, cachePath, sizeof(cachePath)))
      return false;
    if (!openValidIndex(directory, includePng, validateBmpHeaders, cache, header, cachePath, sizeof(cachePath)))
      return false;
  }

  const uint16_t selectedIndex =
      chooseIndex(header.recordCount, recentIndices, recentCapacity, recentPos, recentFill, recentWindow);
  std::string filename;
  bool isPng = false;
  if (!readRecordName(cache, header, selectedIndex, filename, isPng)) {
    cache.close();
    if (!allowRebuild || !buildIndex(directory, includePng, validateBmpHeaders, cachePath, sizeof(cachePath)) ||
        !openValidIndex(directory, includePng, validateBmpHeaders, cache, header, cachePath, sizeof(cachePath))) {
      return false;
    }
    const uint16_t rebuiltIndex =
        chooseIndex(header.recordCount, recentIndices, recentCapacity, recentPos, recentFill, recentWindow);
    if (!readRecordName(cache, header, rebuiltIndex, filename, isPng)) {
      cache.close();
      return false;
    }
    selection.index = rebuiltIndex;
  } else {
    selection.index = selectedIndex;
  }
  cache.close();

  selection.path = directory;
  if (selection.path.back() != '/') selection.path += '/';
  selection.path += filename;
  selection.isPng = isPng;
  if (cacheRecordExists(directory, filename)) return true;

  // The card can be changed outside the firmware, so an otherwise valid index
  // may point at a deleted or renamed entry. Rebuild once before giving the
  // caller a chance to use its safe legacy fallback.
  if (!allowRebuild || !buildIndex(directory, includePng, validateBmpHeaders, cachePath, sizeof(cachePath)) ||
      !openValidIndex(directory, includePng, validateBmpHeaders, cache, header, cachePath, sizeof(cachePath))) {
    return false;
  }

  const uint16_t rebuiltIndex =
      chooseIndex(header.recordCount, recentIndices, recentCapacity, recentPos, recentFill, recentWindow);
  if (!readRecordName(cache, header, rebuiltIndex, filename, isPng)) {
    cache.close();
    return false;
  }
  cache.close();
  selection.index = rebuiltIndex;
  selection.path = directory;
  if (selection.path.back() != '/') selection.path += '/';
  selection.path += filename;
  selection.isPng = isPng;
  return cacheRecordExists(directory, filename);
}

}  // namespace

bool resolveBootScreenDirectory(std::string& directory) {
  char resolvedPath[256] = {};  // FAT long filenames are at most 255 bytes.
  if (!FsHelpers::resolveRootDirectoryIgnoreCase("/.bootscreen", resolvedPath, sizeof(resolvedPath)) &&
      !FsHelpers::resolveRootDirectoryIgnoreCase("/bootscreen", resolvedPath, sizeof(resolvedPath))) {
    return false;
  }

  directory = resolvedPath;
  return true;
}

bool select(const std::string& directory, const bool includePng, const bool validateBmpHeaders,
            const uint16_t* recentIndices, const uint8_t recentCapacity, const uint8_t recentPos,
            const uint8_t recentFill, const uint8_t recentWindow, Selection& selection) {
  selection = Selection{};
  if (directory.empty() || directory.size() > MAX_FILENAME_LENGTH) return false;

  // A normal selection may use an existing index. A missing entry triggers one
  // rebuild, which catches deletions and renames without a directory walk on
  // every selection. A second miss falls back to the caller's legacy scan.
  if (selectFromCache(directory, includePng, validateBmpHeaders, recentIndices, recentCapacity, recentPos, recentFill,
                      recentWindow, selection, true))
    return true;

  invalidate();
  return false;
}

void invalidate() {
  if (Storage.exists(INDEX_DIR)) {
    if (!Storage.removeDir(INDEX_DIR)) LOG_ERR("IMGIDX", "Failed to remove image index cache");
  }
}

void invalidateForPath(const char* path) {
  if (path && isTrackedImageFolderPath(path)) invalidate();
}

}  // namespace ImageFolderIndex
