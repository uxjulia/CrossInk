#include <gtest/gtest.h>

#include <string>

#include "ContentOpfParser.h"
#include "Epub/BookMetadataCache.h"

namespace {

// The parser retains references to both paths, so they must outlive each instance.
const std::string kEmptyPath;
const std::string kMissingCachePath = "/missing-cache";
const std::string kContentPath = "OPS/";

void parse(ContentOpfParser& parser, const std::string& xml) {
  ASSERT_TRUE(parser.setup());
  EXPECT_EQ(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
}

}  // namespace

TEST(ContentOpfParserMetadata, EntityCallbackDoesNotSplitOneAuthor) {
  const std::string xml =
      R"(<package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:creator>&#201;mile Zola</dc:creator></metadata></package>)";
  ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.author, "Émile Zola");
}

TEST(ContentOpfParserMetadata, ClampsOversizedMetadataTextInsteadOfGrowingUnbounded) {
  const std::string hugeTitle(64 * 1024, 'A');
  const std::string xml = "<package xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><metadata><dc:title>" + hugeTitle +
                          " tail</dc:title></metadata></package>";
  ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.title.size(), 512u);
  EXPECT_EQ(parser.title[0], 'A');
}

TEST(ContentOpfParserMetadata, ClampDoesNotSplitUtf8Codepoints) {
  const std::string prefix(511, 'A');
  const std::string xml = "<package xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><metadata><dc:title>" + prefix +
                          "\xC3\xA9&amp;tail</dc:title></metadata></package>";
  ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.title, prefix);
}

TEST(ContentOpfParserMetadata, ClampNeverOvershootsAtAMultiCreatorSeparatorBoundary) {
  // First creator fills the field to exactly one byte under the cap, so the
  // second creator's leading ", " separator is the thing that would push the
  // total past 512 if the clamp checked only the next character.
  const std::string firstAuthor(511, 'A');
  const std::string xml = "<package xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><metadata><dc:creator>" +
                          firstAuthor + "</dc:creator><dc:creator>B</dc:creator></metadata></package>";
  ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_LE(parser.author.size(), 512u);
  EXPECT_EQ(parser.author, firstAuthor);
}

TEST(ContentOpfParserMetadata, SeparatesCreatorElementsAndCollapsesXmlWhitespace) {
  const std::string xml = R"(<package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata>
    <dc:title>  The
   Left Hand   of Darkness  </dc:title>
    <dc:creator> Ursula   K. Le Guin </dc:creator>
    <dc:creator>
Octavia E. Butler
</dc:creator>
  </metadata></package>)";
  ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr);

  parse(parser, xml);

  EXPECT_EQ(parser.title, "The Left Hand of Darkness");
  EXPECT_EQ(parser.author, "Ursula K. Le Guin, Octavia E. Butler");
}

TEST(ContentOpfParserMetadata, ReadsCalibreSeriesAndFirstSubject) {
  const std::string xml = R"(<package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata>
    <meta content="Earthsea" name="calibre:series"/>
    <meta content="2.5" name="calibre:series_index"/>
    <dc:subject> Fantasy &amp; Adventure </dc:subject>
    <dc:subject>Young adult</dc:subject>
  </metadata></package>)";
  ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr);
  parse(parser, xml);
  EXPECT_EQ(parser.series, "Earthsea");
  EXPECT_EQ(parser.seriesIndex, "2.5");
  EXPECT_EQ(parser.subject, "Fantasy & Adventure");
}

TEST(ContentOpfParserMetadata, DistinguishesEpubThreeSeriesFromSets) {
  const std::string xml = R"(<package><metadata>
    <meta property="belongs-to-collection" id="collection">Earthsea</meta>
    <meta refines="#collection" property="collection-type">series</meta>
    <meta refines="#collection" property="group-position"> 3 </meta>
  </metadata></package>)";
  ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr);
  parse(parser, xml);
  EXPECT_EQ(parser.series, "Earthsea");
  EXPECT_EQ(parser.seriesIndex, "3");

  const std::string setXml = R"(<package><metadata>
    <meta property="belongs-to-collection" id="collection">Boxed books</meta>
    <meta refines="#collection" property="collection-type">set</meta>
    <meta refines="#collection" property="group-position">1</meta>
  </metadata></package>)";
  ContentOpfParser setParser(kEmptyPath, kEmptyPath, setXml.size(), nullptr);
  parse(setParser, setXml);
  EXPECT_TRUE(setParser.series.empty());
  EXPECT_TRUE(setParser.seriesIndex.empty());

  const std::string mixedXml = R"(<package><metadata>
    <meta property="belongs-to-collection" id="set">Boxed books</meta>
    <meta refines="#set" property="collection-type">set</meta>
    <meta property="belongs-to-collection" id="series">Earthsea</meta>
    <meta refines="#series" property="collection-type">series</meta>
    <meta refines="#series" property="group-position">2</meta>
  </metadata></package>)";
  ContentOpfParser mixedParser(kEmptyPath, kEmptyPath, mixedXml.size(), nullptr);
  parse(mixedParser, mixedXml);
  EXPECT_EQ(mixedParser.series, "Earthsea");
  EXPECT_EQ(mixedParser.seriesIndex, "2");
}

