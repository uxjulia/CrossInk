#include "TtfRenderOptionsActivity.h"

#if CROSSINK_SCALABLE_FONTS

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFontSystem.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
const std::vector<std::string>& hintingOptions() {
  static const std::vector<std::string> values = {tr(STR_NONE_OPT), tr(STR_TTF_NATIVE), tr(STR_TTF_AUTO),
                                                  tr(STR_LIGHT)};
  return values;
}
const std::vector<std::string>& rasterOptions() {
  static const std::vector<std::string> values = {tr(STR_TTF_GRAYSCALE), tr(STR_TTF_MONOCHROME)};
  return values;
}
const std::vector<std::string>& interpreterOptions() {
  static const std::vector<std::string> values = {tr(STR_DEFAULT_VALUE), "FreeType 35", "FreeType 40"};
  return values;
}
const std::vector<std::string>& weightOptions() {
  static const std::vector<std::string> values = {"-0.50 px", "-0.25 px", tr(STR_DEFAULT_VALUE), "+0.25 px", "+0.50 px",
                                                  "+0.75 px", "+1.00 px"};
  return values;
}
const std::vector<std::string>& slantOptions() {
  static const std::vector<std::string> values = {tr(STR_NONE_OPT), tr(STR_TTF_GENTLE), tr(STR_TTF_MEDIUM),
                                                  tr(STR_TTF_STRONG)};
  return values;
}
}  // namespace

const std::vector<std::string>* TtfRenderOptionsActivity::optionLabels(const Row row) {
  switch (row) {
    case Row::Hinting:
      return &hintingOptions();
    case Row::Raster:
      return &rasterOptions();
    case Row::Interpreter:
      return &interpreterOptions();
    case Row::Weight:
      return &weightOptions();
    case Row::Slant:
      return &slantOptions();
    default:
      return nullptr;
  }
}

uint8_t TtfRenderOptionsActivity::selectedOption(const TtfRenderProfile& profile, const Row row) {
  switch (row) {
    case Row::Hinting:
      return profile.hinting;
    case Row::Raster:
      return profile.raster;
    case Row::Interpreter:
      return profile.interpreter;
    case Row::Weight:
      return profile.weight;
    case Row::Slant:
      return profile.slant;
    default:
      return 0;
  }
}

void TtfRenderOptionsActivity::setOption(TtfRenderProfile& profile, const Row row, const int index) {
  switch (row) {
    case Row::Hinting:
      profile.hinting = static_cast<uint8_t>(index);
      if (profile.hinting != 1) profile.interpreter = 0;
      break;
    case Row::Raster:
      profile.raster = static_cast<uint8_t>(index);
      break;
    case Row::Interpreter:
      profile.interpreter = static_cast<uint8_t>(index);
      break;
    case Row::Weight:
      profile.weight = static_cast<uint8_t>(index);
      break;
    case Row::Slant:
      profile.slant = static_cast<uint8_t>(index);
      break;
    default:
      break;
  }
}

StrId TtfRenderOptionsActivity::titleId(const Row row) {
  switch (row) {
    case Row::Hinting:
      return StrId::STR_TTF_HINTING;
    case Row::Raster:
      return StrId::STR_TTF_RASTER_MODE;
    case Row::Interpreter:
      return StrId::STR_TTF_INTERPRETER;
    case Row::Weight:
      return StrId::STR_TTF_WEIGHT;
    case Row::Slant:
      return StrId::STR_TTF_SLANT;
    default:
      return StrId::STR_NONE_OPT;
  }
}

const char* TtfRenderOptionsActivity::rowLabel(const Row row) {
  switch (row) {
    case Row::Hinting:
      return tr(STR_TTF_HINTING);
    case Row::Raster:
      return tr(STR_TTF_RASTER_MODE);
    case Row::Interpreter:
      return tr(STR_TTF_INTERPRETER);
    case Row::Weight:
      return tr(STR_TTF_WEIGHT);
    case Row::Slant:
      return tr(STR_TTF_SLANT);
    case Row::StemDarkening:
      return tr(STR_TTF_STEM_DARKENING);
    case Row::Reset:
      return tr(STR_TTF_RESET);
  }
  return "";
}

