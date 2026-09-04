#include "DictionaryWordSelectActivity.h"

#include <BidiUtils.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <MemoryBudget.h>
#include <SdCardFont.h>
#include <Utf8.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

#include "../settings/DictionarySelectActivity.h"
#include "ClipSelectionPaging.h"
#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "MappedInputManager.h"
#include "Memory.h"
#include "PageWordGeometry.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"

namespace {

// Soft-hyphen U+00AD encoded as 2 UTF-8 bytes. Layout (ParsedText.cpp:19)
// strips these before measurement, so we mirror that here — otherwise
// derived word widths include the soft-hyphen glyph's advance and the
// highlight rectangle overruns into the inter-word gap.
constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;
constexpr size_t WORD_SELECT_ALLOCATION_HEADROOM = 8U * 1024U;
constexpr size_t ADVANCE_CODEPOINT_CAPACITY = SdCardFont::MAX_PAGE_GLYPHS;

struct WorkingSetBudget {
  size_t wordCount = 0;
  size_t rowCount = 0;
  size_t textBytes = 0;
  size_t maxSourceWordBytes = 0;
};

struct WordPartRef {
  const char* text = nullptr;
  size_t length = 0;
  size_t sourceOffset = 0;
};

template <typename Callback>
bool forEachPageTextLine(const Page& page, Callback&& callback) {
  using CallbackType = std::remove_reference_t<Callback>;
  return page.forEachTextLine(
      [](const PageTextLine& line, void* context) { return (*static_cast<CallbackType*>(context))(line); }, &callback);
}

bool isDashSeparator(const char* text, const size_t length, const size_t offset) {
  return offset + 2 < length && static_cast<uint8_t>(text[offset]) == 0xE2 &&
         static_cast<uint8_t>(text[offset + 1]) == 0x80 &&
         (static_cast<uint8_t>(text[offset + 2]) == 0x93 || static_cast<uint8_t>(text[offset + 2]) == 0x94);
}

bool containsDashSeparator(const char* text, const size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (isDashSeparator(text, length, i)) return true;
  }
  return false;
}

bool hasVisibleWordText(const char* text) {
  if (!text) return false;
  const char* cursor = text;
  if (static_cast<unsigned char>(cursor[0]) == 0xE2 && cursor[1] != '\0' && cursor[2] != '\0' &&
      static_cast<unsigned char>(cursor[1]) == 0x80 && static_cast<unsigned char>(cursor[2]) == 0x83) {
    cursor += 3;
  }
  while (*cursor) {
    if (*cursor != ' ' && *cursor != '\t' && *cursor != '\r' && *cursor != '\n') return true;
    ++cursor;
  }
  return false;
}

template <typename Sink>
void forEachWordPart(const char* text, const size_t length, Sink&& sink) {
  size_t partStart = 0;
  for (size_t i = 0; i < length;) {
    if (!isDashSeparator(text, length, i)) {
      i++;
      continue;
    }
    if (i > partStart) sink(WordPartRef{text + partStart, i - partStart, partStart});
    i += 3;
    partStart = i;
  }
  if (partStart < length) sink(WordPartRef{text + partStart, length - partStart, partStart});
}

bool addBudgetBytes(size_t& total, const size_t bytes) {
  if (bytes > std::numeric_limits<size_t>::max() - total) return false;
  total += bytes;
  return true;
}

const char* withoutSoftHyphens(const char* word, const size_t length, char* scratch, const size_t scratchCapacity) {
  if (!word || !scratch || scratchCapacity == 0) return word;
  bool hasSoftHyphen = false;
  for (size_t i = 0; i + 1 < length; ++i) {
    if (word[i] == SOFT_HYPHEN_UTF8[0] && word[i + 1] == SOFT_HYPHEN_UTF8[1]) {
      hasSoftHyphen = true;
      break;
    }
  }
  if (!hasSoftHyphen) return word;

  size_t out = 0;
  for (size_t i = 0; i < length && out + 1 < scratchCapacity;) {
    if (i + 1 < length && word[i] == SOFT_HYPHEN_UTF8[0] && word[i + 1] == SOFT_HYPHEN_UTF8[1]) {
      i += SOFT_HYPHEN_BYTES;
      continue;
    }
    scratch[out++] = word[i++];
  }
  scratch[out] = '\0';
  return scratch;
}

int16_t measureWordAdvanceX(const GfxRenderer& renderer, const int fontId, const char* word, const size_t length,
                            const EpdFontFamily::Style style, char* scratch, const size_t scratchCapacity) {
  const char* measured = withoutSoftHyphens(word, length, scratch, scratchCapacity);
  return static_cast<int16_t>(renderer.getTextAdvanceX(fontId, measured, style));
}

int16_t measureWordAdvanceX(const GfxRenderer& renderer, const int fontId, const char* word, const size_t length,
                            const EpdFontFamily::Style style, const uint8_t bionicBoundary,
                            const uint16_t bionicSuffixX, char* scratch, const size_t scratchCapacity) {
  if (bionicBoundary == 0 || bionicSuffixX == 0) {
    return measureWordAdvanceX(renderer, fontId, word, length, style, scratch, scratchCapacity);
  }
  const size_t suffixStart = std::min<size_t>(bionicBoundary, length);
  return static_cast<int16_t>(bionicSuffixX + renderer.getTextAdvanceX(fontId, word + suffixStart, style));
}

int16_t measureWordAdvanceX(const GfxRenderer& renderer, const int fontId, const char* word, const size_t length,
                            const EpdFontFamily::Style style, const uint8_t bionicBoundary,
                            const uint16_t bionicRunOffset, const bool wordIsRtl, char* scratch,
                            const size_t scratchCapacity) {
  if (!wordIsRtl || bionicBoundary == 0 || bionicRunOffset == 0) {
    return measureWordAdvanceX(renderer, fontId, word, length, style, bionicBoundary, bionicRunOffset, scratch,
                               scratchCapacity);
  }

  const auto boldStyle = static_cast<EpdFontFamily::Style>(style | EpdFontFamily::BOLD);
  char boldBuf[40];
  const size_t boldLen = std::min<size_t>({static_cast<size_t>(bionicBoundary), length, sizeof(boldBuf) - 1});
  memcpy(boldBuf, word, boldLen);
  boldBuf[boldLen] = '\0';
  return static_cast<int16_t>(bionicRunOffset + renderer.getTextAdvanceX(fontId, boldBuf, boldStyle));
}

bool isRtlWord(const char* word, const bool fallbackRtl) {
  return BidiUtils::detectParagraphLevel(word, fallbackRtl ? 1 : 0) == 1;
}

// Single-style prewarm/advance-table bitmask: bit 0 = REGULAR, 1 = BOLD,
// 2 = ITALIC, 3 = BOLD_ITALIC. The `& 0x03` is defensive — Style enum
// is two bits, but UNDERLINE etc. live in higher bits if ever OR'd in.
constexpr uint8_t styleToBitMask(EpdFontFamily::Style style) {
  return static_cast<uint8_t>(1u << (static_cast<uint8_t>(style) & 0x03));
}

}  // namespace

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  Dictionary::clearLookupDictPathOverride();
  mappedInput.setReaderTouchscreenOverride(true);
  ignoreInitialBackRelease_ = mappedInput.isPressed(MappedInputManager::Button::Back);
  const bool consumeInitialConfirm = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  if (!buildWorkingSet(consumeInitialConfirm)) {
    if (workingSetMemoryError_) {
      GUI.drawPopup(renderer, tr(STR_MEMORY_ERROR));
      renderer.displayBuffer();
      delay(1000);
    }
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  autoLookupInitialWord_ = false;
  requestUpdate();
}

