#include "FrontlightPanelActivity.h"

#include <CrossInkHalFrontlight.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/RenderLock.h"
#include "activities/home/BookActions.h"
#include "activities/settings/SettingsActivity.h"
#include "components/DrawerHandle.h"
#include "components/HeaderDate.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/frontlightHeaderIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/readingStatsIcons.h"
#include "components/icons/tablerIcons.h"
#include "components/icons/touchscreenStateIcons.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_BRIGHTNESS = 1;
constexpr fui::ActionId ACTION_WARMTH = 2;
constexpr fui::ActionId ACTION_TOGGLE = 3;
constexpr fui::ActionId ACTION_BRIGHTNESS_STEP = 4;
constexpr fui::ActionId ACTION_WARMTH_STEP = 5;
constexpr fui::ActionId ACTION_QUICK = 6;
constexpr fui::ActionId ACTION_DISMISS = 7;
constexpr int BRIGHTNESS_STEP = 5;
constexpr int FINE_STEP = 1;
constexpr int HEADER_ICON_SIZE = 24;
constexpr int HEADER_BUTTON_WIDTH = 64;
constexpr int HEADER_CONTENT_BOTTOM_GAP = 8;
constexpr int ACTION_BAR_HEIGHT = 58;

uint8_t percentFromPermille(const int16_t permille) {
  int value = (static_cast<int>(permille) * 100 + 500) / 1000;
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  return static_cast<uint8_t>(value);
}

fui::SheetProps frontlightSheetProps() {
  fui::SheetProps sheet;
  sheet.anchor = fui::SheetEdge::Top;
  sheet.dismissAction = ACTION_DISMISS;
  sheet.radius = 0;
  return sheet;
}
}  // namespace

FrontlightPanelActivity::FrontlightPanelActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 FrontlightPanelContext context)
    : Activity("FrontlightPanel", renderer, mappedInput),
      context(std::move(context)),
      drawerState(this->context.drawerState),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void FrontlightPanelActivity::onEnter() {
  Activity::onEnter();

  // The HAL is the live source of truth; SETTINGS only mirrors it for boot.
  brightness = Frontlight.brightness();
  warmth = Frontlight.warmth();
  lightOn = Frontlight.isOn();
  initialInversion = SETTINGS.screenInverted;
  initialTouchscreenDisabled = SETTINGS.disableReaderTouchscreen;
  pendingTouchscreenDisabled = initialTouchscreenDisabled;
  mappedInput.setReaderTouchscreenOverride(true);

  uiReady = false;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_BRIGHTNESS, &FrontlightPanelActivity::onBrightnessEvent, this);
  app.on(ACTION_WARMTH, &FrontlightPanelActivity::onWarmthEvent, this);
  app.on(ACTION_TOGGLE, &FrontlightPanelActivity::onToggleEvent, this);
  app.on(ACTION_BRIGHTNESS_STEP, &FrontlightPanelActivity::onBrightnessStepEvent, this);
  app.on(ACTION_WARMTH_STEP, &FrontlightPanelActivity::onWarmthStepEvent, this);
  app.on(ACTION_QUICK, &FrontlightPanelActivity::onQuickActionEvent, this);
  app.on(ACTION_DISMISS, &FrontlightPanelActivity::onDismissEvent, this);
  app.setScreen(&FrontlightPanelActivity::panelScreen, this);
  prepareReaderDetailsLayout();
  requestUpdate();
}

void FrontlightPanelActivity::onExit() {
  // Debounced persistence: one SPIFFS write on close, never per slider tick.
  const bool inversionChanged = initialInversion != static_cast<bool>(SETTINGS.screenInverted);
  const bool touchscreenChanged = initialTouchscreenDisabled != pendingTouchscreenDisabled;
  SETTINGS.disableReaderTouchscreen = pendingTouchscreenDisabled ? 1 : 0;
  const bool frontlightChanged =
      !context.showReaderDetails && (SETTINGS.frontlightBrightness != brightness ||
                                     SETTINGS.frontlightWarmth != warmth || SETTINGS.frontlightOn != (lightOn ? 1 : 0));
  if (frontlightChanged || inversionChanged || touchscreenChanged) {
    if (!context.showReaderDetails) {
      SETTINGS.frontlightBrightness = brightness;
      SETTINGS.frontlightWarmth = warmth;
      SETTINGS.frontlightOn = lightOn ? 1 : 0;
    }
    if (context.sourceActivity)
      context.sourceActivity->persistFrontlightPanelSettings();
    else
      SETTINGS.saveToFile();
  }
  mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();
}

