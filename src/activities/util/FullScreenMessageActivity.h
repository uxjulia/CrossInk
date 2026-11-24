#pragma once

#include "activities/Activity.h"

class FullScreenMessageActivity final : public Activity {
  StrId title;
  StrId message;

 public:
  FullScreenMessageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, StrId title, StrId message)
      : Activity("FullScreenMessage", renderer, mappedInput), title(title), message(message) {}
  void onEnter() override;
};
