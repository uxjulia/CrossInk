#if CROSSINK_SCALABLE_FONTS
#include <HalScalableFont.h>
#endif
#include <FontDecompressor.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <ShapingTokens.h>
#include <Utf8.h>

#include <algorithm>
#include <cstring>

#include "FontCacheManager.h"

namespace {

char* appendUtf8Codepoint(char* output, const uint32_t codepoint) {
  if (codepoint < 0x80) {
    *output++ = static_cast<char>(codepoint);
  } else if (codepoint < 0x800) {
    *output++ = static_cast<char>(0xC0 | (codepoint >> 6));
    *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
  } else if (codepoint < 0x10000) {
    *output++ = static_cast<char>(0xE0 | (codepoint >> 12));
    *output++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
  } else {
    *output++ = static_cast<char>(0xF0 | (codepoint >> 18));
    *output++ = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    *output++ = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    *output++ = static_cast<char>(0x80 | (codepoint & 0x3F));
  }
  return output;
}

}  // namespace

FontCacheManager::FontCacheManager(const std::map<int, EpdFontFamily>& fontMap,
                                   const std::map<int, SdCardFont*>& sdCardFonts)
    : fontMap_(fontMap), sdCardFonts_(sdCardFonts) {}

void FontCacheManager::setFontDecompressor(FontDecompressor* d) { fontDecompressor_ = d; }

void FontCacheManager::clearCache() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->clearCache();
  }
}

void FontCacheManager::releaseSdFontCaches() {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  for (auto& [id, font] : sdCardFonts_) {
    font->releaseForLowMemory(false);
  }
}

void FontCacheManager::releaseInactiveSdFontCaches(const int activeFontId) {
  if (fontDecompressor_) fontDecompressor_->clearCache();
  const auto active = sdCardFonts_.find(activeFontId);
  const SdCardFont* activeFont = active == sdCardFonts_.end() ? nullptr : active->second;
  for (auto& [id, font] : sdCardFonts_) {
    if (font && font != activeFont) font->releaseForLowMemory(false);
  }
}

bool FontCacheManager::prewarmCache(int fontId, const char* utf8Text, uint8_t styleMask,
                                    const PreparationPolicy policy) {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess access;
#endif
  // SD card font prewarm path: prewarm all requested styles in one call
  auto it = sdCardFonts_.find(fontId);
  if (it != sdCardFonts_.end()) {
    int missed =
        it->second->prewarm(utf8Text, styleMask, /*metadataOnly=*/false, policy != PreparationPolicy::DictionaryLean);
    if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache(SD): %d glyph(s) not found (styleMask=0x%02X)", missed, styleMask);
    }
    return !it->second->lastPrewarmFailed();
  }

  // Standard compressed font prewarm path: loop over all requested styles
  if (!fontDecompressor_ || fontMap_.count(fontId) == 0) return false;

  // The scan admits dominant groups first; only four compressed page slots exist.
  for (int8_t i = 3; i >= 0; i--) {
    if (!(styleMask & (1 << i))) continue;
    auto style = static_cast<EpdFontFamily::Style>(i);
    const EpdFontData* data = fontMap_.at(fontId).getData(style);
#if CROSSINK_SCALABLE_FONTS
    if (data && data->bitmapHandler && utf8Text) {
      const auto& family = fontMap_.at(fontId);
      const char* text = utf8Text;
      while (*text) {
        uint32_t cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text));
        if (!cp) break;
        cp = family.applyLigatures(cp, text, style);
        const auto gd = family.getGlyphData(cp, style);
        if (!gd.glyph) {
          if (family.hasCodepoint(cp, style)) {
            LOG_ERR("FCM", "TTF metrics preparation failed: U+%04X font=%d style=%d", unsigned(cp), fontId, i);
            return false;
          }
          continue;
        }
        if (gd.glyph->width && gd.glyph->height && gd.fontData->bitmapHandler &&
            !gd.fontData->bitmapHandler(gd.fontData->glyphMissCtx, gd.glyph)) {
          LOG_ERR("FCM", "TTF glyph preparation failed: U+%04X", unsigned(cp));
          return false;
        }
      }
      continue;
    }
