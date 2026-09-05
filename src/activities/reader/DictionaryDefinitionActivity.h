#pragma once
#include <EpdFontFamily.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/DictLayout.h"
#include "util/DictionaryLookupController.h"
#include "util/IpaUtils.h"
#include "util/LookupChain.h"
#include "util/LookupHistory.h"
#include "util/WordSelectNavigator.h"

class DictionaryDefinitionActivity final : public Activity {
 public:
  using BackgroundRenderFn = void (*)(void* context);

  // showLookupButton=true:
  //   Confirm = enter word-select mode on the definition text (Look Up Word).
  //   Back (short press) = return to caller (isCancelled=true).
  //   Touch Back gesture = exit the lookup flow and return to the reader.
  //   Back (long press, >= LONG_PRESS_MS) = Done — exit to reader (isCancelled=false).
  // showLookupButton=false:
  //   Back/Confirm both return to caller (isCancelled=true). Unchanged from old behaviour.
  explicit DictionaryDefinitionActivity(
      GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& headword, const DictLocation& location,
      bool showLookupButton = false, std::string bookCachePath = "", bool recordHistory = false,
      std::string historyWord = "", LookupHistory::Status historyStatus = LookupHistory::Status::NotFound,
      void* backgroundContext = nullptr, BackgroundRenderFn backgroundRender = nullptr,
      const char* dictionaryFontFamilyName = nullptr, uint8_t dictionaryFontPointSize = 0,
      bool modalBackgroundAlreadyPrepared = false, const DictionaryClippingRequest* clippingRequest = nullptr,
      WordSelectNavigator::HighlightSnapshotStorage* sharedHighlightSnapshotStorage = nullptr)
      : Activity("DictionaryDefinition", renderer, mappedInput),
        headword(headword),
        foundLocation(location),
        showLookupButton(showLookupButton),
        cachePath(std::move(bookCachePath)),
        recordHistory(recordHistory),
        historyWord(std::move(historyWord)),
        historyStatus(historyStatus),
        backgroundContext_(backgroundContext),
        backgroundRender_(backgroundRender),
        dictionaryFontFamilyName_(dictionaryFontFamilyName),
        dictionaryFontPointSize_(dictionaryFontPointSize),
        skipInitialModalBackgroundRedraw_(modalBackgroundAlreadyPrepared),
        hasClippingRequest_(clippingRequest != nullptr),
        clippingRequest_(clippingRequest ? *clippingRequest : DictionaryClippingRequest{}),
        hasSharedHighlightSnapshotStorage_(sharedHighlightSnapshotStorage != nullptr),
        controller(renderer, mappedInput, *this, cachePath) {
    navigator.setHighlightSnapshotStorage(sharedHighlightSnapshotStorage);
    controller.setLookupToastEnabled(!backgroundRender);
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string headword;
  DictLocation foundLocation;
  bool showLookupButton;
  std::string cachePath;
  bool recordHistory;
  std::string historyWord;
  LookupHistory::Status historyStatus;
  // Cross-definition back-navigation stack (compact: history-index + page per
  // entry, not owned strings). pendingBack_ carries the popped entry from the
  // Back keypress to the async FoundDefinition that completes the re-lookup.
  LookupChain chain_;
  LookupChain::Entry pendingBack_{};
  bool chainBackNavInProgress = false;
  bool dictionarySwitchLookupInProgress = false;
  bool exitAllOnBackRelease_ = false;
  // The reader activity beneath this modal owns the page and outlives us. A
  // small callback lets it redraw that page without retaining a second 48 KB
  // framebuffer while dictionary parsing is active.
  void* backgroundContext_ = nullptr;
  BackgroundRenderFn backgroundRender_ = nullptr;
  // Non-owning pointer to EpubReaderActivity's fixed per-book settings.
  const char* dictionaryFontFamilyName_ = nullptr;
  // Zero keeps the dictionary at the reader's active physical point size.
  uint8_t dictionaryFontPointSize_ = 0;
  bool skipInitialModalBackgroundRedraw_ = false;
  bool hasClippingRequest_ = false;
  DictionaryClippingRequest clippingRequest_{};
  bool hasSharedHighlightSnapshotStorage_ = false;
  // The framebuffer retains the book pixels outside the opaque modal. Normal
  // page turns keep this false and redraw only the modal; screens and overlays
  // that replace unrelated pixels set it true to rebuild the book.
  bool modalBackgroundNeedsRedraw_ = true;
  int modalX_ = 0;
  int modalY_ = 0;
  int modalWidth_ = 0;
  int modalHeight_ = 0;
  // Chained definitions may grow this frame but never shrink it. Keeping old
  // modal pixels covered avoids a reader-background redraw and the associated
  // reader/dictionary SD-font swap for ordinary lookup chaining.
  int modalSessionHeight_ = 0;
  // Erasing a prior black modal border with FAST_REFRESH can leave that border
  // visible on e-ink. Consume this on the next complete modal paint; it costs
  // one byte of activity state and does not allocate another framebuffer.
  bool modalCleanRefreshNeeded_ = false;
  std::string dictionaryName_;

  // Resident page representation (Stage 2b-pool). Segments reference text by
  // {offset, len} into pagePool_ instead of owning a std::string each — the
  // Wrapper already merged same-style runs, so each segment is one pooled,
  // null-terminated entry (kerning preserved, valid for C-API drawText).
  struct PooledSegment {
    uint16_t offset = 0;  // into pagePool_
    uint16_t len = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    bool isIpa = false;
  };
  struct PooledLine {
    uint16_t firstSegment = 0;
    uint16_t segmentCount = 0;
    uint8_t indentLevel = 0;
    bool isListItem = false;
  };

  // layoutLines holds ONLY the current page's lines (Stage 2a streaming). render
  // and extractWordsFromLayout index it from 0; loadPage() refills it per turn.
  // pagePool_ backs all segment text for the resident page.
  std::vector<PooledLine> layoutLines;
  std::vector<PooledSegment> layoutSegments;
  std::string pagePool_;
  int currentPage = 0;
  int linesPerPage = 0;
  int totalPages = 0;
  uint32_t definitionOffset_ = 0;
  uint32_t definitionSize_ = 0;
  bool definitionIsHtml_ = false;
  bool definitionHtmlNeedsPlainFallback_ = false;
  bool definitionReadFailed_ = false;
  // Kept per lookup so a failed SD-font prewarm can use the matching built-in
  // reader font without changing the user's selected font setting.
  int definitionFontId_ = 0;
  enum class DefinitionFontSource { Dictionary, Reader, BuiltIn };
  DefinitionFontSource definitionFontSource_ = DefinitionFontSource::Reader;

  // SD-font layout needs advance widths before the .dict stream is opened for
  // wrapping. This fixed 1 KB buffer lives inside the heap-owned activity (not
  // on the small task stack) and matches the SD font's persistent-cache budget.
  static constexpr uint16_t kDefinitionAdvanceCodepointCapacity = 256;
  std::array<uint32_t, kDefinitionAdvanceCodepointCapacity> definitionAdvanceCodepoints_{};
  uint16_t definitionAdvanceCodepointCount_ = 0;
  uint8_t definitionAdvanceStyleMask_ = 0;
  bool definitionAdvanceCodepointsTruncated_ = false;
  // Reused RTL shaping scratch; BidiUtils caps input at 512 codepoints, so the
  // retained high-water capacity is about 2 KB in the worst UTF-8 case.
  std::string definitionAdvanceVisualText_;

  // Reused across page turns (3.1-A): avoids re-allocating the renderer object +
  // its parser/buffers on every loadPage. A value member is fine — the activity
  // is heap-allocated, so this lives on the heap. reset()+re-feed each turn (NOT
  // kept alive mid-parse; that is the won't-fixed 2c).
  DictHtmlRenderer htmlRenderer_;

  // Page-collector state (used by collectLineSink during a wrap pass): keep only
  // collectTargetPage_'s lines into layoutLines, counting all lines produced.
  int collectTargetPage_ = 0;
  int collectLineCount_ = 0;

  // Orientation-aware layout gutters (computed in wrapText, used in render and extractWordsFromLayout)
  int leftPadding = 20;
  int rightPadding = 20;
  int hintGutterHeight = 0;
  int contentX = 0;
  int hintGutterWidth = 0;
  int bodyStartY = 0;  // top of the text body (set in wrapText)

  // Word-select mode (activated by pressing Look Up Word in view mode)
  bool isWordSelectMode = false;
  bool wordSelectHintsVisible_ = false;
  WordSelectNavigator navigator;
  // History-launched definitions have no parent snapshot to borrow. Allocate
  // the same bounded storage only if the user enters definition word-select.
  std::unique_ptr<WordSelectNavigator::HighlightSnapshotStorage> ownedHighlightSnapshotStorage_;
  DictionaryLookupController controller;
#if CROSSINK_APP_CAP_TOUCH
  bool touchDragLookup_ = false;
  bool touchDictionaryLookupHandled_ = false;
#endif

  // Differential repaint state for in-definition word-select mode. Only consulted
  // when isWordSelectMode is true; reset on every view-mode render.
  enum class RenderMode { FullPage, Differential };
  RenderMode nextRenderMode_ = RenderMode::FullPage;
  int prevHighlightIdx_ = -1;

  bool skipLoopDelay() override { return controller.skipLoopDelay(); }

  bool enterWordSelectMode();
  void wrapText();
  // Re-parse the definition and lay out ONLY page `page` into layoutLines,
  // discarding other pages as they are produced; also recomputes totalPages.
  void loadPage(int page);
  void wrapHtml();
  void wrapPlain();
  void prepareDefinitionFontAdvances();
  void collectDefinitionAdvanceText(const char* text, EpdFontFamily::Style style);
  static void collectSpanForAdvances(void* ctx, const StyledSpan& span);
  void extractWordsFromLayout();
  void openDictionarySwitch();
#if CROSSINK_APP_CAP_TOUCH
  bool showTouchDictionarySwitch() const;
  bool dictionarySwitchButtonContains(int x, int y) const;
  bool dictionaryCreateClippingButtonContains(int x, int y) const;
  bool modalContains(int x, int y) const;
#endif
  int dictionaryFooterHeight() const;
  bool hasModalBackground() const { return backgroundContext_ && backgroundRender_; }
  void sizeModalForCurrentPage();
  void drawModalFrame() const;
  int getMixedWidth(std::vector<IpaTextSpan>& ipaRuns, const char* text, EpdFontFamily::Style style);
  bool definitionTextNeedsApproximation(const char* text) const;
  std::string approximateDefinitionText(const char* text, bool inEtymologyTree) const;
  bool shouldApproximateDefinitionCodepoint(uint32_t cp) const;
  // Width measurement adapter injected into DictLayout::wrapSpans. ctx is `this`.
  static int measureWidthAdapter(void* ctx, const char* text, EpdFontFamily::Style style, bool isIpa);
  // Line sink injected into DictLayout::wrapSpans: keeps collectTargetPage_'s
  // lines, counts the rest. ctx is `this`.
  static void collectLineSink(void* ctx, const DictLayout::LayoutLineView& line);
  // Span sink bridge: sanitizes and forwards each streamed span into the DictLayout::Wrapper.
  static void feedSpanToWrapper(void* ctx, const StyledSpan& span);
  bool handleLongPressExitAll(bool enabled);
#if CROSSINK_APP_CAP_TOUCH
  bool handleTouchDictionaryLookup();
#endif
  int getDefinitionFontId(bool isIpa = false) const;
  void useBuiltInDefinitionFontFallback();
  void reflowForDefinitionFontChange();
  void redrawModalBackground();
  void displayModalBuffer();
  int getLineHeight() const;
};
