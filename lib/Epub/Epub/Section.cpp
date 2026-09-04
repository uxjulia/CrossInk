#include "Section.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <Serialization.h>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint32_t SECTION_CACHE_MAGIC = 0x535843FF;  // bytes: 0xFF, "CXS"
// v62: Text blocks persist source whitespace semantics for font-preview reflow.
// This changes their serialized payload, so full and suspended section caches
// must rebuild together.
// v63: Paragraph base direction excludes direction changes from inline elements.
// v66: Internal EPUB links preserve CSS superscript/subscript positioning.
constexpr uint8_t SECTION_FILE_VERSION = 66;
// Suspended incremental build: valid pages plus LUTs and a parse-watermark trailer.
// Change this with layout or payload changes so stale partial pages cannot resume
// under a different layout contract.
constexpr uint8_t SECTION_FILE_PARTIAL_VERSION = 0xF6;
constexpr uint16_t INITIAL_SECTION_PAGE_LUT_ENTRIES = 1024;
constexpr uint32_t HEADER_SIZE =
    sizeof(SECTION_CACHE_MAGIC) + sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(bool) +
    sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) + sizeof(uint8_t) +
    sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint32_t) +
    sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
constexpr size_t SECTION_HTML_STREAM_CHUNK_SIZE = 8192;
constexpr size_t LOW_MEMORY_SECTION_HTML_STREAM_CHUNK_SIZE = 1024;

struct PageLutEntry {
  uint32_t fileOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
  uint32_t visibleTextOffset;
};

template <typename Entry>
bool ensurePageLutCapacity(std::unique_ptr<Entry[]>& lut, uint16_t& lutCapacity, const uint16_t lutCount) {
  if (lutCount < lutCapacity) return true;
  if (lutCapacity == UINT16_MAX) return false;

  uint32_t nextCapacity = static_cast<uint32_t>(lutCapacity) * 2U;
  if (nextCapacity > UINT16_MAX) {
    nextCapacity = UINT16_MAX;
  }

  auto grown = makeUniqueNoThrow<Entry[]>(nextCapacity);
  if (!grown) return false;

  for (uint16_t i = 0; i < lutCount; i++) {
    grown[i] = lut[i];
  }
  lut = std::move(grown);
  lutCapacity = static_cast<uint16_t>(nextCapacity);
  return true;
}

void prepareSectionZipInflate(GfxRenderer& renderer, const int fontId) {
  if (ESP.getMaxAllocHeap() < InflateStream::requiredStorageSize(true) && renderer.isSdCardFont(fontId)) {
    renderer.releaseSdCardFontForLowMemory(fontId);
  }
}

size_t sectionHtmlStreamChunkSize(const bool preview) {
  if (preview) {
    return LOW_MEMORY_SECTION_HTML_STREAM_CHUNK_SIZE;
  }

  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  const size_t largeStreamBudget = InflateStream::requiredStorageSize(true) + (2U * SECTION_HTML_STREAM_CHUNK_SIZE);
  if (maxAlloc < largeStreamBudget) {
    LOG_DBG("SCT", "Using low-memory HTML stream chunk (maxAlloc=%u)", maxAlloc);
    return LOW_MEMORY_SECTION_HTML_STREAM_CHUNK_SIZE;
  }
  return SECTION_HTML_STREAM_CHUNK_SIZE;
}

std::string sectionBackupPath(const std::string& filePath) { return filePath + ".bak"; }

void recoverSectionCacheBackup(const std::string& filePath) {
  const std::string backupPath = sectionBackupPath(filePath);
  if (!Storage.exists(backupPath.c_str())) return;

  if (Storage.exists(filePath.c_str())) {
    Storage.remove(backupPath.c_str());
    return;
  }

  if (Storage.rename(backupPath.c_str(), filePath.c_str())) {
    LOG_INF("SCT", "Recovered section cache backup: %s", filePath.c_str());
  } else {
    LOG_ERR("SCT", "Failed to recover section cache backup: %s", filePath.c_str());
  }
}

bool promoteSectionCache(const std::string& tmpPath, const std::string& filePath) {
  recoverSectionCacheBackup(filePath);
  if (!Storage.exists(filePath.c_str())) {
    return Storage.rename(tmpPath.c_str(), filePath.c_str());
  }

  const std::string backupPath = sectionBackupPath(filePath);
  if (!Storage.rename(filePath.c_str(), backupPath.c_str())) {
    LOG_ERR("SCT", "Failed to preserve old section cache before replacement");
    return false;
  }
  if (Storage.rename(tmpPath.c_str(), filePath.c_str())) {
    Storage.remove(backupPath.c_str());
    return true;
  }

  LOG_ERR("SCT", "Failed to promote section cache; restoring previous cache");
  if (!Storage.rename(backupPath.c_str(), filePath.c_str())) {
    LOG_ERR("SCT", "Failed to restore section cache backup");
  }
  return false;
}
}  // namespace

Section::Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer,
                 const char* cacheSuffix)
    : Section(*epub, spineIndex, renderer, cacheSuffix) {
  epubOwner = epub;
}

Section::Section(Epub& epub, const int spineIndex, GfxRenderer& renderer, const char* cacheSuffix)
    : epub(&epub),
      spineIndex(spineIndex),
      renderer(renderer),
      filePath(epub.getCachePath() + "/sections/" + std::to_string(spineIndex) + (cacheSuffix ? cacheSuffix : "") +
               ".bin") {
  recoverSectionCacheBackup(filePath);
}

// Suspend any in-progress build so every section.reset() / navigation / sleep path
// persists the pages already laid out as a partial .bin instead of discarding them
// (no-op once a build has completed or never started).
Section::~Section() { suspendBuild(); }

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!ensureBuildFileOpen()) {
    LOG_ERR("SCT", "File not open for writing page %d", builtPageCount_);
    return 0;
  }
  if (!page) {
    LOG_ERR("SCT", "Cannot write null page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  // Scan the page before serializing it so image-only and mixed pages can be
  // protected from the later XHTML byte-density projection without changing
  // the serialized page payload.
  const uint16_t imageUnits = page->imageEstimateUnits(imageEstimateViewportHeight_);
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", builtPageCount_);
    return 0;
  }

  protectedImageUnits_ += imageUnits;
  builtPageCount_++;
  // pageCount is the pages available to read: a rebuild over a partial only raises it
  // once it has laid out more pages than the partial already covers.
  if (builtPageCount_ > pageCount) {
    pageCount = builtPageCount_;
  }
  return position;
}

bool Section::ensureBuildFileOpen() {
  if (file) {
    return true;
  }
  const std::string& tmpSectionPath =
      build_ && !build_->tmpSectionPath.empty() ? build_->tmpSectionPath : activeBuildTmpSectionPath_;
  if (tmpSectionPath.empty()) {
    return false;
  }
  file = Storage.open(tmpSectionPath.c_str(), O_RDWR);
  if (!file) {
    LOG_ERR("SCT", "Failed to reopen section temp file");
    return false;
  }
  if (!file.seek(file.size())) {
    LOG_ERR("SCT", "Failed to seek section temp file");
    file.close();
    return false;
  }
  return true;
}

void Section::releaseBuildFile() {
  if (!build_) {
    return;
  }
  if (file) {
    file.flush();
    if (!file.sync()) {
      LOG_ERR("SCT", "Failed to sync incremental section temp file before release");
    }
    if (!file.close()) {
      LOG_ERR("SCT", "Failed to close incremental section temp file before progress save");
    }
  }
  if (build_->parser) {
    build_->parser->releaseInputFile();
  }
}

