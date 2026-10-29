#include "Epub.h"

#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <GifImageLayout.h>
#include <GifLimitedWriter.h>
#include <GifToBmpConverter.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <PngToBmpConverter.h>
#include <Utf8.h>
#include <ZipFile.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string_view>
#include <utility>

#include "Epub/ReferencePageNavigation.h"
#include "Epub/image/OptimizerCachePublish.h"
#include "Epub/image/OptimizerIndex.h"
#include "Epub/image/PxcHeaderSink.h"
#include "Epub/parsers/ContainerParser.h"
#include "Epub/parsers/ContentOpfParser.h"
#include "Epub/parsers/TocNavParser.h"
#include "Epub/parsers/TocNcxParser.h"

namespace {
constexpr int kDefaultThumbHeight = 180;
constexpr char kXLocationsPath[] = "META-INF/x-locations.json";
constexpr char kXLocationsFormat[] = "x-locations";
constexpr char kLegacyXLocationsFormat[] = "crossink-locations";
constexpr size_t kXLocationsMaxBytes = 64 * 1024;
constexpr uint32_t kDefaultReferenceCharactersPerPage = 1500;
constexpr char kOptimizerManifestPath[] = "META-INF/crossink/optimizer-v1.json";
constexpr char kOptimizerManifestFormat[] = "crossink-optimizer";
constexpr size_t kOptimizerManifestMaxBytes = 64 * 1024;
constexpr size_t kOptimizerMaxPxcBytes = 4 + 128 * 1024;
constexpr uint16_t kOptimizerMaxPxcDimension = 1024;
constexpr size_t kOptimizerMaxPxcRowBytes = (kOptimizerMaxPxcDimension + 3) / 4;
// Legacy sidecar inflation borrows framebuffer scratch during page preflight.
// Keep the remaining ZIP input/output allocations bounded.
constexpr size_t kOptimizerPxcExtractionChunkSize = 256;
// The parser is single-threaded. Static row buffers avoid a cold-path heap
// allocation and cap resize working memory at 512 bytes.
uint8_t optimizerSourceRow[kOptimizerMaxPxcRowBytes];
uint8_t optimizerTargetRow[kOptimizerMaxPxcRowBytes];

bool isSupportedLocationsFormat(const char* format) {
  return std::strcmp(format, kXLocationsFormat) == 0 || std::strcmp(format, kLegacyXLocationsFormat) == 0;
}

void buildXLocationsJsonFilter(JsonDocument& filter) {
  JsonObject root = filter.to<JsonObject>();
  root["format"] = true;
  root["version"] = true;
  root["totalLocations"] = true;
  root["totalWords"] = true;
  root["totalCharacters"] = true;
  root["wordsPerReferencePage"] = true;
  root["charactersPerReferencePage"] = true;
  root["totalReferencePages"] = true;

  JsonArray spine = root["spine"].to<JsonArray>();
  JsonObject spineEntry = spine.add<JsonObject>();
  spineEntry["index"] = true;
  spineEntry["startLocation"] = true;
  spineEntry["endLocation"] = true;
  spineEntry["wordStart"] = true;
  spineEntry["wordCount"] = true;
  spineEntry["characterStart"] = true;
  spineEntry["characterCount"] = true;
  spineEntry["chapterGroup"] = true;

  JsonArray chapterGroups = root["chapterGroups"].to<JsonArray>();
  JsonObject chapterGroup = chapterGroups.add<JsonObject>();
  chapterGroup["index"] = true;
  chapterGroup["firstSpineIndex"] = true;
  chapterGroup["lastSpineIndex"] = true;
  chapterGroup["startSpineIndex"] = true;
  chapterGroup["endSpineIndex"] = true;

  JsonObject sourceMap = root["sourceSpineMap"].to<JsonObject>();
  sourceMap["version"] = true;
  sourceMap["spineCount"] = true;
  JsonArray sourceSpine = sourceMap["spine"].to<JsonArray>();
  JsonObject sourceEntry = sourceSpine.add<JsonObject>();
  sourceEntry["index"] = true;
  sourceEntry["sourceSpineIndex"] = true;
  sourceEntry["containerDepth"] = true;
  JsonArray ranges = sourceEntry["childRanges"].to<JsonArray>();
  JsonObject range = ranges.add<JsonObject>();
  range["name"] = true;
  range["offset"] = true;
  range["count"] = true;
}

void buildOptimizerManifestJsonFilter(JsonDocument& filter) {
  JsonObject root = filter.to<JsonObject>();
  root["format"] = true;
  root["version"] = true;
  JsonArray images = root["images"].to<JsonArray>();
  JsonObject image = images.add<JsonObject>();
  image["href"] = true;
  image["pxc"] = true;
  image["width"] = true;
  image["height"] = true;
  image["pxcFormat"] = true;
  image["pxcBytes"] = true;
  image["pixelCrc32"] = true;
}

bool isSafeOptimizerPxcPath(const std::string_view path) {
  static constexpr char prefix[] = "META-INF/crossink/pxc/";
  return path.rfind(prefix, 0) == 0 && path.find("..") == std::string::npos;
}

bool isSafeOptimizerImagePath(const std::string_view path) {
  return !path.empty() && path.front() != '/' && path.find("..") == std::string::npos;
}

size_t pxcByteCount(const uint16_t width, const uint16_t height) {
  return 4U + static_cast<size_t>((width + 3) / 4) * static_cast<size_t>(height);
}

bool readOptimizerImageMetadata(JsonObjectConst image, const size_t maxHrefLength, const size_t maxPxcHrefLength,
                                const char*& href, const char*& pxcHref, uint16_t& width, uint16_t& height) {
  href = image["href"] | "";
  pxcHref = image["pxc"] | "";
  const int rawWidth = image["width"] | 0;
  const int rawHeight = image["height"] | 0;
  const size_t hrefLength = std::strlen(href);
  const size_t pxcHrefLength = std::strlen(pxcHref);
  if (!isSafeOptimizerImagePath(href) || !isSafeOptimizerPxcPath(pxcHref) || rawWidth <= 0 || rawHeight <= 0 ||
      rawWidth > kOptimizerMaxPxcDimension || rawHeight > kOptimizerMaxPxcDimension ||
      pxcByteCount(static_cast<uint16_t>(rawWidth), static_cast<uint16_t>(rawHeight)) > kOptimizerMaxPxcBytes ||
      hrefLength > maxHrefLength || pxcHrefLength > maxPxcHrefLength) {
    return false;
  }
  width = static_cast<uint16_t>(rawWidth);
  height = static_cast<uint16_t>(rawHeight);
  return true;
}

bool readPxcHeader(FsFile& file, uint16_t& width, uint16_t& height) {
  return file.read(&width, sizeof(width)) == sizeof(width) && file.read(&height, sizeof(height)) == sizeof(height);
}

bool rescalePxcFile(const std::string& sourcePath, const std::string& destinationPath, const int expectedWidth,
                    const int expectedHeight) {
  FsFile source;
  if (!Storage.openFileForRead("EBP", sourcePath, source)) return false;
  uint16_t sourceWidth = 0;
  uint16_t sourceHeight = 0;
  if (!readPxcHeader(source, sourceWidth, sourceHeight) || sourceWidth == 0 || sourceHeight == 0 ||
      sourceWidth > kOptimizerMaxPxcDimension || sourceHeight > kOptimizerMaxPxcDimension ||
      source.size() != pxcByteCount(sourceWidth, sourceHeight)) {
    LOG_ERR("EBP", "Invalid optimizer PXC resize source: %s", sourcePath.c_str());
    source.close();
    return false;
  }

  const size_t sourceRowBytes = (sourceWidth + 3) / 4;
  const size_t targetRowBytes = (expectedWidth + 3) / 4;
  if (sourceRowBytes > sizeof(optimizerSourceRow) || targetRowBytes > sizeof(optimizerTargetRow)) {
    source.close();
    return false;
  }

  const std::string tempDestination = destinationPath + ".optimizer.tmp";
  Storage.remove(tempDestination.c_str());
  FsFile destination;
  if (!Storage.openFileForWrite("EBP", tempDestination, destination)) {
    source.close();
    return false;
  }
  const uint16_t targetWidth = static_cast<uint16_t>(expectedWidth);
  const uint16_t targetHeight = static_cast<uint16_t>(expectedHeight);
  bool ok = destination.write(&targetWidth, sizeof(targetWidth)) == sizeof(targetWidth) &&
            destination.write(&targetHeight, sizeof(targetHeight)) == sizeof(targetHeight);
  int loadedSourceRow = -1;
  for (int targetY = 0; ok && targetY < expectedHeight; targetY++) {
    const int wantedSourceRow = static_cast<int>((static_cast<int64_t>(targetY) * sourceHeight) / expectedHeight);
    while (ok && loadedSourceRow < wantedSourceRow) {
      ok = source.read(optimizerSourceRow, sourceRowBytes) == static_cast<int>(sourceRowBytes);
      loadedSourceRow++;
    }
    if (!ok) break;
    memset(optimizerTargetRow, 0, targetRowBytes);
    for (int targetX = 0; targetX < expectedWidth; targetX++) {
      const int sourceX = static_cast<int>((static_cast<int64_t>(targetX) * sourceWidth) / expectedWidth);
      const uint8_t value = (optimizerSourceRow[sourceX >> 2] >> (6 - ((sourceX & 3) * 2))) & 0x03;
      optimizerTargetRow[targetX >> 2] |= static_cast<uint8_t>(value << (6 - ((targetX & 3) * 2)));
    }
    ok = destination.write(optimizerTargetRow, targetRowBytes) == targetRowBytes;
  }
  ok = source.close() && ok;
  const std::string backup = destinationPath + ".optimizer.previous";
  ok = OptimizerFormat::finishCache(destination, Storage, ok, tempDestination.c_str(), destinationPath.c_str(),
                                    backup.c_str());
  if (!ok) {
    LOG_ERR("EBP", "Failed to publish resized optimizer image cache");
    Storage.remove(tempDestination.c_str());
  }
  return ok;
}

float clampUnit(const float value) {
  if (value <= 0.0f) {
    return 0.0f;
  }
  if (value >= 1.0f) {
    return 1.0f;
  }
  return value;
}

int32_t readLe32(const uint8_t* data) {
  return static_cast<int32_t>(static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                              (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24));
}

void normalizeThumbDimensions(int& width, int& height) {
  if (height <= 0) {
    height = kDefaultThumbHeight;
  }
  if (width <= 0) {
    width = static_cast<int>((static_cast<int64_t>(height) * 2 + 1) / 3);
  }
}

std::unique_ptr<BookMetadataCache> makeBookMetadataCacheNoThrow(const std::string& cachePath,
                                                                const bool cacheCumulativeSpineSizes) {
  auto cache = makeUniqueNoThrow<BookMetadataCache>(cachePath, cacheCumulativeSpineSizes);
  if (!cache) {
    LOG_ERR("EBP", "OOM: BookMetadataCache (%u free, %u max alloc)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
  return cache;
}

std::unique_ptr<CssParser> makeCssParserNoThrow(const std::string& cachePath) {
  auto parser = makeUniqueNoThrow<CssParser>(cachePath);
  if (!parser) {
    LOG_ERR("EBP", "OOM: CssParser (%u free, %u max alloc)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
  return parser;
}

bool cachedBmpMatchesDimensions(const std::string& path, const int width, const int height,
                                const bool allowContainedDimensions = false) {
  if (!Storage.exists(path.c_str())) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("EBP", path, file)) {
    return false;
  }

  uint8_t header[26] = {};
  const bool hasHeader = file.size() >= sizeof(header) && file.read(header, sizeof(header)) == sizeof(header);
  file.close();
  const bool isBmp = hasHeader && header[0] == 'B' && header[1] == 'M';
  const int32_t bmpWidth = isBmp ? readLe32(header + 18) : 0;
  const int32_t bmpHeight = isBmp ? readLe32(header + 22) : 0;
  const int32_t absHeight = bmpHeight < 0 ? -bmpHeight : bmpHeight;
  const bool exactMatch = isBmp && bmpWidth == width && absHeight == height;
  const bool containedMatch = allowContainedDimensions && isBmp && bmpWidth > 0 && absHeight > 0 && bmpWidth <= width &&
                              absHeight <= height && (bmpWidth == width || absHeight == height);
  const bool matches = exactMatch || containedMatch;
  if (!matches) {
    LOG_DBG("EBP", "Removing stale thumbnail dimensions: %s (%dx%d expected %dx%d)", path.c_str(), bmpWidth, absHeight,
            width, height);
    Storage.remove(path.c_str());
  }
  return matches;
}

void releaseReaderSdFontCachesBeforeCoverDecode(const GfxRenderer* renderer, const int readerFontId,
                                                const char* reason) {
  if (!renderer) return;
  if (readerFontId <= 0) return;
  if (!renderer->isSdCardFont(readerFontId)) return;

  const auto before = MemoryBudget::snapshot();
  if (!MemoryBudget::shouldReleaseSdFontCachesForEpubInlineImage(before)) return;

  if (!renderer->releaseSdCardFontForLowMemory(readerFontId)) return;

  const auto after = MemoryBudget::snapshot();
  LOG_DBG("EBP", "Released SD font caches before %s: free=%u->%u maxAlloc=%u->%u", reason, before.freeHeap,
          after.freeHeap, before.maxAllocHeap, after.maxAllocHeap);
}

std::string getThumbBmpPathForDimensions(const std::string& cachePath, int width, int height) {
  return cachePath + "/thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
}

std::string getAdaptiveThumbBmpPathForDimensions(const std::string& cachePath, int width, int height) {
  return cachePath + "/thumb_" + std::to_string(width) + "x" + std::to_string(height) + "_fit.bmp";
}

std::string legacyCachePathForFilePath(const std::string& filepath, const std::string& cacheDir) {
  return cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(filepath));
}

class CoverImageRefScanner final : public Print {
 public:
  std::string imageRef;

  size_t write(uint8_t data) override { return write(&data, 1); }

  size_t write(const uint8_t* buffer, size_t size) override {
    for (size_t i = 0; i < size && imageRef.empty(); ++i) {
      consume(static_cast<char>(buffer[i]));
    }
    return size;
  }

 private:
  static constexpr size_t kMaxImageRefLen = 512;
  static constexpr const char* kXlinkPattern = "xlink:href=\"";
  static constexpr const char* kSrcPattern = "src=\"";

  size_t xlinkMatched = 0;
  size_t srcMatched = 0;
  bool collecting = false;
  std::string candidate;

  static bool isSupportedImageRef(const std::string& ref) {
    const auto view = std::string_view{ref};
    return FsHelpers::hasPngExtension(view) || FsHelpers::hasJpgExtension(view) || FsHelpers::hasGifExtension(view);
  }

  void consume(const char c) {
    if (collecting) {
      if (c == '"') {
        if (isSupportedImageRef(candidate)) {
          imageRef = candidate;
        }
        candidate.clear();
        collecting = false;
        xlinkMatched = 0;
        srcMatched = 0;
        return;
      }
      if (candidate.size() < kMaxImageRefLen) {
        candidate.push_back(c);
      } else {
        candidate.clear();
        collecting = false;
      }
      return;
    }

    const auto advance = [c](const char* pattern, size_t matched) {
      if (c == pattern[matched]) {
        return matched + 1;
      }
      return c == pattern[0] ? size_t{1} : size_t{0};
    };

    xlinkMatched = advance(kXlinkPattern, xlinkMatched);
    srcMatched = advance(kSrcPattern, srcMatched);

    if (kXlinkPattern[xlinkMatched] == '\0' || kSrcPattern[srcMatched] == '\0') {
      collecting = true;
      candidate.clear();
    }
  }
};

class ContentsDocumentScanner final : public Print {
 public:
  bool isContentsDocument() const { return contentsDocument; }

  size_t write(const uint8_t data) override { return write(&data, 1); }

  size_t write(const uint8_t* buffer, const size_t size) override {
    for (size_t index = 0; index < size; index++) {
      if (scannedBytes >= kMaxScanBytes || contentsDocument) return index;
      consume(static_cast<char>(buffer[index]));
      scannedBytes++;
    }
    return size;
  }

 private:
  static constexpr size_t kMaxScanBytes = 8 * 1024;
  static constexpr size_t kMaxTagLength = 192;
  static constexpr size_t kMaxLabelLength = 64;

  char tag[kMaxTagLength] = {};
  char label[kMaxLabelLength] = {};
  size_t tagLength = 0;
  size_t labelLength = 0;
  size_t scannedBytes = 0;
  bool insideTag = false;
  bool collectingLabel = false;
  bool contentsDocument = false;

  static bool hasTocTypeAttribute(const char* tag) {
    for (const char* attr = tag; (attr = std::strstr(attr, "type")) != nullptr; attr += 4) {
      const char preceding = attr == tag ? ' ' : attr[-1];
      if (preceding != ' ' && preceding != '\t' && preceding != '\r' && preceding != '\n' && preceding != ':') {
        continue;
      }
      const char* value = attr + 4;
      while (std::isspace(static_cast<unsigned char>(*value))) value++;
      if (*value++ != '=') continue;
      while (std::isspace(static_cast<unsigned char>(*value))) value++;
      const char quote = *value == '\'' || *value == '\"' ? *value++ : '\0';
      if (std::strncmp(value, "toc", 3) != 0) continue;
      const char terminator = value[3];
      if ((quote && terminator == quote) ||
          (!quote && (terminator == '\0' || std::isspace(static_cast<unsigned char>(terminator))))) {
        return true;
      }
    }
    return false;
  }

  void clearLabel() {
    labelLength = 0;
    label[0] = '\0';
  }

  void appendLabel(const char c) {
    if (labelLength + 1 >= kMaxLabelLength) return;
    const unsigned char byte = static_cast<unsigned char>(c);
    if (std::isspace(byte)) {
      if (labelLength == 0 || label[labelLength - 1] == ' ') return;
      label[labelLength++] = ' ';
    } else {
      label[labelLength++] = static_cast<char>(std::tolower(byte));
    }
    label[labelLength] = '\0';
  }

  void finishTag() {
    tag[tagLength] = '\0';
    const char* name = tag;
    while (*name == ' ' || *name == '\t' || *name == '\r' || *name == '\n') name++;
    const bool closing = *name == '/';
    if (closing) name++;
    const bool title = std::strncmp(name, "title", 5) == 0 && (name[5] == '\0' || std::isspace(name[5]));
    const bool heading =
        name[0] == 'h' && (name[1] == '1' || name[1] == '2') && (name[2] == '\0' || std::isspace(name[2]));

    if (!closing && (std::strstr(name, "doc-toc") || hasTocTypeAttribute(name))) {
      contentsDocument = true;
    }
    if (!closing && (title || heading)) {
      collectingLabel = true;
      clearLabel();
    } else if (closing && (title || heading)) {
      while (labelLength > 0 && label[labelLength - 1] == ' ') label[--labelLength] = '\0';
      if (std::strcmp(label, "contents") == 0 || std::strcmp(label, "table of contents") == 0) {
        contentsDocument = true;
      }
      collectingLabel = false;
    }
  }

  void consume(const char c) {
    if (insideTag) {
      if (c == '>') {
        finishTag();
        tagLength = 0;
        insideTag = false;
      } else if (tagLength + 1 < kMaxTagLength) {
        tag[tagLength++] = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      return;
    }
    if (c == '<') {
      insideTag = true;
      tagLength = 0;
    } else if (collectingLabel) {
      appendLabel(c);
    }
  }
};
}  // namespace

Epub::Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
  cachePath = resolveCachePathForFilePath(this->filepath, cacheDir);
}

std::string Epub::cachePathForFilePath(const std::string& filepath, const std::string& cacheDir) {
  // Keep on-disk EPUB cache keys stable across standard library/toolchain changes.
  return cacheDir + "/epub_" + std::to_string(ZipFile::fnvHash64(filepath.c_str(), filepath.size()));
}

bool Epub::hasCache(const std::string& filepath, const std::string& cacheDir) {
  return BookMetadataCache::exists(cachePathForFilePath(filepath, cacheDir));
}

std::string Epub::resolveCachePathForFilePath(const std::string& filepath, const std::string& cacheDir) {
  const std::string cachePath = cachePathForFilePath(filepath, cacheDir);
  if (Storage.exists(cachePath.c_str())) {
    return cachePath;
  }

  const std::string legacyCachePath = legacyCachePathForFilePath(filepath, cacheDir);
  if (legacyCachePath == cachePath || !Storage.exists(legacyCachePath.c_str())) {
    return cachePath;
  }

  if (Storage.rename(legacyCachePath.c_str(), cachePath.c_str())) {
    LOG_INF("EBP", "Migrated legacy EPUB cache: %s -> %s", legacyCachePath.c_str(), cachePath.c_str());
  } else {
    LOG_ERR("EBP", "Failed to migrate legacy EPUB cache: %s -> %s", legacyCachePath.c_str(), cachePath.c_str());
  }
  return cachePath;
}

bool Epub::findContentOpfFile(std::string* contentOpfFile) const {
  const auto containerPath = "META-INF/container.xml";
  size_t containerSize;

  // Get file size without loading it all into heap
  if (!getItemSize(containerPath, &containerSize)) {
    LOG_ERR("EBP", "Could not find or size META-INF/container.xml");
    return false;
  }

  ContainerParser containerParser(containerSize);

  if (!containerParser.setup()) {
    return false;
  }

  // Stream read (reusing your existing stream logic)
  if (!readItemContentsToStream(containerPath, containerParser, 512)) {
    LOG_ERR("EBP", "Could not read META-INF/container.xml");
    return false;
  }

  // Extract the result
  if (containerParser.fullPath.empty()) {
    LOG_ERR("EBP", "Could not find valid rootfile in container.xml");
    return false;
  }

  *contentOpfFile = std::move(containerParser.fullPath);
  return true;
}

bool Epub::parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, const bool writeSpineEntries,
                           const bool collectCssFiles, const bool metadataOnly, std::string* seriesIndex,
                           SyncMetadata* syncMetadata) {
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    LOG_ERR("EBP", "Could not find content.opf in zip");
    return false;
  }

  contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  size_t contentOpfSize;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    LOG_ERR("EBP", "Could not get size of content.opf");
    return false;
  }

  ContentOpfParser opfParser(getCachePath(), getBasePath(), contentOpfSize,
                             writeSpineEntries ? bookMetadataCache.get() : nullptr, collectCssFiles, metadataOnly);
  if (!opfParser.setup()) {
    LOG_ERR("EBP", "Could not setup content.opf parser");
    if (opfParser.failedForLowMemory()) {
      lastLoadFailure = OpenFailure::OutOfMemory;
    }
    return false;
  }

  // metadataOnly's allowEarlyStop lets the stream stop decompressing once the
  // parser has left </metadata>, well before the manifest/spine/guide of a
  // large content.opf; readItemContentsToStream() reports that as a short
  // write rather than a full success, so it is not itself a read failure here.
  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024, metadataOnly)) {
    LOG_ERR("EBP", "Could not read content.opf");
    if (opfParser.failedForLowMemory()) {
      lastLoadFailure = OpenFailure::OutOfMemory;
    }
    return false;
  }

  // Grab data from opfParser into epub. Normalize titles to NFC so NFD (combining
  // mark) text renders correctly — the device fonts have no mark positioning.
  bookMetadata.title = utf8ComposeNfc(opfParser.title);
  bookMetadata.author = opfParser.author;
  bookMetadata.language = opfParser.language;
  bookMetadata.series = utf8ComposeNfc(opfParser.series);
  if (syncMetadata) {
    syncMetadata->isbn = std::move(opfParser.isbn);
    syncMetadata->asin = std::move(opfParser.asin);
    syncMetadata->series = bookMetadata.series;
    syncMetadata->seriesIndex = opfParser.seriesIndex;
  }
  if (seriesIndex) *seriesIndex = std::move(opfParser.seriesIndex);
  bookMetadata.subject = utf8ComposeNfc(opfParser.subject);

  if (metadataOnly) {
    // Nothing below is populated: the parser stopped at </metadata>, before
    // the manifest that would carry the cover item and TOC/guide references.
    LOG_DBG("EBP", "Successfully parsed package metadata");
    return true;
  }

  bookMetadata.coverItemHref = opfParser.coverItemHref;

  // Guide-based cover fallback: if no cover found via metadata/properties,
  // try extracting the image reference from the guide's cover page XHTML
  if (bookMetadata.coverItemHref.empty() && !opfParser.guideCoverPageHref.empty()) {
    LOG_DBG("EBP", "No cover from metadata, trying guide cover page: %s", opfParser.guideCoverPageHref.c_str());
    CoverImageRefScanner scanner;
    if (readItemContentsToStream(opfParser.guideCoverPageHref, scanner, 512) && !scanner.imageRef.empty()) {
      std::string coverPageBase;
      const auto lastSlash = opfParser.guideCoverPageHref.rfind('/');
      if (lastSlash != std::string::npos) {
        coverPageBase = opfParser.guideCoverPageHref.substr(0, lastSlash + 1);
      }
      bookMetadata.coverItemHref =
          FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(coverPageBase + scanner.imageRef));
      LOG_DBG("EBP", "Found cover image from guide: %s", bookMetadata.coverItemHref.c_str());
    }
  }

  bookMetadata.textReferenceHref = opfParser.textReferenceHref;

  if (!opfParser.tocNcxPath.empty()) {
    tocNcxItem = opfParser.tocNcxPath;
  }

  if (!opfParser.tocNavPath.empty()) {
    tocNavItem = opfParser.tocNavPath;
  }

  if (!opfParser.guideTocPageHref.empty()) {
    tocGuideItem = opfParser.guideTocPageHref;
  }

  if (collectCssFiles && !opfParser.cssFiles.empty()) {
    cssFiles = std::move(opfParser.cssFiles);
  }

  return true;
}

