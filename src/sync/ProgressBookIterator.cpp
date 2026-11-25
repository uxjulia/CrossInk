#include "ProgressBookIterator.h"

#include <FsHelpers.h>
#include <LibraryBuilder.h>
#include <Memory.h>
#include <ZipFile.h>

#include <charconv>

#ifndef SIMULATOR
#include <esp_attr.h>
#endif

#include "SyncFiles.h"
#include "activities/reader/EpubReaderUtils.h"

namespace {
constexpr char CACHE_ROOT[] = "/.crosspoint";
constexpr char PATH_FILE[] = "/progress-path.bin";
constexpr uint8_t VERSION = 1;
constexpr size_t MAX_BOOK_PATH = 1024;

// A successful card walk can leave orphan caches behind. Remember the remaining
// hashes for the whole card and the last folder, including across reader sleep.
struct LegacyLookup {
  uint64_t scope;
  uint64_t digest;
  uint32_t count;
  uint32_t revision;

  bool matches(uint64_t expectedScope, uint64_t expectedDigest, uint32_t expectedCount,
               uint32_t currentRevision) const {
    return scope == expectedScope && digest == expectedDigest && count == expectedCount && revision == currentRevision;
  }
};

#ifndef SIMULATOR
RTC_NOINIT_ATTR
#endif
LegacyLookup legacyLookups[2];

uint64_t scopeHash(const std::string_view scope) { return ZipFile::fnvHash64(scope.data(), scope.size()); }

bool validBookPath(std::string_view path) {
  if (path.size() > MAX_BOOK_PATH || !path.starts_with('/') || !FsHelpers::hasEpubExtension(path)) return false;
  path.remove_prefix(1);
  while (!path.empty()) {
    const auto slash = path.find('/');
    const auto part = path.substr(0, slash);
    if (part.starts_with('.') || !FsHelpers::isSafePathComponent(part)) return false;
    if (slash == std::string_view::npos) return true;
    path.remove_prefix(slash + 1);
  }
  return false;
}

bool hasProgress(const std::string& cachePath) {
  EpubReaderUtils::Progress saved;
  return EpubReaderUtils::loadProgressPath(cachePath + "/progress.bin", saved, "SyncBooks") && saved.hasPageCount &&
         saved.pageNumber >= 0 && saved.pageNumber < saved.pageCount && saved.spineIndex >= 0;
}

bool readPath(const std::string& cachePath, std::string& bookPath) {
  const auto path = cachePath + PATH_FILE;
  SyncFiles::Reader file(path.c_str());
  uint8_t version = 0;
  return file.open() != SyncFiles::Reader::Source::None && file.pod(version) && version == VERSION &&
         file.string16(bookPath, MAX_BOOK_PATH) && file.atEnd() && validBookPath(bookPath) &&
         Epub::cachePathForFilePath(bookPath, CACHE_ROOT) == cachePath;
}

bool writePath(const std::string& cachePath, const std::string& bookPath) {
  if (!validBookPath(bookPath) || Epub::cachePathForFilePath(bookPath, CACHE_ROOT) != cachePath) return false;
  const auto path = cachePath + PATH_FILE;
  SyncFiles::Writer file(path.c_str());
  if (file.open() && file.pod(VERSION) && file.string16(bookPath) && file.commit()) return true;
  LOG_ERR("SyncBooks", "Cannot record EPUB progress path");
  return false;
}
}  // namespace

ProgressBookIterator::ProgressBookIterator(std::string root)
    : root(std::move(root)), pathRevision(library::libraryPathRevision()) {}

void ProgressBookIterator::resetLegacyLookups() {
  legacyLookups[0] = {};
  legacyLookups[1] = {};
  library::resetLibraryPathRevision();
}

void ProgressBookIterator::rememberPath(const std::string& bookPath, const std::string& cachePath) {
  if (!Storage.exists((cachePath + PATH_FILE).c_str())) writePath(cachePath, bookPath);
}

void ProgressBookIterator::updateMovedPath(const std::string& bookPath, const std::string& cachePath) {
  if (hasProgress(cachePath)) writePath(cachePath, bookPath);
}

ProgressBookIterator::Result ProgressBookIterator::next(std::string& bookPath) {
  if (done) return Result::Done;
  if (pathRevision != library::libraryPathRevision()) {
    LOG_ERR("SyncBooks", "The library changed during progress discovery");
    return Result::Error;
  }
  if (legacy) return nextLegacy(bookPath);
  const auto result = nextCache(bookPath);
  if (result != Result::Done) return result;
  if (!unlinkedCaches || (pathRevision == library::libraryPathRevision() &&
                          (legacyLookups[0].matches(scopeHash("/"), unlinkedDigest, unlinkedCaches, pathRevision) ||
                           legacyLookups[1].matches(scopeHash(root), unlinkedDigest, unlinkedCaches, pathRevision)))) {
    done = true;
    return Result::Done;
  }
  legacy = makeUniqueNoThrow<FolderBookIterator>(root);
  if (!legacy) {
    LOG_ERR("SyncBooks", "Cannot allocate legacy EPUB progress scan");
    return Result::Error;
  }
  LOG_INF("SyncBooks", "Looking up paths for %lu older EPUB caches", static_cast<unsigned long>(unlinkedCaches));
  return Result::Entry;
}

