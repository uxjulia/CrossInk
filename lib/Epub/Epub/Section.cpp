#include "Section.h"

#include <Arduino.h>
#if defined(ESP_PLATFORM) || defined(SIMULATOR)
#include <AssetFlashLayout.h>
#endif
#include <BufferedFile.h>
#include <ComplexShaper.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <PrintSerialization.h>
#include <Serialization.h>

#include "Epub/ReferencePageNavigation.h"
#include "Epub/css/CssParser.h"
#include "Epub/image/OptimizerFormat.h"
#include "Page.h"
#include "PreparedEpubLayout.h"
#include "SectionPageIndexSerialization.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint32_t SECTION_CACHE_MAGIC = 0x535843FF;  // bytes: 0xFF, "CXS"

constexpr uint8_t SECTION_FILE_VERSION = 125;
// Suspended incremental build: valid pages plus LUTs and a parse-watermark trailer.
// Change this with layout or payload changes so stale partial pages cannot resume
// under a different layout contract.
constexpr uint8_t SECTION_FILE_PARTIAL_VERSION = 0xEE;
constexpr uint32_t SHAPING_STAMP_MAGIC = 0x31524853;  // "SHR1"
constexpr uint32_t SHAPING_STAMP_BYTES = 7 * sizeof(uint32_t);

uint32_t wordLayoutIdentity(const ReaderRenderSpec& spec, const std::string& language) {
  const uint32_t hyphenationIdentity = spec.hyphenationEnabled ? Hyphenator::patternIdentity(language) : 0u;
  // Extend the stored hyphenation identity only for Focus Reading layouts.
  return spec.focusReadingEnabled ? hyphenationIdentity ^ FocusReading::LAYOUT_IDENTITY : hyphenationIdentity;
}

shaping_recovery::Readiness shapingReadiness() {
#if CROSSINK_SHAPING_FLASH_CACHE
  const bool flash = asset_flash_layout::hasShapingReservation();
#else
  const bool flash = false;
#endif
  return {ESP.getFreeHeap(), ESP.getMaxAllocHeap(), flash};
}

bool writeShapingStamp(HalFile& output, const shaping_recovery::Failures& failures,
                       const shaping_recovery::Readiness& readiness) {
  const uint32_t flags = uint32_t(failures.degraded) | (uint32_t(readiness.flash) << 1) |
                         (uint32_t(failures.faces[0].scripts) << 2) | (uint32_t(failures.faces[1].scripts) << 12) |
                         (uint32_t(failures.sourceUnavailable) << 22) | (uint32_t(failures.blockedFlash) << 23);
  // A torn retry-gate write must never invalidate the page cache.
  const auto checksum =
      SHAPING_STAMP_MAGIC ^ flags ^ readiness.free ^ readiness.largest ^ failures.faces[0].key ^ failures.faces[1].key;
  return serialization::tryWritePod(output, SHAPING_STAMP_MAGIC) && serialization::tryWritePod(output, flags) &&
         serialization::tryWritePod(output, readiness.free) && serialization::tryWritePod(output, readiness.largest) &&
         serialization::tryWritePod(output, failures.faces[0].key) &&
         serialization::tryWritePod(output, failures.faces[1].key) && serialization::tryWritePod(output, checksum);
}

bool readShapingStamp(HalFile& input, shaping_recovery::Failures& failures, shaping_recovery::Readiness& readiness) {
  uint32_t magic = 0, flags = 0, checksum = 0;
  if (!serialization::tryReadPod(input, magic) || magic != SHAPING_STAMP_MAGIC ||
      !serialization::tryReadPod(input, flags) || !serialization::tryReadPod(input, readiness.free) ||
      !serialization::tryReadPod(input, readiness.largest) ||
      !serialization::tryReadPod(input, failures.faces[0].key) ||
      !serialization::tryReadPod(input, failures.faces[1].key) || !serialization::tryReadPod(input, checksum) ||
      checksum !=
          (magic ^ flags ^ readiness.free ^ readiness.largest ^ failures.faces[0].key ^ failures.faces[1].key)) {
    return false;
  }
  failures.degraded = (flags & 1u) != 0;
  failures.sourceUnavailable = (flags & (1u << 22)) != 0;
  failures.blockedFlash = (flags & (1u << 23)) != 0;
  readiness.flash = (flags & 2u) != 0;
  failures.faces[0].scripts = (flags >> 2) & 0x3ff;
  failures.faces[1].scripts = (flags >> 12) & 0x3ff;
  return true;
}

constexpr uint32_t HEADER_SIZE =
    sizeof(bool) + 4 * sizeof(uint32_t) + sizeof(PublisherFonts) + sizeof(SECTION_CACHE_MAGIC) + sizeof(uint8_t) +
    sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint16_t) +
    sizeof(bool) + sizeof(uint32_t) + sizeof(bool) + sizeof(uint8_t) + sizeof(bool) + sizeof(bool) + sizeof(uint8_t) +
    sizeof(int8_t) + sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) +
    sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
constexpr size_t SECTION_HTML_STREAM_CHUNK_SIZE = 8192;
// One staging buffer per build (not per page) turns a page's hundreds of 1-4
// byte field writes into a handful of SD writes. Larger fields (a long line's
// text arena) bypass it; a failed allocation falls back to direct writes.
constexpr size_t PAGE_WRITE_BUFFER_SIZE = 1024;
constexpr size_t LOW_MEMORY_SECTION_HTML_STREAM_CHUNK_SIZE = 1024;

class CancellableSectionHtmlOutput final : public Print {
 public:
  CancellableSectionHtmlOutput(HalFile& file, const SectionBuildOptions& options) : file(file), options(options) {}

  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t length) override {
    if (options.isCancellationRequested()) {
      cancelled = true;
      return 0;
    }
    const size_t written = file.write(bytes, length);
    writeFailed = written != length;
    return written;
  }

  bool cancelled = false;
  bool writeFailed = false;

 private:
  HalFile& file;
  const SectionBuildOptions& options;
};

void prepareSectionZipInflate(GfxRenderer& renderer, const int fontId) {
  const auto required = InflateStream::requiredInternalStorageSize(true);
  if (ESP.getMaxAllocHeap() >= required || !renderer.isSdCardFont(fontId)) return;
  renderer.releaseSdCardFontBitmapsForLowMemory(fontId);
  if (ESP.getMaxAllocHeap() < required) renderer.releaseSdCardFontForLowMemory(fontId);
}

size_t sectionHtmlStreamChunkSize(const bool preview) {
  if (preview) {
    return LOW_MEMORY_SECTION_HTML_STREAM_CHUNK_SIZE;
  }

  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  const size_t largeStreamBudget =
      InflateStream::requiredInternalStorageSize(true) + (2U * SECTION_HTML_STREAM_CHUNK_SIZE);
  if (maxAlloc < largeStreamBudget) {
    LOG_DBG("SCT", "Using low-memory HTML stream chunk (maxAlloc=%u)", maxAlloc);
    return LOW_MEMORY_SECTION_HTML_STREAM_CHUNK_SIZE;
  }
  return SECTION_HTML_STREAM_CHUNK_SIZE;
}

enum class MalformedSourceCheck { Matches, CachedHtmlMismatch, Unavailable };

MalformedSourceCheck checkMalformedInputAgainstOriginalSource(const Epub& epub, const std::string& itemPath,
                                                              const std::string& cachedHtmlPath,
                                                              const ChapterHtmlSlimParser& parser) {
  const uint32_t inputLength = parser.malformedInputCrcLength();
  Epub::ItemIdentity identity;
  if (inputLength == 0 || !epub.getItemIdentity(itemPath, identity) || identity.encrypted || identity.adobeObfuscated ||
      inputLength > identity.uncompressedSize) {
    return MalformedSourceCheck::Unavailable;
  }

  HalFile input;
  if (!Storage.openFileForRead("SCT", cachedHtmlPath, input)) return MalformedSourceCheck::Unavailable;
  if (input.size() != identity.uncompressedSize) {
    input.close();
    return MalformedSourceCheck::CachedHtmlMismatch;
  }
  if (!input.seek(inputLength)) {
    input.close();
    return MalformedSourceCheck::Unavailable;
  }
  uint32_t crc = parser.malformedInputCrc();
  uint8_t buffer[256];
  bool readable = true;
  while (input.position() < identity.uncompressedSize) {
    const size_t wanted = std::min<size_t>(sizeof(buffer), identity.uncompressedSize - input.position());
    const int count = input.read(buffer, wanted);
    if (count <= 0 || static_cast<size_t>(count) != wanted) {
      readable = false;
      break;
    }
    crc = OptimizerFormat::crc(crc, buffer, static_cast<size_t>(count));
  }
  const bool closed = input.close();
  if (!readable || !closed) return MalformedSourceCheck::Unavailable;
  return crc == identity.crc32 ? MalformedSourceCheck::Matches : MalformedSourceCheck::CachedHtmlMismatch;
}