void FrontlightPanelActivity::onBrightnessEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->brightness = percentFromPermille(event.dragPermille);
  Frontlight.setBrightness(self->brightness);
  // Adjusting brightness while off is an obvious "I want light" intent.
  if (!self->lightOn) {
    self->lightOn = true;
    Frontlight.setOn(true);
  }
}

void FrontlightPanelActivity::onWarmthEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->warmth = percentFromPermille(event.dragPermille);
  Frontlight.setWarmth(self->warmth);
}

void FrontlightPanelActivity::onBrightnessStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustBrightness(event.value * FINE_STEP);
}

void FrontlightPanelActivity::onWarmthStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->adjustWarmth(event.value * FINE_STEP);
}

void FrontlightPanelActivity::onToggleEvent(const fui::ActionEvent&, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->toggleLight();
}

void FrontlightPanelActivity::onQuickActionEvent(const fui::ActionEvent& event, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->activateQuickAction(event.value);
}

void FrontlightPanelActivity::onDismissEvent(const fui::ActionEvent&, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->close();
}

void FrontlightPanelActivity::adjustBrightness(const int delta) {
  int next = static_cast<int>(brightness) + delta;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  if (next == brightness) return;
  brightness = static_cast<uint8_t>(next);
  Frontlight.setBrightness(brightness);
  if (!lightOn) {
    lightOn = true;
    Frontlight.setOn(true);
  }
  requestUpdate();
}

void FrontlightPanelActivity::adjustWarmth(const int delta) {
  int next = static_cast<int>(warmth) + delta;
  if (next < 0) next = 0;
  if (next > 100) next = 100;
  if (next == warmth) return;
  warmth = static_cast<uint8_t>(next);
  Frontlight.setWarmth(warmth);
  requestUpdate();
}

void FrontlightPanelActivity::toggleLight() {
  lightOn = !lightOn;
  Frontlight.setOn(lightOn);
  requestUpdate();
}

void FrontlightPanelActivity::toggleReaderTouchscreen() {
  pendingTouchscreenDisabled = !pendingTouchscreenDisabled;
  {
    RenderLock lock;
    BookActions::drawToast(renderer,
                           pendingTouchscreenDisabled ? tr(STR_TOUCHSCREEN_DISABLED) : tr(STR_TOUCHSCREEN_ENABLED));
  }
  delay(1000);
  requestUpdate();
}

void FrontlightPanelActivity::close() { finish(); }

void FrontlightPanelActivity::openReadingStats() {
  if (!context.readingStatsActivity && context.sourceActivity) {
    context.readingStatsActivity = context.sourceActivity->createFrontlightReadingStatsActivity();
  }
  if (!context.readingStatsActivity) return;
  startActivityForResult(std::move(context.readingStatsActivity), [this](const ActivityResult&) { close(); });
}

void FrontlightPanelActivity::openGlobalSettings() {
  if (context.sourceActivity) context.sourceActivity->onFrontlightGlobalSettingsOpened();
  auto settings = makeUniqueNoThrow<SettingsActivity>(renderer, mappedInput, true, true);
  if (!settings) {
    LOG_ERR("LIGHT", "OOM opening Settings from frontlight panel");
    if (context.sourceActivity) context.sourceActivity->onFrontlightGlobalSettingsClosed();
    return;
  }
  startActivityForResult(std::move(settings), [this](const ActivityResult&) {
    if (context.sourceActivity) context.sourceActivity->onFrontlightGlobalSettingsClosed();
    close();
  });
}

void FrontlightPanelActivity::openSyncDialog() {
  static constexpr std::array<StrId, 3> OPTIONS = {StrId::STR_SYNC_PROGRESS, StrId::STR_NEARBY_POSITION_SYNC,
                                                   StrId::STR_SEND_NEARBY_BOOK};
  drawerState.syncDialogOpen = true;
  const bool canSyncBookProgress = context.activeEpub;
  const bool canSendBook = !context.bookPath.empty();
  optionPopup.show(StrId::STR_SYNC_AND_TRANSFER, OPTIONS.data(), OPTIONS.size(), canSyncBookProgress ? 0 : 2,
                   [this](const int index) {
                     drawerState.syncDialogOpen = false;
                     FrontlightPanelResult panelResult;
                     panelResult.state = drawerState;
                     panelResult.activeEpub = context.activeEpub;
                     panelResult.bookPath = context.bookPath;
                     if (index == 0) panelResult.action = FrontlightPanelAction::SyncProgress;
                     if (index == 1) panelResult.action = FrontlightPanelAction::NearbyPositionSync;
                     if (index == 2) panelResult.action = FrontlightPanelAction::SendNearbyBook;
                     setResult(ActivityResult(std::move(panelResult)));
                     finish();
                   });
  optionPopup.setDisabledOptions({!canSyncBookProgress, !canSyncBookProgress, !canSendBook});
  optionPopup.setCancelCallback([this] { closeSyncDialog(); });
  requestUpdate();
}

