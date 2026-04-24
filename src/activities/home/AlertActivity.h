#pragma once
#include "../Activity.h"

class AlertActivity final : public Activity {
  std::string title;
  std::string body;

 public:
  explicit AlertActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Alert", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
