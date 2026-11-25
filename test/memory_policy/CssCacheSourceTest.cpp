#include <CssCacheSource.h>
#include <gtest/gtest.h>

TEST(CssCacheSourceTest, MissingStaleTruncatedAndUnreadableMarkersCannotReuse) {
  Storage.reset();
  Storage.put("source.epub", {1, 2, 3});
  CssCacheSource::Identity identity{};
  ASSERT_TRUE(CssCacheSource::read("source.epub", 23, 131072, identity));
  EXPECT_FALSE(CssCacheSource::matches("book", identity));
  ASSERT_TRUE(CssCacheSource::save("book", identity));
  for (int reopen = 0; reopen < 2; ++reopen) {
    CssCacheSource::Identity current{};
    ASSERT_TRUE(CssCacheSource::read("source.epub", 23, 131072, current));
    EXPECT_TRUE(CssCacheSource::matches("book", current));
  }
  EXPECT_EQ(Storage.data("source.epub").readBytes, 0u);  // No ZIP contents/extraction.
  for (size_t field : {1u, 2u, 3u, 4u, 5u}) {
    auto stale = identity;
    ++stale[field];
    EXPECT_FALSE(CssCacheSource::matches("book", stale));
  }
  Storage.data("book/css_terminal.source").bytes.pop_back();
  EXPECT_FALSE(CssCacheSource::matches("book", identity));
  EXPECT_TRUE(CssCacheSource::invalidate("book"));
  EXPECT_FALSE(CssCacheSource::matches("book", identity));
  Storage.data("source.epub").modified = 0;
  EXPECT_FALSE(CssCacheSource::read("source.epub", 23, 131072, identity));
  EXPECT_FALSE(CssCacheSource::read("missing.epub", 23, 131072, identity));
  Storage.reset();
}