void FrontlightPanelActivity::closeSyncDialog() {
  drawerState.syncDialogOpen = false;
  close();
}

void FrontlightPanelActivity::activateQuickAction(const int index) {
  if (index < 0 || index >= 5) return;
  drawerState.selectedAction = static_cast<int8_t>(index);
  switch (index) {
    case 0:
      openReadingStats();
      return;
    case 1:
      openSyncDialog();
      return;
    case 2:
      SETTINGS.screenInverted = SETTINGS.screenInverted ? 0 : 1;
      display.setInverted(SETTINGS.screenInverted != 0);
      requestUpdate();
      return;
    case 3:
      openGlobalSettings();
      return;
    case 4:
      toggleReaderTouchscreen();
      return;
  }
}

bool FrontlightPanelActivity::handleHomeGesture() {
  if (context.activeReaderBook) {
    activityManager.goHome();
    return true;
  }
  close();
  return true;
}

void FrontlightPanelActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // A power-button shortcut can change the light globally while this panel is
  // open. Keep the panel's state and eventual persistence in sync.
  if (!context.showReaderDetails && lightOn != Frontlight.isOn()) {
    lightOn = Frontlight.isOn();
    requestUpdate();
  }

  const Rect homeButton = homeButtonRect();
  if (context.activeReaderBook &&
      mappedInput.wasTapInRect(homeButton.x, homeButton.y, homeButton.width, homeButton.height)) {
    activityManager.goHome();
    return;
  }

  if (DrawerHandle::wasDismissSwipe(mappedInput, drawerHandleRect, fui::SheetEdge::Top)) {
    close();
    return;
  }

  fui::InputSnapshot snap{};
  if (uiReady) {
    snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchHeld || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) {
        if (event.dragPermille >= 0) draggingSlider = true;
        return;
      }
    }
    if (draggingSlider) {
      // Drag ended (possibly off the slider): swallow the release's swipe so
      // it can't double as the left-edge back gesture and close the panel.
      if (!snap.touchHeld) draggingSlider = false;
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    close();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!context.showReaderDetails) toggleLight();
    return;
  }

  if (context.showReaderDetails) return;

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left},
                                       [this] { adjustBrightness(-BRIGHTNESS_STEP); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right},
                                       [this] { adjustBrightness(BRIGHTNESS_STEP); });
}

Rect FrontlightPanelActivity::homeButtonRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return Rect{renderer.getScreenWidth() - HEADER_BUTTON_WIDTH, metrics.topPadding, HEADER_BUTTON_WIDTH,
              TouchHeaderBackButton::height(metrics, mappedInput)};
}

int FrontlightPanelActivity::computePanelBottom() const {
  // Mirror buildPanelScreen's takeTop/spacer sequence so the frame, content
  // margin, and dismiss threshold land on the same edge.
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto tokens = uiThemeTokens(uiTarget);
  const int16_t lh = uiTarget.lineHeight(tokens.bodyText.font);
  int y = metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput);
  if (context.showReaderDetails) {
    const int16_t titleLh = uiTarget.lineHeight(tokens.titleText.font);
    y += tokens.spaceLg * 2;
    y += titleLh * static_cast<int>(readerTitleLines.size());
    if (!readerTitleLines.empty()) y += tokens.spaceLg;
    if (!context.bookDetails.chapter.empty() || !context.bookDetails.author.empty()) {
      y += lh + tokens.spaceLg;
    }
    y += lh;
    y += tokens.spaceLg * 3 + ACTION_BAR_HEIGHT;
    const auto sheet = frontlightSheetProps();
    return y + DrawerHandle::bandHeight(sheet);
  }
  y += tokens.spaceLg;                     // leading spacer
  y += tokens.rowHeight + tokens.spaceSm;  // brightness label + frontlight toggle row
  y += tokens.rowHeight + tokens.spaceLg;  // brightness slider
  if (Frontlight.hasColorTemperature()) {
    y += lh + tokens.spaceSm + tokens.rowHeight + tokens.spaceLg;  // warmth label + slider
  }
  y += tokens.spaceLg + ACTION_BAR_HEIGHT;  // trailing padding + actions
  const auto sheet = frontlightSheetProps();
  return y + DrawerHandle::bandHeight(sheet);
}

