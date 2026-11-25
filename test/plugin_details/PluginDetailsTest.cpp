#include <gtest/gtest.h>

#include "util/HtmlToPlainText.h"
TEST(PluginDetails, KeepsParagraphsAndDecodesEntities) {
  EXPECT_EQ(htmlToPlainText("<p>Caf&eacute; &amp; books</p><p>Second &#x1F4DA;</p>", ' '), "Café & books\n\nSecond 📚");
}
TEST(PluginDetails, PreservesPlainComparisons) { EXPECT_EQ(htmlToPlainText("1 < 2 & 3 > 1", ' '), "1 < 2 & 3 > 1"); }
TEST(PluginDetails, HeadingDoesNotJoinFollowingParagraph) {
  EXPECT_EQ(htmlToPlainText("<h2>Title</h2><p>Body<br>Next</p>", ' '), " Title\n\nBody\nNext");
}
TEST(PluginDetails, InvalidNumericEntitiesDoNotProduceInvalidUnicode) {
  EXPECT_EQ(htmlToPlainText("&#x110000; &#xD800;", ' '), "&#x110000;");
}
