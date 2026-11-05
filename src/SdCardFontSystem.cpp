#include "SdCardFontSystem.h"
#if CROSSINK_SCALABLE_FONTS
#include <HalScalableFont.h>
#include <ScalableBuiltins.h>
#endif

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MemoryBudget.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "FilenameFontSystem.h"
#if CROSSINK_SCALABLE_FONTS
#include "TtfRenderProfileStore.h"
#endif
#include "fontIds.h"

namespace {
enum class FontFileSelection : uint8_t { Closest, Exact };

// This is a cold setup path, not a render loop. The 320-byte stack footprint
// (path and filename) replace a heap-allocated whole-font catalog
// during every dictionary swap, avoiding persistent fragmentation on the C3.
bool findInstalledFontFile(const char* familyName, const uint8_t targetPointSize, const FontFileSelection selection,
                           char* path, const size_t pathSize, uint8_t& selectedPointSize,
                           const bool contentGrowth = false) {
  if (!familyName || familyName[0] == '\0' || !path || pathSize == 0) return false;

  const char* root = SdCardFontRegistry::findFamilyRoot(familyName);
  if (!root) return false;
  const int directoryLength = std::snprintf(path, pathSize, "%s/%s", root, familyName);
  if (directoryLength <= 0 || static_cast<size_t>(directoryLength) >= pathSize) return false;

  HalFile dir = Storage.open(path);
  if (!dir || !dir.isDirectory()) return false;

  uint8_t closestSize = 0;
  uint8_t closestDiff = UINT8_MAX;
  char filename[128] = {};
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    const bool isDirectory = entry.isDirectory();
    if (!isDirectory) entry.getName(filename, sizeof(filename));
    entry.close();
    if (isDirectory) continue;

    uint8_t pointSize = 0;
    uint8_t style = 0;
    if (!SdCardFontRegistry::parseFilename(filename, pointSize, style) || style != 0) continue;

    // Optional growth follows the renderer's tie-break within its supported
    // content range; body/dictionary selections retain their existing policy.
    if (contentGrowth && (pointSize < 8 || pointSize > 44)) continue;
    if (selection == FontFileSelection::Closest) {
      const uint8_t diff = pointSize > targetPointSize ? pointSize - targetPointSize : targetPointSize - pointSize;
      if (closestDiff == UINT8_MAX || diff < closestDiff ||
          (diff == closestDiff && (contentGrowth ? pointSize > closestSize : pointSize < closestSize))) {
        closestSize = pointSize;
        closestDiff = diff;
      }
    }
  }
  dir.close();

  if (selection == FontFileSelection::Closest) {
    selectedPointSize = closestSize;
  } else if (selection == FontFileSelection::Exact) {
    selectedPointSize = targetPointSize;
  }

  if (selectedPointSize == 0) return false;
  // Scan once more to preserve the exact file name rather than assuming the
  // file base name matches the directory name.
  dir = Storage.open(path);
  if (!dir || !dir.isDirectory()) return false;
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    const bool isDirectory = entry.isDirectory();
    if (!isDirectory) entry.getName(filename, sizeof(filename));
    entry.close();
    if (isDirectory) continue;

    uint8_t pointSize = 0;
    uint8_t style = 0;
    if (!SdCardFontRegistry::parseFilename(filename, pointSize, style) || style != 0 ||
        pointSize != selectedPointSize) {
      continue;
    }
    const int pathLength = std::snprintf(path, pathSize, "%s/%s/%s", root, familyName, filename);
    dir.close();
    return pathLength > 0 && static_cast<size_t>(pathLength) < pathSize;
  }
  dir.close();
  return false;
}

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer, const bool loadReaderFont) {
  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;

  if (!loadReaderFont) return;

  if (SETTINGS.sdFontFamilyName[0] == '\0') {
    LOG_DBG("SDFS", "SD font resolver ready; discovery deferred until requested");
    return;
  }

#if CROSSINK_SCALABLE_FONTS
  LOG_DBG("SDFS", "SD font resolver ready; selected font load deferred until requested");
#else
  // C3 bitmap fonts retain their existing load timing and memory budget.
  ensureLoaded(renderer);
  releaseRegistry();
#endif
}

int SdCardFontSystem::ensureBuiltInReaderFont(GfxRenderer& renderer) {
#if CROSSINK_SCALABLE_FONTS
  ensureScalableBuiltinFamily(renderer, SETTINGS.fontFamily == CrossPointSettings::BITTER ? 1 : 0);
#else
  (void)renderer;
#endif
  return SETTINGS.getBuiltInReaderFontId();
}

