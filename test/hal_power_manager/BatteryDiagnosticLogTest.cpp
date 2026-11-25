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

TEST_F(BatteryDiagnosticLogTest, LogsConfirmedCompletionWithThePaintedSleepPercentageOnce) {
  using BatteryDiagnosticLog::pollChargeComplete;
  BatteryDiagnosticLog::noteUsbDisconnected();
  power_test::usbPowered = true;
  power_test::percentage = 100;
  int painted = 97;
  pollChargeComplete("picco", &painted);
  EXPECT_EQ(battery_log_test::writes, 0u);

  power_test::chargeComplete = true;
  power_test::chargerStatus = 26;
  battery_log_test::failOpen = true;
  pollChargeComplete("picco", &painted);
  battery_log_test::failOpen = false;
  pollChargeComplete("picco", &painted);
  pollChargeComplete("picco");  // Changing from sleep to awake does not duplicate the row.
  EXPECT_EQ(battery_log_test::writes, 2u);
  const auto& text = battery_log_test::files["/battery_log.csv"];
  const auto fields = split(text.substr(text.find('\n') + 1));
  ASSERT_EQ(fields.size(), 19u);
  EXPECT_EQ(fields[2], "100");
  EXPECT_EQ(fields[5], "charge_complete");
  EXPECT_EQ(fields[11], "97");
  EXPECT_EQ(fields[13], "1");
  EXPECT_EQ(fields[14], "26");

  power_test::chargeComplete = false;  // A later top-off can complete on the same cable.
  pollChargeComplete("picco");
  power_test::chargeComplete = true;
  painted = -1;
  const auto next = text.size();
  pollChargeComplete("picco", &painted);
  ASSERT_GT(text.size(), next);
  EXPECT_EQ(split(text.substr(next))[11], "");
  BatteryDiagnosticLog::noteUsbDisconnected();
  pollChargeComplete("picco");
  EXPECT_EQ(battery_log_test::writes, 4u);
}

TEST_F(BatteryDiagnosticLogTest, AwakeCompletionUsesLastFinishedStatusBarRender) {
  using namespace BatteryDiagnosticLog;
  noteUsbDisconnected();
  power_test::usbPowered = power_test::chargeComplete = true;
  power_test::percentage = 100;
  beginDisplayFrame();
  noteDisplayPercent(97, 24);
  finishDisplayFrame();
  beginDisplayFrame();
  noteDisplayPercent(100, 24);  // The newer status bar has not yet been presented.
  pollChargeComplete("picco");
  const auto& text = battery_log_test::files["/battery_log.csv"];
  ASSERT_NE(text.find('\n'), std::string::npos);
  EXPECT_EQ(split(text.substr(text.find('\n') + 1))[11], "97");
  finishDisplayFrame();
  noteUsbDisconnected();
  auto popup = text.size();
  beginDisplayFrame();
  retainDisplayAbove(50);  // The popup leaves the entire label above it visible.
  finishDisplayFrame();
  pollChargeComplete("picco");
  EXPECT_EQ(split(text.substr(popup))[11], "100");
  noteUsbDisconnected();
  beginDisplayFrame();
  retainDisplayAbove(20);  // The popup covers the label.
  finishDisplayFrame();
  const auto next = text.size();
  pollChargeComplete("picco");
  EXPECT_EQ(split(text.substr(next))[11], "");
}

TEST_F(BatteryDiagnosticLogTest, UsesTheSleepPollThatTriggeredTheEvent) {
  BatteryDiagnosticLog::noteUsbDisconnected();
  HalPowerManager::ChargerStatus observed{};
  observed.supported = observed.chargeCompleteKnown = observed.chargeComplete = true;
  const int painted = 96;
  const unsigned reads = power_test::gaugeReads;
  BatteryDiagnosticLog::pollChargeComplete("picco", &painted, &observed);
  ASSERT_EQ(battery_log_test::writes, 2u);
  observed.chargeCompleteKnown = false;
  BatteryDiagnosticLog::pollChargeComplete("picco", &painted, &observed);
  observed.chargeCompleteKnown = true;
  BatteryDiagnosticLog::pollChargeComplete("picco", &painted, &observed);
  EXPECT_EQ(battery_log_test::writes, 2u);
  EXPECT_EQ(power_test::gaugeReads, reads);
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
