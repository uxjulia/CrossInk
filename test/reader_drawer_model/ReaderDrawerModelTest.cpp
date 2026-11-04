#include <gtest/gtest.h>

#include "EpubReaderMenuModel.h"
#include "FrontlightPanelModel.h"
#include "PendingOverlayResume.h"
#include "components/SliderValue.h"

TEST(ReaderDrawerModel, TabOrderMatchesTouchDesign) {
  EXPECT_EQ(static_cast<uint8_t>(ReaderDrawerTab::Font), 0);
  EXPECT_EQ(static_cast<uint8_t>(ReaderDrawerTab::Layout), 1);
  EXPECT_EQ(static_cast<uint8_t>(ReaderDrawerTab::More), 2);
  EXPECT_EQ(static_cast<uint8_t>(ReaderDrawerTab::Location), 3);
  EXPECT_EQ(static_cast<uint8_t>(ReaderDrawerTab::Settings), 4);
}

TEST(ReaderDrawerModel, ButtonTabsWrapAtBothEnds) {
  EXPECT_EQ(adjacentReaderDrawerTab(ReaderDrawerTab::Settings, true), ReaderDrawerTab::Font);
  EXPECT_EQ(adjacentReaderDrawerTab(ReaderDrawerTab::Font, false), ReaderDrawerTab::Settings);
  EXPECT_EQ(adjacentReaderDrawerTab(ReaderDrawerTab::More, true), ReaderDrawerTab::Location);
}

TEST(ReaderDrawerModel, ButtonSliderFocusEditAndBackAreExplicit) {
  ReaderButtonSliderState state;
  EXPECT_EQ(readerButtonSliderInput(state, ReaderButtonSliderInput::Next), ReaderButtonSliderAction::None);
  EXPECT_EQ(state.focus, 1);
  EXPECT_EQ(readerButtonSliderInput(state, ReaderButtonSliderInput::Confirm), ReaderButtonSliderAction::None);
  EXPECT_TRUE(state.editing);
  EXPECT_EQ(readerButtonSliderInput(state, ReaderButtonSliderInput::Next), ReaderButtonSliderAction::Increase);
  EXPECT_EQ(readerButtonSliderInput(state, ReaderButtonSliderInput::Previous), ReaderButtonSliderAction::Decrease);
  EXPECT_EQ(readerButtonSliderInput(state, ReaderButtonSliderInput::Back), ReaderButtonSliderAction::None);
  EXPECT_FALSE(state.editing);
  EXPECT_EQ(readerButtonSliderInput(state, ReaderButtonSliderInput::Previous), ReaderButtonSliderAction::None);
  EXPECT_EQ(state.focus, 0);
  EXPECT_EQ(readerButtonSliderInput(state, ReaderButtonSliderInput::Back), ReaderButtonSliderAction::LeavePane);
}

TEST(ReaderDrawerModel, FailedPreviewRestoresFontWithoutDiscardingOtherEdits) {
  ReaderSettingsDraft draft;
  draft.fontFamily = 2;
  draft.readerFontPointSize = 25;
  draft.sdFontFamilyName[0] = 'X';
  draft.orientation = 3;
  draft.hyphenationEnabled = 1;
  draft.screenMarginHorizontal = 45;
  ReaderSettingsDraft lastGood;
  lastGood.fontFamily = 1;
  lastGood.readerFontPointSize = 18;
  lastGood.orientation = 0;
  lastGood.screenMarginHorizontal = 15;

  restoreReaderDraftFont(draft, lastGood);
  EXPECT_EQ(draft.fontFamily, 1);
  EXPECT_EQ(draft.readerFontPointSize, 18);
  EXPECT_EQ(draft.sdFontFamilyName[0], '\0');
  EXPECT_EQ(draft.orientation, 3);
  EXPECT_EQ(draft.hyphenationEnabled, 1);
  EXPECT_EQ(draft.screenMarginHorizontal, 45);
}

