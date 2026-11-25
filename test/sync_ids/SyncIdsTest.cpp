#include <gtest/gtest.h>

#include <map>

#include "HalStorage.h"
#include "KOReaderCredentialStore.h"
#include "SyncIds.h"

namespace {
using Map = std::multimap<std::string, uint8_t>;  // path -> first id byte

void id(uint8_t (&out)[16], const uint8_t seed) {
  for (uint8_t i = 0; i < 16; ++i) out[i] = static_cast<uint8_t>(seed + i);
}

Map entries(uint32_t* stale = nullptr) {
  Map out;
  SyncIds::forEach(
      [](const uint8_t (&stored)[16], const std::string& path, void* context) {
        static_cast<Map*>(context)->emplace(path, stored[0]);
        return true;
      },
      &out, nullptr, stale);
  return out;
}

class SyncIdsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    KOREADER_STORE = {};
  }
};
}  // namespace

TEST_F(SyncIdsTest, RecordsOncePerPathAndId) {
  uint8_t a[16];
  id(a, 1);
  const uint32_t generation = SyncIds::record("/a.epub", a);
  ASSERT_NE(generation, 0u);
  EXPECT_EQ(SyncIds::record("/a.epub", a), generation);
  EXPECT_EQ(SyncIds::generation(), generation);
  EXPECT_EQ(entries(), (Map{{"/a.epub", 1}}));
}

TEST_F(SyncIdsTest, ANewIdForAPathReplacesTheOldOne) {
  uint8_t a[16], b[16];
  id(a, 1);
  id(b, 2);
  ASSERT_TRUE(SyncIds::record("/a.epub", a));
  ASSERT_TRUE(SyncIds::record("/a.epub", b));
  EXPECT_EQ(entries(), (Map{{"/a.epub", 2}}));
}

TEST_F(SyncIdsTest, CopiesShareAnId) {
  uint8_t a[16];
  id(a, 1);
  ASSERT_TRUE(SyncIds::record("/a.epub", a));
  ASSERT_TRUE(SyncIds::record("/copy/a.epub", a));
  EXPECT_EQ(entries().size(), 2u);
}

TEST_F(SyncIdsTest, RenameAndRemoveFollowTheBook) {
  uint8_t a[16], b[16];
  id(a, 1);
  id(b, 2);
  ASSERT_TRUE(SyncIds::record("/a.epub", a));
  ASSERT_TRUE(SyncIds::record("/b.epub", b));
  const uint32_t generation = SyncIds::generation();
  ASSERT_TRUE(SyncIds::renamePath("/a.epub", "/Read/a.epub"));
  ASSERT_TRUE(SyncIds::removePath("/b.epub"));
  EXPECT_EQ(entries(), (Map{{"/Read/a.epub", 1}}));
  EXPECT_EQ(SyncIds::generation(), generation);  // same file incarnation
}

TEST_F(SyncIdsTest, ATornAppendOnlyLosesThatEntry) {
  uint8_t a[16], b[16], c[16], d[16];
  id(a, 1);
  id(b, 2);
  id(c, 3);
  id(d, 4);
  ASSERT_TRUE(SyncIds::record("/a.epub", a));
  ASSERT_TRUE(SyncIds::record("/b.epub", b));
  Storage.bytes(SyncIds::PATH)->resize(Storage.bytes(SyncIds::PATH)->size() - 5);  // power lost mid-append
  EXPECT_EQ(entries(), (Map{{"/a.epub", 1}}));
  // Later appends must not land behind the partial entry.
  ASSERT_TRUE(SyncIds::record("/c.epub", c));
  ASSERT_TRUE(SyncIds::record("/d.epub", d));
  EXPECT_EQ(entries(), (Map{{"/a.epub", 1}, {"/c.epub", 3}, {"/d.epub", 4}}));
}

TEST_F(SyncIdsTest, EditsForUnrecordedBooksWriteNothing) {
  uint8_t a[16];
  id(a, 1);
  ASSERT_TRUE(SyncIds::record("/a.epub", a));
  ASSERT_TRUE(SyncIds::removePath("/never-recorded.epub"));
  ASSERT_TRUE(SyncIds::renamePath("/also-not.xtc", "/moved.xtc"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/sync_ids.bin.bak"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/sync_ids.bin.tmp"));
}

TEST_F(SyncIdsTest, RemovingAFolderDropsEveryBookInIt) {
  uint8_t a[16], b[16], c[16];
  id(a, 1);
  id(b, 2);
  id(c, 3);
  ASSERT_TRUE(SyncIds::record("/Old/a.epub", a));
  ASSERT_TRUE(SyncIds::record("/Old/Sub/b.epub", b));
  ASSERT_TRUE(SyncIds::record("/Older/c.epub", c));
  ASSERT_TRUE(SyncIds::removeUnder("/Old"));
  EXPECT_EQ(entries(), (Map{{"/Older/c.epub", 3}}));
}

TEST_F(SyncIdsTest, ACrashBetweenTheSwapRenamesKeepsTheEntries) {
  uint8_t a[16], b[16];
  id(a, 1);
  id(b, 2);
  ASSERT_TRUE(SyncIds::record("/a.epub", a));
  ASSERT_TRUE(SyncIds::record("/b.epub", b));
  const uint32_t generation = SyncIds::generation();
  // Old file already rotated away, complete new one still named .tmp.
  ASSERT_TRUE(Storage.rename(SyncIds::PATH, "/.crosspoint/sync_ids.bin.tmp"));
  EXPECT_EQ(SyncIds::generation(), generation);
  EXPECT_EQ(entries().size(), 2u);
}

TEST_F(SyncIdsTest, AnotherMatchMethodMeansAFreshMap) {
  uint8_t a[16];
  id(a, 1);
  const uint32_t generation = SyncIds::record("/a.epub", a);
  KOREADER_STORE.method = DocumentMatchMethod::BINARY;
  EXPECT_EQ(SyncIds::generation(), 0u);
  EXPECT_TRUE(entries().empty());
  const uint32_t binary = SyncIds::record("/a.epub", a);
  EXPECT_NE(binary, 0u);
  EXPECT_NE(binary, generation);
}

TEST_F(SyncIdsTest, CompactionDropsBooksThatAreGone) {
  uint8_t a[16], b[16];
  id(a, 1);
  id(b, 2);
  HalFile file;
  ASSERT_TRUE(Storage.openFileForWrite("t", "/a.epub", file));
  file.close();
  ASSERT_TRUE(SyncIds::record("/a.epub", a));
  ASSERT_TRUE(SyncIds::record("/gone.epub", b));
  uint32_t stale = 0;
  EXPECT_EQ(entries(&stale).size(), 2u);
  EXPECT_EQ(stale, 1u);
  const uint32_t before = SyncIds::generation();
  ASSERT_TRUE(SyncIds::compact());
  EXPECT_EQ(entries(), (Map{{"/a.epub", 1}}));
  // Dropped books must record themselves again later.
  EXPECT_NE(SyncIds::generation(), before);
}
