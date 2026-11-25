#include <gtest/gtest.h>

#include "ReaderStatusBarConfig.h"
#include "ReaderStatusBarJson.h"

TEST(ReaderStatusBarConfig, MigratesSixLegacyItemsIntoSevenSlots) {
  const LegacyReaderStatusBarSettings old{true, true, true, 1, 2, true, 2, 0, 2};
  const auto bar = migrateBottomStatusBar(old);
  EXPECT_EQ(bar.slots[0], ReaderStatusBarItem::Battery);
  EXPECT_EQ(bar.slots[1], ReaderStatusBarItem::TimeLeftBook);
  EXPECT_EQ(bar.slots[2], ReaderStatusBarItem::Empty);
  EXPECT_EQ(bar.slots[3], ReaderStatusBarItem::TitleChapter);
  EXPECT_EQ(bar.slots[4], ReaderStatusBarItem::ChapterPageCount);
  EXPECT_EQ(bar.slots[5], ReaderStatusBarItem::StablePageNumber);
  EXPECT_EQ(bar.slots[6], ReaderStatusBarItem::BookProgressPercentage);
  EXPECT_EQ(bar.percentageFormat, 2);
  EXPECT_EQ(bar.progressBar, 0);
  EXPECT_EQ(bar.progressBarThickness, 2);
}

TEST(ReaderStatusBarConfig, LegacyXtcTopInitiallyUsesMigratedBottomItems) {
  const auto bottom = migrateBottomStatusBar({true, true, true, 1, 2, true, 2, 0, 2});
  ReaderStatusBarConfig top;
  top.slots[ReaderStatusBarConfig::CENTER] = ReaderStatusBarItem::Clock;
  const auto select = [&](const ReaderStatusBarPosition displayed,
                          const bool legacyTopUsesBottom) -> const ReaderStatusBarConfig& {
    return xtcStatusBarConfigPosition(displayed, legacyTopUsesBottom) == ReaderStatusBarPosition::Top ? top : bottom;
  };
  EXPECT_EQ(select(ReaderStatusBarPosition::Top, true).slots, bottom.slots);
  EXPECT_EQ(select(ReaderStatusBarPosition::Top, false).slots, top.slots);
  EXPECT_EQ(select(ReaderStatusBarPosition::Bottom, true).slots, bottom.slots);
}

TEST(ReaderStatusBarConfig, EmptySlotsDoNotReserveSpaceAndDuplicatesStayIndependent) {
  ReaderStatusBarConfig bar;
  bar.slots = {ReaderStatusBarItem::Battery,   ReaderStatusBarItem::Empty, ReaderStatusBarItem::Empty,
               ReaderStatusBarItem::TitleBook, ReaderStatusBarItem::Empty, ReaderStatusBarItem::Battery,
               ReaderStatusBarItem::Empty};
  EXPECT_TRUE(bar.contains(ReaderStatusBarItem::Battery));
  const std::array<int, ReaderStatusBarConfig::SLOT_COUNT> widths{16, 0, 0, 0, 0, 16, 0};
  const auto placement = layoutReaderStatusBarItems(10, 190, widths);
  EXPECT_EQ(placement.x[0], 10);
  EXPECT_EQ(placement.x[5], 174);
  EXPECT_EQ(placement.centerLeft, 34);
  EXPECT_EQ(placement.centerRight, 166);
}

TEST(ReaderStatusBarConfig, LoneSideTitleUsesFreeRowWidth) {
  std::array<int, ReaderStatusBarConfig::SLOT_COUNT> widths{500, 0, 0, 0, 0, 0, 0};
  fitReaderStatusBarSideWidths(widths, 700);
  EXPECT_EQ(widths[0], 500);
  const auto placement = layoutReaderStatusBarItems(10, 710, widths);
  EXPECT_EQ(placement.x[0], 10);
  EXPECT_EQ(placement.centerLeft, 518);
  EXPECT_EQ(placement.centerRight, 710);
}

TEST(ReaderStatusBarConfig, CrowdedSidesFitWithoutOverlap) {
  std::array<int, ReaderStatusBarConfig::SLOT_COUNT> widths{200, 24, 24, 0, 200, 24, 24};
  fitReaderStatusBarSideWidths(widths, 400, 13);
  const auto placement = layoutReaderStatusBarItems(10, 410, widths, 13);
  EXPECT_GT(placement.x[2], placement.x[1]);
  EXPECT_LE(placement.centerLeft, placement.centerRight);
  EXPECT_EQ(placement.x[6] + widths[6], 410);
  EXPECT_EQ(widths[1], 24);
  EXPECT_EQ(widths[2], 24);
  EXPECT_EQ(widths[5], 24);
}

TEST(ReaderStatusBarConfig, ProgressOnlyBottomReservesBookmarkAndAutoTurnText) {
  EXPECT_EQ(readerStatusBarTotalHeight(ReaderStatusBarPosition::Top, false, 7, 19), 7);
  EXPECT_EQ(readerStatusBarTotalHeight(ReaderStatusBarPosition::Bottom, false, 7, 19), 14);
  EXPECT_EQ(readerStatusBarTotalHeight(ReaderStatusBarPosition::Bottom, true, 7, 19), 26);
}

