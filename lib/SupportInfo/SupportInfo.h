#pragma once
#include <HalDeviceInfo.h>

#include <cstddef>
#include <cstdint>

namespace SupportInfo {
// Deliberately manual numeric-only allowlist. Never derive this from the settings catalog.
enum Preference {
#define SUPPORT_PREF(name) name,
#include "SupportPreferences.inc"
#undef SUPPORT_PREF
  PreferenceCount
};
enum ReaderField {
#define SUPPORT_READER(name) Reader_##name,
#include "SupportReaderFields.inc"
#undef SUPPORT_READER
  ReaderFieldCount
};
enum class BookStatus : uint8_t { Excluded, Unavailable, Inherited, Loaded, Invalid };
struct BookContext {
  BookStatus status = BookStatus::Excluded;
  uint32_t overrideMask = 0;
  uint32_t fallbackMask = 0;
  uint8_t values[ReaderFieldCount]{};
  bool fontPointSizeKnown = true;
  bool customFont = false;
  bool fontOverride = false;
  bool dictionaryOverride = false;
  bool dictionaryCustomFont = false;
  uint8_t dictionaryPointSize = 0;
  bool safeMode = false;
  bool renderModeOverride = false;
  uint8_t renderMode = 0;
  bool autoPageTurnOverride = false;
  uint16_t autoPageTurnSeconds = 0;
  bool statsEnabled = false;
};
struct Snapshot {
  HalDeviceInfo::Snapshot hardware;
  uint16_t preferences[PreferenceCount]{};
  uint8_t statusSlots[17]{};   // top 7, bottom 7, display 3; choices, never rendered titles.
  uint8_t statusOptions[9]{};  // percentage/bar/thickness for top and bottom, then top/bottom/display battery.
  bool statusHidden[2]{};      // top and bottom, independent of their assigned slots.
  uint8_t quickActions[5]{};
  bool customFont = false;
  bool dictionaryCustomFont = false;
  bool wifiFile = false;
  bool opdsFile = false;
  bool koreaderFile = false;
  bool fontProfilesFile = false;
  BookContext book;
  const char* version = nullptr;  // canonical version, never a branch-bearing label.
  const char* sourceSha = nullptr;
  const char* sdkSha = nullptr;
  const char* target = nullptr;
  const char* dirty = nullptr;
};
// Mirrors applyReaderSettings scalar normalization; adapter resolves built-in legacy sizes.
void normalizeBookValues(BookContext& book, const uint16_t* globalPreferences);
using Write = size_t (*)(void*, const void*, size_t);
// Fixed 256-byte streaming buffer; no document allocation or private string input.
class Writer {
 public:
  Writer(Write write, void* ctx) : write_(write), ctx_(ctx) {}
  bool snapshot(const Snapshot& data);

 private:
  Write write_;
  void* ctx_;
  char buffer_[256]{};
  size_t used_ = 0;
  bool ok_ = true;
  void text(const char* value);
  void string(const char* value);
  void number(uint64_t value);
  void field(const char* key, uint64_t value);
  void stringField(const char* key, const char* value);
  bool flush();
};
struct FileOps {
  void* ctx;
  bool (*exists)(void*, const char*);
  bool (*remove)(void*, const char*);
  bool (*rename)(void*, const char*, const char*);
  bool (*open)(void*, const char*);
  Write write;
  bool (*sync)(void*);
  bool (*close)(void*);
};
enum class Result : uint8_t { Saved, SavedBackupRetained, Failed, RecoveryRequired, OutOfMemory };
constexpr const char* Path = "/crossink-support.json";
constexpr const char* TempPath = "/crossink-support.json.tmp";
constexpr const char* BackupPath = "/crossink-support.json.bak";
Result exportAtomically(const FileOps& ops, Writer& writer, const Snapshot& data);
}  // namespace SupportInfo
