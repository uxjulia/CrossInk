#if !CROSSINK_SCALABLE_FONTS

#include "DropCapStrikeCache.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "CpfontStrike.h"
#include "SdCardFont.h"
#include "SdCardFontRegistry.h"

static_assert(cpfontStrike::FormatVersion == CPFONT_VERSION, "Update bounded strike reader for a new CPFont format");

bool DropCapStrikeCache::load(const char* path, uint32_t cp, uint8_t style, uint8_t bodyWidth, uint8_t bodyHeight) {
  auto file = Storage.open(path);
  if (!file) return false;
  const auto size = file.fileSize64();
  const cpfontStrike::Reader reader{&file, static_cast<uint32_t>(size),
                                    [](void* context, uint32_t offset, void* dest, size_t bytes) {
                                      auto& f = *static_cast<HalFile*>(context);
                                      return f.seekSet(offset) && f.read(dest, bytes) == static_cast<int>(bytes);
                                    }};
  cpfontStrike::Glyph glyph;
  if (size > UINT32_MAX || !cpfontStrike::find(reader, style, cp, glyph) || glyph.width < bodyWidth ||
      glyph.height < bodyHeight || (glyph.width == bodyWidth && glyph.height == bodyHeight)) {
    file.close();
    return false;
  }
  if (capacity_ < glyph.bytes) {
    scratch_.reset();
    capacity_ = 0;
    // Protect the grayscale strip and near-term reader work from an optional
    // glyph; use a malloc-backed, fallible buffer only once a candidate fits.
    constexpr size_t FreeReserve = 32 * 1024, LargestReserve = 12 * 1024;
    if (ESP.getFreeHeap() >= FreeReserve + glyph.bytes && ESP.getMaxAllocHeap() >= LargestReserve + glyph.bytes) {
      scratch_ = makeDefaultByteBufferNoThrow(glyph.bytes);
    }
    if (!scratch_) {
      file.close();
      return false;
    }
    capacity_ = glyph.bytes;
  }
  const bool read = reader.at(glyph.offset, scratch_.get(), glyph.bytes);
  file.close();
  if (!read) return false;
  bitmap_ = {scratch_.get(), glyph.width, glyph.height};
  LOG_DBG("DCP", "Streamed U+%04X: %u bytes, %ux%u", static_cast<unsigned>(cp), static_cast<unsigned>(glyph.bytes),
          static_cast<unsigned>(glyph.width), static_cast<unsigned>(glyph.height));
  return true;
}

DropCapStrikeCache::Bitmap DropCapStrikeCache::locate(int fontId, const char* bodyPath, const uint8_t points,
                                                      uint32_t cp, uint8_t style, uint16_t scale256, uint8_t bodyWidth,
                                                      uint8_t bodyHeight) {
  if (!bodyPath || !points || points >= cpfontStrike::MaxContentPoints || scale256 <= 256 || style >= 4) return {};
  const auto target = cpfontStrike::targetPoints(points, scale256);
  if (fontId == sourceFont_ && cp == sourceCodepoint_ && style == sourceStyle_ && target == sourceTarget_)
    return bitmap_;
  sourceFont_ = fontId;
  sourceCodepoint_ = cp;
  sourceStyle_ = style;
  sourceTarget_ = target;
  bitmap_ = {};

  if (fontId != selectionFont_ || target != selectionTarget_) select(fontId, bodyPath, points, target);
  if (selectionPath_[0]) load(selectionPath_, cp, style, bodyWidth, bodyHeight);
  return bitmap_;
}

void DropCapStrikeCache::select(int fontId, const char* bodyPath, uint8_t points, uint8_t target) {
  selectionFont_ = fontId;
  selectionTarget_ = target;
  auto& path = selectionPath_;
  path[0] = '\0';
  const char* filename = std::strrchr(bodyPath, '/');
  if (!filename || filename == bodyPath || !filename[1]) return;
  const size_t directoryLength = filename - bodyPath;
  if (directoryLength + 1 >= sizeof(path)) return;
  std::memcpy(path, bodyPath, directoryLength);
  path[directoryLength] = '\0';
  auto dir = Storage.open(path);
  if (!dir || !dir.isDirectory()) {
    dir.close();
    path[0] = '\0';
    return;
  }

  // The body registry is not kept alive during reading. Probe this one
  // already-selected family directory; keep only one path and no font tables.
  uint8_t bestPoints = 0;
  constexpr unsigned MaxDirectoryEntries = 96;  // 37 standard sizes, plus unrelated files
  char name[136];
  for (unsigned entryIndex = 0; entryIndex < MaxDirectoryEntries; ++entryIndex) {
    auto entry = dir.openNextFile();
    if (!entry) break;
    const size_t length = entry.isDirectory() ? 0 : entry.getName(name, sizeof(name));
    entry.close();
    if (!length || length >= sizeof(name) || directoryLength + 1 + length >= sizeof(path) ||
        (name[0] == '.' || name[0] == '_'))
      continue;
    uint8_t pointSize = 0, fileStyle = 0;
    if (!SdCardFontRegistry::parseFilename(name, pointSize, fileStyle) || fileStyle ||
        !cpfontStrike::preferPoints(points, target, bestPoints, pointSize))
      continue;
    bestPoints = pointSize;
    path[directoryLength] = '/';
    std::memcpy(path + directoryLength + 1, name, length + 1);
    if (bestPoints == target) break;
  }
  dir.close();
  if (!bestPoints) path[0] = '\0';
}

#endif