// Each FAT directory holds at most 32 chapter numbers (plus their layout variants).
std::string chapterDirectory(const Epub& epub, const char* root, int spine) {
  return epub.getCachePath() + root + "/" + std::to_string(spine / Section::ChaptersPerDirectory);
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

bool promoteSectionCache(const Epub& epub, const std::string& tmpPath, const std::string& filePath) {
  if (!prepared_epub_layout::invalidate(epub.getCachePath())) return false;
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
      filePath(layoutDirectory(epub, spineIndex) + "/" + std::to_string(spineIndex) + (cacheSuffix ? cacheSuffix : "") +
               ".bin") {
  recoverSectionCacheBackup(filePath);
}

// Suspend any in-progress build so every section.reset() / navigation / sleep path
// persists the pages already laid out as a partial .bin instead of discarding them
// (no-op once a build has completed or never started).
Section::~Section() { suspendBuild(); }

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (publisherFontsEnabled_) {
    publisherFontFaces_ = renderer.bookFontUsage();
    publisherLoadedFaces_ = renderer.bookFontLoaded();
    publisherFaceSignature_ = renderer.bookFontSignature(publisherFontFaces_);
  }
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
  const uint16_t imageUnits = page->imageEstimateUnits(imageEstimateViewportWidth_, imageEstimateViewportHeight_);
  // Stage the page's many small field writes in RAM and hand SD a few large
  // writes instead; the bytes written to the file are unchanged.
  uint8_t* const stage = build_ ? build_->pageWriteBuffer.get() : fullBuildPageWriteBuffer_;
  serialization::BufferedFilePrint out(file, stage, PAGE_WRITE_BUFFER_SIZE);
  if (!page->serialize(out) || !out.commit()) {
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
    if (!file.sync()) {
      LOG_ERR("SCT", "Failed to sync incremental section temp file before release");
      build_->pageCompletionFailed = true;
    }
    if (!file.close()) {
      LOG_ERR("SCT", "Failed to close incremental section temp file before progress save");
      build_->pageCompletionFailed = true;
    }
  }
  if (build_->parser) {
    build_->parser->releaseInputFile();
  }
}

#if CROSSINK_SCALABLE_FONTS
uint32_t Section::publisherBodyFontFamily() {
  // The incremental parser may own this HTML reader. Release it before probing;
  // it resumes from its saved offset on the next build step.
  releaseBuildFile();
  auto* css = epub->getCssParser();
  if (!css) return 0;
  const bool ownsCss = !build_ || !build_->cssParser;
  if (ownsCss && !css->loadFromCache()) {
    LOG_ERR("SCT", "Cannot load CSS for publisher body font");
    css->clear();
    return 0;
  }
  const std::string path =
      build_ ? build_->parsePath
             : chapterDirectory(*epub, "/content", spineIndex) + "/" + std::to_string(spineIndex) + ".html";
  const auto family = ChapterHtmlSlimParser::publisherBodyFontFamily(path, *css);
  if (ownsCss) css->clear();
  return family;
}
#endif

bool Section::writePublisherFontState() {
  return file.seek(HEADER_SIZE - sizeof(uint32_t) * 9 - sizeof(pageCount)) &&
         serialization::tryWritePod(file, publisherFontFaces_) &&
         serialization::tryWritePod(file, publisherFaceSignature_) &&
         serialization::tryWritePod(file, publisherLoadedFaces_);
}

bool Section::writeSectionFileHeader(const ReaderRenderSpec& spec) {
  baseLayoutSignature_ = readerRenderSpecSignature(spec);
  publisherFontsEnabled_ = spec.publisherFonts != PublisherFonts::Off;
  publisherFontFaces_ = publisherFaceSignature_ = publisherLoadedFaces_ = 0;
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return false;
  }
  static_assert(
      HEADER_SIZE == sizeof(spec.ruled) + 4 * sizeof(uint32_t) + sizeof(PublisherFonts) + sizeof(SECTION_CACHE_MAGIC) +
                         sizeof(SECTION_FILE_VERSION) + sizeof(spec.fontId) + sizeof(spec.lineCompression) +
                         sizeof(spec.extraParagraphSpacing) + sizeof(spec.forceParagraphIndents) +
                         sizeof(spec.paragraphAlignment) + sizeof(spec.viewportWidth) + sizeof(spec.viewportHeight) +
                         sizeof(spec.hyphenationEnabled) + sizeof(uint32_t) + sizeof(spec.embeddedStyle) +
                         sizeof(spec.imageRendering) + sizeof(spec.focusReadingEnabled) +
                         sizeof(spec.guideReadingEnabled) + sizeof(spec.wordSpacing) + sizeof(spec.characterSpacing) +
                         sizeof(uint8_t) + sizeof(pageCount) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t) +
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
         serialization::tryWritePod(file, wordLayoutIdentity(spec, epub->getLanguage())) &&
         serialization::tryWritePod(file, spec.embeddedStyle) &&
         serialization::tryWritePod(file, spec.imageRendering) &&
         serialization::tryWritePod(file, spec.focusReadingEnabled) &&
         serialization::tryWritePod(file, spec.guideReadingEnabled) && serialization::tryWritePod(file, spec.ruled) &&
         serialization::tryWritePod(file, spec.wordSpacing) &&
         serialization::tryWritePod(file, spec.characterSpacing) &&
         serialization::tryWritePod(file, static_cast<uint8_t>(spec.renderMode)) &&
         serialization::tryWritePod(file, spec.publisherFonts) &&
         serialization::tryWritePod(file, spec.publisherFontSignature) &&
         serialization::tryWritePod(file, publisherFontFaces_) &&
         serialization::tryWritePod(file, publisherFaceSignature_) &&
         serialization::tryWritePod(file, publisherLoadedFaces_) &&
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

bool Section::loadSectionFile(const ReaderRenderSpec& spec, bool* layoutMismatch) {
  if (layoutMismatch) *layoutMismatch = false;
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
      return false;
    }
    if (magic != SECTION_CACHE_MAGIC) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: cache magic mismatch");
      clearCache();
      resetLoadedCacheState();
      return false;
    }

    uint8_t version;
    if (!serialization::tryReadPod(file, version)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: could not read version");
      return false;
    }
    if (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
      resetLoadedCacheState();
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
    uint32_t fileWordLayoutIdentity;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    bool fileFocusReadingEnabled;
    bool fileGuideReadingEnabled;
    bool fileRuled;
    uint8_t fileWordSpacing;
    int8_t fileCharacterSpacing;
    uint8_t fileRenderMode;
    PublisherFonts filePublisherFonts;
    uint32_t filePublisherFontSignature;
    if (!serialization::tryReadPod(file, fileFontId) || !serialization::tryReadPod(file, fileLineCompression) ||
        !serialization::tryReadPod(file, fileExtraParagraphSpacing) ||
        !serialization::tryReadPod(file, fileForceParagraphIndents) ||
        !serialization::tryReadPod(file, fileParagraphAlignment) ||
        !serialization::tryReadPod(file, fileViewportWidth) || !serialization::tryReadPod(file, fileViewportHeight) ||
        !serialization::tryReadPod(file, fileHyphenationEnabled) ||
        !serialization::tryReadPod(file, fileWordLayoutIdentity) ||
        !serialization::tryReadPod(file, fileEmbeddedStyle) || !serialization::tryReadPod(file, fileImageRendering) ||
        !serialization::tryReadPod(file, fileFocusReadingEnabled) ||
        !serialization::tryReadPod(file, fileGuideReadingEnabled) || !serialization::tryReadPod(file, fileRuled) ||
        !serialization::tryReadPod(file, fileWordSpacing) || !serialization::tryReadPod(file, fileCharacterSpacing) ||
        !serialization::tryReadPod(file, fileRenderMode) || !serialization::tryReadPod(file, filePublisherFonts) ||
        !serialization::tryReadPod(file, filePublisherFontSignature)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: truncated section header");
      return false;
    }

    if (spec.fontId != fileFontId || spec.lineCompression != fileLineCompression ||
        spec.extraParagraphSpacing != fileExtraParagraphSpacing ||
        spec.forceParagraphIndents != fileForceParagraphIndents || spec.paragraphAlignment != fileParagraphAlignment ||
        spec.viewportWidth != fileViewportWidth || spec.viewportHeight != fileViewportHeight ||
        spec.hyphenationEnabled != fileHyphenationEnabled ||
        fileWordLayoutIdentity != wordLayoutIdentity(spec, epub->getLanguage()) ||
        spec.embeddedStyle != fileEmbeddedStyle || spec.imageRendering != fileImageRendering ||
        spec.focusReadingEnabled != fileFocusReadingEnabled || spec.guideReadingEnabled != fileGuideReadingEnabled ||
        spec.ruled != fileRuled || spec.wordSpacing != fileWordSpacing ||
        spec.characterSpacing != fileCharacterSpacing || static_cast<uint8_t>(spec.renderMode) != fileRenderMode ||
        spec.publisherFonts != filePublisherFonts || spec.publisherFontSignature != filePublisherFontSignature) {
      file.close();
      if (layoutMismatch) *layoutMismatch = true;
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      resetLoadedCacheState();
      return false;
    }
  }

  uint32_t requiredFaces = 0, faceSignature = 0, loadedFaces = 0;
  if (!serialization::tryReadPod(file, requiredFaces) || !serialization::tryReadPod(file, faceSignature) ||
      !serialization::tryReadPod(file, loadedFaces) ||
      !renderer.restoreBookFonts(requiredFaces, loadedFaces, faceSignature)) {
    file.close();
    LOG_ERR("SCT", "Publisher font availability changed");
    resetLoadedCacheState();
    return false;
  }

  baseLayoutSignature_ = readerRenderSpecSignature(spec);
  publisherFontFaces_ = requiredFaces;
  publisherFaceSignature_ = faceSignature;
  publisherLoadedFaces_ = loadedFaces;

  if (!serialization::tryReadPod(file, pageCount)) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: missing page count");
    return false;
  }
  if (!serialization::tryReadPod(file, partialProtectedImageUnits_)) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: missing protected image units");
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
      pageCount = 0;
      return false;
    }
    const uint64_t trailerOffset =
        static_cast<uint64_t>(visibleTextLutOffset) + (2U * static_cast<uint32_t>(pageCount) + 1U) * sizeof(uint32_t);
    const bool trailerValid =
        pageCount > 0 && visibleTextLutOffset >= HEADER_SIZE && trailerOffset + 2 * sizeof(uint32_t) <= file.size();
    if (!trailerValid) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: malformed partial section");
      pageCount = 0;
      return false;
    }
    if (!file.seek(trailerOffset) || !serialization::tryReadPod(file, partialBytesConsumed_) ||
        !serialization::tryReadPod(file, partialTotalBytes_)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: truncated partial trailer");
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

  // The visible/reference offsets and the optional incremental watermark have
  // fixed sizes. Old files end here and stay readable, even if recovery fails.
  uint32_t visibleLut = 0;
  shapingTrailerOffset_ = 0;
  shapingTrailerKnown_ = false;
  cachedShapingFailures_ = {};
  cachedShapingReadiness_ = {};
  if (file.seek(HEADER_SIZE - sizeof(uint32_t)) && serialization::tryReadPod(file, visibleLut) &&
      visibleLut >= HEADER_SIZE) {
    const uint64_t offset = uint64_t(visibleLut) + (2u * pageCount + 1u + (filePartial ? 2u : 0u)) * sizeof(uint32_t);
    if (offset <= file.size() && offset <= UINT32_MAX) {
      shapingTrailerOffset_ = static_cast<uint32_t>(offset);
      if (file.size() - offset >= SHAPING_STAMP_BYTES && file.seek(offset)) {
        shapingTrailerKnown_ = readShapingStamp(file, cachedShapingFailures_, cachedShapingReadiness_);
      }
    }
  }
  file.close();
  return true;
}