TEST(ReaderStatusBarConfig, BookmarkStaysAtBottomLeftAndRightItemsAnchorToEdge) {
  const std::array<int, ReaderStatusBarConfig::SLOT_COUNT> widths{20, 0, 0, 0, 15, 0, 10};
  const auto placement = layoutReaderStatusBarItems(10, 190, widths, 13);
  EXPECT_EQ(placement.x[0], 31);
  EXPECT_EQ(placement.x[4], 157);
  EXPECT_EQ(placement.x[6], 180);
  EXPECT_LE(placement.centerLeft, placement.centerRight);
}

TEST(ReaderStatusBarConfig, RejectsInvalidSavedSlotsAndUnavailableClock) {
  EXPECT_TRUE(validReaderStatusBarItemValue(static_cast<int>(ReaderStatusBarItem::Clock), true));
  EXPECT_FALSE(validReaderStatusBarItemValue(static_cast<int>(ReaderStatusBarItem::Clock), false));
  EXPECT_FALSE(validReaderStatusBarItemValue(-1, true));
  EXPECT_FALSE(validReaderStatusBarItemValue(static_cast<int>(ReaderStatusBarItem::Count), true));
  EXPECT_TRUE(validReaderStatusBarChoice(2, 3));
  EXPECT_FALSE(validReaderStatusBarChoice(3, 3));
}

TEST(ReaderStatusBarConfig, WebPayloadRoundTripsBothBarsWithDuplicates) {
  ReaderStatusBarConfig top;
  top.slots = {ReaderStatusBarItem::Clock,
               ReaderStatusBarItem::Battery,
               ReaderStatusBarItem::TimeLeftBook,
               ReaderStatusBarItem::TitleBook,
               ReaderStatusBarItem::Battery,
               ReaderStatusBarItem::Empty,
               ReaderStatusBarItem::BookProgressPercentage};
  top.percentageFormat = 2;
  top.progressBar = 0;
  JsonDocument outgoing;
  writeReaderStatusBarJson(outgoing["top"].to<JsonObject>(), top);
  writeReaderStatusBarJson(outgoing["bottom"].to<JsonObject>(), migrateBottomStatusBar({}));
  outgoing["xtcMode"] = 3;

  std::string saved;
  serializeJson(outgoing, saved);
  JsonDocument reloaded;
  ASSERT_FALSE(deserializeJson(reloaded, saved));
  ReaderStatusBarsPayload loaded;
  ASSERT_TRUE(readReaderStatusBarsPayload(reloaded.as<JsonVariantConst>(), loaded, true, 3, 3, 3, 4));
  EXPECT_EQ(loaded.top.slots, top.slots);
  EXPECT_EQ(loaded.top.percentageFormat, 2);
  EXPECT_EQ(loaded.top.progressBar, 0);
  EXPECT_EQ(loaded.xtcMode, 3);
}

TEST(ReaderStatusBarConfig, WebPayloadRequiresBothBarsAndValidXtcModeBeforeSaving) {
  JsonDocument request;
  writeReaderStatusBarJson(request["top"].to<JsonObject>(), ReaderStatusBarConfig{});
  request["xtcMode"] = 2;
  ReaderStatusBarsPayload parsed;
  EXPECT_FALSE(readReaderStatusBarsPayload(request.as<JsonVariantConst>(), parsed, true, 3, 3, 3, 4));
  writeReaderStatusBarJson(request["bottom"].to<JsonObject>(), ReaderStatusBarConfig{});
  EXPECT_TRUE(readReaderStatusBarsPayload(request.as<JsonVariantConst>(), parsed, true, 3, 3, 3, 4));
  request["xtcMode"] = 4;
  EXPECT_FALSE(readReaderStatusBarsPayload(request.as<JsonVariantConst>(), parsed, true, 3, 3, 3, 4));
  EXPECT_EQ(parsed.xtcMode, 2);
}

TEST(ReaderStatusBarConfig, WebPayloadRejectsInvalidOrIncompleteBars) {
  JsonDocument payload;
  auto bar = payload["top"].to<JsonObject>();
  writeReaderStatusBarJson(bar, ReaderStatusBarConfig{});
  ReaderStatusBarConfig loaded;
  EXPECT_TRUE(readReaderStatusBarJson(payload["top"], loaded, true, 3, 3, 3));
  bar["slots"][0] = 99;
  EXPECT_FALSE(readReaderStatusBarJson(payload["top"], loaded, true, 3, 3, 3));
  bar["slots"][0] = 1;
  EXPECT_FALSE(readReaderStatusBarJson(payload["top"], loaded, false, 3, 3, 3));
  bar["slots"][0] = 0;
  bar["percentageFormat"] = 3;
  EXPECT_FALSE(readReaderStatusBarJson(payload["top"], loaded, true, 3, 3, 3));
  bar["percentageFormat"] = 0;
  bar["slots"].as<JsonArray>().remove(6);
  EXPECT_FALSE(readReaderStatusBarJson(payload["top"], loaded, true, 3, 3, 3));
}

