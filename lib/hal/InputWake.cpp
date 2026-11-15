#include "InputWake.h"

#include <Arduino.h>
#include <InputManager.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <BoardConfig.h>
#include <Logging.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>

namespace {
// Static task/queue storage avoids heap allocation in input and page-turn paths.
StaticSemaphore_t wakeStorage;
SemaphoreHandle_t wakeSignal = nullptr;
StaticSemaphore_t sampleStorage, stoppedStorage, modeStorage;
SemaphoreHandle_t sampleSignal = nullptr, stoppedSignal = nullptr, modeMutex = nullptr;
uint64_t wakePins = 0;
constexpr uint32_t ADC_POLL_MS = 10;
constexpr uint32_t CAPTURE_STACK_BYTES = 2048;
// A separate, never-begun decoder reads existing SDK button mappings without
// initializing touch/I2C a second time. Only this task accesses its state.
InputManager captureDecoder;
ButtonInputBuffer buttonBuffer;
portMUX_TYPE bufferMux = portMUX_INITIALIZER_UNLOCKED;
StaticTask_t captureTaskStorage;
StackType_t captureStack[CAPTURE_STACK_BYTES / sizeof(StackType_t)];
TaskHandle_t captureTask = nullptr;
std::atomic<bool> stopRequested{false};
bool captureEnabled = false;  // Main-task owned; the ISR only signals semaphores.

void IRAM_ATTR onInputChange(void*) {
  BaseType_t taskWoken = pdFALSE;
  xSemaphoreGiveFromISR(wakeSignal, &taskWoken);
  xSemaphoreGiveFromISR(sampleSignal, &taskWoken);
  if (taskWoken == pdTRUE) portYIELD_FROM_ISR();
}

bool addWakePin(const int8_t pin) {
  if (pin < 0) return true;
  if (pin >= 64) {
    LOG_ERR("INPUT", "Invalid input wake GPIO%d", pin);
    return false;
  }
#if CONFIG_IDF_TARGET_ESP32
  // Original ESP32 GPIO36/39 can produce false edges with ADC/Wi-Fi activity.
  // CrossInk's C3/S3 targets do not have that erratum; retain timed fallback
  // if a custom firmware board uses these original-ESP32 inputs.
  if (pin == 36 || pin == 39) return true;
#endif
  const uint64_t bit = uint64_t{1} << pin;
  if (wakePins & bit) return true;
  const auto gpioPin = static_cast<gpio_num_t>(pin);
  // Permanent edge handlers latch changes throughout the main loop. Arming
  // against a freshly read level only at wait() would miss an earlier press.
  if (gpio_set_intr_type(gpioPin, GPIO_INTR_ANYEDGE) != ESP_OK ||
      gpio_isr_handler_add(gpioPin, onInputChange, nullptr) != ESP_OK) {
    gpio_intr_disable(gpioPin);
    LOG_ERR("INPUT", "Could not attach input wake to GPIO%d", pin);
    return false;
  }
  wakePins |= bit;
  return true;
}

void removeWakePins() {
  for (uint8_t pin = 0; pin < 64; ++pin) {
    if (!(wakePins & (uint64_t{1} << pin))) continue;
    gpio_isr_handler_remove(static_cast<gpio_num_t>(pin));
  }
  wakePins = 0;
}

bool waitForSignal(const uint32_t timeoutMs) {
  if (wakePins == 0) {
    delay(timeoutMs);
    return false;
  }
  // Never drain before blocking: an edge during earlier work must stay latched.
  const TickType_t ticks = pdMS_TO_TICKS(timeoutMs);
  return xSemaphoreTake(wakeSignal, timeoutMs == 0 || ticks > 0 ? ticks : 1) == pdTRUE;
}

bool analogButtonsChanged(InputManager& input) {
  // The SDK diagnostic sampler shares its real ADC bands with normal decoding
  // and does not mutate button state or consume one-shot events.
  InputManager::ButtonAdcSample front{}, side{};
  input.readButtonAdc(front, side);
  for (uint8_t button = InputManager::BTN_BACK; button < InputManager::BTN_POWER; ++button) {
    const bool pressed = front.button == button || side.button == button;
    if (pressed != input.isPressed(button)) return true;
  }
  const auto& pins = BoardConfig::ACTIVE.input;
  return pins.power >= 0 &&
         ((digitalRead(pins.power) == (pins.powerActiveHigh ? HIGH : LOW)) != input.isPressed(InputManager::BTN_POWER));
}

uint8_t sampleButtons(uint8_t& pulses, uint32_t& heldMs, uint32_t& powerMs) {
  pulses = 0;
  heldMs = powerMs = 0;
  if (BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::DigitalConfirmPowerHold) {
    // getState() shares the SDK's pin maps and ADC bands. Firmware debounce
    // below runs on capture time, rather than on unpredictable UI polls.
    return captureDecoder.getState();
  }
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  captureDecoder.update();
  uint8_t state = 0;
  for (uint8_t button = 0; button < ButtonInputBuffer::BUTTON_COUNT; ++button) {
    if (captureDecoder.isPressed(button)) state |= 1u << button;
    if (captureDecoder.wasPressed(button) && captureDecoder.wasReleased(button)) pulses |= 1u << button;
  }
  heldMs = captureDecoder.getHeldTime();
  powerMs = captureDecoder.getPowerButtonHeldTime();
  xSemaphoreGive(modeMutex);
  return state;
}

void captureButtons(void*) {
  for (;;) {
    if (stopRequested.load()) {
      xSemaphoreGive(stoppedSignal);
      vTaskSuspend(nullptr);  // Main deletes us only after all locks are released.
      continue;
    }
    uint8_t pulses;
    uint32_t heldMs, powerMs;
    const uint8_t raw = sampleButtons(pulses, heldMs, powerMs);
    const uint32_t now = millis();
    portENTER_CRITICAL(&bufferMux);
    const bool changed = buttonBuffer.sample(raw, now, heldMs, powerMs);
    buttonBuffer.pulse(pulses, heldMs, now);
    const bool debouncing = buttonBuffer.debouncing();
    const bool held = buttonBuffer.held();
    portEXIT_CRITICAL(&bufferMux);
    if (changed || pulses) xSemaphoreGive(wakeSignal);
    const bool analog = BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::XteinkAdcLadder;
    const bool sharedHeld = BoardConfig::ACTIVE.inputStyle == BoardConfig::InputStyle::DigitalConfirmPowerHold &&
                            captureDecoder.isPowerButtonPhysicallyPressed();
    const uint32_t waitMs = debouncing || (analog && held) ? 2 : analog || sharedHeld || wakePins == 0 ? 10 : 250;
    const TickType_t ticks = pdMS_TO_TICKS(waitMs);
    xSemaphoreTake(sampleSignal, ticks > 0 ? ticks : 1);
  }
}
}  // namespace

