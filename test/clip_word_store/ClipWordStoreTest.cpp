#include <gtest/gtest.h>

#include <string>
#include <type_traits>

#include "ClippingStore.h"
#include "Epub/Epub/ReaderRenderSpec.h"
#include "activities/reader/ClipSelectionPaging.h"
#include "activities/reader/WordRef.h"
#include "clippings/ClipTextBuilder.h"
#include "clippings/ClippingHighlightGeometry.h"
#include "clippings/ClippingMatchTracker.h"
#include "clippings/ClippingTextMatcher.h"

TEST(ClipWordStore, StoresNullTerminatedUtf8TextWithStableOffsets) {
  ClipWordStore store;
  WordRef first;
  WordRef second;

  ASSERT_TRUE(store.appendText(first, "first"));
  ASSERT_TRUE(store.appendText(second, "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D"));  // Hebrew UTF-8

  EXPECT_EQ(first.textOffset, 0);
  EXPECT_EQ(first.textLength, 5);
  EXPECT_STREQ(store.text(first), "first");
  EXPECT_STREQ(store.text(second), "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D");
  EXPECT_TRUE(std::is_trivially_copyable_v<WordRef>);
}

TEST(ClipWordStore, RejectsTextPastTheUint16PoolBoundary) {
  ClipWordStore store;
  WordRef longWord;
  const std::string text(ClipWordStore::MAX_TEXT_POOL_BYTES - 1, 'x');

  ASSERT_TRUE(store.appendText(longWord, text.c_str()));
  EXPECT_EQ(store.textPool.size(), ClipWordStore::MAX_TEXT_POOL_BYTES);

  WordRef overflow;
  EXPECT_FALSE(store.appendText(overflow, "x"));
}

TEST(ClipSelectionPaging, AdvancesOnlyAfterTheFinalWordOfTheCurrentPage) {
  std::vector<WordRef> words(5);
  words[0].pageIdx = 0;
  words[1].pageIdx = 0;
  words[2].pageIdx = 1;
  words[3].pageIdx = 1;
  words[4].pageIdx = 2;
  const uint16_t order[] = {0, 1, 2, 3, 4};

  EXPECT_EQ(ClipSelectionPaging::nextPageStartIndex(words, order, std::size(order), 0), -1);
  EXPECT_EQ(ClipSelectionPaging::nextPageStartIndex(words, order, std::size(order), 1), 2);
  EXPECT_EQ(ClipSelectionPaging::nextPageStartIndex(words, order, std::size(order), 3), 4);
  EXPECT_EQ(ClipSelectionPaging::nextPageStartIndex(words, order, std::size(order), 4), -1);
}

TEST(ClipSelectionPaging, RequiresARealDragBeforePaging) {
  EXPECT_FALSE(ClipSelectionPaging::hasDraggedFrom(10, 20, 10, 20));
  EXPECT_FALSE(ClipSelectionPaging::hasDraggedFrom(10, 20, 13, 17));
  EXPECT_TRUE(ClipSelectionPaging::hasDraggedFrom(10, 20, 14, 20));
  EXPECT_TRUE(ClipSelectionPaging::hasDraggedFrom(10, 20, 10, 16));
}

TEST(ClipSelectionPaging, RequiresAHoldAtTheFinalDraggedWordBeforePaging) {
  std::vector<WordRef> words(3);
  words[0].pageIdx = 0;
  words[1].pageIdx = 0;
  words[2].pageIdx = 1;
  const uint16_t order[] = {0, 1, 2};

  EXPECT_EQ(ClipSelectionPaging::nextPageStartIndexForTouchDrag(false, 0, words, order, std::size(order), 1), -1);
  EXPECT_EQ(ClipSelectionPaging::nextPageStartIndexForTouchDrag(true, 2, words, order, std::size(order), 1), -1);
  EXPECT_EQ(ClipSelectionPaging::nextPageStartIndexForTouchDrag(true, 0, words, order, std::size(order), 1), 2);
  EXPECT_FALSE(ClipSelectionPaging::hasHeldPageEndLongEnough(999, 0));
  EXPECT_TRUE(ClipSelectionPaging::hasHeldPageEndLongEnough(1000, 0));
}

TEST(ClipSelectionPaging, AllowsOnlySmallPageEndDwellTouchJitter) {
  WordRef word;
  word.x = 100;
  word.y = 200;
  word.w = 20;
  word.h = 10;

  EXPECT_TRUE(ClipSelectionPaging::isWithinPageEndDwellSlop(word, 95, 195));
  EXPECT_TRUE(ClipSelectionPaging::isWithinPageEndDwellSlop(word, 127, 217));
  EXPECT_FALSE(ClipSelectionPaging::isWithinPageEndDwellSlop(word, 91, 200));
  EXPECT_FALSE(ClipSelectionPaging::isWithinPageEndDwellSlop(word, 120, 218));
}

