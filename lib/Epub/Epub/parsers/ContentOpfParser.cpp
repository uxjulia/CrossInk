#include "ContentOpfParser.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <Utf8.h>
#include <XmlParserUtils.h>

#include <cctype>
#include <cstring>

#include "Epub/BookMetadataCache.h"

namespace {
// Expat expands namespace prefixes to URIs, so optimizer-generated prefixes
// such as ns0 are equivalent to opf or the default OPF namespace.
constexpr char OPF_NAMESPACE[] = "http://www.idpf.org/2007/opf|";
constexpr char DC_NAMESPACE[] = "http://purl.org/dc/elements/1.1/|";

bool isOpfElement(const char* name, const char* localName) {
  constexpr size_t prefixLength = sizeof(OPF_NAMESPACE) - 1;
  if (strncmp(name, OPF_NAMESPACE, prefixLength) == 0) {
    return strcmp(name + prefixLength, localName) == 0;
  }
  // Keep support for older packages without a declared OPF namespace.
  return strcmp(name, localName) == 0;
}

bool isDcElement(const char* name, const char* localName) {
  constexpr size_t prefixLength = sizeof(DC_NAMESPACE) - 1;
  return strncmp(name, DC_NAMESPACE, prefixLength) == 0 && strcmp(name + prefixLength, localName) == 0;
}

constexpr char MEDIA_TYPE_NCX[] = "application/x-dtbncx+xml";
constexpr char MEDIA_TYPE_CSS[] = "text/css";
constexpr char MEDIA_TYPE_IMAGE_PREFIX[] = "image/";
constexpr char itemCacheFile[] = "/.items.bin";
constexpr size_t ITEM_INDEX_ARENA_SLAB_BYTES = 4096;

bool startsWithImageMediaType(const std::string& mediaType) {
  constexpr size_t prefixLen = sizeof(MEDIA_TYPE_IMAGE_PREFIX) - 1;
  if (mediaType.size() < prefixLen) {
    return false;
  }

  for (size_t i = 0; i < prefixLen; ++i) {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(mediaType[i])));
    if (c != MEDIA_TYPE_IMAGE_PREFIX[i]) {
      return false;
    }
  }

  return true;
}

bool isXmlWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Metadata text comes straight from the (untrusted) OPF; unbounded growth on a
// maliciously large title would exhaust the heap. Downstream consumers
// truncate far below this anyway, so overflow is clamped, not fatal.
constexpr size_t MAX_METADATA_TEXT = 512;

// Appends one characterData() chunk to a metadata field, collapsing XML
// whitespace runs to a single space and (for multi-author dc:creator) joining
// entries with ", " — done here rather than per-callback because expat can
// split one text node into several write() calls, notably around entity
// references, and title/author must read the same either way.
void appendMetadataText(std::string& out, const XML_Char* text, const int len, bool& spacePending, bool& truncated,
                        bool* separatorPending = nullptr) {
  if (truncated) return;
  if (out.size() >= MAX_METADATA_TEXT) {
    out.resize(static_cast<size_t>(utf8SafeTruncateBuffer(out.data(), static_cast<int>(out.size()))));
    truncated = true;
    return;
  }
  for (int i = 0; i < len; i++) {
    const char c = text[i];
    if (isXmlWhitespace(c)) {
      spacePending = true;
      continue;
    }

    // Check capacity against the WHOLE unit about to be appended (separator
    // or space, plus the character), not just the character: checking the
    // character alone let a pending ", " or ' ' push `out` a byte or two past
    // the cap right at a multi-<dc:creator> boundary.
    const bool useSeparator = separatorPending != nullptr && *separatorPending;
    const bool useSpace = !useSeparator && spacePending && !out.empty();
    const size_t prefixLen = useSeparator ? 2 : (useSpace ? 1 : 0);
    if (out.size() + prefixLen + 1 > MAX_METADATA_TEXT) {
      LOG_DBG("COF", "Metadata text exceeds %u bytes; truncating", static_cast<unsigned>(MAX_METADATA_TEXT));
      out.resize(static_cast<size_t>(utf8SafeTruncateBuffer(out.data(), static_cast<int>(out.size()))));
      truncated = true;
      return;
    }
    if (useSeparator) {
      out.append(", ");
      *separatorPending = false;
    } else if (useSpace) {
      out.push_back(' ');
    }
    spacePending = false;
    out.push_back(c);
  }
}

