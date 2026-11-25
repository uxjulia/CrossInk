#include "FontSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/reader/TouchReaderPreviewModel.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/FontFamilyLabel.h"

namespace fui = freeink::ui;
namespace {
constexpr uint8_t INVALID_STORED_FONT_SIZE = 0xFF;
constexpr const char* ELLIPSIS_UTF8 = "\xe2\x80\xa6";
constexpr fui::ActionId ACTION_ROW = 1;

uint8_t closestSizeIndex(const std::vector<uint8_t>& sizes, const uint8_t targetPointSize) {
  if (sizes.empty()) return 0;

  uint8_t bestIndex = 0;
  uint8_t bestDiff = UINT8_MAX;
  for (size_t i = 0; i < sizes.size(); i++) {
    const uint8_t size = sizes[i];
    const uint8_t diff = size > targetPointSize ? size - targetPointSize : targetPointSize - size;
    if (diff < bestDiff || (diff == bestDiff && size < sizes[bestIndex])) {
      bestIndex = static_cast<uint8_t>(i);
      bestDiff = diff;
    }
  }
  return bestIndex;
}

uint8_t currentFontPointSize(const SdCardFontRegistry* registry) {
  (void)registry;
  return SETTINGS.readerFontPointSize;
}

void queueFontIntegrityAlert() {
  std::snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s", tr(STR_ERROR_MSG));
  std::snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), "%s", tr(STR_FONT_DATA_UNREADABLE));
  APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
  APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
}

int findCurrentFontIndex(const SdCardFontRegistry* registry, const char* sdFontFamilyName, uint8_t fontFamily) {
  if (sdFontFamilyName[0] != '\0' && registry) {
    const auto& families = registry->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      if (families[i].name == sdFontFamilyName) {
        return CrossPointSettings::BUILTIN_FONT_COUNT + i;
      }
    }
  }

  return fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? fontFamily : 0;
}
}  // namespace

FontSelectionActivity::FontSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const SdCardFontRegistry* registry)
    : Activity("FontSelect", renderer, mappedInput),
      registry_(registry),
      uiTarget_(makeUiTarget(renderer)),
      app_(uiTarget_, uiTarget_.deviceContext()) {}

void FontSelectionActivity::onEnter() {
  Activity::onEnter();
  if (registry_ == &sdFontSystem.registry()) {
    RenderLock lock;
    GUI.drawPopup(renderer, tr(STR_LOADING), true);
    sdFontSystem.refreshIfDirty();
    sdFontSystem.ensureLoaded(renderer);
  }

  updateLayoutMetrics();

  originalFontFamily_ = SETTINGS.fontFamily;
  strncpy(originalSdFontFamilyName_, SETTINGS.sdFontFamilyName, sizeof(originalSdFontFamilyName_) - 1);
  originalSdFontFamilyName_[sizeof(originalSdFontFamilyName_) - 1] = '\0';

  fonts_.clear();
  fonts_.reserve(CrossPointSettings::BUILTIN_FONT_COUNT + (registry_ ? registry_->getFamilyCount() : 0));

  constexpr auto builtinRange = BUILTIN_FONT_POINT_SIZE_RANGE;
  fonts_.push_back({fontFamilyLabel(I18N.get(StrId::STR_LEXEND_DECA), builtinRange), true, 0});
  fonts_.push_back({fontFamilyLabel(I18N.get(StrId::STR_BITTER), builtinRange), true, 1});

  if (registry_) {
    const auto& families = registry_->getFamilies();
    for (int i = 0; i < static_cast<int>(families.size()); i++) {
      fonts_.push_back({fontFamilyLabel(families[i].name, fontFamilyPointSizeRange(families[i])), false,
                        static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i)});
    }
  }

  selectedIndex_ = findCurrentFontIndex(registry_, SETTINGS.sdFontFamilyName, SETTINGS.fontFamily);
  previewFontIndex_ = selectedIndex_;
  topIndex_ = 0;
  visibleRows_ = 1;
  initialViewportPending_ = true;
  uiReady_ = false;
  applySharedUiTheme(app_, uiTarget_);
  app_.on(ACTION_ROW, &FontSelectionActivity::onRowEvent, this);
  app_.setScreen(&FontSelectionActivity::listScreen, this);

  requestUpdate();
}