bool DictionaryWordSelectActivity::buildWorkingSet(const bool consumeInitialConfirm) {
  workingSetMemoryError_ = false;
  if (!page) {
    LOG_ERR("DICT", "Cannot build word selection without a reader page");
    return false;
  }
  navigator.releaseWorkingSet();
  workingSet_.clear();
  if (!allocateWorkingSet()) {
    workingSetMemoryError_ = true;
    return false;
  }
  prebuildAdvanceTable();
  if (!extractWords() || !mergeHyphenatedWords()) {
    workingSetMemoryError_ = true;
    navigator.releaseWorkingSet();
    workingSet_.clear();
    return false;
  }
  navigator.loadView(workingSet_.words.get(), workingSet_.wordCount, workingSet_.rows.get(), workingSet_.rowCount,
                     workingSet_.textPool.get(), consumeInitialConfirm);
#if CROSSINK_APP_CAP_TOUCH
  navigator.setTouchDragCursorVisible(mappedInput.hasTouch());
  bool initialTouchHit = false;
  if (initialTouchX_ >= 0 && initialTouchY_ >= 0) {
    navigator.selectWordAtPoint(initialTouchX_, initialTouchY_, renderer.getLineHeight(SETTINGS.getReaderFontId()),
                                &initialTouchHit);
  }
  if (autoLookupInitialWord_) {
    const auto* selected = initialTouchHit ? navigator.getSelected() : nullptr;
    if (!selected) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
      return false;
    }
    touchDragLookup_ = navigator.beginTouchMultiSelect();
    touchDragStartX_ = initialTouchX_;
    touchDragStartY_ = initialTouchY_;
  }
#else
  navigator.setTouchDragCursorVisible(false);
#endif
  return true;
}

void DictionaryWordSelectActivity::suspendWorkingSet() {
  if (workingSetSuspended_ || !readerPageLoad_) return;
  if (const auto* selected = navigator.getSelected()) {
    suspendedSelectionX_ = selected->screenX + selected->width / 2;
    suspendedSelectionY_ = selected->screenY + renderer.getLineHeight(SETTINGS.getReaderFontId()) / 2;
  }
  navigator.releaseWorkingSet();
  workingSet_.clear();
  page.reset();
  workingSetSuspended_ = true;
  MemoryBudget::logHeapShape("dict.parent_suspended");
}

void DictionaryWordSelectActivity::onExit() {
  controller.onExit();
  navigator.releaseWorkingSet();
  workingSet_.clear();
  Dictionary::clearLookupDictPathOverride();
  mappedInput.setReaderTouchscreenOverride(false);
  const auto& sdFonts = renderer.getSdCardFonts();
  auto it = sdFonts.find(SETTINGS.getReaderFontId());
  if (it != sdFonts.end()) it->second->clearPersistentCache();
  Activity::onExit();
}

void DictionaryWordSelectActivity::prewarmHighlightGlyphs(int currIdx) {
  const auto* w = navigator.getWordAt(currIdx);
  if (!w) return;
  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) return;
  uint8_t styleMask = styleToBitMask(w->style);
  if (w->bionicBoundary > 0) {
    styleMask |= styleToBitMask(static_cast<EpdFontFamily::Style>(w->style | EpdFontFamily::BOLD));
  }
  fcm->prewarmCache(SETTINGS.getReaderFontId(), navigator.getDisplay(*w), styleMask);
}

void DictionaryWordSelectActivity::prebuildAdvanceTable() {
  if (!renderer.isSdCardFont(SETTINGS.getReaderFontId())) return;

  // A page can contain hundreds of distinct glyphs, so this 2 KB collector is
  // too large for the render-task stack. Allocate it fallibly and release it
  // before extraction; a failure only loses the SD-font speed-up.
  auto codepoints = makeUniqueNoThrow<uint32_t[]>(ADVANCE_CODEPOINT_CAPACITY);
  if (!codepoints) {
    const auto heap = MemoryBudget::snapshot();
    LOG_ERR("DICT", "OOM allocating advance collector (%u bytes, free=%u maxAlloc=%u)",
            static_cast<unsigned>(ADVANCE_CODEPOINT_CAPACITY * sizeof(uint32_t)), heap.freeHeap, heap.maxAllocHeap);
    return;
  }
  const uint32_t* const codepointData = codepoints.get();

  uint16_t codepointCount = 0;
  uint8_t pageStyleMask = 0;
  bool truncated = false;
  forEachPageTextLine(*page, [&](const PageTextLine& line) {
    const auto* block = line.block;
    if (!block) return true;
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      const auto* cursor = reinterpret_cast<const unsigned char*>(block->wordText(i));
      uint32_t codepoint = 0;
      while ((codepoint = utf8NextCodepoint(&cursor))) {
        if (std::find(codepointData, codepointData + codepointCount, codepoint) != codepointData + codepointCount) {
          continue;
        }
        if (codepointCount >= ADVANCE_CODEPOINT_CAPACITY) {
          truncated = true;
          break;
        }
        codepoints[codepointCount++] = codepoint;
      }
      pageStyleMask |= styleToBitMask(block->wordStyle(i));
    }
    return true;
  });
  if (pageStyleMask == 0) pageStyleMask = styleToBitMask(EpdFontFamily::REGULAR);
  if (truncated) {
    LOG_ERR("DICT", "SD-font advance collector cap hit (%u); remaining glyphs will load on demand",
            static_cast<unsigned>(ADVANCE_CODEPOINT_CAPACITY));
  }
  if (codepointCount > 0) {
    // Use the codepoint overload so prewarm itself has no growing std::string.
    // RTL presentation forms that are absent here still use the normal
    // fallible on-demand font path during measurement.
    renderer.ensureSdCardFontReady(SETTINGS.getReaderFontId(), codepointData, codepointCount,
                                   /*includeSpace=*/true, /*includeHyphen=*/true, pageStyleMask);
  }
}

void DictionaryWordSelectActivity::clearFrontButtonHintArea() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int hintSize = metrics.buttonHintsHeight;
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  switch (renderer.getOrientation()) {
    case GfxRenderer::Orientation::Portrait:
      renderer.fillRect(0, screenHeight - hintSize, screenWidth, hintSize, false);
      break;
    case GfxRenderer::Orientation::LandscapeClockwise:
      renderer.fillRect(0, 0, hintSize, screenHeight, false);
      break;
    case GfxRenderer::Orientation::PortraitInverted:
      renderer.fillRect(0, 0, screenWidth, hintSize, false);
      break;
    case GfxRenderer::Orientation::LandscapeCounterClockwise:
      renderer.fillRect(screenWidth - hintSize, 0, hintSize, screenHeight, false);
      break;
  }
}