bool readItemIdMatches(HalFile& file, const std::string& targetId, bool& matches) {
  uint32_t storedLength = 0;
  if (!serialization::tryReadPod(file, storedLength)) {
    return false;
  }

  const uint64_t idEnd = static_cast<uint64_t>(file.position()) + storedLength;
  if (idEnd > file.size()) {
    return false;
  }

  matches = storedLength == targetId.size();
  if (!matches) {
    return file.seekCur(static_cast<int64_t>(storedLength));
  }

  constexpr size_t COMPARE_BUFFER_SIZE = 64;
  uint8_t compareBuffer[COMPARE_BUFFER_SIZE];
  size_t compared = 0;
  while (compared < storedLength) {
    const size_t chunkSize = std::min(COMPARE_BUFFER_SIZE, static_cast<size_t>(storedLength) - compared);
    if (file.read(compareBuffer, chunkSize) != chunkSize) {
      return false;
    }
    if (std::memcmp(compareBuffer, targetId.data() + compared, chunkSize) != 0) {
      matches = false;
    }
    compared += chunkSize;
  }
  return true;
}
}  // namespace

bool ContentOpfParser::appendItemIndexEntry(const ItemIndexEntry& entry) {
  if (!itemIndexTail || itemIndexTail->count == ITEM_INDEX_CHUNK_CAPACITY) {
    auto* const chunk = arenaNew<ItemIndexChunk>(itemIndexArena);
    if (!chunk) {
      return false;
    }
    if (itemIndexTail) {
      itemIndexTail->next = chunk;
    } else {
      itemIndexHead = chunk;
    }
    itemIndexTail = chunk;
    ++itemIndexChunkCount;
  }

  itemIndexTail->entries[itemIndexTail->count++] = entry;
  ++itemIndexCount;
  return true;
}

void ContentOpfParser::sortItemIndexChunks() {
  for (auto* chunk = itemIndexHead; chunk; chunk = chunk->next) {
    std::sort(chunk->entries, chunk->entries + chunk->count, itemIndexEntryLess);
  }
}

bool ContentOpfParser::findItemHref(const std::string& idref, std::string& href) {
  if (!tempItemStore) {
    return false;
  }

  const uint64_t targetHash = fnvHash(idref);
  const uint16_t targetLen = static_cast<uint16_t>(idref.size());
  const ItemIndexEntry target{targetHash, targetLen, 0};
  for (auto* chunk = itemIndexHead; chunk; chunk = chunk->next) {
    auto* const begin = chunk->entries;
    auto* const end = begin + chunk->count;
    auto* it = std::lower_bound(begin, end, target, itemIndexEntryLess);
    while (it != end && it->idHash == targetHash && it->idLen == targetLen) {
      if (!tempItemStore.seek(it->fileOffset)) {
        LOG_ERR("COF", "Failed seeking manifest index row at %u", static_cast<unsigned>(it->fileOffset));
        return false;
      }

      uint64_t rowHash = 0;
      uint16_t rowLen = 0;
      if (!serialization::tryReadPod(tempItemStore, rowHash) || !serialization::tryReadPod(tempItemStore, rowLen)) {
        LOG_ERR("COF", "Failed reading manifest index row at %u", static_cast<unsigned>(it->fileOffset));
        return false;
      }

      if (rowHash == targetHash && rowLen == targetLen) {
        bool idMatches = false;
        if (!readItemIdMatches(tempItemStore, idref, idMatches)) {
          LOG_ERR("COF", "Failed reading manifest item ID at %u", static_cast<unsigned>(it->fileOffset));
          return false;
        }
        if (idMatches && !serialization::tryReadString(tempItemStore, href)) {
          LOG_ERR("COF", "Failed reading manifest item href at %u", static_cast<unsigned>(it->fileOffset));
          return false;
        }
        if (idMatches) {
          return true;
        }
      }
      ++it;
    }
  }
  return false;
}