void SdCardFontSystem::persistSettingsChange() const {
  if (settingsPersistenceCallback_) {
    settingsPersistenceCallback_(settingsPersistenceContext_);
  } else {
    SETTINGS.saveToFile();
  }
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess access;
#endif
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  bool registryWasDirty = registryDirty_.load(std::memory_order_acquire) || fontReloadPending_ ||
                          loadedRegistryRevision_ != registry_.revision();

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();
  uint8_t targetPointSize = SETTINGS.getSdFontTargetPointSize();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
      loadedFontPointSize_ = 0;
    }
#if CROSSINK_SCALABLE_FONTS
    ensureBuiltInReaderFont(renderer);
#endif
    return;
  }

  if (!registryWasDirty && !manager_.hasTemporaryScalableFamily() && currentFamily == wantedFamily &&
      loadedFontPointSize_ == targetPointSize && SETTINGS.legacySdFontSizeStep == UINT8_MAX) {
    return;
  }

  ensureRegistry();
  if (registry_.lastDiscoveryFailed()) {
#if CROSSINK_SCALABLE_FONTS
    ensureBuiltInReaderFont(renderer);
#endif
    return;
  }
  registryWasDirty = registryWasDirty || fontReloadPending_ || loadedRegistryRevision_ != registry_.revision();

  const auto* family = registry_.findSummary(wantedFamily);
  if (family && !family->ensureDetails()) {
    LOG_ERR("SDFS", "Cannot read SD font details: %s (preserving selection)", wantedFamily);
#if CROSSINK_SCALABLE_FONTS
    ensureBuiltInReaderFont(renderer);
#endif
    return;
  }
  if (family && SETTINGS.legacySdFontSizeStep != UINT8_MAX) {
    const auto sizes = family->availableSizes();
    if (!sizes.empty()) {
      const uint8_t step = std::min<uint8_t>(SETTINGS.legacySdFontSizeStep, sizes.size() - 1);
      targetPointSize = sizes[step];
      SETTINGS.readerFontPointSize = targetPointSize;
      SETTINGS.legacySdFontSizeStep = UINT8_MAX;
      persistSettingsChange();
      LOG_INF("SDFS", "Migrated SD font size to %u pt", targetPointSize);
    }
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.sdFontFamilyName[0] = '\0';
      persistSettingsChange();
#if CROSSINK_SCALABLE_FONTS
      ensureBuiltInReaderFont(renderer);
#endif
      return;
    }
    const auto* wantedFile = family->findClosestFile(targetPointSize);
    uint8_t wantedPt = family->isScalable() ? targetPointSize : (wantedFile ? wantedFile->pointSize : 0);
    if (!registryWasDirty && !manager_.hasTemporaryScalableFamily() && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (target %u)%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            targetPointSize, registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty() && (!family || !family->isScalable() || !familyMatches || registryWasDirty)) {
    manager_.unloadAll(renderer);
  }

  if (family) {
#if CROSSINK_SCALABLE_FONTS
    const auto options = family->isScalable() ? ttfRenderOptions(TTF_RENDER_PROFILES.profileFor(wantedFamily))
                                              : freeink::font::FtFont::RenderOptions{};
    const bool loaded = family->isScalable() ? manager_.loadFamilyClosest(*family, renderer, targetPointSize, options)
                                             : manager_.loadFamilyClosest(*family, renderer, targetPointSize);
#else
    const bool loaded = manager_.loadFamilyClosest(*family, renderer, targetPointSize);
#endif
    if (loaded) {
      loadedFontPointSize_ = targetPointSize;
      fontReloadPending_ = false;
      loadedRegistryRevision_ = registry_.revision();
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (preserving selection)", wantedFamily);
#if CROSSINK_SCALABLE_FONTS
      ensureBuiltInReaderFont(renderer);
#endif
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
    persistSettingsChange();
#if CROSSINK_SCALABLE_FONTS
    ensureBuiltInReaderFont(renderer);
#endif
  }
}

bool SdCardFontSystem::isScalableFamily(const char* familyName) {
#if CROSSINK_SCALABLE_FONTS
  if (!familyName || !familyName[0]) return false;
  ensureRegistry();
  const auto* family = registry_.findFamily(familyName);
  return family && family->ensureDetails() && family->isScalable();
#else
  (void)familyName;
  return false;
#endif
}

