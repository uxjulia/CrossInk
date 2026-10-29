#include <OpdsSearchTemplate.h>
#include <gtest/gtest.h>

#include "util/UrlUtils.h"

TEST(OpdsUrlTest, ResolvesDocumentReferencesAndRetainsQueries) {
  const std::string base = "https://catalog.example:8443/a/b/feed.json?old=1#fragment";
  const struct {
    const char* reference;
    const char* expected;
  } cases[] = {{"next.json", "https://catalog.example:8443/a/b/next.json"},
               {"?source=https://example.com", "https://catalog.example:8443/a/b/feed.json?source=https://example.com"},
               {"?page=2", "https://catalog.example:8443/a/b/feed.json?page=2"},
               {"#section", "https://catalog.example:8443/a/b/feed.json?old=1#section"},
               {"", "https://catalog.example:8443/a/b/feed.json?old=1"},
               {"../search?q=a/b", "https://catalog.example:8443/a/search?q=a/b"},
               {"./next", "https://catalog.example:8443/a/b/next"},
               {"/new/../books/./", "https://catalog.example:8443/books/"},
               {"../../../root", "https://catalog.example:8443/root"},
               {"..", "https://catalog.example:8443/a/"},
               {".", "https://catalog.example:8443/a/b/"},
               {"/path//file", "https://catalog.example:8443/path//file"},
               {"//other.example/book", "https://other.example/book"},
               {"https://other.example/new book.epub", "https://other.example/new%20book.epub"},
               {"new book%20two.epub", "https://catalog.example:8443/a/b/new%20book%20two.epub"}};
  for (const auto& item : cases) EXPECT_EQ(UrlUtils::resolveReference(base, item.reference), item.expected);
  EXPECT_EQ(UrlUtils::resolveReference("http://example.com", "book"), "http://example.com/book");
  EXPECT_EQ(UrlUtils::resolveReference("http://example.com/catalog/", "book"), "http://example.com/catalog/book");
}
TEST(OpdsUrlTest, ExpandsBothSearchDialectsWithEscapedQuery) {
  const auto query = opdsPercentEncode("a & café");
  EXPECT_EQ(expandOpdsSearchTemplate("search{?query,title,author}", query), "search?query=a%20%26%20caf%C3%A9");
  EXPECT_EQ(expandOpdsSearchTemplate("?q={searchTerms}&page={startPage?}", query), "?q=a%20%26%20caf%C3%A9&page=");
}
