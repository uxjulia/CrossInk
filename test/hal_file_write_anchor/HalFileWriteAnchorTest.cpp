#include <HalFileWriteAnchor.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {
struct FakeFile {
  size_t maxWrite = SIZE_MAX;
  bool failSync = false;
  int syncsBeforeFailure = -1;
  std::vector<size_t> writes;
  std::vector<std::string> events;
  std::string bytes;

  size_t write(const void* input, const size_t size) {
    writes.push_back(size);
    events.push_back("write");
    const size_t written = std::min(size, maxWrite);
    bytes.append(static_cast<const char*>(input), written);
    return written;
  }

  bool sync() {
    events.push_back("sync");
    if (failSync || syncsBeforeFailure == 0) {
      syncsBeforeFailure = -1;
      return false;
    }
    if (syncsBeforeFailure > 0) --syncsBeforeFailure;
    return true;
  }
};
}  // namespace

TEST(HalFileWriteAnchorTest, LargeFirstWritePersistsTheInitialSectorBeforeTheRemainder) {
  FakeFile file;
  bool pending = hal_file_write_anchor::required(true, 0);
  bool growthEnabled = pending;
  bool failed = false;
  size_t bytesSinceSync = 0;
  const std::string payload(1400, 'x');

  EXPECT_EQ(hal_file_write_anchor::write(file, pending, growthEnabled, failed, bytesSinceSync, payload.data(),
                                         payload.size()),
            payload.size());
  EXPECT_EQ(file.events, (std::vector<std::string>{"write", "sync", "write"}));
  EXPECT_EQ(file.writes, (std::vector<size_t>{512, payload.size() - 512}));
  EXPECT_EQ(file.bytes, payload);
  EXPECT_FALSE(pending);
  EXPECT_FALSE(failed);

  const std::string more = "next block";
  EXPECT_EQ(
      hal_file_write_anchor::write(file, pending, growthEnabled, failed, bytesSinceSync, more.data(), more.size()),
      more.size());
  EXPECT_EQ(file.events.size(), 4u);
  EXPECT_EQ(file.events.back(), "write");
}

TEST(HalFileWriteAnchorTest, OneByteFirstWriteIsAnchoredWithoutSyncingEveryByte) {
  FakeFile file;
  bool pending = hal_file_write_anchor::required(true, 0);
  bool growthEnabled = pending;
  bool failed = false;
  size_t bytesSinceSync = 0;
  const uint8_t first = 'A';
  const uint8_t second = 'B';

  EXPECT_EQ(hal_file_write_anchor::write(file, pending, growthEnabled, failed, bytesSinceSync, &first, 1), 1u);
  EXPECT_EQ(file.events, (std::vector<std::string>{"write", "sync"}));
  EXPECT_EQ(hal_file_write_anchor::write(file, pending, growthEnabled, failed, bytesSinceSync, &second, 1), 1u);
  EXPECT_EQ(file.events, (std::vector<std::string>{"write", "sync", "write"}));
}

TEST(HalFileWriteAnchorTest, AnchorSyncFailureStopsThePayloadAndCannotBeRetriedAway) {
  FakeFile file;
  file.failSync = true;
  bool pending = hal_file_write_anchor::required(true, 0);
  bool growthEnabled = pending;
  bool failed = false;
  size_t bytesSinceSync = 0;
  const std::string payload(1400, 'x');

  EXPECT_EQ(hal_file_write_anchor::write(file, pending, growthEnabled, failed, bytesSinceSync, payload.data(),
                                         payload.size()),
            0u);
  EXPECT_EQ(file.events, (std::vector<std::string>{"write", "sync"}));
  EXPECT_TRUE(pending);
  EXPECT_TRUE(failed);
  file.failSync = false;
  EXPECT_EQ(hal_file_write_anchor::write(file, pending, growthEnabled, failed, bytesSinceSync, payload.data(),
                                         payload.size()),
            0u);
  EXPECT_EQ(file.events, (std::vector<std::string>{"write", "sync"}));
}

