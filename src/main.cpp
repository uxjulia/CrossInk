#include <Arduino.h>
#include <BoardConfig.h>
#include <CrossInkHalFrontlight.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FreeInkUIGfxRenderer.h>
#include <FreeInkUIIcon.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SPI.h>
#if !defined(SIMULATOR) && !FREEINK_MCU_C3
#include <XteinkDetect.h>
#endif
#include <builtinFonts/all.h>
#include <uzlib.h>

#include "AppCapabilities.h"

#ifdef SIMULATOR
using esp_reset_reason_t = int;
using esp_sleep_wakeup_cause_t = int;
enum : int {
  ESP_RST_UNKNOWN = 0,
  ESP_RST_POWERON,
  ESP_RST_EXT,
  ESP_RST_SW,
  ESP_RST_PANIC,
  ESP_RST_INT_WDT,
  ESP_RST_TASK_WDT,
  ESP_RST_WDT,
  ESP_RST_DEEPSLEEP,
  ESP_RST_BROWNOUT,
  ESP_RST_SDIO,
  ESP_RST_USB,
  ESP_RST_JTAG,
  ESP_RST_EFUSE,
  ESP_RST_PWR_GLITCH,
  ESP_RST_CPU_LOCKUP
};
enum : int {
  ESP_SLEEP_WAKEUP_UNDEFINED = 0,
  ESP_SLEEP_WAKEUP_ALL,
  ESP_SLEEP_WAKEUP_EXT0,
  ESP_SLEEP_WAKEUP_EXT1,
  ESP_SLEEP_WAKEUP_TIMER,
  ESP_SLEEP_WAKEUP_TOUCHPAD,
  ESP_SLEEP_WAKEUP_ULP,
  ESP_SLEEP_WAKEUP_GPIO,
  ESP_SLEEP_WAKEUP_UART,
  ESP_SLEEP_WAKEUP_WIFI,
  ESP_SLEEP_WAKEUP_COCPU,
  ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG,
  ESP_SLEEP_WAKEUP_BT
};
inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_UNKNOWN; }
inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause() { return ESP_SLEEP_WAKEUP_UNDEFINED; }
#else
#include <esp_sleep.h>
#include <esp_system.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#ifndef SIMULATOR
#include <nvs.h>
#endif

#include "AppVersion.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GlobalActions.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/boot_sleep/ImageFolderIndex.h"
#include "activities/home/BookActions.h"
#include "activities/reader/KOReaderSyncActivity.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "activities/reader/StatsBackup.h"
#include "activities/settings/FontDownloadActivity.h"
#include "activities/settings/KOReaderAuthActivity.h"
#include "activities/settings/KOReaderSettingsActivity.h"
#include "activities/settings/OtaUpdateActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "components/icons/tablerFilledIcons.h"
#include "fontIds.h"
#include "network/UsbSerialFileTransfer.h"
#ifdef SIMULATOR
#include <SimulatorLifecycle.h>

#include "simulator/SimulatorHomeKeyInput.h"
#include "simulator/SimulatorSmokeTest.h"
#endif
#include "images/LoadingIcon.h"
#include "util/BatteryDiagnosticLog.h"
#include "util/ButtonNavigator.h"
#include "util/ButtonShortcutController.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"
#include "util/FrontlightSchedule.h"
#include "util/ScreenshotUtil.h"
#include "util/SleepWakePolicy.h"

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
DictionaryRegistry dictionaryRegistry;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;
static ButtonShortcutController buttonShortcutController;
static unsigned long lastX4ProHomeKeyTapAt = 0;
static bool x4ProHomeKeyTapPending = false;
// A held power button can span deep-sleep wake and the first main-loop frame.
// Do not treat that wake gesture as an in-session shortcut until it has been released.
static bool powerButtonReleasedSinceWake = false;
// Wake can continue once its hold has been verified. Swallow the release that
// ends that wake gesture so it cannot become an in-session button action.
static bool wakePowerReleasePending = false;

namespace {
constexpr unsigned long X4PRO_HOME_KEY_DOUBLE_TAP_MS = 300;

struct QuickLockBadgeBackdrop {
  static constexpr int SIZE = 40;
  // A 40 px square can straddle at most six 1-bit framebuffer bytes per row.
  static constexpr size_t MAX_BYTES = 6 * SIZE;

  std::array<uint8_t, MAX_BYTES> pixels{};
  int x = 0;
  int y = 0;
  size_t size = 0;
  GfxRenderer::Orientation orientation = GfxRenderer::Portrait;
  bool valid = false;

  void save(GfxRenderer& renderer, const int savedX, const int savedY) {
    x = savedX;
    y = savedY;
    orientation = renderer.getOrientation();
    size = renderer.getRegionByteSize(x, y, SIZE, SIZE);
    valid = size > 0 && size <= pixels.size() && renderer.copyRegionToBuffer(x, y, SIZE, SIZE, pixels.data(), size);
  }

  bool restore(GfxRenderer& renderer) {
    if (!valid || renderer.getOrientation() != orientation) return false;
    valid = false;
    return renderer.copyBufferToRegion(x, y, SIZE, SIZE, pixels.data(), size);
  }
};

QuickLockBadgeBackdrop quickLockBadgeBackdrop;
}  // namespace

