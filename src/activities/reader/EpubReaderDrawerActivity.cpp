#include "EpubReaderDrawerActivity.h"

#include <Epub/Page.h>
#include <FontCacheManager.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <SdCardFontSystem.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DeviceCapabilities.h"
#include "FilenameFontSystem.h"
#include "MappedInputManager.h"
#include "ReaderFontLoading.h"
#include "ReaderUtils.h"
#include "SettingsList.h"
#include "StablePageSelectionModel.h"
#include "activities/reader/ControlsOptionsActivity.h"
#include "activities/settings/StatusBarSettingsActivity.h"
#include "util/ImageDisplayMode.h"
#if CROSSINK_SCALABLE_FONTS
#include "activities/settings/TtfRenderOptionsActivity.h"
#endif
#include "components/DrawerHandle.h"
#include "components/ReaderBookSummary.h"
#include "components/ReaderSliderHints.h"
#include "components/SliderValue.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/keyboardIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/touchHeaderIcons.h"
#include "fontIds.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"
#include "util/FontFamilyLabel.h"
#include "util/ReaderBookProgress.h"

namespace fui = freeink::ui;

namespace {
constexpr int16_t TAB_BAR_HEIGHT = 58;
constexpr int16_t SLIDER_CONTROL_HEIGHT = 30;
constexpr int16_t SLIDER_CAPTION_GAP = 15;
constexpr int16_t COMPACT_SLIDER_CAPTION_GAP = 5;
constexpr int16_t SLIDER_SCALE_GAP = 4;
constexpr int16_t BUTTON_SLIDER_FOCUS_PADDING_X = 10;
constexpr int16_t BUTTON_SLIDER_FOCUS_PADDING_Y = 4;
constexpr int16_t DRAWER_SIDE_INSET = 16;
constexpr int16_t DRAWER_SCROLLBAR_RIGHT_INSET = 4;
constexpr int16_t DRAWER_LIST_TOP_PADDING = 5;
constexpr int16_t DRAWER_RULE_WIDTH = 3;
constexpr int16_t TAB_BAR_VERTICAL_PADDING = 4;
constexpr int16_t FONT_FAMILY_ROW_HEIGHT_REDUCTION = 8;
constexpr int16_t BACK_CARET_VISUAL_INSET = 5;
constexpr int16_t BACK_ICON_VISIBLE_LEFT_INSET = 7;
constexpr int16_t BACK_ICON_SIZE = 32;
constexpr int16_t BACK_CARET_HIT_WIDTH = 64;
constexpr int LANDSCAPE_ROOT_ROWS = 4;
// Sentinel ACTION_KEYPAD_KEY values for the grid's two non-digit keys; digits use
// their own 0-9 value. Backspace is a separate button (ACTION_KEYPAD_BACKSPACE),
// not a grid key, so it does not need a sentinel here.
constexpr int16_t KEYPAD_DOT = -1;
constexpr int16_t KEYPAD_OK = -2;
constexpr unsigned long PERCENT_KEYPAD_LONG_PRESS_MS = 1000;
constexpr uint8_t PORTRAIT_DRAWER_HEIGHT_PERCENT = 50;
// Non-root landscape panes retain a little more room than portrait. Root
// height is calculated from the rows reserved in drawerHeight().
constexpr uint8_t LANDSCAPE_DRAWER_HEIGHT_PERCENT = 65;
constexpr uint8_t LANDSCAPE_DUAL_SLIDER_DRAWER_HEIGHT_PERCENT = 75;
// Button-device landscape preview panes split the body: controls left, sample right.
constexpr uint8_t LANDSCAPE_SAMPLE_PREVIEW_WIDTH_PERCENT = 55;

bool isLandscapeOrientation(const GfxRenderer::Orientation orientation) {
  return orientation == GfxRenderer::Orientation::LandscapeClockwise ||
         orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
}

int16_t readerDrawerHeight(const GfxRenderer& renderer, const ReaderDrawerPane pane) {
  int heightPercent = PORTRAIT_DRAWER_HEIGHT_PERCENT;
  if (isLandscapeOrientation(renderer.getOrientation())) {
    heightPercent = readerDrawerNeedsTallLandscapeSheet(pane) ? LANDSCAPE_DUAL_SLIDER_DRAWER_HEIGHT_PERCENT
                                                              : LANDSCAPE_DRAWER_HEIGHT_PERCENT;
  }
  return static_cast<int16_t>(renderer.getScreenHeight() * heightPercent / 100);
}

StrId readerOrientationLabel(const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::PORTRAIT:
      return StrId::STR_PORTRAIT;
    case CrossPointSettings::LANDSCAPE_CW:
      return StrId::STR_LANDSCAPE_CW;
    case CrossPointSettings::INVERTED:
      return StrId::STR_ORIENTATION_INVERTED;
    case CrossPointSettings::LANDSCAPE_CCW:
      return StrId::STR_LANDSCAPE_CCW;
    default:
      return StrId::STR_PORTRAIT;
  }
}

struct ReaderSliderRowProps {
  const char* label = nullptr;
  const char* value = nullptr;
  const char* minimumLabel = nullptr;
  const char* maximumLabel = nullptr;
  int32_t sliderValue = 0;
  int32_t max = 100;
  fui::ActionId sliderAction = fui::NO_ACTION;
  fui::ActionId decrement = fui::NO_ACTION;
  fui::ActionId increment = fui::NO_ACTION;
  int16_t captionGap = SLIDER_CAPTION_GAP;
  int16_t inlineLabelWidth = 0;
  bool compact = false;
  bool focused = false;
  bool editing = false;
  bool enabled = true;
};

void configureReaderSliderScale(ReaderSliderRowProps& row, const char* minimum, const char* maximum) {
  row.minimumLabel = minimum;
  row.maximumLabel = maximum;
}

int16_t readerSliderRowHeight(const fui::DrawTarget& target, const fui::ThemeTokens& theme,
                              const ReaderSliderRowProps& row) {
  const int16_t lineHeight = target.lineHeight(theme.bodyText.font);
  const int16_t captionHeight =
      row.label && row.inlineLabelWidth == 0 ? static_cast<int16_t>(lineHeight + row.captionGap) : 0;
  const int16_t currentValueHeight =
      row.value && !row.compact ? static_cast<int16_t>(lineHeight + SLIDER_SCALE_GAP) : 0;
  const int16_t endpointHeight =
      (row.minimumLabel || row.maximumLabel) && !row.compact ? static_cast<int16_t>(lineHeight + SLIDER_SCALE_GAP) : 0;
  return static_cast<int16_t>(captionHeight + currentValueHeight + SLIDER_CONTROL_HEIGHT + endpointHeight);
}

template <size_t MaxInteractions>
int16_t readerSliderRowHeight(fui::Screen<MaxInteractions>& screen, const ReaderSliderRowProps& row) {
  return readerSliderRowHeight(screen.target(), screen.theme(), row);
}

// Use the same label column for both controls, including translated labels.
template <size_t MaxInteractions>
void alignReaderSliderLabels(fui::Screen<MaxInteractions>& screen, ReaderSliderRowProps& first,
                             ReaderSliderRowProps& second) {
  const auto& style = screen.theme().bodyText;
  const int16_t labelWidth = std::max(screen.target().measureText(style.font, first.label, style).width,
                                      screen.target().measureText(style.font, second.label, style).width);
  // Leave a usable track and both step buttons even with longer translations.
  const int16_t controlWidth =
      static_cast<int16_t>(2 * screen.theme().rowHeight + screen.theme().spaceSm * 2 + screen.theme().minTouchSize);
  first.inlineLabelWidth = second.inlineLabelWidth =
      std::max<int16_t>(1, std::min<int16_t>(labelWidth, screen.body().width - controlWidth - screen.theme().spaceMd));
}

template <size_t MaxInteractions>
int16_t readerSliderControlTopInset(fui::Screen<MaxInteractions>& screen, const ReaderSliderRowProps& row) {
  const int16_t lineHeight = screen.target().lineHeight(screen.theme().bodyText.font);
  const int16_t captionHeight =
      row.label && row.inlineLabelWidth == 0 ? static_cast<int16_t>(lineHeight + row.captionGap) : 0;
  const int16_t currentValueHeight =
      row.value && !row.compact ? static_cast<int16_t>(lineHeight + SLIDER_SCALE_GAP) : 0;
  return static_cast<int16_t>(captionHeight + currentValueHeight);
}

template <size_t MaxInteractions>
void drawReaderSliderRow(fui::Screen<MaxInteractions>& screen, const ReaderSliderRowProps& row) {
  const int16_t height = readerSliderRowHeight(screen, row);
  const fui::Rect rect = screen.takeTop(height);
  fui::TextStyle labelStyle = screen.theme().bodyText;
  labelStyle.bold = row.editing;
  fui::TextStyle valueStyle = screen.theme().bodyText;
  valueStyle.bold = true;
  if (row.focused) {
    const fui::Rect focusRect{static_cast<int16_t>(rect.x - BUTTON_SLIDER_FOCUS_PADDING_X),
                              static_cast<int16_t>(rect.y - BUTTON_SLIDER_FOCUS_PADDING_Y),
                              static_cast<int16_t>(rect.width + BUTTON_SLIDER_FOCUS_PADDING_X * 2),
                              static_cast<int16_t>(rect.height + BUTTON_SLIDER_FOCUS_PADDING_Y * 2)};
    if (row.editing)
      screen.target().fill(focusRect, fui::Paint::dither(fui::Color::LightGray), 4);
    else
      screen.target().stroke(focusRect, fui::Paint::solid(fui::Color::Black), 1, 4);
  }

  const int16_t lineHeight = screen.target().lineHeight(labelStyle.font);
  const int16_t captionHeight =
      row.label && row.inlineLabelWidth == 0 ? static_cast<int16_t>(lineHeight + row.captionGap) : 0;
  int16_t labelWidth = rect.width;
  if (row.compact && row.value) {
    const fui::Size valueSize = screen.target().measureText(valueStyle.font, row.value, valueStyle);
    labelWidth = std::max<int16_t>(0, rect.width - valueSize.width - screen.theme().spaceSm);
    screen.target().text(
        fui::Rect{static_cast<int16_t>(rect.right() - valueSize.width), rect.y, valueSize.width, lineHeight}, row.value,
        valueStyle);
  }
  if (row.label && row.inlineLabelWidth == 0)
    screen.target().text(fui::Rect{rect.x, rect.y, labelWidth, lineHeight}, row.label, labelStyle);

  const int16_t currentValueHeight =
      row.value && !row.compact ? static_cast<int16_t>(lineHeight + SLIDER_SCALE_GAP) : 0;
  const int16_t controlTop = static_cast<int16_t>(rect.y + captionHeight + currentValueHeight);
  const int16_t labelInset =
      row.inlineLabelWidth > 0 ? static_cast<int16_t>(row.inlineLabelWidth + screen.theme().spaceMd) : 0;
  const fui::Rect band{static_cast<int16_t>(rect.x + labelInset), controlTop,
                       static_cast<int16_t>(rect.width - labelInset), SLIDER_CONTROL_HEIGHT};
  if (row.inlineLabelWidth > 0)
    screen.target().text(fui::Rect{rect.x, band.y, row.inlineLabelWidth, band.height}, row.label, labelStyle);
  // Match the frontlight slider's control lanes: a shorter track leaves
  // genuinely finger-sized +/- targets at either end, even on the Sticky.
  int16_t stepWidth = std::max<int16_t>(band.height, screen.theme().rowHeight);
  const auto& endpointText = row.compact ? screen.theme().smallText : screen.theme().bodyText;
  if (row.minimumLabel)
    stepWidth =
        std::max(stepWidth, screen.target().measureText(endpointText.font, row.minimumLabel, endpointText).width);
  if (row.maximumLabel)
    stepWidth =
        std::max(stepWidth, screen.target().measureText(endpointText.font, row.maximumLabel, endpointText).width);
  const int16_t sideGap = static_cast<int16_t>(stepWidth + screen.theme().spaceSm);
  // In the narrow button landscape column, put the range at the track ends
  // and the current value beside its caption. Keyboard step hints remain below.
  if (row.compact) {
    fui::TextStyle endpointStyle = screen.theme().smallText;
    endpointStyle.align = fui::TextAlign::Center;
    if (row.minimumLabel)
      screen.target().text(fui::Rect{band.x, band.y, stepWidth, band.height}, row.minimumLabel, endpointStyle);
    if (row.maximumLabel)
      screen.target().text(fui::Rect{static_cast<int16_t>(band.right() - stepWidth), band.y, stepWidth, band.height},
                           row.maximumLabel, endpointStyle);
  } else {
    fui::ButtonProps step;
    step.text = screen.theme().bodyText;
    step.text.bold = true;
    step.styles = fui::plainStyles();
    step.inputMask = fui::InputTouch;
    step.enabled = row.enabled;
    step.minTouchSize = stepWidth;
    step.label = "-";
    step.action = row.decrement;
    step.value = -1;
    step.hitPadding.right = screen.theme().spaceSm;
    fui::button(screen.frame(), fui::Rect{band.x, band.y, stepWidth, band.height}, step);

    const int16_t plusX = static_cast<int16_t>(band.right() - stepWidth);
    step.label = "+";
    step.action = row.increment;
    step.value = 1;
    step.hitPadding.left = screen.theme().spaceSm;
    step.hitPadding.right = 0;
    fui::button(screen.frame(), fui::Rect{plusX, band.y, stepWidth, band.height}, step);
  }

  const fui::Rect trackRect = band.inset(fui::Insets{0, sideGap, 0, sideGap});
  fui::SliderProps slider;
  slider.value = row.sliderValue;
  slider.max = row.max;
  slider.action = row.sliderAction;
  slider.inputMask = fui::InputTouch | fui::InputDrag;
  slider.trackHeight = 3;
  slider.knobWidth = 10;
  slider.knobHeight = 22;
  slider.horizontalPadding = 0;
  slider.minTouchSize = screen.theme().minTouchSize;
  slider.radius = 0;
  slider.border = fui::Paint::none();
  slider.enabled = row.enabled;

  if (row.value && !row.compact) {
    const int32_t maxValue = slider.max <= 0 ? 1 : slider.max;
    const int32_t value = std::clamp<int32_t>(slider.value, 0, maxValue);
    const int16_t knobWidth = std::max<int16_t>(4, slider.knobWidth);
    const int16_t travel = std::max<int16_t>(0, static_cast<int16_t>(trackRect.width - knobWidth));
    const int16_t knobCenter =
        static_cast<int16_t>(trackRect.x + knobWidth / 2 + (static_cast<int32_t>(travel) * value) / maxValue);
    const fui::Size valueSize = screen.target().measureText(valueStyle.font, row.value, valueStyle);
    const fui::Rect valueLane{trackRect.x, static_cast<int16_t>(rect.y + captionHeight), trackRect.width, lineHeight};
    fui::Rect valueRect{static_cast<int16_t>(knobCenter - valueSize.width / 2), valueLane.y, valueSize.width,
                        valueLane.height};
    valueRect.x = std::max<int16_t>(valueLane.x, std::min<int16_t>(valueRect.x, valueLane.right() - valueRect.width));
    valueStyle.align = fui::TextAlign::Left;
    screen.target().text(valueRect, row.value, valueStyle);
  }
  fui::slider(screen.frame(), trackRect, slider);

  if (!row.compact && (row.minimumLabel || row.maximumLabel)) {
    const int16_t plusX = static_cast<int16_t>(band.right() - stepWidth);
    const int16_t endpointY = static_cast<int16_t>(band.bottom() + SLIDER_SCALE_GAP);
    fui::TextStyle endpointStyle = valueStyle;
    endpointStyle.bold = false;
    if (row.minimumLabel) {
      endpointStyle.align = fui::TextAlign::Center;
      screen.target().text(fui::Rect{band.x, endpointY, stepWidth, lineHeight}, row.minimumLabel, endpointStyle);
    }
    if (row.maximumLabel) {
      endpointStyle.align = fui::TextAlign::Center;
      screen.target().text(fui::Rect{plusX, endpointY, stepWidth, lineHeight}, row.maximumLabel, endpointStyle);
    }
  }
}

template <size_t MaxInteractions>
int16_t centeredReaderSliderControlTop(fui::Screen<MaxInteractions>& screen, const ReaderSliderRowProps& row) {
  const int16_t controlOffset = readerSliderControlTopInset(screen, row);
  return std::max<int16_t>(0, static_cast<int16_t>((screen.body().height - SLIDER_CONTROL_HEIGHT) / 2 - controlOffset));
}

template <size_t MaxInteractions>
void drawButtonSliderStepHints(fui::Screen<MaxInteractions>& screen, const int fineStep, const int coarseStep,
                               const char* unit) {
  const fui::TextStyle hint = ReaderSliderHints::style(screen.theme());
  const int16_t lineHeight = screen.target().lineHeight(hint.font);
  char line[64];
  std::snprintf(line, sizeof(line), "%s: %d%s", tr(STR_FRONT_BUTTONS), fineStep, unit);
  screen.target().text(screen.takeTop(lineHeight, screen.theme().spaceSm), line, hint);
  std::snprintf(line, sizeof(line), "%s: %d%s", tr(STR_SIDE_BUTTONS), coarseStep, unit);
  screen.target().text(screen.takeTop(lineHeight), line, hint);
}

template <size_t MaxInteractions>
void drawDualReaderSliderRows(fui::Screen<MaxInteractions>& screen, const ReaderSliderRowProps& first,
                              const ReaderSliderRowProps& second, const bool showButtonHints) {
  const int16_t bottomPadding = screen.theme().spaceSm;
  const fui::TextStyle hint = ReaderSliderHints::style(screen.theme());
  char frontHint[64] = {};
  char sideHint[64] = {};
  const int16_t hintGap = screen.theme().spaceMd;
  const int16_t hintColumnWidth = std::max<int16_t>(0, static_cast<int16_t>((screen.body().width - hintGap) / 2));
  bool inlineHints = false;
  if (showButtonHints) {
    std::snprintf(frontHint, sizeof(frontHint), "%s: +/- 1", tr(STR_FRONT_BUTTONS));
    std::snprintf(sideHint, sizeof(sideHint), "%s: +/- 5", tr(STR_SIDE_BUTTONS));
    inlineHints = screen.target().measureText(hint.font, frontHint, hint).width <= hintColumnWidth &&
                  screen.target().measureText(hint.font, sideHint, hint).width <= hintColumnWidth;
  }
  const int16_t hintLineHeight = showButtonHints ? screen.target().lineHeight(hint.font) : 0;
  const int16_t hintRows = inlineHints ? 1 : 2;
  const int16_t hintHeight =
      showButtonHints ? static_cast<int16_t>((hintLineHeight + screen.theme().spaceSm) * hintRows) : 0;
  const int16_t remaining =
      std::max<int16_t>(0, static_cast<int16_t>(screen.body().height - readerSliderRowHeight(screen, first) -
                                                readerSliderRowHeight(screen, second) - hintHeight - bottomPadding));
  // Move a little spare space above and between the sliders, keeping the
  // button hints anchored in place. Tight layouts simply use a smaller inset.
  const int16_t topInset = showButtonHints ? std::min<int16_t>(screen.theme().spaceSm, remaining / 6) : 0;
  screen.spacer(topInset);
  drawReaderSliderRow(screen, first);
  const int16_t betweenRows = showButtonHints ? static_cast<int16_t>(remaining / 2 + topInset * 2) : remaining;
  screen.spacer(betweenRows);
  drawReaderSliderRow(screen, second);
  if (showButtonHints) {
    screen.spacer(static_cast<int16_t>(remaining - topInset - betweenRows));
    screen.spacer(screen.theme().spaceSm);
    const fui::Rect hintRow = screen.takeTop(hintLineHeight);
    if (inlineHints) {
      screen.target().text(fui::Rect{hintRow.x, hintRow.y, hintColumnWidth, hintRow.height}, frontHint, hint);
      screen.target().text(fui::Rect{static_cast<int16_t>(hintRow.x + hintColumnWidth + hintGap), hintRow.y,
                                     static_cast<int16_t>(hintRow.width - hintColumnWidth - hintGap), hintRow.height},
                           sideHint, hint);
    } else {
      screen.target().text(hintRow, frontHint, hint);
      screen.spacer(screen.theme().spaceSm);
      screen.target().text(screen.takeTop(hintLineHeight), sideHint, hint);
    }
  }
  screen.spacer(bottomPadding);
}

void setDrawerSelectionStyle(fui::StyleSet& styles) {
  styles = fui::defaultListRowStyles();
  styles.selected.background = fui::Paint::dither(fui::Color::LightGray);
  styles.selected.foreground = fui::Paint::solid(fui::Color::Black);
  styles.selected.radius = 4;
}

uint16_t configureDrawerList(fui::ListProps& props, const fui::ThemeTokens& theme, const fui::Rect bounds) {
  // Root rows use SettingRowProps, whose content starts at the drawer inset
  // plus its 8px side padding. Do not inherit themed list pills here: their
  // additional row inset makes picker labels visibly farther to the right.
  props.rowInset = 0;
  props.sidePadding = fui::SettingRowProps{}.sidePadding;
  setDrawerSelectionStyle(props.rowStyles);
  return configureUiList(props, theme, bounds);
}

void evenlySpaceDrawerListRows(fui::ListProps& props, const fui::Rect bounds, const int rowCount) {
  if (rowCount <= 1 || props.rowHeight <= 0) return;
  const int remaining = std::max(0, static_cast<int>(bounds.height) - static_cast<int>(props.rowHeight) * rowCount);
  props.rowGap = static_cast<int16_t>(remaining / (rowCount - 1));
}

fui::Rect drawerScrollbarBounds(fui::Rect bounds) {
  bounds.width = static_cast<int16_t>(bounds.width + DRAWER_SIDE_INSET - DRAWER_SCROLLBAR_RIGHT_INSET);
  return bounds;
}

