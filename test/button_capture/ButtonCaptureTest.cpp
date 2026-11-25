#include <BoardConfig.h>
#include <InputManager.h>
#include <InputWake.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <util/ButtonNavigator.h>
#include <util/ButtonShortcutController.h>

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
uint32_t nextSampleAt = 0;
bool digitalWake = false;
void setButton(uint8_t button, bool pressed) {
  const auto& pins = BoardConfig::ACTIVE.input;
  if (BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::XteinkAdcLadder && button != InputManager::BTN_POWER) {
    const int levels[] = {3512, 2694, 1493, 5, 2242, 5};
    adcLevels[button < 4 ? 1 : 2] = pressed ? levels[button] : 4095;
    return;
  }
  const int8_t mapping[] = {pins.back, pins.confirm, pins.left, pins.right, pins.up, pins.down, pins.power};
  const int pin = mapping[button];
  assert(pin >= 0);
  const int active = button == InputManager::BTN_POWER && pins.powerActiveHigh ? HIGH : LOW;
  const int level = pressed ? active : 1 - active;
  if (gpioLevels[pin] != level) {
    edge(pin);
    digitalWake = true;
  }
}

void captureStep() {
  assert(fakeTask);
  taskRunning = true;
  try {
    fakeTask->function(fakeTask->arg);
  } catch (const FakeTaskYield& yielded) {
    nextSampleAt = millis() + yielded.ticks;
  }
  taskRunning = false;
  assert(criticalDepth == 0);
}
}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  const char* scenario = argv[1];
  std::fill(std::begin(gpioLevels), std::end(gpioLevels), HIGH);
  std::fill(std::begin(adcLevels), std::end(adcLevels), 4095);
  InputManager primary;
  beforeInputRead = [] { assert(criticalDepth == 0); };
  InputWake::setSharedConfirmPowerShortPressEmitsPower(true);
  assert(InputWake::begin());
  if (strcmp(scenario, "unsupported-style") == 0) {
    BoardConfig::ACTIVE.inputStyle = BoardConfig::InputStyle::DigitalTwoButton;
    assert(!InputWake::startCapture() && !InputWake::capturing());
    return 0;
  }
  if (strcmp(scenario, "task-failure") == 0) {
    failTask = true;
    assert(!InputWake::startCapture() && !InputWake::capturing());
    failTask = false;
  }
  if (strcmp(scenario, "startup-held") == 0) setButton(InputManager::BTN_DOWN, true);
  assert(InputWake::startCapture() && InputWake::capturing());
  if (strcmp(scenario, "stop") == 0) {
    duringWait = captureStep;
    InputWake::stopCapture();
    assert(!InputWake::capturing() && !fakeTask);
    return 0;
  }
  const bool bounce = strcmp(scenario, "bounce") == 0;
  const bool quick = strncmp(scenario, "quick-taps", 10) == 0;
  const uint32_t phase = quick && scenario[10] == '-' ? static_cast<uint32_t>(atoi(scenario + 11)) : 0;
  const bool burst = strcmp(scenario, "burst") == 0;
  const bool wrap = strcmp(scenario, "clock-wrap") == 0;
  const bool power = strcmp(scenario, "power-hold") == 0;
  const bool longSide = strcmp(scenario, "long-side") == 0;
  const bool startupHeld = strcmp(scenario, "startup-held") == 0;
  const bool overflow = strcmp(scenario, "overflow") == 0;
  const bool noise = strcmp(scenario, "noise") == 0;
  const bool chord = strcmp(scenario, "chord") == 0;
  const bool chordGap = strcmp(scenario, "chord-gap") == 0;
  if (wrap) fakeNow = UINT32_MAX - 100UL;
  const auto start = fakeNow;
  const uint32_t length = power || longSide ? 1200 : overflow ? 8000 : burst ? 2200 : 1000;
  nextSampleAt = start;
  for (uint32_t elapsed = 0; elapsed < length; ++elapsed) {
    fakeNow = start + elapsed;
    digitalWake = false;
    bool down = elapsed >= 20 && elapsed < 80;
    if (bounce) down = (elapsed >= 20 && elapsed < 76) || (elapsed >= 84 && elapsed < 108);
    if (quick)
      down = (elapsed >= 20 + phase && elapsed < 40 + phase) || (elapsed >= 60 + phase && elapsed < 80 + phase);
    if (burst || overflow) down = elapsed >= 20 && elapsed < (overflow ? 7900 : 1940) && (elapsed - 20) % 80 < 20;
    if (power || longSide) down = elapsed >= 20 && elapsed < 1000;
    if (startupHeld) down = elapsed < 80;
    if (noise) down = elapsed >= 20 && elapsed < 23;
    if (chord) down = elapsed >= 40 && elapsed < 120;
    if (chordGap) down = elapsed >= 60 && elapsed < 100;
    setButton(power ? InputManager::BTN_POWER : InputManager::BTN_DOWN, down);
    if (chord) setButton(InputManager::BTN_POWER, elapsed >= 20 && elapsed < 100);
    if (chordGap) setButton(InputManager::BTN_POWER, elapsed >= 20 && elapsed < 60);
    // Respect the production task's wait policy. Digital edges wake it;
    // ADC-only changes must wait for the sampler's actual next timed poll.
    if (digitalWake || static_cast<int32_t>(static_cast<uint32_t>(fakeNow) - nextSampleAt) >= 0) captureStep();
  }
  if (!noise) {
    assert(InputWake::hasPendingButtons());
    assert(InputWake::hasPendingButtons());  // Peeking never consumes queued edges.
  }
  ButtonInputBuffer::Frame frame;
  MappedInputManager mapped(frame);
  ButtonNavigator::setMappedInputManager(mapped);
  ButtonNavigator home, settings;
  int homeMoves = 0, settingMoves = 0, presses = 0, releases = 0;
  bool sawHeldPower = false;
  ButtonShortcutController shortcuts;
  if (longSide) shortcuts.toggleQuickLock(start, QuickLockTrigger::SideDownLong);
  bool unlocked = false;
  int screenshots = 0;
  uint32_t lastTimestamp = static_cast<uint32_t>(start);
  for (int frames = 0; frames < 200; ++frames) {
    frame = InputWake::takeFrame();
    assert(static_cast<uint32_t>(frame.timestamp - lastTimestamp) < 10000);
    lastTimestamp = frame.timestamp;
    if (frame.pressed) ++presses;
    if (frame.released) ++releases;
    if (strcmp(scenario, "slow-render") == 0) fakeNow += 350;
    home.onNext([&] { ++homeMoves; });
    settings.onNextRelease([&] { ++settingMoves; });
    settings.onNextContinuous([&] { ++settingMoves; });
    if (longSide && shortcuts.tryUnlockSide(frame.timestamp, false, false, false, false,
                                            frame.state & (1u << InputManager::BTN_DOWN),
                                            frame.pressed & (1u << InputManager::BTN_DOWN),
                                            frame.released & (1u << InputManager::BTN_DOWN), true, 700))
      unlocked = true;
    if (shortcuts
            .updatePowerDown(frame.state & ButtonInputBuffer::POWER_MASK, frame.state & (1u << InputManager::BTN_DOWN),
                             false)
            .event == ButtonShortcutController::Event::Screenshot)
      ++screenshots;
    if (frame.state & ButtonInputBuffer::POWER_MASK && frame.powerHeldMs > 500) sawHeldPower = true;
    if (!InputWake::wait(0, primary)) break;
    assert(frames != 199);
  }
  assert(!InputWake::hasPendingButtons());
  if (overflow) {
    assert(!frame.state);  // A full FIFO must not leave buttons stuck down.
  } else if (noise) {
    assert(presses == 0 && releases == 0 && homeMoves == 0 && settingMoves == 0);
  } else if (chord || chordGap) {
    assert(presses == 2 && releases == 2 && screenshots == (chord ? 1 : 0));
  } else if (startupHeld) {
    assert(presses == 0 && releases == 1 && homeMoves == 0);
  } else if (longSide) {
    assert(presses == 1 && releases == 1 && homeMoves == 2 && settingMoves == 1 && unlocked);
  } else if (power) {
    assert(presses == 1 && releases == 1 && sawHeldPower);
  } else {
    const int taps = burst ? 24 : quick ? 2 : 1;
    std::printf("%s %s: Home=%d Settings=%d expected=%d\n", BoardConfig::ACTIVE.name, scenario, homeMoves, settingMoves,
                taps);
    assert(presses == taps && releases == taps);
    assert(homeMoves == taps && settingMoves == taps);
  }
  assert(!frame.state);
}
