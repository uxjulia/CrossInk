#include "LibraryBuilder.h"

#include <Arduino.h>
#include <BufferedFile.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>

#include "../../src/util/BookCacheUtils.h"
#include "LibraryFileTypes.h"
#include "LibraryIndexFile.h"
#include "LibraryMetadataCache.h"
#include "LibraryScanPolicy.h"
#include "LibrarySort.h"
#include "LibraryText.h"

namespace library {
namespace {

constexpr char INDEX_PATH[] = "/.crosspoint/library.idx";
constexpr char NEW_PATH[] = "/.crosspoint/library.new";
constexpr char BACKUP_PATH[] = "/.crosspoint/library.bak";
constexpr char STAGE_PATH[] = "/.crosspoint/library.stage";
// Build-only scratch. Each holds 16 bytes or less per book and is streamed, so
// nothing proportional to the library stays in RAM between phases.
constexpr char PRIOR_PATH[] = "/.crosspoint/library.prior";      // previous books by path hash
constexpr char RENAME_PATH[] = "/.crosspoint/library.rename";    // unmatched previous books by size
constexpr char ORDER_PATH[] = "/.crosspoint/library.order";      // title position -> stage index
constexpr char AUTHORS_PATH[] = "/.crosspoint/library.authors";  // author-key order for the spelling vote
constexpr char CANON_PATH[] = "/.crosspoint/library.canon";      // title position -> canonical author's stage index
constexpr char RUN_PATH[] = "/.crosspoint/library.runs";         // spilled sort runs
constexpr char CACHE_DIR[] = "/.crosspoint";
constexpr size_t LIBRARY_IO_BUFFER_SIZE = 4096;
// Sequential scratch streams need only a sector of buffering each.
constexpr size_t SCRATCH_IO_BUFFER_SIZE = 512;

// Matches lib/FileIndex's buffer so a name this walk accepts is one the file
// browser could also show.
constexpr size_t NAME_BUF_SIZE = 512;

// One staged entry: the record as far as the filename can fill it, followed by
// the display name. Fixed stride keeps the second pass a seek rather than a scan.
constexpr size_t STAGE_NAME_BYTES = 255;
constexpr size_t STAGE_AUTHOR_BYTES = 128;
constexpr size_t STAGE_METADATA_BYTES = 128;
// A folder path is stored behind one length byte in the folder section.
constexpr size_t FOLDER_PATH_BYTES = 255;
struct StagedEntry {
  ClixRecord record;
  uint32_t creationTime;
  uint32_t seriesPosition;
  uint64_t pathHash;
  char name[STAGE_NAME_BYTES];
  // Cleaned source spelling from this book. The spelling actually shown
  // is chosen later, across every book by the same person.
  uint8_t authorLen;
  char author[STAGE_AUTHOR_BYTES];
  // The title the book gives itself, kept SEPARATE from `name`. Writing it into
  // the name slot reconstructs the wrong path in readPath() and hashes a dirent
  // name on one side of reconciliation against a stored title on the other.
  uint8_t titleLen;
  char title[STAGE_NAME_BYTES];
  uint8_t seriesLen;
  char series[STAGE_METADATA_BYTES];
  uint8_t genreLen;
  char genre[STAGE_METADATA_BYTES];
};
constexpr size_t STAGE_STRIDE = sizeof(StagedEntry);

// Flip the sign half of IEEE-754 so u32 order matches signed numeric order.
// The all-ones value is reserved for missing or malformed metadata.
uint32_t parseSeriesPosition(const std::string& text) {
  static_assert(std::numeric_limits<float>::is_iec559, "series positions require IEEE-754 floats");
  if (text.empty()) return CLIX_UNKNOWN_SERIES_POSITION;
  char* end = nullptr;
  const float value = std::strtof(text.c_str(), &end);
  const bool parsed = end != text.c_str();
  while (std::isspace(static_cast<unsigned char>(*end))) ++end;
  if (!parsed || *end != '\0' || !std::isfinite(value)) return CLIX_UNKNOWN_SERIES_POSITION;
  uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value), "series position needs a 32-bit float");
  const float normalized = value == 0 ? 0 : value;  // -0 and +0 have the same order.
  memcpy(&bits, &normalized, sizeof(bits));
  return bits & 0x80000000u ? ~bits : bits ^ 0x80000000u;
}

// Sort array element. Holding a 12-byte key segment rather than the whole fold
// keeps this at 14 bytes per book. Equal-prefix runs are refined from the
// staged source in later passes without growing the resident array.
constexpr uint8_t MAX_AUTHOR_SPELLINGS = 16;
struct SpellingSlot {
  char text[STAGE_AUTHOR_BYTES];
  uint16_t source;  // stage index of a book carrying this spelling
  uint16_t count;
  uint8_t len;
};
static_assert(sizeof(SpellingSlot) <= 136, "spelling vote scratch grew unexpectedly");

// Hooks for the one foreground build. Builds never overlap, so file scope
// avoids threading the callbacks through every phase helper.
struct BuildControl {
  const BuildCallbacks* callbacks = nullptr;
  BuildProgress progress;
  BuildProgress reported;
  uint32_t lastPollMs = 0;
  uint32_t lastProgressMs = 0;
  bool cancelled = false;
};
BuildControl buildControl;

bool buildCancelled() { return buildControl.cancelled; }

void reportProgress() {
  const BuildCallbacks* callbacks = buildControl.callbacks;
  if (!callbacks || !callbacks->progress) return;
  callbacks->progress(callbacks->context, buildControl.progress);
  buildControl.reported = buildControl.progress;
  // Measured after the callback, so a slow e-ink refresh does not eat the interval.
  buildControl.lastProgressMs = millis();
}

void pollBuildControl() {
  const BuildCallbacks* callbacks = buildControl.callbacks;
  if (!callbacks || buildControl.cancelled) return;
  if (callbacks->cancelRequested && millis() - buildControl.lastPollMs >= LIBRARY_CANCEL_POLL_MS) {
    const bool cancel = callbacks->cancelRequested(callbacks->context);
    buildControl.lastPollMs = millis();
    if (cancel) {
      LOG_INF("LIBIDX", "build cancelled; keeping the previous index");
      buildControl.cancelled = true;
      return;
    }
  }
  // Each report repaints the panel, so skip it when nothing visible changed.
  const bool changed = buildControl.progress.phase != buildControl.reported.phase ||
                       buildControl.progress.books != buildControl.reported.books;
  if (changed && millis() - buildControl.lastProgressMs >= LIBRARY_PROGRESS_INTERVAL_MS) reportProgress();
}

void setBuildPhase(const BuildPhase phase) {
  if (buildControl.progress.phase == phase) return;
  buildControl.progress.phase = phase;
  reportProgress();
}

// Let FreeRTOS run the idle task during every long phase, including builds
// without a UI callback and the sort/emit work after the directory walk. The
// counter keeps the delay out of tight per-byte operations while bounding CPU
// work between yields. Cancellation and progress ride on the same calls.
void serviceBuilder(uint32_t& workUnits) {
  if ((++workUnits & 0x1Fu) == 0) delay(1);
  pollBuildControl();
}

// Non-capturing adapters so the bounded sorter can share the build's yield and
// cancellation without depending on this file.
uint32_t sortServiceUnits = 0;
void serviceSort() { serviceBuilder(sortServiceUnits); }

SortConfig sortConfig(const size_t keyBytes = SORT_SEGMENT_BYTES, const SortSegmentLoader load = nullptr,
                      void* const loadContext = nullptr) {
  SortConfig config;
  config.runPath = RUN_PATH;
  config.keyBytes = keyBytes;
  config.load = load;
  config.loadContext = loadContext;
  config.service = &serviceSort;
  config.stopped = &buildCancelled;
  return config;
}

void putBigEndian(char* out, const uint64_t value, const size_t bytes) {
  for (size_t i = 0; i < bytes; i++) out[i] = static_cast<char>(value >> (8 * (bytes - 1 - i)));
}

// Fill one key segment from `key` starting at `offset`. An empty value is all
// 0xFF, which sorts after every folded byte, so missing values come last.
void writeKeySegment(const std::string& key, const size_t offset, char* segment) {
  if (key.empty()) {
    memset(segment, 0xFF, SORT_SEGMENT_BYTES);
    return;
  }
  memset(segment, 0, SORT_SEGMENT_BYTES);
  if (offset < key.size()) memcpy(segment, key.data() + offset, std::min(key.size() - offset, SORT_SEGMENT_BYTES));
}

void removeBuildScratch(const char* folderStagePath) {
  for (const char* path : {STAGE_PATH, PRIOR_PATH, RENAME_PATH, ORDER_PATH, AUTHORS_PATH, CANON_PATH, RUN_PATH}) {
    Storage.remove(path);
  }
  Storage.remove(folderStagePath);
}

// A sequential u16-per-title-position scratch stream, read back by later passes.
class OrderReader {
 public:
  bool open(const char* path) {
    close();
    if (!Storage.openFileForRead("LIBIDX", path, file)) return false;
    reader.emplace(file, SCRATCH_IO_BUFFER_SIZE);
    return true;
  }
  bool next(uint16_t& out) {
    if (reader && reader->read(&out, sizeof(out)) == sizeof(out)) return true;
    LOG_ERR("LIBIDX", "order stream ended early");
    return false;
  }
  void close() {
    reader.reset();
    if (file) file.close();
  }
  ~OrderReader() { close(); }

 private:
  HalFile file;
  std::optional<serialization::BufferedFileReader> reader;
};

// Writes each sorted entry's ordinal or source to a scratch stream.
struct StreamWriter {
  HalFile file;
  std::optional<serialization::BufferedFileWriter> out;
  bool open(const char* path) {
    if (!Storage.openFileForWrite("LIBIDX", path, file)) return false;
    out.emplace(file, SCRATCH_IO_BUFFER_SIZE);
    return true;
  }
  bool close() {
    const bool flushed = out && out->flush();
    out.reset();
    const bool closed = file && file.close();
    return flushed && closed;
  }
  static bool writeSource(void* context, const SortEntry& entry) {
    static_cast<StreamWriter*>(context)->out->write(&entry.source, sizeof(entry.source));
    return true;
  }
  static bool writeEntry(void* context, const SortEntry& entry) {
    static_cast<StreamWriter*>(context)->out->write(&entry, sizeof(entry));
    return true;
  }
};

