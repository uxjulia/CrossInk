#pragma once

// Read side of the CLX1 index.
//
// Holds one open file handle. Browsers can opt into a bounded single-row cache;
// nothing proportional to the library stays resident. The fixed record stride
// locates record k at recordStart + 128k without a resident offset table.

#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>

#include "LibraryFormat.h"

namespace library {

enum class SortOrder : uint8_t {
  // "Recent" orders by file creation time, oldest first in Asc; firstSeen
  // breaks ties and orders files without creation timestamps.
  RecentAsc,
  RecentDesc,
  TitleAsc,
  TitleDesc,
  AuthorAsc,
  AuthorDesc,
  AuthorFirstAsc,
  AuthorFirstDesc,
  SeriesAsc,
  SeriesDesc,
  GenreAsc,
  GenreDesc,
};

// One book to locate in the index: the complete-path hash (clixPathHash) is
// the identity; fileSize, when nonzero, is a cheap in-record prefilter that
// avoids reading the hash blob for most records. Zero means size unknown and
// every record's hash is checked.
struct BookIdentity {
  uint64_t pathHash;
  uint32_t fileSize;
};

class LibraryIndexFile {
 public:
  explicit LibraryIndexFile(bool cacheDisplayRow = false) : cacheDisplayRow(cacheDisplayRow) {}
  ~LibraryIndexFile();
  LibraryIndexFile(const LibraryIndexFile&) = delete;
  LibraryIndexFile& operator=(const LibraryIndexFile&) = delete;

  // Open and validate. On failure `validity()` says why, so a rebuild loop
  // caused by a format bug is visible in the log rather than looking like a slow
  // first boot.
  bool open(const char* path);
  // Accept an otherwise valid stale fold so a rebuild can preserve arrival
  // history without exposing stale sort/search keys to the browser.
  bool openForReconciliation(const char* path);
  void close();
  bool isOpen() const { return opened; }
  bool ioFailed() const { return readFailed; }

  ClixValidity validity() const { return lastValidity; }
  const ClixHeader& header() const { return head; }
  uint16_t bookCount() const { return opened ? head.bookCount : 0; }
  bool ranksDegraded() const { return opened && (head.flags & CLIX_FLAG_RANKS_DEGRADED) != 0; }
  bool dedupDegraded() const { return opened && (head.flags & CLIX_FLAG_DEDUP_DEGRADED) != 0; }

  // Record ordinal of the row at display position `row` in `order`. Returns
  // 0xFFFF when out of range, which callers treat as "no such row" rather than
  // indexing anyway.
  uint16_t ordinalForRow(SortOrder order, uint16_t row);

  // Display rows (RecentAsc space) of up to MAX_IDENTITY_LOOKUPS books, 0xFFFF
  // for books not in the index. One chunked pass over the record section plus
  // one over the arrival permutation, so cost is bounded by the library, not by
  // `count` — callers batch their lookups instead of calling per book.
  static constexpr size_t MAX_IDENTITY_LOOKUPS = 16;
  bool recentRowsFor(const BookIdentity* books, size_t count, uint16_t* outRows);

  bool readRecord(uint16_t ordinal, ClixRecord& out);
  // V5 creation timestamp in the title-ordered side array. Zero means the
  // filesystem provided no creation time. Older versions have no such array.
  bool readCreationTime(uint16_t ordinal, uint32_t& out);
  // Persisted complete-path fingerprint used by rebuild reconciliation.
  bool readPathHash(const ClixRecord& record, uint64_t& out);

  // Display basename, exactly as it sits on the card. This is the only string
  // the UI draws, and it is never shortened on disk.
  bool readName(const ClixRecord& record, std::string& out);
  // The author the build settled on, stored right after the name. Reading it
  // rather than re-deriving it from the name is what makes the metadata pass and
  // the spelling harmonisation visible: neither survives a filename that no
  // longer carries "Title - Author".
  bool readAuthor(const ClixRecord& record, std::string& out);
  bool readTitle(const ClixRecord& record, std::string& out);
  // Checked display text for browsing/search. Empty metadata is valid: use
  // the filename stem for a missing title and leave a missing author blank.
  bool readDisplayText(const ClixRecord& record, std::string& title, std::string& author);
  // Cleaned author spelling before the library-wide spelling vote. Empty is a
  // valid value, so success is independent of `out.empty()`.
  bool readSourceAuthor(const ClixRecord& record, std::string& out);
  bool readSeries(const ClixRecord& record, std::string& out);
  bool readGenre(const ClixRecord& record, std::string& out);
  // V6 sortable signed float bits; UINT32_MAX means no usable order.
  bool readSeriesPosition(const ClixRecord& record, uint32_t& out);

  // Absolute path of the book, rebuilt from its folder record.
  bool readPath(const ClixRecord& record, std::string& out);
  // A small, caller-owned offset table bounds folder walks during full-library scans.
  static constexpr uint16_t FOLDER_CHECKPOINT_COUNT = 128;
  bool buildFolderCheckpoints(uint32_t* offsets, uint16_t& stride);
  bool readPath(const ClixRecord& record, std::string& out, const uint32_t* offsets, uint16_t stride);

 private:
  bool openImpl(const char* path, bool acceptStaleFold);
  bool readAt(uint32_t offset, void* dst, size_t len);
  bool readBlobField(const ClixRecord& record, uint8_t field, std::string& out);

  // Optional browser scratch: one record and at most one serialized row, never
  // proportional to book count. Heap-owned to keep scan callers' stacks small.
  struct DisplayRowCache {
    static constexpr size_t MAX_BLOB_BYTES = sizeof(uint64_t) + UINT8_MAX + 5 * (1 + UINT8_MAX) + sizeof(uint32_t);
    ClixRecord record{};
    uint16_t ordinal = UINT16_MAX;
    uint8_t bytes[MAX_BLOB_BYTES];
    uint32_t nameOff = UINT32_MAX;
    uint8_t nameLen = 0;
    uint16_t fieldOffsets[5]{};
    uint8_t fieldCount = 0;
  };
  static_assert(sizeof(DisplayRowCache) <= 1800);
  bool ensureDisplayRowCache();
  bool cacheBlob(const ClixRecord& record);
  bool cachedBlobMatches(const ClixRecord& record) const;
  const bool cacheDisplayRow;
  bool cacheAllocationAttempted = false;
  std::unique_ptr<DisplayRowCache> displayRowCache;
  HalFile file;
  ClixHeader head{};
  bool opened = false;
  bool readFailed = false;
  ClixValidity lastValidity = ClixValidity::BadMagic;
};

}  // namespace library