static void logBootHeap(const char* stage) {
  LOG_DBG("BOOTMEM", "%s: free=%u maxAlloc=%u", stage, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

// Fonts
EpdFont lexenddeca10RegularFont(&lexenddeca_10_regular);
EpdFont lexenddeca10BoldFont(&lexenddeca_10_bold);
EpdFont lexenddeca10ItalicFont(&lexenddeca_10_italic);
EpdFont lexenddeca10BoldItalicFont(&lexenddeca_10_bolditalic);
EpdFontFamily lexenddeca10FontFamily(&lexenddeca10RegularFont, &lexenddeca10BoldFont, &lexenddeca10ItalicFont,
                                     &lexenddeca10BoldItalicFont);
EpdFont lexenddeca12RegularFont(&lexenddeca_12_regular);
EpdFont lexenddeca12BoldFont(&lexenddeca_12_bold);
EpdFont lexenddeca12ItalicFont(&lexenddeca_12_italic);
EpdFont lexenddeca12BoldItalicFont(&lexenddeca_12_bolditalic);
EpdFontFamily lexenddeca12FontFamily(&lexenddeca12RegularFont, &lexenddeca12BoldFont, &lexenddeca12ItalicFont,
                                     &lexenddeca12BoldItalicFont);
EpdFont lexenddeca14RegularFont(&lexenddeca_14_regular);
EpdFont lexenddeca14BoldFont(&lexenddeca_14_bold);
EpdFont lexenddeca14ItalicFont(&lexenddeca_14_italic);
EpdFont lexenddeca14BoldItalicFont(&lexenddeca_14_bolditalic);
EpdFontFamily lexenddeca14FontFamily(&lexenddeca14RegularFont, &lexenddeca14BoldFont, &lexenddeca14ItalicFont,
                                     &lexenddeca14BoldItalicFont);
EpdFont lexenddeca16RegularFont(&lexenddeca_16_regular);
EpdFont lexenddeca16BoldFont(&lexenddeca_16_bold);
EpdFont lexenddeca16ItalicFont(&lexenddeca_16_italic);
EpdFont lexenddeca16BoldItalicFont(&lexenddeca_16_bolditalic);
EpdFontFamily lexenddeca16FontFamily(&lexenddeca16RegularFont, &lexenddeca16BoldFont, &lexenddeca16ItalicFont,
                                     &lexenddeca16BoldItalicFont);
EpdFont bitter10RegularFont(&bitter_10_regular);
EpdFont bitter10BoldFont(&bitter_10_bold);
EpdFont bitter10ItalicFont(&bitter_10_italic);
EpdFont bitter10BoldItalicFont(&bitter_10_bolditalic);
EpdFontFamily bitter10FontFamily(&bitter10RegularFont, &bitter10BoldFont, &bitter10ItalicFont, &bitter10BoldItalicFont);
EpdFont bitter12RegularFont(&bitter_12_regular);
EpdFont bitter12BoldFont(&bitter_12_bold);
EpdFont bitter12ItalicFont(&bitter_12_italic);
EpdFont bitter12BoldItalicFont(&bitter_12_bolditalic);
EpdFontFamily bitter12FontFamily(&bitter12RegularFont, &bitter12BoldFont, &bitter12ItalicFont, &bitter12BoldItalicFont);
EpdFont bitter14RegularFont(&bitter_14_regular);
EpdFont bitter14BoldFont(&bitter_14_bold);
EpdFont bitter14ItalicFont(&bitter_14_italic);
EpdFont bitter14BoldItalicFont(&bitter_14_bolditalic);
EpdFontFamily bitter14FontFamily(&bitter14RegularFont, &bitter14BoldFont, &bitter14ItalicFont, &bitter14BoldItalicFont);
EpdFont bitter16RegularFont(&bitter_16_regular);
EpdFont bitter16BoldFont(&bitter_16_bold);
EpdFont bitter16ItalicFont(&bitter_16_italic);
EpdFont bitter16BoldItalicFont(&bitter_16_bolditalic);
EpdFontFamily bitter16FontFamily(&bitter16RegularFont, &bitter16BoldFont, &bitter16ItalicFont, &bitter16BoldItalicFont);

EpdFont smallFont(&inter_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&inter_10_regular);
EpdFont ui10BoldFont(&inter_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&inter_12_regular);
EpdFont ui12BoldFont(&inter_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

const char* resetReasonName(const esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_EXT:
      return "EXT";
    case ESP_RST_SW:
      return "SW";
    case ESP_RST_PANIC:
      return "PANIC";
    case ESP_RST_INT_WDT:
      return "INT_WDT";
    case ESP_RST_TASK_WDT:
      return "TASK_WDT";
    case ESP_RST_WDT:
      return "WDT";
    case ESP_RST_DEEPSLEEP:
      return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_SDIO:
      return "SDIO";
    case ESP_RST_USB:
      return "USB";
    case ESP_RST_JTAG:
      return "JTAG";
    case ESP_RST_EFUSE:
      return "EFUSE";
    case ESP_RST_PWR_GLITCH:
      return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP:
      return "CPU_LOCKUP";
    case ESP_RST_UNKNOWN:
    default:
      return "UNKNOWN";
  }
}

const char* wakeupCauseName(const esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return "UNDEFINED";
    case ESP_SLEEP_WAKEUP_ALL:
      return "ALL";
    case ESP_SLEEP_WAKEUP_EXT0:
      return "EXT0";
    case ESP_SLEEP_WAKEUP_EXT1:
      return "EXT1";
    case ESP_SLEEP_WAKEUP_TIMER:
      return "TIMER";
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
      return "TOUCHPAD";
    case ESP_SLEEP_WAKEUP_ULP:
      return "ULP";
    case ESP_SLEEP_WAKEUP_GPIO:
      return "GPIO";
    case ESP_SLEEP_WAKEUP_UART:
      return "UART";
    case ESP_SLEEP_WAKEUP_WIFI:
      return "WIFI";
    case ESP_SLEEP_WAKEUP_COCPU:
      return "COCPU";
    case ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG:
      return "COCPU_TRAP";
    case ESP_SLEEP_WAKEUP_BT:
      return "BT";
    default:
      return "UNKNOWN";
  }
}

const char* wakeupRouteName(const HalGPIO::WakeupReason reason) {
  switch (reason) {
    case HalGPIO::WakeupReason::PowerButton:
      return "PowerButton";
    case HalGPIO::WakeupReason::AfterFlash:
      return "AfterFlash";
    case HalGPIO::WakeupReason::AfterUSBPower:
      return "AfterUSBPower";
    case HalGPIO::WakeupReason::Other:
    default:
      return "Other";
  }
}

void logMemoryStats(const char* phase) {
#if defined(BOARD_HAS_PSRAM)
  LOG_INF("MEM", "%s: heap free=%u total=%u min=%u maxAlloc=%u psram free=%u total=%u min=%u maxAlloc=%u", phase,
          ESP.getFreeHeap(), ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(), ESP.getFreePsram(),
          ESP.getPsramSize(), ESP.getMinFreePsram(), ESP.getMaxAllocPsram());
#else
  LOG_INF("MEM", "%s: heap free=%u total=%u min=%u maxAlloc=%u", phase, ESP.getFreeHeap(), ESP.getHeapSize(),
          ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
#endif
}

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
RTC_NOINIT_ATTR uint32_t silentRebootPayload;
RTC_NOINIT_ATTR uint32_t silentReaderPageBuildMagic;
RTC_NOINIT_ATTR uint32_t silentReaderPageBuildBookHash;
RTC_NOINIT_ATTR uint32_t silentReaderPageBuildPackedTarget;
RTC_NOINIT_ATTR uint32_t silentReaderPageBuildFlags;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;
constexpr uint32_t SILENT_REBOOT_READER_CLEAN_IMAGE_BASE = 1U << 0;
constexpr uint32_t SILENT_REBOOT_FOLLOW_LIGHT_WAKE_POLICY = 1U << 1;
constexpr uint32_t SILENT_READER_PAGE_BUILD_MAGIC = 0xC1EAB017;
constexpr uint32_t SILENT_READER_PAGE_BUILD_AUTO_TURN = 1U << 0;
constexpr uint32_t NETWORK_RENDER_TASK_STACK_BYTES = 8192;
constexpr uint32_t READER_RENDER_TASK_STACK_BYTES = 16384;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
using BootResume = SleepWakePolicy::Resume;

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

static void restartWithSilentToken() {
#ifdef SIMULATOR
  SimulatorLifecycle::setSilentRebootToken(silentRebootMagic, silentRebootTarget, silentRebootPayload);
#endif
  ESP.restart();
}

static uint32_t silentRestartBookHash(const std::string& bookPath) {
  return uzlib_crc32(bookPath.data(), static_cast<unsigned int>(bookPath.size()), 0);
}

static void clearSilentRestartReaderPageBuild() {
  silentReaderPageBuildMagic = 0;
  silentReaderPageBuildBookHash = 0;
  silentReaderPageBuildPackedTarget = 0;
  silentReaderPageBuildFlags = 0;
}

static void silentRestartToHome(const uint32_t payload, const char* const description) {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  clearSilentRestartReaderPageBuild();
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootPayload = payload;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (%s)", description);
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  restartWithSilentToken();
}

void silentRestart() { silentRestartToHome(0, "target=home"); }

void silentRestartAfterNetwork() {
  silentRestartToHome(SILENT_REBOOT_FOLLOW_LIGHT_WAKE_POLICY, "target=home after network");
}

void restartToHomeAfterStorageHandoff() {
  // Keep this distinct from other callers: USB Drive has released raw SD
  // storage and must reboot into Home before any normal filesystem work runs.
  silentRestart();
}

void armSilentRestartReaderPageBuild(const std::string& bookPath, const uint16_t spineIndex, const uint16_t targetPage,
                                     const bool autoPageTurnActive) {
  silentReaderPageBuildBookHash = silentRestartBookHash(bookPath);
  silentReaderPageBuildPackedTarget = (static_cast<uint32_t>(spineIndex) << 16) | targetPage;
  silentReaderPageBuildFlags = autoPageTurnActive ? SILENT_READER_PAGE_BUILD_AUTO_TURN : 0;
  silentReaderPageBuildMagic = SILENT_READER_PAGE_BUILD_MAGIC;
}

bool consumeSilentRestartReaderPageBuild(const std::string& bookPath, uint16_t& spineIndex, uint16_t& targetPage,
                                         bool& autoPageTurnActive) {
  const bool matches = silentReaderPageBuildMagic == SILENT_READER_PAGE_BUILD_MAGIC &&
                       silentReaderPageBuildBookHash == silentRestartBookHash(bookPath);
  const uint32_t packedTarget = silentReaderPageBuildPackedTarget;
  const uint32_t flags = silentReaderPageBuildFlags;
  clearSilentRestartReaderPageBuild();
  if (!matches) {
    autoPageTurnActive = false;
    return false;
  }

  spineIndex = static_cast<uint16_t>(packedTarget >> 16);
  targetPage = static_cast<uint16_t>(packedTarget & 0xFFFFU);
  autoPageTurnActive = (flags & SILENT_READER_PAGE_BUILD_AUTO_TURN) != 0;
  return true;
}

static void silentRestartToReader(const bool cleanImageBaseOnEntry, const bool followsWakeLightPolicy) {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootPayload = (cleanImageBaseOnEntry ? SILENT_REBOOT_READER_CLEAN_IMAGE_BASE : 0) |
                        (followsWakeLightPolicy ? SILENT_REBOOT_FOLLOW_LIGHT_WAKE_POLICY : 0);
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader cleanImageBase=%d wakeLightPolicy=%d)", cleanImageBaseOnEntry ? 1 : 0,
          followsWakeLightPolicy ? 1 : 0);
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  restartWithSilentToken();
}

void silentRestartToReader(const bool cleanImageBaseOnEntry) { silentRestartToReader(cleanImageBaseOnEntry, false); }

void silentRestartToReaderAfterNetwork(const bool cleanImageBaseOnEntry) {
  silentRestartToReader(cleanImageBaseOnEntry, true);
}

void silentRestartToNetwork(const NetworkBootTarget target, const uint32_t payload) {
  if (deepSleepInProgress) return;
  clearSilentRestartReaderPageBuild();
  silentRebootTarget = static_cast<uint32_t>(target);
  silentRebootPayload = payload;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=network/%lu payload=%lu)", static_cast<unsigned long>(silentRebootTarget),
          static_cast<unsigned long>(payload));
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  restartWithSilentToken();
}

