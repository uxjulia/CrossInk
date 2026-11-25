#include <BufferedFile.h>
#include <PrintSerialization.h>
#include <Serialization.h>
#include <gtest/gtest.h>

#include <array>
#include <string>

namespace {
using serialization::BufferedFileReader;
using serialization::BufferedFileWriter;

class BufferedFileTest : public testing::TestWithParam<size_t> {};

TEST_P(BufferedFileTest, MixedWritesPreserveBytesAndLogicalPosition) {
  HalFile file;
  file.bytes = {'x', 'y'};
  file.cursor = 2;
  BufferedFileWriter writer(file, GetParam());
  writer.write("abc", 3);
  EXPECT_EQ(writer.position(), 5U);
  writer.write("0123456789", 10);
  writer.write("z", 1);
  EXPECT_EQ(writer.position(), 16U);
  ASSERT_TRUE(writer.flush());
  EXPECT_EQ(std::string(file.bytes.begin(), file.bytes.end()), "xyabc0123456789z");
  const auto writes = file.writes;
  EXPECT_TRUE(writer.flush());
  EXPECT_EQ(file.writes, writes);
}

TEST_P(BufferedFileTest, DestructorFlushesPendingBytes) {
  HalFile file;
  {
    BufferedFileWriter writer(file, GetParam());
    writer.write("abc", 3);
  }
  EXPECT_EQ(std::string(file.bytes.begin(), file.bytes.end()), "abc");
}

TEST_P(BufferedFileTest, ShortWriteFailureRemainsStickyAfterRecovery) {
  HalFile file;
  file.writeLimit = 1;
  BufferedFileWriter writer(file, GetParam());
  writer.write("abc", 3);
  EXPECT_FALSE(writer.flush());
  file.writeLimit = 100;
  writer.write("d", 1);
  EXPECT_FALSE(writer.flush());
}

TEST_P(BufferedFileTest, ReadsAcrossBoundariesAndStopsAtEof) {
  HalFile file;
  file.bytes = {'x', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
  file.cursor = 1;
  BufferedFileReader reader(file, GetParam());
  std::array<char, 12> out{};
  ASSERT_EQ(reader.read(out.data(), 3), 3U);
  EXPECT_EQ(std::string(out.data(), 3), "abc");
  EXPECT_EQ(reader.position(), 4U);
  ASSERT_EQ(reader.read(out.data(), out.size()), 5U);
  EXPECT_EQ(std::string(out.data(), 5), "defgh");
  EXPECT_EQ(reader.position(), 9U);
  EXPECT_EQ(reader.read(out.data(), 1), 0U);
  EXPECT_EQ(reader.position(), 9U);
}

TEST_P(BufferedFileTest, ReadErrorDoesNotAdvanceAndCanRecover) {
  HalFile file;
  file.bytes = {'a', 'b'};
  file.readError = true;
  BufferedFileReader reader(file, GetParam());
  char out = '!';
  EXPECT_EQ(reader.read(&out, 1), 0U);
  EXPECT_EQ(out, '!');
  EXPECT_EQ(reader.position(), 0U);
  file.readError = false;
  EXPECT_EQ(reader.read(&out, 1), 1U);
  EXPECT_EQ(out, 'a');
}

TEST_P(BufferedFileTest, FailedExternalSeekPreservesUnreadData) {
  HalFile file;
  file.bytes = {'a', 'b', 'c', 'd', 'e', 'f'};
  BufferedFileReader reader(file, GetParam());
  char out{};
  ASSERT_EQ(reader.read(&out, 1), 1U);
  file.seekError = true;
  EXPECT_FALSE(reader.seek(100));
  EXPECT_EQ(reader.position(), 1U);
  ASSERT_EQ(reader.read(&out, 1), 1U);
  EXPECT_EQ(out, 'b');
}

INSTANTIATE_TEST_SUITE_P(BufferSizes, BufferedFileTest, testing::Values(0U, 1U, 4U, 16U));

TEST(BufferedFile, BufferedSeekAvoidsDeviceSeekAndExternalSeekInvalidatesWindow) {
  HalFile file;
  file.bytes = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
  BufferedFileReader reader(file, 4);
  char out{};
  ASSERT_EQ(reader.read(&out, 1), 1U);
  ASSERT_TRUE(reader.seek(3));
  EXPECT_EQ(file.seeks, 0U);
  ASSERT_EQ(reader.read(&out, 1), 1U);
  EXPECT_EQ(out, 'd');
  ASSERT_TRUE(reader.seek(0));
  ASSERT_EQ(reader.read(&out, 1), 1U);
  EXPECT_EQ(out, 'a');
  ASSERT_TRUE(reader.seek(4));
  EXPECT_EQ(file.seeks, 1U);
  ASSERT_EQ(reader.read(&out, 1), 1U);
  EXPECT_EQ(out, 'e');
  EXPECT_EQ(reader.position(), 5U);
}

TEST(BufferedFile, ShortDeviceReadsAreAssembledUntilRequestIsSatisfied) {
  HalFile file;
  file.bytes = {'a', 'b', 'c', 'd', 'e'};
  file.readLimit = 2;
  BufferedFileReader reader(file, 4);
  char out[5]{};
  ASSERT_EQ(reader.read(out, sizeof(out)), sizeof(out));
  EXPECT_EQ(std::string(out, sizeof(out)), "abcde");
  EXPECT_EQ(reader.position(), 5U);
}

TEST(BufferedFile, BufferedSerializationMatchesExistingWireFormat) {
  HalFile file;
  {
    BufferedFileWriter writer(file, 3);
    serialization::writePod(writer, uint32_t{0x12345678});
    serialization::writeString(writer, std::string("a\0b", 3));
    serialization::writeString(writer, "");
    ASSERT_TRUE(writer.flush());
  }
  const std::vector<uint8_t> expected = {0x78, 0x56, 0x34, 0x12, 3, 0, 0, 0, 'a', 0, 'b', 0, 0, 0, 0};
  EXPECT_EQ(file.bytes, expected);
  file.cursor = 0;
  BufferedFileReader reader(file, 2);
  uint32_t number = 0;
  std::string value;
  serialization::readPod(reader, number);
  EXPECT_EQ(number, 0x12345678U);
  serialization::readString(reader, value);
  EXPECT_EQ(value, std::string("a\0b", 3));
  serialization::readString(reader, value);
  EXPECT_TRUE(value.empty());
}

TEST(Serialization, CheckedStringReadRejectsTruncatedLengthWithoutChangingOutput) {
  for (size_t length = 0; length < 4; ++length) {
    HalFile file;
    file.bytes.assign(length, 0);
    std::string value = "existing";
    EXPECT_FALSE(serialization::tryReadString(file, value));
    EXPECT_EQ(value, "existing");
  }
}

TEST(Serialization, CheckedStringReadRejectsUnrepresentableLength) {
  HalFile file;
  file.bytes = {0xff, 0xff, 0xff, 0xff};
  std::string value = "existing";
  EXPECT_FALSE(serialization::tryReadString(file, value));
  EXPECT_EQ(value, "existing");
}

TEST(Serialization, CheckedStringReadRejectsTruncatedPayload) {
  HalFile file;
  file.bytes = {3, 0, 0, 0, 'a', 'b'};
  std::string value;
  EXPECT_FALSE(serialization::tryReadString(file, value));
}

TEST(Serialization, CheckedStringWriteStopsAfterShortLengthWrite) {
  HalFile file;
  file.writeLimit = 2;
  EXPECT_FALSE(serialization::tryWriteString(file, "abc"));
  EXPECT_EQ(file.writes, 1U);
}
}  // namespace

TEST(BufferedFilePrint, StagedPrintWritesMatchDirectWritesWithFewerDeviceCalls) {
  const auto serializeFields = [](Print& out) {
    EXPECT_TRUE(serialization::tryWritePod(out, uint16_t{0x0102}));
    EXPECT_TRUE(serialization::tryWritePod(out, uint8_t{7}));
    EXPECT_TRUE(serialization::tryWriteString(out, "ruby"));
    const std::string arena(40, 'q');
    EXPECT_EQ(out.write(reinterpret_cast<const uint8_t*>(arena.data()), arena.size()), arena.size());
    for (int16_t i = 0; i < 20; ++i) EXPECT_TRUE(serialization::tryWritePod(out, i));
  };

  class FilePrint : public Print {
   public:
    explicit FilePrint(HalFile& file) : file(file) {}
    size_t write(uint8_t value) override { return file.write(&value, 1); }
    size_t write(const uint8_t* data, size_t length) override { return file.write(data, length); }
    HalFile& file;
  };

  HalFile direct;
  FilePrint directOut(direct);
  serializeFields(directOut);

  std::array<uint8_t, 16> stage{};
  HalFile staged;
  serialization::BufferedFilePrint stagedOut(staged, stage.data(), stage.size());
  serializeFields(stagedOut);
  ASSERT_TRUE(stagedOut.commit());

  EXPECT_EQ(staged.bytes, direct.bytes);
  EXPECT_LT(staged.writes, direct.writes / 2);
}

TEST(BufferedFilePrint, NullBufferPassesThrough) {
  HalFile file;
  serialization::BufferedFilePrint out(file, nullptr, 64);
  EXPECT_TRUE(serialization::tryWritePod(out, uint32_t{0xA1B2C3D4}));
  EXPECT_EQ(file.writes, 1U);
  EXPECT_TRUE(out.commit());
  const std::vector<uint8_t> expected = {0xD4, 0xC3, 0xB2, 0xA1};
  EXPECT_EQ(file.bytes, expected);
}

TEST(BufferedFilePrint, ShortDeviceWriteIsReportedByCommitAndStopsLaterWrites) {
  HalFile file;
  file.writeLimit = 3;
  std::array<uint8_t, 8> stage{};
  serialization::BufferedFilePrint out(file, stage.data(), stage.size());
  EXPECT_TRUE(serialization::tryWritePod(out, uint32_t{1}));
  EXPECT_TRUE(serialization::tryWritePod(out, uint32_t{2}));
  // The staged bytes flush when the buffer fills; the device accepts only 3.
  EXPECT_FALSE(serialization::tryWritePod(out, uint32_t{3}));
  EXPECT_FALSE(out.commit());
}
