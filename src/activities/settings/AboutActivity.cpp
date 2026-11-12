#include "AboutActivity.h"

#include <AppVersion.h>
#include <I18n.h>
#include <Memory.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "SupportInfoExport.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/TouchActionButtons.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;
namespace {
enum Row {
  Device,
#if FREEINK_DEVICE_EEGO_A4
  Uid,
#endif
  Firmware,
  Build,
  Chip,
  Cpu,
  Flash,
  Display,
  Resolution,
  Touch,
  FrontlightRow,
  Rtc,
  ImuRow,
  SdTransport,
  SdCapacity,
  Psram,
  InternalFree,
  InternalMinimum,
  InternalLargest,
  PsramFree,
  PsramLargest,
  Uptime,
  Reset,
  Sdk,
  RowCount
};
constexpr StrId labels[] = {StrId::STR_ABOUT_DEVICE,
#if FREEINK_DEVICE_EEGO_A4
                            StrId::STR_ABOUT_UID,
#endif
                            StrId::STR_ABOUT_FIRMWARE,
                            StrId::STR_ABOUT_BUILD,
                            StrId::STR_ABOUT_CHIP,
                            StrId::STR_ABOUT_CPU,
                            StrId::STR_ABOUT_FLASH,
                            StrId::STR_ABOUT_DISPLAY,
                            StrId::STR_ABOUT_RESOLUTION,
                            StrId::STR_ABOUT_TOUCH,
                            StrId::STR_FRONTLIGHT,
                            StrId::STR_ABOUT_RTC,
                            StrId::STR_ABOUT_IMU,
                            StrId::STR_ABOUT_SD_TRANSPORT,
                            StrId::STR_ABOUT_SD_CAPACITY,
                            StrId::STR_ABOUT_PSRAM,
                            StrId::STR_ABOUT_INTERNAL_FREE,
                            StrId::STR_ABOUT_INTERNAL_MINIMUM,
                            StrId::STR_ABOUT_INTERNAL_LARGEST,
                            StrId::STR_ABOUT_PSRAM_FREE,
                            StrId::STR_ABOUT_PSRAM_LARGEST,
                            StrId::STR_ABOUT_UPTIME,
                            StrId::STR_ABOUT_RESET,
                            StrId::STR_ABOUT_SDK};
static_assert(sizeof(labels) / sizeof(labels[0]) == RowCount);
const char* presenceText(HalDeviceInfo::Presence value) {
  switch (value) {
    case HalDeviceInfo::Presence::Absent:
      return tr(STR_ABOUT_NOT_PRESENT);
    case HalDeviceInfo::Presence::Available:
      return tr(STR_ABOUT_AVAILABLE);
    case HalDeviceInfo::Presence::Unavailable:
      return tr(STR_UNAVAILABLE);
    case HalDeviceInfo::Presence::Simulated:
      return tr(STR_ABOUT_SIMULATED);
  }
  return tr(STR_UNAVAILABLE);
}
}  // namespace

