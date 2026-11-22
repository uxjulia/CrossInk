#include <gtest/gtest.h>

#include <limits>

#include "SleepProgressPolicy.h"

using SleepProgressPolicy::mayUpload;
using SleepProgressPolicy::Remote;

TEST(SleepProgressPolicy, FirstPositionRequiresNoRemoteRecord) {
  EXPECT_TRUE(mayUpload(0.0f, Remote::Absent, 0.0f));
  EXPECT_TRUE(mayUpload(0.5f, Remote::Absent, 0.0f));
  EXPECT_FALSE(mayUpload(0.5f, Remote::Unavailable, 0.0f));
}

TEST(SleepProgressPolicy, DoesNotOverwriteEqualOrFurtherRemote) {
  EXPECT_FALSE(mayUpload(0.2f, Remote::Present, 0.8f));
  EXPECT_FALSE(mayUpload(0.5f, Remote::Present, 0.5f));
  EXPECT_FALSE(mayUpload(0.5005f, Remote::Present, 0.5f));
  EXPECT_TRUE(mayUpload(0.502f, Remote::Present, 0.5f));
  EXPECT_TRUE(mayUpload(1.0f, Remote::Present, 0.75f));
}

TEST(SleepProgressPolicy, RejectsInvalidPercentages) {
  for (float bad : {-0.01f, 1.01f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
    EXPECT_FALSE(mayUpload(bad, Remote::Absent, 0.0f));
    EXPECT_FALSE(mayUpload(bad, Remote::Present, 0.5f));
    EXPECT_FALSE(mayUpload(0.5f, Remote::Present, bad));
  }
}
