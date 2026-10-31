#include <gtest/gtest.h>

#include "SyncFiles.h"

namespace {
constexpr const char* PATH = "/.crosspoint/sync_test.bin";

bool writeValue(const uint32_t value, const std::string& text) {
  SyncFiles::Writer writer(PATH);
  return writer.open() && writer.pod(value) && writer.string16(text) && writer.commit();
}

bool readValue(uint32_t& value, std::string& text, SyncFiles::Reader::Source* source = nullptr) {
  SyncFiles::Reader reader(PATH);
  const auto opened = reader.open();
  if (source) *source = opened;
  return opened != SyncFiles::Reader::Source::None && reader.pod(value) && reader.string16(text, 64) && reader.atEnd();
}

class SyncFilesTest : public ::testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};
}  // namespace

TEST_F(SyncFilesTest, RoundTripsAndKeepsThePreviousVersionAsBackup) {
  ASSERT_TRUE(writeValue(1, "first"));
  ASSERT_TRUE(writeValue(2, "second"));
  uint32_t value = 0;
  std::string text;
  SyncFiles::Reader::Source source;
  ASSERT_TRUE(readValue(value, text, &source));
  EXPECT_EQ(source, SyncFiles::Reader::Source::Main);
  EXPECT_EQ(value, 2u);
  EXPECT_EQ(text, "second");
  EXPECT_TRUE(Storage.exists("/.crosspoint/sync_test.bin.bak"));
  EXPECT_FALSE(Storage.exists("/.crosspoint/sync_test.bin.tmp"));
}

TEST_F(SyncFilesTest, CorruptMainFallsBackToBackup) {
  ASSERT_TRUE(writeValue(1, "first"));
  ASSERT_TRUE(writeValue(2, "second"));
  (*Storage.bytes(PATH))[2] ^= 0xFF;
  uint32_t value = 0;
  std::string text;
  SyncFiles::Reader::Source source;
  ASSERT_TRUE(readValue(value, text, &source));
  EXPECT_EQ(source, SyncFiles::Reader::Source::Backup);
  EXPECT_EQ(value, 1u);
}

TEST_F(SyncFilesTest, TruncatedFilesAndMissingBackupReadAsLostNotMissing) {
  ASSERT_TRUE(writeValue(1, "only"));
  Storage.bytes(PATH)->resize(5);
  SyncFiles::Reader reader(PATH);
  bool missing = true;
  EXPECT_EQ(reader.open(&missing), SyncFiles::Reader::Source::None);
  EXPECT_FALSE(missing);
}

TEST_F(SyncFilesTest, NeverWrittenReadsAsMissing) {
  SyncFiles::Reader reader(PATH);
  bool missing = false;
  EXPECT_EQ(reader.open(&missing), SyncFiles::Reader::Source::None);
  EXPECT_TRUE(missing);
}

TEST_F(SyncFilesTest, FailedSwapKeepsTheCurrentFile) {
  ASSERT_TRUE(writeValue(1, "kept"));
  Storage.failNextRenameFrom("/.crosspoint/sync_test.bin.tmp");
  EXPECT_FALSE(writeValue(2, "lost"));
  uint32_t value = 0;
  std::string text;
  ASSERT_TRUE(readValue(value, text));
  EXPECT_EQ(value, 1u);
  EXPECT_EQ(text, "kept");
}

TEST_F(SyncFilesTest, ReadsNeverCrossIntoTheChecksum) {
  ASSERT_TRUE(writeValue(7, "x"));
  SyncFiles::Reader reader(PATH);
  ASSERT_NE(reader.open(), SyncFiles::Reader::Source::None);
  uint32_t value = 0;
  std::string text;
  ASSERT_TRUE(reader.pod(value) && reader.string16(text, 8));
  uint32_t extra = 0;
  EXPECT_FALSE(reader.pod(extra));
}

TEST_F(SyncFilesTest, AbandonedWriteLeavesNoTempFile) {
  ASSERT_TRUE(writeValue(1, "kept"));
  {
    SyncFiles::Writer writer(PATH);
    ASSERT_TRUE(writer.open() && writer.pod(uint32_t{9}));
  }
  EXPECT_FALSE(Storage.exists("/.crosspoint/sync_test.bin.tmp"));
  uint32_t value = 0;
  std::string text;
  ASSERT_TRUE(readValue(value, text));
  EXPECT_EQ(value, 1u);
}

TEST_F(SyncFilesTest, CrashBeforeTheFinalRenameKeepsTheNewestWrite) {
  ASSERT_TRUE(writeValue(1, "old"));
  ASSERT_TRUE(writeValue(2, "new"));
  // Power lost after the old file became .bak but before .tmp took its name.
  ASSERT_TRUE(Storage.rename(PATH, "/.crosspoint/sync_test.bin.tmp"));
  uint32_t value = 0;
  std::string text;
  ASSERT_TRUE(readValue(value, text));
  EXPECT_EQ(value, 2u);
  EXPECT_TRUE(Storage.exists(PATH));  // promoted, so the next write can't discard it
  EXPECT_FALSE(Storage.exists("/.crosspoint/sync_test.bin.tmp"));
}

TEST_F(SyncFilesTest, RecoveringFromTheBackupKeepsItUntilTheNewFileIsInPlace) {
  ASSERT_TRUE(writeValue(1, "old"));
  ASSERT_TRUE(writeValue(2, "new"));
  ASSERT_TRUE(Storage.remove(PATH));
  ASSERT_TRUE(writeValue(3, "rewritten"));
  EXPECT_TRUE(Storage.exists("/.crosspoint/sync_test.bin.bak"));
  uint32_t value = 0;
  std::string text;
  ASSERT_TRUE(readValue(value, text));
  EXPECT_EQ(value, 3u);
}

TEST_F(SyncFilesTest, FailedReplacementOfCorruptMainRetainsReadableBackup) {
  ASSERT_TRUE(writeValue(1, "kept"));
  ASSERT_TRUE(writeValue(2, "damaged"));
  (*Storage.bytes(PATH))[2] ^= 0xff;
  Storage.failNextRenameFrom(std::string(PATH) + ".tmp");
  EXPECT_FALSE(writeValue(3, "replacement"));
  uint32_t value = 0;
  std::string text;
  ASSERT_TRUE(readValue(value, text));
  EXPECT_EQ(text, "kept");
  ASSERT_TRUE(writeValue(3, "replacement"));
  ASSERT_TRUE(readValue(value, text));
  EXPECT_EQ(text, "replacement");
}