bool Section::writeSectionFileHeader(const ReaderRenderSpec& spec) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return false;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_CACHE_MAGIC) + sizeof(SECTION_FILE_VERSION) + sizeof(spec.fontId) +
                                   sizeof(spec.lineCompression) + sizeof(spec.extraParagraphSpacing) +
                                   sizeof(spec.forceParagraphIndents) + sizeof(spec.paragraphAlignment) +
                                   sizeof(spec.viewportWidth) + sizeof(spec.viewportHeight) +
                                   sizeof(spec.hyphenationEnabled) + sizeof(spec.embeddedStyle) +
                                   sizeof(spec.imageRendering) + sizeof(spec.bionicReadingEnabled) +
                                   sizeof(spec.guideReadingEnabled) + sizeof(spec.wordSpacing) + sizeof(uint8_t) +
                                   sizeof(pageCount) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) +
                                   sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  return serialization::tryWritePod(file, SECTION_CACHE_MAGIC) &&
         serialization::tryWritePod(file, SECTION_FILE_VERSION) && serialization::tryWritePod(file, spec.fontId) &&
         serialization::tryWritePod(file, spec.lineCompression) &&
         serialization::tryWritePod(file, spec.extraParagraphSpacing) &&
         serialization::tryWritePod(file, spec.forceParagraphIndents) &&
         serialization::tryWritePod(file, spec.paragraphAlignment) &&
         serialization::tryWritePod(file, spec.viewportWidth) &&
         serialization::tryWritePod(file, spec.viewportHeight) &&
         serialization::tryWritePod(file, spec.hyphenationEnabled) &&
         serialization::tryWritePod(file, spec.embeddedStyle) &&
         serialization::tryWritePod(file, spec.imageRendering) &&
         serialization::tryWritePod(file, spec.bionicReadingEnabled) &&
         serialization::tryWritePod(file, spec.guideReadingEnabled) &&
         serialization::tryWritePod(file, spec.wordSpacing) &&
         serialization::tryWritePod(file, static_cast<uint8_t>(spec.renderMode)) &&
         serialization::tryWritePod(file,
                                    pageCount) &&  // Placeholder for page count (will be initially 0, patched later)
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // Protected image units (patched later)
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // Placeholder for LUT offset (patched later)
         serialization::tryWritePod(file,
                                    static_cast<uint32_t>(0)) &&  // Placeholder for anchor map offset (patched later)
         serialization::tryWritePod(
             file,
             static_cast<uint32_t>(0)) &&  // Placeholder for paragraph LUT offset (patched later)
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // li LUT offset
         serialization::tryWritePod(file, static_cast<uint32_t>(0));    // visible text LUT offset
}

bool Section::loadSectionFile(const ReaderRenderSpec& spec) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  bool filePartial = false;
  {
    uint32_t magic;
    if (!serialization::tryReadPod(file, magic)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: could not read cache magic");
      clearCache();
      return false;
    }
    if (magic != SECTION_CACHE_MAGIC) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: cache magic mismatch");
      clearCache();
      return false;
    }

    uint8_t version;
    if (!serialization::tryReadPod(file, version)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: could not read version");
      clearCache();
      return false;
    }
    if (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
      return false;
    }
    filePartial = (version == SECTION_FILE_PARTIAL_VERSION);

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    bool fileForceParagraphIndents;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    bool fileBionicReadingEnabled;
    bool fileGuideReadingEnabled;
    uint8_t fileWordSpacing;
    uint8_t fileRenderMode;
    if (!serialization::tryReadPod(file, fileFontId) || !serialization::tryReadPod(file, fileLineCompression) ||
        !serialization::tryReadPod(file, fileExtraParagraphSpacing) ||
        !serialization::tryReadPod(file, fileForceParagraphIndents) ||
        !serialization::tryReadPod(file, fileParagraphAlignment) ||
        !serialization::tryReadPod(file, fileViewportWidth) || !serialization::tryReadPod(file, fileViewportHeight) ||
        !serialization::tryReadPod(file, fileHyphenationEnabled) ||
        !serialization::tryReadPod(file, fileEmbeddedStyle) || !serialization::tryReadPod(file, fileImageRendering) ||
        !serialization::tryReadPod(file, fileBionicReadingEnabled) ||
        !serialization::tryReadPod(file, fileGuideReadingEnabled) ||
        !serialization::tryReadPod(file, fileWordSpacing) || !serialization::tryReadPod(file, fileRenderMode)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: truncated section header");
      clearCache();
      return false;
    }

    if (spec.fontId != fileFontId || spec.lineCompression != fileLineCompression ||
        spec.extraParagraphSpacing != fileExtraParagraphSpacing ||
        spec.forceParagraphIndents != fileForceParagraphIndents || spec.paragraphAlignment != fileParagraphAlignment ||
        spec.viewportWidth != fileViewportWidth || spec.viewportHeight != fileViewportHeight ||
        spec.hyphenationEnabled != fileHyphenationEnabled || spec.embeddedStyle != fileEmbeddedStyle ||
        spec.imageRendering != fileImageRendering || spec.bionicReadingEnabled != fileBionicReadingEnabled ||
        spec.guideReadingEnabled != fileGuideReadingEnabled || spec.wordSpacing != fileWordSpacing ||
        static_cast<uint8_t>(spec.renderMode) != fileRenderMode) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  if (!serialization::tryReadPod(file, pageCount)) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: missing page count");
    clearCache();
    return false;
  }
  if (!serialization::tryReadPod(file, partialProtectedImageUnits_)) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: missing protected image units");
    clearCache();
    pageCount = 0;
    return false;
  }

  if (filePartial) {
    // A partial's pageCount is the watermark of a suspended build. Read the watermark
    // trailer (appended after the page-start visible-text LUT) so estimatedTotalPages can extrapolate.
    uint32_t visibleTextLutOffset = 0;
    if (!file.seek(HEADER_SIZE - sizeof(uint32_t)) || !serialization::tryReadPod(file, visibleTextLutOffset)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: missing partial trailer offset");
      clearCache();
      pageCount = 0;
      return false;
    }
    const uint32_t trailerOffset = visibleTextLutOffset + static_cast<uint32_t>(pageCount) * sizeof(uint32_t);
    const bool trailerValid =
        pageCount > 0 && visibleTextLutOffset >= HEADER_SIZE && trailerOffset + 2 * sizeof(uint32_t) <= file.size();
    if (!trailerValid) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: malformed partial section");
      clearCache();
      pageCount = 0;
      return false;
    }
    if (!file.seek(trailerOffset) || !serialization::tryReadPod(file, partialBytesConsumed_) ||
        !serialization::tryReadPod(file, partialTotalBytes_)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: truncated partial trailer");
      clearCache();
      pageCount = 0;
      return false;
    }
    partial_ = true;
    partialPageCount_ = pageCount;
  } else {
    partial_ = false;
    partialPageCount_ = 0;
    protectedImageUnits_ = partialProtectedImageUnits_;
    partialProtectedImageUnits_ = 0;
    partialBytesConsumed_ = 0;
    partialTotalBytes_ = 0;
  }

  // Explicit close() required: member variable persists beyond function scope
  file.close();
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  const std::string tmpBin = binTmpPath();
  if (Storage.exists(tmpBin.c_str())) {
    Storage.remove(tmpBin.c_str());
  }
  const std::string backupPath = sectionBackupPath(filePath);
  if (Storage.exists(backupPath.c_str())) {
    Storage.remove(backupPath.c_str());
  }
  if (!Storage.exists(filePath.c_str())) {
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  return true;
}

