#include "StatusBarSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
enum MenuItem {
  ITEM_CHAPTER_PAGE_COUNT = 0,
  ITEM_STABLE_PAGE_NUMBERS,
  ITEM_BOOK_PROGRESS_PERCENTAGE,
  ITEM_BOOK_PERCENTAGE_FORMAT,
  ITEM_PROGRESS_BAR,
  ITEM_PROGRESS_BAR_THICKNESS,
  ITEM_TITLE,
  ITEM_TIME_LEFT,
  ITEM_BATTERY,
  ITEM_XTC_STATUS_BAR,
  ITEM_COUNT
};

const StrId menuNames[ITEM_COUNT] = {
    StrId::STR_CHAPTER_PAGE_COUNT,
    StrId::STR_STABLE_PAGE_NUMBERS,
    StrId::STR_BOOK_PROGRESS_PERCENTAGE,
    StrId::STR_PERCENTAGE_FORMAT,
    StrId::STR_PROGRESS_BAR,
    StrId::STR_PROGRESS_BAR_THICKNESS,
    StrId::STR_TITLE,
    StrId::STR_TIME_LEFT,
    StrId::STR_BATTERY,
    StrId::STR_XTC_STATUS_BAR,
};

const StrId percentageFormatNames[CrossPointSettings::BOOK_PERCENTAGE_FORMAT_COUNT] = {
    StrId::STR_PERCENTAGE_FORMAT_WHOLE,
    StrId::STR_PERCENTAGE_FORMAT_ONE_DECIMAL,
    StrId::STR_PERCENTAGE_FORMAT_TWO_DECIMALS,
};

constexpr int PROGRESS_BAR_ITEMS = 3;
const StrId progressBarNames[PROGRESS_BAR_ITEMS] = {StrId::STR_HIDE, StrId::STR_BOOK, StrId::STR_CHAPTER};
const uint8_t progressBarRawValues[PROGRESS_BAR_ITEMS] = {
    CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS,
    CrossPointSettings::STATUS_BAR_PROGRESS_BAR::BOOK_PROGRESS,
    CrossPointSettings::STATUS_BAR_PROGRESS_BAR::CHAPTER_PROGRESS,
};

constexpr int PROGRESS_BAR_THICKNESS_ITEMS = 3;
const StrId progressBarThicknessNames[PROGRESS_BAR_THICKNESS_ITEMS] = {
    StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK};

constexpr int TITLE_ITEMS = 3;
const StrId titleNames[TITLE_ITEMS] = {StrId::STR_HIDE, StrId::STR_BOOK, StrId::STR_CHAPTER};
const uint8_t titleRawValues[TITLE_ITEMS] = {
    CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE,
    CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE,
    CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE,
};

constexpr int TIME_LEFT_ITEMS = 3;
const StrId timeLeftNames[TIME_LEFT_ITEMS] = {StrId::STR_HIDE, StrId::STR_CHAPTER, StrId::STR_BOOK};

constexpr int XTC_STATUS_BAR_ITEMS = 3;
const StrId xtcStatusBarNames[XTC_STATUS_BAR_ITEMS] = {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP};

int optionCountForItem(const int item) {
  switch (item) {
    case ITEM_BOOK_PERCENTAGE_FORMAT:
      return CrossPointSettings::BOOK_PERCENTAGE_FORMAT_COUNT;
    case ITEM_PROGRESS_BAR:
      return PROGRESS_BAR_ITEMS;
    case ITEM_PROGRESS_BAR_THICKNESS:
      return PROGRESS_BAR_THICKNESS_ITEMS;
    case ITEM_TITLE:
      return TITLE_ITEMS;
    case ITEM_TIME_LEFT:
      return TIME_LEFT_ITEMS;
    case ITEM_XTC_STATUS_BAR:
      return XTC_STATUS_BAR_ITEMS;
    default:
      return 0;
  }
}

const char* optionNameForItem(const int item, const int optionIndex) {
  switch (item) {
    case ITEM_BOOK_PERCENTAGE_FORMAT:
      return I18N.get(percentageFormatNames[optionIndex]);
    case ITEM_PROGRESS_BAR:
      return I18N.get(progressBarNames[optionIndex]);
    case ITEM_PROGRESS_BAR_THICKNESS:
      return I18N.get(progressBarThicknessNames[optionIndex]);
    case ITEM_TITLE:
      return I18N.get(titleNames[optionIndex]);
    case ITEM_TIME_LEFT:
      return I18N.get(timeLeftNames[optionIndex]);
    case ITEM_XTC_STATUS_BAR:
      return I18N.get(xtcStatusBarNames[optionIndex]);
    default:
      return tr(STR_NONE_OPT);
  }
}