void DictionaryWordSelectActivity::renderDefinitionBackground() {
  std::unique_ptr<Page> reloadedPage;
  const Page* backgroundPage = page.get();
  if (!backgroundPage && readerPageLoad_) {
    reloadedPage = readerPageLoad_(readerContext_, activePageOffset_);
    backgroundPage = reloadedPage.get();
  }
  if (!backgroundPage) {
    LOG_ERR("DICT", "Cannot redraw dictionary background without a reader page");
    return;
  }
  const bool foregroundBlack = ReaderUtils::readerForegroundBlack();
  renderer.clearScreen(ReaderUtils::readerBackgroundColor());

  // Dictionary layout can evict the reader font's bitmap glyph cache. Rebuild
  // it before redrawing the page behind the modal; the persistent advance
  // table only preserves glyph widths, not the bitmaps themselves.
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  backgroundPage->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop, foregroundBlack);  // scan pass
  scope.endScanAndPrewarm();
  backgroundPage->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop, foregroundBlack);
}

void DictionaryWordSelectActivity::renderDefinitionBackgroundCallback(void* context) {
  static_cast<DictionaryWordSelectActivity*>(context)->renderDefinitionBackground();
}

bool DictionaryWordSelectActivity::allocateWorkingSet() {
  WorkingSetBudget budget;
  bool valid = true;
  bool haveRow = false;
  int16_t currentRowY = 0;
  WordPartRef previousRowLast{};
  bool previousRowLastWasTableText = false;

  const auto addMergedBudget = [&](const WordPartRef& first, const WordPartRef& second, const bool stripLeadingSecond) {
    if (first.length == 0 || first.text[first.length - 1] != '-' || first.text[0] == '-') return;
    const size_t secondSkip = stripLeadingSecond && second.length > 0 && second.text[0] == '-' ? 1 : 0;
    const size_t mergedLength = first.length - 1 + second.length - secondSkip;
    if (mergedLength > UINT16_MAX || !addBudgetBytes(budget.textBytes, mergedLength + 1)) valid = false;
  };

  forEachPageTextLine(*page, [&](const PageTextLine& line) {
    const auto* block = line.block;
    if (!block) return true;
    const int16_t screenY = static_cast<int16_t>(
        line.yPos + marginTop + block->getRubyShift(renderer.getFontAscenderSize(SETTINGS.getReaderFontId())));
    for (uint16_t wordIndex = 0; wordIndex < block->wordCount(); ++wordIndex) {
      const char* word = block->wordText(wordIndex);
      const size_t wordLength = block->wordTextLen(wordIndex);
      budget.maxSourceWordBytes = std::max(budget.maxSourceWordBytes, wordLength + 1);
      if (wordLength > UINT16_MAX || !utf8ContainsLookupCharacter(word)) continue;

      forEachWordPart(word, wordLength, [&](const WordPartRef& part) {
        if (!valid || part.length == 0 || part.length > UINT16_MAX || budget.wordCount >= UINT16_MAX) {
          valid = false;
          return;
        }
        budget.wordCount++;
        if (!addBudgetBytes(budget.textBytes, part.length + 1)) {
          valid = false;
          return;
        }

        if (!haveRow) {
          haveRow = true;
          currentRowY = screenY;
        } else if (std::abs(static_cast<int>(screenY) - static_cast<int>(currentRowY)) > 2) {
          budget.rowCount++;
          if (!previousRowLastWasTableText && !line.isTableText) {
            addMergedBudget(previousRowLast, part, /*stripLeadingSecond=*/true);
          }
          currentRowY = screenY;
        }
        previousRowLast = part;
        previousRowLastWasTableText = line.isTableText;
      });
    }
    return valid;
  });
  if (haveRow) budget.rowCount++;
  if (budget.rowCount > static_cast<size_t>(INT16_MAX)) valid = false;
  if (haveRow && !previousRowLastWasTableText && !nextPageFirstWord.empty()) {
    const WordPartRef next{nextPageFirstWord.c_str(), nextPageFirstWord.size(), 0};
    addMergedBudget(previousRowLast, next, /*stripLeadingSecond=*/false);
  }

  constexpr size_t MAX_TEXT_POOL_BYTES = static_cast<size_t>(UINT16_MAX) + 1U;
  if (!valid || budget.textBytes > MAX_TEXT_POOL_BYTES ||
      budget.maxSourceWordBytes > std::numeric_limits<size_t>::max() / 2U) {
    LOG_ERR("DICT", "Word selection page exceeds bounded working-set limits");
    return false;
  }

  const size_t wordBytes = budget.wordCount * sizeof(WordSelectNavigator::WordInfo);
  const size_t rowBytes = budget.rowCount * sizeof(WordSelectNavigator::Row);
  const size_t scratchBytes = budget.maxSourceWordBytes * 2U;
  size_t totalBytes = 0;
  valid = addBudgetBytes(totalBytes, wordBytes) && addBudgetBytes(totalBytes, rowBytes) &&
          addBudgetBytes(totalBytes, budget.textBytes) && addBudgetBytes(totalBytes, scratchBytes);
  if (!valid) {
    LOG_ERR("DICT", "Word selection working-set size overflow");
    return false;
  }

  const size_t largestBlock = std::max({wordBytes, rowBytes, budget.textBytes, scratchBytes});
  auto heap = MemoryBudget::snapshot();
  if (heap.maxAllocHeap < largestBlock ||
      heap.freeHeap <
          totalBytes + std::min(WORD_SELECT_ALLOCATION_HEADROOM, std::numeric_limits<size_t>::max() - totalBytes)) {
    const auto before = heap;
    if (renderer.releaseSdCardFontForLowMemory(SETTINGS.getReaderFontId())) {
      heap = MemoryBudget::snapshot();
      LOG_DBG("DICT", "Released reader SD-font caches for %u-byte working set: free=%u->%u maxAlloc=%u->%u",
              static_cast<unsigned>(totalBytes), before.freeHeap, heap.freeHeap, before.maxAllocHeap,
              heap.maxAllocHeap);
    }
  }

  workingSet_.wordCapacity = budget.wordCount;
  workingSet_.rowCapacity = budget.rowCount;
  workingSet_.textCapacity = budget.textBytes;
  workingSet_.measurementScratchCapacity = scratchBytes;

  if (budget.wordCount > 0) {
    workingSet_.words = makeUniqueNoThrow<WordSelectNavigator::WordInfo[]>(budget.wordCount);
    if (!workingSet_.words) {
      heap = MemoryBudget::snapshot();
      LOG_ERR("DICT", "OOM allocating word metadata (%u bytes, free=%u maxAlloc=%u)", static_cast<unsigned>(wordBytes),
              heap.freeHeap, heap.maxAllocHeap);
      workingSet_.clear();
      return false;
    }
  }
  if (budget.textBytes > 0) {
    workingSet_.textPool = makeUniqueNoThrow<char[]>(budget.textBytes);
    if (!workingSet_.textPool) {
      heap = MemoryBudget::snapshot();
      LOG_ERR("DICT", "OOM allocating word text arena (%u bytes, free=%u maxAlloc=%u)",
              static_cast<unsigned>(budget.textBytes), heap.freeHeap, heap.maxAllocHeap);
      workingSet_.clear();
      return false;
    }
  }
  if (budget.rowCount > 0) {
    workingSet_.rows = makeUniqueNoThrow<WordSelectNavigator::Row[]>(budget.rowCount);
    if (!workingSet_.rows) {
      heap = MemoryBudget::snapshot();
      LOG_ERR("DICT", "OOM allocating row metadata (%u bytes, free=%u maxAlloc=%u)", static_cast<unsigned>(rowBytes),
              heap.freeHeap, heap.maxAllocHeap);
      workingSet_.clear();
      return false;
    }
  }
  if (scratchBytes > 0) {
    workingSet_.measurementScratch = makeUniqueNoThrow<char[]>(scratchBytes);
    if (!workingSet_.measurementScratch) {
      heap = MemoryBudget::snapshot();
      LOG_ERR("DICT", "OOM allocating word measurement scratch (%u bytes, free=%u maxAlloc=%u)",
              static_cast<unsigned>(scratchBytes), heap.freeHeap, heap.maxAllocHeap);
      workingSet_.clear();
      return false;
    }
  }
  return true;
}

