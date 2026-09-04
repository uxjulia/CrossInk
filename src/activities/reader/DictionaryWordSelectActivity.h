#pragma once
#include <Epub/Page.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/DictionaryLookupController.h"
#include "util/WordSelectNavigator.h"

class DictionaryWordSelectActivity final : public Activity {
 public:
  using ReaderPageLoadFn = std::unique_ptr<Page> (*)(void* context, int pageOffset);

  // reservedBottomHeight is the post-bezel reserved space the caller (EpubReader)
  // left below the page text — status-bar height OR auto-page-turn indicator
  // height, per the caller's own layout formula. The skip-initial-render fast
  // path clears exactly that strip so the framebuffer matches the menu→lookup
  // path (no status bar, no auto-turn label visible during word-select).
  explicit DictionaryWordSelectActivity(
      GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page, int marginLeft, int marginTop,
      std::string cachePath, std::string nextPageFirstWord = "", bool hasNextReaderPage = false,
      bool framebufferContainsPage = false, int reservedBottomHeight = 0, int initialTouchX = -1,
      int initialTouchY = -1, bool autoLookupInitialWord = false, const char* dictionaryFontFamilyName = nullptr,
      uint8_t dictionaryFontPointSize = 0, void* readerContext = nullptr, ReaderPageLoadFn readerPageLoad = nullptr)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        cachePath(std::move(cachePath)),
        nextPageFirstWord(std::move(nextPageFirstWord)),
        hasNextReaderPage_(hasNextReaderPage),
        // DictionaryLookupController borrows this activity-owned path.  Qualify
        // the member so it cannot instead bind to the constructor parameter,
        // which is destroyed as soon as construction completes.
        controller(renderer, mappedInput, *this, this->cachePath, true),
        framebufferContainsPage_(framebufferContainsPage),
        reservedBottomHeight_(reservedBottomHeight),
        initialTouchX_(initialTouchX),
        initialTouchY_(initialTouchY),
        autoLookupInitialWord_(autoLookupInitialWord),
        dictionaryFontPointSize_(dictionaryFontPointSize),
        readerContext_(readerContext),
        readerPageLoad_(readerPageLoad) {
    if (dictionaryFontFamilyName) {
      std::strncpy(dictionaryFontFamilyName_, dictionaryFontFamilyName, sizeof(dictionaryFontFamilyName_) - 1);
    }
    navigator.setHighlightSnapshotStorage(&highlightSnapshotStorage_);
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::unique_ptr<Page> page;
  int marginLeft;
  int marginTop;
  std::string cachePath;
  std::string nextPageFirstWord;
  bool hasNextReaderPage_ = false;

  struct WorkingSet {
    std::unique_ptr<WordSelectNavigator::WordInfo[]> words;
    std::unique_ptr<WordSelectNavigator::Row[]> rows;
    std::unique_ptr<char[]> textPool;
    std::unique_ptr<char[]> measurementScratch;
    size_t wordCapacity = 0;
    size_t wordCount = 0;
    size_t rowCapacity = 0;
    size_t rowCount = 0;
    size_t textCapacity = 0;
    size_t textUsed = 0;
    size_t measurementScratchCapacity = 0;

    void clear() {
      words.reset();
      rows.reset();
      textPool.reset();
      measurementScratch.reset();
      wordCapacity = 0;
      wordCount = 0;
      rowCapacity = 0;
      rowCount = 0;
      textCapacity = 0;
      textUsed = 0;
      measurementScratchCapacity = 0;
    }
  };

  WorkingSet workingSet_;
  WordSelectNavigator navigator;
  // Shared with the stacked definition activity. It is fixed storage inside
  // this activity, so it adds no allocator churn and replaces two simultaneous
  // 4 KB snapshots with one.
  WordSelectNavigator::HighlightSnapshotStorage highlightSnapshotStorage_;
  DictionaryLookupController controller;

  // Differential repaint state. The first render in a session always goes through
  // the full path (page->render + snapshot setup). After that, cursor moves use the
  // differential path: restore previous highlight pixels, snapshot new region, draw
  // new highlight, push only the dirty rect to the panel. State is reset to FullPage
  // whenever the framebuffer is disturbed by something other than the highlight
  // (controller overlay, multi-select, hyphenated wrap).
  enum class RenderMode { FullPage, Differential };
  RenderMode nextRenderMode_ = RenderMode::FullPage;
  int prevHighlightIdx_ = -1;

  // One-shot opt-in from the caller: "the framebuffer already contains the
  // page at marginLeft/marginTop (e.g. EpubReader just rendered it before the
  // hold-to-lookup gesture)". When true, the first render() skips clearScreen
  // + page->render and overlays only the highlight + button hints. Consumed
  // (cleared) on the first render() call regardless of which branch is taken,
  // so any future full-repaint goes through the normal re-render path.
  bool framebufferContainsPage_ = false;

  // Bottom-of-screen reserved area the caller left below the page text
  // (status bar OR auto-page-turn indicator). Cleared in the skip-initial
  // fast path so the entry frame matches the menu→lookup visual state.
  int reservedBottomHeight_ = 0;
  bool ignoreInitialBackRelease_ = false;
  int initialTouchX_ = -1;
  int initialTouchY_ = -1;
  bool autoLookupInitialWord_ = false;
  // This child owns its fixed-size copy so the reader does not keep the
  // dictionary selection resident while laying out EPUB sections.
  char dictionaryFontFamilyName_[64] = "";
  uint8_t dictionaryFontPointSize_ = 0;
  bool touchDragLookup_ = false;
  bool touchDragHasMoved_ = false;
  int touchDragStartX_ = 0;
  int touchDragStartY_ = 0;
  void* readerContext_ = nullptr;
  ReaderPageLoadFn readerPageLoad_ = nullptr;
  int activePageOffset_ = 0;
  bool workingSetSuspended_ = false;
  bool workingSetMemoryError_ = false;
  DictionaryClippingRequest pendingClippingRequest_{};
  bool hasPendingClippingRequest_ = false;
  bool hasCrossPageSelection_ = false;
  DictionaryClippingRequest crossPageFirstRequest_{};
  std::string crossPageLookupPrefix_;
  size_t crossPageLookupWordCount_ = 0;
  std::string crossPageMergedFirstWord_;
  bool crossPageFirstWordWasMerged_ = false;
  int suspendedSelectionX_ = -1;
  int suspendedSelectionY_ = -1;

  bool skipLoopDelay() override { return controller.skipLoopDelay(); }

  // Force the next render() to take the full-repaint path. Used after the lookup controller
  // or a sub-activity (e.g. DictionaryDefinitionActivity) has drawn directly to the
  // framebuffer outside our render(), making the snapshot/dirty-rect state stale. Without
  // this reset, the differential path would restore a small region of "page bg + word text"
  // onto a framebuffer full of unrelated content, and the next push would show that
  // unrelated content with one word's worth of correct page state overlaid.
  void forceFullRepaintOnNextRender() {
    nextRenderMode_ = RenderMode::FullPage;
    prevHighlightIdx_ = -1;
  }

  // Batched bitmap-glyph prewarm for the word at currIdx, so the upcoming
  // drawText (inside renderHighlightDifferential / renderHighlight) doesn't
  // serialize cold-miss SD reads one character at a time. No-op for invalid
  // indices or missing FontCacheManager.
  void prewarmHighlightGlyphs(int currIdx);

  // Build SdCardFont's persistent advance table for every codepoint on the
  // current page, in one batched SD operation. After this, all subsequent
  // getTextAdvanceX calls take the in-RAM binary-search fast path instead
  // of the slow per-glyph fontMap fallback (~50ms each).
  void prebuildAdvanceTable();

  void clearFrontButtonHintArea();
  void openDictionarySwitch();
  void renderDefinitionBackground();
  static void renderDefinitionBackgroundCallback(void* context);
  bool buildWorkingSet(bool consumeInitialConfirm);
  void suspendWorkingSet();

  bool allocateWorkingSet();
  bool extractWords();
  bool mergeHyphenatedWords();
  bool captureCurrentClippingRequest(DictionaryClippingRequest& request) const;
  bool captureClippingRequest();
  bool continueTouchSelectionOnNextPage();
  bool selectFirstWordForTouchDrag();
  void finishTouchLookupOnCurrentPage(const std::string& phrase, size_t wordCount,
                                      const DictionaryClippingRequest& request);
  std::string finishTouchLookupPhrase();
  void resetCrossPageSelection();
  void finishWithClippingRequest();
  bool appendText(const char* text, size_t length, uint16_t& offset);
  bool appendMergedText(const char* first, size_t firstLength, const char* second, size_t secondLength,
                        uint16_t& offset);
  bool appendWord(WordSelectNavigator::WordInfo word);
};
