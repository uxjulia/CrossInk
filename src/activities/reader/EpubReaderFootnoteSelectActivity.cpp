#include "EpubReaderFootnoteSelectActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <optional>

#include "MappedInputManager.h"
#include "ReaderRuling.h"
#include "ReaderUtils.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "util/InputReleaseGuard.h"

void EpubReaderFootnoteSelectActivity::onEnter() {
  Activity::onEnter();
  ignoreInitialBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
  ignoreInitialConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  ignoreInitialPowerRelease = mappedInput.isPressed(MappedInputManager::Button::Power);
  for (size_t i = 0; i < page->footnotes.size() && i < targets.size(); ++i) {
    if (targets[i].width > 0 && targets[i].height > 0) selectable[selectableCount++] = static_cast<uint8_t>(i);
  }
  snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);
  if (!snapshot) LOG_ERR("FNS", "OOM allocating footnote highlight snapshot; using full repaint");
  requestUpdate();
}

void EpubReaderFootnoteSelectActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void EpubReaderFootnoteSelectActivity::performJump() {
  if (selected >= selectableCount) return;
  setResult(FootnoteResult{page->footnotes[selectable[selected]].href});
  finish();
}

void EpubReaderFootnoteSelectActivity::loop() {
  if (InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Back,
                                               ignoreInitialBackRelease) ||
      InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Power,
                                               ignoreInitialPowerRelease) ||
      InputReleaseGuard::consumeInitialRelease(mappedInput, MappedInputManager::Button::Confirm,
                                               ignoreInitialConfirmRelease)) {
    return;
  }

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }

  int touchX = 0;
  int touchY = 0;
  if (mappedInput.hasTouch() && mappedInput.wasScreenTapped(touchX, touchY)) {
    int bestIndex = -1;
    int64_t bestDistance = INT64_MAX;
    for (uint8_t i = 0; i < selectableCount; ++i) {
      const auto& target = targets[selectable[i]];
      if (touchX < target.x - 4 || touchX >= target.x + target.width + 4 || touchY < target.y - 4 ||
          touchY >= target.y + target.height + 4) {
        continue;
      }
      const int64_t dx = static_cast<int64_t>(touchX) - (target.x + target.width / 2);
      const int64_t dy = static_cast<int64_t>(touchY) - (target.y + target.height / 2);
      const int64_t distance = dx * dx + dy * dy;
      if (distance < bestDistance) {
        bestDistance = distance;
        bestIndex = i;
      }
    }
    if (bestIndex >= 0) {
      selected = static_cast<uint8_t>(bestIndex);
      performJump();
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    performJump();
    return;
  }
  if (selectableCount == 0) return;

  const unsigned long now = millis();
  const bool repeat = mappedInput.getHeldTime() >= REPEAT_START_MS && now - lastMoveTime >= REPEAT_INTERVAL_MS;
  const bool left = mappedInput.wasPressed(MappedInputManager::Button::Left) ||
                    (repeat && mappedInput.isPressed(MappedInputManager::Button::Left));
  const bool right = mappedInput.wasPressed(MappedInputManager::Button::Right) ||
                     (repeat && mappedInput.isPressed(MappedInputManager::Button::Right));
  if ((left || mappedInput.wasPressed(MappedInputManager::Button::Up)) && selected > 0) {
    --selected;
    lastMoveTime = now;
    requestUpdate();
  } else if ((right || mappedInput.wasPressed(MappedInputManager::Button::Down)) && selected + 1 < selectableCount) {
    ++selected;
    lastMoveTime = now;
    requestUpdate();
  }
}

bool EpubReaderFootnoteSelectActivity::drawHighlightWithSnapshot() {
  if (selected >= selectableCount) return false;
  const auto& target = targets[selectable[selected]];
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int x = std::max(0, static_cast<int>(target.x) - 2);
  const int y = std::max(0, static_cast<int>(target.y) - 2);
  const int w = std::min(screenW, static_cast<int>(target.x) + target.width + 2) - x;
  const int h = std::min(screenH, static_cast<int>(target.y) + target.height + 2) - y;
  if (w <= 0 || h <= 0) return false;

  const bool saved = snapshot && renderer.readFramebufferRegion(x, y, w, h, snapshot.get(), SNAPSHOT_CAPACITY) > 0;
  snapshotX = static_cast<int16_t>(x);
  snapshotY = static_cast<int16_t>(y);
  snapshotW = static_cast<int16_t>(w);
  snapshotH = static_cast<int16_t>(h);
  snapshotIdx = saved ? selected : -1;

  // Invert the already rendered marker. This preserves SD-font glyph pixels
  // without trying to rasterize text after the page prewarm scope has ended.
  renderer.invertRect(x, y, w, h);
  return saved;
}

void EpubReaderFootnoteSelectActivity::drawChrome() const {
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, TouchHeaderBackButton::headerRect(renderer, mappedInput), tr(STR_FOOTNOTES),
                                true);
  }
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
}

void EpubReaderFootnoteSelectActivity::render(RenderLock&&) {
  if (snapshotIdx >= 0 && selected != snapshotIdx) {
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
    if (drawHighlightWithSnapshot()) {
      drawChrome();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
  }

  snapshotIdx = -1;
  // The selector repaints the page after the reader has paused. SD font glyphs
  // may have been evicted, so use the same scan and prewarm sequence as reading.
  std::optional<FontCacheManager::PrewarmScope> pageRenderScope;
  if (auto* fcm = renderer.getFontCacheManager()) {
    const auto prewarmVisibleText = [&]() {
      pageRenderScope.emplace(*fcm, FontCacheManager::PreparationPolicy::Normal);
      page->renderText(renderer, fontId, marginLeft, marginTop);
      drawChrome();
      if (!pageRenderScope->endScanAndPrewarm()) {
        pageRenderScope.reset();
        return false;
      }
      return true;
    };

    bool prewarmSucceeded = prewarmVisibleText();
    if (!prewarmSucceeded && renderer.isSdCardFont(fontId)) {
      LOG_ERR("FNS", "SD-font page prewarm failed (font=%d free=%u maxAlloc=%u); releasing caches and retrying", fontId,
              ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      renderer.releaseSdCardFontForLowMemory(fontId, /*preserveAdvanceTable=*/true);
      prewarmSucceeded = prewarmVisibleText();
    }
    if (!prewarmSucceeded) {
      LOG_ERR("FNS", "Font page prewarm failed after retry (font=%d free=%u maxAlloc=%u)", fontId, ESP.getFreeHeap(),
              ESP.getMaxAllocHeap());
      renderer.clearScreen(ReaderUtils::readerBackgroundColor());
      GUI.drawPopup(renderer, tr(STR_MEMORY_ERROR));
      return;
    }
  }

  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  page->renderText(renderer, fontId, marginLeft, marginTop, ReaderUtils::readerForegroundBlack());
  ReaderRuling::drawSelectionPage(renderer, *page, fontId, marginLeft, marginTop);
  page->renderImages(renderer, fontId, marginLeft, marginTop, ReaderUtils::readerForegroundBlack());
  drawHighlightWithSnapshot();
  drawChrome();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
