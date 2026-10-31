#include <BoardConfig.h>
#include <HalPowerManager.h>
#include <WiFi.h>
#include <gtest/gtest.h>

#include <optional>

class HalPowerManagerTest : public testing::Test {
 protected:
  static void SetUpTestSuite() { powerManager.begin(); }
  void SetUp() override {
    WiFi.value = WIFI_MODE_NULL;
    power_test::frequencyFails = false;
    powerManager.setPowerSaving(false);
    power_test::now = 1;
    power_test::gaugeReads = 0;
    power_test::percentage = 80;
    power_test::gaugeFails = false;
    power_test::calibrationPending = false;
    BoardConfig::ACTIVE = {};
  }
};

TEST_F(HalPowerManagerTest, GaugeReadsAreDemandDrivenAndCachedForThirtySeconds) {
  HalPowerManager manager;
  EXPECT_EQ(manager.getBatteryPercentage(), 80);
  power_test::percentage = 79;
  power_test::now = 30000;
  EXPECT_EQ(manager.getBatteryPercentage(), 80);
  EXPECT_EQ(power_test::gaugeReads, 1u);
  power_test::now = 30001;
  EXPECT_EQ(manager.getBatteryPercentage(), 79);
  EXPECT_EQ(power_test::gaugeReads, 2u);
}

TEST_F(HalPowerManagerTest, FailedGaugeReadPreservesLastValueAndBacksOff) {
  HalPowerManager manager;
  EXPECT_EQ(manager.getBatteryPercentage(), 80);
  power_test::now += 30000;
  power_test::gaugeFails = true;
  EXPECT_EQ(manager.getBatteryPercentage(), 80);
  power_test::gaugeFails = false;
  power_test::percentage = 78;
  power_test::now += 29999;
  EXPECT_EQ(manager.getBatteryPercentage(), 80);
  EXPECT_EQ(power_test::gaugeReads, 2u);
  ++power_test::now;
  EXPECT_EQ(manager.getBatteryPercentage(), 78);
}

TEST_F(HalPowerManagerTest, CalibrationCompletionInvalidatesGaugeCache) {
  HalPowerManager manager;
  EXPECT_EQ(manager.getBatteryPercentage(), 80);
  power_test::calibrationPending = true;
  EXPECT_TRUE(manager.updateBatteryCalibration());
  power_test::calibrationPending = false;
  power_test::percentage = 90;
  EXPECT_FALSE(manager.updateBatteryCalibration());
  EXPECT_EQ(manager.getBatteryPercentage(), 90);
  EXPECT_EQ(power_test::gaugeReads, 2u);
}

TEST_F(HalPowerManagerTest, AdcReadsStayStableAcrossStatusBarMeasurementAndDrawing) {
  HalPowerManager manager;
  BoardConfig::ACTIVE.batteryGauge.gaugeAddr = 0;
  EXPECT_EQ(manager.getBatteryPercentage(), 80);
  power_test::percentage = 70;
  for (int i = 0; i < 12; ++i) {
    power_test::now += 10;
    EXPECT_EQ(manager.getBatteryPercentage(), 80);
  }
  EXPECT_EQ(power_test::gaugeReads, 1u);
  power_test::now = 30001;
  EXPECT_EQ(manager.getBatteryPercentage(), 79);
  EXPECT_EQ(power_test::gaugeReads, 2u);
  power_test::percentage = 90;
  EXPECT_EQ(manager.getBatteryPercentage(), 79);
  EXPECT_EQ(power_test::gaugeReads, 2u);
}

TEST_F(HalPowerManagerTest, AdcZeroIsAValidCachedSample) {
  HalPowerManager manager;
  BoardConfig::ACTIVE.batteryGauge.gaugeAddr = 0;
  power_test::percentage = 0;
  EXPECT_EQ(manager.getBatteryPercentage(), 0);
  power_test::percentage = 100;
  EXPECT_EQ(manager.getBatteryPercentage(), 0);
  EXPECT_EQ(power_test::gaugeReads, 1u);
  power_test::now += 30000;
  EXPECT_EQ(manager.getBatteryPercentage(), 10);
}