void FrontlightPanelActivity::prepareReaderDetailsLayout() {
  readerTitleLines.clear();
  if (!context.showReaderDetails || context.bookDetails.title.empty()) return;
  const auto tokens = uiThemeTokens(uiTarget);
  const int16_t sideInset = static_cast<int16_t>(tokens.spaceLg * 2);
  const int width = std::max<int>(1, renderer.getScreenWidth() - sideInset * 2);
  readerTitleLines =
      renderer.wrappedText(UI_12_FONT_ID, context.bookDetails.title.c_str(), width, 2, EpdFontFamily::BOLD);
}

void FrontlightPanelActivity::drawReaderDetails(fui::Screen<20>& screen) {
  const auto& theme = screen.theme();
  const fui::Insets sideInset{0, static_cast<int16_t>(theme.spaceLg * 2), 0, static_cast<int16_t>(theme.spaceLg * 2)};
  screen.spacer(static_cast<int16_t>(theme.spaceLg * 2));

  fui::TextStyle title = theme.titleText;
  title.bold = true;
  title.align = fui::TextAlign::Center;
  const int16_t titleLh = screen.target().lineHeight(title.font);
  for (const std::string& line : readerTitleLines) {
    screen.target().text(screen.takeTop(titleLh).inset(sideInset), line.c_str(), title);
  }
  if (!readerTitleLines.empty()) screen.spacer(theme.spaceLg);

  const char* subtitle =
      !context.bookDetails.chapter.empty() ? context.bookDetails.chapter.c_str() : context.bookDetails.author.c_str();
  fui::TextStyle body = theme.bodyText;
  body.align = fui::TextAlign::Center;
  const int16_t bodyLh = screen.target().lineHeight(body.font);
  if (subtitle[0] != '\0') {
    const std::string visible =
        renderer.truncatedText(UI_10_FONT_ID, subtitle, screen.body().width - sideInset.left - sideInset.right);
    screen.target().text(screen.takeTop(bodyLh, theme.spaceLg).inset(sideInset), visible.c_str(), body);
  }

  char progress[48];
  std::snprintf(progress, sizeof(progress), "%d%% %s", context.bookDetails.progressPercent, tr(STR_COMPLETE));
  screen.target().text(screen.takeTop(bodyLh).inset(sideInset), progress, body);
  screen.spacer(static_cast<int16_t>(theme.spaceLg * 3));
}

void FrontlightPanelActivity::panelScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<FrontlightPanelActivity*>(user)->buildPanelScreen(screen);
}