void FontSelectionActivity::onExit() { Activity::onExit(); }

void FontSelectionActivity::activateSelected() {
  if (selectedIndex_ == previewFontIndex_) {
    handleSelection();
    return;
  }
  RenderLock lock;
  GUI.drawPopup(renderer, tr(STR_LOADING), true);
  const uint8_t previousFamily = SETTINGS.fontFamily;
  char previousSdFamily[sizeof(SETTINGS.sdFontFamilyName)];
  std::memcpy(previousSdFamily, SETTINGS.sdFontFamilyName, sizeof(previousSdFamily));
  const auto& font = fonts_[selectedIndex_];
  if (font.isBuiltin) {
    SETTINGS.fontFamily = font.settingIndex;
    SETTINGS.sdFontFamilyName[0] = '\0';
    sdFontSystem.ensureLoaded(renderer);
  } else if (registry_) {
    const int sdIdx = font.settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT;
    const auto& families = registry_->getFamilies();
    if (sdIdx < static_cast<int>(families.size())) {
      strncpy(SETTINGS.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      sdFontSystem.ensureLoaded(renderer);
      const bool integrityWarning = sdFontSystem.lastLoadHadIntegrityWarning();
      if (sdFontSystem.resolveFontId(SETTINGS.sdFontFamilyName, SETTINGS.readerFontPointSize) == 0) {
        LOG_ERR("FONT", "Cannot preview SD font: %s", SETTINGS.sdFontFamilyName);
        SETTINGS.fontFamily = previousFamily;
        std::memcpy(SETTINGS.sdFontFamilyName, previousSdFamily, sizeof(previousSdFamily));
        sdFontSystem.ensureLoaded(renderer);
        if (integrityWarning && integrityAlertFontIndex_ != selectedIndex_) {
          integrityAlertFontIndex_ = selectedIndex_;
          queueFontIntegrityAlert();
        }
        requestUpdate();
        return;
      }
      if (integrityWarning && integrityAlertFontIndex_ != selectedIndex_) {
        integrityAlertFontIndex_ = selectedIndex_;
        queueFontIntegrityAlert();
      }
    }
  }
  previewFontIndex_ = selectedIndex_;
  requestUpdate();
}

void FontSelectionActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FontSelectionActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->fonts_.size())) return;
  self->selectedIndex_ = event.value;
  self->app_.clearTapFlash();
  self->activateSelected();
}

void FontSelectionActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    mappedInput.suppressNextBackRelease();
    SETTINGS.fontFamily = originalFontFamily_;
    strncpy(SETTINGS.sdFontFamilyName, originalSdFontFamilyName_, sizeof(SETTINGS.sdFontFamilyName) - 1);
    SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
    sdFontSystem.ensureLoaded(renderer);
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }

  const int listSize = static_cast<int>(fonts_.size());
  if (uiReady_) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app_.route(snap);
      if (app_.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int next = scrollListBy(topIndex_, swipe == MappedInputManager::SwipeDir::Up ? visibleRows_ : -visibleRows_,
                                  visibleRows_, listSize);
    if (next != topIndex_) {
      topIndex_ = next;
      requestUpdate();
    }
    return;
  }

  const auto move = [this, listSize](const int next) {
    selectedIndex_ = next;
    topIndex_ = followListSelection(selectedIndex_, topIndex_, visibleRows_, listSize);
    requestUpdate();
  };
  buttonNavigator_.onNextRelease(
      [this, listSize, &move] { move(ButtonNavigator::nextIndex(selectedIndex_, listSize)); });
  buttonNavigator_.onPreviousRelease(
      [this, listSize, &move] { move(ButtonNavigator::previousIndex(selectedIndex_, listSize)); });
  buttonNavigator_.onNextContinuous(
      [this, listSize, &move] { move(ButtonNavigator::nextPageIndex(selectedIndex_, listSize, visibleRows_)); });
  buttonNavigator_.onPreviousContinuous(
      [this, listSize, &move] { move(ButtonNavigator::previousPageIndex(selectedIndex_, listSize, visibleRows_)); });
}