void silentRestartToManageFonts() { silentRestartToNetwork(NetworkBootTarget::MANAGE_FONTS); }

static uint32_t encodeKOReaderSyncOrientation(const uint8_t orientation) {
  return orientation < CrossPointSettings::ORIENTATION_COUNT ? static_cast<uint32_t>(orientation) + 1 : 0;
}

static uint8_t decodeKOReaderSyncOrientation(const uint32_t payload) {
  return payload > 0 && payload <= CrossPointSettings::ORIENTATION_COUNT ? static_cast<uint8_t>(payload - 1)
                                                                         : CrossPointSettings::ORIENTATION_COUNT;
}

bool isGlobalPowerButtonAction(const CrossPointSettings::SHORT_PWRBTN action) {
  return isPowerButtonActionAvailableOutsideReader(action);
}

bool startGlobalSyncProgress(const bool networkBootReady, const uint8_t readerOrientation) {
  if (activityManager.hasActivityNamed(KOReaderSyncActivity::NAME)) {
    LOG_DBG("MAIN", "Ignoring KOReader sync shortcut while sync is already active");
    return true;
  }

  if (!KOREADER_STORE.hasCredentials()) {
    if (networkBootReady) return false;
    activityManager.pushActivity(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInputManager));
    return true;
  }

  std::string epubPath = APP_STATE.openEpubPath;
  if (epubPath.empty() || !FsHelpers::hasEpubExtension(epubPath) || !Storage.exists(epubPath.c_str())) {
    if (networkBootReady) return false;
    LOG_DBG("MAIN", "No syncable EPUB open, opening KOReader settings instead");
    activityManager.pushActivity(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInputManager));
    return true;
  }

  if (!networkBootReady) {
    silentRestartToNetwork(NetworkBootTarget::KOREADER_SYNC, encodeKOReaderSyncOrientation(readerOrientation));
    return true;
  }

  const DocumentMatchMethod matchMethod = KOREADER_STORE.getMatchMethod();
  auto syncActivity = makeUniqueNoThrow<KOReaderSyncActivity>(renderer, mappedInputManager, std::move(epubPath),
                                                              matchMethod, readerOrientation);
  if (!syncActivity) {
    LOG_ERR("MAIN", "OOM: KOReader sync activity (free=%u maxAlloc=%u)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return false;
  }
  activityManager.replaceActivity(std::move(syncActivity));
  return true;
}

CrossPointSettings::SHORT_PWRBTN getPowerButtonAction() {
  static bool longPowerButtonHandled = false;

  if (mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    if (longPowerButtonHandled) {
      longPowerButtonHandled = false;
      return CrossPointSettings::SHORT_PWRBTN::IGNORE;
    }

    return mappedInputManager.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()
               ? static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn)
               : static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn);
  }

  if (longPowerButtonHandled || !mappedInputManager.isPressed(MappedInputManager::Button::Power) ||
      mappedInputManager.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()) {
    return CrossPointSettings::SHORT_PWRBTN::IGNORE;
  }

  const auto action = static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn);
  if (!isGlobalPowerButtonAction(action)) {
    return CrossPointSettings::SHORT_PWRBTN::IGNORE;
  }

  longPowerButtonHandled = true;
  return action;
}

