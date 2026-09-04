#include "WordSelectNavigator.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <cstdlib>
#include <cstring>

#include "MappedInputManager.h"
#include "TextPool.h"

namespace {
constexpr unsigned long WORD_REPEAT_START_MS = 500;
constexpr unsigned long WORD_REPEAT_INTERVAL_MS = 500;
}  // namespace

void WordSelectNavigator::load(std::vector<WordInfo> w, std::vector<Row> r, std::string pool,
                               bool consumeInitialConfirm) {
  ownedWords = std::move(w);
  ownedRows = std::move(r);
  ownedTextPool = std::move(pool);
  words.reset(ownedWords.data(), ownedWords.size());
  rows.reset(ownedRows.data(), ownedRows.size());
  textPool = ownedTextPool.data();
  currentRow = static_cast<int>(rows.size()) / 2;
  currentWordInRow =
      (!rows.empty() && rows[currentRow].wordCount > 0) ? static_cast<int>(rows[currentRow].wordCount) / 2 : 0;
  confirmReleaseConsumed = consumeInitialConfirm;
  lastWordRepeatTime = 0;
  wordRepeatActive = false;
}

void WordSelectNavigator::loadView(WordInfo* w, const size_t wordCount, Row* r, const size_t rowCount, const char* pool,
                                   const bool consumeInitialConfirm) {
  std::vector<WordInfo>().swap(ownedWords);
  std::vector<Row>().swap(ownedRows);
  std::string().swap(ownedTextPool);
  words.reset(w, wordCount);
  rows.reset(r, rowCount);
  textPool = pool;
  currentRow = static_cast<int>(rows.size()) / 2;
  currentWordInRow =
      (!rows.empty() && rows[currentRow].wordCount > 0) ? static_cast<int>(rows[currentRow].wordCount) / 2 : 0;
  confirmReleaseConsumed = consumeInitialConfirm;
  lastWordRepeatTime = 0;
  wordRepeatActive = false;
}

void WordSelectNavigator::organizeIntoRows(std::vector<WordInfo>& words, std::vector<Row>& rows) {
  if (words.empty()) return;
  int16_t currentY = words[0].screenY;
  rows.push_back({currentY, 0, 0});
  for (size_t i = 0; i < words.size(); i++) {
    if (std::abs(words[i].screenY - currentY) > 2) {
      currentY = words[i].screenY;
      rows.push_back({currentY, static_cast<uint16_t>(i), 0});
    }
    words[i].row = static_cast<int16_t>(rows.size() - 1);
    rows.back().wordCount++;
  }
}

uint16_t WordSelectNavigator::poolAppend(std::string& pool, const char* s, size_t len) {
  return TextPool::append(pool, s, len);
}

void WordSelectNavigator::reset() {
  ownedWords.clear();
  ownedRows.clear();
  ownedTextPool.clear();
  words.reset();
  rows.reset();
  textPool = nullptr;
  currentRow = 0;
  currentWordInRow = 0;
  inMultiSelectMode = false;
  confirmReleaseConsumed = false;
  anchorFlatIndex = -1;
  completedSelectionStart = -1;
  completedSelectionEnd = -1;
  lastWordRepeatTime = 0;
  wordRepeatActive = false;
  pendingSnapIdx = -1;
  snapshot_.clear();
}

const WordSelectNavigator::WordInfo* WordSelectNavigator::getSelected() const {
  if (rows.empty() || currentRow >= static_cast<int>(rows.size())) return nullptr;
  if (rows[currentRow].wordCount == 0) return nullptr;
  return &words[rows[currentRow].firstWord + currentWordInRow];
}

int WordSelectNavigator::getCurrentFlatIndex() const {
  if (rows.empty() || currentRow >= static_cast<int>(rows.size())) return -1;
  if (rows[currentRow].wordCount == 0) return -1;
  return rows[currentRow].firstWord + currentWordInRow;
}

bool WordSelectNavigator::getLookupSelectionRange(int& fromIdx, int& toIdx) const {
  const int currentIdx = getCurrentFlatIndex();
  if (currentIdx < 0) return false;
  if (completedSelectionStart >= 0 && completedSelectionEnd >= 0) {
    fromIdx = completedSelectionStart;
    toIdx = completedSelectionEnd;
    return true;
  }
  if (inMultiSelectMode && anchorFlatIndex >= 0) {
    fromIdx = anchorFlatIndex;
    toIdx = currentIdx;
    return true;
  }
  fromIdx = currentIdx;
  toIdx = currentIdx;
  return true;
}

