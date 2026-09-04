#include "FileBrowserActionActivity.h"

#include <I18n.h>

#include <algorithm>

FileBrowserActionActivity::FileBrowserActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     std::string title, std::vector<MenuItem> items,
                                                     const bool ignoreInitialConfirmRelease,
                                                     const bool ignoreOpeningTouchRelease)
    : Activity("FileBrowserAction", renderer, mappedInput),
      title(std::move(title)),
      items(std::move(items)),
      ignoreConfirmRelease(ignoreInitialConfirmRelease),
      ignoreOpeningTouchRelease(ignoreOpeningTouchRelease) {}

void FileBrowserActionActivity::onEnter() {
  Activity::onEnter();
  // Context menus are modal overlays: a tap on the dimmed image/list area
  // should close the menu instead of requiring the Back button.
  optionPopup.setDismissOnOutsideTouchDown(true);
  // List long-presses open this activity while the finger is still down. The
  // image viewer instead opens it after consuming a completed photo tap, so
  // it must accept the first deliberate option tap.
  if (ignoreOpeningTouchRelease) {
    int touchX = 0;
    int touchY = 0;
    ignoreTouchRelease = mappedInput.isScreenTouchHeld(touchX, touchY);
  }
  optionLabels.resize(items.size());
  std::transform(items.begin(), items.end(), optionLabels.begin(),
                 [](const MenuItem& item) { return std::string(I18N.get(item.labelId)); });
  optionPopup.show(title.c_str(), optionLabels, 0, [this](const int index) {
    if (index < 0 || index >= static_cast<int>(items.size())) return;
    selectionMade = true;
    setResult(FileBrowserActionResult{static_cast<int>(items[index].action)});
    finish();
  });
  requestUpdate();
}

void FileBrowserActionActivity::finishCancelled() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void FileBrowserActionActivity::loop() {
  if (ignoreTouchRelease) {
    // The caller may suppress the long-press tap before opening us. Clear that
    // one-shot suppression while consuming the opening contact, so it cannot
    // discard the user's first deliberate option tap.
    int touchX = 0;
    int touchY = 0;
    (void)mappedInput.wasScreenTapped(touchX, touchY);
    if (mappedInput.wasScreenTouchReleased()) {
      ignoreTouchRelease = false;
    }
    return;
  }

  if (ignoreConfirmRelease) {
    const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    if (confirmReleased || !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
      return;
    }
  }

  optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
  if (!optionPopup.isActive() && !selectionMade) finishCancelled();
}

void FileBrowserActionActivity::render(RenderLock&&) { optionPopup.processRender(renderer, mappedInput); }
