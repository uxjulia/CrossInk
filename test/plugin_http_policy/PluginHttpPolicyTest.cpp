#include <gtest/gtest.h>

#include "util/PluginHttpPolicy.h"

TEST(PluginHttpPolicy, CredentialsStayAtTheirConfiguredOrigin) {
  EXPECT_TRUE(pluginhttp::sameOrigin("https://CAT.example/books", "https://cat.example:443/next"));
  EXPECT_FALSE(pluginhttp::sameOrigin("https://cat.example/books", "http://cat.example/next"));
  EXPECT_FALSE(pluginhttp::sameOrigin("https://cat.example/books", "https://cdn.example/file"));
  EXPECT_FALSE(pluginhttp::sameOrigin("https://cat.example/books", "https://cat.example:444/next"));
  EXPECT_FALSE(pluginhttp::sameOrigin("{url}", "https://cat.example/next"));
}
TEST(PluginHttpPolicy, FeedControlledAuthoritiesUseTheBrowseOrigin) {
  const std::string browse = "https://catalog.test/api";
  for (const auto* tpl : {"{url}", "https://{id}/resolve", "https://catalog.test:{title}/api"}) {
    EXPECT_TRUE(pluginhttp::trustedDownloadTarget(tpl, browse, "https://catalog.test:443/book"));
    for (const auto* target : {"https://cdn.test/file", "http://catalog.test/file", "https://catalog.test:444/file",
                               "https://catalog.test@cdn.test/file"})
      EXPECT_FALSE(pluginhttp::trustedDownloadTarget(tpl, browse, target));
  }
  EXPECT_TRUE(pluginhttp::trustedDownloadTarget("https://api.test/lookup/{url}", browse, "https://api.test/lookup/1"));
  EXPECT_TRUE(pluginhttp::usesSecretTemplate("/page/{token}"));
  EXPECT_TRUE(pluginhttp::usesSecretTemplate("{cfg.password}"));
  EXPECT_FALSE(pluginhttp::usesSecretTemplate("/books?page={page}"));
}
TEST(PluginHttpPolicy, RejectsHttpFramingAndRequestInjection) {
  EXPECT_TRUE(pluginhttp::validRequest("https://example.org/list?q=a%20b", "PROPFIND"));
  EXPECT_FALSE(pluginhttp::validRequest("https://example.org/\r\nHost: other", "GET"));
  EXPECT_FALSE(pluginhttp::validRequest("https://example.org", "GET\r\n"));
  EXPECT_FALSE(pluginhttp::validRequest("file:///local", "GET"));
  EXPECT_FALSE(pluginhttp::validRequest("https://user:pass@example.org", "GET"));
  EXPECT_TRUE(pluginhttp::validHeader("Authorization", "Bearer token"));
  EXPECT_FALSE(pluginhttp::validHeader("Authorization", "token\r\nHost: other"));
  EXPECT_FALSE(pluginhttp::validHeader("Content-Length", "1"));
  EXPECT_FALSE(pluginhttp::validHeader("rAnGe", "bytes=9-"));
  EXPECT_FALSE(pluginhttp::validHeader("bad:header", "value"));
}
