#pragma once
#include <string>

#include "../Activity.h"
#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

class BookStatsActivity final : public Activity {
  std::string bookTitle;
  BookReadingStats stats;
  GlobalReadingStats globalStats;

 public:
  BookStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title,
                    const BookReadingStats& stats, const GlobalReadingStats& globalStats);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
