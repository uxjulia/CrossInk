#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

#include "FirmwareBoardTag.h"

namespace {

constexpr char prefix[] = "CROSSPOINT-BOARD-V1:";

#if FREEINK_DEVICE_EEGO_A4
TEST(FirmwareBoardTag, UsesExpectedA4Identity) {
  EXPECT_STREQ(board_tag::TAG, "CROSSPOINT-BOARD-V1:eego_a4;");
  EXPECT_EQ(std::string(board_tag::boardName(), board_tag::boardNameLen()), "eego_a4");
}
#endif

TEST(FirmwareBoardTag, MatchesRunningBoardAcrossChunkBoundaries) {
  board_tag::Scanner scanner;
  scanner.feed(reinterpret_cast<const uint8_t*>(prefix), sizeof(prefix) - 1);
  scanner.feed(reinterpret_cast<const uint8_t*>(board_tag::boardName()), board_tag::boardNameLen());
  scanner.feed(reinterpret_cast<const uint8_t*>(";"), 1);

  EXPECT_FALSE(scanner.mismatch());
}

TEST(FirmwareBoardTag, RejectsDifferentBoardAcrossChunkBoundaries) {
  board_tag::Scanner scanner;
  constexpr char image[] = "payloadCROSSPOINT-BOARD-V1:x4pro;";
  for (size_t offset = 0; offset < sizeof(image) - 1; offset += 3) {
    const size_t length = std::min<size_t>(3, sizeof(image) - 1 - offset);
    scanner.feed(reinterpret_cast<const uint8_t*>(image + offset), length);
  }

  EXPECT_TRUE(scanner.mismatch());
  EXPECT_STREQ(scanner.foundName(), "x4pro");
}

TEST(FirmwareBoardTag, AllowsUntaggedAndMalformedData) {
  board_tag::Scanner scanner;
  constexpr char image[] = "legacy image\0CROSSPOINT-BOARD-V1:bad\x01;";
  scanner.feed(reinterpret_cast<const uint8_t*>(image), sizeof(image) - 1);

  EXPECT_FALSE(scanner.mismatch());
}

}  // namespace