TEST(ReaderStatusBarConfig, RepairsDamagedSavedValuesWithoutDroppingValidSlots) {
  JsonDocument saved;
  auto bar = saved["bottom"].to<JsonObject>();
  ReaderStatusBarConfig original;
  original.slots = {ReaderStatusBarItem::Battery,
                    ReaderStatusBarItem::Clock,
                    ReaderStatusBarItem::Empty,
                    ReaderStatusBarItem::TitleBook,
                    ReaderStatusBarItem::ChapterPageCount,
                    ReaderStatusBarItem::Empty,
                    ReaderStatusBarItem::Empty};
  writeReaderStatusBarJson(bar, original);
  bar["slots"][4] = 99;
  bar["thickness"] = 99;

  ReaderStatusBarConfig repaired;
  EXPECT_TRUE(repairReaderStatusBarJson(saved["bottom"], repaired, false, 3, 3, 3));
  EXPECT_EQ(repaired.slots[0], ReaderStatusBarItem::Battery);
  EXPECT_EQ(repaired.slots[1], ReaderStatusBarItem::Empty);
  EXPECT_EQ(repaired.slots[3], ReaderStatusBarItem::TitleBook);
  EXPECT_EQ(repaired.slots[4], ReaderStatusBarItem::Empty);
  EXPECT_EQ(repaired.progressBarThickness, 1);
}

TEST(ReaderStatusBarConfig, MigratesSavedSixSlotBarsAndResavesSevenSlots) {
  JsonDocument saved;
  JsonArray topSlots = saved["top"]["slots"].to<JsonArray>();
  for (const auto item : {ReaderStatusBarItem::Clock, ReaderStatusBarItem::Battery, ReaderStatusBarItem::TitleBook,
                          ReaderStatusBarItem::StablePageNumber, ReaderStatusBarItem::TimeLeftChapter,
                          ReaderStatusBarItem::BookProgressPercentage}) {
    topSlots.add(static_cast<int>(item));
  }
  saved["top"]["percentageFormat"] = 2;
  saved["top"]["progressBar"] = 0;
  saved["top"]["thickness"] = 2;

  JsonArray bottomSlots = saved["bottom"]["slots"].to<JsonArray>();
  for (const auto item : {ReaderStatusBarItem::Battery, ReaderStatusBarItem::TimeLeftBook,
                          ReaderStatusBarItem::TitleChapter, ReaderStatusBarItem::ChapterPageCount,
                          ReaderStatusBarItem::StablePageNumber, ReaderStatusBarItem::BookProgressPercentage}) {
    bottomSlots.add(static_cast<int>(item));
  }
  saved["bottom"]["percentageFormat"] = 1;
  saved["bottom"]["progressBar"] = 1;
  saved["bottom"]["thickness"] = 0;

  ReaderStatusBarConfig top;
  ReaderStatusBarConfig bottom;
  EXPECT_TRUE(repairReaderStatusBarJson(saved["top"], top, true, 3, 3, 3));
  EXPECT_TRUE(repairReaderStatusBarJson(saved["bottom"], bottom, true, 3, 3, 3));
  const std::array expectedTop{ReaderStatusBarItem::Clock,
                               ReaderStatusBarItem::Battery,
                               ReaderStatusBarItem::Empty,
                               ReaderStatusBarItem::TitleBook,
                               ReaderStatusBarItem::StablePageNumber,
                               ReaderStatusBarItem::TimeLeftChapter,
                               ReaderStatusBarItem::BookProgressPercentage};
  const std::array expectedBottom{ReaderStatusBarItem::Battery,
                                  ReaderStatusBarItem::TimeLeftBook,
                                  ReaderStatusBarItem::Empty,
                                  ReaderStatusBarItem::TitleChapter,
                                  ReaderStatusBarItem::ChapterPageCount,
                                  ReaderStatusBarItem::StablePageNumber,
                                  ReaderStatusBarItem::BookProgressPercentage};
  EXPECT_EQ(top.slots, expectedTop);
  EXPECT_EQ(bottom.slots, expectedBottom);
  EXPECT_EQ(top.percentageFormat, 2);
  EXPECT_EQ(top.progressBar, 0);
  EXPECT_EQ(top.progressBarThickness, 2);
  EXPECT_EQ(bottom.percentageFormat, 1);
  EXPECT_EQ(bottom.progressBar, 1);
  EXPECT_EQ(bottom.progressBarThickness, 0);

  JsonDocument resaved;
  writeReaderStatusBarJson(resaved["top"].to<JsonObject>(), top);
  writeReaderStatusBarJson(resaved["bottom"].to<JsonObject>(), bottom);
  EXPECT_EQ(resaved["top"]["slots"].as<JsonArrayConst>().size(), ReaderStatusBarConfig::SLOT_COUNT);
  EXPECT_EQ(resaved["bottom"]["slots"].as<JsonArrayConst>().size(), ReaderStatusBarConfig::SLOT_COUNT);
  EXPECT_FALSE(repairReaderStatusBarJson(resaved["top"], top, true, 3, 3, 3));
  EXPECT_FALSE(repairReaderStatusBarJson(resaved["bottom"], bottom, true, 3, 3, 3));
}

