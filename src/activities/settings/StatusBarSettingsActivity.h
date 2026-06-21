#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Reader status bar configuration activity
class StatusBarSettingsActivity final : public Activity {
 public:
  explicit StatusBarSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool readerContext = false,
                                     bool stablePageNumbersAvailable = false)
      : Activity("StatusBarSettings", renderer, mappedInput),
        readerContext(readerContext),
        stablePageNumbersAvailable(stablePageNumbersAvailable) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;

  int selectedIndex = 0;
  int visibleItemCount = 0;
  bool readerContext = false;
  bool stablePageNumbersAvailable = false;

  int itemForVisibleIndex(int visibleIndex) const;
  bool selectedItemUsesOptionMenu() const;
  void handleSelection();
  void openOptionPicker();
};
