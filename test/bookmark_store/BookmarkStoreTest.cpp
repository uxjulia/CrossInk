#include <HalStorage.h>
#include <uzlib.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <new>

#include "BookmarkStore.h"

namespace {
size_t allocationNumber = 0;
size_t failAllocation = 0;
size_t maxArrayBytes = SIZE_MAX;
bool denyLargeThrowingAllocation = false;
constexpr const char* BOOK = "/books/large.epub";
constexpr const char* MOVED = "/books/moved.epub";
}  // namespace

// Record allocations must use the nothrow array overload. Any large allocation
// through a throwing overload during a load is an immediate regression failure.
void* operator new(size_t size) {
  if (denyLargeThrowingAllocation && !bookmarkMockWriting && size >= 4096) {
    std::fprintf(stderr, "Unchecked allocation during bookmark load: %zu bytes\n", size);
    std::abort();
  }
  if (auto* bytes = std::malloc(size ? size : 1)) return bytes;
  std::abort();
}
void operator delete(void* bytes) noexcept { std::free(bytes); }
void operator delete(void* bytes, size_t) noexcept { std::free(bytes); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  ++allocationNumber;
  if (allocationNumber == failAllocation || size > maxArrayBytes) return nullptr;
  return std::malloc(size ? size : 1);
}
void operator delete[](void* bytes) noexcept { std::free(bytes); }
void operator delete[](void* bytes, const std::nothrow_t&) noexcept { std::free(bytes); }

