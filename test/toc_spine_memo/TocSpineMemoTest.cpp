#include <gtest/gtest.h>

#include <string>

#include "TocSpineMemo.h"

TEST(TocSpineMemo, AdjacentAnchorsReuseOnlyTheExactSuccessfulSpineLookup) {
  TocSpineMemo memo;
  size_t searches = 0;
  const auto resolve = [&](const std::string_view href) {
    return memo.lookup(href, [&]() -> int16_t {
      ++searches;
      if (href == "OPS/a/ch.xhtml") return 3;
      if (href == "OPS/b/ch.xhtml") return 9;
      return -1;
    });
  };
  // The TOC parser retains the separate anchor in each output entry, but
  // these 18 adjacent entries all resolve the same unfragmented filename.
  for (int anchor = 0; anchor < 18; ++anchor) EXPECT_EQ(resolve("OPS/a/ch.xhtml"), 3);
  EXPECT_EQ(searches, 1U);

  EXPECT_EQ(resolve("OPS/z/ch.xhtml"), -1);
  EXPECT_EQ(resolve("OPS/z/ch.xhtml"), -1);
  EXPECT_EQ(resolve("OPS/a/ch.xhtml"), 3);
  EXPECT_EQ(resolve("OPS/b/ch.xhtml"), 9);
  EXPECT_EQ(resolve("OPS/b/ch.xhtml"), 9);
  EXPECT_EQ(resolve("OPS/a/ch.xhtml"), 3);
  EXPECT_EQ(searches, 6U);
}

TEST(TocSpineMemo, DoesNotTruncateUnknownPathsAndClearsBetweenPasses) {
  TocSpineMemo memo;
  const std::string prefix(64, 'p');
  const std::string longer = prefix + 'a';
  EXPECT_EQ(memo.lookup(prefix, [] { return 3; }), 3);
  EXPECT_EQ(memo.lookup(prefix, [] { return 8; }), 3);
  EXPECT_EQ(memo.lookup(longer, [] { return 9; }), 9);
  EXPECT_EQ(memo.lookup(longer, [] { return 10; }), 10);
  EXPECT_EQ(memo.lookup(prefix, [] { return 11; }), 11);
  memo.clear();
  EXPECT_EQ(memo.lookup(prefix, [] { return 12; }), 12);
}
