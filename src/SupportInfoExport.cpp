#include "SupportInfoExport.h"

#include <AppVersion.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "activities/reader/BookStatsTracking.h"
#include "activities/reader/EpubReaderActivity.h"

namespace {
// Keep the tested support normalizer aligned with applyReaderSettings' constraints.
static_assert(CrossPointSettings::BUILTIN_FONT_COUNT == 2 && CrossPointSettings::ORIENTATION_COUNT == 4 &&
              CrossPointSettings::MIN_LINE_HEIGHT_PERCENT == 70 && CrossPointSettings::MAX_LINE_HEIGHT_PERCENT == 200 &&
              CrossPointSettings::MAX_WORD_SPACING == 8 && CrossPointSettings::MIN_SCREEN_MARGIN == 5 &&
              CrossPointSettings::MAX_SCREEN_MARGIN == 150 && CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT == 5 &&
              CrossPointSettings::IMAGE_RENDERING_COUNT == 3 && CrossPointSettings::INDEXING_METHOD_COUNT == 2 &&
              CrossPointSettings::INDEXING_FULL_SECTION == 1);
bool isEpub(const std::string& path) {
  const auto n = path.size();
  if (n < 5 || n > 1024 || path[n - 5] != '.') return false;
  const char* suffix = path.c_str() + n - 4;
  return (suffix[0] == 'e' || suffix[0] == 'E') && (suffix[1] == 'p' || suffix[1] == 'P') &&
         (suffix[2] == 'u' || suffix[2] == 'U') && (suffix[3] == 'b' || suffix[3] == 'B');
}
struct ExportState {
  SupportInfo::Snapshot data;
  HalFile file;
  SupportInfo::Writer writer;
  EpubReaderActivity::BookReaderSettingsData bookData;
  ExportState() : writer(write, this) {}
  static size_t write(void* ctx, const void* data, size_t size) {
    return static_cast<ExportState*>(ctx)->file.write(data, size);
  }
};
void capturePreferences(SupportInfo::Snapshot& d) {
  std::lock_guard<std::mutex> lock(SETTINGS.getMutex());
// Preserve the allowlisted numeric schema; custom identities stay private and map to English.
#define SUPPORT_LANGUAGE_CODE() \
  d.preferences[SupportInfo::language] = static_cast<uint8_t>(I18n::languageFromCode(SETTINGS.languageCode));
#define SUPPORT_PREF(name) d.preferences[SupportInfo::name] = SETTINGS.name;
#include <SupportPreferences.inc>
#undef SUPPORT_PREF
#undef SUPPORT_LANGUAGE_CODE
  for (unsigned i = 0; i < 7; ++i) {
    d.statusSlots[i] = static_cast<uint8_t>(SETTINGS.topReaderStatusBar.slots[i]);
    d.statusSlots[i + 7] = static_cast<uint8_t>(SETTINGS.bottomReaderStatusBar.slots[i]);
  }
  for (unsigned i = 0; i < 3; ++i) d.statusSlots[i + 14] = static_cast<uint8_t>(SETTINGS.displayStatusBar.slots[i]);
  d.statusOptions[0] = SETTINGS.topReaderStatusBar.percentageFormat;
  d.statusOptions[1] = SETTINGS.topReaderStatusBar.progressBar;
  d.statusOptions[2] = SETTINGS.topReaderStatusBar.progressBarThickness;
  d.statusOptions[3] = SETTINGS.bottomReaderStatusBar.percentageFormat;
  d.statusOptions[4] = SETTINGS.bottomReaderStatusBar.progressBar;
  d.statusOptions[5] = SETTINGS.bottomReaderStatusBar.progressBarThickness;
  d.statusOptions[6] = static_cast<uint8_t>(SETTINGS.topReaderStatusBar.batteryStyle);
  d.statusOptions[7] = static_cast<uint8_t>(SETTINGS.bottomReaderStatusBar.batteryStyle);
  d.statusOptions[8] = static_cast<uint8_t>(SETTINGS.displayStatusBar.batteryStyle);
  d.statusHidden[0] = SETTINGS.topReaderStatusBar.hidden;
  d.statusHidden[1] = SETTINGS.bottomReaderStatusBar.hidden;
  std::memcpy(d.quickActions, SETTINGS.quickActionSlots, sizeof(d.quickActions));
  d.customFont = SETTINGS.sdFontFamilyName[0] != 0;
  d.dictionaryCustomFont = SETTINGS.dictionarySdFontFamilyName[0] != 0;
}
void captureBook(ExportState& state) {
  auto& b = state.data.book;
  b.status = SupportInfo::BookStatus::Unavailable;
  std::string path;
  {
    std::lock_guard<std::mutex> lock(APP_STATE.getMutex());
    if (!isEpub(APP_STATE.openEpubPath)) return;
    // Single bounded cold-path copy; never exported or used to load metadata.
    path = APP_STATE.openEpubPath;
  }
  if (!Storage.exists(path.c_str())) return;
  // Pure path hashing; intentionally avoid resolveCachePathForFilePath's migration writes.
  const std::string cache = Epub::cachePathForFilePath(path, "/.crosspoint");
  if (!Storage.exists(cache.c_str())) return;  // No current cache; do not claim missing legacy overrides are inherited.
  EpubReaderActivity::BookSettingsReadStatus status;
  state.bookData = EpubReaderActivity::readBookReaderSettingsForSupport(cache, status);
  if (status == EpubReaderActivity::BookSettingsReadStatus::Invalid) {
    b.status = SupportInfo::BookStatus::Invalid;
    return;
  }
  b.status = status == EpubReaderActivity::BookSettingsReadStatus::Missing ? SupportInfo::BookStatus::Inherited
                                                                           : SupportInfo::BookStatus::Loaded;
  const auto& raw = state.bookData;
  b.overrideMask = raw.readerSettingsOverrideMask;
#define SUPPORT_READER(name)                                                                                   \
  b.values[SupportInfo::Reader_##name] = (raw.readerSettingsOverrideMask & (1U << SupportInfo::Reader_##name)) \
                                             ? raw.readerSettings.name                                         \
                                             : state.data.preferences[SupportInfo::name];
#include <SupportReaderFields.inc>
#undef SUPPORT_READER
  b.customFont = (b.overrideMask & (1U << SupportInfo::ReaderFieldCount)) ? raw.readerSettings.sdFontFamilyName[0] != 0
                                                                          : state.data.customFont;
  if (raw.hasCustomReaderSettings) {
    SupportInfo::normalizeBookValues(b, state.data.preferences);
    if (b.values[SupportInfo::Reader_readerFontPointSize] < CrossPointSettings::MIN_READER_FONT_POINT_SIZE) {
      if (b.customFont) {
        // Resolving this legacy step scans font registries; report unavailable instead.
        b.fontPointSizeKnown = false;
      } else {
        b.values[SupportInfo::Reader_readerFontPointSize] = CrossPointSettings::getReaderFontPointSize(
            static_cast<CrossPointSettings::FONT_SIZE>(b.values[SupportInfo::Reader_readerFontPointSize]));
      }
    }
  }
  b.safeMode = raw.hasSafeModeOverride;
  if (b.safeMode) {
    b.values[SupportInfo::Reader_embeddedStyle] = 0;
    b.values[SupportInfo::Reader_focusReadingEnabled] = 0;
    b.values[SupportInfo::Reader_guideReadingEnabled] = 0;
  }
  b.fontOverride = (b.overrideMask & (1U << SupportInfo::ReaderFieldCount)) != 0;
  b.dictionaryOverride = raw.hasDictionaryFontOverride;
  b.dictionaryCustomFont =
      raw.hasDictionaryFontOverride ? raw.dictionarySdFontFamilyName[0] != 0 : state.data.dictionaryCustomFont;
  b.dictionaryPointSize = raw.hasDictionaryFontOverride ? raw.dictionaryFontPointSize
                                                        : state.data.preferences[SupportInfo::dictionaryFontPointSize];
  b.renderModeOverride = raw.hasRenderModeOverride;
  // Matches reader entry: its render-mode override is applied after safe mode.
  b.renderMode = raw.hasRenderModeOverride ? raw.renderMode : 0;
  b.autoPageTurnOverride = raw.hasAutoPageTurnInterval;
  b.autoPageTurnSeconds = raw.autoPageTurnSeconds;
  b.statsEnabled = BookStatsTracking::isBookEnabled(cache);
}
}  // namespace
namespace SupportInfoExport {
bool lastOpenedEpubAvailable() {
  std::lock_guard<std::mutex> lock(APP_STATE.getMutex());
  return isEpub(APP_STATE.openEpubPath);
}
SupportInfo::Result save(bool includeLastOpenedEpub) {
  if (!Storage.ready()) {
    LOG_ERR("SUPPORT", "SD unavailable");
    return SupportInfo::Result::Failed;
  }
  // ~1 KiB fixed state (exact sizeof logged); kept off the small main-task stack.
  // Owned for this synchronous export only, with no whole JSON document allocation.
  auto state = makeUniqueNoThrow<ExportState>();
  if (!state) {
    LOG_ERR("SUPPORT", "OOM allocating export state (%u bytes)", static_cast<unsigned>(sizeof(ExportState)));
    return SupportInfo::Result::OutOfMemory;
  }
  LOG_DBG("SUPPORT", "Export state allocation: %u bytes", static_cast<unsigned>(sizeof(ExportState)));
  auto& d = state->data;
  capturePreferences(d);
  if (includeLastOpenedEpub) captureBook(*state);
  d.wifiFile = Storage.exists("/.crosspoint/wifi.json");
  d.opdsFile = Storage.exists("/.crosspoint/opds.json");
  d.koreaderFile = Storage.exists("/.crosspoint/koreader.json");
  d.fontProfilesFile = Storage.exists("/.crosspoint/ttf-rendering.json");
  d.version = AppVersion::supportVersion();
  d.sourceSha = AppVersion::gitSha();
  d.sdkSha = AppVersion::sdkSha();
  d.target = CROSSINK_FIRMWARE_DEVICE_TYPE;
  d.dirty = AppVersion::gitDirtyFlag();
  // Capture immediately before writing, so heap values include export's own allocation.
  d.hardware = HalDeviceInfo::capture();
  const SupportInfo::FileOps ops{
      state.get(),
      [](void*, const char* p) { return Storage.exists(p); },
      [](void*, const char* p) { return Storage.remove(p); },
      [](void*, const char* a, const char* b) { return Storage.rename(a, b); },
      [](void* c, const char* p) { return Storage.openFileForWrite("SUPPORT", p, static_cast<ExportState*>(c)->file); },
      &ExportState::write,
      [](void* c) { return static_cast<ExportState*>(c)->file.sync(); },
      [](void* c) { return static_cast<ExportState*>(c)->file.close(); }};
  const auto result = SupportInfo::exportAtomically(ops, state->writer, d);
  if (result != SupportInfo::Result::Saved) LOG_ERR("SUPPORT", "Export result: %u", static_cast<unsigned>(result));
  return result;
}
}  // namespace SupportInfoExport
