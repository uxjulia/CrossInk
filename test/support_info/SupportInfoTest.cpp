#include <ArduinoJson.h>
#include <SupportInfo.h>
#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>
using namespace SupportInfo;
namespace {
struct Files {
  std::map<std::string, std::string> files;
  std::string active;
  std::string failRenameFrom;
  std::string failRenameTo;
  std::string failRemove;
  bool failOpen = false, shortWrite = false, failSync = false, failClose = false, failRestore = false;
  size_t maxChunk = 0;
  FileOps ops() {
    return {this,
            [](void* c, const char* p) { return static_cast<Files*>(c)->files.count(p) != 0; },
            [](void* c, const char* p) {
              auto& s = *static_cast<Files*>(c);
              if (s.failRemove == p) return false;
              return s.files.erase(p) != 0;
            },
            [](void* c, const char* a, const char* b) {
              auto& s = *static_cast<Files*>(c);
              if ((s.failRenameFrom == a && (s.failRenameTo.empty() || s.failRenameTo == b)) ||
                  (s.failRestore && std::string(a) == BackupPath))
                return false;
              if (!s.files.count(a) || s.files.count(b)) return false;
              s.files[b] = s.files[a];
              s.files.erase(a);
              return true;
            },
            [](void* c, const char* p) {
              auto& s = *static_cast<Files*>(c);
              if (s.failOpen) return false;
              s.active = p;
              s.files[p] = "";
              return true;
            },
            [](void* c, const void* d, size_t n) {
              auto& s = *static_cast<Files*>(c);
              s.maxChunk = std::max(n, s.maxChunk);
              size_t count = s.shortWrite ? n / 2 : n;
              s.files[s.active].append(static_cast<const char*>(d), count);
              return count;
            },
            [](void* c) { return !static_cast<Files*>(c)->failSync; },
            [](void* c) {
              auto& s = *static_cast<Files*>(c);
              s.active.clear();
              return !s.failClose;
            }};
  }
  Result save(const Snapshot& d = {}) {
    auto o = ops();
    Writer w(o.write, this);
    return exportAtomically(o, w, d);
  }
};
JsonDocument parse(const std::string& value) {
  JsonDocument d;
  EXPECT_FALSE(deserializeJson(d, value));
  return d;
}
std::string render(const Snapshot& d) {
  Files f;
  EXPECT_EQ(f.save(d), Result::Saved);
  EXPECT_LE(f.maxChunk, 256);
  return f.files[Path];
}
}  // namespace
TEST(SupportInfo, SchemaAndExplicitNumericAllowlist) {
  Snapshot s;
  s.preferences[readerFontPointSize] = 22;
  s.preferences[clockUtcOffsetQ] = 52;
  s.preferences[displayStatusBarTextSize] = 2;
  s.preferences[statusBarTextSize] = 1;
  s.statusHidden[0] = true;
  auto d = parse(render(s));
  EXPECT_STREQ(d["schema"], "crossink-support");
  EXPECT_EQ(d["version"].as<int>(), 1);
  EXPECT_EQ(d["globalPreferences"]["readerFontPointSize"].as<int>(), 22);
  EXPECT_EQ(d["globalPreferences"]["clockUtcOffsetQ"].as<int>(), 52);
  EXPECT_EQ(d["globalPreferences"]["displayStatusBarTextSize"].as<int>(), 2);
  EXPECT_EQ(d["globalPreferences"]["statusBarTextSize"].as<int>(), 1);
  EXPECT_EQ(d["globalPreferences"]["statusHidden"][0].as<int>(), 1);
  EXPECT_EQ(d["globalPreferences"]["statusHidden"][1].as<int>(), 0);
  auto prefs = d["globalPreferences"].as<JsonObjectConst>();
  for (const char* key :
       {"deviceName", "opdsServerUrl", "opdsUsername", "opdsPassword", "opdsDownloadFolder", "nearbyReceiveFolder",
        "sdFontFamilyName", "dictionarySdFontFamilyName", "password_obf", "openEpubPath"})
    EXPECT_FALSE(prefs.containsKey(key)) << key;
  EXPECT_STREQ(d["bookContext"]["status"], "excluded");
  EXPECT_FALSE(d["bookContext"].as<JsonObjectConst>().containsKey("effectiveReaderPreferences"));
}
TEST(SupportInfo, SimulatorDoesNotInventHardwareNumbers) {
  Snapshot s;
  s.hardware.simulated = true;
  s.hardware.internalFree = 999;
  s.hardware.psramTotal = 999;
  s.hardware.flashBytes = 999;
  auto d = parse(render(s));
  auto about = d["about"].as<JsonObjectConst>();
  EXPECT_STREQ(about["chipAndMemoryStatus"], "unsupported");
  EXPECT_FALSE(about.containsKey("internalFreeBytes"));
  EXPECT_FALSE(about.containsKey("psramTotalBytes"));
  EXPECT_FALSE(about.containsKey("flashBytes"));
  EXPECT_FALSE(about.containsKey("displayPanel"));
}
TEST(SupportInfo, IdentifiedPanelIsSeparateFromController) {
  Snapshot s;
  s.hardware.displayController = "SSD2677";
  s.hardware.displayPanel = "SE0400NQW47";
  auto d = parse(render(s));
  EXPECT_STREQ(d["about"]["displayController"], "SSD2677");
  EXPECT_STREQ(d["about"]["displayPanel"], "SE0400NQW47");
}
TEST(SupportInfo, RuntimePoolsAndAbsentPsramRemainDistinct) {
  Snapshot s;
  s.hardware.internalFree = 321;
  s.hardware.psram = HalDeviceInfo::Presence::Absent;
  s.hardware.sdReady = false;
  auto d = parse(render(s));
  auto a = d["about"].as<JsonObjectConst>();
  EXPECT_EQ(a["internalFreeBytes"].as<int>(), 321);
  EXPECT_STREQ(a["psram"], "absent");
  EXPECT_FALSE(a.containsKey("psramFreeBytes"));
  EXPECT_STREQ(a["sdCapacityStatus"], "unavailable");
  EXPECT_FALSE(a.containsKey("sdCapacityBytes"));
}
TEST(SupportInfo, AvailablePsramAndLargeSdUseBytesWithoutTruncation) {
  Snapshot s;
  s.hardware.psram = HalDeviceInfo::Presence::Available;
  s.hardware.psramTotal = 8388608;
  s.hardware.psramFree = 1234;
  s.hardware.sdReady = true;
  s.hardware.sdBytes = 64ULL * 1024 * 1024 * 1024;
  auto d = parse(render(s));
  EXPECT_EQ(d["about"]["sdCapacityBytes"].as<uint64_t>(), s.hardware.sdBytes);
  EXPECT_EQ(d["about"]["psramFreeBytes"].as<int>(), 1234);
}
TEST(SupportInfo, BookInheritanceAndSafeModeSourcesAreExplicit) {
  Snapshot s;
  s.book.status = BookStatus::Loaded;
  s.book.overrideMask = 1U << Reader_readerFontPointSize;
  s.book.values[Reader_readerFontPointSize] = 27;
  s.book.safeMode = true;
  auto d = parse(render(s));
  auto e = d["bookContext"]["effectiveReaderPreferences"];
  EXPECT_STREQ(e["readerFontPointSize"]["source"], "book_override");
  EXPECT_EQ(e["readerFontPointSize"]["value"].as<int>(), 27);
  EXPECT_STREQ(e["orientation"]["source"], "global_default");
  EXPECT_STREQ(e["embeddedStyle"]["source"], "safe_mode");
  EXPECT_STREQ(d["scope"], "device_and_last_opened_epub");
}
TEST(SupportInfo, InvalidOrUnavailableBookNeverClaimsEffectiveValues) {
  for (auto status : {BookStatus::Invalid, BookStatus::Unavailable}) {
    Snapshot s;
    s.book.status = status;
    s.book.values[0] = 123;
    auto d = parse(render(s));
    EXPECT_FALSE(d["bookContext"].as<JsonObjectConst>().containsKey("effectiveReaderPreferences"));
  }
}
TEST(SupportInfo, JsonEscapesHardwareLabelsAndRetainsUnsignedBounds) {
  Snapshot s;
  s.hardware.device = "test\"\\\n";
  s.hardware.uptimeSeconds = UINT32_MAX;
  auto d = parse(render(s));
  EXPECT_STREQ(d["about"]["deviceProfile"], s.hardware.device);
  EXPECT_EQ(d["about"]["uptimeSeconds"].as<uint32_t>(), UINT32_MAX);
}
TEST(SupportInfo, ConfigurationPresenceDoesNotClaimCredentialsOrConnectivity) {
  Snapshot s;
  s.wifiFile = s.opdsFile = s.koreaderFile = true;
  auto d = parse(render(s));
  auto c = d["configurationStatus"].as<JsonObjectConst>();
  EXPECT_EQ(c["wifiFilePresent"].as<int>(), 1);
  EXPECT_STREQ(c["meaning"], "file_presence_only_not_validated_or_connected");
  EXPECT_FALSE(c.containsKey("configured"));
  EXPECT_FALSE(c.containsKey("connected"));
}
TEST(SupportInfo, SuccessfulReplacementClosesAndRemovesTransactionFiles) {
  Files f;
  f.files[Path] = "old";
  EXPECT_EQ(f.save(), Result::Saved);
  EXPECT_NE(f.files[Path], "old");
  EXPECT_FALSE(f.files.count(TempPath));
  EXPECT_FALSE(f.files.count(BackupPath));
  EXPECT_TRUE(f.active.empty());
  parse(f.files[Path]);
}
TEST(SupportInfo, OpenFailurePreservesPriorExport) {
  Files f;
  f.files[Path] = "old";
  f.failOpen = true;
  EXPECT_EQ(f.save(), Result::Failed);
  EXPECT_EQ(f.files[Path], "old");
}
TEST(SupportInfo, ShortWriteSyncAndCloseFailuresPreservePriorExport) {
  for (unsigned mode = 0; mode < 3; ++mode) {
    Files f;
    f.files[Path] = "old";
    f.shortWrite = mode == 0;
    f.failSync = mode == 1;
    f.failClose = mode == 2;
    EXPECT_EQ(f.save(), Result::Failed);
    EXPECT_EQ(f.files[Path], "old");
    EXPECT_FALSE(f.files.count(TempPath));
    EXPECT_TRUE(f.active.empty());
  }
}
TEST(SupportInfo, BackupRenameFailurePreservesPriorExport) {
  Files f;
  f.files[Path] = "old";
  f.failRenameFrom = Path;
  EXPECT_EQ(f.save(), Result::Failed);
  EXPECT_EQ(f.files[Path], "old");
  EXPECT_FALSE(f.files.count(TempPath));
}
TEST(SupportInfo, InstallFailureRollsBackPreviousExport) {
  Files f;
  f.files[Path] = "old";
  f.failRenameFrom = TempPath;
  EXPECT_EQ(f.save(), Result::Failed);
  EXPECT_EQ(f.files[Path], "old");
  EXPECT_FALSE(f.files.count(TempPath));
  EXPECT_FALSE(f.files.count(BackupPath));
}
TEST(SupportInfo, FailedRollbackLeavesRecoverableBackup) {
  Files f;
  f.files[Path] = "old";
  f.failRenameFrom = TempPath;
  f.failRestore = true;
  EXPECT_EQ(f.save(), Result::RecoveryRequired);
  EXPECT_EQ(f.files[BackupPath], "old");
  EXPECT_FALSE(f.files.count(Path));
  f.failRestore = false;
  f.failRenameFrom.clear();
  EXPECT_EQ(f.save(), Result::Saved);
  parse(f.files[Path]);
}
TEST(SupportInfo, InterruptedReplacementRecoversBeforeNewWrite) {
  Files f;
  f.files[BackupPath] = "old";
  f.files[TempPath] = "partial";
  f.failOpen = true;
  EXPECT_EQ(f.save(), Result::Failed);
  EXPECT_EQ(f.files[Path], "old");
  EXPECT_FALSE(f.files.count(TempPath));
}
TEST(SupportInfo, FailedRecoveryDoesNotDestroyBackup) {
  Files f;
  f.files[BackupPath] = "old";
  f.failRestore = true;
  EXPECT_EQ(f.save(), Result::RecoveryRequired);
  EXPECT_EQ(f.files[BackupPath], "old");
}
TEST(SupportInfo, StaleTransactionCleanupFailureStopsBeforeReplacement) {
  for (const char* stale : {TempPath, BackupPath}) {
    Files f;
    f.files[Path] = "old";
    f.files[stale] = "stale";
    f.failRemove = stale;
    EXPECT_EQ(f.save(), Result::Failed);
    EXPECT_EQ(f.files[Path], "old");
  }
}
TEST(SupportInfo, BackupCleanupFailureReportsSavedWithBackup) {
  Files f;
  f.files[Path] = "old";
  f.failRemove = BackupPath;
  EXPECT_EQ(f.save(), Result::SavedBackupRetained);
  EXPECT_EQ(f.files[BackupPath], "old");
  parse(f.files[Path]);
}
TEST(SupportInfo, NewExportInstallFailureNeverLeavesPartialDestination) {
  Files f;
  f.failRenameFrom = TempPath;
  EXPECT_EQ(f.save(), Result::Failed);
  EXPECT_FALSE(f.files.count(Path));
  EXPECT_FALSE(f.files.count(TempPath));
}