bool SdCardFontSystem::fontUsesMonochromeRaster(const GfxRenderer& renderer, const int fontId,
                                                const char* familyName) const {
#if CROSSINK_SCALABLE_FONTS
  return renderer.isSdCardFont(fontId) && hasResidentScalableFamily(familyName) &&
         TTF_RENDER_PROFILES.profileFor(familyName).raster != 0;
#else
  (void)renderer;
  (void)fontId;
  (void)familyName;
  return false;
#endif
}

bool SdCardFontSystem::reloadActiveScalableFamily(GfxRenderer& renderer, const char* familyName) {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess access;
  if (!hasResidentScalableFamily(familyName)) return false;
  const auto options = ttfRenderOptions(TTF_RENDER_PROFILES.profileFor(familyName));
  if (!manager_.setScalableRenderOptions(renderer, options)) return false;
  scalableRenderOptionsGeneration_++;
  return true;
#else
  (void)renderer;
  (void)familyName;
  return false;
#endif
}

void SdCardFontSystem::releaseLoadedFont(GfxRenderer& renderer) {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess access;
#endif
  if (manager_.currentFamilyName().empty()) return;

  const std::string familyName = manager_.currentFamilyName();
  (void)familyName;
  manager_.unloadAll(renderer);
  loadedFontPointSize_ = 0;
  LOG_DBG("SDFS", "Released SD card font before low-memory operation: %s", familyName.c_str());
}

void SdCardFontSystem::ensureRegistry() {
  const bool dirty = registryDirty_.exchange(false, std::memory_order_acq_rel);
  if (dirty) fontReloadPending_ = true;
  if (registryLoaded_ && !dirty && !registry_.needsRefresh()) return;
  if (dirty) LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
  registry_.loadNames();
  if (registry_.lastDiscoveryFailed()) {
    LOG_ERR("SDFS", "SD font registry scan failed (free=%u maxAlloc=%u)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    registryDirty_.store(true, std::memory_order_release);
    return;
  }
  registryLoaded_ = true;
}

void SdCardFontSystem::releaseRegistry() {
  // FontInstaller can populate the shared registry directly.
  LOG_DBG("SDFS", "Releasing SD font catalog (%d families)", registry_.getFamilyCount());
  registry_.clear();
  registryLoaded_ = false;
}

void SdCardFontSystem::releaseForNetwork(GfxRenderer& renderer) {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess access;
#endif
  releaseLoadedFont(renderer);
  // UI fallback is shared by translated network screens too. It uses the
  // existing S3/PSRAM font budget; storage-ownership flows suspend it explicitly.

  releaseRegistry();
  registryDirty_.store(true, std::memory_order_release);
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
  // Return the active body-size identity; optional content sizes are selected
  // internally by the renderer and must not change the saved reader size.
  // ensureLoaded() must have been called with the current settings before this.
  return manager_.getFontId(familyName);
}

bool SdCardFontSystem::changeReaderFontSize(const bool larger, const FontSizeStepMode mode) {
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    refreshIfDirty();
    if (registry_.lastDiscoveryFailed()) return false;
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (!family->ensureDetails()) return false;
      const auto sizes = family->availableSizes();
      if (changeReaderFontSizeStep(sizes.data(), sizes.size(), SETTINGS.readerFontPointSize, larger, mode)) return true;
      if (sizes.size() > 0) return false;
    }
  }

  return SETTINGS.changeReaderFontSize(larger, mode);
}

uint8_t SdCardFontSystem::resolveLegacySizeStep(const char* familyName, const uint8_t sizeStep) {
  ensureRegistry();
  const auto* family = familyName ? registry_.findFamily(familyName) : nullptr;
  if (family) {
    const auto sizes = family->availableSizes();
    if (!sizes.empty()) return sizes[std::min<uint8_t>(sizeStep, sizes.size() - 1)];
  }
  return CrossPointSettings::getSdFontRangePointSize(SETTINGS.sdFontSizeRange, sizeStep);
}