void notifyQuickLockChanged(const bool restoringAfterWake = false) {
  const bool locked = buttonShortcutController.isQuickLocked();
  x4ProHomeKeyTapPending = false;
  mappedInputManager.clearInjectedReleases();
  LOG_DBG("MAIN", "Quick Lock %s", locked ? "enabled" : "disabled");
  if (locked) {
    if (!restoringAfterWake) {
      APP_STATE.quickLockRestoreFrontlight = Frontlight.isOn();
    }
    Frontlight.setOn(false);
    activityManager.notifyInputLockChanged(true);
    int top = 0;
    int right = 0;
    int bottom = 0;
    int left = 0;
    renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
    const int x = std::max(left, renderer.getScreenWidth() - right - QuickLockBadgeBackdrop::SIZE);
    const int y = std::max(top, renderer.getScreenHeight() - bottom - QuickLockBadgeBackdrop::SIZE);
    // ActivityManager applies Night Mode at the display boundary, so direct
    // framebuffer writes retain the normal palette.
    constexpr bool background = false;
    constexpr bool foreground = true;
    RenderLock lock;
    quickLockBadgeBackdrop.save(renderer, x, y);
    renderer.fillRect(x, y, QuickLockBadgeBackdrop::SIZE, QuickLockBadgeBackdrop::SIZE, background);
    freeink::ui::GfxRendererTarget target(renderer);
    target.bitmap(freeink::ui::Rect{x + (QuickLockBadgeBackdrop::SIZE - icon_tabler_lock_28.w) / 2,
                                    y + (QuickLockBadgeBackdrop::SIZE - icon_tabler_lock_28.h) / 2,
                                    icon_tabler_lock_28.w, icon_tabler_lock_28.h},
                  freeink::ui::bitmapFromIcon(icon_tabler_lock_28), freeink::ui::BitmapMode::Center,
                  freeink::ui::Paint::solid(foreground ? freeink::ui::Color::Black : freeink::ui::Color::White));
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    bool restoredBadgeBackdrop = false;
    {
      RenderLock lock;
      restoredBadgeBackdrop = quickLockBadgeBackdrop.restore(renderer);
      if (restoredBadgeBackdrop) renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
    if (APP_STATE.quickLockRestoreFrontlight) {
      Frontlight.setOn(true);
      APP_STATE.quickLockRestoreFrontlight = false;
    }
    if (!restoredBadgeBackdrop) (void)activityManager.requestUpdateAndWait();
    activityManager.notifyInputLockChanged(false);
  }
}

bool handleGlobalPowerButtonAction(const CrossPointSettings::SHORT_PWRBTN action,
                                   const QuickLockTrigger quickLockTrigger) {
  switch (action) {
    case CrossPointSettings::SHORT_PWRBTN::SLEEP:
      enterDeepSleep();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK:
      if (quickLockTrigger == QuickLockTrigger::None) {
        LOG_ERR("MAIN", "Quick Lock requested without an input trigger");
        return false;
      }
      buttonShortcutController.toggleQuickLock(millis(), quickLockTrigger,
                                               quickLockTrigger == QuickLockTrigger::LongPower);
      notifyQuickLockChanged();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH: {
      // Reader redraws must replace overlays before the panel refreshes.
      if (activityManager.requestManualReaderRefresh()) {
        return true;
      }
      RenderLock lock;
      renderer.displayBuffer(manualScreenRefreshMode());
      return true;
    }
    case CrossPointSettings::SHORT_PWRBTN::SCREENSHOT: {
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      RenderLock lock;
      ScreenshotUtil::takeScreenshot(renderer);
      return true;
    }
    case CrossPointSettings::SHORT_PWRBTN::SYNC_PROGRESS:
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      return startGlobalSyncProgress();
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      activityManager.goToFileTransfer();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      activityManager.goToCalibreWireless();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      activityManager.goToJoinNetworkFileTransfer();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      activityManager.goToHotspotFileTransfer();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FRONTLIGHT: {
      if (!Frontlight.present()) return false;
      const bool lightOn = !Frontlight.isOn();
      Frontlight.setOn(lightOn);
      SETTINGS.frontlightOn = lightOn ? 1 : 0;
      SETTINGS.saveToFile();
      LOG_INF("LIGHT", "Frontlight toggled %s by shortcut", lightOn ? "on" : "off");
      return true;
    }
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TOUCHSCREEN:
      if (!gpio.hasTouch()) return false;
      SETTINGS.disableReaderTouchscreen = SETTINGS.disableReaderTouchscreen ? 0 : 1;
      SETTINGS.saveToFile();
      LOG_INF("TOUCH", "Reader touchscreen %s by shortcut", SETTINGS.disableReaderTouchscreen ? "disabled" : "enabled");
      {
        RenderLock lock;
        BookActions::drawToast(
            renderer, SETTINGS.disableReaderTouchscreen ? tr(STR_TOUCHSCREEN_DISABLED) : tr(STR_TOUCHSCREEN_ENABLED));
      }
      delay(1000);
      activityManager.requestUpdate();
      return true;
    default:
      return false;
  }
  return false;
}

bool dispatchShortcutAction(const CrossPointSettings::SHORT_PWRBTN action) {
  // An EPUB reader may have a per-book orientation that is restored during
  // teardown. Let it hand off Sync Progress before the global restart drops
  // that transient setting.
  if (action == CrossPointSettings::SHORT_PWRBTN::SYNC_PROGRESS && activityManager.handleShortcutAction(action)) {
    return true;
  }
  return handleGlobalPowerButtonAction(action) || activityManager.handleShortcutAction(action);
}

ButtonShortcutController::ChordAction configuredChordAction() {
  const auto rawAction = SETTINGS.powerChordAction;
  if (rawAction >= CrossPointSettings::POWER_CHORD_ACTION_COUNT) {
    return ButtonShortcutController::ChordAction::Disabled;
  }
  return static_cast<ButtonShortcutController::ChordAction>(rawAction);
}

ButtonShortcutController::ChordAction configuredSideButtonChordAction() {
  const auto rawAction = SETTINGS.sideButtonChordAction;
  if (rawAction >= CrossPointSettings::POWER_CHORD_ACTION_COUNT) {
    return ButtonShortcutController::ChordAction::Disabled;
  }
  return static_cast<ButtonShortcutController::ChordAction>(rawAction);
}

CrossPointSettings::SHORT_PWRBTN chordPowerAction(const ButtonShortcutController::ChordAction action) {
  using Chord = ButtonShortcutController::ChordAction;
  using Power = CrossPointSettings::SHORT_PWRBTN;
  switch (action) {
    case Chord::Sleep:
      return Power::SLEEP;
    case Chord::PageTurn:
      return Power::PAGE_TURN;
    case Chord::PreviousPage:
      return Power::PREVIOUS_PAGE;
    case Chord::ToggleBookmark:
      return Power::TOGGLE_BOOKMARK;
    case Chord::ReadingStats:
      return Power::READING_STATS;
    case Chord::MarkFinished:
      return Power::MARK_FINISHED;
    case Chord::ForceRefresh:
      return Power::FORCE_REFRESH;
    case Chord::ToggleFont:
      return Power::TOGGLE_FONT;
    case Chord::ToggleGuideDots:
      return Power::TOGGLE_GUIDE_DOTS;
    case Chord::ToggleBionicReading:
      return Power::TOGGLE_BIONIC_READING;
    case Chord::CyclePageTurn:
      return Power::CYCLE_PAGE_TURN;
    case Chord::SyncProgress:
      return Power::SYNC_PROGRESS;
    case Chord::NearbyPositionSync:
      return Power::NEARBY_POSITION_SYNC;
    case Chord::FileTransfer:
      return Power::FILE_TRANSFER;
    case Chord::CalibreWireless:
      return Power::CALIBRE_WIRELESS;
    case Chord::JoinNetwork:
      return Power::JOIN_NETWORK;
    case Chord::CreateHotspot:
      return Power::CREATE_HOTSPOT;
    case Chord::ToggleDarkMode:
      return Power::TOGGLE_DARK_MODE;
    case Chord::Footnotes:
      return Power::FOOTNOTES;
    case Chord::FileBrowser:
      return Power::FILE_BROWSER;
    case Chord::CreateClipping:
      return Power::CREATE_CLIPPING;
    case Chord::LookupWord:
      return Power::LOOKUP_WORD;
    case Chord::ToggleHomeButton:
      return Power::TOGGLE_HOME_BUTTON_IN_READER;
    case Chord::QuickActions:
      return Power::QUICK_ACTIONS;
    case Chord::ToggleFrontlight:
      return Power::TOGGLE_FRONTLIGHT;
    case Chord::ToggleTouchscreen:
      return Power::TOGGLE_TOUCHSCREEN;
    default:
      return Power::IGNORE;
  }
}

bool dispatchButtonShortcut(const ButtonShortcutController::Result& result) {
  switch (result.event) {
    case ButtonShortcutController::Event::None:
      return false;
    case ButtonShortcutController::Event::QuickLockChanged:
      notifyQuickLockChanged();
      return true;
    case ButtonShortcutController::Event::Screenshot: {
      RenderLock lock;
      ScreenshotUtil::takeScreenshot(renderer);
      return true;
    }
    case ButtonShortcutController::Event::PageTurn:
      mappedInputManager.injectRelease(MappedInputManager::Button::Right);
      break;
    case ButtonShortcutController::Event::ConfiguredAction:
      return dispatchShortcutAction(chordPowerAction(result.action));
    case ButtonShortcutController::Event::TouchscreenEscapeHatch:
      return activityManager.openReaderSettingsForTouchscreenEscapeHatch();
  }

  activityManager.loop();
  mappedInputManager.clearInjectedReleases();
  return true;
}

namespace {
constexpr uint16_t POST_SLEEP_SCREEN_SETTLE_MS = 500;
constexpr uint8_t TILT_SLEEP_MAX_ATTEMPTS = 3;
constexpr uint16_t TILT_SLEEP_RETRY_DELAY_MS = 10;

void putTiltSensorToSleepForDeepSleep() {
  if (!halTiltSensor.isAvailable()) {
    return;
  }

  for (uint8_t attempt = 0; attempt < TILT_SLEEP_MAX_ATTEMPTS; ++attempt) {
    if (halTiltSensor.deepSleep()) {
      return;
    }
    delay(TILT_SLEEP_RETRY_DELAY_MS);
  }
  LOG_ERR("MAIN", "Tilt sensor did not confirm sleep before deep sleep");
}

bool executeX4ProHomeButtonAction(const uint8_t action,
                                  const QuickLockTrigger quickLockTrigger = QuickLockTrigger::None) {
  switch (action) {
    case CrossPointSettings::HOME_BUTTON_BACK_HOME:
      return activityManager.handleHomeButtonBackOrHome();
    case CrossPointSettings::HOME_BUTTON_TOGGLE_FRONTLIGHT: {
      const bool lightOn = !Frontlight.isOn();
      Frontlight.setOn(lightOn);
      SETTINGS.frontlightOn = lightOn ? 1 : 0;
      SETTINGS.saveToFile();
      LOG_INF("LIGHT", "Frontlight toggled %s by Home key", lightOn ? "on" : "off");
      return true;
    }
    case CrossPointSettings::HOME_BUTTON_READER_MENU:
      return activityManager.openReaderMenuFromShortcut();
    default:
      break;
  }

  if (action >= CrossPointSettings::SHORT_PWRBTN_COUNT) {
    return false;
  }

  const auto powerAction = static_cast<CrossPointSettings::SHORT_PWRBTN>(action);
  if (powerAction == CrossPointSettings::SHORT_PWRBTN::SYNC_PROGRESS) {
    dispatchShortcutAction(powerAction);
    return true;
  }
  if (handleGlobalPowerButtonAction(powerAction, quickLockTrigger)) {
    return true;
  }
  activityManager.handleShortcutAction(powerAction);
  return true;
}

bool wasX4ProHomeKeyTapped() {
#ifdef SIMULATOR
  return simulatorHomeKeyInput.wasTapped();
#else
  return gpio.wasHomeKeyTapped();
#endif
}

bool wasX4ProHomeKeyLongPressed() {
#ifdef SIMULATOR
  return simulatorHomeKeyInput.wasLongPressed();
#else
  return gpio.wasHomeKeyLongPressed();
#endif
}

bool unlockX4ProHomeQuickLock(const QuickLockTrigger trigger) {
  if (!buttonShortcutController.tryUnlockWithTrigger(millis(), trigger)) {
    return false;
  }
  notifyQuickLockChanged();
  return true;
}

bool handleX4ProHomeKeyQuickLockUnlock() {
  if (!mappedInputManager.hasHomeKey()) {
    return false;
  }

  const QuickLockTrigger trigger = buttonShortcutController.quickLockTrigger();
  if (trigger == QuickLockTrigger::HomeTap) {
    return wasX4ProHomeKeyTapped() && unlockX4ProHomeQuickLock(trigger);
  }
  if (trigger == QuickLockTrigger::HomeLongPress) {
    return wasX4ProHomeKeyLongPressed() && unlockX4ProHomeQuickLock(trigger);
  }
  if (trigger != QuickLockTrigger::HomeDoubleTap) {
    return false;
  }

  const unsigned long now = millis();
  if (x4ProHomeKeyTapPending && now - lastX4ProHomeKeyTapAt > X4PRO_HOME_KEY_DOUBLE_TAP_MS) {
    x4ProHomeKeyTapPending = false;
  }
  if (!wasX4ProHomeKeyTapped()) {
    return false;
  }
  if (!x4ProHomeKeyTapPending) {
    lastX4ProHomeKeyTapAt = now;
    x4ProHomeKeyTapPending = true;
    return true;
  }

  x4ProHomeKeyTapPending = false;
  return unlockX4ProHomeQuickLock(trigger);
}

bool handleX4ProHomeKeyShortcuts() {
  if (!mappedInputManager.hasHomeKey()) {
    return false;
  }

  // A modal owns Home too. Clear a pending single tap so a gesture started
  // while Quick Actions is open cannot fire after the popup closes.
  if (activityManager.blocksGlobalInput()) {
    const bool hadPendingTap = x4ProHomeKeyTapPending;
    x4ProHomeKeyTapPending = false;
    mappedInputManager.clearDeferredHomeGesture();
    return hadPendingTap || wasX4ProHomeKeyTapped() || wasX4ProHomeKeyLongPressed();
  }

  // Reader menus set the touchscreen override while they are active, which
  // intentionally lets Home work there. On a page, consume every Home edge
  // and discard a deferred single tap so nothing fires after it is re-enabled.
  if (mappedInputManager.isHomeButtonLockedInReader()) {
    const bool hadPendingTap = x4ProHomeKeyTapPending;
    x4ProHomeKeyTapPending = false;
    mappedInputManager.clearDeferredHomeGesture();
    return hadPendingTap || wasX4ProHomeKeyTapped() || wasX4ProHomeKeyLongPressed();
  }

  // A lower-bezel swipe can report a capacitive Home tap as well. Let the
  // screen gesture win and cancel any delayed single-tap interpretation so it
  // cannot navigate Home after the list has already handled the swipe.
  if (mappedInputManager.wasSwipe() != MappedInputManager::SwipeDir::None) {
    x4ProHomeKeyTapPending = false;
    mappedInputManager.clearDeferredHomeGesture();
    return false;
  }

  const unsigned long now = millis();
  bool completedPendingTap = false;
  if (x4ProHomeKeyTapPending && now - lastX4ProHomeKeyTapAt > X4PRO_HOME_KEY_DOUBLE_TAP_MS) {
    // A single-tap action must wait briefly so the reader does not navigate
    // away before a second capacitive-key tap can be recognized.
    x4ProHomeKeyTapPending = false;
    if (SETTINGS.homeButtonTapAction == CrossPointSettings::HOME_BUTTON_BACK_HOME) {
      // Keep reader menus and other overlays on their existing local Home route.
      mappedInputManager.queueDeferredHomeGesture();
    } else {
      executeX4ProHomeButtonAction(SETTINGS.homeButtonTapAction, QuickLockTrigger::HomeTap);
      completedPendingTap = true;
    }
  }

  if (wasX4ProHomeKeyLongPressed()) {
    // A hold is a separate gesture, not the second half of a double tap.
    x4ProHomeKeyTapPending = false;
    if (SETTINGS.homeButtonLongPressAction == CrossPointSettings::HOME_BUTTON_READER_MENU) {
      return completedPendingTap;
    }
    executeX4ProHomeButtonAction(SETTINGS.homeButtonLongPressAction, QuickLockTrigger::HomeLongPress);
    return true;
  }

  if (!wasX4ProHomeKeyTapped()) return completedPendingTap;

  if (!x4ProHomeKeyTapPending) {
    lastX4ProHomeKeyTapAt = now;
    x4ProHomeKeyTapPending = true;
    return true;
  }

  x4ProHomeKeyTapPending = false;
  executeX4ProHomeButtonAction(SETTINGS.homeButtonDoubleTapAction, QuickLockTrigger::HomeDoubleTap);
  return true;
}
}  // namespace
constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

static bool preflightSleepFrameBuffer() {
  if (!Storage.exists(SLEEP_FRAME_FILE)) return false;

  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) {
    LOG_ERR("SLP", "Failed to open Quick Resume frame for preflight");
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }

#ifdef SIMULATOR
  // The simulator's legacy display stub has no X3 runtime geometry. This path
  // is unreachable there because it cannot identify a UC8279 X3 profile.
  const size_t expectedSize = HalDisplay::BUFFER_SIZE;
#else
  const size_t expectedSize = EInkDisplay::X3_BUFFER_SIZE;
#endif
  const size_t actualSize = file.fileSize();
  file.close();
  if (SleepWakePolicy::hasValidSavedFrame(true, actualSize, expectedSize)) return true;

  LOG_ERR("SLP", "Invalid Quick Resume frame: expected=%u actual=%u", static_cast<unsigned>(expectedSize),
          static_cast<unsigned>(actualSize));
  Storage.remove(SLEEP_FRAME_FILE);
  return false;
}

