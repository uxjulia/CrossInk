#include <HalStorage.h>
#include <gtest/gtest.h>

#include "LookupHistory.h"

class LookupHistoryTest : public testing::Test {
 protected:
  const std::string path = "/book/dictionary_history.txt";

  void SetUp() override { Storage.reset(); }

  std::string contents() const {
    const auto found = Storage.files.find(path);
    if (found == Storage.files.end()) return {};
    return {found->second->bytes.begin(), found->second->bytes.end()};
  }
};

TEST_F(LookupHistoryTest, AppendShortWriteLeavesPriorRecordsAndNextAppendRepairsTail) {
  ASSERT_TRUE(LookupHistory::addWord("/book", "first", LookupHistory::Status::Direct));
  const std::string original = contents();

  Storage.writeLimit = 3;
  EXPECT_FALSE(LookupHistory::addWord("/book", "interrupted", LookupHistory::Status::Stem));
  EXPECT_EQ(contents().substr(0, original.size()), original);
  EXPECT_EQ(Storage.openHandles, 0);
  auto entries = LookupHistory::load("/book");
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].word, "first");

  ASSERT_TRUE(LookupHistory::addWord("/book", "second", LookupHistory::Status::Suggestion));
  EXPECT_EQ(contents(), "first|D\nsecond|S\n");
}

TEST_F(LookupHistoryTest, AppendCloseAndSyncFailuresKeepEarlierHistory) {
  ASSERT_TRUE(LookupHistory::addWord("/book", "first", LookupHistory::Status::Direct));
  const std::string original = contents();

  Storage.failClose = true;
  EXPECT_FALSE(LookupHistory::addWord("/book", "closing", LookupHistory::Status::Stem));
  EXPECT_EQ(contents().substr(0, original.size()), original);
  EXPECT_EQ(Storage.openHandles, 0);

  Storage.failClose = false;
  Storage.failSync = true;
  EXPECT_FALSE(LookupHistory::addWord("/book", "syncing", LookupHistory::Status::Stem));
  EXPECT_EQ(contents().substr(0, original.size()), original);
  EXPECT_EQ(Storage.openHandles, 0);

  Storage.failSync = false;
  auto entries = LookupHistory::load("/book");
  ASSERT_GE(entries.size(), 1u);
  EXPECT_EQ(entries.back().word, "first");
}

TEST_F(LookupHistoryTest, FailedRewriteKeepsOriginalUntilBackupRecovery) {
  ASSERT_TRUE(LookupHistory::addWord("/book", "first", LookupHistory::Status::Direct));
  ASSERT_TRUE(LookupHistory::addWord("/book", "second", LookupHistory::Status::Stem));
  const std::string original = contents();

  Storage.failWrites = true;
  EXPECT_FALSE(LookupHistory::removeRecentAt("/book", 0));
  EXPECT_EQ(contents(), original);
  EXPECT_EQ(Storage.openHandles, 0);

  Storage.failWrites = false;
  Storage.failClose = true;
  EXPECT_FALSE(LookupHistory::removeRecentAt("/book", 0));
  EXPECT_EQ(contents(), original);
  EXPECT_EQ(Storage.openHandles, 0);

  Storage.failClose = false;
  Storage.failSync = true;
  EXPECT_FALSE(LookupHistory::removeRecentAt("/book", 0));
  EXPECT_EQ(contents(), original);

  Storage.failSync = false;
  Storage.failRenameFrom.insert(path + ".tmp");
  EXPECT_FALSE(LookupHistory::removeRecentAt("/book", 0));
  EXPECT_EQ(contents(), original);
  ASSERT_TRUE(Storage.rename(path.c_str(), (path + ".bak").c_str()));
  auto entries = LookupHistory::load("/book");
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].word, "second");
  EXPECT_EQ(entries[1].word, "first");
  EXPECT_EQ(Storage.openHandles, 0);
}
