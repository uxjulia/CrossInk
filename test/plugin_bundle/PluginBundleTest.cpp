#include <HalStorage.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include "util/PluginBundle.h"
#include "util/PluginEvents.h"

namespace {
int subscriptionRefreshes = 0;
}
namespace pluginevents {
void refreshSubscriptions() { ++subscriptionRefreshes; }
}  // namespace pluginevents

namespace {
class BundleTest : public testing::Test {
 protected:
  static constexpr const char* dest = "/plugins/example";
  void SetUp() override {
    bundle_test::root = std::filesystem::temp_directory_path() / ("crossink-bundle-test-" + std::to_string(getpid()));
    std::filesystem::remove_all(bundle_test::root);
    std::filesystem::create_directories(bundle_test::root / ".crosspoint");
    bundle_test::renameCalls = bundle_test::failRename = 0;
    bundle_test::failJournalClose = false;
    subscriptionRefreshes = 0;
    write(std::string(dest) + "/plugin.js", "old script");
    write(std::string(dest) + "/token.json", "saved login");
  }
  void TearDown() override { std::filesystem::remove_all(bundle_test::root); }
  static void write(const std::string& path, const std::string& value) {
    const auto disk = bundle_test::physical(path);
    std::filesystem::create_directories(disk.parent_path());
    std::ofstream(disk) << value;
  }
  static std::string read(const std::string& path) {
    std::ifstream file(bundle_test::physical(path));
    return {std::istreambuf_iterator<char>(file), {}};
  }
  void stage() {
    ASSERT_TRUE(PluginBundle::prepare(dest, nullptr, nullptr));
    write(PluginBundle::stagingPath(dest) + "/plugin.js", "new script");
  }
};
TEST_F(BundleTest, SuccessfulUpdatePreservesExistingLogin) {
  stage();
  ASSERT_TRUE(PluginBundle::publish(dest));
  EXPECT_EQ(read(std::string(dest) + "/plugin.js"), "new script");
  EXPECT_EQ(read(std::string(dest) + "/token.json"), "saved login");
  EXPECT_FALSE(Storage.exists("/.crosspoint/plugin-install.txt"));
  EXPECT_EQ(subscriptionRefreshes, 1);
}
TEST_F(BundleTest, FailedRenameNeverLosesInstalledFolder) {
  for (int failAt = 1; failAt <= 3; ++failAt) {
    stage();
    bundle_test::renameCalls = 0;
    bundle_test::failRename = failAt;
    EXPECT_FALSE(PluginBundle::publish(dest));
    EXPECT_EQ(subscriptionRefreshes, 0);
    bundle_test::failRename = 0;
    ASSERT_TRUE(PluginBundle::recoverPending());
    EXPECT_EQ(read(std::string(dest) + "/plugin.js"), "old script");
    EXPECT_EQ(read(std::string(dest) + "/token.json"), "saved login");
    PluginBundle::discard(dest);
  }
}
TEST_F(BundleTest, InterruptedSwapRestoresOldFolder) {
  stage();
  write("/.crosspoint/plugin-install.txt", dest);
  ASSERT_TRUE(Storage.rename(dest, "/plugins/.example.plugin-old"));
  ASSERT_TRUE(PluginBundle::recoverPending());
  EXPECT_EQ(read(std::string(dest) + "/plugin.js"), "old script");
}
TEST_F(BundleTest, InterruptedCleanupKeepsNewFolder) {
  stage();
  write("/.crosspoint/plugin-install.txt", dest);
  ASSERT_TRUE(Storage.rename(dest, "/plugins/.example.plugin-old"));
  ASSERT_TRUE(Storage.rename(PluginBundle::stagingPath(dest).c_str(), dest));
  ASSERT_TRUE(PluginBundle::recoverPending());
  EXPECT_EQ(read(std::string(dest) + "/plugin.js"), "new script");
  EXPECT_FALSE(Storage.exists("/plugins/.example.plugin-old"));
}
TEST_F(BundleTest, FailedJournalCloseLeavesOriginalIntact) {
  stage();
  bundle_test::failJournalClose = true;
  EXPECT_FALSE(PluginBundle::publish(dest));
  EXPECT_EQ(read(std::string(dest) + "/plugin.js"), "old script");
}
TEST_F(BundleTest, CancelledStagingLeavesOriginalIntact) {
  EXPECT_FALSE(PluginBundle::prepare(dest, [](void*) { return true; }, nullptr));
  EXPECT_EQ(read(std::string(dest) + "/plugin.js"), "old script");
  EXPECT_FALSE(Storage.exists(PluginBundle::stagingPath(dest).c_str()));
}
}  // namespace
