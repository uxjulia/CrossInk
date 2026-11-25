#pragma once
#include <Arena.h>
#include <Print.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "Epub.h"
#include "expat.h"

class BookMetadataCache;

class ContentOpfParser final : public Print {
  enum ParserState {
    START,
    IN_PACKAGE,
    IN_METADATA,
    IN_BOOK_TITLE,
    IN_BOOK_AUTHOR,
    IN_BOOK_LANGUAGE,
    IN_BOOK_IDENTIFIER,
    IN_BOOK_SUBJECT,
    IN_BOOK_COLLECTION,
    IN_BOOK_COLLECTION_TYPE,
    IN_BOOK_COLLECTION_POSITION,
    IN_MANIFEST,
    IN_SPINE,
    IN_GUIDE,
  };

  const std::string& cachePath;
  const std::string& baseContentPath;
  size_t remainingSize;
  XML_Parser parser = nullptr;
  ParserState state = START;
  BookMetadataCache* cache;
  const bool metadataOnly;
  bool metadataComplete = false;
  HalFile tempItemStore;
  std::string coverItemId;
  Arena itemIndexArena;
  bool parseFailed = false;
  bool lowMemoryFailure = false;
  // XML character data can arrive in several write() calls for one text node
  // (notably around entity references). Tracked as element state rather than
  // inferred per-callback, so a title or author split across callbacks still
  // collapses whitespace and separators correctly.
  bool metadataSpacePending = false;
  bool authorSeparatorPending = false;
  bool titleTruncated = false;
  bool authorTruncated = false;
  bool languageTruncated = false;
  bool identifierTruncated = false;
  std::string identifierText;
  uint8_t identifierKind = 0;
  bool subjectTruncated = false;
  bool seriesTruncated = false;
  bool collectionTypeTruncated = false;
  bool collectionPositionTruncated = false;
  // ID-based resolution (jadehawk, CrossPoint #3804), with one bounded shared
  // arena instead of eight independently allocated 512-byte IDs/titles/indexes.
  static constexpr size_t COLLECTION_ARENA_BYTES = 2048;
  static constexpr size_t MAX_COLLECTIONS = 8;
  struct Collection {
    uint16_t id = 0, title = 0, position = 0;  // arena offset + 1; zero is absent
    bool isSeries = false;
    bool invalid = false;
  };
  Collection collections[MAX_COLLECTIONS]{};
  std::unique_ptr<char[]> collectionArena;
  uint16_t collectionBytes = 0;
  uint8_t collectionCount = 0;
  int8_t activeCollection = -1;
  std::string collectionText;  // one existing 512-byte UTF-8-limited text accumulator
  uint16_t storeCollectionText(const char* text, size_t length);
  int8_t findCollection(const char* id);
  bool hasExplicitStartReference = false;
  bool collectCssFiles = true;

  // Index for compact idref->href lookup. The temp manifest rows retain the
  // full ID for collision-safe matching without retaining IDs in heap memory.
  struct ItemIndexEntry {
    uint64_t idHash;      // FNV-1a hash of itemId
    uint16_t idLen;       // length for collision reduction
    uint32_t fileOffset;  // offset in .items.bin
  };
  static constexpr size_t ITEM_INDEX_CHUNK_CAPACITY = 240;
  struct ItemIndexChunk {
    ItemIndexChunk* next = nullptr;
    uint16_t count = 0;
    ItemIndexEntry entries[ITEM_INDEX_CHUNK_CAPACITY];
  };
  ItemIndexChunk* itemIndexHead = nullptr;
  ItemIndexChunk* itemIndexTail = nullptr;
  size_t itemIndexCount = 0;
  size_t itemIndexChunkCount = 0;

  bool appendItemIndexEntry(const ItemIndexEntry& entry);
  void sortItemIndexChunks();
  bool findItemHref(const std::string& idref, std::string& href);
  static bool itemIndexEntryLess(const ItemIndexEntry& lhs, const ItemIndexEntry& rhs) {
    return lhs.idHash < rhs.idHash || (lhs.idHash == rhs.idHash && lhs.idLen < rhs.idLen);
  }

  // FNV-1a hash function
  static uint64_t fnvHash(const char* s, size_t len) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; ++i) {
      hash ^= static_cast<uint8_t>(s[i]);
      hash *= 1099511628211ull;
    }
    return hash;
  }
  static uint64_t fnvHash(const std::string& s) { return fnvHash(s.c_str(), s.size()); }
  static uint64_t fnvHash(const char* s) {
    if (!s) return 0;
    uint64_t hash = 14695981039346656037ull;
    while (*s != '\0') {
      hash ^= static_cast<uint8_t>(*s);
      hash *= 1099511628211ull;
      ++s;
    }
    return hash;
  }

  static void startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void characterData(void* userData, const XML_Char* s, int len);
  static void endElement(void* userData, const XML_Char* name);

 public:
  std::string title;
  std::string author;
  std::string language;
  std::string isbn;
  std::string asin;
  std::string subject;
  std::string series;
  std::string seriesIndex;
  std::string tocNcxPath;
  std::string tocNavPath;        // EPUB 3 nav document path
  std::string guideTocPageHref;  // EPUB 2 guide TOC page, if declared
  std::string coverItemHref;
  std::string guideCoverPageHref;  // Guide reference with type="cover" or "cover-page" (points to XHTML wrapper)
  std::string textReferenceHref;
  std::vector<std::string> cssFiles;  // CSS stylesheet paths

  explicit ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, const size_t xmlSize,
                            BookMetadataCache* cache, const bool collectCssFiles = true,
                            const bool metadataOnly = false)
      : cachePath(cachePath),
        baseContentPath(baseContentPath),
        remainingSize(xmlSize),
        cache(cache),
        metadataOnly(metadataOnly),
        collectCssFiles(collectCssFiles) {}
  ~ContentOpfParser() override;

  bool setup();
  bool failedForLowMemory() const { return lowMemoryFailure; }

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