const char* wordSpacingValue(const uint8_t value) {
  static constexpr const char* labels[] = {"-4", "-3", "-2", "-1", "0", "1", "2", "3", "4"};
  return labels[WordSpacing::sliderValue(value)];
}

uint8_t percentToByte(const int16_t permille, const uint8_t minimum, const uint8_t maximum) {
  const int range = maximum - minimum;
  return static_cast<uint8_t>(minimum + (static_cast<int>(permille) * range + 500) / 1000);
}

int16_t byteToPermille(const uint8_t value, const uint8_t minimum, const uint8_t maximum) {
  if (maximum <= minimum) return 0;
  return static_cast<int16_t>((static_cast<int>(value - minimum) * 1000) / (maximum - minimum));
}

template <size_t N>
int indexForRaw(const std::array<uint8_t, N>& rawValues, const uint8_t value) {
  const auto it = std::find(rawValues.begin(), rawValues.end(), value);
  return it == rawValues.end() ? 0 : static_cast<int>(std::distance(rawValues.begin(), it));
}
}  // namespace

EpubReaderDrawerActivity::EpubReaderDrawerActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> epub,
    const EpubReaderPreviewModel* previewModel, const float bookProgressPercent, const uint32_t chapterPage,
    const uint32_t chapterPageCount, const bool chapterPageCountEstimated, const bool hasFootnotes,
    const bool hasDictionary, const bool hasBookmarks, const bool hasClippings, const bool isCurrentPageBookmarked,
    const bool isBookCompleted, const bool showReadingPaceReset, const bool globalStatsEnabled,
    const bool bookStatsEnabled, const uint32_t stableCurrentPage, const uint32_t stablePageCount,
    const uint16_t autoPageTurnIntervalSeconds, const bool automaticPageTurnActive,
    SaveSettingsCallback saveReaderSettingsCallback, void* saveReaderSettingsContext,
    SaveGlobalSettingsCallback saveGlobalSettingsCallback, void* saveGlobalSettingsContext,
    GlobalSettingsEditCallback beginGlobalSettingsEditCallback, void* beginGlobalSettingsEditContext,
    GlobalSettingsEditCallback endGlobalSettingsEditCallback, void* endGlobalSettingsEditContext,
    const char* dictionaryFontFamilyName, const uint8_t dictionaryFontPointSize, const bool hasDictionaryFontOverride,
    DictionaryFontChangedCallback dictionaryFontChangedCallback, void* dictionaryFontChangedContext,
    const ReaderDrawerState initialState, std::unique_ptr<EpubReaderPreviewModel> ownedPreviewModel)
    : Activity("EpubReaderDrawer", renderer, mappedInput),
      epub(std::move(epub)),
      previewModel(previewModel),
      ownedPreviewModel(std::move(ownedPreviewModel)),
      // Round to the nearest whole percent, not the nearest centipercent, so the pane
      // opens on a clean number (e.g. "43.00%") rather than the book's exact fractional
      // position; the keypad is how the user reaches a decimal destination.
      percent(static_cast<int>(std::lround(std::clamp(bookProgressPercent, 0.0f, 100.0f))) * 100),
      stablePage(clampStablePage(stableCurrentPage, stablePageCount)),
      stablePageCount(stablePageCount),
      percentSeed(percent),
      chapterPage(chapterPage),
      chapterPageCount(chapterPageCount),
      chapterPageCountEstimated(chapterPageCountEstimated),
      stablePageSeed(stablePage),
      hasFootnotes(hasFootnotes),
      hasDictionary(hasDictionary),
      hasBookmarks(hasBookmarks),
      hasClippings(hasClippings),
      globalStatsEnabled(globalStatsEnabled),
      bookStatsEnabled(bookStatsEnabled),
      isCurrentPageBookmarked(isCurrentPageBookmarked),
      isBookCompleted(isBookCompleted),
      showReadingPaceReset(showReadingPaceReset),
      automaticPageTurnActive(automaticPageTurnActive),
      autoPageTurnIntervalSeconds(std::clamp(autoPageTurnIntervalSeconds, READER_AUTO_PAGE_TURN_MIN_SECONDS,
                                             READER_AUTO_PAGE_TURN_MAX_SECONDS)),
      state(initialState),
      draft(captureSettings()),
      sourceSettings(draft),
      lastGoodPreviewSettings(draft),
      saveReaderSettingsCallback(saveReaderSettingsCallback),
      saveReaderSettingsContext(saveReaderSettingsContext),
      saveGlobalSettingsCallback(saveGlobalSettingsCallback),
      saveGlobalSettingsContext(saveGlobalSettingsContext),
      beginGlobalSettingsEditCallback(beginGlobalSettingsEditCallback),
      beginGlobalSettingsEditContext(beginGlobalSettingsEditContext),
      endGlobalSettingsEditCallback(endGlobalSettingsEditCallback),
      endGlobalSettingsEditContext(endGlobalSettingsEditContext),
      dictionaryFontPointSize(dictionaryFontPointSize),
      hasDictionaryFontOverride(hasDictionaryFontOverride),
      dictionaryFontChangedCallback(dictionaryFontChangedCallback),
      dictionaryFontChangedContext(dictionaryFontChangedContext),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {
  ReaderSliderHints::bindFont(uiTarget);
  if (dictionaryFontFamilyName) {
    std::strncpy(this->dictionaryFontFamilyName, dictionaryFontFamilyName, sizeof(this->dictionaryFontFamilyName) - 1);
  }
  if (!this->previewModel) this->previewModel = this->ownedPreviewModel.get();
  previewUnavailable = !mappedInput.hasTouchHardware() && !this->previewModel;
  // The button menu paints the whole screen, including its sample preview.
  previewDirty = !mappedInput.hasTouchHardware();
}

void EpubReaderDrawerActivity::onEnter() {
  Activity::onEnter();
  // Only the full-screen button drawer displays the book/chapter summary.
  if (!mappedInput.hasTouchHardware()) {
    RenderLock lock(*this);
    filenameFontSystem.ensureLoaded(renderer);
  }
  if (mappedInput.hasTouchHardware()) mappedInput.setReaderTouchscreenOverride(true);

  const ReaderDrawerCatalog catalog = makeReaderDrawerCatalog(
      {hasFootnotes, hasDictionary, hasBookmarks, hasClippings, showReadingPaceReset, stablePageCount > 0,
       !mappedInput.hasTouchHardware(), globalStatsEnabled, bookStatsEnabled});
  for (size_t tab = 0; tab < rootRows.size(); ++tab) {
    rootRows[tab].reserve(catalog[tab].count);
    rootRows[tab].assign(catalog[tab].items.begin(), catalog[tab].items.begin() + catalog[tab].count);
  }
  if (!mappedInput.hasTouchHardware()) {
    buttonFocusActive = state.pane != ReaderDrawerPane::Root;
    if (state.pane == ReaderDrawerPane::Root) {
      const auto count = rootRows[static_cast<size_t>(state.tab)].size();
      state.selectedIndex =
          static_cast<int16_t>(std::clamp<int>(state.selectedIndex, 0, std::max(0, static_cast<int>(count) - 1)));
    }
  }
  paneRows.reserve(7);
  discoverDictionaries();
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &EpubReaderDrawerActivity::onRowEvent, this);
  app.on(ACTION_TAB, &EpubReaderDrawerActivity::onTabEvent, this);
  app.on(ACTION_DISMISS, &EpubReaderDrawerActivity::onDismissEvent, this);
  app.on(ACTION_BACK, &EpubReaderDrawerActivity::onBackEvent, this);
  app.on(ACTION_SLIDER, &EpubReaderDrawerActivity::onSliderEvent, this);
  app.on(ACTION_STEP, &EpubReaderDrawerActivity::onStepEvent, this);
  app.on(ACTION_SLIDER + 3, &EpubReaderDrawerActivity::onSliderEvent, this);
  app.on(ACTION_STEP + 3, &EpubReaderDrawerActivity::onStepEvent, this);
  app.on(ACTION_CONFIRM, &EpubReaderDrawerActivity::onConfirmEvent, this);
  app.on(ACTION_KEYPAD_KEY, &EpubReaderDrawerActivity::onKeypadKeyEvent, this);
  app.on(ACTION_KEYPAD_BACKSPACE, &EpubReaderDrawerActivity::onKeypadBackspaceEvent, this);
  if (app.handlerOverflowed()) {
    LOG_ERR("ERDM", "Touch menu handler table overflowed; a control's action is silently dead");
  }
  app.setScreen(&EpubReaderDrawerActivity::drawerScreen, this);
  requestUpdate();
}

void EpubReaderDrawerActivity::onExit() {
  commitSettings();
  ownedPreviewModel.reset();
  if (!mappedInput.hasTouchHardware()) {
    const auto heap = MemoryBudget::snapshot();
    LOG_DBG("ERDM", "Button preview released: free=%u maxAlloc=%u", heap.freeHeap, heap.maxAllocHeap);
    (void)heap;
  }
  dictionaryRegistry.clear();
  // The reader remains active beneath this drawer. Keep the small catalog for
  // its resident scalable family so reopening Font Size avoids an SD rescan.
#if CROSSINK_SCALABLE_FONTS
  if (!sdFontSystem.hasResidentScalableFamily(SETTINGS.sdFontFamilyName))
#endif
    sdFontSystem.releaseRegistry();
  if (mappedInput.hasTouchHardware()) mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();
}

void EpubReaderDrawerActivity::discoverFonts() {
  RenderLock lock;
  GUI.drawPopup(renderer, tr(STR_LOADING), true);
  sdFontSystem.refreshIfDirty();
  const auto& families = sdFontSystem.registry().getFamilies();
  fontLabels.clear();
  fontSettingIndexes.clear();
  fontLabels.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + families.size());
  fontSettingIndexes.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + families.size());
  constexpr auto builtinRange = BUILTIN_FONT_POINT_SIZE_RANGE;
  fontLabels.push_back(fontFamilyLabel(tr(STR_LEXEND_DECA), builtinRange));
  fontLabels.push_back(fontFamilyLabel(tr(STR_BITTER), builtinRange));
  fontSettingIndexes.push_back(0);
  fontSettingIndexes.push_back(1);
  for (size_t i = 0; i < families.size(); ++i) {
    fontLabels.push_back(fontFamilyLabel(families[i].name, fontFamilyPointSizeRange(families[i])));
    fontSettingIndexes.push_back(static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i));
  }
}

void EpubReaderDrawerActivity::refreshTtfRenderingRow() {
  paneRows.erase(std::remove(paneRows.begin(), paneRows.end(), RowId::TtfRendering), paneRows.end());
#if CROSSINK_SCALABLE_FONTS
  if (state.pane == ReaderDrawerPane::ReaderFont && sdFontSystem.isScalableFamily(draft.sdFontFamilyName.data())) {
    const auto spacing = std::find(paneRows.begin(), paneRows.end(), RowId::CharacterSpacing);
    if (spacing != paneRows.end()) paneRows.insert(std::next(spacing), RowId::TtfRendering);
  }
#endif
}

#if CROSSINK_SCALABLE_FONTS
void EpubReaderDrawerActivity::rebuildTtfRenderingRows() {
  paneRows = {RowId::TtfHinting, RowId::TtfRaster};
  if (ttfRenderProfile.hinting == 1) paneRows.push_back(RowId::TtfInterpreter);
  paneRows.push_back(RowId::TtfWeight);
  paneRows.push_back(RowId::TtfSlant);
  paneRows.push_back(RowId::TtfStemDarkening);
  paneRows.push_back(RowId::TtfReset);
  state.selectedIndex = std::min<int16_t>(state.selectedIndex, static_cast<int16_t>(paneRows.size() - 1));
  state.paneTopIndex = std::min<int16_t>(state.paneTopIndex, static_cast<int16_t>(paneRows.size() - 1));
}

bool EpubReaderDrawerActivity::saveTtfRenderingProfile() {
  const char* const family = draft.sdFontFamilyName.data();
  if (ttfRenderProfile == TTF_RENDER_PROFILES.profileFor(family)) return true;
  if (!TTF_RENDER_PROFILES.setProfile(family, ttfRenderProfile)) {
    LOG_ERR("ERDM", "Failed to save TTF rendering profile for %s", family);
    return false;
  }
  ttfRenderingChanged = ttfRenderProfile != initialTtfRenderProfile;
  return true;
}

void EpubReaderDrawerActivity::finishTtfRenderingEdit() {
  if (!ttfRenderingChanged) return;
  if (sdFontSystem.reloadActiveScalableFamily(renderer, draft.sdFontFamilyName.data())) {
    previewFontMetricsChanged = true;
    markSettingChanged(ReaderSettingsChangeMask::Preview | ReaderSettingsChangeMask::Relayout);
  }
  ttfRenderingChanged = false;
}

void EpubReaderDrawerActivity::showTtfRenderingOptions(const RowId row) {
  using TtfRow = TtfRenderOptionsActivity::Row;
  TtfRow optionRow;
  switch (row) {
    case RowId::TtfHinting:
      optionRow = TtfRow::Hinting;
      break;
    case RowId::TtfRaster:
      optionRow = TtfRow::Raster;
      break;
    case RowId::TtfInterpreter:
      optionRow = TtfRow::Interpreter;
      break;
    case RowId::TtfWeight:
      optionRow = TtfRow::Weight;
      break;
    case RowId::TtfSlant:
      optionRow = TtfRow::Slant;
      break;
    default:
      return;
  }
  const auto* options = TtfRenderOptionsActivity::optionLabels(optionRow);
  if (options == nullptr) return;
  std::vector<uint8_t> values;
  values.reserve(options->size());
  for (size_t i = 0; i < options->size(); ++i) values.push_back(static_cast<uint8_t>(i));
  openEnumOptions(row, TtfRenderOptionsActivity::titleId(optionRow), *options, std::move(values),
                  TtfRenderOptionsActivity::selectedOption(ttfRenderProfile, optionRow));
}
#endif

void EpubReaderDrawerActivity::discoverDictionaries() {
  dictionaryRegistry.discover();
  dictionaryLabels.clear();
  dictionaryPaths.clear();
  dictionaryLabels.reserve(dictionaryRegistry.getEntries().size() + 1);
  dictionaryPaths.reserve(dictionaryRegistry.getEntries().size() + 1);
  dictionaryLabels.emplace_back(tr(STR_DICT_USE_GLOBAL));
  dictionaryPaths.emplace_back();
  for (const auto& entry : dictionaryRegistry.getEntries()) {
    dictionaryLabels.push_back(entry.name);
    dictionaryPaths.push_back(entry.basePath);
  }
  bookDictionaryPath = epub ? Dictionary::readDictPath(epub->getCachePath().c_str()) : std::string{};
}

void EpubReaderDrawerActivity::commitSettings() {
  if (!settingsChanged) return;
  if (!mappedInput.hasTouchHardware() && draft.sdFontFamilyName[0] != '\0' &&
      (draft.sdFontFamilyName != lastGoodPreviewSettings.sdFontFamilyName ||
       draft.readerFontPointSize != lastGoodPreviewSettings.readerFontPointSize)) {
    // A queued preview may not have rendered before Back or an external exit.
    // Save only an SD font whose preview already loaded and prewarmed.
    LOG_ERR("ERDM", "Selected SD font was not previewed; retaining previous reader font");
    restoreReaderDraftFont(draft, lastGoodPreviewSettings);
    state.pendingFontIndex = -1;
  }
  applySettings(draft);
  if (saveReaderSettingsCallback) {
    saveReaderSettingsCallback(saveReaderSettingsContext);
  } else if (!SETTINGS.saveToFile()) {
    LOG_ERR("ERDM", "Failed to persist touch reader settings");
  }
  settingsChanged = false;
}

ReaderSettingsDraft EpubReaderDrawerActivity::captureSettings() {
  ReaderSettingsDraft value;
  value.fontFamily = SETTINGS.fontFamily;
  value.readerFontPointSize = SETTINGS.readerFontPointSize;
  std::strncpy(value.sdFontFamilyName.data(), SETTINGS.sdFontFamilyName, value.sdFontFamilyName.size() - 1);
  value.lineHeightPercent = SETTINGS.lineHeightPercent;
  value.wordSpacing = SETTINGS.wordSpacing;
  value.characterSpacing = SETTINGS.characterSpacing;
  value.screenMarginVertical = SETTINGS.screenMarginVertical;
  value.screenMarginHorizontal = SETTINGS.screenMarginHorizontal;
  value.orientation = SETTINGS.orientation;
  value.paragraphAlignment = SETTINGS.paragraphAlignment;
  value.textAntiAliasing = SETTINGS.textAntiAliasing;
  value.focusReadingEnabled = SETTINGS.focusReadingEnabled;
  value.guideReadingEnabled = SETTINGS.guideReadingEnabled;
  value.hyphenationEnabled = SETTINGS.hyphenationEnabled;
  value.publisherPageNumbers = SETTINGS.publisherPageNumbers;
  value.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  value.forceParagraphIndents = SETTINGS.forceParagraphIndents;
  value.embeddedStyle = SETTINGS.embeddedStyle;
  value.publisherFonts = SETTINGS.publisherFonts;
  value.imageRendering = SETTINGS.imageRendering;
  value.imageGrayscale = SETTINGS.imageGrayscale;
  value.epubRenderMode = SETTINGS.epubRenderMode;
  value.indexingMethod = SETTINGS.indexingMethod;
  return value;
}

void EpubReaderDrawerActivity::applySettings(const ReaderSettingsDraft& value) {
  SETTINGS.fontFamily = value.fontFamily;
  SETTINGS.readerFontPointSize = value.readerFontPointSize;
  std::strncpy(SETTINGS.sdFontFamilyName, value.sdFontFamilyName.data(), sizeof(SETTINGS.sdFontFamilyName) - 1);
  SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
  SETTINGS.lineHeightPercent = value.lineHeightPercent;
  SETTINGS.wordSpacing = value.wordSpacing;
  SETTINGS.characterSpacing = value.characterSpacing;
  SETTINGS.screenMarginVertical = value.screenMarginVertical;
  SETTINGS.screenMarginHorizontal = value.screenMarginHorizontal;
  SETTINGS.orientation = value.orientation;
  SETTINGS.paragraphAlignment = value.paragraphAlignment;
  SETTINGS.textAntiAliasing = value.textAntiAliasing;
  SETTINGS.focusReadingEnabled = value.focusReadingEnabled;
  SETTINGS.guideReadingEnabled = value.guideReadingEnabled;
  SETTINGS.hyphenationEnabled = value.hyphenationEnabled;
  SETTINGS.publisherPageNumbers = value.publisherPageNumbers;
  SETTINGS.extraParagraphSpacing = value.extraParagraphSpacing;
  SETTINGS.forceParagraphIndents = value.forceParagraphIndents;
  SETTINGS.embeddedStyle = value.embeddedStyle;
  SETTINGS.publisherFonts = value.publisherFonts;
  SETTINGS.imageRendering = value.imageRendering;
  SETTINGS.imageGrayscale = value.imageGrayscale;
  SETTINGS.epubRenderMode = value.epubRenderMode;
  SETTINGS.indexingMethod = value.indexingMethod;
}

void EpubReaderDrawerActivity::markSettingChanged(const ReaderSettingsChangeMask mask) {
  settingsChanged = true;
  didChangeSettings = true;
  previewDirty = previewDirty || hasReaderSettingsChange(mask, ReaderSettingsChangeMask::Preview);
  changeMask = changeMask | mask;
}

void EpubReaderDrawerActivity::closeAndReturn(const bool cancelled, const EpubReaderMenuAction action,
                                              const bool reopenDrawer) {
#if CROSSINK_SCALABLE_FONTS
  // ActivityManager copies the result before onExit(). Resolve a pending TTF
  // edit here so the reader knows its old font ID and section need replacing.
  finishTtfRenderingEdit();
#endif
  const bool changed = didChangeSettings;
  commitSettings();
  ownedPreviewModel.reset();
  if (!mappedInput.hasTouchHardware()) previewModel = nullptr;
  MenuResult menu{cancelled ? -1 : static_cast<int>(action), draft.orientation, changed};
  menu.drawerState = state;
  menu.changeMask = changeMask;
  menu.reopenDrawer = !cancelled && reopenDrawer;
  ActivityResult result;
  result.isCancelled = cancelled;
  result.data = menu;
  setResult(std::move(result));
  finish();
}

bool EpubReaderDrawerActivity::handleHomeGesture() {
  closePane();
  return true;
}

void EpubReaderDrawerActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EpubReaderDrawerActivity*>(user);
  self->buttonFocusActive = false;
  self->state.selectedIndex = event.value;
  self->app.clearTapFlash();
  self->activateListIndex(event.value);
}

void EpubReaderDrawerActivity::onTabEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EpubReaderDrawerActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int16_t>(READER_DRAWER_TAB_COUNT)) return;
  self->buttonFocusActive = false;
  self->app.clearTapFlash();
  self->changeTab(static_cast<ReaderDrawerTab>(event.value));
}

void EpubReaderDrawerActivity::onDismissEvent(const fui::ActionEvent&, void* user) {
  static_cast<EpubReaderDrawerActivity*>(user)->closeAndReturn(true);
}

void EpubReaderDrawerActivity::onBackEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<EpubReaderDrawerActivity*>(user);
  self->buttonFocusActive = false;
  self->closePane();
}

