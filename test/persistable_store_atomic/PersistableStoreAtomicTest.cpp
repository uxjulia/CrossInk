#include <HalStorage.h>
#include <PersistableStore.h>
#include <gtest/gtest.h>

namespace {
constexpr char STORE_PATH[] = "/.crosspoint/recent.json";

JsonDocument documentWithPath(const char* path) {
  JsonDocument doc;
  doc["books"][0]["path"] = path;
  return doc;
}

const char* firstPath(const JsonDocument& doc) { return doc["books"][0]["path"] | ""; }
}  // namespace

class PersistableStoreAtomicTest : public testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

TEST_F(PersistableStoreAtomicTest, RestoresOriginalWhenReplacementRenameFails) {
  ASSERT_TRUE(PersistableStoreBase::writeDocToFile(STORE_PATH, documentWithPath("/old.epub")));
  Storage.failNextRenameFrom(std::string(STORE_PATH) + ".tmp");

  EXPECT_FALSE(PersistableStoreBase::writeDocToFileAtomically(STORE_PATH, documentWithPath("/new.epub")));

  JsonDocument loaded;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_STREQ(firstPath(loaded), "/old.epub");
  EXPECT_FALSE(Storage.exists((std::string(STORE_PATH) + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((std::string(STORE_PATH) + ".bak").c_str()));
}

TEST_F(PersistableStoreAtomicTest, RecoversBackupAfterInterruptedReplacement) {
  Storage.put(std::string(STORE_PATH) + ".bak", R"({"books":[{"path":"/old.epub"}]})");

  JsonDocument loaded;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_STREQ(firstPath(loaded), "/old.epub");
  EXPECT_TRUE(Storage.exists(STORE_PATH));
  EXPECT_FALSE(Storage.exists((std::string(STORE_PATH) + ".bak").c_str()));
}

TEST_F(PersistableStoreAtomicTest, ReplacesStoreAndRemovesTemporaryFiles) {
  ASSERT_TRUE(PersistableStoreBase::writeDocToFile(STORE_PATH, documentWithPath("/old.epub")));
  ASSERT_TRUE(PersistableStoreBase::writeDocToFileAtomically(STORE_PATH, documentWithPath("/new.epub")));

  JsonDocument loaded;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_STREQ(firstPath(loaded), "/new.epub");
  EXPECT_FALSE(Storage.exists((std::string(STORE_PATH) + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((std::string(STORE_PATH) + ".bak").c_str()));
}

TEST_F(PersistableStoreAtomicTest, UsesBoundedBulkReadAtLegacySizeLimit) {
  std::string json = R"({"books":[{"path":"/old.epub"}]})";
  json.resize(50000, ' ');
  Storage.put(STORE_PATH, json);
  JsonDocument loaded;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_STREQ(firstPath(loaded), "/old.epub");
  EXPECT_EQ(Storage.bulkReadAttempts, 1U);
  EXPECT_EQ(Storage.lastReadCap, 50000U);
  EXPECT_EQ(Storage.lastReadPath, STORE_PATH);
}

TEST_F(PersistableStoreAtomicTest, RejectsOversizeRatherThanParsingValidPrefix) {
  std::string json = R"({"books":[{"path":"/old.epub"}]})";
  json.resize(50001, ' ');
  Storage.put(STORE_PATH, json);
  JsonDocument loaded;
  EXPECT_FALSE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_TRUE(loaded.isNull());
}

TEST_F(PersistableStoreAtomicTest, FailedBulkReadIsNotParsedEvenIfOutputIsValid) {
  Storage.put(STORE_PATH, R"({"books":[{"path":"/partial.epub"}]})");
  Storage.failNextBulkRead();
  JsonDocument loaded;
  EXPECT_FALSE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_TRUE(loaded.isNull());
}

TEST_F(PersistableStoreAtomicTest, ReadsBackupDirectlyWhenRestoreRenameFails) {
  const std::string backup = std::string(STORE_PATH) + ".bak";
  Storage.put(backup, R"({"books":[{"path":"/old.epub"}]})");
  Storage.failNextRenameFrom(backup);
  JsonDocument loaded;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_STREQ(firstPath(loaded), "/old.epub");
  EXPECT_EQ(Storage.lastReadPath, backup);
  EXPECT_TRUE(Storage.exists(backup.c_str()));
  EXPECT_FALSE(Storage.exists(STORE_PATH));
}

TEST_F(PersistableStoreAtomicTest, RejectsMissingEmptyAndMalformedDocuments) {
  JsonDocument loaded;
  EXPECT_FALSE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_EQ(Storage.bulkReadAttempts, 0U);
  Storage.put(STORE_PATH, "");
  EXPECT_FALSE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  Storage.put(STORE_PATH, R"({"books":[)");
  EXPECT_FALSE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
}

TEST_F(PersistableStoreAtomicTest, RecoversLegacyTemporaryWhenPrimaryAndBackupAreMissing) {
  const std::string temporary = std::string(STORE_PATH) + ".tmp";
  Storage.put(temporary, R"({"books":[{"path":"/legacy.epub"}]})");

  JsonDocument loaded;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_STREQ(firstPath(loaded), "/legacy.epub");
  EXPECT_TRUE(Storage.exists(STORE_PATH));
  EXPECT_FALSE(Storage.exists(temporary.c_str()));
}

TEST_F(PersistableStoreAtomicTest, FirstSavePreservesValidLegacyTemporaryBeforeReplacingIt) {
  const std::string temporary = std::string(STORE_PATH) + ".tmp";
  Storage.put(temporary, R"({"books":[{"path":"/legacy.epub"}]})");

  ASSERT_TRUE(PersistableStoreBase::writeDocToFile(STORE_PATH, documentWithPath("/new.epub")));

  JsonDocument loaded;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_STREQ(firstPath(loaded), "/new.epub");
  EXPECT_FALSE(Storage.exists(temporary.c_str()));
  EXPECT_FALSE(Storage.exists((std::string(STORE_PATH) + ".bak").c_str()));
}

TEST_F(PersistableStoreAtomicTest, TornFirstSaveCanBeDiscardedAndRetried) {
  const std::string temporary = std::string(STORE_PATH) + ".tmp";
  Storage.put(temporary, R"({"books":[)");

  ASSERT_TRUE(PersistableStoreBase::writeDocToFile(STORE_PATH, documentWithPath("/retry.epub")));

  JsonDocument loaded;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_STREQ(firstPath(loaded), "/retry.epub");
  EXPECT_FALSE(Storage.exists(temporary.c_str()));
}

TEST_F(PersistableStoreAtomicTest, UnreadableOrphanIsRetainedAndBlocksOverwrite) {
  const std::string temporary = std::string(STORE_PATH) + ".tmp";
  const std::string orphan = R"({"books":[{"path":"/possibly-valid.epub"}]})";
  Storage.put(temporary, orphan);
  Storage.failNextBulkRead();

  EXPECT_FALSE(PersistableStoreBase::writeDocToFile(STORE_PATH, documentWithPath("/new.epub")));
  EXPECT_EQ(Storage.readFile(temporary.c_str()), orphan);
  EXPECT_FALSE(Storage.exists(STORE_PATH));
  EXPECT_TRUE(PersistableStoreBase::hasStoreFile(STORE_PATH));

  Storage.failNextBulkRead();
  JsonDocument loaded;
  EXPECT_FALSE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_EQ(Storage.readFile(temporary.c_str()), orphan);
}

TEST_F(PersistableStoreAtomicTest, ReadsValidTemporaryIfPromotionRenameFails) {
  const std::string temporary = std::string(STORE_PATH) + ".tmp";
  Storage.put(temporary, R"({"books":[{"path":"/legacy.epub"}]})");
  Storage.failNextRenameFrom(temporary);

  JsonDocument loaded;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_STREQ(firstPath(loaded), "/legacy.epub");
  EXPECT_EQ(Storage.lastReadPath, temporary);
  EXPECT_TRUE(PersistableStoreBase::hasStoreFile(STORE_PATH));
}

TEST_F(PersistableStoreAtomicTest, MalformedOrphanIsRetainedAndBlocksOverwrite) {
  const std::string temporary = std::string(STORE_PATH) + ".tmp";
  const std::string orphan = R"({broken})";
  Storage.put(temporary, orphan);

  EXPECT_FALSE(PersistableStoreBase::writeDocToFile(STORE_PATH, documentWithPath("/new.epub")));
  EXPECT_EQ(Storage.readFile(temporary.c_str()), orphan);
  EXPECT_FALSE(Storage.exists(STORE_PATH));
}

TEST_F(PersistableStoreAtomicTest, OrphanMustContainOneCompleteStoreDocument) {
  const std::string temporary = std::string(STORE_PATH) + ".tmp";
  const std::string trailingGarbage = R"({"books":[]} trailing)";
  Storage.put(temporary, trailingGarbage);

  EXPECT_FALSE(PersistableStoreBase::writeDocToFile(STORE_PATH, documentWithPath("/new.epub")));
  EXPECT_EQ(Storage.readFile(temporary.c_str()), trailingGarbage);

  Storage.reset();
  Storage.put(temporary, "[]");
  JsonDocument loaded;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_TRUE(loaded.is<JsonArray>());
}