bool Epub::parseTocNcxFile() const {
  // the ncx file should have been specified in the content.opf file
  if (tocNcxItem.empty()) {
    LOG_DBG("EBP", "No ncx file specified");
    return false;
  }

  size_t ncxSize;
  if (!getItemSize(tocNcxItem, &ncxSize)) {
    LOG_ERR("EBP", "Could not get size of toc ncx file");
    return false;
  }

  TocNcxParser ncxParser(contentBasePath, ncxSize, bookMetadataCache.get());

  if (!ncxParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc ncx parser");
    return false;
  }

  // Stream the decompressed NCX straight into the parser instead of round-tripping
  // through a temp file on the SD card (decompress -> write -> reopen -> reread -> delete).
  if (!readItemContentsToStream(tocNcxItem, ncxParser, 1024)) {
    LOG_ERR("EBP", "Could not read toc ncx file");
    return false;
  }

  return true;
}

bool Epub::parseTocNavFile() const {
  // the nav file should have been specified in the content.opf file (EPUB 3)
  if (tocNavItem.empty()) {
    LOG_DBG("EBP", "No nav file specified");
    return false;
  }

  size_t navSize;
  if (!getItemSize(tocNavItem, &navSize)) {
    LOG_ERR("EBP", "Could not get size of toc nav file");
    return false;
  }

  // Note: We can't use `contentBasePath` here as the nav file may be in a different folder to the content.opf
  // and the HTMLX nav file will have hrefs relative to itself
  const std::string navContentBasePath = tocNavItem.substr(0, tocNavItem.find_last_of('/') + 1);
  TocNavParser navParser(navContentBasePath, navSize, bookMetadataCache.get());

  if (!navParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc nav parser");
    return false;
  }

  // Stream the decompressed nav document straight into the parser instead of round-tripping
  // through a temp file on the SD card (decompress -> write -> reopen -> reread -> delete).
  if (!readItemContentsToStream(tocNavItem, navParser, 1024)) {
    LOG_ERR("EBP", "Could not read toc nav file");
    return false;
  }

  return true;
}

void Epub::discoverCssFilesFromZip() {
  const std::string& opfDir = contentBasePath;
  ZipFile zf(filepath);

  if (!zf.enumerateFilePaths([&](std::string_view filePath) {
        if (!opfDir.empty() && filePath.find(opfDir) != 0) {
          return;
        }

        if (!FsHelpers::hasCssExtension(filePath)) {
          return;
        }

        if (std::find(cssFiles.begin(), cssFiles.end(), filePath) != cssFiles.end()) {
          return;
        }

        cssFiles.push_back(std::string{filePath});
      })) {
    LOG_ERR("EBP", "Failed to enumerate ZIP file paths for CSS discovery");
  }
}

void Epub::releaseCssFileList() {
  // Stylesheet paths are one-shot cache-build scratch. Swap with an empty
  // vector so hundreds of generator-emitted paths do not remain resident
  // while the chapter itself is laid out.
  std::vector<std::string>().swap(cssFiles);
}

Epub::CssParseStatus Epub::parseCssFiles(const bool forceRebuild) const {
  const size_t maxCssFileSize = CssParser::maxSourceBytes();
  // Minimum heap required before attempting CSS parsing
  constexpr size_t MIN_HEAP_FOR_CSS_PARSING = 64 * 1024;  // 64KB

  // Cache validation is intentionally disk-only here. Hydrating the rules just
  // to inspect the header duplicates the section builder's peak allocation.
  if (!forceRebuild) {
    switch (cssParser->inspectCache()) {
      case CssParser::CacheStatus::Complete:
        LOG_DBG("EBP", "CSS cache valid, skipping parseCssFiles");
        return CssParseStatus::Complete;
      case CssParser::CacheStatus::Partial:
        LOG_DBG("EBP", "Partial CSS cache valid, skipping parseCssFiles");
        return CssParseStatus::Partial;
      case CssParser::CacheStatus::Invalid:
        LOG_DBG("EBP", "CSS cache invalid, rebuilding CSS rules");
        cssParser->deleteCache();
        break;
      case CssParser::CacheStatus::Missing:
        break;
    }
  }

  // No cache yet - parse CSS files. If memory runs out partway through, keep
  // the rules already parsed and persist them as a marked partial cache so
  // chapter layout can still use most of the book's stylesheet.
  bool parsedAllCss = true;
  size_t parsedCssFileCount = 0;
  size_t skippedDuplicateCount = 0;
  size_t failedCssFileIndex = 0;
  std::string failedCssPath;

  // Some EPUB generators emit a byte-identical stylesheet for every chapter.
  // Resolve all CSS identities with one ZIP directory scan, then retain only
  // identities whose first stylesheet parsed successfully. These temporary
  // arrays are fallible and live only for this one-time cache build.
  std::unique_ptr<ZipFile::EntryTarget[]> cssTargets;
  std::unique_ptr<ZipFile::EntryIdentity[]> cssIdentities;
  std::unique_ptr<ZipFile::EntryIdentity[]> parsedIdentities;
  size_t parsedIdentityCount = 0;
  if (cssFiles.size() > 1 && cssFiles.size() <= UINT16_MAX) {
    cssTargets = makeUniqueNoThrow<ZipFile::EntryTarget[]>(cssFiles.size());
    cssIdentities = makeUniqueNoThrow<ZipFile::EntryIdentity[]>(cssFiles.size());
    parsedIdentities = makeUniqueNoThrow<ZipFile::EntryIdentity[]>(cssFiles.size());
    if (!cssTargets || !cssIdentities || !parsedIdentities) {
      LOG_ERR("EBP", "Could not allocate CSS duplicate metadata; parsing all stylesheets");
      cssTargets.reset();
      cssIdentities.reset();
      parsedIdentities.reset();
    } else {
      for (size_t i = 0; i < cssFiles.size(); ++i) {
        cssTargets[i] = {ZipFile::fnvHash64(cssFiles[i].data(), cssFiles[i].size()),
                         static_cast<uint16_t>(cssFiles[i].size()), static_cast<uint16_t>(i), cssFiles[i].c_str()};
      }
      std::sort(cssTargets.get(), cssTargets.get() + cssFiles.size(),
                [](const ZipFile::EntryTarget& a, const ZipFile::EntryTarget& b) {
                  return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
                });
      ZipFile(filepath).fillEntryIdentities(cssTargets.get(), cssFiles.size(), cssIdentities.get(), cssFiles.size());
    }
  }

  for (size_t cssFileIndex = 0; cssFileIndex < cssFiles.size(); ++cssFileIndex) {
    const auto& cssPath = cssFiles[cssFileIndex];
    const auto* identity = cssIdentities ? &cssIdentities[cssFileIndex] : nullptr;
    if (identity && identity->found) {
      const auto duplicate = std::find_if(parsedIdentities.get(), parsedIdentities.get() + parsedIdentityCount,
                                          [identity](const ZipFile::EntryIdentity& parsed) {
                                            return parsed.crc32 == identity->crc32 &&
                                                   parsed.compressedSize == identity->compressedSize &&
                                                   parsed.uncompressedSize == identity->uncompressedSize;
                                          });
      if (duplicate != parsedIdentities.get() + parsedIdentityCount) {
        ++skippedDuplicateCount;
        continue;
      }
    }

    // Check heap before parsing - CSS parsing allocates heavily
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_CSS_PARSING) {
      LOG_ERR("EBP", "Insufficient heap for CSS parsing (%u bytes free, need %zu), skipping: %s", freeHeap,
              MIN_HEAP_FOR_CSS_PARSING, cssPath.c_str());
      parsedAllCss = false;
      failedCssFileIndex = cssFileIndex + 1;
      failedCssPath = cssPath;
      break;
    }

    // Check CSS file size before decompressing - skip files that are too large
    size_t cssFileSize = 0;
    if (getItemSize(cssPath, &cssFileSize)) {
      if (cssFileSize > maxCssFileSize) {
        LOG_ERR("EBP", "CSS file too large (%zu bytes > %zu max), skipping: %s", cssFileSize, maxCssFileSize,
                cssPath.c_str());
        continue;
      }
    }

    // Extract CSS file to temp location
    const auto tmpCssPath = getCachePath() + "/.tmp.css";
    FsFile tempCssFile;
    if (!Storage.openFileForWrite("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not create temp CSS file");
      continue;
    }
    if (!readItemContentsToStream(cssPath, tempCssFile, 1024)) {
      LOG_ERR("EBP", "Could not read CSS file: %s", cssPath.c_str());
      // Explicitly close() file before calling Storage.remove()
      tempCssFile.close();
      Storage.remove(tmpCssPath.c_str());
      continue;
    }
    // Explicitly close() file before reopening for reading
    tempCssFile.close();

    // Parse the CSS file
    if (!Storage.openFileForRead("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not open temp CSS file for reading");
      Storage.remove(tmpCssPath.c_str());
      continue;
    }
    const auto slash = cssPath.find_last_of('/');
    const std::string_view baseDir =
        slash == std::string::npos ? std::string_view{} : std::string_view(cssPath).substr(0, slash + 1);
    if (!cssParser->loadFromStream(tempCssFile, baseDir)) {
      failedCssFileIndex = cssFileIndex + 1;
      failedCssPath = cssPath;
      LOG_ERR("EBP", "CSS parsing failed for file %zu/%zu after %zu parsed files: %s", failedCssFileIndex,
              cssFiles.size(), parsedCssFileCount, cssPath.c_str());
      parsedAllCss = false;
    } else {
      ++parsedCssFileCount;
      if (identity && identity->found) {
        parsedIdentities[parsedIdentityCount++] = *identity;
      }
    }
    // Explicitly close() file before calling Storage.remove()
    tempCssFile.close();
    Storage.remove(tmpCssPath.c_str());
    if (!parsedAllCss) {
      break;
    }
  }

  if (!parsedAllCss && cssParser->empty()) {
    LOG_ERR("EBP", "CSS parsing failed for %s before any usable rules were loaded; CSS cache will not be written",
            failedCssPath.empty() ? "<unknown>" : failedCssPath.c_str());
    cssParser->clear();
    return CssParseStatus::Failed;
  }

  if (!parsedAllCss) {
    LOG_ERR("EBP", "Saving %zu partial CSS rules after parse stopped in %s", cssParser->ruleCount(),
            failedCssPath.empty() ? "<unknown>" : failedCssPath.c_str());
  }

  // Save to cache for next time
  if (!cssParser->saveToCache(parsedAllCss)) {
    LOG_ERR("EBP", "Failed to save CSS rules to cache");
    cssParser->clear();
    return CssParseStatus::Failed;
  }

  LOG_DBG("EBP", "Loaded %zu %s CSS style rules from %zu/%zu files", cssParser->ruleCount(),
          parsedAllCss ? "complete" : "partial", parsedCssFileCount, cssFiles.size());
  if (skippedDuplicateCount > 0) {
    LOG_DBG("EBP", "Skipped %zu byte-identical CSS files", skippedDuplicateCount);
  }
  cssParser->clear();
  return parsedAllCss ? CssParseStatus::Complete : CssParseStatus::Partial;
}

bool Epub::openProtection() const {
  decryptor = freeink::content::openProtectedBook(filepath, protectionError);
  if (!protectionError.empty()) LOG_ERR("EBP", "Protected book: %s", protectionError.c_str());
  return protectionError.empty();
}