bool Section::createSectionFile(const ReaderRenderSpec& spec, const std::function<void()>& popupFn,
                                bool* imagesWereSuppressed, bool* layoutAbortedForLowMemory,
                                const SectionBuildOptions buildOptions) {
  const int fontId = spec.fontId;
  const float lineCompression = spec.lineCompression;
  const bool extraParagraphSpacing = spec.extraParagraphSpacing;
  const bool forceParagraphIndents = spec.forceParagraphIndents;
  const uint8_t paragraphAlignment = spec.paragraphAlignment;
  const uint16_t viewportWidth = spec.viewportWidth;
  const uint16_t viewportHeight = spec.viewportHeight;
  const bool hyphenationEnabled = spec.hyphenationEnabled;
  const bool embeddedStyle = spec.embeddedStyle;
  const uint8_t imageRendering = spec.imageRendering;
  const bool bionicReadingEnabled = spec.bionicReadingEnabled;
  const bool guideReadingEnabled = spec.guideReadingEnabled;
  const uint8_t wordSpacing = spec.wordSpacing;
  const EpubRenderMode renderMode = spec.renderMode;
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto htmlDir = epub->getCachePath() + "/html";
  const auto htmlPath = htmlDir + "/" + std::to_string(spineIndex) + ".html";
  const auto tmpHtmlPath = htmlDir + "/.tmp_" + std::to_string(spineIndex) + ".html";
  const auto tmpSectionPath = filePath + ".tmp";
  activeBuildTmpSectionPath_ = tmpSectionPath;
  struct ClearActiveBuildTmpPath {
    std::string& path;
    ~ClearActiveBuildTmpPath() { path.clear(); }
  } clearActiveBuildTmpPath{activeBuildTmpSectionPath_};
  pageCount = 0;
  builtPageCount_ = 0;
  imageEstimateViewportHeight_ = viewportHeight;
  protectedImageUnits_ = 0;
  if (layoutAbortedForLowMemory) *layoutAbortedForLowMemory = false;
  if (buildOptions.cancellationObserved) *buildOptions.cancellationObserved = false;
  const auto cancelBuild = [&buildOptions]() {
    if (!buildOptions.isCancellationRequested()) {
      return false;
    }
    if (buildOptions.cancellationObserved) {
      *buildOptions.cancellationObserved = true;
    }
    return true;
  };
  const bool effectiveBionicReadingEnabled = bionicReadingEnabled;
  const bool effectiveGuideReadingEnabled = guideReadingEnabled;
  LOG_DBG("SCT",
          "Create section start: spine=%d mode=%u preview=%u viewport=%ux%u image=%u bionic=%u guide=%u free=%u "
          "maxAlloc=%u",
          spineIndex, static_cast<unsigned>(renderMode), buildOptions.isPreview() ? 1U : 0U, viewportWidth,
          viewportHeight, imageRendering, effectiveBionicReadingEnabled, effectiveGuideReadingEnabled,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  MemoryBudget::logEpubHeapPools("section build start");

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Reuse the previously unzipped HTML if we already have it. The unzipped HTML is keyed only on the
  // book (it lives in the per-book cache dir), not on render settings, so it survives the invalidation
  // that wipes the layout (.bin) caches when font/margin/orientation change -- rebuilds then skip zip
  // inflation entirely. It's promoted by an atomic rename as soon as the inflate succeeds (below), so
  // even a window-only giant spine -- whose .bin never finalizes -- still caches its HTML, letting a
  // reopen skip the multi-second inflate. If htmlPath exists it is known-complete.
  const bool reusedHtml = Storage.exists(htmlPath.c_str());
  bool htmlCached = reusedHtml;
  const auto cleanupTempHtml = [&]() {
    if (!htmlCached && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }
  };
  if (cancelBuild()) {
    LOG_DBG("SCT", "Section build cancelled before HTML inflate: spine=%d", spineIndex);
    return false;
  }
  if (!reusedHtml) {
    Storage.mkdir(htmlDir.c_str());

    // Retry logic for SD card timing issues
    bool streamed = false;
    uint32_t fileSize = 0;
    for (int attempt = 0; attempt < 3 && !streamed; attempt++) {
      if (attempt > 0) {
        LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
        delay(50);  // Brief delay before retry
      }

      // Remove any incomplete file from previous attempt before retrying
      if (Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
      }

      HalFile tmpHtml;
      if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
        continue;
      }
      const size_t htmlStreamChunkSize = sectionHtmlStreamChunkSize(buildOptions.isPreview());
      prepareSectionZipInflate(renderer, fontId);
      streamed = epub->readItemContentsToStream(localPath, tmpHtml, htmlStreamChunkSize);
      fileSize = tmpHtml.size();
      // Explicitly close() file before calling Storage.remove()
      tmpHtml.close();

      // If streaming failed, remove the incomplete file immediately
      if (!streamed && Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
        LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
      }
    }

    if (!streamed) {
      LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
      return false;
    }

    // Promote to the persistent HTML cache immediately -- the inflate is complete and the bytes are
    // valid regardless of whether the layout build finishes, so reopening (even a window-only spine
    // that never finalizes its .bin) skips re-inflation. If the rename fails we just parse the temp.
    if (Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
      htmlCached = true;
    } else {
      LOG_DBG("SCT", "Failed to promote HTML cache; parsing from temp");
    }
  }
  const std::string& parsePath = htmlCached ? htmlPath : tmpHtmlPath;

  if (cancelBuild()) {
    LOG_DBG("SCT", "Section build cancelled after HTML inflate: spine=%d", spineIndex);
    cleanupTempHtml();
    return false;
  }

  if (Storage.exists(tmpSectionPath.c_str())) {
    Storage.remove(tmpSectionPath.c_str());
  }

  if (!Storage.openFileForWrite("SCT", tmpSectionPath, file)) {
    cleanupTempHtml();
    return false;
  }
  ReaderRenderSpec effectiveSpec = spec;
  effectiveSpec.bionicReadingEnabled = effectiveBionicReadingEnabled;
  effectiveSpec.guideReadingEnabled = effectiveGuideReadingEnabled;
  if (!writeSectionFileHeader(effectiveSpec)) {
    LOG_ERR("SCT", "Failed to write section header");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    cleanupTempHtml();
    return false;
  }
  // 1024 entries is 8 KB. Stack is too small, and std::vector growth in the page callback can abort on OOM.
  uint16_t lutCapacity = INITIAL_SECTION_PAGE_LUT_ENTRIES;
  auto lut = makeUniqueNoThrow<PageLutEntry[]>(lutCapacity);
  if (!lut) {
    LOG_ERR("SCT", "Failed to allocate page LUT (%u bytes)", static_cast<unsigned>(sizeof(PageLutEntry) * lutCapacity));
    if (layoutAbortedForLowMemory) *layoutAbortedForLowMemory = true;
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    cleanupTempHtml();
    return false;
  }
  uint16_t lutCount = 0;
  bool pageCompletionFailed = false;

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  CssParser* cssParser = nullptr;
  if (embeddedStyle) {
    cssParser = epub->getCssParser();
    if (cssParser) {
      const auto cssHeapBefore = MemoryBudget::snapshot();
      const bool cssLoaded = cssParser->loadFromCache();
      const auto cssHeapAfter = MemoryBudget::snapshot();
      LOG_DBG("SCT", "CSS cache load: ok=%u partial=%u rules=%u free=%u->%u delta=%d maxAlloc=%u->%u delta=%d",
              cssLoaded ? 1U : 0U, cssParser->isCachePartial() ? 1U : 0U, static_cast<unsigned>(cssParser->ruleCount()),
              cssHeapBefore.freeHeap, cssHeapAfter.freeHeap,
              static_cast<int32_t>(cssHeapAfter.freeHeap) - static_cast<int32_t>(cssHeapBefore.freeHeap),
              cssHeapBefore.maxAllocHeap, cssHeapAfter.maxAllocHeap,
              static_cast<int32_t>(cssHeapAfter.maxAllocHeap) - static_cast<int32_t>(cssHeapBefore.maxAllocHeap));
      if (!cssLoaded) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  // Collect TOC anchors for this spine so the parser can insert page breaks at chapter boundaries
  std::vector<std::string> tocAnchors;
  const int startTocIndex = buildOptions.isPreview() ? -1 : epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spineIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  ChapterHtmlSlimParser visitor(
      *epub, parsePath, renderer, fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents,
      paragraphAlignment, viewportWidth, viewportHeight, hyphenationEnabled, effectiveBionicReadingEnabled,
      effectiveGuideReadingEnabled, wordSpacing,
      [this, &lut, &lutCapacity, &lutCount, &pageCompletionFailed, layoutAbortedForLowMemory](
          std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex,
          const uint32_t visibleTextOffset) {
        if (pageCompletionFailed) {
          return;
        }
        if (lutCount == UINT16_MAX) {
          LOG_ERR("SCT", "Section page count exceeded cache format limit");
          pageCompletionFailed = true;
          return;
        }
        if (!ensurePageLutCapacity(lut, lutCapacity, lutCount)) {
          LOG_ERR("SCT", "Failed to grow section page LUT from %u entries", lutCapacity);
          if (layoutAbortedForLowMemory) *layoutAbortedForLowMemory = true;
          pageCompletionFailed = true;
          return;
        }
        const uint32_t fileOffset = this->onPageComplete(std::move(page));
        if (fileOffset == 0) {
          pageCompletionFailed = true;
          return;
        }
        lut[lutCount++] = {fileOffset, paragraphIndex, listItemIndex, visibleTextOffset};
      },
      embeddedStyle, contentBase, imageBasePath, imageRendering, std::move(tocAnchors), popupFn, cssParser, renderMode,
      buildOptions.isPreview() ? std::string(buildOptions.previewAnchor) : std::string{}, buildOptions.previewMaxPages);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  bool cancelled = false;
  bool success = false;
  if (cancelBuild()) {
    cancelled = true;
  } else {
    success = visitor.beginParse();
  }
  while (success && !cancelled) {
    if (cancelBuild()) {
      visitor.abortParse();
      cancelled = true;
      break;
    }
    const auto status = visitor.parseStep();
    if (status == ChapterHtmlSlimParser::ParseStatus::Error) {
      visitor.abortParse();
      success = false;
      break;
    }
    if (status == ChapterHtmlSlimParser::ParseStatus::Done) {
      if (cancelBuild()) {
        visitor.abortParse();
        cancelled = true;
      } else {
        success = visitor.finishParse();
      }
      break;
    }
  }
  LOG_DBG("SCT", "Parser done: spine=%d success=%u pages=%u free=%u maxAlloc=%u", spineIndex, success, pageCount,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  if (imagesWereSuppressed) *imagesWereSuppressed = visitor.wasLowMemoryFallbackTriggered();
  if (layoutAbortedForLowMemory) {
    *layoutAbortedForLowMemory = *layoutAbortedForLowMemory || visitor.wasLowMemoryAbortTriggered();
  }

  if (!htmlCached) {
    if (success || pageCompletionFailed) {
      // Promote the freshly unzipped HTML to the persistent cache so future rebuilds (e.g. after a
      // settings change invalidates the layout caches) can skip zip inflation. If promotion fails,
      // drop the temp file; the section build can still continue from the already-open source.
      if (!Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
        LOG_DBG("SCT", "Failed to promote HTML cache, removing temp");
        Storage.remove(tmpHtmlPath.c_str());
      }
    } else {
      // Parse failed on a freshly unzipped file -- discard it rather than caching a bad source.
      Storage.remove(tmpHtmlPath.c_str());
    }
  }

  if (!success || pageCompletionFailed || cancelled) {
    if (cancelled) {
      LOG_DBG("SCT", "Section build cancelled during parse: spine=%d", spineIndex);
    } else {
      LOG_ERR("SCT", "Failed to parse XML and build pages");
    }
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  // Write LUT
  for (uint16_t i = 0; i < lutCount; i++) {
    if (lut[i].fileOffset == 0) {
      hasFailedLutRecords = true;
      break;
    }
    if (!serialization::tryWritePod(file, lut[i].fileOffset)) {
      hasFailedLutRecords = true;
      break;
    }
  }

  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets)
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = visitor.getAnchors();
  if (!serialization::tryWritePod(file, static_cast<uint16_t>(anchors.size()))) {
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }
  for (const auto& [anchor, page] : anchors) {
    if (!serialization::tryWriteString(file, anchor) || !serialization::tryWritePod(file, page)) {
      file.close();
      Storage.remove(tmpSectionPath.c_str());
      return false;
    }
  }

  const uint32_t paragraphLutOffset = file.position();
  if (!serialization::tryWritePod(file, lutCount)) {
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }
  for (uint16_t i = 0; i < lutCount; i++) {
    if (!serialization::tryWritePod(file, lut[i].paragraphIndex)) {
      file.close();
      Storage.remove(tmpSectionPath.c_str());
      return false;
    }
  }

  const uint32_t liLutFileOffset = static_cast<uint32_t>(file.position());
  for (uint16_t i = 0; i < lutCount; i++) {
    if (!serialization::tryWritePod(file, lut[i].listItemIndex)) {
      file.close();
      Storage.remove(tmpSectionPath.c_str());
      return false;
    }
  }

  const uint32_t visibleTextLutOffset = static_cast<uint32_t>(file.position());
  for (uint16_t i = 0; i < lutCount; i++) {
    if (!serialization::tryWritePod(file, lut[i].visibleTextOffset)) {
      file.close();
      Storage.remove(tmpSectionPath.c_str());
      return false;
    }
  }

  // Patch header with final pageCount and all cache lookup-table offsets.
  if (!file.seek(HEADER_SIZE - sizeof(uint32_t) * 6 - sizeof(pageCount)) ||
      !serialization::tryWritePod(file, pageCount) || !serialization::tryWritePod(file, protectedImageUnits_) ||
      !serialization::tryWritePod(file, lutOffset) || !serialization::tryWritePod(file, anchorMapOffset) ||
      !serialization::tryWritePod(file, paragraphLutOffset) || !serialization::tryWritePod(file, liLutFileOffset) ||
      !serialization::tryWritePod(file, visibleTextLutOffset) || !file.sync()) {
    LOG_ERR("SCT", "Failed to finalize section cache");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  if (!promoteSectionCache(tmpSectionPath, filePath)) {
    LOG_ERR("SCT", "Failed to promote temp section cache into place");
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  if (cssParser) {
    cssParser->clear();
  }
  partial_ = false;
  partialPageCount_ = 0;
  partialProtectedImageUnits_ = 0;
  return true;
}

bool Section::startBuild(const ReaderRenderSpec& spec, const SectionBuildOptions buildOptions,
                         const std::function<void()>& popupFn) {
  const int fontId = spec.fontId;
  const float lineCompression = spec.lineCompression;
  const bool extraParagraphSpacing = spec.extraParagraphSpacing;
  const bool forceParagraphIndents = spec.forceParagraphIndents;
  const uint8_t paragraphAlignment = spec.paragraphAlignment;
  const uint16_t viewportWidth = spec.viewportWidth;
  const uint16_t viewportHeight = spec.viewportHeight;
  const bool hyphenationEnabled = spec.hyphenationEnabled;
  const bool embeddedStyle = spec.embeddedStyle;
  const uint8_t imageRendering = spec.imageRendering;
  const bool bionicReadingEnabled = spec.bionicReadingEnabled;
  const bool guideReadingEnabled = spec.guideReadingEnabled;
  const uint8_t wordSpacing = spec.wordSpacing;
  const EpubRenderMode renderMode = spec.renderMode;
  if (build_) {
    LOG_ERR("SCT", "startBuild called while a build is already active");
    return false;
  }

  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto htmlDir = epub->getCachePath() + "/html";
  const auto htmlPath = htmlDir + "/" + std::to_string(spineIndex) + ".html";
  const auto tmpHtmlPath = htmlDir + "/.tmp_" + std::to_string(spineIndex) + ".html";
  const auto tmpSectionPath = binTmpPath();
  builtPageCount_ = 0;
  imageEstimateViewportHeight_ = viewportHeight;
  protectedImageUnits_ = 0;
  pageCount = partial_ ? partialPageCount_ : 0;
  buildComplete_ = false;
  lastImagesWereSuppressed_ = false;
  lastLayoutAbortedForLowMemory_ = false;

  if (Storage.exists(tmpSectionPath.c_str())) {
    Storage.remove(tmpSectionPath.c_str());
  }

  LOG_DBG("SCT",
          "Start incremental section build: spine=%d mode=%u preview=%u viewport=%ux%u image=%u bionic=%u guide=%u "
          "free=%u maxAlloc=%u",
          spineIndex, static_cast<unsigned>(renderMode), buildOptions.isPreview() ? 1U : 0U, viewportWidth,
          viewportHeight, imageRendering, bionicReadingEnabled, guideReadingEnabled, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());

  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  const bool reusedHtml = Storage.exists(htmlPath.c_str());
  bool htmlCached = reusedHtml;
  const auto cleanupTempHtml = [&]() {
    if (!htmlCached && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }
  };
  if (!reusedHtml) {
    Storage.mkdir(htmlDir.c_str());

    bool streamed = false;
    uint32_t fileSize = 0;
    for (int attempt = 0; attempt < 3 && !streamed; attempt++) {
      if (attempt > 0) {
        LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
        delay(50);
      }
      if (Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
      }

      HalFile tmpHtml;
      if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
        continue;
      }
      const size_t htmlStreamChunkSize = sectionHtmlStreamChunkSize(buildOptions.isPreview());
      prepareSectionZipInflate(renderer, fontId);
      streamed = epub->readItemContentsToStream(localPath, tmpHtml, htmlStreamChunkSize);
      fileSize = tmpHtml.size();
      tmpHtml.close();
      if (!streamed && Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
      }
    }

    if (!streamed) {
      LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
      return false;
    }
    if (Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
      htmlCached = true;
    } else {
      LOG_DBG("SCT", "Failed to promote HTML cache; parsing from temp");
    }
  }

  if (!Storage.openFileForWrite("SCT", tmpSectionPath, file)) {
    cleanupTempHtml();
    return false;
  }
  if (!writeSectionFileHeader(spec)) {
    LOG_ERR("SCT", "Failed to write section header");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    cleanupTempHtml();
    return false;
  }

  auto ctx = makeUniqueNoThrow<BuildContext>();
  if (!ctx) {
    LOG_ERR("SCT", "Failed to allocate section build context");
    lastLayoutAbortedForLowMemory_ = true;
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    cleanupTempHtml();
    return false;
  }
  ctx->lutCapacity = INITIAL_SECTION_PAGE_LUT_ENTRIES;
  ctx->lut = makeUniqueNoThrow<Section::PageLutEntry[]>(ctx->lutCapacity);
  if (!ctx->lut) {
    LOG_ERR("SCT", "Failed to allocate incremental page LUT (%u bytes)",
            static_cast<unsigned>(sizeof(Section::PageLutEntry) * ctx->lutCapacity));
    lastLayoutAbortedForLowMemory_ = true;
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    cleanupTempHtml();
    return false;
  }
  ctx->reusedHtml = htmlCached;
  ctx->htmlPath = htmlPath;
  ctx->tmpHtmlPath = tmpHtmlPath;
  ctx->tmpSectionPath = tmpSectionPath;
  ctx->parsePath = htmlCached ? htmlPath : tmpHtmlPath;

  const size_t lastSlash = localPath.find_last_of('/');
  ctx->contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  ctx->imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  if (embeddedStyle) {
    ctx->cssParser = epub->getCssParser();
    if (ctx->cssParser) {
      const auto cssHeapBefore = MemoryBudget::snapshot();
      const bool cssLoaded = ctx->cssParser->loadFromCache();
      const auto cssHeapAfter = MemoryBudget::snapshot();
      LOG_DBG("SCT", "CSS cache load: ok=%u partial=%u rules=%u free=%u->%u delta=%d maxAlloc=%u->%u delta=%d",
              cssLoaded ? 1U : 0U, ctx->cssParser->isCachePartial() ? 1U : 0U,
              static_cast<unsigned>(ctx->cssParser->ruleCount()), cssHeapBefore.freeHeap, cssHeapAfter.freeHeap,
              static_cast<int32_t>(cssHeapAfter.freeHeap) - static_cast<int32_t>(cssHeapBefore.freeHeap),
              cssHeapBefore.maxAllocHeap, cssHeapAfter.maxAllocHeap,
              static_cast<int32_t>(cssHeapAfter.maxAllocHeap) - static_cast<int32_t>(cssHeapBefore.maxAllocHeap));
      if (!cssLoaded) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  std::vector<std::string> tocAnchors;
  const int startTocIndex = buildOptions.isPreview() ? -1 : epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spineIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  BuildContext* ctxPtr = ctx.get();
  ctx->parser = makeUniqueNoThrow<ChapterHtmlSlimParser>(
      *epub, ctxPtr->parsePath, renderer, fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents,
      paragraphAlignment, viewportWidth, viewportHeight, hyphenationEnabled, bionicReadingEnabled, guideReadingEnabled,
      wordSpacing,
      [this, ctxPtr](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex,
                     const uint32_t visibleTextOffset) {
        if (ctxPtr->pageCompletionFailed) {
          return;
        }
        if (ctxPtr->lutCount == UINT16_MAX ||
            !ensurePageLutCapacity(ctxPtr->lut, ctxPtr->lutCapacity, ctxPtr->lutCount)) {
          LOG_ERR("SCT", "Failed to grow incremental section page LUT from %u entries", ctxPtr->lutCapacity);
          ctxPtr->pageCompletionFailed = true;
          lastLayoutAbortedForLowMemory_ = true;
          return;
        }
        const uint32_t fileOffset = this->onPageComplete(std::move(page));
        if (fileOffset == 0) {
          ctxPtr->pageCompletionFailed = true;
          return;
        }
        ctxPtr->lut[ctxPtr->lutCount++] = {fileOffset, paragraphIndex, listItemIndex, visibleTextOffset};
      },
      embeddedStyle, ctxPtr->contentBase, ctxPtr->imageBasePath, imageRendering, std::move(tocAnchors), popupFn,
      ctxPtr->cssParser, renderMode, buildOptions.isPreview() ? std::string(buildOptions.previewAnchor) : std::string{},
      buildOptions.previewMaxPages);
  if (!ctx->parser) {
    LOG_ERR("SCT", "Failed to allocate section parser");
    lastLayoutAbortedForLowMemory_ = true;
    if (ctx->cssParser) ctx->cssParser->clear();
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    cleanupTempHtml();
    return false;
  }

  Hyphenator::setPreferredLanguage(epub->getLanguage());
  build_ = std::move(ctx);
  if (!build_->parser->beginParse()) {
    LOG_ERR("SCT", "Failed to begin incremental section parse");
    lastLayoutAbortedForLowMemory_ = build_->parser->wasLowMemoryAbortTriggered();
    if (lastLayoutAbortedForLowMemory_ && partial_) {
      suspendBuild();
    } else {
      abandonBuild();
    }
    return false;
  }
  build_->totalBytes = build_->parser->parseTotalBytes();
  return true;
}

bool Section::buildSomeMore(const int maxPages) {
  if (!build_ || !build_->parser) {
    LOG_ERR("SCT", "buildSomeMore called with no active build");
    return false;
  }
  // Pace on pages laid out by THIS build, not pageCount: during a rebuild over a partial,
  // pageCount stays pinned at the partial's watermark until the build passes it, which
  // would otherwise turn one "small" chunk into a blocking rebuild of the whole watermark.
  const int startCount = builtPageCount_;
  for (;;) {
    const auto status = build_->parser->parseStep();
    lastImagesWereSuppressed_ = lastImagesWereSuppressed_ || build_->parser->wasLowMemoryFallbackTriggered();
    lastLayoutAbortedForLowMemory_ = lastLayoutAbortedForLowMemory_ || build_->parser->wasLowMemoryAbortTriggered();
    if (build_->pageCompletionFailed || status == ChapterHtmlSlimParser::ParseStatus::Error) {
      LOG_ERR("SCT", "Failed during incremental section build");
      // A low-memory replay over an existing partial may fail before rebuilding page 0.
      // Suspend in that case too so suspendBuild() keeps the older readable partial.
      if (lastLayoutAbortedForLowMemory_ && (builtPageCount_ > 0 || partial_)) {
        suspendBuild();
      } else {
        abandonBuild();
      }
      return false;
    }
    if (status == ChapterHtmlSlimParser::ParseStatus::Done) {
      return finalizeBuild();
    }
    // ParseStatus::More: yield once we've laid out the requested number of pages.
    if (maxPages > 0 && (builtPageCount_ - startCount) >= maxPages) {
      build_->bytesConsumed = build_->parser->parseBytesConsumed();
      return true;
    }
  }
}

bool Section::hasHtmlCache() const {
  const std::string htmlPath = epub->getCachePath() + "/html/" + std::to_string(spineIndex) + ".html";
  return Storage.exists(htmlPath.c_str());
}

std::optional<uint16_t> Section::findAnchorDuringBuild(const std::string& anchor) const {
  if (!build_ || !build_->parser) return std::nullopt;
  for (const auto& [key, page] : build_->parser->getAnchors()) {
    if (key == anchor) return page;
  }
  return std::nullopt;
}

std::optional<uint16_t> Section::findAnchor(const std::string& anchor) const {
  if (const auto page = findAnchorDuringBuild(anchor)) {
    return page;
  }
  // Fall back to the on-disk anchor map: a finalized section, or a partial whose map
  // covers everything up to its watermark (nullopt past it -- build further and retry).
  return getPageForAnchor(anchor);
}

uint16_t Section::estimatedTotalPages() const {
  // Extrapolation from a suspended session's watermark trailer. A static snapshot, so no EMA
  // damping is needed. Also the best guess while a rebuild is running but hasn't laid out
  // enough pages yet to extrapolate from its own progress.
  const auto partialEstimate = [this]() -> uint16_t {
    if (!partial_ || partialBytesConsumed_ == 0 || partialTotalBytes_ <= partialBytesConsumed_) {
      return pageCount;
    }
    const uint32_t est = PageCountEstimator::estimate(partialPageCount_, partialProtectedImageUnits_,
                                                      partialTotalBytes_, partialBytesConsumed_);
    if (est <= pageCount) return pageCount;
    return static_cast<uint16_t>(est);
  };

  if (!build_) {
    return partial_ ? partialEstimate() : pageCount;  // partial -> extrapolate, finalized -> exact
  }
  const uint32_t consumed = build_->bytesConsumed;
  const uint32_t total = build_->totalBytes;
  if (builtPageCount_ == 0 || consumed == 0 || total <= consumed) return partialEstimate();

  // Raw extrapolation protects image units and scales only the non-image portion
  // by the fraction of XHTML still unparsed. This keeps image-heavy prefixes
  // from being multiplied by their byte size.
  const uint32_t raw = PageCountEstimator::estimate(builtPageCount_, protectedImageUnits_, total, consumed);

  // Damp that jitter with an exponential moving average. Step it once per build advance (keyed on
  // bytesConsumed) rather than per status-bar redraw, so the smoothing rate doesn't depend on how
  // often we repaint. As the build nears the end, consumed -> total and raw -> the built count, so
  // the average settles onto the true count (and finalizeBuild then returns the exact pageCount).
  constexpr float ALPHA = 0.25f;  // weight of each new sample; lower = steadier but slower to settle
  if (build_->smoothedEstimate <= 0) {
    build_->smoothedEstimate = static_cast<float>(raw);  // seed on the first estimate
  } else if (consumed != build_->smoothedAtConsumed) {
    build_->smoothedEstimate += ALPHA * (static_cast<float>(raw) - build_->smoothedEstimate);
  }
  build_->smoothedAtConsumed = consumed;

  const uint64_t est = static_cast<uint64_t>(build_->smoothedEstimate + 0.5f);
  if (est <= pageCount) return pageCount;  // never fewer than the pages already available
  return est > PageCountEstimator::kMaxPages ? PageCountEstimator::kMaxPages : static_cast<uint16_t>(est);
}

uint64_t Section::estimatedProtectedImageUnits() const {
  if (build_ && builtPageCount_ > 0) return protectedImageUnits_;
  if (partial_) return partialProtectedImageUnits_;
  return protectedImageUnits_;
}

uint64_t Section::estimatedNonImageProjectionUnits() const {
  const uint32_t physicalPages =
      build_ && builtPageCount_ > 0 ? builtPageCount_ : (partial_ ? partialPageCount_ : pageCount);
  const uint64_t imageUnits = estimatedProtectedImageUnits();
  const uint64_t knownNonImageUnits = PageCountEstimator::nonImageUnits(physicalPages, imageUnits);
  if (build_ && builtPageCount_ > 0) {
    return PageCountEstimator::projectNonImageUnits(knownNonImageUnits, build_->totalBytes, build_->bytesConsumed);
  }
  if (partial_) {
    return PageCountEstimator::projectNonImageUnits(knownNonImageUnits, partialTotalBytes_, partialBytesConsumed_);
  }
  return knownNonImageUnits;
}

// Write the LUTs and anchor map into the open tmp .bin, patch the header with the built
// page count and table offsets, stamp `version` as the commit point, then swap the tmp
// file over filePath. For SECTION_FILE_PARTIAL_VERSION a watermark trailer
// (bytesConsumed, totalBytes) is appended after the li LUT so a later open can estimate
// the total page count. The parser must still be alive (anchors are read from it).
// On failure the tmp is removed and any pre-existing file at filePath is left intact.
bool Section::commitBuildFile(const uint8_t version, const uint32_t bytesConsumed, const uint32_t totalBytes) {
  const bool asPartial = (version == SECTION_FILE_PARTIAL_VERSION);

  const auto failCommit = [this]() {
    // Explicit close() required before remove (member variable, O_RDWR handle).
    file.close();
    if (build_ && !build_->tmpSectionPath.empty()) {
      Storage.remove(build_->tmpSectionPath.c_str());
    }
    return false;
  };

  if (!ensureBuildFileOpen()) {
    return failCommit();
  }

  const uint32_t lutOffset = file.position();
  for (uint16_t i = 0; i < build_->lutCount; i++) {
    if (build_->lut[i].fileOffset == 0 || !serialization::tryWritePod(file, build_->lut[i].fileOffset)) {
      LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
      return failCommit();
    }
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets). For a
  // partial, skip anchors that landed on the incomplete trailing page the suspend drops.
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = build_->parser->getAnchors();
  uint16_t anchorCount = 0;
  for (const auto& [anchor, page] : anchors) {
    if (!asPartial || page < builtPageCount_) anchorCount++;
  }
  if (!serialization::tryWritePod(file, anchorCount)) {
    return failCommit();
  }
  for (const auto& [anchor, page] : anchors) {
    if (asPartial && page >= builtPageCount_) continue;
    if (!serialization::tryWriteString(file, anchor) || !serialization::tryWritePod(file, page)) {
      return failCommit();
    }
  }

  const uint32_t paragraphLutOffset = file.position();
  if (!serialization::tryWritePod(file, build_->lutCount)) {
    return failCommit();
  }
  for (uint16_t i = 0; i < build_->lutCount; i++) {
    if (!serialization::tryWritePod(file, build_->lut[i].paragraphIndex)) {
      return failCommit();
    }
  }

  const uint32_t liLutFileOffset = static_cast<uint32_t>(file.position());
  for (uint16_t i = 0; i < build_->lutCount; i++) {
    if (!serialization::tryWritePod(file, build_->lut[i].listItemIndex)) {
      return failCommit();
    }
  }

  const uint32_t visibleTextLutOffset = static_cast<uint32_t>(file.position());
  for (uint16_t i = 0; i < build_->lutCount; i++) {
    if (!serialization::tryWritePod(file, build_->lut[i].visibleTextOffset)) {
      return failCommit();
    }
  }

  if (asPartial) {
    // Watermark trailer follows the page-start visible-text LUT.
    if (!serialization::tryWritePod(file, bytesConsumed) || !serialization::tryWritePod(file, totalBytes)) {
      return failCommit();
    }
  }

  // Patch header with the built page count and section offsets...
  if (!file.seek(HEADER_SIZE - sizeof(uint32_t) * 6 - sizeof(builtPageCount_)) ||
      !serialization::tryWritePod(file, builtPageCount_) || !serialization::tryWritePod(file, protectedImageUnits_) ||
      !serialization::tryWritePod(file, lutOffset) || !serialization::tryWritePod(file, anchorMapOffset) ||
      !serialization::tryWritePod(file, paragraphLutOffset) || !serialization::tryWritePod(file, liLutFileOffset) ||
      !serialization::tryWritePod(file, visibleTextLutOffset) || !file.seek(sizeof(SECTION_CACHE_MAGIC)) ||
      !serialization::tryWritePod(file, version) || !file.sync()) {
    LOG_ERR("SCT", "Failed to commit section cache");
    return failCommit();
  }
  // Explicit close() required: member variable persists beyond function scope
  file.close();

  // Keep the readable cache as a backup until the completed replacement is in
  // place. A reboot after the backup rename recovers it in the constructor.
  if (!promoteSectionCache(build_->tmpSectionPath, filePath)) {
    LOG_ERR("SCT", "Failed to move built section into place");
    Storage.remove(build_->tmpSectionPath.c_str());
    return false;
  }
  return true;
}

bool Section::finalizeBuild() {
  if (!build_ || !build_->parser) {
    return false;
  }

  const bool success = build_->parser->finishParse();
  lastImagesWereSuppressed_ = lastImagesWereSuppressed_ || build_->parser->wasLowMemoryFallbackTriggered();
  lastLayoutAbortedForLowMemory_ = lastLayoutAbortedForLowMemory_ || build_->parser->wasLowMemoryAbortTriggered();
  if (!success || build_->pageCompletionFailed) {
    LOG_ERR("SCT", "Failed to finalize parser output");
    abandonBuild();
    return false;
  }

  const bool committed = commitBuildFile(SECTION_FILE_VERSION, 0, 0);
  if (build_->cssParser) build_->cssParser->clear();
  if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  build_.reset();
  if (!committed) {
    // The previous cache was retained/restored by promoteSectionCache(). Keep
    // its in-memory watermark too so this Section remains readable.
    pageCount = partial_ ? partialPageCount_ : 0;
    builtPageCount_ = 0;
    return false;
  }
  buildComplete_ = true;
  partial_ = false;
  partialPageCount_ = 0;
  partialProtectedImageUnits_ = 0;
  pageCount = builtPageCount_;
  return true;
}

void Section::suspendBuild() {
  if (!build_) return;

  // Only worth persisting if this build produced pages a pre-existing partial doesn't
  // already cover; otherwise keep the older (bigger) partial and just drop the tmp.
  const bool worthKeeping = builtPageCount_ > 0 && (!partial_ || builtPageCount_ > partialPageCount_);

  bool committed = false;
  if (worthKeeping) {
    // Capture the parse watermark and commit before tearing the parser down (the anchor
    // map is read from it). The incomplete trailing page is intentionally not flushed:
    // only fully laid-out pages are persisted, and the rebuild re-derives the rest.
    const uint32_t consumed = static_cast<uint32_t>(build_->parser->parseBytesConsumed());
    committed = commitBuildFile(SECTION_FILE_PARTIAL_VERSION, consumed, build_->totalBytes);
    if (committed) {
      partial_ = true;
      partialPageCount_ = builtPageCount_;
      partialProtectedImageUnits_ = protectedImageUnits_;
      partialBytesConsumed_ = consumed;
      partialTotalBytes_ = build_->totalBytes;
      LOG_INF("SCT", "Suspended build: %u pages persisted", builtPageCount_);
    }
  }

  if (build_->parser) build_->parser->abortParse();
  if (build_->cssParser) build_->cssParser->clear();
  if (!committed && file) {
    // Explicit close() required before remove (member variable, O_RDWR handle).
    file.close();
  }
  if (!committed && Storage.exists(binTmpPath().c_str())) {
    Storage.remove(binTmpPath().c_str());
  }
  if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  build_.reset();
  buildComplete_ = false;
  pageCount = partial_ ? partialPageCount_ : 0;
  builtPageCount_ = 0;
}

void Section::abandonBuild() {
  if (!build_) return;
  if (build_->parser) {
    build_->parser->abortParse();
  }
  if (build_->cssParser) {
    build_->cssParser->clear();
  }
  if (file) {
    file.close();
  }
  if (!build_->tmpSectionPath.empty() && Storage.exists(build_->tmpSectionPath.c_str())) {
    Storage.remove(build_->tmpSectionPath.c_str());
  }
  // A parse error would recur against the same HTML, so drop any partial too -- resuming
  // from it would just re-enter the failing build every open.
  if (Storage.exists(filePath.c_str())) {
    Storage.remove(filePath.c_str());
  }
  if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  build_.reset();
  buildComplete_ = false;
  partial_ = false;
  partialPageCount_ = 0;
  partialProtectedImageUnits_ = 0;
  protectedImageUnits_ = 0;
  pageCount = 0;
  builtPageCount_ = 0;
}

std::unique_ptr<Page> Section::loadPageDuringBuild(const int page) {
  if (!build_ || page < 0 || page >= static_cast<int>(build_->lutCount)) {
    return nullptr;
  }
  const uint32_t pos = build_->lut[page].fileOffset;
  if (pos == 0) {
    return nullptr;
  }

  if (!file) {
    HalFile tmp;
    if (!Storage.openFileForRead("SCT", build_->tmpSectionPath, tmp) || !tmp.seek(pos)) {
      return nullptr;
    }
    return Page::deserialize(tmp);
  }

  const uint32_t writePos = file.position();
  if (!file.seek(pos)) {
    return nullptr;
  }
  auto pageData = Page::deserialize(file);
  file.seek(writePos);
  return pageData;
}

// Read a page from the committed file at filePath (finalized section or partial from a
// previous session). Uses a local handle so it is safe while a build holds the member
// `file` open on the tmp .bin.
std::unique_ptr<Page> Section::loadPageAt(const int page) const {
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return nullptr;
  }

  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 5)) {
    return nullptr;
  }
  uint32_t lutOffset;
  if (!serialization::tryReadPod(f, lutOffset) || !f.seek(lutOffset + sizeof(uint32_t) * page)) {
    return nullptr;
  }
  uint32_t pagePos;
  if (!serialization::tryReadPod(f, pagePos) || !f.seek(pagePos)) {
    return nullptr;
  }

  return Page::deserialize(f);
  // No f.close() needed -- DESTRUCTOR_CLOSES_FILE=1 handles it at scope exit
}

std::unique_ptr<Page> Section::loadPage(const int page) {
  if (page < 0) {
    return nullptr;
  }
  if (build_ && page < static_cast<int>(build_->lutCount)) {
    return loadPageDuringBuild(page);
  }
  // Not (yet) in the active build: serve from the file on disk -- a finalized section,
  // or a partial from a previous session whose pages the rebuild hasn't reached again.
  const int onDisk = partial_ ? partialPageCount_ : (build_ ? 0 : pageCount);
  if (page >= onDisk) {
    return nullptr;
  }
  return loadPageAt(page);
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() { return loadPage(currentPage); }

std::optional<uint16_t> Section::getCachedPageCount() const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (fileSize < HEADER_SIZE) {
    return std::nullopt;
  }

  // Only a finalized section's count is the chapter total; a partial's count is just the
  // suspended build's watermark, which would skew progress mapping. Callers fall back to
  // their own estimates.
  uint32_t magic;
  if (!serialization::tryReadPod(f, magic) || magic != SECTION_CACHE_MAGIC) {
    return std::nullopt;
  }
  uint8_t version;
  if (!serialization::tryReadPod(f, version)) {
    return std::nullopt;
  }
  if (version != SECTION_FILE_VERSION) {
    return std::nullopt;
  }

  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 6 - sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  return count;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 4)) {
    return std::nullopt;
  }
  uint32_t anchorMapOffset;
  if (!serialization::tryReadPod(f, anchorMapOffset)) {
    return std::nullopt;
  }
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(anchorMapOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    if (!serialization::tryReadString(f, key) || !serialization::tryReadPod(f, page)) {
      return std::nullopt;
    }
    if (key == anchor) {
      return page;
    }
  }

  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  if (const auto page = findParagraphDuringBuild(pIndex)) {
    return page;
  }

  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 3)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = paragraphLutOffset + sizeof(uint16_t) + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pagePIdx;
    if (!serialization::tryReadPod(f, pagePIdx)) {
      return std::nullopt;
    }
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::findParagraphDuringBuild(const uint16_t pIndex) const {
  if (build_) {
    for (uint16_t i = 0; i < build_->lutCount; i++) {
      if (build_->lut[i].paragraphIndex >= pIndex) {
        return i;
      }
    }
  }
  return std::nullopt;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  if (build_ && page < build_->lutCount) {
    return build_->lut[page].paragraphIndex;
  }

  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 3)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = paragraphLutOffset + sizeof(uint16_t) + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset + sizeof(uint16_t) + page * sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t pIdx;
  if (!serialization::tryReadPod(f, pIdx)) {
    return std::nullopt;
  }
  return pIdx;
}

