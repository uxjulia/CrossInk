#include <gtest/gtest.h>

#include "ReaderTiltPageTurn.h"

TEST(ReaderTiltPageTurn, DefaultModePreservesPreviousAndNext) {
  HalTiltSensor sensor;
  sensor.forward = true;
  auto turn = detectReaderTiltPageTurn(sensor, true, false);
  EXPECT_FALSE(turn.prev);
  EXPECT_TRUE(turn.next);

  sensor.backward = true;
  turn = detectReaderTiltPageTurn(sensor, true, false);
  EXPECT_TRUE(turn.prev);
  EXPECT_FALSE(turn.next);
}

TEST(ReaderTiltPageTurn, EitherDirectionAdvancesWhenEnabled) {
  HalTiltSensor sensor;
  sensor.forward = true;
  auto turn = detectReaderTiltPageTurn(sensor, true, true);
  EXPECT_FALSE(turn.prev);
  EXPECT_TRUE(turn.next);

  sensor.backward = true;
  turn = detectReaderTiltPageTurn(sensor, true, true);
  EXPECT_FALSE(turn.prev);
  EXPECT_TRUE(turn.next);
}

TEST(ReaderTiltPageTurn, SimultaneousEventsProduceOneAdvanceWithoutReplaying) {
  HalTiltSensor sensor;
  sensor.forward = sensor.backward = true;
  auto turn = detectReaderTiltPageTurn(sensor, true, true);
  EXPECT_FALSE(turn.prev);
  EXPECT_TRUE(turn.next);
  EXPECT_EQ(sensor.forwardReads, 1);
  EXPECT_EQ(sensor.backwardReads, 1);

  turn = detectReaderTiltPageTurn(sensor, true, true);
  EXPECT_FALSE(turn.prev);
  EXPECT_FALSE(turn.next);
}

TEST(ReaderTiltPageTurn, MasterOffDiscardsPendingEventsBeforeReenabling) {
  for (const bool nextOnly : {false, true}) {
    HalTiltSensor sensor;
    sensor.forward = sensor.backward = true;
    auto turn = detectReaderTiltPageTurn(sensor, false, nextOnly);
    EXPECT_FALSE(turn.prev);
    EXPECT_FALSE(turn.next);

    turn = detectReaderTiltPageTurn(sensor, true, nextOnly);
    EXPECT_FALSE(turn.prev);
    EXPECT_FALSE(turn.next);
  }
}

TEST(ReaderTiltPageTurn, SwitchingOffRestoresBackwardPageTurns) {
  HalTiltSensor sensor;
  sensor.backward = true;
  EXPECT_TRUE(detectReaderTiltPageTurn(sensor, true, true).next);
  sensor.backward = true;
  const auto turn = detectReaderTiltPageTurn(sensor, true, false);
  EXPECT_TRUE(turn.prev);
  EXPECT_FALSE(turn.next);
}
