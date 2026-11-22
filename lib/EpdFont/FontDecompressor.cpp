#include "FontDecompressor.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace {

enum class InsertResult : uint8_t { Added, AlreadyPresent, Full };

template <typename T, typename Count>
InsertResult insertSorted(T* values, Count& count, const Count capacity, const T value) {
  T* const end = values + count;
  T* const position = std::lower_bound(values, end, value);
  if (position != end && *position == value) return InsertResult::AlreadyPresent;
  if (count >= capacity) return InsertResult::Full;
  memmove(position + 1, position, static_cast<size_t>(end - position) * sizeof(*position));
  *position = value;
  ++count;
  return InsertResult::Added;
}

}  // namespace

FontDecompressor::~FontDecompressor() { deinit(); }

bool FontDecompressor::init() {
  clearCache();
  return true;
}

void FontDecompressor::deinit() {
  freePageBuffer();
  freeHotGroup();
}

void FontDecompressor::clearCache() {
  freePageBuffer();
  freeHotGroup();
}

void FontDecompressor::freePageBuffer() {
  for (uint8_t i = 0; i < pageSlotCount; i++) {
    free(pageSlots[i].buffer);
    delete[] pageSlots[i].glyphs;
    pageSlots[i] = {};
  }
  pageSlotCount = 0;
}

void FontDecompressor::freeHotGroup() {
  free(hotGroup);
  hotGroup = nullptr;
  hotGroupCapacity = 0;
  hotGroupFont = nullptr;
  hotGroupIndex = UINT16_MAX;
  free(hotGlyphBuf);
  hotGlyphBuf = nullptr;
  hotGlyphBufCapacity = 0;
  resetHotOffsetCache();
}

void FontDecompressor::resetHotOffsetCache() {
  for (auto& entry : hotOffsetCache_) entry.glyphIndex = UINT32_MAX;
}

bool FontDecompressor::ensureCapacity(uint8_t*& buf, uint32_t& capacity, uint32_t needed) {
  if (capacity >= needed) return true;
  // Grow-only, free-then-malloc: every caller fully rewrites the buffer after a grow, so the
  // old contents are dead -- freeing first gives the allocator its best shot on a tight heap.
  free(buf);
  buf = static_cast<uint8_t*>(malloc(needed));  // owned by FontDecompressor, freed in freeHotGroup()
  capacity = buf ? needed : 0;
  return buf != nullptr;
}

uint16_t FontDecompressor::getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex) {
  // O(1) path for frequency-grouped fonts with glyphToGroup mapping
  if (fontData->glyphToGroup != nullptr) {
    return fontData->glyphToGroup[glyphIndex];
  }

  // Contiguous-group fonts: linear scan
  for (uint16_t i = 0; i < fontData->groupCount; i++) {
    uint32_t first = fontData->groups[i].firstGlyphIndex;
    if (glyphIndex >= first && glyphIndex < first + fontData->groups[i].glyphCount) {
      return i;
    }
  }
  return fontData->groupCount;  // sentinel = not found
}

bool FontDecompressor::decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf,
                                       uint32_t outSize) {
  const EpdFontGroup& group = fontData->groups[groupIndex];

  const uint32_t tDecomp = millis();
  inflateReader.init();
  inflateReader.setSource(&fontData->bitmap[group.compressedOffset], group.compressedSize);
  if (!inflateReader.read(outBuf, outSize)) {
    stats.decompressTimeMs += millis() - tDecomp;
    LOG_ERR("FDC", "Decompression failed for group %u", groupIndex);
    return false;
  }
  stats.decompressTimeMs += millis() - tDecomp;
  return true;
}

// --- Byte-aligned helpers ---

uint32_t FontDecompressor::getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex) {
  uint32_t offset = 0;

  auto accumGlyph = [&](const EpdGlyph& g) {
    if (g.width > 0 && g.height > 0) {
      offset += ((g.width + 3) / 4) * g.height;
    }
  };

  if (fontData->glyphToGroup) {
    // Frequency-grouped: scan glyphs before glyphIndex that belong to this group
    for (uint32_t i = 0; i < glyphIndex; i++) {
      if (fontData->glyphToGroup[i] == groupIndex) {
        accumGlyph(fontData->glyph[i]);
      }
    }
  } else {
    // Contiguous-group: sum aligned sizes of preceding glyphs in the group
    const EpdFontGroup& group = fontData->groups[groupIndex];
    for (uint32_t i = group.firstGlyphIndex; i < glyphIndex; i++) {
      accumGlyph(fontData->glyph[i]);
    }
  }

  return offset;
}