uint16_t ContentOpfParser::storeCollectionText(const char* text, const size_t length) {
  if (!collectionArena || length + 1 > COLLECTION_ARENA_BYTES - collectionBytes) {
    LOG_DBG("COF", "Collection metadata exceeds bounded arena; ignoring candidate");
    return 0;
  }
  const uint16_t offset = collectionBytes;
  memcpy(collectionArena.get() + offset, text, length);
  collectionArena[offset + length] = '\0';
  collectionBytes += length + 1;
  return offset + 1;
}

int8_t ContentOpfParser::findCollection(const char* id) {
  // IDs must match exactly: rejecting oversized IDs avoids truncated-prefix aliases.
  if (!id || !*id || strlen(id) > MAX_METADATA_TEXT) return -1;
  for (uint8_t i = 0; i < collectionCount; ++i) {
    if (strcmp(collectionArena.get() + collections[i].id - 1, id) == 0) return i;
  }
  if (collectionCount == MAX_COLLECTIONS) return -1;
  if (!collectionArena) {
    // Lazy 2 KiB allocation, held only through metadata; too large for C3 task stacks.
    collectionArena = makeUniqueNoThrow<char[]>(COLLECTION_ARENA_BYTES);
    if (!collectionArena) {
      LOG_ERR("COF", "Failed to allocate collection metadata arena");
      return -1;
    }
  }
  const uint16_t offset = storeCollectionText(id, strlen(id));
  if (!offset) return -1;
  collections[collectionCount] = Collection{};
  collections[collectionCount].id = offset;
  return collectionCount++;
}

bool ContentOpfParser::setup() {
  if (!metadataOnly && !itemIndexArena.init(ITEM_INDEX_ARENA_SLAB_BYTES)) {
    LOG_ERR("COF", "Failed to allocate manifest index arena (%u bytes)",
            static_cast<unsigned>(ITEM_INDEX_ARENA_SLAB_BYTES));
    lowMemoryFailure = true;
    return false;
  }

  parser = XML_ParserCreateNS(nullptr, '|');
  if (!parser) {
    LOG_DBG("COF", "Couldn't allocate memory for parser");
    lowMemoryFailure = true;
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

ContentOpfParser::~ContentOpfParser() {
  destroyXmlParser(parser);
  // metadataOnly stops before <manifest>/<spine>/<guide> ever open the item
  // cache file, so there is nothing below to close or remove.
  if (metadataOnly) {
    return;
  }
  if (tempItemStore) {
    tempItemStore.close();
  }
  const auto itemCachePath = cachePath + itemCacheFile;
  if (Storage.exists(itemCachePath.c_str())) {
    Storage.remove(itemCachePath.c_str());
  }
}

size_t ContentOpfParser::write(const uint8_t data) { return write(&data, 1); }

size_t ContentOpfParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser || parseFailed) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);

    if (!buf) {
      LOG_ERR("COF", "Couldn't allocate memory for buffer");
      lowMemoryFailure = true;
      destroyXmlParser(parser);
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);

    const XML_Status parseStatus = XML_ParseBuffer(parser, static_cast<int>(toRead), remainingSize == toRead);
    if (parseStatus != XML_STATUS_OK) {
      if (!parseFailed) {
        LOG_DBG("COF", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
                XML_ErrorString(XML_GetErrorCode(parser)));
      }
      destroyXmlParser(parser);
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;

    if (metadataOnly && metadataComplete) {
      // Signal the caller to stop feeding bytes (Epub::readItemContentsToStream's
      // allowEarlyStop) with a short count. `size - remainingInBuffer` is how
      // much of THIS call was actually consumed; if that happens to equal the
      // full `size` (metadata ended exactly on a chunk boundary), report one
      // byte short instead so the return value is unambiguously "not all of it".
      const size_t processed = size - remainingInBuffer;
      return processed < size ? processed : size - 1;
    }
  }

  return size;
}

