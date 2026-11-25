#pragma once
#include <Print.h>
#include <StreamingJsonParser.h>

#include <algorithm>
#include <string>

#include "OpdsParser.h"

// OPDS 2 SAX parser adapted from the pinned FreeInk Opds2Parser. Uses the
// same bounded caller-owned storage as CrossInk's Atom parser. Groups and
// facets become navigation rows; unsupported service acquisitions stay disabled.
class OpdsJsonParser final : public Print {
 public:
  OpdsJsonParser(OpdsEntry* buffer, size_t capacity, bool publication = false);

  OpdsJsonParser(const OpdsJsonParser&) = delete;
  OpdsJsonParser& operator=(const OpdsJsonParser&) = delete;

  // Preferred UI language (primary subtag, e.g. "en"). When a title is a
  // localized object ({"fr": "...", "en": "..."}), the matching translation is
  // chosen instead of the first one. Empty keeps first-translation behavior.
  void setPreferredLanguage(const char* lang);

  size_t write(uint8_t c) override;
  size_t write(const uint8_t* data, size_t length) override;
  void flush() override;

  bool error() const;
  bool truncated() const { return feedTruncated; }

  size_t getEntryCount() const { return entryCount; }
  const std::string& getFeedTitle() const { return feedTitle; }
  // Feed pagination (metadata numberOfItems/itemsPerPage/currentPage); 0 when
  // the feed does not report it.
  int currentPage() const { return feedCurrentPage > 0 ? feedCurrentPage : 0; }
  int pageCount() const {
    if (feedNumberOfItems <= 0 || feedItemsPerPage <= 0) return 0;
    return static_cast<int>((static_cast<int64_t>(feedNumberOfItems) + feedItemsPerPage - 1) / feedItemsPerPage);
  }
  const std::string& getSearchTemplate() const { return searchTemplate; }
  const std::string& getNextPageUrl() const { return nextPageUrl; }
  const std::string& getPrevPageUrl() const { return prevPageUrl; }
  const std::string& getFirstPageUrl() const { return firstPageUrl; }
  const std::string& getLastPageUrl() const { return lastPageUrl; }
  // Standard OPDS user-collection links (empty when the feed omits them).
  const std::string& getShelfUrl() const { return shelfUrl; }
  const std::string& getWishlistUrl() const { return wishlistUrl; }
  const std::string& getHistoryUrl() const { return historyUrl; }

