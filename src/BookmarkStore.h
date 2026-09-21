#pragma once
#include <cstdint>
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
  const std::vector<Bookmark>& getBookmarks() const { return bookmarks; }

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

  std::vector<Bookmark> bookmarks;
  std::string bookFilePath;
  std::string bookTitle;
  std::string bookAuthor;
  std::string storeFilePath;
  bool dirty = false;

  bool readFromFile();
  bool readFromFile(const std::string& path, std::vector<Bookmark>& out, bool& needsRewrite) const;
  bool writeToFile() const;
};

#define BOOKMARKS BookmarkStore::getInstance()