bool DictionaryWordSelectActivity::appendText(const char* text, const size_t length, uint16_t& offset) {
  if (!text || length > UINT16_MAX || workingSet_.textUsed > UINT16_MAX ||
      length + 1 > workingSet_.textCapacity - workingSet_.textUsed) {
    LOG_ERR("DICT", "Word text arena overflow (used=%u capacity=%u append=%u)",
            static_cast<unsigned>(workingSet_.textUsed), static_cast<unsigned>(workingSet_.textCapacity),
            static_cast<unsigned>(length + 1));
    return false;
  }
  offset = static_cast<uint16_t>(workingSet_.textUsed);
  char* const textPool = workingSet_.textPool.get();
  memcpy(textPool + workingSet_.textUsed, text, length);
  workingSet_.textPool[workingSet_.textUsed + length] = '\0';
  workingSet_.textUsed += length + 1;
  return true;
}

bool DictionaryWordSelectActivity::appendMergedText(const char* first, const size_t firstLength, const char* second,
                                                    const size_t secondLength, uint16_t& offset) {
  const size_t mergedLength = firstLength + secondLength;
  if (!first || !second || mergedLength > UINT16_MAX || workingSet_.textUsed > UINT16_MAX ||
      mergedLength + 1 > workingSet_.textCapacity - workingSet_.textUsed) {
    LOG_ERR("DICT", "Merged word text exceeds arena (used=%u capacity=%u append=%u)",
            static_cast<unsigned>(workingSet_.textUsed), static_cast<unsigned>(workingSet_.textCapacity),
            static_cast<unsigned>(mergedLength + 1));
    return false;
  }
  offset = static_cast<uint16_t>(workingSet_.textUsed);
  char* const textPool = workingSet_.textPool.get();
  char* destination = textPool + workingSet_.textUsed;
  memcpy(destination, first, firstLength);
  memcpy(destination + firstLength, second, secondLength);
  destination[mergedLength] = '\0';
  workingSet_.textUsed += mergedLength + 1;
  return true;
}

bool DictionaryWordSelectActivity::appendWord(WordSelectNavigator::WordInfo word) {
  if (workingSet_.wordCount >= workingSet_.wordCapacity) {
    LOG_ERR("DICT", "Word metadata capacity exceeded");
    return false;
  }
  if (workingSet_.rowCount == 0 || std::abs(static_cast<int>(word.screenY) -
                                            static_cast<int>(workingSet_.rows[workingSet_.rowCount - 1].yPos)) > 2) {
    if (workingSet_.rowCount >= workingSet_.rowCapacity || workingSet_.wordCount > UINT16_MAX) {
      LOG_ERR("DICT", "Row metadata capacity exceeded");
      return false;
    }
    workingSet_.rows[workingSet_.rowCount] = {word.screenY, static_cast<uint16_t>(workingSet_.wordCount), 0};
    workingSet_.rowCount++;
  }
  auto& row = workingSet_.rows[workingSet_.rowCount - 1];
  if (row.wordCount == UINT16_MAX) {
    LOG_ERR("DICT", "Row word count exceeded");
    return false;
  }
  word.row = static_cast<int16_t>(workingSet_.rowCount - 1);
  workingSet_.words[workingSet_.wordCount++] = word;
  row.wordCount++;
  return true;
}

