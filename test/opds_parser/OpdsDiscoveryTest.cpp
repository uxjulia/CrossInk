#include <OpdsAuthChallenge.h>
#include <OpdsFeedParser.h>
#include <OpenSearchDescParser.h>
#include <gtest/gtest.h>

TEST(OpdsDiscovery, DescriptionLinkDoesNotBecomeInlineTemplate) {
  OpdsEntry entries[2];
  OpdsFeedParser parser(entries, 2);
  const std::string feed =
      R"(<feed><link rel="search" type="application/opensearchdescription+xml" href="../search.xml"/><entry><link rel="search" type="application/opensearchdescription+xml" href="wrong.xml"/></entry></feed>)";
  ASSERT_EQ(parser.write(reinterpret_cast<const uint8_t*>(feed.data()), feed.size()), feed.size());
  parser.flush();
  ASSERT_TRUE(parser);
  EXPECT_EQ(parser.getSearchDescriptionUrl(), "../search.xml");
  EXPECT_TRUE(parser.getSearchTemplate().empty());
}
TEST(OpdsDiscovery, SelectsFeedTemplateAndDecodesEntities) {
  OpenSearchDescParser parser;
  const std::string doc =
      R"(<OpenSearchDescription><Url type="text/html" template="/wrong?q={searchTerms}"/><Url type="application/opds+json" template="search{?query}"/><Url type="application/atom+xml;profile=opds-catalog" template="../results?q={searchTerms}&amp;page={startPage?}"/></OpenSearchDescription>)";
  for (char c : doc) ASSERT_EQ(parser.write(uint8_t(c)), 1u);
  parser.flush();
  EXPECT_FALSE(parser.error());
  EXPECT_EQ(parser.getTemplate(), "../results?q={searchTerms}&page={startPage?}");
}
TEST(OpdsDiscovery, RejectsTruncatedOversizedAndNonCatalogDocuments) {
  for (const auto& doc : {std::string("<OpenSearchDescription>"), std::string(16385, 'x')}) {
    OpenSearchDescParser parser;
    parser.write(reinterpret_cast<const uint8_t*>(doc.data()), doc.size());
    parser.flush();
    EXPECT_TRUE(parser.error());
  }
  OpenSearchDescParser parser;
  const std::string doc = "<root><Url type=\"text/html\" template=\"/search?q={searchTerms}\"/></root>";
  parser.write(reinterpret_cast<const uint8_t*>(doc.data()), doc.size());
  parser.flush();
  EXPECT_TRUE(parser.getTemplate().empty());
}
TEST(OpdsDiscovery, BasicAuthenticationIsExactBoundedAndComplete) {
  const std::string valid = R"({"authentication":[{"type":"http://opds-spec.org/auth/basic"}]})";
  OpdsAuthChallenge good;
  for (char c : valid) ASSERT_TRUE(good.write(reinterpret_cast<const uint8_t*>(&c), 1));
  EXPECT_TRUE(good.offersBasic());
  for (const auto& doc : {valid.substr(0, valid.size() - 1), valid + "x", std::string(8193, ' '),
                          std::string(R"({"authentication":[{"type":"http://opds-spec.org/auth/basic-malicious"}]})"),
                          std::string(R"({"authentication":[{"type":"http://opds-spec.org/auth/oauth/password"}]})"),
                          std::string(R"({"type":"http://opds-spec.org/auth/basic"})")}) {
    OpdsAuthChallenge parser;
    parser.write(reinterpret_cast<const uint8_t*>(doc.data()), doc.size());
    EXPECT_FALSE(parser.offersBasic()) << doc.substr(0, 80);
  }
}

TEST(OpdsDiscovery, RejectsMalformedAuthenticationGrammar) {
  const std::string type = "\"http://opds-spec.org/auth/basic\"";
  for (const auto& doc :
       {"{\"authentication\" [{\"type\":" + type + "}]}", "{\"authentication\":[{\"type\":" + type + ",}]}",
        "{\"authentication\":[{\"type\":" + type + "}] ,}", "{\"authentication\":[{\"type\":" + type + "}]]",
        "{\"authentication\":[{\"type\":" + type + "}],\"other\":01}",
        "{\"authentication\":[{\"type\":" + type + "}],\"other\":tru}",
        "{\"authentication\":[{\"type\":" + type + "}],\"other\":\"\\q\"}"}) {
    OpdsAuthChallenge parser;
    parser.write(reinterpret_cast<const uint8_t*>(doc.data()), doc.size());
    EXPECT_FALSE(parser.offersBasic()) << doc;
  }
  const std::string valid =
      "{\"authentication\":[{\"type\":" + type + "}],\"other\":[null,true,false,-1.23e-4,0,{},[]]}";
  OpdsAuthChallenge parser;
  ASSERT_TRUE(parser.write(reinterpret_cast<const uint8_t*>(valid.data()), valid.size()));
  EXPECT_TRUE(parser.offersBasic());
}