bool InputWake::begin() {
  if (wakePins != 0) return true;
  const auto& board = BoardConfig::ACTIVE;
  if (wakeSignal == nullptr) wakeSignal = xSemaphoreCreateBinaryStatic(&wakeStorage);
  if (sampleSignal == nullptr) sampleSignal = xSemaphoreCreateBinaryStatic(&sampleStorage);
  if (wakeSignal == nullptr || sampleSignal == nullptr) {
    LOG_ERR("INPUT", "Could not initialize input wake signal");
    return false;
  }
  // The display may already own the shared GPIO ISR service. Do not replace it.
#ifdef ARDUINO_ISR_FLAG
  const auto result = gpio_install_isr_service(ARDUINO_ISR_FLAG);
#else
  const auto result = gpio_install_isr_service(0);
#endif
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
    LOG_ERR("INPUT", "Could not initialize input wake interrupts (%d)", static_cast<int>(result));
    return false;
  }
  // Analog ladders cannot reliably raise GPIO edges for every key. Their power
  // and toggle pins are still digital; wait() samples the ladders separately.
  const bool analog = board.inputStyle == BoardConfig::InputStyle::XteinkAdcLadder ||
                      board.inputStyle == BoardConfig::InputStyle::OnePageAdcLadder;
  if (!analog) {
    for (const int8_t pin : {board.input.back, board.input.confirm, board.input.left, board.input.right, board.input.up,
                             board.input.down}) {
      if (!addWakePin(pin)) {
        removeWakePins();
        return false;
      }
    }
  }
  for (const int8_t pin : {board.input.power, board.input.toggleSwitch}) {
    if (!addWakePin(pin)) {
      removeWakePins();
      return false;
    }
  }
  if (board.touch.controller == BoardConfig::TouchController::Gt911 && !addWakePin(board.touch.irq)) {
    removeWakePins();
    return false;
  }
  return wakePins != 0;
}

bool InputWake::wait(const uint32_t timeoutMs, InputManager& input) {
  if (captureEnabled) {
    portENTER_CRITICAL(&bufferMux);
    const bool pending = buttonBuffer.pending();
    portEXIT_CRITICAL(&bufferMux);
    if (pending) return true;
    const TickType_t ticks = pdMS_TO_TICKS(timeoutMs);
    return xSemaphoreTake(wakeSignal, timeoutMs == 0 || ticks > 0 ? ticks : 1) == pdTRUE;
  }
  if (input.isDebouncePending()) return true;
  if (BoardConfig::ACTIVE.inputStyle != BoardConfig::InputStyle::XteinkAdcLadder) {
    return waitForSignal(timeoutMs);
  }
  const uint32_t startedAt = millis();
  do {
    if (analogButtonsChanged(input)) return true;
    const uint32_t elapsed = millis() - startedAt;
    if (elapsed >= timeoutMs) return waitForSignal(0);
    if (waitForSignal(std::min(ADC_POLL_MS, timeoutMs - elapsed))) return true;
  } while (true);
}