size_t WordSelectNavigator::getLookupSelectionWordCount() const {
  int fromIdx = -1;
  int toIdx = -1;
  if (!getLookupSelectionRange(fromIdx, toIdx)) return 0;
  return static_cast<size_t>(fromIdx <= toIdx ? toIdx - fromIdx : fromIdx - toIdx) + 1;
}

const WordSelectNavigator::WordInfo* WordSelectNavigator::getWordAt(int idx) const {
  if (idx < 0 || idx >= static_cast<int>(words.size())) return nullptr;
  return &words[idx];
}

std::string WordSelectNavigator::buildPhrase(int fromIdx, int toIdx) const {
  const int lo = std::min(fromIdx, toIdx);
  const int hi = std::max(fromIdx, toIdx);
  std::string phrase;
  // Skip index for a hyphenated pair's second half once its merged lookup text
  // has already been emitted via the first half, so the pair isn't duplicated.
  int skipIdx = -1;
  for (int i = lo; i <= hi; i++) {
    if (i == skipIdx) continue;
    const auto* w = getWordAt(i);
    if (!w) continue;
    if (!phrase.empty() && !w->joinWithoutSpaceBefore) phrase += ' ';
    // getLookup() returns the merged, hyphen-stripped text for a hyphenated
    // pair (e.g. "externity" for "exter-" + "nity"), matching the single-word
    // lookup path. For ordinary words it equals the display text.
    phrase += getLookup(*w);
    if (w->continuationIndex >= 0) skipIdx = w->continuationIndex;
  }
  return phrase;
}

int WordSelectNavigator::findClosestWord(int targetRow) const {
  if (rows[targetRow].wordCount == 0) return 0;
  const int wordIdx = rows[currentRow].firstWord + currentWordInRow;
  const int currentCenterX = words[wordIdx].screenX + words[wordIdx].width / 2;
  return findClosestWordFromX(targetRow, currentCenterX);
}

int WordSelectNavigator::findClosestWordFromX(int targetRow, int refCenterX) const {
  if (rows[targetRow].wordCount == 0) return 0;
  int bestMatch = 0;
  int bestDist = INT_MAX;
  for (int i = 0; i < rows[targetRow].wordCount; i++) {
    const int idx = rows[targetRow].firstWord + i;
    const int centerX = words[idx].screenX + words[idx].width / 2;
    const int dist = std::abs(centerX - refCenterX);
    if (dist < bestDist) {
      bestDist = dist;
      bestMatch = i;
    }
  }
  return bestMatch;
}