TEST(ReaderDrawerModel, TapSlidersSnapArbitraryValuesToTheNearestFive) {
  EXPECT_EQ(snapSliderTapValue(82, 70, 200, 5), 80);   // Line Spacing
  EXPECT_EQ(snapSliderTapValue(83, 70, 200, 5), 85);   // Line Spacing
  EXPECT_EQ(snapSliderTapValue(8, 5, 150, 5), 10);     // Screen Margin
  EXPECT_EQ(snapSliderTapValue(43, 0, 100, 5), 45);    // Go to %
  EXPECT_EQ(snapSliderTapValue(118, 5, 120, 5), 120);  // Auto Page Turn
  EXPECT_EQ(snapSliderTapValue(43, 0, 100, 1), 43);    // Fixed values stay exact
  EXPECT_EQ(snapSliderTapValue(3, 0, 4, 1), 3);        // Word spacing levels stay numeric and exact
}

TEST(ReaderDrawerModel, DrawerStateRoundTripsWithoutLosingScrollOrFontPreview) {
  ReaderDrawerState original;
  original.tab = ReaderDrawerTab::Location;
  original.pane = ReaderDrawerPane::EnumOptions;
  original.selectedIndex = 17;
  original.rootTopIndex = {2, 4, 6, 8, 10};
  original.paneTopIndex = 13;
  original.pendingFontIndex = 5;

  const ReaderDrawerState restored = original;
  EXPECT_EQ(restored.tab, ReaderDrawerTab::Location);
  EXPECT_EQ(restored.pane, ReaderDrawerPane::EnumOptions);
  EXPECT_EQ(restored.selectedIndex, 17);
  EXPECT_EQ(restored.rootTopIndex, (std::array<int16_t, READER_DRAWER_TAB_COUNT>{2, 4, 6, 8, 10}));
  EXPECT_EQ(restored.paneTopIndex, 13);
  EXPECT_EQ(restored.pendingFontIndex, 5);
}

TEST(ReaderDrawerModel, ResumeRestoresScrollToTheActiveRootOrPane) {
  ReaderDrawerState root;
  root.tab = ReaderDrawerTab::Settings;
  root.pane = ReaderDrawerPane::Root;
  restoreReaderDrawerScroll(root, 7);
  EXPECT_EQ(root.rootTopIndex[static_cast<size_t>(ReaderDrawerTab::Settings)], 7);
  EXPECT_EQ(root.paneTopIndex, 0);

  ReaderDrawerState pane;
  pane.tab = ReaderDrawerTab::Location;
  pane.pane = ReaderDrawerPane::EnumOptions;
  restoreReaderDrawerScroll(pane, 11);
  EXPECT_EQ(pane.paneTopIndex, 11);
  EXPECT_EQ(pane.rootTopIndex[static_cast<size_t>(ReaderDrawerTab::Location)], 0);
}