bool recoverInterruptedInstall() {
  if (!Storage.exists(BACKUP_PATH)) return true;
  if (!Storage.exists(INDEX_PATH)) {
    if (Storage.rename(BACKUP_PATH, INDEX_PATH)) {
      LOG_INF("LIBIDX", "restored previous index after interrupted install");
      return true;
    }
    LOG_ERR("LIBIDX", "cannot restore %s; rebuild deferred", BACKUP_PATH);
    return false;
  }

  // Both names exist when power was lost after the new index became live but
  // before backup cleanup. Validate them one at a time (SdFat has one reader)
  // before deciding which copy is stale.
  LibraryIndexFile candidate;
  // An older live index is still valid during a format upgrade. Keep it over
  // a stale backup so interrupted cleanup cannot roll back arrival history.
  if (candidate.openForReconciliation(INDEX_PATH)) {
    candidate.close();
    if (Storage.remove(BACKUP_PATH)) return true;
    LOG_ERR("LIBIDX", "cannot remove stale backup; rebuild deferred");
    return false;
  }
  if (candidate.ioFailed()) {
    LOG_ERR("LIBIDX", "cannot read live index during recovery");
    return false;
  }
  candidate.close();

  if (candidate.openForReconciliation(BACKUP_PATH)) {
    candidate.close();
    if (!Storage.remove(INDEX_PATH) || !Storage.rename(BACKUP_PATH, INDEX_PATH)) {
      LOG_ERR("LIBIDX", "validated backup could not replace an invalid live index");
      return false;
    }
    LOG_INF("LIBIDX", "restored previous index after interrupted install");
    return true;
  }
  if (candidate.ioFailed()) {
    LOG_ERR("LIBIDX", "cannot read backup index during recovery");
    return false;
  }
  candidate.close();

  // Neither file validates. The live path will be preserved until a complete
  // new index is ready; the unusable backup only blocks transactional install.
  if (!Storage.remove(BACKUP_PATH)) {
    LOG_ERR("LIBIDX", "invalid stale backup cannot be removed; rebuild deferred");
    return false;
  }
  return true;
}

bool installNewIndex() {
  const bool hadPrevious = Storage.exists(INDEX_PATH);

  // A backup beside a live index is left by a successful install interrupted
  // before cleanup. It is stale now; remove it before reserving that name for
  // the current previous index.
  if (Storage.exists(BACKUP_PATH) && !Storage.remove(BACKUP_PATH)) {
    LOG_ERR("LIBIDX", "cannot remove stale backup; keeping the live index");
    Storage.remove(NEW_PATH);
    return false;
  }

  if (hadPrevious && !Storage.rename(INDEX_PATH, BACKUP_PATH)) {
    LOG_ERR("LIBIDX", "cannot stage previous index for replacement");
    Storage.remove(NEW_PATH);
    return false;
  }

  if (!Storage.rename(NEW_PATH, INDEX_PATH)) {
    LOG_ERR("LIBIDX", "rename %s -> %s failed", NEW_PATH, INDEX_PATH);
    if (hadPrevious && !Storage.rename(BACKUP_PATH, INDEX_PATH)) {
      // recoverInterruptedInstall() retries this on the next rebuild. Do not
      // remove the backup: it is the only complete index left.
      LOG_ERR("LIBIDX", "previous index rollback failed; backup retained at %s", BACKUP_PATH);
    }
    Storage.remove(NEW_PATH);
    return false;
  }

  if (hadPrevious && !Storage.remove(BACKUP_PATH)) {
    // The new live index is already complete. A stale backup is harmless and is
    // removed before the next replacement attempt.
    LOG_ERR("LIBIDX", "new index installed but stale backup cleanup failed");
  }
  return true;
}

bool isBookName(const std::string& name) { return fileTypeFor(name) != 0; }

// macOS AppleDouble sidecars and hidden entries. The file browser already hides
// these (FileBrowserActivity isMacOSMetadataEntry); the shelf must agree, or a
// card written on a Mac shows every book twice.
bool isHiddenOrSidecar(const char* name) { return name[0] == '.'; }

bool isSystemDirectory(const char* name) {
  // FAT names are case-insensitive. Match whole directory names, never book
  // basenames or prefixes such as XTCacheArchive. No temporary strings needed.
  for (const char* system : {"XTCache", "System Volume Information"}) {
    const char* candidate = name;
    while (*candidate && *system &&
           std::tolower(static_cast<unsigned char>(*candidate)) == std::tolower(static_cast<unsigned char>(*system))) {
      ++candidate;
      ++system;
    }
    if (!*candidate && !*system) return true;
  }
  return false;
}

std::string stemOf(const std::string& name) {
  const size_t dot = name.find_last_of('.');
  return (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
}

// The previous index's books, sorted on the card by one key, for matching this
// scan's books against them. RAM holds only every block's first key (at most
// 6 KiB), one block, and a claimed-entry bitset (at most 4 KiB), so a lookup is
// one block read instead of a resident entry per previous book.
class PriorTable {
 public:
  static constexpr uint32_t MAX_FENCE = 512;

  bool beginWrite(const char* tablePath, const uint16_t entries) {
    close();
    path = tablePath;
    count = entries;
    blockEntries = 64;
    while ((static_cast<uint32_t>(count) + blockEntries - 1) / blockEntries > MAX_FENCE) blockEntries *= 2;
    const uint32_t blocks = (static_cast<uint32_t>(count) + blockEntries - 1) / blockEntries;
    if (!fence.allocate(blocks) || !claims.allocate((count + 7u) / 8u) || !block.allocate(blockEntries)) {
      LOG_ERR("LIBIDX", "prior table alloc failed (%u books)", static_cast<unsigned>(count));
      return false;
    }
    if (!out.open(path)) return false;
    written = 0;
    return true;
  }
  static bool onSorted(void* context, const SortEntry& entry) {
    auto* table = static_cast<PriorTable*>(context);
    if (table->written >= table->count) return false;
    if (table->written % table->blockEntries == 0)
      memcpy(table->fence[table->written / table->blockEntries].key, entry.key, SORT_SEGMENT_BYTES);
    table->written++;
    return StreamWriter::writeEntry(&table->out, entry);
  }
  bool endWrite() {
    if (!out.close() || written != count) {
      LOG_ERR("LIBIDX", "prior table write failed (%u of %u)", static_cast<unsigned>(written),
              static_cast<unsigned>(count));
      return false;
    }
    loadedBlock = UINT32_MAX;
    return Storage.openFileForRead("LIBIDX", path, file);
  }
  // First unclaimed entry with exactly `key`. False for "none" and for I/O
  // failure; failed() tells them apart.
  bool find(const char* key, uint32_t& position, uint16_t& ordinal) {
    if (count == 0 || ioFailed) return false;
    uint32_t lo = 0;
    uint32_t hi = (static_cast<uint32_t>(count) + blockEntries - 1) / blockEntries;
    while (lo < hi) {
      const uint32_t mid = (lo + hi) / 2;
      if (memcmp(fence[mid].key, key, SORT_SEGMENT_BYTES) < 0)
        lo = mid + 1;
      else
        hi = mid;
    }
    // Equal keys can begin in the block before the first block that starts at
    // or after the key.
    for (uint32_t at = (lo == 0 ? 0 : lo - 1) * blockEntries; at < count; at++) {
      SortEntry entry{};
      if (!entryAt(at, entry)) return false;
      const int cmp = memcmp(entry.key, key, SORT_SEGMENT_BYTES);
      if (cmp < 0) continue;
      if (cmp > 0) return false;
      if (!claimed(at)) {
        position = at;
        ordinal = entry.ordinal;
        return true;
      }
    }
    return false;
  }
  bool entryAt(const uint32_t position, SortEntry& out) {
    const uint32_t wanted = position / blockEntries;
    if (wanted != loadedBlock) {
      const uint32_t first = wanted * blockEntries;
      const uint32_t entries = std::min<uint32_t>(blockEntries, count - first);
      const size_t bytes = entries * sizeof(SortEntry);
      if (!file.seekSet(static_cast<size_t>(first) * sizeof(SortEntry)) ||
          file.read(block.get(), bytes) != static_cast<int>(bytes)) {
        LOG_ERR("LIBIDX", "prior table read failed at %u", static_cast<unsigned>(first));
        ioFailed = true;
        return false;
      }
      loadedBlock = wanted;
    }
    out = block[position % blockEntries];
    return true;
  }
  void claim(const uint32_t position) {
    if (claimed(position)) return;
    claims[position / 8] |= static_cast<uint8_t>(1u << (position % 8));
    claimedEntries++;
  }
  bool claimed(const uint32_t position) const { return (claims[position / 8] & (1u << (position % 8))) != 0; }
  uint16_t size() const { return count; }
  uint16_t claimedCount() const { return claimedEntries; }
  bool failed() const { return ioFailed; }
  void close() {
    out.close();
    if (file) file.close();
    fence.reset();
    claims.reset();
    block.reset();
    count = 0;
    claimedEntries = 0;
    ioFailed = false;
  }
  ~PriorTable() { close(); }

 private:
  struct FenceKey {
    char key[SORT_SEGMENT_BYTES];
  };
  const char* path = nullptr;
  StreamWriter out;
  HalFile file;
  BookArray<FenceKey> fence;
  BookArray<uint8_t> claims;
  BookArray<SortEntry> block;
  uint32_t loadedBlock = UINT32_MAX;
  uint32_t written = 0;
  uint16_t count = 0;
  uint16_t blockEntries = 64;
  uint16_t claimedEntries = 0;
  bool ioFailed = false;
};

uint32_t fnv1a32(const char* data, const size_t len) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    hash ^= static_cast<unsigned char>(data[i]);
    hash *= 16777619u;
  }
  return hash;
}

// Sentinel written into a staged record whose book matched no previous path. A
// second pass decides whether it is a rename or genuinely new.
constexpr uint16_t FIRST_SEEN_UNRESOLVED = 0xFFFF;
// The arrival counter only grows. Rather than wrap into the sentinel, books
// past it share the last value and order by creation time and title.
constexpr uint16_t FIRST_SEEN_LAST = 0xFFFE;

uint16_t takeFirstSeen(uint16_t& next) {
  if (next < FIRST_SEEN_LAST) return next++;
  if (next == FIRST_SEEN_LAST) {
    LOG_INF("LIBIDX", "arrival counter exhausted; new books share the last arrival number");
    next = FIRST_SEEN_LAST + 1;
  }
  return FIRST_SEEN_LAST;
}

