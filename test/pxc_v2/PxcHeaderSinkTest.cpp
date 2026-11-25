#include <gtest/gtest.h>

#include "HalStorage.h"
#include "lib/Epub/Epub/image/PxcHeaderSink.h"

TEST(PxcHeaderSink, PreservesExactBytesAcrossEveryHeaderSplit) {
  const std::vector<uint8_t> bytes = {0x20, 0x03, 0xe0, 0x01, 0x1b, 0xe4, 0x55};
  for (size_t split = 0; split <= bytes.size(); ++split) {
    Storage.reset();
    HalFile output;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", "/out", output));
    PxcHeaderSink sink(output);
    ASSERT_EQ(sink.write(bytes.data(), split), split);
    ASSERT_EQ(sink.write(bytes.data() + split, bytes.size() - split), bytes.size() - split);
    EXPECT_EQ(sink.width(), 800);
    EXPECT_EQ(sink.height(), 480);
    output.close();
    EXPECT_EQ(Storage.bytes("/out"), bytes);
  }
}

TEST(PxcHeaderSink, RejectsIncompleteHeaderAndReportsShortWrites) {
  auto data = std::make_shared<HostFileData>();
  data->failAt = 3;
  HalFile output(data);
  PxcHeaderSink sink(output);
  const uint8_t bytes[] = {0x20, 0x03, 0xe0, 0x01};
  EXPECT_EQ(sink.write(bytes, sizeof(bytes)), 3u);
  EXPECT_EQ(sink.width(), 0);
  EXPECT_EQ(sink.height(), 0);
  EXPECT_EQ(sink.write(uint8_t{1}), 0u);
  output.close();
}