TEST(ReaderDrawerModel, CatalogOrderAndConditionalRowsMatchTouchDesign) {
  const ReaderDrawerCatalog minimal = makeReaderDrawerCatalog({});
  const auto& font = minimal[static_cast<size_t>(ReaderDrawerTab::Font)];
  EXPECT_EQ(font.count, 6);
  EXPECT_EQ(font.items[0], ReaderDrawerCatalogItem::ReaderFont);
  EXPECT_EQ(font.items[5], ReaderDrawerCatalogItem::GuideDots);

  const auto& layout = minimal[static_cast<size_t>(ReaderDrawerTab::Layout)];
  EXPECT_EQ(layout.count, 9);
  EXPECT_EQ(layout.items[0], ReaderDrawerCatalogItem::Margins);
  EXPECT_EQ(layout.items[3], ReaderDrawerCatalogItem::Images);
  EXPECT_EQ(layout.items[4], ReaderDrawerCatalogItem::Hyphenation);

  const auto& minimalMore = minimal[static_cast<size_t>(ReaderDrawerTab::More)];
  EXPECT_EQ(minimalMore.count, 3);
  EXPECT_EQ(minimalMore.items[0], ReaderDrawerCatalogItem::SelectChapter);
  EXPECT_EQ(minimalMore.items[2], ReaderDrawerCatalogItem::AutoPageTurn);

  ReaderDrawerAvailability stableAvailable{};
  stableAvailable.hasStablePageNumbers = true;
  const ReaderDrawerCatalog stableCatalog = makeReaderDrawerCatalog(stableAvailable);
  const auto& stableMore = stableCatalog[static_cast<size_t>(ReaderDrawerTab::More)];
  EXPECT_EQ(stableMore.count, 4);
  EXPECT_EQ(stableMore.items[1], ReaderDrawerCatalogItem::GoToPercent);
  EXPECT_EQ(stableMore.items[2], ReaderDrawerCatalogItem::GoToStablePage);

  const auto& minimalLocation = minimal[static_cast<size_t>(ReaderDrawerTab::Location)];
  EXPECT_EQ(minimalLocation.count, 4);
  EXPECT_EQ(minimalLocation.items[0], ReaderDrawerCatalogItem::BookmarkToggle);
  EXPECT_EQ(minimalLocation.items[3], ReaderDrawerCatalogItem::DisplayQr);

  const ReaderDrawerCatalog complete = makeReaderDrawerCatalog({true, true, true, true, true, true});
  const auto& more = complete[static_cast<size_t>(ReaderDrawerTab::More)];
  EXPECT_EQ(more.count, 7);
  EXPECT_EQ(more.items[0], ReaderDrawerCatalogItem::Lookup);
  EXPECT_EQ(more.items[1], ReaderDrawerCatalogItem::LookupHistory);
  EXPECT_EQ(more.items[2], ReaderDrawerCatalogItem::SelectChapter);
  EXPECT_EQ(more.items[3], ReaderDrawerCatalogItem::GoToPercent);
  EXPECT_EQ(more.items[4], ReaderDrawerCatalogItem::GoToStablePage);
  EXPECT_EQ(more.items[5], ReaderDrawerCatalogItem::AutoPageTurn);

  const auto& location = complete[static_cast<size_t>(ReaderDrawerTab::Location)];
  EXPECT_EQ(location.count, 7);
  EXPECT_EQ(location.items[0], ReaderDrawerCatalogItem::BookmarkToggle);
  EXPECT_EQ(location.items[1], ReaderDrawerCatalogItem::ViewBookmarks);
  EXPECT_EQ(location.items[2], ReaderDrawerCatalogItem::DeleteBookmarks);
  EXPECT_EQ(location.items[3], ReaderDrawerCatalogItem::SaveClipping);
  EXPECT_EQ(location.items[4], ReaderDrawerCatalogItem::ViewClippings);
  EXPECT_EQ(location.items[5], ReaderDrawerCatalogItem::Screenshot);
  EXPECT_EQ(location.items[6], ReaderDrawerCatalogItem::DisplayQr);

  const auto& settings = complete[static_cast<size_t>(ReaderDrawerTab::Settings)];
  EXPECT_EQ(settings.count, 11);
  EXPECT_EQ(settings.items[0], ReaderDrawerCatalogItem::StatusBar);
  EXPECT_EQ(settings.items[1], ReaderDrawerCatalogItem::Controls);
  EXPECT_EQ(settings.items[6], ReaderDrawerCatalogItem::TrackBookStats);
  EXPECT_EQ(settings.items[7], ReaderDrawerCatalogItem::ResetReadingPace);
  EXPECT_EQ(settings.items[10], ReaderDrawerCatalogItem::ResetBookReaderSettings);
}

TEST(ReaderDrawerModel, StatsRowsFollowGlobalAndBookTrackingChoices) {
  ReaderDrawerAvailability available{};
  available.buttonDevice = true;
  available.bookStatsEnabled = false;
  auto catalog = makeReaderDrawerCatalog(available);
  EXPECT_EQ(catalog[static_cast<size_t>(ReaderDrawerTab::More)].count, 3);
  const auto& settings = catalog[static_cast<size_t>(ReaderDrawerTab::Settings)];
  EXPECT_EQ(settings.items[6], ReaderDrawerCatalogItem::TrackBookStats);
  EXPECT_EQ(settings.items[settings.count - 1], ReaderDrawerCatalogItem::ResetBookReaderSettings);

  available.globalStatsEnabled = false;
  catalog = makeReaderDrawerCatalog(available);
  const auto& globalOffSettings = catalog[static_cast<size_t>(ReaderDrawerTab::Settings)];
  for (uint8_t i = 0; i < globalOffSettings.count; ++i) {
    EXPECT_NE(globalOffSettings.items[i], ReaderDrawerCatalogItem::TrackBookStats);
    EXPECT_NE(globalOffSettings.items[i], ReaderDrawerCatalogItem::DeleteStats);
  }
}

