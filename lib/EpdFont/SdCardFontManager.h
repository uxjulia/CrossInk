#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#if CROSSINK_SCALABLE_FONTS
#include <HalScalableFont.h>
#endif

class GfxRenderer;
class SdCardFont;
struct SdCardFontFamilyInfo;
struct SdCardFontFileInfo;

class SdCardFontManager {
 public:
  SdCardFontManager() = default;
  ~SdCardFontManager();
  SdCardFontManager(const SdCardFontManager&) = delete;
  SdCardFontManager& operator=(const SdCardFontManager&) = delete;

  // Load the closest body file, plus up to two budgeted content sizes for bitmap fonts.
  bool loadFamilyClosest(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t targetPointSize);
  // Shared by registry loads and bounded dictionary-return discovery.
  static std::array<uint8_t, 2> contentSizeTargets(uint8_t bodySize, std::array<uint16_t, 2> scales = {512, 192});
  bool setContentSizeScales(std::array<uint16_t, 2> scales) {
    if (contentSizeScales_ == scales) return false;
    contentSizeScales_ = scales;
    return true;
  }
  std::array<uint8_t, 2> contentSizeTargetsForBody(uint8_t bodySize) const {
    return contentSizeTargets(bodySize, contentSizeScales_);
  }
#if CROSSINK_SCALABLE_FONTS
  bool loadFamilyClosest(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t targetPointSize,
                         const freeink::font::FtFont::RenderOptions& renderOptions);
#endif

#if CROSSINK_SCALABLE_FONTS
  // Reuse an active reader face, or load a temporary dictionary family
  // without computing identities for persistent EPUB layouts.
  bool loadDictionaryFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t pointSize,
                            const freeink::font::FtFont::RenderOptions& options);
#endif
  bool hasTemporaryScalableFamily() const {
#if CROSSINK_SCALABLE_FONTS
    return temporaryScalable_;
#else
    return false;
#endif
  }

  // Load a known file path without constructing a registry family. Used by
  // dictionary lookup to avoid allocating a whole catalog for one family.
  bool loadFamilyFile(const char* path, const char* familyName, uint8_t pointSize, GfxRenderer& renderer);

  // Additively load the .cpfont of `family` at the exact physical `pointSize`
  // for temporary text surfaces such as dictionary lookup.
  // Does not unload anything. If a font of that size is already loaded its id
  // is reused. Returns the font id, or 0 if the family has no file at that size
  // or loading failed.
  int loadFamilyExtraSize(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t pointSize);

  // Add an exact extra-size file found by the fixed-buffer dictionary path.
  int loadFamilyExtraFile(const char* path, const char* familyName, uint8_t pointSize, GfxRenderer& renderer);

  // Unload everything, unregister from renderer.
  void unloadAll(GfxRenderer& renderer);

  // Look up the font ID for the loaded family. Returns 0 if nothing loaded
  // or familyName doesn't match.
  int getFontId(const std::string& familyName) const;

  // Get name of currently loaded family (empty if none).
  const std::string& currentFamilyName() const { return loadedFamilyName_; };

  // Point size that was actually loaded (closest match to targetPtSize).
  // 0 if nothing loaded.
  uint8_t currentPointSize() const { return loadedPointSize_; };

  // True when the most recent scalable-font load found both an invalid
  // whole-file OpenType checksum and unusable basic-text probe data.
  bool lastLoadHadIntegrityWarning() const {
#if CROSSINK_SCALABLE_FONTS
    return lastLoadHadIntegrityWarning_;
#else
    return false;
#endif
  }

  // Scalable faces are shared by every point size in the active family. This
  // distinguishes a catalog entry from a family whose TTF bytes are actually
  // resident and can therefore change size without another SD read.
  bool hasResidentScalableFamily(const char* familyName) const {
#if CROSSINK_SCALABLE_FONTS
    return familyName && loadedFamilyName_ == familyName && scalable_[0] != nullptr;
#else
    (void)familyName;
    return false;
#endif
  }

#if CROSSINK_SCALABLE_FONTS
  // Update the resident faces without rereading their TTF files. Existing
  // renderer registrations are replaced because metrics and cache identity
  // depend on these options.
  bool setScalableRenderOptions(GfxRenderer& renderer, const freeink::font::FtFont::RenderOptions& renderOptions);
#endif

 private:
#if CROSSINK_SCALABLE_FONTS
  std::unique_ptr<HalScalableFont> scalable_[4];
  uint32_t scalableHash_ = 0;
  int activeScalableId_ = 0;
  bool lastLoadHadIntegrityWarning_ = false;
  bool temporaryScalable_ = false;
  bool loadScalable(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t size,
                    const freeink::font::FtFont::RenderOptions& renderOptions, bool temporary = false);
  void refreshScalableHash();
  int registerScalableSize(GfxRenderer& renderer, uint8_t size);
  bool registerScalableContentSizes(GfxRenderer& renderer);
#endif
  void loadContentSizes(const SdCardFontFamilyInfo& family, GfxRenderer& renderer);
  struct LoadedFont {
    SdCardFont* font;  // heap-allocated, owned
    int fontId;
    uint8_t size;
  };
  static int computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize);

  // Load+register a single .cpfont file and append it to loaded_.
  // Returns the font id, or 0 on failure (allocation, read, or id collision).
  int loadFile(const SdCardFontFileInfo& file, const char* familyName, GfxRenderer& renderer);
  int loadFilePath(const char* path, const char* familyName, uint8_t pointSize, GfxRenderer& renderer,
                   size_t residentLimit = 0);

  std::array<uint16_t, 2> contentSizeScales_{512, 192};
  std::string loadedFamilyName_;
  uint8_t loadedPointSize_ = 0;
  std::vector<LoadedFont> loaded_;
};
