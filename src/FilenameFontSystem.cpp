#include "FilenameFontSystem.h"

#include <FontFileTypes.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "fontIds.h"

FilenameFontSystem filenameFontSystem;

void FilenameFontSystem::invalidateForPath(const char* path) {
  if (!path) return;
  const size_t length = std::strlen(FONT_DIR);
  if (strncasecmp(path, FONT_DIR, length) == 0 && (path[length] == '/' || path[length] == '\0')) invalidate();
}

#if CROSSINK_SCALABLE_FONTS
namespace {
constexpr size_t MaxFamilies = 64;
struct ScanScratch {
  char directory[128];
  char path[256];
  char name[128];
  HalScalableFont::Info info;
};
bool validFamilyName(const char* name) {
  return name && name[0] && std::strlen(name) < 64 && !std::strchr(name, '/') && !std::strchr(name, '\\') &&
         std::strcmp(name, ".") != 0 && std::strcmp(name, "..") != 0;
}
}  // namespace

FontLoadError FilenameFontSystem::scanFamily(const char* name, SdCardFontFamilyInfo& family) {
  if (!validFamilyName(name)) return FontLoadError::InvalidFamily;
  // Cold-path scratch (~600 bytes) is too large for the render-task stack. One
  // fallible allocation is reused across every entry in the selected directory.
  auto scratch = makeUniqueNoThrow<ScanScratch>();
  if (!scratch) {
    LOG_ERR("FNFONT", "Cannot allocate font scan scratch");
    return FontLoadError::Memory;
  }
  std::snprintf(scratch->directory, sizeof(scratch->directory), "%s/%s", FONT_DIR, name);
  HalFile dir = Storage.open(scratch->directory);
  if (!dir || !dir.isDirectory()) {
    const auto error = dir.allocationFailed() ? FontLoadError::Memory : FontLoadError::Read;
    dir.close();
    return error;
  }
  family.name = name;
  family.files.reserve(2);
  bool failed = false;
  bool candidate = false;
  FontLoadError error = FontLoadError::NoFont;
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) {
      if (dir.allocationFailed() || entry.allocationFailed()) {
        failed = true;
        error = FontLoadError::Memory;
      }
      entry.close();
      break;
    }
    const bool directory = entry.isDirectory();
    const size_t nameLength = entry.getName(scratch->name, sizeof(scratch->name));
    entry.close();
    const size_t length = std::strlen(scratch->name);
    if (directory || nameLength >= sizeof(scratch->name) - 1 || length < 5 || !fontfiles::isScalable(scratch->name))
      continue;
    // macOS resource-fork sidecars are metadata, not font candidates.
    if (std::strncmp(scratch->name, "._", 2) == 0) continue;
    candidate = true;
    const int written = std::snprintf(scratch->path, sizeof(scratch->path), "%s/%s", scratch->directory, scratch->name);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(scratch->path)) {
      if (!failed) error = FontLoadError::InvalidFamily;
      continue;
    }
    uint8_t style = 0;
    // SdFat cannot reopen the same path while the selected face streams it.
    if (regular_ && regularPath_ == scratch->path) {
      style = 0;
    } else if (bold_ && boldPath_ == scratch->path) {
      style = 1;
    } else {
      FontLoadError fileError;
      if (!HalScalableFont::inspectFile(scratch->path, scratch->info, nullptr, HalScalableFont::MaxFilenameFileBytes,
                                        &fileError)) {
        if (fileError == FontLoadError::Memory || fileError == FontLoadError::Read) {
          failed = true;
          if (error != FontLoadError::Memory) error = fileError;
        } else if (!failed) {
          error = fileError;
        }
        continue;
      }
      if (scratch->info.variable) {
        if (!failed) error = FontLoadError::Unsupported;
        continue;
      }
      style = scratch->info.style;
    }
    if (style > 1) {
      if (!failed) error = FontLoadError::InvalidFamily;
      continue;  // regular/bold only; no italic-only families
    }
    if (family.findFile(0, style)) {
      LOG_ERR("FNFONT", "Duplicate style %u in %s", unsigned(style), name);
      failed = true;
      if (error != FontLoadError::Memory && error != FontLoadError::Read) error = FontLoadError::InvalidFamily;
      break;
    }
    family.files.push_back({scratch->path, 0, style});
  }
  if (FsHelpers::directoryIterationFailed(dir)) {
    failed = true;
    if (error != FontLoadError::Memory) error = FontLoadError::Read;
  }
  dir.close();
  if (failed) LOG_ERR("FNFONT", "Cannot scan filename font %s", name);
  if (!failed && family.findFile(0, 0)) return FontLoadError::None;
  return error != FontLoadError::NoFont ? error : candidate ? FontLoadError::InvalidFamily : FontLoadError::NoFont;
}