TEST(ReaderDrawerModel, ButtonDevicesRestoreStatsAndTransfersInTheirTabs) {
  ReaderDrawerAvailability available{};
  available.buttonDevice = true;
  const ReaderDrawerCatalog catalog = makeReaderDrawerCatalog(available);
  const auto& more = catalog[static_cast<size_t>(ReaderDrawerTab::More)];
  EXPECT_EQ(more.count, 4);
  EXPECT_EQ(more.items[3], ReaderDrawerCatalogItem::ReadingStats);

  const auto& location = catalog[static_cast<size_t>(ReaderDrawerTab::Location)];
  EXPECT_EQ(location.count, 7);
  EXPECT_EQ(location.items[2], ReaderDrawerCatalogItem::SyncProgress);
  EXPECT_EQ(location.items[3], ReaderDrawerCatalogItem::NearbyPositionSync);
  EXPECT_EQ(location.items[4], ReaderDrawerCatalogItem::SendNearbyBook);
  EXPECT_EQ(location.items[5], ReaderDrawerCatalogItem::Screenshot);
  EXPECT_EQ(location.items[6], ReaderDrawerCatalogItem::DisplayQr);

  available.hasBookmarks = true;
  available.hasClippings = true;
  const ReaderDrawerCatalog fullCatalog = makeReaderDrawerCatalog(available);
  const auto& fullLocation = fullCatalog[static_cast<size_t>(ReaderDrawerTab::Location)];
  EXPECT_EQ(fullLocation.count, 10);
  EXPECT_EQ(fullLocation.items[5], ReaderDrawerCatalogItem::SyncProgress);
}

TEST(ReaderDrawerModel, ChangeMaskSeparatesPreviewRelayoutAndOrientation) {
  const auto mask =
      ReaderSettingsChangeMask::Preview | ReaderSettingsChangeMask::Relayout | ReaderSettingsChangeMask::Orientation;
  EXPECT_TRUE(hasReaderSettingsChange(mask, ReaderSettingsChangeMask::Preview));
  EXPECT_TRUE(hasReaderSettingsChange(mask, ReaderSettingsChangeMask::Relayout));
  EXPECT_TRUE(hasReaderSettingsChange(mask, ReaderSettingsChangeMask::Orientation));
  EXPECT_FALSE(hasReaderSettingsChange(mask, ReaderSettingsChangeMask::NonLayout));
}

TEST(FrontlightPanelModel, ChoosesCurrentThenLastThenDeviceStats) {
  EXPECT_EQ(chooseFrontlightBookSource(true, true, true), FrontlightBookSource::CurrentBook);
  EXPECT_EQ(chooseFrontlightBookSource(false, false, true), FrontlightBookSource::LastBook);
  EXPECT_EQ(chooseFrontlightBookSource(true, false, true), FrontlightBookSource::LastBook);
  EXPECT_EQ(chooseFrontlightBookSource(false, false, false), FrontlightBookSource::DeviceOnly);
}

TEST(FrontlightPanelModel, ActiveReaderBookRequiresReaderAndExistingBook) {
  EXPECT_TRUE(hasFrontlightActiveReaderBook(true, true));
  EXPECT_FALSE(hasFrontlightActiveReaderBook(false, true));
  EXPECT_FALSE(hasFrontlightActiveReaderBook(true, false));
}

TEST(FrontlightPanelModel, StickyReaderDetailsRequireAnOpenReaderWithoutFrontlight) {
  EXPECT_TRUE(shouldShowStickyReaderDetails(true, false, true));
  EXPECT_FALSE(shouldShowStickyReaderDetails(false, false, true));
  EXPECT_FALSE(shouldShowStickyReaderDetails(true, true, true));
  EXPECT_FALSE(shouldShowStickyReaderDetails(true, false, false));
}

TEST(FrontlightPanelModel, TouchDrawerSupportsFrontlightOrReaderDetails) {
  EXPECT_TRUE(supportsFrontlightDrawer(true, true));
  EXPECT_FALSE(supportsFrontlightDrawer(true, false));
  EXPECT_FALSE(supportsFrontlightDrawer(false, true));
  EXPECT_TRUE(supportsFrontlightDrawer(true, false, true));
  EXPECT_FALSE(supportsFrontlightDrawer(false, false, true));
}

