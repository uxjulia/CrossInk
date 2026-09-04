#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "Epub.h"
#include "EpubRenderMode.h"
#include "ReaderRenderSpec.h"

class Page;
class GfxRenderer;
class ChapterHtmlSlimParser;
class CssParser;

struct SectionBuildOptions {
  const char* previewAnchor = nullptr;
  uint16_t previewMaxPages = 0;
  // Full-section callers can stop between parser chunks without creating a
  // readable partial cache. The callback must remain valid for the build.
  bool (*shouldCancel)(void* context) = nullptr;
  void* cancelContext = nullptr;
  bool* cancellationObserved = nullptr;

  bool isPreview() const { return previewAnchor && previewAnchor[0] != '\0' && previewMaxPages > 0; }
  bool isCancellationRequested() const { return shouldCancel && shouldCancel(cancelContext); }
};

class Section {
  std::shared_ptr<Epub> epubOwner;
  Epub* epub;
  const int spineIndex;
  GfxRenderer& renderer;
  std::string filePath;
  HalFile file;

  struct PageLutEntry {
    uint32_t fileOffset;
    uint16_t paragraphIndex;
    uint16_t listItemIndex;
    uint32_t visibleTextOffset;
  };

  struct BuildContext {
    std::unique_ptr<ChapterHtmlSlimParser> parser;
    std::unique_ptr<PageLutEntry[]> lut;
    uint16_t lutCapacity = 0;
    uint16_t lutCount = 0;
    std::string parsePath;
    std::string contentBase;
    std::string imageBasePath;
    std::string htmlPath;
    std::string tmpHtmlPath;
    std::string tmpSectionPath;
    bool reusedHtml = false;
    bool pageCompletionFailed = false;
    CssParser* cssParser = nullptr;
    // HTML byte progress, for estimating the section's total page count while it's still building.
    uint32_t bytesConsumed = 0;
    uint32_t totalBytes = 0;
    // Exponentially-smoothed page-count estimate (0 = not yet seeded) and the bytesConsumed at its
    // last update. The raw byte-ratio estimate jitters as the build crosses dense/sparse regions;
    // the EMA is stepped once per build advance (not per redraw) to damp that wobble.
    float smoothedEstimate = 0;
    uint32_t smoothedAtConsumed = 0;
  };
  std::unique_ptr<BuildContext> build_;
  bool buildComplete_ = false;
  bool lastImagesWereSuppressed_ = false;
  bool lastLayoutAbortedForLowMemory_ = false;
  uint16_t imageEstimateViewportHeight_ = 0;
  uint32_t protectedImageUnits_ = 0;
  // Pages laid out by the active build. Distinct from pageCount, which is the pages
  // available to read and may include a loaded partial file's pages.
  uint16_t builtPageCount_ = 0;
  bool partial_ = false;
  uint16_t partialPageCount_ = 0;
  uint32_t partialProtectedImageUnits_ = 0;
  uint32_t partialBytesConsumed_ = 0;
  uint32_t partialTotalBytes_ = 0;
  std::string activeBuildTmpSectionPath_;