void FontDecompressor::compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width,
                                          uint8_t height) {
  if (width == 0 || height == 0) return;
  const uint32_t rowStride = (width + 3) / 4;
  if (width % 4 == 0) {
    memcpy(packedDst, alignedSrc, rowStride * height);
    return;
  }
  uint8_t outByte = 0, outBits = 0;
  uint32_t writeIdx = 0;
  for (uint8_t y = 0; y < height; y++) {
    for (uint8_t x = 0; x < width; x++) {
      outByte = (outByte << 2) | ((alignedSrc[y * rowStride + x / 4] >> ((3 - (x % 4)) * 2)) & 0x3);
      outBits += 2;
      if (outBits == 8) {
        packedDst[writeIdx++] = outByte;
        outByte = 0;
        outBits = 0;
      }
    }
  }
  if (outBits > 0) packedDst[writeIdx] = outByte << (8 - outBits);
}

// --- getBitmap: page slots → hot group → decompress ---

const uint8_t* FontDecompressor::getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex) {
  const uint32_t tStart = micros();
  stats.getBitmapCalls++;

  if (!fontData->groups || fontData->groupCount == 0) {
    stats.getBitmapTimeUs += micros() - tStart;
    return &fontData->bitmap[glyph->dataOffset];
  }

  // Check page buffers first (populated by prewarmCache)
  for (uint8_t slotIndex = 0; slotIndex < pageSlotCount; slotIndex++) {
    PageSlot& slot = pageSlots[slotIndex];
    if (!slot.buffer || !slot.glyphs || slot.fontData != fontData || slot.glyphCount == 0) continue;

    int left = 0, right = slot.glyphCount - 1;
    while (left <= right) {
      int mid = left + (right - left) / 2;
      if (slot.glyphs[mid].glyphIndex == glyphIndex) {
        if (slot.glyphs[mid].bufferOffset != UINT32_MAX) {
          stats.cacheHits++;
          stats.getBitmapTimeUs += micros() - tStart;
          return &slot.buffer[slot.glyphs[mid].bufferOffset];
        }
        break;  // Not extracted during prewarm; fall through to hot-group path
      }
      if (slot.glyphs[mid].glyphIndex < glyphIndex)
        left = mid + 1;
      else
        right = mid - 1;
    }
    break;
  }

  // Fallback: hot group slot
  uint16_t groupIndex = getGroupIndex(fontData, glyphIndex);
  if (groupIndex >= fontData->groupCount) {
    LOG_ERR("FDC", "Glyph %u not found in any group", glyphIndex);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  // Check if hot group already has this group decompressed — if not, decompress it
  if (!(hotGroup != nullptr && hotGroupFont == fontData && hotGroupIndex == groupIndex)) {
    stats.cacheMisses++;
    const EpdFontGroup& group = fontData->groups[groupIndex];

    // ensureCapacity may free the buffer, so the cached-group identity dies with it either way.
    hotGroupFont = nullptr;
    hotGroupIndex = UINT16_MAX;
    resetHotOffsetCache();
    if (!ensureCapacity(hotGroup, hotGroupCapacity, group.uncompressedSize)) {
      LOG_ERR("FDC", "Failed to allocate %u bytes for hot group %u", group.uncompressedSize, groupIndex);
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    if (!decompressGroup(fontData, groupIndex, hotGroup, group.uncompressedSize)) {
      stats.getBitmapTimeUs += micros() - tStart;
      return nullptr;
    }

    hotGroupFont = fontData;
    hotGroupIndex = groupIndex;
    stats.hotGroupBytes = group.uncompressedSize;
  } else {
    stats.cacheHits++;
  }

  // Compact just the requested glyph from byte-aligned data into scratch buffer
  if (!ensureCapacity(hotGlyphBuf, hotGlyphBufCapacity, glyph->dataLength)) {
    LOG_ERR("FDC", "Failed to allocate %u bytes for glyph scratch", (unsigned)glyph->dataLength);
    stats.getBitmapTimeUs += micros() - tStart;
    return nullptr;
  }

  const uint32_t cacheHash = glyphIndex * 2654435761U;
  HotOffsetEntry& offsetEntry = hotOffsetCache_[cacheHash >> (32 - 4)];
  uint32_t alignedOff;
  if (offsetEntry.glyphIndex == glyphIndex) {
    alignedOff = offsetEntry.alignedOffset;
    ++stats.hotOffsetCacheHits;
  } else {
    alignedOff = getAlignedOffset(fontData, groupIndex, glyphIndex);
    offsetEntry = {glyphIndex, alignedOff};
  }
  compactSingleGlyph(&hotGroup[alignedOff], hotGlyphBuf, glyph->width, glyph->height);
  stats.getBitmapTimeUs += micros() - tStart;
  return hotGlyphBuf;
}

// --- Prewarm: pre-decompress glyph bitmaps for a page of text ---

int32_t FontDecompressor::findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint) {
  const EpdUnicodeInterval* intervals = fontData->intervals;
  const int count = fontData->intervalCount;

  if (count == 0) return -1;

  // Binary search
  int left = 0;
  int right = count - 1;

  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const EpdUnicodeInterval* interval = &intervals[mid];

    if (codepoint < interval->first) {
      right = mid - 1;
    } else if (codepoint > interval->last) {
      left = mid + 1;
    } else {
      return static_cast<int32_t>(interval->offset + (codepoint - interval->first));
    }
  }

  return -1;
}