bool FilenameFontSystem::discover(std::vector<Family>& families) {
  families.clear();
  lastError_ = FontLoadError::None;
  ScalableFontAccess access;
  HalFile root = Storage.open(FONT_DIR);
  if (!root) {
    const bool failed = root.allocationFailed();
    if (failed) lastError_ = FontLoadError::Memory;
    root.close();
    return !failed;
  }
  if (!root.isDirectory()) {
    lastError_ = FontLoadError::Read;
    root.close();
    return false;
  }
  families.reserve(MaxFamilies + 1);  // The picker prepends its None option.
  char name[64] = {};
  bool failed = false;
  while (true) {
    HalFile entry = root.openNextFile();
    if (!entry) {
      failed = root.allocationFailed() || entry.allocationFailed();
      if (failed) lastError_ = FontLoadError::Memory;
      entry.close();
      break;
    }
    const bool directory = entry.isDirectory();
    const size_t length = entry.getName(name, sizeof(name));
    entry.close();
    if (!directory || length >= sizeof(name) - 1 || !validFamilyName(name)) continue;
    if (families.size() == MaxFamilies) {
      LOG_ERR("FNFONT", "Too many filename font families (limit=%u)", unsigned(MaxFamilies));
      failed = true;
      lastError_ = FontLoadError::Read;
      break;
    }
    SdCardFontFamilyInfo family;
    const auto error = scanFamily(name, family);
    if (error != FontLoadError::NoFont) families.push_back({name, error});
  }
  failed = failed || FsHelpers::directoryIterationFailed(root);
  root.close();
  if (failed) {
    if (lastError_ == FontLoadError::None) lastError_ = FontLoadError::Read;
    LOG_ERR("FNFONT", "Cannot discover filename fonts");
    families.clear();
    return false;
  }
  std::sort(families.begin(), families.end(), [](const Family& a, const Family& b) { return a.name < b.name; });
  return true;
}
#endif

void FilenameFontSystem::release(GfxRenderer& renderer) {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess access;
  renderer.clearFilenameFallbacks();  // unregister borrowed font pointers first
  bold_.reset();
  regular_.reset();
  regularPath_.clear();
  boldPath_.clear();
  attemptedFamily_[0] = '\0';
  lastError_ = FontLoadError::None;
#endif
  invalidate();
}

bool FilenameFontSystem::ensureLoaded(GfxRenderer& renderer) {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess access;
  const char* wanted = SETTINGS.filenameFallbackFont;
  if (suspensionCount_) return !wanted[0];
  const bool dirty = dirty_.exchange(false, std::memory_order_acq_rel);
  if (!dirty && std::strcmp(attemptedFamily_, wanted) == 0) return !wanted[0] || regular_ != nullptr;
  release(renderer);
  dirty_.store(false, std::memory_order_release);
  std::strncpy(attemptedFamily_, wanted, sizeof(attemptedFamily_) - 1);
  if (!wanted[0]) return true;
  SdCardFontFamilyInfo family;
  lastError_ = scanFamily(wanted, family);
  if (lastError_ != FontLoadError::None) {
    LOG_ERR("FNFONT", "Filename font %s unavailable; using built-in glyphs", wanted);
    return false;
  }
  const auto regularFile = family.findFile(0, 0);
  const auto boldFile = family.findFile(0, 1);
  size_t regularBytes = 0;
  if (!HalScalableFont::fileSize(regularFile->path.c_str(), regularBytes, HalScalableFont::MaxFilenameFileBytes,
                                 &lastError_))
    return false;
  // Owned once until selection changes or storage/network work releases it.
  // Font bytes, descriptors and rendering scratch use the existing bounded PSRAM path.
  freeink::font::FtFont::RenderOptions options;
  options.hinting = freeink::font::FtFont::HintingMode::Auto;
  regular_ = makeUniqueNoThrow<HalScalableFont>();
  if (!regular_ || !regular_->openFile(regularFile->path.c_str(), HalScalableFont::MaxFilenameFileBytes, options,
                                       HalScalableFont::FileMode::Filename, boldFile ? 1 : 0)) {
    const auto error = regular_ ? regular_->lastLoadError() : FontLoadError::Memory;
    LOG_ERR("FNFONT", "Cannot open regular face in %s", wanted);
    release(renderer);
    lastError_ = error;
    dirty_.store(false, std::memory_order_release);
    std::strncpy(attemptedFamily_, wanted, sizeof(attemptedFamily_) - 1);
    return false;
  }
  regularPath_ = regularFile->path;
  if (boldFile) {
    bold_ = makeUniqueNoThrow<HalScalableFont>();
    if (!bold_ || !bold_->openFile(boldFile->path.c_str(), HalScalableFont::MaxFilenameFileBytes, options,
                                   HalScalableFont::FileMode::Filename)) {
      LOG_ERR("FNFONT", "Cannot open bold face in %s; using regular", wanted);
      bold_.reset();
    } else {
      boldPath_ = boldFile->path;
    }
  }
  static constexpr int primaryIds[] = {SMALL_FONT_ID, UI_10_FONT_ID, UI_12_FONT_ID};
  static constexpr uint8_t sizes[] = {8, 10, 12};
  for (size_t i = 0; i < 3; ++i) {
    const auto regular = regular_->atSize(sizes[i]);
    const auto bold = bold_ ? bold_->atSize(sizes[i]) : nullptr;
    if (!renderer.setFilenameFallback(primaryIds[i], -101 - static_cast<int>(i), regular, bold)) {
      const auto error = regular ? FontLoadError::Read : FontLoadError::Memory;
      LOG_ERR("FNFONT", "Cannot prepare filename font UI sizes");
      release(renderer);
      lastError_ = error;
      return false;
    }
  }
  LOG_INF("FNFONT", "Loaded filename fallback %s (%s)", wanted, bold_ ? "regular/bold" : "regular");
#endif
  return true;
}

void FilenameFontSystem::suspend(GfxRenderer& renderer) {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess access;
  ++suspensionCount_;
  release(renderer);
#endif
}

void FilenameFontSystem::resume() {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess access;
  if (suspensionCount_) --suspensionCount_;
  invalidate();
#endif
}

uint32_t FilenameFontSystem::fingerprint() const {
#if CROSSINK_SCALABLE_FONTS
  return (regular_ ? regular_->fingerprint() : 0) ^ (bold_ ? bold_->fingerprint() * 16777619u : 0);
#else
  return 0;
#endif
}
