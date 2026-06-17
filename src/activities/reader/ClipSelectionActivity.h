#pragma once

#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Memory.h>

#include <array>
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
    INVERT = 1 << 1,
    UNDERLINE = 1 << 2,
    BORDER = 1 << 3,
  };

  uint8_t flags = FILL;
  Color fillColor = Color::LightGray;
};

class ClipSelectionActivity final : public Activity {
 public:
  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::vector<WordRef> words, int fontId,
                        Section& section, int startPageInSection, int marginTop, int marginLeft);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  static constexpr size_t BUFFER_CHUNK_SIZE = 4096;

  std::vector<WordRef> words;
  int fontId = 0;
  Section& section;
  int startPageInSection = 0;
  int marginTop = 0;
  int marginLeft = 0;

  std::vector<std::unique_ptr<uint8_t[]>> savedBufferChunks;
  size_t savedBufferSize = 0;
  int currentDisplayPage = 0;
  int savedSectionPage = 0;

  int cursorIdx = 0;
  int startMarkIdx = -1;
  bool needsPageSwitch = false;
  bool hasSavedBuffer = false;
  mutable std::array<std::string, 4> prewarmTextByStyle;

  ButtonNavigator buttonNavigator;

  bool allocateSavedBuffer();
  void storeCurrentBuffer();
  void restoreSavedBuffer() const;
  bool switchToPage(int pageIdx);
  void prewarmHighlightedWords() const;
  void drawHighlights();
  void applyWordStyle(const WordRef& word, const ClipWordStyle& style) const;
  int lineEndForward(int idx) const;
  int lineEndBackward(int idx) const;
};
