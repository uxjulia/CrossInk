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
