#include "SdCardFontManager.h"

#if CROSSINK_SCALABLE_FONTS
#include <FtFont.h>
#endif

#include <EpdFontFamily.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <SdCardFontRegistry.h>

#include <cstdlib>

SdCardFontManager::~SdCardFontManager() {
  for (auto& lf : loaded_) {
    delete lf.font;
  }
}

// FNV-1a continuation: seeds with contentHash, then hashes family name + point size.
// Produces a deterministic ID that is stable across load/unload cycles and reboots,
// and changes when font content changes (different header/TOC = different contentHash).
int SdCardFontManager::computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize) {
  static constexpr uint32_t FNV_PRIME = 16777619u;
  uint32_t hash = contentHash;
  while (*familyName) {
    hash ^= static_cast<uint8_t>(*familyName++);
    hash *= FNV_PRIME;
  }
  hash ^= pointSize;
  hash *= FNV_PRIME;
  int id = static_cast<int>(hash);
  return id != 0 ? id : 1;  // 0 is reserved as "not found" sentinel
}

int SdCardFontManager::loadFile(const SdCardFontFileInfo& file, const char* familyName, GfxRenderer& renderer) {
  return loadFilePath(file.path.c_str(), familyName, file.pointSize, renderer);
}

int SdCardFontManager::loadFilePath(const char* path, const char* familyName, uint8_t pointSize, GfxRenderer& renderer,
                                    const size_t residentLimit) {
  auto font = makeUniqueNoThrow<SdCardFont>();
  if (!font) {
    LOG_ERR("SDMGR", "Failed to allocate SdCardFont for %s", path);
    return 0;
  }

  if (!font->load(path, residentLimit)) {
    LOG_ERR("SDMGR", "Failed to load %s", path);
    return 0;
  }

  int fontId = computeFontId(font->contentHash(), familyName, pointSize);
  // Guard against collision with built-in font IDs (astronomically unlikely
  // with FNV-1a hashes, but provides a safety net)
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_ERR("SDMGR", "Font ID %d collides with existing font, skipping %s", fontId, path);
    return 0;
  }
  renderer.registerSdCardFont(fontId, font.get());
  loaded_.push_back({font.get(), fontId, pointSize});

  LOG_DBG("SDMGR", "Loaded %s size=%u id=%d styles=%u", path, pointSize, fontId, font->styleCount());

  EpdFontFamily fontFamily(font->getEpdFont(0), font->getEpdFont(1), font->getEpdFont(2), font->getEpdFont(3));
  // Dictionary-only extra sizes must not change the reader's available set
  // behind its cache identity; only body and budgeted content sizes participate.
  if (loaded_.size() == 1 || residentLimit) fontFamily = fontFamily.withSize(loaded_.front().fontId, pointSize);
  renderer.insertFont(fontId, fontFamily);
  // Transfer to the manager's existing LoadedFont owner list; unloadAll/dtor free it.
  font.release();
  return fontId;
}

bool SdCardFontManager::loadFamilyClosest(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                          uint8_t targetPointSize) {
#if CROSSINK_SCALABLE_FONTS
  if (family.isScalable()) {
    freeink::font::FtFont::RenderOptions defaults;
    defaults.hinting = freeink::font::FtFont::HintingMode::Auto;
    return loadScalable(family, renderer, targetPointSize, defaults);
  }
#endif
  if (!loadedFamilyName_.empty()) {
    unloadAll(renderer);
  }

  const SdCardFontFileInfo* selected = family.findClosestFile(targetPointSize);
  if (!selected) {
    LOG_ERR("SDMGR", "Family %s has no files near %u pt", family.name.c_str(), targetPointSize);
    return false;
  }

  // Body plus two optional sizes; allocate this tiny owner list once at load.
  loaded_.reserve(3);
  if (loadFile(*selected, family.name.c_str(), renderer) == 0) {
    return false;
  }

  loadedFamilyName_ = family.name;
  loadedPointSize_ = selected->pointSize;
  loadContentSizes(family, renderer);
  return true;
}

std::array<uint8_t, 2> SdCardFontManager::contentSizeTargets(const uint8_t bodySize,
                                                             const std::array<uint16_t, 2> scales) {
  std::array<uint8_t, 2> sizes{};
  for (size_t i = 0; i < sizes.size(); ++i)
    sizes[i] = static_cast<uint8_t>(std::clamp<unsigned>((unsigned(bodySize) * scales[i] + 128) / 256, 8, 44));
  return sizes;
}