TEST(ContentOpfParserMetadata, StopsBeforeManifestWithoutOpeningTemporaryStorage) {
  const std::string xml = R"(<package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata>
    <dc:title>A Wizard of Earthsea</dc:title>
    <dc:creator>Ursula K. Le Guin</dc:creator>
    <dc:language>en</dc:language>
  </metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest>
  </package>)";
  Storage = {};
  ContentOpfParser parser(kMissingCachePath, kContentPath, xml.size(), nullptr, /*collectCssFiles=*/true,
                          /*metadataOnly=*/true);

  ASSERT_TRUE(parser.setup());
  EXPECT_LT(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  EXPECT_EQ(parser.title, "A Wizard of Earthsea");
  EXPECT_EQ(parser.author, "Ursula K. Le Guin");
  EXPECT_EQ(parser.language, "en");
  EXPECT_EQ(Storage.writeOpens, 0);
  EXPECT_EQ(Storage.readOpens, 0);
}

TEST(ContentOpfParserMetadata, NeverEntersManifestWhenMetadataElementIsMissing) {
  const std::string xml =
      R"(<package><manifest><item id="chapter" href="chapter.xhtml"/></manifest><spine/></package>)";
  Storage = {};
  ContentOpfParser parser(kMissingCachePath, kContentPath, xml.size(), nullptr, /*collectCssFiles=*/true,
                          /*metadataOnly=*/true);

  ASSERT_TRUE(parser.setup());
  EXPECT_LT(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  EXPECT_EQ(Storage.writeOpens, 0);
  EXPECT_EQ(Storage.readOpens, 0);
}

TEST(ContentOpfParserNamespaces, RecognizesOptimizerPrefixesAndAlternateDublinCorePrefix) {
  const std::string xml = R"(<ns0:package xmlns:ns0="http://www.idpf.org/2007/opf"
      xmlns:d="http://purl.org/dc/elements/1.1/">
    <ns0:metadata><d:title>Sample</d:title><d:creator>Author</d:creator>
      <d:language>en</d:language><d:subject>Fiction</d:subject>
    </ns0:metadata>
    <ns0:manifest>
      <ns0:item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>
      <ns0:item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
      <ns0:item id="css" href="style.css" media-type="text/css"/>
    </ns0:manifest>
    <ns0:spine><ns0:itemref idref="chapter"/></ns0:spine>
    <ns0:guide><ns0:reference type="start" href="chapter.xhtml"/></ns0:guide>
  </ns0:package>)";
  Storage = {};
  const std::string cachePath = "/cache";
  const std::string basePath = "OPS/";
  BookMetadataCache cache;
  ContentOpfParser parser(cachePath, basePath, xml.size(), &cache);
  parse(parser, xml);
  ASSERT_EQ(cache.spine.size(), 1u);
  EXPECT_EQ(cache.spine[0], "OPS/chapter.xhtml");
  EXPECT_EQ(parser.title, "Sample");
  EXPECT_EQ(parser.author, "Author");
  EXPECT_EQ(parser.language, "en");
  EXPECT_EQ(parser.subject, "Fiction");
  EXPECT_EQ(parser.tocNcxPath, "OPS/toc.ncx");
  ASSERT_EQ(parser.cssFiles.size(), 1u);
  EXPECT_EQ(parser.cssFiles[0], "OPS/style.css");
  EXPECT_EQ(parser.textReferenceHref, "OPS/chapter.xhtml");
  EXPECT_EQ(Storage.writeOpens, 1);
  EXPECT_EQ(Storage.readOpens, 2);
}