void EpubReaderDrawerActivity::onSliderEvent(const fui::ActionEvent& event, void* user) {
  if (event.dragPermille < 0) return;
  auto* self = static_cast<EpubReaderDrawerActivity*>(user);
  self->buttonFocusActive = false;
  const auto tapValue = [self](const int value, const int minimum, const int maximum) {
    return self->sliderTapPending ? snapSliderTapValue(value, minimum, maximum, 5) : value;
  };
  const bool second = event.action == ACTION_SLIDER + 3;
  if (self->state.pane == ReaderDrawerPane::CharacterSpacing) {
    self->draft.characterSpacing = percentToByte(event.dragPermille, 0, CrossPointSettings::MAX_CHARACTER_SPACING);
  } else if (self->state.pane == ReaderDrawerPane::Spacing) {
    if (second) {
      self->draft.wordSpacing =
          WordSpacing::fromSlider(percentToByte(event.dragPermille, 0, CrossPointSettings::MAX_WORD_SPACING));
    } else {
      const int value = percentToByte(event.dragPermille, CrossPointSettings::MIN_LINE_HEIGHT_PERCENT,
                                      CrossPointSettings::MAX_LINE_HEIGHT_PERCENT);
      self->draft.lineHeightPercent = CrossPointSettings::clampedLineHeightPercent(
          tapValue(value, CrossPointSettings::MIN_LINE_HEIGHT_PERCENT, CrossPointSettings::MAX_LINE_HEIGHT_PERCENT));
    }
  } else if (self->state.pane == ReaderDrawerPane::Margins) {
    auto& target = second ? self->draft.screenMarginHorizontal : self->draft.screenMarginVertical;
    target = tapValue(
        percentToByte(event.dragPermille, CrossPointSettings::MIN_SCREEN_MARGIN, CrossPointSettings::MAX_SCREEN_MARGIN),
        CrossPointSettings::MIN_SCREEN_MARGIN, CrossPointSettings::MAX_SCREEN_MARGIN);
  } else if (self->state.pane == ReaderDrawerPane::AutoPageTurn) {
    self->autoPageTurnIntervalSeconds = static_cast<uint16_t>(tapValue(
        percentToByte(event.dragPermille, READER_AUTO_PAGE_TURN_MIN_SECONDS, READER_AUTO_PAGE_TURN_MAX_SECONDS),
        READER_AUTO_PAGE_TURN_MIN_SECONDS, READER_AUTO_PAGE_TURN_MAX_SECONDS));
    self->requestUpdate();
    return;
  }
  self->markSettingChanged(ReaderSettingsChangeMask::Preview | ReaderSettingsChangeMask::Relayout);
  // A slider update changes the reader preview as well as the control. Do not
  // rely on the control framework's invalidation to schedule that redraw.
  self->requestUpdate();
}

void EpubReaderDrawerActivity::onStepEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EpubReaderDrawerActivity*>(user);
  self->buttonFocusActive = false;
  if (self->state.pane == ReaderDrawerPane::AutoPageTurn) {
    self->adjustActiveSlider(event.value);
    self->requestUpdate();
    return;
  }
  if (!readerDrawerStepChangesSettings(self->state.pane)) {
    self->adjustActiveSlider(event.value);
    self->requestUpdate();
    return;
  }
  const bool second = event.action == ACTION_STEP + 3;
  if (second) {
    if (self->state.pane == ReaderDrawerPane::Spacing) {
      self->draft.wordSpacing = WordSpacing::fromLevel(WordSpacing::level(self->draft.wordSpacing) + event.value);
    } else if (self->state.pane == ReaderDrawerPane::Margins) {
      self->draft.screenMarginHorizontal =
          std::clamp<int>(self->draft.screenMarginHorizontal + event.value, CrossPointSettings::MIN_SCREEN_MARGIN,
                          CrossPointSettings::MAX_SCREEN_MARGIN);
    }
  } else {
    self->adjustActiveSlider(event.value);
  }
  self->markSettingChanged(ReaderSettingsChangeMask::Preview | ReaderSettingsChangeMask::Relayout);
  self->requestUpdate();
}

void EpubReaderDrawerActivity::onConfirmEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<EpubReaderDrawerActivity*>(user);
  self->buttonFocusActive = false;
  if (self->state.pane == ReaderDrawerPane::Percent) {
    self->completePercentSelection();
    return;
  }
  if (self->state.pane == ReaderDrawerPane::StablePage) {
    self->completeStablePageSelection();
    return;
  }
  if (self->state.pane == ReaderDrawerPane::AutoPageTurn) {
    self->completeAutoPageTurnSelection();
    return;
  }
  self->closePane();
}

void EpubReaderDrawerActivity::drawerScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<EpubReaderDrawerActivity*>(user)->buildDrawer(screen);
}

int16_t EpubReaderDrawerActivity::drawerHeight() const {
  if (CROSSINK_APP_READER_SAMPLE_PREVIEW) return renderer.getScreenHeight();
  fui::SheetProps sheet;
  sheet.ruleWidth = DRAWER_RULE_WIDTH;
  const int16_t grabberBand = DrawerHandle::bandHeight(sheet);
  // Size landscape root menus from their real row cadence so the tab chrome
  // and grabber do not consume one of the intended visible rows.
  const int16_t tabBarHeight = static_cast<int16_t>(TAB_BAR_HEIGHT + TAB_BAR_VERTICAL_PADDING * 2);
  int16_t drawerHeight = static_cast<int16_t>(readerDrawerHeight(renderer, state.pane) + grabberBand);
  if (mappedInput.hasTouchHardware() && readerDrawerSliderPreviewsText(state.pane)) {
    ReaderSliderRowProps row;
    row.label = row.value = row.minimumLabel = row.maximumLabel = " ";
    row.inlineLabelWidth = 1;
    const auto& theme = app.theme();
    // Fit the inline rows instead of retaining the old tall sheet's empty space.
    drawerHeight = static_cast<int16_t>(grabberBand + sheet.ruleWidth + tabBarHeight + theme.headerHeight +
                                        (state.pane == ReaderDrawerPane::CharacterSpacing ? 1 : 2) *
                                            readerSliderRowHeight(uiTarget, theme, row) +
                                        theme.spaceMd + theme.spaceSm);
    drawerHeight = std::min<int16_t>(drawerHeight, renderer.getScreenHeight());
  } else if (state.pane == ReaderDrawerPane::Root && isLandscapeOrientation(renderer.getOrientation())) {
    const int16_t rowHeight = app.theme().rowHeight;
    const int16_t gap = app.theme().spaceSm;
    drawerHeight = static_cast<int16_t>(grabberBand + sheet.ruleWidth + tabBarHeight + DRAWER_LIST_TOP_PADDING +
                                        readerDrawerListHeightForRows(LANDSCAPE_ROOT_ROWS, rowHeight, gap));
    drawerHeight = std::min<int16_t>(drawerHeight, renderer.getScreenHeight());
  }
  return drawerHeight;
}

fui::Rect EpubReaderDrawerActivity::previewBounds() const {
  if (CROSSINK_APP_READER_SAMPLE_PREVIEW) return samplePreviewBounds;
  const int16_t height = static_cast<int16_t>(renderer.getScreenHeight() - drawerHeight());
  return fui::Rect{0, 0, renderer.getScreenWidth(), height};
}

bool EpubReaderDrawerActivity::showsSamplePreview() const {
  if (!CROSSINK_APP_READER_SAMPLE_PREVIEW) return false;
  return readerDrawerShowsSamplePreview(state.pane, state.tab, enumOptionRow);
}

bool EpubReaderDrawerActivity::samplePreviewBesideControls() const {
  return readerDrawerSamplePreviewBesideControls(isLandscapeOrientation(renderer.getOrientation()));
}

void EpubReaderDrawerActivity::buildDrawer(UiApp::ScreenType& screen) {
  fui::SheetProps sheet;
  const bool buttonDevice = !mappedInput.hasTouchHardware();
  sheet.anchor = fui::SheetEdge::Bottom;
  sheet.dismissAction = ACTION_DISMISS;
  sheet.radius = 0;
  sheet.ruleWidth = DRAWER_RULE_WIDTH;
  const int16_t tabBarHeight = static_cast<int16_t>(TAB_BAR_HEIGHT + TAB_BAR_VERTICAL_PADDING * 2);
  if (CROSSINK_APP_READER_SAMPLE_PREVIEW) {
    screen.target().fill(screen.device().screen(), fui::Paint::solid(fui::Color::White));
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    screen.setContentMarginFromScreen(fui::Insets{
        static_cast<int16_t>(safe.y), static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
        static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height), static_cast<int16_t>(safe.x)});
    drawerHandleRect = {};
  } else {
    const fui::Rect sheetContent = screen.sheet(sheet, drawerHeight());
    drawerHandleRect = DrawerHandle::registerTap(screen.frame(), sheetContent, sheet, ACTION_DISMISS);
  }
  int16_t buttonHeaderHeight = 0;
  if (buttonDevice) {
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const auto header = TouchHeaderBackButton::headerRect(renderer, mappedInput, safe);
    const int summaryHeight = ReaderBookSummary::height(renderer, chapterTitle.c_str());
    buttonHeaderHeight =
        static_cast<int16_t>(std::max(0, header.y + header.height + summaryHeight - screen.contentRect().y));
    screen.takeTop(buttonHeaderHeight);
  }
  // Give every tab row four pixels of white space above and below its icons.
  // The tab pill keeps its previous size so the selected background does not
  // become taller with the row.
  const fui::Rect tabs = buttonDevice ? screen.takeTop(tabBarHeight) : screen.takeBottom(tabBarHeight);
  buildTabBar(screen, tabs, buttonDevice);
  samplePreviewBounds = {};
#if CROSSINK_APP_READER_SAMPLE_PREVIEW
  if (showsSamplePreview()) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    // In portrait, keep the sample at its pre-header height so the new book
    // progress row does not remove a line. Landscape needs room for its rows.
    const int previewBaseHeight =
        screen.body().height + (isLandscapeOrientation(renderer.getOrientation()) ? 0 : buttonHeaderHeight);
    int previewHeight = previewBaseHeight * metrics.previewHeightPercent / 100;
    if (readerDrawerSliderPreviewsText(state.pane) && !samplePreviewBesideControls()) {
      // Keep the sample height unchanged; smaller hints free space between the controls.
      ReaderSliderRowProps row;
      row.label = row.value = row.minimumLabel = row.maximumLabel = " ";
      row.captionGap = COMPACT_SLIDER_CAPTION_GAP;
      const int controlsHeight =
          screen.theme().headerHeight +
          (state.pane == ReaderDrawerPane::CharacterSpacing ? 1 : 2) * readerSliderRowHeight(screen, row) +
          2 * (screen.target().lineHeight(screen.theme().smallText.font) + screen.theme().spaceSm) +
          screen.theme().spaceSm + sheet.ruleWidth + metrics.verticalSpacing;
      previewHeight = std::min(previewHeight, std::max(0, screen.body().height - controlsHeight));
    }
    if (samplePreviewBesideControls()) {
      // Give the sample the full body height in a right-hand column; the pane
      // header and controls keep the left column.
      const fui::Rect body = screen.body();
      const int16_t width = static_cast<int16_t>(body.width * LANDSCAPE_SAMPLE_PREVIEW_WIDTH_PERCENT / 100);
      samplePreviewBounds = fui::Rect{static_cast<int16_t>(body.right() - width), body.y, width, body.height};
      screen.insetContent(fui::Insets{0, static_cast<int16_t>(width + metrics.verticalSpacing), 0, 0});
    } else {
      samplePreviewBounds =
          screen.takeTop(static_cast<int16_t>(previewHeight), static_cast<int16_t>(metrics.verticalSpacing));
    }
  }
#endif
  screen.insetContent(fui::Insets{sheet.ruleWidth, DRAWER_SIDE_INSET, 0, DRAWER_SIDE_INSET});

  switch (state.pane) {
    case ReaderDrawerPane::Root:
      screen.spacer(DRAWER_LIST_TOP_PADDING);
      buildRows(screen);
      break;
    case ReaderDrawerPane::CharacterSpacing:
      buildCharacterSpacingPane(screen);
      break;
    case ReaderDrawerPane::Spacing:
      buildSpacingPane(screen);
      break;
    case ReaderDrawerPane::Margins:
      buildMarginsPane(screen);
      break;
    case ReaderDrawerPane::Percent:
      buildPercentPane(screen);
      break;
    case ReaderDrawerPane::StablePage:
      buildStablePagePane(screen);
      break;
    case ReaderDrawerPane::AutoPageTurn:
      buildAutoPageTurnPane(screen);
      break;
    case ReaderDrawerPane::Dictionary:
      buildDictionaryPane(screen);
      break;
    case ReaderDrawerPane::FontFamily:
      buildFontFamilyPane(screen);
      break;
    case ReaderDrawerPane::EnumOptions:
      buildEnumOptionsPane(screen);
      break;
#if CROSSINK_SCALABLE_FONTS
    case ReaderDrawerPane::TtfRendering:
      buildTtfRenderingPane(screen);
      break;
#endif
    default:
      buildSimplePane(screen);
      break;
  }
}

void EpubReaderDrawerActivity::drawButtonBookHeader() {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput, safe);
  GUI.drawHeader(renderer, header, epub ? epub->getTitle().c_str() : "", nullptr, false, false, true);
  GUI.drawDisplayStatusBar(renderer, header.y, &header);

  char progress[96];
  formatReaderBookProgress(progress, sizeof(progress), chapterPage, chapterPageCount, chapterPageCountEstimated,
                           percentSeed / 100);
  ReaderBookSummary::draw(renderer, Rect{safe.x, header.y + header.height, safe.width, 0}, chapterTitle.c_str(),
                          progress);
}

void EpubReaderDrawerActivity::buildTabBar(UiApp::ScreenType& screen, const fui::Rect rect, const bool drawBottomRule) {
  const std::array<fui::BitmapRef, READER_DRAWER_TAB_COUNT> icons = {
      fui::bitmapFromIcon(icon_case_sensitive_32), fui::bitmapFromIcon(icon_text_align_start_24),
      fui::bitmapFromIcon(icon_ellipsis_24), fui::bitmapFromIcon(icon_bookmark_24), fui::bitmapFromIcon(icon_cog_24)};
  std::array<fui::TabItem, READER_DRAWER_TAB_COUNT> tabs{};
  for (size_t i = 0; i < tabs.size(); ++i) {
    tabs[i].icon = icons[i];
    tabs[i].value = static_cast<int16_t>(i);
    tabs[i].selected = static_cast<size_t>(state.tab) == i;
  }
  fui::TabBarProps props;
  props.tabs = tabs.data();
  props.count = static_cast<uint8_t>(tabs.size());
  props.action = ACTION_TAB;
  props.inputMask = fui::InputTouch;
  props.tabStyles = fui::plainStyles();
  props.tabStyles.selected.background = fui::Paint::solid(fui::Color::Black);
  props.tabStyles.selected.foreground = fui::Paint::solid(fui::Color::White);
  props.tabStyles.selected.radius = 4;
  if (!mappedInput.hasTouchHardware() && (state.pane != ReaderDrawerPane::Root || buttonFocusActive)) {
    props.tabStyles.selected.background = fui::Paint::solid(fui::Color::White);
    props.tabStyles.selected.foreground = fui::Paint::solid(fui::Color::Black);
    props.tabStyles.selected.border = fui::Paint::solid(fui::Color::Black);
    props.tabStyles.selected.borderWidth = 1;
  }
  props.tabInset = fui::Insets{static_cast<int16_t>(4 + TAB_BAR_VERTICAL_PADDING), 4,
                               static_cast<int16_t>(8 + TAB_BAR_VERTICAL_PADDING), 4};
  const int16_t ruleY = drawBottomRule ? static_cast<int16_t>(rect.bottom() - 1) : rect.y;
  screen.target().fill(fui::Rect{rect.x, ruleY, rect.width, 1}, fui::Paint::solid(fui::Color::Black));
  fui::tabBar(screen.frame(), rect, props);
}

void EpubReaderDrawerActivity::buildPaneHeader(UiApp::ScreenType& screen) {
  fui::HeaderProps header;
  header.title = paneTitle();
  header.centered = true;
  header.borderEdges = fui::EdgesNone;
  header.styles = fui::plainStyles();
  header.titleText = screen.theme().bodyText;
  header.titleText.bold = true;
  header.sidePadding = screen.theme().headerSidePadding;
  header.minTouchSize = screen.theme().minTouchSize;
  // Two sliders and their button hints need the landscape column's full height.
  const bool compactSliders =
      CROSSINK_APP_READER_SAMPLE_PREVIEW && samplePreviewBesideControls() && readerDrawerSliderPreviewsText(state.pane);
  const int16_t height =
      compactSliders ? screen.target().lineHeight(header.titleText.font) + 10 : screen.theme().headerHeight;
  const fui::Rect rect = screen.takeTop(height);
  fui::header(screen.frame(), rect, header);

  fui::ButtonProps back;
  back.icon = fui::bitmapFromIcon(icon_back_32);
  back.action = ACTION_BACK;
  back.styles = fui::plainStyles();
  back.minTouchSize = screen.theme().minTouchSize;
  const int16_t buttonSize = static_cast<int16_t>(rect.height - 8);
  // The caret stays at the left edge, but reserve a wide header-only tap band
  // to its right. It does not grow vertically into the pane's first control.
  back.hitPadding.right = std::max<int16_t>(0, static_cast<int16_t>(BACK_CARET_HIT_WIDTH - buttonSize));
  const int16_t centeredIconInset = std::max<int16_t>(0, static_cast<int16_t>((buttonSize - 8 - BACK_ICON_SIZE) / 2));
  const int16_t leadingInset =
      static_cast<int16_t>(BACK_CARET_VISUAL_INSET - 4 - centeredIconInset - BACK_ICON_VISIBLE_LEFT_INSET);
  fui::button(
      screen.frame(),
      fui::Rect{static_cast<int16_t>(rect.x + leadingInset), static_cast<int16_t>(rect.y + 4), buttonSize, buttonSize},
      back);
}

const std::vector<EpubReaderDrawerActivity::RowId>& EpubReaderDrawerActivity::activeRows() const {
  return state.pane == ReaderDrawerPane::Root ? rootRows[static_cast<size_t>(state.tab)] : paneRows;
}

int EpubReaderDrawerActivity::activeTopIndex() const {
  return state.pane == ReaderDrawerPane::Root ? state.rootTopIndex[static_cast<size_t>(state.tab)] : state.paneTopIndex;
}

void EpubReaderDrawerActivity::buildRows(UiApp::ScreenType& screen) {
  const auto& rows = activeRows();
  const fui::Rect listBounds = screen.body();
  const bool buttonDevice = !mappedInput.hasTouchHardware();
  const int16_t rowHeight =
      buttonDevice ? uiListRowHeight(screen.theme(), UiListRowType::SingleLine) : screen.theme().rowHeight;
  const int16_t gap = buttonDevice ? static_cast<int16_t>(screen.theme().spaceMd * rowHeight / screen.theme().rowHeight)
                                   : screen.theme().spaceSm;
  visibleRows = std::max(1, readerDrawerVisibleRows(listBounds.height, rowHeight, gap));
  auto& topIndex =
      state.pane == ReaderDrawerPane::Root ? state.rootTopIndex[static_cast<size_t>(state.tab)] : state.paneTopIndex;
  const int top = std::clamp<int>(topIndex, 0, std::max(0, static_cast<int>(rows.size()) - visibleRows));
  topIndex = static_cast<int16_t>(top);
  const int displayedRows = std::min(visibleRows, static_cast<int>(rows.size()) - top);
  for (int i = 0; i < displayedRows; ++i) {
    const RowId row = rows[static_cast<size_t>(top + i)];
    char value[48] = {};
    fui::SettingRowProps props;
    props.label = rowLabel(row);
    props.value = rowValue(row, value, sizeof(value));
    props.action = ACTION_ROW;
    props.valueId = static_cast<int16_t>(top + i);
    props.drawChevron = rowShowsNavigationCaret(row);
    props.labelText = screen.theme().bodyText;
    props.valueText = screen.theme().smallText;
    if (buttonDevice) props.minTouchSize = rowHeight;
    setDrawerSelectionStyle(props.styles);
    props.state = isReaderDrawerRowFocused(buttonFocusActive, state.selectedIndex, static_cast<int16_t>(top + i))
                      ? fui::StateSelected
                      : fui::StateNormal;
    if (state.pane == ReaderDrawerPane::Root && rowIsToggle(row)) {
      fui::ToggleRowProps toggle;
      toggle.row = props;
      toggle.checked = rowToggleValue(row);
      toggle.toggleAction = ACTION_ROW;
      toggle.toggleValue = static_cast<int16_t>(top + i);
      screen.toggleRow(toggle, buttonDevice ? rowHeight : 0);
    } else {
      screen.settingRow(props, buttonDevice ? rowHeight : 0);
    }
  }
  fui::drawListScrollIndicator(screen.target(), drawerScrollbarBounds(listBounds), rows.size(), visibleRows, top,
                               screen.theme().listScrollWidth, screen.theme().listScrollSide,
                               screen.theme().listScrollInset);
}

void EpubReaderDrawerActivity::buildSimplePane(UiApp::ScreenType& screen) {
  buildPaneHeader(screen);
  buildRows(screen);
}

void EpubReaderDrawerActivity::buildCharacterSpacingPane(UiApp::ScreenType& screen) {
  buildPaneHeader(screen);
  char value[8];
  CrossPointSettings::formatCharacterSpacing(draft.characterSpacing, value, sizeof(value));
  ReaderSliderRowProps row;
  row.value = value;
  row.sliderValue = byteToPermille(draft.characterSpacing, 0, CrossPointSettings::MAX_CHARACTER_SPACING);
  row.sliderAction = ACTION_SLIDER;
  row.max = 1000;
  row.decrement = ACTION_STEP;
  row.increment = ACTION_STEP;
  configureReaderSliderScale(row, "-5", "+5");
  row.focused = !mappedInput.hasTouchHardware();
  row.editing = row.focused && buttonSliderState.editing;
  drawReaderSliderRow(screen, row);
  if (!mappedInput.hasTouchHardware()) drawButtonSliderStepHints(screen, 1, 5, "");
}

