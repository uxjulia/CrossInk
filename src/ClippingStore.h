#pragma once

#include <Memory.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "sync/SyncPending.h"

inline constexpr size_t CLIPPING_CHAPTER_TITLE_MAX = 48;
// Clipping text lives on the SD card rather than in every in-memory clipping
// record. Match the reader's bounded selection-text budget so previews retain
// a complete multi-paragraph selection without growing the saved-item index.
inline constexpr size_t CLIPPING_TEXT_MAX = 4U * 1024U;
inline constexpr uint16_t CLIPPING_MAX_PER_BOOK = 256;
inline constexpr uint16_t CLIPPING_MAX_PAGE_MATCHES = 16;
inline constexpr uint32_t CLIPPING_WORD_LAYOUT_VERSION = 2;
inline constexpr uint8_t CLIPPING_LAYOUT_START_RESOLVED = 1U << 0;
inline constexpr uint8_t CLIPPING_LAYOUT_END_RESOLVED = 1U << 1;
inline constexpr uint8_t CLIPPING_LAYOUT_BOUNDARIES_RESOLVED =
    CLIPPING_LAYOUT_START_RESOLVED | CLIPPING_LAYOUT_END_RESOLVED;

inline uint32_t clippingWordLayoutSignature(const uint32_t readerLayoutSignature) {
  if (readerLayoutSignature == 0) return 0;
  uint32_t signature = readerLayoutSignature;
  signature ^= CLIPPING_WORD_LAYOUT_VERSION;
  signature *= 16777619U;
  return signature == 0 ? 1 : signature;
}

// Legacy values were millis()/1000 (at most one 32-bit millisecond wrap).
// Zero is the wire/storage sentinel for an unknown calendar creation date.
inline constexpr uint32_t clippingUnixTimestamp(uint32_t saved) { return saved >= 946684800U ? saved : 0; }

struct Clipping {
  uint16_t spineIndex = 0;
  uint16_t startPage = 0;
  uint16_t endPage = 0;
  uint16_t pageCount = 1;
  uint16_t startWordIndex = 0;
  uint16_t endWordIndex = 0;
  uint16_t wordCount = 0;
  uint16_t paragraphIndex = UINT16_MAX;
  uint32_t timestamp = 0;
  uint32_t layoutSignature = 0;
  uint32_t textOffset = 0;
  uint16_t textLength = 0;
  uint16_t tableSelection = UINT16_MAX;
  // Session-only migration state, intentionally omitted from the on-disk
  // record.
  uint8_t resolvedLayoutBoundaries = 0;
  // Sync keys stay on SD. Their length locates them relative to textOffset.
  uint8_t syncKeyLength = 0;
  bool uploaded = false;
  bool scoped = false;
  char chapterTitle[CLIPPING_CHAPTER_TITLE_MAX] = {};
};

// Checked, bounded index storage: vector::reserve aborts on allocation failure
// in firmware builds with exceptions disabled. Text remains on the SD card.
class ClippingRecords {
 public:
  ClippingRecords() = default;
  ClippingRecords(ClippingRecords&& other) noexcept { *this = std::move(other); }
  ClippingRecords& operator=(ClippingRecords&& other) noexcept {
    if (this != &other) {
      records = std::move(other.records);
      count = std::exchange(other.count, 0);
      reserved = std::exchange(other.reserved, 0);
    }
    return *this;
  }
  bool reserve(size_t capacity) {
    if (capacity <= reserved) return true;
    if (capacity > CLIPPING_MAX_PER_BOOK) return false;
    // At most 256 records, about 24 KiB: runtime-sized and too large for stack.
    auto next = makeUniqueNoThrow<Clipping[]>(capacity);
    if (!next) return false;
    if (count) std::copy_n(records.get(), count, next.get());
    records = std::move(next);
    reserved = capacity;
    return true;
  }
  bool push_back(const Clipping& clipping) {
    if (count == reserved && !reserve(std::min<size_t>(CLIPPING_MAX_PER_BOOK, std::max<size_t>(4, count * 2))))
      return false;
    if (count == reserved) return false;
    records[count++] = clipping;
    return true;
  }
  void pop_back() {
    if (count) --count;
  }
  void erase(const Clipping* position) {
    const size_t index = position - records.get();
    std::move(records.get() + index + 1, records.get() + count, records.get() + index);
    --count;
  }
  void insert(const Clipping* position, const Clipping& clipping) {
    // Only used to restore an erased record; capacity is already available.
    const size_t index = position - records.get();
    std::move_backward(records.get() + index, records.get() + count, records.get() + count + 1);
    records[index] = clipping;
    ++count;
  }
  void clear() { count = 0; }
  void release() {
    records.reset();
    count = reserved = 0;
  }
  size_t size() const { return count; }
  size_t capacity() const { return reserved; }
  bool empty() const { return count == 0; }
  Clipping* begin() { return records.get(); }
  const Clipping* begin() const { return records.get(); }
  Clipping* end() { return count ? records.get() + count : records.get(); }
  const Clipping* end() const { return count ? records.get() + count : records.get(); }
  Clipping& front() { return records[0]; }
  Clipping& operator[](size_t i) { return records[i]; }
  const Clipping& operator[](size_t i) const { return records[i]; }

