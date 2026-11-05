#include "FrontlightPanelActivity.h"

#include <CrossInkHalFrontlight.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "FilenameFontSystem.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "activities/home/BookActions.h"
#include "activities/settings/SettingsActivity.h"
#include "components/DrawerHandle.h"
#include "components/HeaderDate.h"
#include "components/ReaderBookSummary.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/frontlightHeaderIcons.h"
#include "components/icons/listIcons.h"
#include "components/icons/readingStatsIcons.h"
#include "components/icons/tablerIcons.h"
#include "components/icons/touchscreenStateIcons.h"
#include "util/ReaderBookProgress.h"

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
  if (context.activeReaderBook) {
    RenderLock lock(*this);
    filenameFontSystem.ensureLoaded(renderer);
  }

  // The HAL is the live source of truth; SETTINGS only mirrors it for boot.
  brightness = Frontlight.brightness();
  warmth = Frontlight.warmth();
  observedLightOn = Frontlight.isOn();
  initialInversion = SETTINGS.screenInverted;
  initialTouchscreenDisabled = SETTINGS.disableReaderTouchscreen;
  mappedInput.setReaderTouchscreenOverride(true);
  // Picco also locks touch outside readers. Keep this recovery panel usable,
  // preserving an outer recovery session when the drawer is nested.
  previousTouchscreenRecoveryOverride = mappedInput.isTouchscreenRecoveryOverrideEnabled();
  mappedInput.setTouchscreenRecoveryOverride(true);

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
  const bool touchscreenChanged = initialTouchscreenDisabled != static_cast<bool>(SETTINGS.disableReaderTouchscreen);
  const bool frontlightChanged = !context.showReaderDetails &&
                                 (SETTINGS.frontlightBrightness != brightness || SETTINGS.frontlightWarmth != warmth ||
                                  SETTINGS.frontlightOn != (Frontlight.isOn() ? 1 : 0));
  if (frontlightChanged || inversionChanged || touchscreenChanged) {
    if (!context.showReaderDetails) {
      SETTINGS.frontlightBrightness = brightness;
      SETTINGS.frontlightWarmth = warmth;
      SETTINGS.frontlightOn = Frontlight.isOn() ? 1 : 0;
    }
    // The drawer can be opened over a dictionary or another reader child.
    // Find the owning reader so its per-book values stay out of the global file.
    activityManager.persistGlobalSettings();
  }
  mappedInput.setReaderTouchscreenOverride(false);
  mappedInput.setTouchscreenRecoveryOverride(previousTouchscreenRecoveryOverride);
  Activity::onExit();
}

void FrontlightPanelActivity::onExternalFrontlightChange() {
  brightness = Frontlight.brightness();
  warmth = Frontlight.warmth();
  observedLightOn = Frontlight.isOn();
  requestUpdate();
}

void FrontlightPanelActivity::onBrightnessEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<FrontlightPanelActivity*>(user);
  if (event.dragPermille < 0) return;
  self->brightness = percentFromPermille(event.dragPermille);
  Frontlight.setBrightness(self->brightness);
  // Adjusting brightness while off is an obvious "I want light" intent.
  Frontlight.setOn(true);
  self->observedLightOn = true;
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
  Frontlight.setOn(true);
  observedLightOn = true;
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
  Frontlight.setOn(!Frontlight.isOn());
  observedLightOn = Frontlight.isOn();
  requestUpdate();
}

void FrontlightPanelActivity::toggleReaderTouchscreen() {
  SETTINGS.disableReaderTouchscreen = SETTINGS.disableReaderTouchscreen ? 0 : 1;
  BookActions::showTouchscreenToast(renderer, activityManager.isReaderActivity(),
                                    BookActions::TouchscreenToggleSource::FrontlightDrawer);
  requestUpdate();
}

void FrontlightPanelActivity::close() {
  FrontlightPanelResult result;
  result.state = drawerState;
  result.activeEpub = context.activeEpub;
  result.ttfRenderingChanged = ttfRenderingChanged;
  result.bookPath = context.bookPath;
  setResult(ActivityResult(std::move(result)));
  finish();
}

