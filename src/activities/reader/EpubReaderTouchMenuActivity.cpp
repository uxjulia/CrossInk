#include "EpubReaderTouchMenuActivity.h"

#if CROSSINK_APP_CAP_TOUCH

#include <Epub/Page.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFontSystem.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "SettingsList.h"
#include "activities/reader/ControlsOptionsActivity.h"
#include "activities/settings/StatusBarSettingsActivity.h"
#include "components/DrawerHandle.h"
#include "components/SliderValue.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/listIcons.h"
#include "components/icons/touchHeaderIcons.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"
#include "util/FontFamilyLabel.h"

namespace fui = freeink::ui;

namespace {
constexpr int16_t TAB_BAR_HEIGHT = 58;
constexpr int16_t SLIDER_CONTROL_HEIGHT = 30;
constexpr int16_t SLIDER_CAPTION_GAP = 15;
constexpr int16_t COMPACT_SLIDER_CAPTION_GAP = 5;
constexpr int16_t SLIDER_SCALE_GAP = 4;
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
constexpr uint8_t PORTRAIT_DRAWER_HEIGHT_PERCENT = 50;
// Non-root landscape panes retain a little more room than portrait. Root
// height is calculated from four actual theme rows in buildDrawer().
constexpr uint8_t LANDSCAPE_DRAWER_HEIGHT_PERCENT = 65;
constexpr uint8_t LANDSCAPE_DUAL_SLIDER_DRAWER_HEIGHT_PERCENT = 75;

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
  bool enabled = true;
};

void configureReaderSliderScale(ReaderSliderRowProps& row, const char* minimum, const char* maximum) {
  row.minimumLabel = minimum;
  row.maximumLabel = maximum;
}

template <size_t MaxInteractions>
int16_t readerSliderRowHeight(fui::Screen<MaxInteractions>& screen, const ReaderSliderRowProps& row) {
  const int16_t lineHeight = screen.target().lineHeight(screen.theme().bodyText.font);
  const int16_t captionHeight = row.label ? static_cast<int16_t>(lineHeight + row.captionGap) : 0;
  const int16_t currentValueHeight = row.value ? static_cast<int16_t>(lineHeight + SLIDER_SCALE_GAP) : 0;
  const int16_t endpointHeight =
      (row.minimumLabel || row.maximumLabel) ? static_cast<int16_t>(lineHeight + SLIDER_SCALE_GAP) : 0;
  return static_cast<int16_t>(captionHeight + currentValueHeight + SLIDER_CONTROL_HEIGHT + endpointHeight);
}

template <size_t MaxInteractions>
int16_t readerSliderControlTopInset(fui::Screen<MaxInteractions>& screen, const ReaderSliderRowProps& row) {
  const int16_t lineHeight = screen.target().lineHeight(screen.theme().bodyText.font);
  const int16_t captionHeight = row.label ? static_cast<int16_t>(lineHeight + row.captionGap) : 0;
  const int16_t currentValueHeight = row.value ? static_cast<int16_t>(lineHeight + SLIDER_SCALE_GAP) : 0;
  return static_cast<int16_t>(captionHeight + currentValueHeight);
}