// State threaded through the recursive walk. Passed by reference rather than
// captured, so the walk stays a plain function and its stack frame stays small.
struct WalkState {
  HalFile stage;
  serialization::BufferedFileWriter* stageOut = nullptr;
  char* nameBuf = nullptr;
  StagedEntry* stagedEntry = nullptr;
  uint16_t books = 0;
  uint16_t folderId = 0;
  uint32_t folderBytes = 0;
  uint16_t nextFirstSeen = 0;
  uint16_t duplicatesDropped = 0;
  uint16_t unreadableSkipped = 0;
  uint64_t* dedupKeys = nullptr;
  uint16_t activeDedupCount = 0;
  bool dedupDegraded = false;
  bool failed = false;
  bool creationTimesUnchanged = true;
  bool readMetadata = false;
  LibraryIndexFile* previous = nullptr;
  // Parses kept across cancelled or failed builds; null when metadata is off.
  LibraryMetadataCache* metadataCache = nullptr;
  BuildStats* stats = nullptr;
  uint16_t enriched = 0;
  HalFile folders;  // folder section, staged separately then copied in
  // Books the previous index knew, by path hash. Null or empty on a first
  // build, in which case every book is new and gets a fresh firstSeen at once.
  PriorTable* prior = nullptr;
  uint16_t reused = 0;
  uint16_t unresolved = 0;  // matched no path; renamed or new, decided after the walk
  uint32_t serviceUnits = 0;
};

// Bound to shownTitle when a book told us nothing. A `std::string()` temporary
// in that ternary would copy the title on every book that DID tell us something,
// because the two branches have different value categories.
const std::string kNoTitle;

// Returns false for "no previous entry" and on failure; failure also sets st.failed.
bool findPrior(WalkState& st, const uint64_t pathHash, uint32_t& position, uint16_t& ordinal) {
  serviceBuilder(st.serviceUnits);
  if (!st.prior || st.prior->size() == 0) return false;
  char key[SORT_SEGMENT_BYTES] = {};
  putBigEndian(key, pathHash, sizeof(pathHash));
  if (st.prior->find(key, position, ordinal)) return true;
  if (st.prior->failed()) st.failed = true;
  return false;
}

// Nothing about a book's surroundings names its author: no directory-as-author
// inference lives here, only the filename-as-title fallback.
[[gnu::noinline]] bool stageRecord(WalkState& st, const std::string& name, const uint32_t fileSize,
                                   const uint16_t folderId, const std::string& fullPath, const uint32_t creationTime,
                                   const uint32_t modificationTime) {
  StagedEntry& entry = *st.stagedEntry;
  memset(&entry, 0, sizeof(entry));
  entry.seriesPosition = CLIX_UNKNOWN_SERIES_POSITION;
  // The filename is a fallback for the title and nothing else: no parsing, and
  // never an author. No other reader parses filenames, and a name pulled out
  // of one by pattern is a guess wearing a fact's clothes.
  std::string title = stemOf(name);
  std::string author;
  std::string series;
  std::string seriesIndex;
  std::string genre;
  bool titleFromBook = false;
  bool authorFromBook = false;

  entry.pathHash = clixPathHash(fullPath.data(), fullPath.size());
  entry.creationTime = creationTime;
  uint32_t priorPosition = 0;
  uint16_t priorOrdinal = 0;
  const bool priorFound = findPrior(st, entry.pathHash, priorPosition, priorOrdinal);
  if (st.failed) return false;

  const bool extractionExpected = st.readMetadata && FsHelpers::hasEpubExtension(name);
  const uint8_t expectedStatus = extractionExpected ? CLIX_METADATA_EXTRACTED : CLIX_METADATA_NOT_ATTEMPTED;
  bool reuseMetadata = false;
  ClixRecord priorRecord{};
  if (priorFound) {
    if (st.previous->header().formatVersion >= 5) {
      uint32_t priorCreationTime = 0;
      if (!st.previous->readCreationTime(priorOrdinal, priorCreationTime)) {
        st.failed = true;
        return false;
      }
      if (priorCreationTime != creationTime) st.creationTimesUnchanged = false;
    }
    if (!st.previous->readRecord(priorOrdinal, priorRecord)) {
      st.failed = true;
      return false;
    }
    reuseMetadata = priorRecord.fileSize == fileSize && modificationTime != 0 &&
                    priorRecord.modificationTime == modificationTime && st.previous->header().formatVersion >= 3 &&
                    st.previous->header().metadataEnabled == st.readMetadata &&
                    priorRecord.metadataStatus == expectedStatus &&
                    (!extractionExpected || st.previous->header().formatVersion >= 7);
  }
  // A fold update invalidates derived sort keys, not the stored book metadata.
  const bool reuseSortKeys = reuseMetadata && st.previous->header().foldVersion == CLIX_FOLD_VERSION;

  if (reuseMetadata) {
    if (!st.previous->readSourceAuthor(priorRecord, author)) {
      st.failed = true;
      return false;
    }
    if (!st.previous->readSeries(priorRecord, series) || !st.previous->readGenre(priorRecord, genre)) {
      st.failed = true;
      return false;
    }
    if (st.previous->header().formatVersion >= 6 &&
        !st.previous->readSeriesPosition(priorRecord, entry.seriesPosition)) {
      st.failed = true;
      return false;
    }
    const bool hasBookTitle = st.previous->readTitle(priorRecord, title);
    if (!hasBookTitle && st.previous->ioFailed()) {
      st.failed = true;
      return false;
    }
    entry.record = priorRecord;
    authorFromBook = !author.empty();
    if (hasBookTitle) {
      titleFromBook = true;
    } else {
      title = stemOf(name);
    }
    st.stats->metadataReused++;
  }

  // An EPUB that changed or is new gets a short OPF metadata read. The reader
  // cache does not contain series or genre, so only our own index can reuse
  // those fields without parsing the book again.
  CachedBookMetadata cached;
  const bool cacheHit = !reuseMetadata && extractionExpected && st.metadataCache &&
                        st.metadataCache->lookup(entry.pathHash, fileSize, modificationTime, cached);
  if (cacheHit) {
    // A hit was parsed from these exact bytes, after any stale reader cache
    // for them had already been cleared, so neither step repeats.
    st.stats->metadataCached++;
    entry.record.metadataStatus = CLIX_METADATA_EXTRACTED;
    entry.seriesPosition = cached.seriesPosition;
    if (!cached.title.empty()) {
      title = std::move(cached.title);
      titleFromBook = true;
    }
    author = std::move(cached.author);
    series = std::move(cached.series);
    genre = std::move(cached.genre);
    authorFromBook = !author.empty();
  }
  if (!reuseMetadata && extractionExpected && !cacheHit) {
    st.stats->parsed++;
    Epub epub(fullPath, CACHE_DIR);
    std::string bookTitle;
    // A missing timestamp cannot prove that a path-keyed EPUB cache still
    // belongs to this file, even when its byte length happens to match.
    const bool sourceChanged = modificationTime == 0 ||
                               (priorFound && (priorRecord.fileSize != fileSize || priorRecord.modificationTime == 0 ||
                                               priorRecord.modificationTime != modificationTime));
    if (sourceChanged && !clearBookCachePreservingUserState(fullPath, true)) {
      LOG_ERR("LIBIDX", "Cannot invalidate stale EPUB cache while preserving reading state: %s", fullPath.c_str());
      st.failed = true;
      return false;
    }
    if (epub.loadMetadata(bookTitle, author, !sourceChanged, &series, &genre, &seriesIndex)) {
      entry.record.metadataStatus = CLIX_METADATA_EXTRACTED;
      entry.seriesPosition = parseSeriesPosition(seriesIndex);
      // Failures are not cached: they may come from low memory or a card
      // glitch, and the next scan should simply try again.
      if (st.metadataCache) {
        cached.title = bookTitle;
        cached.author = author;
        cached.series = series;
        cached.genre = genre;
        cached.seriesPosition = entry.seriesPosition;
        st.metadataCache->store(entry.pathHash, fileSize, modificationTime, cached);
      }
      if (!bookTitle.empty()) {
        title = std::move(bookTitle);
        titleFromBook = true;
      }
      authorFromBook = !author.empty();
    } else {
      entry.record.metadataStatus = CLIX_METADATA_FAILED;
    }
    if (!titleFromBook && !authorFromBook) LOG_DBG("LIBIDX", "no metadata for %s", fullPath.c_str());
  }
  // Exporters write "Unknown" into dc:creator often enough that treating it as
  // a person would put a fictional author at the top of the shelf. fold() already
  // lowercases and trims, so recognising it is the one comparison this needs.
  if (!author.empty() && fold(author) == "unknown") {
    author.clear();
    authorFromBook = false;
  }

  if (titleFromBook || authorFromBook) st.enriched++;

  // An absent author is a fact, not a gap to fill: the row joins the Unknown
  // group rather than borrowing a name from its surroundings.
  const std::string folded = reuseSortKeys ? std::string() : fold(title);
  const std::string key = reuseSortKeys ? std::string() : authorKey(author);

  entry.record.fileSize = fileSize;
  entry.record.modificationTime = modificationTime;

  // Reuse the arrival order this book already had. Without this every rebuild
  // renumbers the whole library in disk-walk order, and "Recently added" silently
  // becomes "whatever order the card enumerates in".
  if (priorFound) {
    st.prior->claim(priorPosition);
    entry.record.firstSeen = priorRecord.firstSeen;
    st.reused++;
  } else if (!st.prior || st.prior->size() == 0) {
    // Nothing to have been renamed from, so the book is new.
    entry.record.firstSeen = takeFirstSeen(st.nextFirstSeen);
    st.stats->added++;
  } else {
    // Might be a rename rather than a new book; resolved after the walk, when
    // the set of genuinely unmatched previous entries is known.
    entry.record.firstSeen = FIRST_SEEN_UNRESOLVED;
    st.unresolved++;
  }
  entry.record.folderId = folderId;
  // In range: walk() skips names longer than STAGE_NAME_BYTES before staging.
  // readPath() rebuilds the file path from this slot, so a clamp here would
  // stage a row that renders but cannot open.
  entry.record.nameLen = static_cast<uint8_t>(name.size());
  // Only stored when the book actually told us something; otherwise the row falls
  // back to the filename and nothing is duplicated.
  const std::string& shownTitle = titleFromBook ? title : kNoTitle;
  entry.titleLen = static_cast<uint8_t>(utf8SafeTruncateBuffer(
      shownTitle.data(), static_cast<int>(std::min<size_t>(shownTitle.size(), STAGE_NAME_BYTES))));
  if (entry.titleLen > 0) memcpy(entry.title, shownTitle.data(), entry.titleLen);
  if (!reuseSortKeys) {
    const size_t foldBytes = std::min(folded.size(), CLIX_FOLD_BYTES);
    entry.record.foldLen = static_cast<uint8_t>(utf8SafeTruncateBuffer(folded.data(), static_cast<int>(foldBytes)));
    entry.record.authorKeyLen = static_cast<uint8_t>(std::min(key.size(), CLIX_AUTHOR_KEY_BYTES));
    memset(entry.record.fold, 0, sizeof(entry.record.fold));
    memset(entry.record.authorKey, 0, sizeof(entry.record.authorKey));
    memcpy(entry.record.fold, folded.data(), entry.record.foldLen);
    memcpy(entry.record.authorKey, key.data(), entry.record.authorKeyLen);
  }
  memcpy(entry.name, name.data(), entry.record.nameLen);

  const std::string displayAuthor = cleanPersonName(author);
  entry.authorLen = static_cast<uint8_t>(utf8SafeTruncateBuffer(
      displayAuthor.data(), static_cast<int>(std::min(displayAuthor.size(), STAGE_AUTHOR_BYTES))));
  memcpy(entry.author, displayAuthor.data(), entry.authorLen);
  entry.seriesLen = static_cast<uint8_t>(
      utf8SafeTruncateBuffer(series.data(), static_cast<int>(std::min(series.size(), STAGE_METADATA_BYTES))));
  if (entry.seriesLen > 0) memcpy(entry.series, series.data(), entry.seriesLen);
  if (entry.seriesLen == 0) entry.seriesPosition = CLIX_UNKNOWN_SERIES_POSITION;
  entry.genreLen = static_cast<uint8_t>(
      utf8SafeTruncateBuffer(genre.data(), static_cast<int>(std::min(genre.size(), STAGE_METADATA_BYTES))));
  if (entry.genreLen > 0) memcpy(entry.genre, genre.data(), entry.genreLen);

  st.stageOut->write(&entry, STAGE_STRIDE);
  st.books++;
  buildControl.progress.books = st.books;
  return true;
}

