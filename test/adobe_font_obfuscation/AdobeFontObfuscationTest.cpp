#include <AdobeFontObfuscation.h>
#include <FtFont.h>
#include <TrueTypeFormat.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numeric>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;
constexpr AdobeFontObfuscation::Key Key{0x53, 0x12, 0x4c, 0xf5, 0x71, 0x95, 0x45, 0xb2,
                                        0xb3, 0xa0, 0x7f, 0x29, 0x74, 0xbc, 0x2b, 0x56};
constexpr char Uuid[] = "urn:uuid:53124cf5-7195-45b2-b3a0-7f2974bc2b56";
std::string package(const std::string& identifiers) {
  return "<p:package xmlns:p='http://www.idpf.org/2007/opf' xmlns:d='http://purl.org/dc/elements/1.1/' "
         "unique-identifier='other'><p:metadata>" +
         identifiers + "</p:metadata></p:package>";
}
std::string entry(const std::string& uri, const std::string& method = "http://ns.adobe.com/pdf/enc#RC") {
  return "<e:EncryptedData><e:EncryptionMethod Algorithm='" + method + "'/><e:CipherData><e:CipherReference URI='" +
         uri + "'/></e:CipherData></e:EncryptedData>";
}
std::string manifest(const std::string& entries) {
  return "<encryption xmlns='urn:oasis:names:tc:opendocument:xmlns:container' "
         "xmlns:e='http://www.w3.org/2001/04/xmlenc#'>" +
         entries + "</encryption>";
}
bool parse(AdobeFontObfuscation& parser, const std::string& xml, size_t chunk = 1) {
  if (!parser.setup()) return false;
  for (size_t pos = 0; pos < xml.size(); pos += chunk) {
    const size_t count = std::min(chunk, xml.size() - pos);
    if (parser.write(reinterpret_cast<const uint8_t*>(xml.data() + pos), count) != count) return false;
  }
  return parser.finish();
}
class Sink : public Print {
 public:
  Bytes bytes;
  size_t capacity = SIZE_MAX;
  size_t write(uint8_t data) override { return write(&data, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    const size_t count = std::min(size, capacity - bytes.size());
    bytes.insert(bytes.end(), data, data + count);
    return count;
  }
};
Bytes read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}
std::string readText(const std::filesystem::path& path) {
  const auto bytes = read(path);
  return {bytes.begin(), bytes.end()};
}
}  // namespace

TEST(AdobeFontObfuscation, SelectsFirstUuidInsteadOfIsbnOrPackageUniqueIdentifier) {
  AdobeFontObfuscation parser;
  ASSERT_TRUE(parse(
      parser, package("<d:identifier id='other'>9781234567890</d:identifier><d:identifier>\n " + std::string(Uuid) +
                      "\t </d:identifier><d:identifier>urn:uuid:00000000-0000-0000-0000-"
                      "000000000000</d:identifier>")));
  ASSERT_TRUE(parser.hasKey());
  EXPECT_EQ(parser.key(), Key);
}

TEST(AdobeFontObfuscation, AcceptsCaseAndUuidSchemeWithSplitCharacterData) {
  for (const auto& id : {"<d:identifier>URN:UUID:53124CF5-7195-45B2-B3A0-7F2974BC2B56</d:identifier>",
                         "<d:identifier p:scheme='UUID'>53124cf5719545b2b3a07f2974bc2b56</d:identifier>"}) {
    AdobeFontObfuscation parser;
    ASSERT_TRUE(parse(parser, package(id)));
    EXPECT_TRUE(parser.hasKey());
    EXPECT_EQ(parser.key(), Key);
  }
}

TEST(AdobeFontObfuscation, RejectsInvalidMissingAndOversizedIdentifiers) {
  for (const auto& value : {std::string("9781234567890"), std::string("urn:uuid:53124cf5-7195-45b2-b3a0-7f2974bc2b5z"),
                            std::string(Uuid) + "0", std::string(10000, 'a'), std::string()}) {
    AdobeFontObfuscation parser;
    ASSERT_TRUE(parse(parser, package("<d:identifier>" + value + "</d:identifier>"), 37));
    EXPECT_FALSE(parser.hasKey());
  }
}