void Section::resetLoadedCacheState() {
  pageCount = 0;
  builtPageCount_ = 0;
  buildComplete_ = false;
  partial_ = false;
  partialPageCount_ = 0;
  partialProtectedImageUnits_ = 0;
  partialBytesConsumed_ = 0;
  partialTotalBytes_ = 0;
  protectedImageUnits_ = 0;
  baseLayoutSignature_ = 0;
  publisherFontsEnabled_ = false;
  publisherFontFaces_ = 0;
  publisherFaceSignature_ = 0;
  publisherLoadedFaces_ = 0;
  shapingTrailerOffset_ = 0;
  shapingTrailerKnown_ = false;
  cachedShapingFailures_ = {};
  cachedShapingReadiness_ = {};
}

bool Section::hasCommittedCacheOnDisk() const {
  if (!Storage.exists(filePath.c_str())) return false;

  HalFile previous;
  if (!Storage.openFileForRead("SCT", filePath, previous)) {
    // The path exists but storage could not open it. Treat that as uncertain and
    // keep it safe if a rebuild later turns out to be only malformed salvage.
    return true;
  }

  bool valid = previous.size() >= HEADER_SIZE;
  uint32_t magic = 0;
  uint8_t version = 0;
  uint16_t count = 0;
  uint32_t pageIndexOffset = 0;
  if (valid && (!serialization::tryReadPod(previous, magic) || !serialization::tryReadPod(previous, version))) {
    valid = true;  // A read error is not proof that the committed cache is disposable.
  } else if (valid && (magic != SECTION_CACHE_MAGIC ||
                       (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION))) {
    valid = false;
  } else if (valid &&
             (!previous.seek(HEADER_SIZE - sizeof(uint32_t) * 6 - sizeof(uint16_t)) ||
              !serialization::tryReadPod(previous, count) || !previous.seek(HEADER_SIZE - sizeof(uint32_t) * 5) ||
              !serialization::tryReadPod(previous, pageIndexOffset))) {
    valid = true;  // Preserve on an inconclusive header/table read.
  } else if (valid) {
    const uint64_t pageIndexEnd =
        static_cast<uint64_t>(pageIndexOffset) + static_cast<uint64_t>(count) * sizeof(uint32_t);
    valid = count > 0 && pageIndexOffset >= HEADER_SIZE && pageIndexEnd <= previous.size();
  }

  const bool closed = previous.close();
  // A close error also leaves the cache's validity uncertain; never turn that
  // storage problem into permission to overwrite it after markup salvage.
  return valid || !closed;
}

bool Section::claimShapingRecovery(const ReaderRenderSpec& spec, int targetPage) {
  if (!shapingTrailerOffset_ || !pageCount) return false;
  const auto now = shapingReadiness();
  if (shapingTrailerKnown_) {
    if (!cachedShapingFailures_.degraded ||
        (!cachedShapingFailures_.sourceUnavailable && !now.improvedFrom(cachedShapingReadiness_)))
      return false;
#if CROSSINK_SHAPING_FLASH_CACHE
    // A missing C3 reservation is a known blocked condition, not a reason
    // to reopen large font files on every reader entry.
    if (!now.flash && cachedShapingFailures_.blockedFlash) return false;
#endif
  } else {
    // Pre-fix caches do not record shaping success. Limit lazy migration to
    // readers with an actual shaping source, and keep the old readable pages
    // until a quality-checked rebuild succeeds.
    if (!renderer.hasComplexShapingSource(spec.fontId)) return false;
#if CROSSINK_SHAPING_FLASH_CACHE
    if (!now.flash) return false;
#endif
    // Avoid rebuilding a healthy Latin or already-shaped old section just
    // because the selected font happens to contain shaping tables.
    const auto page = loadPage(std::clamp(targetPage, 0, int(pageCount) - 1));
    if (!page || !page->hasRecoverableUnshapedText(renderer, spec.fontId)) return false;
    cachedShapingFailures_ = {};
    cachedShapingFailures_.degraded = true;
  }
  // Do not burn a persistent retry solely because an opportunistic probe
  // failed (e.g. temporary SD read/slot pressure). Only attempt the expensive
  // replacement after the recorded sources actually load again.
  if (!ComplexShaper::probeRecovery(cachedShapingFailures_)) return false;
  cachedShapingFailures_.sourceUnavailable = false;
  auto stamp = Storage.open(filePath.c_str(), O_RDWR);
  if (!stamp || !stamp.seek(shapingTrailerOffset_) || !writeShapingStamp(stamp, cachedShapingFailures_, now) ||
      !stamp.sync()) {
    return false;  // no persistent rate limit: keep serving the old layout
  }
  stamp.close();
  cachedShapingReadiness_ = now;
  shapingTrailerKnown_ = true;
  return true;
}

std::string Section::layoutDirectory(const Epub& epub, int spineIndex) {
  return chapterDirectory(epub, "/layout", spineIndex);
}

std::array<uint32_t, 3> Section::completedLayoutIdentity(const Epub& epub, const ReaderRenderSpec& spec) {
  return {SECTION_FILE_VERSION, readerRenderSpecSignature(spec), wordLayoutIdentity(spec, epub.getLanguage())};
}

bool Section::collectPreparedFaces(uint32_t& faces) const {
  if (publisherFontFaces_ != publisherLoadedFaces_) return false;
  faces |= publisherFontFaces_;
  return true;
}