// load in the meta data for the epub file
bool Epub::load(const bool buildIfMissing, const bool skipLoadingCss, const XLocationLoadMode xLocationLoadMode,
                const bool cacheCumulativeSpineSizes) {
  lastLoadFailure = OpenFailure::InvalidOrUnreadable;
  // Enforce access before loading any previously rendered cache.
  if (!openProtection()) {
    lastLoadFailure = protectionError == "access expired"         ? OpenFailure::LoanExpired
                      : protectionError == "loan date unverified" ? OpenFailure::LoanClockUnavailable
                      : protectionError == "out of memory"        ? OpenFailure::OutOfMemory
                                                                  : OpenFailure::ProtectedUnavailable;
    return false;
  }
  // Initialize spine/TOC cache
  bookMetadataCache = makeBookMetadataCacheNoThrow(cachePath, cacheCumulativeSpineSizes);
  if (!bookMetadataCache) {
    lastLoadFailure = OpenFailure::OutOfMemory;
    return false;
  }
  // Always create CssParser - needed for inline style parsing even without CSS files
  cssParser = makeCssParserNoThrow(cachePath);
  if (!cssParser) {
    lastLoadFailure = OpenFailure::OutOfMemory;
    bookMetadataCache.reset();
    return false;
  }

  // Try to load existing cache first
  if (bookMetadataCache->load()) {
    if (!skipLoadingCss) {
      // Rebuild CSS cache when missing, invalid, or previously incomplete.
      bool rebuildCssCache = false;
      bool forceCssRebuild = false;
      bool retryingPartialCssCache = false;
      switch (cssParser->inspectCache()) {
        case CssParser::CacheStatus::Missing:
          LOG_DBG("EBP", "CSS rules cache missing, attempting to parse CSS files");
          rebuildCssCache = true;
          break;
        case CssParser::CacheStatus::Invalid:
          LOG_DBG("EBP", "CSS rules cache invalid, attempting to parse CSS files");
          cssParser->deleteCache();
          rebuildCssCache = true;
          break;
        case CssParser::CacheStatus::Partial:
          LOG_DBG("EBP", "CSS rules cache is partial, attempting to rebuild complete CSS cache");
          rebuildCssCache = true;
          forceCssRebuild = true;
          retryingPartialCssCache = true;
          break;
        case CssParser::CacheStatus::Complete:
          break;
      }

      if (rebuildCssCache) {
        BookMetadataCache::BookMetadata cachedMetadata = bookMetadataCache->coreMetadata;
        if (!parseContentOpf(cachedMetadata, /*writeSpineEntries=*/false, /*collectCssFiles=*/true)) {
          LOG_ERR("EBP", "Could not parse content.opf from cached bookMetadata for CSS files");
          // continue anyway - book will work without CSS and we'll still load any inline style CSS
        } else {
          discoverCssFilesFromZip();
        }
        bookMetadataCache.reset();
        const CssParseStatus cssStatus = parseCssFiles(forceCssRebuild);
        releaseCssFileList();
        bookMetadataCache = makeBookMetadataCacheNoThrow(cachePath, cacheCumulativeSpineSizes);
        if (!bookMetadataCache) {
          lastLoadFailure = OpenFailure::OutOfMemory;
          return false;
        }
        if (!bookMetadataCache->load()) {
          LOG_ERR("EBP", "Failed to reload cache after CSS rebuild");
          return false;
        }
        if (cssStatus == CssParseStatus::Complete ||
            (cssStatus == CssParseStatus::Partial && !retryingPartialCssCache)) {
          // Invalidate section caches so they are rebuilt with the new CSS.
          Storage.removeDir((cachePath + "/sections").c_str());
        } else if (cssStatus == CssParseStatus::Partial) {
          LOG_ERR("EBP", "CSS cache is still partial after rebuild; preserving existing section caches");
        } else {
          LOG_ERR("EBP", "CSS cache rebuild failed; preserving existing section caches");
        }
      }
    }
    // Release the resolved CSS rule map: it is only needed transiently while building
    // section caches, and createSectionFile reloads it from cache on demand. Holding it
    // resident pins tens of KB for the whole reading session (more on warm resume into
    // an already-cached chapter, where createSectionFile never runs to clear it).
    cssParser->clear();
    if (xLocationLoadMode == XLocationLoadMode::Immediate) {
      loadXLocations();
    }

    lastLoadFailure = OpenFailure::None;
    return true;
  }

  // If we didn't load from cache above and we aren't allowed to build, fail now
  if (!buildIfMissing) {
    return false;
  }

  // Cache doesn't exist or is invalid, build it
  LOG_DBG("EBP", "Cache not found, building spine/TOC cache");
  setupCacheDir();

  const uint32_t indexingStart = millis();

  // Begin building cache - stream entries to disk immediately
  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("EBP", "Could not begin writing cache");
    return false;
  }

  // OPF Pass
  BookMetadataCache::BookMetadata bookMetadata;
  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR("EBP", "Could not begin writing content.opf pass");
    return false;
  }
  if (!parseContentOpf(bookMetadata, /*writeSpineEntries=*/true, /*collectCssFiles=*/!skipLoadingCss)) {
    LOG_ERR("EBP", "Could not parse content.opf");
    return false;
  }
  if (!skipLoadingCss) {
    discoverCssFilesFromZip();
  }
  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR("EBP", "Could not end writing content.opf pass");
    return false;
  }

  // TOC Pass - try EPUB 3 nav first, fall back to NCX
  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR("EBP", "Could not begin writing toc pass");
    if (bookMetadataCache->failedForLowMemory()) {
      lastLoadFailure = OpenFailure::OutOfMemory;
    }
    return false;
  }

  bool tocParsed = false;

  // Try EPUB 3 nav document first (preferred)
  if (!tocNavItem.empty()) {
    tocParsed = parseTocNavFile();
  }

  // Fall back to NCX if nav parsing failed or wasn't available
  if (!tocParsed && !tocNcxItem.empty()) {
    tocParsed = parseTocNcxFile();
  }

  if (!tocParsed) {
    LOG_ERR("EBP", "Warning: Could not parse any TOC format");
    // Continue anyway - book will work without TOC
  }

  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR("EBP", "Could not end writing toc pass");
    return false;
  }

  // Close the cache files
  if (!bookMetadataCache->endWrite()) {
    LOG_ERR("EBP", "Could not end writing cache");
    return false;
  }

  // Build final book.bin
  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata)) {
    LOG_ERR("EBP", "Could not update mappings and sizes");
    if (bookMetadataCache->failedForLowMemory()) {
      lastLoadFailure = OpenFailure::OutOfMemory;
    }
    return false;
  }
  LOG_DBG("EBP", "Total indexing completed in %lu ms", millis() - indexingStart);

  if (!bookMetadataCache->cleanupTmpFiles()) {
    LOG_DBG("EBP", "Could not cleanup tmp files - ignoring");
  }

  if (!skipLoadingCss) {
    // Parse CSS before reloading book.bin to keep heap as open as possible for rule-table growth.
    bookMetadataCache.reset();
    if (parseCssFiles() != CssParseStatus::Failed) {
      Storage.removeDir((cachePath + "/sections").c_str());
    } else {
      LOG_ERR("EBP", "CSS cache build failed; leaving any existing section caches in place");
    }
    releaseCssFileList();
  }

  // Reload the cache from disk so it's in the correct state
  bookMetadataCache = makeBookMetadataCacheNoThrow(cachePath, cacheCumulativeSpineSizes);
  if (!bookMetadataCache) {
    lastLoadFailure = OpenFailure::OutOfMemory;
    return false;
  }
  if (!bookMetadataCache->load()) {
    LOG_ERR("EBP", "Failed to reload cache after writing");
    return false;
  }

  if (xLocationLoadMode == XLocationLoadMode::Immediate) {
    loadXLocations();
  }

  lastLoadFailure = OpenFailure::None;
  return true;
}

bool Epub::loadMetadata(std::string& title, std::string& author, const bool allowCachedMetadata, std::string* series,
                        std::string* genre, std::string* seriesIndex) {
  title.clear();
  author.clear();
  if (series) series->clear();
  if (genre) genre->clear();
  if (seriesIndex) seriesIndex->clear();

  // The reader cache holds title and author but not the Library's series and
  // genre fields. Reuse it for callers that need only title/author; the Library
  // parses OPF metadata once and then keeps the extra fields in its own index.
  // This LOCAL reader does not alter the full load()/spine cache lifecycle.
  if (allowCachedMetadata && !series && !genre && !seriesIndex) {
    auto metadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
    if (metadataCache && metadataCache->load()) {
      title = metadataCache->coreMetadata.title;
      author = metadataCache->coreMetadata.author;
      return true;
    }
    if (!metadataCache) {
      LOG_ERR("EBP", "Could not allocate metadata cache reader");
    }
  }

  BookMetadataCache::BookMetadata metadata;
  const bool loaded = parseContentOpf(metadata, /*writeSpineEntries=*/false, /*collectCssFiles=*/false,
                                      /*metadataOnly=*/true, seriesIndex);
  if (!loaded) return false;

  title = std::move(metadata.title);
  author = std::move(metadata.author);
  if (series) *series = std::move(metadata.series);
  if (genre) *genre = std::move(metadata.subject);
  return true;
}

bool Epub::loadSyncMetadata(SyncMetadata& metadata) {
  metadata = {};
  BookMetadataCache::BookMetadata bookMetadata;
  return parseContentOpf(bookMetadata, false, false, true, nullptr, &metadata);
}

bool Epub::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("EPB", "Failed to clear cache");
    return false;
  }

  return true;
}

void Epub::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  Storage.mkdir(cachePath.c_str());
}

const std::string& Epub::getCachePath() const { return cachePath; }

const std::string& Epub::getPath() const { return filepath; }

const std::string& Epub::getTitle() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.title;
}

const std::string& Epub::getAuthor() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.author;
}

const std::string& Epub::getLanguage() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.language;
}

bool Epub::hasCoverImage() const {
  return bookMetadataCache && bookMetadataCache->isLoaded() && !bookMetadataCache->coreMetadata.coverItemHref.empty();
}

std::string Epub::getCoverBmpPath(bool cropped, bool imageLevels) const {
  const auto coverFileName = std::string("cover") + (cropped ? "_crop" : "") + (imageLevels ? "_absolute" : "");
  return cachePath + "/" + coverFileName + ".bmp";
}

namespace {
// One process-lifetime mutex serializes the converter callback and temporary files.
const SemaphoreHandle_t gifGenerationMutex = xSemaphoreCreateMutex();

void removeGifTemporaryFile(const std::string& path) {
  if (!Storage.remove(path.c_str()) && Storage.exists(path.c_str())) {
    LOG_ERR("EBP", "Failed to remove GIF temporary file: %s", path.c_str());
  }
}

bool extractGifCover(const Epub& epub, const std::string& href, const std::string& path) {
  FsFile file;
  bool success = false;
  const ScopedCleanup cleanup{[&]() {
    file.close();
    if (!success) removeGifTemporaryFile(path);
  }};
  if (!Storage.openFileForWrite("EBP", path, file)) {
    LOG_ERR("EBP", "Failed to create GIF extraction file: %s", path.c_str());
    return false;
  }
  GifLimitedWriter limited(file, GifCommon::MAX_FILE_BYTES);
  success = epub.readItemContentsToStream(href, limited, 1024);
  const bool closed = file.close();
  success = success && closed;
  if (!success) LOG_ERR("EBP", "GIF extraction failed, exceeded byte limit, or could not close: %s", path.c_str());
  return success;
}

template <typename Convert>
bool generateGifBmp(const Epub& epub, const std::string& href, const std::string& bmpPath, Convert convert) {
  if (!gifGenerationMutex || xSemaphoreTake(gifGenerationMutex, portMAX_DELAY) != pdTRUE) {
    LOG_ERR("EBP", "Failed to lock GIF cache generation");
    return false;
  }
  const ScopedCleanup unlock{[]() { xSemaphoreGive(gifGenerationMutex); }};

  const auto gifTempPath = epub.getCachePath() + "/.cover.gif";
  const auto bmpTempPath = bmpPath + ".tmp";
  if (!extractGifCover(epub, href, gifTempPath)) return false;

  FsFile gifFile;
  if (!Storage.openFileForRead("EBP", gifTempPath, gifFile)) {
    LOG_ERR("EBP", "Failed to open extracted GIF: %s", gifTempPath.c_str());
    removeGifTemporaryFile(gifTempPath);
    return false;
  }
  FsFile bmpFile;
  if (!Storage.openFileForWrite("EBP", bmpTempPath, bmpFile)) {
    LOG_ERR("EBP", "Failed to create GIF BMP: %s", bmpPath.c_str());
    gifFile.close();
    removeGifTemporaryFile(gifTempPath);
    removeGifTemporaryFile(bmpTempPath);
    return false;
  }

  bool success = convert(gifFile, bmpFile);
  const bool sourceClosed = gifFile.close();
  const bool outputClosed = bmpFile.close();
  success = success && sourceClosed && outputClosed;
  removeGifTemporaryFile(gifTempPath);
  // SdFat cannot rename over an existing file. Only discard the rejected cache
  // after its replacement has been converted and closed successfully.
  if (success && Storage.exists(bmpPath.c_str()) && !Storage.remove(bmpPath.c_str())) {
    LOG_ERR("EBP", "Failed to replace invalid GIF BMP: %s", bmpPath.c_str());
    success = false;
  }
  if (success && !Storage.rename(bmpTempPath.c_str(), bmpPath.c_str())) {
    LOG_ERR("EBP", "Failed to publish GIF BMP: %s", bmpPath.c_str());
    success = false;
  }
  if (!success) removeGifTemporaryFile(bmpTempPath);
  return success;
}
}  // namespace