TEST(ClipTextBuilder, JoinsInsertedHyphenAcrossParagraphBoundary) {
  ClipWordStore store;
  WordRef prefix;
  prefix.pageIdx = 0;
  prefix.pageWordIndex = 0;
  prefix.endsWithInsertedHyphen = true;
  ASSERT_TRUE(store.appendText(prefix, "hyphen-"));
  store.words.push_back(prefix);

  WordRef suffix;
  suffix.pageIdx = 1;
  suffix.pageWordIndex = 0;
  suffix.paragraphStart = true;  // Layout-derived marker at the start of the next page.
  ASSERT_TRUE(store.appendText(suffix, "ated"));
  store.words.push_back(suffix);

  const uint16_t order[] = {0, 1};
  const ClippingResult result = ClipTextBuilder::build(store, order, 0, 1, 2, 0, 2);

  EXPECT_EQ(result.text, "hyphenated");
}

TEST(ClipTextBuilder, AppliesDictionaryFragmentBoundsToTheirOwnPages) {
  ClipWordStore store;
  WordRef first;
  first.pageIdx = 0;
  first.pageWordIndex = 0;
  first.x = 0;
  first.w = 5;
  ASSERT_TRUE(store.appendText(first, "start"));
  store.words.push_back(first);

  WordRef middle;
  middle.pageIdx = 0;
  middle.pageWordIndex = 1;
  middle.x = 10;
  middle.w = 6;
  ASSERT_TRUE(store.appendText(middle, "middle"));
  store.words.push_back(middle);

  WordRef last;
  last.pageIdx = 1;
  last.pageWordIndex = 0;
  ASSERT_TRUE(store.appendText(last, "finish"));
  store.words.push_back(last);

  const uint16_t order[] = {0, 1, 2};
  const ClipTextBuilder::SelectionBounds bounds{0, 0, 4, 1, 0, 1};
  const ClippingResult result = ClipTextBuilder::build(store, order, 0, 2, 3, 0, 2, &bounds);

  EXPECT_EQ(result.text, "t middle f");
}

TEST(ClipTextBuilder, KeepsVerticalTableSelectionsInTheirColumn) {
  ClipWordStore store;
  constexpr uint16_t leftColumn = 8;
  constexpr uint16_t rightColumn = 9;
  const char* texts[] = {"left-top", "right-top", "left-bottom", "right-bottom"};
  const uint16_t columns[] = {leftColumn, rightColumn, leftColumn, rightColumn};
  const int xs[] = {0, 100, 0, 100};
  const int ys[] = {0, 0, 20, 20};
  for (uint16_t i = 0; i < 4; ++i) {
    WordRef word;
    word.pageWordIndex = i;
    word.tableSelection = columns[i];
    word.x = xs[i];
    word.y = ys[i];
    word.w = 20;
    word.h = 10;
    ASSERT_TRUE(store.appendText(word, texts[i]));
    store.words.push_back(word);
  }

  const uint16_t order[] = {0, 1, 2, 3};
  const ClippingResult result = ClipTextBuilder::build(store, order, 1, 3, 4, 0, 1, nullptr, rightColumn);

  EXPECT_EQ(result.text, "right-top\nright-bottom");
  EXPECT_EQ(result.wordCount, 2);
  EXPECT_EQ(result.startPageWordIndex, 1);
  EXPECT_EQ(result.endPageWordIndex, 3);
  EXPECT_EQ(result.tableSelection, rightColumn);
}

TEST(ClippingTextMatcher, MatchesLayoutInsertedHyphenFragmentsAsOneToken) {
  constexpr char token[] = "correctly";
  EXPECT_EQ(ClippingTextMatcher::matchTokenFragment("cor-", true, token, sizeof(token) - 1, 0),
            ClippingTextMatcher::TokenFragmentMatch::CONTINUES_TOKEN);
  EXPECT_EQ(ClippingTextMatcher::matchTokenFragment("rectly", false, token, sizeof(token) - 1, 3),
            ClippingTextMatcher::TokenFragmentMatch::COMPLETES_TOKEN);
}

TEST(ClippingTextMatcher, MatchesAdjacentDisplayFragmentsAsOneToken) {
  constexpr char token[] = "it\xE2\x80\xA6";
  EXPECT_EQ(ClippingTextMatcher::matchTokenFragment("it", false, token, sizeof(token) - 1, 0),
            ClippingTextMatcher::TokenFragmentMatch::CONTINUES_TOKEN);
  EXPECT_EQ(ClippingTextMatcher::matchTokenFragment("\xE2\x80\xA6", false, token, sizeof(token) - 1, 2),
            ClippingTextMatcher::TokenFragmentMatch::COMPLETES_TOKEN);
}