bool DictionaryWordSelectActivity::extractWords() {
  const size_t scratchHalf = workingSet_.measurementScratchCapacity / 2U;
  char* prefixScratch = workingSet_.measurementScratch.get();
  char* sanitizeScratch = prefixScratch ? prefixScratch + scratchHalf : nullptr;
  const char* const textPool = workingSet_.textPool.get();

  // Fallback used by blocks where we can't derive a per-line gap
  // (single-word blocks, degenerate first-word measurements).
  const int16_t naturalSpaceWidth =
      static_cast<int16_t>(renderer.getTextAdvanceX(SETTINGS.getReaderFontId(), " ", EpdFontFamily::REGULAR));

  uint16_t pageWordOrdinal = 0;
  return forEachPageTextLine(*page, [&](const PageTextLine& line) {
    const auto* block = line.block;
    if (!block) return true;

    const uint16_t sourceWordCount = block->wordCount();
    const int rubyShift = block->getRubyShift(renderer.getFontAscenderSize(SETTINGS.getReaderFontId()));
    int16_t lineGapWidth = naturalSpaceWidth;
    if (sourceWordCount >= 2 && block->wordTextLen(0) > 0) {
      const char* firstWord = block->wordText(0);
      const size_t firstLength = block->wordTextLen(0);
      const auto firstStyle = block->wordStyle(0);
      const uint8_t firstBionicBoundary = block->bionicBoundary(0);
      const uint16_t firstBionicSuffixX = block->bionicRunOffset(0);
      const bool firstWordIsRtl = isRtlWord(firstWord, block->getBlockStyle().isRtl);
      const int16_t firstWidth =
          measureWordAdvanceX(renderer, SETTINGS.getReaderFontId(), firstWord, firstLength, firstStyle,
                              firstBionicBoundary, firstBionicSuffixX, firstWordIsRtl, sanitizeScratch, scratchHalf);
      const int16_t derivedGap = static_cast<int16_t>(block->wordXpos(1) - block->wordXpos(0) - firstWidth);
      if (derivedGap > naturalSpaceWidth / 2) lineGapWidth = derivedGap;
    }

    int lastSelectableWordIndex = -2;
    for (uint16_t wordIndex = 0; wordIndex < sourceWordCount; ++wordIndex) {
      const char* wordText = block->wordText(wordIndex);
      const size_t wordLength = block->wordTextLen(wordIndex);
      const auto wordStyle = block->wordStyle(wordIndex);
      const uint8_t bionicBoundary = block->bionicBoundary(wordIndex);
      const uint16_t bionicSuffixX = block->bionicRunOffset(wordIndex);
      const bool wordIsRtl = isRtlWord(wordText, block->getBlockStyle().isRtl);

      if (!hasVisibleWordText(wordText)) {
        lastSelectableWordIndex = -2;
        continue;
      }
      const PageWordGeometry sourceGeometry =
          pageWordGeometry(renderer, SETTINGS.getReaderFontId(), line, *block, wordIndex);
      if (sourceGeometry.width <= 0) {
        lastSelectableWordIndex = -2;
        continue;
      }
      const uint16_t sourcePageWordOrdinal = pageWordOrdinal++;
      const int16_t screenX = static_cast<int16_t>(line.xPos + sourceGeometry.xOffset + marginLeft);
      const int16_t screenY = static_cast<int16_t>(line.yPos + marginTop + rubyShift);

      if (!utf8ContainsLookupCharacter(wordText)) {
        lastSelectableWordIndex = -2;
        continue;
      }

      if (!containsDashSeparator(wordText, wordLength)) {
        int16_t wordWidth;
        if (bionicBoundary > 0 && bionicSuffixX > 0) {
          wordWidth = measureWordAdvanceX(renderer, SETTINGS.getReaderFontId(), wordText, wordLength, wordStyle,
                                          bionicBoundary, bionicSuffixX, wordIsRtl, sanitizeScratch, scratchHalf);
        } else if (wordIndex + 1 < sourceWordCount) {
          const int16_t raw = static_cast<int16_t>(block->wordXpos(wordIndex + 1) - block->wordXpos(wordIndex));
          wordWidth = std::max(static_cast<int16_t>(1), static_cast<int16_t>(raw - lineGapWidth));
        } else {
          wordWidth = measureWordAdvanceX(renderer, SETTINGS.getReaderFontId(), wordText, wordLength, wordStyle,
                                          bionicBoundary, bionicSuffixX, sanitizeScratch, scratchHalf);
        }
        wordWidth = static_cast<int16_t>(std::min<int>(wordWidth, sourceGeometry.width));
        if (wordWidth <= 0) {
          lastSelectableWordIndex = -2;
          continue;
        }

        bool joinWithoutSpaceBefore = false;
        if (lastSelectableWordIndex == static_cast<int>(wordIndex) - 1 && workingSet_.wordCount > 0) {
          const uint16_t previousIndex = wordIndex - 1;
          const auto previousStyle = block->wordStyle(previousIndex);
          const int16_t previousMeasuredWidth = static_cast<int16_t>(
              renderer.getTextAdvanceX(SETTINGS.getReaderFontId(), block->wordText(previousIndex), previousStyle));
          const int16_t currentMeasuredWidth =
              measureWordAdvanceX(renderer, SETTINGS.getReaderFontId(), wordText, wordLength, wordStyle, bionicBoundary,
                                  bionicSuffixX, wordIsRtl, sanitizeScratch, scratchHalf);
          const int currentLeft = screenX;
          const int currentRight = screenX + currentMeasuredWidth;
          auto& previousWord = workingSet_.words[workingSet_.wordCount - 1];
          const int previousLeft = previousWord.screenX;
          const int previousRight = previousLeft + previousMeasuredWidth;
          const int gap = currentLeft >= previousLeft ? currentLeft - previousRight : previousLeft - currentRight;
          joinWithoutSpaceBefore = gap < naturalSpaceWidth / 2;
          if (joinWithoutSpaceBefore) {
            previousWord.width = previousMeasuredWidth;
            wordWidth = currentMeasuredWidth;
          }
        }

        uint16_t offset = 0;
        if (!appendText(wordText, wordLength, offset)) return false;
        WordSelectNavigator::WordInfo word;
        word.textOffset = offset;
        word.textLen = static_cast<uint16_t>(wordLength);
        word.lookupOffset = offset;
        word.lookupLen = word.textLen;
        word.pageWordOrdinal = sourcePageWordOrdinal;
        word.sourceWordByteOffset = 0;
        word.screenX = screenX;
        word.screenY = screenY;
        word.width = wordWidth;
        word.style = wordStyle;
        word.fontId = SETTINGS.getReaderFontId();
        word.isRtl = wordIsRtl;
        word.joinWithoutSpaceBefore = joinWithoutSpaceBefore;
        word.isTableText = line.isTableText;
        word.bionicBoundary = bionicBoundary;
        word.bionicSuffixX = bionicSuffixX;
        if (!appendWord(word)) return false;
        lastSelectableWordIndex = wordIndex;
        continue;
      }

      bool partSucceeded = true;
      forEachWordPart(wordText, wordLength, [&](const WordPartRef& part) {
        if (!partSucceeded || part.length == 0) return;
        int16_t offsetX = 0;
        if (part.sourceOffset > 0) {
          if (!prefixScratch || part.sourceOffset + 1 > scratchHalf) {
            partSucceeded = false;
            return;
          }
          memcpy(prefixScratch, wordText, part.sourceOffset);
          prefixScratch[part.sourceOffset] = '\0';
          offsetX = measureWordAdvanceX(renderer, SETTINGS.getReaderFontId(), prefixScratch, part.sourceOffset,
                                        wordStyle, sanitizeScratch, scratchHalf);
        }

        uint16_t offset = 0;
        if (!appendText(part.text, part.length, offset)) {
          partSucceeded = false;
          return;
        }
        const char* storedPart = textPool + offset;
        const int16_t measuredPartWidth = measureWordAdvanceX(renderer, SETTINGS.getReaderFontId(), storedPart,
                                                              part.length, wordStyle, sanitizeScratch, scratchHalf);
        const PageWordGeometry partGeometry =
            clipPageTextRange(line, block->wordXpos(wordIndex) + offsetX, measuredPartWidth);
        if (partGeometry.width <= 0) return;
        WordSelectNavigator::WordInfo word;
        word.textOffset = offset;
        word.textLen = static_cast<uint16_t>(part.length);
        word.lookupOffset = offset;
        word.lookupLen = word.textLen;
        word.pageWordOrdinal = sourcePageWordOrdinal;
        word.sourceWordByteOffset = static_cast<uint16_t>(part.sourceOffset);
        word.screenX = static_cast<int16_t>(line.xPos + marginLeft + partGeometry.xOffset);
        word.screenY = screenY;
        word.width = static_cast<int16_t>(partGeometry.width);
        word.style = wordStyle;
        word.fontId = SETTINGS.getReaderFontId();
        word.isRtl = wordIsRtl;
        word.isTableText = line.isTableText;
        if (!appendWord(word)) partSucceeded = false;
      });
      if (!partSucceeded) return false;
      lastSelectableWordIndex = -2;
    }
    return true;
  });
}

bool DictionaryWordSelectActivity::captureCurrentClippingRequest(DictionaryClippingRequest& request) const {
  int firstIdx = -1;
  int lastIdx = -1;
  if (!navigator.getLookupSelectionRange(firstIdx, lastIdx)) {
    LOG_ERR("CLIP", "Dictionary clipping has no selected words");
    return false;
  }
  const auto* first = navigator.getWordAt(firstIdx);
  const auto* last = navigator.getWordAt(lastIdx);
  if (!first || !last) {
    LOG_ERR("CLIP", "Dictionary clipping selection indexes are invalid");
    return false;
  }
  if (first->continuationOf >= 0) first = navigator.getWordAt(first->continuationOf);
  if (last->continuationIndex >= 0) last = navigator.getWordAt(last->continuationIndex);
  if (!first || !last) {
    LOG_ERR("CLIP", "Dictionary clipping hyphenated selection is invalid");
    return false;
  }
  const bool firstPrecedesLast =
      first->pageWordOrdinal < last->pageWordOrdinal ||
      (first->pageWordOrdinal == last->pageWordOrdinal && first->sourceWordByteOffset <= last->sourceWordByteOffset);
  const auto* rangeFirst = firstPrecedesLast ? first : last;
  const auto* rangeLast = firstPrecedesLast ? last : first;
  request.firstPageOffset = static_cast<uint8_t>(activePageOffset_);
  request.firstPageWordOrdinal = rangeFirst->pageWordOrdinal;
  request.lastPageOffset = static_cast<uint8_t>(activePageOffset_);
  request.lastPageWordOrdinal = rangeLast->pageWordOrdinal;
  request.firstWordByteOffset = rangeFirst->sourceWordByteOffset;
  request.lastWordByteEndOffset = static_cast<uint16_t>(rangeLast->sourceWordByteOffset + rangeLast->textLen);
  return true;
}