bool Epub::generateCoverBmp(bool cropped, const GfxRenderer* renderer, const int readerFontId, bool imageLevels) const {
  if (!decryptor && !openProtection()) return false;
  // Already generated, return true
  if (Storage.exists(getCoverBmpPath(cropped, imageLevels).c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate cover BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    LOG_ERR("EBP", "No known cover image");
    return false;
  }

  if (FsHelpers::hasJpgExtension(coverImageHref)) {
    std::string coverJpgPath;
    if (!ensureCachedCoverImage(coverImageHref, coverJpgPath)) {
      return false;
    }

    FsFile coverJpg;
    if (!Storage.openFileForRead("EBP", coverJpgPath, coverJpg)) {
      return false;
    }

    FsFile coverBmp;
    if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped, imageLevels), coverBmp)) {
      coverJpg.close();
      return false;
    }
    releaseReaderSdFontCachesBeforeCoverDecode(renderer, readerFontId, "cover JPG decode");
    const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp, cropped, imageLevels);
    // Explicitly close() files before leaving the converter path.
    coverJpg.close();
    coverBmp.close();

    if (!success) {
      LOG_ERR("EBP", "Failed to generate BMP from cover image");
      Storage.remove(getCoverBmpPath(cropped, imageLevels).c_str());
    }
    return success;
  }

  if (FsHelpers::hasPngExtension(coverImageHref)) {
    std::string coverPngPath;
    if (!ensureCachedCoverImage(coverImageHref, coverPngPath)) {
      return false;
    }

    FsFile coverPng;
    if (!Storage.openFileForRead("EBP", coverPngPath, coverPng)) {
      return false;
    }

    FsFile coverBmp;
    if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped, imageLevels), coverBmp)) {
      coverPng.close();
      return false;
    }
    releaseReaderSdFontCachesBeforeCoverDecode(renderer, readerFontId, "cover PNG decode");
    const bool success = PngToBmpConverter::pngFileToBmpStream(coverPng, coverBmp, cropped, imageLevels);
    // Explicitly close() files before leaving the converter path.
    coverPng.close();
    coverBmp.close();

    if (!success) {
      LOG_ERR("EBP", "Failed to generate BMP from PNG cover image");
      Storage.remove(getCoverBmpPath(cropped, imageLevels).c_str());
    }
    return success;
  }

  if (FsHelpers::hasGifExtension(coverImageHref)) {
    releaseReaderSdFontCachesBeforeCoverDecode(renderer, readerFontId, "cover GIF decode");
    return generateGifBmp(*this, coverImageHref, getCoverBmpPath(cropped, imageLevels),
                          [cropped, imageLevels](FsFile& gif, Print& bmp) {
                            return GifToBmpConverter::gifFileToBmpStream(gif, bmp, cropped, imageLevels);
                          });
  }

  LOG_ERR("EBP", "Cover image is not a supported format, skipping");
  return false;
}

std::string Epub::getThumbBmpPath() const { return cachePath + "/thumb_[WIDTH]x[HEIGHT].bmp"; }
std::string Epub::getThumbBmpPath(int height) const { return getThumbBmpPath(0, height); }
std::string Epub::getThumbBmpPath(int width, int height) const {
  normalizeThumbDimensions(width, height);
  const std::string newPath = getThumbBmpPathForDimensions(cachePath, width, height);
  if (Storage.exists(newPath.c_str())) {
    return newPath;
  }
  const std::string legacyPath = cachePath + "/thumb_" + std::to_string(height) + ".bmp";
  if (Storage.exists(legacyPath.c_str())) {
    return legacyPath;
  }
  return newPath;
}

std::string Epub::getAdaptiveThumbBmpPath(int width, int height) const {
  normalizeThumbDimensions(width, height);
  return getAdaptiveThumbBmpPathForDimensions(cachePath, width, height);
}

bool Epub::generateThumbBmp(int height, const GfxRenderer* renderer, const int readerFontId) const {
  return generateThumbBmp(0, height, renderer, readerFontId);
}

bool Epub::generateThumbBmp(int width, int height, const GfxRenderer* renderer, const int readerFontId) const {
  return generateThumbBmpInternal(width, height, false, renderer, readerFontId);
}

bool Epub::generateThumbBmpFromSource(int height, const GfxRenderer* renderer, const int readerFontId) {
  return generateThumbBmpFromSource(0, height, renderer, readerFontId);
}

bool Epub::generateThumbBmpFromSource(int width, int height, const GfxRenderer* renderer, const int readerFontId) {
  normalizeThumbDimensions(width, height);
  const std::string thumbPath = getThumbBmpPathForDimensions(cachePath, width, height);
  if (cachedBmpMatchesDimensions(thumbPath, width, height)) return true;

  auto metadata = makeUniqueNoThrow<BookMetadataCache::BookMetadata>();
  if (!metadata) {
    LOG_ERR("EBP", "Cannot allocate cover metadata");
    return false;
  }
  setupCacheDir();
  if (!parseContentOpf(*metadata, /*writeSpineEntries=*/false, /*collectCssFiles=*/false)) {
    return false;
  }
  return generateThumbBmpInternal(width, height, false, renderer, readerFontId, &metadata->coverItemHref);
}

bool Epub::generateAdaptiveThumbBmp(int width, int height, const GfxRenderer* renderer, const int readerFontId) const {
  return generateThumbBmpInternal(width, height, true, renderer, readerFontId);
}

std::string Epub::getCachedCoverImagePath(const std::string& coverImageHref) const {
  if (FsHelpers::hasJpgExtension(coverImageHref)) {
    return getCachePath() + "/cover_src.jpg";
  }
  if (FsHelpers::hasPngExtension(coverImageHref)) {
    return getCachePath() + "/cover_src.png";
  }
  return {};
}

bool Epub::ensureCachedCoverImage(const std::string& coverImageHref, std::string& outPath) const {
  outPath = getCachedCoverImagePath(coverImageHref);
  if (outPath.empty()) {
    LOG_ERR("EBP", "Cover image is not a supported format, cannot cache source");
    return false;
  }
  if (Storage.exists(outPath.c_str())) {
    return true;
  }

  const std::string tmpPath = outPath + ".tmp";
  if (Storage.exists(tmpPath.c_str())) {
    Storage.remove(tmpPath.c_str());
  }

  FsFile coverFile;
  if (!Storage.openFileForWrite("EBP", tmpPath, coverFile)) {
    return false;
  }
  if (!readItemContentsToStream(coverImageHref, coverFile, 1024)) {
    LOG_ERR("EBP", "Failed to cache cover image item: %s", coverImageHref.c_str());
    coverFile.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  coverFile.close();

  if (Storage.exists(outPath.c_str())) {
    Storage.remove(outPath.c_str());
  }
  if (!Storage.rename(tmpPath.c_str(), outPath.c_str())) {
    LOG_ERR("EBP", "Failed to finalize cached cover image: %s", outPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }

  return true;
}

bool Epub::generateThumbBmpInternal(int width, int height, const bool adaptiveContain, const GfxRenderer* renderer,
                                    const int readerFontId, const std::string* coverHrefOverride) const {
  if (height <= 0) {
    LOG_DBG("EBP", "Using default thumb BMP height for requested dimensions: %dx%d", width, height);
  }
  normalizeThumbDimensions(width, height);
  const std::string thumbPath = adaptiveContain ? getAdaptiveThumbBmpPathForDimensions(cachePath, width, height)
                                                : getThumbBmpPathForDimensions(cachePath, width, height);

  // Already generated with matching dimensions, return true
  if (cachedBmpMatchesDimensions(thumbPath, width, height, adaptiveContain)) {
    return true;
  }

  if (!coverHrefOverride && (!bookMetadataCache || !bookMetadataCache->isLoaded())) {
    LOG_ERR("EBP", "Cannot generate thumb BMP, cache not loaded");
    return false;
  }

  const auto& coverImageHref = coverHrefOverride ? *coverHrefOverride : bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    LOG_DBG("EBP", "No known cover image for thumbnail");
  } else if (FsHelpers::hasJpgExtension(coverImageHref)) {
    std::string coverJpgPath;
    if (!ensureCachedCoverImage(coverImageHref, coverJpgPath)) {
      return false;
    }

    FsFile coverJpg;
    if (!Storage.openFileForRead("EBP", coverJpgPath, coverJpg)) {
      return false;
    }

    FsFile thumbBmp;
    if (!Storage.openFileForWrite("EBP", thumbPath, thumbBmp)) {
      coverJpg.close();
      return false;
    }
    int THUMB_TARGET_WIDTH = width;
    int THUMB_TARGET_HEIGHT = height;
    releaseReaderSdFontCachesBeforeCoverDecode(renderer, readerFontId, "thumbnail JPG decode");
    const bool success = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(coverJpg, thumbBmp, THUMB_TARGET_WIDTH,
                                                                             THUMB_TARGET_HEIGHT, adaptiveContain);
    // Explicitly close() files before leaving the converter path.
    coverJpg.close();
    thumbBmp.close();

    if (!success) {
      LOG_ERR("EBP", "Failed to generate thumb BMP from JPG cover image");
      Storage.remove(thumbPath.c_str());
    }
    return success;
  } else if (FsHelpers::hasPngExtension(coverImageHref)) {
    std::string coverPngPath;
    if (!ensureCachedCoverImage(coverImageHref, coverPngPath)) {
      return false;
    }

    FsFile coverPng;
    if (!Storage.openFileForRead("EBP", coverPngPath, coverPng)) {
      return false;
    }

    FsFile thumbBmp;
    if (!Storage.openFileForWrite("EBP", thumbPath, thumbBmp)) {
      coverPng.close();
      return false;
    }
    int THUMB_TARGET_WIDTH = width;
    int THUMB_TARGET_HEIGHT = height;
    releaseReaderSdFontCachesBeforeCoverDecode(renderer, readerFontId, "thumbnail PNG decode");
    const bool success = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(coverPng, thumbBmp, THUMB_TARGET_WIDTH,
                                                                           THUMB_TARGET_HEIGHT, adaptiveContain);
    // Explicitly close() files before leaving the converter path.
    coverPng.close();
    thumbBmp.close();

    if (!success) {
      LOG_ERR("EBP", "Failed to generate thumb BMP from PNG cover image");
      Storage.remove(thumbPath.c_str());
    }
    return success;
  } else if (FsHelpers::hasGifExtension(coverImageHref)) {
    releaseReaderSdFontCachesBeforeCoverDecode(renderer, readerFontId, "thumbnail GIF decode");
    return generateGifBmp(*this, coverImageHref, thumbPath, [width, height, adaptiveContain](FsFile& gif, Print& bmp) {
      return GifToBmpConverter::gifFileTo1BitBmpStreamWithSize(gif, bmp, width, height, adaptiveContain);
    });
  } else {
    LOG_ERR("EBP", "Cover image is not a supported format, skipping thumbnail");
  }

  return false;
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, const bool trailingNullByte) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return nullptr;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);

  // Decode encrypted entries on demand in memory.
  if (decryptor && decryptor->isEncrypted(path)) {
    const size_t plainSize = decryptor->decryptedSize(path);
    if (plainSize > SIZE_MAX - (trailingNullByte ? 1 : 0)) return nullptr;
    const size_t total = plainSize + (trailingNullByte ? 1 : 0);
    uint8_t* content = static_cast<uint8_t*>(malloc(total > 0 ? total : 1));
    if (!content) {
      LOG_ERR("EBP", "insufficient memory for %s (%u bytes)", path.c_str(), static_cast<unsigned>(total));
      return nullptr;
    }
    struct BufferSink {
      uint8_t* data;
      size_t capacity;
      size_t written;
    } state{content, plainSize, 0};
    auto append = [](void* context, const uint8_t* data, size_t size) {
      auto* target = static_cast<BufferSink*>(context);
      if (size > SIZE_MAX - target->written - 1) return false;
      const size_t needed = target->written + size;
      if (needed > target->capacity) {
        // ADEPT manifests may omit OriginalLength; the ZIP then reports the
        // compressed ciphertext size, so grow only as plaintext arrives.
        auto* grown = static_cast<uint8_t*>(realloc(target->data, needed + 1));
        if (!grown) {
          LOG_ERR("EBP", "OOM growing decrypted resource");
          return false;
        }
        target->data = grown;
        target->capacity = needed;
      }
      memcpy(target->data + target->written, data, size);
      target->written += size;
      return true;
    };
    if (!decryptor->decryptToSink(path, append, &state)) {
      free(state.data);
      LOG_ERR("EBP", "content read failed for %s", path.c_str());
      return nullptr;
    }
    if (trailingNullByte) state.data[state.written] = 0;
    if (size) *size = state.written;
    return state.data;
  }

  const auto content = ZipFile(filepath).readFileToMemory(path.c_str(), size, trailingNullByte);
  if (!content) {
    LOG_DBG("EBP", "Failed to read item %s", path.c_str());
    return nullptr;
  }

  return content;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize,
                                    const bool allowEarlyStop) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return false;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);
  if (decryptor && decryptor->isEncrypted(path)) {
    struct Sink {
      Print& out;
      bool stopped = false;
    } sink{out};
    auto append = [](void* context, const uint8_t* bytes, size_t length) {
      auto& sink = *static_cast<Sink*>(context);
      if (sink.stopped) return true;
      sink.stopped = sink.out.write(bytes, length) != length;
      // Finish decryption/decompression even when a parser stops early.
      return true;
    };
    return decryptor->decryptToSink(path, append, &sink) && (!sink.stopped || allowEarlyStop);
  }
  return ZipFile(filepath).readFileToStream(path.c_str(), out, chunkSize, allowEarlyStop);
}

bool Epub::extractItemToFile(const std::string& itemHref, const std::string& destPath, const size_t chunkSize) const {
  FsFile out;
  if (!Storage.openFileForWrite("EBP", destPath, out)) {
    return false;
  }

  const uint32_t start = millis();
  const bool success = readItemContentsToStream(itemHref, out, chunkSize);
  const uint32_t written = millis();
  const size_t bytes = out.size();
  out.flush();
  out.close();
  LOG_DBG("EBP", "Extracted %s: ok=%d bytes=%u stream=%ums flush/close=%ums chunk=%u", itemHref.c_str(), success,
          static_cast<unsigned>(bytes), static_cast<unsigned>(written - start),
          static_cast<unsigned>(millis() - written), static_cast<unsigned>(chunkSize));
  if (!success) {
    Storage.remove(destPath.c_str());
  }
  return success;
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  if (decryptor && decryptor->isEncrypted(path)) {
    // ADEPT may omit OriginalLength. Ciphertext size is not an exact size
    // (publisher-font loading, for example, compares it with extracted bytes).
    size_t count = 0;
    auto countBytes = [](void* context, const uint8_t*, size_t length) {
      auto& count = *static_cast<size_t*>(context);
      if (length > SIZE_MAX - count) return false;
      count += length;
      return true;
    };
    if (!decryptor->decryptToSink(path, countBytes, &count)) {
      LOG_ERR("EBP", "Could not measure protected resource: %s", path.c_str());
      return false;
    }
    if (size) *size = count;
    return true;
  }
  return ZipFile(filepath).getInflatedFileSize(path.c_str(), size);
}

