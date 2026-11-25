#pragma once
#include <I18n.h>

#include <functional>
#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

class ConfirmationActivity : public Activity {
 private:
  std::string popupTitle;
  OptionPopup confirmPopup;
  std::string customCancelLabel, customConfirmLabel;
  bool ignoreConfirmRelease = false;
  bool overrideDisabledReaderTouchscreen = false;
  StrId cancelLabel = StrId::STR_CANCEL;
  StrId confirmLabel = StrId::STR_CONFIRM;

 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body, bool ignoreInitialConfirmRelease = false,
                       bool overrideDisabledReaderTouchscreen = false);

  // Replaces the Cancel / Confirm option labels. Call before the activity starts.
  void setOptionLabels(StrId cancel, StrId confirm) {
    cancelLabel = cancel;
    confirmLabel = confirm;
  }

  void setOptionLabels(const std::string& cancel, const std::string& confirm) {
    customCancelLabel = cancel;
    customConfirmLabel = confirm;
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