bool InputWake::startCapture() {
  if (captureEnabled) return true;
  if (captureTask || !wakeSignal || !sampleSignal) return false;
  const auto style = BoardConfig::ACTIVE.inputStyle;
  if (style != BoardConfig::InputStyle::XteinkAdcLadder && style != BoardConfig::InputStyle::DigitalButtons &&
      style != BoardConfig::InputStyle::DigitalConfirmPowerHold) {
    LOG_ERR("INPUT", "Button capture unavailable for this input style; using polling fallback");
    return false;
  }
  if (!stoppedSignal) stoppedSignal = xSemaphoreCreateBinaryStatic(&stoppedStorage);
  if (!modeMutex) modeMutex = xSemaphoreCreateMutexStatic(&modeStorage);
  if (!stoppedSignal || !modeMutex) {
    LOG_ERR("INPUT", "Could not initialize button capture; using polling fallback");
    return false;
  }
  uint8_t pulses;
  uint32_t heldMs, powerMs;
  const uint8_t raw = sampleButtons(pulses, heldMs, powerMs);
  buttonBuffer.reset(raw, millis());  // Absorb startup/wake holds without injecting a click.
  stopRequested.store(false);
  captureTask = xTaskCreateStatic(captureButtons, "ButtonCapture", CAPTURE_STACK_BYTES, nullptr, 2, captureStack,
                                  &captureTaskStorage);
  captureEnabled = captureTask != nullptr;
  if (!captureEnabled)
    LOG_ERR("INPUT", "Could not start button capture; using polling fallback");
  else
    LOG_INF("INPUT", "Button capture started: %u-byte stack, %u buffered frames", CAPTURE_STACK_BYTES,
            static_cast<unsigned>(ButtonInputBuffer::CAPACITY));
  return captureEnabled;
}

void InputWake::stopCapture() {
  if (!captureEnabled) return;
  stopRequested.store(true);
  xSemaphoreGive(sampleSignal);
  if (xSemaphoreTake(stoppedSignal, pdMS_TO_TICKS(100)) == pdTRUE) {
    vTaskDelete(captureTask);
    captureTask = nullptr;
  } else {
    LOG_ERR("INPUT", "Button capture did not stop before sleep");
  }
  captureEnabled = false;
}

bool InputWake::capturing() { return captureEnabled; }

bool InputWake::hasPendingEdge() { return wakeSignal && uxSemaphoreGetCount(wakeSignal) != 0; }

bool InputWake::hasPendingButtons() {
  if (!captureEnabled) return false;
  portENTER_CRITICAL(&bufferMux);
  const bool busy = buttonBuffer.pending() || buttonBuffer.debouncing() || buttonBuffer.held();
  portEXIT_CRITICAL(&bufferMux);
  return busy;
}

ButtonInputBuffer::Frame InputWake::takeFrame() {
  static uint32_t reportedLost = 0;
  static uint32_t lastStackReport = 0;
  portENTER_CRITICAL(&bufferMux);
  const auto frame = buttonBuffer.take(millis());
  const auto lost = buttonBuffer.lostEdges();
  portEXIT_CRITICAL(&bufferMux);
  if (lost != reportedLost) {
    LOG_ERR("INPUT", "Button FIFO exceeded %u frames (%lu edges lost)",
            static_cast<unsigned>(ButtonInputBuffer::CAPACITY), static_cast<unsigned long>(lost));
    reportedLost = lost;
  }
  const uint32_t now = millis();
  if (captureTask && now - lastStackReport >= 10000) {
    lastStackReport = now;
    LOG_DBG("INPUT", "Button capture stack minimum free: %u bytes",
            static_cast<unsigned>(uxTaskGetStackHighWaterMark(captureTask)));
  }
  return frame;
}

void InputWake::setSharedConfirmPowerShortPressEmitsPower(bool enabled) {
  if (modeMutex) xSemaphoreTake(modeMutex, portMAX_DELAY);
  InputManager::setSharedConfirmPowerShortPressEmitsPower(enabled);
  if (modeMutex) xSemaphoreGive(modeMutex);
}

#else
bool InputWake::begin() { return false; }
bool InputWake::wait(const uint32_t timeoutMs, InputManager&) {
  delay(timeoutMs);
  return false;
}
bool InputWake::startCapture() { return false; }
void InputWake::stopCapture() {}
bool InputWake::capturing() { return false; }
bool InputWake::hasPendingButtons() { return false; }
bool InputWake::hasPendingEdge() { return false; }
ButtonInputBuffer::Frame InputWake::takeFrame() { return {}; }
void InputWake::setSharedConfirmPowerShortPressEmitsPower(bool enabled) {
  InputManager::setSharedConfirmPowerShortPressEmitsPower(enabled);
}
#endif