uint8_t optionRawValueForItem(const int item, const int optionIndex) {
  switch (item) {
    case ITEM_PROGRESS_BAR:
      return progressBarRawValues[optionIndex];
    case ITEM_TITLE:
      return titleRawValues[optionIndex];
    default:
      return static_cast<uint8_t>(optionIndex);
  }
}

uint8_t currentOptionIndexForItem(const int item) {
  uint8_t rawValue = 0;
  switch (item) {
    case ITEM_BOOK_PERCENTAGE_FORMAT:
      rawValue = SETTINGS.statusBarBookPercentageFormat;
      break;
    case ITEM_PROGRESS_BAR:
      rawValue = SETTINGS.statusBarProgressBar;
      break;
    case ITEM_PROGRESS_BAR_THICKNESS:
      rawValue = SETTINGS.statusBarProgressBarThickness;
      break;
    case ITEM_TITLE:
      rawValue = SETTINGS.statusBarTitle;
      break;
    case ITEM_TIME_LEFT:
      rawValue = SETTINGS.statusBarTimeLeft;
      break;
    case ITEM_XTC_STATUS_BAR:
      rawValue = SETTINGS.xtcStatusBarMode;
      break;
    default:
      return 0;
  }

  const int optionCount = optionCountForItem(item);
  for (int i = 0; i < optionCount; i++) {
    if (optionRawValueForItem(item, i) == rawValue) return static_cast<uint8_t>(i);
  }
  return 0;
}

void setOptionIndexForItem(const int item, const uint8_t optionIndex) {
  const uint8_t rawValue = optionRawValueForItem(item, optionIndex);
  switch (item) {
    case ITEM_BOOK_PERCENTAGE_FORMAT:
      SETTINGS.statusBarBookPercentageFormat = rawValue;
      break;
    case ITEM_PROGRESS_BAR:
      SETTINGS.statusBarProgressBar = rawValue;
      break;
    case ITEM_PROGRESS_BAR_THICKNESS:
      SETTINGS.statusBarProgressBarThickness = rawValue;
      break;
    case ITEM_TITLE:
      SETTINGS.statusBarTitle = rawValue;
      break;
    case ITEM_TIME_LEFT:
      SETTINGS.statusBarTimeLeft = rawValue;
      break;
    case ITEM_XTC_STATUS_BAR:
      SETTINGS.xtcStatusBarMode = rawValue;
      break;
    default:
      break;
  }
}

std::string valueTextForItem(const int item) {
  switch (item) {
    case ITEM_CHAPTER_PAGE_COUNT:
      return SETTINGS.statusBarChapterPageCount ? tr(STR_SHOW) : tr(STR_HIDE);
    case ITEM_STABLE_PAGE_NUMBERS:
      return SETTINGS.stablePageNumbers ? tr(STR_SHOW) : tr(STR_HIDE);
    case ITEM_BOOK_PROGRESS_PERCENTAGE:
      return SETTINGS.statusBarBookProgressPercentage ? tr(STR_SHOW) : tr(STR_HIDE);
    case ITEM_BATTERY:
      return SETTINGS.statusBarBattery ? tr(STR_SHOW) : tr(STR_HIDE);
    default: {
      const int optionCount = optionCountForItem(item);
      const uint8_t optionIndex = currentOptionIndexForItem(item);
      if (optionCount == 0 || optionIndex >= optionCount) return tr(STR_HIDE);
      return optionNameForItem(item, optionIndex);
    }
  }
}

}  // namespace

void StatusBarSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  visibleItemCount = stablePageNumbersAvailable ? ITEM_COUNT : ITEM_COUNT - 1;
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &StatusBarSettingsActivity::onRowEvent, this);
  app.setScreen(&StatusBarSettingsActivity::settingsScreen, this);

  if (SETTINGS.statusBarBookPercentageFormat >= CrossPointSettings::BOOK_PERCENTAGE_FORMAT_COUNT) {
    SETTINGS.statusBarBookPercentageFormat = CrossPointSettings::BOOK_PERCENTAGE_WHOLE;
  }

  // Clamp statusBarProgressBar and statusBarTitle in case of corrupt/migrated data
  if (SETTINGS.statusBarProgressBar >= PROGRESS_BAR_ITEMS) {
    SETTINGS.statusBarProgressBar = CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  }

  if (SETTINGS.statusBarProgressBarThickness >= PROGRESS_BAR_THICKNESS_ITEMS) {
    SETTINGS.statusBarProgressBarThickness = CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS::PROGRESS_BAR_NORMAL;
  }

  if (SETTINGS.statusBarTitle >= TITLE_ITEMS) {
    SETTINGS.statusBarTitle = CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE;
  }

  if (SETTINGS.statusBarTimeLeft >= TIME_LEFT_ITEMS) {
    SETTINGS.statusBarTimeLeft = CrossPointSettings::STATUS_BAR_TIME_LEFT::TIME_LEFT_HIDE;
  }

  if (SETTINGS.xtcStatusBarMode >= XTC_STATUS_BAR_ITEMS) {
    SETTINGS.xtcStatusBarMode = CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_HIDE;
  }

  requestUpdate();
}

void StatusBarSettingsActivity::onExit() { Activity::onExit(); }

bool StatusBarSettingsActivity::handleHomeGesture() {
  finish();
  return true;
}

void StatusBarSettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finishAfterBackPress();
    return;
  }

  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  if (mappedInput.hasTouch()) {
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
      const int next = scrollListBy(topIndex, delta, visibleRows, visibleItemCount);
      if (next != topIndex) {
        topIndex = next;
        requestUpdate();
      }
      return;
    }
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    requestUpdate();
    return;
  }

  // Handle navigation
  buttonNavigator.onNextRelease([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, visibleItemCount);
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, visibleItemCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, visibleItemCount);
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, visibleItemCount);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, visibleItemCount);
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, visibleItemCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, visibleItemCount);
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, visibleItemCount);
    requestUpdate();
  });
}

int StatusBarSettingsActivity::itemForVisibleIndex(const int visibleIndex) const {
  return !stablePageNumbersAvailable && visibleIndex >= ITEM_STABLE_PAGE_NUMBERS ? visibleIndex + 1 : visibleIndex;
}

bool StatusBarSettingsActivity::selectedItemUsesOptionMenu() const {
  return optionCountForItem(itemForVisibleIndex(selectedIndex)) > 2;
}

void StatusBarSettingsActivity::handleSelection() {
  const int item = itemForVisibleIndex(selectedIndex);
  if (selectedItemUsesOptionMenu()) {
    openOptionPicker();
    return;
  }

  switch (item) {
    case ITEM_CHAPTER_PAGE_COUNT:
      SETTINGS.statusBarChapterPageCount = (SETTINGS.statusBarChapterPageCount + 1) % 2;
      break;
    case ITEM_STABLE_PAGE_NUMBERS:
      SETTINGS.stablePageNumbers = (SETTINGS.stablePageNumbers + 1) % 2;
      break;
    case ITEM_BOOK_PROGRESS_PERCENTAGE:
      SETTINGS.statusBarBookProgressPercentage = (SETTINGS.statusBarBookProgressPercentage + 1) % 2;
      break;
    case ITEM_BATTERY:
      SETTINGS.statusBarBattery = (SETTINGS.statusBarBattery + 1) % 2;
      break;
    default:
      return;
  }
  SETTINGS.saveToFile();
}

void StatusBarSettingsActivity::openOptionPicker() {
  const int item = itemForVisibleIndex(selectedIndex);
  const int optionCount = optionCountForItem(item);
  if (optionCount <= 0) return;

  std::vector<std::string> options;
  options.reserve(optionCount);
  for (int i = 0; i < optionCount; i++) {
    options.push_back(optionNameForItem(item, i));
  }

  uint8_t currentIndex = currentOptionIndexForItem(item);
  if (currentIndex >= optionCount) currentIndex = 0;

  optionPopup.show(menuNames[item], options, currentIndex, [this, item](int selectedIndex) {
    setOptionIndexForItem(item, static_cast<uint8_t>(selectedIndex));
    SETTINGS.saveToFile();
    requestUpdate();
  });
  requestUpdate();
}

