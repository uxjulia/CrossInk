#pragma once

#include <Arena.h>
#include <HalStorage.h>

#include <array>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "CssStyle.h"

/**
 * Lightweight CSS parser for EPUB stylesheets
 *
 * Parses CSS files and extracts styling information relevant for e-ink display.
 * Uses a two-phase approach: first tokenizes the CSS content, then builds
 * a rule database that can be queried during HTML parsing.
 *
 * Supported selectors:
 *   - Element selectors: p, div, h1, etc.
 *   - Class selectors: .classname
 *   - Combined: element.classname
 *   - Grouped: selector1, selector2 { }
 *   - Two-part descendant: ancestor subject (e.g. "div p", "section.chapter p")
 *
 * Not supported (silently ignored):
 *   - Three-or-more-part descendant selectors
 *   - Child/sibling combinators (>, +, ~)
 *   - Pseudo-classes and pseudo-elements
 *   - Media queries (content is skipped)
 *   - @import and other at-rules (except @font-face)
 */

/**
 * Represents one open ancestor element in the HTML parse tree.
 * The `depth` field is used by ChapterHtmlSlimParser for stack management;
 * CssParser only reads `tag` and `classAttr` for selector matching.
 */
struct CssAncestorEntry {
  int depth = 0;
  std::string tag;
  std::string classAttr;
  std::string idAttr;
};

struct CssFontFace {
  uint32_t family = 0;
  bool bold = false;
  bool italic = false;
  std::string href;
};

class CssParser {
 public:
  enum class CacheStatus : uint8_t {
    Missing,
    Invalid,
    Complete,
    Partial,
  };

  // Bump when CSS cache format or rules change; section caches are invalidated when this changes
  static constexpr uint32_t CSS_CACHE_MAGIC = 0x435843FF;  // bytes: 0xFF, "CXC"
  static constexpr uint8_t CSS_CACHE_VERSION = 23;

  // Source text is streamed, never loaded as one allocation. PSRAM readers
  // can admit larger publisher stylesheets; rule-count and internal-heap
  // guards still bound the transient selector map on every device.
  static size_t maxSourceBytes() { return psramHeapAvailable() ? 512U * 1024U : 128U * 1024U; }

  static constexpr size_t MAX_DESCENDANT_RULES = 100;
  static constexpr size_t CSS_INDEX_BYTES_PER_RULE = 8;

  explicit CssParser(std::string cachePath) : cachePath(std::move(cachePath)) {}
  ~CssParser() = default;

  // Non-copyable
  CssParser(const CssParser&) = delete;
  CssParser& operator=(const CssParser&) = delete;

  /**
   * Load and parse CSS from a file stream.
   * Can be called multiple times to accumulate rules from multiple stylesheets.
   * @param source Open file handle to read from
   * @return true if parsing completed (even if no rules found)
   */
  bool loadFromStream(HalFile& source, std::string_view baseDir = {});
  static uint32_t hashFontFamily(std::string_view name);
  static bool loadFontFaces(const std::string& cachePath, std::vector<CssFontFace>& out);

  /**
   * Look up the style for an HTML element, considering tag name, class attributes, and ancestors.
   * Applies CSS cascade: element style < descendant rules < class style < element.class style
   *
   * @param tagName The HTML element name (e.g., "p", "div")
   * @param classAttr The class attribute value (may contain multiple space-separated classes)
   * @param ancestors Open ancestor elements in the parse tree, innermost last
   * @return Combined style with all applicable rules merged
   */
  [[nodiscard]] CssStyle resolveStyle(std::string_view tagName, std::string_view classAttr,
                                      const std::vector<CssAncestorEntry>& ancestors = {}, std::string_view idAttr = {},
                                      bool firstLetter = false) const;

  /**
   * Parse an inline style attribute string.
   * @param styleValue The value of a style="" attribute
   * @return Parsed style properties
   */
  [[nodiscard]] static CssStyle parseInlineStyle(std::string_view styleValue);