struct DedupFrame {
  WalkState& state;
  uint16_t base;
  ~DedupFrame() { state.activeDedupCount = base; }
};

void walk(WalkState& st, const std::string& path, const int depth) {
  if (st.failed || depth > LIBRARY_MAX_DEPTH || isLibraryResourceDirectory(path)) return;

  const uint16_t dedupBase = st.activeDedupCount;
  const DedupFrame dedupFrame{st, dedupBase};

  HalFile dir = Storage.open(path.c_str());
  if (!dir || !dir.isDirectory()) {
    LOG_ERR("LIBIDX", "cannot open library directory %s", path.c_str());
    if (dir) dir.close();
    st.failed = true;
    return;
  }
  dir.rewindDirectory();

  // Identities already staged from THIS directory. A damaged FAT can enumerate
  // the same entry twice; the second one would be a phantom book the user cannot
  // open.
  //
  // Hashes because the names do not fit. Two thousand books in one flat folder
  // is about 360 KB of std::string against a device that has under 200 KB free,
  // and std::vector grows by throwing, so the failure is abort() and a reboot
  // loop on every rebuild rather than a degraded scan. The one fixed buffer is
  // allocated fallibly by buildLibraryIndex(), reused for each directory, and
  // never grows.
  //
  // Keyed on (name hash, size) packed into 64 bits, not the hash alone. Two
  // different books colliding in 32 bits AND sharing a byte-exact size is
  // implausible where a bare hash collision is merely unlikely, and the cost of
  // being wrong is a real book silently missing from the shelf — the failure
  // hardest to notice and hardest to explain.
  bool folderEmitted = false;
  uint16_t myFolderId = 0;

  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    serviceBuilder(st.serviceUnits);
    if (buildCancelled()) st.failed = true;
    if (st.failed) {
      entry.close();
      break;
    }
    st.nameBuf[0] = '\0';
    entry.getName(st.nameBuf, NAME_BUF_SIZE);
    const bool isDir = entry.isDirectory();
    const uint32_t size = isDir ? 0 : static_cast<uint32_t>(entry.fileSize());
    const uint32_t creationTime = isDir ? 0 : entry.creationTime();
    const uint32_t modificationTime = isDir ? 0 : entry.modificationTime();
    entry.close();

    if (st.nameBuf[0] == '\0' || isHiddenOrSidecar(st.nameBuf) || (isDir && isSystemDirectory(st.nameBuf))) continue;
    const std::string name(st.nameBuf);

    if (isDir) {
      const size_t resumePosition = dir.position();
      dir.close();
      walk(st, joinLibraryPath(path, name), depth + 1);
      if (st.failed) return;

      dir = Storage.open(path.c_str());
      if (!dir || !dir.isDirectory() || !dir.seekSet(resumePosition)) {
        if (dir) dir.close();
        LOG_ERR("LIBIDX", "cannot resume directory %s at %u", path.c_str(), static_cast<unsigned>(resumePosition));
        st.failed = true;
        return;
      }
      continue;
    }
    if (!isBookName(name)) continue;

    // A zero-length book is a dangling directory entry: the name enumerates but
    // the contents do not exist. Counted rather than silently dropped.
    if (size == 0) {
      st.unreadableSkipped++;
      continue;
    }
    // The index stores the name and the folder path behind one length byte
    // each, and readPath() reconstructs "<folder>/<name>" from those bytes. An
    // entry that does not fit is skipped and counted, never clamped: a clamped
    // name still renders on the shelf but reconstructs to a path that cannot
    // open, and a byte-level cut is not even valid UTF-8. The limit is real —
    // FAT allows 255 UTF-16 units, so a long Cyrillic or CJK filename can run
    // to ~765 UTF-8 bytes.
    if (name.size() > STAGE_NAME_BYTES || path.size() > FOLDER_PATH_BYTES) {
      st.unreadableSkipped++;
      continue;
    }
    const uint64_t key = (static_cast<uint64_t>(fnv1a32(name.data(), name.size())) << 32) | size;
    if (st.dedupKeys != nullptr) {
      uint64_t* const end = st.dedupKeys + st.activeDedupCount;
      uint64_t* const slot = std::lower_bound(st.dedupKeys + dedupBase, end, key);
      if (slot != end && *slot == key) {
        st.duplicatesDropped++;
        continue;
      }
      if (st.activeDedupCount < LIBRARY_MAX_DEDUP_KEYS) {
        memmove(slot + 1, slot, static_cast<size_t>(end - slot) * sizeof(*slot));
        *slot = key;
        st.activeDedupCount++;
      } else if (!st.dedupDegraded) {
        LOG_INF("LIBIDX", "duplicate detection capped at %u entries in %s",
                static_cast<unsigned>(LIBRARY_MAX_DEDUP_KEYS), path.c_str());
        st.dedupDegraded = true;
      }
    }
    if (st.books >= libraryBookLimit()) {
      LOG_ERR("LIBIDX", "library exceeds the %u-book index limit; keeping the previous index",
              static_cast<unsigned>(libraryBookLimit()));
      st.stats->failure = BuildFailure::TooManyBooks;
      st.failed = true;
      break;
    }
    if (!folderEmitted) {
      // Folders are emitted lazily, so only directories that actually hold a
      // book get an id and the ids stay dense.
      myFolderId = st.folderId;
      // In range: entries whose folder path exceeds FOLDER_PATH_BYTES were
      // skipped above, so no book reaches this line with an overlong path.
      const uint8_t pathLen = static_cast<uint8_t>(path.size());
      if (st.folders.write(&pathLen, 1) != 1 ||
          st.folders.write(reinterpret_cast<const uint8_t*>(path.data()), pathLen) != pathLen) {
        LOG_ERR("LIBIDX", "folder stage write failed: %s", path.c_str());
        st.failed = true;
        break;
      }
      st.folderBytes += 1u + pathLen;
      st.folderId++;
      folderEmitted = true;
    }
    if (!stageRecord(st, name, size, myFolderId, joinLibraryPath(path, name), creationTime, modificationTime)) break;
  }
  // openNextFile() returning falsy is ambiguous between "reached the end of
  // the directory" and an SdFat allocation/iteration error partway through.
  // Left unchecked, a card glitch mid-listing looks identical to a folder
  // that was fully scanned: books after the failure point silently never
  // reach the index, with nothing to force a retry on the next rebuild.
  if (!st.failed && FsHelpers::directoryIterationFailed(dir)) {
    LOG_ERR("LIBIDX", "directory listing failed before EOF: %s", path.c_str());
    st.failed = true;
  }
  dir.close();
}

// Shared by the offset and write passes so the name-blob layout has one source of
// truth.
uint32_t blobBytesFor(const StagedEntry& entry, const uint8_t canonicalAuthorLen) {
  return sizeof(entry.pathHash) + entry.record.nameLen + 1u + canonicalAuthorLen + 1u + entry.titleLen + 1u +
         entry.authorLen + 1u + entry.seriesLen + 1u + entry.genreLen + sizeof(entry.seriesPosition);
}

uint64_t stageOffset(const uint16_t index) { return static_cast<uint64_t>(index) * STAGE_STRIDE; }

// Random access to staged books. Every read seeks first, so the sort key
// loaders and the passes that drive them share the single handle real
// hardware allows per file.
class StageReader {
 public:
  bool open() {
    if (Storage.openFileForRead("LIBIDX", STAGE_PATH, file)) return true;
    failed = true;
    return false;
  }
  // A short read leaves the previous book's bytes in the buffer, and a
  // duplicate row is internally consistent enough to pass validation, so any
  // failure latches and fails the emit.
  bool read(const uint64_t offset, void* data, const size_t len) {
    if (buildCancelled()) failed = true;
    if (failed) return false;
    if (!file.seekSet(offset) || file.read(data, len) != static_cast<int>(len)) {
      LOG_ERR("LIBIDX", "record stage read failed at %u", static_cast<unsigned>(offset));
      failed = true;
      return false;
    }
    return true;
  }
  bool entry(const uint16_t index, StagedEntry& out) { return read(stageOffset(index), &out, STAGE_STRIDE); }
  bool record(const uint16_t index, ClixRecord& out) {
    return read(stageOffset(index) + offsetof(StagedEntry, record), &out, sizeof(out));
  }
  // Length-prefixed staged text of at most `capacity` bytes.
  bool text(const uint16_t index, const size_t lengthOffset, const size_t textOffset, const size_t capacity,
            std::string& out) {
    uint8_t len = 0;
    if (!read(stageOffset(index) + lengthOffset, &len, sizeof(len))) return false;
    if (len > capacity) {
      LOG_ERR("LIBIDX", "invalid staged text length: %u", static_cast<unsigned>(len));
      failed = true;
      return false;
    }
    out.resize(len);
    return len == 0 || read(stageOffset(index) + textOffset, &out[0], len);
  }
  bool author(const uint16_t index, std::string& out) {
    return text(index, offsetof(StagedEntry, authorLen), offsetof(StagedEntry, author), STAGE_AUTHOR_BYTES, out);
  }
  bool isFailed() const { return failed; }
  ~StageReader() {
    if (file) file.close();
  }

