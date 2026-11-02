#pragma once

#if CROSSINK_SCALABLE_FONTS

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <I18n.h>

#include <atomic>
#include <string>
#include <vector>

#include "TtfRenderProfileStore.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "components/UiAppHelpers.h"
#include "util/ButtonNavigator.h"

class TtfRenderOptionsActivity final : public Activity {
 public:
  enum class Row : uint8_t { Hinting, Raster, Interpreter, Weight, Slant, StemDarkening, Reset };

  static const std::vector<std::string>* optionLabels(Row row);
  static uint8_t selectedOption(const TtfRenderProfile& profile, Row row);
  static void setOption(TtfRenderProfile& profile, Row row, int index);
  static StrId titleId(Row row);
  static const char* rowLabel(Row row);
  static const char* rowValue(Row row, const TtfRenderProfile& profile);

 private:
  std::string family_;
  TtfRenderProfile initialProfile_;
  TtfRenderProfile profile_;
  std::vector<Row> rows_;
  ButtonNavigator buttonNavigator_;
  OptionPopup optionPopup_;
  int selectedIndex_ = 0;
  int visibleRows_ = 1;
  int topIndex_ = 0;
  bool readerMode_ = false;
  bool changed_ = false;
  bool reloadHandled_ = false;

  using UiApp = freeink::ui::FreeInkApp<12, 4>;
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  freeink::ui::GfxRendererTarget uiTarget_;
  UiApp app_;
  std::atomic<bool> uiReady_{false};

  void rebuildRows();
  void activateSelected();
  void showOptions(Row row);
  bool save();
  void finishWithResult();
  static void optionsScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildOptionsScreen(UiApp::ScreenType& screen);

 public:
  TtfRenderOptionsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* family, bool readerMode)
      : Activity("TtfRenderOptions", renderer, mappedInput),
        family_(family ? family : ""),
        readerMode_(readerMode),
        uiTarget_(makeUiTarget(renderer)),
        app_(uiTarget_, uiTarget_.deviceContext()) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return readerMode_; }
  bool allowPowerAsConfirmInReaderMode() const override { return readerMode_; }
  bool allowGlobalHomeGesture() const override { return false; }
};

#endif