  bool writeSectionFileHeader(const ReaderRenderSpec& spec);
  uint32_t onPageComplete(std::unique_ptr<Page> page);
  bool ensureBuildFileOpen();
  bool finalizeBuild();
  // Write the LUTs/anchor map (and, for a partial, the watermark trailer), patch the
  // header, stamp the version byte, and swap the tmp .bin over filePath.
  bool commitBuildFile(uint8_t version, uint32_t bytesConsumed, uint32_t totalBytes);
  // Builds write here and are swapped over filePath only on commit, so a prior
  // partial/finalized file stays readable while a rebuild is in progress.
  std::string binTmpPath() const { return filePath + ".part"; }
  std::unique_ptr<Page> loadPageAt(int page) const;
  // Read a page already laid out by the in-progress build (page < build LUT size), from
  // the partially-written tmp .bin without disturbing the build's write cursor.
  std::unique_ptr<Page> loadPageDuringBuild(int page);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<Epub>& epub, int spineIndex, GfxRenderer& renderer,
                   const char* cacheSuffix = "");
  explicit Section(Epub& epub, int spineIndex, GfxRenderer& renderer, const char* cacheSuffix = "");
  ~Section();
  bool loadSectionFile(const ReaderRenderSpec& spec);
  bool clearCache() const;
  bool createSectionFile(const ReaderRenderSpec& spec, const std::function<void()>& popupFn = nullptr,
                         bool* imagesWereSuppressed = nullptr, bool* layoutAbortedForLowMemory = nullptr,
                         SectionBuildOptions buildOptions = {});

  bool startBuild(const ReaderRenderSpec& spec, SectionBuildOptions buildOptions = {},
                  const std::function<void()>& popupFn = nullptr);
  bool buildSomeMore(int maxPages);
  void releaseBuildFile();
  bool lastBuildImagesWereSuppressed() const { return lastImagesWereSuppressed_; }
  bool lastBuildLayoutAbortedForLowMemory() const { return lastLayoutAbortedForLowMemory_; }
  bool isBuilding() const { return static_cast<bool>(build_); }
  bool isBuildComplete() const { return buildComplete_; }
  bool activeBuildHasCaughtReadablePages() const { return !build_ || builtPageCount_ >= pageCount; }
  // Best-known total page count: the exact pageCount once finalized, or a smoothed byte-based
  // estimate (pages so far scaled by totalBytes/bytesConsumed, damped by an EMA) while a giant spine
  // is still building, so "page X of Y" / progress don't read off the small build watermark.
  uint16_t estimatedTotalPages() const;
  void abandonBuild();
  // Persist an in-progress build as a partial section file (version sentinel + LUTs +
  // watermark trailer) instead of discarding it, so the next open of this spine can show
  // its pages instantly and only rebuild in the background. Called by the destructor, so
  // any teardown path (exit, sleep, navigation) keeps the work already done. Keeps a
  // pre-existing partial when it covers more pages than this build reached.
  void suspendBuild();
  // True when a partial file was loaded: pageCount is a watermark, not the chapter total.
  bool isPartial() const { return partial_; }

  // Unified page read: from the active build if it has reached the page, otherwise from
  // the on-disk file (finalized section, or a partial the rebuild hasn't caught up to).
  std::unique_ptr<Page> loadPage(int page);

  std::unique_ptr<Page> loadPageFromSectionFile();

  // Resolve an anchor from the in-progress build first, then the on-disk anchor map
  // (covers finalized sections and partials from a previous session).
  std::optional<uint16_t> findAnchor(const std::string& anchor) const;

  // True if this spine's unzipped HTML is already cached, so a build won't pay the (multi-second on a
  // giant spine) zip inflation. Lets the reader skip the indexing popup on a fast reopen/rebuild.
  bool hasHtmlCache() const;

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Look up an anchor among the pages built so far by the in-progress build, so an anchor jump
  // (TOC / chapter select, usually the chapter top = page 0) can resolve without laying out the
  // whole chapter. Returns nullopt if the anchor hasn't been reached yet (build more) or no build.
  std::optional<uint16_t> findAnchorDuringBuild(const std::string& anchor) const;

  // Get the page count from the section cache file without fully loading it.
  std::optional<uint16_t> getCachedPageCount() const;

  // Image units already protected by laid-out pages, and the non-image units
  // projected through the section's XHTML byte density. Both accessors are
  // allocation-free and are used by grouped-chapter progress estimates.
  uint64_t estimatedProtectedImageUnits() const;
  uint64_t estimatedNonImageProjectionUnits() const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  // Checks the active incremental build before falling back to the committed cache.
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up a synthetic paragraph among pages produced by the active build only.
  std::optional<uint16_t> findParagraphDuringBuild(uint16_t pIndex) const;

  // Look up the page number for a running list-item index from the li LUT.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;

  // Look up the running list-item index for the given rendered page.
  std::optional<uint16_t> getListItemIndexForPage(uint16_t page) const;

  // Content coordinate recorded at the start of each rendered page. Available
  // for finalized sections and the readable prefix of incremental sections.
  std::optional<uint32_t> getVisibleTextOffsetForPage(uint16_t page) const;
  std::optional<uint16_t> getPageForVisibleTextOffset(uint32_t offset, bool preferFirstAtOffset = false) const;
};