bool WordSelectNavigator::handleNavigation(const MappedInputManager& input, const GfxRenderer& renderer) {
  if (rows.empty()) return false;

  const auto orient = renderer.getOrientation();
  const bool isLandscapeCw = orient == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orient == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isInverted = orient == GfxRenderer::Orientation::PortraitInverted;
  const bool landscape = isLandscapeCw || isLandscapeCcw;

  bool rowPrevPressed, rowNextPressed, wordPrevPressed, wordNextPressed;
  bool wordPrevHeld, wordNextHeld;

  if (isLandscapeCw) {
    rowPrevPressed = input.wasReleased(MappedInputManager::Button::Left);
    rowNextPressed = input.wasReleased(MappedInputManager::Button::Right);
    wordPrevPressed = input.wasReleased(MappedInputManager::Button::Down);
    wordNextPressed = input.wasReleased(MappedInputManager::Button::Up);
    wordPrevHeld = input.isPressed(MappedInputManager::Button::Down);
    wordNextHeld = input.isPressed(MappedInputManager::Button::Up);
  } else if (landscape) {
    const bool frontNavSwapped = input.isFrontNavButtonSwapActive();
    rowPrevPressed =
        input.wasReleased(frontNavSwapped ? MappedInputManager::Button::Left : MappedInputManager::Button::Right);
    rowNextPressed =
        input.wasReleased(frontNavSwapped ? MappedInputManager::Button::Right : MappedInputManager::Button::Left);
    wordPrevPressed = input.wasReleased(MappedInputManager::Button::Up);
    wordNextPressed = input.wasReleased(MappedInputManager::Button::Down);
    wordPrevHeld = input.isPressed(MappedInputManager::Button::Up);
    wordNextHeld = input.isPressed(MappedInputManager::Button::Down);
  } else if (isInverted) {
    rowPrevPressed = input.wasReleased(MappedInputManager::Button::Down);
    rowNextPressed = input.wasReleased(MappedInputManager::Button::Up);
    wordPrevPressed = input.wasReleased(MappedInputManager::Button::Right);
    wordNextPressed = input.wasReleased(MappedInputManager::Button::Left);
    wordPrevHeld = input.isPressed(MappedInputManager::Button::Right);
    wordNextHeld = input.isPressed(MappedInputManager::Button::Left);
  } else {
    rowPrevPressed = input.wasReleased(MappedInputManager::Button::Up);
    rowNextPressed = input.wasReleased(MappedInputManager::Button::Down);
    wordPrevPressed = input.wasReleased(MappedInputManager::Button::Left);
    wordNextPressed = input.wasReleased(MappedInputManager::Button::Right);
    wordPrevHeld = input.isPressed(MappedInputManager::Button::Left);
    wordNextHeld = input.isPressed(MappedInputManager::Button::Right);
  }

  const unsigned long now = millis();
  const bool repeatDue = (wordPrevHeld || wordNextHeld) && input.getHeldTime() >= WORD_REPEAT_START_MS &&
                         (!wordRepeatActive || now - lastWordRepeatTime >= WORD_REPEAT_INTERVAL_MS);
  if (repeatDue) {
    wordPrevPressed = wordPrevHeld;
    wordNextPressed = wordNextHeld;
    wordRepeatActive = true;
    lastWordRepeatTime = now;
  } else if (wordRepeatActive && (wordPrevPressed || wordNextPressed)) {
    // A repeated hold already moved at the threshold; do not add one more
    // step when that same button is released.
    wordPrevPressed = false;
    wordNextPressed = false;
    wordRepeatActive = false;
  } else if (!wordPrevHeld && !wordNextHeld) {
    wordRepeatActive = false;
  }

  const int rowCount = static_cast<int>(rows.size());
  bool changed = false;
  const int prevFlatIdx = getCurrentFlatIndex();

  // If the previous action was a wordPrev snap (second half → first half across
  // rows), use the second half's position as the row-nav reference so that
  // rowPrev/rowNext feels like it originates from where the user was.
  // Any directional input clears this state.
  const bool hasPendingSnap = pendingSnapIdx >= 0;
  const int rowNavBase = hasPendingSnap ? words[pendingSnapIdx].row : currentRow;
  const int rowNavRefX = hasPendingSnap ? words[pendingSnapIdx].screenX + words[pendingSnapIdx].width / 2 : -1;
  if (rowPrevPressed || rowNextPressed || wordPrevPressed || wordNextPressed) {
    pendingSnapIdx = -1;
  }

  if (rowPrevPressed) {
    const int targetRow = (rowNavBase > 0) ? rowNavBase - 1 : rowCount - 1;
    currentWordInRow = (rowNavRefX >= 0) ? findClosestWordFromX(targetRow, rowNavRefX) : findClosestWord(targetRow);
    currentRow = targetRow;
    changed = true;
  }

  if (rowNextPressed) {
    const int targetRow = (rowNavBase < rowCount - 1) ? rowNavBase + 1 : 0;
    currentWordInRow = (rowNavRefX >= 0) ? findClosestWordFromX(targetRow, rowNavRefX) : findClosestWord(targetRow);
    currentRow = targetRow;
    changed = true;
  }

  if (wordPrevPressed) {
    if (currentWordInRow > 0) {
      currentWordInRow--;
    } else if (rowCount > 1) {
      currentRow = (currentRow > 0) ? currentRow - 1 : rowCount - 1;
      currentWordInRow = static_cast<int>(rows[currentRow].wordCount) - 1;
    }
    changed = true;
  }

  if (wordNextPressed) {
    if (currentWordInRow < static_cast<int>(rows[currentRow].wordCount) - 1) {
      currentWordInRow++;
    } else if (rowCount > 1) {
      currentRow = (currentRow < rowCount - 1) ? currentRow + 1 : 0;
      currentWordInRow = 0;
    } else {
      currentWordInRow = 0;  // single-row wrap
    }
    changed = true;
  }

  // Hyphenated pair smoothing for horizontal navigation:
  // the second half should not be a horizontal stop since both halves
  // highlight together. Row navigation (up/down) is exempt — the user
  // may intend to land on the second half's row.
  if (changed) {
    const int idx = getCurrentFlatIndex();
    if (idx >= 0 && words[idx].continuationOf >= 0) {
      if (wordNextPressed) {
        // Moving forward: skip past the second half to the next word.
        if (currentWordInRow < static_cast<int>(rows[currentRow].wordCount) - 1) {
          currentWordInRow++;
        } else if (rowCount > 1) {
          currentRow = (currentRow < rowCount - 1) ? currentRow + 1 : 0;
          currentWordInRow = 0;
        } else {
          currentWordInRow = 0;  // single-row wrap
        }
        // If the skip landed on yet another continuation, snap to its first half.
        const int skippedIdx = getCurrentFlatIndex();
        if (skippedIdx >= 0 && words[skippedIdx].continuationOf >= 0) {
          const int firstIdx = words[skippedIdx].continuationOf;
          currentRow = words[firstIdx].row;
          currentWordInRow = firstIdx - rows[currentRow].firstWord;
        }
      } else if (wordPrevPressed) {
        // Moving backward: snap to the first half.
        // Record the second half's index so subsequent row navigation
        // references its position rather than the first half's.
        pendingSnapIdx = idx;
        const int firstIdx = words[idx].continuationOf;
        currentRow = words[firstIdx].row;
        currentWordInRow = firstIdx - rows[currentRow].firstWord;
      }
      // Row navigation leaves cursor on whichever half
      // findClosestWord landed on. Both halves highlight regardless.
    }

    // Symmetric with the wordNext skip: if we came directly from the second
    // half and wrapped into its first half, skip backward past the first half
    // so the pair is treated as a single navigation unit in both directions.
    if (wordPrevPressed) {
      const int curIdx = getCurrentFlatIndex();
      if (curIdx >= 0 && words[curIdx].continuationOf < 0 && words[curIdx].continuationIndex >= 0 &&
          prevFlatIdx == words[curIdx].continuationIndex) {
        if (currentWordInRow > 0) {
          currentWordInRow--;
        } else if (rowCount > 1) {
          currentRow = (currentRow > 0) ? currentRow - 1 : rowCount - 1;
          currentWordInRow = static_cast<int>(rows[currentRow].wordCount) - 1;
        }
      }
    }
  }

  return changed;
}

