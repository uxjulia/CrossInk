#include "ChargingBookPreparation.h"

#include <Epub/Section.h>
#include <FsHelpers.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <MemoryBudget.h>

#ifndef SIMULATOR
#include <InputWake.h>
#endif

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "SleepActivity.h"
#include "activities/reader/EpubReaderActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/FolderBookIterator.h"

namespace {
constexpr char FRAME_SNAPSHOT[] = "/.crosspoint/charging_frame.tmp";

bool saveFrameSnapshot(const GfxRenderer& renderer) {
  HalFile file;
  if (!Storage.openFileForWrite("CHG", FRAME_SNAPSHOT, file)) return false;
  const auto size = renderer.getBufferSize();
  const bool written = file.write(renderer.getFrameBuffer(), size) == size;
  const bool closed = file.close();
  if (written && closed) return true;
  Storage.remove(FRAME_SNAPSHOT);
  LOG_ERR("CHG", "Cannot save sleep screen to SD");
  return false;
}

bool restoreFrameSnapshot(GfxRenderer& renderer) {
  HalFile file;
  bool restored = false;
  if (Storage.openFileForRead("CHG", FRAME_SNAPSHOT, file)) {
    const auto size = renderer.getBufferSize();
    restored = file.read(renderer.getFrameBuffer(), size) == size;
    file.close();
  }
  if (!Storage.remove(FRAME_SNAPSHOT)) LOG_ERR("CHG", "Cannot remove sleep screen snapshot");
  if (!restored) LOG_ERR("CHG", "Cannot restore sleep screen from SD");
  return restored;
}
}  // namespace

ChargingBookPreparation::ChargingBookPreparation()
#if FREEINK_DEVICE_PICCO
    : shortWake(true),
#else
    : shortWake(SETTINGS.shortPowerPressWakes()),
#endif
      wakeArmed(!gpio.isPressed(HalGPIO::BTN_POWER)) {
}

bool ChargingBookPreparation::hasExternalPower() {
#if FREEINK_DEVICE_X3 && !defined(SIMULATOR)
  // Host traffic confirms power even after the X3's charge current goes to zero.
  // The wall adapter fallback uses the gauge sample already polled by gpio.update().
  if (gpio.deviceIsX3()) return gpio.isUsbHostConnected() || gpio.isUsbConnectedCached();
#endif
  return gpio.isUsbConnected();
}

void ChargingBookPreparation::render(GfxRenderer& renderer, SleepActivity* sleep) {
  if (done.load()) return;
  scan(renderer, sleep);
  done.store(true);
}

void ChargingBookPreparation::scan(GfxRenderer& renderer, SleepActivity* sleep) {
  auto books = makeUniqueNoThrow<FolderBookIterator>(APP_STATE.chargePreparationFolder,
                                                     FolderBookIterator::TraversalLimit::Skip);
  if (!books) {
    LOG_ERR("CHG", "Cannot allocate folder walker");
    return;
  }
  HeapByteBuffer frame;
  const auto orientation = renderer.getOrientation();
  SectionBuildOptions options{};
  options.shouldCancel = [](void* ctx) { return static_cast<ChargingBookPreparation*>(ctx)->cancel.load(); };
  options.cancelContext = this;
  options.onBuildCheckpoint = [] { delay(1); };
  bool snapshotSaved = false;
  std::string bookPath;
  unsigned entriesSinceYield = 0;
  while (!cancel.load()) {
    if (++entriesSinceYield == 16) {
      delay(1);
      entriesSinceYield = 0;
      if (cancel.load()) break;
    }
    const auto result = books->next(bookPath);
    if (result == FolderBookIterator::Result::Done) break;
    if (result == FolderBookIterator::Result::Error) {
      LOG_ERR("CHG", "Could not finish scanning the selected folder");
      break;
    }
    if (result != FolderBookIterator::Result::Book || !FsHelpers::hasEpubExtension(bookPath)) continue;
    if (sleep && !snapshotSaved) {
      // Chapter layout borrows the actual framebuffer. Preserve the outgoing
      // screen for Quick Resume and Page Overlay without using the C3 heap.
#ifndef SIMULATOR
      if (MemoryBudget::canAllocatePsram(renderer.getBufferSize())) {
        frame = makePsramByteBufferNoThrow(renderer.getBufferSize());
      }
#endif
      if (frame) {
        memcpy(frame.get(), renderer.getFrameBuffer(), renderer.getBufferSize());
      } else if (!saveFrameSnapshot(renderer)) {
        return;
      }
      snapshotSaved = true;
      if (cancel.load()) break;
      sleep->paintSleepScreen();
      if (cancel.load()) break;
      const auto safe = UITheme::getInstance().getScreenSafeArea(renderer);
      const char* label = tr(STR_INDEXING);
      const int width = std::min(safe.width, renderer.getTextWidth(SMALL_FONT_ID, label) + 16);
      const int height = renderer.getLineHeight(SMALL_FONT_ID) + 8;
      const int x = safe.x + (safe.width - width) / 2;
      const int y = safe.y + safe.height - height;
      renderer.fillRect(x, y, width, height, false);
      renderer.drawRect(x, y, width, height);
      renderer.drawText(SMALL_FONT_ID, x + 8, y + 4, label);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
    EpubReaderActivity::prepareBookWhileCharging(bookPath, renderer, options);
    delay(1);
  }
  renderer.setOrientation(orientation);
  if (!snapshotSaved) return;
  if (frame) {
    memcpy(renderer.getFrameBuffer(), frame.get(), renderer.getBufferSize());
  } else if (!restoreFrameSnapshot(renderer)) {
    sleep->discardSavedBackground();
  }
}

bool ChargingBookPreparation::pollWake() {
  while (!wakeRequested) {
    gpio.update();
    const bool pressed = gpio.isPressed(HalGPIO::BTN_POWER);
    const bool heldLong = (pressed || gpio.wasReleased(HalGPIO::BTN_POWER)) &&
                          gpio.getPowerButtonHeldTime() >= CrossPointSettings::POWER_BUTTON_LONG_PRESS_MS;
    wakeRequested = wakeArmed && (shortWake ? (pressed || gpio.wasPressed(HalGPIO::BTN_POWER)) : heldLong);
    if (!pressed) wakeArmed = true;
#ifndef SIMULATOR
    if (!InputWake::hasQueuedFrames()) break;
#else
    break;
#endif
  }
  return wakeRequested;
}

bool ChargingBookPreparation::finishWake() {
  // An armed hold can cross the final screen refresh. Let it reach the wake
  // threshold or be released before the hardware sleep path takes over.
  const auto started = millis();
  while (!pollWake() && wakeArmed && gpio.isPressed(HalGPIO::BTN_POWER) &&
         millis() - started < CrossPointSettings::POWER_BUTTON_LONG_PRESS_MS) {
    delay(10);
  }
  return wakeRequested;
}

void ChargingBookPreparation::wait() {
  do {
    if (pollWake() || !hasExternalPower()) cancel.store(true);
    delay(10);
  } while (!done.load());
}