int FontDecompressor::prewarmCache(const EpdFontData* fontData, const char* utf8Text) {
  if (!fontData || !fontData->groups || !utf8Text) return 0;
  if (pageSlotCount >= MAX_PAGE_SLOTS) {
    ++stats.prewarmSlotRefusals;
    LOG_DBG("FDC", "Prewarm slot cap (%u) reached; using hot-group fallback", MAX_PAGE_SLOTS);
    return -1;
  }

  // Step 1: Collect unique glyph indices needed for this page
  uint32_t neededGlyphs[MAX_PAGE_GLYPHS];
  uint16_t glyphCount = 0;
  bool glyphCapWarned = false;

  const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8Text);
  while (*p) {
    uint32_t cp = utf8NextCodepoint(&p);
    if (cp == 0) break;
    if (utf8IsVariationSelector(cp)) continue;

    int32_t glyphIdx = findGlyphIndex(fontData, cp);
    if (glyphIdx < 0 && !syntheticGlyph::isSpaceFallback(cp)) {
      glyphIdx = findGlyphIndex(fontData, REPLACEMENT_GLYPH);
    }
    if (glyphIdx < 0) continue;

    const auto result = insertSorted(neededGlyphs, glyphCount, MAX_PAGE_GLYPHS, static_cast<uint32_t>(glyphIdx));
    if (result == InsertResult::Full && !glyphCapWarned) {
      LOG_DBG("FDC", "Glyph cap (%u) reached during prewarm; excess glyphs will use hot-group fallback",
              MAX_PAGE_GLYPHS);
      glyphCapWarned = true;
    }
  }

  addLigatureGlyphs(fontData, neededGlyphs, glyphCount, glyphCapWarned);
  return prewarmGlyphs(fontData, neededGlyphs, glyphCount);
}