bool Section::clearCache() const {
  if (!prepared_epub_layout::invalidate(epub->getCachePath())) return false;
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
  const bool previousCacheCompatible = pageCount > 0 && baseLayoutSignature_ == readerRenderSpecSignature(spec);
  const bool previousCacheCandidate = Storage.exists(filePath.c_str());
  const uint32_t previousPageCount = pageCount;
  const uint32_t previousBuiltPageCount = builtPageCount_;
  const uint32_t previousProtectedImageUnits = protectedImageUnits_;
  const bool previousBuildComplete = buildComplete_;
  const uint32_t previousBaseLayoutSignature = baseLayoutSignature_;
  const bool previousPublisherFontsEnabled = publisherFontsEnabled_;
  const uint32_t previousPublisherFontFaces = publisherFontFaces_;
  const uint32_t previousPublisherFaceSignature = publisherFaceSignature_;
  const uint32_t previousPublisherLoadedFaces = publisherLoadedFaces_;
  bool replacementCommitted = false;
  const ScopedCleanup restorePageCounts{
      [this, &replacementCommitted, previousCacheCompatible, previousPageCount, previousBuiltPageCount,
       previousProtectedImageUnits, previousBuildComplete, previousBaseLayoutSignature, previousPublisherFontsEnabled,
       previousPublisherFontFaces, previousPublisherFaceSignature, previousPublisherLoadedFaces] {
        if (replacementCommitted) return;
        baseLayoutSignature_ = previousBaseLayoutSignature;
        publisherFontsEnabled_ = previousPublisherFontsEnabled;
        publisherFontFaces_ = previousPublisherFontFaces;
        publisherFaceSignature_ = previousPublisherFaceSignature;
        publisherLoadedFaces_ = previousPublisherLoadedFaces;
        pageCount = previousCacheCompatible ? previousPageCount : 0;
        builtPageCount_ = previousCacheCompatible ? previousBuiltPageCount : 0;
        protectedImageUnits_ = previousCacheCompatible ? previousProtectedImageUnits : 0;
        buildComplete_ = previousCacheCompatible && previousBuildComplete;
        if (!previousCacheCompatible) {
          partial_ = false;
          partialPageCount_ = 0;
          partialProtectedImageUnits_ = 0;
          partialBytesConsumed_ = 0;
          partialTotalBytes_ = 0;
        }
      }};
  auto initialShapingReadiness = shapingReadiness();
  shaping_recovery::Failures shapingFailures;
  shapingFailures.expected = buildOptions.expectedShapingRecovery;
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
  const bool focusReadingEnabled = spec.focusReadingEnabled;
  const bool guideReadingEnabled = spec.guideReadingEnabled;
  const uint8_t wordSpacing = spec.wordSpacing;
  const EpubRenderMode renderMode = spec.renderMode;
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto htmlDir = chapterDirectory(*epub, "/content", spineIndex);
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
  imageEstimateViewportWidth_ = viewportWidth;
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
  const bool effectiveFocusReadingEnabled = focusReadingEnabled;
  const bool effectiveGuideReadingEnabled = guideReadingEnabled;
  LOG_DBG("SCT",
          "Create section start: spine=%d mode=%u preview=%u viewport=%ux%u image=%u focus=%u guide=%u free=%u "
          "maxAlloc=%u",
          spineIndex, static_cast<unsigned>(renderMode), buildOptions.isPreview() ? 1U : 0U, viewportWidth,
          viewportHeight, imageRendering, effectiveFocusReadingEnabled, effectiveGuideReadingEnabled, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
  MemoryBudget::logEpubHeapPools("section build start");

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = chapterDirectory(*epub, "/layout", spineIndex);
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
    for (int attempt = 0; attempt < 3 && !streamed; attempt++) {
      if (cancelBuild()) {
        cleanupTempHtml();
        return false;
      }
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
      const bool synced = !streamed || tmpHtml.sync();
      const bool closed = tmpHtml.close();
      streamed = streamed && synced && closed;
      if (!synced || !closed) LOG_ERR("SCT", "Failed to sync or close streamed chapter HTML");

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

    // Keep fresh XHTML temporary when there is an old layout to protect. The
    // parser will promote it after source validation succeeds.
    if (!previousCacheCandidate && Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
      htmlCached = true;
    } else {
      LOG_DBG("SCT", "Deferring HTML cache promotion until section parse validation");
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
  effectiveSpec.focusReadingEnabled = effectiveFocusReadingEnabled;
  effectiveSpec.guideReadingEnabled = effectiveGuideReadingEnabled;
  if (!writeSectionFileHeader(effectiveSpec)) {
    LOG_ERR("SCT", "Failed to write section header");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    cleanupTempHtml();
    return false;
  }
  SectionPageIndex pageIndex;
  bool pageCompletionFailed = false;

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  CssParser* cssParser = nullptr;
  if (embeddedStyle) {
    cssParser = epub->getCssParser();
    if (cssParser) {
      const bool cssCacheExisted = cssParser->hasCache();
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
        if (!cssCacheExisted) {
          LOG_ERR("SCT", "CSS was requested but its cache is missing; preserving the previous section cache");
        } else {
          LOG_ERR("SCT", "Could not hydrate CSS cache; preserving the previous section cache");
        }
        cssParser->clear();
        file.close();
        Storage.remove(tmpSectionPath.c_str());
        cleanupTempHtml();
        return false;
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

  auto pageWriteBuffer = makeUniqueNoThrow<uint8_t[]>(PAGE_WRITE_BUFFER_SIZE);
  if (!pageWriteBuffer) LOG_ERR("SCT", "Page write buffer alloc failed; writing pages unstaged");
  fullBuildPageWriteBuffer_ = pageWriteBuffer.get();
  // Clear the borrowed pointer on every return path, before the buffer is freed.
  struct ClearPageWriteBuffer {
    uint8_t*& buffer;
    ~ClearPageWriteBuffer() { buffer = nullptr; }
  } clearPageWriteBuffer{fullBuildPageWriteBuffer_};

  ChapterHtmlSlimParser visitor(
      *epub, parsePath, renderer, fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents,
      paragraphAlignment, viewportWidth, viewportHeight, hyphenationEnabled, effectiveFocusReadingEnabled,
      effectiveGuideReadingEnabled, wordSpacing,
      [this, &pageIndex, &pageCompletionFailed, layoutAbortedForLowMemory, buildOptions](
          std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex,
          const uint32_t visibleTextOffset, const uint32_t referenceTextOffset) {
        if (pageCompletionFailed) {
          return;
        }
        const auto prepareResult = pageIndex.prepareAppend();
        if (prepareResult == SectionPageIndex::PrepareResult::EntryLimit) {
          LOG_ERR("SCT", "Section page count exceeded cache format limit");
          pageCompletionFailed = true;
          return;
        }
        if (prepareResult == SectionPageIndex::PrepareResult::AllocationFailure) {
          LOG_ERR("SCT", "Failed to allocate section page index storage at %u entries",
                  static_cast<unsigned>(pageIndex.size()));
          if (layoutAbortedForLowMemory) *layoutAbortedForLowMemory = true;
          pageCompletionFailed = true;
          return;
        }
        if (buildOptions.referenceUnitsAreCharacters && buildOptions.resolvedReferencePage &&
            referenceTextOffset <= buildOptions.referenceUnitOffset) {
          *buildOptions.resolvedReferencePage = static_cast<uint16_t>(pageIndex.size());
        }
        const uint32_t fileOffset = this->onPageComplete(std::move(page));
        if (fileOffset == 0) {
          pageCompletionFailed = true;
          return;
        }
        pageIndex.appendPrepared({fileOffset, paragraphIndex, listItemIndex, visibleTextOffset, referenceTextOffset});
      },
      embeddedStyle, contentBase, imageBasePath, imageRendering, std::move(tocAnchors), popupFn, cssParser, renderMode,
      buildOptions.isPreview() ? std::string(buildOptions.previewAnchor) : std::string{}, buildOptions.previewMaxPages,
      true, spec.characterSpacing);
  visitor.setTrackMalformedInputCrc(previousCacheCandidate);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  ComplexShaper::setDocumentLanguage(epub->getLanguage().c_str());
  bool cancelled = false;
  bool success = false;
  {
    const ComplexShaper::LayoutObservation observe(shapingFailures);
    if (cancelBuild()) {
      cancelled = true;
    } else {
      visitor.setRuled(spec.ruled);
      visitor.setPublisherFonts(spec.publisherFonts);
      success = visitor.beginParse();
    }
    while (success && !cancelled) {
      if (cancelBuild()) {
        visitor.abortParse();
        cancelled = true;
        break;
      }
      if (buildOptions.onBuildCheckpoint) buildOptions.onBuildCheckpoint();
      const auto status = visitor.parseStep();
      if (pageCompletionFailed) {
        // A failed page write is terminal for this temp file; stop parsing the chapter now.
        visitor.abortParse();
        success = false;
        break;
      }
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
  }
  LOG_DBG("SCT", "Parser done: spine=%d success=%u pages=%u free=%u maxAlloc=%u", spineIndex, success, pageCount,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  if (imagesWereSuppressed) *imagesWereSuppressed = visitor.wasLowMemoryFallbackTriggered();
  if (layoutAbortedForLowMemory) {
    *layoutAbortedForLowMemory = *layoutAbortedForLowMemory || visitor.wasLowMemoryAbortTriggered();
  }

  const auto sourceCheck =
      success && visitor.wasMalformedMarkupTruncated() && previousCacheCandidate && hasCommittedCacheOnDisk()
          ? checkMalformedInputAgainstOriginalSource(*epub, localPath, parsePath, visitor)
          : MalformedSourceCheck::Matches;
  const bool malformedWithPreviousCache = sourceCheck != MalformedSourceCheck::Matches;
  if (malformedWithPreviousCache) {
    LOG_ERR("SCT", "Malformed XHTML salvage did not match the original source; preserving committed section cache");
    success = false;
    if (sourceCheck == MalformedSourceCheck::CachedHtmlMismatch && htmlCached) {
      LOG_ERR("SCT", "Removing chapter XHTML cache that failed ZIP CRC verification");
      Storage.remove(htmlPath.c_str());
      htmlCached = false;
    }
  }
  if (!htmlCached) {
    if ((success || pageCompletionFailed) && !malformedWithPreviousCache) {
      // Promote the freshly unzipped HTML to the persistent cache so future rebuilds (e.g. after a
      // settings change invalidates the layout caches) can skip zip inflation. If promotion fails,
      // drop the temp file; the section build can still continue from the already-open source.
      if (!Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
        LOG_DBG("SCT", "Failed to promote HTML cache, removing temp");
        Storage.remove(tmpHtmlPath.c_str());
      }
    } else {
      // Parse failed, or salvage was rejected to protect an existing chapter cache.
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

  if (malformedWithPreviousCache) {
    LOG_ERR("SCT", "Malformed XHTML salvage rejected; preserving committed section cache");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) cssParser->clear();
    return false;
  }

  if (!shapingFailures.recovered()) {
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) cssParser->clear();
    LOG_INF("SCT", "Shaping still unavailable; retained previous readable section");
    return false;
  }

  if (buildOptions.resolvedReferencePage && buildOptions.referenceUnitCount > 0 &&
      !buildOptions.referenceUnitsAreCharacters) {
    *buildOptions.resolvedReferencePage = EpubNavigation::resolveReferenceTargetToRenderedPage(
        buildOptions.referenceUnitOffset, buildOptions.referenceUnitCount, visitor.getVisibleTextLength(), pageIndex);
  }

  const auto& anchors = visitor.getAnchors();
  SectionPageIndexOffsets pageIndexOffsets;
  uint8_t* const indexWriteBuffer = fullBuildPageWriteBuffer_;
  const bool wrotePageIndex = writeSectionPageIndex(
      file, pageIndex,
      [&anchors](Print& output) {
        if (!serialization::tryWritePod(output, static_cast<uint16_t>(anchors.size()))) return false;
        for (const auto& [anchor, page] : anchors) {
          if (!serialization::tryWriteString(output, anchor) || !serialization::tryWritePod(output, page)) return false;
        }
        return true;
      },
      pageIndexOffsets, indexWriteBuffer, PAGE_WRITE_BUFFER_SIZE);
  if (shapingFailures.degraded) {
    initialShapingReadiness.free = shapingFailures.failureFree;
    initialShapingReadiness.largest = shapingFailures.failureLargest;
  }
  if (!wrotePageIndex || !serialization::tryWritePod(file, visitor.getReferenceTextLength()) ||
      !writeShapingStamp(file, shapingFailures, initialShapingReadiness)) {
    LOG_ERR("SCT", "Failed to write section page index");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }

  // Patch header with final pageCount and all cache lookup-table offsets.
  if (!writePublisherFontState() || !serialization::tryWritePod(file, pageCount) ||
      !serialization::tryWritePod(file, protectedImageUnits_) ||
      !serialization::tryWritePod(file, pageIndexOffsets.page) ||
      !serialization::tryWritePod(file, pageIndexOffsets.anchorMap) ||
      !serialization::tryWritePod(file, pageIndexOffsets.paragraph) ||
      !serialization::tryWritePod(file, pageIndexOffsets.listItem) ||
      !serialization::tryWritePod(file, pageIndexOffsets.visibleText) || !file.sync()) {
    LOG_ERR("SCT", "Failed to finalize section cache");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  // Explicit close() required: member variable persists beyond function scope
  if (!file.close()) {
    LOG_ERR("SCT", "Failed to close finalized temporary section cache");
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) cssParser->clear();
    return false;
  }
  if (!promoteSectionCache(*epub, tmpSectionPath, filePath)) {
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
  replacementCommitted = true;
  return true;
}

bool Section::startBuild(const ReaderRenderSpec& spec, const SectionBuildOptions buildOptions,
                         const std::function<void()>& popupFn) {
  const auto cancelBuild = [&](bool alreadyCancelled = false) {
    if (!alreadyCancelled && !buildOptions.isCancellationRequested()) return false;
    if (buildOptions.cancellationObserved) *buildOptions.cancellationObserved = true;
    return true;
  };
  if (cancelBuild()) return false;

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
  const bool focusReadingEnabled = spec.focusReadingEnabled;
  const bool guideReadingEnabled = spec.guideReadingEnabled;
  const uint8_t wordSpacing = spec.wordSpacing;
  const EpubRenderMode renderMode = spec.renderMode;
  if (build_) {
    LOG_ERR("SCT", "startBuild called while a build is already active");
    return false;
  }

  const bool previousCacheCompatible = pageCount > 0 && baseLayoutSignature_ == readerRenderSpecSignature(spec);
  const bool previousCacheCandidate = Storage.exists(filePath.c_str());
  const uint16_t previousPageCount = pageCount;
  const uint16_t previousBuiltPageCount = builtPageCount_;
  const uint32_t previousProtectedImageUnits = protectedImageUnits_;
  const bool previousBuildComplete = buildComplete_;
  const uint32_t previousBaseLayoutSignature = baseLayoutSignature_;
  const bool previousPublisherFontsEnabled = publisherFontsEnabled_;
  const uint32_t previousPublisherFontFaces = publisherFontFaces_;
  const uint32_t previousPublisherFaceSignature = publisherFaceSignature_;
  const uint32_t previousPublisherLoadedFaces = publisherLoadedFaces_;
  bool buildSetupTransferred = false;
  const auto restorePrevious = [this, previousCacheCompatible, previousPageCount, previousBuiltPageCount,
                                previousProtectedImageUnits, previousBuildComplete, previousBaseLayoutSignature,
                                previousPublisherFontsEnabled, previousPublisherFontFaces,
                                previousPublisherFaceSignature, previousPublisherLoadedFaces] {
    baseLayoutSignature_ = previousBaseLayoutSignature;
    publisherFontsEnabled_ = previousPublisherFontsEnabled;
    publisherFontFaces_ = previousPublisherFontFaces;
    publisherFaceSignature_ = previousPublisherFaceSignature;
    publisherLoadedFaces_ = previousPublisherLoadedFaces;
    if (previousCacheCompatible) {
      pageCount = previousPageCount;
      builtPageCount_ = previousBuiltPageCount;
      protectedImageUnits_ = previousProtectedImageUnits;
      buildComplete_ = previousBuildComplete;
    } else {
      pageCount = 0;
      builtPageCount_ = 0;
      protectedImageUnits_ = 0;
      buildComplete_ = false;
      partial_ = false;
      partialPageCount_ = 0;
      partialProtectedImageUnits_ = 0;
      partialBytesConsumed_ = 0;
      partialTotalBytes_ = 0;
    }
  };
  const ScopedCleanup restoreSetup{[&buildSetupTransferred, &restorePrevious] {
    if (!buildSetupTransferred) restorePrevious();
  }};

  // Reclaim rebuildable font data before CSS and layout allocate their
  // working buffers. Font objects remain registered and reload on demand.
  if (auto* fontCache = renderer.getFontCacheManager()) {
    fontCache->releaseSdFontCaches();
  }

  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto htmlDir = chapterDirectory(*epub, "/content", spineIndex);
  const auto htmlPath = htmlDir + "/" + std::to_string(spineIndex) + ".html";
  const auto tmpHtmlPath = htmlDir + "/.tmp_" + std::to_string(spineIndex) + ".html";
  const auto tmpSectionPath = binTmpPath();
  builtPageCount_ = 0;
  imageEstimateViewportHeight_ = viewportHeight;
  imageEstimateViewportWidth_ = viewportWidth;
  protectedImageUnits_ = 0;
  pageCount = partial_ ? partialPageCount_ : 0;
  buildComplete_ = false;
  lastImagesWereSuppressed_ = false;
  lastLayoutAbortedForLowMemory_ = false;

  if (Storage.exists(tmpSectionPath.c_str())) {
    Storage.remove(tmpSectionPath.c_str());
  }

  LOG_DBG("SCT",
          "Start incremental section build: spine=%d mode=%u preview=%u viewport=%ux%u image=%u focus=%u guide=%u "
          "free=%u maxAlloc=%u",
          spineIndex, static_cast<unsigned>(renderMode), buildOptions.isPreview() ? 1U : 0U, viewportWidth,
          viewportHeight, imageRendering, focusReadingEnabled, guideReadingEnabled, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());

  {
    const auto sectionsDir = chapterDirectory(*epub, "/layout", spineIndex);
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
    for (int attempt = 0; attempt < 3 && !streamed; attempt++) {
      if (cancelBuild()) {
        cleanupTempHtml();
        return false;
      }
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
      if (buildOptions.shouldCancel) {
        CancellableSectionHtmlOutput out(tmpHtml, buildOptions);
        streamed =
            epub->readItemContentsToStream(localPath, out, htmlStreamChunkSize, Epub::StreamEarlyStop::Immediate);
        if (cancelBuild(out.cancelled)) {
          tmpHtml.close();
          cleanupTempHtml();
          return false;
        }
        streamed = streamed && !out.writeFailed;
      } else {
        streamed = epub->readItemContentsToStream(localPath, tmpHtml, htmlStreamChunkSize);
      }
      const bool synced = !streamed || tmpHtml.sync();
      const bool closed = tmpHtml.close();
      streamed = streamed && synced && closed;
      if (!synced || !closed) LOG_ERR("SCT", "Failed to sync or close streamed chapter HTML");
      if (!streamed && Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
      }
    }

    if (!streamed) {
      LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
      return false;
    }
    if (previousCacheCandidate) {
      // Keep the XHTML temporary until the complete parser pass proves it was not
      // accepted only through malformed-markup salvage over a good old section.
      LOG_DBG("SCT", "Deferring XHTML cache promotion until section parse validates");
    } else if (Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
      htmlCached = true;
    } else {
      LOG_DBG("SCT", "Failed to promote HTML cache; parsing from temp");
    }
  }

  if (cancelBuild()) {
    cleanupTempHtml();
    return false;
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
  ctx->pageWriteBuffer = makeUniqueNoThrow<uint8_t[]>(PAGE_WRITE_BUFFER_SIZE);
  if (!ctx->pageWriteBuffer) LOG_ERR("SCT", "Page write buffer alloc failed; writing pages unstaged");
  ctx->replayWatermarkBytes = partial_ ? partialBytesConsumed_ : 0;
  ctx->replayWatermarkPages = partial_ ? partialPageCount_ : 0;
  ctx->reusedHtml = htmlCached;
  ctx->htmlPath = htmlPath;
  ctx->tmpHtmlPath = tmpHtmlPath;
  ctx->tmpSectionPath = tmpSectionPath;
  ctx->previousCacheCompatible = previousCacheCompatible;
  ctx->previousCacheCandidate = previousCacheCandidate;
  ctx->previousBuildComplete = previousBuildComplete;
  ctx->previousPageCount = previousPageCount;
  ctx->previousBuiltPageCount = previousBuiltPageCount;
  ctx->previousProtectedImageUnits = previousProtectedImageUnits;
  ctx->previousBaseLayoutSignature = previousBaseLayoutSignature;
  ctx->previousPublisherFontsEnabled = previousPublisherFontsEnabled;
  ctx->previousPublisherFontFaces = previousPublisherFontFaces;
  ctx->previousPublisherFaceSignature = previousPublisherFaceSignature;
  ctx->previousPublisherLoadedFaces = previousPublisherLoadedFaces;
  ctx->parsePath = htmlCached ? htmlPath : tmpHtmlPath;

  const size_t lastSlash = localPath.find_last_of('/');
  ctx->contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  ctx->imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  if (embeddedStyle) {
    ctx->cssParser = epub->getCssParser();
    if (ctx->cssParser) {
      const bool cssCacheExisted = ctx->cssParser->hasCache();
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
        if (!cssCacheExisted) {
          LOG_ERR("SCT", "CSS was requested but its cache is missing; preserving the previous section cache");
        } else {
          LOG_ERR("SCT", "Could not hydrate CSS cache; preserving the previous section cache");
        }
        ctx->cssParser->clear();
        file.close();
        Storage.remove(tmpSectionPath.c_str());
        cleanupTempHtml();
        return false;
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
      paragraphAlignment, viewportWidth, viewportHeight, hyphenationEnabled, focusReadingEnabled, guideReadingEnabled,
      wordSpacing,
      [this, ctxPtr](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex,
                     const uint32_t visibleTextOffset, const uint32_t referenceTextOffset) {
        if (ctxPtr->pageCompletionFailed) {
          return;
        }
        const auto prepareResult = ctxPtr->pageIndex.prepareAppend();
        if (prepareResult == SectionPageIndex::PrepareResult::EntryLimit) {
          LOG_ERR("SCT", "Incremental section page count exceeded cache format limit");
          ctxPtr->pageCompletionFailed = true;
          return;
        }
        if (prepareResult == SectionPageIndex::PrepareResult::AllocationFailure) {
          LOG_ERR("SCT", "Failed to allocate incremental page index storage at %u entries",
                  static_cast<unsigned>(ctxPtr->pageIndex.size()));
          ctxPtr->pageCompletionFailed = true;
          lastLayoutAbortedForLowMemory_ = true;
          return;
        }
        const uint32_t fileOffset = this->onPageComplete(std::move(page));
        if (fileOffset == 0) {
          ctxPtr->pageCompletionFailed = true;
          return;
        }
        ctxPtr->pageIndex.appendPrepared(
            {fileOffset, paragraphIndex, listItemIndex, visibleTextOffset, referenceTextOffset});
      },
      embeddedStyle, ctxPtr->contentBase, ctxPtr->imageBasePath, imageRendering, std::move(tocAnchors), popupFn,
      ctxPtr->cssParser, renderMode, buildOptions.isPreview() ? std::string(buildOptions.previewAnchor) : std::string{},
      buildOptions.previewMaxPages, true, spec.characterSpacing);
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
  ComplexShaper::setDocumentLanguage(epub->getLanguage().c_str());
  build_ = std::move(ctx);
  buildSetupTransferred = true;
  build_->parser->setRuled(spec.ruled);
  build_->parser->setPublisherFonts(spec.publisherFonts);
  build_->parser->setTrackMalformedInputCrc(previousCacheCandidate);
  build_->parser->setDeferAdvancePrewarm(buildOptions.deferAdvancePrewarm);
  build_->shapingReadiness = shapingReadiness();
  const bool parseStarted = [&] {
    const ComplexShaper::LayoutObservation observe(build_->shapingFailures);
    return build_->parser->beginParse();
  }();
  if (cancelBuild()) {
    abandonBuild();
    return false;
  }
  if (!parseStarted) {
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

bool Section::prewarmSomeMore() { return build_ && build_->parser && build_->parser->prewarmSectionAdvanceStep(); }

bool Section::buildSomeMore(const int maxPages, const bool singleParseStep) {
  if (!build_ || !build_->parser) {
    LOG_ERR("SCT", "buildSomeMore called with no active build");
    return false;
  }
  // Pace on pages laid out by THIS build, not pageCount: during a rebuild over a partial,
  // pageCount stays pinned at the partial's watermark until the build passes it, which
  // would otherwise turn one "small" chunk into a blocking rebuild of the whole watermark.
  const int startCount = builtPageCount_;
  for (;;) {
    const auto status = [&] {
      const ComplexShaper::LayoutObservation observe(build_->shapingFailures);
      return build_->parser->parseStep();
    }();
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
    if (singleParseStep || (maxPages > 0 && (builtPageCount_ - startCount) >= maxPages)) {
      build_->bytesConsumed = build_->parser->parseBytesConsumed();
      return true;
    }
  }
}

bool Section::hasHtmlCache() const {
  const std::string htmlPath =
      chapterDirectory(*epub, "/content", spineIndex) + "/" + std::to_string(spineIndex) + ".html";
  return Storage.exists(htmlPath.c_str());
}

bool Section::hasLayoutCache() const { return Storage.exists(filePath.c_str()); }

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

int Section::getTocIndexForPage(const int page) const {
  int selected = epub->getTocIndexForSpineIndex(spineIndex);
  int latestPage = -1;
  struct TocAnchor {
    size_t hash;
    int index;
  };
  // Bound scratch space to 512 bytes on C3; anchor text stays in the existing caches.
  constexpr size_t BATCH_SIZE = 64;
  auto targets = makeUniqueNoThrow<TocAnchor[]>(BATCH_SIZE);
  if (!targets) {
    LOG_ERR("SCT", "OOM: TOC anchor lookup");
    return selected;
  }
  FsFile anchorFile;
  const ScopedCleanup closeAnchors{[&anchorFile] { anchorFile.close(); }};
  uint16_t anchorCount = 0;
  std::optional<serialization::BufferedFileReader> anchorReader;
  uint32_t anchorStart = 0;
  if (openAnchorMap(anchorFile, anchorCount)) {
    anchorStart = anchorFile.position();
    // Reuse one 1 KB buffer across batches and interleaved TOC metadata reads.
    anchorReader.emplace(anchorFile, 1024);
  }
  std::string cachedAnchor;
  size_t count = 0;
  const auto considerAnchor = [&](const std::string& anchor, const uint16_t anchorPage, const bool fromDisk) {
    const size_t hash = std::hash<std::string>{}(anchor);
    for (size_t i = 0; i < count; ++i) {
      if (targets[i].hash != hash || epub->getTocItem(targets[i].index).anchor != anchor) continue;
      const int resolvedPage = fromDisk ? findAnchorDuringBuild(anchor).value_or(anchorPage) : anchorPage;
      if (resolvedPage <= page &&
          (resolvedPage > latestPage || (resolvedPage == latestPage && targets[i].index > selected))) {
        selected = targets[i].index;
        latestPage = resolvedPage;
      }
    }
  };
  const auto resolveBatch = [&]() {
    if (build_ && build_->parser) {
      for (const auto& [anchor, anchorPage] : build_->parser->getAnchors()) {
        considerAnchor(anchor, anchorPage, false);
      }
    }
    if (anchorReader && anchorReader->seek(anchorStart)) {
      for (uint16_t i = 0; i < anchorCount; ++i) {
        uint16_t anchorPage;
        uint32_t length = 0;
        if (anchorReader->read(&length, sizeof(length)) != sizeof(length) ||
            anchorReader->position() > anchorFile.size() || length > anchorFile.size() - anchorReader->position() ||
            length > 4096) {
          LOG_ERR("SCT", "Invalid TOC anchor record");
          break;
        }
        cachedAnchor.resize(length);
        if (anchorReader->read(cachedAnchor.data(), length) != length ||
            anchorReader->read(&anchorPage, sizeof(anchorPage)) != sizeof(anchorPage)) {
          LOG_ERR("SCT", "Truncated TOC anchor record");
          break;
        }
        considerAnchor(cachedAnchor, anchorPage, true);
      }
    }
    count = 0;
  };
  const int first = std::max(0, selected);
  for (int i = first; i < epub->getTocItemsCount(); ++i) {
    const auto item = epub->getTocItem(i);
    if (item.spineIndex != spineIndex) continue;
    if (item.anchor.empty()) {
      if (page >= 0 && latestPage <= 0) {
        selected = i;
        latestPage = 0;
      }
    } else {
      targets[count++] = {std::hash<std::string>{}(item.anchor), i};
      if (count == BATCH_SIZE) resolveBatch();
    }
  }
  if (count > 0) resolveBatch();
  return selected;
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
// (bytesConsumed, totalBytes) is appended after the visible-text LUT so a later open can estimate
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

  const auto& anchors = build_->parser->getAnchors();
  SectionPageIndexOffsets pageIndexOffsets;
  uint8_t* const indexWriteBuffer = build_->pageWriteBuffer.get();
  const bool wrotePageIndex = writeSectionPageIndex(
      file, build_->pageIndex,
      [this, asPartial, &anchors](Print& output) {
        uint16_t anchorCount = 0;
        for (const auto& [anchor, page] : anchors) {
          if (!asPartial || page < builtPageCount_) anchorCount++;
        }
        if (!serialization::tryWritePod(output, anchorCount)) return false;
        for (const auto& [anchor, page] : anchors) {
          if (asPartial && page >= builtPageCount_) continue;
          if (!serialization::tryWriteString(output, anchor) || !serialization::tryWritePod(output, page)) return false;
        }
        return true;
      },
      pageIndexOffsets, indexWriteBuffer, PAGE_WRITE_BUFFER_SIZE);
  if (!wrotePageIndex ||
      !serialization::tryWritePod(file, asPartial ? uint32_t{0} : build_->parser->getReferenceTextLength())) {
    LOG_ERR("SCT", "Failed to write section page index");
    return failCommit();
  }

  if (asPartial) {
    // Watermark trailer follows the page-start visible-text LUT.
    if (!serialization::tryWritePod(file, bytesConsumed) || !serialization::tryWritePod(file, totalBytes)) {
      return failCommit();
    }
  }

  auto readiness = build_->shapingReadiness;
  if (build_->shapingFailures.degraded) {
    readiness.free = build_->shapingFailures.failureFree;
    readiness.largest = build_->shapingFailures.failureLargest;
  }
  if (!writeShapingStamp(file, build_->shapingFailures, readiness)) return failCommit();

  // Patch header with the built page count and section offsets...
  if (!writePublisherFontState() || !serialization::tryWritePod(file, builtPageCount_) ||
      !serialization::tryWritePod(file, protectedImageUnits_) ||
      !serialization::tryWritePod(file, pageIndexOffsets.page) ||
      !serialization::tryWritePod(file, pageIndexOffsets.anchorMap) ||
      !serialization::tryWritePod(file, pageIndexOffsets.paragraph) ||
      !serialization::tryWritePod(file, pageIndexOffsets.listItem) ||
      !serialization::tryWritePod(file, pageIndexOffsets.visibleText) || !file.seek(sizeof(SECTION_CACHE_MAGIC)) ||
      !serialization::tryWritePod(file, version) || !file.sync()) {
    LOG_ERR("SCT", "Failed to commit section cache");
    return failCommit();
  }
  // Explicit close() required: member variable persists beyond function scope
  if (!file.close()) {
    LOG_ERR("SCT", "Failed to close incremental temporary section cache");
    return failCommit();
  }

  // Keep the readable cache as a backup until the completed replacement is in
  // place. A reboot after the backup rename recovers it in the constructor.
  if (!promoteSectionCache(*epub, build_->tmpSectionPath, filePath)) {
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

  [[maybe_unused]] const uint32_t consumedAtFinalize = static_cast<uint32_t>(build_->parser->parseBytesConsumed());
  const bool success = [&] {
    const ComplexShaper::LayoutObservation observe(build_->shapingFailures);
    return build_->parser->finishParse();
  }();
  lastImagesWereSuppressed_ = lastImagesWereSuppressed_ || build_->parser->wasLowMemoryFallbackTriggered();
  lastLayoutAbortedForLowMemory_ = lastLayoutAbortedForLowMemory_ || build_->parser->wasLowMemoryAbortTriggered();
  if (!success || build_->pageCompletionFailed) {
    LOG_ERR("SCT", "Failed to finalize parser output");
    abandonBuild();
    return false;
  }

  const auto sourceCheck =
      build_->parser->wasMalformedMarkupTruncated() && build_->previousCacheCandidate && hasCommittedCacheOnDisk()
          ? checkMalformedInputAgainstOriginalSource(*epub, epub->getSpineItem(spineIndex).href, build_->parsePath,
                                                     *build_->parser)
          : MalformedSourceCheck::Matches;
  if (sourceCheck != MalformedSourceCheck::Matches) {
    LOG_ERR("SCT", "Malformed XHTML salvage did not match the original source; preserving committed section cache");
    if (sourceCheck == MalformedSourceCheck::CachedHtmlMismatch && build_->reusedHtml) {
      LOG_ERR("SCT", "Removing chapter XHTML cache that failed ZIP CRC verification");
      Storage.remove(build_->htmlPath.c_str());
    }
    abandonBuild();
    return false;
  }

  if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str()) &&
      !Storage.rename(build_->tmpHtmlPath.c_str(), build_->htmlPath.c_str())) {
    LOG_DBG("SCT", "Failed to promote validated streamed HTML cache");
    Storage.remove(build_->tmpHtmlPath.c_str());
  }

  if (build_->replayWatermarkBytes > 0) {
    LOG_DBG("SCT", "Partial rebuild replay: bytes=%u/%u pages=%u/%u",
            std::min(consumedAtFinalize, build_->replayWatermarkBytes), build_->replayWatermarkBytes,
            std::min(builtPageCount_, build_->replayWatermarkPages), build_->replayWatermarkPages);
  }

  const bool committed = commitBuildFile(SECTION_FILE_VERSION, 0, 0);
  if (build_->cssParser) build_->cssParser->clear();
  if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  if (!committed) restoreFailedBuildState(*build_);
  build_.reset();
  if (!committed) {
    // The previous cache was retained/restored by promoteSectionCache(). Keep
    // its in-memory watermark too so this Section remains readable.
    return false;
  }
  buildComplete_ = true;
  partial_ = false;
  partialPageCount_ = 0;
  partialProtectedImageUnits_ = 0;
  pageCount = builtPageCount_;
  return true;
}

void Section::suspendBuild(const bool replaceIncompatibleLayout) {
  if (!build_) return;

  if (build_->parser->wasMalformedMarkupTruncated()) build_->parser->releaseInputFile();
  const auto sourceCheck =
      build_->parser->wasMalformedMarkupTruncated() && build_->previousCacheCandidate && hasCommittedCacheOnDisk()
          ? checkMalformedInputAgainstOriginalSource(*epub, epub->getSpineItem(spineIndex).href, build_->parsePath,
                                                     *build_->parser)
          : MalformedSourceCheck::Matches;
  if (sourceCheck != MalformedSourceCheck::Matches) {
    LOG_ERR("SCT", "Malformed XHTML salvage did not match the original source during suspend");
    if (sourceCheck == MalformedSourceCheck::CachedHtmlMismatch && build_->reusedHtml) {
      LOG_ERR("SCT", "Removing chapter XHTML cache that failed ZIP CRC verification");
      Storage.remove(build_->htmlPath.c_str());
    }
    abandonBuild();
    return;
  }

  if (build_->replayWatermarkBytes > 0) {
    LOG_DBG("SCT", "Partial rebuild suspended: bytes=%u/%u pages=%u/%u zeroProgress=%u",
            std::min(static_cast<uint32_t>(build_->parser->parseBytesConsumed()), build_->replayWatermarkBytes),
            build_->replayWatermarkBytes, std::min(builtPageCount_, build_->replayWatermarkPages),
            build_->replayWatermarkPages, builtPageCount_ <= build_->replayWatermarkPages ? 1U : 0U);
  }

  // Only worth persisting if this build produced pages a pre-existing partial doesn't
  // already cover; otherwise keep the older (bigger) partial and just drop the tmp.
  const bool mayReplaceCache =
      !build_->previousCacheCandidate || (replaceIncompatibleLayout && !build_->previousCacheCompatible);
  const bool worthKeeping = !build_->pageCompletionFailed && builtPageCount_ > 0 &&
                            (partial_ ? builtPageCount_ > partialPageCount_ : mayReplaceCache);

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
  if (!committed) restoreFailedBuildState(*build_);
  build_.reset();
}

void Section::restoreFailedBuildState(const BuildContext& context) {
  baseLayoutSignature_ = context.previousBaseLayoutSignature;
  publisherFontsEnabled_ = context.previousPublisherFontsEnabled;
  publisherFontFaces_ = context.previousPublisherFontFaces;
  publisherFaceSignature_ = context.previousPublisherFaceSignature;
  publisherLoadedFaces_ = context.previousPublisherLoadedFaces;
  if (context.previousCacheCompatible) {
    pageCount = context.previousPageCount;
    builtPageCount_ = context.previousBuiltPageCount;
    protectedImageUnits_ = context.previousProtectedImageUnits;
    buildComplete_ = context.previousBuildComplete;
    return;
  }

  pageCount = 0;
  builtPageCount_ = 0;
  protectedImageUnits_ = 0;
  buildComplete_ = false;
  partial_ = false;
  partialPageCount_ = 0;
  partialProtectedImageUnits_ = 0;
  partialBytesConsumed_ = 0;
  partialTotalBytes_ = 0;
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
  if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  restoreFailedBuildState(*build_);
  build_.reset();
}

std::unique_ptr<Page> Section::loadPageDuringBuild(const int page) {
  if (!build_ || page < 0 || static_cast<size_t>(page) >= build_->pageIndex.size()) {
    return nullptr;
  }
  const uint32_t pos = build_->pageIndex[static_cast<size_t>(page)].fileOffset;
  if (pos == 0) {
    return nullptr;
  }

  if (!file) {
    HalFile tmp;
    if (!Storage.openFileForRead("SCT", build_->tmpSectionPath, tmp) || !tmp.seek(pos)) {
      build_->pageCompletionFailed = true;
      tmp.close();
      return nullptr;
    }
    auto pageData = Page::deserialize(tmp);
    const bool closed = tmp.close();
    if (!pageData || !closed) {
      build_->pageCompletionFailed = true;
      return nullptr;
    }
    return pageData;
  }

  const uint32_t writePos = file.position();
  if (!file.seek(pos)) {
    build_->pageCompletionFailed = true;
    return nullptr;
  }
  auto pageData = Page::deserialize(file);
  if (!file.seek(writePos)) {
    LOG_ERR("SCT", "Failed to restore section append position after page read");
    build_->pageCompletionFailed = true;
    return nullptr;
  }
  if (!pageData) build_->pageCompletionFailed = true;
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
  const ScopedCleanup closeFile{[&f] { f.close(); }};

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
  if (build_ && static_cast<size_t>(page) < build_->pageIndex.size()) {
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

bool Section::openAnchorMap(FsFile& f, uint16_t& count) const {
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return false;
  }

  const uint32_t fileSize = f.size();
  uint32_t magic = 0;
  uint8_t version = 0;
  if (!serialization::tryReadPod(f, magic) || magic != SECTION_CACHE_MAGIC || !serialization::tryReadPod(f, version) ||
      (version != SECTION_FILE_VERSION && version != SECTION_FILE_PARTIAL_VERSION))
    return false;
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 4)) {
    return false;
  }
  uint32_t anchorMapOffset;
  if (!serialization::tryReadPod(f, anchorMapOffset)) {
    return false;
  }
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return false;
  }

  if (!f.seek(anchorMapOffset)) {
    return false;
  }
  if (!serialization::tryReadPod(f, count)) {
    return false;
  }
  return true;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  FsFile f;
  const ScopedCleanup closeFile{[&f] { f.close(); }};
  uint16_t count = 0;
  if (!openAnchorMap(f, count)) return std::nullopt;
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

  return findSectionPageAtOrAfter(f, paragraphLutOffset + sizeof(count), count, pIndex);
}

std::optional<uint16_t> Section::findParagraphDuringBuild(const uint16_t pIndex) const {
  if (build_) {
    size_t first = 0;
    size_t last = build_->pageIndex.size();
    while (first < last) {
      const size_t middle = first + (last - first) / 2;
      if (build_->pageIndex[middle].paragraphIndex < pIndex) {
        first = middle + 1;
      } else {
        last = middle;
      }
    }
    if (first < build_->pageIndex.size()) return static_cast<uint16_t>(first);
  }
  return std::nullopt;
}

std::optional<Section::ParagraphPosition> Section::getParagraphPositionForPage(const uint16_t page) const {
  if (page >= pageCount) return std::nullopt;
  FsFile f;
  bool attemptedOpen = false;
  bool readable = false;
  uint32_t lutOffset = 0;
  uint16_t count = 0;
  constexpr uint16_t CHUNK_ENTRIES = 64;
  uint16_t chunk[CHUNK_ENTRIES];  // 128 bytes; bounded independently of chapter length.
  uint16_t chunkStart = 0;
  uint16_t chunkCount = 0;
  const auto readParagraph = [&](const uint16_t at) -> std::optional<uint16_t> {
    // Partial extension builds may only have rebuilt part of the readable cache.
    // Preserve the existing per-page preference for the active build's index.
    if (build_ && at < build_->pageIndex.size()) return build_->pageIndex[at].paragraphIndex;
    if (!attemptedOpen) {
      attemptedOpen = true;
      if (Storage.openFileForRead("SCT", filePath, f)) {
        const uint32_t size = f.size();
        readable = f.seek(HEADER_SIZE - sizeof(uint32_t) * 3) && serialization::tryReadPod(f, lutOffset) &&
                   lutOffset > 0 && lutOffset < size && f.seek(lutOffset) && serialization::tryReadPod(f, count) &&
                   sizeof(count) + static_cast<uint32_t>(count) * sizeof(uint16_t) <= size - lutOffset;
        lutOffset += sizeof(count);
        if (!readable) LOG_ERR("SCT", "Invalid paragraph lookup table");
      }
    }
    if (!readable || at >= count) return std::nullopt;
    if (at < chunkStart || at >= static_cast<uint32_t>(chunkStart) + chunkCount) {
      chunkStart = (at / CHUNK_ENTRIES) * CHUNK_ENTRIES;
      chunkCount = std::min<uint16_t>(CHUNK_ENTRIES, count - chunkStart);
      const size_t bytes = chunkCount * sizeof(uint16_t);
      if (!f.seek(lutOffset + static_cast<uint32_t>(chunkStart) * sizeof(uint16_t)) ||
          f.read(chunk, bytes) != static_cast<int>(bytes)) {
        LOG_ERR("SCT", "Failed to read paragraph lookup chunk");
        readable = false;
        return std::nullopt;
      }
    }
    return chunk[at - chunkStart];
  };
  const auto index = readParagraph(page);
  if (!index) {
    if (f) f.close();
    return std::nullopt;
  }
  uint16_t start = page;
  while (start > 0) {
    const auto previous = readParagraph(start - 1);
    if (!previous || *previous != *index) break;
    --start;
  }
  uint16_t end = page + 1;
  while (end < pageCount) {
    const auto next = readParagraph(end);
    if (!next || *next != *index) break;
    ++end;
  }
  if (f) f.close();
  return ParagraphPosition{*index, static_cast<uint16_t>(page - start), static_cast<uint16_t>(end - start)};
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  if (build_ && page < build_->pageIndex.size()) {
    return build_->pageIndex[page].paragraphIndex;
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
  if (build_ && page < build_->pageIndex.size()) {
    return build_->pageIndex[page].listItemIndex;
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

  return findSectionPageAtOrAfter(f, liLutOffset, count, liIndex);
}

std::optional<uint32_t> Section::getVisibleTextOffsetForPage(const uint16_t page) const {
  return getTextOffsetForPage(page, false, nullptr);
}

std::optional<uint32_t> Section::getReferenceTextOffsetForPage(const uint16_t page, uint32_t& textLength) const {
  textLength = 0;
  return getTextOffsetForPage(page, true, &textLength);
}

std::optional<uint32_t> Section::getTextOffsetForPage(const uint16_t page, const bool reference,
                                                      uint32_t* textLength) const {
  if (build_ && page < build_->pageIndex.size()) {
    return reference ? build_->pageIndex[page].referenceTextOffset : build_->pageIndex[page].visibleTextOffset;
  }
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) return std::nullopt;
  const auto read = [&]() -> std::optional<uint32_t> {
    const uint32_t fileSize = f.size();
    if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 6 - sizeof(uint16_t))) return std::nullopt;
    uint16_t count = 0;
    if (!serialization::tryReadPod(f, count) || page >= count) return std::nullopt;
    if (!f.seek(HEADER_SIZE - sizeof(uint32_t))) return std::nullopt;
    uint32_t lutOffset = 0;
    if (!serialization::tryReadPod(f, lutOffset) || lutOffset < HEADER_SIZE) return std::nullopt;
    const uint64_t start = static_cast<uint64_t>(lutOffset) + (reference ? count * sizeof(uint32_t) : 0U);
    const uint64_t position = start + static_cast<uint32_t>(page) * sizeof(uint32_t);
    if (position + sizeof(uint32_t) > fileSize || !f.seek(position)) return std::nullopt;
    uint32_t offset = 0;
    if (!serialization::tryReadPod(f, offset)) return std::nullopt;
    if (reference && textLength && !partial_) {
      const uint64_t end = start + count * sizeof(uint32_t);
      if (end + sizeof(uint32_t) > fileSize || !f.seek(end) || !serialization::tryReadPod(f, *textLength))
        return std::nullopt;
    }
    return offset;
  };
  const auto result = read();
  f.close();
  return result;
}

std::optional<uint16_t> Section::getPageForVisibleTextOffset(const uint32_t offset,
                                                             const bool preferFirstAtOffset) const {
  if (build_ && !build_->pageIndex.empty()) {
    uint16_t result = 0;
    for (size_t i = 0; i < build_->pageIndex.size(); i++) {
      const uint32_t start = build_->pageIndex[i].visibleTextOffset;
      if (start > offset) break;
      result = static_cast<uint16_t>(i);
      if (preferFirstAtOffset && start == offset) break;
    }
    const uint32_t last = build_->pageIndex[build_->pageIndex.size() - 1].visibleTextOffset;
    // The start of the next page already bounds the last readable page.
    // Waiting for that next page to finish delays incremental relayout by a page.
    if (offset <= last || build_->parser->unfinishedPageStartsAfter(offset)) return result;
    // An extension build starts from page zero while its previously committed
    // partial remains readable.  Its shorter live prefix must not hide a page
    // that the committed cache already knows how to resolve.
  }

  // A from-scratch build (no partial ever committed for this cache key) has nothing on
  // disk yet: an offset beyond the live prefix above just hasn't been laid out, and
  // openFileForRead() would fail every call until the build catches up or finishes.
  if (build_ && !partial_) {
    return std::nullopt;
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
  if (!f.seek(lutOffset)) return std::nullopt;
  uint16_t result = 0;
  uint32_t last = 0;
  // Read a bounded batch instead of seeking and reading four bytes per page.
  // Keep the original first-match order, including repeated offsets on image/table pages.
  uint32_t starts[32];  // 128 bytes; no heap allocation on the restore/reflow path.
  for (uint32_t base = 0; base < count; base += 32) {
    const uint32_t batch = std::min<uint32_t>(32, count - base);
    const size_t bytes = batch * sizeof(uint32_t);
    if (f.read(starts, bytes) != static_cast<int>(bytes)) {
      f.close();
      return std::nullopt;
    }
    for (uint32_t i = 0; i < batch; ++i) {
      last = starts[i];
      if (last > offset) {
        f.close();
        return result;
      }
      result = static_cast<uint16_t>(base + i);
      if (preferFirstAtOffset && last == offset) {
        f.close();
        return result;
      }
    }
  }
  f.close();
  if (version == SECTION_FILE_PARTIAL_VERSION && offset > last) return std::nullopt;
  return result;
}