void XMLCALL ContentOpfParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)atts;

  if (self->metadataOnly && self->metadataComplete) {
    return;
  }
  // A metadata-only read is done the moment any of these open: well-formed
  // EPUB2/3 already closed </metadata> first (handled in endElement below),
  // but a malformed package that jumps straight to the manifest must not open
  // the item-cache file just to have this same check discard it a level down.
  if (self->metadataOnly &&
      (isOpfElement(name, "manifest") || isOpfElement(name, "spine") || isOpfElement(name, "guide"))) {
    self->metadataComplete = true;
    return;
  }

  if (self->state == START && isOpfElement(name, "package")) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && isOpfElement(name, "metadata")) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && isDcElement(name, "title")) {
    // Only capture the first dc:title element; subsequent ones are subtitles
    if (self->title.empty()) {
      self->state = IN_BOOK_TITLE;
      self->metadataSpacePending = false;
    }
    return;
  }

  if (self->state == IN_METADATA && isDcElement(name, "creator")) {
    self->state = IN_BOOK_AUTHOR;
    self->metadataSpacePending = false;
    self->authorSeparatorPending = !self->author.empty();
    return;
  }

  if (self->state == IN_METADATA && isDcElement(name, "identifier")) {
    self->state = IN_BOOK_IDENTIFIER;
    self->identifierText.clear();
    self->identifierKind = 0;
    self->identifierTruncated = false;
    self->metadataSpacePending = false;
    for (int i = 0; atts[i]; i += 2) {
      if (!isOpfElement(atts[i], "scheme")) continue;
      if (strcasecmp(atts[i + 1], "ISBN") == 0)
        self->identifierKind = 1;
      else if (strcasecmp(atts[i + 1], "ASIN") == 0 || strcasecmp(atts[i + 1], "MOBI-ASIN") == 0 ||
               strcasecmp(atts[i + 1], "AMAZON") == 0)
        self->identifierKind = 2;
    }
    return;
  }

  if (self->state == IN_METADATA && isDcElement(name, "language")) {
    self->state = IN_BOOK_LANGUAGE;
    self->metadataSpacePending = false;
    return;
  }

  if (self->state == IN_METADATA && isDcElement(name, "subject")) {
    // Subjects are free-form tags; keep the first one as the Library genre.
    if (self->subject.empty()) {
      self->state = IN_BOOK_SUBJECT;
      self->metadataSpacePending = false;
    }
    return;
  }

  if (self->state == IN_PACKAGE && isOpfElement(name, "manifest")) {
    self->state = IN_MANIFEST;
    if (!Storage.openFileForWrite("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for writing. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_PACKAGE && isOpfElement(name, "spine")) {
    self->state = IN_SPINE;
    if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }

    // Sort each fixed-capacity chunk so every idref lookup can use binary search.
    self->sortItemIndexChunks();
    LOG_DBG("COF", "Using chunked manifest index for %zu items in %zu chunks (arena high-water=%u bytes)",
            self->itemIndexCount, self->itemIndexChunkCount, static_cast<unsigned>(self->itemIndexArena.used()));
    return;
  }

  if (self->state == IN_PACKAGE && isOpfElement(name, "guide")) {
    self->state = IN_GUIDE;
    // TODO Remove print
    if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_METADATA && isOpfElement(name, "meta")) {
    bool isCover = false;
    bool isSeries = false;
    bool isSeriesIndex = false;
    bool isCollection = false;
    bool isCollectionType = false;
    bool isCollectionPosition = false;
    const char* content = nullptr;
    const char* id = nullptr;
    const char* refines = nullptr;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "name") == 0 && strcmp(atts[i + 1], "cover") == 0) {
        isCover = true;
      } else if (strcmp(atts[i], "content") == 0) {
        content = atts[i + 1];
      } else if (strcmp(atts[i], "name") == 0 && strcmp(atts[i + 1], "calibre:series") == 0) {
        isSeries = true;
      } else if (strcmp(atts[i], "name") == 0 && strcmp(atts[i + 1], "calibre:series_index") == 0) {
        isSeriesIndex = true;
      } else if (strcmp(atts[i], "property") == 0 && strcmp(atts[i + 1], "belongs-to-collection") == 0) {
        isCollection = true;
      } else if (strcmp(atts[i], "property") == 0 && strcmp(atts[i + 1], "collection-type") == 0) {
        isCollectionType = true;
      } else if (strcmp(atts[i], "property") == 0 && strcmp(atts[i + 1], "group-position") == 0) {
        isCollectionPosition = true;
      } else if (strcmp(atts[i], "id") == 0) {
        id = atts[i + 1];
      } else if (strcmp(atts[i], "refines") == 0) {
        refines = atts[i + 1];
      }
    }

    if (isCover) {
      if (content) self->coverItemId = content;
    }
    if (isSeries && self->series.empty() && content) {
      const size_t bytes = std::min(strlen(content), MAX_METADATA_TEXT);
      self->series.assign(content, static_cast<size_t>(utf8SafeTruncateBuffer(content, static_cast<int>(bytes))));
    }
    if (isSeriesIndex && self->seriesIndex.empty() && content) {
      const size_t bytes = std::min(strlen(content), MAX_METADATA_TEXT);
      self->seriesIndex.assign(content, static_cast<size_t>(utf8SafeTruncateBuffer(content, static_cast<int>(bytes))));
    }
    if (isCollection || isCollectionType || isCollectionPosition) {
      const char* target = isCollection ? id : (refines && refines[0] == '#' ? refines + 1 : nullptr);
      self->activeCollection = self->findCollection(target);
      if (self->activeCollection >= 0) {
        self->collectionText.clear();
        self->seriesTruncated = false;
        self->collectionTypeTruncated = false;
        self->collectionPositionTruncated = false;
        self->metadataSpacePending = false;
        self->state = isCollection ? IN_BOOK_COLLECTION
                                   : (isCollectionType ? IN_BOOK_COLLECTION_TYPE : IN_BOOK_COLLECTION_POSITION);
      }
    }
    return;
  }

  if (self->state == IN_MANIFEST && isOpfElement(name, "item")) {
    std::string itemId;
    std::string href;
    std::string mediaType;
    std::string properties;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "id") == 0) {
        itemId = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
      } else if (strcmp(atts[i], "media-type") == 0) {
        mediaType = atts[i + 1];
      } else if (strcmp(atts[i], "properties") == 0) {
        properties = atts[i + 1];
      }
    }

    // Record index entry for fast lookup later
    if (self->tempItemStore) {
      ItemIndexEntry entry;
      entry.idHash = fnvHash(itemId);
      entry.idLen = static_cast<uint16_t>(itemId.size());
      entry.fileOffset = static_cast<uint32_t>(self->tempItemStore.position());
      if (!self->appendItemIndexEntry(entry)) {
        LOG_ERR("COF", "Manifest index arena OOM at %zu items", self->itemIndexCount);
        self->parseFailed = true;
        self->lowMemoryFailure = true;
        if (self->parser) {
          XML_StopParser(self->parser, XML_FALSE);
        }
        return;
      }
    }

    // Write manifest rows down to SD card. The full ID makes hash collisions
    // safe while the in-memory index keeps idref lookup fast.
    serialization::writePod(self->tempItemStore, fnvHash(itemId));
    serialization::writePod(self->tempItemStore, static_cast<uint16_t>(itemId.size()));
    serialization::writeString(self->tempItemStore, itemId);
    serialization::writeString(self->tempItemStore, href);

    if (itemId == self->coverItemId) {
      // Some EPUBs set meta name="cover" to an XHTML wrapper item.
      // Only treat it as a cover image when the manifest media-type is image/*.
      if (startsWithImageMediaType(mediaType)) {
        self->coverItemHref = href;
      } else {
        LOG_DBG("COF", "Ignoring meta cover item '%s' with non-image media type: %s", itemId.c_str(),
                mediaType.c_str());
      }
    }

    if (mediaType == MEDIA_TYPE_NCX) {
      if (self->tocNcxPath.empty()) {
        self->tocNcxPath = href;
      } else {
        LOG_DBG("COF", "Warning: Multiple NCX files found in manifest. Ignoring duplicate: %s", href.c_str());
      }
    }

    // Collect CSS files
    if (self->collectCssFiles && mediaType == MEDIA_TYPE_CSS) {
      self->cssFiles.push_back(href);
    }

    // EPUB 3: Check for nav document (properties contains "nav")
    if (!properties.empty() && self->tocNavPath.empty()) {
      // Properties is space-separated, check if "nav" is present as a word
      if (properties == "nav" || properties.find("nav ") == 0 || properties.find(" nav") != std::string::npos) {
        self->tocNavPath = href;
      }
    }

    // EPUB 3: Check for cover image (properties contains "cover-image")
    if (!properties.empty() && self->coverItemHref.empty()) {
      if (properties == "cover-image" || properties.find("cover-image ") == 0 ||
          properties.find(" cover-image") != std::string::npos) {
        self->coverItemHref = href;
      }
    }
    return;
  }

  // NOTE: This relies on spine appearing after item manifest (which is pretty safe as it's part of the EPUB spec)
  // Only run the spine parsing if there's a cache to add it to
  if (self->cache) {
    if (self->state == IN_SPINE && isOpfElement(name, "itemref")) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "idref") == 0) {
          const std::string idref = atts[i + 1];
          std::string href;
          if (self->findItemHref(idref, href)) {
            self->cache->createSpineEntry(href);
          }
        }
      }
      return;
    }
  }
  // parse the guide
  if (self->state == IN_GUIDE && isOpfElement(name, "reference")) {
    std::string type;
    std::string guideHref;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "type") == 0) {
        type = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        guideHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
      }
    }
    if (!guideHref.empty()) {
      // EPUB 2 guides often mark every content file as "text", so that type
      // does not identify a reliable first-reading location. Only use the
      // explicit "start" semantic; otherwise the reader opens at spine index 0.
      if (type == "start" && !self->hasExplicitStartReference) {
        LOG_DBG("COF", "Found %s reference in guide: %s", type.c_str(), guideHref.c_str());
        self->textReferenceHref = guideHref;
        self->hasExplicitStartReference = true;
      } else if (type == "toc" && self->guideTocPageHref.empty()) {
        const auto fragmentPos = guideHref.find('#');
        self->guideTocPageHref = guideHref.substr(0, fragmentPos);
      } else if ((type == "cover" || type == "cover-page") && self->guideCoverPageHref.empty()) {
        self->guideCoverPageHref = guideHref;
      }
    }
    return;
  }
}

