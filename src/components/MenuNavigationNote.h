#pragma once

#include "components/OptionPopup.h"

// Both Settings pickers retain these fragments while OptionPopup borrows them.
// Build them once on opening instead of allocating text on every highlight move.
class MenuNavigationNote {
 public:
  void apply(OptionPopup& popup) {
    frontLabel = std::string(tr(STR_FRONT_BUTTONS)) + ":";
    sideLabel = std::string(tr(STR_SIDE_BUTTONS)) + ":";
    upDown = std::string(tr(STR_DIR_UP)) + "/" + tr(STR_DIR_DOWN);
    leftRight = std::string(tr(STR_DIR_LEFT)) + "/" + tr(STR_DIR_RIGHT);
    popup.setOptionNotes({
        {frontLabel.c_str(), upDown.c_str(), sideLabel.c_str(), leftRight.c_str()},
        {frontLabel.c_str(), upDown.c_str(), sideLabel.c_str(), upDown.c_str()},
    });
  }

 private:
  std::string frontLabel;
  std::string sideLabel;
  std::string upDown;
  std::string leftRight;
};