TEST(ReaderStatusBarConfig, MigratesValidSixSlotItemsWhenAnotherItemIsInvalid) {
  JsonDocument saved;
  JsonArray slots = saved["slots"].to<JsonArray>();
  for (const int item : {2, 1, 9, 5, 99, 7}) slots.add(item);
  saved["progressBar"] = 0;

  ReaderStatusBarConfig config;
  EXPECT_TRUE(repairReaderStatusBarJson(saved.as<JsonVariantConst>(), config, false, 3, 3, 3));
  EXPECT_EQ(config.slots[0], ReaderStatusBarItem::Battery);
  EXPECT_EQ(config.slots[1], ReaderStatusBarItem::Empty);
  EXPECT_EQ(config.slots[2], ReaderStatusBarItem::Empty);
  EXPECT_EQ(config.slots[3], ReaderStatusBarItem::TitleChapter);
  EXPECT_EQ(config.slots[4], ReaderStatusBarItem::ChapterPageCount);
  EXPECT_EQ(config.slots[5], ReaderStatusBarItem::Empty);
  EXPECT_EQ(config.slots[6], ReaderStatusBarItem::BookProgressPercentage);
  EXPECT_EQ(config.progressBar, 0);
}

TEST(ReaderStatusBarConfig, UnknownLengthSavedBarKeepsCurrentDefault) {
  JsonDocument saved;
  JsonArray slots = saved["slots"].to<JsonArray>();
  for (int i = 0; i < 5; ++i) slots.add(static_cast<int>(ReaderStatusBarItem::Battery));
  ReaderStatusBarConfig config;
  config.slots[ReaderStatusBarConfig::CENTER] = ReaderStatusBarItem::TitleChapter;

  EXPECT_TRUE(repairReaderStatusBarJson(saved.as<JsonVariantConst>(), config, true, 3, 3, 3));
  EXPECT_EQ(config.slots[ReaderStatusBarConfig::LEFT_THIRD], ReaderStatusBarItem::Empty);
  EXPECT_EQ(config.slots[ReaderStatusBarConfig::CENTER], ReaderStatusBarItem::TitleChapter);
}

TEST(ReaderStatusBarLayout, TopTextUsesHomeInsetAndOnlyEnabledProgressReservesSpace) {
  constexpr int textLane = 19;
  EXPECT_EQ(readerStatusBarTotalHeight(ReaderStatusBarPosition::Top, true, 0, textLane), 21);
  EXPECT_EQ(readerStatusBarTotalHeight(ReaderStatusBarPosition::Top, true, 7, textLane), 28);
  EXPECT_EQ(readerStatusBarTotalHeight(ReaderStatusBarPosition::Top, false, 0, textLane), 0);
  EXPECT_EQ(readerStatusBarTotalHeight(ReaderStatusBarPosition::Top, false, 7, textLane), 7);
  // Bottom text and progress-only bookmark reservations retain their existing bounds.
  EXPECT_EQ(readerStatusBarTotalHeight(ReaderStatusBarPosition::Bottom, true, 0, textLane), 19);
  EXPECT_EQ(readerStatusBarTotalHeight(ReaderStatusBarPosition::Bottom, false, 7, textLane), 14);
}

TEST(ReaderStatusBarConfig, DateKeepsExistingItemIdsAndRequiresClock) {
  EXPECT_EQ(static_cast<int>(ReaderStatusBarItem::TitleChapter), 9);
  EXPECT_EQ(static_cast<int>(ReaderStatusBarItem::Date), 10);
  EXPECT_TRUE(validReaderStatusBarItemValue(10, true));
  EXPECT_FALSE(validReaderStatusBarItemValue(10, false));
  ReaderStatusBarConfig config;
  config.slots[ReaderStatusBarConfig::CENTER] = ReaderStatusBarItem::Date;
  EXPECT_TRUE(config.hasTextItems(true));
  EXPECT_FALSE(config.hasTextItems(false));
  JsonDocument json;
  writeReaderStatusBarJson(json.to<JsonObject>(), config);
  ReaderStatusBarConfig loaded;
  EXPECT_TRUE(readReaderStatusBarJson(json.as<JsonVariantConst>(), loaded, true, 3, 3, 3));
  EXPECT_EQ(loaded.slots, config.slots);
}

TEST(DisplayStatusBarConfig, MapsThreeIndependentPositionsWithoutProgress) {
  DisplayStatusBarConfig display;
  display.slots = {ReaderStatusBarItem::Battery, ReaderStatusBarItem::Date, ReaderStatusBarItem::Clock};
  const auto config = display.asReaderConfig();
  EXPECT_EQ(config.slots[0], ReaderStatusBarItem::Battery);
  EXPECT_EQ(config.slots[3], ReaderStatusBarItem::Date);
  EXPECT_EQ(config.slots[4], ReaderStatusBarItem::Clock);
  EXPECT_EQ(config.slots[1], ReaderStatusBarItem::Empty);
  EXPECT_EQ(config.slots[6], ReaderStatusBarItem::Empty);
  EXPECT_EQ(config.progressBar, 2);
}

TEST(DisplayStatusBarConfig, JsonRejectsReaderItemsAndLeavesPreviousSettingsIntact) {
  DisplayStatusBarConfig display;
  JsonDocument json;
  ASSERT_FALSE(deserializeJson(json, "[10,1,2]"));
  ASSERT_TRUE(readDisplayStatusBarJson(json.as<JsonVariantConst>(), display, true));
  const auto previous = display.slots;
  for (const char* invalid : {"[3,1,2]", "[10,1]", "[10,1,2,0]", "[10,1,null]", "[10,1,\"2\"]"}) {
    ASSERT_FALSE(deserializeJson(json, invalid));
    EXPECT_FALSE(readDisplayStatusBarJson(json.as<JsonVariantConst>(), display, true));
    EXPECT_EQ(display.slots, previous);
  }
  ASSERT_FALSE(deserializeJson(json, "[0,0,0]"));
  EXPECT_TRUE(readDisplayStatusBarJson(json.as<JsonVariantConst>(), display, true));
}