int FontDecompressor::prewarmCodepoints(const EpdFontData* fontData, uint32_t* codepoints, const uint16_t count) {
  if (!fontData || !fontData->groups || (count > 0 && !codepoints)) return 0;
  if (count == 0) return 0;
  if (count > MAX_PAGE_GLYPHS) {
    LOG_ERR("FDC", "Codepoint span exceeds prewarm cap (%u > %u)", count, MAX_PAGE_GLYPHS);
    return count;
  }
  if (pageSlotCount >= MAX_PAGE_SLOTS) {
    ++stats.prewarmSlotRefusals;
    LOG_DBG("FDC", "Prewarm slot cap (%u) reached; using hot-group fallback", MAX_PAGE_SLOTS);
    return -1;
  }

  uint16_t glyphCount = 0;
  for (uint16_t i = 0; i < count; ++i) {
    const uint32_t cp = codepoints[i];
    if (utf8IsVariationSelector(cp)) continue;
    int32_t glyphIdx = findGlyphIndex(fontData, cp);
    if (glyphIdx < 0 && !syntheticGlyph::isSpaceFallback(cp)) {
      glyphIdx = findGlyphIndex(fontData, REPLACEMENT_GLYPH);
    }
    if (glyphIdx < 0) continue;
    codepoints[glyphCount++] = static_cast<uint32_t>(glyphIdx);
  }
  std::sort(codepoints, codepoints + glyphCount);
  glyphCount = static_cast<uint16_t>(std::unique(codepoints, codepoints + glyphCount) - codepoints);
  if (glyphCount == 0) return 0;

  const uint16_t ligatureCount = countLigatureOutputs(fontData, codepoints, glyphCount);
  const uint16_t glyphCapacity = static_cast<uint16_t>(glyphCount + ligatureCount);
  auto glyphs = makeUniqueNoThrow<PageGlyphEntry[]>(glyphCapacity);
  if (!glyphs) {
    LOG_ERR("FDC", "Failed to allocate page glyph table (%u entries)", glyphCapacity);
    return glyphCount;
  }
  for (uint16_t i = 0; i < glyphCount; ++i) glyphs[i] = {codepoints[i], UINT32_MAX, 0};
  addLigatureGlyphEntries(fontData, glyphs.get(), glyphCount, glyphCapacity);
  return prewarmGlyphTable(fontData, std::move(glyphs), glyphCount);
}

void FontDecompressor::addLigatureGlyphs(const EpdFontData* fontData, uint32_t* neededGlyphs, uint16_t& glyphCount,
                                         bool& glyphCapWarned) {
  // Add ligature output glyphs: if both input codepoints of a ligature pair are
  // in the needed set, the output glyph will be queried during rendering.
  if (fontData->ligaturePairs && fontData->ligaturePairCount > 0) {
    for (uint32_t li = 0; li < fontData->ligaturePairCount && glyphCount < MAX_PAGE_GLYPHS; li++) {
      uint32_t leftCp = fontData->ligaturePairs[li].pair >> 16;
      uint32_t rightCp = fontData->ligaturePairs[li].pair & 0xFFFF;

      int32_t leftIdx = findGlyphIndex(fontData, leftCp);
      int32_t rightIdx = findGlyphIndex(fontData, rightCp);
      if (leftIdx < 0 || rightIdx < 0) continue;

      if (!std::binary_search(neededGlyphs, neededGlyphs + glyphCount, static_cast<uint32_t>(leftIdx)) ||
          !std::binary_search(neededGlyphs, neededGlyphs + glyphCount, static_cast<uint32_t>(rightIdx)))
        continue;

      int32_t outIdx = findGlyphIndex(fontData, fontData->ligaturePairs[li].ligatureCp);
      if (outIdx < 0) continue;

      const auto result = insertSorted(neededGlyphs, glyphCount, MAX_PAGE_GLYPHS, static_cast<uint32_t>(outIdx));
      if (result == InsertResult::Full && !glyphCapWarned) {
        LOG_DBG("FDC", "Glyph cap (%u) reached during prewarm; excess glyphs will use hot-group fallback",
                MAX_PAGE_GLYPHS);
        glyphCapWarned = true;
      }
    }
  }
}

