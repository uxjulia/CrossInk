#include "EpubReaderBookmarkListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

static constexpr int ROW_HEIGHT = 50;
static constexpr int LIST_START_Y = 60;

int EpubReaderBookmarkListActivity::getPageItems() const {
  const auto orientation = renderer.getOrientation();
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int startY = LIST_START_Y + hintGutterHeight;
  const int available = renderer.getScreenHeight() - startY - ROW_HEIGHT;
  return std::max(1, available / ROW_HEIGHT);
}

void EpubReaderBookmarkListActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void EpubReaderBookmarkListActivity::onExit() { Activity::onExit(); }

void EpubReaderBookmarkListActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!bookmarks.empty() && selectedIndex >= 0 && selectedIndex < static_cast<int>(bookmarks.size())) {
      setResult(BookmarkResult{bookmarks[selectedIndex].spineIndex, bookmarks[selectedIndex].progress});
      finish();
    }
    return;
  }

  const int total = static_cast<int>(bookmarks.size());
  const int pageItems = getPageItems();

  buttonNavigator.onNextRelease([this, total] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, total);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, total] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, total);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this, total, pageItems] {
    selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, total, pageItems);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this, total, pageItems] {
    selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, total, pageItems);
    requestUpdate();
  });
}

void EpubReaderBookmarkListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const bool isPortraitInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? 30 : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int hintGutterHeight = isPortraitInverted ? 50 : 0;
  const int contentY = hintGutterHeight;

  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_BOOKMARKS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, tr(STR_BOOKMARKS), true, EpdFontFamily::BOLD);

  if (bookmarks.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, LIST_START_Y + contentY + 20, tr(STR_NO_BOOKMARKS));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const int pageItems = getPageItems();
  const int total = static_cast<int>(bookmarks.size());
  const int pageStartIndex = (selectedIndex / pageItems) * pageItems;
  const int marginLeft = contentX + 20;

  for (int i = 0; i < pageItems; i++) {
    const int itemIndex = pageStartIndex + i;
    if (itemIndex >= total) break;

    const int rowY = LIST_START_Y + contentY + i * ROW_HEIGHT;
    const bool isSelected = (itemIndex == selectedIndex);

    if (isSelected) {
      renderer.fillRect(contentX, rowY, contentWidth - 1, ROW_HEIGHT, true);
    }

    const Bookmark& bm = bookmarks[itemIndex];
    const char* chapter = (bm.chapterTitle[0] != '\0') ? bm.chapterTitle : tr(STR_UNKNOWN_CHAPTER);
    const std::string chapterTrunc = renderer.truncatedText(UI_10_FONT_ID, chapter, contentWidth - 40);
    renderer.drawText(UI_10_FONT_ID, marginLeft, rowY + 6, chapterTrunc.c_str(), !isSelected);

    char pageBuf[24];
    snprintf(pageBuf, sizeof(pageBuf), "%d%%", static_cast<int>(bm.progress * 100.0f));
    renderer.drawText(SMALL_FONT_ID, marginLeft, rowY + 28, pageBuf, !isSelected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
