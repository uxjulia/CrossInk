#pragma once
#include <Memory.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// chapterTitle is always NUL-terminated within BOOKMARK_CHAPTER_TITLE_MAX bytes.
// This size is part of the on-disk format — do not change without incrementing the file version.
inline constexpr size_t BOOKMARK_CHAPTER_TITLE_MAX = 48;
inline constexpr size_t BOOKMARK_SNIPPET_MAX = 64;

struct Bookmark {
  uint16_t spineIndex;
  float progress;
  uint32_t timestamp;
  char chapterTitle[BOOKMARK_CHAPTER_TITLE_MAX];
  // Optional 1-based paragraph anchor from the section cache. UINT16_MAX means unavailable.
  uint16_t paragraphIndex = UINT16_MAX;
  char snippet[BOOKMARK_SNIPPET_MAX] = {};
};

// Owned record storage with explicit allocation failure. std::vector::reserve
// aborts on ESP32 when its allocation fails because exceptions are disabled.
class BookmarkRecords {
 public:
  BookmarkRecords() = default;
  BookmarkRecords(BookmarkRecords&& other) noexcept { *this = std::move(other); }
  BookmarkRecords& operator=(BookmarkRecords&& other) noexcept {
    if (this != &other) {
      records = std::move(other.records);
      count = std::exchange(other.count, 0);
      reserved = std::exchange(other.reserved, 0);
    }
    return *this;
  }

  bool reserve(size_t capacity) {
    if (capacity <= reserved) return true;
    if (capacity > SIZE_MAX / sizeof(Bookmark)) return false;
    // Up to 1024 records (128 KiB). Runtime-sized, too large for stack/static
    // storage; retain the old buffer until the fallible replacement succeeds.
    auto next = makeUniqueNoThrow<Bookmark[]>(capacity);
    if (!next) return false;
    if (count > 0) std::copy_n(records.get(), count, next.get());
    records = std::move(next);
    reserved = capacity;
    return true;
  }
  bool push_back(const Bookmark& bookmark) {
    if (count == reserved && !reserve(count + 1)) return false;
    records[count++] = bookmark;
    return true;
  }
  void erase(const Bookmark* position) {
    Bookmark* first = records.get();
    const size_t index = position - first;
    if (index >= count) return;
    std::move(first + index + 1, first + count, first + index);
    --count;
  }
  void clear() { count = 0; }
  void release() {
    records.reset();
    count = reserved = 0;
  }
  bool empty() const { return count == 0; }
  size_t size() const { return count; }
  size_t capacity() const { return reserved; }
  Bookmark* data() { return records.get(); }
  const Bookmark* data() const { return records.get(); }
  Bookmark* begin() { return data(); }
  const Bookmark* begin() const { return data(); }
  Bookmark* end() { return count ? data() + count : data(); }
  const Bookmark* end() const { return count ? data() + count : data(); }
  Bookmark& operator[](size_t index) { return records[index]; }
  const Bookmark& operator[](size_t index) const { return records[index]; }

 private:
  std::unique_ptr<Bookmark[]> records;
  size_t count = 0;
  size_t reserved = 0;
};

struct BookmarkedBookEntry {
  std::string bookTitle;
  std::string bookAuthor;
  std::string bookPath;
  std::string bookType;
  uint16_t count;
};

class BookmarkStore {
 public:
  enum class AddResult : uint8_t {
    Added,
    LimitReached,
    Unavailable,
  };

  struct RenameMigration {
    std::string sourceCurrentPath;
    std::string sourceLegacyPath;
    std::string destinationCurrentPath;
    std::string destinationLegacyPath;
    std::string destinationCurrentBackupPath;
    std::string destinationLegacyBackupPath;
    bool destinationCurrentBackedUp = false;
    bool destinationLegacyBackedUp = false;
    bool active = false;
  };

  static BookmarkStore& getInstance() { return instance; }

  // Load bookmarks for a book. Returns true even when no file exists yet (empty store).
  // bookType must be "epub", "xtc", or "txt" — used to form the cache filename.
  bool loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                   const std::string& bookType);
  void unload();

  AddResult addBookmark(uint16_t spineIndex, float progress, int pageCount, const char* chapterTitle,
                        uint16_t paragraphIndex = UINT16_MAX, const char* snippet = nullptr);
  void removeBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount);
  bool removeBookmarkAt(size_t index);
  bool hasBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount);
  const BookmarkRecords& getBookmarks() const { return bookmarks; }

  // Flush to disk if dirty. Called automatically by add/remove; also call from reader onExit().
  void saveToFile();

  // Remove all bookmarks for the current book and delete its bookmark file.
  void clearAll();

  // Returns true if any bookmark files exist on disk (directory scan, no file parsing).
  static bool hasAnyBookmarks();

  // Delete the bookmark file for a given file path and book type without loading the book.
  // bookType must be "epub", "xtc", or "txt".
  static void deleteForFilePath(const std::string& filePath, const std::string& bookType);

  // Rewrite bookmark storage to follow a file move/rename while preserving existing bookmarks.
  // oldFilePath and newFilePath must refer to the same logical book.
  static bool migrateForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                 const std::string& title, const std::string& author, const std::string& bookType);
  static bool hasStoredStateForFilePath(const std::string& filePath, const std::string& bookType);

  // Transactional variant used by file rename. Source files and destination
  // backups remain in place until commit, so a later failure can restore both
  // logical books without reverse-merging their metadata.
  static bool beginRenameMigration(const std::string& oldFilePath, const std::string& newFilePath,
                                   const std::string& title, const std::string& author, const std::string& bookType,
                                   RenameMigration& migration);
  static bool commitRenameMigration(RenameMigration& migration);
  static bool rollbackRenameMigration(RenameMigration& migration);

  // Scan /.crosspoint/bookmarks/ and populate `out` with one entry per book that has bookmarks.
  // Reads only the file header (does not load full bookmark records).
  // Caller should reserve `out` before calling.
  static bool getAllBookmarkedBooks(std::vector<BookmarkedBookEntry>& out);

 private:
  static BookmarkStore instance;

  BookmarkRecords bookmarks;
  std::string bookFilePath;
  std::string bookTitle;
  std::string bookAuthor;
  std::string storeFilePath;
  bool dirty = false;
  // Never allow an incomplete/failed load to overwrite the source file.
  bool writable = false;

  bool readFromFile();
  bool readFromFile(const std::string& path, BookmarkRecords& out, bool& needsRewrite) const;
  bool writeToFile() const;
};

#define BOOKMARKS BookmarkStore::getInstance()