void StatusBarSettingsActivity::settingsScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<StatusBarSettingsActivity*>(user)->buildSettingsScreen(screen);
}

void StatusBarSettingsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<StatusBarSettingsActivity*>(user);
  if (self->optionPopup.isActive() || event.value < 0 || event.value >= self->visibleItemCount) return;
  self->selectedIndex = event.value;
  self->app.clearTapFlash();
  self->handleSelection();
}

void StatusBarSettingsActivity::buildSettingsScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto orientation = renderer.getOrientation();
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.buttonHintsHeight : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;
  const int contentTop =
      metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput) + metrics.verticalSpacing;
  const int previewLabelLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  constexpr int previewLabelGap = 18;
  const int previewSectionHeight = previewLabelLineHeight + previewLabelGap + UITheme::getStatusBarHeight();
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - previewSectionHeight - metrics.verticalSpacing * 2;

  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(contentTop), static_cast<int16_t>(pageWidth - (contentX + contentWidth)),
                  static_cast<int16_t>(pageHeight - (contentTop + contentHeight)), static_cast<int16_t>(contentX)});

  std::vector<std::string> values(visibleItemCount);
  std::vector<fui::ListItem> items;
  items.reserve(visibleItemCount);
  for (int i = 0; i < visibleItemCount; ++i) {
    const int itemIndex = itemForVisibleIndex(i);
    values[i] = valueTextForItem(itemIndex);
    fui::ListItem item;
    item.label = I18N.get(menuNames[itemIndex]);
    item.value = values[i].c_str();
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 2;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, visibleItemCount);
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void StatusBarSettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const auto orientation = renderer.getOrientation();
  const bool isInverted = orientation == GfxRenderer::Orientation::PortraitInverted;
  const bool isLandscapeCw = orientation == GfxRenderer::Orientation::LandscapeClockwise;
  const bool isLandscapeCcw = orientation == GfxRenderer::Orientation::LandscapeCounterClockwise;
  const int hintGutterWidth = (isLandscapeCw || isLandscapeCcw) ? metrics.buttonHintsHeight : 0;
  const int contentX = isLandscapeCw ? hintGutterWidth : 0;
  const int contentWidth = pageWidth - hintGutterWidth;

  const int previewLabelLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  constexpr int previewLabelGap = 18;

  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)),
                                            selectedItemUsesOptionMenu() ? tr(STR_SELECT) : tr(STR_TOGGLE),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  int bottomPreviewPadding = metrics.buttonHintsHeight + metrics.verticalSpacing;

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_CUSTOMISE_STATUS_BAR), readerContext);
  } else {
    GUI.drawHeader(
        renderer, Rect{contentX, metrics.topPadding, contentWidth, TouchHeaderBackButton::height(metrics, mappedInput)},
        tr(STR_CUSTOMISE_STATUS_BAR), nullptr, readerContext);
  }

  uiReady = false;
  app.render();
  uiReady = true;
  // Draw button hints
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  std::string title;
  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = tr(STR_EXAMPLE_BOOK);
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_EXAMPLE_CHAPTER);
  }

  if (isLandscapeCw || isLandscapeCcw || isInverted) {
    bottomPreviewPadding = 0;
  }

  const char* timeLeftPreview = nullptr;
  if (SETTINGS.statusBarTimeLeft == CrossPointSettings::STATUS_BAR_TIME_LEFT::TIME_LEFT_CHAPTER) {
    timeLeftPreview = "1h 20m";
  } else if (SETTINGS.statusBarTimeLeft == CrossPointSettings::STATUS_BAR_TIME_LEFT::TIME_LEFT_BOOK) {
    timeLeftPreview = "3h 40m";
  }
  const int previewX = contentX + metrics.contentSidePadding;
  const int bottomPreviewTop = pageHeight - UITheme::getStatusBarHeight() - bottomPreviewPadding;
  const int previewLabelY = bottomPreviewTop - previewLabelLineHeight - previewLabelGap;

  renderer.drawText(UI_10_FONT_ID, previewX, previewLabelY, tr(STR_PREVIEW));
  GUI.drawStatusBar(renderer, 75.12f, 8, 32, title.c_str(), bottomPreviewPadding, 0, false, timeLeftPreview, false,
                    -1.0f, stablePageNumbersAvailable ? 120 : 0, stablePageNumbersAvailable ? 540 : 0);

  renderer.displayBuffer();
}