template <size_t MaxInteractions>
void drawReaderSliderRow(fui::Screen<MaxInteractions>& screen, const ReaderSliderRowProps& row) {
  const int16_t height = readerSliderRowHeight(screen, row);
  const fui::Rect rect = screen.takeTop(height);
  fui::TextStyle labelStyle = screen.theme().bodyText;
  labelStyle.bold = false;
  fui::TextStyle valueStyle = screen.theme().bodyText;
  valueStyle.bold = true;

  const int16_t lineHeight = screen.target().lineHeight(labelStyle.font);
  const int16_t captionHeight = row.label ? static_cast<int16_t>(lineHeight + row.captionGap) : 0;
  if (row.label) screen.target().text(fui::Rect{rect.x, rect.y, rect.width, lineHeight}, row.label, labelStyle);

  const int16_t currentValueHeight = row.value ? static_cast<int16_t>(lineHeight + SLIDER_SCALE_GAP) : 0;
  const int16_t controlTop = static_cast<int16_t>(rect.y + captionHeight + currentValueHeight);
  const fui::Rect band{rect.x, controlTop, rect.width, SLIDER_CONTROL_HEIGHT};
  // Match the frontlight slider's control lanes: a shorter track leaves
  // genuinely finger-sized +/- targets at either end, even on the Sticky.
  const int16_t stepWidth = std::max<int16_t>(band.height, screen.theme().rowHeight);
  const int16_t sideGap = static_cast<int16_t>(stepWidth + screen.theme().spaceSm);
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

  if (row.value) {
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

  if (row.minimumLabel || row.maximumLabel) {
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
void drawDualReaderSliderRows(fui::Screen<MaxInteractions>& screen, const ReaderSliderRowProps& first,
                              const ReaderSliderRowProps& second) {
  const int16_t bottomPadding = screen.theme().spaceSm;
  const int16_t remaining =
      std::max<int16_t>(0, static_cast<int16_t>(screen.body().height - readerSliderRowHeight(screen, first) -
                                                readerSliderRowHeight(screen, second) - bottomPadding));
  drawReaderSliderRow(screen, first);
  // Keep the controls evenly separated while reserving a small lane beneath
  // the second scale so its endpoint labels do not sit on the tab-bar rule.
  screen.spacer(remaining);
  drawReaderSliderRow(screen, second);
  screen.spacer(bottomPadding);
}

uint16_t configureDrawerList(fui::ListProps& props, const fui::ThemeTokens& theme, const fui::Rect bounds) {
  // Root rows use SettingRowProps, whose content starts at the drawer inset
  // plus its 8px side padding. Do not inherit themed list pills here: their
  // additional row inset makes picker labels visibly farther to the right.
  props.rowInset = 0;
  props.sidePadding = fui::SettingRowProps{}.sidePadding;
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
  switch (std::min<uint8_t>(value, CrossPointSettings::MAX_WORD_SPACING)) {
    case 0:
      return "0";
    case 1:
      return "1";
    case 2:
      return "2";
    case 3:
      return "3";
    default:
      return "4";
  }
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

EpubReaderTouchMenuActivity::EpubReaderTouchMenuActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> epub,
    const TouchReaderPreviewModel* previewModel, const int bookProgressPercent, const bool hasFootnotes,
    const bool hasDictionary, const bool hasBookmarks, const bool hasClippings, const bool isCurrentPageBookmarked,
    const bool isBookCompleted, const bool showReadingPaceReset, const bool stablePageNumbersAvailable,
    const uint16_t autoPageTurnIntervalSeconds, const bool automaticPageTurnActive,
    ReaderOptionsActivity::SaveSettingsCallback saveReaderSettingsCallback, void* saveReaderSettingsContext,
    ReaderOptionsActivity::SaveGlobalSettingsCallback saveGlobalSettingsCallback, void* saveGlobalSettingsContext,
    ReaderOptionsActivity::GlobalSettingsEditCallback beginGlobalSettingsEditCallback,
    void* beginGlobalSettingsEditContext,
    ReaderOptionsActivity::GlobalSettingsEditCallback endGlobalSettingsEditCallback, void* endGlobalSettingsEditContext,
    const char* dictionaryFontFamilyName, const uint8_t dictionaryFontPointSize, const bool hasDictionaryFontOverride,
    ReaderOptionsActivity::DictionaryFontChangedCallback dictionaryFontChangedCallback,
    void* dictionaryFontChangedContext, const ReaderDrawerState initialState)
    : Activity("EpubReaderTouchMenu", renderer, mappedInput),
      epub(std::move(epub)),
      previewModel(previewModel),
      percent(std::clamp(bookProgressPercent, 0, 100)),
      hasFootnotes(hasFootnotes),
      hasDictionary(hasDictionary),
      hasBookmarks(hasBookmarks),
      hasClippings(hasClippings),
      isCurrentPageBookmarked(isCurrentPageBookmarked),
      isBookCompleted(isBookCompleted),
      showReadingPaceReset(showReadingPaceReset),
      stablePageNumbersAvailable(stablePageNumbersAvailable),
      automaticPageTurnActive(automaticPageTurnActive),
      autoPageTurnIntervalSeconds(std::clamp(autoPageTurnIntervalSeconds, READER_AUTO_PAGE_TURN_MIN_SECONDS,
                                             READER_AUTO_PAGE_TURN_MAX_SECONDS)),
      state(initialState),
      draft(captureSettings()),
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
  if (dictionaryFontFamilyName) {
    std::strncpy(this->dictionaryFontFamilyName, dictionaryFontFamilyName, sizeof(this->dictionaryFontFamilyName) - 1);
  }
}

void EpubReaderTouchMenuActivity::onEnter() {
  Activity::onEnter();
  mappedInput.setReaderTouchscreenOverride(true);

  const ReaderDrawerCatalog catalog =
      makeReaderDrawerCatalog({hasFootnotes, hasDictionary, hasBookmarks, hasClippings, showReadingPaceReset});
  for (size_t tab = 0; tab < rootRows.size(); ++tab) {
    rootRows[tab].reserve(catalog[tab].count);
    rootRows[tab].assign(catalog[tab].items.begin(), catalog[tab].items.begin() + catalog[tab].count);
  }

  paneRows.reserve(4);
  discoverFonts();
  discoverDictionaries();
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &EpubReaderTouchMenuActivity::onRowEvent, this);
  app.on(ACTION_TAB, &EpubReaderTouchMenuActivity::onTabEvent, this);
  app.on(ACTION_DISMISS, &EpubReaderTouchMenuActivity::onDismissEvent, this);
  app.on(ACTION_BACK, &EpubReaderTouchMenuActivity::onBackEvent, this);
  app.on(ACTION_SLIDER, &EpubReaderTouchMenuActivity::onSliderEvent, this);
  app.on(ACTION_STEP, &EpubReaderTouchMenuActivity::onStepEvent, this);
  app.on(ACTION_SLIDER + 3, &EpubReaderTouchMenuActivity::onSliderEvent, this);
  app.on(ACTION_STEP + 3, &EpubReaderTouchMenuActivity::onStepEvent, this);
  app.on(ACTION_CONFIRM, &EpubReaderTouchMenuActivity::onConfirmEvent, this);
  app.setScreen(&EpubReaderTouchMenuActivity::drawerScreen, this);
  requestUpdate();
}

void EpubReaderTouchMenuActivity::onExit() {
  commitSettings();
  dictionaryRegistry.clear();
  sdFontSystem.releaseRegistry();
  mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();
}

void EpubReaderTouchMenuActivity::discoverFonts() {
  sdFontSystem.ensureRegistry();
  const auto& families = sdFontSystem.registry().getFamilies();
  fontLabels.clear();
  fontSettingIndexes.clear();
  fontLabels.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + families.size());
  fontSettingIndexes.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + families.size());
  constexpr FontFamilyPointSizeRange builtinRange{10, 16};
  fontLabels.push_back(fontFamilyLabel(tr(STR_LEXEND_DECA), builtinRange));
  fontLabels.push_back(fontFamilyLabel(tr(STR_BITTER), builtinRange));
  fontSettingIndexes.push_back(0);
  fontSettingIndexes.push_back(1);
  for (size_t i = 0; i < families.size(); ++i) {
    fontLabels.push_back(fontFamilyLabel(families[i].name, fontFamilyPointSizeRange(families[i])));
    fontSettingIndexes.push_back(static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i));
  }
}