bool DictionaryWordSelectActivity::captureClippingRequest() {
  DictionaryClippingRequest currentRequest;
  if (!captureCurrentClippingRequest(currentRequest)) return false;

  pendingClippingRequest_ = hasCrossPageSelection_ ? crossPageFirstRequest_ : currentRequest;
  if (hasCrossPageSelection_) {
    pendingClippingRequest_.lastPageOffset = currentRequest.lastPageOffset;
    pendingClippingRequest_.lastPageWordOrdinal = currentRequest.lastPageWordOrdinal;
    pendingClippingRequest_.lastWordByteEndOffset = currentRequest.lastWordByteEndOffset;
  }
  hasPendingClippingRequest_ = true;
  navigator.clearCompletedSelection();
  return true;
}

bool DictionaryWordSelectActivity::selectFirstWordForTouchDrag() {
  const auto* first = navigator.getWordAt(0);
  if (!first) return false;
  bool hit = false;
  navigator.selectWordAtPoint(first->screenX + first->width / 2,
                              first->screenY + renderer.getLineHeight(SETTINGS.getReaderFontId()) / 2,
                              renderer.getLineHeight(SETTINGS.getReaderFontId()), &hit);
  return hit && navigator.beginTouchMultiSelect();
}

void DictionaryWordSelectActivity::finishTouchLookupOnCurrentPage(const std::string& phrase, const size_t wordCount,
                                                                  const DictionaryClippingRequest& request) {
  pendingClippingRequest_ = request;
  hasPendingClippingRequest_ = true;
  touchDragLookup_ = false;
  controller.lookupOrPopup(phrase, wordCount);
}

bool DictionaryWordSelectActivity::continueTouchSelectionOnNextPage() {
  // Clipping preloads three pages, but dictionary lookup intentionally owns
  // just one page working set. Retain the first endpoint and switch to page
  // one only after a real drag reaches page zero's final word.
  if (activePageOffset_ != 0 || !hasNextReaderPage_ || !readerPageLoad_) return false;

  int firstIdx = -1;
  int lastIdx = -1;
  DictionaryClippingRequest currentRequest;
  if (!navigator.getLookupSelectionRange(firstIdx, lastIdx) || !captureCurrentClippingRequest(currentRequest)) {
    return false;
  }
  std::string prefix = navigator.buildPhrase(firstIdx, lastIdx);
  if (prefix.empty()) return false;

  const auto* finalSelectedWord = navigator.getWordAt(lastIdx);
  const std::string mergedFirstWord = nextPageFirstWord;
  const bool firstWordWasMerged =
      finalSelectedWord && utf8EndsWithHyphen(navigator.getDisplay(*finalSelectedWord), finalSelectedWord->textLen) &&
      !mergedFirstWord.empty();
  const size_t prefixWordCount = navigator.getLookupSelectionWordCount();

  bool nextPageLoaded = false;
  bool nextPagePrepared = false;
  {
    // A highlight update for the last word may still be rendering on the other
    // core. Keep that render from reading the page or navigator while their
    // backing storage is replaced for the next page.
    RenderLock lock(*this);
    auto nextPage = readerPageLoad_(readerContext_, 1);
    if (nextPage) {
      nextPageLoaded = true;
      navigator.releaseWorkingSet();
      workingSet_.clear();
      page.reset();
      page = std::move(nextPage);
      activePageOffset_ = 1;
      nextPageFirstWord.clear();
      nextPagePrepared = buildWorkingSet(/*consumeInitialConfirm=*/false) && selectFirstWordForTouchDrag();
      if (!nextPagePrepared) {
        // The fallback lookup is for the saved page-zero selection. Leave no
        // page-one state behind for its definition background to render.
        navigator.releaseWorkingSet();
        workingSet_.clear();
        page.reset();
        activePageOffset_ = 0;
      }
    }
  }
  if (!nextPageLoaded) {
    LOG_ERR("DICT", "Failed to load the next reader page for touch selection");
    // Preserve the valid page-zero selection if the next page cannot be read.
    finishTouchLookupOnCurrentPage(prefix, prefixWordCount, currentRequest);
    return true;
  }
  if (!nextPagePrepared) {
    LOG_ERR("DICT", "Failed to prepare next reader page for touch selection");
    // We no longer have the page-zero working set, but its request and lookup
    // text were captured before the switch. Finish that valid selection.
    finishTouchLookupOnCurrentPage(prefix, prefixWordCount, currentRequest);
    return true;
  }

  hasCrossPageSelection_ = true;
  crossPageFirstRequest_ = currentRequest;
  crossPageLookupPrefix_ = std::move(prefix);
  crossPageLookupWordCount_ = prefixWordCount;
  crossPageMergedFirstWord_ = std::move(mergedFirstWord);
  crossPageFirstWordWasMerged_ = firstWordWasMerged;
  requestUpdate();
  return true;
}

std::string DictionaryWordSelectActivity::finishTouchLookupPhrase() {
  std::string phrase = navigator.finishTouchMultiSelect();
  if (!hasCrossPageSelection_) return phrase;

  // Page zero may have already joined a final hyphenated word with page
  // one's first word for ordinary dictionary lookup. That first word is now
  // represented by the next-page selection too, so consume the overlap.
  if (crossPageFirstWordWasMerged_ && !crossPageMergedFirstWord_.empty() &&
      phrase.rfind(crossPageMergedFirstWord_, 0) == 0) {
    phrase.erase(0, crossPageMergedFirstWord_.size());
    if (!phrase.empty() && phrase.front() == ' ') phrase.erase(0, 1);
  }
  if (phrase.empty()) return crossPageLookupPrefix_;
  if (!crossPageLookupPrefix_.empty()) crossPageLookupPrefix_ += ' ';
  crossPageLookupPrefix_ += phrase;
  return crossPageLookupPrefix_;
}

void DictionaryWordSelectActivity::resetCrossPageSelection() {
  hasCrossPageSelection_ = false;
  crossPageFirstRequest_ = {};
  crossPageLookupPrefix_.clear();
  crossPageLookupWordCount_ = 0;
  crossPageMergedFirstWord_.clear();
  crossPageFirstWordWasMerged_ = false;
}

void DictionaryWordSelectActivity::finishWithClippingRequest() {
  if (!hasPendingClippingRequest_) {
    LOG_ERR("CLIP", "Dictionary clipping requested without a selected range");
    DictUtils::cancelAndFinish(*this);
    return;
  }
  setResult(ActivityResult{pendingClippingRequest_});
  finish();
}

