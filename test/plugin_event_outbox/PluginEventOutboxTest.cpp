#include <HalStorage.h>
#include <gtest/gtest.h>

#include "util/PluginEventOutbox.h"

namespace {
constexpr char DIRECTORY[] = "/plugins/sleep-stats";
constexpr char QUEUE[] = "/plugins/sleep-stats/events.jsonl";
constexpr char PART[] = "/plugins/sleep-stats/events.jsonl.part";
constexpr char BACKUP[] = "/plugins/sleep-stats/events.jsonl.old";

std::string event(const size_t size, const char fill) { return "{\"e\":\"" + std::string(size - 9, fill) + "\"}\n"; }

bool append(const std::string& line) { return PluginEventOutbox::append(DIRECTORY, line.data(), line.size()); }

class PluginEventOutboxTest : public testing::Test {
 protected:
  void SetUp() override { Storage = {}; }
  void put(const char* path, std::string content) { Storage.files[path].data = std::move(content); }
  const std::string& get(const char* path) { return Storage.files.at(path).data; }
  const std::string newest = event(193, 'x');
};

TEST_F(PluginEventOutboxTest, UsesBoundedReadsAndPreservesReportedBacklogAndNewLine) {
  const auto old = event(1111, 'a') + event(1123, 'b') + event(1047, 'c');
  ASSERT_EQ(old.size(), 3281);
  ASSERT_EQ(newest.size(), 193);
  put(QUEUE, old);
  ASSERT_TRUE(append(newest));
  EXPECT_EQ(get(QUEUE), old + newest);
  EXPECT_LE(Storage.biggestRead, 128);
  EXPECT_FALSE(Storage.exists(BACKUP));
  EXPECT_FALSE(Storage.exists(PART));
  EXPECT_FALSE(Storage.files.at(QUEUE).opened);
}

TEST_F(PluginEventOutboxTest, FirstWriteAndTwoSidesOfRolloverPolicy) {
  ASSERT_TRUE(append(newest));
  EXPECT_EQ(get(QUEUE), newest);
  const auto exact = event(2047, 'a') + event(2049, 'b');
  ASSERT_EQ(exact.size(), PluginEventOutbox::MAX_OUTBOX_BYTES);
  put(QUEUE, exact);
  ASSERT_TRUE(append(newest));
  EXPECT_EQ(get(QUEUE), exact + newest);
  ASSERT_TRUE(append(newest));
  EXPECT_EQ(get(QUEUE), newest);
}

TEST_F(PluginEventOutboxTest, RollsOverAtMaximumAllowedBacklogButDoesNotDestroyOversizedFiles) {
  const auto maximum = std::string(PluginEventOutbox::MAX_OUTBOX_BYTES + PluginEventOutbox::MAX_EVENT_LINE, 'a');
  put(QUEUE, maximum);
  ASSERT_TRUE(append(newest));
  EXPECT_EQ(get(QUEUE), newest);
  put(QUEUE, maximum + "x");
  EXPECT_FALSE(append(newest));
  EXPECT_EQ(get(QUEUE), maximum + "x");
  EXPECT_FALSE(Storage.exists(PART));
}

TEST_F(PluginEventOutboxTest, RejectsInvalidNewEventWithoutChangingExistingQueue) {
  put(QUEUE, newest);
  EXPECT_FALSE(append(""));
  EXPECT_FALSE(append("not terminated"));
  EXPECT_FALSE(append(event(PluginEventOutbox::MAX_EVENT_LINE + 1, 'd')));
  EXPECT_EQ(get(QUEUE), newest);
  EXPECT_FALSE(Storage.exists(PART));
}

TEST_F(PluginEventOutboxTest, FailedStorageOperationPreservesOriginalQueue) {
  const auto old = event(1111, 'a');
  for (auto operation : {"read open", "output open", "short read", "short write old", "short write new", "source close",
                         "sync", "output close", "preserve rename", "publish rename"}) {
    SCOPED_TRACE(operation);
    Storage = {};
    put(QUEUE, old);
    const std::string_view failure = operation;
    if (failure == "read open") Storage.failReadOpen = true;
    if (failure == "output open") Storage.failOutputOpen = true;
    if (failure == "short read") Storage.maxReadable = 141;
    if (failure == "short write old") Storage.maxWritable = 145;
    if (failure == "short write new") Storage.maxWritable = old.size() + newest.size() - 1;
    if (failure == "source close") Storage.failSourceClose = true;
    if (failure == "sync") Storage.failSync = true;
    if (failure == "output close") Storage.failOutputClose = true;
    if (failure == "preserve rename") Storage.failRenameFrom = QUEUE;
    if (failure == "publish rename") Storage.failRenameFrom = PART;
    EXPECT_FALSE(append(newest));
    EXPECT_EQ(get(QUEUE), old);
    EXPECT_FALSE(Storage.exists(BACKUP));
    EXPECT_FALSE(Storage.exists(PART));
  }
}

TEST_F(PluginEventOutboxTest, RecoversInterruptedPublishAndDoesNotReplayPart) {
  const auto old = event(1111, 'a');
  put(BACKUP, old);
  put(PART, event(357, 'p'));
  ASSERT_TRUE(append(newest));
  EXPECT_EQ(get(QUEUE), old + newest);
  EXPECT_FALSE(Storage.exists(BACKUP));
  EXPECT_FALSE(Storage.exists(PART));
}

TEST_F(PluginEventOutboxTest, ACommittedQueueWinsEvenIfBackupCleanupPreviouslyFailed) {
  const auto old = event(1111, 'a');
  put(QUEUE, old);
  Storage.failRemovePath = BACKUP;
  ASSERT_TRUE(append(newest));
  ASSERT_EQ(get(QUEUE), old + newest);
  ASSERT_EQ(get(BACKUP), old);
  ASSERT_FALSE(append(newest));  // Do not modify either file if recovery cannot finish.
  ASSERT_EQ(get(QUEUE), old + newest);
  Storage.failRemovePath.clear();
  ASSERT_TRUE(append(newest));
  EXPECT_EQ(get(QUEUE), old + newest + newest);
}
}  // namespace
