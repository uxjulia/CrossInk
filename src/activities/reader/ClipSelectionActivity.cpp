#include "ClipSelectionActivity.h"

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "../ActivityResult.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

ClipSelectionActivity::ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::vector<WordRef> words, std::string bookTitle, std::string author,
                                             std::string chapterTitle, int pageNumber, int fontId, Section& section,
                                             int startPageInSection, int marginTop, int marginLeft)
    : Activity("ClipSelection", renderer, mappedInput),
      words(std::move(words)),
      bookTitle(std::move(bookTitle)),
      author(std::move(author)),
      chapterTitle(std::move(chapterTitle)),
      pageNumber(pageNumber),
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

  savedSectionPage = section.currentPage;
  savedBufferSize = renderer.getBufferSize();
  savedBuffer = static_cast<uint8_t*>(malloc(savedBufferSize));
  if (!savedBuffer) {
    LOG_ERR("CLIP", "malloc failed: %u bytes", savedBufferSize);
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  // Re-render page 0 to get a clean framebuffer — the previous activity (menu)
  // may still be painted on screen when onEnter() runs.
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
  free(savedBuffer);
  savedBuffer = nullptr;
  Activity::onExit();
}

void ClipSelectionActivity::loop() {
  const int total = static_cast<int>(words.size());

  buttonNavigator.onRelease({MappedInputManager::Button::Right}, [this, total] {
    if (cursorIdx + 1 >= total) return;
    moveCursorToIndex(cursorIdx + 1);
  });

  buttonNavigator.onContinuous({MappedInputManager::Button::Right}, [this] {
    const int next = lineEndForward(cursorIdx);
    if (next == cursorIdx) return;
    moveCursorToIndex(next);
  });

  buttonNavigator.onRelease({MappedInputManager::Button::Left}, [this] {
    if (cursorIdx == 0) return;
    moveCursorToIndex(cursorIdx - 1);
  });

  buttonNavigator.onContinuous({MappedInputManager::Button::Left}, [this] {
    const int prev = lineEndBackward(cursorIdx);
    if (prev == cursorIdx) return;
    moveCursorToIndex(prev);
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] {
    const int nextLine = adjacentLineIndex(cursorIdx, 1);
    if (nextLine == cursorIdx) return;
    moveCursorToIndex(nextLine);
  });

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] {
    const int prevLine = adjacentLineIndex(cursorIdx, -1);
    if (prevLine == cursorIdx) return;
    moveCursorToIndex(prevLine);
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (startMarkIdx == -1) {
      startMarkIdx = cursorIdx;
      requestUpdate();
    } else {
      const int from = std::min(startMarkIdx, cursorIdx);
      const int to = std::max(startMarkIdx, cursorIdx);
      std::string text;
      for (int i = from; i <= to; ++i) {
        if (!text.empty()) text += ' ';
        text += words[i].text;
      }
      ActivityResult result;
      result.data = ClippingResult{std::move(text)};
      setResult(std::move(result));
      finish();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (startMarkIdx != -1) {
      startMarkIdx = -1;
      requestUpdate();
    } else {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
  }
}

void ClipSelectionActivity::render(RenderLock&&) {
  if (!savedBuffer) return;

  if (needsPageSwitch) {
    if (!switchToPage(words[cursorIdx].pageIdx)) {
      return;
    }
    needsPageSwitch = false;
  }

  // Restore the saved page framebuffer, then draw highlights on top
  memcpy(renderer.getFrameBuffer(), savedBuffer, savedBufferSize);
  drawHighlights();

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), startMarkIdx == -1 ? tr(STR_SELECT) : tr(STR_DONE),
                                            tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

bool ClipSelectionActivity::switchToPage(int pageIdx) {
  const int oldPage = section.currentPage;
  section.currentPage = startPageInSection + pageIdx;
  auto page = section.loadPageFromSectionFile();
  if (!page) {
    section.currentPage = oldPage;
    LOG_ERR("CLIP", "Failed to load page %d (section.currentPage=%d, currentDisplayPage=%d) — reverted", pageIdx,
            section.currentPage, currentDisplayPage);
    return false;
  }

  renderer.clearScreen();
  page->render(renderer, fontId, marginLeft, marginTop);
  // displayBuffer is intentionally omitted here — render() always controls the final display call
  memcpy(savedBuffer, renderer.getFrameBuffer(), savedBufferSize);
  currentDisplayPage = pageIdx;
  return true;
}

void ClipSelectionActivity::drawHighlights() {
  // Draw selection range (words on the currently displayed page only)
  if (startMarkIdx != -1) {
    const int from = std::min(startMarkIdx, cursorIdx);
    const int to = std::max(startMarkIdx, cursorIdx);
    for (int i = from; i <= to; ++i) {
      if (i == cursorIdx) continue;
      if (words[i].pageIdx != currentDisplayPage) continue;
      const auto r = alignedRect(words[i].x, words[i].y, words[i].w, words[i].h);
      renderer.fillRectDither(r.x, r.y, r.w, r.h, Color::LightGray);
      renderer.drawText(fontId, words[i].x, words[i].y, words[i].text.c_str(), true);
    }
  }

  // Draw cursor highlight (always on top)
  const auto& cw = words[cursorIdx];
  if (cw.pageIdx == currentDisplayPage) {
    const auto r = alignedRect(cw.x, cw.y, cw.w, cw.h);
    renderer.fillRectDither(r.x, r.y, r.w, r.h, Color::LightGray);
    renderer.drawText(fontId, cw.x, cw.y, cw.text.c_str(), true);
  }
}

ClipSelectionActivity::Rect ClipSelectionActivity::alignedRect(int x, int y, int w, int h) const {
  const int alignedX = (x / 8) * 8;
  const int alignedW = ((x + w + 7) / 8) * 8 - alignedX;
  return {alignedX, y, alignedW, h};
}

int ClipSelectionActivity::lineEndForward(int idx) const {
  const int total = static_cast<int>(words.size());
  const int lineY = words[idx].y;
  const int page = words[idx].pageIdx;

  // Find last word on the same line
  int last = idx;
  for (int i = idx + 1; i < total; ++i) {
    if (words[i].pageIdx != page || words[i].y != lineY) break;
    last = i;
  }

  // Already at line end — jump to first word of next line
  if (last == idx && idx + 1 < total) {
    return idx + 1;
  }

  return last;
}

int ClipSelectionActivity::lineEndBackward(int idx) const {
  const int lineY = words[idx].y;
  const int page = words[idx].pageIdx;

  // Find first word on the same line
  int first = idx;
  for (int i = idx - 1; i >= 0; --i) {
    if (words[i].pageIdx != page || words[i].y != lineY) break;
    first = i;
  }

  // Already at line start — jump to last word of previous line
  if (first == idx && idx - 1 >= 0) {
    return idx - 1;
  }

  return first;
}

int ClipSelectionActivity::adjacentLineIndex(int idx, int direction) const {
  if (direction == 0 || idx < 0 || idx >= static_cast<int>(words.size())) {
    return idx;
  }

  const auto& current = words[idx];
  const int currentPage = current.pageIdx;
  const int currentY = current.y;
  const int targetX = current.x + current.w / 2;
  const int total = static_cast<int>(words.size());

  int candidateStart = -1;
  int candidateEnd = -1;
  int candidatePage = currentPage;
  int candidateY = currentY;

  if (direction > 0) {
    for (int i = idx + 1; i < total; ++i) {
      if (words[i].pageIdx == currentPage && words[i].y == currentY) {
        continue;
      }
      candidateStart = i;
      candidatePage = words[i].pageIdx;
      candidateY = words[i].y;
      candidateEnd = i;
      for (int j = i + 1; j < total; ++j) {
        if (words[j].pageIdx != candidatePage || words[j].y != candidateY) {
          break;
        }
        candidateEnd = j;
      }
      break;
    }
  } else {
    for (int i = idx - 1; i >= 0; --i) {
      if (words[i].pageIdx == currentPage && words[i].y == currentY) {
        continue;
      }
      candidateEnd = i;
      candidatePage = words[i].pageIdx;
      candidateY = words[i].y;
      candidateStart = i;
      for (int j = i - 1; j >= 0; --j) {
        if (words[j].pageIdx != candidatePage || words[j].y != candidateY) {
          break;
        }
        candidateStart = j;
      }
      break;
    }
  }

  if (candidateStart == -1 || candidateEnd == -1) {
    return idx;
  }

  int bestIdx = candidateStart;
  int bestDistance = std::abs((words[candidateStart].x + words[candidateStart].w / 2) - targetX);
  for (int i = candidateStart + 1; i <= candidateEnd; ++i) {
    const int wordCenter = words[i].x + words[i].w / 2;
    const int distance = std::abs(wordCenter - targetX);
    if (distance < bestDistance) {
      bestDistance = distance;
      bestIdx = i;
    }
  }

  return bestIdx;
}

void ClipSelectionActivity::moveCursorToIndex(int idx) {
  if (idx < 0 || idx >= static_cast<int>(words.size()) || idx == cursorIdx) {
    return;
  }

  const int prevPage = words[cursorIdx].pageIdx;
  cursorIdx = idx;
  if (words[cursorIdx].pageIdx != prevPage) {
    needsPageSwitch = true;
  }
  requestUpdate();
}