TEST(ContentOpfParserNamespaces, MetadataOnlyStopsBeforePrefixedManifest) {
  const std::string xml = R"(<ns0:package xmlns:ns0="http://www.idpf.org/2007/opf"
      xmlns:dc="http://purl.org/dc/elements/1.1/">
    <ns0:metadata><dc:title>Sample</dc:title></ns0:metadata>
    <ns0:manifest><ns0:item id="chapter" href="chapter.xhtml"/></ns0:manifest>
  </ns0:package>)";
  Storage = {};
  ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr, true, true);
  ASSERT_TRUE(parser.setup());
  EXPECT_LT(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  EXPECT_EQ(parser.title, "Sample");
  EXPECT_EQ(Storage.writeOpens, 0);
  EXPECT_EQ(Storage.readOpens, 0);
}

TEST(ContentOpfParserNamespaces, MetadataOnlySkipsPrefixedManifestWithoutMetadata) {
  const std::string xml = R"(<ns0:package xmlns:ns0="http://www.idpf.org/2007/opf">
    <ns0:manifest><ns0:item id="chapter" href="chapter.xhtml"/></ns0:manifest>
  </ns0:package>)";
  Storage = {};
  ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr, true, true);
  ASSERT_TRUE(parser.setup());
  EXPECT_LT(parser.write(reinterpret_cast<const uint8_t*>(xml.data()), xml.size()), xml.size());
  EXPECT_EQ(Storage.writeOpens, 0);
  EXPECT_EQ(Storage.readOpens, 0);
}

TEST(ContentOpfParserNamespaces, IgnoresUnrelatedNamespacesWithMatchingLocalNames) {
  const std::string xml = R"(<package xmlns="http://www.idpf.org/2007/opf"
      xmlns:other="urn:other" xmlns:dc="http://purl.org/dc/elements/1.1/">
    <metadata><other:title>Wrong title</other:title><dc:title>Right title</dc:title></metadata>
    <other:manifest><other:item id="wrong" href="wrong.css" media-type="text/css"/></other:manifest>
    <manifest><item id="right" href="right.css" media-type="text/css"/></manifest>
  </package>)";
  ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr);
  parse(parser, xml);
  EXPECT_EQ(parser.title, "Right title");
  ASSERT_EQ(parser.cssFiles.size(), 1u);
  EXPECT_EQ(parser.cssFiles[0], "right.css");
}

// Series slice adapted from jadehawk's CrossPoint #3804 regression cases.
TEST(ContentOpfParserMetadata, ResolvesInterleavedAndEarlyRefinementsByExactId) {
  const std::string xml = R"(<package xmlns="http://www.idpf.org/2007/opf"><metadata>
    <meta property="group-position" refines="#series">2.5a</meta>
    <meta property="collection-type" refines="#series">series</meta>
    <meta property="belongs-to-collection" id="set">A set</meta>
    <meta property="belongs-to-collection" id="series">The Series</meta>
    <meta property="group-position" refines="#set">99</meta>
    <meta property="collection-type" refines="#set">set</meta>
  </metadata></package>)";
  for (bool metadataOnly : {false, true}) {
    ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr, true, metadataOnly);
    ASSERT_TRUE(parser.setup());
    for (const char c : xml) {
      const size_t written = parser.write(static_cast<uint8_t>(c));
      if (metadataOnly && written == 0) break;  // intentional stop at </metadata>
      ASSERT_EQ(written, 1u);
    }
    EXPECT_EQ(parser.series, "The Series");
    EXPECT_EQ(parser.seriesIndex, "2.5a");
  }
}

TEST(ContentOpfParserMetadata, CalibrePrecedenceNeverMixesCollectionIndexes) {
  const std::string collection = R"(<meta property="belongs-to-collection" id="s">EPUB series</meta>
    <meta property="collection-type" refines="#s">series</meta>
    <meta property="group-position" refines="#s">9</meta>)";
  for (bool calibreFirst : {false, true}) {
    for (bool withIndex : {false, true}) {
      std::string calibre = R"(<meta name="calibre:series" content="Calibre series"/>)";
      if (withIndex) calibre += R"(<meta name="calibre:series_index" content="01.5b"/>)";
      const std::string xml = "<package><metadata>" + (calibreFirst ? calibre + collection : collection + calibre) +
                              "</metadata></package>";
      ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr);
      parse(parser, xml);
      EXPECT_EQ(parser.series, "Calibre series");
      EXPECT_EQ(parser.seriesIndex, withIndex ? "01.5b" : "");
    }
  }
}