 private:
  HalFile file;
  bool failed = false;
};

// Context for the key loaders. The strings are reserved once and reused for
// every book instead of allocating per comparison.
struct KeyLoader {
  StageReader* stage = nullptr;
  std::string text;
  std::string key;
  size_t lengthOffset = 0;
  size_t textOffset = 0;
  bool seriesOrder = false;
};

bool loadTitleSegment(void* context, const uint16_t source, const size_t offset, char* segment, size_t& valueBytes) {
  auto& loader = *static_cast<KeyLoader*>(context);
  const uint64_t record = stageOffset(source) + offsetof(StagedEntry, record);
  uint8_t foldLen = 0;
  if (!loader.stage->read(record + offsetof(ClixRecord, foldLen), &foldLen, sizeof(foldLen))) return false;
  valueBytes = foldLen;
  return loader.stage->read(record + offsetof(ClixRecord, fold) + offset, segment, SORT_SEGMENT_BYTES);
}

// The shelf is ordered by surname of the canonical spelling, so `source` is
// the stage index of the book whose author spelling won the vote.
bool loadSurnameSegment(void* context, const uint16_t source, const size_t offset, char* segment, size_t& valueBytes) {
  auto& loader = *static_cast<KeyLoader*>(context);
  if (!loader.stage->author(source, loader.text)) return false;
  loader.key = loader.text.empty() ? std::string() : surnameKey(loader.text);
  writeKeySegment(loader.key, offset, segment);
  valueBytes = loader.key.size();
  return true;
}

bool loadFirstNameSegment(void* context, const uint16_t source, const size_t offset, char* segment,
                          size_t& valueBytes) {
  auto& loader = *static_cast<KeyLoader*>(context);
  if (!loader.stage->author(source, loader.text)) return false;
  foldInto(loader.text, loader.key);
  writeKeySegment(loader.key, offset, segment);
  valueBytes = loader.key.size();
  return true;
}

// Series and genre: the folded name, and for series a NUL then the big-endian
// position, so books sort by name, then number, then title order.
bool loadMetadataSegment(void* context, const uint16_t source, const size_t offset, char* segment, size_t& valueBytes) {
  auto& loader = *static_cast<KeyLoader*>(context);
  if (!loader.stage->text(source, loader.lengthOffset, loader.textOffset, STAGE_METADATA_BYTES, loader.text))
    return false;
  foldInto(loader.text, loader.key);
  if (!loader.key.empty() && loader.seriesOrder) {
    uint32_t position = CLIX_UNKNOWN_SERIES_POSITION;
    if (!loader.stage->read(stageOffset(source) + offsetof(StagedEntry, seriesPosition), &position, sizeof(position)))
      return false;
    const char suffix[] = {0, static_cast<char>(position >> 24), static_cast<char>(position >> 16),
                           static_cast<char>(position >> 8), static_cast<char>(position)};
    loader.key.append(suffix, sizeof(suffix));
  }
  writeKeySegment(loader.key, offset, segment);
  valueBytes = loader.key.size();
  return true;
}

// Sorted entries written straight into the index as u16 ordinals.
struct IndexSink {
  serialization::BufferedFileWriter* out;
  bool* ioFailed;
  static bool putOrdinal(void* context, const SortEntry& entry) {
    auto& sink = *static_cast<IndexSink*>(context);
    if (buildCancelled()) *sink.ioFailed = true;
    if (*sink.ioFailed) return false;
    sink.out->write(&entry.ordinal, sizeof(entry.ordinal));
    return true;
  }
};

// --- one spelling per person ----------------------------------------------
//
// The author KEY already merges "Xun, Lu", "Lu Xun_" and
// "Lu Xun [Xun, Lu]" into one identity, because its tokens are
// sorted. The displayed STRING is still whatever each filename happened to
// carry, so one person appears under several spellings in the same list.
//
// Fix: within each key group show the spelling that occurs most often, ties
// broken by the shortest and then alphabetically. It never invents or reorders
// a name — it picks one of the strings that actually exist — which is what
// keeps "Lu Xun" and "Natsume Soseki" safe from a forename/surname rule
// that would confidently get them backwards.
//
// Books are sorted by author key into a scratch file so each person's books
// are contiguous; a group is read once to count spellings and once more to
// record the winner. The result is CANON_PATH: for each title position, the
// stage index of the book whose spelling that row displays and sorts by.
bool writeCanonicalAuthors(StageReader& stage, const uint16_t n, BuildStats& stats, uint32_t& serviceUnits) {
  {
    ExternalSorter sorter;
    OrderReader titles;
    StreamWriter authors;
    if (!sorter.begin(sortConfig(), n) || !titles.open(ORDER_PATH) || !authors.open(AUTHORS_PATH)) return false;
    for (uint16_t t = 0; t < n; t++) {
      serviceBuilder(serviceUnits);
      uint16_t s = 0;
      ClixRecord r{};
      if (!titles.next(s) || !stage.record(s, r)) return false;
      SortEntry entry{};
      if (r.authorKeyLen == 0) {
        // 0xFF outranks every folded byte, so unknown authors land at the end.
        memset(entry.key, 0xFF, sizeof(entry.key));
      } else {
        memcpy(entry.key, r.authorKey, std::min<size_t>(r.authorKeyLen, sizeof(entry.key)));
      }
      entry.ordinal = t;
      entry.source = s;
      if (!sorter.add(entry)) return false;
    }
    if (!sorter.finish(&StreamWriter::writeEntry, &authors) || !authors.close()) return false;
  }

  auto spellings = makeUniqueNoThrow<SpellingSlot[]>(MAX_AUTHOR_SPELLINGS);
  if (!spellings) {
    LOG_ERR("LIBIDX", "author spelling scratch alloc failed; spelling harmonisation skipped");
    stats.ranksDegraded = true;
  }
  ExternalSorter canon;
  HalFile authorsFile;
  if (!canon.begin(sortConfig(), n) || !Storage.openFileForRead("LIBIDX", AUTHORS_PATH, authorsFile)) return false;
  bool ok = true;
  {
    serialization::BufferedFileReader authors(authorsFile, SCRATCH_IO_BUFFER_SIZE);
    const auto next = [&authors](SortEntry& out) { return authors.read(&out, sizeof(out)) == sizeof(out); };
    std::string spelling;
    spelling.reserve(STAGE_AUTHOR_BYTES);
    uint32_t runStart = 0;
    while (ok && runStart < n) {
      SortEntry first{};
      if (!authors.seek(runStart * sizeof(SortEntry)) || !next(first)) {
        ok = false;
        break;
      }
      uint32_t runEnd = runStart + 1;
      for (SortEntry entry{}; runEnd < n; runEnd++) {
        serviceBuilder(serviceUnits);
        if (!next(entry)) {
          ok = false;
          break;
        }
        if (memcmp(entry.key, first.key, sizeof(first.key)) != 0) break;
      }
      // A run of one has nothing to reconcile, and the unknown-author run (key
      // all 0xFF) must not be collapsed onto one arbitrary empty string.
      const bool vote = ok && spellings && runEnd - runStart > 1 && static_cast<uint8_t>(first.key[0]) != 0xFF;
      uint16_t best = first.source;
      if (vote) {
        // Bounded by DISTINCT spellings rather than by run length: one person
        // has two or three spellings on a real card, however many books they
        // wrote. Beyond sixteen the vote decides among the first sixteen.
        uint8_t spellingCount = 0;
        ok = authors.seek(runStart * sizeof(SortEntry));
        for (uint32_t a = runStart; ok && a < runEnd; a++) {
          serviceBuilder(serviceUnits);
          SortEntry entry{};
          ok = next(entry) && stage.author(entry.source, spelling);
          if (!ok || spelling.empty()) continue;
          bool merged = false;
          for (uint8_t i = 0; i < spellingCount; i++) {
            SpellingSlot& slot = spellings[i];
            if (slot.len == spelling.size() && memcmp(slot.text, spelling.data(), spelling.size()) == 0) {
              slot.count++;
              merged = true;
              break;
            }
          }
          if (!merged && spellingCount < MAX_AUTHOR_SPELLINGS) {
            SpellingSlot& slot = spellings[spellingCount++];
            memcpy(slot.text, spelling.data(), spelling.size());
            slot.source = entry.source;
            slot.count = 1;
            slot.len = static_cast<uint8_t>(spelling.size());
          }
        }
        int bestScore = -1;
        size_t bestLen = 0;
        const char* bestText = nullptr;
        for (uint8_t i = 0; ok && i < spellingCount; i++) {
          const SpellingSlot& slot = spellings[i];
          const bool better = slot.count > bestScore || (slot.count == bestScore && slot.len < bestLen) ||
                              (slot.count == bestScore && slot.len == bestLen &&
                               (bestText == nullptr || memcmp(slot.text, bestText, slot.len) < 0));
          if (better) {
            bestScore = slot.count;
            bestLen = slot.len;
            bestText = slot.text;
            best = slot.source;
          }
        }
      }
      ok = ok && authors.seek(runStart * sizeof(SortEntry));
      for (uint32_t a = runStart; ok && a < runEnd; a++) {
        serviceBuilder(serviceUnits);
        SortEntry entry{};
        if (!next(entry)) {
          ok = false;
          break;
        }
        SortEntry mapping{};
        putBigEndian(mapping.key, entry.ordinal, sizeof(entry.ordinal));
        mapping.ordinal = entry.ordinal;
        mapping.source = vote ? best : entry.source;
        ok = canon.add(mapping);
      }
      runStart = runEnd;
    }
  }
  authorsFile.close();
  if (!ok) {
    LOG_ERR("LIBIDX", "author spelling pass failed");
    return false;
  }
  StreamWriter canonOut;
  return canonOut.open(CANON_PATH) && canon.finish(&StreamWriter::writeSource, &canonOut) && canonOut.close();
}