TEST(HalFileWriteAnchorTest, ShortAnchorWriteSyncsAndDoesNotContinue) {
  FakeFile file;
  file.maxWrite = 128;
  bool pending = hal_file_write_anchor::required(true, 0);
  bool growthEnabled = pending;
  bool failed = false;
  size_t bytesSinceSync = 0;
  const std::string payload(1400, 'x');

  EXPECT_EQ(hal_file_write_anchor::write(file, pending, growthEnabled, failed, bytesSinceSync, payload.data(),
                                         payload.size()),
            128u);
  EXPECT_EQ(file.events, (std::vector<std::string>{"write", "sync"}));
  EXPECT_TRUE(pending);
  EXPECT_TRUE(failed);
}

TEST(HalFileWriteAnchorTest, ExistingFilesAndReadOnlyHandlesDoNotNeedAnAnchor) {
  EXPECT_FALSE(hal_file_write_anchor::required(false, 0));
  EXPECT_FALSE(hal_file_write_anchor::required(true, 4096));

  FakeFile file;
  bool pending = hal_file_write_anchor::required(true, 4096);
  bool growthEnabled = pending;
  bool failed = false;
  size_t bytesSinceSync = 0;
  const std::string replacement = "overwrite from offset zero";
  EXPECT_EQ(hal_file_write_anchor::write(file, pending, growthEnabled, failed, bytesSinceSync, replacement.data(),
                                         replacement.size()),
            replacement.size());
  EXPECT_EQ(file.events, (std::vector<std::string>{"write"}));
  EXPECT_FALSE(failed);
}

TEST(HalFileWriteAnchorTest, GrowthCheckpointBoundsLargeWritesAndLatchesBoundarySyncFailure) {
  FakeFile file;
  bool pending = hal_file_write_anchor::required(true, 0);
  bool growthEnabled = pending;
  bool failed = false;
  size_t bytesSinceSync = 0;
  const uint8_t anchor = 'A';
  const std::string growth(hal_file_write_anchor::GROWTH_SYNC_BYTES, 'x');

  ASSERT_EQ(hal_file_write_anchor::write(file, pending, growthEnabled, failed, bytesSinceSync, &anchor, 1), 1u);
  file.syncsBeforeFailure = 0;
  EXPECT_EQ(
      hal_file_write_anchor::write(file, pending, growthEnabled, failed, bytesSinceSync, growth.data(), growth.size()),
      0u);
  EXPECT_EQ(file.events, (std::vector<std::string>{"write", "sync", "write", "sync"}));
  EXPECT_TRUE(failed);
  EXPECT_EQ(hal_file_write_anchor::write(file, pending, growthEnabled, failed, bytesSinceSync, &anchor, 1), 0u);
  EXPECT_EQ(file.events.size(), 4u);
}

TEST(HalFileWriteAnchorTest, ShortGrowthWriteLatchesEvenWhenItsSyncSucceeds) {
  FakeFile file;
  file.maxWrite = 128;
  bool pending = false;
  const bool growthEnabled = true;
  bool failed = false;
  size_t bytesSinceSync = 100;
  const std::string payload(1024, 'x');

  EXPECT_EQ(hal_file_write_anchor::write(file, pending, growthEnabled, failed, bytesSinceSync, payload.data(),
                                         payload.size()),
            128u);
  EXPECT_EQ(file.events, (std::vector<std::string>{"write", "sync"}));
  EXPECT_TRUE(failed);
  EXPECT_EQ(bytesSinceSync, 0u);
  EXPECT_EQ(hal_file_write_anchor::write(file, pending, growthEnabled, failed, bytesSinceSync, payload.data(),
                                         payload.size()),
            0u);
  EXPECT_EQ(file.events.size(), 2u);
}

TEST(HalFileWriteAnchorTest, ManySmallWritesShareOneGrowthCheckpoint) {
  FakeFile file;
  bool pending = hal_file_write_anchor::required(true, 0);
  bool growthEnabled = pending;
  bool failed = false;
  size_t bytesSinceSync = 0;
  const uint8_t byte = 'x';

  ASSERT_EQ(hal_file_write_anchor::write(file, pending, growthEnabled, failed, bytesSinceSync, &byte, 1), 1u);
  for (size_t index = 0; index < hal_file_write_anchor::GROWTH_SYNC_BYTES; ++index) {
    ASSERT_EQ(hal_file_write_anchor::write(file, pending, growthEnabled, failed, bytesSinceSync, &byte, 1), 1u);
  }
  EXPECT_EQ(bytesSinceSync, 0u);
  EXPECT_EQ(std::count(file.events.begin(), file.events.end(), "sync"), 2);
}
