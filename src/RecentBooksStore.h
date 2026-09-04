#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

struct RecentBook {
  // Unknown failures stay retryable; Missing is set only after EPUB metadata has no cover image.
  enum class CoverState : uint8_t { Unknown = 0, Missing = 1 };

  std::string path;
  std::string title;
  std::string author;
  std::string coverBmpPath;
  CoverState coverState = CoverState::Unknown;

  bool operator==(const RecentBook& other) const { return path == other.path; }
};

class RecentBooksStore : public PersistableStore<RecentBooksStore> {
 private:
  std::vector<RecentBook> recentBooks;

  static constexpr int MAX_RECENT_BOOKS = 18;

  RecentBooksStore() = default;
  ~RecentBooksStore() = default;
  bool loadFromBinaryFile();

  friend class PersistableStore<RecentBooksStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/recent.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
  bool loadFromFile();

  // Add a new book to the front, or refresh an existing entry and promote it
  // to the front.
  void addOrUpdateBook(const std::string& path, const std::string& title, const std::string& author,
                       const std::string& coverBmpPath,
                       RecentBook::CoverState coverState = RecentBook::CoverState::Unknown);

  // updateBook updates metadata for an existing book only and must not change
  // recent-books ordering. Use addOrUpdateBook when the touched book should
  // become most recent. Returns false if the book does not exist.
  [[nodiscard]] bool updateBook(const std::string& path, const std::string& title, const std::string& author,
                                const std::string& coverBmpPath, RecentBook::CoverState coverState);

  // Remove the entry whose path matches (used when a book is removed from recents or finished/read).
  // Returns true if an entry was found and removed (no-op + false otherwise).
  // Persistence is best-effort: a failed save is logged, not reflected in the return.
  bool removeByPath(const std::string& path);

  // Repoint an entry's path (and coverBmpPath, if it lived under the old cache dir) after the
  // backing file and cache dir were moved on disk. No-op if no entry matches oldPath.
  // Persists on success. Keeps the entry's list position (does not reorder).
  void updatePath(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                  const std::string& newCachePath);

  // True if the book's backing file is no longer present on the SD card.
  static bool isMissing(const RecentBook& book);

  // Remove entries whose backing file is no longer on the SD card.
  // Returns true if any entry was removed. Does not persist — caller decides.
  bool pruneMissing();

  // Get the list of recent books (most recent first)
  const std::vector<RecentBook>& getBooks() const {
    ensureLoaded();
    return recentBooks;
  }

  // Get the count of recent books
  int getCount() const {
    ensureLoaded();
    return static_cast<int>(recentBooks.size());
  }

  RecentBook getDataFromBook(std::string path) const;
};

// Helper macro to access recent books store
#define RECENT_BOOKS RecentBooksStore::getInstance()