void FontSelectionActivity::handleSelection() {
  const auto& font = fonts_[selectedIndex_];
  const bool sameBuiltin =
      font.isBuiltin && originalSdFontFamilyName_[0] == '\0' && font.settingIndex == originalFontFamily_;
  const bool sameSdFamily = !font.isBuiltin && registry_ &&
                            registry_->getFamilies()[font.settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT].name ==
                                originalSdFontFamilyName_;
  if (sameBuiltin || sameSdFamily) {
    // Previewing another family and choosing the original one is still a no-op.
    // Avoid snapping its existing size to the closest available size.
    SETTINGS.fontFamily = originalFontFamily_;
    strncpy(SETTINGS.sdFontFamilyName, originalSdFontFamilyName_, sizeof(SETTINGS.sdFontFamilyName) - 1);
    SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
    mappedInput.suppressNextConfirmRelease();
    finish();
    return;
  }
  const uint8_t targetPointSize = currentFontPointSize(registry_);
  const uint8_t previousPointSize = SETTINGS.readerFontPointSize;
  char previousSdFamily[sizeof(SETTINGS.sdFontFamilyName)];
  std::memcpy(previousSdFamily, SETTINGS.sdFontFamilyName, sizeof(previousSdFamily));
  if (font.settingIndex < CrossPointSettings::BUILTIN_FONT_COUNT) {
    SETTINGS.fontFamily = font.settingIndex;
    SETTINGS.sdFontFamilyName[0] = '\0';
    SETTINGS.readerFontPointSize = closestBuiltinReaderPointSize(targetPointSize);
    RenderLock lock;
    sdFontSystem.ensureLoaded(renderer);
  } else if (registry_) {
    const int sdIdx = font.settingIndex - CrossPointSettings::BUILTIN_FONT_COUNT;
    const auto& families = registry_->getFamilies();
    if (sdIdx < static_cast<int>(families.size())) {
      const std::vector<uint8_t> sizes = families[sdIdx].availableSizes();
      if (sizes.empty()) return;
      SETTINGS.readerFontPointSize = sizes[closestSizeIndex(sizes, targetPointSize)];
      strncpy(SETTINGS.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
      SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
      RenderLock lock;
      sdFontSystem.ensureLoaded(renderer);
      const bool integrityWarning = sdFontSystem.lastLoadHadIntegrityWarning();
      if (sdFontSystem.resolveFontId(SETTINGS.sdFontFamilyName, SETTINGS.readerFontPointSize) == 0) {
        LOG_ERR("FONT", "Cannot select SD font: %s", SETTINGS.sdFontFamilyName);
        SETTINGS.readerFontPointSize = previousPointSize;
        std::memcpy(SETTINGS.sdFontFamilyName, previousSdFamily, sizeof(previousSdFamily));
        sdFontSystem.ensureLoaded(renderer);
        if (integrityWarning && integrityAlertFontIndex_ != selectedIndex_) {
          integrityAlertFontIndex_ = selectedIndex_;
          queueFontIntegrityAlert();
        }
        requestUpdate();
        return;
      }
      if (integrityWarning && integrityAlertFontIndex_ != selectedIndex_) {
        integrityAlertFontIndex_ = selectedIndex_;
        queueFontIntegrityAlert();
      }
    }
  }
  mappedInput.suppressNextConfirmRelease();
  finish();
}

void FontSelectionActivity::renderPreviewPane(int top, int height, int fontId, const char* fontName) const {
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int left = std::max(metrics_.previewPadding, safe.x);
  const int right = std::min(renderer.getScreenWidth() - metrics_.previewPadding, safe.x + safe.width);
  const int width = right - left;
  if (width <= 0 || height <= 0) return;

  const int labelFontId = UI_10_FONT_ID;
  const int labelH = renderer.getTextHeight(labelFontId);
  const int labelGap = 4;
  const int labelReserved = labelH + 2 * labelGap + metrics_.previewPadding;

  char labelBuf[128];
  snprintf(labelBuf, sizeof(labelBuf), "%s \"%s\"", tr(STR_PREVIEW), fontName ? fontName : "");
  const int dividerY = top + height - metrics_.previewPadding - labelH - labelGap;
  const int bottomDividerY = top + height + metrics_.verticalSpacing / 2;
  const int labelY = dividerY + (bottomDividerY - dividerY - labelH) / 2;
  renderer.drawLine(safe.x, dividerY, safe.x + safe.width - 1, dividerY);
  const auto visibleLabel = renderer.truncatedText(labelFontId, labelBuf, width);
  renderer.drawText(labelFontId, left, labelY, visibleLabel.c_str());

  if (fontId == 0) return;

  const int lineH = renderer.getTextHeight(fontId);
  if (lineH <= 0) return;

  const int innerHeight = height - metrics_.previewPadding - labelReserved;
  const int maxLines = std::max(1, innerHeight / (lineH + 2));

  const char* previewText = I18N.getLanguage() == Language::EN ? "" : tr(STR_FONT_PREVIEW_TEXT);
  if (auto* fcm = renderer.getFontCacheManager()) {
    // Prepare both samples together: SD font prewarming replaces the active
    // glyph set. This exceeds the stack budget; size the temporary heap buffer
    // to preserve every translated UTF-8 character.
    const size_t prewarmSize =
        sizeof(READER_PREVIEW_PARAGRAPH) + std::strlen(previewText) + std::strlen(ELLIPSIS_UTF8) + 2;
    auto prewarmText = makeUniqueNoThrow<char[]>(prewarmSize);
    if (!prewarmText) {
      LOG_ERR("FONT", "Cannot allocate font preview text (%u bytes)", static_cast<unsigned>(prewarmSize));
    } else {
      snprintf(prewarmText.get(), prewarmSize, "%s %s %s", READER_PREVIEW_PARAGRAPH, previewText, ELLIPSIS_UTF8);
      if (!fcm->prewarmCache(fontId, prewarmText.get(), 0x01)) {
        LOG_ERR("FONT", "Cannot prepare font preview glyphs");
      }
    }
  }

  // The pane starts after the header gap; include that gap when centering
  // between the visible header and preview-label dividers.
  const int textAreaTop = top - metrics_.verticalSpacing;
  const int textAreaHeight = dividerY - textAreaTop;
  if (textAreaHeight <= 0) return;
  // English uses the regular paragraph throughout. Other languages retain
  // room for their localized glyph sample, even when only one line fits.
  const int sampleLines = *previewText ? std::min(2, maxLines - 1) : maxLines;
  const auto loremLines = renderer.wrappedText(fontId, READER_PREVIEW_PARAGRAPH, width, sampleLines);
  const int remainingLines = maxLines - static_cast<int>(loremLines.size());
  const auto localizedLines = *previewText && remainingLines > 0
                                  ? renderer.wrappedText(fontId, previewText, width, remainingLines)
                                  : std::vector<std::string>{};
  const int renderedLineCount = static_cast<int>(loremLines.size() + localizedLines.size());
  if (renderedLineCount == 0) return;

  int blockTop = 0, blockBottom = 0, lineOffset = 0;
  bool hasInk = false;
  const auto measureLines = [&](const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
      const auto bounds = renderer.getTextVerticalBounds(fontId, line.c_str());
      if (bounds.bottom > bounds.top) {
        blockTop = hasInk ? std::min(blockTop, lineOffset + bounds.top) : lineOffset + bounds.top;
        blockBottom = hasInk ? std::max(blockBottom, lineOffset + bounds.bottom) : lineOffset + bounds.bottom;
        hasInk = true;
      }
      lineOffset += lineH + 2;
    }
  };
  measureLines(loremLines);
  measureLines(localizedLines);
  if (!hasInk) return;

  const int textBlockHeight = blockBottom - blockTop;
  int y = textAreaTop + (textAreaHeight - textBlockHeight) / 2 - blockTop;
  renderer.beginTextClip(0, textAreaTop, renderer.getScreenWidth(), textAreaHeight);
  for (const auto& line : loremLines) {
    renderer.drawText(fontId, left, y, line.c_str());
    y += lineH + 2;
  }
  for (const auto& line : localizedLines) {
    renderer.drawText(fontId, left, y, line.c_str());
    y += lineH + 2;
  }
  renderer.endTextClip();
}

void FontSelectionActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<FontSelectionActivity*>(user)->buildListScreen(screen);
}

void FontSelectionActivity::buildListScreen(UiApp::ScreenType& screen) {
  const int listTop = afterHeader + previewHeight + metrics_.verticalSpacing;
  const int listHeight = usableHeight - previewHeight - metrics_.verticalSpacing;
  setUiContentMargin(screen, renderer,
                     fui::Insets{static_cast<int16_t>(listTop), 0,
                                 static_cast<int16_t>(renderer.getScreenHeight() - listTop - listHeight), 0});
  const int currentFontIndex = findCurrentFontIndex(registry_, originalSdFontFamilyName_, originalFontFamily_);
  std::vector<fui::ListItem> items;
  items.reserve(fonts_.size());
  for (size_t i = 0; i < fonts_.size(); ++i) {
    fui::ListItem item;
    item.label = fonts_[i].name.c_str();
    if (static_cast<int>(i) == previewFontIndex_ && static_cast<int>(i) != currentFontIndex) {
      item.value = tr(STR_PREVIEW);
    } else if (static_cast<int>(i) == currentFontIndex) {
      item.value = tr(STR_SELECTED);
    }
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }
  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex_);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows_ = rows > 0 ? rows : 1;
  const int fontCount = static_cast<int>(fonts_.size());
  topIndex_ = initialViewportPending_ ? followListSelection(selectedIndex_, 0, visibleRows_, fontCount)
                                      : scrollListBy(topIndex_, 0, visibleRows_, fontCount);
  initialViewportPending_ = false;
  props.topIndex = static_cast<uint16_t>(topIndex_);
  screen.list(props);
}