const char* TtfRenderOptionsActivity::rowValue(const Row row, const TtfRenderProfile& profile) {
  if (row == Row::StemDarkening) return profile.stemDarkening ? tr(STR_ON) : tr(STR_OFF);
  if (row == Row::Reset) return "";
  const auto* options = optionLabels(row);
  if (options == nullptr || options->empty()) return "";
  const size_t index = std::min<size_t>(selectedOption(profile, row), options->size() - 1);
  return (*options)[index].c_str();
}

void TtfRenderOptionsActivity::onEnter() {
  Activity::onEnter();
  profile_ = TTF_RENDER_PROFILES.profileFor(family_.c_str());
  initialProfile_ = profile_;
  rebuildRows();
  applySharedUiTheme(app_, uiTarget_);
  app_.on(ACTION_ROW, &TtfRenderOptionsActivity::onRowEvent, this);
  app_.setScreen(&TtfRenderOptionsActivity::optionsScreen, this);
  requestUpdate();
}

void TtfRenderOptionsActivity::onExit() {
  // A Home/Reader unwind pops this screen without finishWithResult(); still
  // apply saved profile changes so the active font matches its stored profile.
  if (changed_ && !reloadHandled_) sdFontSystem.reloadActiveScalableFamily(renderer, family_.c_str());
  Activity::onExit();
}

void TtfRenderOptionsActivity::rebuildRows() {
  rows_ = {Row::Hinting, Row::Raster};
  if (profile_.hinting == 1) rows_.push_back(Row::Interpreter);
  rows_.push_back(Row::Weight);
  rows_.push_back(Row::Slant);
  rows_.push_back(Row::StemDarkening);
  rows_.push_back(Row::Reset);
  selectedIndex_ = std::clamp(selectedIndex_, 0, static_cast<int>(rows_.size()) - 1);
}

bool TtfRenderOptionsActivity::save() {
  if (profile_ == TTF_RENDER_PROFILES.profileFor(family_.c_str())) return true;
  if (!TTF_RENDER_PROFILES.setProfile(family_.c_str(), profile_)) {
    LOG_ERR("TTFUI", "Failed to save rendering profile for %s", family_.c_str());
    return false;
  }
  changed_ = profile_ != initialProfile_;
  return true;
}

void TtfRenderOptionsActivity::finishWithResult() {
  const bool activeFamilyChanged = changed_ && sdFontSystem.reloadActiveScalableFamily(renderer, family_.c_str());
  reloadHandled_ = true;
  setResult(TtfRenderOptionsResult{activeFamilyChanged});
  finish();
}

void TtfRenderOptionsActivity::showOptions(const Row row) {
  const auto* options = optionLabels(row);
  if (options == nullptr) return;
  optionPopup_.show(titleId(row), *options, selectedOption(profile_, row), [this, row](const int index) {
    setOption(profile_, row, index);
    if (row == Row::Hinting) rebuildRows();
    save();
    requestUpdate();
  });
}

void TtfRenderOptionsActivity::activateSelected() {
  if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(rows_.size())) return;
  const Row row = rows_[static_cast<size_t>(selectedIndex_)];
  if (row == Row::StemDarkening) {
    profile_.stemDarkening = !profile_.stemDarkening;
    save();
  } else if (row == Row::Reset) {
    const TtfRenderProfile previous = profile_;
    profile_ = {};
    const bool saved = save();
    if (!saved) profile_ = previous;
    rebuildRows();
    static constexpr StrId options[] = {StrId::STR_OK};
    optionPopup_.show(saved ? StrId::STR_TTF_DEFAULTS_RESET : StrId::STR_TTF_RESET_FAILED, options, 1, 0, nullptr);
  } else {
    showOptions(row);
  }
  requestUpdate();
}