TEST(ContentOpfParserMetadata, RejectsForeignNamespaceAndOversizedIdAliases) {
  const std::string longId(513, 'x');
  const std::string xml = std::string(R"(<package xmlns="http://www.idpf.org/2007/opf" xmlns:bad="urn:bad"><metadata>
    <bad:meta property="belongs-to-collection" id="s">Wrong</bad:meta>
    <meta property="collection-type" refines="#s">series</meta>)") +
                          "<meta property=\"belongs-to-collection\" id=\"" + longId + "\">Alias</meta>" +
                          "<meta property=\"collection-type\" refines=\"#" + longId.substr(0, 512) +
                          "\">series</meta>" + "</metadata></package>";
  ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr);
  parse(parser, xml);
  EXPECT_TRUE(parser.series.empty());
}

TEST(ContentOpfParserMetadata, CollectionArenaAndCandidateLimitsFailWithoutMixingFields) {
  for (bool exhaustBytes : {false, true}) {
    std::string xml = "<package><metadata>";
    for (int i = 0; i < 9; ++i) {
      const std::string id = std::to_string(i);
      xml += "<meta property=\"belongs-to-collection\" id=\"" + id + "\">" +
             (exhaustBytes ? std::string(512, 'x') : "A set") + "</meta>";
      if (i == 8) xml += "<meta property=\"collection-type\" refines=\"#8\">series</meta>";
    }
    xml += "</metadata></package>";
    ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr);
    parse(parser, xml);
    EXPECT_TRUE(parser.series.empty());
    EXPECT_TRUE(parser.seriesIndex.empty());
  }
}

TEST(ContentOpfParserMetadata, SeriesTitleAndStringIndexRespectUtf8Limit) {
  const std::string text = std::string(511, 'A') + "é";
  const std::string xml = "<package><metadata><meta property=\"belongs-to-collection\" id=\"s\">" + text +
                          "</meta><meta property=\"collection-type\" refines=\"#s\">series</meta>" +
                          "<meta property=\"group-position\" refines=\"#s\">" + text + "</meta></metadata></package>";
  ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr);
  ASSERT_TRUE(parser.setup());
  for (const char c : xml) ASSERT_EQ(parser.write(static_cast<uint8_t>(c)), 1u);
  EXPECT_EQ(parser.series, std::string(511, 'A'));
  EXPECT_EQ(parser.seriesIndex, std::string(511, 'A'));
}

TEST(ContentOpfParserMetadata, RepeatedMetadataCannotReuseReleasedCollectionSlots) {
  const std::string xml = R"(<package><metadata>
    <meta property="belongs-to-collection" id="old"></meta>
    <meta property="collection-type" refines="#old">series</meta>
  </metadata><metadata></metadata></package>)";
  ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr);
  parse(parser, xml);
  EXPECT_TRUE(parser.series.empty());
}

TEST(ContentOpfParserMetadata, SyncIdentifiersAndExistingSeries) {
  const std::string xml =
      R"(<package xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf"><metadata>
    <dc:identifier opf:scheme="ISBN">978-1-4028-9462-6</dc:identifier>
    <dc:identifier opf:scheme="AMAZON">B012345678</dc:identifier>
    <meta name="calibre:series" content="Series"/><meta name="calibre:series_index" content="2.5"/>
  </metadata></package>)";
  ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr);
  parse(parser, xml);
  EXPECT_EQ(parser.isbn, "978-1-4028-9462-6");
  EXPECT_EQ(parser.asin, "B012345678");
  EXPECT_EQ(parser.series, "Series");
  EXPECT_EQ(parser.seriesIndex, "2.5");
}
TEST(ContentOpfParserMetadata, SyncIdentifierPrefixesAndBounds) {
  const std::string xml =
      R"(<package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:identifier>urn:isbn:9781234567890</dc:identifier><dc:identifier>ASIN: B012345678</dc:identifier></metadata></package>)";
  ContentOpfParser parser(kEmptyPath, kEmptyPath, xml.size(), nullptr);
  parse(parser, xml);
  EXPECT_EQ(parser.isbn, "9781234567890");
  EXPECT_EQ(parser.asin, "B012345678");
  const std::string huge = R"(<package xmlns:dc="http://purl.org/dc/elements/1.1/"><metadata><dc:identifier>ISBN:)" +
                           std::string(4096, '1') + "</dc:identifier></metadata></package>";
  ContentOpfParser bounded(kEmptyPath, kEmptyPath, huge.size(), nullptr);
  parse(bounded, huge);
  EXPECT_TRUE(bounded.isbn.empty());
}
