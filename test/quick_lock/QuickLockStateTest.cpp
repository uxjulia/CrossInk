#include <gtest/gtest.h>

#include <cstdint>

#include "ButtonShortcutController.h"
#include "QuickLockState.h"

TEST(QuickLockState, NeverTimeoutDoesNotSleep) {
  QuickLockState state;
  state.toggle(100U);
  EXPECT_FALSE(state.shouldSleep(100000U, 0U));
}

TEST(QuickLockState, TimeoutUsesWraparoundSafeElapsedTime) {
  QuickLockState state;
  state.toggle(UINT32_MAX - 20U);
  EXPECT_FALSE(state.shouldSleep(10U, 40U));
  EXPECT_TRUE(state.shouldSleep(25U, 40U));
}

TEST(ButtonShortcutController, QuickLockChordFiresOnceUntilFullRelease) {
  ButtonShortcutController controller;
  using Action = ButtonShortcutController::ChordAction;
  using Event = ButtonShortcutController::Event;

  EXPECT_EQ(controller.update(10U, true, true, false, false, Action::QuickLock).event, Event::QuickLockChanged);
  EXPECT_TRUE(controller.isQuickLocked());
  EXPECT_EQ(controller.update(11U, true, true, false, false, Action::QuickLock).event, Event::None);
  EXPECT_TRUE(controller.update(12U, false, false, false, false, Action::QuickLock).consumeInput);
  EXPECT_EQ(controller.update(13U, true, true, false, false, Action::QuickLock).event, Event::QuickLockChanged);
  EXPECT_FALSE(controller.isQuickLocked());
}

TEST(ButtonShortcutController, LockedStateBlocksOtherChordsAndShortPower) {
  ButtonShortcutController controller;
  using Action = ButtonShortcutController::ChordAction;
  using Event = ButtonShortcutController::Event;

  EXPECT_EQ(controller.update(10U, true, true, false, false, Action::QuickLock).event, Event::QuickLockChanged);
  controller.update(11U, false, false, false, false, Action::QuickLock);
  const auto blocked = controller.update(12U, true, true, false, false, Action::Screenshot);
  EXPECT_EQ(blocked.event, Event::None);
  EXPECT_TRUE(blocked.consumeInput);
  controller.update(13U, false, false, false, false, Action::Screenshot);
  EXPECT_EQ(controller.update(14U, false, false, true, false, Action::Screenshot).event, Event::None);
  EXPECT_TRUE(controller.isQuickLocked());
  EXPECT_EQ(controller.update(15U, true, true, false, false, Action::QuickLock).event, Event::QuickLockChanged);
  EXPECT_FALSE(controller.isQuickLocked());
}

TEST(ButtonShortcutController, ShortPowerQuickLockOnlyUnlocksOnShortPowerRelease) {
  ButtonShortcutController controller;
  using Action = ButtonShortcutController::ChordAction;
  using Event = ButtonShortcutController::Event;

  EXPECT_EQ(controller.update(10U, false, false, true, true, Action::Disabled).event, Event::QuickLockChanged);
  EXPECT_EQ(controller.quickLockTrigger(), QuickLockTrigger::ShortPower);
  EXPECT_EQ(controller.update(11U, true, true, false, false, Action::Screenshot).event, Event::None);
  controller.update(12U, false, false, false, false, Action::Screenshot);
  EXPECT_EQ(controller.update(13U, false, false, true, false, Action::Disabled).event, Event::QuickLockChanged);
  EXPECT_FALSE(controller.isQuickLocked());
}

TEST(ButtonShortcutController, LongPowerRequiresReleaseBeforeItCanUnlock) {
  ButtonShortcutController controller;

  controller.toggleQuickLock(10U, QuickLockTrigger::LongPower, true);
  EXPECT_TRUE(controller.isQuickLocked());
  EXPECT_FALSE(controller.tryUnlockLongPower(11U, true));
  EXPECT_FALSE(controller.tryUnlockLongPower(12U, false));
  EXPECT_TRUE(controller.tryUnlockLongPower(13U, true));
  EXPECT_FALSE(controller.isQuickLocked());
}

TEST(ButtonShortcutController, HomeQuickLockOnlyUnlocksWithTheMatchingHomeGesture) {
  ButtonShortcutController controller;

  constexpr QuickLockTrigger homeTriggers[] = {QuickLockTrigger::HomeTap, QuickLockTrigger::HomeDoubleTap,
                                               QuickLockTrigger::HomeLongPress};
  for (const auto trigger : homeTriggers) {
    controller.toggleQuickLock(10U, trigger);

    EXPECT_FALSE(controller.tryUnlockWithTrigger(11U, QuickLockTrigger::ShortPower));
    EXPECT_TRUE(controller.isQuickLocked());
    EXPECT_TRUE(controller.tryUnlockWithTrigger(12U, trigger));
    EXPECT_FALSE(controller.isQuickLocked());
  }
}

TEST(ButtonShortcutController, PageTurnChordEmitsPageTurn) {
  ButtonShortcutController controller;
  using Action = ButtonShortcutController::ChordAction;
  using Event = ButtonShortcutController::Event;

  EXPECT_EQ(controller.update(1U, true, true, false, false, Action::PageTurn).event, Event::PageTurn);
}

TEST(ButtonShortcutController, UpDownChordWorksWithoutTouchscreenEscapeHatch) {
  ButtonShortcutController controller;
  using Action = ButtonShortcutController::ChordAction;
  using Event = ButtonShortcutController::Event;

  const auto result = controller.updateUpDown(1U, true, true, Action::QuickActions, false);
  EXPECT_EQ(result.event, Event::ConfiguredAction);
  EXPECT_TRUE(result.consumeInput);
  EXPECT_EQ(result.action, Action::QuickActions);
}