TEST_F(HalPowerManagerTest, BatteryReadsAtBootTimeZeroAreCached) {
  for (const int gaugeAddr : {0, 1}) {
    HalPowerManager manager;
    BoardConfig::ACTIVE.batteryGauge.gaugeAddr = gaugeAddr;
    power_test::now = 0;
    power_test::percentage = 80;
    power_test::gaugeReads = 0;
    EXPECT_EQ(manager.getBatteryPercentage(), 80);
    power_test::now = 1;
    power_test::percentage = 70;
    EXPECT_EQ(manager.getBatteryPercentage(), 80);
    EXPECT_EQ(power_test::gaugeReads, 1u);
  }
}

TEST_F(HalPowerManagerTest, BatteryPollIntervalSurvivesClockWrap) {
  for (const int gaugeAddr : {0, 1}) {
    HalPowerManager manager;
    BoardConfig::ACTIVE.batteryGauge.gaugeAddr = gaugeAddr;
    power_test::now = ~0UL - 100;
    power_test::percentage = 80;
    power_test::gaugeReads = 0;
    EXPECT_EQ(manager.getBatteryPercentage(), 80);
    power_test::percentage = 70;
    power_test::now += 29999;
    EXPECT_EQ(manager.getBatteryPercentage(), 80);
    EXPECT_EQ(power_test::gaugeReads, 1u);
    ++power_test::now;
    EXPECT_EQ(manager.getBatteryPercentage(), gaugeAddr == 0 ? 79 : 70);
    EXPECT_EQ(power_test::gaugeReads, 2u);
  }
}

TEST_F(HalPowerManagerTest, OverlappingLocksCanEndOutOfOrder) {
  powerManager.setPowerSaving(true);
  EXPECT_EQ(power_test::frequency, HalPowerManager::LOW_POWER_FREQ);
  std::optional<HalPowerManager::Lock> first(std::in_place);
  std::optional<HalPowerManager::Lock> second(std::in_place);
  EXPECT_EQ(power_test::frequency, 160);
  first.reset();
  powerManager.setPowerSaving(true);
  EXPECT_EQ(power_test::frequency, 160);
  second.reset();
  powerManager.setPowerSaving(true);
  EXPECT_EQ(power_test::frequency, HalPowerManager::LOW_POWER_FREQ);
}

TEST_F(HalPowerManagerTest, NestedLockDoesNotReleaseOuterLock) {
  {
    HalPowerManager::Lock outer;
    {
      HalPowerManager::Lock inner;
    }
    powerManager.setPowerSaving(true);
    EXPECT_EQ(power_test::frequency, 160);
  }
  powerManager.setPowerSaving(true);
  EXPECT_EQ(power_test::frequency, HalPowerManager::LOW_POWER_FREQ);
}

TEST_F(HalPowerManagerTest, WifiStillPreventsCpuThrottling) {
  WiFi.value = WIFI_MODE_STA;
  powerManager.setPowerSaving(true);
  EXPECT_EQ(power_test::frequency, 160);
  WiFi.value = WIFI_MODE_NULL;
  powerManager.setPowerSaving(true);
  EXPECT_EQ(power_test::frequency, HalPowerManager::LOW_POWER_FREQ);
}

TEST_F(HalPowerManagerTest, FailedFrequencyChangeCanBeRetried) {
  power_test::frequencyFails = true;
  powerManager.setPowerSaving(true);
  EXPECT_EQ(power_test::frequency, 160);
  power_test::frequencyFails = false;
  powerManager.setPowerSaving(true);
  EXPECT_EQ(power_test::frequency, HalPowerManager::LOW_POWER_FREQ);
}
