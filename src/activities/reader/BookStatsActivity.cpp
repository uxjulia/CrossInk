#include "BookStatsActivity.h"

#include "BookStatsView.h"
#include "MappedInputManager.h"

BookStatsActivity::BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                                     const BookReadingStats& stats, const GlobalReadingStats& globalStats)
    : Activity("BookStats", renderer, mappedInput), bookTitle(title), stats(stats), globalStats(globalStats) {}

void BookStatsActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void BookStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void BookStatsActivity::render(RenderLock&&) {
  renderBookStatsView(renderer, &mappedInput, bookTitle, stats, globalStats, true);
  renderer.displayBuffer();
}
