#include "ClipSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "activities/ActivityResult.h"
#include "clippings/ClipTextBuilder.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

bool hasEmSpace(const std::string& text) {
  return text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xE2 &&
         static_cast<unsigned char>(text[1]) == 0x80 && static_cast<unsigned char>(text[2]) == 0x83;
}

}  // namespace

ClipSelectionActivity::ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::vector<WordRef> words, const int fontId, Section& section,
                                             const int startPageInSection, const int marginTop, const int marginLeft)
    : Activity("ClipSelection", renderer, mappedInput),
      words(std::move(words)),
      fontId(fontId),
      section(section),
      startPageInSection(startPageInSection),
      marginTop(marginTop),
      marginLeft(marginLeft) {}

void ClipSelectionActivity::onEnter() {
  Activity::onEnter();

  if (words.empty()) {
    LOG_ERR("CLIP", "No words available for selection");
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  buildReadingOrder();
  if (readingOrder.empty()) {
    LOG_ERR("CLIP", "No readable word order available");
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  cursorIdx = 0;

  savedSectionPage = section.currentPage;
  if (!allocateSavedBuffer()) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (!switchToPage(0)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  requestUpdate();
}

void ClipSelectionActivity::onExit() {
  section.currentPage = savedSectionPage;
  savedBufferChunks.clear();
  hasSavedBuffer = false;
  Activity::onExit();
}

bool ClipSelectionActivity::allocateSavedBuffer() {
  savedBufferSize = renderer.getBufferSize();
  const size_t chunkCount = (savedBufferSize + BUFFER_CHUNK_SIZE - 1) / BUFFER_CHUNK_SIZE;
  savedBufferChunks.reserve(chunkCount);

  for (size_t i = 0; i < chunkCount; i++) {
    const size_t offset = i * BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BUFFER_CHUNK_SIZE, savedBufferSize - offset);
    auto chunk = makeUniqueNoThrow<uint8_t[]>(chunkSize);
    if (!chunk) {
      LOG_ERR("CLIP", "OOM: clipping page snapshot chunk %u (%u bytes)", static_cast<unsigned>(i),
              static_cast<unsigned>(chunkSize));
      savedBufferChunks.clear();
      return false;
    }
    savedBufferChunks.push_back(std::move(chunk));
  }
  return true;
}

void ClipSelectionActivity::storeCurrentBuffer() {
  const uint8_t* frameBuffer = renderer.getFrameBuffer();
  for (size_t i = 0; i < savedBufferChunks.size(); i++) {
    const size_t offset = i * BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BUFFER_CHUNK_SIZE, savedBufferSize - offset);
    memcpy(savedBufferChunks[i].get(), frameBuffer + offset, chunkSize);
  }
  hasSavedBuffer = true;
}

void ClipSelectionActivity::restoreSavedBuffer() const {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  for (size_t i = 0; i < savedBufferChunks.size(); i++) {
    const size_t offset = i * BUFFER_CHUNK_SIZE;
    const size_t chunkSize = std::min(BUFFER_CHUNK_SIZE, savedBufferSize - offset);
    memcpy(frameBuffer + offset, savedBufferChunks[i].get(), chunkSize);
  }
}

void ClipSelectionActivity::buildReadingOrder() {
  readingOrder.clear();
  readingOrder.reserve(words.size());

  int lineStart = 0;
  const int total = static_cast<int>(words.size());
  while (lineStart < total) {
    int lineEnd = lineStart + 1;
    while (lineEnd < total && words[lineEnd].pageIdx == words[lineStart].pageIdx &&
           words[lineEnd].y == words[lineStart].y) {
      lineEnd++;
    }

    if (words[lineStart].lineIsRtl) {
      for (int i = lineEnd - 1; i >= lineStart; --i) {
        readingOrder.push_back(i);
      }
    } else {
      for (int i = lineStart; i < lineEnd; ++i) {
        readingOrder.push_back(i);
      }
    }
    lineStart = lineEnd;
  }
}

void ClipSelectionActivity::loop() {
  const int total = static_cast<int>(readingOrder.size());
  using Button = MappedInputManager::Button;

  auto moveCursor = [this](const int nextOrderIdx) {
    if (nextOrderIdx == cursorIdx || nextOrderIdx < 0 || nextOrderIdx >= static_cast<int>(readingOrder.size())) return;
    const int previousPage = words[readingOrder[cursorIdx]].pageIdx;
    cursorIdx = nextOrderIdx;
    if (words[readingOrder[cursorIdx]].pageIdx != previousPage) {
      needsPageSwitch = true;
    }
    requestUpdate();
  };

  buttonNavigator.onRelease({Button::Left}, [this, &moveCursor] {
    if (cursorIdx > 0) moveCursor(cursorIdx - 1);
  });
  buttonNavigator.onContinuous({Button::Left}, [this, &moveCursor] {
    if (cursorIdx > 0) moveCursor(cursorIdx - 1);
  });
  buttonNavigator.onRelease({Button::Right}, [this, total, &moveCursor] {
    if (cursorIdx + 1 < total) moveCursor(cursorIdx + 1);
  });
  buttonNavigator.onContinuous({Button::Right}, [this, total, &moveCursor] {
    if (cursorIdx + 1 < total) moveCursor(cursorIdx + 1);
  });
  buttonNavigator.onRelease({Button::Down}, [this, &moveCursor] { moveCursor(lineEndForward(cursorIdx)); });
  buttonNavigator.onContinuous({Button::Down}, [this, &moveCursor] { moveCursor(lineEndForward(cursorIdx)); });
  buttonNavigator.onRelease({Button::Up}, [this, &moveCursor] { moveCursor(lineEndBackward(cursorIdx)); });
  buttonNavigator.onContinuous({Button::Up}, [this, &moveCursor] { moveCursor(lineEndBackward(cursorIdx)); });

  if (mappedInput.wasReleased(Button::Confirm)) {
    if (startMarkIdx == -1) {
      startMarkIdx = cursorIdx;
      requestUpdate();
    } else {
      const int from = std::min(startMarkIdx, cursorIdx);
      const int to = std::max(startMarkIdx, cursorIdx);
      auto result = ClipTextBuilder::build(words, readingOrder, from, to, total, startPageInSection, section.pageCount);
      if (const auto paragraphIndex = section.getParagraphIndexForPage(result.sectionPage)) {
        result.paragraphIndex = *paragraphIndex;
      }
      setResult(std::move(result));
      finish();
    }
    return;
  }

  if (mappedInput.wasReleased(Button::Back)) {
    if (startMarkIdx != -1) {
      startMarkIdx = -1;
      requestUpdate();
      return;
    }

    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
  }
}

void ClipSelectionActivity::render(RenderLock&&) {
  if (!hasSavedBuffer) return;

  if (needsPageSwitch) {
    switchToPage(words[readingOrder[cursorIdx]].pageIdx);
    needsPageSwitch = false;
  } else {
    restoreSavedBuffer();
  }

  prewarmHighlightedWords();
  drawHighlights();

  const auto confirmLabel = startMarkIdx == -1 ? tr(STR_SELECT) : tr(STR_DONE);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

bool ClipSelectionActivity::switchToPage(const int pageIdx) {
  const int oldPage = section.currentPage;
  section.currentPage = startPageInSection + pageIdx;
  auto page = section.loadPageFromSectionFile();
  if (!page) {
    section.currentPage = oldPage;
    LOG_ERR("CLIP", "Failed to load selection page %d", pageIdx);
    return false;
  }

  if (auto* fcm = renderer.getFontCacheManager()) {
    auto scope = fcm->createPrewarmScope();
    page->renderText(renderer, fontId, marginLeft, marginTop, ReaderUtils::readerForegroundBlack());
    scope.endScanAndPrewarm();
    renderer.clearScreen(ReaderUtils::readerBackgroundColor());
    page->render(renderer, fontId, marginLeft, marginTop, ReaderUtils::readerForegroundBlack());
  } else {
    renderer.clearScreen(ReaderUtils::readerBackgroundColor());
    page->render(renderer, fontId, marginLeft, marginTop, ReaderUtils::readerForegroundBlack());
  }

  storeCurrentBuffer();
  currentDisplayPage = pageIdx;
  return true;
}

void ClipSelectionActivity::applyWordStyle(const WordRef& word, const ClipWordStyle& style) const {
  const auto textStyle = static_cast<EpdFontFamily::Style>(word.style & ~EpdFontFamily::UNDERLINE);
  const int skipX = hasEmSpace(word.text) ? renderer.getTextAdvanceX(fontId, "\xe2\x80\x83", textStyle) : 0;
  const int drawX = word.x + skipX;
  const int drawW = word.w - skipX;
  if (drawW <= 0) return;

  const bool invert = (style.flags & ClipWordStyle::INVERT) != 0;
  const bool fill = !invert && (style.flags & ClipWordStyle::FILL) != 0;
  if (invert) {
    renderer.fillRect(drawX, word.y, drawW, word.h, true);
  } else if (fill) {
    renderer.fillRectDither(drawX, word.y, drawW, word.h, style.fillColor);
  }

  if ((style.flags & ClipWordStyle::BORDER) != 0) {
    renderer.drawRect(drawX, word.y, drawW, word.h, !invert);
  }

  if (word.text.find_first_not_of(" \t") != std::string::npos) {
    const bool textBlack = !invert;
    renderer.drawText(fontId, drawX, word.y, hasEmSpace(word.text) ? word.text.c_str() + 3 : word.text.c_str(),
                      textBlack, textStyle);
  }

  if ((style.flags & ClipWordStyle::UNDERLINE) != 0) {
    const int underlineY = word.y + renderer.getFontAscenderSize(fontId) + 2;
    renderer.drawLine(drawX, underlineY, drawX + drawW, underlineY, true);
  }
}

void ClipSelectionActivity::prewarmHighlightedWords() const {
  if (!renderer.isSdCardFont(fontId)) return;

  auto* fcm = renderer.getFontCacheManager();
  if (!fcm) return;

  for (auto& text : prewarmTextByStyle) {
    text.clear();
  }

  const auto appendWord = [this](const WordRef& word) {
    if (word.pageIdx != currentDisplayPage) return;
    const uint8_t styleIdx = static_cast<uint8_t>(word.style) & 0x03;
    if (styleIdx >= prewarmTextByStyle.size()) return;
    prewarmTextByStyle[styleIdx] += word.text;
    prewarmTextByStyle[styleIdx].push_back(' ');
  };

  if (startMarkIdx != -1) {
    const int from = std::min(startMarkIdx, cursorIdx);
    const int to = std::max(startMarkIdx, cursorIdx);
    for (int i = from; i <= to; i++) {
      appendWord(words[readingOrder[i]]);
    }
  }

  appendWord(words[readingOrder[cursorIdx]]);

  for (uint8_t styleIdx = 0; styleIdx < prewarmTextByStyle.size(); styleIdx++) {
    if (!prewarmTextByStyle[styleIdx].empty()) {
      fcm->prewarmCache(fontId, prewarmTextByStyle[styleIdx].c_str(), static_cast<uint8_t>(1u << styleIdx));
    }
  }
}

void ClipSelectionActivity::drawHighlights() {
  static constexpr ClipWordStyle selectionStyle{ClipWordStyle::FILL | ClipWordStyle::UNDERLINE, Color::LightGray};
  static constexpr ClipWordStyle cursorStyle{ClipWordStyle::INVERT, Color::LightGray};

  if (startMarkIdx != -1) {
    const int from = std::min(startMarkIdx, cursorIdx);
    const int to = std::max(startMarkIdx, cursorIdx);
    for (int i = from; i <= to; i++) {
      const WordRef& word = words[readingOrder[i]];
      if (word.pageIdx == currentDisplayPage) {
        applyWordStyle(word, selectionStyle);
      }
    }
  }

  const WordRef& cursorWord = words[readingOrder[cursorIdx]];
  if (cursorWord.pageIdx == currentDisplayPage) {
    applyWordStyle(cursorWord, cursorStyle);
  }
}

int ClipSelectionActivity::lineEndForward(const int orderIdx) const {
  const int total = static_cast<int>(readingOrder.size());
  const WordRef& current = words[readingOrder[orderIdx]];
  for (int i = orderIdx + 1; i < total; ++i) {
    const WordRef& word = words[readingOrder[i]];
    if (word.pageIdx != current.pageIdx || word.y != current.y) return i;
  }
  return orderIdx;
}

int ClipSelectionActivity::lineEndBackward(const int orderIdx) const {
  const WordRef& current = words[readingOrder[orderIdx]];
  int i = orderIdx - 1;
  for (; i >= 0; --i) {
    const WordRef& word = words[readingOrder[i]];
    if (word.pageIdx != current.pageIdx || word.y != current.y) break;
  }
  if (i < 0) return orderIdx;

  const WordRef& previous = words[readingOrder[i]];
  int first = i;
  for (; i >= 0; --i) {
    const WordRef& word = words[readingOrder[i]];
    if (word.pageIdx != previous.pageIdx || word.y != previous.y) break;
    first = i;
  }
  return first;
}
