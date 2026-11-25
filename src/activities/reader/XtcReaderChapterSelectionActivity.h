#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <Xtc.h>

#include <array>
#include <atomic>
#include <memory>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class XtcReaderChapterSelectionActivity final : public Activity {
  using UiApp = freeink::ui::FreeInkApp<20, 4>;

  std::shared_ptr<Xtc> xtc;
  ButtonNavigator buttonNavigator;
  bool ignoreInitialUpRelease = false;
  bool ignoreInitialDownRelease = false;
  uint32_t currentPage = 0;
  int selectorIndex = 0;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  int topIndex = 0;
  bool initialViewportPending = true;
  static constexpr size_t CHAPTER_WINDOW_SIZE = 20;
  std::array<xtc::ChapterInfo, CHAPTER_WINDOW_SIZE> chapterWindow{};
  std::array<freeink::ui::ListItem, CHAPTER_WINDOW_SIZE> itemWindow{};

  static void chapterScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildChapterScreen(UiApp::ScreenType& screen);
  void selectChapter();

  int findChapterIndexForPage(uint32_t page) const;

 public:
  XtcReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                    const std::shared_ptr<Xtc>& xtc, uint32_t currentPage);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