DictionaryFontActivation SdCardFontSystem::activateDictionaryFont(GfxRenderer& renderer, const char* familyName,
                                                                  uint8_t targetPointSize) {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess access;
#endif
  // A non-zero size with no dedicated family means "use the reader's installed
  // family at this size". This keeps the setting useful when the same custom
  // family is wanted for reading and definitions without keeping two families
  // resident.
  if ((!familyName || familyName[0] == '\0') && targetPointSize != 0 && SETTINGS.sdFontFamilyName[0] != '\0') {
    familyName = SETTINGS.sdFontFamilyName;
  }
  if (!familyName || familyName[0] == '\0') {
    return {restoreReaderFont(renderer), false};
  }

#if CROSSINK_SCALABLE_FONTS
  ensureRegistry();
  if (const auto* family = registry_.findFamily(familyName); family && family->isScalable()) {
    const uint8_t size = targetPointSize ? targetPointSize : SETTINGS.getSdFontTargetPointSize();
    const auto options = ttfRenderOptions(TTF_RENDER_PROFILES.profileFor(familyName));
    if (manager_.loadDictionaryFamily(*family, renderer, size, options)) return {manager_.getFontId(familyName), true};
    return {restoreReaderFont(renderer), false};
  }
#endif
  MemoryBudget::logHeapShape("dict.font_before_activate");
  char path[160] = {};
  uint8_t selectedPointSize = 0;
  // Prefer the actual loaded reader-file size. Built-in readers have no SD
  // file, so use their effective physical point size instead.
  if (targetPointSize == 0) {
    targetPointSize = manager_.currentPointSize() != 0 ? manager_.currentPointSize()
                                                       : closestBuiltinReaderPointSize(SETTINGS.readerFontPointSize);
  }
  if (!findInstalledFontFile(familyName, targetPointSize, FontFileSelection::Closest, path, sizeof(path),
                             selectedPointSize)) {
    LOG_DBG("SDFS", "Dictionary font not found on card: %s", familyName);
    const char* globalFamilyName = SETTINGS.dictionarySdFontFamilyName;
    if (globalFamilyName[0] != '\0' && std::strcmp(familyName, globalFamilyName) != 0) {
      LOG_DBG("SDFS", "Using global dictionary font while per-book font is unavailable: %s", globalFamilyName);
      return activateDictionaryFont(renderer, globalFamilyName, SETTINGS.dictionaryFontPointSize);
    }
    const int readerFontId = restoreReaderFont(renderer);
    MemoryBudget::logHeapShape("dict.font_reader_fallback");
    return {readerFontId, false};
  }

  if (manager_.currentFamilyName() == familyName && manager_.currentPointSize() == selectedPointSize) {
    const int fontId = manager_.getFontId(manager_.currentFamilyName());
    MemoryBudget::logHeapShape("dict.font_reused_reader");
    return {fontId, true};
  }

  // A reader SD font can retain page glyphs, kerning, and advance tables after
  // a long reading session. They are disposable at this handoff: keeping them
  // through the headroom check makes a dictionary font appear unavailable until
  // its book cache is deleted or the heap happens to be less fragmented.
  const int activeReaderFontId = SETTINGS.getReaderFontId();
  const auto beforeCacheRelease = MemoryBudget::snapshot();
  if (renderer.releaseSdCardFontForLowMemory(activeReaderFontId)) {
    const auto afterCacheRelease = MemoryBudget::snapshot();
    LOG_DBG("SDFS", "Released reader SD-font caches before dictionary swap: free=%u->%u maxAlloc=%u->%u",
            beforeCacheRelease.freeHeap, afterCacheRelease.freeHeap, beforeCacheRelease.maxAllocHeap,
            afterCacheRelease.maxAllocHeap);
  }

  auto heap = MemoryBudget::snapshot();
  if (!MemoryBudget::hasHeapForDictionarySdFont(heap)) {
    // The reader family itself is also disposable for a dictionary swap. Retry
    // after releasing it so its font data does not cause a false low-memory
    // fallback.
    const auto beforeReaderUnload = heap;
    if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
    loadedFontPointSize_ = 0;
    heap = MemoryBudget::snapshot();
    LOG_DBG("SDFS", "Released reader font before dictionary swap retry: free=%u->%u maxAlloc=%u->%u",
            beforeReaderUnload.freeHeap, heap.freeHeap, beforeReaderUnload.maxAllocHeap, heap.maxAllocHeap);
  }
  if (!MemoryBudget::hasHeapForDictionarySdFont(heap)) {
    LOG_ERR("SDFS", "Low heap for dictionary font swap (%u free, %u max alloc, need %u/%u); using reader font",
            heap.freeHeap, heap.maxAllocHeap, MemoryBudget::DICTIONARY_SD_FONT_MIN_FREE,
            MemoryBudget::DICTIONARY_SD_FONT_MIN_MAX_ALLOC);
    const int readerFontId = restoreReaderFont(renderer);
    MemoryBudget::logHeapShape("dict.font_heap_fallback");
    return {readerFontId, false};
  }

  // unloadAll() also drops optional CJK UI sizes before the dictionary file is
  // allocated, so both families are never resident at once.
  if (!manager_.currentFamilyName().empty()) {
    manager_.unloadAll(renderer);
  }
  loadedFontPointSize_ = 0;

  if (manager_.loadFamilyFile(path, familyName, selectedPointSize, renderer)) {
    const int fontId = manager_.getFontId(manager_.currentFamilyName());
    LOG_DBG("SDFS", "Activated dictionary font %s at %u pt", familyName, manager_.currentPointSize());
    MemoryBudget::logHeapShape("dict.font_after_activate");
    return {fontId, true};
  }

  LOG_ERR("SDFS", "Failed to load dictionary font %s; restoring reader font", familyName);
  const int readerFontId = restoreReaderFont(renderer);
  MemoryBudget::logHeapShape("dict.font_reader_fallback");
  return {readerFontId, false};
}