TEST(DisplayStatusBarConfig, ClockAndDateRequireRtcButEmptyAndBatteryDoNot) {
  for (const auto item : {ReaderStatusBarItem::Clock, ReaderStatusBarItem::Date}) {
    EXPECT_TRUE(validDisplayStatusBarItemValue(static_cast<int>(item), true));
    EXPECT_FALSE(validDisplayStatusBarItemValue(static_cast<int>(item), false));
  }
  for (const auto item : {ReaderStatusBarItem::Empty, ReaderStatusBarItem::Battery}) {
    EXPECT_TRUE(validDisplayStatusBarItemValue(static_cast<int>(item), false));
  }
}

TEST(DisplayStatusBarConfig, JsonRejectsClockAndDateWithoutRtcInEveryPosition) {
  DisplayStatusBarConfig display;
  const auto previous = display.slots;
  JsonDocument json;
  for (const char* invalid : {"[1,0,2]", "[0,1,2]", "[0,0,1]", "[10,0,2]", "[0,10,2]", "[0,0,10]"}) {
    ASSERT_FALSE(deserializeJson(json, invalid));
    EXPECT_FALSE(readDisplayStatusBarJson(json.as<JsonVariantConst>(), display, false));
    EXPECT_EQ(display.slots, previous);
  }
  ASSERT_FALSE(deserializeJson(json, "[2,0,2]"));
  EXPECT_TRUE(readDisplayStatusBarJson(json.as<JsonVariantConst>(), display, false));
  EXPECT_EQ(display.slots[0], ReaderStatusBarItem::Battery);
  EXPECT_EQ(display.slots[1], ReaderStatusBarItem::Empty);
  EXPECT_EQ(display.slots[2], ReaderStatusBarItem::Battery);
}

TEST(ReaderStatusBarConfig, HideRoundTripsIndependentlyWithoutClearingAssignments) {
  for (unsigned mask = 0; mask < 4; ++mask) {
    ReaderStatusBarsPayload original;
    original.top = migrateBottomStatusBar({true, true, true, 1, 2, true, 2, 0, 2});
    original.bottom = original.top;
    original.top.hidden = mask & 1;
    original.bottom.hidden = mask & 2;
    JsonDocument saved;
    writeReaderStatusBarJson(saved["top"].to<JsonObject>(), original.top);
    writeReaderStatusBarJson(saved["bottom"].to<JsonObject>(), original.bottom);
    saved["xtcMode"] = 3;
    ReaderStatusBarsPayload loaded;
    ASSERT_TRUE(readReaderStatusBarsPayload(saved.as<JsonVariantConst>(), loaded, true, 3, 3, 3, 4));
    EXPECT_EQ(loaded.top.hidden, original.top.hidden);
    EXPECT_EQ(loaded.bottom.hidden, original.bottom.hidden);
    EXPECT_EQ(loaded.top.slots, original.top.slots);
    EXPECT_EQ(loaded.bottom.slots, original.bottom.slots);
    EXPECT_EQ(loaded.top.progressBar, 0);
    EXPECT_EQ(loaded.bottom.progressBarThickness, 2);
    loaded.top.hidden = false;
    EXPECT_EQ(loaded.top.slots, original.top.slots);
  }
}

TEST(ReaderStatusBarConfig, MissingHideDefaultsVisibleAndMalformedHideIsAtomic) {
  ReaderStatusBarConfig original = migrateBottomStatusBar({true, true, true, 1, 2, true, 2, 0, 2});
  JsonDocument saved;
  writeReaderStatusBarJson(saved.to<JsonObject>(), original);
  saved.remove("hidden");
  ReaderStatusBarConfig parsed;
  parsed.hidden = true;
  ASSERT_TRUE(readReaderStatusBarJson(saved.as<JsonVariantConst>(), parsed, true, 3, 3, 3));
  EXPECT_FALSE(parsed.hidden);
  EXPECT_EQ(parsed.slots, original.slots);
  for (const char* raw : {"1", "null", "\"true\"", "[]"}) {
    JsonDocument bad;
    ASSERT_FALSE(deserializeJson(bad, raw));
    saved["hidden"] = bad.as<JsonVariantConst>();
    parsed.hidden = true;
    EXPECT_FALSE(readReaderStatusBarJson(saved.as<JsonVariantConst>(), parsed, true, 3, 3, 3));
    EXPECT_TRUE(parsed.hidden);
    EXPECT_TRUE(repairReaderStatusBarJson(saved.as<JsonVariantConst>(), parsed, true, 3, 3, 3));
    EXPECT_FALSE(parsed.hidden);
    EXPECT_EQ(parsed.slots, original.slots);
    EXPECT_EQ(parsed.progressBarThickness, original.progressBarThickness);
  }
}

