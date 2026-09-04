#pragma once

#include <Print.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class ZipFile;
class ZipFileStreamReader;
class GfxRenderer;

class Epub {
 private:
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // the guide TOC page (EPUB 2)
  std::string tocGuideItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Stable cache path based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;
  struct LocationSpineEntry {
    uint32_t startLocation = 0;
    uint32_t endLocation = 0;
    uint32_t wordStart = 0;
    uint32_t wordCount = 0;
    uint16_t chapterGroup = UINT16_MAX;
  };
  struct LocationChapterGroupEntry {
    uint16_t firstSpineIndex = 0;
    uint16_t lastSpineIndex = 0;
    bool valid = false;
  };

 public:
  enum class OpenFailure : uint8_t {
    None,
    OutOfMemory,
    InvalidOrUnreadable,
  };

  struct SourceChildRange {
    char name[12] = {};
    uint16_t offset = 0;
    uint16_t count = 0;
  };
  struct SourceSpineMapEntry {
    uint16_t sourceSpineIndex = UINT16_MAX;
    uint16_t firstRange = 0;
    uint8_t rangeCount = 0;
    uint8_t containerDepth = 0;
  };

 private:
  std::unique_ptr<LocationSpineEntry[]> locationSpine;
  size_t locationSpineCount = 0;
  std::unique_ptr<LocationChapterGroupEntry[]> locationChapterGroups;
  size_t locationChapterGroupCount = 0;
  std::unique_ptr<SourceSpineMapEntry[]> sourceSpineMap;
  std::unique_ptr<SourceChildRange[]> sourceChildRanges;
  size_t sourceSpineMapCount = 0;
  size_t sourceChildRangeCount = 0;
  uint16_t sourceSpineCount = 0;
  uint32_t totalLocations = 0;
  uint32_t totalWords = 0;
  uint32_t wordsPerReferencePage = 0;
  uint32_t totalReferencePages = 0;
  bool xLocationsLoaded = false;
  OpenFailure lastLoadFailure = OpenFailure::None;
  bool sourceSpineMapDeclared = false;
  enum class CssParseStatus : uint8_t {
    Failed,
    Partial,
    Complete,
  };

  void migrateLegacyCachePath(const std::string& cacheDir) const;
  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true,
                       bool collectCssFiles = true);
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  CssParseStatus parseCssFiles(bool forceRebuild = false) const;
  void discoverCssFilesFromZip();
  void releaseCssFileList();

 public:
  enum class XLocationLoadMode : uint8_t {
    Immediate,
    Skip,
  };

  explicit Epub(std::string filepath, const std::string& cacheDir);
  ~Epub() = default;
  static std::string cachePathForFilePath(const std::string& filepath, const std::string& cacheDir);
  // True when a metadata cache already exists for this book, i.e. load() will
  // hit the fast path instead of rebuilding. Cheap: no parsing, just a stat.
  static bool hasCache(const std::string& filepath, const std::string& cacheDir);
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false,
            XLocationLoadMode xLocationLoadMode = XLocationLoadMode::Immediate, bool cacheCumulativeSpineSizes = false);
  // Loads optional stable-page and source-spine metadata after a Skip-mode open.
  // Failure leaves normal size-based progress available.
  bool loadXLocations();
  OpenFailure getLastLoadFailure() const { return lastLoadFailure; }
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const;
  const std::string& getPath() const;
  const std::string& getTitle() const;
  const std::string& getAuthor() const;
  const std::string& getLanguage() const;
  // True when parsed EPUB metadata identifies a cover image. Requires load().
  bool hasCoverImage() const;
  std::string getCoverBmpPath(bool cropped = false) const;
  bool generateCoverBmp(bool cropped = false, const GfxRenderer* renderer = nullptr, int readerFontId = 0) const;
  std::string getThumbBmpPath() const;
  // Deprecated compatibility wrapper; forwards to getThumbBmpPath(0, height).
  [[deprecated("use getThumbBmpPath(int width, int height)")]]
  std::string getThumbBmpPath(int height) const;
  // Returns the thumbnail cache path. width <= 0 derives the default 3:5
  // (width:height) thumbnail width from height; height <= 0 uses the default
  // thumbnail height.
  std::string getThumbBmpPath(int width, int height) const;
  // Returns a Minimal-style adaptive thumbnail path. Normal cover ratios fill
  // the requested box; unusual ratios are contained inside the box.
  std::string getAdaptiveThumbBmpPath(int width, int height) const;
  // Deprecated compatibility wrapper; forwards to generateThumbBmp(0, height).
  [[deprecated("use generateThumbBmp(int width, int height)")]]
  bool generateThumbBmp(int height, const GfxRenderer* renderer = nullptr, int readerFontId = 0) const;
  // Writes a thumbnail BMP to cache. width <= 0 derives the default 3:5
  // (width:height) thumbnail width from height; height <= 0 uses the default
  // thumbnail height.
  // Returns false on missing cache/cover, unsupported image format, or conversion failure.
  bool generateThumbBmp(int width, int height, const GfxRenderer* renderer = nullptr, int readerFontId = 0) const;
  // Writes a thumbnail that can either crop-to-fill or contain unusual cover
  // ratios, depending on the source image dimensions.
  bool generateAdaptiveThumbBmp(int width, int height, const GfxRenderer* renderer = nullptr,
                                int readerFontId = 0) const;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize,
                                bool allowEarlyStop = false) const;
  bool extractItemToFile(const std::string& itemHref, const std::string& destPath) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  bool isNavigationDocumentSpine(int spineIndex, bool* scanSucceeded = nullptr) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  bool hasXLocations() const { return xLocationsLoaded; }
  bool hasStablePageNumbers() const {
    return xLocationsLoaded && totalWords > 0 && wordsPerReferencePage > 0 && totalReferencePages > 0;
  }
  float calculateSizeProgress(int currentSpineIndex, float currentSpineRead) const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  bool resolveLocationPercentToSpineProgress(int percent, int& spineIndex, float& spineProgress) const;
  bool resolveReferencePage(int currentSpineIndex, float currentSpineRead, uint32_t& currentPage,
                            uint32_t& pageCount) const;
  bool resolveChapterGroupRange(int currentSpineIndex, int& firstSpineIndex, int& lastSpineIndex) const;
  bool hasChapterGroups() const { return locationChapterGroupCount > 0; }
  bool hasSourceSpineMap() const { return sourceSpineMapCount > 0; }
  bool requiresSourceSpineMap() const { return (hasChapterGroups() || sourceSpineMapDeclared) && !hasSourceSpineMap(); }
  bool getSourceSpineMapEntry(int currentSpineIndex, SourceSpineMapEntry& entry) const;
  const SourceChildRange* getSourceChildRange(const SourceSpineMapEntry& entry, size_t ordinal) const;
  bool findCurrentSpineForSource(int sourceIndex, uint8_t containerDepth, const char* childName,
                                 uint16_t sourceSiblingIndex, int& currentSpineIndex,
                                 uint16_t& currentSiblingIndex) const;
  CssParser* getCssParser() const { return cssParser.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;

 private:
  std::string getCachedCoverImagePath(const std::string& coverImageHref) const;
  bool ensureCachedCoverImage(const std::string& coverImageHref, std::string& outPath) const;
  bool generateThumbBmpInternal(int width, int height, bool adaptiveContain, const GfxRenderer* renderer,
                                int readerFontId) const;
};
