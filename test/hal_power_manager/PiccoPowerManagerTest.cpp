#include <HalPowerManager.h>
#include <WiFi.h>
#include <gtest/gtest.h>

#include "src/util/UsbSleepWakeToken.h"

bool HalGPIO::isUsbConnected() const { return power_test::usbPowered; }
bool HalGPIO::prepareForDeepSleep() {
  power_test::touchPreparedWithLightOff = !power_test::lightOn;
  return true;
}

class PiccoPowerManagerTest : public testing::Test {
 protected:
  HalGPIO gpio;
  HalPowerManager manager;
  void SetUp() override {
    power_test::usbPowered = false;
    power_test::powerPressed = false;
    power_test::pressOnWake = false;
    power_test::lightOn = true;
    power_test::touchPreparedWithLightOff = false;
    power_test::chargerSetups = 0;
    power_test::lightSleeps = 0;
    power_test::releaseWaits = 0;
    power_test::wakeCleanup = 0;
    power_test::runtimeInterruptEnabled = true;
    power_test::unsafeLevelWake = false;
    power_test::interruptDisableFails = false;
    power_test::pressAfterDelay = false;
    power_test::timerWakesBeforeAction = 0;
    WiFi.value = WIFI_MODE_NULL;
    manager.begin();
  }
};

TEST_F(PiccoPowerManagerTest, ConfiguresChargerAtStartup) { EXPECT_EQ(power_test::chargerSetups, 1u); }

TEST_F(PiccoPowerManagerTest, BatterySleepDoesNotHoldInLightSleep) {
  EXPECT_FALSE(manager.holdWhileUsbPowered(gpio));
  EXPECT_EQ(power_test::lightSleeps, 0u);
}

TEST_F(PiccoPowerManagerTest, UsbUnplugLeavesHoldAndCleansWakeSources) {
  power_test::usbPowered = true;
  WiFi.value = WIFI_MODE_STA;
  EXPECT_FALSE(manager.holdWhileUsbPowered(gpio));
  EXPECT_EQ(WiFi.value, WIFI_MODE_NULL);
  EXPECT_EQ(power_test::lightSleeps, 1u);
  EXPECT_EQ(power_test::wakeCleanup, 1u);
}

TEST_F(PiccoPowerManagerTest, UsbPowerPressReturnsAnAcceptedWakeEvenAfterRelease) {
  power_test::usbPowered = true;
  power_test::pressOnWake = true;
  const bool wakeRequested = manager.holdWhileUsbPowered(gpio);
  power_test::powerPressed = false;
  EXPECT_TRUE(wakeRequested);
  EXPECT_EQ(power_test::wakeCleanup, 1u);
}

TEST_F(PiccoPowerManagerTest, OrdinarySleepTurnsOffLightBeforePreparingTouch) {
  manager.startDeepSleep(gpio);
  EXPECT_TRUE(power_test::touchPreparedWithLightOff);
  EXPECT_EQ(power_test::releaseWaits, 1u);
}

TEST(PiccoUsbSleepWakeToken, AcceptedWakeIsRetainedUntilSoftwareRestart) {
  UsbSleepWakeToken token{0};
  token.arm();
  EXPECT_TRUE(token.consume(true));
  EXPECT_FALSE(token.consume(true));
}

TEST(PiccoUsbSleepWakeToken, ColdBootOrPanicDiscardsEvenAnArmedToken) {
  UsbSleepWakeToken token{0};
  token.arm();
  EXPECT_FALSE(token.consume(false));
  EXPECT_FALSE(token.consume(true));
}

TEST(PiccoUsbSleepWakeToken, UninitializedRtcDoesNotRequestWake) {
  for (const uint32_t garbage : {0u, 0xffffffffu, 0xC1EAB007u}) {
    UsbSleepWakeToken token{garbage};
    EXPECT_FALSE(token.consume(true));
  }
}

TEST_F(PiccoPowerManagerTest, UsbWakeMasksTheRuntimeInterruptBeforeArmingLevelWake) {
  power_test::usbPowered = true;
  power_test::pressOnWake = true;
  power_test::timerWakesBeforeAction = 2;
  EXPECT_TRUE(manager.holdWhileUsbPowered(gpio));
  EXPECT_FALSE(power_test::unsafeLevelWake);
  EXPECT_FALSE(power_test::runtimeInterruptEnabled);
  EXPECT_EQ(power_test::lightSleeps, 3u);
  EXPECT_EQ(power_test::wakeCleanup, 3u);
}

TEST_F(PiccoPowerManagerTest, InterruptMaskFailureUsesPollingWithoutArmingLevelWake) {
  power_test::usbPowered = true;
  power_test::interruptDisableFails = true;
  power_test::pressAfterDelay = true;
  EXPECT_TRUE(manager.holdWhileUsbPowered(gpio));
  EXPECT_FALSE(power_test::unsafeLevelWake);
  EXPECT_EQ(power_test::lightSleeps, 0u);
}