void SdCardFontManager::loadContentSizes(const SdCardFontFamilyInfo& family, GfxRenderer& renderer) {
  if (loadedPointSize_ < 8 || loadedPointSize_ > 44) return;
  // At most 24KB for two complete optional font objects, tables and reusable
  // glyph scratch. Body font ownership/caches keep their existing policy.
  constexpr size_t SIZE_BUDGET = 12U * 1024U;
  constexpr size_t FREE_RESERVE = 96U * 1024U;
  constexpr size_t BLOCK_RESERVE = 48U * 1024U;
  for (const uint8_t target : contentSizeTargetsForBody(loadedPointSize_)) {
    const auto* file = family.findClosestFile(target);
    // Match the renderer's growth tie-break. Body selection still prefers the
    // smaller file, but a 13pt heading with only 12/14pt files should use 14pt.
    if (target > loadedPointSize_) {
      file = nullptr;
      for (const auto& candidate : family.files) {
        if (candidate.style != 0 || candidate.pointSize < 8 || candidate.pointSize > 44) continue;
        const int distance = std::abs(int(candidate.pointSize) - int(target));
        const int bestDistance = file ? std::abs(int(file->pointSize) - int(target)) : 256;
        if (distance < bestDistance || (distance == bestDistance && candidate.pointSize > file->pointSize))
          file = &candidate;
      }
    }
    if (!file || file->pointSize < 8 || file->pointSize > 44) continue;
    bool present = false;
    for (const auto& loaded : loaded_) present |= loaded.size == file->pointSize;
    if (present) continue;
    if (ESP.getFreeHeap() < FREE_RESERVE + SIZE_BUDGET || ESP.getMaxAllocHeap() < BLOCK_RESERVE + SIZE_BUDGET) {
      LOG_DBG("SDMGR", "Skipping optional size %u: preserving heap reserve", file->pointSize);
      continue;
    }
    loadFilePath(file->path.c_str(), family.name.c_str(), file->pointSize, renderer, SIZE_BUDGET);
  }
  if (loaded_.size() < 2) return;

  // Cache identity includes the sizes actually loaded and their content. A
  // reboot under memory pressure must rebuild pages instead of drawing cached
  // large text with body metrics. Changed extra-size files invalidate too.
  uint32_t hash = static_cast<uint32_t>(loaded_.front().fontId);
  for (size_t i = 1; i < loaded_.size(); ++i) hash = (hash * 16777619U) ^ uint32_t(loaded_[i].fontId);
  const int oldId = loaded_.front().fontId;
  const int newId = computeFontId(hash, family.name.c_str(), loadedPointSize_);
  if (newId != oldId && renderer.getFontMap().count(newId)) {
    LOG_ERR("SDMGR", "Content font identity collision; keeping body size");
    while (loaded_.size() > 1) {
      auto& extra = loaded_.back();
      renderer.removeFont(extra.fontId);
      delete extra.font;
      loaded_.pop_back();
    }
    return;
  }
  renderer.removeFont(oldId);
  loaded_.front().fontId = newId;
  for (const auto& loaded : loaded_) {
    auto* font = loaded.font;
    // insertFont rejects duplicate IDs; replace the old family registration
    // before publishing the final body identity shared by all content sizes.
    renderer.removeFont(loaded.fontId);
    renderer.registerSdCardFont(loaded.fontId, font);
    renderer.insertFont(
        loaded.fontId, EpdFontFamily(font->getEpdFont(0), font->getEpdFont(1), font->getEpdFont(2), font->getEpdFont(3))
                           .withSize(newId, loaded.size));
  }
}

#if CROSSINK_SCALABLE_FONTS
bool SdCardFontManager::loadFamilyClosest(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                          const uint8_t targetPointSize,
                                          const freeink::font::FtFont::RenderOptions& renderOptions) {
  if (family.isScalable()) return loadScalable(family, renderer, targetPointSize, renderOptions);
  return loadFamilyClosest(family, renderer, targetPointSize);
}
#endif

bool SdCardFontManager::loadFamilyFile(const char* path, const char* familyName, uint8_t pointSize,
                                       GfxRenderer& renderer) {
  if (!loadedFamilyName_.empty()) {
    unloadAll(renderer);
  }
  if (loadFilePath(path, familyName, pointSize, renderer) == 0) {
    return false;
  }
  loadedFamilyName_ = familyName;
  loadedPointSize_ = pointSize;
  return true;
}

