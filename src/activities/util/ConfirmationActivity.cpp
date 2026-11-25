#include "ConfirmationActivity.h"

#include <I18n.h>

#include "components/UITheme.h"
#include "fontIds.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body,
                                           bool ignoreInitialConfirmRelease, bool overrideDisabledReaderTouchscreen)
    : Activity("Confirmation", renderer, mappedInput),
      ignoreConfirmRelease(ignoreInitialConfirmRelease),
      overrideDisabledReaderTouchscreen(overrideDisabledReaderTouchscreen) {
  popupTitle.reserve(heading.size() + body.size() + 1);
  popupTitle = heading;
  if (!heading.empty() && !body.empty()) {
    popupTitle += ' ';
  }
  popupTitle += body;
}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();
  if (overrideDisabledReaderTouchscreen) {
    mappedInput.setReaderTouchscreenOverride(true);
  }

  const char* options[] = {customCancelLabel.empty() ? I18N.get(cancelLabel) : customCancelLabel.c_str(),
                           customConfirmLabel.empty() ? I18N.get(confirmLabel) : customConfirmLabel.c_str()};
  confirmPopup.show(popupTitle.c_str(), options, 2, 0, [this](int idx) {
    ActivityResult res;
    res.isCancelled = (idx != 1);
    setResult(std::move(res));
    finish();
  });
  confirmPopup.setPrimaryOptionIndex(1);

  requestUpdate(true);
}

void ConfirmationActivity::onExit() {
  if (overrideDisabledReaderTouchscreen) {
    mappedInput.setReaderTouchscreenOverride(false);
  }
  Activity::onExit();
}

void ConfirmationActivity::render(RenderLock&&) {
  if (confirmPopup.processRender(renderer, mappedInput)) return;
}

void ConfirmationActivity::loop() {
  if (ignoreConfirmRelease) {
    const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    if (confirmReleased || !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
      return;
    }
  }

  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // Popup dismissed without a selection (Back button or tap outside): cancel.
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}