void FrontlightPanelActivity::buildPanelScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& theme = screen.theme();
  const fui::SheetProps sheet = frontlightSheetProps();
  const fui::Rect sheetRect{0, 0, static_cast<int16_t>(renderer.getScreenWidth()), static_cast<int16_t>(panelBottom)};
  fui::sheet(screen.frame(), sheetRect, sheet);
  // Body spans from below the header to the sheet's content edge. The grabber
  // owns the remaining band at the bottom of this top-anchored drawer.
  const fui::Rect sheetContent = fui::sheetContentRect(sheetRect, sheet);
  drawerHandleRect = DrawerHandle::registerTap(screen.frame(), sheetContent, sheet, ACTION_DISMISS);
  const int16_t bottomInset = static_cast<int16_t>(renderer.getScreenHeight() - sheetContent.bottom());
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  bottomInset, 0});

  const fui::Rect actionBar = screen.takeBottom(ACTION_BAR_HEIGHT);
  const int16_t slotWidth = static_cast<int16_t>(actionBar.width / 5);
  const std::array<fui::BitmapRef, 5> icons = {
      fui::bitmapFromIcon(icon_reading_stats_24), fui::bitmapFromIcon(icon_transfer_24),
      fui::bitmapFromIcon(icon_tabler_moon_filled_24), fui::bitmapFromIcon(icon_sliders_horizontal_24),
      fui::bitmapFromIcon(pendingTouchscreenDisabled ? icon_device_tablet_off_24 : icon_device_tablet_24)};
  for (int16_t i = 0; i < 5; ++i) {
    const int16_t x = static_cast<int16_t>(actionBar.x + i * slotWidth);
    const int16_t width = i == 4 ? static_cast<int16_t>(actionBar.right() - x) : slotWidth;
    const fui::Rect slot{x, actionBar.y, width, actionBar.height};
    screen.frame().hit(slot, ACTION_QUICK, i);
    screen.target().bitmap(slot, icons[static_cast<size_t>(i)], fui::BitmapMode::Center);
  }
  screen.target().fill(fui::Rect{actionBar.x, actionBar.y, actionBar.width, 1}, fui::Paint::solid(fui::Color::Black));

  if (context.showReaderDetails) {
    drawReaderDetails(screen);
    return;
  }

  const int16_t lh = screen.target().lineHeight(theme.bodyText.font);
  const int16_t rowH = theme.rowHeight;
  const fui::Insets sideInset{0, static_cast<int16_t>(theme.spaceLg * 2), 0, static_cast<int16_t>(theme.spaceLg * 2)};
  char line[48];

  screen.spacer(theme.spaceLg);

  // Header row: "Brightness NN%" on the left, a tappable bulb icon on the right
  // that toggles the light — `lightbulb` when on, `lightbulb-off` when off (the
  // icon itself is the state indicator). Sharing a row with the label frees the
  // whole bottom toggle row, shrinking the panel.
  const fui::Rect headerRow = screen.takeTop(rowH, theme.spaceSm).inset(sideInset);
  snprintf(line, sizeof(line), "%s  %u%%", tr(STR_BRIGHTNESS), static_cast<unsigned>(brightness));
  const fui::BitmapRef lightIcon = fui::bitmapFromIcon(lightOn ? icon_lightbulb_28 : icon_lightbulb_off_28);
  const int16_t iconW = static_cast<int16_t>(lightIcon.width);
  const int16_t iconH = static_cast<int16_t>(lightIcon.height);
  const fui::Rect iconRect{static_cast<int16_t>(headerRow.x + headerRow.width - iconW),
                           static_cast<int16_t>(headerRow.y + (rowH - iconH) / 2), iconW, iconH};
  const fui::Rect labelRect{headerRow.x, static_cast<int16_t>(headerRow.y + (rowH - lh) / 2),
                            static_cast<int16_t>(headerRow.width - iconW - theme.spaceMd), lh};
  screen.target().text(labelRect, line, theme.bodyText);
  // Keep the visible glyph aligned to the content inset, but let its touch
  // target fill the otherwise blank right edge and the adjacent row gaps.
  // This makes the control more forgiving without reaching the brightness
  // slider below.
  const int16_t hitW = static_cast<int16_t>(iconW + theme.spaceLg * 4);
  const fui::Rect hitRect{static_cast<int16_t>(headerRow.right() - hitW),
                          static_cast<int16_t>(headerRow.y - theme.spaceSm),
                          static_cast<int16_t>(hitW + sideInset.right), static_cast<int16_t>(rowH + theme.spaceSm * 2)};
  screen.frame().hit(hitRect, ACTION_TOGGLE);
  screen.target().bitmap(iconRect, lightIcon, fui::BitmapMode::Center);

  addStepSlider(screen, screen.takeTop(theme.rowHeight, theme.spaceLg).inset(sideInset), brightness, ACTION_BRIGHTNESS,
                ACTION_BRIGHTNESS_STEP);

  if (Frontlight.hasColorTemperature()) {
    snprintf(line, sizeof(line), "%s  %u%%", tr(STR_WARMTH), static_cast<unsigned>(warmth));
    screen.target().text(screen.takeTop(lh, theme.spaceSm).inset(sideInset), line, theme.bodyText);
    addStepSlider(screen, screen.takeTop(theme.rowHeight, theme.spaceLg).inset(sideInset), warmth, ACTION_WARMTH,
                  ACTION_WARMTH_STEP);
  }

  screen.spacer(theme.spaceLg);
}