void EpubReaderTouchMenuActivity::discoverDictionaries() {
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

void EpubReaderTouchMenuActivity::commitSettings() {
  if (!settingsChanged) return;
  applySettings(draft);
  if (saveReaderSettingsCallback) {
    saveReaderSettingsCallback(saveReaderSettingsContext);
  } else if (!SETTINGS.saveToFile()) {
    LOG_ERR("ERDM", "Failed to persist touch reader settings");
  }
  settingsChanged = false;
}

ReaderSettingsDraft EpubReaderTouchMenuActivity::captureSettings() {
  ReaderSettingsDraft value;
  value.fontFamily = SETTINGS.fontFamily;
  value.readerFontPointSize = SETTINGS.readerFontPointSize;
  std::strncpy(value.sdFontFamilyName.data(), SETTINGS.sdFontFamilyName, value.sdFontFamilyName.size() - 1);
  value.lineHeightPercent = SETTINGS.lineHeightPercent;
  value.wordSpacing = SETTINGS.wordSpacing;
  value.screenMarginVertical = SETTINGS.screenMarginVertical;
  value.screenMarginHorizontal = SETTINGS.screenMarginHorizontal;
  value.orientation = SETTINGS.orientation;
  value.paragraphAlignment = SETTINGS.paragraphAlignment;
  value.textAntiAliasing = SETTINGS.textAntiAliasing;
  value.bionicReadingEnabled = SETTINGS.bionicReadingEnabled;
  value.guideReadingEnabled = SETTINGS.guideReadingEnabled;
  value.hyphenationEnabled = SETTINGS.hyphenationEnabled;
  value.publisherPageNumbers = SETTINGS.publisherPageNumbers;
  value.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  value.forceParagraphIndents = SETTINGS.forceParagraphIndents;
  value.embeddedStyle = SETTINGS.embeddedStyle;
  value.imageRendering = SETTINGS.imageRendering;
  value.epubRenderMode = SETTINGS.epubRenderMode;
  value.indexingMethod = SETTINGS.indexingMethod;
  return value;
}

void EpubReaderTouchMenuActivity::applySettings(const ReaderSettingsDraft& value) {
  SETTINGS.fontFamily = value.fontFamily;
  SETTINGS.readerFontPointSize = value.readerFontPointSize;
  std::strncpy(SETTINGS.sdFontFamilyName, value.sdFontFamilyName.data(), sizeof(SETTINGS.sdFontFamilyName) - 1);
  SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
  SETTINGS.lineHeightPercent = value.lineHeightPercent;
  SETTINGS.wordSpacing = value.wordSpacing;
  SETTINGS.screenMarginVertical = value.screenMarginVertical;
  SETTINGS.screenMarginHorizontal = value.screenMarginHorizontal;
  SETTINGS.orientation = value.orientation;
  SETTINGS.paragraphAlignment = value.paragraphAlignment;
  SETTINGS.textAntiAliasing = value.textAntiAliasing;
  SETTINGS.bionicReadingEnabled = value.bionicReadingEnabled;
  SETTINGS.guideReadingEnabled = value.guideReadingEnabled;
  SETTINGS.hyphenationEnabled = value.hyphenationEnabled;
  SETTINGS.publisherPageNumbers = value.publisherPageNumbers;
  SETTINGS.extraParagraphSpacing = value.extraParagraphSpacing;
  SETTINGS.forceParagraphIndents = value.forceParagraphIndents;
  SETTINGS.embeddedStyle = value.embeddedStyle;
  SETTINGS.imageRendering = value.imageRendering;
  SETTINGS.epubRenderMode = value.epubRenderMode;
  SETTINGS.indexingMethod = value.indexingMethod;
}

void EpubReaderTouchMenuActivity::markSettingChanged(const ReaderSettingsChangeMask mask) {
  settingsChanged = true;
  didChangeSettings = true;
  previewDirty = previewDirty || hasReaderSettingsChange(mask, ReaderSettingsChangeMask::Preview);
  changeMask = changeMask | mask;
}

void EpubReaderTouchMenuActivity::closeAndReturn(const bool cancelled, const EpubReaderMenuAction action,
                                                 const bool reopenDrawer) {
  const bool changed = didChangeSettings;
  commitSettings();
  if (!cancelled && epub &&
      (action == EpubReaderMenuAction::SYNC || action == EpubReaderMenuAction::NEARBY_POSITION_SYNC ||
       action == EpubReaderMenuAction::SEND_NEARBY_BOOK)) {
    PendingOverlayResume resume;
    resume.origin = PendingOverlayOrigin::Reader;
    resume.overlay = PendingOverlayType::ReaderDrawer;
    resume.tab = static_cast<uint8_t>(state.tab);
    resume.pane = static_cast<uint8_t>(state.pane);
    resume.selectedIndex = state.selectedIndex;
    resume.scrollPosition =
        state.pane == ReaderDrawerPane::Root ? state.rootTopIndex[static_cast<size_t>(state.tab)] : state.paneTopIndex;
    resume.bookPath = epub->getPath();
    APP_STATE.setPendingOverlayResume(std::move(resume));
  }
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

bool EpubReaderTouchMenuActivity::handleHomeGesture() {
  closePane();
  return true;
}

void EpubReaderTouchMenuActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EpubReaderTouchMenuActivity*>(user);
  self->buttonFocusActive = false;
  self->state.selectedIndex = event.value;
  self->app.clearTapFlash();
  self->activateListIndex(event.value);
}

void EpubReaderTouchMenuActivity::onTabEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EpubReaderTouchMenuActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int16_t>(READER_DRAWER_TAB_COUNT)) return;
  self->buttonFocusActive = false;
  self->app.clearTapFlash();
  self->changeTab(static_cast<ReaderDrawerTab>(event.value));
}

void EpubReaderTouchMenuActivity::onDismissEvent(const fui::ActionEvent&, void* user) {
  static_cast<EpubReaderTouchMenuActivity*>(user)->closeAndReturn(true);
}

void EpubReaderTouchMenuActivity::onBackEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<EpubReaderTouchMenuActivity*>(user);
  self->buttonFocusActive = false;
  self->closePane();
}