std::optional<uint16_t> Section::getListItemIndexForPage(const uint16_t page) const {
  if (build_ && page < build_->lutCount) {
    return build_->lut[page].listItemIndex;
  }

  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t liLutOffset;
  if (!serialization::tryReadPod(f, liLutOffset)) {
    return std::nullopt;
  }
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 3)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = liLutOffset + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek(liLutOffset + page * sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t liIdx;
  if (!serialization::tryReadPod(f, liIdx)) {
    return std::nullopt;
  }
  return liIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t liLutOffset;
  if (!serialization::tryReadPod(f, liLutOffset)) {
    return std::nullopt;
  }
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 3)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = liLutOffset + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek(liLutOffset)) {
    return std::nullopt;
  }
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx;
    if (!serialization::tryReadPod(f, pageLiIdx)) {
      return std::nullopt;
    }
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint32_t> Section::getVisibleTextOffsetForPage(const uint16_t page) const {
  if (build_ && page < build_->lutCount) {
    return build_->lut[page].visibleTextOffset;
  }

  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) return std::nullopt;
  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 6 - sizeof(uint16_t))) return std::nullopt;
  uint16_t count = 0;
  if (!serialization::tryReadPod(f, count) || page >= count) return std::nullopt;
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t))) return std::nullopt;
  uint32_t lutOffset = 0;
  if (!serialization::tryReadPod(f, lutOffset) || lutOffset < HEADER_SIZE ||
      lutOffset + (static_cast<uint32_t>(page) + 1U) * sizeof(uint32_t) > fileSize ||
      !f.seek(lutOffset + static_cast<uint32_t>(page) * sizeof(uint32_t))) {
    return std::nullopt;
  }
  uint32_t offset = 0;
  return serialization::tryReadPod(f, offset) ? std::optional<uint32_t>(offset) : std::nullopt;
}

