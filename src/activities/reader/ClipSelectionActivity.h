#pragma once

#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Memory.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "WordRef.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct ClipWordStyle {
  enum Flags : uint8_t {
    NONE = 0,
    FILL = 1 << 0,
    BORDER = 1 << 1,
  };

  uint8_t flags = FILL;
};

class ClipSelectionActivity final : public Activity {
 public:
  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, ClipWordStore wordStore, int fontId,
                        Section& section, int startPageInSection, int marginTop, int marginLeft,
                        const DictionaryClippingRequest* dictionaryRequest = nullptr,
                        bool ignoreInitialBackRelease = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
  bool handleHomeGesture() override;

 private:
  static constexpr size_t BUFFER_CHUNK_SIZE = 4096;
  static constexpr size_t MAX_SAVED_BUFFER_CHUNKS = 16;
  static constexpr size_t MAX_READING_ORDER_WORDS = 240;

  ClipWordStore wordStore;
  int renderFontId = 0;
  Section& section;
  int startPageInSection = 0;
  int marginTop = 0;
  int marginLeft = 0;

  std::array<std::unique_ptr<uint8_t[]>, MAX_SAVED_BUFFER_CHUNKS> savedBufferChunks;
  size_t savedBufferChunkCount = 0;
  size_t savedBufferSize = 0;
  int currentDisplayPage = 0;
  int savedSectionPage = 0;

  int cursorIdx = 0;
  int startMarkIdx = -1;
  bool needsPageSwitch = false;
  bool hasSavedBuffer = false;
  bool usingFallbackFont = false;
  bool touchDragSelecting = false;
  bool touchDragHasMoved = false;
  int touchDragStartX = 0;
  int touchDragStartY = 0;
  int touchDragPageEndIdx = -1;
  uint32_t touchDragPageEndHeldSince = 0;
  bool hasDictionaryRequest = false;
  bool ignoreInitialConfirmRelease = false;
  bool ignoreInitialPowerRelease = false;
  bool ignoreInitialBackRelease = false;
  DictionaryClippingRequest dictionaryRequest{};
  std::array<uint16_t, MAX_READING_ORDER_WORDS> readingOrder{};
  size_t readingOrderSize = 0;

  ButtonNavigator buttonNavigator;

  void buildReadingOrder();
  void positionCursorAtInitialPageCenter();
  void resetSavedBufferChunks();
  void allocateSavedBuffer();
  void storeCurrentBuffer();
  void restoreSavedBuffer() const;
  bool switchToPage(int pageIdx);
  void drawHighlights();
  void applyWordStyle(const WordRef& word, const ClipWordStyle& style) const;
  void useFallbackFont(const char* reason);
  bool selectWordAtPoint(int x, int y);
  bool isWithinCurrentPageEndDwellSlop(int x, int y) const;
  uint16_t tableSelectionForRange(int from, int to) const;
  void confirmSelection();
  bool finishDictionarySelection();
  int lineEndForward(int orderIdx) const;
  int lineEndBackward(int orderIdx) const;
};
