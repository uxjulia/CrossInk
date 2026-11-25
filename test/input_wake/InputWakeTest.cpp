#include <BoardConfig.h>
#include <InputManager.h>
#include <InputWake.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
  assert(argc == 2);
  const char* scenario = argv[1];
  const auto& board = BoardConfig::ACTIVE;
  std::fill(std::begin(gpioLevels), std::end(gpioLevels), HIGH);
  std::fill(std::begin(adcLevels), std::end(adcLevels), 4095);
  if (strcmp(scenario, "timeout-wrap") == 0) fakeNow = UINT32_MAX - 20UL;
  InputManager input;
  input.update();
  const bool analog = board.inputStyle == BoardConfig::InputStyle::XteinkAdcLadder;
  const auto wait = [&](uint32_t ms) { return InputWake::wait(ms, input); };
  if (strcmp(scenario, "native-fallback") == 0) {
    assert(!InputWake::begin());
    assert(serviceCalls == 0 && signalCreates == 0);
    const auto start = fakeNow;
    assert(!wait(50));
    assert(fakeNow - start == 50);
    return 0;
  }
  if (strcmp(scenario, "original-esp32") == 0) {
    BoardConfig::ACTIVE.input.up = 36;
    BoardConfig::ACTIVE.input.down = 39;
    BoardConfig::ACTIVE.touch.irq = 36;
    assert(InputWake::begin());
    assert(!wakeHandlers[36].handler && !wakeHandlers[39].handler);
    edge(board.input.power);
    assert(wait(50));
    return 0;
  }
  if (strcmp(scenario, "toggle") == 0) BoardConfig::ACTIVE.input.toggleSwitch = 30;
  if (strcmp(scenario, "touch-attach-failure") == 0) {
    if (board.touch.controller != BoardConfig::TouchController::Gt911) return 0;
    failAttachPin = board.touch.irq;
  }
  if (strcmp(scenario, "other-touch-controller") == 0) {
    if (board.touch.irq < 0) return 0;
    BoardConfig::ACTIVE.touch.controller = BoardConfig::TouchController::Cst816s;
    wakeHandlers[board.touch.irq].handler = [](void*) {};
  }
  const auto originalTouchHandler = board.touch.irq < 0 ? nullptr : wakeHandlers[board.touch.irq].handler;
  if (strcmp(scenario, "signal-failure") == 0) failSignal = true;
  if (strcmp(scenario, "service-failure") == 0) serviceResult = ESP_FAIL;
  if (strcmp(scenario, "partial-attach-failure") == 0) failAttachPin = analog ? board.input.power : board.input.down;
  if (strcmp(scenario, "partial-type-failure") == 0) failTypePin = analog ? board.input.power : board.input.down;
  if (strcmp(scenario, "shared-service") == 0) serviceResult = ESP_ERR_INVALID_STATE;
  const bool failure = strstr(scenario, "failure") != nullptr;
  assert(InputWake::begin() == !failure);
  if (failure) {
    for (const auto& pin : wakeHandlers) assert(pin.handler == nullptr);
    const auto start = fakeNow;
    assert(!wait(50));
    assert(fakeNow - start == 50);
    failSignal = false;
    serviceResult = ESP_OK;
    failAttachPin = failTypePin = -1;
    assert(InputWake::begin());
  }
  if (strcmp(scenario, "other-touch-controller") == 0) {
    assert(wakeHandlers[board.touch.irq].handler == originalTouchHandler);
  }
  const auto attached = attachCalls;
  assert(InputWake::begin());
  assert(attachCalls == attached);  // Shared Confirm/Power GPIO must be deduplicated.
  if (!analog) {
    for (int pin : {board.input.back, board.input.confirm, board.input.left, board.input.right, board.input.up,
                    board.input.down, board.input.power}) {
      if (pin < 0) continue;
      assert(wakeHandlers[pin].type == GPIO_INTR_ANYEDGE);
      for (int transition = 0; transition < 2; ++transition) {
        edge(pin);
        assert(InputWake::hasPendingEdge());
        assert(InputWake::hasPendingEdge());  // Reading the latch never consumes it.
        const auto start = fakeNow;
        assert(wait(50));
        assert(!InputWake::hasPendingEdge());
        assert(fakeNow == start);
      }
    }
    duringWait = [] { edge(BoardConfig::ACTIVE.input.up); };
    auto start = fakeNow;
    assert(wait(50));
    assert(fakeNow == start);
    duringWait = nullptr;
    assert(!wait(50));  // Held key must not cause a level-interrupt storm.
    assert(fakeNow - start == 50);
    edge(board.input.up);
    edge(board.input.up);
    edge(board.input.up);
    assert(wait(50));
    start = fakeNow;
    assert(!wait(10));
    assert(fakeNow - start == 10);
  } else {
    // Do not attach interrupts to the two ADC ladders.
    assert(!wakeHandlers[InputManager::BUTTON_ADC_PIN_1].handler);
    assert(!wakeHandlers[InputManager::BUTTON_ADC_PIN_2].handler);
    // Reuse the real SDK's six ADC bands, and verify both edges of each key.
    const int values[] = {3512, 2694, 1493, 5, 2242, 5};
    for (int button = 0; button < InputManager::BTN_POWER; ++button) {
      const auto pin = button < 4 ? InputManager::BUTTON_ADC_PIN_1 : InputManager::BUTTON_ADC_PIN_2;
      adcLevels[pin] = values[button];
      auto start = fakeNow;
      assert(wait(50));  // Change between update and wait is not missed.
      assert(fakeNow == start && !input.wasAnyPressed());
      input.update();
      assert(input.isDebouncePending());
      assert(wait(50));  // Pending debounce does not wait for a slow poll.
      fakeNow += 6;
      input.update();
      assert(input.isPressed(button) && input.wasPressed(button));
      assert(!wait(50));
      assert(input.wasPressed(button));  // Raw checks do not consume events.
      adcLevels[pin] = 4095;
      assert(wait(50));
      input.update();
      fakeNow += 6;
      input.update();
      assert(input.wasReleased(button));
    }
    const auto readsBefore = adcReads;
    duringWait = [] { adcLevels[InputManager::BUTTON_ADC_PIN_2] = 5; };
    const auto start = fakeNow;
    assert(wait(50));
    assert(fakeNow - start == 10);  // Quick raw sample wakes without a full SDK update.
    assert(adcReads > readsBefore && !input.wasAnyPressed());
    duringWait = nullptr;
    adcLevels[InputManager::BUTTON_ADC_PIN_2] = 4095;
  }
  if (board.touch.controller == BoardConfig::TouchController::Gt911) {
    edge(board.touch.irq);
    assert(InputWake::hasPendingEdge());
    assert(InputWake::hasPendingEdge());
    assert(wait(50));
    assert(!InputWake::hasPendingEdge());
  }
  if (board.input.toggleSwitch >= 0) {
    edge(board.input.toggleSwitch);
    assert(wait(50));
  }
  const auto start = fakeNow;
  assert(!wait(0));
  assert(fakeNow == start);
  std::printf("%s: %s passed\n", board.name, scenario);
}
