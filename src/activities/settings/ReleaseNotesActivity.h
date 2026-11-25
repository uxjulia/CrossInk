#pragma once

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <I18nKeys.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ReleaseNotes.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

// Shows ReleaseNotes.cpp's table. FirstRun is pushed once over Home after an update;
// FromSettings is opened from Settings > System > Release Notes.
class ReleaseNotesActivity final : public Activity {
 public:
  enum class Mode : uint8_t { FirstRun, FromSettings };
  // Result action returned to Settings when Download is chosen from Settings.
  static constexpr int RESULT_OPEN_LANGUAGE_DOWNLOADS = 1;

  ReleaseNotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Mode mode);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Vertical swipes page the notes, including swipes that start on the bottom actions.
  bool allowGlobalHomeSwipeGesture() const override { return false; }

#ifdef SIMULATOR
  bool simulatorUpdateOffered() const { return updateOffered; }
  int simulatorPageCount() const { return static_cast<int>(pageStarts.size()); }
  void simulatorSetPage(int value) { page = value; }
  void simulatorConfirm() { onPrimaryAction(); }
  int simulatorPage() const { return page; }
  int simulatorLayoutWidth() const { return layoutWidth; }
  int simulatorLayoutHeight() const { return layoutHeight; }
  Rect simulatorBodyRect() const { return Rect{contentX, bodyTop, contentWidth, bodyBottom - bodyTop}; }
  Rect simulatorHeaderRect() const { return headerRect(); }
  Rect simulatorActionRect(int index) const;
#endif

 private:
  enum class LineKind : uint8_t { Title, Body, Path, Gap };
  struct Line {
    std::string text;
    LineKind kind;
    uint16_t height;
    bool startsItem;  // First line of a note item; pages prefer to break here
  };

  Mode mode;
  ButtonNavigator buttonNavigator;
  std::vector<Line> lines;
  std::vector<uint16_t> pageStarts;  // Index into lines of each page's first line
  int page = 0;
  int layoutWidth = 0;
  int layoutHeight = 0;
  bool layoutTouch = false;
  // Width and height alone miss a turn between the two landscapes, which moves the side button hints.
  GfxRenderer::Orientation layoutOrientation = GfxRenderer::Orientation::Portrait;
  int contentX = 0;
  int contentWidth = 0;
  int bodyTop = 0;
  int bodyBottom = 0;
  bool updateOffered = false;
  bool languageIndexValid = false;
  int languageIndex = 0;
  bool leavingForUpdate = false;
  bool seenMarked = false;
  StrId errorText = StrId::_COUNT;
  char primaryText[96] = {};  // "Install Languages" or "Update <language>", built in layout()

  void layout();
  void addWrapped(const char* text, LineKind kind, int fontId, int width, EpdFontFamily::Style style);
  void addGap(int height);
  void nextPage();
  void previousPage();
  void onPrimaryAction();
  void startUpdate();
  void markSeen();
  void close();
  Rect headerRect() const;
  bool usesBackHeader() const;
  const char* primaryLabel() const;
  int actionCount() const;
  void addItem(const char* title, const char* body, const StrId* path, uint8_t pathLength, const char* literal,
               int width);
};
