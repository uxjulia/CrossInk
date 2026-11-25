#pragma once
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/TouchActionButtons.h"
#include "util/ButtonNavigator.h"

struct DetailsSpec {
  std::string header, title, author, source, metadata, description, actionLabel;
  bool descriptionHtml = false;
};

// Only the current page is wrapped; long descriptions do not retain every line.
class DetailsActivity final : public Activity {
 public:
  DetailsActivity(GfxRenderer& renderer, MappedInputManager& input, DetailsSpec spec);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
#ifdef SIMULATOR
  size_t simulatorPage() const { return page; }
  const std::string& simulatorFirstLine() const { return lines.front(); }
  size_t simulatorPageEnd() const { return nextOffset; }
  const std::string& simulatorText() const { return text; }
#endif
 private:
  DetailsSpec spec;
  std::string text;
  std::vector<std::string> lines;
  std::vector<size_t> starts;
  size_t page = 0, nextOffset = 0;
  int width = 0, height = 0, lineHeight = 0;
  Rect header, body;
  TouchActionButtons::Layout actions;
  GfxRenderer::Orientation orientation = GfxRenderer::Orientation::Portrait;
  ButtonNavigator navigator;
  void layout();
  void turn(int direction);
  void confirm();
};
