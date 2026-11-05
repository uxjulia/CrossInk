#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <atomic>

#include "ReaderFontSizeStep.h"

class GfxRenderer;

struct DictionaryFontActivation {
  int fontId = 0;
  bool usingDictionaryFont = false;
};

/// Facade that owns the SD card font registry, manager, and resolver logic.
/// Hides implementation details behind a single begin() + ensureLoaded() API.
class SdCardFontSystem {
 public:
  using SettingsPersistenceCallback = void (*)(void* context);

  SdCardFontSystem() = default;
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;
  /// Register the font resolver. On scalable-font devices, defer all reader
  /// font loading until the reader or a font preview requests it.
  void begin(GfxRenderer& renderer);

  /// Ensure the correct SD font family is loaded for the current settings.
  /// Call before entering the reader or after settings change.
  /// Also re-discovers if the registry has been marked dirty (e.g. by web upload).
  void ensureLoaded(GfxRenderer& renderer);

  // Book-level bitmap size preferences, in 1/256 em. Font loading and cache
  // identities are settled before layout; dictionary return reuses these hints.
  void setContentSizeScales(std::array<uint16_t, 2> scales) {
    if (manager_.setContentSizeScales(scales) &&
        !manager_.hasResidentScalableFamily(manager_.currentFamilyName().c_str()))
      fontReloadPending_ = true;
  }

  /// Prepare the selected built-in family before using it as a reader fallback.
  int ensureBuiltInReaderFont(GfxRenderer& renderer);

  // An EPUB can own a temporary per-book settings snapshot while this system
  // repairs a missing font selection. Let that reader persist its own state.
  void setSettingsPersistenceCallback(SettingsPersistenceCallback callback, void* context) {
    settingsPersistenceCallback_ = callback;
    settingsPersistenceContext_ = context;
  }

  /// Temporarily unload the active SD font without clearing the saved setting.
  /// Call ensureLoaded() later to restore it before reader rendering.
  void releaseLoadedFont(GfxRenderer& renderer);

  /// Release all SD-font RAM that network/TLS work does not need.
  void releaseForNetwork(GfxRenderer& renderer);

  /// Ensure the font catalog is available for settings/web enumeration, including
  /// newly uploaded or deleted fonts visible in the web UI.
  void ensureRegistry();

  /// Release catalog names and paths without unloading the active reader font.
  void releaseRegistry();

  /// Resolve an SD card font ID from family name + selected point size.
  /// Returns 0 if not found. Used by CrossPointSettings::getReaderFontId().
  int resolveFontId(const char* familyName, uint8_t pointSize) const;

  /// Whether changing point size can reuse the active scalable faces. A font
  /// catalog update invalidates those faces even while they are still present
  /// in PSRAM, because a replaced file must be loaded before the next preview.
  bool canResizeResidentScalableFamilyWithoutReload(const char* familyName) const {
    return registryLoaded_ && !registryDirty_.load(std::memory_order_acquire) && !fontReloadPending_ &&
           !registry_.needsRefresh() && loadedRegistryRevision_ == registry_.revision() &&
           manager_.hasResidentScalableFamily(familyName);
  }

  /// True when the selected scalable family is already resident. Reader code
  /// uses this to keep the lightweight catalog while that family is active.
  bool hasResidentScalableFamily(const char* familyName) const {
    return manager_.hasResidentScalableFamily(familyName);
  }
  /// True when the active reader font is a TTF using black-and-white glyphs.
  bool fontUsesMonochromeRaster(const GfxRenderer& renderer, int fontId, const char* familyName) const;
  bool lastLoadHadIntegrityWarning() const { return manager_.lastLoadHadIntegrityWarning(); }

  /// True when the named installed family is backed by TTF outlines.
  bool isScalableFamily(const char* familyName);

  /// Drop and reopen the active TTF family after its rendering profile changes.
  /// Returns true when the named reader family was resident and invalidated.
  bool reloadActiveScalableFamily(GfxRenderer& renderer, const char* familyName);
  // Advances each time reloadActiveScalableFamily applies new render options, so
  // a reader below an overlay can tell its laid-out text is stale.
  uint32_t scalableRenderOptionsGeneration() const { return scalableRenderOptionsGeneration_; }

  /// Change the reader font size using the active SD family when one is selected.
  bool changeReaderFontSize(bool larger, FontSizeStepMode mode = FontSizeStepMode::Wrap);

  /// Convert a pre-point-size SD font slot into the exact installed size.
  /// Used while reading legacy per-book reader settings.
  uint8_t resolveLegacySizeStep(const char* familyName, uint8_t sizeStep);

  // Temporarily replace the reader SD font with a per-book dictionary font.
  // At most one SD font family remains resident. Missing or failed dictionary
  // fonts leave the reader font active and retain the saved selection.
  // targetPointSize of zero follows the active reader size. The selected file
  // still uses the closest installed size, so a removed font file degrades
  // safely without retaining another family or size cache in RAM.
  DictionaryFontActivation activateDictionaryFont(GfxRenderer& renderer, const char* familyName,
                                                  uint8_t targetPointSize = 0);

  // Restore the saved reader font after a dictionary lookup and return its ID.
  int restoreReaderFont(GfxRenderer& renderer);

  /// Access the registry (e.g. for settings UI to enumerate available fonts).
  const SdCardFontRegistry& registry() const { return registry_; }

  /// Non-const access to the registry (for FontInstaller).
  SdCardFontRegistry& registry() { return registry_; }

  /// Mark the registry as needing re-discovery.
  /// Thread-safe: can be called from the web server task.
  void markRegistryDirty() {
    registryDirty_.store(true, std::memory_order_release);
    SdCardFontRegistry::invalidateIndex();
  }
  void markRegistryDirtyForPath(const char* path);

  /// Ensure the registry is available and re-scan it after SD changes.
  /// Used by the web UI so uploaded/deleted fonts appear in the list
  /// without waiting for the reader activity to run ensureLoaded().
  void refreshIfDirty() { ensureRegistry(); }

 private:
  void persistSettingsChange() const;

  SdCardFontRegistry registry_;
  SdCardFontManager manager_;
  std::atomic<bool> registryDirty_{false};
  bool registryLoaded_ = false;
  uint8_t loadedFontPointSize_ = 0;
  bool fontReloadPending_ = false;
  uint32_t loadedRegistryRevision_ = 0;
  uint32_t scalableRenderOptionsGeneration_ = 0;
  SettingsPersistenceCallback settingsPersistenceCallback_ = nullptr;
  void* settingsPersistenceContext_ = nullptr;
};

// Global SD card font system instance (defined in main.cpp).
extern SdCardFontSystem sdFontSystem;
