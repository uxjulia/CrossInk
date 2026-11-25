#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <strings.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>

// One outstanding replacement on the volume. A reset after parking the old file
// leaves its destination recorded in a closed, synced journal. Only this
// firmware-owned directory is used for transaction metadata.
namespace FilePublication {
inline constexpr char DIRECTORY[] = "/.crosspoint/xfer";
inline constexpr char JOURNAL[] = "/.crosspoint/xfer/destination";
inline constexpr char BACKUP[] = "/.crosspoint/xfer/previous";
inline constexpr char HTTP_STAGE[] = "/.crosspoint/xfer/http";
inline constexpr char WS_STAGE[] = "/.crosspoint/xfer/ws";
inline constexpr char DAV_STAGE[] = "/.crosspoint/xfer/dav";
inline constexpr char USB_SERIAL_STAGE[] = "/.crosspoint/usb-upload.tmp";
inline std::mutex publicationMutex;

inline bool prepare() {
  return Storage.ensureDirectoryExists("/.crosspoint") && Storage.ensureDirectoryExists(DIRECTORY);
}

inline bool readEntryName(HalFile& entry, char* shortName, const size_t shortCapacity, const char*& name) {
  size_t length = entry.getName(shortName, shortCapacity);
  name = shortName;
  if (length) return length < shortCapacity;

  // FAT permits 255 UTF-16 units. A BMP name needs at most three UTF-8 bytes
  // per unit, so retry with enough space only when the common short buffer is
  // too small. Recovery holds publicationMutex while this shared scratch is in use.
  static char longName[3 * 255 + 1];
  length = entry.getName(longName, sizeof(longName));
  name = longName;
  return length && length < sizeof(longName);
}

inline bool equalsIgnoreCase(const char* first, const char* last, const char* expected) {
  while (first < last && *expected &&
         std::tolower(static_cast<unsigned char>(*first)) == std::tolower(static_cast<unsigned char>(*expected))) {
    ++first;
    ++expected;
  }
  return first == last && !*expected;
}

inline bool isAliasFor(const char* first, const char* last, const char* aliasPrefix) {
  const char* tilde = std::find(first, last, '~');
  const size_t prefixLength = std::strlen(aliasPrefix);
  const size_t beforeTilde = static_cast<size_t>(tilde - first);
  if (tilde == last || beforeTilde < prefixLength || beforeTilde > 6 ||
      !equalsIgnoreCase(first, first + prefixLength, aliasPrefix))
    return false;
  const size_t suffixLength = static_cast<size_t>(last - tilde - 1);
  if (suffixLength == 1) return tilde[1] >= '0' && tilde[1] <= '9';
  if (suffixLength != 4) return false;
  for (const char* suffix = tilde + 1; suffix < last; ++suffix) {
    if (!((*suffix >= '0' && *suffix <= '9') || (*suffix >= 'A' && *suffix <= 'F') ||
          (*suffix >= 'a' && *suffix <= 'f')))
      return false;
  }
  return true;
}

inline bool isSafeDestination(const char* path) {
  if (!path || path[0] != '/') return false;
  const size_t pathLength = std::strlen(path);
  if (pathLength < 2 || pathLength > 4096 || path[pathLength - 1] == '/') return false;
  for (const unsigned char* c = reinterpret_cast<const unsigned char*>(path); *c; ++c) {
    if (*c < 32 || *c == 127 || *c == '\\') return false;
  }

  bool inCrosspoint = false;
  bool hasSegment = false;
  int depth = 0;
  for (const char* segment = path + 1; *segment;) {
    while (*segment == '/') ++segment;
    if (!*segment) break;
    const char* separator = std::strchr(segment, '/');
    const char* end = separator ? separator : segment + std::strlen(segment);
    const char* next = separator ? separator + 1 : end;
    const char* first = segment;
    while (first < end && *first == ' ') ++first;
    while (end > first && (end[-1] == '.' || end[-1] == ' ')) --end;
    if (first == end) return false;
    if (equalsIgnoreCase(first, end, ".") || equalsIgnoreCase(first, end, "..")) return false;
    hasSegment = true;

    if (depth == 0) {
      inCrosspoint = equalsIgnoreCase(first, end, ".crosspoint") || isAliasFor(first, end, "CRO");
    } else if (depth == 1 && inCrosspoint) {
      if (equalsIgnoreCase(first, end, "xfer")) return false;
    }
    ++depth;
    segment = next;
  }
  return hasSegment && !(inCrosspoint && depth == 1);
}

// `exists()` returns false both for a missing name and for a failed volume
// lookup. Recovery must distinguish those cases before it removes its journal.
// Directory iteration exposes read and allocation failures through HalFile.
inline bool directoryContains(const char* path, const char* name, bool& contains) {
  contains = false;
  HalFile directory = Storage.open(path);
  if (!directory) return false;
  if (!directory.isDirectory()) {
    directory.close();
    return false;
  }

  bool ok = true;
  while (true) {
    HalFile entry = directory.openNextFile();
    if (!entry) {
      ok = !directory.iterationFailed() && !directory.allocationFailed();
      break;
    }
    char shortName[32] = {};
    const char* entryName = nullptr;
    const bool hasName = readEntryName(entry, shortName, sizeof(shortName), entryName);
    const bool closed = entry.close();
    if (!hasName || !closed) {
      ok = false;
      break;
    }
    if (strcasecmp(entryName, name) == 0) {
      contains = true;
      break;
    }
  }
  return directory.close() && ok;
}

inline bool readTransactionEntries(HalFile& directory, bool& journal, bool& backup) {
  journal = false;
  backup = false;
  bool ok = true;
  while (true) {
    HalFile entry = directory.openNextFile();
    if (!entry) {
      ok = !directory.iterationFailed() && !directory.allocationFailed();
      break;
    }
    char shortName[32] = {};
    const char* entryName = nullptr;
    const bool hasName = readEntryName(entry, shortName, sizeof(shortName), entryName);
    const bool closed = entry.close();
    if (!hasName || !closed) {
      ok = false;
      break;
    }
    journal = journal || strcasecmp(entryName, "destination") == 0;
    backup = backup || strcasecmp(entryName, "previous") == 0;
    if (journal && backup) break;
  }
  return directory.close() && ok;
}

// Mirror the destination below a private, per-protocol directory. Keeping the
// full path avoids hash collisions and preserves filenames needed by validators.
inline bool stagePath(const std::string& destination, const char* kind, std::string& staged) {
  staged.clear();
  if (!kind || !*kind || std::strchr(kind, '/') || std::strchr(kind, '\\') ||
      destination.find('\0') != std::string::npos || !isSafeDestination(destination.c_str()))
    return false;
  staged = std::string(DIRECTORY) + "/" + kind + destination;
  return prepare() && Storage.ensureDirectoryExists(staged.substr(0, staged.rfind('/')).c_str());
}

inline bool recoverLocked() {
  bool journalExists = false;
  bool backupExists = false;
  HalFile transferDirectory = Storage.open(DIRECTORY);
  if (transferDirectory && transferDirectory.isDirectory()) {
    if (!readTransactionEntries(transferDirectory, journalExists, backupExists)) {
      LOG_ERR("XFER", "Could not inspect interrupted transfer state");
      return false;
    }
  } else {
    if (transferDirectory) transferDirectory.close();
    bool xferExists = false;
    if (!directoryContains("/.crosspoint", "xfer", xferExists)) {
      bool crosspointExists = false;
      if (!directoryContains("/", ".crosspoint", crosspointExists)) {
        LOG_ERR("XFER", "Could not inspect interrupted transfer state");
        return false;
      }
      if (!crosspointExists) return true;
      LOG_ERR("XFER", "Could not inspect interrupted transfer state");
      return false;
    }
    if (!xferExists) return true;
    LOG_ERR("XFER", "Could not open interrupted transfer directory");
    return false;
  }
  if (!journalExists) {
    // A backup without its journal has no safe destination to restore to.
    // Keep it in place for diagnosis and refuse to start another replacement.
    if (backupExists) LOG_ERR("XFER", "Previous file exists without a recovery journal");
    return !backupExists;
  }
  if (backupExists) {
    std::string destination;
    if (!Storage.readFileToString("XFER", JOURNAL, 4096, destination) || destination.empty() ||
        destination.find('\0') != std::string::npos || !isSafeDestination(destination.c_str())) {
      LOG_ERR("XFER", "Cannot read interrupted transfer destination");
      return false;
    }
    // FAT rename will refuse an occupied name. Only discard the previous copy
    // when we can positively open the published file.
    HalFile current = Storage.open(destination.c_str());
    if (current) {
      const bool regularFile = !current.isDirectory();
      const bool closed = current.close();
      if (!regularFile || !closed || !Storage.remove(BACKUP)) return false;
    } else if (!Storage.rename(BACKUP, destination.c_str())) {
      LOG_ERR("XFER", "Could not restore %s", destination.c_str());
      return false;
    }
  }
  return Storage.remove(JOURNAL);
}

inline bool removeStageIfPresent(const char* path) {
  HalFile stage = Storage.open(path);
  if (!stage) return true;
  const bool regularFile = !stage.isDirectory();
  const bool closed = stage.close();
  if (!regularFile || !closed || !Storage.remove(path)) return false;
  return true;
}

inline bool recoverAtBoot() {
  std::lock_guard<std::mutex> lock(publicationMutex);
  if (!recoverLocked()) return false;

  const bool cleaned = removeStageIfPresent(HTTP_STAGE) && removeStageIfPresent(WS_STAGE) &&
                       removeStageIfPresent(DAV_STAGE) && removeStageIfPresent(USB_SERIAL_STAGE);
  const bool prepared = prepare();
#ifndef SIMULATOR
  const bool synced = Storage.syncFilesystem();
#else
  constexpr bool synced = true;
#endif
  return cleaned && prepared && synced;
}

inline bool recover() {
  std::lock_guard<std::mutex> lock(publicationMutex);
  return recoverLocked();
}

inline bool publish(const char* staged, const char* destination, bool replace = true, bool requireCleanup = true) {
  std::lock_guard<std::mutex> lock(publicationMutex);
  if (!isSafeDestination(destination) || !staged) return false;
  HalFile source = Storage.open(staged);
  if (!source) return false;
  const bool regularFile = !source.isDirectory();
  const bool sourceClosed = source.close();
  if (!regularFile || !sourceClosed) return false;
  if (!recoverLocked()) return false;
  if (!Storage.exists(destination)) return Storage.rename(staged, destination);
  if (!replace || !prepare()) return false;
  HalFile prior = Storage.open(destination);
  if (!prior || prior.isDirectory() || !prior.close()) return false;

  const size_t length = std::strlen(destination);
  if (!length || length > 4096 || destination[0] != '/') return false;
  HalFile journal;
  if (!Storage.openFileForWrite("XFER", JOURNAL, journal)) return false;
  const bool saved = journal.write(reinterpret_cast<const uint8_t*>(destination), length) == length && journal.sync();
  const bool closed = journal.close();
  if (!saved || !closed || !Storage.rename(destination, BACKUP)) return false;
  if (!Storage.rename(staged, destination)) {
    if (!Storage.rename(BACKUP, destination)) LOG_ERR("XFER", "Rollback pending for %s", destination);
    return false;
  }
  const bool cleaned = recoverLocked();
  return cleaned || !requireCleanup;
}
}  // namespace FilePublication