void XMLCALL ContentOpfParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ContentOpfParser*>(userData);

  if (self->metadataOnly && self->metadataComplete) {
    return;
  }

  if (self->state == IN_BOOK_TITLE) {
    appendMetadataText(self->title, s, len, self->metadataSpacePending, self->titleTruncated);
    return;
  }

  if (self->state == IN_BOOK_AUTHOR) {
    appendMetadataText(self->author, s, len, self->metadataSpacePending, self->authorTruncated,
                       &self->authorSeparatorPending);
    return;
  }

  if (self->state == IN_BOOK_IDENTIFIER) {
    appendMetadataText(self->identifierText, s, len, self->metadataSpacePending, self->identifierTruncated);
    return;
  }
  if (self->state == IN_BOOK_LANGUAGE) {
    appendMetadataText(self->language, s, len, self->metadataSpacePending, self->languageTruncated);
    return;
  }
  if (self->state == IN_BOOK_SUBJECT) {
    appendMetadataText(self->subject, s, len, self->metadataSpacePending, self->subjectTruncated);
    return;
  }
  if (self->state == IN_BOOK_COLLECTION) {
    appendMetadataText(self->collectionText, s, len, self->metadataSpacePending, self->seriesTruncated);
    return;
  }
  if (self->state == IN_BOOK_COLLECTION_TYPE) {
    appendMetadataText(self->collectionText, s, len, self->metadataSpacePending, self->collectionTypeTruncated);
    return;
  }
  if (self->state == IN_BOOK_COLLECTION_POSITION) {
    appendMetadataText(self->collectionText, s, len, self->metadataSpacePending, self->collectionPositionTruncated);
    return;
  }
}