void EpubReaderDrawerActivity::buildSpacingPane(UiApp::ScreenType& screen) {
  buildPaneHeader(screen);
  char lineValue[16];
  std::snprintf(lineValue, sizeof(lineValue), "%u%%", draft.lineHeightPercent);
  ReaderSliderRowProps line;
  line.label = tr(STR_LINE_SPACING);
  line.value = lineValue;
  line.sliderValue = byteToPermille(draft.lineHeightPercent, CrossPointSettings::MIN_LINE_HEIGHT_PERCENT,
                                    CrossPointSettings::MAX_LINE_HEIGHT_PERCENT);
  line.max = 1000;
  line.sliderAction = ACTION_SLIDER;
  line.decrement = ACTION_STEP;
  line.increment = ACTION_STEP;
  configureReaderSliderScale(line, "70%", "200%");
  ReaderSliderRowProps word;
  word.label = tr(STR_WORD_SPACING);
  word.value = wordSpacingValue(draft.wordSpacing);
  word.sliderValue =
      byteToPermille(WordSpacing::sliderValue(draft.wordSpacing), 0, CrossPointSettings::MAX_WORD_SPACING);
  word.max = 1000;
  word.sliderAction = ACTION_SLIDER + 3;
  word.decrement = ACTION_STEP + 3;
  word.increment = ACTION_STEP + 3;
  configureReaderSliderScale(word, "-4", "4");
  line.compact = word.compact = CROSSINK_APP_READER_SAMPLE_PREVIEW && samplePreviewBesideControls();
  line.captionGap = COMPACT_SLIDER_CAPTION_GAP;
  word.captionGap = COMPACT_SLIDER_CAPTION_GAP;
  line.focused = !mappedInput.hasTouchHardware() && buttonSliderState.focus == 0;
  word.focused = !mappedInput.hasTouchHardware() && buttonSliderState.focus == 1;
  line.editing = line.focused && buttonSliderState.editing;
  word.editing = word.focused && buttonSliderState.editing;
  if (mappedInput.hasTouchHardware()) alignReaderSliderLabels(screen, line, word);
  drawDualReaderSliderRows(screen, line, word, !mappedInput.hasTouchHardware());
}

void EpubReaderDrawerActivity::buildMarginsPane(UiApp::ScreenType& screen) {
  buildPaneHeader(screen);
  char verticalValue[16];
  char horizontalValue[16];
  std::snprintf(verticalValue, sizeof(verticalValue), "%u", draft.screenMarginVertical);
  std::snprintf(horizontalValue, sizeof(horizontalValue), "%u", draft.screenMarginHorizontal);
  ReaderSliderRowProps vertical;
  vertical.label = tr(STR_TOP_BOTTOM);
  vertical.value = verticalValue;
  vertical.sliderValue = byteToPermille(draft.screenMarginVertical, CrossPointSettings::MIN_SCREEN_MARGIN,
                                        CrossPointSettings::MAX_SCREEN_MARGIN);
  vertical.max = 1000;
  vertical.sliderAction = ACTION_SLIDER;
  vertical.decrement = ACTION_STEP;
  vertical.increment = ACTION_STEP;
  configureReaderSliderScale(vertical, "5", "150");
  ReaderSliderRowProps horizontal;
  horizontal.label = tr(STR_LEFT_RIGHT);
  horizontal.value = horizontalValue;
  horizontal.sliderValue = byteToPermille(draft.screenMarginHorizontal, CrossPointSettings::MIN_SCREEN_MARGIN,
                                          CrossPointSettings::MAX_SCREEN_MARGIN);
  horizontal.max = 1000;
  horizontal.sliderAction = ACTION_SLIDER + 3;
  horizontal.decrement = ACTION_STEP + 3;
  horizontal.increment = ACTION_STEP + 3;
  configureReaderSliderScale(horizontal, "5", "150");
  vertical.compact = horizontal.compact = CROSSINK_APP_READER_SAMPLE_PREVIEW && samplePreviewBesideControls();
  vertical.captionGap = COMPACT_SLIDER_CAPTION_GAP;
  horizontal.captionGap = COMPACT_SLIDER_CAPTION_GAP;
  vertical.focused = !mappedInput.hasTouchHardware() && buttonSliderState.focus == 0;
  horizontal.focused = !mappedInput.hasTouchHardware() && buttonSliderState.focus == 1;
  vertical.editing = vertical.focused && buttonSliderState.editing;
  horizontal.editing = horizontal.focused && buttonSliderState.editing;
  if (mappedInput.hasTouchHardware()) alignReaderSliderLabels(screen, vertical, horizontal);
  drawDualReaderSliderRows(screen, vertical, horizontal, !mappedInput.hasTouchHardware());
}

void EpubReaderDrawerActivity::buildPercentPane(UiApp::ScreenType& screen) {
  buildPaneHeader(screen);
  if (!mappedInput.hasTouchHardware() && !percentKeypadActive) {
    buildPercentSlider(screen);
    return;
  }
  char value[16];
  // Nothing typed yet: show the actual value the OK key would use (the position the
  // pane opened with), not a placeholder that would misrepresent what confirming does.
  if (entryLen > 0) {
    std::snprintf(value, sizeof(value), "%s%%", entryText);
  } else if (percentKeypadActive) {
    std::snprintf(value, sizeof(value), "0%%");
  } else {
    std::snprintf(value, sizeof(value), "%d.%02d%%", percent / 100, percent % 100);
  }
  buildDrawerKeypad(screen, /*allowDecimal=*/true, value);
}

void EpubReaderDrawerActivity::buildPercentSlider(UiApp::ScreenType& screen) {
  const auto& theme = screen.theme();
  fui::TextStyle readout = theme.titleText;
  readout.align = fui::TextAlign::Center;
  const int16_t readoutHeight = screen.target().lineHeight(readout.font);
  const int16_t hintHeight = screen.target().lineHeight(theme.smallText.font);
  const int16_t stepHintHeight = screen.target().lineHeight(ReaderSliderHints::style(theme).font);
  const int16_t groupHeight = static_cast<int16_t>(readoutHeight + SLIDER_CONTROL_HEIGHT + stepHintHeight * 2 +
                                                   hintHeight + theme.spaceLg * 2 + theme.spaceSm * 2);
  screen.spacer(std::max<int16_t>(0, static_cast<int16_t>((screen.body().height - groupHeight) / 2)));

  char value[16];
  std::snprintf(value, sizeof(value), "%d.%02d%%", percent / 100, percent % 100);
  screen.target().text(screen.takeTop(readoutHeight, theme.spaceLg), value, readout);

  // The slider is visual only on button devices. Physical buttons change the
  // value directly, preserving the old selector's 1% and 10% steps.
  const fui::Insets sideInset{0, static_cast<int16_t>(theme.spaceLg * 2), 0, static_cast<int16_t>(theme.spaceLg * 2)};
  const fui::Rect row = screen.takeTop(SLIDER_CONTROL_HEIGHT, theme.spaceLg).inset(sideInset);
  fui::SliderProps slider;
  slider.value = percent / 10;
  slider.max = 1000;
  fui::slider(screen.frame(), row, slider);

  drawButtonSliderStepHints(screen, 1, 10, "%");
  screen.spacer(theme.spaceSm);
  char keyboardHint[64];
  std::snprintf(keyboardHint, sizeof(keyboardHint), I18N.get(StrId::STR_HOLD_FOR_KEYBOARD), tr(STR_SELECT));
  fui::TextStyle hint = theme.smallText;
  hint.align = fui::TextAlign::Center;
  screen.target().text(screen.takeTop(hintHeight), keyboardHint, hint);
}

void EpubReaderDrawerActivity::buildStablePagePane(UiApp::ScreenType& screen) {
  buildPaneHeader(screen);
  char value[32];
  if (entryLen > 0) {
    std::snprintf(value, sizeof(value), "%s / %lu", entryText, static_cast<unsigned long>(stablePageCount));
  } else {
    std::snprintf(value, sizeof(value), "%lu / %lu", static_cast<unsigned long>(stablePage),
                  static_cast<unsigned long>(stablePageCount));
  }
  buildDrawerKeypad(screen, /*allowDecimal=*/false, value);
}