uint16_t FontDecompressor::countLigatureOutputs(const EpdFontData* fontData, const uint32_t* glyphIndices,
                                                const uint16_t glyphCount) {
  if (!fontData->ligaturePairs || glyphCount >= MAX_PAGE_GLYPHS) return 0;
  if (fontData->ligaturePairCount > UINT8_MAX + 1U) {
    return static_cast<uint16_t>(std::min<uint32_t>(fontData->ligaturePairCount, MAX_PAGE_GLYPHS - glyphCount));
  }
  uint8_t activePairs[(UINT8_MAX + 1) / 8] = {};
  auto hasActiveOutput = [&](const uint32_t glyphIndex, const uint32_t pairLimit) {
    if (std::binary_search(glyphIndices, glyphIndices + glyphCount, glyphIndex)) return true;
    for (uint32_t previous = 0; previous < pairLimit; ++previous) {
      if (!(activePairs[previous / 8] & (uint8_t(1) << (previous % 8)))) continue;
      if (findGlyphIndex(fontData, fontData->ligaturePairs[previous].ligatureCp) == static_cast<int32_t>(glyphIndex))
        return true;
    }
    return false;
  };

  uint16_t extra = 0;
  for (uint32_t li = 0; li < fontData->ligaturePairCount && glyphCount + extra < MAX_PAGE_GLYPHS; ++li) {
    const uint32_t leftCp = fontData->ligaturePairs[li].pair >> 16;
    const uint32_t rightCp = fontData->ligaturePairs[li].pair & 0xFFFF;
    const int32_t leftIdx = findGlyphIndex(fontData, leftCp);
    const int32_t rightIdx = findGlyphIndex(fontData, rightCp);
    if (leftIdx < 0 || rightIdx < 0 || !hasActiveOutput(static_cast<uint32_t>(leftIdx), li) ||
        !hasActiveOutput(static_cast<uint32_t>(rightIdx), li))
      continue;

    const int32_t outIdx = findGlyphIndex(fontData, fontData->ligaturePairs[li].ligatureCp);
    if (outIdx < 0) continue;
    const bool alreadyAvailable = hasActiveOutput(static_cast<uint32_t>(outIdx), li);
    activePairs[li / 8] |= uint8_t(1) << (li % 8);
    if (!alreadyAvailable) ++extra;
  }
  return extra;
}

void FontDecompressor::addLigatureGlyphEntries(const EpdFontData* fontData, PageGlyphEntry* glyphs,
                                               uint16_t& glyphCount, const uint16_t glyphCapacity) {
  if (!fontData->ligaturePairs) return;
  for (uint32_t li = 0; li < fontData->ligaturePairCount && glyphCount < glyphCapacity; ++li) {
    const uint32_t leftCp = fontData->ligaturePairs[li].pair >> 16;
    const uint32_t rightCp = fontData->ligaturePairs[li].pair & 0xFFFF;
    const int32_t leftIdx = findGlyphIndex(fontData, leftCp);
    const int32_t rightIdx = findGlyphIndex(fontData, rightCp);
    if (leftIdx < 0 || rightIdx < 0) continue;

    auto hasGlyph = [&](const uint32_t index) {
      PageGlyphEntry* const end = glyphs + glyphCount;
      PageGlyphEntry* const entry = std::lower_bound(
          glyphs, end, index,
          [](const PageGlyphEntry& candidate, const uint32_t value) { return candidate.glyphIndex < value; });
      return entry != end && entry->glyphIndex == index;
    };
    if (!hasGlyph(static_cast<uint32_t>(leftIdx)) || !hasGlyph(static_cast<uint32_t>(rightIdx))) continue;

    const int32_t outIdx = findGlyphIndex(fontData, fontData->ligaturePairs[li].ligatureCp);
    if (outIdx < 0 || hasGlyph(static_cast<uint32_t>(outIdx))) continue;
    PageGlyphEntry* const end = glyphs + glyphCount;
    PageGlyphEntry* const position = std::lower_bound(
        glyphs, end, static_cast<uint32_t>(outIdx),
        [](const PageGlyphEntry& candidate, const uint32_t value) { return candidate.glyphIndex < value; });
    memmove(position + 1, position, static_cast<size_t>(end - position) * sizeof(*position));
    *position = {static_cast<uint32_t>(outIdx), UINT32_MAX, 0};
    ++glyphCount;
  }
}

int FontDecompressor::prewarmGlyphs(const EpdFontData* fontData, uint32_t* neededGlyphs, const uint16_t glyphCount) {
  if (glyphCount == 0) return 0;

  auto glyphs = makeUniqueNoThrow<PageGlyphEntry[]>(glyphCount);
  if (!glyphs) {
    LOG_ERR("FDC", "Failed to allocate page glyph table (%u entries)", glyphCount);
    return glyphCount;
  }
  for (uint16_t i = 0; i < glyphCount; ++i) glyphs[i] = {neededGlyphs[i], UINT32_MAX, 0};
  return prewarmGlyphTable(fontData, std::move(glyphs), glyphCount);
}