 private:
  std::unique_ptr<Clipping[]> records;
  size_t count = 0;
  size_t reserved = 0;
};

struct ClippedBookEntry {
  std::string bookTitle;
  std::string bookAuthor;
  std::string bookPath;
  std::string bookType;
  uint16_t count = 0;
};

class ClippingStore {
 public:
  enum class AddResult : uint8_t {
    Added,
    LimitReached,
    SaveFailed,
  };

  struct RenameMigration {
    std::string sourcePath;
    std::string destinationPath;
    std::string destinationBackupPath;
    bool destinationBackedUp = false;
    bool active = false;
  };

  static ClippingStore& getInstance() { return instance; }

  bool loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                   const std::string& bookType);
  void unload();

  AddResult addClipping(uint16_t spineIndex, uint16_t startPage, uint16_t endPage, uint16_t pageCount,
                        uint16_t startWordIndex, uint16_t endWordIndex, uint16_t wordCount, const char* chapterTitle,
                        uint16_t paragraphIndex, const std::string& text, uint16_t tableSelection,
                        uint32_t layoutSignature);
  bool removeClippingAt(size_t index);
  bool saveToFile();
  void clearAll();

  bool hasClippings() const { return !clippings.empty(); }
  size_t clippingCount() const { return clippings.size(); }
  const Clipping* clippingAt(size_t index) const;
  const ClippingRecords& getClippings() const { return clippings; }
  bool cacheResolvedLayoutRange(size_t index, uint16_t page, uint16_t startWord, uint16_t endWord,
                                uint32_t layoutSignature);
  bool readClippingText(size_t index, std::string& out) const;
  bool readClippingPreview(size_t index, std::string& out) const;
  bool readClippingText(const Clipping& clipping, std::string& out) const;

  // Read a single saved record for upload without loading the book's full index.
  // Missing files/end of records are successful with done=true; corrupt data fails.
  static bool readForUpload(const std::string& filePath, size_t ordinal, Clipping& clipping, std::string& text,
                            bool& done, std::string* key = nullptr);
  static bool readDeletionForUpload(const std::string& path, size_t ordinal, std::string& id, bool& done);
  static std::string syncStatePath(const std::string& path);
  static bool prepareUploads(const std::string& path, const std::string& scope);
  static bool acknowledgeDeletions(const std::string& path, const std::string& scope);
  static bool mergeFromSync(const std::string& path, const Clipping& clipping, const std::string& text,
                            const std::string& key, bool deleted, bool* full = nullptr);
  static bool hasAnyClippings();
  static bool getAllClippedBooks(std::vector<ClippedBookEntry>& out);
  enum class ClippedBookStep : uint8_t { Book, Skip, Done, Error };
  // Streams the clippings folder one file per call, with no list in RAM and no
  // handle held between calls. `position` starts at 0 and is advanced here.
  // Book: an EPUB with saved clippings that still exists at `bookPath`.
  static ClippedBookStep nextClippedEpub(uint64_t& position, std::string& bookPath);
  static void deleteForFilePath(const std::string& filePath, const std::string& bookType);
  static bool migrateForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                 const std::string& title, const std::string& author, const std::string& bookType);
  static bool hasStoredStateForFilePath(const std::string& filePath, const std::string& bookType);
  static bool beginRenameMigration(const std::string& oldFilePath, const std::string& newFilePath,
                                   const std::string& title, const std::string& author, const std::string& bookType,
                                   RenameMigration& migration);
  static bool commitRenameMigration(RenameMigration& migration);
  static bool rollbackRenameMigration(RenameMigration& migration);

 private:
  static ClippingStore instance;

  ClippingRecords clippings;
  std::string bookFilePath;
  std::string bookTitle;
  std::string bookAuthor;
  std::string storeFilePath;
  bool dirty = false;
  std::vector<std::string> deletedIds;  // At most 256 pending deletions, persisted with the live records.
  bool loaded = false;                  // Failed or partial loads must never authorize writes.
  // Queues the book for the next Sync Clippings, once per load.
  SyncPending::SessionMarker pendingSync;

  bool readKey(const Clipping& clip, std::string& key) const;
  bool loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                   const std::string& bookType, bool reserveImportSlot);
  static bool loadForSync(const std::string& path, ClippingStore& store, bool reserveImportSlot = false);
  bool readFromFile(const std::string& path, ClippingRecords& out, size_t onlyIndex = SIZE_MAX,
                    bool reserveImportSlot = false);
  struct UploadBinding {
    const std::string& scope;
    std::vector<std::string> legacyIds;
  };
  struct Rewrite {
    size_t index = SIZE_MAX;
    const std::string* text = nullptr;
    const std::string* key = nullptr;
    const std::string* sourcePath = nullptr;
    UploadBinding* binding = nullptr;
  };
  bool writeToFile(const Rewrite& rewrite);
};