// Shared by the Percent and StablePage panes: a live readout with a backspace icon,
// and a 4x3 grid (1-9 / 0, ., OK). There is no separate Confirm button here (unlike
// every other pane) - the drawer sheet's height budget only has room for the grid
// itself once the header and readout are accounted for, so OK lives in the grid
// instead, matching EpubReaderPercentSelectionActivity's non-touch keypad. The
// button device moves focus through the grid and uses Confirm on the selected key.
void EpubReaderDrawerActivity::buildDrawerKeypad(UiApp::ScreenType& screen, const bool allowDecimal,
                                                 const char* value) {
  const auto& theme = screen.theme();
  fui::TextStyle readout = theme.titleText;
  readout.align = fui::TextAlign::Center;
  const int16_t readoutLh = screen.target().lineHeight(readout.font);
  const int16_t readoutRowH = std::max<int16_t>(theme.rowHeight, static_cast<int16_t>(readoutLh + 16));
  const fui::Rect readoutRow = screen.takeTop(readoutRowH, theme.spaceLg);
  const int16_t iconSize = readoutRow.height;
  const fui::Rect iconRect{static_cast<int16_t>(readoutRow.right() - iconSize), readoutRow.y, iconSize,
                           readoutRow.height};
  // The icon sits in unused space at the row's edge; keep the readout centered
  // in the full row instead of shifting it left when the icon appears.
  screen.target().text(readoutRow, value, readout);

  if (entryLen > 0 || !mappedInput.hasTouchHardware()) {
    fui::ButtonProps backspaceBtn;
    backspaceBtn.icon = fui::bitmapFromIcon(icon_backspace_28);
    backspaceBtn.action = ACTION_KEYPAD_BACKSPACE;
    backspaceBtn.inputMask = mappedInput.hasTouchHardware() ? fui::InputTouch : fui::InputNone;
    backspaceBtn.state = keypadBackspaceFocused ? fui::StateSelected : fui::StateNormal;
    backspaceBtn.enabled = entryLen > 0;
    screen.button(backspaceBtn, iconRect);
  }

  fui::Rect gridArea = screen.body().inset(fui::Insets{0, theme.spaceLg, theme.spaceLg, theme.spaceLg});
  gridArea.height = std::max<int16_t>(0, gridArea.height);

  static const char* const kDigitLabels[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
  for (int i = 0; i < 9; ++i) {
    keypadKeys[static_cast<size_t>(i)].label = kDigitLabels[i + 1];
    keypadKeys[static_cast<size_t>(i)].value = static_cast<int16_t>(i + 1);
  }
  keypadKeys[9].label = kDigitLabels[0];
  keypadKeys[9].value = 0;
  keypadKeys[10].label = ".";
  keypadKeys[10].value = KEYPAD_DOT;
  keypadKeys[10].enabled = allowDecimal;
  keypadKeys[11].label = tr(STR_OK);
  keypadKeys[11].value = KEYPAD_OK;

  fui::KeyGridProps gridProps;
  gridProps.keys = keypadKeys.data();
  gridProps.rows = 4;
  gridProps.cols = 3;
  gridProps.action = ACTION_KEYPAD_KEY;
  gridProps.inputMask = fui::InputTouch;
  if (!mappedInput.hasTouchHardware() && !keypadBackspaceFocused)
    gridProps.selectedIndex = static_cast<int16_t>(keypadRow * 3 + keypadCol);
  gridProps.labelText = theme.bodyText;
  gridProps.labelText.align = fui::TextAlign::Center;
  gridProps.gap = theme.spaceSm;
  // Let cells shrink below the theme's usual touch minimum rather than let
  // ensureMinTouchRect() inflate a hit rect into overlapping a neighboring row or
  // column, which would route a tap to the wrong key. Derived from whichever axis is
  // tighter; on this 800-wide panel it is always the 4-row height, not the 3-column
  // width, but deriving from both keeps the invariant true regardless of orientation.
  const int16_t cellH = static_cast<int16_t>((gridArea.height - gridProps.gap * 3) / 4);
  const int16_t cellW = static_cast<int16_t>((gridArea.width - gridProps.gap * 2) / 3);
  const int16_t tightestCell = std::min<int16_t>(cellH, cellW);
  gridProps.minTouchSize = std::min<int16_t>(theme.minTouchSize, std::max<int16_t>(1, tightestCell));
  gridProps.radius = 3;
  fui::keyGrid(screen.frame(), gridArea, gridProps);
}

void EpubReaderDrawerActivity::buildAutoPageTurnPane(UiApp::ScreenType& screen) {
  buildPaneHeader(screen);
  const bool buttonDevice = !mappedInput.hasTouchHardware();
  if (!buttonDevice) buildConfirmButton(screen);
  char value[16];
  std::snprintf(value, sizeof(value), "%us", autoPageTurnIntervalSeconds);
  ReaderSliderRowProps slider;
  slider.value = value;
  slider.sliderValue =
      byteToPermille(autoPageTurnIntervalSeconds, READER_AUTO_PAGE_TURN_MIN_SECONDS, READER_AUTO_PAGE_TURN_MAX_SECONDS);
  slider.max = 1000;
  slider.sliderAction = ACTION_SLIDER;
  slider.decrement = ACTION_STEP;
  slider.increment = ACTION_STEP;
  configureReaderSliderScale(slider, "5s", "120s");
  const int16_t hintLineHeight =
      buttonDevice ? screen.target().lineHeight(ReaderSliderHints::style(screen.theme()).font) : 0;
  const int16_t hintsHeight =
      buttonDevice ? static_cast<int16_t>(hintLineHeight * 2 + screen.theme().spaceMd + screen.theme().spaceSm) : 0;
  const int16_t top =
      buttonDevice
          ? std::max<int16_t>(0, static_cast<int16_t>(
                                     (screen.body().height - readerSliderRowHeight(screen, slider) - hintsHeight) / 2))
          : centeredReaderSliderControlTop(screen, slider);
  screen.spacer(top);
  drawReaderSliderRow(screen, slider);
  if (buttonDevice) {
    screen.spacer(screen.theme().spaceMd);
    drawButtonSliderStepHints(screen, 1, 5, "");
  }
}

void EpubReaderDrawerActivity::buildConfirmButton(UiApp::ScreenType& screen) {
  const int16_t buttonHeight = std::max<int16_t>(screen.theme().minTouchSize, screen.theme().rowHeight);
  // `takeBottom(..., gap)` creates space above a control. Reserve this band
  // first so Confirm clears the navigation tabs on touch devices.
  screen.spacer(screen.theme().spaceLg, fui::LayoutAnchor::Bottom);
  const fui::Rect rect = screen.takeBottom(buttonHeight, screen.theme().spaceLg);
  fui::ButtonProps confirm;
  confirm.label = tr(STR_CONFIRM);
  confirm.action = ACTION_CONFIRM;
  confirm.inputMask = fui::InputTouch;
  confirm.styles = fui::outlinedButtonStyles();
  confirm.text = screen.theme().bodyText;
  confirm.text.align = fui::TextAlign::Center;
  confirm.text.bold = true;
  confirm.minTouchSize = buttonHeight;
  screen.button(confirm, rect);
}

void EpubReaderDrawerActivity::buildDictionaryPane(UiApp::ScreenType& screen) {
  buildPaneHeader(screen);
  const int total = static_cast<int>(dictionaryLabels.size());
  fui::ListProps props;
  visibleRows = configureDrawerList(props, screen.theme(), screen.body());
  const int top = std::clamp<int>(state.paneTopIndex, 0, std::max(0, total - visibleRows));
  state.paneTopIndex = static_cast<int16_t>(top);
  const int drawCount = std::min<int>({visibleRows, WINDOW_SIZE, total - top});
  for (int i = 0; i < drawCount; ++i) {
    itemWindow[static_cast<size_t>(i)] = fui::ListItem{};
    itemWindow[static_cast<size_t>(i)].label = dictionaryLabels[static_cast<size_t>(top + i)].c_str();
    itemWindow[static_cast<size_t>(i)].actionValue = static_cast<int16_t>(top + i);
  }
  props.items = itemWindow.data();
  props.count = static_cast<uint16_t>(std::max(0, drawCount));
  props.action = ACTION_ROW;
  props.selectedIndex = readerDrawerFocusedWindowIndex(buttonFocusActive, state.selectedIndex, top);
  props.inputMask = fui::InputTouch;
  screen.list(props);
  fui::drawListScrollIndicator(screen.target(), drawerScrollbarBounds(screen.body()), total, visibleRows, top,
                               screen.theme().listScrollWidth, screen.theme().listScrollSide,
                               screen.theme().listScrollInset);
}

int EpubReaderDrawerActivity::currentFontSelectionIndex() const {
  int selectedFontIndex = state.pendingFontIndex;
  if (selectedFontIndex < 0) {
    if (draft.sdFontFamilyName[0] != '\0') {
      const auto& families = sdFontSystem.registry().getFamilies();
      const auto selected = std::find_if(families.begin(), families.end(), [this](const auto& family) {
        return family.name == draft.sdFontFamilyName.data();
      });
      if (selected != families.end()) {
        selectedFontIndex =
            static_cast<int>(CrossPointSettings::BUILTIN_FONT_COUNT + std::distance(families.begin(), selected));
      }
    } else {
      const auto selected = std::find(fontSettingIndexes.begin(), fontSettingIndexes.end(), draft.fontFamily);
      if (selected != fontSettingIndexes.end()) {
        selectedFontIndex = static_cast<int>(std::distance(fontSettingIndexes.begin(), selected));
      }
    }
  }
  return selectedFontIndex;
}

void EpubReaderDrawerActivity::buildFontFamilyPane(UiApp::ScreenType& screen) {
  buildPaneHeader(screen);
  const int total = static_cast<int>(fontLabels.size());
  const fui::Rect listBounds = screen.body();
  fui::ListProps props;
  // This picker has no subtitles, so it does not need the default
  // label-plus-subtitle row height that left a conspicuous blank band above
  // the tab row when scrolling.
  props.rowHeight =
      !mappedInput.hasTouchHardware()
          ? uiListRowHeight(screen.theme(), UiListRowType::SingleLine)
          : std::max<int16_t>(screen.theme().minTouchSize,
                              static_cast<int16_t>(screen.theme().rowHeight - FONT_FAMILY_ROW_HEIGHT_REDUCTION));
  visibleRows = configureDrawerList(props, screen.theme(), listBounds);
  const int top = std::clamp<int>(state.paneTopIndex, 0, std::max(0, total - visibleRows));
  state.paneTopIndex = static_cast<int16_t>(top);
  const int drawCount = std::min<int>({visibleRows, WINDOW_SIZE, total - top});
  if (!CROSSINK_APP_READER_SAMPLE_PREVIEW) evenlySpaceDrawerListRows(props, listBounds, drawCount);
  const int selectedFontIndex = currentFontSelectionIndex();
  for (int i = 0; i < drawCount; ++i) {
    itemWindow[static_cast<size_t>(i)] = fui::ListItem{};
    itemWindow[static_cast<size_t>(i)].label = fontLabels[static_cast<size_t>(top + i)].c_str();
    itemWindow[static_cast<size_t>(i)].value = top + i == selectedFontIndex ? tr(STR_SELECTED) : nullptr;
    itemWindow[static_cast<size_t>(i)].actionValue = static_cast<int16_t>(top + i);
  }
  props.items = itemWindow.data();
  props.count = static_cast<uint16_t>(std::max(0, drawCount));
  props.action = ACTION_ROW;
  props.selectedIndex = readerDrawerFocusedWindowIndex(buttonFocusActive, state.selectedIndex, top);
  props.inputMask = fui::InputTouch;
  screen.list(props);
  fui::drawListScrollIndicator(screen.target(), drawerScrollbarBounds(screen.body()), total, visibleRows, top,
                               screen.theme().listScrollWidth, screen.theme().listScrollSide,
                               screen.theme().listScrollInset);
}

void EpubReaderDrawerActivity::buildEnumOptionsPane(UiApp::ScreenType& screen) {
  buildPaneHeader(screen);
  const int total = static_cast<int>(enumOptionLabels.size());
  const fui::Rect listBounds = screen.body();
  fui::ListProps props;
  visibleRows = configureDrawerList(props, screen.theme(), listBounds);
  const int top = std::clamp<int>(state.paneTopIndex, 0, std::max(0, total - visibleRows));
  state.paneTopIndex = static_cast<int16_t>(top);
  const int drawCount = std::min<int>({visibleRows, WINDOW_SIZE, total - top});
  if (!CROSSINK_APP_READER_SAMPLE_PREVIEW &&
      (enumOptionRow == RowId::FontSize || enumOptionRow == RowId::DictionaryFontFamily ||
       enumOptionRow == RowId::DictionaryFontSize)) {
    evenlySpaceDrawerListRows(props, listBounds, drawCount);
  }
  const int selectedIndex = previewedEnumOptionIndex >= 0 ? previewedEnumOptionIndex : enumOptionSelectedIndex;
  for (int i = 0; i < drawCount; ++i) {
    itemWindow[static_cast<size_t>(i)] = fui::ListItem{};
    itemWindow[static_cast<size_t>(i)].label = enumOptionLabels[static_cast<size_t>(top + i)].c_str();
    itemWindow[static_cast<size_t>(i)].value = top + i == selectedIndex ? tr(STR_SELECTED) : nullptr;
    itemWindow[static_cast<size_t>(i)].actionValue = static_cast<int16_t>(top + i);
  }
  props.items = itemWindow.data();
  props.count = static_cast<uint16_t>(std::max(0, drawCount));
  props.action = ACTION_ROW;
  props.selectedIndex = readerDrawerFocusedWindowIndex(buttonFocusActive, state.selectedIndex, top);
  props.inputMask = fui::InputTouch;
  screen.list(props);
  fui::drawListScrollIndicator(screen.target(), drawerScrollbarBounds(screen.body()), total, visibleRows, top,
                               screen.theme().listScrollWidth, screen.theme().listScrollSide,
                               screen.theme().listScrollInset);
}

#if CROSSINK_SCALABLE_FONTS
void EpubReaderDrawerActivity::buildTtfRenderingPane(UiApp::ScreenType& screen) {
  buildPaneHeader(screen);
  const auto& rows = activeRows();
  const int total = static_cast<int>(rows.size());
  const fui::Rect listBounds = screen.body();
  fui::ListProps props;
  visibleRows = configureDrawerList(props, screen.theme(), listBounds);
  const int top = std::clamp<int>(state.paneTopIndex, 0, std::max(0, total - visibleRows));
  state.paneTopIndex = static_cast<int16_t>(top);
  const int drawCount = std::min<int>({visibleRows, WINDOW_SIZE, total - top});
  for (int i = 0; i < drawCount; ++i) {
    const RowId row = rows[static_cast<size_t>(top + i)];
    itemWindow[static_cast<size_t>(i)] = fui::ListItem{};
    itemWindow[static_cast<size_t>(i)].label = rowLabel(row);
    itemWindow[static_cast<size_t>(i)].value =
        rowValue(row, labelWindow[static_cast<size_t>(i)].data(), labelWindow[static_cast<size_t>(i)].size());
    itemWindow[static_cast<size_t>(i)].actionValue = static_cast<int16_t>(top + i);
    itemWindow[static_cast<size_t>(i)].toggle = rowIsToggle(row);
    itemWindow[static_cast<size_t>(i)].toggleChecked = rowToggleValue(row);
    if (itemWindow[static_cast<size_t>(i)].toggle) itemWindow[static_cast<size_t>(i)].value = nullptr;
  }
  props.items = itemWindow.data();
  props.count = static_cast<uint16_t>(std::max(0, drawCount));
  props.action = ACTION_ROW;
  props.selectedIndex = readerDrawerFocusedWindowIndex(buttonFocusActive, state.selectedIndex, top);
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  screen.list(props);
  fui::drawListScrollIndicator(screen.target(), drawerScrollbarBounds(listBounds), total, visibleRows, top,
                               screen.theme().listScrollWidth, screen.theme().listScrollSide,
                               screen.theme().listScrollInset);
}
#endif

void EpubReaderDrawerActivity::openPane(const ReaderDrawerPane pane) {
  state.pane = pane;
  state.paneTopIndex = 0;
  state.selectedIndex = 0;
  buttonSliderState = {};
  if (pane == ReaderDrawerPane::FontFamily) {
    const int currentIndex = currentFontSelectionIndex();
    state.selectedIndex = static_cast<int16_t>(std::max(0, currentIndex));
    state.paneTopIndex = state.selectedIndex;
    buttonFocusActive = currentIndex >= 0;
  }
  if (pane == ReaderDrawerPane::Percent || pane == ReaderDrawerPane::StablePage) {
    percentKeypadActive = false;
    percentConfirmLongPressFired = false;
    keypadRow = 0;
    keypadCol = 0;
    keypadBackspaceFocused = false;
    resetKeypadEntry();
  }
  paneRows.clear();
  if (pane == ReaderDrawerPane::ReaderFont) {
    paneRows = {RowId::FontFamily, RowId::FontSize, RowId::CharacterSpacing};
    refreshTtfRenderingRow();
  }
  if (pane == ReaderDrawerPane::DictionaryFont) {
    paneRows = {RowId::DictionaryFontFamily, RowId::DictionaryFontSize};
  }
#if CROSSINK_SCALABLE_FONTS
  if (pane == ReaderDrawerPane::TtfRendering) {
    ttfRenderProfile = TTF_RENDER_PROFILES.profileFor(draft.sdFontFamilyName.data());
    initialTtfRenderProfile = ttfRenderProfile;
    ttfRenderingChanged = false;
    rebuildTtfRenderingRows();
  }
#endif
  requestUpdate();
}

void EpubReaderDrawerActivity::closePane() {
  buttonSliderState.editing = false;
  percentKeypadActive = false;
  if (state.pane == ReaderDrawerPane::Root) {
    closeAndReturn(true);
    return;
  }
  if (state.pane == ReaderDrawerPane::EnumOptions) {
    state.pane = enumOptionReturnPane;
  } else if (state.pane == ReaderDrawerPane::FontFamily || state.pane == ReaderDrawerPane::CharacterSpacing) {
    state.pane = ReaderDrawerPane::ReaderFont;
#if CROSSINK_SCALABLE_FONTS
  } else if (state.pane == ReaderDrawerPane::TtfRendering) {
    finishTtfRenderingEdit();
    state.pane = ReaderDrawerPane::ReaderFont;
#endif
  } else {
    state.pane = ReaderDrawerPane::Root;
  }
  paneRows.clear();
  if (state.pane == ReaderDrawerPane::ReaderFont) {
    paneRows = {RowId::FontFamily, RowId::FontSize, RowId::CharacterSpacing};
    refreshTtfRenderingRow();
  }
  if (state.pane == ReaderDrawerPane::DictionaryFont) {
    paneRows = {RowId::DictionaryFontFamily, RowId::DictionaryFontSize};
  }
#if CROSSINK_SCALABLE_FONTS
  if (state.pane == ReaderDrawerPane::TtfRendering) rebuildTtfRenderingRows();
#endif
  state.paneTopIndex = 0;
  state.selectedIndex = 0;
  if (!mappedInput.hasTouchHardware() && state.pane == ReaderDrawerPane::Root) buttonFocusActive = false;
  requestUpdate();
}

void EpubReaderDrawerActivity::changeTab(const ReaderDrawerTab tab) {
#if CROSSINK_SCALABLE_FONTS
  finishTtfRenderingEdit();
#endif
  state.tab = tab;
  state.pane = ReaderDrawerPane::Root;
  percentKeypadActive = false;
  state.selectedIndex = 0;
  if (!mappedInput.hasTouchHardware()) buttonFocusActive = false;
  requestUpdate();
}

void EpubReaderDrawerActivity::activateListIndex(const int index) {
  if (state.pane == ReaderDrawerPane::EnumOptions) {
    selectEnumOption(index);
    return;
  }
  if (state.pane == ReaderDrawerPane::Dictionary) {
    if (index < 0 || index >= static_cast<int>(dictionaryPaths.size())) return;
    if (saveBookDictionary(dictionaryPaths[static_cast<size_t>(index)])) {
      bookDictionaryPath = dictionaryPaths[static_cast<size_t>(index)];
      state.tab = ReaderDrawerTab::Settings;
      state.pane = ReaderDrawerPane::Root;
      state.selectedIndex = 1;
      requestUpdate();
    }
    return;
  }
  if (state.pane == ReaderDrawerPane::FontFamily) {
    if (index < 0 || index >= static_cast<int>(fontSettingIndexes.size())) return;
    if (state.pendingFontIndex == index) {
      state.pane = ReaderDrawerPane::ReaderFont;
      paneRows = {RowId::FontFamily, RowId::FontSize, RowId::CharacterSpacing};
      refreshTtfRenderingRow();
      state.pendingFontIndex = -1;
      state.selectedIndex = 0;
      requestUpdate();
      return;
    }
    fontPreviewLoading = true;
    state.pendingFontIndex = static_cast<int16_t>(index);
    const uint8_t settingIndex = fontSettingIndexes[static_cast<size_t>(index)];
    if (settingIndex < CrossPointSettings::BUILTIN_FONT_COUNT) {
      draft.fontFamily = settingIndex;
      draft.sdFontFamilyName[0] = '\0';
    } else {
      const int familyIndex = settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT;
      const auto& families = sdFontSystem.registry().getFamilies();
      if (familyIndex >= 0 && familyIndex < static_cast<int>(families.size())) {
        std::strncpy(draft.sdFontFamilyName.data(), families[static_cast<size_t>(familyIndex)].name.c_str(),
                     draft.sdFontFamilyName.size() - 1);
        draft.sdFontFamilyName.back() = '\0';
        // The render task loads the draft font when rebuilding its preview.
      }
    }
    markSettingChanged(ReaderSettingsChangeMask::Preview | ReaderSettingsChangeMask::Relayout);
    requestUpdate();
    return;
  }

  const auto& rows = activeRows();
  if (index < 0 || index >= static_cast<int>(rows.size())) return;
  activateRow(rows[static_cast<size_t>(index)]);
}

void EpubReaderDrawerActivity::activateRow(const RowId row) {
  switch (row) {
    case RowId::CharacterSpacing:
      openPane(ReaderDrawerPane::CharacterSpacing);
      return;
    case RowId::ReaderFont:
      openPane(ReaderDrawerPane::ReaderFont);
      return;
    case RowId::TtfRendering:
#if CROSSINK_SCALABLE_FONTS
      openPane(ReaderDrawerPane::TtfRendering);
#endif
      return;
#if CROSSINK_SCALABLE_FONTS
    case RowId::TtfHinting:
    case RowId::TtfRaster:
    case RowId::TtfInterpreter:
    case RowId::TtfWeight:
    case RowId::TtfSlant:
      showTtfRenderingOptions(row);
      return;
    case RowId::TtfStemDarkening:
      ttfRenderProfile.stemDarkening = !ttfRenderProfile.stemDarkening;
      saveTtfRenderingProfile();
      requestUpdate();
      return;
    case RowId::TtfReset: {
      const TtfRenderProfile previous = ttfRenderProfile;
      ttfRenderProfile = {};
      const bool saved = saveTtfRenderingProfile();
      if (!saved) ttfRenderProfile = previous;
      rebuildTtfRenderingRows();
      static constexpr StrId options[] = {StrId::STR_OK};
      ttfResetPopup.show(saved ? StrId::STR_TTF_DEFAULTS_RESET : StrId::STR_TTF_RESET_FAILED, options, 1, 0, nullptr);
      requestUpdate();
      return;
    }
#endif
    case RowId::FontFamily:
      discoverFonts();
      openPane(ReaderDrawerPane::FontFamily);
      return;
    case RowId::FontSize:
    case RowId::DictionaryFontFamily:
    case RowId::DictionaryFontSize:
    case RowId::Orientation:
    case RowId::Alignment:
    case RowId::Images:
    case RowId::PublisherFonts:
    case RowId::RenderMode:
      showEnumOptions(row);
      return;
    case RowId::IndexingMethod:
      draft.indexingMethod = draft.indexingMethod == 0 ? 1 : 0;
      markSettingChanged(ReaderSettingsChangeMask::Relayout);
      requestUpdate();
      return;
    case RowId::Spacing:
      openPane(ReaderDrawerPane::Spacing);
      return;
    case RowId::Margins:
      openPane(ReaderDrawerPane::Margins);
      return;
    case RowId::SelectChapter:
      closeAndReturn(false, EpubReaderMenuAction::SELECT_CHAPTER, !mappedInput.hasTouchHardware());
      return;
    case RowId::GoToPercent:
      openPane(ReaderDrawerPane::Percent);
      return;
    case RowId::GoToStablePage:
      if (!mappedInput.hasTouchHardware()) {
        closeAndReturn(false, EpubReaderMenuAction::GO_TO_STABLE_PAGE, true);
        return;
      }
      openPane(ReaderDrawerPane::StablePage);
      return;
    case RowId::AutoPageTurn:
      openPane(ReaderDrawerPane::AutoPageTurn);
      return;
    case RowId::BookDictionary:
      openPane(ReaderDrawerPane::Dictionary);
      return;
    case RowId::DictionaryFont:
      openPane(ReaderDrawerPane::DictionaryFont);
      return;
    case RowId::StatusBar:
      if (!mappedInput.hasTouchHardware()) {
        closeAndReturn(false, EpubReaderMenuAction::STATUS_BAR_SETTINGS, true);
        return;
      }
      commitSettings();
      if (auto statusBar =
              makeUniqueNoThrow<StatusBarSettingsActivity>(renderer, mappedInput, true, stablePageCount > 0)) {
        if (beginGlobalSettingsEditCallback) beginGlobalSettingsEditCallback(beginGlobalSettingsEditContext);
        startActivityForResult(std::move(statusBar), [this](const ActivityResult&) {
          if (saveGlobalSettingsCallback) saveGlobalSettingsCallback(saveGlobalSettingsContext);
          if (endGlobalSettingsEditCallback) endGlobalSettingsEditCallback(endGlobalSettingsEditContext);
          requestUpdate();
        });
      }
      return;
    case RowId::Controls:
      if (!mappedInput.hasTouchHardware()) {
        closeAndReturn(false, EpubReaderMenuAction::CONTROLS_OPTIONS, true);
        return;
      }
      commitSettings();
      if (auto controls = makeUniqueNoThrow<ControlsOptionsActivity>(renderer, mappedInput)) {
        if (beginGlobalSettingsEditCallback) beginGlobalSettingsEditCallback(beginGlobalSettingsEditContext);
        startActivityForResult(std::move(controls), [this](const ActivityResult&) {
          if (endGlobalSettingsEditCallback) endGlobalSettingsEditCallback(endGlobalSettingsEditContext);
          requestUpdate();
        });
      }
      return;
    case RowId::ResetBookReaderSettings:
      closeAndReturn(false, EpubReaderMenuAction::RESET_BOOK_READER_SETTINGS);
      return;
    case RowId::TextAa:
    case RowId::Focus:
    case RowId::GuideDots:
    case RowId::Hyphenation:
    case RowId::PublisherPages:
    case RowId::ExtraSpacing:
    case RowId::ForceIndents:
    case RowId::EmbeddedStyle:
      toggleSetting(row);
      return;
    case RowId::BookmarkToggle:
      isCurrentPageBookmarked = !isCurrentPageBookmarked;
      closeAndReturn(false, EpubReaderMenuAction::BOOKMARK_TOGGLE, !mappedInput.hasTouchHardware());
      return;
    case RowId::ToggleCompleted:
      isBookCompleted = !isBookCompleted;
      closeAndReturn(false, EpubReaderMenuAction::TOGGLE_COMPLETED);
      return;
    case RowId::TrackBookStats:
      closeAndReturn(false, EpubReaderMenuAction::TOGGLE_BOOK_STATS_TRACKING, true);
      return;
    case RowId::DeleteBookmarks:
      closeAndReturn(false, EpubReaderMenuAction::DELETE_BOOKMARKS);
      return;
    case RowId::DeleteCache:
      closeAndReturn(false, EpubReaderMenuAction::DELETE_CACHE);
      return;
    case RowId::DeleteStats:
      closeAndReturn(false, EpubReaderMenuAction::DELETE_STATS);
      return;
    default:
      break;
  }

  EpubReaderMenuAction action = EpubReaderMenuAction::GO_HOME;
  switch (row) {
    case RowId::ReadingStats:
      action = EpubReaderMenuAction::READING_STATS;
      break;
    case RowId::SyncProgress:
      action = EpubReaderMenuAction::SYNC;
      break;
    case RowId::NearbyPositionSync:
      action = EpubReaderMenuAction::NEARBY_POSITION_SYNC;
      break;
    case RowId::SendNearbyBook:
      action = EpubReaderMenuAction::SEND_NEARBY_BOOK;
      break;
    case RowId::ViewBookmarks:
      action = EpubReaderMenuAction::VIEW_BOOKMARKS;
      break;
    case RowId::Screenshot:
      action = EpubReaderMenuAction::SCREENSHOT;
      break;
    case RowId::DisplayQr:
      action = EpubReaderMenuAction::DISPLAY_QR;
      break;
    case RowId::Footnotes:
      action = EpubReaderMenuAction::FOOTNOTES;
      break;
    case RowId::Lookup:
      action = EpubReaderMenuAction::LOOKUP;
      break;
    case RowId::LookupHistory:
      action = EpubReaderMenuAction::LOOKUP_HISTORY;
      break;
    case RowId::SaveClipping:
      action = EpubReaderMenuAction::SAVE_CLIPPING;
      break;
    case RowId::ViewClippings:
      action = EpubReaderMenuAction::VIEW_CLIPPINGS;
      break;
    case RowId::ResetReadingPace:
      action = EpubReaderMenuAction::RESET_READING_PACE;
      break;
    default:
      return;
  }
  closeAndReturn(false, action, !mappedInput.hasTouchHardware());
}

void EpubReaderDrawerActivity::toggleSetting(const RowId row) {
  switch (row) {
    case RowId::TextAa:
      draft.textAntiAliasing = !draft.textAntiAliasing;
      break;
    case RowId::Focus:
      draft.focusReadingEnabled = !draft.focusReadingEnabled;
      break;
    case RowId::GuideDots:
      draft.guideReadingEnabled = !draft.guideReadingEnabled;
      break;
    case RowId::Hyphenation:
      draft.hyphenationEnabled = !draft.hyphenationEnabled;
      break;
    case RowId::PublisherPages:
      draft.publisherPageNumbers = !draft.publisherPageNumbers;
      break;
    case RowId::ExtraSpacing:
      draft.extraParagraphSpacing = !draft.extraParagraphSpacing;
      break;
    case RowId::ForceIndents:
      draft.forceParagraphIndents = !draft.forceParagraphIndents;
      break;
    case RowId::EmbeddedStyle:
      draft.embeddedStyle = !draft.embeddedStyle;
      break;
    default:
      return;
  }
  if (row == RowId::TextAa) {
    // Anti-aliasing changes pixels only. Rebuilding the EPUB section here
    // makes the in-drawer preview appear to zoom while the page reflows.
    markSettingChanged(ReaderSettingsChangeMask::Preview | ReaderSettingsChangeMask::NonLayout);
  } else {
    const bool previews = row == RowId::Focus || row == RowId::GuideDots;
    markSettingChanged(previews ? ReaderSettingsChangeMask::Preview | ReaderSettingsChangeMask::Relayout
                                : ReaderSettingsChangeMask::Relayout);
  }
  requestUpdate();
}

void EpubReaderDrawerActivity::showEnumOptions(const RowId row) {
  RenderLock lock;
  const bool fontSizeNeedsLoading = row == RowId::FontSize && draft.sdFontFamilyName[0] != '\0' &&
                                    ReaderUtils::shouldShowFontPreviewLoading(draft.sdFontFamilyName.data());
  if (row == RowId::DictionaryFontFamily || row == RowId::DictionaryFontSize || fontSizeNeedsLoading) {
    GUI.drawPopup(renderer, tr(STR_LOADING), true);
  }
  if (row == RowId::DictionaryFontFamily || row == RowId::DictionaryFontSize) sdFontSystem.refreshIfDirty();
  std::vector<std::string> labels;
  std::vector<uint8_t> raw;
  StrId title = StrId::STR_NONE_OPT;
  uint8_t currentRaw = 0;

  if (row == RowId::FontSize) {
    if (draft.sdFontFamilyName[0] != '\0') {
      sdFontSystem.refreshIfDirty();
      if (const auto* family = sdFontSystem.registry().findFamily(draft.sdFontFamilyName.data())) {
        raw = family->availableSizes();
      }
    }
    if (raw.empty()) {
      raw.assign(std::begin(BUILTIN_READER_FONT_SIZES), std::end(BUILTIN_READER_FONT_SIZES));
    }
    labels.reserve(raw.size());
    std::transform(raw.begin(), raw.end(), std::back_inserter(labels),
                   [](const uint8_t size) { return fontSizePointLabel(size); });
    const auto it = std::find(raw.begin(), raw.end(), draft.readerFontPointSize);
    const int current = it == raw.end() ? 0 : static_cast<int>(std::distance(raw.begin(), it));
    openEnumOptions(row, StrId::STR_FONT_SIZE, std::move(labels), std::move(raw), current);
    return;
  }

  switch (row) {
    case RowId::Orientation:
      title = StrId::STR_ORIENTATION;
      labels = {tr(STR_PORTRAIT), tr(STR_LANDSCAPE_CW), tr(STR_LANDSCAPE_CCW), tr(STR_ORIENTATION_INVERTED)};
      raw = {CrossPointSettings::PORTRAIT, CrossPointSettings::LANDSCAPE_CW, CrossPointSettings::LANDSCAPE_CCW,
             CrossPointSettings::INVERTED};
      currentRaw = draft.orientation;
      break;
    case RowId::Alignment:
      title = StrId::STR_PARA_ALIGNMENT;
      labels = {tr(STR_JUSTIFY), tr(STR_DIR_LEFT), tr(STR_CENTER), tr(STR_DIR_RIGHT), tr(STR_BOOK_S_STYLE)};
      raw = {0, 1, 2, 3, 4};
      currentRaw = draft.paragraphAlignment;
      break;
    case RowId::PublisherFonts:
      title = StrId::STR_PUBLISHER_FONTS;
      labels = {tr(STR_OFF), tr(STR_PUBLISHER_FONTS_ACCENTS), tr(STR_PUBLISHER_FONTS_ALL)};
      raw = {0, 1, 2};
      currentRaw = draft.publisherFonts;
      break;
    case RowId::Images:
      title = StrId::STR_IMAGES;
      labels = {tr(STR_IMAGES_DISPLAY), tr(STR_IMAGES_DISPLAY_BW), tr(STR_IMAGES_PLACEHOLDER), tr(STR_IMAGES_SUPPRESS)};
      raw = {ImageDisplayMode::Grayscale, ImageDisplayMode::BlackAndWhite, ImageDisplayMode::Placeholder,
             ImageDisplayMode::Hidden};
      currentRaw = ImageDisplayMode::selected(draft.imageRendering, draft.imageGrayscale);
      break;
    case RowId::RenderMode:
      title = StrId::STR_EPUB_RENDER_MODE;
      labels = {tr(STR_RENDER_MODE_CROSSINK_DEFAULT), tr(STR_RENDER_MODE_BALANCED), tr(STR_RENDER_MODE_LIGHT)};
      raw = {0, 1, 2};
      currentRaw = draft.epubRenderMode;
      break;
    case RowId::DictionaryFontFamily: {
      title = StrId::STR_FONT_FAMILY;
      labels.emplace_back(tr(STR_DICT_USE_GLOBAL));
      raw.emplace_back(0);
      const auto& families = sdFontSystem.registry().getFamilies();
      for (size_t i = 0; i < families.size(); ++i) {
        labels.push_back(families[i].name);
        raw.push_back(static_cast<uint8_t>(i + 1));
      }
      if (hasDictionaryFontOverride && dictionaryFontFamilyName[0] != '\0' &&
          sdFontSystem.registry().findSummary(dictionaryFontFamilyName) == nullptr) {
        labels.push_back(std::string(dictionaryFontFamilyName) + " (" + tr(STR_UNAVAILABLE) + ")");
        raw.push_back(0);
      }
      if (hasDictionaryFontOverride && dictionaryFontFamilyName[0] != '\0') {
        for (size_t i = 1; i < labels.size(); ++i) {
          if (labels[i] == dictionaryFontFamilyName) {
            currentRaw = raw[i];
            break;
          }
        }
      }
      break;
    }
    case RowId::DictionaryFontSize: {
      title = StrId::STR_DICTIONARY_FONT_SIZE;
      labels.emplace_back(tr(STR_DICT_USE_GLOBAL));
      raw.emplace_back(0);
      const char* familyName =
          dictionaryFontFamilyName[0] != '\0' ? dictionaryFontFamilyName : SETTINGS.sdFontFamilyName;
      if (familyName[0] != '\0') {
        if (const auto* family = sdFontSystem.registry().findFamily(familyName)) {
          const auto sizes = family->availableSizes();
          labels.reserve(sizes.size() + 1);
          raw.reserve(sizes.size() + 1);
          for (const uint8_t size : sizes) {
            labels.push_back(fontSizePointLabel(size));
            raw.push_back(size);
          }
        }
      }
      currentRaw = hasDictionaryFontOverride ? dictionaryFontPointSize : 0;
      break;
    }
    default:
      return;
  }
  int current = 0;
  if (row == RowId::DictionaryFontFamily && hasDictionaryFontOverride && dictionaryFontFamilyName[0] != '\0' &&
      sdFontSystem.registry().findSummary(dictionaryFontFamilyName) == nullptr) {
    current = static_cast<int>(labels.size()) - 1;
  } else {
    const auto rawIt = std::find(raw.begin(), raw.end(), currentRaw);
    if (rawIt != raw.end()) current = static_cast<int>(std::distance(raw.begin(), rawIt));
  }
  openEnumOptions(row, title, std::move(labels), std::move(raw), current);
}

void EpubReaderDrawerActivity::openEnumOptions(const RowId row, const StrId title, std::vector<std::string> labels,
                                               std::vector<uint8_t> values, const int selectedIndex) {
  if (labels.empty() || labels.size() != values.size()) return;
  enumOptionRow = row;
  enumOptionTitle = title;
  enumOptionLabels = std::move(labels);
  enumOptionValues = std::move(values);
  enumOptionSelectedIndex =
      static_cast<int16_t>(std::clamp(selectedIndex, 0, static_cast<int>(enumOptionLabels.size()) - 1));
  previewedEnumOptionIndex = -1;
  enumOptionReturnPane = state.pane;
  state.pane = ReaderDrawerPane::EnumOptions;
  state.selectedIndex = enumOptionSelectedIndex;
  state.paneTopIndex = state.selectedIndex;
  buttonFocusActive = true;
  requestUpdate();
}

void EpubReaderDrawerActivity::notifyDictionaryFontChanged() {
  if (dictionaryFontChangedCallback) {
    dictionaryFontChangedCallback(dictionaryFontChangedContext,
                                  hasDictionaryFontOverride ? dictionaryFontFamilyName : nullptr,
                                  dictionaryFontPointSize);
  }
}

void EpubReaderDrawerActivity::selectEnumOption(const int index) {
  if (index < 0 || index >= static_cast<int>(enumOptionValues.size())) return;
  const uint8_t value = enumOptionValues[static_cast<size_t>(index)];
  const bool previewsBeforeSelecting = enumOptionRow == RowId::FontSize || enumOptionRow == RowId::Alignment;
  if (previewsBeforeSelecting && previewedEnumOptionIndex != index) {
    previewedEnumOptionIndex = static_cast<int16_t>(index);
    if (enumOptionRow == RowId::FontSize) {
      fontPreviewLoading = ReaderUtils::shouldShowFontPreviewLoading(draft.sdFontFamilyName.data());
      draft.readerFontPointSize = value;
    } else {
      draft.paragraphAlignment = value;
    }
    markSettingChanged(ReaderSettingsChangeMask::Preview | ReaderSettingsChangeMask::Relayout);
    requestUpdate();
    return;
  }
  switch (enumOptionRow) {
    case RowId::FontSize:
      draft.readerFontPointSize = value;
      enumOptionSelectedIndex = static_cast<int16_t>(index);
      previewedEnumOptionIndex = -1;
      markSettingChanged(ReaderSettingsChangeMask::Preview | ReaderSettingsChangeMask::Relayout);
      break;
    case RowId::Orientation:
      draft.orientation = value;
      markSettingChanged(ReaderSettingsChangeMask::Orientation);
      break;
    case RowId::Alignment:
      draft.paragraphAlignment = value;
      enumOptionSelectedIndex = static_cast<int16_t>(index);
      previewedEnumOptionIndex = -1;
      markSettingChanged(ReaderSettingsChangeMask::Preview | ReaderSettingsChangeMask::Relayout);
      break;
    case RowId::PublisherFonts:
      draft.publisherFonts = value;
      markSettingChanged(ReaderSettingsChangeMask::Relayout);
      break;
    case RowId::Images:
      ImageDisplayMode::apply(value, draft.imageRendering, draft.imageGrayscale);
      markSettingChanged(ReaderSettingsChangeMask::NonLayout);
      break;
    case RowId::RenderMode:
      draft.epubRenderMode = value;
      markSettingChanged(ReaderSettingsChangeMask::Relayout);
      break;
    case RowId::DictionaryFontFamily:
      if (index == 0) {
        hasDictionaryFontOverride = false;
        std::strncpy(dictionaryFontFamilyName, SETTINGS.dictionarySdFontFamilyName,
                     sizeof(dictionaryFontFamilyName) - 1);
        dictionaryFontFamilyName[sizeof(dictionaryFontFamilyName) - 1] = '\0';
        dictionaryFontPointSize = SETTINGS.dictionaryFontPointSize;
      } else if (const auto* family =
                     sdFontSystem.registry().findFamily(enumOptionLabels[static_cast<size_t>(index)].c_str())) {
        hasDictionaryFontOverride = true;
        std::strncpy(dictionaryFontFamilyName, family->name.c_str(), sizeof(dictionaryFontFamilyName) - 1);
        dictionaryFontFamilyName[sizeof(dictionaryFontFamilyName) - 1] = '\0';
      }
      notifyDictionaryFontChanged();
      break;
    case RowId::DictionaryFontSize:
      if (!hasDictionaryFontOverride) {
        if (index == 0) {
          dictionaryFontPointSize = SETTINGS.dictionaryFontPointSize;
        } else if (dictionaryFontFamilyName[0] != '\0') {
          hasDictionaryFontOverride = true;
          dictionaryFontPointSize = value;
        } else if (SETTINGS.sdFontFamilyName[0] != '\0') {
          std::strncpy(dictionaryFontFamilyName, SETTINGS.sdFontFamilyName, sizeof(dictionaryFontFamilyName) - 1);
          dictionaryFontFamilyName[sizeof(dictionaryFontFamilyName) - 1] = '\0';
          hasDictionaryFontOverride = true;
          dictionaryFontPointSize = value;
        }
      } else {
        dictionaryFontPointSize = value;
      }
      notifyDictionaryFontChanged();
      break;
#if CROSSINK_SCALABLE_FONTS
    case RowId::TtfHinting:
    case RowId::TtfRaster:
    case RowId::TtfInterpreter:
    case RowId::TtfWeight:
    case RowId::TtfSlant: {
      using TtfRow = TtfRenderOptionsActivity::Row;
      const TtfRow optionRow = enumOptionRow == RowId::TtfHinting       ? TtfRow::Hinting
                               : enumOptionRow == RowId::TtfRaster      ? TtfRow::Raster
                               : enumOptionRow == RowId::TtfInterpreter ? TtfRow::Interpreter
                               : enumOptionRow == RowId::TtfWeight      ? TtfRow::Weight
                                                                        : TtfRow::Slant;
      TtfRenderOptionsActivity::setOption(ttfRenderProfile, optionRow, value);
      if (enumOptionRow == RowId::TtfHinting) rebuildTtfRenderingRows();
      saveTtfRenderingProfile();
      break;
    }
#endif
    default:
      return;
  }
  state.pane = enumOptionReturnPane;
  state.paneTopIndex = 0;
  state.selectedIndex = 0;
  requestUpdate();
}

void EpubReaderDrawerActivity::resetKeypadEntry() {
  entryLen = 0;
  entryText[0] = 0;
  // Discard anything typed on a previous visit to this pane along with the buffer,
  // so the readout goes back to the book's actual position rather than a value the
  // user backspaced away from or left without confirming.
  percent = percentSeed;
  stablePage = stablePageSeed;
  requestUpdate();
}

void EpubReaderDrawerActivity::enterPercentKeypad() {
  percentBeforeKeypad = percent;
  if (percent % 100 == 0) {
    std::snprintf(entryText, sizeof(entryText), "%d", percent / 100);
  } else if (percent % 10 == 0) {
    std::snprintf(entryText, sizeof(entryText), "%d.%d", percent / 100, (percent % 100) / 10);
  } else {
    std::snprintf(entryText, sizeof(entryText), "%d.%02d", percent / 100, percent % 100);
  }
  entryLen = static_cast<uint8_t>(std::strlen(entryText));
  keypadRow = 0;
  keypadCol = 0;
  keypadBackspaceFocused = false;
  percentKeypadActive = true;
  requestUpdate();
}

void EpubReaderDrawerActivity::exitPercentKeypad() {
  percent = percentBeforeKeypad;
  percentKeypadActive = false;
  entryLen = 0;
  entryText[0] = 0;
  requestUpdate();
}

void EpubReaderDrawerActivity::adjustPercentSlider(const int steps) {
  const int raw = percent + steps * 100;
  // Keep 100% as a reachable endpoint while preserving the old wrap behavior.
  percent = raw > 0 && raw % 10000 == 0 ? 10000 : ((raw % 10000) + 10000) % 10000;
  requestUpdate();
}

void EpubReaderDrawerActivity::moveKeypadFocus(const int rowDelta, const int colDelta) {
  if (keypadBackspaceFocused) {
    if (rowDelta > 0 || colDelta != 0) keypadBackspaceFocused = false;
  } else if (rowDelta < 0 && keypadRow == 0 && entryLen > 0) {
    keypadBackspaceFocused = true;
  } else {
    keypadRow = static_cast<uint8_t>((keypadRow + rowDelta + 4) % 4);
    keypadCol = static_cast<uint8_t>((keypadCol + colDelta + 3) % 3);
    if (state.pane == ReaderDrawerPane::StablePage && keypadRow == 3 && keypadCol == 1) {
      if (colDelta != 0)
        keypadCol = static_cast<uint8_t>((keypadCol + colDelta + 3) % 3);
      else
        keypadRow = static_cast<uint8_t>((keypadRow + rowDelta + 4) % 4);
    }
  }
  requestUpdate();
}

void EpubReaderDrawerActivity::activateKeypadFocus() {
  if (keypadBackspaceFocused) {
    backspaceKeypadEntry();
    return;
  }
  const int index = keypadRow * 3 + keypadCol;
  if (index == 11) {
    if (percentKeypadActive && entryLen == 0) return;
    if (state.pane == ReaderDrawerPane::Percent)
      completePercentSelection();
    else
      completeStablePageSelection();
  } else if (index == 10) {
    appendKeypadDecimalPoint();
  } else {
    appendKeypadDigit(static_cast<char>('0' + (index == 9 ? 0 : index + 1)));
  }
}

void EpubReaderDrawerActivity::appendKeypadDigit(const char digit) {
  if (entryLen >= sizeof(entryText) - 1) return;
  // Reject a digit that would push the typed value out of range, rather than let the
  // readout show a number Confirm would silently clamp to something else.
  char candidate[sizeof(entryText)];
  std::memcpy(candidate, entryText, entryLen);
  candidate[entryLen] = digit;
  candidate[entryLen + 1] = 0;
  if (state.pane == ReaderDrawerPane::Percent) {
    // Cap at two decimal places: the readout and the stored value both round to
    // hundredths, so a third digit would type something the confirmed value silently
    // rounds away.
    const char* dot = static_cast<const char*>(std::memchr(entryText, '.', entryLen));
    if (dot != nullptr && (entryText + entryLen) - (dot + 1) >= 2) return;
    if (std::strtof(candidate, nullptr) > 100.0f) return;
  } else if (state.pane == ReaderDrawerPane::StablePage) {
    // Page 0 does not exist; clampStablePage() would silently round it up to 1.
    const unsigned long candidateValue = std::strtoul(candidate, nullptr, 10);
    if (candidateValue == 0 || candidateValue > stablePageCount) return;
  }
  entryText[entryLen++] = digit;
  entryText[entryLen] = 0;
  syncKeypadValue();
  requestUpdate();
}

void EpubReaderDrawerActivity::appendKeypadDecimalPoint() {
  if (state.pane != ReaderDrawerPane::Percent) return;
  // Require a leading digit ("0." rather than bare "."): otherwise the readout
  // would show "." while OK silently confirms 0%.
  if (entryLen == 0) return;
  for (uint8_t i = 0; i < entryLen; ++i) {
    if (entryText[i] == '.') return;  // one decimal point at most
  }
  if (entryLen >= sizeof(entryText) - 1) return;
  entryText[entryLen++] = '.';
  entryText[entryLen] = 0;
  syncKeypadValue();
  requestUpdate();
}

void EpubReaderDrawerActivity::backspaceKeypadEntry() {
  if (entryLen == 0) return;
  entryText[--entryLen] = 0;
  if (entryLen == 0) {
    keypadBackspaceFocused = false;
    // Nothing left to parse: go back to the book's actual position rather than
    // leaving the last successfully-parsed (now abandoned) value in place.
    percent = percentKeypadActive ? percentBeforeKeypad : percentSeed;
    stablePage = stablePageSeed;
  } else {
    syncKeypadValue();
  }
  requestUpdate();
}

void EpubReaderDrawerActivity::syncKeypadValue() {
  if (entryLen == 0) return;
  if (state.pane == ReaderDrawerPane::StablePage) {
    stablePage = clampStablePage(static_cast<uint32_t>(std::strtoul(entryText, nullptr, 10)), stablePageCount);
  } else if (state.pane == ReaderDrawerPane::Percent) {
    const float parsed = std::strtof(entryText, nullptr);
    percent = static_cast<int>(std::lround(std::clamp(parsed, 0.0f, 100.0f) * 100.0f));
  }
}

void EpubReaderDrawerActivity::onKeypadKeyEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EpubReaderDrawerActivity*>(user);
  self->buttonFocusActive = false;
  const int16_t value = event.value;
  if (value == KEYPAD_OK) {
    if (self->state.pane == ReaderDrawerPane::Percent) {
      self->completePercentSelection();
    } else if (self->state.pane == ReaderDrawerPane::StablePage) {
      self->completeStablePageSelection();
    }
  } else if (value == KEYPAD_DOT) {
    self->appendKeypadDecimalPoint();
  } else {
    self->appendKeypadDigit(static_cast<char>('0' + value));
  }
}

