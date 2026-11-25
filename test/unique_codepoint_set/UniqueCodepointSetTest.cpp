#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "UniqueCodepointSet.h"

namespace {

// The linear-scan collector UniqueCodepointSet replaces. Returns true on cap hit.
bool referenceAdd(const uint32_t cp, std::vector<uint32_t>& out, const uint32_t cap) {
  if (cp == 0) return false;
  if (std::find(out.begin(), out.end(), cp) != out.end()) return false;
  if (out.size() >= cap) return true;
  out.push_back(cp);
  return false;
}

struct Result {
  std::vector<uint32_t> codepoints;
  bool hitCap = false;
};

Result collectWithSet(const std::vector<uint32_t>& input, const uint32_t cap) {
  std::vector<uint32_t> buffer(cap);
  UniqueCodepointSet set(buffer.data(), cap);
  Result result;
  for (const uint32_t cp : input) {
    if (set.add(cp)) {
      result.hitCap = true;
      break;
    }
  }
  const uint32_t count = set.finish();
  result.codepoints.assign(buffer.begin(), buffer.begin() + count);
  return result;
}

Result collectWithReference(const std::vector<uint32_t>& input, const uint32_t cap) {
  Result result;
  for (const uint32_t cp : input) {
    if (referenceAdd(cp, result.codepoints, cap)) {
      result.hitCap = true;
      break;
    }
  }
  std::sort(result.codepoints.begin(), result.codepoints.end());
  return result;
}

void expectMatchesReference(const std::vector<uint32_t>& input, const uint32_t cap) {
  const Result actual = collectWithSet(input, cap);
  const Result expected = collectWithReference(input, cap);
  EXPECT_EQ(actual.hitCap, expected.hitCap);
  EXPECT_EQ(actual.codepoints, expected.codepoints);
}

}  // namespace

TEST(UniqueCodepointSet, EmptyInputYieldsNothing) {
  uint32_t buffer[4] = {};
  UniqueCodepointSet set(buffer, 4);
  EXPECT_EQ(set.finish(), 0u);
}

TEST(UniqueCodepointSet, IgnoresZeroAndDuplicates) {
  expectMatchesReference({0, 'a', 'b', 'a', 0, 'b', 0x4E2D, 0x4E2D, 'a'}, 16);
}

TEST(UniqueCodepointSet, ResultIsSortedWithAsciiFirst) {
  const Result result = collectWithSet({0x6587, 'z', 0x4E2D, ' ', 0xFFFD, 'A'}, 16);
  const std::vector<uint32_t> expected = {' ', 'A', 'z', 0x4E2D, 0x6587, 0xFFFD};
  EXPECT_FALSE(result.hitCap);
  EXPECT_EQ(result.codepoints, expected);
}

TEST(UniqueCodepointSet, CapCountsAsciiAndOtherCodepointsTogether) {
  // Cap 3: two ASCII + one CJK fill it; the next new codepoint of either kind hits it.
  expectMatchesReference({'a', 0x4E2D, 'b', 'a', 0x4E2D, 'c'}, 3);
  expectMatchesReference({'a', 0x4E2D, 'b', 0x6587}, 3);
  // A duplicate arriving after the buffer is full is not a cap hit.
  expectMatchesReference({0x4E2D, 0x6587, 0x8A9E, 0x6587, 0x4E2D, 0x8A9E}, 3);
}

TEST(UniqueCodepointSet, DuplicatesInUnsortedTailAreFoundWhenFull) {
  // Fill the buffer with repeats of a few values, then add one more repeat: the
  // set must compact rather than report a false cap hit.
  std::vector<uint32_t> input;
  for (int i = 0; i < 20; ++i) input.push_back(0x4E00 + (i % 3));
  expectMatchesReference(input, 4);
}

TEST(UniqueCodepointSet, ZeroCapacityReportsCapForAnyCodepoint) {
  expectMatchesReference({'a'}, 0);
  expectMatchesReference({0x4E2D}, 0);
}

TEST(UniqueCodepointSet, MatchesLinearScanOnRandomText) {
  std::mt19937 rng(1234);
  for (const uint32_t cap : {1u, 7u, 64u, 512u, 4096u}) {
    for (int round = 0; round < 20; ++round) {
      std::uniform_int_distribution<int> lengthDist(0, 6000);
      std::uniform_int_distribution<int> kindDist(0, 9);
      std::uniform_int_distribution<uint32_t> asciiDist(0, 127);
      std::uniform_int_distribution<uint32_t> cjkDist(0x4E00, 0x4E00 + 3000);
      std::uniform_int_distribution<uint32_t> wideDist(0x80, 0x10FFFF);
      std::vector<uint32_t> input(lengthDist(rng));
      for (auto& cp : input) {
        const int kind = kindDist(rng);
        cp = kind < 5 ? asciiDist(rng) : (kind < 9 ? cjkDist(rng) : wideDist(rng));
      }
      expectMatchesReference(input, cap);
    }
  }
}
