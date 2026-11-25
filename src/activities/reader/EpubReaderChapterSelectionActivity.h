#pragma once
#include <Epub.h>
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <array>
#include <atomic>
#include <memory>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderChapterSelectionActivity final : public Activity {
  using UiApp = freeink::ui::FreeInkApp<20, 4>;
  static constexpr size_t CHAPTER_WINDOW_SIZE = 20;

  std::shared_ptr<Epub> epub;
  std::string epubPath;
  ButtonNavigator buttonNavigator;
  bool ignoreInitialUpRelease = false;
  bool ignoreInitialDownRelease = false;
  int currentTocIndex = 0;
  int selectorIndex = 0;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;
  bool initialViewportPending = true;
  std::array<std::string, CHAPTER_WINDOW_SIZE> labelWindow{};
  std::array<freeink::ui::ListItem, CHAPTER_WINDOW_SIZE> itemWindow{};

  static void chapterScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildChapterScreen(UiApp::ScreenType& screen);
  void selectChapter();

  // Total TOC items count
  int getTotalItems() const;

 public:
  EpubReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                                     int currentTocIndex);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