  /**
   * Check if any rules have been loaded
   */
  [[nodiscard]] bool empty() const {
    return parsedRuleCount() == 0 && cacheRuleOffsets_.empty() && descendantRules_.empty();
  }

  /**
   * Get count of loaded rule sets
   */
  [[nodiscard]] size_t ruleCount() const { return parsedRuleCount() == 0 ? cachedRuleCount_ : parsedRuleCount(); }

  /**
   * Clear all loaded rules
   */
  void clear() {
    decltype(fontFaces_){}.swap(fontFaces_);
    // These buffers can grow large during chapter indexing. Swap with empty
    // vectors so the capacity is released back to the heap, matching the old
    // post-index cleanup behavior callers relied on.
    decltype(rulesBySelector_){}.swap(rulesBySelector_);
    parsedRuleArena_.release();
    parsedRuleBuckets_ = nullptr;
    parsedRuleHead_ = nullptr;
    psramParsedRuleCount_ = 0;
    decltype(descendantRules_){}.swap(descendantRules_);
    decltype(cacheRuleOffsets_){}.swap(cacheRuleOffsets_);
    cachedRuleArena_.release();
    cachedRules_ = nullptr;
    cachedRuleTableCount_ = 0;
    cacheIndexLoaded_ = false;
    cachedRuleCount_ = 0;
    cachePartial_ = false;
  }

  /**
   * Check if CSS rules cache file exists
   */
  bool hasCache() const;

  /**
   * Validate the cache structure without hydrating any rule containers.
   */
  CacheStatus inspectCache() const;

  /**
   * Delete CSS rules cache file exists
   */
  void deleteCache() const;

  /**
   * Save parsed CSS rules to a cache file.
   * @param complete false when the cache contains only rules parsed before a safe low-memory stop
   * @return true if cache was written successfully
   */
  bool saveToCache(bool complete = true) const;

  /**
   * Load CSS rules from a cache file.
   * Clears any existing rules before loading.
   * @return true if cache was loaded successfully
   */
  bool loadFromCache();

  // Estimate the two most useful non-body bitmap sizes from cached stylesheet
  // declarations without hydrating rules. Heading selectors take priority.
  // Values use 1/256 em; absent/invalid CSS retains the default size policy.
  std::array<uint16_t, 2> contentFontSizeScales() const;

  /**
   * True when the last loaded/saved cache is a usable but incomplete CSS parse.
   */
  [[nodiscard]] bool isCachePartial() const { return cachePartial_; }

 private:
  std::vector<CssFontFace> fontFaces_;
  bool parseFontFace(std::string_view declarations, std::string_view baseDir);
  static constexpr uint8_t CSS_CACHE_FLAG_PARTIAL = 1 << 0;

  struct DescendantRule {
    std::string ancestorSelector;  // e.g. "div", ".chapter", "section.body"
    std::string subjectSelector;   // e.g. "p", ".indent", "p.indent"
    CssStyle style;
  };

  // Lookup key for a multi-piece selector. The pieces are hashed and compared
  // as if concatenated, so callers can look up composite keys without
  // materializing the concatenation in a scratch buffer. Constructed from a
  // braced list of any arity, e.g. `CompositeKey{tagName, ".", cls}` or
  // `CompositeKey{".", cls}`. The initializer_list's backing array lives for
  // the full expression, which covers the lifetime of the find() call.
  struct CompositeKey {
    std::initializer_list<std::string_view> pieces;
    CompositeKey(std::initializer_list<std::string_view> p) noexcept : pieces(p) {}
  };