bool WordSelectNavigator::selectWordAtPoint(const int x, const int y, const int lineHeight, bool* hit) {
  if (hit) *hit = false;
  if (rows.empty() || words.empty() || lineHeight <= 0) return false;

  const int horizontalPad = 6;
  const int verticalPad = std::max(4, lineHeight / 4);
  int bestIdx = -1;
  int bestScore = INT_MAX;

  for (int idx = 0; idx < static_cast<int>(words.size()); idx++) {
    const auto& word = words[idx];
    const int left = static_cast<int>(word.screenX) - horizontalPad;
    const int right = static_cast<int>(word.screenX) + static_cast<int>(word.width) + horizontalPad;
    const int top = static_cast<int>(word.screenY) - verticalPad;
    const int bottom = static_cast<int>(word.screenY) + lineHeight + verticalPad;
    if (x < left || x > right || y < top || y > bottom) continue;

    const int centerX = static_cast<int>(word.screenX) + static_cast<int>(word.width) / 2;
    const int centerY = static_cast<int>(word.screenY) + lineHeight / 2;
    const int score = std::abs(x - centerX) + std::abs(y - centerY);
    if (score < bestScore) {
      bestScore = score;
      bestIdx = idx;
    }
  }

  if (bestIdx < 0) return false;
  if (hit) *hit = true;
  const int targetRow = words[bestIdx].row;
  if (targetRow < 0 || targetRow >= static_cast<int>(rows.size())) return false;

  int targetWordInRow = -1;
  if (bestIdx >= rows[targetRow].firstWord && bestIdx < rows[targetRow].firstWord + rows[targetRow].wordCount) {
    targetWordInRow = bestIdx - rows[targetRow].firstWord;
  }
  if (targetWordInRow < 0) return false;

  const bool changed = currentRow != targetRow || currentWordInRow != targetWordInRow;
  currentRow = targetRow;
  currentWordInRow = targetWordInRow;
  pendingSnapIdx = -1;
  return changed;
}

