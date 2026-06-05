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
#include "activities/util/OptionSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
enum MenuItem {
  ITEM_CHAPTER_PAGE_COUNT = 0,
  ITEM_BOOK_PROGRESS_PERCENTAGE,
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
    StrId::STR_BOOK_PROGRESS_PERCENTAGE,
    StrId::STR_PROGRESS_BAR,
    StrId::STR_PROGRESS_BAR_THICKNESS,
    StrId::STR_TITLE,
    StrId::STR_TIME_LEFT,
    StrId::STR_BATTERY,
    StrId::STR_XTC_STATUS_BAR,
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

StrId optionNameForItem(const int item, const int optionIndex) {
  switch (item) {
    case ITEM_PROGRESS_BAR:
      return progressBarNames[optionIndex];
    case ITEM_PROGRESS_BAR_THICKNESS:
      return progressBarThicknessNames[optionIndex];
    case ITEM_TITLE:
      return titleNames[optionIndex];
    case ITEM_TIME_LEFT:
      return timeLeftNames[optionIndex];
    case ITEM_XTC_STATUS_BAR:
      return xtcStatusBarNames[optionIndex];
    default:
      return StrId::STR_NONE_OPT;
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
    case ITEM_BOOK_PROGRESS_PERCENTAGE:
      return SETTINGS.statusBarBookProgressPercentage ? tr(STR_SHOW) : tr(STR_HIDE);
    case ITEM_BATTERY:
      return SETTINGS.statusBarBattery ? tr(STR_SHOW) : tr(STR_HIDE);
    default: {
      const int optionCount = optionCountForItem(item);
      const uint8_t optionIndex = currentOptionIndexForItem(item);
      if (optionCount == 0 || optionIndex >= optionCount) return tr(STR_HIDE);
      return I18N.get(optionNameForItem(item, optionIndex));
    }
  }
}

}  // namespace

void StatusBarSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  visibleItemCount = ITEM_COUNT;

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

void StatusBarSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
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
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, visibleItemCount);
    requestUpdate();
  });

  buttonNavigator.onNextContinuous([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, visibleItemCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousContinuous([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, visibleItemCount);
    requestUpdate();
  });
}

bool StatusBarSettingsActivity::selectedItemUsesOptionMenu() const { return optionCountForItem(selectedIndex) > 2; }

void StatusBarSettingsActivity::handleSelection() {
  if (selectedItemUsesOptionMenu()) {
    openOptionPicker();
    return;
  }

  switch (selectedIndex) {
    case ITEM_CHAPTER_PAGE_COUNT:
      SETTINGS.statusBarChapterPageCount = (SETTINGS.statusBarChapterPageCount + 1) % 2;
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
  const int item = selectedIndex;
  const int optionCount = optionCountForItem(item);
  if (optionCount <= 0) return;

  std::vector<std::string> options;
  options.reserve(optionCount);
  for (int i = 0; i < optionCount; i++) {
    options.push_back(I18N.get(optionNameForItem(item, i)));
  }

  uint8_t currentIndex = currentOptionIndexForItem(item);
  if (currentIndex >= optionCount) currentIndex = 0;

  startActivityForResult(
      std::make_unique<OptionSelectionActivity>(renderer, mappedInput, "StatusBarOptionSelect", menuNames[item],
                                                std::move(options), currentIndex, readerContext),
      [this, item](const ActivityResult& result) {
        if (result.isCancelled) {
          requestUpdate();
          return;
        }

        const auto* selection = std::get_if<OptionSelectionResult>(&result.data);
        if (selection != nullptr) {
          setOptionIndexForItem(item, selection->index);
          SETTINGS.saveToFile();
        }
        requestUpdate();
      });
}

void StatusBarSettingsActivity::render(RenderLock&&) {
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

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int previewLabelLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  constexpr int previewLabelGap = 18;
  const int previewStatusBarHeight = UITheme::getStatusBarHeight();
  const int previewSectionHeight = previewLabelLineHeight + previewLabelGap + previewStatusBarHeight;
  const int contentHeight =
      pageHeight - contentTop - metrics.buttonHintsHeight - previewSectionHeight - metrics.verticalSpacing * 2;

  const auto labels = mappedInput.mapLabels(
      tr(STR_BACK), selectedItemUsesOptionMenu() ? tr(STR_SELECT) : tr(STR_TOGGLE), tr(STR_DIR_UP), tr(STR_DIR_DOWN));

  int bottomPreviewPadding = metrics.buttonHintsHeight + metrics.verticalSpacing;

  GUI.drawHeader(renderer, Rect{contentX, metrics.topPadding, contentWidth, metrics.headerHeight},
                 tr(STR_CUSTOMISE_STATUS_BAR), nullptr, readerContext);

  GUI.drawList(
      renderer, Rect{contentX, contentTop, contentWidth, contentHeight}, visibleItemCount,
      static_cast<int>(selectedIndex), [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr,
      nullptr, [](int index) -> std::string { return valueTextForItem(index); }, true);
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
  GUI.drawStatusBar(renderer, 75, 8, 32, title, bottomPreviewPadding, 0, false, timeLeftPreview);

  renderer.displayBuffer();
}