int SdCardFontSystem::restoreReaderFont(GfxRenderer& renderer) {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess access;
#endif
  const char* familyName = SETTINGS.sdFontFamilyName;
  if (!familyName || familyName[0] == '\0') {
    if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
    loadedFontPointSize_ = 0;
    MemoryBudget::logHeapShape("dict.font_after_restore");
    return ensureBuiltInReaderFont(renderer);
  }

#if CROSSINK_SCALABLE_FONTS
  ensureRegistry();
  if (const auto* family = registry_.findFamily(familyName); family && family->isScalable()) {
    const auto options = ttfRenderOptions(TTF_RENDER_PROFILES.profileFor(familyName));
    if (manager_.loadFamilyClosest(*family, renderer, SETTINGS.getSdFontTargetPointSize(), options)) {
      loadedFontPointSize_ = SETTINGS.getSdFontTargetPointSize();
      return manager_.getFontId(familyName);
    }
    return ensureBuiltInReaderFont(renderer);
  }
#endif
  char path[160] = {};
  uint8_t selectedPointSize = 0;
  if (!findInstalledFontFile(familyName, SETTINGS.getSdFontTargetPointSize(), FontFileSelection::Closest, path,
                             sizeof(path), selectedPointSize)) {
    LOG_ERR("SDFS", "Reader font unavailable while restoring: %s", familyName);
    if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
    loadedFontPointSize_ = 0;
    MemoryBudget::logHeapShape("dict.font_after_restore");
    return ensureBuiltInReaderFont(renderer);
  }

  if (manager_.currentFamilyName() != familyName || manager_.currentPointSize() != selectedPointSize) {
    if (!manager_.currentFamilyName().empty()) manager_.unloadAll(renderer);
    // Discover just the body and two candidates, rather than allocating the
    // whole catalog on a tight dictionary-return heap. At most three short
    // paths are heap-owned here and freed before the reader resumes.
    SdCardFontFamilyInfo contentFamily;
    contentFamily.name = familyName;
    contentFamily.files.reserve(3);
    contentFamily.files.push_back({path, selectedPointSize, 0});
    const uint8_t bodySize = selectedPointSize;
    if (bodySize >= 8 && bodySize <= 44) {
      for (const uint8_t target : manager_.contentSizeTargetsForBody(bodySize)) {
        if (findInstalledFontFile(familyName, target, FontFileSelection::Closest, path, sizeof(path), selectedPointSize,
                                  target > bodySize))
          contentFamily.files.push_back({path, selectedPointSize, 0});
      }
    }
    const bool loaded = manager_.loadFamilyClosest(contentFamily, renderer, bodySize);
    if (!loaded) {
      LOG_ERR("SDFS", "Failed to restore reader font: %s", familyName);
      MemoryBudget::logHeapShape("dict.font_after_restore");
      return ensureBuiltInReaderFont(renderer);
    }
    loadedFontPointSize_ = SETTINGS.getSdFontTargetPointSize();
  }

  const int fontId = manager_.getFontId(manager_.currentFamilyName());
  MemoryBudget::logHeapShape("dict.font_after_restore");
  return fontId != 0 ? fontId : ensureBuiltInReaderFont(renderer);
}

void SdCardFontSystem::markRegistryDirtyForPath(const char* path) {
  filenameFontSystem.invalidateForPath(path);
  if (!path) return;
  for (const char* root : {SdCardFontRegistry::FONTS_DIR_HIDDEN, SdCardFontRegistry::FONTS_DIR_VISIBLE}) {
    const size_t length = std::strlen(root);
    if (strncasecmp(path, root, length) == 0 && (path[length] == '/' || path[length] == '\0')) {
      markRegistryDirty();
      return;
    }
  }
}
