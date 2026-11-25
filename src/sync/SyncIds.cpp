#include "SyncIds.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>
#if !defined(SIMULATOR)
#include <esp_system.h>
#endif

#include <cstring>

#include "SyncFiles.h"

namespace SyncIds {
namespace {
constexpr uint8_t VERSION = 1;

uint32_t randomGeneration() {
#if !defined(SIMULATOR)
  return esp_random() | 1u;
#else
  return (static_cast<uint32_t>(millis()) ^ 0x49445331u) | 1u;
#endif
}

struct Header {
  uint8_t method = 0;
  uint32_t generation = 0;
};

uint32_t headerCrc(const uint8_t method, const uint32_t generation) {
  uint8_t bytes[6] = {VERSION, method};
  memcpy(bytes + 2, &generation, sizeof(generation));
  return SyncFiles::crc(bytes, sizeof(bytes), 0);
}

// The header must be intact and for the configured match method; ids under
// another method are a different set of documents.
bool readHeader(FsFile& file, Header& header) {
  uint8_t version = 0;
  uint32_t crc = 0;
  if (file.read(&version, 1) != 1 || version != VERSION || file.read(&header.method, 1) != 1 ||
      file.read(&header.generation, 4) != 4 || file.read(&crc, 4) != 4)
    return false;
  if (crc != headerCrc(header.method, header.generation) || !header.generation) return false;
  return header.method == static_cast<uint8_t>(KOREADER_STORE.getMatchMethod());
}

bool writeHeader(SyncFiles::Writer& writer, const uint32_t generation) {
  const uint8_t method = static_cast<uint8_t>(KOREADER_STORE.getMatchMethod());
  return writer.pod(VERSION) && writer.pod(method) && writer.pod(generation) &&
         writer.pod(headerCrc(method, generation));
}

enum class Step : uint8_t { Entry, Corrupt, End, Torn };

// One entry. End: clean end of file. Torn: a partial entry at the end (an
// interrupted append). Corrupt: a complete entry that fails its CRC.
Step readEntry(FsFile& file, uint8_t (&id)[16], std::string& path) {
  const int first = file.read(id, sizeof(id));
  if (first == 0) return Step::End;
  uint8_t length = 0;
  if (first != static_cast<int>(sizeof(id)) || file.read(&length, 1) != 1) return Step::Torn;
  path.resize(length);
  uint32_t crc = 0;
  if ((length && file.read(&path[0], length) != length) || file.read(&crc, 4) != 4) return Step::Torn;
  uint32_t expected = SyncFiles::crc(id, sizeof(id), 0);
  expected = SyncFiles::crc(&length, 1, expected);
  expected = SyncFiles::crc(path.data(), path.size(), expected);
  return crc == expected ? Step::Entry : Step::Corrupt;
}

bool writeEntry(SyncFiles::Writer& writer, const uint8_t (&id)[16], const std::string& path) {
  const auto length = static_cast<uint8_t>(path.size());
  uint32_t crc = SyncFiles::crc(id, sizeof(id), 0);
  crc = SyncFiles::crc(&length, 1, crc);
  crc = SyncFiles::crc(path.data(), path.size(), crc);
  return writer.write(id, sizeof(id)) && writer.pod(length) && writer.write(path.data(), path.size()) &&
         writer.pod(crc);
}

// The previous file's generation whatever its match method, so a new map
// never reuses it (books would wrongly think they are already recorded).
uint32_t previousGeneration() {
  FsFile file;
  if (!Storage.exists(PATH) || !Storage.openFileForRead("SYNC", PATH, file)) return 0;
  uint8_t prefix[2];
  uint32_t value = 0;
  const bool ok = file.read(prefix, 2) == 2 && file.read(&value, 4) == 4;
  file.close();
  return ok ? value : 0;
}

// `avoid`: the open file's own generation when the caller already read it
// (reopening the file while it is open would break SdFat's one-handle rule).
uint32_t freshGeneration(const uint32_t avoid = 0) {
  const uint32_t previous = avoid ? avoid : previousGeneration();
  uint32_t generation = randomGeneration();
  if (generation == previous) generation = (generation + 2) | 1u;
  return generation;
}

bool headerValidAt(const std::string& path) {
  FsFile file;
  Header header;
  if (!Storage.openFileForRead("SYNC", path, file)) return false;
  const bool ok = readHeader(file, header);
  file.close();
  return ok;
}

// A crash between the rewrite's two renames leaves no main file. Put the
// newest complete copy back before anything else (the next rewrite would
// start by clearing .tmp): a .tmp with a valid header (its entries carry their
// own CRCs), else the .bak.
void recoverMissingMain() {
  if (Storage.exists(PATH)) return;
  const std::string tmpPath = std::string(PATH) + ".tmp";
  const std::string backupPath = std::string(PATH) + ".bak";
  if (Storage.exists(tmpPath.c_str()) && headerValidAt(tmpPath) && Storage.rename(tmpPath.c_str(), PATH)) return;
  if (Storage.exists(backupPath.c_str())) Storage.rename(backupPath.c_str(), PATH);
}

bool openValid(FsFile& file, Header& header) {
  recoverMissingMain();
  if (!Storage.exists(PATH) || !Storage.openFileForRead("SYNC", PATH, file)) return false;
  if (readHeader(file, header)) return true;
  file.close();
  header = {};  // a rejected header (other match method) must not look usable
  return false;
}

// Rewrites the file, letting `keep` drop or change each entry; torn and
// corrupt entries are dropped. A missing or unusable file becomes a new, empty
// one with a fresh generation. `newGeneration` also forces one, for a rewrite
// that drops books the app did not touch (they must record themselves again).
template <typename Keep>
bool rewrite(Keep keep, uint32_t* outGeneration = nullptr, const bool newGeneration = false) {
  FsFile source;
  Header header;
  const bool readable = openValid(source, header);
  const uint32_t generation = !readable       ? freshGeneration()
                              : newGeneration ? freshGeneration(header.generation)
                                              : header.generation;
  SyncFiles::Writer writer(PATH);
  if (!writer.open() || !writeHeader(writer, generation)) {
    if (source) source.close();
    return false;
  }
  if (readable) {
    uint8_t id[16];
    std::string path;
    path.reserve(64);
    for (Step step = readEntry(source, id, path); step == Step::Entry || step == Step::Corrupt;
         step = readEntry(source, id, path)) {
      if (step != Step::Entry || !keep(id, path)) continue;
      if (!writeEntry(writer, id, path)) {
        source.close();
        return false;
      }
    }
    source.close();
  }
  if (!writer.commit(false)) return false;
  // The previous copy is only useful while swapping; entries carry their own CRC.
  Storage.remove((std::string(PATH) + ".bak").c_str());
  if (outGeneration) *outGeneration = generation;
  return true;
}
}  // namespace

uint32_t generation() {
  FsFile file;
  Header header;
  if (!openValid(file, header)) return 0;
  file.close();
  return header.generation;
}

uint32_t record(const std::string& path, const uint8_t (&id)[16]) {
  if (path.empty() || path.size() > MAX_PATH_LENGTH) return 0;
  FsFile file;
  Header header;
  bool sameId = false;
  bool otherId = false;
  bool damaged = false;
  bool corrupt = false;
  if (openValid(file, header)) {
    uint8_t storedId[16];
    std::string storedPath;
    storedPath.reserve(64);
    Step step = readEntry(file, storedId, storedPath);
    for (; step == Step::Entry || step == Step::Corrupt; step = readEntry(file, storedId, storedPath)) {
      if (step == Step::Corrupt) {
        damaged = corrupt = true;
        continue;
      }
      if (storedPath != path) continue;
      if (memcmp(storedId, id, sizeof(id)) == 0)
        sameId = true;
      else
        otherId = true;
    }
    // An interrupted append left a partial entry at the end: appending after it
    // would misalign every later entry, so rewrite first.
    damaged |= step == Step::Torn;
    file.close();
    if (sameId && !otherId && !damaged) return header.generation;
  }
  // New file, this path recorded under another id (filename-mode rename,
  // replaced book), or damage to clean up: rewrite, then append.
  if (!header.generation || otherId || damaged) {
    uint32_t created = 0;
    // A corrupt entry (a damaged length) can misalign and drop every entry
    // after it; those books must record themselves again, so take a new
    // generation. A torn tail only ever loses this append.
    if (!rewrite([&](const uint8_t (&)[16], const std::string& stored) { return stored != path; }, &created, corrupt)) {
      LOG_ERR("SYNC", "Cannot prepare sync id map");
      return 0;
    }
    header.generation = created;
  }
  FsFile out = Storage.open(PATH, O_WRONLY | O_APPEND);
  if (!out) {
    LOG_ERR("SYNC", "Cannot append to sync id map");
    return 0;
  }
  const auto length = static_cast<uint8_t>(path.size());
  uint32_t crc = SyncFiles::crc(id, sizeof(id), 0);
  crc = SyncFiles::crc(&length, 1, crc);
  crc = SyncFiles::crc(path.data(), path.size(), crc);
  const bool ok = out.write(id, sizeof(id)) == sizeof(id) && out.write(&length, 1) == 1 &&
                  out.write(path.data(), path.size()) == path.size() && out.write(&crc, 4) == 4;
  out.close();
  if (!ok) {
    // The partial entry is detected and cleaned up by the next record().
    LOG_ERR("SYNC", "Short write to sync id map");
    return 0;
  }
  return header.generation;
}

namespace {
// Read-only check, so edits for books that were never recorded (most renames,
// every XTC/TXT book) write nothing.
template <typename Match>
bool anyEntry(Match match) {
  FsFile file;
  Header header;
  if (!openValid(file, header)) return false;
  uint8_t id[16];
  std::string path;
  path.reserve(64);
  bool found = false;
  for (Step step = readEntry(file, id, path); !found && (step == Step::Entry || step == Step::Corrupt);
       step = readEntry(file, id, path))
    found = step == Step::Entry && match(path);
  file.close();
  return found;
}

bool under(const std::string& folder, const std::string& path) {
  return path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 && path[folder.size()] == '/';
}
}  // namespace

bool removePath(const std::string& path) {
  if (!anyEntry([&](const std::string& stored) { return stored == path; })) return true;
  return rewrite([&](const uint8_t (&)[16], const std::string& stored) { return stored != path; });
}

bool removeUnder(const std::string& folder) {
  if (!anyEntry([&](const std::string& stored) { return under(folder, stored); })) return true;
  return rewrite([&](const uint8_t (&)[16], const std::string& stored) { return !under(folder, stored); });
}

bool renamePath(const std::string& from, const std::string& to) {
  if (from == to || !anyEntry([&](const std::string& stored) { return stored == from; })) return true;
  if (to.size() > MAX_PATH_LENGTH) return removePath(from);
  return rewrite([&](const uint8_t (&)[16], std::string& stored) {
    if (stored == from) stored = to;
    return true;
  });
}

void reset() { SyncFiles::removeAll(PATH); }

bool forEach(const Visit visit, void* context, uint32_t* entries, uint32_t* stale) {
  if (entries) *entries = 0;
  if (stale) *stale = 0;
  FsFile file;
  Header header;
  if (!openValid(file, header)) return false;
  uint8_t id[16];
  std::string path;
  path.reserve(64);
  bool keepGoing = true;
  for (Step step = readEntry(file, id, path); keepGoing && (step == Step::Entry || step == Step::Corrupt);
       step = readEntry(file, id, path)) {
    if (step != Step::Entry) continue;
    if (entries) ++*entries;
    // Checked after the file is closed would need the whole list in RAM; an
    // exists() on another path is fine while this read handle is open.
    if (stale && !Storage.exists(path.c_str())) ++*stale;
    keepGoing = visit(id, path, context);
  }
  file.close();
  return true;
}

bool compact() {
  // Dropped books still have this generation in their cache record; a new one
  // makes them record themselves again when next synced.
  return rewrite([](const uint8_t (&)[16], const std::string& stored) { return Storage.exists(stored.c_str()); },
                 nullptr, true);
}
}  // namespace SyncIds