void EpubReaderDrawerActivity::onKeypadBackspaceEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<EpubReaderDrawerActivity*>(user);
  self->buttonFocusActive = false;
  self->backspaceKeypadEntry();
}

void EpubReaderDrawerActivity::completePercentSelection() {
  const bool changed = didChangeSettings;
  commitSettings();
  MenuResult menu{static_cast<int>(EpubReaderMenuAction::GO_TO_PERCENT), draft.orientation, changed};
  menu.drawerState = state;
  menu.changeMask = changeMask;
  // Centipercent (0-10000); the receiving jumpToPercent() divides back to 0.0-100.0.
  menu.drawerValue = static_cast<int16_t>(percent);
  setResult(std::move(menu));
  finish();
}

void EpubReaderDrawerActivity::completeStablePageSelection() {
  const bool changed = didChangeSettings;
  commitSettings();
  MenuResult menu{static_cast<int>(EpubReaderMenuAction::GO_TO_STABLE_PAGE), draft.orientation, changed};
  menu.drawerState = state;
  menu.changeMask = changeMask;
  menu.drawerPage = stablePage;
  setResult(std::move(menu));
  finish();
}

void EpubReaderDrawerActivity::completeAutoPageTurnSelection() {
  const bool changed = didChangeSettings;
  commitSettings();
  MenuResult menu{static_cast<int>(EpubReaderMenuAction::AUTO_PAGE_TURN), draft.orientation, changed};
  menu.drawerState = state;
  menu.changeMask = changeMask;
  menu.drawerValue = static_cast<int16_t>(autoPageTurnIntervalSeconds);
  setResult(std::move(menu));
  finish();
}

void EpubReaderDrawerActivity::adjustActiveSlider(const int delta) {
  if (state.pane == ReaderDrawerPane::CharacterSpacing) {
    draft.characterSpacing =
        std::clamp<int>(draft.characterSpacing + delta, 0, CrossPointSettings::MAX_CHARACTER_SPACING);
  } else if (state.pane == ReaderDrawerPane::Spacing) {
    draft.lineHeightPercent = CrossPointSettings::clampedLineHeightPercent(static_cast<uint8_t>(
        std::clamp<int>(draft.lineHeightPercent + delta, CrossPointSettings::MIN_LINE_HEIGHT_PERCENT,
                        CrossPointSettings::MAX_LINE_HEIGHT_PERCENT)));
  } else if (state.pane == ReaderDrawerPane::Margins) {
    draft.screenMarginVertical =
        std::clamp<int>(draft.screenMarginVertical + delta, CrossPointSettings::MIN_SCREEN_MARGIN,
                        CrossPointSettings::MAX_SCREEN_MARGIN);
  } else if (state.pane == ReaderDrawerPane::AutoPageTurn) {
    autoPageTurnIntervalSeconds = static_cast<uint16_t>(std::clamp<int>(
        autoPageTurnIntervalSeconds + delta, READER_AUTO_PAGE_TURN_MIN_SECONDS, READER_AUTO_PAGE_TURN_MAX_SECONDS));
  }
}

void EpubReaderDrawerActivity::adjustButtonSlider(const int delta) {
  bool changed = false;
  if (state.pane == ReaderDrawerPane::CharacterSpacing) {
    const uint8_t previous = draft.characterSpacing;
    adjustActiveSlider(delta);
    changed = previous != draft.characterSpacing;
  } else if (state.pane == ReaderDrawerPane::Spacing) {
    if (buttonSliderState.focus == 0) {
      const uint8_t value = CrossPointSettings::clampedLineHeightPercent(static_cast<uint8_t>(
          std::clamp<int>(draft.lineHeightPercent + delta, CrossPointSettings::MIN_LINE_HEIGHT_PERCENT,
                          CrossPointSettings::MAX_LINE_HEIGHT_PERCENT)));
      changed = value != draft.lineHeightPercent;
      draft.lineHeightPercent = value;
    } else {
      const uint8_t value = WordSpacing::fromLevel(WordSpacing::level(draft.wordSpacing) + delta);
      changed = value != draft.wordSpacing;
      draft.wordSpacing = value;
    }
  } else if (state.pane == ReaderDrawerPane::Margins) {
    uint8_t& target = buttonSliderState.focus == 0 ? draft.screenMarginVertical : draft.screenMarginHorizontal;
    const uint8_t value = static_cast<uint8_t>(
        std::clamp<int>(target + delta, CrossPointSettings::MIN_SCREEN_MARGIN, CrossPointSettings::MAX_SCREEN_MARGIN));
    changed = value != target;
    target = value;
  } else if (state.pane == ReaderDrawerPane::AutoPageTurn) {
    autoPageTurnIntervalSeconds = static_cast<uint16_t>(std::clamp<int>(
        autoPageTurnIntervalSeconds + delta, READER_AUTO_PAGE_TURN_MIN_SECONDS, READER_AUTO_PAGE_TURN_MAX_SECONDS));
  }
  if (changed) markSettingChanged(ReaderSettingsChangeMask::Preview | ReaderSettingsChangeMask::Relayout);
}

void EpubReaderDrawerActivity::moveSelection(const bool forward, const bool page) {
  int count = 0;
  if (state.pane == ReaderDrawerPane::EnumOptions) {
    count = static_cast<int>(enumOptionLabels.size());
  } else if (state.pane == ReaderDrawerPane::Dictionary) {
    count = static_cast<int>(dictionaryLabels.size());
  } else if (state.pane == ReaderDrawerPane::FontFamily) {
    count = static_cast<int>(fontLabels.size());
  } else {
    count = static_cast<int>(activeRows().size());
  }
  if (count <= 0) return;
  if (!mappedInput.hasTouchHardware() && !buttonFocusActive &&
      (state.pane == ReaderDrawerPane::Root || state.pane == ReaderDrawerPane::FontFamily)) {
    state.selectedIndex = forward ? 0 : static_cast<int16_t>(count - 1);
  } else {
    state.selectedIndex = page ? (forward ? ButtonNavigator::nextPageIndex(state.selectedIndex, count, visibleRows)
                                          : ButtonNavigator::previousPageIndex(state.selectedIndex, count, visibleRows))
                               : (forward ? ButtonNavigator::nextIndex(state.selectedIndex, count)
                                          : ButtonNavigator::previousIndex(state.selectedIndex, count));
  }
  buttonFocusActive = true;
  const int top = followListSelection(state.selectedIndex, activeTopIndex(), visibleRows, count);
  if (state.pane == ReaderDrawerPane::Root) {
    state.rootTopIndex[static_cast<size_t>(state.tab)] = static_cast<int16_t>(top);
  } else {
    state.paneTopIndex = static_cast<int16_t>(top);
  }
  requestUpdate();
}

