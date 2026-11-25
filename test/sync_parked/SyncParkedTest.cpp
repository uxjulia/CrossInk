#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "HalStorage.h"
#include "SyncParked.h"

namespace {
std::string hex(const char digit) { return std::string(32, digit); }

KOReaderProgressChange change(const char digit, const int64_t timestamp, const float percentage = 0.5f) {
  KOReaderProgressChange out;
  out.document = hex(digit);
  out.percentage = percentage;
  out.progress = "/body/DocFragment[2]/body/p[3]/text().0";
  out.device = "Phone";
  out.timestamp = timestamp;
  return out;
}

size_t usedSlots() {
  const auto* bytes = Storage.bytes(SyncParked::PATH);
  if (!bytes) return 0;
  size_t used = 0;
  for (size_t offset = 0; offset + 384 <= bytes->size(); offset += 384) used += (*bytes)[offset] == 1;
  return used;
}

class SyncParkedTest : public ::testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};
}  // namespace

TEST_F(SyncParkedTest, ParksAndFindsByAnyHash) {
  auto parked = change('a', 10, 0.25f);
  parked.aliases[0] = hex('b');
  parked.aliasCount = 1;
  KOReaderRichPosition rich;
  rich.pctQ = 250000;
  rich.spineIndex = 3;
  rich.pageNumber = 4;
  rich.totalPages = 9;
  rich.paragraphIndex = 7;
  parked.position = rich;
  EXPECT_TRUE(SyncParked::empty());
  ASSERT_TRUE(SyncParked::park(parked));
  EXPECT_FALSE(SyncParked::empty());

  SyncParked::Entry entry;
  ASSERT_TRUE(SyncParked::find(hex('b'), entry));
  EXPECT_EQ(entry.hashCount, 2);
  EXPECT_STREQ(entry.device, "Phone");
  const auto progress = entry.toProgress(hex('b'));
  EXPECT_FLOAT_EQ(progress.percentage, 0.25f);
  EXPECT_EQ(progress.progress, parked.progress);
  ASSERT_TRUE(progress.position.has_value());
  EXPECT_EQ(progress.position->spineIndex, 3);
  EXPECT_EQ(progress.position->totalPages, 9);
  EXPECT_EQ(progress.position->paragraphIndex.value_or(0), 7);
  EXPECT_FALSE(SyncParked::find(hex('c'), entry));
}

TEST_F(SyncParkedTest, ANewerChangeReplacesTheSameDocument) {
  ASSERT_TRUE(SyncParked::park(change('a', 10, 0.25f)));
  ASSERT_TRUE(SyncParked::park(change('a', 20, 0.75f)));
  EXPECT_EQ(usedSlots(), 1u);
  SyncParked::Entry entry;
  ASSERT_TRUE(SyncParked::find(hex('a'), entry));
  EXPECT_FLOAT_EQ(entry.percentage, 0.75f);
}

TEST_F(SyncParkedTest, RemoveFreesTheSlotForReuse) {
  ASSERT_TRUE(SyncParked::park(change('a', 10)));
  ASSERT_TRUE(SyncParked::park(change('b', 11)));
  ASSERT_TRUE(SyncParked::remove(hex('a')));
  SyncParked::Entry entry;
  EXPECT_FALSE(SyncParked::find(hex('a'), entry));
  EXPECT_TRUE(SyncParked::remove(hex('a')));  // already gone
  ASSERT_TRUE(SyncParked::park(change('c', 12)));
  EXPECT_EQ(Storage.bytes(SyncParked::PATH)->size(), 2u * 384);  // reused, not grown
  EXPECT_EQ(usedSlots(), 2u);
}

TEST_F(SyncParkedTest, EvictsTheOldestWhenFull) {
  const auto idFor = [](const size_t i) {
    char byte[3];
    snprintf(byte, sizeof(byte), "%02x", static_cast<unsigned>(i));
    std::string id;
    for (int j = 0; j < 16; ++j) id += byte;
    return id;
  };
  for (size_t i = 0; i < SyncParked::MAX_ENTRIES; ++i) {
    auto parked = change('a', 100 + static_cast<int64_t>(i));
    parked.document = idFor(i);
    ASSERT_TRUE(SyncParked::park(parked));
  }
  EXPECT_EQ(usedSlots(), SyncParked::MAX_ENTRIES);
  // Re-parking a known document replaces it and evicts nothing.
  auto again = change('a', 50);
  again.document = idFor(5);
  ASSERT_TRUE(SyncParked::park(again));
  auto extra = change('f', 200);
  ASSERT_TRUE(SyncParked::park(extra));
  EXPECT_EQ(Storage.bytes(SyncParked::PATH)->size(), SyncParked::MAX_ENTRIES * 384);
  SyncParked::Entry entry;
  EXPECT_FALSE(SyncParked::find(idFor(5), entry));  // timestamp 50 was the oldest
  EXPECT_TRUE(SyncParked::find(hex('f'), entry));
  EXPECT_TRUE(SyncParked::find(idFor(0), entry));
}

