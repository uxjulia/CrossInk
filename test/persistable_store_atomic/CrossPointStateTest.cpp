#include <CrossPointState.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

namespace {
constexpr char STATE_PATH[] = "/.crosspoint/state.json";
}

class CrossPointStateTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    EXPECT_FALSE(APP_STATE.loadFromFile());  // Clear the remembered on-disk snapshot.
    JsonDocument defaults;
    APP_STATE.fromJson(defaults.as<JsonVariantConst>());
  }
};

TEST_F(CrossPointStateTest, SkipsRepeatedSuccessfulSnapshot) {
  APP_STATE.openEpubPath = "/book.epub";
  ASSERT_TRUE(APP_STATE.saveToFile());
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.writeAttempts, 1U);
}

TEST_F(CrossPointStateTest, PersistsReaderExitAndWallpaperChanges) {
  APP_STATE.readerActivityLoadCount = 1;
  ASSERT_TRUE(APP_STATE.saveToFile());
  APP_STATE.readerActivityLoadCount = 0;
  ASSERT_TRUE(APP_STATE.saveToFile());
  APP_STATE.pushRecentSleep(9);
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.writeAttempts, 3U);
  ASSERT_TRUE(APP_STATE.loadFromFile());
  EXPECT_EQ(APP_STATE.readerActivityLoadCount, 0);
  EXPECT_TRUE(APP_STATE.isRecentSleep(9, 1));
}

TEST_F(CrossPointStateTest, RecreatesDeletedStateFile) {
  ASSERT_TRUE(APP_STATE.saveToFile());
  ASSERT_TRUE(Storage.remove(STATE_PATH));
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_TRUE(Storage.exists(STATE_PATH));
  EXPECT_EQ(Storage.writeAttempts, 2U);
}

TEST_F(CrossPointStateTest, RetriesFailedFirstSave) {
  Storage.failNextWrite();
  EXPECT_FALSE(APP_STATE.saveToFile());
  EXPECT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.writeAttempts, 2U);
}

TEST_F(CrossPointStateTest, FailedRewriteInvalidatesPreviousSnapshot) {
  APP_STATE.openEpubPath = "/old.epub";
  ASSERT_TRUE(APP_STATE.saveToFile());
  APP_STATE.openEpubPath = "/new.epub";
  Storage.failNextWrite();
  EXPECT_FALSE(APP_STATE.saveToFile());
  APP_STATE.openEpubPath = "/old.epub";
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.writeAttempts, 3U);
  ASSERT_TRUE(APP_STATE.loadFromFile());
  EXPECT_EQ(APP_STATE.openEpubPath, "/old.epub");
}

TEST_F(CrossPointStateTest, ReloadInvalidatesPreviousSnapshot) {
  APP_STATE.openEpubPath = "/old.epub";
  ASSERT_TRUE(APP_STATE.saveToFile());
  Storage.put(STATE_PATH, R"({"openEpubPath":"/new.epub"})");
  ASSERT_TRUE(APP_STATE.loadFromFile());
  APP_STATE.openEpubPath = "/old.epub";
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.writeAttempts, 2U);
  ASSERT_TRUE(APP_STATE.loadFromFile());
  EXPECT_EQ(APP_STATE.openEpubPath, "/old.epub");
}

TEST_F(CrossPointStateTest, FailedSaveLeavesPreviousStateIntactAndCanRetry) {
  APP_STATE.openEpubPath = "/original.epub";
  ASSERT_TRUE(APP_STATE.saveToFile());
  const auto original = Storage.readFile(STATE_PATH);
  APP_STATE.openEpubPath = "/updated.epub";
  for (int fault = 0; fault < 3; ++fault) {
    Storage.shortWrite = fault == 0;
    Storage.failSync = fault == 1;
    Storage.failClose = fault == 2;
    EXPECT_FALSE(APP_STATE.saveToFile());
    EXPECT_EQ(Storage.readFile(STATE_PATH), original);
  }
  Storage.failClose = false;
  ASSERT_TRUE(APP_STATE.saveToFile());
  ASSERT_TRUE(APP_STATE.loadFromFile());
  EXPECT_EQ(APP_STATE.openEpubPath, "/updated.epub");
}

TEST_F(CrossPointStateTest, BootReadsParkedStateWhenRollbackRenameFails) {
  const std::string backup = std::string(STATE_PATH) + ".bak";
  Storage.put(backup, R"({"openEpubPath":"/original.epub"})");
  Storage.put(std::string(STATE_PATH) + ".tmp", "{");
  Storage.failNextRenameFrom(backup);
  ASSERT_TRUE(APP_STATE.loadFromFile());
  EXPECT_EQ(APP_STATE.openEpubPath, "/original.epub");
  EXPECT_TRUE(Storage.exists(backup.c_str()));
}

TEST_F(CrossPointStateTest, RestoresCompleteLegacyTemporaryStateOnBoot) {
  Storage.put(std::string(STATE_PATH) + ".tmp", R"({"openEpubPath":"/legacy.epub"})");

  ASSERT_TRUE(APP_STATE.loadFromFile());

  EXPECT_EQ(APP_STATE.openEpubPath, "/legacy.epub");
  EXPECT_TRUE(Storage.exists(STATE_PATH));
  EXPECT_FALSE(Storage.exists((std::string(STATE_PATH) + ".tmp").c_str()));
}