TEST(ReaderStatusBarConfig, LegacyXtcHideBelongsToDisplayedBar) {
  ReaderStatusBarConfig top;
  ReaderStatusBarConfig bottom = migrateBottomStatusBar({true, true, true, 1, 2, true, 2, 0, 2});
  for (unsigned mask = 0; mask < 4; ++mask) {
    top.hidden = mask & 1;
    bottom.hidden = mask & 2;
    const auto displayedTop = xtcStatusBarConfigForDisplay(ReaderStatusBarPosition::Top, true, top, bottom);
    const auto displayedBottom = xtcStatusBarConfigForDisplay(ReaderStatusBarPosition::Bottom, true, top, bottom);
    EXPECT_EQ(displayedTop.slots, bottom.slots);
    EXPECT_EQ(displayedTop.hidden, top.hidden);
    EXPECT_EQ(displayedBottom.hidden, bottom.hidden);
  }
}

TEST(ReaderStatusBarConfig, BatteryStyleIsOptionalAndValidated) {
  ReaderStatusBarConfig original;
  original.batteryStyle = ReaderStatusBarBatteryStyle::PercentOnly;
  JsonDocument saved;
  writeReaderStatusBarJson(saved.to<JsonObject>(), original);
  ReaderStatusBarConfig parsed;
  ASSERT_TRUE(readReaderStatusBarJson(saved.as<JsonVariantConst>(), parsed, true, 3, 3, 3));
  EXPECT_EQ(parsed.batteryStyle, ReaderStatusBarBatteryStyle::PercentOnly);

  // Documents written before battery styles parse with the default style.
  saved.remove("battery");
  ASSERT_TRUE(readReaderStatusBarJson(saved.as<JsonVariantConst>(), parsed, true, 3, 3, 3));
  EXPECT_EQ(parsed.batteryStyle, ReaderStatusBarBatteryStyle::IconAndPercent);

  for (const char* raw : {"3", "-1", "\"2\"", "true", "null"}) {
    JsonDocument bad;
    ASSERT_FALSE(deserializeJson(bad, raw));
    saved["battery"] = bad.as<JsonVariantConst>();
    parsed.batteryStyle = ReaderStatusBarBatteryStyle::IconOnly;
    EXPECT_FALSE(readReaderStatusBarJson(saved.as<JsonVariantConst>(), parsed, true, 3, 3, 3)) << raw;
    EXPECT_EQ(parsed.batteryStyle, ReaderStatusBarBatteryStyle::IconOnly);
    EXPECT_TRUE(repairReaderStatusBarJson(saved.as<JsonVariantConst>(), parsed, true, 3, 3, 3));
    EXPECT_EQ(parsed.batteryStyle, ReaderStatusBarBatteryStyle::IconAndPercent);
  }
}

TEST(ReaderStatusBarConfig, DisplaySlotsKeepBatteryStyle) {
  DisplayStatusBarConfig display;
  display.batteryStyle = ReaderStatusBarBatteryStyle::PercentOnly;
  JsonDocument slots;
  ASSERT_FALSE(deserializeJson(slots, "[0,0,2]"));
  ASSERT_TRUE(readDisplayStatusBarJson(slots.as<JsonVariantConst>(), display, false));
  EXPECT_EQ(display.batteryStyle, ReaderStatusBarBatteryStyle::PercentOnly);
  EXPECT_EQ(display.asReaderConfig().batteryStyle, ReaderStatusBarBatteryStyle::PercentOnly);
}

TEST(ReaderStatusBarConfig, ChapterPercentageKeepsSavedItemIdsAndRoundTripsBothBars) {
  EXPECT_EQ(static_cast<int>(ReaderStatusBarItem::BookProgressPercentage), 7);
  EXPECT_EQ(static_cast<int>(ReaderStatusBarItem::Date), 10);
  const auto item = ReaderStatusBarItem::ChapterProgressPercentage;
  EXPECT_TRUE(validReaderStatusBarItemValue(static_cast<int>(item), false));
  EXPECT_FALSE(validDisplayStatusBarItemValue(static_cast<int>(item), true));
  ReaderStatusBarsPayload outgoing;
  outgoing.top.slots[0] = item;
  outgoing.bottom.slots[6] = item;
  outgoing.top.percentageFormat = 2;
  outgoing.bottom.percentageFormat = 1;
  JsonDocument saved;
  writeReaderStatusBarJson(saved["top"].to<JsonObject>(), outgoing.top);
  writeReaderStatusBarJson(saved["bottom"].to<JsonObject>(), outgoing.bottom);
  saved["xtcMode"] = 3;
  ReaderStatusBarsPayload loaded;
  ASSERT_TRUE(readReaderStatusBarsPayload(saved.as<JsonVariantConst>(), loaded, false, 3, 3, 3, 4));
  EXPECT_EQ(loaded.top.slots, outgoing.top.slots);
  EXPECT_EQ(loaded.bottom.slots, outgoing.bottom.slots);
  EXPECT_EQ(loaded.top.percentageFormat, 2);
  EXPECT_EQ(loaded.bottom.percentageFormat, 1);
}

namespace {
using SlotWidths = std::array<int, ReaderStatusBarConfig::SLOT_COUNT>;
constexpr int SEP_WIDTH = 6;
constexpr int JOIN = SEP_WIDTH + 12;  // Separator plus 6 px on each side.
}  // namespace