void EpubReaderDrawerActivity::scrollBy(const int delta) {
  buttonFocusActive = false;
  int count = 0;
  if (state.pane == ReaderDrawerPane::EnumOptions)
    count = enumOptionLabels.size();
  else if (state.pane == ReaderDrawerPane::Dictionary)
    count = dictionaryLabels.size();
  else if (state.pane == ReaderDrawerPane::FontFamily)
    count = fontLabels.size();
  else
    count = activeRows().size();
  const int currentTop =
      state.pane == ReaderDrawerPane::Root ? state.rootTopIndex[static_cast<size_t>(state.tab)] : state.paneTopIndex;
  const int next = scrollListBy(currentTop, delta, visibleRows, count);
  if (state.pane == ReaderDrawerPane::Root)
    state.rootTopIndex[static_cast<size_t>(state.tab)] = next;
  else
    state.paneTopIndex = next;
  requestUpdate();
}

bool EpubReaderDrawerActivity::saveBookDictionary(const std::string& path) {
  if (!epub) return false;
  HalFile file;
  const std::string target = epub->getCachePath() + "/dictionary.bin";
  if (!Storage.openFileForWrite("ERDM", target, file)) {
    LOG_ERR("ERDM", "Could not save per-book dictionary");
    return false;
  }
  const bool ok =
      path.empty() || file.write(reinterpret_cast<const uint8_t*>(path.c_str()), path.size()) == path.size();
  file.close();
  if (!ok) LOG_ERR("ERDM", "Short write saving per-book dictionary");
  return ok;
}

void EpubReaderDrawerActivity::renderPreviewContents(const ReaderSettingsDraft& previewSettings,
                                                     const int previewFontId) {
  const fui::Rect preview = previewBounds();
  renderer.fillRect(preview.x, preview.y, preview.width, preview.height, ReaderUtils::readerDarkModeEnabled());
  renderPreviewText(previewSettings, previewFontId);
  if (CROSSINK_APP_READER_SAMPLE_PREVIEW) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int labelTextHeight = renderer.getTextHeight(UI_10_FONT_ID);
    const int noteHeight = previewSettings.textAntiAliasing ? labelTextHeight + 2 : 0;
    const char* name = previewSettings.sdFontFamilyName[0]
                           ? previewSettings.sdFontFamilyName.data()
                           : (previewSettings.fontFamily == 0 ? tr(STR_LEXEND_DECA) : tr(STR_BITTER));
    char label[128];
    std::snprintf(label, sizeof(label), "%s \"%s\", %upt", tr(STR_PREVIEW), name, previewSettings.readerFontPointSize);
    const int labelY = preview.bottom() - metrics.previewPadding - labelTextHeight - noteHeight;
    const int separatorY = labelY - 4;
    renderer.drawLine(preview.x, separatorY, preview.right() - 1, separatorY, ReaderUtils::readerForegroundBlack());
    renderer.beginTextClip(preview.x, preview.y, preview.width, preview.height);
    renderer.drawText(UI_10_FONT_ID, preview.x + metrics.previewPadding, labelY, label,
                      ReaderUtils::readerForegroundBlack());
    if (previewSettings.textAntiAliasing) {
      std::snprintf(label, sizeof(label), "%s: %s", tr(STR_TEXT_AA), tr(STR_PREVIEW_UNAVAILABLE));
      renderer.drawText(SMALL_FONT_ID, preview.x + metrics.previewPadding, labelY + labelTextHeight + 6, label,
                        ReaderUtils::readerForegroundBlack());
    }
    renderer.endTextClip();
    if (samplePreviewBesideControls()) {
      renderer.drawLine(preview.x, preview.y, preview.x, preview.bottom() - 1, ReaderUtils::readerForegroundBlack());
    } else {
      renderer.drawLine(preview.x, preview.bottom() - 1, preview.right() - 1, preview.bottom() - 1,
                        ReaderUtils::readerForegroundBlack());
    }
  }
}

void EpubReaderDrawerActivity::renderPreviewText(const ReaderSettingsDraft& previewSettings, const int previewFontId) {
  if (CROSSINK_APP_READER_SAMPLE_PREVIEW) {
    renderSamplePreviewText(previewSettings, previewFontId);
    return;
  }
  const fui::Rect preview = previewBounds();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  const int clipTop = std::max<int>(preview.y, safe.y);
  const int clipBottom = std::min<int>(preview.bottom(), safe.y + safe.height);
  const int clipHeight = std::max(0, clipBottom - clipTop);
  const bool sourceLayout = !previewFontMetricsChanged && previewSettings.fontFamily == sourceSettings.fontFamily &&
                            previewSettings.readerFontPointSize == sourceSettings.readerFontPointSize &&
                            previewSettings.sdFontFamilyName == sourceSettings.sdFontFamilyName &&
                            previewSettings.lineHeightPercent == sourceSettings.lineHeightPercent &&
                            previewSettings.wordSpacing == sourceSettings.wordSpacing &&
                            previewSettings.characterSpacing == sourceSettings.characterSpacing &&
                            previewSettings.screenMarginVertical == sourceSettings.screenMarginVertical &&
                            previewSettings.screenMarginHorizontal == sourceSettings.screenMarginHorizontal &&
                            previewSettings.paragraphAlignment == sourceSettings.paragraphAlignment &&
                            previewSettings.focusReadingEnabled == sourceSettings.focusReadingEnabled &&
                            previewSettings.guideReadingEnabled == sourceSettings.guideReadingEnabled;
  if (sourceLayout && EpubReaderPreviewModel::RETAINS_SOURCE_BLOCKS) {
    renderer.beginTextClip(safe.x, clipTop, safe.width, clipHeight);
    previewModel->renderSource(renderer, previewFontId, ReaderUtils::readerForegroundBlack());
    renderer.endTextClip();
    return;
  }
  int orientedTop, orientedRight, orientedBottom, orientedLeft;
  renderer.getOrientedViewableTRBL(&orientedTop, &orientedRight, &orientedBottom, &orientedLeft);
  (void)orientedBottom;
  const int clockReservation = ReaderUtils::getTopStatusBarReservedHeight(renderer);
  const int previewYOffset =
      orientedTop + std::max(static_cast<int>(previewSettings.screenMarginVertical),
                             clockReservation > 0 ? clockReservation + ReaderUtils::TOP_STATUS_BAR_TEXT_PADDING : 0);
  const int previewXOffset = orientedLeft + previewSettings.screenMarginHorizontal;
  const int previewWidth = std::max(1, renderer.getScreenWidth() - orientedLeft - orientedRight -
                                           static_cast<int>(previewSettings.screenMarginHorizontal) * 2);
  renderer.beginTextClip(safe.x, clipTop, safe.width, clipHeight);
  previewModel->renderText(
      renderer, previewFontId, previewXOffset, previewYOffset, previewWidth, previewSettings.lineHeightPercent,
      previewSettings.wordSpacing, previewSettings.paragraphAlignment, previewSettings.focusReadingEnabled,
      previewSettings.guideReadingEnabled, ReaderUtils::readerForegroundBlack(), std::numeric_limits<int>::max(),
      CrossPointSettings::characterSpacingLevel(previewSettings.characterSpacing));
  renderer.endTextClip();
}

void EpubReaderDrawerActivity::renderSamplePreviewText(const ReaderSettingsDraft& settings, const int fontId) {
  if (!previewModel || !previewModel->valid()) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const fui::Rect area = previewBounds();
  const int labelTextHeight = renderer.getTextHeight(UI_10_FONT_ID);
  const int labelHeight =
      labelTextHeight + (settings.textAntiAliasing ? labelTextHeight + 2 : 0) + metrics.previewPadding + 8;
  const int textHeight = std::max(0, area.height - labelHeight - metrics.previewPadding);
  // The sample is a short page: show top AND bottom margins proportionally
  // to its height, while horizontal margins and font sizes remain actual pixels.
  const int marginY =
      settings.screenMarginVertical * textHeight / std::max(1, static_cast<int>(renderer.getScreenHeight()));
  const int marginX = settings.screenMarginHorizontal;
  const int top = area.y + metrics.previewPadding + marginY;
  const int width = std::max(1, area.width - marginX * 2);
  renderer.beginTextClip(area.x + marginX, top, width, std::max(0, textHeight - marginY * 2));
  previewModel->renderText(renderer, fontId, area.x + marginX, top, width, settings.lineHeightPercent,
                           settings.wordSpacing, settings.paragraphAlignment, settings.focusReadingEnabled,
                           settings.guideReadingEnabled, ReaderUtils::readerForegroundBlack(),
                           top + std::max(0, textHeight - marginY * 2),
                           CrossPointSettings::characterSpacingLevel(settings.characterSpacing));
  renderer.endTextClip();
}

void EpubReaderDrawerActivity::renderPreviewUnavailable() {
  const fui::Rect preview = previewBounds();
  renderer.fillRect(preview.x, preview.y, preview.width, preview.height, ReaderUtils::readerDarkModeEnabled());
  const char* text = tr(STR_PREVIEW_UNAVAILABLE);
  const int x = preview.x + std::max(0, (preview.width - renderer.getTextWidth(UI_12_FONT_ID, text)) / 2);
  renderer.drawText(UI_12_FONT_ID, x, preview.y + preview.height / 2, text, ReaderUtils::readerForegroundBlack());
}

bool EpubReaderDrawerActivity::renderPreview(int& previewFontId,
                                             std::optional<FontCacheManager::PrewarmScope>& prewarmScope) {
  previewFontId = -1;
#if CROSSINK_APP_READER_SAMPLE_PREVIEW
  if (!showsSamplePreview()) return false;
#endif
  if (!previewDirty) return false;
  previewDirty = false;
  const auto releasePreviewIfBelowReserve = [this, &previewFontId] {
    if (!ownedPreviewModel) return false;
    const auto heap = MemoryBudget::snapshot();
    if (MemoryBudget::hasHeap(heap, MemoryBudget::EPUB_TEXT_LAYOUT_MIN_FREE,
                              MemoryBudget::EPUB_TEXT_LAYOUT_MIN_MAX_ALLOC))
      return false;
    LOG_ERR("ERDM", "Button preview exhausted EPUB layout reserve: free=%u maxAlloc=%u", heap.freeHeap,
            heap.maxAllocHeap);
    restoreReaderDraftFont(draft, lastGoodPreviewSettings);
    state.pendingFontIndex = -1;
    previewUnavailable = true;
    previewFontId = -1;
    renderPreviewUnavailable();
    return true;
  };
  if (!previewModel || !previewModel->valid()) {
    if (previewUnavailable) {
      renderPreviewUnavailable();
      return false;
    }
    if (!previewFontMetricsChanged) return false;
    const fui::Rect preview = previewBounds();
    renderer.fillRect(preview.x, preview.y, preview.width, preview.height, ReaderUtils::readerDarkModeEnabled());
    return true;
  }
  if (CROSSINK_APP_READER_SAMPLE_PREVIEW && releasePreviewIfBelowReserve()) return false;
  if (fontPreviewLoading) {
    GUI.drawPopup(renderer, tr(STR_LOADING), true);
    fontPreviewLoading = false;
  }
  const ReaderSettingsDraft previewSettings = draft;
  const ReaderSettingsDraft liveSettings = captureSettings();
  applySettings(previewSettings);
  sdFontSystem.ensureLoaded(renderer);
  previewFontId = SETTINGS.getReaderFontId();
  const bool fontLoaded =
      previewSettings.sdFontFamilyName[0] == '\0' ||
      sdFontSystem.resolveFontId(previewSettings.sdFontFamilyName.data(), previewSettings.readerFontPointSize) != 0;
  applySettings(liveSettings);
  if (!fontLoaded && ownedPreviewModel) {
    LOG_ERR("ERDM", "Could not load selected SD font for button preview");
    restoreReaderDraftFont(draft, lastGoodPreviewSettings);
    state.pendingFontIndex = -1;
    previewUnavailable = true;
    previewFontId = -1;
    renderPreviewUnavailable();
    return false;
  }
  if (auto* fontCacheManager = renderer.getFontCacheManager()) {
    prewarmScope.emplace(*fontCacheManager, FontCacheManager::PreparationPolicy::Normal);
    renderPreviewContents(previewSettings,
                          previewFontId);  // Scan the page text before loading the selected font's glyphs.
    if (!prewarmScope->endScanAndPrewarm() && ownedPreviewModel) {
      LOG_ERR("ERDM", "Could not prewarm selected font for button preview");
      restoreReaderDraftFont(draft, lastGoodPreviewSettings);
      state.pendingFontIndex = -1;
      previewUnavailable = true;
      previewFontId = -1;
      renderPreviewUnavailable();
      return false;
    }
    if (releasePreviewIfBelowReserve()) return false;
  }
  renderPreviewContents(previewSettings, previewFontId);
  if (releasePreviewIfBelowReserve()) return false;
  lastGoodPreviewSettings = draft;
  previewUnavailable = false;
  return true;
}

void EpubReaderDrawerActivity::renderPreviewWithAntiAliasing(const int previewFontId) {
  if (!previewModel || !previewModel->valid()) return;

  const ReaderSettingsDraft previewSettings = draft;
  // The BW refresh has already drawn the reader preview and drawer. Re-render
  // only preview text into the grayscale planes so the drawer stays crisp.
  ReaderUtils::renderAntiAliased(
      renderer, [this, &previewSettings, previewFontId] { renderPreviewText(previewSettings, previewFontId); });
}

void EpubReaderDrawerActivity::loop() {
#if CROSSINK_SCALABLE_FONTS
  if (ttfResetPopup.handleInput(mappedInput, [this] { requestUpdate(); })) {
    if (!ttfResetPopup.isActive()) {
      // The centered popup can cover reader pixels above the drawer. Restore
      // the book backdrop as well as any draft preview before repainting.
      activityManager.restoreBackdropBehindCurrentOverlay();
      previewDirty = true;
    }
    return;
  }
#endif
  if (mappedInput.wasBottomEdgeUpSwipe()) {
    closeAndReturn(false, EpubReaderMenuAction::GO_HOME, false);
    return;
  }
  if (DrawerHandle::wasDismissSwipe(mappedInput, drawerHandleRect,
                                    mappedInput.hasTouchHardware() ? fui::SheetEdge::Bottom : fui::SheetEdge::Top)) {
    closeAndReturn(true);
    return;
  }
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    scrollBy(swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows);
    return;
  }
  fui::InputSnapshot snap{};
  if (uiReady) {
    snap = touchSnapshotFrom(mappedInput);
    // List rows (and the Percent/StablePage keypad, which is buttons only, not a
    // slider) activate on release. Do not route their touch-down edge, otherwise a
    // swipe briefly paints the row where the finger landed as pressed even though it
    // never activates that row.
    if (!readerDrawerStepChangesSettings(state.pane)) {
      snap.touchPressed = false;
    }
    if (snap.touchPressed || snap.touchHeld || snap.touchReleased) {
      sliderTapPending = snap.touchReleased && snap.touchX >= 0;
      const auto event = app.route(snap);
      sliderTapPending = false;
      if (event.dragPermille >= 0) {
        if (snap.touchHeld) draggingSlider = true;
        if (app.invalidated()) requestUpdate();
        if (snap.touchReleased) {
          draggingSlider = false;
          previewDirty = readerDrawerSliderPreviewsText(state.pane);
          requestUpdate();
        }
        return;
      }
      // InputDrag owns a slider after touch-down. A release just outside the
      // track has no routed event, but it still needs to finish the preview.
      if (draggingSlider && !snap.touchHeld) {
        draggingSlider = false;
        previewDirty = readerDrawerSliderPreviewsText(state.pane);
        requestUpdate();
        return;
      }
      if (event) {
        if (app.invalidated()) requestUpdate();
        return;
      }
    }
  }
  // Holding Confirm enters the Percent keypad (or deletes a digit there).
  // Swallow that hold's release so it cannot also jump to a location.
  if (!mappedInput.hasTouchHardware() && state.pane == ReaderDrawerPane::Percent && percentConfirmLongPressFired) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) percentConfirmLongPressFired = false;
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (!mappedInput.hasTouchHardware() && state.pane == ReaderDrawerPane::Root && buttonFocusActive) {
      buttonFocusActive = false;
      requestUpdate();
      return;
    }
    buttonFocusActive = true;
    if (!mappedInput.hasTouchHardware() && state.pane == ReaderDrawerPane::Percent && percentKeypadActive) {
      exitPercentKeypad();
      return;
    }
    if (!mappedInput.hasTouchHardware() && state.pane == ReaderDrawerPane::AutoPageTurn) {
      closePane();
      return;
    }
    if (!mappedInput.hasTouchHardware() && readerDrawerStepChangesSettings(state.pane)) {
      if (readerButtonSliderInput(buttonSliderState, ReaderButtonSliderInput::Back) ==
          ReaderButtonSliderAction::LeavePane)
        closePane();
      else
        requestUpdate();
      return;
    }
    closePane();
    return;
  }
  if (!mappedInput.hasTouchHardware() && state.pane == ReaderDrawerPane::Percent) {
    // getHeldTime() can include another held button in a chord. Confirm's own
    // long press only counts while no directional button is held.
    const bool directionHeld = mappedInput.isPressed(MappedInputManager::Button::Left) ||
                               mappedInput.isPressed(MappedInputManager::Button::Right) ||
                               mappedInput.isPressed(MappedInputManager::Button::Up) ||
                               mappedInput.isPressed(MappedInputManager::Button::Down);
    if (!directionHeld && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
        mappedInput.getHeldTime() >= PERCENT_KEYPAD_LONG_PRESS_MS) {
      percentConfirmLongPressFired = true;
      if (percentKeypadActive)
        backspaceKeypadEntry();
      else
        enterPercentKeypad();
      return;
    }
    if (percentKeypadActive) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        activateKeypadFocus();
        return;
      }
      buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { moveKeypadFocus(0, -1); });
      buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { moveKeypadFocus(0, 1); });
      buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { moveKeypadFocus(-1, 0); });
      buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { moveKeypadFocus(1, 0); });
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      completePercentSelection();
      return;
    }
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustPercentSlider(-1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustPercentSlider(1); });
    buttonNavigator.onPressAndContinuous({mappedInput.sliderIncreaseButton()}, [this] { adjustPercentSlider(10); });
    buttonNavigator.onPressAndContinuous({mappedInput.sliderDecreaseButton()}, [this] { adjustPercentSlider(-10); });
    return;
  }
  if (!mappedInput.hasTouchHardware() && state.pane == ReaderDrawerPane::AutoPageTurn) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      completeAutoPageTurnSelection();
      return;
    }
    const auto adjust = [this](const int delta) {
      adjustButtonSlider(delta);
      requestUpdate();
    };
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [&] { adjust(-1); });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [&] { adjust(1); });
    buttonNavigator.onPressAndContinuous({mappedInput.sliderIncreaseButton()}, [&] { adjust(5); });
    buttonNavigator.onPressAndContinuous({mappedInput.sliderDecreaseButton()}, [&] { adjust(-5); });
    return;
  }
  if (!mappedInput.hasTouchHardware() && readerDrawerStepChangesSettings(state.pane)) {
    const auto onDirectionRelease = [this](const ReaderButtonSliderInput input, const int delta) {
      const auto action = readerButtonSliderInput(buttonSliderState, input);
      if (action == ReaderButtonSliderAction::Increase || action == ReaderButtonSliderAction::Decrease)
        adjustButtonSlider(delta);
      requestUpdate();
    };
    const auto handleDirection = [&](const MappedInputManager::Button button, const ReaderButtonSliderInput input,
                                     const int delta) {
      buttonNavigator.onRelease({button, button}, [&] { onDirectionRelease(input, delta); });
      if (!buttonSliderState.editing) return;
      // Held buttons update the draft without refreshing the e-ink panel on
      // every repeat; release draws the resulting preview once.
      buttonNavigator.onContinuous({button, button}, [this, input, delta] {
        const auto action = readerButtonSliderInput(buttonSliderState, input);
        if (action == ReaderButtonSliderAction::Increase || action == ReaderButtonSliderAction::Decrease)
          adjustButtonSlider(delta);
      });
      if (mappedInput.wasReleased(button)) requestUpdate();
    };
    handleDirection(MappedInputManager::Button::Left, ReaderButtonSliderInput::Previous, -1);
    handleDirection(MappedInputManager::Button::Right, ReaderButtonSliderInput::Next, 1);
    // Outside edit mode the side keys select a row; while editing they adjust its value.
    const auto previous =
        buttonSliderState.editing ? mappedInput.sliderDecreaseButton() : MappedInputManager::Button::Up;
    const auto next = buttonSliderState.editing ? mappedInput.sliderIncreaseButton() : MappedInputManager::Button::Down;
    handleDirection(previous, ReaderButtonSliderInput::Previous, -5);
    handleDirection(next, ReaderButtonSliderInput::Next, 5);
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      readerButtonSliderInput(buttonSliderState, ReaderButtonSliderInput::Confirm);
      requestUpdate();
    }
    return;
  }
  if (SETTINGS.menuNavigation == CrossPointSettings::MENU_NAV_CLASSIC) {
    buttonNavigator.onNextRelease([this] { moveSelection(true, false); });
    buttonNavigator.onPreviousRelease([this] { moveSelection(false, false); });
    buttonNavigator.onNextContinuous([this] { moveSelection(true, true); });
    buttonNavigator.onPreviousContinuous([this] { moveSelection(false, true); });
  } else {
    const auto left = mappedInput.menuButton(MappedInputManager::Button::Left);
    const auto right = mappedInput.menuButton(MappedInputManager::Button::Right);
    const auto up = mappedInput.menuButton(MappedInputManager::Button::Up);
    const auto down = mappedInput.menuButton(MappedInputManager::Button::Down);
    buttonNavigator.onRelease({down, down}, [this] { moveSelection(true, false); });
    buttonNavigator.onRelease({up, up}, [this] { moveSelection(false, false); });
    buttonNavigator.onContinuous({down, down}, [this] { moveSelection(true, true); });
    buttonNavigator.onContinuous({up, up}, [this] { moveSelection(false, true); });
    if (state.pane == ReaderDrawerPane::Root) {
      buttonNavigator.onRelease({right, right}, [this] { changeTab(adjacentReaderDrawerTab(state.tab, true)); });
      buttonNavigator.onRelease({left, left}, [this] { changeTab(adjacentReaderDrawerTab(state.tab, false)); });
      buttonNavigator.onContinuous({right, right}, [this] { changeTab(adjacentReaderDrawerTab(state.tab, true)); });
      buttonNavigator.onContinuous({left, left}, [this] { changeTab(adjacentReaderDrawerTab(state.tab, false)); });
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!mappedInput.hasTouchHardware() && state.pane == ReaderDrawerPane::Root && !buttonFocusActive) {
      changeTab(adjacentReaderDrawerTab(state.tab, true));
      return;
    }
    buttonFocusActive = true;
    if (state.pane == ReaderDrawerPane::Percent) {
      completePercentSelection();
      return;
    }
    if (state.pane == ReaderDrawerPane::StablePage) {
      completeStablePageSelection();
      return;
    }
    if (state.pane == ReaderDrawerPane::AutoPageTurn) {
      completeAutoPageTurnSelection();
      return;
    }
    activateListIndex(state.selectedIndex);
  }
}

