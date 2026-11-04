#include <HalPowerManager.h>
#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "lib/hal/BatteryEstimateRetention.h"

class BatteryReadoutTest : public testing::Test {
 protected:
  HalPowerManager manager;
  void SetUp() override {
    power_test::now = 0;
    power_test::rtcMs = 100000;
    power_test::wakeupCause = 0;
    power_test::percentage = 98;
    power_test::batteryMv = 4280;
    power_test::gaugeFails = false;
    power_test::gaugeReads = 0;
    power_test::usbPowered = power_test::charging = power_test::chargeComplete = false;
    manager.begin();
    manager.restoreBatteryAfterSleep(false);
  }
  void advance(unsigned ms) {
    power_test::now += ms;
    power_test::rtcMs += ms;
  }
};

TEST_F(BatteryReadoutTest, PollingRemainsDemandDrivenAndThirtySecondCached) {
  EXPECT_EQ(manager.getBatteryPercentage(), 98);
  power_test::percentage = 90;
  for (int i = 0; i < 20; ++i) EXPECT_EQ(manager.getBatteryPercentage(), 98);
  advance(29999);
  EXPECT_EQ(manager.getBatteryPercentage(), 98);
  EXPECT_EQ(power_test::gaugeReads, 1u);
  advance(1);
  EXPECT_LT(manager.getBatteryPercentage(), 98);
  EXPECT_EQ(power_test::gaugeReads, 2u);
}

TEST_F(BatteryReadoutTest, MainAndRenderCallsShareOneSample) {
  std::vector<std::thread> callers;
  for (unsigned i = 0; i < 8; ++i) callers.emplace_back([&] { EXPECT_EQ(manager.getBatteryPercentage(), 98); });
  for (auto& caller : callers) caller.join();
  EXPECT_EQ(power_test::gaugeReads, 1u);
}

TEST_F(BatteryReadoutTest, DeepSleepAndUsbRestartRestoreEstimateWithoutStorageWrites) {
  power_test::usbPowered = power_test::chargeComplete = true;
  EXPECT_EQ(manager.getBatteryPercentage(), 100);
  power_test::usbPowered = power_test::chargeComplete = false;
  advance(30000);
  power_test::wakeupCause = 1;
  HalPowerManager resumed;
  resumed.restoreBatteryAfterSleep(false);
  EXPECT_EQ(resumed.getBatteryPercentage(), 100);
  HalPowerManager usbResume;
  power_test::wakeupCause = 0;
  usbResume.restoreBatteryAfterSleep(true);
  EXPECT_EQ(usbResume.getBatteryPercentage(), 100);
  HalPowerManager::BatteryDiagnostics diag;
  ASSERT_TRUE(usbResume.getBatteryDiagnostics(diag));
  EXPECT_TRUE(diag.resumed);
  EXPECT_EQ(diag.displayedPercent, 100);
  EXPECT_EQ(diag.soc, 98);
}

TEST_F(BatteryReadoutTest, ColdBootIgnoresRetainedHistory) {
  power_test::usbPowered = power_test::chargeComplete = true;
  EXPECT_EQ(manager.getBatteryPercentage(), 100);
  power_test::usbPowered = power_test::chargeComplete = false;
  HalPowerManager cold;
  cold.restoreBatteryAfterSleep(false);
  EXPECT_EQ(cold.getBatteryPercentage(), 98);
}

TEST_F(BatteryReadoutTest, FlashingNearFullBatteryDoesNotRestartAtNinetyEight) {
  power_test::usbPowered = power_test::charging = true;
  power_test::percentage = 100;
  power_test::batteryMv = 4340;
  EXPECT_EQ(manager.getBatteryPercentage(), 100);
  HalPowerManager::BatteryDiagnostics diag;
  ASSERT_TRUE(manager.getBatteryDiagnostics(diag));
  EXPECT_EQ(diag.displayedPercent, 100);
  EXPECT_FALSE(diag.chargeComplete);
  EXPECT_EQ(diag.estimateState, static_cast<int>(BatteryEstimate::Power::Charging));

  power_test::usbPowered = power_test::charging = false;
  power_test::percentage = 98;
  power_test::batteryMv = 4280;
  advance(30000);
  EXPECT_EQ(manager.getBatteryPercentage(), 100);  // Keep the unplug settling behavior.
}

TEST_F(BatteryReadoutTest, LongSleepReconcilesRealLossAndFailuresBackOff) {
  EXPECT_EQ(manager.getBatteryPercentage(), 98);
  advance(8 * 60 * 60 * 1000);
  power_test::wakeupCause = 1;
  power_test::percentage = 50;
  power_test::batteryMv = 3812;
  HalPowerManager resumed;
  resumed.restoreBatteryAfterSleep(false);
  EXPECT_EQ(resumed.getBatteryPercentage(), 50);
  advance(30000);
  power_test::gaugeFails = true;
  EXPECT_EQ(resumed.getBatteryPercentage(), 50);
  const unsigned reads = power_test::gaugeReads;
  EXPECT_EQ(resumed.getBatteryPercentage(), 50);
  EXPECT_EQ(power_test::gaugeReads, reads);
}

TEST_F(BatteryReadoutTest, DiagnosticsDoNotAdvanceTheDisplayedEstimate) {
  EXPECT_EQ(manager.getBatteryPercentage(), 98);
  advance(120000);
  power_test::percentage = 70;
  power_test::batteryMv = 3955;
  HalPowerManager::BatteryDiagnostics first, second;
  ASSERT_TRUE(manager.getBatteryDiagnostics(first));
  ASSERT_TRUE(manager.getBatteryDiagnostics(second));
  EXPECT_EQ(first.displayedPercent, 98);
  EXPECT_EQ(second.displayedPercent, 98);
  EXPECT_EQ(first.soc, 70);
  EXPECT_EQ(first.sampleAgeMs, 120000u);
}

TEST(BatteryEstimateRetention, RejectsStaleCorruptAndUninitializedRtcData) {
  BatteryEstimate estimate;
  ASSERT_TRUE(estimate.restore({98 * 256, 0, BatteryEstimate::Power::Battery}));
  BatteryEstimateRetention retained{};
  EXPECT_FALSE(retained.restore(estimate, 1000));
  retained.save(estimate, 1000);
  EXPECT_TRUE(retained.restore(estimate, 2000));
  EXPECT_FALSE(retained.restore(estimate, 999));
  EXPECT_FALSE(retained.restore(estimate, 8ull * 24 * 60 * 60 * 1000));
  retained.state.percentQ8 ^= 16;
  EXPECT_FALSE(retained.restore(estimate, 2000));
}