TEST(CenteredReaderStatusBar, GroupIsCenteredOnCenterXWithSeparatorsBetweenItemsOnly) {
  const SlotWidths widths{40, 0, 30, 0, 20, 0, 0};
  const auto g = layoutCenteredReaderStatusBarItems(10, 790, 400, widths, SEP_WIDTH);
  EXPECT_EQ(g.separatorCount, 2);
  EXPECT_EQ(g.totalWidth, 90 + 2 * JOIN);
  EXPECT_EQ(g.x[0], 400 - g.totalWidth / 2);
  EXPECT_EQ(g.separatorX[0], g.x[0] + 40 + 6);
  EXPECT_EQ(g.x[2], g.x[0] + 40 + JOIN);
  EXPECT_EQ(g.separatorX[1], g.x[2] + 30 + 6);
  EXPECT_EQ(g.x[4], g.x[2] + 30 + JOIN);
  EXPECT_EQ(g.x[4] + 20 - g.x[0], g.totalWidth);
}

TEST(CenteredReaderStatusBar, SingleItemHasNoSeparatorAndEmptySlotsAreSkipped) {
  const SlotWidths widths{0, 0, 0, 50, 0, 0, 0};
  const auto g = layoutCenteredReaderStatusBarItems(10, 790, 400, widths, SEP_WIDTH);
  EXPECT_EQ(g.separatorCount, 0);
  EXPECT_EQ(g.x[3], 375);
  const auto none = layoutCenteredReaderStatusBarItems(10, 790, 400, SlotWidths{}, SEP_WIDTH);
  EXPECT_EQ(none.separatorCount, 0);
  EXPECT_EQ(none.totalWidth, 0);
}

TEST(CenteredReaderStatusBar, CenterSlotParticipatesInSlotOrder) {
  const SlotWidths widths{20, 0, 0, 20, 0, 0, 20};
  const auto g = layoutCenteredReaderStatusBarItems(0, 800, 400, widths, SEP_WIDTH);
  EXPECT_LT(g.x[0], g.x[3]);
  EXPECT_LT(g.x[3], g.x[6]);
  EXPECT_EQ(g.separatorCount, 2);
}

TEST(CenteredReaderStatusBar, OverflowTrimsWidestItemFirst) {
  const SlotWidths widths{40, 0, 0, 700, 0, 0, 40};
  const auto g = layoutCenteredReaderStatusBarItems(10, 790, 400, widths, SEP_WIDTH);
  EXPECT_EQ(g.widths[0], 40);
  EXPECT_EQ(g.widths[6], 40);
  EXPECT_EQ(g.widths[3], 780 - 80 - 2 * JOIN);
  EXPECT_LE(g.totalWidth, 780);
  EXPECT_GE(g.x[0], 10);
  EXPECT_LE(g.x[6] + g.widths[6], 790);
}

TEST(CenteredReaderStatusBar, GroupStaysRightOfBookmarkReserveAndInsideRightEdge) {
  const SlotWidths narrow{0, 0, 0, 100, 0, 0, 0};
  const auto left = layoutCenteredReaderStatusBarItems(20, 800, 60, narrow, SEP_WIDTH, 13);
  EXPECT_EQ(left.x[3], 33);
  const auto right = layoutCenteredReaderStatusBarItems(20, 800, 790, narrow, SEP_WIDTH, 13);
  EXPECT_EQ(right.x[3], 700);
  const SlotWidths wide{0, 0, 0, 900, 0, 0, 0};
  const auto trimmed = layoutCenteredReaderStatusBarItems(20, 800, 400, wide, SEP_WIDTH, 13);
  EXPECT_EQ(trimmed.x[3], 33);
  EXPECT_EQ(trimmed.widths[3], 767);
}

TEST(CenteredReaderStatusBar, CustomCenterXIsRespected) {
  const SlotWidths widths{0, 0, 0, 100, 0, 0, 0};
  const auto g = layoutCenteredReaderStatusBarItems(10, 790, 300, widths, SEP_WIDTH);
  EXPECT_EQ(g.x[3], 250);
}

TEST(CenteredReaderStatusBar, SeparatorGlyphsMatchPersistedValues) {
  EXPECT_STREQ(readerStatusBarSeparatorGlyph(ReaderStatusBarSeparator::Bullet), "\xE2\x80\xA2");
  EXPECT_STREQ(readerStatusBarSeparatorGlyph(ReaderStatusBarSeparator::Dash), "\xE2\x80\x93");
  EXPECT_STREQ(readerStatusBarSeparatorGlyph(ReaderStatusBarSeparator::Pipe), "|");
  EXPECT_STREQ(readerStatusBarSeparatorGlyph(ReaderStatusBarSeparator::MiddleDot), "\xC2\xB7");
  EXPECT_STREQ(readerStatusBarSeparatorGlyph(ReaderStatusBarSeparator::Count), "\xE2\x80\xA2");
}