#endif
    if (!data || !data->groups) continue;
    int missed = fontDecompressor_->prewarmCache(data, utf8Text);
    if (missed < 0) {
      LOG_DBG("FCM", "Compressed prewarm declined (font=%d style=%d); using hot-group fallback", fontId, i);
    } else if (missed > 0) {
      LOG_DBG("FCM", "prewarmCache: %d glyph(s) not cached for style %d", missed, i);
    }
  }
  return true;
}

void FontCacheManager::logStats(const char* label) {
  if (fontDecompressor_) fontDecompressor_->logStats(label);
  for (auto& [id, font] : sdCardFonts_) {
    font->logStats(label);
  }
}

void FontCacheManager::resetStats() {
  if (fontDecompressor_) fontDecompressor_->resetStats();
  for (auto& [id, font] : sdCardFonts_) {
    font->resetStats();
  }
}

bool FontCacheManager::isScanning() const { return scanMode_ == ScanMode::Scanning; }

uint8_t FontCacheManager::resolveScanStyle(int fontId, EpdFontFamily::Style style) const {
  const uint8_t baseStyle = static_cast<uint8_t>(style) & 0x03;

  const auto sdFont = sdCardFonts_.find(fontId);
  if (sdFont != sdCardFonts_.end()) return sdFont->second->resolveStyle(baseStyle);

  const auto font = fontMap_.find(fontId);
  if (font == fontMap_.end()) return baseStyle;

  const EpdFontData* resolvedData = font->second.getData(static_cast<EpdFontFamily::Style>(baseStyle));
  for (uint8_t candidate = 0; candidate < 4; candidate++) {
    if (font->second.getData(static_cast<EpdFontFamily::Style>(candidate)) == resolvedData) return candidate;
  }
  return baseStyle;
}

void FontCacheManager::recordText(const char* text, int fontId, EpdFontFamily::Style style) {
  if (!text || *text == '\0') return;

  uint8_t fontSlot = scanFontCount_;
  for (uint8_t i = 0; i < scanFontCount_; i++) {
    if (scanFontIds_[i] == fontId) {
      fontSlot = i;
      break;
    }
  }
  if (fontSlot == scanFontCount_) {
    if (scanFontCount_ >= MAX_SCAN_FONTS) return;
    scanFontIds_[scanFontCount_++] = fontId;
  }

  const uint8_t resolvedStyle = resolveScanStyle(fontId, style);
  const uint8_t group = fontSlot * 4 + resolvedStyle;
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text);
  while (*cursor) {
    uint32_t codepoint = utf8NextCodepoint(&cursor);
    if (codepoint == 0) break;
    if (shaping::isPositionToken(codepoint) || utf8IsVariationSelector(codepoint)) continue;
    if ((style & EpdFontFamily::SMALL_CAPS) != 0 && codepoint >= 'a' && codepoint <= 'z') {
      codepoint -= 'a' - 'A';
    }

    if (scanGroupUses_[group] != UINT16_MAX) ++scanGroupUses_[group];

    const uint32_t packed = (static_cast<uint32_t>(fontSlot) << SCAN_FONT_SHIFT) |
                            (static_cast<uint32_t>(resolvedStyle) << SCAN_STYLE_SHIFT) | codepoint;
    bool found = false;
    for (uint16_t i = 0; i < scanCodepointCount_; i++) {
      if (scanCodepoints_[i] == packed) {
        found = true;
        break;
      }
    }
    if (found) continue;

    if (scanCodepointCount_ >= MAX_SCAN_CODEPOINTS) {
      if (!scanOverflowWarned_) {
        LOG_DBG("FCM", "Scan codepoint cap (%u) reached; excess glyphs will load on demand",
                static_cast<unsigned>(MAX_SCAN_CODEPOINTS));
        scanOverflowWarned_ = true;
      }
      continue;
    }

    scanCodepoints_[scanCodepointCount_++] = packed;
    scanGroupCounts_[group]++;
  }
}

// --- PrewarmScope implementation ---