bool emitIndex(const char* folderStagePath, WalkState& st, BuildStats& stats) {
  const uint16_t n = st.books;
  uint32_t serviceUnits = 0;

  ClixHeader header{};
  memcpy(header.magic, CLIX_MAGIC, sizeof(CLIX_MAGIC));
  header.formatVersion = CLIX_FORMAT_VERSION;
  header.foldVersion = CLIX_FOLD_VERSION;
  header.bookCount = n;
  header.folderCount = st.folderId;
  header.nextFirstSeen = st.nextFirstSeen > FIRST_SEEN_LAST ? FIRST_SEEN_LAST : st.nextFirstSeen;
  header.metadataEnabled = st.readMetadata;
  // Placeholder only. Degradations are known after the sorts have run.
  header.flags = 0;
  // The blob is the LAST section, so its size affects only selfSize — every
  // section offset is already fixed by the counts. Lay out with a placeholder
  // and correct selfSize once the blob has actually been written, since the
  // author spelling each record ends up carrying is not known until the
  // one-spelling-per-person pass has run.
  layoutSections(header, st.folderBytes, 0);

  StageReader stage;
  if (!stage.open()) return false;

  // The spelling vote runs before the output opens: the records need each
  // canonical author's length for their blob offsets.
  if (!writeCanonicalAuthors(stage, n, stats, serviceUnits)) return false;

  HalFile out;
  if (!Storage.openFileForWrite("LIBIDX", NEW_PATH, out)) return false;
  serialization::BufferedFileWriter outBuffer(out, LIBRARY_IO_BUFFER_SIZE);

  // Returns false rather than spinning. A full card makes write() return 0, and
  // a naive loop never advances past it — the device would simply hang mid-rebuild
  // with no message, which is worse than any error.
  bool ioFailed = false;
  // Every final-index write goes through here. A short write on a full card
  // leaves a file that still passes the header check when the header describes
  // what was intended rather than what landed.
  const auto put = [&outBuffer, &ioFailed](const void* data, const size_t len) {
    if (buildCancelled()) ioFailed = true;
    if (ioFailed) return;
    outBuffer.write(data, len);
  };
  const auto padTo = [&outBuffer, &ioFailed, &serviceUnits](const uint32_t target) {
    if (ioFailed) return;
    static const uint8_t zeros[64] = {0};
    while (outBuffer.position() < target) {
      serviceBuilder(serviceUnits);
      const uint32_t gap = target - static_cast<uint32_t>(outBuffer.position());
      const size_t want = std::min<uint32_t>(gap, sizeof(zeros));
      outBuffer.write(zeros, want);
    }
  };
  const auto fail = [&outBuffer, &out]([[maybe_unused]] const char* phase) {
    LOG_ERR("LIBIDX", "emit failed while %s", phase);
    outBuffer.flush();
    out.close();
    Storage.remove(NEW_PATH);
    return false;
  };

  // Header placeholder; rewritten below once the sorts have run.
  put(&header, sizeof(header));
  padTo(header.folderStart);

  {
    HalFile folders;
    if (Storage.openFileForRead("LIBIDX", folderStagePath, folders)) {
      uint8_t buf[256];
      uint32_t copied = 0;
      // read() returns int: a -1 error must fail the emit, not wrap into a
      // huge unsigned length.
      int got = 0;
      while ((got = folders.read(buf, sizeof(buf))) > 0) {
        serviceBuilder(serviceUnits);
        put(buf, static_cast<size_t>(got));
        copied += static_cast<uint32_t>(got);
      }
      if (got < 0) ioFailed = true;
      folders.close();
      if (copied != st.folderBytes) {
        LOG_ERR("LIBIDX", "folder stage truncated: read %u of %u bytes", static_cast<unsigned>(copied),
                static_cast<unsigned>(st.folderBytes));
        ioFailed = true;
      }
    } else {
      // Ignoring this would publish an all-zero folder section: selfSize still
      // matches, so the index validates, and readPath() then fails for every
      // book with nothing left to trigger a self-repair.
      LOG_ERR("LIBIDX", "folder stage unreadable: %s", folderStagePath);
      ioFailed = true;
    }
  }
  padTo(header.recordStart);
  if (ioFailed) return fail("copying the folder stage");

  // One pair of staging buffers on the heap, reused by the record and name
  // passes, rather than 2 KB of locals on a 4 KB task stack. On the heap the
  // allocation is checked; on the stack an overflow is a silent corruption.
  auto staged = makeUniqueNoThrow<StagedEntry[]>(2);
  if (!staged) return fail("allocating staging buffers");
  StagedEntry& entry = staged[0];
  StagedEntry& canonical = staged[1];

  // Records, in title order. Name offsets follow the same order as the blob.
  {
    OrderReader titles;
    OrderReader canon;
    if (!titles.open(ORDER_PATH) || !canon.open(CANON_PATH)) return fail("opening the title order");
    uint32_t nameCursor = 0;
    for (uint16_t t = 0; t < n && !ioFailed; t++) {
      serviceBuilder(serviceUnits);
      uint16_t s = 0;
      uint16_t sc = 0;
      uint8_t canonicalAuthorLen = 0;
      if (!titles.next(s) || !canon.next(sc) || !stage.entry(s, entry) ||
          !stage.read(stageOffset(sc) + offsetof(StagedEntry, authorLen), &canonicalAuthorLen,
                      sizeof(canonicalAuthorLen))) {
        ioFailed = true;
        break;
      }
      entry.record.nameOff = nameCursor;
      nameCursor += blobBytesFor(entry, canonicalAuthorLen);
      put(&entry.record, sizeof(ClixRecord));
    }
  }
  padTo(header.permStart);
  if (ioFailed) return fail("writing records");

  // One permutation: a sorted entry per title position, keyed by makeKey, its
  // ordinals written straight into the index as the merge produces them.
  KeyLoader loader;
  loader.stage = &stage;
  loader.text.reserve(STAGE_NAME_BYTES);
  loader.key.reserve(STAGE_METADATA_BYTES * 2);
  IndexSink sink{&outBuffer, &ioFailed};
  const auto permutation = [&](const SortConfig& config, const bool needCanonical, auto&& makeKey) {
    ExternalSorter sorter;
    OrderReader titles;
    OrderReader canon;
    if (!sorter.begin(config, n) || !titles.open(ORDER_PATH) || (needCanonical && !canon.open(CANON_PATH)))
      return false;
    for (uint16_t t = 0; t < n; t++) {
      serviceBuilder(serviceUnits);
      uint16_t s = 0;
      uint16_t sc = 0;
      SortEntry key{};
      if (!titles.next(s) || (needCanonical && !canon.next(sc)) || !makeKey(t, s, sc, key)) return false;
      key.ordinal = t;
      if (!sorter.add(key)) return false;
    }
    titles.close();
    canon.close();
    return sorter.finish(&IndexSink::putOrdinal, &sink) && !ioFailed;
  };

  // --- surname order ---------------------------------------------------------
  //
  // The vote had to run in authorKey order, because that is what puts one
  // author's books in a single run. But authorKey sorts a name's WORDS — the
  // property that lets "Victor Hugo" and "Hugo Victor" be recognised as one
  // person — so ordering by it files Herman Melville under B.
  //
  // Now that every book carries its canonical display name, the shelf is ordered
  // by surname, as a library would. Keying off the canonical name rather than the
  // raw one is what keeps a group whole: all of a group's books resolve to the
  // same string, so they cannot split across two places.
  if (!permutation(sortConfig(STAGE_AUTHOR_BYTES, &loadSurnameSegment, &loader), true,
                   [&](uint16_t, uint16_t, const uint16_t sc, SortEntry& key) {
                     key.source = sc;
                     size_t valueBytes = 0;
                     return loadSurnameSegment(&loader, sc, 0, key.key, valueBytes);
                   }))
    return fail("sorting by surname");
  // Both author orders use the canonical display name.
  if (!permutation(sortConfig(STAGE_AUTHOR_BYTES, &loadFirstNameSegment, &loader), true,
                   [&](uint16_t, uint16_t, const uint16_t sc, SortEntry& key) {
                     key.source = sc;
                     size_t valueBytes = 0;
                     return loadFirstNameSegment(&loader, sc, 0, key.key, valueBytes);
                   }))
    return fail("sorting by first name");

  // --- arrival order -------------------------------------------------------
  //
  // Primary key is the file's creation time. firstSeen breaks ties and orders
  // books without a timestamp. It comes from the PREVIOUS index, so it is no longer a
  // dense sequence in walk order — a rebuild reuses each book's original number
  // and only hands out new ones for books it has never seen. The arrival order
  // has to be SORTED rather than assumed, or "Recent" silently degrades into
  // "the order the card enumerates in" — exactly the bug reconciliation exists
  // to prevent. Title position completes the key, so it never needs refining.
  if (!permutation(sortConfig(), false, [&](const uint16_t t, const uint16_t s, uint16_t, SortEntry& key) {
        uint32_t creationTime = 0;
        uint16_t firstSeen = 0;
        if (!stage.read(stageOffset(s) + offsetof(StagedEntry, creationTime), &creationTime, sizeof(creationTime)) ||
            !stage.read(stageOffset(s) + offsetof(StagedEntry, record) + offsetof(ClixRecord, firstSeen), &firstSeen,
                        sizeof(firstSeen)))
          return false;
        putBigEndian(key.key, creationTime, sizeof(creationTime));
        putBigEndian(key.key + 4, firstSeen, sizeof(firstSeen));
        putBigEndian(key.key + 6, t, sizeof(t));
        key.source = s;
        return true;
      }))
    return fail("sorting by arrival");

  // Metadata orders. Only the 12-byte key prefix stays resident; full strings
  // come from staging only when prefixes tie.
  const auto metadataOrder = [&](const size_t lengthOffset, const size_t textOffset, const bool seriesOrder) {
    loader.lengthOffset = lengthOffset;
    loader.textOffset = textOffset;
    loader.seriesOrder = seriesOrder;
    return permutation(sortConfig(STAGE_METADATA_BYTES * 4u + (seriesOrder ? 5u : 0u), &loadMetadataSegment, &loader),
                       false, [&](uint16_t, const uint16_t s, uint16_t, SortEntry& key) {
                         key.source = s;
                         size_t valueBytes = 0;
                         return loadMetadataSegment(&loader, s, 0, key.key, valueBytes);
                       });
  };
  if (!metadataOrder(offsetof(StagedEntry, seriesLen), offsetof(StagedEntry, series), true))
    return fail("sorting by series");
  if (!metadataOrder(offsetof(StagedEntry, genreLen), offsetof(StagedEntry, genre), false))
    return fail("sorting by genre");

  // One timestamp per title-ordered record; no change to the 128-byte record.
  {
    OrderReader titles;
    if (!titles.open(ORDER_PATH)) return fail("opening the title order");
    for (uint16_t t = 0; t < n && !ioFailed; t++) {
      serviceBuilder(serviceUnits);
      uint16_t s = 0;
      uint32_t creationTime = 0;
      if (!titles.next(s) ||
          !stage.read(stageOffset(s) + offsetof(StagedEntry, creationTime), &creationTime, sizeof(creationTime))) {
        ioFailed = true;
        break;
      }
      put(&creationTime, sizeof(creationTime));
    }
  }
  padTo(header.nameStart);
  if (ioFailed) return fail("writing creation times");

  // The blob holds the path hash, basename, chosen author spelling, title, and
  // source author spelling used by later rebuilds. Keeping them adjacent means
  // no second offset has to live in the record.
  uint32_t blobWritten = 0;
  {
    OrderReader titles;
    OrderReader canon;
    if (!titles.open(ORDER_PATH) || !canon.open(CANON_PATH)) return fail("opening the title order");
    for (uint16_t t = 0; t < n && !ioFailed; t++) {
      serviceBuilder(serviceUnits);
      uint16_t s = 0;
      uint16_t sc = 0;
      if (!titles.next(s) || !canon.next(sc) || !stage.entry(s, entry) || !stage.entry(sc, canonical)) {
        ioFailed = true;
        break;
      }
      put(&entry.pathHash, sizeof(entry.pathHash));
      put(entry.name, entry.record.nameLen);
      put(&canonical.authorLen, 1);
      if (canonical.authorLen > 0) put(canonical.author, canonical.authorLen);
      put(&entry.titleLen, 1);
      if (entry.titleLen > 0) put(entry.title, entry.titleLen);
      put(&entry.authorLen, 1);
      if (entry.authorLen > 0) put(entry.author, entry.authorLen);
      put(&entry.seriesLen, 1);
      if (entry.seriesLen > 0) put(entry.series, entry.seriesLen);
      put(&entry.genreLen, 1);
      if (entry.genreLen > 0) put(entry.genre, entry.genreLen);
      put(&entry.seriesPosition, sizeof(entry.seriesPosition));
      blobWritten += blobBytesFor(entry, canonical.authorLen);
    }
  }
  if (ioFailed || stage.isFailed()) return fail("writing names");
  header.nameLen = blobWritten;
  header.selfSize = header.nameStart + blobWritten;

  // Captured HERE, at the end of the data, and not after the header rewrite
  // below: that rewrite seeks back to 0, so asking afterwards reports 64 — the
  // header's own length — and every rebuild looks truncated.
  const uint32_t written = static_cast<uint32_t>(outBuffer.position());
  if (!outBuffer.flush()) ioFailed = true;

  // Arrival order no longer has a fallback to degrade into; the flag stays
  // defined so older indexes still validate.
  stats.arrivalDegraded = false;
  header.flags =
      (stats.ranksDegraded ? CLIX_FLAG_RANKS_DEGRADED : 0) | (stats.dedupDegraded ? CLIX_FLAG_DEDUP_DEGRADED : 0);

  if (!out.seekSet(0)) {
    ioFailed = true;
  } else if (out.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) != sizeof(header))
    ioFailed = true;
  // The file is only as long as it claims if every write landed. A full card
  // fails them silently, and the result passes the header check while carrying
  // zeros — an index that looks valid and is not.
  const bool sizeMatches = written == header.selfSize;
  const bool closed = out.close();

  if (ioFailed || !sizeMatches || !closed) {
    LOG_ERR("LIBIDX", "emit incomplete (I/O %s, close %s, size %u vs %u) — keeping the old index",
            ioFailed ? "failed" : "ok", closed ? "ok" : "failed", static_cast<unsigned>(written),
            static_cast<unsigned>(header.selfSize));
    // Leave the previous index alone. A shelf that is a rebuild out of date is
    // worth incomparably more than none at all, and a full card is exactly when
    // the reader can least afford to lose it.
    if (closed) Storage.remove(NEW_PATH);
    return false;
  }

  // Rename last. The previous index moves to a recoverable backup until the new
  // file owns the live path; a failed rename rolls it back instead of deleting
  // the only usable shelf.
  return installNewIndex();
}