void TtfRenderOptionsActivity::loop() {
  if (optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finishWithResult();
    return;
  }
  if (uiReady_) {
    const fui::InputSnapshot snapshot = touchSnapshotFrom(mappedInput);
    if (snapshot.touchPressed || snapshot.touchReleased) {
      const auto event = app_.route(snapshot);
      if (app_.invalidated()) requestUpdate();
      if (event) return;
    }
  }
  if (mappedInput.hasTouch()) {
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows_ : -visibleRows_;
      topIndex_ = scrollListBy(topIndex_, delta, visibleRows_, static_cast<int>(rows_.size()));
      requestUpdate();
      return;
    }
  }
  buttonNavigator_.onNextRelease([this] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, static_cast<int>(rows_.size()));
    topIndex_ = followListSelection(selectedIndex_, topIndex_, visibleRows_, static_cast<int>(rows_.size()));
    requestUpdate();
  });
  buttonNavigator_.onPreviousRelease([this] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, static_cast<int>(rows_.size()));
    topIndex_ = followListSelection(selectedIndex_, topIndex_, visibleRows_, static_cast<int>(rows_.size()));
    requestUpdate();
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) activateSelected();
}

void TtfRenderOptionsActivity::optionsScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<TtfRenderOptionsActivity*>(user)->buildOptionsScreen(screen);
}

void TtfRenderOptionsActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<TtfRenderOptionsActivity*>(user);
  if (self->optionPopup_.isActive() || event.value < 0 || event.value >= static_cast<int>(self->rows_.size())) return;
  self->selectedIndex_ = event.value;
  self->app_.clearTapFlash();
  self->activateSelected();
}

void TtfRenderOptionsActivity::buildOptionsScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouchHardware(), false);
  setUiContentMargin(screen, renderer,
                     fui::Insets{static_cast<int16_t>(TouchHeaderBackButton::contentTop(renderer, mappedInput, safe.y)),
                                 static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
                                 static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height),
                                 static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::TextStyle familyStyle = screen.theme().smallText;
  familyStyle.bold = true;
  familyStyle.maxLines = 1;
  fui::Rect familyRect = screen.takeTop(screen.target().lineHeight(familyStyle.font), metrics.verticalSpacing);
  familyRect.x += metrics.contentSidePadding;
  familyRect.width -= metrics.contentSidePadding * 2;
  screen.target().text(familyRect, family_.c_str(), familyStyle);

  std::vector<std::string> values;
  std::vector<fui::ListItem> items;
  values.reserve(rows_.size());
  items.reserve(rows_.size());
  for (size_t i = 0; i < rows_.size(); ++i) {
    values.push_back(rowValue(rows_[i], profile_));
    fui::ListItem item;
    item.label = rowLabel(rows_[i]);
    item.value = values.back().c_str();
    item.toggle = rows_[i] == Row::StemDarkening;
    item.toggleChecked = profile_.stemDarkening;
    if (item.toggle) item.value = nullptr;
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
  props.labelText.maxLines = 2;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows_ = rows > 0 ? rows : 1;
  topIndex_ = scrollListBy(topIndex_, 0, visibleRows_, static_cast<int>(rows_.size()));
  props.topIndex = static_cast<uint16_t>(topIndex_);
  screen.list(props);
}

void TtfRenderOptionsActivity::render(RenderLock&&) {
  if (optionPopup_.processRender(renderer, mappedInput)) return;
  renderer.clearScreen();
  Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, !mappedInput.hasTouchHardware(), false);
  header.x = safe.x;
  header.width = safe.width;
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget_, header, tr(STR_TTF_RENDERING), true);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_TTF_RENDERING), nullptr, true);
  }
  uiReady_ = false;
  renderUiApp(app_, uiTarget_);
  uiReady_ = true;
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}

#endif