FontCacheManager::PrewarmScope::PrewarmScope(FontCacheManager& manager, const PreparationPolicy policy)
    : manager_(&manager), policy_(policy) {
  manager_->scanMode_ = ScanMode::Scanning;
  if (policy_ != PreparationPolicy::IdleScalable) {
    manager_->clearCache();
    manager_->resetStats();
  }
  manager_->scanCodepointCount_ = 0;
  manager_->scanFontCount_ = 0;
  manager_->scanOverflowWarned_ = false;
  memset(manager_->scanGroupCounts_, 0, sizeof(manager_->scanGroupCounts_));
  memset(manager_->scanGroupUses_, 0, sizeof(manager_->scanGroupUses_));
}

bool FontCacheManager::PrewarmScope::endScanAndPrewarm() {
  manager_->scanMode_ = ScanMode::None;
  if (manager_->scanCodepointCount_ == 0) return true;

  // Only reorder when compressed groups compete for the four page slots.
  // Leave SD/scalable preparation at its original positions, including on mixed
  // pages: their allocation order must not change as a side effect of this fix.
  uint8_t groupOrder[SCAN_GROUP_COUNT], compressed[SCAN_GROUP_COUNT];
  uint8_t compressedCount = 0;
  uint16_t compressedMask = 0;
  for (uint8_t group = 0; group < SCAN_GROUP_COUNT; ++group) {
    groupOrder[group] = group;
    if (!manager_->fontDecompressor_ || !manager_->scanGroupCounts_[group]) continue;
    const int fontId = manager_->scanFontIds_[group / 4];
    if (manager_->sdCardFonts_.count(fontId)) continue;
    const auto font = manager_->fontMap_.find(fontId);
    if (font == manager_->fontMap_.end()) continue;
    const auto* data = font->second.getData(static_cast<EpdFontFamily::Style>(group & 3));
    if (!data || !data->groups) continue;
#if CROSSINK_SCALABLE_FONTS
    if (data->bitmapHandler) continue;
#endif
    compressed[compressedCount++] = group;
    compressedMask |= uint16_t(1) << group;
  }
  if (compressedCount > FontDecompressor::MAX_PAGE_SLOTS) {
    // Low demand first in storage; consume backwards. Equal demand favors the
    // first scanned font (normally body), then regular style. No glyph buffer.
    std::sort(compressed, compressed + compressedCount, [&](uint8_t a, uint8_t b) {
      if (manager_->scanGroupUses_[a] != manager_->scanGroupUses_[b])
        return manager_->scanGroupUses_[a] < manager_->scanGroupUses_[b];
      return a > b;
    });
    uint8_t next = 0, rank[SCAN_GROUP_COUNT];
    for (uint8_t group = 0; group < SCAN_GROUP_COUNT; ++group) {
      if (compressedMask & (uint16_t(1) << group)) groupOrder[group] = compressed[next++];
      rank[groupOrder[group]] = group;
    }
    std::sort(manager_->scanCodepoints_, manager_->scanCodepoints_ + manager_->scanCodepointCount_,
              [&](uint32_t a, uint32_t b) {
                const uint8_t ga = a >> SCAN_STYLE_SHIFT, gb = b >> SCAN_STYLE_SHIFT;
                return ga == gb ? a < b : rank[ga] < rank[gb];
              });
  } else {
    std::sort(manager_->scanCodepoints_, manager_->scanCodepoints_ + manager_->scanCodepointCount_);
  }

  uint16_t groupStarts[SCAN_GROUP_COUNT] = {};
  uint16_t offset = 0;
  for (uint8_t group : groupOrder) {
    groupStarts[group] = offset;
    offset += manager_->scanGroupCounts_[group];
  }

  // Each packed entry provides four bytes, enough for one UTF-8 codepoint.
  // Encoding backwards only overwrites an already consumed group's first byte.
  bool ok = true;
  for (int position = SCAN_GROUP_COUNT - 1; position >= 0; --position) {
    const uint8_t group = groupOrder[position];
    const uint16_t groupCount = manager_->scanGroupCounts_[group];
    if (groupCount == 0) continue;

    const uint16_t groupStart = groupStarts[group];
    char* const utf8Text = reinterpret_cast<char*>(manager_->scanCodepoints_ + groupStart);
    char* output = utf8Text;
    for (uint16_t i = 0; i < groupCount; i++) {
      const uint32_t codepoint = manager_->scanCodepoints_[groupStart + i] & SCAN_CODEPOINT_MASK;
      output = appendUtf8Codepoint(output, codepoint);
    }
    *output = '\0';

    const uint8_t fontSlot = static_cast<uint8_t>(group) / 4;
    const uint8_t style = static_cast<uint8_t>(group) & 0x03;
    if (!manager_->prewarmCache(manager_->scanFontIds_[fontSlot], utf8Text, 1 << style, policy_)) {
      ok = false;
    }
  }

  manager_->scanCodepointCount_ = 0;
  manager_->scanFontCount_ = 0;
  memset(manager_->scanGroupCounts_, 0, sizeof(manager_->scanGroupCounts_));
  memset(manager_->scanGroupUses_, 0, sizeof(manager_->scanGroupUses_));
  return ok;
}