bool shouldClearX4WakeGhosting() {
#if FREEINK_DEVICE_X4
  return gpio.deviceIsX4();
#else
  return false;
#endif
}

// Wake validation runs before the SD card and its settings file are available.
// Mirror the one setting that changes its behavior while entering sleep, so a
// deliberate short sleep press can wake the device even after the button has
// been released during boot. The write is skipped when the value is unchanged.
constexpr char WAKE_NVS_NAMESPACE[] = "crosspoint";
constexpr char WAKE_SHORT_PRESS_KEY[] = "wakeShortPr";

bool readWakeShortPressFromNvs() {
#ifdef SIMULATOR
  return false;
#else
  nvs_handle_t handle;
  if (nvs_open(WAKE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
  uint8_t value = 0;
  const esp_err_t result = nvs_get_u8(handle, WAKE_SHORT_PRESS_KEY, &value);
  nvs_close(handle);
  return result == ESP_OK && value != 0;
#endif
}

void mirrorWakeShortPressToNvs() {
#ifndef SIMULATOR
  const uint8_t expected =
      (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP || APP_STATE.quickLockResumePending) ? 1 : 0;
  nvs_handle_t handle;
  if (nvs_open(WAKE_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
  uint8_t current = 0;
  const bool hasCurrent = nvs_get_u8(handle, WAKE_SHORT_PRESS_KEY, &current) == ESP_OK;
  if (!hasCurrent || current != expected) {
    nvs_set_u8(handle, WAKE_SHORT_PRESS_KEY, expected);
    nvs_commit(handle);
  }
  nvs_close(handle);
#endif
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  // Every sleep mode leaves a complete retained frame on the e-ink panel. Keep
  // it visible until the first useful reader or Home paint replaces it.
  APP_STATE.showBootScreen = false;

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  } else {
    if (Storage.exists(SLEEP_FRAME_FILE)) {
      // A stale Quick Resume frame must not replace the selected sleep screen during wake.
      Storage.remove(SLEEP_FRAME_FILE);
    }
    delay(POST_SLEEP_SCREEN_SETTLE_MS);
  }

  if (halClock.isAvailable() && SETTINGS.autoBackupStats != 0) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now) && !backupGlobalStats(false)) {
      LOG_ERR("MAIN", "Automatic reading-stats backup failed before deep sleep");
    }
  }

  // Last chance to sample: startDeepSleep() cuts the SD rail on X3, so nothing
  // can be written again until the next wake.
  BatteryDiagnosticLog::record(BatteryDiagnosticLog::Event::Sleep, BoardConfig::ACTIVE.name);
  // All sleep-time file writes are complete. Stop SDMMC before the power path
  // cuts peripheral rails and isolates the bus pads; SPI boards are a no-op.
  Storage.shutdown();

  putTiltSensorToSleepForDeepSleep();
  display.deepSleep();
  mirrorWakeShortPressToNvs();
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts(const bool seamless, const bool loadReaderResources, const bool useReaderRenderStack) {
#if !defined(SIMULATOR) && !FREEINK_MCU_C3
  // C3 X3/X4 detection already runs in HalGPIO::begin() before SPI owns the
  // panel pins. S3 boards initialize display SPI inside display.begin(), so an
  // X4 Pro must resolve a UC8179 replacement panel here, before driver selection.
  static bool controllerResolved = false;
  if (!controllerResolved) {
    controllerResolved = true;
    if (freeink::applyXteinkDisplayController()) {
      LOG_DBG("MAIN", "Panel controller: UltraChip UC81xx variant detected");
    }
  }
#endif

#ifdef SIMULATOR
  (void)seamless;
  display.begin();
#else
  display.begin(seamless);
#endif
  renderer.begin();
  display.setInverted(SETTINGS.screenInverted != 0);
  // FreeInkUI headers need more than 4 KB once the render loop and nested
  // screen builders share the task stack. KOReader Sync and OPDS need the
  // reader stack on S3 devices because their deferred Wi-Fi transitions can
  // render a parent screen before the child activity is promoted. Other
  // lightweight network targets use 8 KB; reader rendering retains its 16 KB
  // budget.
  activityManager.begin(useReaderRenderStack ? READER_RENDER_TASK_STACK_BYTES : NETWORK_RENDER_TASK_STACK_BYTES);

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);

  renderer.insertFont(LEXENDDECA_10_FONT_ID, lexenddeca10FontFamily);
  renderer.insertFont(LEXENDDECA_12_FONT_ID, lexenddeca12FontFamily);
  renderer.insertFont(LEXENDDECA_14_FONT_ID, lexenddeca14FontFamily);
  renderer.insertFont(LEXENDDECA_16_FONT_ID, lexenddeca16FontFamily);
  renderer.insertFont(BITTER_10_FONT_ID, bitter10FontFamily);
  renderer.insertFont(BITTER_12_FONT_ID, bitter12FontFamily);
  renderer.insertFont(BITTER_14_FONT_ID, bitter14FontFamily);
  renderer.insertFont(BITTER_16_FONT_ID, bitter16FontFamily);
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  if (loadReaderResources) {
    sdFontSystem.begin(renderer);
  } else {
    LOG_DBG("MAIN", "Skipping EPUB scratch workspace and SD fonts for minimal network boot");
  }
}

