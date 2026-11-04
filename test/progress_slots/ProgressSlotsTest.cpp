#include <gtest/gtest.h>

#include "EpubReaderUtils.h"
#include "TwoSlotFile.h"

class ProgressSlotsTest : public testing::Test {
 protected:
  Epub epub;
  EpubReaderUtils::Progress loaded;
  const std::string path = "/epub/progress.bin";
  void SetUp() override { Storage.reset(); }
};
TEST_F(ProgressSlotsTest, AlternatesAndRestoresOffsetsFromNewestSlot) {
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10, 100));
  const auto first = Storage.data(path).bytes;
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 3, 4, 20, 200));
  EXPECT_EQ(Storage.data(path).bytes, first);
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, loaded));
  EXPECT_EQ(loaded.spineIndex, 3);
  EXPECT_EQ(loaded.pageNumber, 4);
  EXPECT_EQ(loaded.visibleTextOffset, 200u);
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 3, 5, 20));
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, loaded));
  EXPECT_EQ(loaded.pageNumber, 5);
  EXPECT_FALSE(loaded.hasVisibleTextOffset);
}
TEST_F(ProgressSlotsTest, CorruptionAndInterruptedWritesRetainPreviousPosition) {
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 2, 10));
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 3, 10));
  Storage.data(path).writeLimit = 5;
  EXPECT_FALSE(EpubReaderUtils::saveProgress(epub, 1, 4, 10));
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, loaded));
  EXPECT_EQ(loaded.pageNumber, 3);
  Storage.data(path + ".bak").bytes[3] ^= 1;
  EXPECT_FALSE(EpubReaderUtils::loadProgress(epub, loaded));
}
TEST_F(ProgressSlotsTest, SoleLegacySnapshotSurvivesFirstUpgradeFailure) {
  auto legacy = Storage.open(path.c_str(), O_RDWR | O_CREAT);
  const uint8_t bytes[] = {1, 0, 2, 0, 10, 0};
  legacy.write(bytes, sizeof(bytes));
  legacy.close();
  auto backup = Storage.open((path + ".bak").c_str(), O_RDWR | O_CREAT);
  backup.close();
  Storage.data(path + ".bak").writeLimit = 5;
  EXPECT_FALSE(EpubReaderUtils::saveProgress(epub, 1, 3, 10));
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, loaded));
  EXPECT_EQ(loaded.pageNumber, 2);
  Storage.data(path + ".bak").writeLimit = SIZE_MAX;
  ASSERT_TRUE(EpubReaderUtils::saveProgress(epub, 1, 3, 10));
  ASSERT_TRUE(EpubReaderUtils::loadProgress(epub, loaded));
  EXPECT_EQ(loaded.pageNumber, 3);
}
TEST_F(ProgressSlotsTest, SequenceWrapAndUnknownFlags) {
  progress_record::Fields fields{1, 2, 10, true, 0, false};
  uint8_t bytes[progress_record::RECORD_BYTES];
  progress_record::encode(fields, UINT32_MAX, bytes);
  auto primary = progress_record::decode(bytes, sizeof(bytes));
  progress_record::encode(fields, 0, bytes);
  auto backup = progress_record::decode(bytes, sizeof(bytes));
  EXPECT_EQ(progress_record::newest(primary, backup), progress_record::BACKUP);
  EXPECT_EQ(progress_record::target(primary, backup), progress_record::PRIMARY);
  bytes[11] = 2;
  progress_record::writeU32(bytes + 16, progress_record::crc32(bytes, 16));
  EXPECT_EQ(progress_record::decode(bytes, sizeof(bytes)).kind, progress_record::SlotKind::Invalid);
}
TEST_F(ProgressSlotsTest, GlobalSizedPayloadRoundTripsWithoutChangingPayloadBytes) {
  uint8_t bytes[159] = {3};
  bytes[158] = 79;
  ASSERT_TRUE(two_slot::write("TEST", path.c_str(), (path + ".bak").c_str(), bytes, sizeof(bytes)));
  bytes[100] = 17;
  ASSERT_TRUE(two_slot::write("TEST", path.c_str(), (path + ".bak").c_str(), bytes, sizeof(bytes)));
  uint8_t result[two_slot::MAX_PAYLOAD_BYTES] = {};
  ASSERT_EQ(two_slot::read(
                "TEST", path.c_str(), (path + ".bak").c_str(), sizeof(bytes),
                [](const uint8_t* data, size_t size) { return size == 159 && data[0] == 3; }, result),
            sizeof(bytes));
  EXPECT_EQ(memcmp(bytes, result, sizeof(bytes)), 0);
}