void FrontlightPanelActivity::drawHeader() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int headerHeight = TouchHeaderBackButton::height(metrics, mappedInput);
  const Rect header{0, metrics.topPadding, renderer.getScreenWidth(), headerHeight};

  char date[16] = {};
  const char* title = context.showReaderDetails ? "" : tr(STR_FRONTLIGHT);
  int titleFontId = UI_12_FONT_ID;
  if (context.showReaderDetails) {
    if (formatHeaderDateText(date, sizeof(date))) title = date;
    titleFontId = UI_10_FONT_ID;
  } else if (context.activeReaderBook && !context.bookTitle.empty()) {
    title = context.bookTitle.c_str();
    titleFontId = UI_10_FONT_ID;
  } else if (formatHeaderDateText(date, sizeof(date))) {
    title = date;
    titleFontId = UI_10_FONT_ID;
  }

  // Reuse the normal header chrome so the drawer retains its separator, clock,
  // and battery indicator. The date remains deliberately smaller than a page
  // title, matching its secondary status role.
  // The panel is a global overlay even when it was opened from a reader. Its
  // clock and battery therefore follow the outside-reader visibility settings.
  GUI.drawHeader(renderer, header, "", nullptr, false);
  // Keep the title/date in the header's lower lane. The status chrome above
  // owns the clock and battery, so centering across the whole header crowds it.
  const int headerBottom = header.y + header.height;
  const int titleY = headerBottom - HEADER_CONTENT_BOTTOM_GAP - renderer.getLineHeight(titleFontId);
  const bool showBookTitle = !context.showReaderDetails && context.activeReaderBook && !context.bookTitle.empty();
  if (showBookTitle) {
    const auto tokens = uiThemeTokens(uiTarget);
    const Rect homeButton = homeButtonRect();
    const int titleX = header.x + tokens.headerSidePadding;
    const int titleRight = homeButton.x - tokens.spaceSm;
    const int titleWidth = std::max(0, titleRight - titleX);
    const std::string visibleTitle = renderer.truncatedText(titleFontId, title, titleWidth, EpdFontFamily::REGULAR);
    renderer.drawText(titleFontId, titleX, titleY, visibleTitle.c_str(), true);
  } else {
    UITheme::drawCenteredText(renderer, header, titleFontId, titleY, title, true);
  }

  if (context.activeReaderBook) {
    const Rect button = homeButtonRect();
    uiTarget.bitmap(fui::Rect{static_cast<int16_t>(button.x + (button.width - HEADER_ICON_SIZE) / 2),
                              static_cast<int16_t>(headerBottom - HEADER_CONTENT_BOTTOM_GAP - HEADER_ICON_SIZE),
                              HEADER_ICON_SIZE, HEADER_ICON_SIZE},
                    fui::bitmapFromIcon(icon_home_24), fui::BitmapMode::Center);
  }
}

void FrontlightPanelActivity::addStepSlider(UiApp::ScreenType& screen, const fui::Rect& row, const uint8_t value,
                                            const fui::ActionId sliderAction, const fui::ActionId stepAction) {
  const auto& theme = screen.theme();
  const int16_t stepWidth = row.height;
  const fui::Rect minusHit{row.x, row.y, stepWidth, row.height};
  const fui::Rect plusHit{static_cast<int16_t>(row.right() - stepWidth), row.y, stepWidth, row.height};

  fui::TextStyle glyph = theme.bodyText;
  glyph.align = fui::TextAlign::Center;
  const int16_t lineHeight = screen.target().lineHeight(glyph.font);
  const int16_t glyphY = static_cast<int16_t>(row.y + (row.height - lineHeight) / 2);
  screen.target().text(fui::Rect{minusHit.x, glyphY, stepWidth, lineHeight}, "-", glyph);
  screen.target().text(fui::Rect{plusHit.x, glyphY, stepWidth, lineHeight}, "+", glyph);
  screen.frame().hit(minusHit, stepAction, -1, fui::InputTouch);
  screen.frame().hit(plusHit, stepAction, +1, fui::InputTouch);

  fui::SliderProps slider;
  slider.value = value;
  slider.max = 100;
  slider.action = sliderAction;
  slider.inputMask = fui::InputTouch | fui::InputDrag;
  const int16_t sideGap = static_cast<int16_t>(stepWidth + theme.spaceSm);
  fui::slider(screen.frame(), row.inset(fui::Insets{0, sideGap, 0, sideGap}), slider);
}

void FrontlightPanelActivity::render(RenderLock&&) {
  // Overlay drop-down: keep the framebuffer content (the reader/menu we opened
  // over) intact below the panel; only the top third is repainted. No full
  // clearScreen — same as the theme popups draw over the current frame.
  panelBottom = computePanelBottom();
  uiReady = false;
  app.render();
  uiReady = true;
  drawHeader();

  // The dialog is the topmost layer.
  if (optionPopup.isActive()) optionPopup.render(renderer);

  renderer.displayBuffer();
}