// Write the previous index's books, sorted by complete-path hash, so the walk
// can recognise the same books without holding them in RAM.
bool buildPriorByPath(LibraryIndexFile& previous, PriorTable& table) {
  const uint16_t count = previous.bookCount();
  uint32_t serviceUnits = 0;
  ExternalSorter sorter;
  if (!table.beginWrite(PRIOR_PATH, count) || !sorter.begin(sortConfig(), count)) return false;
  for (uint16_t i = 0; i < count; i++) {
    serviceBuilder(serviceUnits);
    if (buildCancelled()) return false;
    ClixRecord r{};
    uint64_t pathHash = 0;
    if (!previous.readRecord(i, r) || !previous.readPathHash(r, pathHash)) {
      LOG_ERR("LIBIDX", "prior index read failed at record %u", static_cast<unsigned>(i));
      return false;
    }
    SortEntry entry{};
    putBigEndian(entry.key, pathHash, sizeof(pathHash));
    entry.ordinal = i;
    if (!sorter.add(entry)) return false;
  }
  return sorter.finish(&PriorTable::onSorted, &table) && table.endWrite();
}

// --- second pass: renames, then genuinely new books ----------------------
//
// A book that matched no previous path is either renamed or new. Match it
// against the leftover previous entries by SIZE alone: across a real library,
// two different books sharing a byte-exact size is implausible, and being wrong
// only costs one book its place in "Recently added" and one re-read. A content
// hash would settle it properly but would read ~12 KB per book on every single
// verification, to decide a case that arises when someone renames a file.
//
// Leftovers are sorted by (size, previous ordinal) and staged books are visited
// in walk order, so each takes the lowest-numbered unclaimed previous book of
// its size. The resolved firstSeen is patched into the staged record in place;
// only unresolved books are touched.
bool resolveUnmatchedBooks(WalkState& st, LibraryIndexFile& previous, PriorTable& byPath, BuildStats& stats) {
  uint32_t serviceUnits = 0;
  PriorTable bySize;
  const uint16_t leftovers = byPath.size() - byPath.claimedCount();
  if (leftovers > 0) {
    ExternalSorter sorter;
    if (!bySize.beginWrite(RENAME_PATH, leftovers) || !sorter.begin(sortConfig(), leftovers)) return false;
    for (uint32_t position = 0; position < byPath.size(); position++) {
      serviceBuilder(serviceUnits);
      if (byPath.claimed(position)) continue;
      SortEntry entry{};
      ClixRecord r{};
      if (!byPath.entryAt(position, entry) || !previous.readRecord(entry.ordinal, r)) return false;
      SortEntry bySizeEntry{};
      putBigEndian(bySizeEntry.key, r.fileSize, sizeof(r.fileSize));
      bySizeEntry.ordinal = entry.ordinal;
      if (!sorter.add(bySizeEntry)) return false;
    }
    if (!sorter.finish(&PriorTable::onSorted, &bySize) || !bySize.endWrite()) return false;
  }

  // A stage that cannot be patched fails the BUILD, it does not degrade. The
  // fallback would be a wrong firstSeen read back as prior truth by the next
  // rebuild, which would then propagate it forever. The previous index survives.
  HalFile stage = Storage.open(STAGE_PATH, O_RDWR);
  if (!stage) {
    LOG_ERR("LIBIDX", "firstSeen reconciliation: cannot reopen the stage");
    return false;
  }
  bool ok = true;
  uint16_t renamed = 0;
  for (uint16_t i = 0; ok && i < st.books; i++) {
    serviceBuilder(serviceUnits);
    const uint64_t recordAt = stageOffset(i) + offsetof(StagedEntry, record);
    ClixRecord r{};
    if (buildCancelled() || !stage.seekSet(recordAt) ||
        stage.read(reinterpret_cast<uint8_t*>(&r), sizeof(r)) != static_cast<int>(sizeof(r))) {
      LOG_ERR("LIBIDX", "firstSeen reconciliation: short read at record %u", static_cast<unsigned>(i));
      ok = false;
      break;
    }
    if (r.firstSeen != FIRST_SEEN_UNRESOLVED) continue;
    uint16_t firstSeen = 0;
    char key[SORT_SEGMENT_BYTES] = {};
    putBigEndian(key, r.fileSize, sizeof(r.fileSize));
    uint32_t position = 0;
    uint16_t ordinal = 0;
    ClixRecord prior{};
    if (leftovers > 0 && bySize.find(key, position, ordinal) && previous.readRecord(ordinal, prior)) {
      bySize.claim(position);
      firstSeen = prior.firstSeen;
      renamed++;
    } else if (bySize.failed() || previous.ioFailed()) {
      ok = false;
      break;
    } else {
      firstSeen = takeFirstSeen(st.nextFirstSeen);
      stats.added++;
    }
    const uint64_t firstSeenAt = recordAt + offsetof(ClixRecord, firstSeen);
    if (!stage.seekSet(firstSeenAt) || stage.write(&firstSeen, sizeof(firstSeen)) != sizeof(firstSeen)) {
      LOG_ERR("LIBIDX", "firstSeen reconciliation: patch failed at record %u", static_cast<unsigned>(i));
      ok = false;
    }
  }
  if (!stage.close() || !ok) {
    LOG_ERR("LIBIDX", "firstSeen reconciliation failed");
    return false;
  }
  stats.renamed = renamed;
  stats.removed = byPath.size() - byPath.claimedCount() - renamed;
  return true;
}

// --- title order -----------------------------------------------------------
//
// The staged fold prefixes are sorted with their walk ordinals; equal 12-byte
// prefixes are refined from the staged fold. The result is ORDER_PATH, the
// stage index of each book in title order, which every later pass streams.
bool writeTitleOrder(const uint16_t n) {
  StageReader stage;
  KeyLoader loader;
  loader.stage = &stage;
  ExternalSorter sorter;
  if (!stage.open() || !sorter.begin(sortConfig(CLIX_FOLD_BYTES, &loadTitleSegment, &loader), n)) return false;
  uint32_t serviceUnits = 0;
  for (uint16_t s = 0; s < n; s++) {
    serviceBuilder(serviceUnits);
    ClixRecord r{};
    if (!stage.record(s, r)) return false;
    SortEntry entry{};
    memcpy(entry.key, r.fold, std::min<size_t>(r.foldLen, sizeof(entry.key)));
    entry.ordinal = s;
    entry.source = s;
    if (!sorter.add(entry)) return false;
  }
  StreamWriter order;
  return order.open(ORDER_PATH) && sorter.finish(&StreamWriter::writeSource, &order) && order.close();
}

}  // namespace