void EpubReaderDrawerActivity::render(RenderLock&&) {
#if CROSSINK_SCALABLE_FONTS
  if (ttfResetPopup.processRender(renderer, mappedInput)) return;
#endif
  const bool buttonDevice = !mappedInput.hasTouchHardware();
  const int16_t drawerEdge =
      buttonDevice ? drawerHeight() : static_cast<int16_t>(renderer.getScreenHeight() - drawerHeight());
  if (previousDrawerEdge >= 0 && drawerEdge != previousDrawerEdge) {
    // Only the touch sheet changes height; redraw newly exposed reader pixels.
    if (drawerEdge > previousDrawerEdge) {
      const int16_t exposedTop = std::min(drawerEdge, previousDrawerEdge);
      renderer.fillRect(0, exposedTop, renderer.getScreenWidth(), std::abs(drawerEdge - previousDrawerEdge),
                        ReaderUtils::readerDarkModeEnabled());
      previewDirty = true;
    }
  }
  previousDrawerEdge = drawerEdge;
  int previewFontId = -1;
  // Keep prewarmed glyphs resident through the BW and optional touch grayscale passes.
  std::optional<FontCacheManager::PrewarmScope> previewPrewarmScope;
  bool previewRendered;
#if !CROSSINK_APP_READER_SAMPLE_PREVIEW
  previewRendered = renderPreview(previewFontId, previewPrewarmScope);
#endif
  uiReady = false;
  if (CROSSINK_APP_READER_SAMPLE_PREVIEW && fontPreviewLoading) {
    GUI.drawPopup(renderer, tr(STR_LOADING), true);
    fontPreviewLoading = false;
  }
  renderUiApp(app, uiTarget);
  if (buttonDevice) drawButtonBookHeader();
#if CROSSINK_APP_READER_SAMPLE_PREVIEW
  previewDirty = true;  // The full-screen UI cleared the sample area as well.
  previewRendered = renderPreview(previewFontId, previewPrewarmScope);
  if (showsSamplePreview() && previewUnavailable) {
    renderUiApp(app, uiTarget);  // A failed font selection rolled the draft back; repaint its values too.
    drawButtonBookHeader();
    renderPreviewUnavailable();
  }
#endif
  uiReady = true;
  if (!mappedInput.hasTouchHardware()) {
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    if (safe.y > 0) renderer.fillRect(0, 0, renderer.getScreenWidth(), safe.y, false);
    if (safe.x > 0) renderer.fillRect(0, 0, safe.x, renderer.getScreenHeight(), false);
    const int right = renderer.getScreenWidth() - safe.x - safe.width;
    const int bottom = renderer.getScreenHeight() - safe.y - safe.height;
    if (right > 0) renderer.fillRect(safe.x + safe.width, 0, right, renderer.getScreenHeight(), false);
    if (bottom > 0) renderer.fillRect(0, safe.y + safe.height, renderer.getScreenWidth(), bottom, false);

    const char* confirmLabel = tr(STR_SELECT);
    if (state.pane == ReaderDrawerPane::AutoPageTurn) {
      confirmLabel = tr(STR_CONFIRM);
    } else if (state.pane == ReaderDrawerPane::Root && !buttonFocusActive) {
      confirmLabel = tr(STR_NEXT_FIELD);
    }
    const bool fineAdjustment = (state.pane == ReaderDrawerPane::Percent && !percentKeypadActive) ||
                                state.pane == ReaderDrawerPane::AutoPageTurn ||
                                (readerDrawerStepChangesSettings(state.pane) && buttonSliderState.editing);
    const bool menuNavigation = state.pane != ReaderDrawerPane::Percent &&
                                state.pane != ReaderDrawerPane::AutoPageTurn &&
                                !readerDrawerStepChangesSettings(state.pane);
    const bool horizontalFront =
        SETTINGS.menuNavigation == CrossPointSettings::MENU_NAV_DIRECTIONAL && menuNavigation &&
        mappedInput.menuButton(MappedInputManager::Button::Up) == MappedInputManager::Button::Up;
    const char* previousLabel = horizontalFront ? tr(STR_DIR_LEFT) : tr(STR_DIR_UP);
    const char* nextLabel = horizontalFront ? tr(STR_DIR_RIGHT) : tr(STR_DIR_DOWN);
    if (state.pane == ReaderDrawerPane::Percent && percentKeypadActive) {
      previousLabel = tr(STR_DIR_LEFT);
      nextLabel = tr(STR_DIR_RIGHT);
    } else if (fineAdjustment) {
      previousLabel = "-1";
      nextLabel = "+1";
    }
    const auto labels =
        mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), confirmLabel, previousLabel, nextLabel);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  // Button menus repaint the sample on every navigation step. A grayscale pass
  // here would add a second panel refresh and flash the preview each time.
  if (!CROSSINK_APP_READER_SAMPLE_PREVIEW &&
      shouldRenderReaderDrawerAntiAliasing(previewRendered, draft.textAntiAliasing,
                                           ReaderUtils::readerForegroundBlack()) &&
      !sdFontSystem.fontUsesMonochromeRaster(renderer, previewFontId, draft.sdFontFamilyName.data())) {
    renderPreviewWithAntiAliasing(previewFontId);
  }
}

const char* EpubReaderDrawerActivity::paneTitle() const {
  switch (state.pane) {
    case ReaderDrawerPane::ReaderFont:
      return tr(STR_READER_FONT);
    case ReaderDrawerPane::DictionaryFont:
      return tr(STR_DICTIONARY_FONT);
    case ReaderDrawerPane::FontFamily:
      return tr(STR_FONT_FAMILY);
    case ReaderDrawerPane::CharacterSpacing:
      return tr(STR_CHARACTER_SPACING);
    case ReaderDrawerPane::Spacing:
      return tr(STR_SPACING);
    case ReaderDrawerPane::Margins:
      return tr(STR_SCREEN_MARGIN);
    case ReaderDrawerPane::Percent:
      return tr(STR_GO_TO_PERCENT);
    case ReaderDrawerPane::StablePage:
      return tr(STR_GO_TO_STABLE_PAGE);
    case ReaderDrawerPane::AutoPageTurn:
      return tr(STR_AUTO_TURN_INTERVAL_SECONDS);
    case ReaderDrawerPane::Dictionary:
      return tr(STR_BOOK_DICTIONARY);
    case ReaderDrawerPane::EnumOptions:
      return I18N.get(enumOptionTitle);
#if CROSSINK_SCALABLE_FONTS
    case ReaderDrawerPane::TtfRendering:
      return tr(STR_TTF_RENDERING);
#endif
    case ReaderDrawerPane::Chapters:
      return tr(STR_SELECT_CHAPTER);
    case ReaderDrawerPane::Root:
      return "";
    default:
      break;
  }
  return "";
}

const char* EpubReaderDrawerActivity::rowLabel(const RowId row) const {
  switch (row) {
    case RowId::ReaderFont:
      return tr(STR_READER_FONT);
    case RowId::TtfRendering:
      return tr(STR_TTF_RENDERING);
    case RowId::DictionaryFont:
      return tr(STR_DICTIONARY_FONT);
    case RowId::Spacing:
      return tr(STR_SPACING);
    case RowId::TextAa:
      return tr(STR_TEXT_AA);
    case RowId::Focus:
      return tr(STR_FOCUS_READING);
    case RowId::GuideDots:
      return tr(STR_GUIDE_READING);
    case RowId::Margins:
      return tr(STR_SCREEN_MARGIN);
    case RowId::Orientation:
      return tr(STR_ORIENTATION);
    case RowId::Alignment:
      return tr(STR_PARA_ALIGNMENT);
    case RowId::Hyphenation:
      return tr(STR_HYPHENATION);
    case RowId::PublisherPages:
      return tr(STR_PUBLISHER_PAGE_NUMBERS);
    case RowId::ExtraSpacing:
      return tr(STR_EXTRA_SPACING);
    case RowId::ForceIndents:
      return tr(STR_FORCE_PARAGRAPH_INDENTS);
    case RowId::PublisherFonts:
      return tr(STR_PUBLISHER_FONTS);
    case RowId::EmbeddedStyle:
      return tr(STR_EMBEDDED_STYLE);
    case RowId::Images:
      return tr(STR_IMAGES);
    case RowId::SelectChapter:
      return tr(STR_SELECT_CHAPTER);
    case RowId::GoToPercent:
      return tr(STR_GO_TO_PERCENT);
    case RowId::GoToStablePage:
      return tr(STR_GO_TO_STABLE_PAGE);
    case RowId::BookmarkToggle:
      return isCurrentPageBookmarked ? tr(STR_REMOVE_BOOKMARK) : tr(STR_ADD_BOOKMARK);
    case RowId::ViewBookmarks:
      return tr(STR_VIEW_BOOKMARKS);
    case RowId::Screenshot:
      return tr(STR_SCREENSHOT_BUTTON);
    case RowId::DisplayQr:
      return tr(STR_DISPLAY_QR);
    case RowId::Footnotes:
      return tr(STR_FOOTNOTES);
    case RowId::Lookup:
      return tr(STR_LOOKUP);
    case RowId::LookupHistory:
      return tr(STR_LOOKUP_HISTORY);
    case RowId::SaveClipping:
      return tr(STR_SAVE_CLIPPING);
    case RowId::ViewClippings:
      return tr(STR_VIEW_CLIPPINGS);
    case RowId::DeleteBookmarks:
      return tr(STR_DELETE_BOOKMARKS);
    case RowId::StatusBar:
      return tr(STR_STATUS_BARS);
    case RowId::BookDictionary:
      return tr(STR_BOOK_DICTIONARY);
    case RowId::RenderMode:
      return tr(STR_EPUB_RENDER_MODE);
    case RowId::IndexingMethod:
      return tr(STR_INDEXING_METHOD);
    case RowId::ToggleCompleted:
      return isBookCompleted ? tr(STR_MARK_UNFINISHED) : tr(STR_MARK_FINISHED);
    case RowId::TrackBookStats:
      return tr(STR_TRACK_READING_STATS);
    case RowId::Controls:
      return tr(STR_CAT_CONTROLS);
    case RowId::ResetReadingPace:
      return tr(STR_RESET_READING_PACE);
    case RowId::ResetBookReaderSettings:
      return tr(STR_RESET_BOOK_READER_SETTINGS);
    case RowId::DeleteCache:
      return tr(STR_DELETE_CACHE);
    case RowId::DeleteStats:
      return tr(STR_DELETE_BOOK_STATS);
    case RowId::AutoPageTurn:
      return tr(STR_AUTO_TURN_INTERVAL_SECONDS);
    case RowId::ReadingStats:
      return tr(STR_READING_STATS);
    case RowId::SyncProgress:
      return tr(STR_SYNC_BOOK);
    case RowId::NearbyPositionSync:
      return tr(STR_NEARBY_POSITION_SYNC);
    case RowId::SendNearbyBook:
      return tr(STR_SEND_NEARBY_BOOK);
    case RowId::FontFamily:
      return tr(STR_FONT_FAMILY);
    case RowId::CharacterSpacing:
      return tr(STR_CHARACTER_SPACING);
    case RowId::FontSize:
      return tr(STR_FONT_SIZE);
    case RowId::DictionaryFontFamily:
      return tr(STR_FONT_FAMILY);
    case RowId::DictionaryFontSize:
      return tr(STR_FONT_SIZE);
#if CROSSINK_SCALABLE_FONTS
    case RowId::TtfHinting:
      return TtfRenderOptionsActivity::rowLabel(TtfRenderOptionsActivity::Row::Hinting);
    case RowId::TtfRaster:
      return TtfRenderOptionsActivity::rowLabel(TtfRenderOptionsActivity::Row::Raster);
    case RowId::TtfInterpreter:
      return TtfRenderOptionsActivity::rowLabel(TtfRenderOptionsActivity::Row::Interpreter);
    case RowId::TtfWeight:
      return TtfRenderOptionsActivity::rowLabel(TtfRenderOptionsActivity::Row::Weight);
    case RowId::TtfSlant:
      return TtfRenderOptionsActivity::rowLabel(TtfRenderOptionsActivity::Row::Slant);
    case RowId::TtfStemDarkening:
      return TtfRenderOptionsActivity::rowLabel(TtfRenderOptionsActivity::Row::StemDarkening);
    case RowId::TtfReset:
      return TtfRenderOptionsActivity::rowLabel(TtfRenderOptionsActivity::Row::Reset);
#endif
    default:
      break;
  }
  return "";
}

const char* EpubReaderDrawerActivity::rowValue(const RowId row, char* buffer, const size_t bufferSize) const {
  switch (row) {
    case RowId::FontFamily: {
      if (draft.sdFontFamilyName[0] != '\0') return draft.sdFontFamilyName.data();
      static constexpr std::array<StrId, CrossPointSettings::BUILTIN_FONT_COUNT> labels = {StrId::STR_LEXEND_DECA,
                                                                                           StrId::STR_BITTER};
      if (draft.fontFamily >= labels.size()) return tr(STR_UNAVAILABLE);
      return I18N.get(labels[draft.fontFamily]);
    }
    case RowId::CharacterSpacing:
      CrossPointSettings::formatCharacterSpacing(draft.characterSpacing, buffer, bufferSize);
      return buffer;
    case RowId::FontSize:
      std::snprintf(buffer, bufferSize, "%upt", draft.readerFontPointSize);
      return buffer;
#if CROSSINK_SCALABLE_FONTS
    case RowId::TtfHinting:
      return TtfRenderOptionsActivity::rowValue(TtfRenderOptionsActivity::Row::Hinting, ttfRenderProfile);
    case RowId::TtfRaster:
      return TtfRenderOptionsActivity::rowValue(TtfRenderOptionsActivity::Row::Raster, ttfRenderProfile);
    case RowId::TtfInterpreter:
      return TtfRenderOptionsActivity::rowValue(TtfRenderOptionsActivity::Row::Interpreter, ttfRenderProfile);
    case RowId::TtfWeight:
      return TtfRenderOptionsActivity::rowValue(TtfRenderOptionsActivity::Row::Weight, ttfRenderProfile);
    case RowId::TtfSlant:
      return TtfRenderOptionsActivity::rowValue(TtfRenderOptionsActivity::Row::Slant, ttfRenderProfile);
    case RowId::TtfStemDarkening:
      return TtfRenderOptionsActivity::rowValue(TtfRenderOptionsActivity::Row::StemDarkening, ttfRenderProfile);
    case RowId::TtfReset:
      return TtfRenderOptionsActivity::rowValue(TtfRenderOptionsActivity::Row::Reset, ttfRenderProfile);
#endif
    case RowId::Orientation: {
      return I18N.get(readerOrientationLabel(draft.orientation));
    }
    case RowId::Alignment: {
      static const std::array<StrId, 5> labels = {StrId::STR_JUSTIFY, StrId::STR_DIR_LEFT, StrId::STR_CENTER,
                                                  StrId::STR_DIR_RIGHT, StrId::STR_BOOK_S_STYLE};
      return I18N.get(labels[std::min<size_t>(draft.paragraphAlignment, labels.size() - 1)]);
    }
    case RowId::PublisherFonts: {
      static constexpr std::array<StrId, 3> labels = {StrId::STR_OFF, StrId::STR_PUBLISHER_FONTS_ACCENTS,
                                                      StrId::STR_PUBLISHER_FONTS_ALL};
      return I18N.get(labels[std::min<size_t>(draft.publisherFonts, labels.size() - 1)]);
    }
    case RowId::Images: {
      static constexpr std::array<StrId, ImageDisplayMode::Count> labels = {
          StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_DISPLAY_BW, StrId::STR_IMAGES_PLACEHOLDER,
          StrId::STR_IMAGES_SUPPRESS};
      return I18N.get(labels[ImageDisplayMode::selected(draft.imageRendering, draft.imageGrayscale)]);
    }
    case RowId::RenderMode: {
      static const std::array<StrId, 3> labels = {StrId::STR_RENDER_MODE_CROSSINK_DEFAULT,
                                                  StrId::STR_RENDER_MODE_BALANCED, StrId::STR_RENDER_MODE_LIGHT};
      return I18N.get(labels[std::min<size_t>(draft.epubRenderMode, labels.size() - 1)]);
    }
    case RowId::IndexingMethod:
      return draft.indexingMethod == 0 ? tr(STR_INDEXING_INCREMENTAL) : tr(STR_INDEXING_FULL_SECTION);
    case RowId::BookDictionary:
      if (bookDictionaryPath.empty()) return tr(STR_DICT_USE_GLOBAL);
      for (size_t i = 1; i < dictionaryPaths.size(); ++i) {
        if (dictionaryPaths[i] == bookDictionaryPath) return dictionaryLabels[i].c_str();
      }
      return tr(STR_UNAVAILABLE);
    case RowId::DictionaryFontFamily:
      return hasDictionaryFontOverride ? dictionaryFontFamilyName : tr(STR_DICT_USE_GLOBAL);
    case RowId::DictionaryFontSize:
      if (!hasDictionaryFontOverride || dictionaryFontPointSize == 0) return tr(STR_DICT_USE_GLOBAL);
      std::snprintf(buffer, bufferSize, "%upt", dictionaryFontPointSize);
      return buffer;
    case RowId::AutoPageTurn:
      std::snprintf(buffer, bufferSize, "%us", autoPageTurnIntervalSeconds);
      return buffer;
    default:
      return nullptr;
  }
}

bool EpubReaderDrawerActivity::rowIsToggle(const RowId row) const {
  switch (row) {
    case RowId::TrackBookStats:
    case RowId::TextAa:
    case RowId::Focus:
    case RowId::GuideDots:
    case RowId::Hyphenation:
    case RowId::PublisherPages:
    case RowId::ExtraSpacing:
    case RowId::ForceIndents:
    case RowId::EmbeddedStyle:
#if CROSSINK_SCALABLE_FONTS
    case RowId::TtfStemDarkening:
#endif
      return true;
    default:
      return false;
  }
}

bool EpubReaderDrawerActivity::rowShowsNavigationCaret(const RowId row) const {
  if (rowIsToggle(row)) return false;
  char value[64] = {};
  if (rowValue(row, value, sizeof(value)) != nullptr) return false;
  switch (row) {
    case RowId::BookmarkToggle:
    case RowId::ToggleCompleted:
    case RowId::Screenshot:
    case RowId::DisplayQr:
    case RowId::Lookup:
    case RowId::SaveClipping:
    case RowId::ResetReadingPace:
    case RowId::ResetBookReaderSettings:
    case RowId::DeleteBookmarks:
    case RowId::DeleteCache:
    case RowId::DeleteStats:
    case RowId::SyncProgress:
    case RowId::NearbyPositionSync:
    case RowId::SendNearbyBook:
      return false;
    default:
      return true;
  }
}

bool EpubReaderDrawerActivity::rowToggleValue(const RowId row) const {
  switch (row) {
    case RowId::TrackBookStats:
      return bookStatsEnabled;
    case RowId::TextAa:
      return draft.textAntiAliasing;
    case RowId::Focus:
      return draft.focusReadingEnabled;
    case RowId::GuideDots:
      return draft.guideReadingEnabled;
    case RowId::Hyphenation:
      return draft.hyphenationEnabled;
    case RowId::PublisherPages:
      return draft.publisherPageNumbers;
    case RowId::ExtraSpacing:
      return draft.extraParagraphSpacing;
    case RowId::ForceIndents:
      return draft.forceParagraphIndents;
#if CROSSINK_SCALABLE_FONTS
    case RowId::TtfStemDarkening:
      return ttfRenderProfile.stemDarkening;
#endif
    case RowId::EmbeddedStyle:
      return draft.embeddedStyle;
    default:
      return false;
  }
}