std::optional<uint16_t> Section::getPageForVisibleTextOffset(const uint32_t offset,
                                                             const bool preferFirstAtOffset) const {
  if (build_ && build_->lutCount > 0) {
    uint16_t result = 0;
    for (uint16_t i = 0; i < build_->lutCount; i++) {
      const uint32_t start = build_->lut[i].visibleTextOffset;
      if (start > offset) break;
      result = i;
      if (preferFirstAtOffset && start == offset) break;
    }
    const uint32_t last = build_->lut[build_->lutCount - 1].visibleTextOffset;
    if (offset <= last) return result;
    // An extension build starts from page zero while its previously committed
    // partial remains readable.  Its shorter live prefix must not hide a page
    // that the committed cache already knows how to resolve.
  }

  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) return std::nullopt;
  const uint32_t fileSize = f.size();
  uint32_t magic = 0;
  uint8_t version = 0;
  if (!serialization::tryReadPod(f, magic) || magic != SECTION_CACHE_MAGIC || !serialization::tryReadPod(f, version) ||
      (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION) ||
      !f.seek(HEADER_SIZE - sizeof(uint32_t) * 6 - sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t count = 0;
  if (!serialization::tryReadPod(f, count) || count == 0 || !f.seek(HEADER_SIZE - sizeof(uint32_t))) {
    return std::nullopt;
  }
  uint32_t lutOffset = 0;
  if (!serialization::tryReadPod(f, lutOffset) || lutOffset < HEADER_SIZE ||
      lutOffset + static_cast<uint32_t>(count) * sizeof(uint32_t) > fileSize) {
    return std::nullopt;
  }
  uint16_t result = 0;
  uint32_t last = 0;
  for (uint16_t i = 0; i < count; i++) {
    uint32_t start = 0;
    if (!f.seek(lutOffset + static_cast<uint32_t>(i) * sizeof(uint32_t)) || !serialization::tryReadPod(f, start)) {
      return std::nullopt;
    }
    last = start;
    if (start > offset) break;
    result = i;
    if (preferFirstAtOffset && start == offset) break;
  }
  if (version == SECTION_FILE_PARTIAL_VERSION && offset > last) return std::nullopt;
  return result;
}