TEST_F(SyncParkedTest, ATornSlotReadsAsEmpty) {
  ASSERT_TRUE(SyncParked::park(change('a', 10)));
  ASSERT_TRUE(SyncParked::park(change('b', 11)));
  (*Storage.bytes(SyncParked::PATH))[100] ^= 0xFF;  // inside slot 0
  SyncParked::Entry entry;
  EXPECT_FALSE(SyncParked::find(hex('a'), entry));
  EXPECT_TRUE(SyncParked::find(hex('b'), entry));
  ASSERT_TRUE(SyncParked::park(change('c', 12)));  // reuses the torn slot
  EXPECT_EQ(Storage.bytes(SyncParked::PATH)->size(), 2u * 384);
}

TEST_F(SyncParkedTest, DropsBadIdsAndLongPositions) {
  // Dropped, not errors: only a failed write should stop the pull.
  EXPECT_TRUE(SyncParked::park(change('z', 10)));  // not hex
  auto longPosition = change('a', 10);
  longPosition.progress.assign(SyncParked::MAX_XPATH + 1, 'x');
  EXPECT_TRUE(SyncParked::park(longPosition));
  EXPECT_TRUE(SyncParked::empty());
}

TEST_F(SyncParkedTest, ClearRemovesEverything) {
  ASSERT_TRUE(SyncParked::park(change('a', 10)));
  SyncParked::clear();
  EXPECT_TRUE(SyncParked::empty());
  EXPECT_EQ(Storage.bytes(SyncParked::PATH), nullptr);
}

TEST_F(SyncParkedTest, AMergedDocumentReplacesEveryOverlappingSlot) {
  ASSERT_TRUE(SyncParked::park(change('a', 10)));
  ASSERT_TRUE(SyncParked::park(change('b', 11)));
  auto merged = change('a', 20, 0.9f);
  merged.aliases[0] = hex('b');
  merged.aliasCount = 1;
  ASSERT_TRUE(SyncParked::park(merged));
  EXPECT_EQ(usedSlots(), 1u);
  SyncParked::Entry entry;
  ASSERT_TRUE(SyncParked::find(hex('b'), entry));
  EXPECT_FLOAT_EQ(entry.percentage, 0.9f);
  ASSERT_TRUE(SyncParked::remove(hex('b')));
  EXPECT_TRUE(SyncParked::empty());
}

TEST_F(SyncParkedTest, RemovingTheLastEntryDropsTheFile) {
  // Book open checks empty() first: with nothing kept it must not read slots.
  ASSERT_TRUE(SyncParked::park(change('a', 10)));
  ASSERT_TRUE(SyncParked::park(change('b', 11)));
  ASSERT_TRUE(SyncParked::remove(hex('a')));
  ASSERT_NE(Storage.bytes(SyncParked::PATH), nullptr);
  ASSERT_TRUE(SyncParked::remove(hex('b')));
  EXPECT_EQ(Storage.bytes(SyncParked::PATH), nullptr);
  EXPECT_TRUE(SyncParked::empty());
}

TEST_F(SyncParkedTest, ATruncatedTrailingSlotIsOverwritten) {
  ASSERT_TRUE(SyncParked::park(change('a', 10)));
  Storage.bytes(SyncParked::PATH)->resize(384 + 100, 0x5A);  // torn second slot
  ASSERT_TRUE(SyncParked::park(change('b', 11)));
  EXPECT_EQ(Storage.bytes(SyncParked::PATH)->size(), 2u * 384);
  SyncParked::Entry entry;
  EXPECT_TRUE(SyncParked::find(hex('a'), entry));
  EXPECT_TRUE(SyncParked::find(hex('b'), entry));
}