AboutActivity::AboutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("About", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void AboutActivity::onEnter() {
  Activity::onEnter();
  snapshot = HalDeviceInfo::capture();
#if FREEINK_DEVICE_EEGO_A4
  HalDeviceInfo::readEegoUid(uid);
#endif
  LOG_DBG("ABOUT", "Activity allocation: %u bytes; snapshot: %u bytes", static_cast<unsigned>(sizeof(*this)),
          static_cast<unsigned>(sizeof(snapshot)));
  applySharedUiTheme(app, uiTarget);
  app.on(1, &AboutActivity::onExport, this);
  app.setScreen(&AboutActivity::aboutScreen, this);
  requestUpdate();
}

void AboutActivity::loop() {
  RenderLock lock(*this);  // Protect viewport state shared with the render task.
  if (scopePopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (uiReady) {
    auto input = touchSnapshotFrom(mappedInput);
#if CROSSINK_APP_CAP_TOUCH
    // A completed swipe cancels its button press even if a touch backend also
    // reports a tap at the contact's starting point (the simulator does this).
    if (mappedInput.wasSwipe() != MappedInputManager::SwipeDir::None) {
      input.touchPressed = false;
      input.touchReleased = true;
      input.touchX = -1;
      input.touchY = -1;
    }
#endif
    if (input.touchPressed || input.touchReleased) {
      const bool wasTouchActive = app.touchActive();
      const auto event = app.route(input);
      if (app.invalidated() || wasTouchActive != app.touchActive()) requestUpdate();
      if (event) return;
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    chooseExportScope();
    return;
  }
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }
  const auto scroll = [this](int delta) {
    const int next = scrollListBy(topIndex, delta, visibleRows, RowCount);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
  };
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    scroll(swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows);
    return;
  }
  // Whole-page navigation fires once on release; held buttons must not skip diagnostics.
  buttonNavigator.onNextRelease([&] { scroll(visibleRows); });
  buttonNavigator.onPreviousRelease([&] { scroll(-visibleRows); });
}

void AboutActivity::onExport(const fui::ActionEvent&, void* user) {
  auto& self = *static_cast<AboutActivity*>(user);
  self.app.clearTapFlash();
  self.chooseExportScope();
}

void AboutActivity::chooseExportScope() {
  const StrId choices[] = {StrId::STR_CANCEL, StrId::STR_SUPPORT_DEVICE_ONLY, StrId::STR_SUPPORT_INCLUDE_EPUB};
  scopePopup.show(StrId::STR_SUPPORT_SCOPE, choices, SupportInfoExport::lastOpenedEpubAvailable() ? 3 : 2, 0,
                  [this](int index) {
                    if (index > 0) confirmExport(index == 2);
                  });
  requestUpdate();
}

void AboutActivity::confirmExport(bool includeBook) {
  auto confirm = makeUniqueNoThrow<ConfirmationActivity>(
      renderer, mappedInput, includeBook ? tr(STR_SUPPORT_CONFIRM_BOOK) : tr(STR_SUPPORT_CONFIRM_DEVICE),
      tr(STR_SUPPORT_EXCLUSIONS), true);
  if (!confirm) {
    LOG_ERR("SUPPORT", "OOM allocating export confirmation");
    exportStatus = StrId::STR_SUPPORT_FAILED;
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(confirm), [this, includeBook](const ActivityResult& result) {
    if (!result.isCancelled) {
      const auto saved = SupportInfoExport::save(includeBook);  // SD work must not hold the render mutex.
      RenderLock lock(*this);
      switch (saved) {
        case SupportInfo::Result::Saved:
          exportStatus = StrId::STR_SUPPORT_SAVED;
          break;
        case SupportInfo::Result::SavedBackupRetained:
          exportStatus = StrId::STR_SUPPORT_SAVED_BACKUP;
          break;
        case SupportInfo::Result::RecoveryRequired:
          exportStatus = StrId::STR_SUPPORT_RECOVERY;
          break;
        default:
          exportStatus = StrId::STR_SUPPORT_FAILED;
          break;
      }
      requestUpdate();
    } else {
      RenderLock lock(*this);
      requestUpdate();
    }
  });
}

void AboutActivity::provideRow(void* user, uint16_t index, fui::ListItem& item) {
  auto& self = *static_cast<AboutActivity*>(user);
  const auto& s = self.snapshot;
  auto* buf = self.valueBuffer;
  const auto size = sizeof(self.valueBuffer);
  item.label = I18N.get(labels[index]);
#ifdef SIMULATOR
  if (index == Device && self.simulatorHeading) item.label = self.simulatorHeading;
#endif
  item.subtitle = tr(STR_NOT_SUPPORTED);
  auto kib = [&](uint32_t bytes) {
    snprintf(buf, size, "%lu KiB", static_cast<unsigned long>(bytes / 1024));
    item.subtitle = buf;
  };
  switch (index) {
    case Device:
      item.subtitle = s.device;
      break;
#if FREEINK_DEVICE_EEGO_A4
    case Uid:
      item.subtitle = self.uid[0] ? self.uid : tr(STR_UNAVAILABLE);
      break;
#endif
    case Firmware:
      item.subtitle = AppVersion::version();
      break;
    case Build:
      snprintf(buf, size, "%s / %s / %s", CROSSINK_FIRMWARE_DEVICE_TYPE, AppVersion::gitSha(),
               AppVersion::gitDirtyFlag()[0] == '1'   ? tr(STR_ABOUT_MODIFIED)
               : AppVersion::gitDirtyFlag()[0] == '0' ? tr(STR_ABOUT_CLEAN)
                                                      : tr(STR_UNAVAILABLE));
      item.subtitle = buf;
      break;
    case Chip:
      if (!s.simulated) {
        snprintf(buf, size, "%s / %u", s.chip, s.chipRevision);
        item.subtitle = buf;
      }
      break;
    case Cpu:
      if (!s.simulated) {
        snprintf(buf, size, tr(STR_ABOUT_CPU_FORMAT), s.cores, s.cpuMHz);
        item.subtitle = buf;
      }
      break;
    case Flash:
      if (!s.simulated) kib(s.flashBytes);
      break;
    case Display:
      if (s.displayController) item.subtitle = s.displayController;
      break;
    case Resolution:
      snprintf(buf, size, "%u x %u", s.width, s.height);
      item.subtitle = buf;
      break;
    case Touch:
      item.subtitle = presenceText(s.touch);
      if (s.touchController) {
        snprintf(buf, size, "%s / %s", s.touchController, item.subtitle);
        item.subtitle = buf;
      }
      break;
    case FrontlightRow:
      item.subtitle = presenceText(s.frontlight);
      break;
    case Rtc:
      item.subtitle = presenceText(s.rtc);
      break;
    case ImuRow:
      item.subtitle = presenceText(s.imu);
      break;
    case SdTransport:
      item.subtitle = s.simulated ? tr(STR_ABOUT_SIMULATED) : s.sdmmc ? "SDMMC" : "SPI";
      break;
    case SdCapacity:
      if (s.simulated && s.sdReady)
        item.subtitle = tr(STR_ABOUT_SIMULATED);
      else if (!s.sdReady || !s.sdBytes)
        item.subtitle = tr(STR_UNAVAILABLE);
      else {
        snprintf(buf, size, "%llu MiB", static_cast<unsigned long long>(s.sdBytes / (1024 * 1024)));
        item.subtitle = buf;
      }
      break;
    case Psram:
      if (!s.simulated) {
        if (s.psramTotal)
          kib(s.psramTotal);
        else
          item.subtitle = presenceText(s.psram);
      }
      break;
    case InternalFree:
      if (!s.simulated) kib(s.internalFree);
      break;
    case InternalMinimum:
      if (!s.simulated) kib(s.internalMinimum);
      break;
    case InternalLargest:
      if (!s.simulated) kib(s.internalLargest);
      break;
    case PsramFree:
      if (!s.simulated) {
        if (s.psramTotal)
          kib(s.psramFree);
        else
          item.subtitle = presenceText(s.psram);
      }
      break;
    case PsramLargest:
      if (!s.simulated) {
        if (s.psramTotal)
          kib(s.psramLargest);
        else
          item.subtitle = presenceText(s.psram);
      }
      break;
    case Uptime:
      snprintf(buf, size, tr(STR_ABOUT_UPTIME_FORMAT), static_cast<unsigned long>(s.uptimeSeconds));
      item.subtitle = buf;
      break;
    case Reset:
      if (!s.simulated) {
        snprintf(buf, size, "%u", s.resetReason);
        item.subtitle = buf;
      }
      break;
    case Sdk:
      if (s.sdk) item.subtitle = s.sdk;
      break;
  }
}

void AboutActivity::aboutScreen(UiApp::ScreenType& screen, void* user) {
  auto& self = *static_cast<AboutActivity*>(user);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header = TouchHeaderBackButton::headerRect(self.renderer, self.mappedInput);
  const auto safe = screen.frame().safeRect();
  const Rect hints =
      UITheme::getInstance().getScreenSafeArea(self.renderer, !self.mappedInput.hasTouchHardware(), false);
  const int contentTop = std::max({static_cast<int>(safe.y), header.y + header.height, hints.y});
  // Hint strips rotate with the physical buttons; intersect them with bezel-safe bounds.
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(contentTop - safe.y),
                                      static_cast<int16_t>(std::max<int>(0, safe.right() - (hints.x + hints.width))),
                                      static_cast<int16_t>(std::max<int>(0, safe.bottom() - (hints.y + hints.height))),
                                      static_cast<int16_t>(std::max<int>(0, hints.x - safe.x))});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  self.exportButtonRect = Rect();
  if (self.mappedInput.hasTouchHardware()) {
    const int actionHeight = std::max<int>(TouchActionButtons::kDefaultHeight, screen.theme().rowHeight);
    const auto sideMargin = static_cast<int16_t>(metrics.listSidePadding);
    const auto actionRect = screen.take(fui::LayoutAnchor::Top, actionHeight, screen.theme().spaceSm)
                                .inset(fui::Insets{0, sideMargin, 0, sideMargin});
    const auto actions = TouchActionButtons::vertical(
        Rect{actionRect.x, actionRect.y, actionRect.width, actionRect.height}, 1, actionHeight, 0);
    self.exportButtonRect = actions.buttons[0];
    // Register precisely the existing action's visual bounds; FreeInkUI retains
    // press feedback and release/drag-off routing without a second touch handler.
    screen.frame().hit(actionRect, 1, 0, fui::InputTouch);
    const auto state = screen.frame().stateFor(1);
    const int selected = fui::hasState(state, fui::StateActive) || fui::hasState(state, fui::StateFocused) ? 0 : -1;
    auto actionText = screen.theme().bodyText;
    actionText.bold = true;
    actionText.maxLines = 1;
    // Fit localized labels and the existing result messages without render-loop
    // strings. The actual full-width action helper still paints the whole button.
    fui::layoutText(self.uiTarget, actionRect.inset(fui::Insets{0, screen.theme().spaceSm, 0, screen.theme().spaceSm}),
                    I18N.get(self.exportStatus), actionText, [&](const char* line, fui::Rect) {
                      const char* actionLabels[] = {line};
                      TouchActionButtons::draw(self.renderer, actions, actionLabels, 0, selected,
                                               uiScaleSpec().bodyFontId);
                    });
  }
  fui::ListProps props;
  props.count = RowCount;
  props.rowProvider = &AboutActivity::provideRow;
  props.rowProviderCtx = &self;
  props.inputMask = fui::InputNone;
  props.labelText = screen.theme().bodyText;
  props.labelText.bold = true;
  props.labelText.maxLines = 2;
  props.subtitleText = screen.theme().smallText;
  props.subtitleText.bold = false;
  props.subtitleText.maxLines = 1;
  // Paging uses a fixed row count. Reserve two heading lines plus the value
  // even when only a translated heading on a later page needs that space.
  props.rowPaddingY = screen.theme().spaceSm;
  props.rowHeight = std::max<int>(uiListRowHeight(screen.theme(), UiListRowType::WithSubtitle),
                                  self.uiTarget.lineHeight(props.labelText.font) * 2 +
                                      self.uiTarget.lineHeight(props.subtitleText.font) + props.rowPaddingY * 2);
  self.visibleRows =
      std::max<int>(1, configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle));
  self.topIndex = scrollListBy(self.topIndex, 0, self.visibleRows, RowCount);
  props.topIndex = self.topIndex;
  screen.list(props);
}

void AboutActivity::render(RenderLock&&) {
  if (scopePopup.processRender(renderer, mappedInput)) return;
  renderer.clearScreen();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware())
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_ABOUT), false);
  else
    GUI.drawHeader(renderer, header, tr(STR_ABOUT));
  uiReady = false;
  renderUiApp(app, uiTarget);
  uiReady = true;
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SUPPORT_EXPORT_SHORT),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

#ifdef SIMULATOR
int AboutActivity::simulatorRowCount() const { return RowCount; }
#endif