void FrontlightPanelActivity::openReadingStats() {
  if (!context.showReadingStatsAction || !SETTINGS.shouldTrackReadingStats()) return;
  if (!context.readingStatsActivity && context.sourceActivity) {
    context.readingStatsActivity = context.sourceActivity->createFrontlightReadingStatsActivity();
  }
  if (!context.readingStatsActivity) return;
  startActivityForResult(std::move(context.readingStatsActivity), [this](const ActivityResult&) { close(); });
}

void FrontlightPanelActivity::openGlobalSettings() {
  // A drawer over Settings must not end the outer screen's edit session.
  const bool startedGlobalEdit = activityManager.beginGlobalSettingsEdit();
  auto settings = makeUniqueNoThrow<SettingsActivity>(renderer, mappedInput, true, true);
  if (!settings) {
    LOG_ERR("LIGHT", "OOM opening Settings from frontlight panel");
    if (startedGlobalEdit) activityManager.endGlobalSettingsEdit();
    return;
  }
  startActivityForResult(std::move(settings), [this, startedGlobalEdit](const ActivityResult& result) {
    if (const auto* options = std::get_if<TtfRenderOptionsResult>(&result.data)) {
      ttfRenderingChanged = ttfRenderingChanged || options->activeFamilyChanged;
    }
    if (startedGlobalEdit) activityManager.endGlobalSettingsEdit();
    close();
  });
}