TEST(ButtonShortcutController, IdleUpDownDoesNotPreemptReaderQuickLockUnlock) {
  ButtonShortcutController controller;
  controller.toggleQuickLock(1U, QuickLockTrigger::LongMenu);

  const auto result = controller.updateUpDown(2U, false, false, ButtonShortcutController::ChordAction::Disabled, false);

  EXPECT_EQ(result.event, ButtonShortcutController::Event::None);
  EXPECT_FALSE(result.consumeInput);
  EXPECT_TRUE(controller.isQuickLocked());
}

TEST(ButtonShortcutController, EveryChordActionConsumesBothReleaseOrders) {
  using Action = ButtonShortcutController::ChordAction;
  constexpr Action actions[] = {
      Action::Screenshot,       Action::QuickLock,           Action::Sleep,
      Action::PageTurn,         Action::ToggleBookmark,      Action::ReadingStats,
      Action::MarkFinished,     Action::ForceRefresh,        Action::ToggleFont,
      Action::ToggleGuideDots,  Action::ToggleBionicReading, Action::CyclePageTurn,
      Action::SyncProgress,     Action::FileTransfer,        Action::CalibreWireless,
      Action::JoinNetwork,      Action::CreateHotspot,       Action::ToggleDarkMode,
      Action::Footnotes,        Action::FileBrowser,         Action::CreateClipping,
      Action::LookupWord,       Action::ToggleHomeButton,    Action::QuickActions,
      Action::ToggleFrontlight, Action::ToggleTouchscreen,
  };

  for (const auto action : actions) {
    for (const bool releasePowerFirst : {false, true}) {
      ButtonShortcutController controller;

      EXPECT_NE(controller.update(1U, true, true, false, false, action).event, ButtonShortcutController::Event::None);
      const auto firstRelease = controller.update(2U, !releasePowerFirst, releasePowerFirst, false, false, action);
      EXPECT_EQ(firstRelease.event, ButtonShortcutController::Event::None);
      EXPECT_TRUE(firstRelease.consumeInput);
      const auto secondRelease = controller.update(3U, false, false, false, false, action);
      EXPECT_EQ(secondRelease.event, ButtonShortcutController::Event::None);
      EXPECT_TRUE(secondRelease.consumeInput);
      const auto afterRelease = controller.update(4U, false, false, false, false, action);
      EXPECT_EQ(afterRelease.consumeInput, action == Action::QuickLock);
    }
  }
}

TEST(ButtonShortcutController, ModalConsumesPowerUpChordThroughBothReleases) {
  ButtonShortcutController controller;
  using Action = ButtonShortcutController::ChordAction;

  const auto started = controller.update(1U, true, true, false, false, Action::QuickLock, true);
  EXPECT_EQ(started.event, ButtonShortcutController::Event::None);
  EXPECT_TRUE(started.consumeInput);
  EXPECT_FALSE(controller.isQuickLocked());
  EXPECT_TRUE(controller.update(2U, false, true, false, false, Action::QuickLock, true).consumeInput);
  EXPECT_TRUE(controller.update(3U, false, false, false, false, Action::QuickLock, true).consumeInput);
  EXPECT_FALSE(controller.update(4U, false, false, false, false, Action::QuickLock, false).consumeInput);
}

TEST(ButtonShortcutController, ModalConsumesPowerDownScreenshotChordThroughBothReleaseOrders) {
  for (const bool releasePowerFirst : {false, true}) {
    ButtonShortcutController controller;
    const auto started = controller.updatePowerDown(true, true, true);
    EXPECT_EQ(started.event, ButtonShortcutController::Event::None);
    EXPECT_TRUE(started.consumeInput);
    EXPECT_TRUE(controller.updatePowerDown(!releasePowerFirst, releasePowerFirst, true).consumeInput);
    EXPECT_TRUE(controller.updatePowerDown(false, false, true).consumeInput);
    EXPECT_FALSE(controller.updatePowerDown(false, false, false).consumeInput);
  }
}

TEST(ButtonShortcutController, QuickLockConsumesPowerDownScreenshotChord) {
  ButtonShortcutController controller;
  controller.toggleQuickLock(1U, QuickLockTrigger::ShortPower);

  const auto result = controller.updatePowerDown(true, true, true);

  EXPECT_EQ(result.event, ButtonShortcutController::Event::None);
  EXPECT_TRUE(result.consumeInput);
  EXPECT_TRUE(controller.isQuickLocked());
}

TEST(ButtonShortcutController, ModalConsumesUpDownChordThroughBothReleaseOrders) {
  using Action = ButtonShortcutController::ChordAction;

  for (const bool releaseUpFirst : {false, true}) {
    ButtonShortcutController controller;
    const auto started = controller.updateUpDown(1U, true, true, Action::QuickActions, false, true);
    EXPECT_EQ(started.event, ButtonShortcutController::Event::None);
    EXPECT_TRUE(started.consumeInput);
    EXPECT_TRUE(
        controller.updateUpDown(2U, !releaseUpFirst, releaseUpFirst, Action::QuickActions, false, true).consumeInput);
    EXPECT_TRUE(controller.updateUpDown(3U, false, false, Action::QuickActions, false, true).consumeInput);
    EXPECT_FALSE(controller.updateUpDown(4U, false, false, Action::QuickActions, false, false).consumeInput);
  }
}