void setup() {
#ifdef SIMULATOR
  SimulatorLifecycle::restoreSilentRebootToken(silentRebootMagic, silentRebootTarget, silentRebootPayload);
#endif
  BoardConfig::holdPowerRails();

  const esp_reset_reason_t rawResetReason = esp_reset_reason();
  const esp_sleep_wakeup_cause_t rawWakeupCause = esp_sleep_get_wakeup_cause();

#ifdef ENABLE_SERIAL_LOG
#ifdef CROSSPOINT_WAIT_FOR_USB_SERIAL
  // Development builds preserve reliable early CDC logs; release builds let
  // enumeration proceed asynchronously so users do not pay this startup cost.
  delay(250);
#endif
  // Web Serial sends file data in 256-byte chunks and waits for a 1-byte ACK.
  // Native USB CDC needs a larger queue because TinyUSB can deliver several
  // chunks before the cooperative transfer loop runs.
#if !defined(SIMULATOR) && FREEINK_DEVICE_X4PRO && !ARDUINO_USB_MODE
  logSerial.setRxBufferSize(4096);
#else
  logSerial.setRxBufferSize(1024);
#endif
#if ARDUINO_USB_MODE
  logSerial.setTxBufferSize(1024);
#endif
  Serial.begin(115200);
#if !defined(SIMULATOR) && FREEINK_DEVICE_X4PRO && !ARDUINO_USB_MODE
  UsbSerialFileTransfer::registerUsbCdcOverflowHandler();
#endif
#if !defined(SIMULATOR) && LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif
#endif

  HalSystem::begin();
  // checkPanic() clears the watchdog capture marker after a successful SD
  // dump, so retain the boot classification for the later activity route.
  const bool rebootedFromPanic = HalSystem::isRebootFromPanic();
  LOG_INF("BOOT", "Reset diagnostic: reset=%d(%s) sleepWake=%d(%s)", static_cast<int>(rawResetReason),
          resetReasonName(rawResetReason), static_cast<int>(rawWakeupCause), wakeupCauseName(rawWakeupCause));

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Validate the target too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const bool isValidSilentTarget =
      silentRebootTarget <= SILENT_REBOOT_TARGET_READER || isNetworkBootTargetValue(silentRebootTarget);
  const uint32_t snapshotTarget = (isSilentReboot && isValidSilentTarget) ? silentRebootTarget : 0;
  const uint32_t snapshotPayload = (isSilentReboot && isValidSilentTarget) ? silentRebootPayload : 0;
  const bool cleanImageBaseOnEntry =
      snapshotTarget == SILENT_REBOOT_TARGET_READER && (snapshotPayload & SILENT_REBOOT_READER_CLEAN_IMAGE_BASE) != 0;
  const bool isNetworkResume = snapshotTarget >= static_cast<uint32_t>(NetworkBootTarget::OTA);
  const bool followsWakeLightPolicy =
      isNetworkResume || (snapshotPayload & SILENT_REBOOT_FOLLOW_LIGHT_WAKE_POLICY) != 0;
  // KOReader Sync, OPDS, and File Transfer can render their parent screens
  // while a deferred Wi-Fi child is completing. On S3 devices, keep the
  // reader-sized render stack without loading the rest of the reader
  // resources. C3 devices retain the smaller network stack to preserve their
  // tighter internal-RAM budget.
  const bool useReaderRenderStack =
      !isNetworkResume ||
      (FREEINK_MCU_S3 && (snapshotTarget == static_cast<uint32_t>(NetworkBootTarget::KOREADER_SYNC) ||
                          snapshotTarget == static_cast<uint32_t>(NetworkBootTarget::OPDS) ||
                          snapshotTarget == static_cast<uint32_t>(NetworkBootTarget::FILE_TRANSFER)));
  silentRebootMagic = 0;
  silentRebootTarget = 0;
  silentRebootPayload = 0;
  if (!isSilentReboot || snapshotTarget != SILENT_REBOOT_TARGET_READER) {
    clearSilentRestartReaderPageBuild();
  }

  gpio.begin();
  // Sticky shares Confirm and Power on one GPIO. Emit Power first so the
  // configured shortcut wins; MappedInputManager mirrors it back to Confirm
  // only on screens that explicitly allow the fallback.
  gpio.setSharedConfirmPowerShortPressEmitsPower(true);
  powerManager.begin();

  const auto wakeupReason = gpio.getWakeupReason();
#ifndef SIMULATOR
  const bool shortPressWakes = readWakeShortPressFromNvs();
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton && !gpio.verifyPowerButtonWakeup(shortPressWakes)) {
    LOG_DBG("MAIN", "Power-button wake not held through verification, sleeping");
    powerManager.startDeepSleep(gpio);
  }
#endif

#ifndef SIMULATOR
  // X4 Pro and X4 Classic both map Up to the GPIO0 boot strap. Use Down for
  // recovery so holding the recovery chord cannot strand either S3 board in a
  // boot-mode loop.
  const auto recoveryButton = (BoardConfig::isX4Pro() || CROSSINK_APP_DEVICE_X4CLASSIC)
                                  ? MappedInputManager::Button::Down
                                  : MappedInputManager::Button::Up;
  const bool recoveryFirmwareMode = wakeupReason == HalGPIO::WakeupReason::PowerButton && !BoardConfig::isPaperMono() &&
                                    mappedInputManager.isPressed(recoveryButton);
#else
  const bool recoveryFirmwareMode = false;
#endif

  halTiltSensor.begin();
  halClock.begin();

#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");
  LOG_INF("BOOT", "Post-GPIO diagnostic: device=%s usb=%d silentReboot=%d silentTarget=%lu",
          gpio.deviceIsX3() ? "X3" : "X4", gpio.isUsbConnected() ? 1 : 0, isSilentReboot ? 1 : 0,
          static_cast<unsigned long>(snapshotTarget));
#else
#ifdef SIMULATOR
  LOG_INF("MAIN", "Device: Simulator");
#else
  LOG_INF("MAIN", "Device: %s", BoardConfig::ACTIVE.name);
