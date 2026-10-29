#include <OpdsFeedParser.h>
#include <gtest/gtest.h>

namespace {
class OpdsJsonTest : public ::testing::Test {
 protected:
  OpdsEntry entries[4];
  bool feed(OpdsFeedParser& parser, const std::string& text) {
    for (const auto byte : text)
      if (parser.write(static_cast<uint8_t>(byte)) != 1) return false;
    parser.flush();
    return bool(parser);
  }
};
TEST_F(OpdsJsonTest, SniffsBothFormatsAndKeepsDescriptionsAndPaging) {
  OpdsFeedParser parser(entries, 4);
  parser.setPreferredLanguage("en");
  ASSERT_TRUE(feed(
      parser,
      "\xEF\xBB\xBF \n"
      R"({"publications":[{"metadata":{"title":{"fr":"Livre","en":"Book"},"author":[{"name":"Writer"}],"description":"<p>A story</p>"},"links":[{"href":"sample.epub","type":"application/epub+zip","rel":"http://opds-spec.org/acquisition/sample"},{"href":"book.epub","type":"application/epub+zip","rel":["self","http://opds-spec.org/acquisition/open-access"]}]}],"links":[{"href":"?page=2","rel":"next"},{"href":"?page=0","rel":"previous"},{"href":"?q={searchTerms}","rel":"search","templated":true}]})"));
  ASSERT_EQ(parser.getEntryCount(), 1u);
  EXPECT_EQ(entries[0].title, "Book");
  EXPECT_EQ(entries[0].author, "Writer");
  EXPECT_STREQ(entries[0].description.data(), "A story ");
  EXPECT_EQ(entries[0].href, "book.epub");
  EXPECT_EQ(parser.getNextPageUrl(), "?page=2");
  EXPECT_EQ(parser.getPrevPageUrl(), "?page=0");
  EXPECT_EQ(parser.getSearchTemplate(), "?q={searchTerms}");
  OpdsFeedParser xml(entries, 4);
  ASSERT_TRUE(
      feed(xml, "<feed><entry><title>Folder</title><link href='next' type='application/atom+xml'/></entry></feed>"));
  EXPECT_EQ(xml.getEntryCount(), 1u);
}
TEST_F(OpdsJsonTest, BoundsEntriesButReadsTrailingNextLink) {
  OpdsFeedParser parser(entries, 1);
  ASSERT_TRUE(feed(
      parser,
      R"({"navigation":[{"title":"One","href":"one"},{"title":"Two","href":"two"}],"links":[{"rel":"next","href":"next"}]})"));
  EXPECT_EQ(parser.getEntryCount(), 1u);
  EXPECT_TRUE(parser.wasTruncated());
  EXPECT_EQ(parser.getNextPageUrl(), "next");
  EXPECT_TRUE(entries[1].title.empty());
}
TEST_F(OpdsJsonTest, GroupsAndFacetsRemainNavigable) {
  OpdsFeedParser parser(entries, 4);
  ASSERT_TRUE(feed(
      parser,
      R"({"groups":[{"metadata":{"title":"All stories"},"links":[{"rel":"self","href":"stories"}],"navigation":[{"title":"Category","href":"category"}]}],"facets":[{"links":[{"title":"English","href":"en","properties":{"numberOfItems":42}}]}]})"));
  ASSERT_EQ(parser.getEntryCount(), 3u);
  EXPECT_EQ(entries[1].title, "All stories");
  EXPECT_EQ(entries[1].href, "stories");
  EXPECT_EQ(entries[2].count, 42);
}
TEST_F(OpdsJsonTest, RejectsTruncatedAndInvalidDocuments) {
  for (const std::string text : {"", "{", "{}{}", "[1]", "oops", "{\"navigation\":[{\"title\":\"unfinished"}) {
    OpdsFeedParser parser(entries, 4);
    EXPECT_FALSE(feed(parser, text)) << text;
  }
}
TEST_F(OpdsJsonTest, StandalonePublicationAndIndirectLinks) {
  OpdsJsonParser parser(entries, 1, true);
  const std::string doc =
      R"({"metadata":{"title":"Details","description":"Full description"},"links":[{"rel":"self","type":"application/opds-publication+json","href":"details.json"},{"rel":"acquisition","type":"application/opds-publication+json","href":"fulfill.json"}]})";
  ASSERT_EQ(parser.write(reinterpret_cast<const uint8_t*>(doc.data()), doc.size()), doc.size());
  parser.flush();
  ASSERT_FALSE(parser.error());
  ASSERT_EQ(parser.getEntryCount(), 1u);
  EXPECT_TRUE(entries[0].indirect);
  EXPECT_EQ(entries[0].selfHref, "details.json");
  EXPECT_EQ(entries[0].href, "fulfill.json");
}
TEST_F(OpdsJsonTest, DoesNotTreatLcpOrPurchasesAsEpubDownloads) {
  OpdsFeedParser parser(entries, 4);
  ASSERT_TRUE(feed(
      parser,
      R"({"publications":[{"metadata":{"title":"Loan"},"links":[{"rel":"borrow","type":"application/vnd.readium.lcp.license.v1.0+json","href":"loan.lcpl"}]},{"metadata":{"title":"Purchase"},"links":[{"rel":"buy","type":"application/epub+zip","href":"buy"}]}]})"));
  ASSERT_EQ(parser.getEntryCount(), 2u);
  EXPECT_TRUE(entries[0].unsupportedAcquisition);
  EXPECT_TRUE(entries[1].unsupportedAcquisition);
}
TEST_F(OpdsJsonTest, DescriptionDoesNotSplitUtf8OrOverflowCallerStorage) {
  OpdsFeedParser parser(entries, 4);
  std::string description(255, 'a');
  description += "\xC3\xA9";
  ASSERT_TRUE(
      feed(parser,
           "{\"publications\":[{\"metadata\":{\"title\":\"Book\",\"description\":\"" + description +
               "\"},\"links\":[{\"rel\":\"download\",\"type\":\"application/epub+zip\",\"href\":\"book.epub\"}]}]}"));
  EXPECT_EQ(strlen(entries[0].description.data()), 255u);
}
TEST_F(OpdsJsonTest, KeepsExcerptWhenDescriptionExceedsTokenizerBuffer) {
  OpdsFeedParser parser(entries, 4);
  const std::string text =
      "{\"publications\":[{\"metadata\":{\"title\":\"Book\",\"description\":\"" + std::string(6000, 'd') +
      "\"},\"links\":[{\"rel\":\"download\",\"type\":\"application/epub+zip\",\"href\":\"book.epub\"}]}]}";
  ASSERT_TRUE(feed(parser, text));
  ASSERT_EQ(parser.getEntryCount(), 1u);
  EXPECT_EQ(std::string(entries[0].description.data()), std::string(MAX_OPDS_DESCRIPTION_BYTES, 'd'));
  EXPECT_EQ(entries[0].href, "book.epub");
}
TEST_F(OpdsJsonTest, BoundsCombinedStringsAndRetainsPagination) {
  OpdsEntry largeEntries[50];
  OpdsFeedParser parser(largeEntries, 50);
  std::string json = "{\"navigation\":[";
  for (int i = 0; i < 50; ++i) {
    if (i) json += ',';
    json += "{\"title\":\"" + std::string(160, 't') + "\",\"href\":\"" + std::string(768, 'u') + "\"}";
  }
  json += "],\"links\":[{\"rel\":\"next\",\"href\":\"next\"}]}";
  ASSERT_TRUE(feed(parser, json));
  EXPECT_GT(parser.getEntryCount(), 0u);
  EXPECT_LT(parser.getEntryCount(), 50u);
  EXPECT_TRUE(parser.wasTruncated());
  EXPECT_EQ(parser.getNextPageUrl(), "next");
  size_t bytes = 0;
  for (size_t i = 0; i < parser.getEntryCount(); ++i)
    bytes += largeEntries[i].title.size() + largeEntries[i].href.size();
  EXPECT_LE(bytes, 24u * 1024u);
}
}  // namespace
