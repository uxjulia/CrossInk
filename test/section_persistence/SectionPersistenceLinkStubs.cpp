#include <ComplexShaper.h>
#include <Epub.h>
#include <Epub/image/OptimizerFormat.h>
#include <GfxRenderer.h>
#include <PrintSerialization.h>
#include <Serialization.h>

#include <string>
#include <vector>

#include "Epub/Page.h"
#include "Epub/hyphenation/Hyphenator.h"
#include "Epub/parsers/ChapterHtmlSlimParser.h"

Epub::Epub(std::string path, const std::string& cacheDir) : filepath(std::move(path)), cachePath(cacheDir) {}

const std::string& Epub::getCachePath() const { return cachePath; }
const std::string& Epub::getLanguage() const {
  static const std::string language = "en";
  return language;
}
bool Epub::ensureCssCache() { return true; }
std::string testSpineHref;
bool testStreamHtml = false;
std::string testStreamHtmlContents;
std::string testOriginalItemContents = "<html><body>chapter</body></html>";
bool testParserBeginParse = true;
bool testParserEmitPage = false;
bool testParserPageEmitted = false;
int testParserEmitPageCount = 1;
int testParserPagesEmitted = 0;
std::vector<std::pair<std::string, uint16_t>> testParserAnchors;
std::string testParserReadPath;
ChapterHtmlSlimParser::ParseStatus testParserStatus = ChapterHtmlSlimParser::ParseStatus::Done;
int testParserCalls = 0;
bool testParserMalformedMarkupTruncated = false;
uint32_t testMalformedInputCrcLength = 0;
uint32_t testMalformedInputCrc = 0;
void (*testBeforeParserPage)() = nullptr;

BookMetadataCache::SpineEntry Epub::getSpineItem(int) const {
  BookMetadataCache::SpineEntry entry;
  entry.href = testSpineHref;
  return entry;
}
std::vector<BookMetadataCache::TocEntry> testTocEntries;
BookMetadataCache::TocEntry Epub::getTocItem(int i) const { return testTocEntries.at(i); }
int Epub::getTocItemsCount() const { return testTocEntries.size(); }
int Epub::getTocIndexForSpineIndex(int spine) const {
  for (size_t i = 0; i < testTocEntries.size(); ++i)
    if (testTocEntries[i].spineIndex == spine) return i;
  return -1;
}
bool Epub::readItemContentsToStream(const std::string&, Print& output, size_t, bool) const {
  if (!testStreamHtml) return false;
  const auto* bytes = reinterpret_cast<const uint8_t*>(testStreamHtmlContents.data());
  return output.write(bytes, testStreamHtmlContents.size()) == testStreamHtmlContents.size();
}
bool Epub::getItemIdentity(const std::string&, ItemIdentity& identity) const {
  identity = {};
  identity.uncompressedSize = static_cast<uint32_t>(testOriginalItemContents.size());
  identity.crc32 = OptimizerFormat::crc(0, reinterpret_cast<const uint8_t*>(testOriginalItemContents.data()),
                                        testOriginalItemContents.size());
  return true;
}

uint32_t testHyphenationIdentity = 1;
uint32_t Hyphenator::patternIdentity(const std::string&) { return testHyphenationIdentity; }

void Hyphenator::setPreferredLanguage(const std::string&) {}

ChapterHtmlSlimParser::~ChapterHtmlSlimParser() = default;
bool ChapterHtmlSlimParser::prewarmSectionAdvanceStep() { return false; }
bool ChapterHtmlSlimParser::beginParse() {
  testParserPageEmitted = false;
  testParserPagesEmitted = 0;
  testParserCalls = 0;
  testParserMalformedMarkupTruncated = false;
  testMalformedInputCrcLength = 0;
  testMalformedInputCrc = 0;
  anchorData.assign(testParserAnchors.begin(), testParserAnchors.end());
  return testParserBeginParse;
}
ChapterHtmlSlimParser::ParseStatus ChapterHtmlSlimParser::parseStep() {
  ++testParserCalls;
  if (!testParserReadPath.empty()) {
    HalFile input;
    if (!Storage.openFileForRead("TEST", testParserReadPath, input)) return ParseStatus::Error;
    uint8_t buffer[16];
    const int read = input.read(buffer, sizeof(buffer));
    input.close();
    if (read < 0) return ParseStatus::Error;
    if (read > 0 && buffer[0] != '<') {
      testParserMalformedMarkupTruncated = true;
      testMalformedInputCrcLength = static_cast<uint32_t>(read);
      testMalformedInputCrc = OptimizerFormat::crc(0, buffer, static_cast<size_t>(read));
    }
  }
  while (testParserEmitPage && testParserPagesEmitted < testParserEmitPageCount) {
    ++testParserPagesEmitted;
    testParserPageEmitted = testParserPagesEmitted >= testParserEmitPageCount;
    if (testBeforeParserPage) testBeforeParserPage();
    const auto page = static_cast<uint16_t>(testParserPagesEmitted - 1);
    completePageFn(std::make_unique<Page>(), page, 0, page * 32U, page * 32U);
  }
  return testParserStatus;
}
bool ChapterHtmlSlimParser::finishParse() { return true; }
void ChapterHtmlSlimParser::abortParse() {}
void ChapterHtmlSlimParser::releaseInputFile() {}
bool ChapterHtmlSlimParser::wasMalformedMarkupTruncated() const { return testParserMalformedMarkupTruncated; }
uint32_t ChapterHtmlSlimParser::malformedInputCrcLength() const { return testMalformedInputCrcLength; }
uint32_t ChapterHtmlSlimParser::malformedInputCrc() const { return testMalformedInputCrc; }

bool Page::serialize(Print& file) const {
  constexpr uint32_t marker = 0x50414745;
  return serialization::tryWritePod(file, marker);
}

std::unique_ptr<Page> Page::deserialize(FsFile& file) {
  uint32_t marker = 0;
  if (!serialization::tryReadPod(file, marker) || marker != 0x50414745) return nullptr;
  return std::make_unique<Page>();
}

uint16_t Page::imageEstimateUnits(uint16_t, uint16_t) const { return 0; }

void ComplexShaper::setDocumentLanguage(const char*) {}

bool testShapingProbeReady = true;
bool ComplexShaper::probeRecovery(const shaping_recovery::Failures&) { return testShapingProbeReady; }
ComplexShaper::LayoutObservation::LayoutObservation(shaping_recovery::Failures&) : previous_(nullptr) {}
ComplexShaper::LayoutObservation::~LayoutObservation() = default;

bool Page::hasRecoverableUnshapedText(const GfxRenderer&, int) const { return false; }