TEST(CenteredReaderStatusBar, JsonRoundTripsCenteredAndSeparator) {
  ReaderStatusBarConfig original = migrateBottomStatusBar({true, true, true, 1, 2, true, 2, 0, 2});
  original.centered = true;
  original.separator = ReaderStatusBarSeparator::Pipe;
  JsonDocument saved;
  writeReaderStatusBarJson(saved.to<JsonObject>(), original);
  ReaderStatusBarConfig loaded;
  ASSERT_TRUE(readReaderStatusBarJson(saved.as<JsonVariantConst>(), loaded, true, 3, 3, 3));
  EXPECT_TRUE(loaded.centered);
  EXPECT_EQ(loaded.separator, ReaderStatusBarSeparator::Pipe);
  EXPECT_EQ(loaded.slots, original.slots);
}

TEST(CenteredReaderStatusBar, MiddleDotRoundTripsWithoutChangingExistingValues) {
  EXPECT_EQ(static_cast<unsigned>(ReaderStatusBarSeparator::Bullet), 0u);
  EXPECT_EQ(static_cast<unsigned>(ReaderStatusBarSeparator::Dash), 1u);
  EXPECT_EQ(static_cast<unsigned>(ReaderStatusBarSeparator::Pipe), 2u);
  ReaderStatusBarConfig original;
  original.centered = true;
  original.separator = ReaderStatusBarSeparator::MiddleDot;
  JsonDocument saved;
  writeReaderStatusBarJson(saved.to<JsonObject>(), original);
  EXPECT_EQ(saved["separator"].as<unsigned>(), 3u);
  ReaderStatusBarConfig loaded;
  ASSERT_TRUE(readReaderStatusBarJson(saved.as<JsonVariantConst>(), loaded, true, 3, 3, 3));
  EXPECT_EQ(loaded.separator, ReaderStatusBarSeparator::MiddleDot);
  EXPECT_TRUE(loaded.centered);
}

TEST(CenteredReaderStatusBar, MissingKeysDefaultAndInvalidValuesRejectThenRepair) {
  ReaderStatusBarConfig original = migrateBottomStatusBar({true, true, true, 1, 2, true, 2, 0, 2});
  original.centered = true;
  original.separator = ReaderStatusBarSeparator::Dash;
  JsonDocument saved;
  writeReaderStatusBarJson(saved.to<JsonObject>(), original);
  saved.remove("centered");
  saved.remove("separator");
  ReaderStatusBarConfig parsed;
  parsed.centered = true;
  parsed.separator = ReaderStatusBarSeparator::Pipe;
  ASSERT_TRUE(readReaderStatusBarJson(saved.as<JsonVariantConst>(), parsed, true, 3, 3, 3));
  EXPECT_FALSE(parsed.centered);
  EXPECT_EQ(parsed.separator, ReaderStatusBarSeparator::Bullet);

  for (const char* raw : {"4", "-1", "\"1\"", "null", "true"}) {
    JsonDocument bad;
    ASSERT_FALSE(deserializeJson(bad, raw));
    writeReaderStatusBarJson(saved.to<JsonObject>(), original);
    saved["separator"] = bad.as<JsonVariantConst>();
    EXPECT_FALSE(readReaderStatusBarJson(saved.as<JsonVariantConst>(), parsed, true, 3, 3, 3)) << raw;
    EXPECT_TRUE(repairReaderStatusBarJson(saved.as<JsonVariantConst>(), parsed, true, 3, 3, 3)) << raw;
    EXPECT_TRUE(parsed.centered);
    EXPECT_EQ(parsed.separator, ReaderStatusBarSeparator::Bullet);
    EXPECT_EQ(parsed.slots, original.slots);
  }
  for (const char* raw : {"1", "null", "\"true\"", "[]"}) {
    JsonDocument bad;
    ASSERT_FALSE(deserializeJson(bad, raw));
    writeReaderStatusBarJson(saved.to<JsonObject>(), original);
    saved["centered"] = bad.as<JsonVariantConst>();
    EXPECT_FALSE(readReaderStatusBarJson(saved.as<JsonVariantConst>(), parsed, true, 3, 3, 3)) << raw;
    EXPECT_TRUE(repairReaderStatusBarJson(saved.as<JsonVariantConst>(), parsed, true, 3, 3, 3)) << raw;
    EXPECT_FALSE(parsed.centered);
    EXPECT_EQ(parsed.separator, ReaderStatusBarSeparator::Dash);
  }
}

TEST(CenteredReaderStatusBar, WebPayloadRoundTripsCenteredPerBar) {
  ReaderStatusBarsPayload original;
  original.top = migrateBottomStatusBar({true, true, true, 1, 2, true, 2, 0, 2});
  original.bottom = original.top;
  original.top.centered = true;
  original.top.separator = ReaderStatusBarSeparator::Dash;
  JsonDocument saved;
  writeReaderStatusBarJson(saved["top"].to<JsonObject>(), original.top);
  writeReaderStatusBarJson(saved["bottom"].to<JsonObject>(), original.bottom);
  saved["xtcMode"] = 1;
  ReaderStatusBarsPayload loaded;
  ASSERT_TRUE(readReaderStatusBarsPayload(saved.as<JsonVariantConst>(), loaded, true, 3, 3, 3, 4));
  EXPECT_TRUE(loaded.top.centered);
  EXPECT_EQ(loaded.top.separator, ReaderStatusBarSeparator::Dash);
  EXPECT_FALSE(loaded.bottom.centered);
  EXPECT_EQ(loaded.bottom.separator, ReaderStatusBarSeparator::Bullet);
}