int FontDecompressor::prewarmGlyphTable(const EpdFontData* fontData, std::unique_ptr<PageGlyphEntry[]> glyphs,
                                        const uint16_t glyphCount) {
  if (glyphCount == 0) return 0;

  // Step 2: Compute total buffer size and collect unique groups
  uint32_t totalBytes = 0;
  uint16_t neededGroups[128];
  uint8_t groupCount = 0;
  bool groupCapWarned = false;

  for (uint16_t i = 0; i < glyphCount; i++) {
    totalBytes += fontData->glyph[glyphs[i].glyphIndex].dataLength;
    uint16_t gi = getGroupIndex(fontData, glyphs[i].glyphIndex);
    const auto result = insertSorted(neededGroups, groupCount, static_cast<uint8_t>(128), gi);
    if (result == InsertResult::Full && !groupCapWarned) {
      LOG_DBG("FDC", "Group cap (128) reached during prewarm; some groups will use hot-group fallback");
      groupCapWarned = true;
    }
  }

  stats.uniqueGroupsAccessed = groupCount;

  // Step 3: Allocate page buffer and lookup table
  PageSlot& slot = pageSlots[pageSlotCount];
  slot.buffer = static_cast<uint8_t*>(malloc(totalBytes));
  if (!slot.buffer) {
    LOG_ERR("FDC", "Failed to allocate page buffer (%u bytes, %u glyphs)", totalBytes, glyphCount);
    free(slot.buffer);
    slot = {};
    return glyphCount;
  }
  stats.pageBufferBytes += totalBytes;
  stats.pageGlyphsBytes += glyphCount * sizeof(PageGlyphEntry);

  slot.glyphs = glyphs.release();
  slot.fontData = fontData;
  slot.glyphCount = glyphCount;
  pageSlotCount++;

  // `glyphs` is already sorted by glyphIndex for binary search in getBitmap().

  // Step 3b: Pre-scan to compute each needed glyph's byte-aligned offset within its group.
  // This avoids recomputing aligned offsets per group during extraction in step 4.
  uint32_t groupAlignedTracker[128] = {};  // running byte-aligned offset for each needed group

  if (fontData->glyphToGroup) {
    // Frequency-grouped: single O(totalGlyphs) pass through glyphToGroup
    const auto& lastInterval = fontData->intervals[fontData->intervalCount - 1];
    const uint32_t totalGlyphs = lastInterval.offset + (lastInterval.last - lastInterval.first + 1);

    for (uint32_t i = 0; i < totalGlyphs; i++) {
      const uint16_t gi = fontData->glyphToGroup[i];
      uint16_t* const gpEnd = neededGroups + groupCount;
      uint16_t* const gp = std::lower_bound(neededGroups, gpEnd, gi);
      const uint8_t gpPos = gp != gpEnd && *gp == gi ? static_cast<uint8_t>(gp - neededGroups) : groupCount;
      if (gpPos == groupCount) continue;  // not a needed group

      const EpdGlyph& glyph = fontData->glyph[i];

      // Binary search in sorted slot.glyphs to find if glyph i is needed
      int left = 0, right = (int)slot.glyphCount - 1;
      while (left <= right) {
        const int mid = left + (right - left) / 2;
        if (slot.glyphs[mid].glyphIndex == i) {
          slot.glyphs[mid].alignedOffset = groupAlignedTracker[gpPos];
          break;
        }
        if (slot.glyphs[mid].glyphIndex < i)
          left = mid + 1;
        else
          right = mid - 1;
      }

      if (glyph.width > 0 && glyph.height > 0) {
        groupAlignedTracker[gpPos] += ((glyph.width + 3) / 4) * glyph.height;
      }
    }
  } else {
    // Contiguous-group: iterate each needed group's glyphs directly
    for (uint8_t g = 0; g < groupCount; g++) {
      const EpdFontGroup& group = fontData->groups[neededGroups[g]];
      uint32_t alignedOff = 0;
      for (uint16_t j = 0; j < group.glyphCount; j++) {
        const uint32_t glyphI = group.firstGlyphIndex + j;
        const EpdGlyph& glyph = fontData->glyph[glyphI];

        int left = 0, right = (int)slot.glyphCount - 1;
        while (left <= right) {
          const int mid = left + (right - left) / 2;
          if (slot.glyphs[mid].glyphIndex == glyphI) {
            slot.glyphs[mid].alignedOffset = alignedOff;
            break;
          }
          if (slot.glyphs[mid].glyphIndex < glyphI)
            left = mid + 1;
          else
            right = mid - 1;
        }

        if (glyph.width > 0 && glyph.height > 0) {
          alignedOff += ((glyph.width + 3) / 4) * glyph.height;
        }
      }
    }
  }

  // Step 4: Reuse one call-local scratch buffer, bounded by the largest requested
  // group (up to 64 KiB in generated fonts). Heap storage avoids a large stack or
  // permanent RAM reservation; release before growing so two groups never coexist.
  HeapByteBuffer tempBuf;
  uint32_t tempCapacity = 0;
  uint32_t writeOffset = 0;
  int missed = 0;

  for (uint8_t g = 0; g < groupCount; g++) {
    uint16_t groupIdx = neededGroups[g];
    const EpdFontGroup& group = fontData->groups[groupIdx];

    if (!tempBuf || tempCapacity < group.uncompressedSize) {
      tempBuf.reset();
      tempBuf = makeDefaultByteBufferNoThrow(group.uncompressedSize);
      tempCapacity = tempBuf ? group.uncompressedSize : 0;
    }
    if (!tempBuf) {
      LOG_ERR("FDC", "Failed to allocate temp buffer (%u bytes) for group %u", group.uncompressedSize, groupIdx);
      missed++;
      continue;
    }
    if (group.uncompressedSize > stats.peakTempBytes) {
      stats.peakTempBytes = group.uncompressedSize;
    }

    if (!decompressGroup(fontData, groupIdx, tempBuf.get(), group.uncompressedSize)) {
      missed++;
      continue;
    }

    // Extract needed glyphs directly from the byte-aligned temp buffer, compacting on the fly.
    // alignedOffset was pre-computed in step 3b — no full-group compact scan needed.
    for (uint16_t i = 0; i < slot.glyphCount; i++) {
      if (slot.glyphs[i].bufferOffset != UINT32_MAX) continue;  // already extracted
      if (getGroupIndex(fontData, slot.glyphs[i].glyphIndex) != groupIdx) continue;

      const EpdGlyph& glyph = fontData->glyph[slot.glyphs[i].glyphIndex];
      compactSingleGlyph(&tempBuf[slot.glyphs[i].alignedOffset], &slot.buffer[writeOffset], glyph.width, glyph.height);
      slot.glyphs[i].bufferOffset = writeOffset;
      writeOffset += glyph.dataLength;
    }
  }

  LOG_DBG("FDC", "Prewarm: %u glyphs in %u bytes from %u groups (%d missed)", glyphCount, writeOffset, groupCount,
          missed);

  return missed;
}