FontCacheManager::PrewarmScope::~PrewarmScope() {
  if (active_) {
    if (policy_ == PreparationPolicy::IdleScalable) {
      // Abandoned optional scans must never turn into synchronous cold raster work.
      manager_->scanMode_ = ScanMode::None;
      manager_->scanCodepointCount_ = manager_->scanFontCount_ = 0;
    } else {
      endScanAndPrewarm();  // no-op if already called
      manager_->clearCache();
    }
  }
}

FontCacheManager::PrewarmScope::PrewarmScope(PrewarmScope&& other) noexcept
    : manager_(other.manager_), policy_(other.policy_), active_(other.active_) {
  other.active_ = false;
}

FontCacheManager::PrewarmScope FontCacheManager::createPrewarmScope(const PreparationPolicy policy) {
  return PrewarmScope(*this, policy);
}

#if CROSSINK_SCALABLE_FONTS
bool FontCacheManager::isScalableFont(const int fontId) const {
  const auto it = fontMap_.find(fontId);
  if (it == fontMap_.end() || sdCardFonts_.count(fontId)) return false;
  const auto* data = it->second.getData(EpdFontFamily::REGULAR);
  return data && data->bitmapHandler;
}

void FontCacheManager::PrewarmScope::endScanForIdle(IdleScalableJob& job) {
  manager_->scanMode_ = ScanMode::None;
  job.reset();
  job.count = manager_->scanCodepointCount_;
  std::copy_n(manager_->scanCodepoints_, job.count, job.codepoints);
  std::copy_n(manager_->scanFontIds_, manager_->scanFontCount_, job.fontIds);
  manager_->scanCodepointCount_ = manager_->scanFontCount_ = 0;
}

FontCacheManager::IdleResult FontCacheManager::prewarmIdleSlice(IdleScalableJob& job, bool (*shouldYield)(void*),
                                                                void* context, const uint16_t maxGlyphs,
                                                                bool (*cancelled)(void*)) {
  HalScalableFont::IdlePrewarmScope admission;
  uint16_t attempted = 0;
  while (job.cursor < job.count) {
    if (attempted >= maxGlyphs || (shouldYield && shouldYield(context))) return IdleResult::Yielded;
    const uint32_t packed = job.codepoints[job.cursor];
    const auto font = fontMap_.find(job.fontIds[packed >> SCAN_FONT_SHIFT]);
    const auto style = static_cast<EpdFontFamily::Style>((packed >> SCAN_STYLE_SHIFT) & 3);
    const uint32_t cp = packed & SCAN_CODEPOINT_MASK;
    if (font != fontMap_.end()) {
      const auto* data = font->second.getData(style);
      // Leave cpfont and built-in fallback caches alone during speculative work.
      if (data && data->bitmapHandler) {
        const auto gd = font->second.getGlyphData(cp, style);
        // Time budgets apply between whole glyphs so a slow metrics lookup
        // cannot starve the same glyph forever. Input can still cancel here.
        if (cancelled && cancelled(context)) return IdleResult::Yielded;
        if (!gd.glyph) {
          if (font->second.hasCodepoint(cp, style)) return IdleResult::Failed;
        } else if (gd.glyph->width && gd.glyph->height && gd.fontData->bitmapHandler &&
                   HalScalableFont::prepareIdleBitmap(gd.fontData, gd.glyph) ==
                       HalScalableFont::BitmapPreparation::Failed) {
          return IdleResult::Failed;
        }
      }
    }
    ++job.cursor;
    ++attempted;
  }
  return IdleResult::Complete;
}
#endif