#endif
#endif

  LOG_INF("BOOT", "Wake route: %s", wakeupRouteName(wakeupReason));
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      wakePowerReleasePending = true;
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // TEMP: continue booting while diagnosing post-flash/reset behavior.
      // Normal behavior is to go back to sleep when USB power causes a cold boot.
      LOG_INF("BOOT", "AfterUSBPower route: TEMP continuing boot instead of deep sleep");
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
      LOG_INF("BOOT", "AfterFlash route: continuing boot");
      break;
    case HalGPIO::WakeupReason::Other:
    default:
      LOG_INF("BOOT", "Other wake route: continuing boot");
      break;
  }

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot, !isNetworkResume, useReaderRenderStack);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }
  logBootHeap("storage ready");

  HalSystem::checkPanic();

  SETTINGS.loadFromFile();
  Storage.installDateTimeCallback(&SETTINGS.clockUtcOffsetQ);
  APP_STATE.loadFromFile();
  mirrorWakeShortPressToNvs();
  // Needs SETTINGS for the clock's UTC offset, so it cannot run any earlier.
  BatteryDiagnosticLog::record(BatteryDiagnosticLog::Event::Wake, BoardConfig::ACTIVE.name,
                               wakeupRouteName(wakeupReason));
  const bool isSleepWake = wakeupReason == HalGPIO::WakeupReason::PowerButton;
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  // Normal boot store deferral adapted from Sichroteph/YACP commit
  // 20af8aee8d3e1d560456753b08d1f52e5488621f (MIT). Accessors load these
  // stores when Home, reader bookkeeping, or sync actually need them.
  if (!isNetworkResume) {
    Dictionary::isValidDictionary();
  } else if (snapshotTarget == static_cast<uint32_t>(NetworkBootTarget::KOREADER_SYNC) ||
             snapshotTarget == static_cast<uint32_t>(NetworkBootTarget::KOREADER_AUTH) ||
             snapshotTarget == static_cast<uint32_t>(NetworkBootTarget::FILE_TRANSFER)) {
    KOREADER_STORE.loadFromFile();
  }
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);
  logBootHeap("boot state ready");
  // X4 Pro wakes through a POWERON reset, so keep its frontlight off until
  // the saved Quick Lock is explicitly unlocked below.
  const bool restoreQuickLockAfterWake = APP_STATE.quickLockResumePending && isSleepWake && !recoveryFirmwareMode &&
                                         !rebootedFromPanic && !isNetworkResume && !isSilentReboot;
  // Internal silent restarts retain the current light state. Network entry and
  // exit restarts must honor Restore on Wake like a normal user wake.
  const bool wasLightOnBeforeSleep = SETTINGS.frontlightOn != 0;
  const bool preserveLightAcrossRestart =
      FrontlightSchedule::shouldPreserveLightAcrossRestart(isSilentReboot, followsWakeLightPolicy);
  bool restoreLightOn = FrontlightSchedule::shouldRestoreLightOnStart(
      preserveLightAcrossRestart, SETTINGS.frontlightRestoreOnWake != 0, wasLightOnBeforeSleep);
  if (FrontlightSchedule::shouldApplyOnWakeSchedule(preserveLightAcrossRestart, SETTINGS.frontlightRestoreOnWake != 0,
                                                    wasLightOnBeforeSleep) &&
      FrontlightSchedule::hasCompleteWindow(SETTINGS.frontlightScheduleEnabled != 0, SETTINGS.frontlightScheduleStart,
                                            SETTINGS.frontlightScheduleEnd)) {
    uint8_t utcHour = 0;
    uint8_t utcMinute = 0;
    if (halClock.getTime(utcHour, utcMinute)) {
      const uint16_t localTimeOfDay = FrontlightSchedule::localTimeOfDay(utcHour, utcMinute, SETTINGS.clockUtcOffsetQ);
      restoreLightOn = FrontlightSchedule::containsTimeOfDay(SETTINGS.frontlightScheduleStart,
                                                             SETTINGS.frontlightScheduleEnd, localTimeOfDay);
    } else {
      restoreLightOn = false;
    }
  }
  if (restoreQuickLockAfterWake) {
    restoreLightOn = false;
  }
  Frontlight.begin(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth, restoreLightOn);

  if (recoveryFirmwareMode) {
    LOG_INF("MAIN", "Recovery firmware mode (%s + POWER held at boot)",
            (BoardConfig::isX4Pro() || CROSSINK_APP_DEVICE_X4CLASSIC) ? "DOWN" : "UP");
  }

  LOG_DBG("MAIN", "Starting CrossInk version " CROSSINK_VERSION);
  logMemoryStats("Boot");

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  // X4 Pro cuts its switched rails during sleep and wakes with a POWERON reset,
  // while C3 boards normally report DEEPSLEEP. HalGPIO normalizes both hardware
  // paths to PowerButton, so use that route with the one-shot persisted flag.
  const auto quickLockResumeTrigger = static_cast<QuickLockTrigger>(APP_STATE.quickLockResumeTrigger);
  if (APP_STATE.quickLockResumePending) {
    // Consume this before routing so a later cold boot cannot inherit a stale
    // lock if reader restoration itself fails.
    APP_STATE.quickLockResumePending = false;
    APP_STATE.quickLockResumeTrigger = static_cast<uint8_t>(QuickLockTrigger::None);
    APP_STATE.saveToFile();
    mirrorWakeShortPressToNvs();
  }
  // A boot-screen folder or an explicitly selected BMP opts a reader into
  // seeing its boot image after a power-button wake as well as a cold boot.
  // Without either, retain the fast splashless resume path.
  bool hasBootScreenDirectory = false;
  bool hasPinnedBootScreen = false;
  if (isSleepWake && !APP_STATE.showBootScreen) {
    std::string bootScreenDirectory;
    hasBootScreenDirectory = ImageFolderIndex::resolveBootScreenDirectory(bootScreenDirectory);
    hasPinnedBootScreen = !APP_STATE.favoriteBootImagePath.empty() &&
                          FsHelpers::hasBmpExtension(APP_STATE.favoriteBootImagePath) &&
                          Storage.exists(APP_STATE.favoriteBootImagePath.c_str());
  }
  const bool skipSplashOnWake =
      isSleepWake && !APP_STATE.showBootScreen && !hasBootScreenDirectory && !hasPinnedBootScreen;
  const BootResume resume = isNetworkResume    ? BootResume::Network
                            : isSilentReboot   ? BootResume::Silent
                            : skipSplashOnWake ? BootResume::SplashlessWake
                                               : BootResume::Splash;
  bool isUc8279X3 = false;
#ifndef SIMULATOR
  isUc8279X3 = gpio.deviceIsX3() && BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279;
#endif
  const bool hasValidSleepFrame = resume == BootResume::SplashlessWake && isUc8279X3 && preflightSleepFrameBuffer();
  const bool shouldRestoreSleepFrame =
      resume == BootResume::SplashlessWake && (isUc8279X3 ? hasValidSleepFrame : Storage.exists(SLEEP_FRAME_FILE));
  bool allowFastInitialReaderRefresh = false;

  setupDisplayAndFonts(SleepWakePolicy::shouldInitializeSeamlessly(resume, isUc8279X3, hasValidSleepFrame),
                       resume != BootResume::Network, useReaderRenderStack);
  logBootHeap("display and selected fonts ready");

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::Network:
      LOG_INF("BOOT", "Minimal network boot ready: target=%lu free=%u maxAlloc=%u",
              static_cast<unsigned long>(snapshotTarget), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      break;
    case BootResume::SplashlessWake:
      // One-shot flag: re-arm the splash for the next ordinary boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a splashless-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (shouldRestoreSleepFrame && loadSleepFrameBuffer()) {
        const bool useDifferentialRefresh = gpio.deviceIsX3();
        if (useDifferentialRefresh) {
          // begin() clears the X3 controller RAM, so restore the saved frame as
          // the baseline before replacing the moon with the loading icon.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }

        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        if (useDifferentialRefresh) {
          renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
          allowFastInitialReaderRefresh = true;
        } else {
          renderer.displayBuffer(HalDisplay::HALF_REFRESH);
          if (shouldClearX4WakeGhosting()) {
            // The X4's explicit wake refresh has already cleaned the retained
            // frame, so the reader can use its fast initial cycle as well.
            allowFastInitialReaderRefresh = true;
          }
        }
      } else if (isUc8279X3 && hasValidSleepFrame) {
        // The frame passed the size preflight but could not be read after display
        // setup. Do one clean, device-specific recovery rather than painting
        // differentially against an invalid controller baseline.
        LOG_ERR("BOOT", "Quick Resume frame load failed; rebuilding UC8279 X3 display baseline");
        Storage.remove(SLEEP_FRAME_FILE);
        renderer.clearScreen();
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      } else if (shouldClearX4WakeGhosting() && SETTINGS.fadingFix != 0) {
        LOG_INF("BOOT", "X4 wake: clearing retained sleep image with half refresh");
        renderer.clearScreen();
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        // The explicit HALF refresh above has already established a clean panel
        // baseline, so the reader's first page can use its fast initial cycle
        // instead of repeating the cleanup waveform.
        allowFastInitialReaderRefresh = true;
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (rebootedFromPanic) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Network) {
    bool launched = false;
    switch (static_cast<NetworkBootTarget>(snapshotTarget)) {
      case NetworkBootTarget::OTA: {
        auto otaActivity = makeUniqueNoThrow<OtaUpdateActivity>(renderer, mappedInputManager);
        if (otaActivity) {
          activityManager.replaceActivity(std::move(otaActivity));
          launched = true;
        } else {
          LOG_ERR("MAIN", "OOM: OTA activity after minimal boot (free=%u maxAlloc=%u)", ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());
        }
        break;
      }
      case NetworkBootTarget::OPDS:
        launched = activityManager.goToOpdsServer(snapshotPayload, true);
        break;
      case NetworkBootTarget::KOREADER_SYNC:
        launched = startGlobalSyncProgress(true, decodeKOReaderSyncOrientation(snapshotPayload));
        break;
      case NetworkBootTarget::KOREADER_AUTH: {
        const auto mode =
            snapshotPayload == 1 ? KOReaderAuthActivity::Mode::SIGN_UP : KOReaderAuthActivity::Mode::AUTHENTICATE;
        auto authActivity = makeUniqueNoThrow<KOReaderAuthActivity>(renderer, mappedInputManager, mode);
        if (authActivity) {
          activityManager.replaceActivity(std::move(authActivity));
          launched = true;
        } else {
          LOG_ERR("MAIN", "OOM: KOReader auth activity after minimal boot (free=%u maxAlloc=%u)", ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());
        }
        break;
      }
      case NetworkBootTarget::FILE_TRANSFER:
        launched = activityManager.resumeFileTransferFromNetworkBoot(snapshotPayload);
        break;
      case NetworkBootTarget::MANAGE_FONTS: {
        auto fontsActivity = makeUniqueNoThrow<FontDownloadActivity>(renderer, mappedInputManager);
        if (fontsActivity) {
          activityManager.replaceActivity(std::move(fontsActivity));
          launched = true;
        } else {
          LOG_ERR("MAIN", "OOM: Manage Fonts activity after minimal boot (free=%u maxAlloc=%u)", ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());
        }
        break;
      }
    }
    if (!launched) {
      LOG_ERR("MAIN", "Minimal network boot target failed; returning home");
      silentRestartAfterNetwork();
    }
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath, false, false, cleanImageBaseOnEntry);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome(HomeMenuItem::NONE, true);
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path, false, allowFastInitialReaderRefresh);
  }

  if (resume == BootResume::Silent || resume == BootResume::Network) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  if (restoreQuickLockAfterWake) {
    // Render the reconstructed route first, then draw the badge. The pending
    // wake release stays swallowed by the main loop, so it cannot unlock the
    // restored lock immediately.
    (void)activityManager.requestUpdateAndWait();
    buttonShortcutController.restoreQuickLock(millis(), quickLockResumeTrigger);
    notifyQuickLockChanged(true);
  }

  allowSleepAt = millis() + 2000;
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();
#ifdef SIMULATOR
  simulatorHomeKeyInput.update();