const char* libraryIndexPath() { return INDEX_PATH; }

static bool rebuildLibraryIndex(const char* rootPath, BuildStats& stats, const bool readMetadata) {
  const uint32_t startMs = millis();
  stats = BuildStats{};

  Storage.mkdir(CACHE_DIR);
  if (!recoverInterruptedInstall()) return false;
  const std::string folderStagePath = std::string(STAGE_PATH) + ".f";
  removeBuildScratch(folderStagePath.c_str());
  // Every exit below, success or failure, leaves no build scratch behind.
  const ScopedCleanup scratchCleanup{[&folderStagePath] { removeBuildScratch(folderStagePath.c_str()); }};

  auto nameBuf = makeUniqueNoThrow<char[]>(NAME_BUF_SIZE);
  if (!nameBuf) {
    LOG_ERR("LIBIDX", "name buffer alloc failed (%u bytes)", static_cast<unsigned>(NAME_BUF_SIZE));
    return false;
  }

  // The staging record exceeds the task's 256-byte stack budget. Allocate one
  // fallibly for the build and reuse it; static storage would pin scarce DRAM.
  auto stagedEntry = makeUniqueNoThrow<StagedEntry>();
  if (!stagedEntry) {
    LOG_ERR("LIBIDX", "staging record alloc failed (%u bytes)", static_cast<unsigned>(sizeof(StagedEntry)));
    return false;
  }

  auto dedupKeys = makeUniqueNoThrow<uint64_t[]>(LIBRARY_MAX_DEDUP_KEYS);
  if (!dedupKeys) {
    // Duplicate detection is defensive against damaged FAT directory entries.
    // Losing that defence may expose duplicate rows, but it must not make the
    // whole library unavailable when 8 KiB cannot be allocated on a C3.
    LOG_ERR("LIBIDX", "dedup key buffer alloc failed; continuing without duplicate detection");
  }

  // Load what the previous index knew, so the walk can recognise the same books.
  // An obsolete format starts fresh; I/O, record, and allocation failures stop
  // the rebuild so the previous index remains untouched.
  PriorTable priorByPath;
  uint16_t priorCount = 0;
  uint16_t nextFirstSeen = 0;
  LibraryIndexFile previous;
  if (Storage.exists(INDEX_PATH)) {
    if (previous.openForReconciliation(INDEX_PATH)) {
      nextFirstSeen = previous.header().nextFirstSeen;
      priorCount = previous.bookCount();
      if (!buildPriorByPath(previous, priorByPath)) {
        LOG_ERR("LIBIDX", "cannot stage the previous index; rebuild deferred");
        return false;
      }
    } else if (previous.ioFailed()) {
      LOG_ERR("LIBIDX", "cannot read previous index; rebuild deferred");
      return false;
    }
  }

  WalkState st;
  st.nameBuf = nameBuf.get();
  st.stagedEntry = stagedEntry.get();
  st.dedupKeys = dedupKeys.get();
  st.dedupDegraded = !dedupKeys;
  st.nextFirstSeen = nextFirstSeen;
  st.prior = previous.isOpen() ? &priorByPath : nullptr;
  st.readMetadata = readMetadata;
  st.previous = previous.isOpen() ? &previous : nullptr;
  st.stats = &stats;
  LibraryMetadataCache metadataCache;
  if (readMetadata) {
    metadataCache.open(&serviceSort, &buildCancelled);
    st.metadataCache = &metadataCache;
  }

  if (!Storage.openFileForWrite("LIBIDX", STAGE_PATH, st.stage) ||
      !Storage.openFileForWrite("LIBIDX", folderStagePath, st.folders)) {
    LOG_ERR("LIBIDX", "cannot open staging files");
    if (st.stage) st.stage.close();
    if (st.folders) st.folders.close();
    return false;
  }

  LOG_DBG("LIBIDX", "phase prepare/prior: %ums", static_cast<unsigned>(millis() - startMs));
  [[maybe_unused]] const uint32_t walkStartMs = millis();
  bool stageFlushed = false;
  {
    serialization::BufferedFileWriter stageOut(st.stage, LIBRARY_IO_BUFFER_SIZE);
    st.stageOut = &stageOut;
    walk(st, rootPath, 0);
    st.stageOut = nullptr;
    stageFlushed = stageOut.flush();
  }
  const bool stageClosed = st.stage.close();
  const bool foldersClosed = st.folders.close();
  st.metadataCache = nullptr;
  metadataCache.close();
  LOG_DBG("LIBIDX", "phase walk/metadata/stage: %ums", static_cast<unsigned>(millis() - walkStartMs));

  if (st.failed || !stageFlushed || !stageClosed || !foldersClosed) {
    LOG_ERR("LIBIDX", "staging failed; keeping the previous index");
    return false;
  }

  setBuildPhase(BuildPhase::Organizing);
  if (buildCancelled()) return false;

  stats.books = st.books;
  stats.folders = st.folderId;
  stats.duplicatesDropped = st.duplicatesDropped;
  stats.unreadableSkipped = st.unreadableSkipped;
  stats.dedupDegraded = st.dedupDegraded;
  stats.dedupAllocFailed = !dedupKeys;
  stats.unchanged = st.reused;
  stats.enriched = st.enriched;

  if (previous.isOpen() && previous.header().formatVersion == CLIX_FORMAT_VERSION &&
      previous.header().foldVersion == CLIX_FOLD_VERSION &&
      previous.header().metadataEnabled == static_cast<uint8_t>(readMetadata) && st.books == priorCount &&
      st.reused == priorCount && stats.metadataReused == priorCount && st.creationTimesUnchanged &&
      st.unreadableSkipped == 0 && !st.dedupDegraded &&
      (previous.header().flags & (CLIX_FLAG_RANKS_DEGRADED | CLIX_FLAG_DEDUP_DEGRADED | CLIX_FLAG_ARRIVAL_DEGRADED)) ==
          0) {
    previous.close();
    stats.walkMs = millis() - startMs;
    LOG_INF("LIBIDX", "unchanged: %u reused, %u parsed, no replacement, %ums",
            static_cast<unsigned>(stats.metadataReused), static_cast<unsigned>(stats.parsed),
            static_cast<unsigned>(stats.walkMs));
    return true;
  }

  [[maybe_unused]] const uint32_t reconcileStartMs = millis();
  if (st.unresolved > 0) {
    if (!resolveUnmatchedBooks(st, previous, priorByPath, stats)) return false;
  } else {
    stats.removed = priorByPath.size() - priorByPath.claimedCount();
  }
  LOG_DBG("LIBIDX", "phase reconcile: %ums", static_cast<unsigned>(millis() - reconcileStartMs));

  // The walk-only allocations are released before the sorts.
  previous.close();
  priorByPath.close();
  st.previous = nullptr;
  st.prior = nullptr;
  st.nameBuf = nullptr;
  st.stagedEntry = nullptr;
  st.dedupKeys = nullptr;
  nameBuf.reset();
  stagedEntry.reset();
  dedupKeys.reset();

  [[maybe_unused]] const uint32_t titleStartMs = millis();
  if (!writeTitleOrder(st.books)) {
    LOG_ERR("LIBIDX", "title sort failed; keeping the previous index");
    return false;
  }
  LOG_DBG("LIBIDX", "phase title order: %ums", static_cast<unsigned>(millis() - titleStartMs));

  [[maybe_unused]] const uint32_t emitStartMs = millis();
  const bool ok = emitIndex(folderStagePath.c_str(), st, stats);
  LOG_DBG("LIBIDX", "phase author/orders/emit: %ums", static_cast<unsigned>(millis() - emitStartMs));

  stats.walkMs = millis() - startMs;
  stats.indexReplaced = ok;
  LOG_INF("LIBIDX",
          "%s: %u books, %u folders, %u parsed, %u metadata reused, %u cached, replaced %u, %u dup dropped, "
          "%u unreadable, %ums",
          ok ? "built" : "FAILED", static_cast<unsigned>(stats.books), static_cast<unsigned>(stats.folders),
          static_cast<unsigned>(stats.parsed), static_cast<unsigned>(stats.metadataReused),
          static_cast<unsigned>(stats.metadataCached), static_cast<unsigned>(stats.indexReplaced),
          static_cast<unsigned>(stats.duplicatesDropped), static_cast<unsigned>(stats.unreadableSkipped),
          static_cast<unsigned>(stats.walkMs));
  return ok;
}

namespace {
std::atomic<bool> indexDirty{true};
}

void invalidateLibraryIndex() { indexDirty.store(true, std::memory_order_relaxed); }

bool libraryIndexNeedsRefresh() { return indexDirty.load(std::memory_order_relaxed); }

void restoreLibraryIndexAfterSleep() { indexDirty.store(false, std::memory_order_relaxed); }

uint16_t libraryBookLimit() { return CLIX_MAX_RECORDS; }

bool buildLibraryIndex(const char* rootPath, BuildStats& stats, const bool readMetadata,
                       const BuildCallbacks* callbacks) {
  // Clear before scanning, not after: a file mutation during the scan must
  // survive as a request for another reconciliation. Builds are foreground-only.
  indexDirty.exchange(false, std::memory_order_relaxed);
  buildControl = BuildControl{};
  buildControl.callbacks = callbacks;
  buildControl.lastPollMs = buildControl.lastProgressMs = millis();
  if (rootPath && *rootPath && normalizeLibraryRootAliases(rootPath).empty()) rootPath = "/";
  const bool ok = rebuildLibraryIndex(rootPath, stats, readMetadata);
  if (!ok && stats.failure == BuildFailure::None)
    stats.failure = buildControl.cancelled ? BuildFailure::Cancelled : BuildFailure::Error;
  buildControl = BuildControl{};
  // Retry only what another scan could change. Unreadable files and a capped
  // duplicate tracker come from the card's contents, so re-dirtying for them
  // would rescan on every Library visit until the files themselves change.
  if (!ok || stats.ranksDegraded || stats.dedupAllocFailed || stats.arrivalDegraded) invalidateLibraryIndex();
  return ok;
}

}  // namespace library