void FrontlightPanelActivity::openSyncDialog() {
  static constexpr std::array<StrId, 3> OPTIONS = {StrId::STR_SYNC_BOOK, StrId::STR_NEARBY_POSITION_SYNC,
                                                   StrId::STR_SEND_NEARBY_BOOK};
  drawerState.syncDialogOpen = true;
  const bool canSyncBookProgress = FsHelpers::hasEpubExtension(context.bookPath);
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
  if (index == 0 && !context.showReadingStatsAction) return;
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
  if (!context.showReaderDetails && observedLightOn != Frontlight.isOn()) {
    observedLightOn = Frontlight.isOn();
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
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  return Rect{header.x + header.width - HEADER_BUTTON_WIDTH, header.y, HEADER_BUTTON_WIDTH, header.height};
}

int FrontlightPanelActivity::computePanelBottom() {
  // Mirror buildPanelScreen's takeTop/spacer sequence so the frame, content
  // margin, and dismiss threshold land on the same edge.
  const auto tokens = uiThemeTokens(uiTarget);
  const int16_t lh = uiTarget.lineHeight(tokens.bodyText.font);
  int y = TouchHeaderBackButton::contentTop(renderer, mappedInput);
  showChapterLine = showsBookProgress() && !context.bookDetails.chapter.empty();
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
  // The warmth row adds one row, its label line, and one gap of each size.
  const int warmthRows = static_cast<int>(Frontlight.hasColorTemperature());
  const int rowCount = 2 + warmthRows;
  const int largeGapCount = 3 + warmthRows;
  const int smallGapCount = 1 + warmthRows;
  const auto sheet = frontlightSheetProps();
  const auto safe = uiTarget.deviceContext().safeArea;
  const int maxBottom = renderer.getScreenHeight() - safe.bottom;
  const auto bottom = [&] {
    return y + bookSummaryHeight() + rowCount * panelRowHeight + warmthRows * lh + largeGapCount * panelSpaceLg +
           smallGapCount * panelSpaceSm + ACTION_BAR_HEIGHT + DrawerHandle::bandHeight(sheet);
  };
  const int minRowHeight = std::max<int>(tokens.minTouchSize, lh);
  const auto fit = [&] {
    panelRowHeight = tokens.rowHeight;
    panelSpaceSm = tokens.spaceSm;
    panelSpaceLg = tokens.spaceLg;
    // Preserve Large text, the action bar, and the close handle. Spend less on
    // blank spacing first, then on the two-line list padding these single-line
    // controls inherited from the shared theme.
    while (bottom() > maxBottom && panelSpaceLg > 2) --panelSpaceLg;
    while (bottom() > maxBottom && panelSpaceSm > 2) --panelSpaceSm;
    while (bottom() > maxBottom && panelRowHeight > minRowHeight) --panelRowHeight;
  };
  fit();
  // The chapter line is the only optional content; give it up before the
  // handle would be pushed off screen.
  if (bottom() > maxBottom && showChapterLine) {
    showChapterLine = false;
    fit();
  }
  return std::min(bottom(), maxBottom);
}

void FrontlightPanelActivity::prepareReaderDetailsLayout() {
  readerTitleLines.clear();
  if (!context.showReaderDetails || context.bookDetails.title.empty()) return;
  const auto tokens = uiThemeTokens(uiTarget);
  const int16_t sideInset = static_cast<int16_t>(tokens.spaceLg * 2);
  const int width = std::max<int>(1, renderer.getScreenWidth() - sideInset * 2);
  readerTitleLines = renderer.wrappedText(renderer.filenameFontId(uiScaleSpec().titleFontId),
                                          context.bookDetails.title.c_str(), width, 2, EpdFontFamily::BOLD);
}

void FrontlightPanelActivity::drawReaderDetails(fui::Screen<20>& screen) {
  FilenameUiFontScope fonts(uiTarget, renderer);
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
    const std::string visible = renderer.truncatedText(renderer.filenameFontId(uiScaleSpec().bodyFontId), subtitle,
                                                       screen.body().width - sideInset.left - sideInset.right);
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
  const auto& theme = screen.theme();
  const fui::SheetProps sheet = frontlightSheetProps();
  const auto safe = screen.device().safeRect();
  const fui::Rect sheetRect{safe.x, safe.y, safe.width,
                            static_cast<int16_t>(std::max(0, std::min<int>(panelBottom, safe.bottom()) - safe.y))};
  fui::sheet(screen.frame(), sheetRect, sheet);
  // Body spans from below the header to the sheet's content edge. The grabber
  // owns the remaining band at the bottom of this top-anchored drawer.
  const fui::Rect sheetContent = fui::sheetContentRect(sheetRect, sheet);
  drawerHandleRect = DrawerHandle::registerTap(screen.frame(), sheetContent, sheet, ACTION_DISMISS);
  const int16_t bottomInset = static_cast<int16_t>(renderer.getScreenHeight() - sheetContent.bottom());
  setUiContentMargin(
      screen, renderer,
      fui::Insets{static_cast<int16_t>(TouchHeaderBackButton::contentTop(renderer, mappedInput) + bookSummaryHeight()),
                  0, bottomInset, 0});

  const fui::Rect actionBar = screen.takeBottom(ACTION_BAR_HEIGHT);
#ifdef SIMULATOR
  simulatorActionBarTop = actionBar.y;
#endif
  const bool showStats = context.showReadingStatsAction;
  const int16_t slotCount = showStats ? 5 : 4;
  const int16_t slotWidth = static_cast<int16_t>(actionBar.width / slotCount);
  const std::array<fui::BitmapRef, 5> icons = {
      fui::bitmapFromIcon(icon_reading_stats_24), fui::bitmapFromIcon(icon_transfer_24),
      fui::bitmapFromIcon(icon_tabler_moon_filled_24), fui::bitmapFromIcon(icon_sliders_horizontal_24),
      fui::bitmapFromIcon(SETTINGS.disableReaderTouchscreen ? icon_device_tablet_off_24 : icon_device_tablet_24)};
  for (int16_t slotIndex = 0; slotIndex < slotCount; ++slotIndex) {
    const int16_t i = showStats ? slotIndex : slotIndex + 1;
    const int16_t x = static_cast<int16_t>(actionBar.x + slotIndex * slotWidth);
    const int16_t width = slotIndex == slotCount - 1 ? static_cast<int16_t>(actionBar.right() - x) : slotWidth;
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
  const int16_t rowH = panelRowHeight;
  const fui::Insets sideInset{0, static_cast<int16_t>(theme.spaceLg * 2), 0, static_cast<int16_t>(theme.spaceLg * 2)};
  char line[48];

  screen.spacer(panelSpaceLg);

  // Header row: "Brightness NN%" on the left, a tappable bulb icon on the right
  // that toggles the light — `lightbulb` when on, `lightbulb-off` when off (the
  // icon itself is the state indicator). Sharing a row with the label frees the
  // whole bottom toggle row, shrinking the panel.
  const fui::Rect headerRow = screen.takeTop(rowH, panelSpaceSm).inset(sideInset);
  snprintf(line, sizeof(line), "%s  %u%%", tr(STR_BRIGHTNESS), static_cast<unsigned>(brightness));
  const fui::BitmapRef lightIcon = fui::bitmapFromIcon(Frontlight.isOn() ? icon_lightbulb_28 : icon_lightbulb_off_28);
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
  const int16_t hitW = static_cast<int16_t>(iconW + panelSpaceLg * 4);
  const fui::Rect hitRect{static_cast<int16_t>(headerRow.right() - hitW),
                          static_cast<int16_t>(headerRow.y - panelSpaceSm),
                          static_cast<int16_t>(hitW + sideInset.right), static_cast<int16_t>(rowH + panelSpaceSm * 2)};
  screen.frame().hit(hitRect, ACTION_TOGGLE);
  screen.target().bitmap(iconRect, lightIcon, fui::BitmapMode::Center);

  addStepSlider(screen, screen.takeTop(rowH, panelSpaceLg).inset(sideInset), brightness, ACTION_BRIGHTNESS,
                ACTION_BRIGHTNESS_STEP);

  if (Frontlight.hasColorTemperature()) {
    snprintf(line, sizeof(line), "%s  %u%%", tr(STR_WARMTH), static_cast<unsigned>(warmth));
    screen.target().text(screen.takeTop(lh, panelSpaceSm).inset(sideInset), line, theme.bodyText);
    addStepSlider(screen, screen.takeTop(rowH, panelSpaceLg).inset(sideInset), warmth, ACTION_WARMTH,
                  ACTION_WARMTH_STEP);
  }

  screen.spacer(panelSpaceLg);
#ifdef SIMULATOR
  simulatorContentBottom = screen.contentRect().y;
#endif
}

void FrontlightPanelActivity::drawHeader() {
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);

  char date[32] = {};
  const char* title = context.showReaderDetails ? "" : tr(STR_FRONTLIGHT);
  int titleFontId = UI_12_FONT_ID;
  if (context.showReaderDetails) {
    if (formatHeaderDateText(date, sizeof(date))) title = date;
    titleFontId = UI_10_FONT_ID;
  } else if (context.activeReaderBook && !context.bookTitle.empty()) {
    title = context.bookTitle.c_str();
    titleFontId = renderer.filenameFontId(UI_10_FONT_ID);
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
    const std::string visibleTitle = renderer.truncatedText(titleFontId, title, titleWidth, EpdFontFamily::BOLD);
    renderer.drawText(titleFontId, titleX, titleY, visibleTitle.c_str(), true, EpdFontFamily::BOLD);
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

bool FrontlightPanelActivity::showsBookProgress() const {
  return context.activeReaderBook && !context.showReaderDetails && !context.bookDetails.title.empty();
}

const char* FrontlightPanelActivity::bookSummaryChapter() const {
  return showChapterLine ? context.bookDetails.chapter.c_str() : nullptr;
}

int FrontlightPanelActivity::bookSummaryHeight() const {
  return showsBookProgress() ? ReaderBookSummary::height(renderer, bookSummaryChapter()) : 0;
}

void FrontlightPanelActivity::drawBookProgress() {
  if (!showsBookProgress()) return;
  const int y = TouchHeaderBackButton::contentTop(renderer, mappedInput);
  char progress[96];
  formatReaderBookProgress(progress, sizeof(progress), context.bookDetails.chapterPage,
                           context.bookDetails.chapterPageCount, context.bookDetails.chapterPageCountEstimated,
                           context.bookDetails.progressPercent);
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  ReaderBookSummary::draw(renderer, Rect{safe.x, y, safe.width, 0}, bookSummaryChapter(), progress);
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
  // over) intact below the panel; only the fitted panel is repainted. No full
  // clearScreen — same as the theme popups draw over the current frame.
  panelBottom = computePanelBottom();
  uiReady = false;
  renderUiApp(app, uiTarget);
  uiReady = true;
  drawHeader();
  drawBookProgress();

  // The dialog is the topmost layer.
  if (optionPopup.isActive()) optionPopup.render(renderer);

  renderer.displayBuffer();
}

#ifdef SIMULATOR
freeink::ui::Rect FrontlightPanelActivity::simulatorQuickActionRect(const int index) const {
  freeink::ui::Interaction hit;
  for (int y = simulatorActionBarTop; y < renderer.getScreenHeight(); y += 4) {
    for (int x = 0; x < renderer.getScreenWidth(); x += 4) {
      if (app.hitPublished(x, y, ACTION_QUICK, hit) && hit.value == index) return hit.rect;
    }
  }
  return {};
}
#endif