bool DictionaryWordSelectActivity::mergeHyphenatedWords() {
  const char* const textPool = workingSet_.textPool.get();
  for (size_t rowIndex = 0; rowIndex + 1 < workingSet_.rowCount; ++rowIndex) {
    const auto& row = workingSet_.rows[rowIndex];
    const auto& nextRow = workingSet_.rows[rowIndex + 1];
    if (row.wordCount == 0 || nextRow.wordCount == 0) continue;
    const size_t lastWordIndex = row.firstWord + row.wordCount - 1;
    const size_t nextWordIndex = nextRow.firstWord;
    auto& last = workingSet_.words[lastWordIndex];
    auto& next = workingSet_.words[nextWordIndex];
    if (last.isTableText || next.isTableText) continue;
    const char* lastText = textPool + last.textOffset;
    if (!utf8EndsWithHyphen(lastText, last.textLen) || lastText[0] == '-') continue;

    const char* nextText = textPool + next.textOffset;
    const size_t nextSkip = next.textLen > 0 && nextText[0] == '-' ? 1 : 0;
    uint16_t mergedOffset = 0;
    if (!appendMergedText(lastText, last.textLen - 1, nextText + nextSkip, next.textLen - nextSkip, mergedOffset)) {
      return false;
    }
    const size_t mergedLength = last.textLen - 1 + next.textLen - nextSkip;
    last.continuationIndex = static_cast<int>(nextWordIndex);
    next.continuationOf = static_cast<int>(lastWordIndex);
    last.lookupOffset = mergedOffset;
    last.lookupLen = static_cast<uint16_t>(mergedLength);
    next.lookupOffset = mergedOffset;
    next.lookupLen = static_cast<uint16_t>(mergedLength);
  }

  if (!nextPageFirstWord.empty() && workingSet_.rowCount > 0) {
    const auto& lastRow = workingSet_.rows[workingSet_.rowCount - 1];
    if (lastRow.wordCount > 0) {
      auto& last = workingSet_.words[lastRow.firstWord + lastRow.wordCount - 1];
      const char* lastText = textPool + last.textOffset;
      if (!last.isTableText && utf8EndsWithHyphen(lastText, last.textLen) && lastText[0] != '-') {
        uint16_t mergedOffset = 0;
        if (!appendMergedText(lastText, last.textLen - 1, nextPageFirstWord.c_str(), nextPageFirstWord.size(),
                              mergedOffset)) {
          return false;
        }
        last.lookupOffset = mergedOffset;
        last.lookupLen = static_cast<uint16_t>(last.textLen - 1 + nextPageFirstWord.size());
      }
    }
  }
  return true;
}

void DictionaryWordSelectActivity::openDictionarySwitch() {
  auto picker = makeUniqueNoThrow<DictionarySelectActivity>(renderer, mappedInput, cachePath, true, true);
  if (!picker) {
    LOG_ERR("DICT", "OOM: DictionarySelectActivity");
    return;
  }

  startActivityForResult(std::move(picker), [this](const ActivityResult& result) {
    forceFullRepaintOnNextRender();
    if (result.isCancelled) {
      requestUpdate();
      return;
    }
    const auto* selection = std::get_if<FilePathResult>(&result.data);
    if (!selection) {
      LOG_ERR("DICT", "Dictionary switch returned no path");
      requestUpdate();
      return;
    }
    Dictionary::setLookupDictPathOverride(selection->path.c_str());
    controller.startLookup(controller.getLookupWord(), false);
  });
}

void DictionaryWordSelectActivity::loop() {
  if (ignoreInitialBackRelease_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        !mappedInput.isPressed(MappedInputManager::Button::Back)) {
      ignoreInitialBackRelease_ = false;
    }
    return;
  }

  if (controller.isActive()) {
    switch (controller.handleInput()) {
      case DictionaryLookupController::LookupEvent::FoundDefinition: {
        // Rebuild the reader page while its font is still resident. The child
        // then overlays the modal after swapping to the dictionary font, so we
        // never need a second framebuffer or two live SD-font families.
        {
          RenderLock lock(*this);
          renderDefinitionBackground();
        }
        auto definition = makeUniqueNoThrow<DictionaryDefinitionActivity>(
            renderer, mappedInput, controller.getFoundWord(), controller.getFoundLocation(), true, cachePath,
            controller.getRecordHistory(), controller.getLookupWord(),
            DictionaryLookupController::toHistStatus(controller.getFoundStatus()), this,
            &DictionaryWordSelectActivity::renderDefinitionBackgroundCallback, dictionaryFontFamilyName_,
            dictionaryFontPointSize_, true, hasPendingClippingRequest_ ? &pendingClippingRequest_ : nullptr,
            &highlightSnapshotStorage_);
        if (!definition) {
          LOG_ERR("DICT", "OOM allocating DictionaryDefinitionActivity (%u bytes)",
                  static_cast<unsigned>(sizeof(DictionaryDefinitionActivity)));
          forceFullRepaintOnNextRender();
          requestUpdate();
          break;
        }
        suspendWorkingSet();
        startActivityForResult(std::move(definition), [this](const ActivityResult& result) {
          if (const auto* request = std::get_if<DictionaryClippingRequest>(&result.data)) {
            setResult(ActivityResult{*request});
            finish();
            return;
          }
          // A definition is the terminal screen of reader-page lookup. Its
          // dismiss paths (Back and an outside tap) must return to the reader,
          // rather than restoring the word-selection highlight beneath it.
          setResult(ActivityResult{});
          finish();
        });
        break;
      }
      case DictionaryLookupController::LookupEvent::NotFoundDismissedBack:
        setResult(ActivityResult{});
        finish();
        break;
      case DictionaryLookupController::LookupEvent::CreateClipping:
        finishWithClippingRequest();
        break;
      case DictionaryLookupController::LookupEvent::NotFoundDismissedDone:
        setResult(ActivityResult{});
        finish();
        break;
      case DictionaryLookupController::LookupEvent::SwitchDictionary:
        openDictionarySwitch();
        break;
      case DictionaryLookupController::LookupEvent::Cancelled:
        resetCrossPageSelection();
        forceFullRepaintOnNextRender();
        requestUpdate();
        break;
      default:
        break;
    }
    return;
  }

  if (navigator.isEmpty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      DictUtils::cancelAndFinish(*this);
    }
    return;
  }

  if (navigator.handleNavigation(mappedInput, renderer)) {
    requestUpdate();
  }

#if CROSSINK_APP_CAP_TOUCH
  if (touchDragLookup_) {
    int dragX = 0;
    int dragY = 0;
    if (mappedInput.isScreenTouchHeld(dragX, dragY)) {
      touchDragHasMoved_ =
          touchDragHasMoved_ || ClipSelectionPaging::hasDraggedFrom(touchDragStartX_, touchDragStartY_, dragX, dragY);
      const int previousIdx = navigator.getCurrentFlatIndex();
      bool hit = false;
      if (navigator.selectWordAtPoint(dragX, dragY, renderer.getLineHeight(SETTINGS.getReaderFontId()), &hit)) {
        requestUpdate();
      } else if (hit && touchDragHasMoved_ && navigator.getCurrentFlatIndex() == previousIdx &&
                 navigator.getWordAt(previousIdx + 1) == nullptr && continueTouchSelectionOnNextPage()) {
        return;
      }
      return;
    }

    touchDragLookup_ = false;
    const size_t wordCount = crossPageLookupWordCount_ + navigator.getLookupSelectionWordCount();
    controller.lookupOrPopup(finishTouchLookupPhrase(), wordCount);
    if (controller.isLookingUp()) captureClippingRequest();
    return;
  }

  int touchX = 0;
  int touchY = 0;
  if (mappedInput.wasScreenTouchDown(touchX, touchY)) {
    resetCrossPageSelection();
    bool touchedWord = false;
    navigator.selectWordAtPoint(touchX, touchY, renderer.getLineHeight(SETTINGS.getReaderFontId()), &touchedWord);
    if (touchedWord && navigator.beginTouchMultiSelect()) {
      touchDragLookup_ = true;
      touchDragHasMoved_ = false;
      touchDragStartX_ = touchX;
      touchDragStartY_ = touchY;
      // Finish this fast refresh before lookup can replace the screen, so the
      // touched word always provides visible press feedback on e-ink.
      requestUpdateAndWait();
    }
    return;
  }
