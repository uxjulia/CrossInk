#include <Logging.h>
#include <gtest/gtest.h>

extern void addToLogRingBuffer(const char* message);

TEST(LogRetention, BootMessagesCannotOverwritePendingCrashLogs) {
  clearLastLogs();
  pauseLogRetention(false);
  addToLogRingBuffer("before crash\n");
  pauseLogRetention(true);
  for (int i = 0; i < 32; ++i) logPrintf("INF", "BOOT", "new boot\n");
  EXPECT_EQ(getLastLogs(), "before crash\n");
  // The crash report owns this snapshot before retention resumes.
  const std::string report = getLastLogs();
  clearLastLogs();
  pauseLogRetention(false);
  addToLogRingBuffer("current boot\n");
  EXPECT_EQ(report, "before crash\n");
  EXPECT_EQ(getLastLogs(), "current boot\n");
}