int SdCardFontManager::loadFamilyExtraSize(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                           uint8_t pointSize) {
#if CROSSINK_SCALABLE_FONTS
  if (family.isScalable()) return registerScalableSize(renderer, pointSize);
#endif
  const SdCardFontFileInfo* file = family.findFile(pointSize);
  if (!file) return 0;  // family has no .cpfont at this exact size

  // Reuse an already-loaded font of the same size (e.g. when a reader size
  // happens to match a UI size) instead of double-loading the file.
  for (const auto& lf : loaded_) {
    if (lf.size == pointSize) return lf.fontId;
  }

  return loadFile(*file, family.name.c_str(), renderer);
}

int SdCardFontManager::loadFamilyExtraFile(const char* path, const char* familyName, uint8_t pointSize,
                                           GfxRenderer& renderer) {
  for (const auto& lf : loaded_) {
    if (lf.size == pointSize) return lf.fontId;
  }
  return loadFilePath(path, familyName, pointSize, renderer);
}

void SdCardFontManager::unloadAll(GfxRenderer& renderer) {
  renderer.clearSdCardFonts();
  for (auto& lf : loaded_) {
    renderer.removeFont(lf.fontId);
    delete lf.font;
  }
  loaded_.clear();
#if CROSSINK_SCALABLE_FONTS
  for (auto& font : scalable_) font.reset();
  activeScalableId_ = 0;
  scalableHash_ = 0;
  temporaryScalable_ = false;
#endif
  loadedFamilyName_.clear();
  loadedPointSize_ = 0;
}

int SdCardFontManager::getFontId(const std::string& familyName) const {
  if (familyName != loadedFamilyName_ || loaded_.empty()) return 0;
#if CROSSINK_SCALABLE_FONTS
  if (activeScalableId_) return activeScalableId_;
#endif
  return loaded_.front().fontId;
}

#if CROSSINK_SCALABLE_FONTS
int SdCardFontManager::registerScalableSize(GfxRenderer& renderer, uint8_t size) {
  for (const auto& f : loaded_)
    if (f.size == size) return f.fontId;
  const EpdFont* styles[4] = {};
  for (unsigned i = 0; i < 4; ++i)
    if (scalable_[i]) styles[i] = scalable_[i]->atSize(size);
  if (!styles[0]) {
    LOG_ERR("SDMGR", "TTF regular face/size unavailable");
    return 0;
  }
  const int id = computeFontId(scalableHash_, loadedFamilyName_.c_str(), size);
  if (renderer.getFontMap().count(id)) {
    LOG_ERR("SDMGR", "TTF font ID collision");
    return 0;
  }
  loaded_.reserve(ScalableContentMaxPointSize - ScalableContentMinPointSize + 1);
  loaded_.push_back({nullptr, id, size});
  renderer.insertFont(id, EpdFontFamily(styles[0], styles[1], styles[2], styles[3]));
  return id;
}
bool SdCardFontManager::registerScalableContentSizes(GfxRenderer& renderer) {
  for (uint8_t size = ScalableContentMinPointSize; size <= ScalableContentMaxPointSize; ++size) {
    if (!registerScalableSize(renderer, size)) {
      LOG_ERR("SDMGR", "Cannot register EPUB font size %u", unsigned(size));
      unloadAll(renderer);
      return false;
    }
  }
  return true;
}
void SdCardFontManager::refreshScalableHash() {
  scalableHash_ = 0;
  for (unsigned style = 0; style < 4; ++style)
    if (scalable_[style]) scalableHash_ = (scalableHash_ * 16777619u) ^ scalable_[style]->fingerprint() ^ style;
}
bool SdCardFontManager::setScalableRenderOptions(GfxRenderer& renderer,
                                                 const freeink::font::FtFont::RenderOptions& renderOptions) {
  if (!scalable_[0] || loadedFamilyName_.empty() || loadedPointSize_ == 0) return false;

  renderer.clearSdCardFonts();
  for (auto& loaded : loaded_) renderer.removeFont(loaded.fontId);
  loaded_.clear();
  activeScalableId_ = 0;

  for (unsigned style = 0; style < 4; ++style) {
    auto& font = scalable_[style];
    if (!font) continue;
    if (!font->setRenderOptions(renderOptions)) {
      LOG_ERR("SDMGR", "Cannot apply TTF rendering options to style %u", style);
      unloadAll(renderer);
      return false;
    }
  }

  refreshScalableHash();
  activeScalableId_ = registerScalableSize(renderer, loadedPointSize_);
  return activeScalableId_ != 0 && (temporaryScalable_ || registerScalableContentSizes(renderer));
}
bool SdCardFontManager::loadDictionaryFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                             const uint8_t pointSize,
                                             const freeink::font::FtFont::RenderOptions& options) {
  return loadScalable(family, renderer, pointSize, options, true);
}