#endif

  // Check Back early when not in multi-select mode. This allows exit even when
  // confirmReleaseConsumed is stuck true (menu-triggered entry has no Confirm release).
  if (!navigator.isMultiSelecting() && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }

  if (controller.handleMultiSelect(navigator)) {
    if (controller.isLookingUp()) captureClippingRequest();
    return;
  }

  if (navigator.isMultiSelecting()) return;

  if (controller.handleConfirmLookup(navigator)) {
    if (controller.isLookingUp()) captureClippingRequest();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    DictUtils::cancelAndFinish(*this);
    return;
  }
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  const int lineHeight = renderer.getLineHeight(SETTINGS.getReaderFontId());
  const int currIdx = navigator.getCurrentFlatIndex();
  const bool foregroundBlack = ReaderUtils::readerForegroundBlack();

  // Differential fast path. Only valid when:
  //   - we set it up on the previous frame (RenderMode::Differential),
  //   - the controller has nothing pending to draw,
  //   - we have a current selection.
  if (nextRenderMode_ == RenderMode::Differential && !controller.isActive() && currIdx >= 0) {
    prewarmHighlightGlyphs(currIdx);
    auto dirty =
        navigator.renderHighlightDifferential(renderer, lineHeight, prevHighlightIdx_, currIdx, foregroundBlack);
    if (dirty.has_value()) {
      // Push full panel — the SDK's windowed-refresh path produces alternating black→white
      // transition failures on consecutive fast partial refreshes, so it's intentionally not
      // wired up here. The savings come from skipping page->render, which dominates the
      // pre-optimization cost; the full push at the end is a hardware floor (~444ms).
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      prevHighlightIdx_ = currIdx;
      return;
    }
    // Fall through to full repaint.
  }

  // Skip-initial-render fast path. Fires at most once per activity instance,
  // when the caller signalled the framebuffer already contains the page at
  // our margins (currently only EpubReaderActivity's hold-to-lookup path).
  // Conditions:
  //   - flag still set (one-shot),
  //   - controller has nothing to draw (an active controller would mean we
  //     re-entered render() after a sub-activity returned without the
  //     framebuffer being reset by forceFullRepaintOnNextRender()),
  //   - we have a current selection (currIdx >= 0); otherwise there is
  //     nothing to overlay and we fall through to a normal repaint.
  // We consume the flag unconditionally on first entry so any later
  // full-repaint goes through the normal clearScreen + page->render path.
  if (framebufferContainsPage_) {
    framebufferContainsPage_ = false;
    if (!controller.isActive() && currIdx >= 0) {
      // Clear the bottom strip the caller reserved (status bar OR auto-turn
      // label). Match the menu→lookup path, which wipes via clearScreen() +
      // page->render(); we skipped both, so clear that one region instead.
      if (reservedBottomHeight_ > 0) {
        int bezelTop, bezelRight, bezelBottom, bezelLeft;
        renderer.getOrientedViewableTRBL(&bezelTop, &bezelRight, &bezelBottom, &bezelLeft);
        const int clearY = renderer.getScreenHeight() - bezelBottom - reservedBottomHeight_;
        const int clearW = renderer.getScreenWidth() - bezelLeft - bezelRight;
        renderer.fillRect(bezelLeft, clearY, clearW, reservedBottomHeight_, false);
      }

      prewarmHighlightGlyphs(currIdx);

      auto setup =
          navigator.renderHighlightDifferential(renderer, lineHeight, /*prevWordIdx=*/-1, currIdx, foregroundBlack);
      bool snapshotPrimed = setup.has_value();
      if (!snapshotPrimed) {
        // Hyphenated wrap or oversize capture. The framebuffer still holds
        // the page, but we cannot prime the snapshot for the differential
        // path. Draw the multi-word highlight (which overwrites pixels under
        // each highlight rect) and force the next render to do a full
        // repaint so the renderer state is consistent. The user just pays
        // for one regular page render on the next cursor move instead of
        // on entry.
        navigator.renderHighlight(renderer, lineHeight, foregroundBlack);
      }
      clearFrontButtonHintArea();
      DictUtils::drawWordSelectButtonHints(renderer, mappedInput, navigator);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      prevHighlightIdx_ = currIdx;
      nextRenderMode_ = snapshotPrimed ? RenderMode::Differential : RenderMode::FullPage;
      return;
    }
    // Flag was set but conditions weren't met (controller active or no
    // current selection). Fall through to the normal full-repaint path.
  }

  // Full repaint path.
  renderer.clearScreen(ReaderUtils::readerBackgroundColor());
  if (controller.render()) {
    // Controller drew an overlay; framebuffer state is unknown.
    nextRenderMode_ = RenderMode::FullPage;
    prevHighlightIdx_ = -1;
    return;
  }

  // Font prewarm: scan pass accumulates text, then prewarm, then real render.
  // Without this, every cold codepoint cold-misses the 8-slot SD glyph
  // overflow ring and the page render serializes ~100+ individual SD reads.
  // Same pattern as EpubReaderActivity::renderContents().
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop, foregroundBlack);  // scan pass
  scope.endScanAndPrewarm();
  page->render(renderer, SETTINGS.getReaderFontId(), marginLeft, marginTop, foregroundBlack);

  // Set up snapshot AND draw the highlight via the differential entry point with
  // prevWordIdx = -1 (no previous highlight to wipe). This both draws the highlight
  // for this frame and primes snapshot_ so the next frame can run the fast path.
  // If the navigator declines (multi-select, hyphenated, oversize), fall back to
  // the multi-word renderHighlight and stay on the full path next frame.
  //
  // The -1 literal is load-bearing: renderHighlightDifferential uses prevWordIdx
  // < 0 as the signal "framebuffer was just redrawn from scratch, discard any
  // stale snapshot rather than restoring it on top of fresh pixels." This is the
  // only path that disturbs the framebuffer outside the differential cycle, so
  // it's also the only call site that must pass -1.
  bool snapshotPrimed = false;
  if (currIdx >= 0) {
    auto setup =
        navigator.renderHighlightDifferential(renderer, lineHeight, /*prevWordIdx=*/-1, currIdx, foregroundBlack);
    snapshotPrimed = setup.has_value();
  }
  if (!snapshotPrimed) {
    navigator.renderHighlight(renderer, lineHeight, foregroundBlack);
  }

  clearFrontButtonHintArea();
  DictUtils::drawWordSelectButtonHints(renderer, mappedInput, navigator);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  prevHighlightIdx_ = currIdx;
  nextRenderMode_ = snapshotPrimed ? RenderMode::Differential : RenderMode::FullPage;
}