  // ASCII-case-insensitive transparent hash/equal. Stored selectors and lookup
  // keys are compared without regard to case, so callers may insert and look up
  // using whatever case the CSS source or HTML element name happens to use.
  // Bodies live in CssParser.cpp so they can share the file-local asciiToLower.
  struct SvHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept;
    size_t operator()(const std::string& s) const noexcept;
    size_t operator()(CompositeKey k) const noexcept;
  };
  struct SvEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept;
    bool operator()(const std::string& a, std::string_view b) const noexcept;
    bool operator()(std::string_view a, const std::string& b) const noexcept;
    bool operator()(const std::string& a, const std::string& b) const noexcept;
    bool operator()(CompositeKey a, std::string_view b) const noexcept;
    bool operator()(std::string_view a, CompositeKey b) const noexcept;
  };

  // Storage: maps selector -> style properties. Hash/equal are case-insensitive.
  std::unordered_map<std::string, CssStyle, SvHash, SvEqual> rulesBySelector_;
  // Simple rules dominate publisher CSS. A fallible PSRAM arena avoids one
  // internal allocation per map node/key; C3 keeps the existing guarded map.
  // At most 1500 rules and 256 bytes per selector are admitted by the parser.
  struct ParsedRule {
    ParsedRule* bucketNext = nullptr;
    ParsedRule* next = nullptr;
    std::string_view selector;
    CssStyle style;
  };
  static constexpr size_t PARSED_RULE_BUCKETS = 256;
  const bool usePsramParsedRules_ = psramHeapAvailable();
  Arena parsedRuleArena_{ArenaBacking::PsramOnly};
  ParsedRule** parsedRuleBuckets_ = nullptr;
  ParsedRule* parsedRuleHead_ = nullptr;
  size_t psramParsedRuleCount_ = 0;
  size_t parsedRuleCount() const { return rulesBySelector_.size() + psramParsedRuleCount_; }
  ParsedRule* findPsramParsedRule(std::string_view selector) const;
  bool addPsramParsedRule(std::string_view selector, const CssStyle& style);
  std::vector<DescendantRule> descendantRules_;

  std::string cachePath;
  bool cachePartial_ = false;

  struct SelectorEntry {
    uint32_t hash;
    uint32_t offset;
  };
  static_assert(sizeof(SelectorEntry) == CSS_INDEX_BYTES_PER_RULE,
                "SelectorEntry size changed; update CSS_INDEX_BYTES_PER_RULE");
  struct CachedRule {
    uint32_t hash;
    uint32_t secondaryHash;
    uint16_t selectorLen;
    CssStyle style;
  };
  Arena cachedRuleArena_{psramHeapAvailable() ? ArenaBacking::PsramOnly : ArenaBacking::Default};
  CachedRule* cachedRules_ = nullptr;
  size_t cachedRuleTableCount_ = 0;
  mutable bool cacheIndexLoaded_ = false;
  mutable size_t cachedRuleCount_ = 0;
  mutable std::vector<SelectorEntry> cacheRuleOffsets_;

  // Internal parsing helpers
  [[nodiscard]] bool processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style);
  static bool selectorMatchesElement(std::string_view selector, std::string_view tag, std::string_view classAttr);
  static CssStyle parseDeclarations(std::string_view declBlock);
  static void parseDeclarationIntoStyle(std::string_view decl, CssStyle& style);

  // Individual property value parsers
  static CssTextAlign interpretAlignment(std::string_view val);
  static CssFontStyle interpretFontStyle(std::string_view val);
  static CssFontWeight interpretFontWeight(std::string_view val);
  static CssFontVariantCaps interpretFontVariantCaps(std::string_view val);
  static CssTextDecoration interpretDecoration(std::string_view val);
  static CssLength interpretLength(std::string_view val);
  /** Returns true only when a numeric length was parsed (e.g. 2em, 50%). False for auto/inherit/initial. */
  static bool tryInterpretLength(std::string_view val, CssLength& out);
  static uint32_t selectorHash(std::string_view selector);
  static uint32_t selectorSecondaryHash(std::string_view selector);
  bool lookupRule(std::string_view selector, CssStyle& outStyle) const;
  bool lookupArenaRule(std::string_view selector, CssStyle& outStyle) const;
  bool readRuleFromDiskAtOffset(uint32_t ruleOffset, std::string_view selector, CssStyle& outStyle) const;
  static bool readCssStylePayload(FsFile& file, CssStyle& style);
  bool writeFontFaces(HalFile& file) const;
  static bool readFontFaces(HalFile& file, std::vector<CssFontFace>* out = nullptr);
  static bool writeCssStylePayload(FsFile& file, const CssStyle& style);
};
