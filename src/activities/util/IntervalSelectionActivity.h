#pragma once

#include <AppCapabilities.h>
#include <I18n.h>

#include <cstddef>
#include <memory>

#if CROSSINK_APP_CAP_TOUCH
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#endif

#include "IntervalSelectionLayout.h"
#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "activities/reader/TouchReaderPreviewModel.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

class GfxRenderer;

class IntervalSelectionActivity final : public Activity {
 public:
#ifdef SIMULATOR
  friend struct SliderOrientationSmokeTest;
#endif
  using ValueFormatter = void (*)(int value, char* buf, size_t len);
  enum class ReaderPreviewSetting : uint8_t {
    None,
    LineSpacing,
    WordSpacing,
    CharacterSpacing,
    VerticalMargin,
    HorizontalMargin
  };

  explicit IntervalSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* activityName,
                                     StrId titleId, int initialValue, int minValue, int maxValue, int smallStep,
                                     int largeStep, StrId valueFormatId = StrId::STR_NONE_OPT,
                                     bool readerActivity = false, bool allowPowerAsConfirm = false,
                                     bool ignoreInitialConfirmRelease = false, bool showPercentValue = false,
                                     StrId maxBoundaryLabelId = StrId::STR_NONE_OPT,
                                     bool overrideDisabledReaderTouchscreen = false,
                                     bool showTouchHeaderBackButton = false, ValueFormatter valueFormatter = nullptr,
                                     int tapStep = 0, bool useReaderSlider = false,
                                     ReaderPreviewSetting readerPreviewSetting = ReaderPreviewSetting::None);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return readerActivity; }
  bool allowPowerAsConfirmInReaderMode() const override { return allowPowerAsConfirm; }

 private:
  StrId titleId;
  StrId valueFormatId;
  StrId maxBoundaryLabelId;
  int value;
  int minValue;
  int maxValue;
  int smallStep;
  int largeStep;
  bool readerActivity;
  bool allowPowerAsConfirm;
  bool ignoreConfirmRelease;
  bool ignoreBackRelease = false;
  bool ignorePowerRelease = false;
  bool showPercentValue;
  bool overrideDisabledReaderTouchscreen;
  bool showTouchHeaderBackButton;
  ValueFormatter valueFormatter;
  int tapStep;
  bool useReaderSlider;
  ReaderPreviewSetting readerPreviewSetting;
  std::unique_ptr<SampleReaderPreviewModel> previewModel;
  Rect readerPreviewArea{};
  bool draggingBar = false;
  ButtonNavigator buttonNavigator;

#if CROSSINK_APP_CAP_TOUCH
  using UiApp = freeink::ui::FreeInkApp<8, 3>;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  std::atomic<bool> uiReady{false};
  bool sliderTapPending = false;
  bool draggingSlider = false;

  static constexpr freeink::ui::ActionId ACTION_SLIDER = 1;
  static constexpr freeink::ui::ActionId ACTION_STEP = 2;

  static void sliderScreen(UiApp::ScreenType& screen, void* user);
  static void onSliderEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onStepEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildSliderScreen(UiApp::ScreenType& screen);
  void formatEndpoint(int endpoint, char* buf, size_t len) const;
#endif
  Rect pickerHeaderRect() const;
  Rect legacyBarRect() const;
  IntervalSelectionLayout legacyLayout() const;
  bool usesReaderSlider() const;
  void formatValue(char* buf, size_t len) const;

  void adjustValue(int delta);
  int clampedValue(int candidate) const;
  int tappedValue(int candidate) const;
  bool usesTextTouchStepControls() const;
  void drawStepHintLine(int y, StrId labelId, int step);
  void renderReaderPreview(const Rect& area);
};