TEST(ReaderDrawerModel, PercentStepsDoNotBecomeSettingsChanges) {
  EXPECT_TRUE(readerDrawerStepChangesSettings(ReaderDrawerPane::Spacing));
  EXPECT_TRUE(readerDrawerStepChangesSettings(ReaderDrawerPane::Margins));
  EXPECT_TRUE(readerDrawerStepChangesSettings(ReaderDrawerPane::AutoPageTurn));
  EXPECT_FALSE(readerDrawerStepChangesSettings(ReaderDrawerPane::Percent));
  EXPECT_TRUE(readerDrawerSliderPreviewsText(ReaderDrawerPane::Spacing));
  EXPECT_TRUE(readerDrawerSliderPreviewsText(ReaderDrawerPane::Margins));
  EXPECT_FALSE(readerDrawerSliderPreviewsText(ReaderDrawerPane::AutoPageTurn));
  EXPECT_FALSE(readerDrawerSliderPreviewsText(ReaderDrawerPane::Percent));
  EXPECT_FALSE(readerDrawerSliderPreviewsText(ReaderDrawerPane::StablePage));
}

TEST(ReaderDrawerModel, AntiAliasingOnlyRunsWhenTheReaderPreviewWasRedrawn) {
  EXPECT_TRUE(shouldRenderReaderDrawerAntiAliasing(true, true, true));
  EXPECT_FALSE(shouldRenderReaderDrawerAntiAliasing(false, true, true));
  EXPECT_FALSE(shouldRenderReaderDrawerAntiAliasing(true, false, true));
  EXPECT_FALSE(shouldRenderReaderDrawerAntiAliasing(true, true, false));
}

TEST(ReaderDrawerModel, TouchModeDoesNotRenderPhysicalButtonFocus) {
  EXPECT_FALSE(isReaderDrawerRowFocused(false, 0, 0));
  EXPECT_EQ(readerDrawerFocusedWindowIndex(false, 7, 5), -1);

  EXPECT_TRUE(isReaderDrawerRowFocused(true, 0, 0));
  EXPECT_FALSE(isReaderDrawerRowFocused(true, 0, 1));
  EXPECT_EQ(readerDrawerFocusedWindowIndex(true, 7, 5), 2);
}

TEST(ReaderDrawerModel, RootRowsUseStandardDrawerCadence) { EXPECT_EQ(readerDrawerVisibleRows(260, 52, 8), 4); }

TEST(ReaderDrawerModel, LandscapeRootHeightReservesFourRealRows) {
  EXPECT_EQ(readerDrawerListHeightForRows(4, 52, 8), 232);
}

TEST(ReaderDrawerModel, TallLandscapeSheetCoversDualSlidersAndTheKeypadPanes) {
  EXPECT_TRUE(readerDrawerNeedsTallLandscapeSheet(ReaderDrawerPane::Spacing));
  EXPECT_TRUE(readerDrawerNeedsTallLandscapeSheet(ReaderDrawerPane::Margins));
  EXPECT_TRUE(readerDrawerNeedsTallLandscapeSheet(ReaderDrawerPane::Percent));
  EXPECT_TRUE(readerDrawerNeedsTallLandscapeSheet(ReaderDrawerPane::StablePage));
  EXPECT_FALSE(readerDrawerNeedsTallLandscapeSheet(ReaderDrawerPane::Root));
  EXPECT_FALSE(readerDrawerNeedsTallLandscapeSheet(ReaderDrawerPane::AutoPageTurn));
}

TEST(ReaderDrawerModel, DirtyPreviewCanReplaceAStaleExternalBackdrop) {
  EXPECT_FALSE(readerDrawerNeedsExternalBackdrop(true, true, false));
  EXPECT_FALSE(readerDrawerNeedsExternalBackdrop(true, false, true));
  EXPECT_TRUE(readerDrawerNeedsExternalBackdrop(false, true, true));
  EXPECT_TRUE(readerDrawerNeedsExternalBackdrop(true, false, false));
}