void FontSelectionActivity::updateLayoutMetrics() {
  metrics_ = UITheme::getInstance().getMetrics();
  afterHeader = TouchHeaderBackButton::contentTop(renderer, mappedInput) + metrics_.verticalSpacing;
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  bottomReserved = renderer.getScreenHeight() - safe.y - safe.height + metrics_.verticalSpacing;
  usableHeight = renderer.getScreenHeight() - afterHeader - bottomReserved;
  previewHeight = usableHeight * metrics_.previewHeightPercent / 100;
}

void FontSelectionActivity::render(RenderLock&&) {
  updateLayoutMetrics();
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget_, header, tr(STR_FONT_FAMILY), false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_FONT_FAMILY));
  }

  const int previewFontId = SETTINGS.getReaderFontId();
  const int textHeight = previewFontId == 0 ? 0 : renderer.getTextHeight(previewFontId);
  const int labelHeight = renderer.getTextHeight(UI_10_FONT_ID);
  const int threeLineHeight = 3 * (textHeight + 2) + labelHeight + 8 + metrics_.previewPadding * 2;
  const int listRowHeight = uiListRowHeight(app_.theme(), UiListRowType::SingleLine);
  const int listReserve = 2 * listRowHeight + app_.theme().listRowGap + metrics_.verticalSpacing;
  previewHeight = std::min(std::max(0, usableHeight - listReserve),
                           std::max(usableHeight * metrics_.previewHeightPercent / 100, threeLineHeight));
  const int previewTop = afterHeader;
  const int listTop = previewTop + previewHeight + metrics_.verticalSpacing;
  const char* previewFontName = (previewFontIndex_ >= 0 && previewFontIndex_ < static_cast<int>(fonts_.size()))
                                    ? fonts_[previewFontIndex_].name.c_str()
                                    : nullptr;
  renderPreviewPane(previewTop, previewHeight, previewFontId, previewFontName);

  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  renderer.drawLine(safe.x, listTop - metrics_.verticalSpacing / 2, safe.x + safe.width - 1,
                    listTop - metrics_.verticalSpacing / 2);

  uiReady_ = false;
  renderUiApp(app_, uiTarget_);
  uiReady_ = true;

  const bool onPreviewed = selectedIndex_ == previewFontIndex_;
  const char* confirmLabel = onPreviewed ? tr(STR_SELECT) : tr(STR_PREVIEW);
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
