#include <gtest/gtest.h>

#include "src/activities/util/KeyboardTipsLayout.h"

TEST(KeyboardTipsTest, EveryModeFitsExactlyOrIsHidden) {
  for (bool cursor : {false, true})
    for (bool password : {false, true})
      for (bool panel : {false, true})
        for (bool symbols : {false, true})
          for (bool url : {false, true}) {
            const int rows = cursor ? 2 : panel ? 4 : symbols ? 3 : url ? 5 : 4;
            const int top = 100 + (cursor ? (password ? 2 : 1) * 16 : 0);
            const int keyboardTop = top + rows * 16;
            EXPECT_EQ(keyboardTipsY(100, keyboardTop, 16, cursor, password, panel, symbols, url), top);
            EXPECT_EQ(keyboardTipsY(100, keyboardTop - 1, 16, cursor, password, panel, symbols, url), -1);
            EXPECT_EQ(keyboardTipsY(100, keyboardTop + 20, 16, cursor, password, panel, symbols, url), top + 10);
          }
}

TEST(KeyboardTipsTest, InvalidOrNegativeSpaceHidesTips) {
  EXPECT_EQ(keyboardTipsY(100, 50, 16, false, false, false, false, false), -1);
  EXPECT_EQ(keyboardTipsY(100, 500, 0, false, false, false, false, false), -1);
}
