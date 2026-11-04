#include <HalStorage.h>
#include <gtest/gtest.h>

#include <sstream>
#include <vector>

#include "src/util/BatteryDiagnosticLog.h"

namespace {
const std::string V1 = "timestamp,uptime_ms,soc,mv,charging,event\n";
const std::string V2 = "timestamp,uptime_ms,soc,mv,charging,event,version,git_sha,git_dirty,device,wake_route\n";
std::vector<std::string> split(const std::string& text) {
  std::vector<std::string> values;
  std::istringstream stream(text);
  std::string value;
  while (std::getline(stream, value, ',')) values.push_back(value);
  return values;
}
}  // namespace

class BatteryDiagnosticLogTest : public testing::Test {
 protected:
  void SetUp() override {
    battery_log_test::files.clear();
    battery_log_test::failOpen = battery_log_test::failRename = false;
    battery_log_test::writes = 0;
    power_test::gaugeFails = false;
    power_test::percentage = 98;
    power_test::batteryMv = 4280;
    power_test::usbPowered = false;
    power_test::charging = power_test::chargeComplete = false;
    power_test::chargerStatus = 0;
    power_test::wakeupCause = 0;
  }
  void record() { BatteryDiagnosticLog::record(BatteryDiagnosticLog::Event::Wake, "picco", "PowerButton"); }
};

TEST_F(BatteryDiagnosticLogTest, WritesAlignedColumnsAndDoesNotAdvanceUi) {
  record();
  const std::string text = battery_log_test::files["/battery_log.csv"];
  const size_t first = text.find('\n');
  const auto header = split(text.substr(0, first));
  const auto data = split(text.substr(first + 1, text.size() - first - 2));
  ASSERT_EQ(header.size(), 19u);
  ASSERT_EQ(data.size(), header.size());
  EXPECT_EQ(data[2], "98");
  EXPECT_EQ(data[3], "4280");
  EXPECT_EQ(data[12], "0");
  EXPECT_EQ(data[13], "0");
  EXPECT_EQ(data[14], "0");
  EXPECT_EQ(battery_log_test::writes, 2u);  // one header plus one event row only
}

TEST_F(BatteryDiagnosticLogTest, PreservesBothOldSchemasAndExistingArchives) {
  for (unsigned version : {1u, 2u}) {
    battery_log_test::files.clear();
    const std::string old = (version == 1 ? V1 : V2) + "old sample\n";
    const std::string archive = "/battery_log_v" + std::to_string(version) + ".csv";
    battery_log_test::files[archive] = "existing archive";
    battery_log_test::files["/battery_log.csv"] = old;
    record();
    EXPECT_EQ(battery_log_test::files[archive], "existing archive");
    EXPECT_EQ(battery_log_test::files["/battery_log_v" + std::to_string(version) + "_1.csv"], old);
    EXPECT_NE(battery_log_test::files["/battery_log.csv"].find("display_soc"), std::string::npos);
  }
}

TEST_F(BatteryDiagnosticLogTest, ReportsFreshRawAndCachedDisplayWithoutAdvancingFilter) {
  powerManager.begin();
  powerManager.restoreBatteryAfterSleep(false);
  EXPECT_EQ(powerManager.getBatteryPercentage(), 98);
  power_test::now += 30000;
  power_test::rtcMs += 30000;
  power_test::percentage = 70;
  power_test::batteryMv = 3955;
  record();
  const std::string text = battery_log_test::files["/battery_log.csv"];
  const size_t first = text.find('\n');
  const auto data = split(text.substr(first + 1, text.size() - first - 2));
  ASSERT_EQ(data.size(), 19u);
  EXPECT_EQ(data[2], "70");
  EXPECT_EQ(data[11], "98");
  EXPECT_EQ(data[15], "1");
  EXPECT_EQ(data[17], "30000");
  EXPECT_EQ(data[18], "0");
  HalPowerManager::BatteryDiagnostics snapshot;
  ASSERT_TRUE(powerManager.getBatteryDiagnostics(snapshot));
  EXPECT_EQ(snapshot.displayedPercent, 98);
}

TEST_F(BatteryDiagnosticLogTest, RefusesUnknownHeadersAndFailedArchives) {
  battery_log_test::files["/battery_log.csv"] = "unknown header\n";
  record();
  EXPECT_EQ(battery_log_test::files["/battery_log.csv"], "unknown header\n");
  battery_log_test::files["/battery_log.csv"] = V2;
  battery_log_test::failRename = true;
  record();
  EXPECT_EQ(battery_log_test::files["/battery_log.csv"], V2);
  EXPECT_EQ(battery_log_test::writes, 0u);
}

TEST_F(BatteryDiagnosticLogTest, PreservesCapAndMissingValueSemantics) {
  record();
  auto& text = battery_log_test::files["/battery_log.csv"];
  text.resize(128 * 1024, 'x');
  battery_log_test::writes = 0;
  record();
  EXPECT_EQ(text.size(), 128u * 1024);
  EXPECT_EQ(battery_log_test::writes, 0u);
  battery_log_test::files.clear();
  power_test::gaugeFails = true;
  record();
  const auto& missing = battery_log_test::files["/battery_log.csv"];
  const auto fields = split(missing.substr(missing.find('\n') + 1));
  EXPECT_EQ(fields[2], "");
  EXPECT_EQ(fields[3], "");
}