TEST(ClippingTextMatcher, MatchesNonBreakingSpaceBeforeAdjacentEllipsisFragment) {
  constexpr char token[] = "it \xE2\x80\xA6";
  const auto firstFragment =
      ClippingTextMatcher::matchTokenFragmentWithLength("it", false, token, sizeof(token) - 1, 0);
  EXPECT_EQ(firstFragment.match, ClippingTextMatcher::TokenFragmentMatch::CONTINUES_TOKEN);
  EXPECT_EQ(firstFragment.tokenBytes, 2);

  const auto ellipsisFragment = ClippingTextMatcher::matchTokenFragmentWithLength(
      "\xC2\xA0\xE2\x80\xA6", false, token, sizeof(token) - 1, firstFragment.tokenBytes);
  EXPECT_EQ(ellipsisFragment.match, ClippingTextMatcher::TokenFragmentMatch::COMPLETES_TOKEN);
  EXPECT_EQ(ellipsisFragment.tokenBytes, 4);
}

TEST(ClippingHighlightGeometry, BridgesVisibleWordsSeparatedByHiddenLayoutSpace) {
  const ClippingHighlightGeometry::WordRect wordBeforeEllipsis{12, 100, 200, 32, 20};
  const ClippingHighlightGeometry::WordRect ellipsis{13, 140, 200, 10, 20};
  ClippingHighlightGeometry::GapRect gap;

  ASSERT_TRUE(ClippingHighlightGeometry::gapBetweenAdjacentWords(wordBeforeEllipsis, ellipsis, gap));
  EXPECT_EQ(gap.x, 132);
  EXPECT_EQ(gap.y, 200);
  EXPECT_EQ(gap.width, 8);
  EXPECT_EQ(gap.height, 20);
}

TEST(ClippingHighlightGeometry, DoesNotBridgeDifferentLinesOrUnselectedWords) {
  const ClippingHighlightGeometry::WordRect first{12, 100, 200, 32, 20};
  ClippingHighlightGeometry::GapRect gap;

  EXPECT_FALSE(ClippingHighlightGeometry::gapBetweenAdjacentWords(first, {13, 140, 220, 10, 20}, gap));
  EXPECT_FALSE(ClippingHighlightGeometry::gapBetweenAdjacentWords(first, {14, 140, 200, 10, 20}, gap));
}

TEST(ClippingHighlightGeometry, KeepsTextFallbackInTheMatchedTableColumn) {
  constexpr uint16_t rightColumn = 9;
  EXPECT_TRUE(ClippingHighlightGeometry::matchesTableSelection(rightColumn, rightColumn));
  EXPECT_FALSE(ClippingHighlightGeometry::matchesTableSelection(rightColumn, 8));
  EXPECT_TRUE(ClippingHighlightGeometry::matchesTableSelection(UINT16_MAX, 8));
  EXPECT_TRUE(ClippingHighlightGeometry::isTableColumnCandidate(rightColumn));
  EXPECT_FALSE(ClippingHighlightGeometry::isTableColumnCandidate(UINT16_MAX));
  EXPECT_TRUE(ClippingHighlightGeometry::matchesTableColumn(rightColumn, 17, 8));
  EXPECT_FALSE(ClippingHighlightGeometry::matchesTableColumn(rightColumn, 8, 8));
}

TEST(ClippingTextMatcher, RejectsAuthoredHyphensAndMismatchedInsertedSuffixes) {
  constexpr char token[] = "correctly";
  constexpr char authoredHyphenToken[] = "wellknown";
  EXPECT_EQ(ClippingTextMatcher::matchTokenFragment("well-known", false, authoredHyphenToken,
                                                    sizeof(authoredHyphenToken) - 1, 0),
            ClippingTextMatcher::TokenFragmentMatch::MISMATCH);
  EXPECT_EQ(ClippingTextMatcher::matchTokenFragment("rectify", false, token, sizeof(token) - 1, 3),
            ClippingTextMatcher::TokenFragmentMatch::MISMATCH);
}

TEST(ClippingLayout, RejectsStoredRangeWhenFontChangesWithoutChangingPageCount) {
  ReaderRenderSpec original;
  original.fontId = 12;
  original.viewportWidth = 760;
  original.viewportHeight = 430;

  ReaderRenderSpec changed = original;
  changed.fontId = 16;

  Clipping clipping;
  clipping.pageCount = 20;
  clipping.layoutSignature = clippingWordLayoutSignature(readerRenderSpecSignature(original));

  EXPECT_TRUE(
      clippingStoredRangeMatchesLayout(clipping, 20, clippingWordLayoutSignature(readerRenderSpecSignature(original))));
  EXPECT_FALSE(
      clippingStoredRangeMatchesLayout(clipping, 20, clippingWordLayoutSignature(readerRenderSpecSignature(changed))));
}