TEST(AdobeFontObfuscation, IgnoresIdentifiersOutsidePackageMetadataOrWrongNamespace) {
  for (const auto& xml :
       {"<wrapper>" + package("<d:identifier>" + std::string(Uuid) + "</d:identifier>") + "</wrapper>",
        package("<identifier xmlns='urn:wrong'>" + std::string(Uuid) + "</identifier>"),
        package("") + "<!--" + Uuid + "-->"}) {
    AdobeFontObfuscation parser;
    ASSERT_TRUE(parse(parser, xml));
    EXPECT_FALSE(parser.hasKey());
  }
}

TEST(AdobeFontObfuscation, MatchesRootRelativeDecodedFontPathWithinItsOwnEntry) {
  const std::string path = "OEBPS/fonts/My Font.otf";
  AdobeFontObfuscation parser(&path);
  ASSERT_TRUE(parse(parser, manifest(entry("other.otf", "http://www.idpf.org/2008/embedding") +
                                     entry("/OEBPS/temp/../fonts/My%20Font.otf"))));
  EXPECT_TRUE(parser.isAdobeFont());
}

TEST(AdobeFontObfuscation, DoesNotApplyAdobeToOtherResourcesOrAlgorithms) {
  const std::string path = "fonts/font.ttf";
  for (const auto& entries : {entry("other.ttf") + entry(path, "http://www.idpf.org/2008/embedding"),
                              entry("other.ttf") + entry(path, "http://www.w3.org/2001/04/xmlenc#aes128-cbc"),
                              entry(path, "http://ns.adobe.com/pdf/enc#RC-extra"), entry("other.ttf"), std::string()}) {
    AdobeFontObfuscation parser(&path);
    ASSERT_TRUE(parse(parser, manifest(entries)));
    EXPECT_FALSE(parser.isAdobeFont());
  }
}

TEST(AdobeFontObfuscation, RemovesSingleDotSegmentsBeforeResolvingParentSegments) {
  const std::string path = "OEBPS/fonts/font.ttf";
  for (const auto* uri : {"./OEBPS/fonts/font.ttf", "OEBPS/./fonts/font.ttf", "OEBPS/temp/./../fonts/font.ttf",
                          "%2e/OEBPS/fonts/font.ttf", "OEBPS/fonts/./font.ttf", "././OEBPS/fonts/font.ttf"}) {
    AdobeFontObfuscation parser(&path);
    ASSERT_TRUE(parse(parser, manifest(entry(uri))));
    EXPECT_TRUE(parser.isAdobeFont()) << uri;
  }
}

TEST(AdobeFontObfuscation, SupportsReorderedMethodAndReference) {
  const std::string path = "fonts/font.ttf";
  AdobeFontObfuscation parser(&path);
  ASSERT_TRUE(parse(parser, manifest("<e:EncryptedData><e:CipherData><e:CipherReference URI='fonts/font.ttf'/>"
                                     "</e:CipherData><e:EncryptionMethod Algorithm='http://ns.adobe.com/pdf/enc#RC'/>"
                                     "</e:EncryptedData>")));
  EXPECT_TRUE(parser.isAdobeFont());
}

TEST(AdobeFontObfuscation, RejectsMalformedXmlAndDtds) {
  for (const auto& xml : {"<package><metadata>", "<!DOCTYPE package [<!ENTITY id 'abc'>]><package/>",
                          "<!DOCTYPE package SYSTEM 'file:///tmp/private'><package/>"}) {
    AdobeFontObfuscation parser;
    EXPECT_FALSE(parse(parser, xml));
  }
}

