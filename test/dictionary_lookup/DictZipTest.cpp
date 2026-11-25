#include <DictZip.h>
#include <Dictionary.h>
#include <InflateReader.h>
#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <string>

namespace {
int failAllocationAfter = -1;
size_t largestAllowedAllocation = SIZE_MAX;
}  // namespace
// Exercise production nothrow allocation failures without failing GTest/STL.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  if (failAllocationAfter == 0 || size > largestAllowedAllocation) return nullptr;
  if (failAllocationAfter > 0) --failAllocationAfter;
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  if (failAllocationAfter == 0 || size > largestAllowedAllocation) return nullptr;
  if (failAllocationAfter > 0) --failAllocationAfter;
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

namespace {
using namespace dictionary_test;
void le16(std::string& out, uint16_t n) {
  out.push_back(static_cast<char>(n));
  out.push_back(static_cast<char>(n >> 8));
}
void le32(std::string& out, uint32_t n) {
  le16(out, static_cast<uint16_t>(n));
  le16(out, static_cast<uint16_t>(n >> 16));
}
void be32(std::string& out, uint32_t n) {
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>(n >> shift));
}
// Real dictzip layout: one raw deflate stream, full-flush chunk boundaries,
// an RA size table in the gzip extra header, then CRC32/ISIZE.
std::string dictzip(const std::string& data, uint16_t chunkLength = 60000) {
  z_stream stream{};
  if (deflateInit2(&stream, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return {};
  std::string compressed, lengths;
  std::array<uint8_t, 70000> buffer{};
  for (size_t offset = 0; offset < data.size(); offset += chunkLength) {
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data() + offset));
    stream.avail_in = static_cast<uInt>(std::min<size_t>(chunkLength, data.size() - offset));
    stream.next_out = buffer.data();
    stream.avail_out = buffer.size();
    if (deflate(&stream, Z_FULL_FLUSH) != Z_OK || stream.avail_in != 0) {
      deflateEnd(&stream);
      return {};
    }
    const size_t size = buffer.size() - stream.avail_out;
    le16(lengths, static_cast<uint16_t>(size));
    compressed.append(reinterpret_cast<char*>(buffer.data()), size);
  }
  stream.next_out = buffer.data();
  stream.avail_out = buffer.size();
  if (deflate(&stream, Z_FINISH) != Z_STREAM_END) {
    deflateEnd(&stream);
    return {};
  }
  compressed.append(reinterpret_cast<char*>(buffer.data()), buffer.size() - stream.avail_out);
  deflateEnd(&stream);
  std::string result("\x1f\x8b\x08\x04\0\0\0\0\0\xff", 10);
  le16(result, static_cast<uint16_t>(10 + lengths.size()));
  result += "RA";
  le16(result, static_cast<uint16_t>(6 + lengths.size()));
  le16(result, 1);
  le16(result, chunkLength);
  le16(result, static_cast<uint16_t>(lengths.size() / 2));
  result += lengths;
  result += compressed;
  le32(result, crc32(0, reinterpret_cast<const Bytef*>(data.data()), data.size()));
  le32(result, data.size());
  return result;
}
class DictZipTest : public ::testing::Test {
 protected:
  void SetUp() override {
    files.clear();
    readers.clear();
    duplicateOpens = 0;
    failRead = failWrite = failSync = false;
    failAllocationAfter = -1;
    largestAllowedAllocation = SIZE_MAX;
    Dictionary::setLookupDictPathOverride("/test");
  }
  void TearDown() override {
    failAllocationAfter = -1;
    largestAllowedAllocation = SIZE_MAX;
    Dictionary::clearLookupDictPathOverride();
    EXPECT_EQ(duplicateOpens, 0u);
    for (const auto& [path, count] : readers) EXPECT_EQ(count, 0) << path;
  }
  DictLocation install(const std::string& text, uint32_t offset = 0, uint32_t size = 0) {
    files["/test.dict.dz"] = dictzip(text);
    std::string idx = "word";
    idx.push_back('\0');
    be32(idx, offset);
    be32(idx, size ? size : text.size());
    files["/test.idx"] = idx;
    files["/test.ifo"] = "StarDict's dict ifo file\nsametypesequence=m\n";
    return Dictionary::locate("word");
  }
  bool extract(uint32_t offset, uint32_t size, DictZip::ExtractError* error = nullptr) {
    HalFile output;
    if (!Storage.openFileForWrite("TEST", "/output", output)) return false;
    const bool ok = DictZip::extractEntry("/test.dict.dz", offset, size, output, error);
    output.close();
    return ok;
  }
};
TEST_F(DictZipTest, ReadsSlicesAcrossChunksAndRingBoundaries) {
  std::string text;
  for (int i = 0; i < 16000; ++i) text += "definition " + std::to_string(i % 997) + "\n";
  files["/test.dict.dz"] = dictzip(text);
  for (const auto [offset, size] :
       {std::pair{0u, 1u}, {32760u, 60000u}, {59991u, 120017u}, {static_cast<unsigned>(text.size() - 100), 100u}}) {
    ASSERT_TRUE(extract(offset, size));
    EXPECT_EQ(files["/output"], text.substr(offset, size));
  }
}
TEST_F(DictZipTest, DiscoversValidityAndBuildsQuickIndexForCompressedData) {
  install("a definition");
  EXPECT_TRUE(Dictionary::exists());
  EXPECT_TRUE(Dictionary::readInfo("/test").isCompressed);
  ASSERT_TRUE(Dictionary::prepareQuickIndex());
  EXPECT_TRUE(files.count("/test.qidx"));
  EXPECT_EQ(Dictionary::lookup("word"), "a definition");
  EXPECT_FALSE(files.count("/test.dict"));
  for (const auto& [path, data] : files) EXPECT_EQ(path.find("dict-entry-"), std::string::npos);
}
TEST_F(DictZipTest, SourceRetainsIndependentBackingAndRemovesItOnReset) {
  const auto location = install("<b>Long HTML definition</b>");
  DictionaryDefinitionSource first, second;
  ASSERT_TRUE(first.open(location));
  const auto firstPath = first.path();
  ASSERT_TRUE(second.open(location));
  EXPECT_NE(first.path(), second.path());
  EXPECT_EQ(files[first.path()], files[second.path()]);
  first.reset();
  EXPECT_FALSE(files.count(firstPath));
  EXPECT_TRUE(files.count(second.path()));
}
TEST_F(DictZipTest, PrefersPlainDefinitionAndRetainsOriginalOffsets) {
  auto location = install("xxxxcompressed", 4, 10);
  files["/test.dict"] = "xxxxplain text";
  DictionaryDefinitionSource source;
  ASSERT_TRUE(source.open(location));
  EXPECT_EQ(source.path(), "/test.dict");
  EXPECT_EQ(source.offset(), 4u);
  EXPECT_EQ(Dictionary::lookup("word"), "plain text");
  source.reset();
  EXPECT_TRUE(files.count("/test.dict"));
}
TEST_F(DictZipTest, ResolvesTypedHtmlAndBinaryFieldsInExtractedEntry) {
  std::string entry("P", 1);
  be32(entry, 3);
  entry += "abc";
  entry += "h<p>HTML meaning</p>";
  entry.push_back('\0');
  auto location = install("padding" + entry, 7, entry.size());
  files["/test.ifo"] = "StarDict's dict ifo file\nversion=3.0.0\n";
  DictionaryDefinitionSource source;
  ASSERT_TRUE(source.open(location));
  auto slice = Dictionary::resolveDefinitionSlice(location, Dictionary::readInfo("/test"), source);
  ASSERT_TRUE(slice.found);
  EXPECT_TRUE(slice.isHtml);
  EXPECT_EQ(files[source.path()].substr(slice.offset, slice.size), "<p>HTML meaning</p>");
  EXPECT_EQ(Dictionary::lookup("word"), "<p>HTML meaning</p>");
}
TEST_F(DictZipTest, RejectsOrdinaryGzipMalformedTablesAndOutOfBoundsRanges) {
  install("definition");
  const auto valid = files["/test.dict.dz"];
  DictZip::ExtractError error;
  EXPECT_FALSE(extract(UINT32_MAX, 10, &error));
  EXPECT_EQ(error, DictZip::ExtractError::ReadError);
  for (const size_t cut : {0u, 10u, 15u, 20u}) {
    files["/test.dict.dz"] = valid.substr(0, cut);
    EXPECT_FALSE(extract(0, 3));
  }
  files["/test.dict.dz"] = valid;
  files["/test.dict.dz"][3] = 0;
  EXPECT_FALSE(extract(0, 3, &error));
  EXPECT_EQ(error, DictZip::ExtractError::Decompress);
  files["/test.dict.dz"] = valid;
  files["/test.dict.dz"][24] = static_cast<char>(0xff);
  EXPECT_FALSE(extract(0, 3));
  files["/test.dict.dz"] = valid;
  files["/test.dict.dz"].back() = 1;
  EXPECT_FALSE(extract(0, 3));
}
TEST_F(DictZipTest, FailedReadWriteSyncAndLowMemoryLeaveNoScratchOrHandles) {
  const auto location = install("definition");
  for (int mode = 0; mode < 11; ++mode) {
    DictionaryDefinitionSource source;
    failRead = mode == 0;
    failWrite = mode == 1;
    failSync = mode == 2;
    // table + 4 ring segments + input/state + output
    failAllocationAfter = mode >= 3 ? mode - 3 : -1;
    const bool ok = source.open(location);
    failAllocationAfter = -1;
    if (mode < 10) EXPECT_FALSE(ok) << mode;
    source.reset();
    for (const auto& [path, data] : files) EXPECT_EQ(path.find("dict-entry-"), std::string::npos);
    for (const auto& [path, count] : readers) EXPECT_EQ(count, 0) << path;
  }
}
TEST_F(DictZipTest, FragmentedHeapNeedsNoBlockLargerThanEightKiB) {
  const auto location = install(std::string(160000, 'x'));
  largestAllowedAllocation = 8192;
  DictionaryDefinitionSource source;
  EXPECT_TRUE(source.open(location));
  largestAllowedAllocation = SIZE_MAX;
}
TEST_F(DictZipTest, CorruptCompressedStreamRemovesPartialOutput) {
  const auto location = install(std::string(120000, 'x'));
  auto& compressed = files["/test.dict.dz"];
  // XLEN is at 10, followed by two length entries in the RA header.
  const size_t start = 12 + static_cast<unsigned char>(compressed[10]);
  compressed[start] = static_cast<char>(0x07);  // reserved deflate block type
  DictionaryDefinitionSource source;
  EXPECT_FALSE(source.open(location));
  EXPECT_TRUE(source.path().empty());
  for (const auto& [path, data] : files) EXPECT_EQ(path.find("dict-entry-"), std::string::npos);
}
TEST_F(DictZipTest, AcceptsOptionalGzipFieldsAndRejectsDuplicateRaTables) {
  const std::string text = "definition with optional gzip metadata";
  install(text);
  const auto valid = files["/test.dict.dz"];
  const size_t extraSize = static_cast<uint8_t>(valid[10]) | (static_cast<uint8_t>(valid[11]) << 8);
  const std::string ra = valid.substr(12, extraSize);
  std::string extra("ZZ\1\0x", 5);
  extra += ra;
  extra.append("YY\1\0y", 5);
  std::string header = valid.substr(0, 10);
  header[3] = 0x1e;  // FEXTRA | FNAME | FCOMMENT | FHCRC
  le16(header, extra.size());
  files["/test.dict.dz"] = header + extra + std::string("name\0comment\0\0\0", 15) + valid.substr(12 + extraSize);
  ASSERT_TRUE(extract(0, text.size()));
  EXPECT_EQ(files["/output"], text);
  header = valid.substr(0, 10);
  le16(header, ra.size() * 2);
  files["/test.dict.dz"] = header + ra + ra + valid.substr(12 + extraSize);
  DictZip::ExtractError error;
  EXPECT_FALSE(extract(0, text.size(), &error));
  EXPECT_EQ(error, DictZip::ExtractError::Decompress);
}
TEST_F(DictZipTest, ReplacementFailureDoesNotRetainPreviousDefinition) {
  const auto location = install("first definition");
  DictionaryDefinitionSource source;
  ASSERT_TRUE(source.open(location));
  const std::string oldPath = source.path();
  failRead = true;
  auto replacement = location;
  replacement.offset++;
  replacement.size--;
  EXPECT_FALSE(source.open(replacement));
  EXPECT_TRUE(source.path().empty());
  EXPECT_FALSE(files.count(oldPath));
}
TEST_F(DictZipTest, ReflowReusesBackingWithoutDecompressionOrWrites) {
  const auto location = install("unchanged definition");
  DictionaryDefinitionSource source;
  ASSERT_TRUE(source.open(location));
  const auto path = source.path();
  failAllocationAfter = 0;
  failWrite = true;
  EXPECT_TRUE(source.open(location));
  failAllocationAfter = -1;
  EXPECT_EQ(source.path(), path);
  EXPECT_EQ(files[path], "unchanged definition");
}
TEST_F(DictZipTest, OneShotInflateAndExactSizeChecksRemainIntact) {
  const std::string data = "font glyph bytes font glyph bytes font glyph bytes";
  // Use a separately finished raw stream for readExact's end-of-stream check.
  z_stream stream{};
  ASSERT_EQ(deflateInit2(&stream, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY), Z_OK);
  std::array<uint8_t, 256> compressed{}, output{};
  stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  stream.avail_in = data.size();
  stream.next_out = compressed.data();
  stream.avail_out = compressed.size();
  ASSERT_EQ(deflate(&stream, Z_FINISH), Z_STREAM_END);
  const auto length = compressed.size() - stream.avail_out;
  deflateEnd(&stream);
  InflateReader reader;
  reader.init();
  reader.setSource(compressed.data(), length);
  EXPECT_TRUE(reader.readExact(output.data(), data.size()));
  EXPECT_EQ(std::string(reinterpret_cast<char*>(output.data()), data.size()), data);
  reader.init();
  reader.setSource(compressed.data(), length);
  EXPECT_FALSE(reader.readExact(output.data(), data.size() - 1));
  reader.init();
  reader.setSource(compressed.data(), length - 1);
  EXPECT_FALSE(reader.readExact(output.data(), data.size()));
}
}  // namespace