void XMLCALL ContentOpfParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  (void)name;

  if (self->metadataOnly && self->metadataComplete) {
    return;
  }

  if (self->state == IN_SPINE && isOpfElement(name, "spine")) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_GUIDE && isOpfElement(name, "guide")) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_MANIFEST && isOpfElement(name, "manifest")) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_BOOK_TITLE && isDcElement(name, "title")) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_AUTHOR && isDcElement(name, "creator")) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_IDENTIFIER && isDcElement(name, "identifier")) {
    const char* value = self->identifierText.c_str();
    if (strncasecmp(value, "urn:", 4) == 0) value += 4;
    if (strncasecmp(value, "isbn:", 5) == 0) {
      self->identifierKind = 1;
      value += 5;
    } else if (strncasecmp(value, "asin:", 5) == 0) {
      self->identifierKind = 2;
      value += 5;
    } else
      value = self->identifierText.c_str();
    while (*value == ' ') ++value;
    if (!self->identifierTruncated) {
      if (self->identifierKind == 1 && self->isbn.empty()) self->isbn = value;
      if (self->identifierKind == 2 && self->asin.empty()) self->asin = value;
    }
    self->state = IN_METADATA;
    return;
  }
  if (self->state == IN_BOOK_LANGUAGE && isDcElement(name, "language")) {
    self->state = IN_METADATA;
    return;
  }
  if (self->state == IN_BOOK_SUBJECT && isDcElement(name, "subject")) {
    self->state = IN_METADATA;
    return;
  }
  if ((self->state == IN_BOOK_COLLECTION || self->state == IN_BOOK_COLLECTION_TYPE ||
       self->state == IN_BOOK_COLLECTION_POSITION) &&
      isOpfElement(name, "meta")) {
    if (self->activeCollection >= 0) {
      auto& c = self->collections[self->activeCollection];
      if (self->state == IN_BOOK_COLLECTION_TYPE) {
        c.isSeries = self->collectionText == "series";
      } else {
        const uint16_t offset = self->storeCollectionText(self->collectionText.data(), self->collectionText.size());
        if (!offset)
          c.invalid = true;  // never publish an incomplete or mismatched pair
        else if (self->state == IN_BOOK_COLLECTION)
          c.title = offset;
        else
          c.position = offset;
      }
    }
    self->activeCollection = -1;
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && isOpfElement(name, "metadata")) {
    // Calibre title and index are one source; do not mix in an EPUB3 index.
    if (self->series.empty()) {
      self->seriesIndex.clear();
      for (uint8_t i = 0; i < self->collectionCount; ++i) {
        const auto& c = self->collections[i];
        if (!c.isSeries || c.invalid || !c.title || self->collectionArena[c.title - 1] == '\0') continue;
        self->series = self->collectionArena.get() + c.title - 1;
        if (c.position) self->seriesIndex = self->collectionArena.get() + c.position - 1;
        break;
      }
    }
    self->collectionArena.reset();
    self->collectionCount = 0;
    self->collectionBytes = 0;
    self->state = IN_PACKAGE;
    if (self->metadataOnly) {
      self->metadataComplete = true;
    }
    return;
  }

  if (self->state == IN_PACKAGE && isOpfElement(name, "package")) {
    self->state = START;
    return;
  }
}