TEST(PendingOverlayResume, ConsumptionIsOneShot) {
  PendingOverlayResume stored;
  stored.origin = PendingOverlayOrigin::Reader;
  stored.overlay = PendingOverlayType::FrontlightDrawer;
  stored.selectedIndex = 3;
  stored.bookPath = "/books/test.epub";
  stored.readerOrientation = 0;
  stored.preserveReaderOrientation = true;

  PendingOverlayResume consumed;
  EXPECT_TRUE(consumePendingOverlayResumeOnce(stored, consumed));
  EXPECT_FALSE(stored.valid());
  EXPECT_EQ(consumed.origin, PendingOverlayOrigin::Reader);
  EXPECT_EQ(consumed.overlay, PendingOverlayType::FrontlightDrawer);
  EXPECT_EQ(consumed.selectedIndex, 3);
  EXPECT_EQ(consumed.bookPath, "/books/test.epub");
  EXPECT_EQ(consumed.readerOrientation, 0);
  EXPECT_TRUE(consumed.preserveReaderOrientation);

  PendingOverlayResume second;
  EXPECT_FALSE(consumePendingOverlayResumeOnce(stored, second));
}

TEST(ReaderDrawerModel, SamplePreviewCoversLiveTextSettingsOnly) {
  using Pane = ReaderDrawerPane;
  using Tab = ReaderDrawerTab;
  using Row = ReaderDrawerCatalogItem;
  EXPECT_TRUE(readerDrawerShowsSamplePreview(Pane::Root, Tab::Font, Row::FontSize));
  for (const auto pane : {Pane::ReaderFont, Pane::FontFamily, Pane::Spacing, Pane::Margins})
    EXPECT_TRUE(readerDrawerShowsSamplePreview(pane, Tab::Layout, Row::FontSize));
  for (const auto row : {Row::FontSize, Row::Alignment})
    EXPECT_TRUE(readerDrawerShowsSamplePreview(Pane::EnumOptions, Tab::Layout, row));
  for (const auto tab : {Tab::Layout, Tab::More, Tab::Location, Tab::Settings})
    EXPECT_FALSE(readerDrawerShowsSamplePreview(Pane::Root, tab, Row::FontSize));
  for (const auto row : {Row::Orientation, Row::DictionaryFontFamily, Row::DictionaryFontSize, Row::Images})
    EXPECT_FALSE(readerDrawerShowsSamplePreview(Pane::EnumOptions, Tab::Layout, row));
  EXPECT_FALSE(readerDrawerShowsSamplePreview(Pane::DictionaryFont, Tab::Font, Row::FontSize));
}

TEST(PendingOverlayResume, FileBrowserReturnPreservesLargeListPosition) {
  PendingOverlayResume stored;
  stored.origin = PendingOverlayOrigin::FileBrowser;
  EXPECT_FALSE(stored.valid());
  stored.fileBrowserPath = "/books";
  stored.selectedIndex = 40000;
  stored.scrollPosition = 39990;

  PendingOverlayResume consumed;
  ASSERT_TRUE(consumePendingOverlayResumeOnce(stored, consumed));
  EXPECT_EQ(consumed.fileBrowserPath, "/books");
  EXPECT_EQ(consumed.selectedIndex, 40000);
  EXPECT_EQ(consumed.scrollPosition, 39990);
  EXPECT_FALSE(stored.valid());
  EXPECT_TRUE(stored.fileBrowserPath.empty());
}

TEST(ReaderDrawerModel, LandscapePreviewPanesPlaceSamplePreviewBesideControls) {
  EXPECT_TRUE(readerDrawerSamplePreviewBesideControls(true));
  EXPECT_FALSE(readerDrawerSamplePreviewBesideControls(false));
}

TEST(ReaderDrawerModelTest, CharacterSpacingReservesPreviewAndUsesSliderInput) {
  EXPECT_TRUE(readerDrawerStepChangesSettings(ReaderDrawerPane::CharacterSpacing));
  EXPECT_TRUE(readerDrawerShowsSamplePreview(ReaderDrawerPane::CharacterSpacing, ReaderDrawerTab::Font,
                                             ReaderDrawerCatalogItem::CharacterSpacing));
  EXPECT_TRUE(readerDrawerSliderPreviewsText(ReaderDrawerPane::CharacterSpacing));
}
