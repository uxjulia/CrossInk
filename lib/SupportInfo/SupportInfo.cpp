#include "SupportInfo.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace SupportInfo {
namespace {
constexpr const char* preferenceNames[] = {
#define SUPPORT_PREF(name) #name,
#include "SupportPreferences.inc"
#undef SUPPORT_PREF
};
constexpr const char* readerNames[] = {
#define SUPPORT_READER(name) #name,
#include "SupportReaderFields.inc"
#undef SUPPORT_READER
};
const char* presence(HalDeviceInfo::Presence value) {
  switch (value) {
    case HalDeviceInfo::Presence::Absent:
      return "absent";
    case HalDeviceInfo::Presence::Available:
      return "available";
    case HalDeviceInfo::Presence::Unavailable:
      return "unavailable";
    case HalDeviceInfo::Presence::Simulated:
      return "simulated";
  }
  return "unavailable";
}
const char* bookStatus(BookStatus status) {
  switch (status) {
    case BookStatus::Excluded:
      return "excluded";
    case BookStatus::Unavailable:
      return "unavailable";
    case BookStatus::Inherited:
      return "inherited_no_record";
    case BookStatus::Loaded:
      return "loaded";
    case BookStatus::Invalid:
      return "invalid_record";
  }
  return "unavailable";
}
}  // namespace
void normalizeBookValues(BookContext& b, const uint16_t* g) {
  auto& v = b.values;
  if (v[Reader_fontFamily] >= 2) {
    v[Reader_fontFamily] = g[fontFamily];
    b.fallbackMask |= 1U << Reader_fontFamily;
  }
  v[Reader_lineHeightPercent] = std::clamp<uint8_t>(v[Reader_lineHeightPercent], 70, 200);
  v[Reader_wordSpacing] = std::min<uint8_t>(v[Reader_wordSpacing], 8);
  if (v[Reader_orientation] >= 4) {
    v[Reader_orientation] = g[orientation];
    b.fallbackMask |= 1U << Reader_orientation;
  }
  v[Reader_screenMarginVertical] = std::clamp<uint8_t>(v[Reader_screenMarginVertical], 5, 150);
  v[Reader_screenMarginHorizontal] = std::clamp<uint8_t>(v[Reader_screenMarginHorizontal], 5, 150);
  if (v[Reader_paragraphAlignment] >= 5) {
    v[Reader_paragraphAlignment] = g[paragraphAlignment];
    b.fallbackMask |= 1U << Reader_paragraphAlignment;
  }
  if (v[Reader_imageRendering] >= 3) {
    v[Reader_imageRendering] = g[imageRendering];
    b.fallbackMask |= 1U << Reader_imageRendering;
  }
  if (v[Reader_indexingMethod] >= 2) v[Reader_indexingMethod] = 1;
  constexpr ReaderField booleanFields[] = {
      Reader_publisherPageNumbers, Reader_embeddedStyle,         Reader_hyphenationEnabled,
      Reader_textAntiAliasing,     Reader_extraParagraphSpacing, Reader_forceParagraphIndents,
      Reader_focusReadingEnabled,  Reader_guideReadingEnabled,   Reader_imageGrayscale};
  for (auto field : booleanFields) v[field] = v[field] != 0;
}
bool Writer::flush() {
  if (ok_ && used_ && write_(ctx_, buffer_, used_) != used_) ok_ = false;
  used_ = 0;
  return ok_;
}
void Writer::text(const char* value) {
  if (!value) return;
  while (ok_ && *value) {
    if (used_ == sizeof(buffer_)) flush();
    if (ok_) buffer_[used_++] = *value++;
  }
}
void Writer::string(const char* value) {
  if (!value) {
    text("null");
    return;
  }
  text("\"");
  // Inputs are compile-time hardware/provenance labels only. Still escape valid JSON.
  while (ok_ && *value) {
    const unsigned char c = *value++;
    char escaped[7]{};
    if (c < 32)
      snprintf(escaped, sizeof(escaped), "\\u%04x", c);
    else if (c == '"' || c == '\\') {
      escaped[0] = '\\';
      escaped[1] = c;
    } else
      escaped[0] = c;
    text(escaped);
  }
  text("\"");
}
void Writer::number(uint64_t value) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
  text(buf);
}
void Writer::field(const char* key, uint64_t value) {
  string(key);
  text(":");
  number(value);
  text(",");
}
void Writer::stringField(const char* key, const char* value) {
  string(key);
  text(":");
  string(value);
  text(",");
}
bool Writer::snapshot(const Snapshot& d) {
  const auto& h = d.hardware;
  text("{\"schema\":\"crossink-support\",\"version\":1,\"scope\":");
  string(d.book.status == BookStatus::Excluded ? "device" : "device_and_last_opened_epub");
  text(",\"firmware\":{");
  stringField("version", d.version);
  stringField("sourceSha", d.sourceSha);
  stringField("freeinkSdkSha", d.sdkSha);
  stringField("target", d.target);
  stringField("trackedTreeModified", d.dirty);
  text("\"branch\":\"excluded\"},\"about\":{");
  stringField("runtimeData", h.simulated ? "simulated" : "snapshot_at_export");
  stringField("deviceProfile", h.device);
  field("uptimeSeconds", h.uptimeSeconds);
  field("panelWidth", h.width);
  field("panelHeight", h.height);
  stringField("touch", presence(h.touch));
  stringField("touchController", h.touchController);
  stringField("frontlight", presence(h.frontlight));
  stringField("externalRtc", presence(h.rtc));
  stringField("imu", presence(h.imu));
  stringField("sdTransport", h.simulated ? "simulated" : h.sdmmc ? "sdmmc" : "spi");
  stringField("sdCapacityStatus", h.simulated ? "simulated" : h.sdReady && h.sdBytes ? "available" : "unavailable");
  if (!h.simulated && h.sdReady && h.sdBytes) field("sdCapacityBytes", h.sdBytes);
  stringField("chipAndMemoryStatus", h.simulated ? "unsupported" : "available");
  if (!h.simulated) {
    stringField("chip", h.chip);
    field("chipRevision", h.chipRevision);
    field("cores", h.cores);
    field("cpuMHz", h.cpuMHz);
    field("flashBytes", h.flashBytes);
    field("internalFreeBytes", h.internalFree);
    field("internalMinimumBytes", h.internalMinimum);
    field("internalLargestBytes", h.internalLargest);
    stringField("psram", presence(h.psram));
    if (h.psramTotal) {
      field("psramTotalBytes", h.psramTotal);
      field("psramFreeBytes", h.psramFree);
      field("psramLargestBytes", h.psramLargest);
    }
    field("resetReasonEspIdf", h.resetReason);
  }
  stringField("displayController", h.displayController);
  if (h.displayPanel) stringField("displayPanel", h.displayPanel);
  text("\"espIdfVersion\":");
  string(h.sdk);
  text("},\"globalPreferences\":{\"source\":\"loaded_in_memory\",\"numericEncoding\":\"firmware_enum_or_units\",");
  for (unsigned i = 0; i < PreferenceCount; ++i) field(preferenceNames[i], d.preferences[i]);
  field("customFontSelected", d.customFont);
  field("dictionaryCustomFontSelected", d.dictionaryCustomFont);
  text("\"statusSlots\":[");
  for (unsigned i = 0; i < 17; ++i) {
    if (i) text(",");
    number(d.statusSlots[i]);
  }
  text("],\"statusOptions\":[");
  for (unsigned i = 0; i < 9; ++i) {
    if (i) text(",");
    number(d.statusOptions[i]);
  }
  text("],\"statusHidden\":[");
  number(d.statusHidden[0]);
  text(",");
  number(d.statusHidden[1]);
  text("],\"quickActions\":[");
  for (unsigned i = 0; i < 5; ++i) {
    if (i) text(",");
    number(d.quickActions[i]);
  }
  text("]},\"configurationStatus\":{\"meaning\":\"file_presence_only_not_validated_or_connected\",");
  field("wifiFilePresent", d.wifiFile);
  field("opdsFilePresent", d.opdsFile);
  field("koreaderFilePresent", d.koreaderFile);
  field("fontProfilesFilePresent", d.fontProfilesFile);
  text("\"privateFields\":\"excluded\",\"fontProfilesAndHardwareNvsValues\":\"not_collected\"},\"bookContext\":{");
  stringField("status", bookStatus(d.book.status));
  if (d.book.status == BookStatus::Loaded || d.book.status == BookStatus::Inherited) {
    text("\"format\":\"epub\",");
    field("readerOverrideMask", d.book.overrideMask);
    text("\"effectiveReaderPreferences\":{");
    for (unsigned i = 0; i < ReaderFieldCount; ++i) {
      if (i) text(",");
      string(readerNames[i]);
      text(":{\"value\":");
      if (i == Reader_readerFontPointSize && !d.book.fontPointSizeKnown)
        text("null");
      else
        number(d.book.values[i]);
      text(",\"source\":");
      const bool safeField =
          i == Reader_embeddedStyle || i == Reader_focusReadingEnabled || i == Reader_guideReadingEnabled;
      string(d.book.safeMode && safeField      ? "safe_mode"
             : d.book.fallbackMask & (1U << i) ? "global_fallback"
             : d.book.overrideMask & (1U << i) ? "book_override"
                                               : "global_default");
      if (i == Reader_readerFontPointSize && !d.book.fontPointSizeKnown)
        text(",\"status\":\"unavailable_legacy_custom_font_size\"");
      text("}");
    }
    text("},");
    field("customFontSelected", d.book.customFont);
    field("fontSelectionOverride", d.book.fontOverride);
    field("dictionaryOverride", d.book.dictionaryOverride);
    field("dictionaryCustomFontSelected", d.book.dictionaryCustomFont);
    field("dictionaryPointSize", d.book.dictionaryPointSize);
    field("safeModeOverride", d.book.safeMode);
    field("renderModeOverride", d.book.renderModeOverride);
    field("renderMode", d.book.renderMode);
    field("autoPageTurnOverride", d.book.autoPageTurnOverride);
    field("autoPageTurnSeconds", d.book.autoPageTurnSeconds);
    field("bookStatsEnabled", d.book.statsEnabled);
    field("effectiveStatsEnabled", d.book.statsEnabled && d.preferences[trackReadingStats]);
  }
  text(
      "\"identityAndHistory\":\"excluded\"},\"exclusions\":[\"credentials\",\"usernames\",\"urls\",\"ssids\",\"device_"
      "identifiers\",\"custom_names_and_paths\",\"book_identity_and_history\",\"raw_logs\"]}\n");
  return flush();
}
Result exportAtomically(const FileOps& o, Writer& writer, const Snapshot& data) {
  auto exists = [&](const char* p) { return o.exists(o.ctx, p); };
  auto remove = [&](const char* p) { return o.remove(o.ctx, p); };
  auto rename = [&](const char* a, const char* b) { return o.rename(o.ctx, a, b); };
  // Recover the last complete export before touching an interrupted transaction.
  if (!exists(Path) && exists(BackupPath) && !rename(BackupPath, Path)) return Result::RecoveryRequired;
  if (exists(TempPath) && !remove(TempPath)) return Result::Failed;
  if (exists(BackupPath) && !remove(BackupPath)) return Result::Failed;
  if (!o.open(o.ctx, TempPath)) return Result::Failed;
  const bool wrote = writer.snapshot(data);
  const bool synced = wrote && o.sync(o.ctx);
  const bool closed = o.close(o.ctx);
  if (!wrote || !synced || !closed) {
    if (exists(TempPath)) remove(TempPath);
    return Result::Failed;
  }
  const bool replacing = exists(Path);
  if (replacing && !rename(Path, BackupPath)) {
    remove(TempPath);
    return Result::Failed;
  }
  if (!rename(TempPath, Path)) {
    remove(TempPath);
    if (replacing && !rename(BackupPath, Path)) return Result::RecoveryRequired;
    return Result::Failed;
  }
  if (replacing && !remove(BackupPath)) return Result::SavedBackupRetained;
  return Result::Saved;
}
}  // namespace SupportInfo
