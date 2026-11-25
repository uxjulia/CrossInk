#include <BoardConfig.h>
#include <InputManager.h>
#include <util/ButtonNavigator.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
void setButton(const uint8_t button, const bool pressed) {
  const auto& board = BoardConfig::ACTIVE;
  if (board.inputStyle == BoardConfig::InputStyle::XteinkAdcLadder) {
    const int values[] = {3512, 2694, 1493, 5, 2242, 5};
    adcLevels[button < 4 ? InputManager::BUTTON_ADC_PIN_1 : InputManager::BUTTON_ADC_PIN_2] =
        pressed ? values[button] : 4095;
  } else {
    const int8_t pins[] = {board.input.back,  board.input.confirm, board.input.left,
                           board.input.right, board.input.up,      board.input.down};
    assert(pins[button] >= 0);
    gpioLevels[pins[button]] = pressed ? LOW : HIGH;
  }
}

void sample(InputManager& input) {
  input.update();
  if (input.isDebouncePending()) {
    delay(6);
    input.update();
  }
}
}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  const char* scenario = argv[1];
  const bool releaseNavigation = strcmp(scenario, "release-navigation") == 0;
  const bool hold = strcmp(scenario, "hold") == 0 || strcmp(scenario, "release-hold") == 0 ||
                    strcmp(scenario, "custom-repeat") == 0 || strcmp(scenario, "timer-wrap") == 0 ||
                    strcmp(scenario, "repeat-at-zero") == 0 || strcmp(scenario, "release-at-zero") == 0;
  const bool releaseHold = strcmp(scenario, "release-hold") == 0 || strcmp(scenario, "release-at-zero") == 0;
  const bool customRepeat = strcmp(scenario, "custom-repeat") == 0;
  const bool repeatAtZero = strcmp(scenario, "repeat-at-zero") == 0 || strcmp(scenario, "release-at-zero") == 0;
  if (strcmp(scenario, "timer-wrap") == 0) fakeNow = UINT32_MAX - 300UL;
  if (repeatAtZero) fakeNow = UINT32_MAX - 506UL;
  std::fill(std::begin(gpioLevels), std::end(gpioLevels), HIGH);
  std::fill(std::begin(adcLevels), std::end(adcLevels), 4095);
  InputManager input;
  MappedInputManager mapped(input);
  ButtonNavigator::setMappedInputManager(mapped);
  const uint16_t startMs = customRepeat ? 250 : 500;
  const uint16_t intervalMs = customRepeat ? 75 : 500;
  ButtonNavigator navigator(intervalMs, startMs);
  sample(input);
  constexpr uint8_t nextButton = InputManager::BTN_DOWN;
  constexpr uint8_t previousButton = InputManager::BTN_UP;
  const bool previous = strcmp(scenario, "previous") == 0;
  const uint8_t button = previous ? previousButton : nextButton;
  setButton(button, true);
  sample(input);
  assert(input.wasPressed(button));
  if (repeatAtZero) fakeNow = UINT32_MAX - 500UL;
  // main.cpp polls input before waiting for the non-reader render lock. The
  // display can hold it past the repeat threshold before Home sees this edge.
  if (!hold) {
    // A regular 80ms physical tap can end while rendering blocks dispatch.
    // The sampled press stays latched until the main loop next updates input.
    delay(80);
    setButton(button, false);
    delay(520);
  } else if (strcmp(scenario, "hold") != 0 && strcmp(scenario, "timer-wrap") != 0 && !customRepeat && !repeatAtZero) {
    delay(600);
  }
  int calls = 0;
  const auto callback = [&] {
    ++calls;
    if (strcmp(scenario, "slow-callback") == 0) delay(600);
  };
  const auto navigate = [&] {
    if (releaseNavigation || releaseHold) {
      navigator.onNextRelease(callback);
      navigator.onNextContinuous(callback);
    } else if (strcmp(scenario, "press-only") == 0)
      navigator.onNextPress(callback);  // Dashboard's menu path.
    else if (previous)
      navigator.onPrevious(callback);
    else if (strcmp(scenario, "explicit-buttons") == 0)
      navigator.onPressAndContinuous({MappedInputManager::Button::Down}, callback);
    else
      navigator.onNext(callback);  // Lyra's Home path.
  };
  const int firstCalls = releaseNavigation || releaseHold ? 0 : 1;
  navigate();
  std::printf("%s %s: first dispatch produced %d action(s)\n", BoardConfig::ACTIVE.name, scenario, calls);
  std::fflush(stdout);
  assert(calls == firstCalls);
  const auto receivedAt = fakeNow;
  // The next loop must not replay a repeat based on the old physical timestamp.
  delay(10);
  sample(input);
  navigate();
  assert(calls == (releaseNavigation ? 1 : firstCalls));
  if (hold) {
    // A real hold still repeats, with independently configured start/interval.
    fakeNow = receivedAt + startMs;
    sample(input);
    navigate();
    assert(calls == firstCalls);
    delay(1);
    sample(input);
    navigate();
    assert(calls == firstCalls + 1);
    if (strcmp(scenario, "release-at-zero") == 0) {
      setButton(button, false);
      sample(input);
      navigate();
      assert(calls == firstCalls + 1);  // Releasing a repeat at millis()==0 is still consumed.
      return 0;
    }
    const auto repeatedAt = fakeNow;
    delay(1);
    sample(input);
    navigate();
    assert(calls == firstCalls + 1);
    fakeNow = repeatedAt + intervalMs;
    sample(input);
    navigate();
    assert(calls == firstCalls + 1);
    delay(1);
    sample(input);
    navigate();
    assert(calls == firstCalls + 2);
  }
  setButton(button, false);
  sample(input);
  navigate();
  assert(calls == (hold ? firstCalls + 2 : 1));
  if (strcmp(scenario, "rapid-taps") == 0) {
    for (int tap = 0; tap < 5; ++tap) {
      delay(20);
      setButton(button, true);
      sample(input);
      navigate();
      assert(calls == tap + 2);
      delay(20);
      setButton(button, false);
      sample(input);
      navigate();
      assert(calls == tap + 2);
    }
  }
}
