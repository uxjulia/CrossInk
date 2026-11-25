#pragma once

#include <KOReaderCredentialStore.h>

#include <cstdint>
#include <string>

// A book's small sync record in its cache folder ("<cache>/sync.bin", the
// epub_<hash>/ or xtc_<hash>/ folder that already follows the book through
// in-app renames and moves). It stores the last KOReader document id, the stamps that let an
// interrupted catch-up or first walk resume by skipping books already done,
// and which sync_ids.bin generation the book was recorded in.
struct BookSyncFile {
  bool hasId = false;
  DocumentMatchMethod method = DocumentMatchMethod::FILENAME;
  uint8_t id[16]{};
  uint32_t fileSize = 0;  // book size when the id was recorded
  uint32_t statsStamp = 0;
  uint32_t clippingsStamp = 0;
  uint32_t walkStamp = 0;      // first walk (sync scope) that finished this book
  uint32_t idsGeneration = 0;  // SyncIds::generation() it was recorded under; 0 = not recorded

  // The book's cache folder, or empty for formats without one. Never creates it.
  static std::string cachePathFor(const std::string& bookPath);
  static bool read(const std::string& cachePath, BookSyncFile& out);
  // Writes only into an existing cache folder: never-opened books get none.
  static bool write(const std::string& cachePath, const BookSyncFile& record);

  // Document id for the configured matching method: binary matching samples the current file. Empty on
  // failure. An unfinished EPUB is also recorded in sync_ids.bin, so the
  // progress pull can find it; XTC and finished books are not.
  static std::string documentId(const std::string& bookPath);
  // Records that a catch-up run (`stamp`) has sent this book's stats or clippings.
  static void stamp(const std::string& bookPath, bool stats, uint32_t stamp);
  // True when this book already carries `stamp` for that catch-up.
  static bool stamped(const std::string& bookPath, bool stats, uint32_t stamp);
  // Keeps sync_ids.bin in step with the library. A finished book leaves the
  // map; un-finishing it lets the next id lookup record it again.
  static void onFinishedChanged(const std::string& bookPath, bool finished);
  // After an in-app rename or move (cache folder already at the new path).
  // Filename matching gives the renamed book a new id, so the old entry goes.
  static void onRenamed(const std::string& oldPath, const std::string& newPath);
  static void onDeleted(const std::string& bookPath);
  // A replacement keeps catch-up history, but must not reuse an id derived
  // from the old bytes (even when the new file has the same size).
  static bool onFileChanged(const std::string& bookPath);
  // First-walk stamps: the walk with this id already finished the book.
  static void stampWalk(const std::string& bookPath, uint32_t walkId);
  static bool walkStamped(const std::string& bookPath, uint32_t walkId);
};