void EpubReaderTouchMenuActivity::onSliderEvent(const fui::ActionEvent& event, void* user) {
  if (event.dragPermille < 0) return;
  auto* self = static_cast<EpubReaderTouchMenuActivity*>(user);
  self->buttonFocusActive = false;
  const auto tapValue = [self](const int value, const int minimum, const int maximum) {
    return self->sliderTapPending ? snapSliderTapValue(value, minimum, maximum, 5) : value;
  };
  const bool second = event.action == ACTION_SLIDER + 3;
  if (self->state.pane == ReaderDrawerPane::Spacing) {
    if (second) {
      self->draft.wordSpacing = percentToByte(event.dragPermille, 0, CrossPointSettings::MAX_WORD_SPACING);
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
  } else if (self->state.pane == ReaderDrawerPane::Percent) {
    self->percent = tapValue(percentToByte(event.dragPermille, 0, 100), 0, 100);
    self->requestUpdate();
    return;
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

void EpubReaderTouchMenuActivity::onStepEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EpubReaderTouchMenuActivity*>(user);
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
      self->draft.wordSpacing =
          std::clamp<int>(self->draft.wordSpacing + event.value, 0, CrossPointSettings::MAX_WORD_SPACING);
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

void EpubReaderTouchMenuActivity::onConfirmEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<EpubReaderTouchMenuActivity*>(user);
  self->buttonFocusActive = false;
  if (self->state.pane == ReaderDrawerPane::Percent) {
    self->completePercentSelection();
    return;
  }
  if (self->state.pane == ReaderDrawerPane::AutoPageTurn) {
    self->completeAutoPageTurnSelection();
    return;
  }
  self->closePane();
}

void EpubReaderTouchMenuActivity::drawerScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<EpubReaderTouchMenuActivity*>(user)->buildDrawer(screen);
}

void EpubReaderTouchMenuActivity::buildDrawer(UiApp::ScreenType& screen) {
  fui::SheetProps sheet;
  sheet.anchor = fui::SheetEdge::Bottom;
  sheet.dismissAction = ACTION_DISMISS;
  sheet.radius = 0;
  sheet.ruleWidth = DRAWER_RULE_WIDTH;
  const int16_t grabberBand = DrawerHandle::bandHeight(sheet);
  // Give root menus exactly four standard row slots in landscape. A percentage
  // was subtly too short once font-derived row height and the tab chrome were
  // subtracted, so some UI scales exposed only three rows.
  const int16_t tabBarHeight = static_cast<int16_t>(TAB_BAR_HEIGHT + TAB_BAR_VERTICAL_PADDING * 2);
  int16_t drawerHeight = static_cast<int16_t>(readerDrawerHeight(renderer, state.pane) + grabberBand);
  if (state.pane == ReaderDrawerPane::Root && isLandscapeOrientation(renderer.getOrientation())) {
    drawerHeight = static_cast<int16_t>(
        grabberBand + sheet.ruleWidth + tabBarHeight + DRAWER_LIST_TOP_PADDING +
        readerDrawerListHeightForRows(LANDSCAPE_ROOT_ROWS, screen.theme().rowHeight, screen.theme().spaceSm));
    drawerHeight = std::min<int16_t>(drawerHeight, renderer.getScreenHeight());
  }
  const fui::Rect sheetContent = screen.sheet(sheet, drawerHeight);
  drawerHandleRect = DrawerHandle::registerTap(screen.frame(), sheetContent, sheet, ACTION_DISMISS);
  // Give every tab row four pixels of white space above and below its icons.
  // The tab pill keeps its previous size so the selected background does not
  // become taller with the row.
  const fui::Rect tabs = screen.takeBottom(tabBarHeight);
  buildTabBar(screen, tabs, false);
  screen.insetContent(fui::Insets{sheet.ruleWidth, DRAWER_SIDE_INSET, 0, DRAWER_SIDE_INSET});

  switch (state.pane) {
    case ReaderDrawerPane::Root:
      buildRootRows(screen);
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
    default:
      buildSimplePane(screen);
      break;
  }
}

void EpubReaderTouchMenuActivity::buildTabBar(UiApp::ScreenType& screen, const fui::Rect rect,
                                              const bool drawBottomRule) {
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
  props.tabInset = fui::Insets{static_cast<int16_t>(4 + TAB_BAR_VERTICAL_PADDING), 4,
                               static_cast<int16_t>(8 + TAB_BAR_VERTICAL_PADDING), 4};
  const int16_t ruleY = drawBottomRule ? static_cast<int16_t>(rect.bottom() - 1) : rect.y;
  screen.target().fill(fui::Rect{rect.x, ruleY, rect.width, 1}, fui::Paint::solid(fui::Color::Black));
  fui::tabBar(screen.frame(), rect, props);
}

void EpubReaderTouchMenuActivity::buildPaneHeader(UiApp::ScreenType& screen) {
  fui::HeaderProps header;
  header.title = paneTitle();
  header.centered = true;
  header.borderEdges = fui::EdgesNone;
  header.styles = fui::plainStyles();
  header.titleText = screen.theme().bodyText;
  header.titleText.bold = true;
  header.sidePadding = screen.theme().headerSidePadding;
  header.minTouchSize = screen.theme().minTouchSize;
  const fui::Rect rect = screen.takeTop(screen.theme().headerHeight);
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

const std::vector<EpubReaderTouchMenuActivity::RowId>& EpubReaderTouchMenuActivity::activeRows() const {
  return state.pane == ReaderDrawerPane::Root ? rootRows[static_cast<size_t>(state.tab)] : paneRows;
}

int EpubReaderTouchMenuActivity::activeTopIndex() const {
  return state.pane == ReaderDrawerPane::Root ? state.rootTopIndex[static_cast<size_t>(state.tab)] : state.paneTopIndex;
}

void EpubReaderTouchMenuActivity::buildRootRows(UiApp::ScreenType& screen) {
  screen.spacer(DRAWER_LIST_TOP_PADDING);
  const auto& rows = activeRows();
  const fui::Rect listBounds = screen.body();
  const int16_t rowHeight = screen.theme().rowHeight;
  const int16_t gap = screen.theme().spaceSm;
  visibleRows = std::max(1, readerDrawerVisibleRows(listBounds.height, rowHeight, gap));
  int top = std::clamp<int>(state.rootTopIndex[static_cast<size_t>(state.tab)], 0,
                            std::max(0, static_cast<int>(rows.size()) - visibleRows));
  state.rootTopIndex[static_cast<size_t>(state.tab)] = static_cast<int16_t>(top);
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
    props.state = isReaderDrawerRowFocused(buttonFocusActive, state.selectedIndex, static_cast<int16_t>(top + i))
                      ? fui::StateSelected
                      : fui::StateNormal;
    if (rowIsToggle(row)) {
      fui::ToggleRowProps toggle;
      toggle.row = props;
      toggle.checked = rowToggleValue(row);
      toggle.toggleAction = ACTION_ROW;
      toggle.toggleValue = static_cast<int16_t>(top + i);
      screen.toggleRow(toggle);
    } else {
      screen.settingRow(props);
    }
  }
  fui::drawListScrollIndicator(screen.target(), drawerScrollbarBounds(listBounds), rows.size(), visibleRows, top,
                               screen.theme().listScrollWidth, screen.theme().listScrollSide,
                               screen.theme().listScrollInset);
}

void EpubReaderTouchMenuActivity::buildSimplePane(UiApp::ScreenType& screen) {
  buildPaneHeader(screen);
  const auto& rows = activeRows();
  for (size_t i = 0; i < rows.size(); ++i) {
    char value[48] = {};
    fui::SettingRowProps props;
    props.label = rowLabel(rows[i]);
    props.value = rowValue(rows[i], value, sizeof(value));
    props.action = ACTION_ROW;
    props.valueId = static_cast<int16_t>(i);
    props.drawChevron = rowShowsNavigationCaret(rows[i]);
    props.labelText = screen.theme().bodyText;
    props.valueText = screen.theme().smallText;
    props.state = isReaderDrawerRowFocused(buttonFocusActive, state.selectedIndex, static_cast<int16_t>(i))
                      ? fui::StateSelected
                      : fui::StateNormal;
    screen.settingRow(props);
  }
}

void EpubReaderTouchMenuActivity::buildSpacingPane(UiApp::ScreenType& screen) {
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
  word.sliderValue = byteToPermille(draft.wordSpacing, 0, CrossPointSettings::MAX_WORD_SPACING);
  word.max = 1000;
  word.sliderAction = ACTION_SLIDER + 3;
  word.decrement = ACTION_STEP + 3;
  word.increment = ACTION_STEP + 3;
  configureReaderSliderScale(word, "0", "4");
  line.captionGap = COMPACT_SLIDER_CAPTION_GAP;
  word.captionGap = COMPACT_SLIDER_CAPTION_GAP;
  drawDualReaderSliderRows(screen, line, word);
}

void EpubReaderTouchMenuActivity::buildMarginsPane(UiApp::ScreenType& screen) {
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
  vertical.captionGap = COMPACT_SLIDER_CAPTION_GAP;
  horizontal.captionGap = COMPACT_SLIDER_CAPTION_GAP;
  drawDualReaderSliderRows(screen, vertical, horizontal);
}

void EpubReaderTouchMenuActivity::buildPercentPane(UiApp::ScreenType& screen) {
  buildPaneHeader(screen);
  buildConfirmButton(screen);
  char value[16];
  std::snprintf(value, sizeof(value), "%d%%", percent);
  ReaderSliderRowProps slider;
  slider.value = value;
  slider.sliderValue = percent * 10;
  slider.max = 1000;
  slider.sliderAction = ACTION_SLIDER;
  slider.decrement = ACTION_STEP;
  slider.increment = ACTION_STEP;
  configureReaderSliderScale(slider, "0%", "100%");
  const int16_t top = centeredReaderSliderControlTop(screen, slider);
  screen.spacer(top);
  drawReaderSliderRow(screen, slider);
}

void EpubReaderTouchMenuActivity::buildAutoPageTurnPane(UiApp::ScreenType& screen) {
  buildPaneHeader(screen);
  buildConfirmButton(screen);
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
  const int16_t top = centeredReaderSliderControlTop(screen, slider);
  screen.spacer(top);
  drawReaderSliderRow(screen, slider);
}

void EpubReaderTouchMenuActivity::buildConfirmButton(UiApp::ScreenType& screen) {
  const int16_t buttonHeight = std::max<int16_t>(screen.theme().minTouchSize, screen.theme().rowHeight);
  // `takeBottom(..., gap)` creates space above a control. Reserve this band
  // first so Confirm also clears the navigation tabs below it.
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

void EpubReaderTouchMenuActivity::buildDictionaryPane(UiApp::ScreenType& screen) {
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

void EpubReaderTouchMenuActivity::buildFontFamilyPane(UiApp::ScreenType& screen) {
  buildPaneHeader(screen);
  const int total = static_cast<int>(fontLabels.size());
  const fui::Rect listBounds = screen.body();
  fui::ListProps props;
  // This picker has no subtitles, so it does not need the default
  // label-plus-subtitle row height that left a conspicuous blank band above
  // the tab row when scrolling.
  props.rowHeight = std::max<int16_t>(
      screen.theme().minTouchSize, static_cast<int16_t>(screen.theme().rowHeight - FONT_FAMILY_ROW_HEIGHT_REDUCTION));
  visibleRows = configureDrawerList(props, screen.theme(), listBounds);
  const int top = std::clamp<int>(state.paneTopIndex, 0, std::max(0, total - visibleRows));
  state.paneTopIndex = static_cast<int16_t>(top);
  const int drawCount = std::min<int>({visibleRows, WINDOW_SIZE, total - top});
  evenlySpaceDrawerListRows(props, listBounds, drawCount);
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

void EpubReaderTouchMenuActivity::buildEnumOptionsPane(UiApp::ScreenType& screen) {
  buildPaneHeader(screen);
  const int total = static_cast<int>(enumOptionLabels.size());
  const fui::Rect listBounds = screen.body();
  fui::ListProps props;
  visibleRows = configureDrawerList(props, screen.theme(), listBounds);
  const int top = std::clamp<int>(state.paneTopIndex, 0, std::max(0, total - visibleRows));
  state.paneTopIndex = static_cast<int16_t>(top);
  const int drawCount = std::min<int>({visibleRows, WINDOW_SIZE, total - top});
  if (enumOptionRow == RowId::FontSize || enumOptionRow == RowId::DictionaryFontFamily ||
      enumOptionRow == RowId::DictionaryFontSize) {
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

void EpubReaderTouchMenuActivity::openPane(const ReaderDrawerPane pane) {
  state.pane = pane;
  state.paneTopIndex = 0;
  state.selectedIndex = 0;
  paneRows.clear();
  if (pane == ReaderDrawerPane::ReaderFont) paneRows = {RowId::FontFamily, RowId::FontSize};
  if (pane == ReaderDrawerPane::DictionaryFont) {
    paneRows = {RowId::DictionaryFontFamily, RowId::DictionaryFontSize};
  }
  requestUpdate();
}

void EpubReaderTouchMenuActivity::closePane() {
  if (state.pane == ReaderDrawerPane::Root) {
    closeAndReturn(true);
    return;
  }
  if (state.pane == ReaderDrawerPane::EnumOptions) {
    state.pane = enumOptionReturnPane;
  } else if (state.pane == ReaderDrawerPane::FontFamily) {
    state.pane = ReaderDrawerPane::ReaderFont;
  } else {
    state.pane = ReaderDrawerPane::Root;
  }
  paneRows.clear();
  if (state.pane == ReaderDrawerPane::ReaderFont) paneRows = {RowId::FontFamily, RowId::FontSize};
  if (state.pane == ReaderDrawerPane::DictionaryFont) {
    paneRows = {RowId::DictionaryFontFamily, RowId::DictionaryFontSize};
  }
  state.paneTopIndex = 0;
  state.selectedIndex = 0;
  requestUpdate();
}

void EpubReaderTouchMenuActivity::changeTab(const ReaderDrawerTab tab) {
  state.tab = tab;
  state.pane = ReaderDrawerPane::Root;
  state.selectedIndex = 0;
  requestUpdate();
}

void EpubReaderTouchMenuActivity::activateListIndex(const int index) {
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
      paneRows = {RowId::FontFamily, RowId::FontSize};
      state.pendingFontIndex = -1;
      state.selectedIndex = 0;
      requestUpdate();
      return;
    }
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
        sdFontSystem.ensureLoaded(renderer);
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

void EpubReaderTouchMenuActivity::activateRow(const RowId row) {
  switch (row) {
    case RowId::ReaderFont:
      openPane(ReaderDrawerPane::ReaderFont);
      return;
    case RowId::FontFamily:
      openPane(ReaderDrawerPane::FontFamily);
      return;
    case RowId::FontSize:
    case RowId::DictionaryFontFamily:
    case RowId::DictionaryFontSize:
    case RowId::Orientation:
    case RowId::Alignment:
    case RowId::Images:
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
      closeAndReturn(false, EpubReaderMenuAction::SELECT_CHAPTER, false);
      return;
    case RowId::GoToPercent:
      openPane(ReaderDrawerPane::Percent);
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
      commitSettings();
      if (auto statusBar =
              makeUniqueNoThrow<StatusBarSettingsActivity>(renderer, mappedInput, true, stablePageNumbersAvailable)) {
        if (beginGlobalSettingsEditCallback) beginGlobalSettingsEditCallback(beginGlobalSettingsEditContext);
        startActivityForResult(std::move(statusBar), [this](const ActivityResult&) {
          if (saveGlobalSettingsCallback) saveGlobalSettingsCallback(saveGlobalSettingsContext);
          if (endGlobalSettingsEditCallback) endGlobalSettingsEditCallback(endGlobalSettingsEditContext);
          requestUpdate();
        });
      }
      return;
    case RowId::Controls:
      commitSettings();
      if (auto controls = makeUniqueNoThrow<ControlsOptionsActivity>(renderer, mappedInput)) {
        startActivityForResult(std::move(controls), [this](const ActivityResult&) { requestUpdate(); });
      }
      return;
    case RowId::TextAa:
    case RowId::Bionic:
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
      closeAndReturn(false, EpubReaderMenuAction::BOOKMARK_TOGGLE, false);
      return;
    case RowId::ToggleCompleted:
      isBookCompleted = !isBookCompleted;
      closeAndReturn(false, EpubReaderMenuAction::TOGGLE_COMPLETED);
      return;
    case RowId::DeleteBookmarks:
      showDestructiveConfirmation(row, EpubReaderMenuAction::DELETE_BOOKMARKS);
      return;
    case RowId::DeleteCache:
      showDestructiveConfirmation(row, EpubReaderMenuAction::DELETE_CACHE);
      return;
    case RowId::DeleteStats:
      showDestructiveConfirmation(row, EpubReaderMenuAction::DELETE_STATS);
      return;
    default:
      break;
  }

  EpubReaderMenuAction action = EpubReaderMenuAction::GO_HOME;
  switch (row) {
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
  closeAndReturn(false, action, false);
}

void EpubReaderTouchMenuActivity::showDestructiveConfirmation(const RowId row, const EpubReaderMenuAction action) {
  static constexpr std::array<StrId, 2> OPTIONS = {StrId::STR_NO, StrId::STR_YES};
  StrId title = StrId::STR_DELETE;
  if (row == RowId::DeleteBookmarks) title = StrId::STR_DELETE_BOOKMARKS;
  if (row == RowId::DeleteCache) title = StrId::STR_DELETE_CACHE;
  if (row == RowId::DeleteStats) title = StrId::STR_DELETE_BOOK_STATS;
  optionPopup.show(title, OPTIONS.data(), OPTIONS.size(), 0, [this, action](const int selected) {
    if (selected == 1) {
      closeAndReturn(false, action, false);
      return;
    }
    requestUpdate();
  });
  requestUpdate();
}

void EpubReaderTouchMenuActivity::toggleSetting(const RowId row) {
  switch (row) {
    case RowId::TextAa:
      draft.textAntiAliasing = !draft.textAntiAliasing;
      break;
    case RowId::Bionic:
      draft.bionicReadingEnabled = !draft.bionicReadingEnabled;
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
    const bool previews = row == RowId::Bionic || row == RowId::GuideDots;
    markSettingChanged(previews ? ReaderSettingsChangeMask::Preview | ReaderSettingsChangeMask::Relayout
                                : ReaderSettingsChangeMask::Relayout);
  }
  requestUpdate();
}

void EpubReaderTouchMenuActivity::showEnumOptions(const RowId row) {
  std::vector<std::string> labels;
  std::vector<uint8_t> raw;
  StrId title = StrId::STR_NONE_OPT;
  uint8_t currentRaw = 0;

  if (row == RowId::FontSize) {
    if (draft.sdFontFamilyName[0] != '\0') {
      if (const auto* family = sdFontSystem.registry().findFamily(draft.sdFontFamilyName.data())) {
        raw = family->availableSizes();
      }
    }
    if (raw.empty()) {
      static constexpr std::array<CrossPointSettings::FONT_SIZE, CrossPointSettings::FONT_SIZE_COUNT> BUILTIN_SIZES = {
          CrossPointSettings::TINY, CrossPointSettings::SMALL, CrossPointSettings::MEDIUM, CrossPointSettings::LARGE};
      raw.reserve(BUILTIN_SIZES.size());
      for (const auto size : BUILTIN_SIZES) {
        raw.push_back(CrossPointSettings::getReaderFontPointSize(size));
      }
    }
    labels.reserve(raw.size());
    for (const uint8_t size : raw) labels.push_back(fontSizePointLabel(size));
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
      labels = {tr(STR_JUSTIFY), tr(STR_ALIGN_LEFT), tr(STR_CENTER), tr(STR_ALIGN_RIGHT), tr(STR_BOOK_S_STYLE)};
      raw = {0, 1, 2, 3, 4};
      currentRaw = draft.paragraphAlignment;
      break;
    case RowId::Images:
      title = StrId::STR_IMAGES;
      labels = {tr(STR_IMAGES_DISPLAY), tr(STR_IMAGES_PLACEHOLDER), tr(STR_IMAGES_SUPPRESS)};
      raw = {0, 1, 2};
      currentRaw = draft.imageRendering;
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
          sdFontSystem.registry().findFamily(dictionaryFontFamilyName) == nullptr) {
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
      sdFontSystem.registry().findFamily(dictionaryFontFamilyName) == nullptr) {
    current = static_cast<int>(labels.size()) - 1;
  } else {
    const auto rawIt = std::find(raw.begin(), raw.end(), currentRaw);
    if (rawIt != raw.end()) current = static_cast<int>(std::distance(raw.begin(), rawIt));
  }
  openEnumOptions(row, title, std::move(labels), std::move(raw), current);
}

void EpubReaderTouchMenuActivity::openEnumOptions(const RowId row, const StrId title, std::vector<std::string> labels,
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
  state.paneTopIndex = 0;
  state.selectedIndex = 0;
  requestUpdate();
}

void EpubReaderTouchMenuActivity::notifyDictionaryFontChanged() {
  if (dictionaryFontChangedCallback) {
    dictionaryFontChangedCallback(dictionaryFontChangedContext,
                                  hasDictionaryFontOverride ? dictionaryFontFamilyName : nullptr,
                                  dictionaryFontPointSize);
  }
}

void EpubReaderTouchMenuActivity::selectEnumOption(const int index) {
  if (index < 0 || index >= static_cast<int>(enumOptionValues.size())) return;
  const uint8_t value = enumOptionValues[static_cast<size_t>(index)];
  const bool previewsBeforeSelecting = enumOptionRow == RowId::FontSize || enumOptionRow == RowId::Alignment;
  if (previewsBeforeSelecting && previewedEnumOptionIndex != index) {
    previewedEnumOptionIndex = static_cast<int16_t>(index);
    if (enumOptionRow == RowId::FontSize) {
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
    case RowId::Images:
      draft.imageRendering = value;
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
    default:
      return;
  }
  state.pane = enumOptionReturnPane;
  state.paneTopIndex = 0;
  state.selectedIndex = 0;
  requestUpdate();
}

void EpubReaderTouchMenuActivity::completePercentSelection() {
  const bool changed = didChangeSettings;
  commitSettings();
  MenuResult menu{static_cast<int>(EpubReaderMenuAction::GO_TO_PERCENT), draft.orientation, changed};
  menu.drawerState = state;
  menu.changeMask = changeMask;
  menu.drawerValue = static_cast<int16_t>(percent);
  setResult(std::move(menu));
  finish();
}

void EpubReaderTouchMenuActivity::completeAutoPageTurnSelection() {
  const bool changed = didChangeSettings;
  commitSettings();
  MenuResult menu{static_cast<int>(EpubReaderMenuAction::AUTO_PAGE_TURN), draft.orientation, changed};
  menu.drawerState = state;
  menu.changeMask = changeMask;
  menu.drawerValue = static_cast<int16_t>(autoPageTurnIntervalSeconds);
  setResult(std::move(menu));
  finish();
}

void EpubReaderTouchMenuActivity::adjustActiveSlider(const int delta) {
  if (state.pane == ReaderDrawerPane::Spacing) {
    draft.lineHeightPercent = CrossPointSettings::clampedLineHeightPercent(static_cast<uint8_t>(
        std::clamp<int>(draft.lineHeightPercent + delta, CrossPointSettings::MIN_LINE_HEIGHT_PERCENT,
                        CrossPointSettings::MAX_LINE_HEIGHT_PERCENT)));
  } else if (state.pane == ReaderDrawerPane::Margins) {
    draft.screenMarginVertical =
        std::clamp<int>(draft.screenMarginVertical + delta, CrossPointSettings::MIN_SCREEN_MARGIN,
                        CrossPointSettings::MAX_SCREEN_MARGIN);
  } else if (state.pane == ReaderDrawerPane::Percent) {
    percent = std::clamp(percent + delta, 0, 100);
  } else if (state.pane == ReaderDrawerPane::AutoPageTurn) {
    autoPageTurnIntervalSeconds = static_cast<uint16_t>(std::clamp<int>(
        autoPageTurnIntervalSeconds + delta, READER_AUTO_PAGE_TURN_MIN_SECONDS, READER_AUTO_PAGE_TURN_MAX_SECONDS));
  }
}

void EpubReaderTouchMenuActivity::moveSelection(const bool forward, const bool page) {
  buttonFocusActive = true;
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
  state.selectedIndex = page ? (forward ? ButtonNavigator::nextPageIndex(state.selectedIndex, count, visibleRows)
                                        : ButtonNavigator::previousPageIndex(state.selectedIndex, count, visibleRows))
                             : (forward ? ButtonNavigator::nextIndex(state.selectedIndex, count)
                                        : ButtonNavigator::previousIndex(state.selectedIndex, count));
  const int top = followListSelection(state.selectedIndex, activeTopIndex(), visibleRows, count);
  if (state.pane == ReaderDrawerPane::Root) {
    state.rootTopIndex[static_cast<size_t>(state.tab)] = static_cast<int16_t>(top);
  } else {
    state.paneTopIndex = static_cast<int16_t>(top);
  }
  requestUpdate();
}

void EpubReaderTouchMenuActivity::scrollBy(const int delta) {
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

bool EpubReaderTouchMenuActivity::saveBookDictionary(const std::string& path) {
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

void EpubReaderTouchMenuActivity::renderPreviewContents(const ReaderSettingsDraft& previewSettings,
                                                        const int previewFontId) {
  const int previewTop = 0;
  const fui::SheetProps sheet;
  const int drawerHeight = readerDrawerHeight(renderer, state.pane) + DrawerHandle::bandHeight(sheet);
  const int previewBottom = renderer.getScreenHeight() - drawerHeight;
  const int previewHeight = std::max(0, previewBottom - previewTop);
  renderer.fillRect(0, previewTop, renderer.getScreenWidth(), previewHeight, ReaderUtils::readerDarkModeEnabled());
  renderPreviewText(previewSettings, previewFontId);
}

void EpubReaderTouchMenuActivity::renderPreviewText(const ReaderSettingsDraft& previewSettings,
                                                    const int previewFontId) {
  int orientedTop, orientedRight, orientedBottom, orientedLeft;
  renderer.getOrientedViewableTRBL(&orientedTop, &orientedRight, &orientedBottom, &orientedLeft);
  (void)orientedRight;
  (void)orientedBottom;
  (void)orientedLeft;
  const int clockReservation = ReaderUtils::getTopClockStatusBarReservedHeight(renderer);
  const int previewYOffset = orientedTop + std::max(static_cast<int>(previewSettings.screenMarginVertical),
                                                    clockReservation + ReaderUtils::TOP_CLOCK_TEXT_PADDING);
  const int previewWidth =
      std::max(1, renderer.getScreenWidth() - static_cast<int>(previewSettings.screenMarginHorizontal) * 2);
  previewModel->renderText(renderer, previewFontId, previewSettings.screenMarginHorizontal, previewYOffset,
                           previewWidth, previewSettings.lineHeightPercent, previewSettings.wordSpacing,
                           previewSettings.paragraphAlignment, previewSettings.bionicReadingEnabled,
                           previewSettings.guideReadingEnabled, ReaderUtils::readerForegroundBlack());
}

bool EpubReaderTouchMenuActivity::renderPreview() {
  if (!previewDirty || !previewModel || !previewModel->valid()) return false;
  previewDirty = false;
  const ReaderSettingsDraft previewSettings = draft;
  const ReaderSettingsDraft liveSettings = captureSettings();
  applySettings(previewSettings);
  sdFontSystem.ensureLoaded(renderer);
  const int previewFontId = SETTINGS.getReaderFontId();
  applySettings(liveSettings);
  if (auto* fontCacheManager = renderer.getFontCacheManager()) {
    auto scope = fontCacheManager->createPrewarmScope();
    renderPreviewContents(previewSettings,
                          previewFontId);  // Scan the page text before loading the selected font's glyphs.
    scope.endScanAndPrewarm();
  }
  renderPreviewContents(previewSettings, previewFontId);
  return true;
}

void EpubReaderTouchMenuActivity::renderPreviewWithAntiAliasing() {
  if (!previewModel || !previewModel->valid()) return;

  const ReaderSettingsDraft previewSettings = draft;
  const ReaderSettingsDraft liveSettings = captureSettings();
  applySettings(previewSettings);
  const int previewFontId = SETTINGS.getReaderFontId();
  applySettings(liveSettings);

  // The BW refresh has already drawn the reader preview and drawer. Re-render
  // only preview text into the grayscale planes so the drawer stays crisp.
  ReaderUtils::renderAntiAliased(
      renderer, [this, &previewSettings, previewFontId] { renderPreviewText(previewSettings, previewFontId); });
}

void EpubReaderTouchMenuActivity::loop() {
  if (mappedInput.wasBottomEdgeUpSwipe()) {
    closeAndReturn(false, EpubReaderMenuAction::GO_HOME, false);
    return;
  }
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (DrawerHandle::wasDismissSwipe(mappedInput, drawerHandleRect, fui::SheetEdge::Bottom)) {
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
    // List rows activate on release. Do not route their touch-down edge,
    // otherwise a swipe briefly paints the row where the finger landed as
    // pressed even though it never activates that row.
    if (!readerDrawerStepChangesSettings(state.pane) && state.pane != ReaderDrawerPane::Percent) {
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
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    buttonFocusActive = true;
    closePane();
    return;
  }
  buttonNavigator.onNextRelease([this] { moveSelection(true, false); });
  buttonNavigator.onPreviousRelease([this] { moveSelection(false, false); });
  buttonNavigator.onNextContinuous([this] {
    const int next = ButtonNavigator::nextIndex(static_cast<int>(state.tab), READER_DRAWER_TAB_COUNT);
    changeTab(static_cast<ReaderDrawerTab>(next));
    buttonFocusActive = true;
  });
  buttonNavigator.onPreviousContinuous([this] {
    const int previous = ButtonNavigator::previousIndex(static_cast<int>(state.tab), READER_DRAWER_TAB_COUNT);
    changeTab(static_cast<ReaderDrawerTab>(previous));
    buttonFocusActive = true;
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    buttonFocusActive = true;
    if (state.pane == ReaderDrawerPane::Percent) {
      completePercentSelection();
      return;
    }
    if (state.pane == ReaderDrawerPane::AutoPageTurn) {
      completeAutoPageTurnSelection();
      return;
    }
    activateListIndex(state.selectedIndex);
  }
}

void EpubReaderTouchMenuActivity::render(RenderLock&&) {
  if (renderPreview()) {
    previewHasAntiAliasing = draft.textAntiAliasing && ReaderUtils::readerForegroundBlack();
  }
  uiReady = false;
  app.setDevice(uiTarget.deviceContext());
  app.render();
  uiReady = true;
  if (optionPopup.isActive()) optionPopup.render(renderer);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  if (previewHasAntiAliasing) renderPreviewWithAntiAliasing();
}

const char* EpubReaderTouchMenuActivity::paneTitle() const {
  switch (state.pane) {
    case ReaderDrawerPane::ReaderFont:
      return tr(STR_READER_FONT);
    case ReaderDrawerPane::DictionaryFont:
      return tr(STR_DICTIONARY_FONT);
    case ReaderDrawerPane::FontFamily:
      return tr(STR_FONT_FAMILY);
    case ReaderDrawerPane::Spacing:
      return tr(STR_SPACING);
    case ReaderDrawerPane::Margins:
      return tr(STR_SCREEN_MARGIN);
    case ReaderDrawerPane::Percent:
      return tr(STR_GO_TO_PERCENT);
    case ReaderDrawerPane::AutoPageTurn:
      return tr(STR_AUTO_TURN_INTERVAL_SECONDS);
    case ReaderDrawerPane::Dictionary:
      return tr(STR_BOOK_DICTIONARY);
    case ReaderDrawerPane::EnumOptions:
      return I18N.get(enumOptionTitle);
    case ReaderDrawerPane::Chapters:
      return tr(STR_SELECT_CHAPTER);
    case ReaderDrawerPane::Root:
      return "";
  }
  return "";
}

const char* EpubReaderTouchMenuActivity::rowLabel(const RowId row) const {
  switch (row) {
    case RowId::ReaderFont:
      return tr(STR_READER_FONT);
    case RowId::DictionaryFont:
      return tr(STR_DICTIONARY_FONT);
    case RowId::Spacing:
      return tr(STR_SPACING);
    case RowId::TextAa:
      return tr(STR_TEXT_AA);
    case RowId::Bionic:
      return tr(STR_BIONIC_READING);
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
    case RowId::EmbeddedStyle:
      return tr(STR_EMBEDDED_STYLE);
    case RowId::Images:
      return tr(STR_IMAGES);
    case RowId::SelectChapter:
      return tr(STR_SELECT_CHAPTER);
    case RowId::GoToPercent:
      return tr(STR_GO_TO_PERCENT);
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
      return tr(STR_CUSTOMISE_STATUS_BAR);
    case RowId::BookDictionary:
      return tr(STR_BOOK_DICTIONARY);
    case RowId::RenderMode:
      return tr(STR_EPUB_RENDER_MODE);
    case RowId::IndexingMethod:
      return tr(STR_INDEXING_METHOD);
    case RowId::ToggleCompleted:
      return isBookCompleted ? tr(STR_MARK_UNFINISHED) : tr(STR_MARK_FINISHED);
    case RowId::Controls:
      return tr(STR_CAT_CONTROLS);
    case RowId::ResetReadingPace:
      return tr(STR_RESET_READING_PACE);
    case RowId::DeleteCache:
      return tr(STR_DELETE_CACHE);
    case RowId::DeleteStats:
      return tr(STR_DELETE_BOOK_STATS);
    case RowId::AutoPageTurn:
      return tr(STR_AUTO_TURN_INTERVAL_SECONDS);
    case RowId::FontFamily:
      return tr(STR_FONT_FAMILY);
    case RowId::FontSize:
      return tr(STR_FONT_SIZE);
    case RowId::DictionaryFontFamily:
      return tr(STR_FONT_FAMILY);
    case RowId::DictionaryFontSize:
      return tr(STR_FONT_SIZE);
  }
  return "";
}

const char* EpubReaderTouchMenuActivity::rowValue(const RowId row, char* buffer, const size_t bufferSize) const {
  switch (row) {
    case RowId::FontFamily: {
      if (draft.sdFontFamilyName[0] != '\0') return draft.sdFontFamilyName.data();
      static constexpr std::array<StrId, CrossPointSettings::BUILTIN_FONT_COUNT> labels = {StrId::STR_LEXEND_DECA,
                                                                                           StrId::STR_BITTER};
      if (draft.fontFamily >= labels.size()) return tr(STR_UNAVAILABLE);
      return I18N.get(labels[draft.fontFamily]);
    }
    case RowId::FontSize:
      std::snprintf(buffer, bufferSize, "%upt", draft.readerFontPointSize);
      return buffer;
    case RowId::Orientation: {
      return I18N.get(readerOrientationLabel(draft.orientation));
    }
    case RowId::Alignment: {
      static const std::array<StrId, 5> labels = {StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER,
                                                  StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE};
      return I18N.get(labels[std::min<size_t>(draft.paragraphAlignment, labels.size() - 1)]);
    }
    case RowId::Images: {
      static const std::array<StrId, 3> labels = {StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER,
                                                  StrId::STR_IMAGES_SUPPRESS};
      return I18N.get(labels[std::min<size_t>(draft.imageRendering, labels.size() - 1)]);
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

bool EpubReaderTouchMenuActivity::rowIsToggle(const RowId row) const {
  switch (row) {
    case RowId::TextAa:
    case RowId::Bionic:
    case RowId::GuideDots:
    case RowId::Hyphenation:
    case RowId::PublisherPages:
    case RowId::ExtraSpacing:
    case RowId::ForceIndents:
    case RowId::EmbeddedStyle:
      return true;
    default:
      return false;
  }
}

bool EpubReaderTouchMenuActivity::rowShowsNavigationCaret(const RowId row) const {
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
    case RowId::DeleteBookmarks:
    case RowId::DeleteCache:
    case RowId::DeleteStats:
      return false;
    default:
      return true;
  }
}

bool EpubReaderTouchMenuActivity::rowToggleValue(const RowId row) const {
  switch (row) {
    case RowId::TextAa:
      return draft.textAntiAliasing;
    case RowId::Bionic:
      return draft.bionicReadingEnabled;
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
    case RowId::EmbeddedStyle:
      return draft.embeddedStyle;
    default:
      return false;
  }
}

#endif