WordSelectNavigator::MultiSelectAction WordSelectNavigator::handleMultiSelectInput(const MappedInputManager& input,
                                                                                   std::string& outPhrase,
                                                                                   unsigned long longPressMs) {
  if (inMultiSelectMode) {
    // Consume the Confirm release that follows the threshold-fire entry into multi-select.
    if (confirmReleaseConsumed) {
      if (input.wasReleased(MappedInputManager::Button::Confirm) ||
          !input.isPressed(MappedInputManager::Button::Confirm)) {
        confirmReleaseConsumed = false;
      }
      return MultiSelectAction::None;
    }
    if (input.wasReleased(MappedInputManager::Button::Confirm)) {
      const int cursorIdx = getCurrentFlatIndex();
      outPhrase = buildPhrase(anchorFlatIndex, cursorIdx);
      completedSelectionStart = anchorFlatIndex;
      completedSelectionEnd = cursorIdx;
      inMultiSelectMode = false;
      return MultiSelectAction::PhraseReady;
    }
    if (input.wasReleased(MappedInputManager::Button::Back)) {
      inMultiSelectMode = false;
      return MultiSelectAction::ExitedMultiSelect;
    }
    return MultiSelectAction::None;
  }

  // Consume the Confirm press+release that carried over from the long-press that opened word selection.
  // Must block both the held-state check (which would immediately enter multi-select) and
  // the subsequent release event (which would trigger a single-word lookup in the activity).
  if (confirmReleaseConsumed) {
    if (input.wasReleased(MappedInputManager::Button::Confirm) ||
        !input.isPressed(MappedInputManager::Button::Confirm)) {
      confirmReleaseConsumed = false;
    }
    return MultiSelectAction::Consumed;
  }

  // Long press Confirm: enter multi-select (fire at threshold, not on release).
  if (input.isPressed(MappedInputManager::Button::Confirm) && input.getHeldTime() >= longPressMs) {
    const int flatIdx = getCurrentFlatIndex();
    if (flatIdx >= 0) {
      inMultiSelectMode = true;
      anchorFlatIndex = flatIdx;
      completedSelectionStart = -1;
      completedSelectionEnd = -1;
      confirmReleaseConsumed = true;
      return MultiSelectAction::EnteredMultiSelect;
    }
    return MultiSelectAction::Consumed;
  }

  return MultiSelectAction::None;
}

bool WordSelectNavigator::beginTouchMultiSelect() {
  const int flatIdx = getCurrentFlatIndex();
  if (flatIdx < 0) return false;
  inMultiSelectMode = true;
  anchorFlatIndex = flatIdx;
  completedSelectionStart = -1;
  completedSelectionEnd = -1;
  return true;
}

std::string WordSelectNavigator::finishTouchMultiSelect() {
  if (!inMultiSelectMode) return {};
  const std::string phrase = buildPhrase(anchorFlatIndex, getCurrentFlatIndex());
  completedSelectionStart = anchorFlatIndex;
  completedSelectionEnd = getCurrentFlatIndex();
  inMultiSelectMode = false;
  return phrase;
}

bool WordSelectNavigator::HighlightSnapshot::capture(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                                     const GfxRenderer& renderer) {
  bytes_ = 0;
  if (!storage_) return false;
  const size_t bytes =
      renderer.readFramebufferRegion(x, y, w, h, storage_->bytes, HighlightSnapshotStorage::MAX_SNAPSHOT_BYTES);
  if (bytes == 0) return false;
  x_ = x;
  y_ = y;
  w_ = w;
  h_ = h;
  bytes_ = bytes;
  return true;
}

