#pragma once
#include <Arena.h>
#include <ArenaVector.h>
#include <Print.h>

#include <algorithm>
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
  HalFile tempItemStore;
  std::string coverItemId;
  Arena itemIndexArena;
  bool parseFailed = false;

  // Index for compact idref->href lookup. The temp manifest rows store only
  // hash/length plus href, not a second full copy of every manifest ID.
  struct ItemIndexEntry {
    uint64_t idHash;      // FNV-1a hash of itemId
    uint16_t idLen;       // length for collision reduction
    uint32_t fileOffset;  // offset in .items.bin
  };
  ArenaVector<ItemIndexEntry> itemIndex;

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
  std::string tocNcxPath;
  std::string tocNavPath;  // EPUB 3 nav document path
  std::string coverItemHref;
  std::string guideCoverPageHref;  // Guide reference with type="cover" or "cover-page" (points to XHTML wrapper)
  std::string textReferenceHref;
  std::vector<std::string> cssFiles;  // CSS stylesheet paths

  explicit ContentOpfParser(const std::string& cachePath, const std::string& baseContentPath, const size_t xmlSize,
                            BookMetadataCache* cache)
      : cachePath(cachePath),
        baseContentPath(baseContentPath),
        remainingSize(xmlSize),
        cache(cache),
        itemIndex(itemIndexArena) {}
  ~ContentOpfParser() override;

  bool setup();

  size_t write(uint8_t) override;
  size_t write(const uint8_t* buffer, size_t size) override;
};
