#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIIcon.h>

#include "MappedInputManager.h"
#include "components/icons/listIcons.h"

// App policy for the SDK's reusable reveal control. Call under RenderLock,
// including routing: a gesture must refer to the rows currently on screen.
class SwipeDeleteRow {
 public:
  int index = -1;

  bool close() {
    const bool open = index >= 0;
    index = -1;
    return open;
  }

  template <typename App>
  bool swipe(App& app, MappedInputManager::SwipeDir direction, int x, int y, freeink::ui::ActionId rowAction) {
    if (direction == MappedInputManager::SwipeDir::None) return false;
    close();
    if (direction == MappedInputManager::SwipeDir::Left) {
      freeink::ui::Interaction hit;
      if (app.hitPublished(x, y, rowAction, hit)) index = hit.value;
    }
    // Release an in-flight press off-target; a swipe must never activate a row.
    freeink::ui::InputSnapshot cancel{};
    cancel.touchReleased = true;
    cancel.touchX = cancel.touchY = -1;
    app.route(cancel);
    app.clearTapFlash();
    return direction == MappedInputManager::SwipeDir::Left || direction == MappedInputManager::SwipeDir::Right;
  }

  template <typename App>
  bool dismissTap(App& app, const freeink::ui::InputSnapshot& snap, freeink::ui::ActionId deleteAction) {
    if (index < 0 || !snap.touchReleased || snap.touchX < 0 || snap.touchY < 0) return false;
    freeink::ui::Interaction hit;
    if (app.hitPublished(snap.touchX, snap.touchY, deleteAction, hit) && hit.value == index) return false;
    close();
    auto cancel = snap;
    cancel.touchX = cancel.touchY = -1;
    app.route(cancel);
    app.clearTapFlash();
    return true;
  }

  freeink::ui::ListRevealAction reveal(freeink::ui::ActionId action) const {
    freeink::ui::ListRevealAction result;
    result.index = static_cast<int16_t>(index);
    result.action = action;
    result.icon = freeink::ui::bitmapFromIcon(icon_trash_2_24);
    result.width = 64;
    return result;
  }
};