namespace {
constexpr char kOptimizerIndexPath[] = "META-INF/crossink/optimizer-images-v1.idx";
using OptimizerFormat::IndexScratch;
// Optional bounded setup scratch is released before reading the book.
bool validateOptimizerIndex(const std::string& path, const ZipFile::EntryIdentity& identity, uint16_t& count) {
  auto scratch = makeUniqueNoThrow<IndexScratch>();
  if (!scratch) {
    LOG_ERR("EBP", "OOM validating optimizer index");
    return false;
  }
  FsFile file;
  if (!Storage.openFileForRead("EBP", path, file)) return false;
  bool ok = OptimizerFormat::validateIndex(
      [](void* context, uint32_t offset, uint8_t* bytes, size_t length) {
        auto& input = *static_cast<FsFile*>(context);
        return input.seek(offset) && input.read(bytes, length) == static_cast<int>(length);
      },
      &file, file.size(), identity.crc32, identity.uncompressedSize, *scratch, count);
  ok = file.close() && ok;
  if (!ok) LOG_ERR("EBP", "Invalid optimizer index: %s", path.c_str());
  return ok;
}
}  // namespace

bool Epub::findOptimizerImage(const std::string& itemHref) const {
  if (!optimizerIndexReady || itemHref.empty() || itemHref.size() > 128) return false;
  const std::string normalized = FsHelpers::normalisePath(itemHref);
  if (normalized == optimizerLastHit.href) return true;
  FsFile file;
  if (!Storage.openFileForRead("EBP", cachePath + "/optimizer-images.idx", file)) return false;
  uint8_t bytes[208];
  bool found = false;
  for (uint16_t i = 0; i < optimizerIndexCount; ++i) {
    if (!file.seek(32U + 208U * i) || file.read(bytes, sizeof(bytes)) != sizeof(bytes)) break;
    if (memcmp(bytes, normalized.c_str(), std::min<size_t>(normalized.size() + 1, 129)) == 0 &&
        normalized.size() <= 128 && OptimizerFormat::decodeRecord(bytes, optimizerLastHit)) {
      found = true;
      break;
    }
  }
  file.close();
  return found;
}

bool Epub::getOptimizerImageDimensions(const std::string& itemHref, uint16_t& width, uint16_t& height) const {
  if (!findOptimizerImage(itemHref)) return false;
  width = optimizerLastHit.width;
  height = optimizerLastHit.height;
  return true;
}