void WordSelectNavigator::HighlightSnapshot::restore(GfxRenderer& renderer) const {
  if (!valid() || !storage_) return;
  renderer.writeFramebufferRegion(x_, y_, w_, h_, storage_->bytes);
}

void WordSelectNavigator::releaseWorkingSet() {
  reset();
  std::vector<WordInfo>().swap(ownedWords);
  std::vector<Row>().swap(ownedRows);
  std::string().swap(ownedTextPool);
}

void WordSelectNavigator::renderHighlight(const GfxRenderer& renderer, int lineHeight,
                                          const bool foregroundBlack) const {
  if (inMultiSelectMode) {
    const int cursorIdx = getCurrentFlatIndex();
    const int lo = std::min(anchorFlatIndex, cursorIdx);
    const int hi = std::max(anchorFlatIndex, cursorIdx);
    for (int i = lo; i <= hi; i++) {
      drawSingleHighlight(renderer, lineHeight, i, foregroundBlack);
      drawContinuationsIfOutside(renderer, lineHeight, getWordAt(i), lo, hi, foregroundBlack);
    }
    drawTouchDragCursor(renderer, lineHeight, cursorIdx, foregroundBlack);
  } else {
    const int selIdx = getCurrentFlatIndex();
    if (selIdx < 0) return;
    drawSingleHighlight(renderer, lineHeight, selIdx, foregroundBlack);
    drawContinuationsIfOutside(renderer, lineHeight, getWordAt(selIdx), selIdx, selIdx, foregroundBlack);
    drawTouchDragCursor(renderer, lineHeight, selIdx, foregroundBlack);
  }
}

void WordSelectNavigator::drawSingleHighlight(const GfxRenderer& renderer, int lineHeight, int wordIndex,
                                              const bool foregroundBlack) const {
  const auto* w = getWordAt(wordIndex);
  if (!w) return;
  renderer.fillRect(w->screenX - 2, w->screenY - 2, w->width + 4, lineHeight + 4, foregroundBlack);
  const char* displayedText = getDisplay(*w);
  const auto baseDir = w->isRtl ? BidiUtils::BidiBaseDir::RTL : BidiUtils::BidiBaseDir::LTR;
  if (w->bionicBoundary > 0 && w->bionicSuffixX > 0) {
    const auto boldStyle = static_cast<EpdFontFamily::Style>(w->style | EpdFontFamily::BOLD);
    char boldBuf[40];
    const size_t boldLen =
        std::min<size_t>({static_cast<size_t>(w->bionicBoundary), strlen(displayedText), sizeof(boldBuf) - 1});
    memcpy(boldBuf, displayedText, boldLen);
    boldBuf[boldLen] = '\0';
    if (w->isRtl) {
      renderer.drawText(w->fontId, w->screenX, w->screenY, displayedText + boldLen, !foregroundBlack, w->style,
                        baseDir);
      renderer.drawText(w->fontId, w->screenX + w->bionicSuffixX, w->screenY, boldBuf, !foregroundBlack, boldStyle,
                        baseDir);
    } else {
      renderer.drawText(w->fontId, w->screenX, w->screenY, boldBuf, !foregroundBlack, boldStyle, baseDir);
      renderer.drawText(w->fontId, w->screenX + w->bionicSuffixX, w->screenY, displayedText + boldLen, !foregroundBlack,
                        w->style, baseDir);
    }
    return;
  }
  renderer.drawText(w->fontId, w->screenX, w->screenY, displayedText, !foregroundBlack, w->style, baseDir);
}

void WordSelectNavigator::drawTouchDragCursor(const GfxRenderer& renderer, int lineHeight, int wordIndex,
                                              const bool foregroundBlack) const {
  if (!touchDragCursorVisible) return;
  const auto* w = getWordAt(wordIndex);
  if (!w) return;
  const int x = w->screenX + w->width + 4;
  const int top = w->screenY - 2;
  const int bottom = w->screenY + lineHeight + 1;
  renderer.drawLine(x, top, x, bottom, 2, foregroundBlack);
  renderer.drawLine(x - 2, top, x + 3, top, 2, foregroundBlack);
  renderer.drawLine(x - 2, bottom, x + 3, bottom, 2, foregroundBlack);
}