 private:
  // Semantic role of each open JSON container, decided from the parent scope
  // and the key that introduced it. SKIP swallows entire subtrees (images,
  // metadata we don't consume, unknown extensions).
  enum class Scope : uint8_t {
    FEED,            // root object
    FEED_META,       // feed "metadata" object
    FEED_TITLE,      // localized feed title object
    FEED_LINKS,      // feed "links" array
    FEED_LINK,       // one feed link object
    LINK_REL,        // "rel" array inside any link object
    NAV,             // "navigation" array (feed or group)
    NAV_LINK,        // one navigation link object
    PUBS,            // "publications" array (feed or group)
    PUB,             // one publication object
    PUB_META,        // publication "metadata" object
    PUB_TITLE,       // localized title object ({"en": "..."})
    AUTHOR,          // contributor object ({"name": ...})
    AUTHOR_ARR,      // contributor array (strings and/or objects)
    PUB_LINKS,       // publication "links" array
    PUB_LINK,        // one publication link object
    PUB_IMAGES,      // publication "images" array (cover art)
    PUB_IMAGE,       // one publication image object
    GROUPS,          // "groups" array
    GROUP,           // one group object
    GROUP_META,      // group "metadata" object
    GROUP_LINKS,     // group "links" array (rel self -> "see all")
    GROUP_LINK,      // one group link object
    FACETS,          // "facets" array
    FACET,           // one facet group object
    FACET_META,      // facet group "metadata" object
    FACET_LINKS,     // facet group "links" array
    FACET_LINK,      // one facet link object
    FACET_PROPS,     // facet link "properties" object (numberOfItems)
    PUB_LINK_PROPS,  // publication link "properties" object
    PRICE,           // "price" object of a buy link ({currency, value})
    SKIP,
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnStringPrefix(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void onContainerStart(bool isObject);
  void onContainerEnd();
  void onStringValue(const char* value, size_t len);
  void onNumberValue(const char* value);

  Scope scopeForChild(Scope parent, bool isObject) const;
  Scope current() const { return depth > 0 ? stack[depth - 1] : Scope::FEED; }

  void resetLink();
  void commitFeedLink();
  void commitPubLink();
  void commitNavLink();
  void commitGroupLink();
  void commitFacetLink();
  void commitPublication();
  void beginGroup();
  void endGroup();
  void applyRel(const char* rel);

  static void assignBounded(std::string& target, const char* value, size_t len, size_t maxLen);

  StreamingJsonParser parser;

  static constexpr uint8_t MAX_DEPTH = StreamingJsonParser::MAX_NESTING + 1;
  Scope stack[MAX_DEPTH];
  uint8_t depth = 0;
  bool sawRoot = false;
  bool errorOccured = false;
  bool feedTruncated = false;

  char pendingKey[24] = {0};

  // Preferred title language (primary subtag) and whether the current localized
  // title object has already locked onto its match; see setPreferredLanguage().
  char preferredLang[12] = {0};
  bool localizedTitleLocked = false;
  // True when pendingKey (a language tag inside a localized title object)
  // matches preferredLang, exactly or as a "<lang>-REGION" variant.
  bool pendingKeyIsPreferred() const;

  OpdsEntry* entries = nullptr;
  size_t entryCount = 0;
  size_t entryLimit = 0;
  bool publicationDocument = false;
  // Entries live in the caller array; cap their variable strings as well so
  // optional publication URLs cannot consume the C3 heap during TLS parsing.
  static constexpr size_t MAX_ENTRY_STRING_BYTES = 24 * 1024;
  size_t entryStringBytes = 0;
  void append(OpdsEntry&& entry) {
    const size_t bytes =
        entry.title.size() + entry.author.size() + entry.href.size() + entry.id.size() + entry.selfHref.size();
    if (bytes > MAX_ENTRY_STRING_BYTES - entryStringBytes) {
      feedTruncated = true;
      return;
    }
    entryStringBytes += bytes;
    entries[entryCount++] = std::move(entry);
  }
  OpdsEntry currentEntry;
  bool currentLcp = false;

  // Accumulator for the link object currently being parsed (feed, navigation,
  // group, facet or publication link — the scope on commit tells which).
  struct {
    std::string href;
    std::string title;
    bool relSearch = false;
    bool relNext = false;
    bool relPrev = false;
    bool relFirst = false;
    bool relLast = false;
    bool relSelf = false;
    bool relShelf = false;
    bool relWishlist = false;
    bool relHistory = false;
    int acqRank = -1;
    bool typeEpub = false;
    bool typeIndirect = false;  // application/opds-publication+json acquisition
    bool typePubDoc = false;    // application/opds-publication+json (any rel)
    bool typeLcp = false;       // application/vnd.readium.lcp.license.v1.0+json
    bool templated = false;
    int32_t numberOfItems = -1;
  } link;
  // Best acquisition rank already committed for the current publication, and
  // whether that href points at a plain EPUB (see commitPubLink()).
  int pubAcqRank = -1;
  bool pubHasPlainEpub = false;

  // A group in the root feed is a preview: keep at most this many publications
  // per group so one large group doesn't consume the whole entry budget. The
  // group's "see all" self link reaches the rest.
  static constexpr int MAX_GROUP_PREVIEW = 5;
  // Current group accumulation (groups don't nest).
  std::string groupTitle;
  std::string groupSelfHref;
  int groupPubCount = 0;  // publications committed in the current group
  // Current facet group accumulation.
  std::string facetTitle;

  std::string feedTitle;
  int32_t feedNumberOfItems = -1;
  int32_t feedItemsPerPage = -1;
  int32_t feedCurrentPage = -1;
  std::string searchTemplate;
  std::string nextPageUrl;
  std::string prevPageUrl;
  std::string firstPageUrl;
  std::string lastPageUrl;
  std::string shelfUrl;
  std::string wishlistUrl;
  std::string historyUrl;
};