bool Epub::seedOptimizerImageCache(const std::string& itemHref, const int expectedWidth, const int expectedHeight,
                                   const std::string& destPxcPath) const {
  if (expectedWidth <= 0 || expectedHeight <= 0 || expectedWidth > 1024 || expectedHeight > 1024 ||
      !OptimizerFormat::dimensions(expectedWidth, expectedHeight) || destPxcPath.empty())
    return false;
  const std::string backup = destPxcPath + ".optimizer.previous";
  if (!OptimizerFormat::recoverCache(Storage, destPxcPath.c_str(), backup.c_str())) {
    LOG_ERR("EBP", "Failed to recover previous optimizer image cache");
    return false;
  }
  // Existing exact-layout output always wins, even if the optional transport is corrupt.
  FsFile existing;
  uint16_t w = 0, h = 0;
  bool cached = Storage.openFileForRead("EBP", destPxcPath, existing) && readPxcHeader(existing, w, h) &&
                w == expectedWidth && h == expectedHeight && existing.size() == pxcByteCount(w, h);
  existing.close();
  if (cached) {
    Storage.remove(backup.c_str());
    return true;
  }
  if (!findOptimizerImage(itemHref)) return false;
  const auto& entry = optimizerLastHit;
  size_t bytes = 0;
  if (!getItemSize(entry.pxcHref, &bytes) || bytes != entry.bytes) return false;
  const std::string temp = destPxcPath + ".optimizer.tmp";
  Storage.remove(temp.c_str());
  if (entry.format == 2) {
    // Reused for the reader session, not on the small render-task stack.
    if (!optimizerWorkspace) {
      optimizerWorkspace = makeUniqueNoThrow<PxcV2Workspace>();
      if (optimizerWorkspace)
        LOG_DBG("EBP", "PXC2 workspace: %u bytes (free=%u maxAlloc=%u)", unsigned(sizeof(PxcV2Workspace)),
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    }
    if (!optimizerWorkspace) {
      LOG_ERR("EBP", "OOM: PXC2 workspace (%u bytes)", unsigned(sizeof(PxcV2Workspace)));
      return false;
    }
    FsFile output;
    if (!Storage.openFileForWrite("EBP", temp, output)) return false;
    PxcV2 decoder(*optimizerWorkspace, output, entry, expectedWidth, expectedHeight);
    bool ok = ZipFile(filepath).readStoredFileToStream(entry.pxcHref, decoder) && decoder.finish() &&
              output.size() == pxcByteCount(expectedWidth, expectedHeight);
    ok = OptimizerFormat::finishCache(output, Storage, ok, temp.c_str(), destPxcPath.c_str(), backup.c_str());
    if (!ok) {
      LOG_ERR("EBP", "PXC2 materialization failed: %s", entry.pxcHref);
      Storage.remove(temp.c_str());
    }
    if (ok) LOG_DBG("EBP", "Materialized PXC2 image: %s", entry.pxcHref);
    return ok;
  }
  if (entry.width == expectedWidth && entry.height == expectedHeight) {
    // No resize is needed: validate while inflating directly to the publish temp file.
    FsFile output;
    if (!Storage.openFileForWrite("EBP", temp, output)) return false;
    PxcHeaderSink sink(output);
    bool ok = readItemContentsToStream(entry.pxcHref, sink, kOptimizerPxcExtractionChunkSize) &&
              sink.width() == entry.width && sink.height() == entry.height && output.size() == entry.bytes;
    ok = OptimizerFormat::finishCache(output, Storage, ok, temp.c_str(), destPxcPath.c_str(), backup.c_str());
    if (!ok) {
      LOG_ERR("EBP", "PXC extraction failed: %s", entry.pxcHref);
      Storage.remove(temp.c_str());
    }
    return ok;
  }
  const std::string source = destPxcPath + ".optimizer.source";
  Storage.remove(source.c_str());
  if (!extractItemToFile(entry.pxcHref, source, kOptimizerPxcExtractionChunkSize)) return false;
  FsFile verify;
  bool ok = Storage.openFileForRead("EBP", source, verify) && readPxcHeader(verify, w, h) && w == entry.width &&
            h == entry.height && verify.size() == entry.bytes;
  verify.close();
  if (ok) ok = rescalePxcFile(source, destPxcPath, expectedWidth, expectedHeight);
  Storage.remove(source.c_str());
  return ok;
}

bool Epub::ensureOptimizerImageIndex() {
  if (decryptor) return false;
  if (optimizerIndexReady) return true;
  if (optimizerManifestAbsent) return false;
  optimizerLastHit = {};
  const size_t length = strlen(kOptimizerManifestPath);
  ZipFile::EntryTarget target{ZipFile::fnvHash64(kOptimizerManifestPath, length), static_cast<uint16_t>(length), 0,
                              kOptimizerManifestPath};
  ZipFile::EntryIdentity identity;
  if (ZipFile(filepath).fillEntryIdentities(&target, 1, &identity, 1) < 0) return false;
  if (!identity.found) {
    // Only a successful directory scan proves absence. I/O failures remain retryable.
    optimizerManifestAbsent = true;
    return false;
  }
  if (!identity.uncompressedSize || identity.uncompressedSize > kOptimizerManifestMaxBytes) return false;
  const std::string path = cachePath + "/optimizer-images.idx";
  const std::string temp = path + ".tmp";
  const std::string manifest = path + ".manifest.tmp";
  Storage.remove(temp.c_str());
  Storage.remove(manifest.c_str());
  if (validateOptimizerIndex(path, identity, optimizerIndexCount)) {
    optimizerIndexReady = true;
    return true;
  }
  size_t packagedSize = 0;
  bool ok = false;
  if (getItemSize(kOptimizerIndexPath, &packagedSize)) {
    if (packagedSize < 32 || packagedSize > 32 + 208 * 256) return false;
    FsFile output;
    if (!Storage.openFileForWrite("EBP", temp, output)) return false;
    ok = ZipFile(filepath).readStoredFileToStream(kOptimizerIndexPath, output) && output.sync();
    ok = output.close() && ok;
  } else {
    // Caller loans the framebuffer for legacy ZIP inflation. JSON is temporary;
    // no manifest or record array survives reader setup.
    if (!extractItemToFile(kOptimizerManifestPath, manifest, 256)) return false;
    FsFile input;
    if (!Storage.openFileForRead("EBP", manifest, input)) {
      Storage.remove(manifest.c_str());
      return false;
    }
    // Bind the legacy JSON bytes to the central-directory identity as well.
    uint32_t manifestCrc = 0;
    uint8_t manifestChunk[256];
    bool manifestOk = input.size() == identity.uncompressedSize;
    size_t remaining = identity.uncompressedSize;
    while (manifestOk && remaining) {
      const size_t n = std::min(remaining, sizeof(manifestChunk));
      manifestOk = input.read(manifestChunk, n) == static_cast<int>(n);
      if (manifestOk) manifestCrc = OptimizerFormat::crc(manifestCrc, manifestChunk, n);
      remaining -= n;
    }
    if (!manifestOk || manifestCrc != identity.crc32 || !input.seek(0)) {
      LOG_ERR("EBP", "Invalid optimizer manifest CRC/size");
      input.close();
      Storage.remove(manifest.c_str());
      return false;
    }
    JsonDocument filter;
    buildOptimizerManifestJsonFilter(filter);
    JsonDocument doc;
    const auto error = deserializeJson(doc, input, DeserializationOption::Filter(filter.as<JsonVariantConst>()));
    input.close();
    Storage.remove(manifest.c_str());
    JsonArrayConst images = doc["images"];
    if (error || strcmp(doc["format"] | "", kOptimizerManifestFormat) || (doc["version"] | 0) != 1 || images.isNull() ||
        images.size() > 256)
      return false;
    auto scratch = makeUniqueNoThrow<IndexScratch>();
    if (!scratch) {
      LOG_ERR("EBP", "OOM building optimizer index");
      return false;
    }
    FsFile output;
    if (!Storage.openFileForWrite("EBP", temp, output)) return false;
    memset(scratch->bytes, 0, 32);
    ok = output.write(scratch->bytes, 32) == 32;
    uint16_t count = 0;
    uint32_t recordsCrc = 0;
    for (JsonObjectConst image : images) {
      if (!ok) break;
      const char *href = nullptr, *pxc = nullptr;
      auto& record = scratch->record;
      record = {};
      ok = readOptimizerImageMetadata(image, 128, 64, href, pxc, record.width, record.height);
      if (!ok) break;
      memcpy(record.href, href, strlen(href) + 1);
      memcpy(record.pxcHref, pxc, strlen(pxc) + 1);
      const char* format = image["pxcFormat"] | "pxc1";
      record.format = !strcmp(format, "pxc2") ? 2 : !strcmp(format, "pxc1") ? 1 : 0;
      record.bytes = record.format == 1 ? pxcByteCount(record.width, record.height) : (image["pxcBytes"] | 0U);
      record.pixelCrc = image["pixelCrc32"] | 0U;
      ok = OptimizerFormat::valid(record);
      if (!ok) break;
      OptimizerFormat::encodeRecord(record, scratch->bytes);
      recordsCrc = OptimizerFormat::crc(recordsCrc, scratch->bytes, 208);
      ok = output.write(scratch->bytes, 208) == 208;
      ++count;
    }
    OptimizerFormat::indexHeader(scratch->bytes, count, identity.crc32, identity.uncompressedSize, recordsCrc);
    ok = ok && output.seek(0) && output.write(scratch->bytes, 32) == 32 && output.sync();
    ok = output.close() && ok;
  }
  ok = ok && validateOptimizerIndex(temp, identity, optimizerIndexCount);
  if (ok) {
    if (Storage.exists(path.c_str())) Storage.remove(path.c_str());
    ok = Storage.rename(temp.c_str(), path.c_str());
  }
  if (!ok) {
    LOG_ERR("EBP", "Failed to build optimizer image index");
    Storage.remove(temp.c_str());
  }
  optimizerIndexReady = ok;
  if (ok) LOG_INF("EBP", "COIX: %u records", optimizerIndexCount);
  return ok;
}

bool Epub::loadXLocations() {
  locationSpine.reset();
  locationSpineCount = 0;
  locationChapterGroups.reset();
  locationChapterGroupCount = 0;
  sourceSpineMap.reset();
  sourceChildRanges.reset();
  sourceSpineMapCount = 0;
  sourceChildRangeCount = 0;
  sourceSpineCount = 0;
  sourceSpineMapDeclared = false;
  totalLocations = 0;
  totalWords = 0;
  wordsPerReferencePage = 0;
  totalReferencePages = 0;
  referencePagesUseCharacters = false;
  xLocationsLoaded = false;

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return false;
  }

  const int spineCount = getSpineItemsCount();
  if (spineCount <= 0) {
    return false;
  }

  size_t manifestSize = 0;
  if (!getItemSize(kXLocationsPath, &manifestSize)) {
    return false;
  }
  if (manifestSize == 0 || manifestSize > kXLocationsMaxBytes) {
    LOG_ERR("EBP", "Ignoring X locations manifest with unsupported size: %zu bytes", manifestSize);
    return false;
  }

  size_t bytesRead = 0;
  uint8_t* manifestData = readItemContentsToBytes(kXLocationsPath, &bytesRead, true);
  if (!manifestData) {
    LOG_ERR("EBP", "Failed to read X locations manifest");
    return false;
  }

  JsonDocument filter;
  buildXLocationsJsonFilter(filter);
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, reinterpret_cast<const char*>(manifestData), bytesRead,
                                                   DeserializationOption::Filter(filter.as<JsonVariantConst>()));
  free(manifestData);

  if (err) {
    LOG_ERR("EBP", "X locations parse error: %s", err.c_str());
    return false;
  }

  const char* format = doc["format"] | "";
  const int version = doc["version"] | 0;
  const uint32_t parsedTotalLocations = doc["totalLocations"] | 0;
  const uint32_t parsedTotalWords = doc["totalWords"] | 0;
  const uint32_t parsedTotalCharacters = doc["totalCharacters"] | 0;
  const uint32_t parsedWordsPerReferencePage = doc["wordsPerReferencePage"] | 0;
  const uint32_t parsedCharactersPerReferencePage = doc["charactersPerReferencePage"] | 0;
  const bool useCharacterReferencePages = parsedTotalCharacters > 0 && parsedCharactersPerReferencePage > 0;
  const uint32_t parsedReferenceUnits = useCharacterReferencePages ? parsedTotalCharacters : parsedTotalWords;
  const uint32_t parsedReferenceUnitsPerPage =
      useCharacterReferencePages ? parsedCharactersPerReferencePage : parsedWordsPerReferencePage;
  const uint32_t parsedTotalReferencePages = doc["totalReferencePages"] | 0;
  JsonArrayConst spine = doc["spine"];
  JsonArrayConst chapterGroups = doc["chapterGroups"];
  JsonObjectConst parsedSourceMap = doc["sourceSpineMap"];

  if (!isSupportedLocationsFormat(format) || version != 1 || parsedTotalLocations == 0 || spine.isNull()) {
    LOG_ERR("EBP", "Ignoring unsupported X locations manifest");
    return false;
  }

  auto parsedSpine = makeUniqueNoThrow<LocationSpineEntry[]>(static_cast<size_t>(spineCount));
  if (!parsedSpine) {
    LOG_ERR("EBP", "OOM: X location spine (%zu bytes)", static_cast<size_t>(spineCount) * sizeof(LocationSpineEntry));
    return false;
  }
  bool hasValidEntry = false;
  size_t ordinal = 0;
  for (JsonObjectConst spineItem : spine) {
    const int index = spineItem["index"] | static_cast<int>(ordinal);
    ordinal++;
    if (index < 0 || index >= spineCount) {
      continue;
    }

    const uint32_t startLocation = spineItem["startLocation"] | 0;
    const uint32_t endLocation = spineItem["endLocation"] | 0;
    const uint32_t wordStart =
        useCharacterReferencePages ? (spineItem["characterStart"] | 0) : (spineItem["wordStart"] | 0);
    const uint32_t wordCount =
        useCharacterReferencePages ? (spineItem["characterCount"] | 0) : (spineItem["wordCount"] | 0);
    const int chapterGroup = spineItem["chapterGroup"] | -1;
    const uint16_t parsedChapterGroup =
        chapterGroup >= 0 && chapterGroup <= UINT16_MAX ? static_cast<uint16_t>(chapterGroup) : UINT16_MAX;
    parsedSpine[static_cast<size_t>(index)].chapterGroup = parsedChapterGroup;
    if (startLocation == 0 && endLocation == 0) {
      continue;
    }
    if (startLocation == 0 || endLocation < startLocation || endLocation > parsedTotalLocations) {
      LOG_ERR("EBP", "Ignoring invalid X location range at spine %d", index);
      continue;
    }

    parsedSpine[static_cast<size_t>(index)] = {startLocation, endLocation, wordStart, wordCount, parsedChapterGroup};
    hasValidEntry = true;
  }

  if (!hasValidEntry) {
    return false;
  }

  locationSpine = std::move(parsedSpine);
  locationSpineCount = static_cast<size_t>(spineCount);

  totalLocations = parsedTotalLocations;
  totalWords = parsedReferenceUnits;
  wordsPerReferencePage =
      parsedReferenceUnitsPerPage > 0 ? parsedReferenceUnitsPerPage : kDefaultReferenceCharactersPerPage;
  referencePagesUseCharacters = useCharacterReferencePages;
  totalReferencePages = parsedTotalReferencePages;
  if (totalReferencePages == 0 && totalWords > 0 && wordsPerReferencePage > 0) {
    totalReferencePages = (totalWords + wordsPerReferencePage - 1) / wordsPerReferencePage;
  }

  if (!chapterGroups.isNull() && chapterGroups.size() > 0 && chapterGroups.size() <= static_cast<size_t>(spineCount)) {
    const size_t groupCount = chapterGroups.size();
    auto parsedGroups = makeUniqueNoThrow<LocationChapterGroupEntry[]>(groupCount);
    if (!parsedGroups) {
      LOG_ERR("EBP", "OOM: chapter groups (%zu bytes)", groupCount * sizeof(LocationChapterGroupEntry));
    } else {
      size_t ordinalGroup = 0;
      for (JsonObjectConst group : chapterGroups) {
        const int index = group["index"] | static_cast<int>(ordinalGroup);
        ordinalGroup++;
        if (index < 0 || index >= static_cast<int>(groupCount)) continue;
        int firstSpineIndex = group["firstSpineIndex"] | -1;
        int lastSpineIndex = group["lastSpineIndex"] | -1;
        if (firstSpineIndex < 0) firstSpineIndex = group["startSpineIndex"] | -1;
        if (lastSpineIndex < 0) lastSpineIndex = group["endSpineIndex"] | -1;
        if (firstSpineIndex < 0 || lastSpineIndex < firstSpineIndex || lastSpineIndex >= spineCount) continue;
        parsedGroups[static_cast<size_t>(index)] = {static_cast<uint16_t>(firstSpineIndex),
                                                    static_cast<uint16_t>(lastSpineIndex), true};
      }
      for (size_t i = 0; i < locationSpineCount; i++) {
        auto& entry = locationSpine[i];
        if (entry.chapterGroup >= groupCount || !parsedGroups[entry.chapterGroup].valid) {
          entry.chapterGroup = UINT16_MAX;
        }
      }
      locationChapterGroups = std::move(parsedGroups);
      locationChapterGroupCount = groupCount;
    }
  }

  if (!parsedSourceMap.isNull()) {
    sourceSpineMapDeclared = true;
    const int mapVersion = parsedSourceMap["version"] | 0;
    const int parsedSourceSpineCount = parsedSourceMap["spineCount"] | 0;
    JsonArrayConst mappedSpine = parsedSourceMap["spine"];
    bool mapValid = mapVersion == 1 && parsedSourceSpineCount > 0 && parsedSourceSpineCount <= UINT16_MAX &&
                    !mappedSpine.isNull() && mappedSpine.size() == static_cast<size_t>(spineCount);
    size_t totalRanges = 0;
    if (mapValid) {
      for (JsonObjectConst mappedEntry : mappedSpine) {
        JsonArrayConst childRanges = mappedEntry["childRanges"];
        if (!childRanges.isNull()) totalRanges += childRanges.size();
        if (totalRanges > static_cast<size_t>(spineCount) * 16U || totalRanges > UINT16_MAX) {
          mapValid = false;
          break;
        }
      }
    }

    auto parsedEntries = mapValid ? makeUniqueNoThrow<SourceSpineMapEntry[]>(static_cast<size_t>(spineCount)) : nullptr;
    auto parsedRanges = mapValid && totalRanges > 0 ? makeUniqueNoThrow<SourceChildRange[]>(totalRanges) : nullptr;
    if (mapValid && (!parsedEntries || (totalRanges > 0 && !parsedRanges))) {
      LOG_ERR("EBP", "OOM: source spine map (%zu bytes)",
              static_cast<size_t>(spineCount) * sizeof(SourceSpineMapEntry) + totalRanges * sizeof(SourceChildRange));
      mapValid = false;
    }

    if (mapValid) {
      size_t nextRange = 0;
      auto seen = makeUniqueNoThrow<bool[]>(static_cast<size_t>(spineCount));
      if (!seen) {
        LOG_ERR("EBP", "OOM: source spine validation (%d bytes)", spineCount);
        mapValid = false;
      } else {
        size_t ordinalEntry = 0;
        for (JsonObjectConst mappedEntry : mappedSpine) {
          const int index = mappedEntry["index"] | static_cast<int>(ordinalEntry);
          ordinalEntry++;
          const int sourceIndex = mappedEntry["sourceSpineIndex"] | -1;
          const int containerDepth = mappedEntry["containerDepth"] | 0;
          JsonArrayConst childRanges = mappedEntry["childRanges"];
          const size_t rangeCount = childRanges.isNull() ? 0 : childRanges.size();
          if (index < 0 || index >= spineCount || seen[static_cast<size_t>(index)] || sourceIndex < 0 ||
              sourceIndex >= parsedSourceSpineCount || containerDepth < 0 || containerDepth >= 16 ||
              rangeCount > UINT8_MAX) {
            mapValid = false;
            break;
          }
          seen[static_cast<size_t>(index)] = true;
          SourceSpineMapEntry& target = parsedEntries[static_cast<size_t>(index)];
          target.sourceSpineIndex = static_cast<uint16_t>(sourceIndex);
          target.firstRange = static_cast<uint16_t>(nextRange);
          target.rangeCount = static_cast<uint8_t>(rangeCount);
          target.containerDepth = static_cast<uint8_t>(containerDepth);
          for (JsonObjectConst childRange : childRanges) {
            const char* name = childRange["name"] | "";
            const int offset = childRange["offset"] | -1;
            const int count = childRange["count"] | 0;
            const size_t nameLen = std::strlen(name);
            if (nameLen == 0 || nameLen >= sizeof(SourceChildRange::name) || offset < 0 || offset > UINT16_MAX ||
                count <= 0 || count > UINT16_MAX || static_cast<uint32_t>(offset) + count > UINT16_MAX) {
              mapValid = false;
              break;
            }
            SourceChildRange& range = parsedRanges[nextRange++];
            std::memcpy(range.name, name, nameLen + 1);
            range.offset = static_cast<uint16_t>(offset);
            range.count = static_cast<uint16_t>(count);
          }
          if (!mapValid) break;
        }
      }
    }

    if (mapValid) {
      sourceSpineMap = std::move(parsedEntries);
      sourceChildRanges = std::move(parsedRanges);
      sourceSpineMapCount = static_cast<size_t>(spineCount);
      sourceChildRangeCount = totalRanges;
      sourceSpineCount = static_cast<uint16_t>(parsedSourceSpineCount);
    } else {
      LOG_ERR("EBP", "Ignoring malformed source spine map");
    }
  }
  xLocationsLoaded = true;
  LOG_INF("EBP", "Loaded X locations: %lu locations, %lu reference pages across %zu spine items",
          static_cast<unsigned long>(totalLocations), static_cast<unsigned long>(totalReferencePages),
          locationSpineCount);
  return true;
}

bool Epub::resolveChapterGroupRange(const int currentSpineIndex, int& firstSpineIndex, int& lastSpineIndex) const {
  firstSpineIndex = currentSpineIndex;
  lastSpineIndex = currentSpineIndex;
  if (currentSpineIndex < 0 || currentSpineIndex >= static_cast<int>(locationSpineCount) || !locationChapterGroups) {
    return false;
  }
  const uint16_t groupIndex = locationSpine[static_cast<size_t>(currentSpineIndex)].chapterGroup;
  if (groupIndex >= locationChapterGroupCount || !locationChapterGroups[groupIndex].valid) return false;
  const auto& group = locationChapterGroups[groupIndex];
  if (currentSpineIndex < group.firstSpineIndex || currentSpineIndex > group.lastSpineIndex) return false;
  firstSpineIndex = group.firstSpineIndex;
  lastSpineIndex = group.lastSpineIndex;
  return true;
}