void WordSelectNavigator::drawContinuationsIfOutside(const GfxRenderer& renderer, int lineHeight, const WordInfo* w,
                                                     int lo, int hi, const bool foregroundBlack) const {
  if (!w) return;
  if (w->continuationIndex >= 0 && (w->continuationIndex < lo || w->continuationIndex > hi)) {
    drawSingleHighlight(renderer, lineHeight, w->continuationIndex, foregroundBlack);
  }
  if (w->continuationOf >= 0 && (w->continuationOf < lo || w->continuationOf > hi)) {
    drawSingleHighlight(renderer, lineHeight, w->continuationOf, foregroundBlack);
  }
}

WordSelectNavigator::Rect WordSelectNavigator::boundsForWord(int wordIndex, int lineHeight) const {
  const auto* w = getWordAt(wordIndex);
  if (!w) return Rect{};
  const int cursorWidth = touchDragCursorVisible ? 10 : 0;
  return Rect{static_cast<int>(w->screenX) - 2, static_cast<int>(w->screenY) - 2,
              static_cast<int>(w->width) + 4 + cursorWidth, lineHeight + 4};
}

WordSelectNavigator::Rect WordSelectNavigator::computeDirtyRect(int prevWordIdx, int currWordIdx,
                                                                int lineHeight) const {
  Rect curr = boundsForWord(currWordIdx, lineHeight);
  if (prevWordIdx < 0) return curr;
  Rect prev = boundsForWord(prevWordIdx, lineHeight);
  if (prev.width == 0 || prev.height == 0) return curr;
  if (curr.width == 0 || curr.height == 0) return prev;
  const int x0 = std::min(prev.x, curr.x);
  const int y0 = std::min(prev.y, curr.y);
  const int x1 = std::max(prev.x + prev.width, curr.x + curr.width);
  const int y1 = std::max(prev.y + prev.height, curr.y + curr.height);
  return Rect{x0, y0, x1 - x0, y1 - y0};
}

std::optional<WordSelectNavigator::Rect> WordSelectNavigator::renderHighlightDifferential(
    GfxRenderer& renderer, int lineHeight, int prevWordIdx, int currWordIdx, const bool foregroundBlack) {
  // Fallback paths.
  if (inMultiSelectMode) return std::nullopt;
  const auto* curr = getWordAt(currWordIdx);
  if (!curr) return std::nullopt;
  if (curr->continuationIndex >= 0 || curr->continuationOf >= 0) {
    // Hyphenated wrap — two-word highlight is not yet supported by the
    // single-snapshot fast path. Caller falls back to full repaint.
    return std::nullopt;
  }

  // Step 1: restore pixels under the previous highlight (wipe it).
  // prevWordIdx < 0 is the caller's signal "no previous highlight on screen"
  // (typically because the framebuffer was just redrawn from scratch via the
  // full-repaint path or a sub-activity return). In that case any snapshot we
  // still hold from a prior render cycle is stale relative to the current
  // framebuffer — discard it rather than restoring it on top of fresh pixels.
  if (prevWordIdx >= 0 && snapshot_.valid()) {
    snapshot_.restore(renderer);
  }
  snapshot_.clear();

  // Step 2: snapshot pixels under the new highlight, clamping coordinates so we
  // never pass negative values into the renderer's uint16_t API.
  const Rect newRect = boundsForWord(currWordIdx, lineHeight);
  const uint16_t snapX = static_cast<uint16_t>(std::max(newRect.x, 0));
  const uint16_t snapY = static_cast<uint16_t>(std::max(newRect.y, 0));
  const uint16_t snapW = static_cast<uint16_t>(std::max(newRect.width, 0));
  const uint16_t snapH = static_cast<uint16_t>(std::max(newRect.height, 0));
  if (!snapshot_.capture(snapX, snapY, snapW, snapH, renderer)) {
    // Capture failed — either too big or out of bounds. Caller falls back.
    return std::nullopt;
  }

  // Step 3: draw the new highlight on top of the captured pixels.
  drawSingleHighlight(renderer, lineHeight, currWordIdx, foregroundBlack);
  drawTouchDragCursor(renderer, lineHeight, currWordIdx, foregroundBlack);

  // Step 4: caller pushes the union region.
  return computeDirtyRect(prevWordIdx, currWordIdx, lineHeight);
}