#endif
  if (activityManager.requiresExclusiveStorageLoop()) {
    // Keep the serial endpoint responsive so Inky receives ERR:not_on_home,
    // while every filesystem/UI/global path remains suspended by the activity.
    (void)UsbSerialFileTransfer::process(false);
    activityManager.loop();
    if (activityManager.preventAutoSleep()) {
      powerManager.setPowerSaving(false);
      delay(10);
    } else {
      // No host is active, so a slower loop is safe. The activity itself times
      // out the raw-storage handoff rather than entering deep sleep detached.
      powerManager.setPowerSaving(true);
      delay(50);
    }
    return;
  }

  if (!buttonShortcutController.isQuickLocked()) {
    halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.tiltPageTurnDirection, SETTINGS.orientation,
                         activityManager.isReaderActivity());
  }

  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    logMemoryStats("Periodic");
    lastMemPrint = millis();
  }

  if (!buttonShortcutController.isQuickLocked() && UsbSerialFileTransfer::process(activityManager.isHomeActivity()) ==
                                                       UsbSerialFileTransfer::ProcessResult::ScreenshotRequested) {
    const uint32_t bufferSize = display.getBufferSize();
    logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
    uint8_t* buf = display.getFrameBuffer();
    logSerial.write(buf, bufferSize);
    logSerial.printf("SCREENSHOT_END\n");
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased()
#if CROSSINK_APP_CAP_TOUCH
      || gpio.wasTouchActivity()
#endif
      || halTiltSensor.hadActivity() || activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  // Let wake continue as soon as its hold has been verified. The release can
  // arrive after setup, so consume that one input frame rather than making it
  // a page turn, refresh, or other short power-button action.
  if (wakePowerReleasePending && !gpio.isPressed(HalGPIO::BTN_POWER)) {
    wakePowerReleasePending = false;
    return;
  }

  const bool modalOwnsInput = activityManager.blocksGlobalInput();

  // Keep Power + Down screenshot-only. The configurable chord below uses Up,
  // so it cannot replace or double-fire this one. The controller consumes both
  // release orders even when an open modal blocks the screenshot itself.
  const bool screenshotActionBlocked = modalOwnsInput || buttonShortcutController.isQuickLocked();
  const auto screenshotChordResult = buttonShortcutController.updatePowerDown(
      gpio.isPressed(HalGPIO::BTN_POWER), gpio.isPressed(HalGPIO::BTN_DOWN), screenshotActionBlocked);
  if (screenshotChordResult.event == ButtonShortcutController::Event::Screenshot) {
    RenderLock lock;
    ScreenshotUtil::takeScreenshot(renderer);
  }
  if (screenshotChordResult.consumeInput) {
    return;
  }

  const bool touchscreenEscapeHatch =
      !modalOwnsInput && gpio.hasTouch() && SETTINGS.disableReaderTouchscreen && activityManager.isReaderActivity();
  const auto sideButtonShortcutResult = buttonShortcutController.updateUpDown(
      millis(), gpio.isPressed(HalGPIO::BTN_UP), gpio.isPressed(HalGPIO::BTN_DOWN), configuredSideButtonChordAction(),
      touchscreenEscapeHatch, modalOwnsInput);
  if (dispatchButtonShortcut(sideButtonShortcutResult) || sideButtonShortcutResult.consumeInput) {
    lastActivityTime = millis();
    return;
  }

  const bool powerPressed = gpio.isPressed(HalGPIO::BTN_POWER);
  const bool chordButtonPressed = gpio.isPressed(HalGPIO::BTN_UP);
  const bool shortPowerRelease = gpio.wasReleased(HalGPIO::BTN_POWER) &&
                                 gpio.getPowerButtonHeldTime() < SETTINGS.getPowerButtonLongPressDuration();
  const bool quickLockOnShortPower =
      shortPowerRelease && SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK;
  const auto shortcutResult =
      buttonShortcutController.update(millis(), powerPressed, chordButtonPressed, shortPowerRelease,
                                      quickLockOnShortPower, configuredChordAction(), modalOwnsInput);
  if (dispatchButtonShortcut(shortcutResult)) {
    lastActivityTime = millis();
    return;
  }

  if (buttonShortcutController.isQuickLocked()) {
    const bool longPowerPressed =
        powerPressed && gpio.getPowerButtonHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
    if (buttonShortcutController.tryUnlockLongPower(millis(), longPowerPressed)) {
      notifyQuickLockChanged();
      lastActivityTime = millis();
      return;
    }
    if (handleX4ProHomeKeyQuickLockUnlock()) {
      lastActivityTime = millis();
      return;
    }
    if (activityManager.handleQuickLockUnlock(buttonShortcutController.quickLockTrigger())) {
      lastActivityTime = millis();
      return;
    }
    const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
    if (sleepTimeoutMs > 0 && buttonShortcutController.shouldQuickLockSleep(millis(), sleepTimeoutMs)) {
      LOG_DBG("SLP", "Quick Lock timeout triggered after %lu ms", sleepTimeoutMs);
      APP_STATE.quickLockResumePending = true;
      APP_STATE.quickLockResumeTrigger = static_cast<uint8_t>(buttonShortcutController.quickLockTrigger());
      enterDeepSleep(true);
      // The simulator's deep sleep returns, unlike hardware. Keep its next
      // test loop from treating the marker as a real reboot restore.
#ifdef SIMULATOR
      APP_STATE.quickLockResumePending = false;
#endif
      lastActivityTime = millis();
    }
    mappedInputManager.clearInjectedReleases();
    return;
  }

  // The entire chord is consumed until both buttons are released, so the
  // Power release cannot also run its ordinary short-press action.
  if (shortcutResult.consumeInput) return;

#ifdef SIMULATOR
  if (gpio.consumeSimulatorSleepRequest()) {
    enterDeepSleep();
    lastActivityTime = millis();
    return;
  }
#endif
  // Home-key taps are consumed until their single- or double-tap action is
  // known.
  if (handleX4ProHomeKeyShortcuts()) {
    return;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    // In the simulator, deep sleep is a no-op and returns — reset the timer so
    // the main loop does not immediately re-trigger auto-sleep.
    lastActivityTime = millis();
    return;
  }

  // Do not feed the wake gesture into getPowerButtonAction(). In particular,
  // the release edge can otherwise run the configured short/long Power action
  // in the same loop that arms the post-wake guard.
  if (!powerButtonReleasedSinceWake) {
    if (!gpio.isPressed(HalGPIO::BTN_POWER)) {
      powerButtonReleasedSinceWake = true;
    }
  } else if (millis() >= allowSleepAt) {
    const auto powerAction = getPowerButtonAction();
    if (powerAction == CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK) {
      const bool longPower = gpio.getPowerButtonHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
      if (handleGlobalPowerButtonAction(powerAction,
                                        longPower ? QuickLockTrigger::LongPower : QuickLockTrigger::ShortPower)) {
        lastActivityTime = millis();
        return;
      }
    } else if (dispatchShortcutAction(powerAction)) {
      lastActivityTime = millis();
      return;
    }
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  // While on external power the percent climbs with no user interaction to
  // repaint it (gauge boards like the X4 Pro report SoC continuously), so poll
  // for a change once a minute. Off-charger the percent moves too slowly to
  // justify unsolicited e-ink refreshes.
#ifdef SIMULATOR
  const bool usbConnected = gpio.isUsbConnected();
#else
  const bool usbConnected = gpio.isUsbConnectedCached();
#endif
  if (usbConnected) {
    static unsigned long lastBatteryPollTime = 0UL;
    static uint16_t lastBatteryPercent = 0xFFFF;
    if (millis() - lastBatteryPollTime >= 60000UL) {
      lastBatteryPollTime = millis();
      const uint16_t percent = powerManager.getBatteryPercentage();
      if (lastBatteryPercent != 0xFFFF && percent != lastBatteryPercent) {
        activityManager.requestUpdate();
      }
      lastBatteryPercent = percent;
    }
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
#if CROSSINK_APP_CAP_TOUCH
  // A delayed Home event is valid for this activity dispatch only. If an
  // unrelated gesture took priority, do not carry it into the next activity.
  mappedInputManager.clearDeferredHomeGesture();
#endif
  const unsigned long activityDuration = millis() - activityStartTime;

#ifdef SIMULATOR
  runSimulatorSmokeTestTick();
#endif

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
      (void)activityDuration;
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