bool Epub::getSourceSpineMapEntry(const int currentSpineIndex, SourceSpineMapEntry& entry) const {
  if (!sourceSpineMap || currentSpineIndex < 0 || currentSpineIndex >= static_cast<int>(sourceSpineMapCount)) {
    return false;
  }
  entry = sourceSpineMap[static_cast<size_t>(currentSpineIndex)];
  return entry.sourceSpineIndex != UINT16_MAX;
}

const Epub::SourceChildRange* Epub::getSourceChildRange(const SourceSpineMapEntry& entry, const size_t ordinal) const {
  if (!sourceChildRanges || ordinal >= entry.rangeCount) return nullptr;
  const size_t index = static_cast<size_t>(entry.firstRange) + ordinal;
  return index < sourceChildRangeCount ? &sourceChildRanges[index] : nullptr;
}

bool Epub::findCurrentSpineForSource(const int sourceIndex, const uint8_t containerDepth, const char* childName,
                                     const uint16_t sourceSiblingIndex, int& currentSpineIndex,
                                     uint16_t& currentSiblingIndex) const {
  if (!sourceSpineMap || sourceIndex < 0 || sourceIndex >= sourceSpineCount) return false;
  for (size_t i = 0; i < sourceSpineMapCount; ++i) {
    const SourceSpineMapEntry& entry = sourceSpineMap[i];
    if (entry.sourceSpineIndex != sourceIndex) continue;
    if (!childName || entry.rangeCount == 0) {
      currentSpineIndex = static_cast<int>(i);
      currentSiblingIndex = sourceSiblingIndex;
      return true;
    }
    if (entry.containerDepth != containerDepth) continue;
    for (size_t rangeIndex = 0; rangeIndex < entry.rangeCount; ++rangeIndex) {
      const SourceChildRange* range = getSourceChildRange(entry, rangeIndex);
      if (!range || strcasecmp(range->name, childName) != 0) continue;
      const uint32_t first = static_cast<uint32_t>(range->offset) + 1U;
      const uint32_t last = static_cast<uint32_t>(range->offset) + range->count;
      if (sourceSiblingIndex >= first && sourceSiblingIndex <= last) {
        currentSpineIndex = static_cast<int>(i);
        currentSiblingIndex = static_cast<uint16_t>(sourceSiblingIndex - range->offset);
        return true;
      }
    }
  }
  return false;
}

int Epub::getSpineItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }
  return bookMetadataCache->getSpineCount();
}

size_t Epub::getCumulativeSpineItemSize(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getCumulativeSpineItemSize called but cache not loaded");
    return 0;
  }

  return bookMetadataCache->getSpineCumulativeSize(spineIndex);
}

BookMetadataCache::SpineEntry Epub::getSpineItem(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineItem called but cache not loaded");
    return {};
  }

  if (spineIndex < 0 || spineIndex >= bookMetadataCache->getSpineCount()) {
    LOG_ERR("EBP", "getSpineItem index:%d is out of range", spineIndex);
    return bookMetadataCache->getSpineEntry(0);
  }

  return bookMetadataCache->getSpineEntry(spineIndex);
}

BookMetadataCache::TocEntry Epub::getTocItem(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_DBG("EBP", "getTocItem called but cache not loaded");
    return {};
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_DBG("EBP", "getTocItem index:%d is out of range", tocIndex);
    return {};
  }

  return bookMetadataCache->getTocEntry(tocIndex);
}

int Epub::getTocItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }

  return bookMetadataCache->getTocCount();
}

bool Epub::isNavigationDocumentSpine(const int spineIndex, bool* const scanSucceeded) const {
  if (scanSucceeded) *scanSucceeded = false;
  if (spineIndex < 0 || spineIndex >= getSpineItemsCount()) {
    return false;
  }
  const std::string& href = getSpineItem(spineIndex).href;
  if (href == tocNavItem || href == tocGuideItem) {
    if (scanSucceeded) *scanSucceeded = true;
    return true;
  }

  ContentsDocumentScanner scanner;
  if (!readItemContentsToStream(href, scanner, 512, /*allowEarlyStop=*/true)) {
    return false;
  }
  if (scanSucceeded) *scanSucceeded = true;
  return scanner.isContentsDocument();
}

// work out the section index for a toc index
int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex called but cache not loaded");
    return 0;
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex: tocIndex %d out of range", tocIndex);
    return 0;
  }

  const int spineIndex = bookMetadataCache->getTocEntry(tocIndex).spineIndex;
  if (spineIndex < 0) {
    LOG_DBG("EBP", "Section not found for TOC index %d", tocIndex);
    return 0;
  }

  return spineIndex;
}

int Epub::getTocIndexForSpineIndex(const int spineIndex) const { return getSpineItem(spineIndex).tocIndex; }

size_t Epub::getBookSize() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || bookMetadataCache->getSpineCount() == 0) {
    return 0;
  }
  return getCumulativeSpineItemSize(getSpineItemsCount() - 1);
}

int Epub::getSpineIndexForTextReference() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTextReference called but cache not loaded");
    return 0;
  }

  if (bookMetadataCache->coreMetadata.textReferenceHref.empty()) {
    // there was no textReference in epub, so we return 0 (the first chapter)
    return 0;
  }

  // loop through spine items to get the correct index matching the text href
  for (size_t i = 0; i < getSpineItemsCount(); i++) {
    if (getSpineItem(i).href == bookMetadataCache->coreMetadata.textReferenceHref) {
      return i;
    }
  }
  // This should not happen, as we checked for empty textReferenceHref earlier
  LOG_DBG("EBP", "Section not found for text reference");
  return 0;
}

float Epub::calculateSizeProgress(const int currentSpineIndex, const float currentSpineRead) const {
  const size_t bookSize = getBookSize();
  if (bookSize == 0) {
    return 0.0f;
  }
  const size_t prevChapterSize = (currentSpineIndex >= 1) ? getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t curChapterSize = getCumulativeSpineItemSize(currentSpineIndex) - prevChapterSize;
  const float sectionProgSize = clampUnit(currentSpineRead) * static_cast<float>(curChapterSize);
  const float totalProgress = static_cast<float>(prevChapterSize) + sectionProgSize;
  return totalProgress / static_cast<float>(bookSize);
}

// Calculate progress in book (returns 0.0-1.0)
float Epub::calculateProgress(const int currentSpineIndex, const float currentSpineRead) const {
  if (!xLocationsLoaded || totalLocations == 0 || currentSpineIndex < 0 ||
      currentSpineIndex >= static_cast<int>(locationSpineCount)) {
    return calculateSizeProgress(currentSpineIndex, currentSpineRead);
  }

  const LocationSpineEntry& entry = locationSpine[static_cast<size_t>(currentSpineIndex)];
  if (entry.startLocation == 0 || entry.endLocation < entry.startLocation) {
    return calculateSizeProgress(currentSpineIndex, currentSpineRead);
  }

  const uint32_t locationCount = entry.endLocation - entry.startLocation + 1;
  const float completedBeforeSpine = static_cast<float>(entry.startLocation - 1);
  const float completedInSpine = clampUnit(currentSpineRead) * static_cast<float>(locationCount);
  return clampUnit((completedBeforeSpine + completedInSpine) / static_cast<float>(totalLocations));
}

bool Epub::resolveLocationPercentToSpineProgress(const float percent, int& spineIndex, float& spineProgress) const {
  if (!xLocationsLoaded || totalLocations == 0 || locationSpineCount == 0) {
    return false;
  }

  const float clampedPercent = std::max(0.0f, std::min(100.0f, percent));
  if (clampedPercent <= 0.0f) {
    spineIndex = 0;
    spineProgress = 0.0f;
    return true;
  }

  if (clampedPercent >= 100.0f) {
    for (int i = static_cast<int>(locationSpineCount) - 1; i >= 0; i--) {
      const LocationSpineEntry& entry = locationSpine[static_cast<size_t>(i)];
      if (entry.startLocation > 0 && entry.endLocation >= entry.startLocation) {
        spineIndex = i;
        spineProgress = 1.0f;
        return true;
      }
    }
    return false;
  }

  const float targetCompletedLocations = static_cast<float>(totalLocations) * clampedPercent / 100.0f;
  for (size_t i = 0; i < locationSpineCount; i++) {
    const LocationSpineEntry& entry = locationSpine[i];
    if (entry.startLocation == 0 || entry.endLocation < entry.startLocation) {
      continue;
    }

    const uint32_t locationCount = entry.endLocation - entry.startLocation + 1;
    const float completedBeforeSpine = static_cast<float>(entry.startLocation - 1);
    const float completedThroughSpine = static_cast<float>(entry.endLocation);
    if (targetCompletedLocations > completedThroughSpine) {
      continue;
    }

    spineIndex = static_cast<int>(i);
    spineProgress = clampUnit((targetCompletedLocations - completedBeforeSpine) / static_cast<float>(locationCount));
    return true;
  }

  return false;
}

bool Epub::resolveReferencePage(const int currentSpineIndex, const float currentSpineRead, uint32_t& currentPage,
                                uint32_t& pageCount) const {
  currentPage = 0;
  pageCount = 0;
  if (!xLocationsLoaded || totalWords == 0 || wordsPerReferencePage == 0 || totalReferencePages == 0 ||
      currentSpineIndex < 0 || currentSpineIndex >= static_cast<int>(locationSpineCount)) {
    return false;
  }

  const LocationSpineEntry& entry = locationSpine[static_cast<size_t>(currentSpineIndex)];
  if (entry.wordCount == 0 || entry.wordStart >= totalWords) {
    return false;
  }

  const float clampedProgress = clampUnit(currentSpineRead);
  const uint32_t completedWords =
      entry.wordStart + static_cast<uint32_t>(clampedProgress * static_cast<float>(entry.wordCount));
  currentPage = std::min<uint32_t>(completedWords / wordsPerReferencePage + 1, totalReferencePages);
  pageCount = totalReferencePages;
  return true;
}

bool Epub::hasStablePageNumbers() const {
  return xLocationsLoaded && wordsPerReferencePage > 0 && totalReferencePages > 0 &&
         EpubNavigation::hasResolvableReferencePageRanges(totalWords, locationSpine.get(), locationSpineCount);
}

bool Epub::resolveReferenceTextProgress(const int spineIndex, const uint32_t textOffset,
                                        const uint32_t sectionTextLength, double& completedUnits,
                                        double& totalUnits) const {
  completedUnits = totalUnits = 0;
  if (!hasStablePageNumbers() || spineIndex < 0 || spineIndex >= static_cast<int>(locationSpineCount)) return false;
  const auto& entry = locationSpine[spineIndex];
  if (entry.wordCount == 0 || entry.startLocation == 0 || entry.endLocation < entry.startLocation) return false;
  // Older manifests count words. Their complete section's normalized text
  // length supplies the conversion; a partial section cannot invent that total.
  if (!EpubNavigation::resolveReferenceTextProgress(entry.wordStart, entry.wordCount, totalWords, textOffset,
                                                    sectionTextLength, referencePagesUseCharacters, completedUnits))
    return false;
  totalUnits = totalWords;
  return true;
}

bool Epub::resolveReferencePageToSpineProgress(const uint32_t page, int& spineIndex, float& spineProgress) const {
  if (!hasStablePageNumbers()) return false;
  return EpubNavigation::resolveReferencePageToSpineProgress(page, totalReferencePages, totalWords,
                                                             wordsPerReferencePage, locationSpine.get(),
                                                             locationSpineCount, spineIndex, spineProgress);
}

bool Epub::resolveReferencePageTarget(const uint32_t page, int& spineIndex, float& spineProgress,
                                      uint32_t& spineUnitOffset, uint32_t& spineUnitCount, bool& usesCharacters) const {
  if (!hasStablePageNumbers()) return false;
  const bool resolved = EpubNavigation::resolveReferencePageToSpineProgress(
      page, totalReferencePages, totalWords, wordsPerReferencePage, locationSpine.get(), locationSpineCount, spineIndex,
      spineProgress, &spineUnitOffset, &spineUnitCount);
  usesCharacters = referencePagesUseCharacters;
  return resolved;
}

int Epub::resolveHrefToSpineIndex(const std::string& href) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return -1;

  // Split before decoding so escaped '#' characters in filenames stay part of the path.
  const size_t hashPos = href.find('#');
  const std::string rawTarget = hashPos != std::string::npos ? href.substr(0, hashPos) : href;
  const std::string target = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(rawTarget));

  // Same-file reference (anchor-only)
  if (target.empty()) return -1;

  // Extract just the filename for comparison
  size_t targetSlash = target.find_last_of('/');
  std::string targetFilename = (targetSlash != std::string::npos) ? target.substr(targetSlash + 1) : target;

  for (int i = 0; i < getSpineItemsCount(); i++) {
    const auto& spineHref = getSpineItem(i).href;
    // Try exact match first
    if (spineHref == target) return i;
    // Then filename-only match
    size_t spineSlash = spineHref.find_last_of('/');
    std::string spineFilename = (spineSlash != std::string::npos) ? spineHref.substr(spineSlash + 1) : spineHref;
    if (spineFilename == targetFilename) return i;
  }
  return -1;
}