namespace {
template <typename T>
void pod(std::string& bytes, const T& value) {
  bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
}
void string(std::string& bytes, const std::string& value) {
  pod(bytes, static_cast<uint32_t>(value.size()));
  bytes += value;
}
std::string storePath(const std::string& book, bool legacy = false) {
  const auto hash = legacy ? std::hash<std::string>{}(book) : uzlib_crc32(book.data(), book.size(), 0);
  return "/.crosspoint/bookmarks/epub_" + std::to_string(hash) + ".bin";
}
std::string fixture(const std::string& book, uint16_t count, uint8_t version = 5, uint16_t first = 0,
                    const std::string& title = "Title") {
  std::string bytes;
  pod(bytes, version);
  if (version == 2)
    pod(bytes, static_cast<uint8_t>(count));
  else
    pod(bytes, count);
  string(bytes, title);
  string(bytes, "Author");
  string(bytes, book);
  for (uint16_t i = 0; i < count; ++i) {
    Bookmark bookmark{};
    bookmark.spineIndex = first + i;
    bookmark.progress = 0.375f;
    bookmark.timestamp = 1234;
    bookmark.paragraphIndex = 17;
    std::snprintf(bookmark.chapterTitle, sizeof(bookmark.chapterTitle), "Chapter %u", first + i);
    std::snprintf(bookmark.snippet, sizeof(bookmark.snippet), "Passage %u", first + i);
    pod(bytes, bookmark.spineIndex);
    pod(bytes, bookmark.progress);
    pod(bytes, bookmark.timestamp);
    bytes.append(bookmark.chapterTitle, sizeof(bookmark.chapterTitle));
    if (version >= 4) pod(bytes, bookmark.paragraphIndex);
    if (version >= 5) bytes.append(bookmark.snippet, sizeof(bookmark.snippet));
  }
  return bytes;
}
void reset() {
  denyLargeThrowingAllocation = false;
  failAllocation = allocationNumber = 0;
  maxArrayBytes = SIZE_MAX;
  shortReadAfter = -1;
  failBookmarkWriteOpen = failBookmarkSync = failBookmarkClose = false;
  maxBookmarkWriteBytes = -1;
  failBookmarkRenameFrom.clear();
  BOOKMARKS.unload();
  assert(openBookmarkReaders == 0);
  Storage.files.clear();
  Storage.writes = 0;
}
bool load() {
  denyLargeThrowingAllocation = true;
  const bool ok = BOOKMARKS.loadForBook(BOOK, "Title", "Author", "epub");
  denyLargeThrowingAllocation = false;
  assert(openBookmarkReaders == 0);
  return ok;
}
void cannotOverwriteAfterFailedLoad() {
  assert(BOOKMARKS.getBookmarks().empty());
  const auto files = Storage.files;
  assert(BOOKMARKS.addBookmark(3, 0.2f, 1, "New") == BookmarkStore::AddResult::Unavailable);
  BOOKMARKS.removeBookmarkForPage(3, 0.2f, 1);
  assert(!BOOKMARKS.removeBookmarkAt(0));
  BOOKMARKS.clearAll();
  BOOKMARKS.saveToFile();
  BOOKMARKS.unload();
  assert(Storage.files == files);
  assert(Storage.writes == 0);
}
void largeLoadFailureAndRetry() {
  reset();
  const auto bytes = fixture(BOOK, 1024);
  Storage.files[storePath(BOOK)] = bytes;
  maxArrayBytes = 16 * 1024;  // Enough total records exist, but no large contiguous block.
  assert(!load());
  assert(allocationNumber == 1);
  cannotOverwriteAfterFailedLoad();
  assert(Storage.files.at(storePath(BOOK)) == bytes);
  maxArrayBytes = SIZE_MAX;
  assert(load());
  assert(BOOKMARKS.getBookmarks().size() == 1024);
  assert(BOOKMARKS.getBookmarks()[1023].spineIndex == 1023);
  assert(BOOKMARKS.getBookmarks()[1023].paragraphIndex == 17);
  assert(BOOKMARKS.getBookmarks()[1023].progress == 0.375f);
  assert(BOOKMARKS.addBookmark(2000, 0, 1, "Extra") == BookmarkStore::AddResult::LimitReached);
  assert(BOOKMARKS.removeBookmarkAt(1023));
  assert(load());
  assert(BOOKMARKS.getBookmarks().size() == 1023);
}
void versionsAndHeaderValidation() {
  for (uint8_t version : {2, 3, 4, 5}) {
    reset();
    const auto bytes = fixture(BOOK, 200, version);
    Storage.files[storePath(BOOK)] = bytes;
    failAllocation = 1;
    assert(!load());
    cannotOverwriteAfterFailedLoad();
    assert(Storage.files.at(storePath(BOOK)) == bytes);  // No partial format upgrade.
    failAllocation = 0;
    assert(load());
    const auto& bookmark = BOOKMARKS.getBookmarks()[199];
    assert(bookmark.spineIndex == 199);
    assert(bookmark.paragraphIndex == (version >= 4 ? 17 : UINT16_MAX));
    assert((bookmark.snippet[0] != '\0') == (version >= 5));
  }
  reset();
  // A large but valid unused title is streamed, never resized into a string.
  Storage.files[storePath(BOOK)] = fixture(BOOK, 1, 5, 0, std::string(32000, 'x'));
  assert(load());
  assert(BOOKMARKS.getBookmarks().size() == 1);
  for (auto bad : {std::string(), std::string("\5", 1), fixture(BOOK, 1025), fixture(MOVED, 1),
                   fixture(BOOK, 1024).substr(0, 200)}) {
    reset();
    Storage.files[storePath(BOOK)] = bad;
    assert(!load());
    assert(allocationNumber == 0);  // Reject impossible headers/sizes before allocating records.
    cannotOverwriteAfterFailedLoad();
  }
  reset();
  std::string bad;
  pod(bad, uint8_t{5});
  pod(bad, uint16_t{1});
  pod(bad, uint32_t{0xffffffff});
  Storage.files[storePath(BOOK)] = bad;
  assert(!load());
  cannotOverwriteAfterFailedLoad();
  reset();
  Storage.files[storePath(BOOK)] = fixture(BOOK, 2);
  shortReadAfter = 100;
  assert(!load());
  cannotOverwriteAfterFailedLoad();
}
void automaticRewriteFailuresPreserveSources() {
  for (bool legacyPresent : {false, true}) {
    for (int failure : {0, 1, 2, 3, 4}) {
      reset();
      Storage.files[storePath(BOOK)] = fixture(BOOK, 200, 3);
      if (legacyPresent) Storage.files[storePath(BOOK, true)] = fixture(BOOK, 8, 5, 200);
      const auto original = Storage.files;
      if (failure == 0) failBookmarkWriteOpen = true;
      if (failure == 1) maxBookmarkWriteBytes = 1;
      if (failure == 2) failBookmarkSync = true;
      if (failure == 3) failBookmarkClose = true;
      if (failure == 4) failBookmarkRenameFrom = storePath(BOOK) + ".tmp";
      assert(load());  // Read succeeded; an optional format rewrite may be retried later.
      assert(BOOKMARKS.getBookmarks().size() == (legacyPresent ? 208 : 200));
      assert(Storage.files == original);
      assert(openBookmarkReaders == 0);
    }
  }
  for (bool primaryPresent : {false, true}) {
    reset();
    Storage.files[BOOK] = "Book";
    const auto backup = fixture(BOOK, 4);
    Storage.files[storePath(BOOK) + ".bak"] = backup;
    if (primaryPresent) Storage.files[storePath(BOOK)] = fixture(BOOK, 2);
    assert(BookmarkStore::hasAnyBookmarks());
    assert(load());
    assert(BOOKMARKS.getBookmarks().size() == 4);
    assert(Storage.files.at(storePath(BOOK)) == backup);
    assert(!Storage.files.contains(storePath(BOOK) + ".bak"));
  }
  reset();
  Storage.files[BOOK] = "Book";
  Storage.files[storePath(BOOK) + ".bak"] = fixture(BOOK, 4);
  std::vector<BookmarkedBookEntry> entries;
  assert(BookmarkStore::getAllBookmarkedBooks(entries));
  assert(entries.size() == 1 && entries[0].count == 4);
  assert(Storage.files.contains(storePath(BOOK)) && !Storage.files.contains(storePath(BOOK) + ".bak"));
}

void homeHeaderScansRejectOversizedFields() {
  for (size_t titleLength : {0u, 128u, 1024u, 32000u}) {
    reset();
    Storage.files[BOOK] = "Book bytes";
    Storage.files[storePath(BOOK)] = fixture(BOOK, 1024, 5, 0, std::string(titleLength, 'x'));
    const auto original = Storage.files;
    std::vector<BookmarkedBookEntry> entries;
    denyLargeThrowingAllocation = true;
    assert(BookmarkStore::getAllBookmarkedBooks(entries));
    denyLargeThrowingAllocation = false;
    if (titleLength <= 1024) {
      assert(entries.size() == 1 && entries[0].count == 1024 && entries[0].bookPath == BOOK);
      assert(entries[0].bookTitle.size() == titleLength);
    } else {
      assert(entries.empty());
    }
    assert(Storage.files == original && Storage.writes == 0);
    assert(openBookmarkReaders == 0 && allocationNumber == 0);
  }
}

void legacyMergeFailures() {
  for (size_t failure : {1, 2, 3}) {
    reset();
    Storage.files[storePath(BOOK)] = fixture(BOOK, 8);
    Storage.files[storePath(BOOK, true)] = fixture(BOOK, 8, 5, 8);
    const auto files = Storage.files;
    failAllocation = failure;
    assert(!load());
    cannotOverwriteAfterFailedLoad();
    assert(Storage.files == files);
    failAllocation = 0;
    assert(load());
    assert(BOOKMARKS.getBookmarks().size() == 16);
    assert(!Storage.files.contains(storePath(BOOK, true)));
  }
  reset();
  Storage.files[storePath(BOOK)] = fixture(BOOK, 1024);
  Storage.files[storePath(BOOK, true)] = fixture(BOOK, 1, 5, 2000);
  const auto files = Storage.files;
  assert(!load());
  cannotOverwriteAfterFailedLoad();
  assert(Storage.files == files);
}
void addFailurePreservesLoadedRecords() {
  reset();
  Storage.files[storePath(BOOK)] = fixture(BOOK, 1);
  assert(load());
  const auto files = Storage.files;
  failAllocation = allocationNumber + 1;
  assert(BOOKMARKS.addBookmark(100, 0.2f, 1, "New") == BookmarkStore::AddResult::Unavailable);
  assert(BOOKMARKS.getBookmarks().size() == 1);
  assert(Storage.files == files);
  // Replacing a bookmark on the same page reuses its slot and needs no allocation.
  assert(BOOKMARKS.addBookmark(0, 0.375f, 10, "Replacement") == BookmarkStore::AddResult::Added);
  assert(BOOKMARKS.getBookmarks().size() == 1);
}
void recordBufferAndMigrationSuccess() {
  reset();
  BookmarkRecords records;
  Bookmark bookmark{};
  bookmark.spineIndex = 42;
  assert(records.reserve(1) && records.push_back(bookmark));
  failAllocation = allocationNumber + 1;
  assert(!records.reserve(2));
  assert(records.size() == 1 && records[0].spineIndex == 42);
  BookmarkRecords moved = std::move(records);
  assert(records.empty() && records.capacity() == 0 && moved.size() == 1);
  moved.erase(moved.begin());
  assert(moved.empty());

  for (int operation : {0, 1, 2}) {
    reset();
    Storage.files[storePath(BOOK)] = fixture(BOOK, 2);
    Storage.files[storePath(BOOK, true)] = fixture(BOOK, 2, 5, 2);
    Storage.files[storePath(MOVED)] = fixture(MOVED, 2, 5, 4);
    Storage.files[storePath(MOVED, true)] = fixture(MOVED, 2, 5, 6);
    const auto original = Storage.files;
    if (operation == 0) {
      assert(BookmarkStore::migrateForFilePath(BOOK, MOVED, "Title", "Author", "epub"));
    } else {
      BookmarkStore::RenameMigration migration;
      assert(BookmarkStore::beginRenameMigration(BOOK, MOVED, "Title", "Author", "epub", migration));
      assert(migration.active);
      if (operation == 2) {
        assert(BookmarkStore::rollbackRenameMigration(migration));
        assert(Storage.files == original);
        continue;
      }
      assert(BookmarkStore::commitRenameMigration(migration));
    }
    assert(!Storage.files.contains(storePath(BOOK)) && !Storage.files.contains(storePath(BOOK, true)));
    assert(!Storage.files.contains(storePath(MOVED, true)));
    assert(BOOKMARKS.loadForBook(MOVED, "Title", "Author", "epub"));
    assert(BOOKMARKS.getBookmarks().size() == 8);
    assert(openBookmarkReaders == 0);
  }
}

void migrationFailuresPreserveBothBooks() {
  for (bool transactional : {false, true}) {
    for (size_t failure = 1; failure <= 7; ++failure) {
      reset();
      Storage.files[storePath(BOOK)] = fixture(BOOK, 4);
      Storage.files[storePath(BOOK, true)] = fixture(BOOK, 4, 5, 4);
      Storage.files[storePath(MOVED)] = fixture(MOVED, 4, 5, 8);
      Storage.files[storePath(MOVED, true)] = fixture(MOVED, 4, 5, 12);
      const auto files = Storage.files;
      failAllocation = failure;
      if (transactional) {
        BookmarkStore::RenameMigration migration;
        assert(!BookmarkStore::beginRenameMigration(BOOK, MOVED, "Title", "Author", "epub", migration));
        assert(!migration.active);
      } else {
        assert(!BookmarkStore::migrateForFilePath(BOOK, MOVED, "Title", "Author", "epub"));
      }
      assert(openBookmarkReaders == 0);
      assert(Storage.files == files);
      assert(Storage.writes == 0);
    }
  }
}
}  // namespace
int main() {
  largeLoadFailureAndRetry();
  versionsAndHeaderValidation();
  legacyMergeFailures();
  homeHeaderScansRejectOversizedFields();
  automaticRewriteFailuresPreserveSources();
  addFailurePreservesLoadedRecords();
  migrationFailuresPreserveBothBooks();
  recordBufferAndMigrationSuccess();
  reset();
  std::puts(
      "PASS: bookmark loading, legacy versions/merges, short reads, retry and migration survive allocation failure "
      "without changing files");
}