bool SdCardFontManager::loadScalable(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t size,
                                     const freeink::font::FtFont::RenderOptions& renderOptions, const bool temporary) {
  lastLoadHadIntegrityWarning_ = false;
  if (loadedFamilyName_ != family.name || !scalable_[0] || (!temporary && temporaryScalable_)) {
    unloadAll(renderer);
    size_t totalBytes = 0;
    size_t faceCount = 0;
    for (unsigned style = 0; style < 4; ++style) {
      const auto* selected = family.findFile(0, style);
      if (!selected) continue;
      const auto& file = *selected;
      size_t bytes = 0;
      if (!HalScalableFont::fileSize(file.path.c_str(), bytes)) return false;
      if (bytes > HalScalableFont::MaxFamilyBytes - totalBytes) {
        LOG_ERR("SDMGR", "TTF family exceeds %u-byte data budget: %s", unsigned(HalScalableFont::MaxFamilyBytes),
                family.name.c_str());
        return false;
      }
      totalBytes += bytes;
      ++faceCount;
    }
    if (!HalScalableFont::prepareFamily(totalBytes, faceCount)) return false;
    size_t remaining = HalScalableFont::MaxFamilyBytes;
    // Load regular first, independent of SD directory order, so the most-used
    // face gets first choice of resident PSRAM.
    for (unsigned style = 0; style < 4; ++style) {
      const auto* selected = family.findFile(0, style);
      if (!selected) continue;
      const auto& file = *selected;
      --faceCount;
      auto font = makeUniqueNoThrow<HalScalableFont>();
      const auto mode = temporary ? HalScalableFont::FileMode::Temporary : HalScalableFont::FileMode::Auto;
      if (!font || !font->openFile(file.path.c_str(), remaining, renderOptions, mode, faceCount)) {
        lastLoadHadIntegrityWarning_ = font && font->integrityMismatch() && font->lastFailureLooksLikeFontData();
        LOG_ERR("SDMGR", "Cannot load TTF face: %s", file.path.c_str());
        unloadAll(renderer);
        return false;
      }
      remaining -= font->fileBytes();
      LOG_DBG("SDMGR", "Loaded TTF face style=%u bytes=%u: %s", unsigned(file.style), unsigned(font->fileBytes()),
              file.path.c_str());
      scalable_[file.style] = std::move(font);
    }
    // Match existing missing-style fallback: a lone face remains usable.
    if (!scalable_[0])
      for (unsigned i = 1; i < 4; ++i)
        if (scalable_[i]) {
          scalable_[0] = std::move(scalable_[i]);
          break;
        }
    refreshScalableHash();
    loadedFamilyName_ = family.name;
    temporaryScalable_ = temporary;
  }
  activeScalableId_ = registerScalableSize(renderer, size);
  if (!activeScalableId_) return false;
  // Validate regular text before accepting the selection. Probing unused
  // styles initializes every auto-hinter and streams their outlines just to
  // show a regular-only preview. Other styles load glyphs when requested.
  const bool hasProbe = scalable_[0]->hasCodepoint('T');
  if (!hasProbe && scalable_[0]->integrityMismatch()) {
    lastLoadHadIntegrityWarning_ = true;
    LOG_INF("SDMGR", "TTF probe U+0054 absent from regular face with invalid checksum: %s", family.name.c_str());
  }
  if (hasProbe && !scalable_[0]->probeGlyph('T', size)) {
    lastLoadHadIntegrityWarning_ = scalable_[0]->integrityMismatch() && scalable_[0]->lastFailureLooksLikeFontData();
    LOG_ERR("SDMGR", "TTF regular probe failed U+0054: %s", family.name.c_str());
    unloadAll(renderer);
    return false;
  }
  loadedPointSize_ = size;
  return temporary || registerScalableContentSizes(renderer);
}
#endif