TEST(AdobeFontObfuscation, StreamPreservesTailInputAndLengthAcrossAllChunkBoundaries) {
  for (size_t length : {0, 1, 15, 16, 127, 1023, 1024, 1025, 4097}) {
    Bytes input(length);
    std::iota(input.begin(), input.end(), uint8_t{0});
    const Bytes original = input;
    Bytes expected = input;
    for (size_t i = 0; i < std::min(length, size_t{1024}); ++i) expected[i] ^= Key[i % Key.size()];
    for (size_t chunk : {1, 7, 127, 512, 1023, 1024, 1025, 4096}) {
      Sink sink;
      AdobeFontStream stream(sink, Key);
      for (size_t pos = 0; pos < length; pos += chunk) {
        const size_t count = std::min(chunk, length - pos);
        ASSERT_EQ(stream.write(input.data() + pos, count), count);
      }
      EXPECT_EQ(sink.bytes, expected);
      EXPECT_EQ(input, original);
    }
  }
}

TEST(AdobeFontObfuscation, PropagatesAndCanResumeShortWritesIncludingTail) {
  Bytes input(2048, 0x55);
  for (size_t capacity : {0, 33, 1023, 1024, 1025, 2000}) {
    Sink sink;
    sink.capacity = capacity;
    AdobeFontStream stream(sink, Key);
    ASSERT_EQ(stream.write(input.data(), input.size()), capacity);
    sink.capacity = SIZE_MAX;
    ASSERT_EQ(stream.write(input.data() + capacity, input.size() - capacity), input.size() - capacity);
    for (size_t i = 0; i < input.size(); ++i) ASSERT_EQ(sink.bytes[i], input[i] ^ (i < 1024 ? Key[i % Key.size()] : 0));
  }
}

TEST(AdobeFontObfuscation, SuppliedBookFontsPassPreflightAndRasterize) {
  // Optional private fixture: extract only content.opf, encryption.xml and fonts
  // outside the repo. No copyrighted book assets are committed with this test.
  const char* fixture = std::getenv("CROSSINK_ADOBE_FONT_FIXTURE");
  if (!fixture) GTEST_SKIP() << "Set CROSSINK_ADOBE_FONT_FIXTURE for a local EPUB font integration check";
  const std::filesystem::path root(fixture);
  AdobeFontObfuscation packageParser;
  ASSERT_TRUE(parse(packageParser, readText(root / "content.opf"), 31));
  ASSERT_TRUE(packageParser.hasKey());
  const auto xml = readText(root / "META-INF/encryption.xml");
  size_t tested = 0;
  for (const auto& file : std::filesystem::directory_iterator(root / "fonts")) {
    const std::string path = "fonts/" + file.path().filename().string();
    SCOPED_TRACE(path);
    AdobeFontObfuscation manifestParser(&path);
    ASSERT_TRUE(parse(manifestParser, xml, 43));
    ASSERT_TRUE(manifestParser.isAdobeFont());
    const auto input = read(file.path());
    ASSERT_GT(input.size(), 1024u);
    Sink sink;
    AdobeFontStream stream(sink, packageParser.key());
    for (size_t offset = 0; offset < input.size(); offset += 503) {
      const size_t count = std::min(size_t{503}, input.size() - offset);
      ASSERT_EQ(stream.write(input.data() + offset, count), count);
    }
    ASSERT_EQ(sink.bytes.size(), input.size());
    EXPECT_EQ(truetype::inspect(sink.bytes.size(),
                                [&](size_t offset, uint8_t* out, size_t count) {
                                  std::copy_n(sink.bytes.data() + offset, count, out);
                                  return true;
                                }),
              truetype::Result::Supported);
    freeink::font::FtFont font;
    ASSERT_TRUE(font.init(sink.bytes.data(), sink.bytes.size(), 24));
    bool rendered = false;
    for (unsigned cp = 33; cp < 0x3000; ++cp) {
      if (!font.hasGlyph(cp)) continue;
      const auto* bitmap = font.rasterize(cp, 24);
      ASSERT_NE(bitmap, nullptr);
      if (bitmap->width && bitmap->height) {
        rendered = true;
        break;
      }
    }
    EXPECT_TRUE(rendered);
    ++tested;
  }
  EXPECT_GT(tested, 0u);
}