ProgressBookIterator::Result ProgressBookIterator::nextCache(std::string& bookPath) {
  auto directory = Storage.open(CACHE_ROOT);
  if (!directory || !directory.isDirectory() || !seekDirectoryPosition(directory, cachePosition)) {
    if (directory) directory.close();
    if (!cacheStarted && !Storage.exists(CACHE_ROOT)) return Result::Done;
    LOG_ERR("SyncBooks", "Cannot enumerate EPUB progress caches");
    return Result::Error;
  }
  auto entry = directory.openNextFile();
  if (!entry) {
    const bool failed = FsHelpers::directoryIterationFailed(directory);
    directory.close();
    return failed ? Result::Error : Result::Done;
  }
  cachePosition = directoryPosition(directory);
  cacheStarted = true;
  // A cache name is "epub_" plus a decimal uint64_t. Ignore other cache entries.
  char name[5 + 20 + 2]{};
  const bool epubCache = entry.isDirectory() && entry.getName(name, sizeof(name)) < sizeof(name) - 1 &&
                         std::string_view(name).starts_with("epub_");
  entry.close();
  directory.close();
  if (!epubCache) return Result::Entry;
  const std::string_view suffix = std::string_view(name).substr(5);
  uint64_t hash = 0;
  const auto parsed = std::from_chars(suffix.begin(), suffix.end(), hash);
  if (parsed.ec != std::errc{} || parsed.ptr != suffix.end()) return Result::Entry;
  const auto cachePath = std::string(CACHE_ROOT) + "/" + name;
  if (!hasProgress(cachePath)) return Result::Entry;
  if (!readPath(cachePath, bookPath)) {
    ++unlinkedCaches;
    unlinkedDigest += hash;
    unlinkedHashes.set(hash % unlinkedHashes.size());
    unlinkedHashes.set((hash >> 32) % unlinkedHashes.size());
    return Result::Entry;
  }
  const bool inScope =
      root == "/" || (bookPath.starts_with(root) && bookPath.size() > root.size() && bookPath[root.size()] == '/');
  if (!inScope) return Result::Entry;
  auto book = Storage.open(bookPath.c_str());
  if (!book) return Result::Entry;
  const bool isFile = !book.isDirectory();
  book.close();
  return isFile ? Result::Book : Result::Entry;
}

bool ProgressBookIterator::mightNeedPath(const uint64_t hash) const {
  return unlinkedHashes.test(hash % unlinkedHashes.size()) && unlinkedHashes.test((hash >> 32) % unlinkedHashes.size());
}

ProgressBookIterator::Result ProgressBookIterator::nextLegacy(std::string& bookPath) {
  const auto result = legacy->next(bookPath);
  if (result == Result::Done) {
    done = true;
    // A live book whose path could not be saved must stay eligible for the fallback.
    if (!pathRecoveryFailed && pathRevision == library::libraryPathRevision()) {
      legacyLookups[root == "/" ? 0 : 1] = {scopeHash(root), unlinkedDigest, unlinkedCaches, pathRevision};
    }
  }
  if (result != Result::Book || !FsHelpers::hasEpubExtension(bookPath)) {
    return result == Result::Book ? Result::Entry : result;
  }

  const auto currentHash = ZipFile::fnvHash64(bookPath.data(), bookPath.size());
  const auto legacyHash = Epub::legacyCachePathHash(bookPath);
  if (!mightNeedPath(currentHash) && !mightNeedPath(legacyHash)) return Result::Entry;

  const auto canonical = Epub::cachePathForFilePath(bookPath, CACHE_ROOT);
  const bool canonicalExists = Storage.exists(canonical.c_str());
  const auto hash = canonicalExists ? currentHash : legacyHash;
  if (!mightNeedPath(hash)) return Result::Entry;
  const auto cachePath = canonicalExists ? canonical : Epub::resolveCachePathForFilePath(bookPath, CACHE_ROOT);
  std::string linkedPath;
  // Path records already returned by the cache walk must not be returned again.
  if (!hasProgress(cachePath)) {
    // The cache may still be on the card after a failed legacy rename or read.
    if ((!canonicalExists &&
         Storage.exists((std::string(CACHE_ROOT) + "/epub_" + std::to_string(legacyHash)).c_str())) ||
        Storage.exists((cachePath + "/progress.bin").c_str()) ||
        Storage.exists((cachePath + "/progress.bin.bak").c_str())) {
      pathRecoveryFailed = true;
    }
    return Result::Entry;
  }
  if (readPath(cachePath, linkedPath)) return Result::Entry;
  if (!writePath(cachePath, bookPath)) {
    pathRecoveryFailed = true;
  } else if (unlinkedCaches) {
    --unlinkedCaches;
    unlinkedDigest -= hash;
  }
  return Result::Book;
}