inline bool clippingStoredRangeMatchesLayout(const Clipping& clipping, const uint16_t currentPageCount,
                                             const uint32_t currentLayoutSignature) {
  if (clipping.pageCount != currentPageCount) return false;
  // Legacy records have no signature, so their body-only word ordinals cannot
  // be trusted now that table words participate in clipping. Their saved text
  // is used to find the highlight instead.
  return clipping.layoutSignature != 0 && currentLayoutSignature != 0 &&
         clipping.layoutSignature == currentLayoutSignature;
}

inline bool clippingUsesLegacyWordLayout(const Clipping& clipping, const uint16_t currentPageCount,
                                         const uint32_t readerLayoutSignature) {
  if (clipping.pageCount != currentPageCount || clipping.layoutSignature == 0 || readerLayoutSignature == 0) {
    return false;
  }
  return clipping.layoutSignature == readerLayoutSignature &&
         clipping.layoutSignature != clippingWordLayoutSignature(readerLayoutSignature);
}

inline bool clippingCachedRangeReadyOnPage(const Clipping& clipping, const uint16_t page) {
  if (clipping.startPage > clipping.endPage || page < clipping.startPage || page > clipping.endPage) {
    return false;
  }
  if (page == clipping.startPage && (clipping.resolvedLayoutBoundaries & CLIPPING_LAYOUT_START_RESOLVED) == 0) {
    return false;
  }
  if (page == clipping.endPage && (clipping.resolvedLayoutBoundaries & CLIPPING_LAYOUT_END_RESOLVED) == 0) {
    return false;
  }
  return true;
}

inline bool cacheClippingResolvedLayoutRange(Clipping& clipping, const uint16_t page, const uint16_t startWord,
                                             const uint16_t endWord, const uint32_t layoutSignature) {
  if (layoutSignature == 0 || clipping.startPage > clipping.endPage || page < clipping.startPage ||
      page > clipping.endPage || startWord > endWord) {
    return false;
  }

  bool changed = false;
  if (page == clipping.startPage) {
    changed = clipping.startWordIndex != startWord ||
              (clipping.resolvedLayoutBoundaries & CLIPPING_LAYOUT_START_RESOLVED) == 0;
    clipping.startWordIndex = startWord;
    clipping.resolvedLayoutBoundaries |= CLIPPING_LAYOUT_START_RESOLVED;
  }
  if (page == clipping.endPage) {
    changed = changed || clipping.endWordIndex != endWord ||
              (clipping.resolvedLayoutBoundaries & CLIPPING_LAYOUT_END_RESOLVED) == 0;
    clipping.endWordIndex = endWord;
    clipping.resolvedLayoutBoundaries |= CLIPPING_LAYOUT_END_RESOLVED;
  }
  if ((clipping.resolvedLayoutBoundaries & CLIPPING_LAYOUT_BOUNDARIES_RESOLVED) ==
          CLIPPING_LAYOUT_BOUNDARIES_RESOLVED &&
      clipping.layoutSignature != layoutSignature) {
    clipping.layoutSignature = layoutSignature;
    changed = true;
  }
  return changed;
}

#define CLIPPINGS ClippingStore::getInstance()