TEST(SupportInfo, BookScalarsMatchReaderClampsAndFallbacks) {
  Snapshot s;
  s.preferences[fontFamily] = 1;
  s.preferences[orientation] = 2;
  s.preferences[paragraphAlignment] = 3;
  s.preferences[imageRendering] = 2;
  auto& b = s.book;
  for (auto& value : b.values) value = 255;
  normalizeBookValues(b, s.preferences);
  EXPECT_EQ(b.values[Reader_fontFamily], 1);
  EXPECT_TRUE(b.fallbackMask & (1U << Reader_fontFamily));
  EXPECT_EQ(b.values[Reader_orientation], 2);
  EXPECT_EQ(b.values[Reader_paragraphAlignment], 3);
  EXPECT_EQ(b.values[Reader_imageRendering], 2);
  EXPECT_EQ(b.values[Reader_imageGrayscale], 1);
  EXPECT_EQ(b.values[Reader_lineHeightPercent], 200);
  EXPECT_EQ(b.values[Reader_wordSpacing], 8);
  EXPECT_EQ(b.values[Reader_screenMarginVertical], 150);
  EXPECT_EQ(b.values[Reader_indexingMethod], 1);
  EXPECT_EQ(b.values[Reader_embeddedStyle], 1);
  b.values[Reader_lineHeightPercent] = 0;
  b.values[Reader_screenMarginHorizontal] = 0;
  normalizeBookValues(b, s.preferences);
  EXPECT_EQ(b.values[Reader_lineHeightPercent], 70);
  EXPECT_EQ(b.values[Reader_screenMarginHorizontal], 5);
}
TEST(SupportInfo, UnresolvedLegacyCustomFontSizeNeverLooksEffective) {
  Snapshot s;
  s.book.status = BookStatus::Loaded;
  s.book.fontPointSizeKnown = false;
  s.book.values[Reader_readerFontPointSize] = 2;
  s.book.overrideMask = 2;
  auto d = parse(render(s));
  auto f = d["bookContext"]["effectiveReaderPreferences"]["readerFontPointSize"];
  EXPECT_TRUE(f["value"].isNull());
  EXPECT_STREQ(f["source"], "book_override");
  EXPECT_STREQ(f["status"], "unavailable_legacy_custom_font_size");
}
