#pragma once
#include <ContentProtection.h>
#include <Print.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"
#include "Epub/image/PxcV2.h"

class ZipFile;
class ZipFileStreamReader;
class GfxRenderer;

class Epub {
 public:
  struct SyncMetadata {
    std::string isbn, asin, series, seriesIndex;
  };
  bool loadSyncMetadata(SyncMetadata& metadata);

 private:
  mutable std::unique_ptr<freeink::content::ContentDecryptor> decryptor;
  bool openProtection() const;
  mutable std::string protectionError;
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
  bool ensureOptimizerImageIndex();
  enum class OpenFailure : uint8_t {
    None,
    OutOfMemory,
    InvalidOrUnreadable,
    ProtectedUnavailable,
    LoanExpired,
    LoanClockUnavailable,
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
  mutable OptimizerFormat::Record optimizerLastHit;
  mutable std::unique_ptr<PxcV2Workspace> optimizerWorkspace;
  uint16_t optimizerIndexCount = 0;
  bool optimizerIndexReady = false;
  bool optimizerManifestAbsent = false;
  bool findOptimizerImage(const std::string& itemHref) const;
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
  bool referencePagesUseCharacters = false;
  bool xLocationsLoaded = false;
  OpenFailure lastLoadFailure = OpenFailure::None;
  bool sourceSpineMapDeclared = false;
  enum class CssParseStatus : uint8_t {
    Failed,
    Partial,
    Complete,
  };

  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true,
                       bool collectCssFiles = true, bool metadataOnly = false, std::string* seriesIndex = nullptr,
                       SyncMetadata* syncMetadata = nullptr);
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
  // Resolve the stable cache path and migrate an older hash-named directory if
  // needed, without opening the EPUB or loading its metadata/location indexes.
  static std::string resolveCachePathForFilePath(const std::string& filepath, const std::string& cacheDir);

  // True when a metadata cache already exists for this book, i.e. load() will
  // hit the fast path instead of rebuilding. Cheap: no parsing, just a stat.
  static bool hasCache(const std::string& filepath, const std::string& cacheDir);
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false,
            XLocationLoadMode xLocationLoadMode = XLocationLoadMode::Immediate, bool cacheCumulativeSpineSizes = false);
  // Title and author only, without building the spine/TOC/CSS/cover caches
  // load() does. Reuses an existing metadata cache when there is one unless
  // allowCachedMetadata is false. Cache invalidation is the caller's job, so
  // metadata reads cannot delete per-book progress or settings. Parsing stops
  // at </metadata>, before the manifest. Used by the
  // Library index builder, which reads every EPUB on the card and cannot
  // afford a full load() per book.
  bool loadMetadata(std::string& title, std::string& author, bool allowCachedMetadata = true,
                    std::string* series = nullptr, std::string* genre = nullptr, std::string* seriesIndex = nullptr);
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
  std::string getCoverBmpPath(bool cropped = false, bool imageLevels = false) const;
  bool generateCoverBmp(bool cropped = false, const GfxRenderer* renderer = nullptr, int readerFontId = 0,
                        bool imageLevels = false) const;
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
  // Generate a Home thumbnail from the EPUB cover without building reader pages or indexes.
  bool generateThumbBmpFromSource(int height, const GfxRenderer* renderer = nullptr, int readerFontId = 0);
  bool generateThumbBmpFromSource(int width, int height, const GfxRenderer* renderer = nullptr, int readerFontId = 0);
  // Writes a thumbnail that can either crop-to-fill or contain unusual cover
  // ratios, depending on the source image dimensions.
  bool generateAdaptiveThumbBmp(int width, int height, const GfxRenderer* renderer = nullptr,
                                int readerFontId = 0) const;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize,
                                bool allowEarlyStop = false) const;
  // Publisher-font access also reverses declared Adobe font obfuscation.
  bool readFontContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize) const;
  bool extractItemToFile(const std::string& itemHref, const std::string& destPath, size_t chunkSize = 4096) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  bool getOptimizerImageDimensions(const std::string& itemHref, uint16_t& width, uint16_t& height) const;
  // Seeds the normal local cache from an exact optimizer sidecar, or streams a
  // bounded nearest-neighbour resize into that cache when the device differs.
  bool seedOptimizerImageCache(const std::string& itemHref, int expectedWidth, int expectedHeight,
                               const std::string& destPxcPath) const;
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
  bool hasStablePageNumbers() const;
  bool resolveReferenceTextProgress(int spineIndex, uint32_t textOffset, uint32_t sectionTextLength,
                                    double& completedUnits, double& totalUnits) const;
  uint32_t getReferencePageCount() const { return hasStablePageNumbers() ? totalReferencePages : 0; }
  float calculateSizeProgress(int currentSpineIndex, float currentSpineRead) const;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const;
  // percent is 0.0-100.0; callers may pass fractional values from decimal keypad entry.
  bool resolveLocationPercentToSpineProgress(float percent, int& spineIndex, float& spineProgress) const;
  bool resolveReferencePage(int currentSpineIndex, float currentSpineRead, uint32_t& currentPage,
                            uint32_t& pageCount) const;
  bool resolveReferencePageToSpineProgress(uint32_t page, int& spineIndex, float& spineProgress) const;
  bool resolveReferencePageTarget(uint32_t page, int& spineIndex, float& spineProgress, uint32_t& spineUnitOffset,
                                  uint32_t& spineUnitCount, bool& usesCharacters) const;
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
                                int readerFontId, const std::string* coverHrefOverride = nullptr) const;
};
