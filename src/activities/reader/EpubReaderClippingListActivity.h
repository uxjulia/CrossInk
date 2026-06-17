#pragma once

#include <string>
#include <vector>

#include "ClippingStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderClippingListActivity final : public Activity {
 public:
  EpubReaderClippingListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 std::vector<Clipping> clippings)
      : Activity("EpubClippingList", renderer, mappedInput), clippings(std::move(clippings)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::vector<Clipping> clippings;
  std::string detailText;
  std::vector<std::string> detailLines;
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  int detailPage = 0;
  int detailLayoutWidth = 0;
  int detailLinesPerPage = 0;
  bool longPressConfirmHandled = false;
  bool detailMode = false;

  int getPageItems() const;
  int getDetailTextWidth() const;
  int getDetailLinesPerPage() const;
  int getDetailPageCount() const;
  void deleteSelectedClipping();
  void closeDetail();
  void jumpToSelectedClipping();
  void openSelectedDetail();
  void rebuildDetailLayoutIfNeeded();
  void showClippingActionMenu(bool ignoreInitialConfirmRelease);
  void renderDetail();
};