// --- Stats ---

void FontDecompressor::resetStats() { stats = Stats{}; }

void FontDecompressor::logStats(const char* label) {
  const uint32_t total = stats.cacheHits + stats.cacheMisses;
  LOG_DBG("FDC", "[%s] hits=%lu misses=%lu (%.1f%% hit rate)", label, stats.cacheHits, stats.cacheMisses,
          total > 0 ? 100.0f * stats.cacheHits / total : 0.0f);
  LOG_DBG("FDC", "[%s] decompress=%lums groups_accessed=%u slot_refusals=%lu", label, stats.decompressTimeMs,
          stats.uniqueGroupsAccessed, stats.prewarmSlotRefusals);
  LOG_DBG("FDC", "[%s] mem: pageBuf=%lu pageGlyphs=%lu hotGroup=%lu peakTemp=%lu", label, stats.pageBufferBytes,
          stats.pageGlyphsBytes, stats.hotGroupBytes, stats.peakTempBytes);
  if (stats.getBitmapCalls > 0) {
    LOG_DBG("FDC", "[%s] getBitmap: %lu calls, %luus total, %luus/call avg", label, stats.getBitmapCalls,
            stats.getBitmapTimeUs, stats.getBitmapTimeUs / stats.getBitmapCalls);
    if (stats.hotOffsetCacheHits > 0)
      LOG_DBG("FDC", "[%s] hot-group aligned-offset cache hits=%lu", label, stats.hotOffsetCacheHits);
  }
  resetStats();
}
