#pragma once
#include <string>
#include <vector>

struct Bookmark {
  uint16_t spineIndex;
  float progress;
  uint32_t timestamp;
  char chapterTitle[48];
};

struct BookmarkedBookEntry {
  std::string bookTitle;
  std::string bookAuthor;
  std::string bookPath;
  uint8_t count;
};

class BookmarkStore {
  static BookmarkStore instance;

  std::vector<Bookmark> bookmarks;
  std::string bookFilePath;
  std::string bookTitle;
  std::string bookAuthor;
  std::string storeFilePath;
  bool dirty = false;

  bool readFromFile();
  bool writeToFile() const;

 public:
  static BookmarkStore& getInstance() { return instance; }

  // Load bookmarks for a book. Returns true even when no file exists yet (empty store).
  // bookType must be "epub", "xtc", or "txt" — used to form the cache filename.
  bool loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                   const std::string& bookType);
  void unload();

  void addBookmark(uint16_t spineIndex, float progress, const char* chapterTitle);
  void removeBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount);
  bool hasBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount);
  const std::vector<Bookmark>& getBookmarks() const { return bookmarks; }

  // Flush to disk if dirty. Called automatically by add/remove; also call from reader onExit().
  void saveToFile();

  // Remove all bookmarks for the current book and delete its bookmark file.
  void clearAll();

  // Scan /.crosspoint/bookmarks/ and populate `out` with one entry per book that has bookmarks.
  // Reads only the file header (does not load full bookmark records).
  // Caller should reserve `out` before calling.
  static bool getAllBookmarkedBooks(std::vector<BookmarkedBookEntry>& out);
};

#define BOOKMARKS BookmarkStore::getInstance()