TEST(ClippingLayout, RejectsUnsignedLegacyRangesAndUsesSavedTextInstead) {
  Clipping clipping;
  clipping.pageCount = 20;
  clipping.layoutSignature = 0;

  ReaderRenderSpec current;
  current.fontId = 12;
  EXPECT_FALSE(clippingStoredRangeMatchesLayout(clipping, 20, readerRenderSpecSignature(current)));
  EXPECT_FALSE(clippingStoredRangeMatchesLayout(clipping, 21, readerRenderSpecSignature(current)));
}

TEST(ClippingLayout, ChangesStoredRangeSignatureWhenWordTraversalChanges) {
  ReaderRenderSpec current;
  current.fontId = 12;
  current.viewportWidth = 760;
  current.viewportHeight = 430;

  const uint32_t readerSignature = readerRenderSpecSignature(current);
  const uint32_t clippingSignature = clippingWordLayoutSignature(readerSignature);
  EXPECT_NE(clippingSignature, readerSignature);

  Clipping clipping;
  clipping.pageCount = 20;
  clipping.layoutSignature = readerSignature;
  EXPECT_FALSE(clippingStoredRangeMatchesLayout(clipping, 20, clippingSignature));
  EXPECT_TRUE(clippingUsesLegacyWordLayout(clipping, 20, readerSignature));
  EXPECT_FALSE(clippingUsesLegacyWordLayout(clipping, 21, readerSignature));
}

TEST(ClippingLayout, CachesLegacyBoundaryRangesWithoutAHeapAllocation) {
  Clipping clipping;
  clipping.startPage = 2;
  clipping.endPage = 4;
  clipping.pageCount = 10;
  clipping.layoutSignature = 123;

  EXPECT_EQ(sizeof(Clipping), 84U);
  EXPECT_FALSE(clippingCachedRangeReadyOnPage(clipping, 2));
  EXPECT_TRUE(clippingCachedRangeReadyOnPage(clipping, 3));
  EXPECT_FALSE(clippingCachedRangeReadyOnPage(clipping, 4));

  EXPECT_TRUE(cacheClippingResolvedLayoutRange(clipping, 2, 7, 12, 456));
  EXPECT_EQ(clipping.startWordIndex, 7);
  EXPECT_EQ(clipping.layoutSignature, 123U);
  EXPECT_TRUE(clippingCachedRangeReadyOnPage(clipping, 2));
  EXPECT_FALSE(clippingCachedRangeReadyOnPage(clipping, 4));

  EXPECT_TRUE(cacheClippingResolvedLayoutRange(clipping, 4, 1, 9, 456));
  EXPECT_EQ(clipping.endWordIndex, 9);
  EXPECT_EQ(clipping.layoutSignature, 456U);
  EXPECT_TRUE(clippingCachedRangeReadyOnPage(clipping, 4));
}

TEST(ClippingLayout, CompletesSinglePageMigrationInOneMatch) {
  Clipping clipping;
  clipping.startPage = 3;
  clipping.endPage = 3;
  clipping.pageCount = 10;
  clipping.layoutSignature = 123;

  EXPECT_TRUE(cacheClippingResolvedLayoutRange(clipping, 3, 4, 8, 456));
  EXPECT_EQ(clipping.startWordIndex, 4);
  EXPECT_EQ(clipping.endWordIndex, 8);
  EXPECT_EQ(clipping.layoutSignature, 456U);
  EXPECT_EQ(clipping.resolvedLayoutBoundaries, CLIPPING_LAYOUT_BOUNDARIES_RESOLVED);
}

TEST(ClippingMatchTracker, RejectsRepeatedShortTextAtDifferentWordRanges) {
  ClippingMatchTracker matches;

  EXPECT_TRUE(matches.record(2, 3));
  EXPECT_TRUE(matches.unique());
  EXPECT_FALSE(matches.record(11, 12));
  EXPECT_TRUE(matches.found());
  EXPECT_FALSE(matches.unique());
  EXPECT_EQ(matches.startWord(), 2);
  EXPECT_EQ(matches.endWord(), 3);
}

TEST(ClippingMatchTracker, TreatsDuplicateCandidatesForTheSameRangeAsUnique) {
  ClippingMatchTracker matches;

  EXPECT_TRUE(matches.record(4, 6));
  EXPECT_FALSE(matches.record(4, 6));
  EXPECT_TRUE(matches.unique());
}
