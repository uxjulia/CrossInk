#include "BookFontSystem.h"

#if CROSSINK_SCALABLE_FONTS
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

BookFontSystem bookFontSystem;

namespace {
constexpr size_t PublisherFontRecordBytes = 52;

struct PublisherFontRecord {
  bool adobeObfuscated = false;
  std::array<uint8_t, 16> adobeKey{};
  uint32_t crc32 = 0;
  uint32_t compressedSize = 0;
  uint32_t uncompressedSize = 0;
  uint32_t fileSize = 0;
  uint32_t fingerprint = 0;
  uint32_t sequence = 0;
};

uint32_t readLittleU32(const uint8_t* bytes) {
  return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

void writeLittleU32(uint8_t* bytes, const uint32_t value) {
  bytes[0] = uint8_t(value);
  bytes[1] = uint8_t(value >> 8);
  bytes[2] = uint8_t(value >> 16);
  bytes[3] = uint8_t(value >> 24);
}

uint32_t recordChecksum(const uint8_t* bytes, const size_t length) {
  uint32_t hash = 2166136261U;
  for (size_t i = 0; i < length; ++i) hash = (hash ^ bytes[i]) * 16777619U;
  return hash;
}

bool readPublisherFontRecord(const std::string& path, PublisherFontRecord& record) {
  HalFile file;
  if (!Storage.openFileForRead("BKF", path, file)) return false;
  uint8_t bytes[PublisherFontRecordBytes]{};
  const bool validSize = file.fileSize() == sizeof(bytes);
  const int read = validSize ? file.read(bytes, sizeof(bytes)) : -1;
  const bool closed = file.close();
  if (!validSize || read != sizeof(bytes) || !closed || std::memcmp(bytes, "BKF1", 4) != 0 ||
      (readLittleU32(bytes + 4) & ~1U) != 0 || readLittleU32(bytes + 48) != recordChecksum(bytes, 48))
    return false;
  record.adobeObfuscated = readLittleU32(bytes + 4) & 1U;
  std::copy_n(bytes + 32, record.adobeKey.size(), record.adobeKey.begin());
  record.crc32 = readLittleU32(bytes + 8);
  record.compressedSize = readLittleU32(bytes + 12);
  record.uncompressedSize = readLittleU32(bytes + 16);
  record.fileSize = readLittleU32(bytes + 20);
  record.fingerprint = readLittleU32(bytes + 24);
  record.sequence = readLittleU32(bytes + 28);
  return true;
}

bool writePublisherFontRecord(const std::string& path, const PublisherFontRecord& record) {
  uint8_t bytes[PublisherFontRecordBytes] = {'B', 'K', 'F', '1'};
  writeLittleU32(bytes + 4, record.adobeObfuscated ? 1U : 0U);
  writeLittleU32(bytes + 8, record.crc32);
  writeLittleU32(bytes + 12, record.compressedSize);
  writeLittleU32(bytes + 16, record.uncompressedSize);
  writeLittleU32(bytes + 20, record.fileSize);
  writeLittleU32(bytes + 24, record.fingerprint);
  writeLittleU32(bytes + 28, record.sequence);
  std::copy(record.adobeKey.begin(), record.adobeKey.end(), bytes + 32);
  writeLittleU32(bytes + 48, recordChecksum(bytes, 48));
  HalFile file;
  if (!Storage.openFileForWrite("BKF", path, file)) return false;
  const bool written = file.write(bytes, sizeof(bytes)) == sizeof(bytes);
  const bool synced = file.sync();
  const bool closed = file.close();
  return written && synced && closed;
}

bool samePublisherFontSource(const PublisherFontRecord& record, const Epub::ItemIdentity& identity,
                             const size_t fileSize) {
  return record.adobeObfuscated == identity.adobeObfuscated && record.adobeKey == identity.adobeKey &&
         record.crc32 == identity.crc32 && record.compressedSize == identity.compressedSize &&
         record.uncompressedSize == identity.uncompressedSize && record.fileSize == fileSize;
}

}  // namespace

int BookFontSystem::fontId(const uint32_t family, const uint8_t points) {
  // A stable ID is useful for the renderer; page caches retain the CSS family instead.
  return static_cast<int>((family * 16777619U ^ points ^ 0x424b4600U) | 0x80000000U);
}

std::string BookFontSystem::publisherFontPath(const size_t index, const char* suffix) const {
  char name[48];
  const bool temporary = suffix && std::strcmp(suffix, ".tmp") == 0;
  std::snprintf(name, sizeof(name), "%spublisher-font-%u%s", temporary ? "/." : "/", unsigned(index),
                suffix ? suffix : "");
  return cachePath_ + name;
}

void BookFontSystem::close() {
  if (renderer_) {
    renderer_->setBookFontResolver(nullptr, nullptr);
    for (size_t i = 0; i < faceCount_; ++i) {
      const auto registered = faces_[i].registeredSizes;
      for (uint8_t points = ScalableContentMinPointSize; points <= ScalableContentMaxPointSize; ++points) {
        const auto bit = uint64_t{1} << (points - ScalableContentMinPointSize);
        if (registered & bit) renderer_->removeFont(fontId(faces_[i].family, points));
      }
    }
  }
  // Streamed protected-font files stay alive only while their face owns the
  // source handle. Destroy the face before removing its temporary extraction.
  for (size_t i = 0; i < faceCount_; ++i) {
    faces_[i].font.reset();
    if (faces_[i].temporaryExtraction) {
      const auto path = publisherFontPath(i, ".tmp");
      Storage.remove(path.c_str());
    }
  }
  for (auto& face : faces_) face = Face{};
  faceCount_ = 0;
  signature_ = 0;
  requestedFaces_ = 0;
  remainingBytes_ = 4U * 1024U * 1024U;
  renderer_ = nullptr;
  book_ = nullptr;
  cachePath_.clear();
}

void BookFontSystem::ensure(Epub& epub, GfxRenderer& renderer, const bool enabled) {
  if (!enabled) {
    close();
    return;
  }
  if (book_ == &epub) return;
  open(epub, renderer);
  book_ = &epub;
}

void BookFontSystem::open(Epub& epub, GfxRenderer& renderer) {
  close();
  std::vector<CssFontFace> faces;
  if (!CssParser::loadFontFaces(epub.getCachePath(), faces)) return;
  renderer_ = &renderer;
  book_ = &epub;
  cachePath_ = epub.getCachePath();
  // Invalidate layouts that fell back before Adobe font obfuscation support.
  signature_ = (2166136261U ^ 1U) * 16777619U;
  // Keep the bounded CSS paths for this session; font bytes are admitted only on demand.
  for (auto& source : faces) {
    const uint8_t style = (source.bold ? 1 : 0) | (source.italic ? 2 : 0);
    bool duplicate = false;
    for (size_t j = 0; j < faceCount_; ++j)
      if (faces_[j].family == source.family && faces_[j].style == style) duplicate = true;
    if (duplicate) continue;
    auto& face = faces_[faceCount_++];
    face.family = source.family;
    face.style = style;
    face.href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(source.href));
    for (const uint32_t value : {face.family, static_cast<uint32_t>(style)})
      signature_ = (signature_ ^ value) * 16777619U;
    for (const unsigned char c : face.href) signature_ = (signature_ ^ c) * 16777619U;
    signature_ = (signature_ ^ 0U) * 16777619U;
  }
  renderer.setBookFontResolver(
      this,
      [](void* ctx, uint32_t family, uint8_t points) {
        return static_cast<BookFontSystem*>(ctx)->resolve(family, points);
      },
      [](void* ctx) { return static_cast<BookFontSystem*>(ctx)->requestedFaces(); },
      [](void* ctx) { return static_cast<BookFontSystem*>(ctx)->loadedFaces(); },
      [](void* ctx, uint32_t mask) { return static_cast<BookFontSystem*>(ctx)->faceSignature(mask); },
      [](void* ctx, uint32_t mask, uint32_t loaded, uint32_t signature) {
        return static_cast<BookFontSystem*>(ctx)->restoreFaces(mask, loaded, signature);
      });
}

void BookFontSystem::loadFace(const size_t index) {
  auto& face = faces_[index];
  requestedFaces_ |= 1U << index;
  if (face.attempted) return;
  face.attempted = true;
  Epub::ItemIdentity identity;
  if (!book_->getItemIdentity(face.href, identity)) {
    LOG_ERR("BKF", "Cannot identify publisher face: %s", face.href.c_str());
    return;
  }
  size_t size = identity.uncompressedSize;
  if (identity.encrypted && !book_->getItemSize(face.href, &size)) {
    LOG_ERR("BKF", "Cannot measure protected publisher face: %s", face.href.c_str());
    return;
  }
  if (size == 0 || size > HalScalableFont::MaxFileBytes || size > remainingBytes_ ||
      !HalScalableFont::prepareFamily(size, 1)) {
    LOG_ERR("BKF", "Skipping publisher face: %s", face.href.c_str());
    return;
  }
  std::string slotPaths[2] = {publisherFontPath(index, ".a"), publisherFontPath(index, ".b")};
  std::string markerPaths[2] = {slotPaths[0] + ".meta", slotPaths[1] + ".meta"};

  // Protected-font output depends on the Adobe key, which is not part of the
  // ZIP entry CRC. Never reuse it across sessions. Unprotected faces validate
  // both source identity and the font-content fingerprint before reuse.
  if (!identity.encrypted) {
    for (size_t slot = 0; slot < 2; ++slot) {
      PublisherFontRecord record;
      if (!readPublisherFontRecord(markerPaths[slot], record) || !samePublisherFontSource(record, identity, size))
        continue;
      HalFile cached;
      if (!Storage.openFileForRead("BKF", slotPaths[slot], cached)) continue;
      const bool sameSize = cached.fileSize() == size;
      const bool closed = cached.close();
      if (!sameSize || !closed) continue;
      auto font = makeUniqueNoThrow<HalScalableFont>();
      if (font && font->openFile(slotPaths[slot].c_str(), size) && font->fingerprint() == record.fingerprint) {
        LOG_DBG("BKF", "Reused %u-byte publisher face from cache: %s", static_cast<unsigned>(size), face.href.c_str());
        remainingBytes_ -= size;
        face.font = std::move(font);
        return;
      }
      // A corrupt or unreadable slot stays untouched until a fresh extraction
      // has passed both the parser and fingerprint checks.
    }
  }

  const std::string extractionPath = publisherFontPath(index, ".tmp");
  const auto removeLegacyExtraction = [&]() {
    const auto legacyPath = publisherFontPath(index, "");
    Storage.remove(legacyPath.c_str());
    Storage.remove((legacyPath + ".meta").c_str());
  };
  HalFile file;
  if (!Storage.openFileForWrite("BKF", extractionPath, file)) return;
  const bool extracted = book_->readFontContentsToStream(face.href, file, 1024) && file.size() == size;
  const bool synced = file.sync();
  const bool closed = file.close();
  if (!extracted || !synced || !closed) {
    LOG_ERR("BKF", "Could not extract publisher face: %s", face.href.c_str());
    Storage.remove(extractionPath.c_str());
    return;
  }

  auto font = makeUniqueNoThrow<HalScalableFont>();
  if (!font || !font->openFile(extractionPath.c_str(), size)) {
    LOG_ERR("BKF", "Cannot load publisher face: %s", face.href.c_str());
    font.reset();
    Storage.remove(extractionPath.c_str());
    return;
  }
  const uint32_t fingerprint = font->fingerprint();
  if (identity.encrypted) {
    LOG_DBG("BKF", "Loaded protected publisher face for this session: %s", face.href.c_str());
    removeLegacyExtraction();
    remainingBytes_ -= size;
    face.temporaryExtraction = true;
    face.font = std::move(font);
    return;
  }

  // Keep the two most recent complete versions. Extraction and validation
  // happen in a third staging path, so read/write failures leave both slots
  // available as the last-known-good fallback.
  PublisherFontRecord records[2]{};
  bool hasRecord[2] = {readPublisherFontRecord(markerPaths[0], records[0]),
                       readPublisherFontRecord(markerPaths[1], records[1])};
  const uint32_t maxSequence =
      std::max(hasRecord[0] ? records[0].sequence : 0U, hasRecord[1] ? records[1].sequence : 0U);
  size_t target = !Storage.exists(slotPaths[0].c_str()) || !Storage.exists(markerPaths[0].c_str())   ? 0
                  : !Storage.exists(slotPaths[1].c_str()) || !Storage.exists(markerPaths[1].c_str()) ? 1
                  : records[0].sequence <= records[1].sequence                                       ? 0
                                                                                                     : 1;
  const std::string markerStage = extractionPath + ".meta";
  PublisherFontRecord record;
  record.adobeObfuscated = identity.adobeObfuscated;
  record.adobeKey = identity.adobeKey;
  record.crc32 = identity.crc32;
  record.compressedSize = identity.compressedSize;
  record.uncompressedSize = identity.uncompressedSize;
  record.fileSize = static_cast<uint32_t>(size);
  record.fingerprint = fingerprint;
  record.sequence = maxSequence + 1;
  if (!writePublisherFontRecord(markerStage, record)) {
    LOG_ERR("BKF", "Could not save publisher font identity: %s", face.href.c_str());
    font.reset();
    Storage.remove(extractionPath.c_str());
    Storage.remove(markerStage.c_str());
    return;
  }

  // Close the staged source before publishing it under the slot name.
  font.reset();
  if ((Storage.exists(slotPaths[target].c_str()) && !Storage.remove(slotPaths[target].c_str())) ||
      (Storage.exists(markerPaths[target].c_str()) && !Storage.remove(markerPaths[target].c_str())) ||
      !Storage.rename(extractionPath.c_str(), slotPaths[target].c_str()) ||
      !Storage.rename(markerStage.c_str(), markerPaths[target].c_str())) {
    LOG_ERR("BKF", "Could not publish publisher font cache: %s", face.href.c_str());
    Storage.remove(extractionPath.c_str());
    Storage.remove(markerStage.c_str());
    return;
  }
  font = makeUniqueNoThrow<HalScalableFont>();
  if (!font || !font->openFile(slotPaths[target].c_str(), size) || font->fingerprint() != fingerprint) {
    LOG_ERR("BKF", "Published publisher font failed verification: %s", face.href.c_str());
    font.reset();
    // Leave the previous slot intact; the next session can rebuild this one.
    return;
  }
  // Remove the legacy single-file cache only after the replacement is proven.
  removeLegacyExtraction();
  LOG_DBG("BKF", "Extracted %u-byte publisher face into cache: %s", static_cast<unsigned>(size), face.href.c_str());
  remainingBytes_ -= size;
  face.font = std::move(font);
}

void BookFontSystem::loadFamily(const uint32_t family) {
  for (size_t i = 0; i < faceCount_; ++i)
    if (faces_[i].family == family) loadFace(i);
  // Register only what layout requested. This keeps renderer map setup
  // proportional to sizes actually used by the book.
}

bool BookFontSystem::registerPointSize(const uint32_t family, const size_t regular, const uint8_t points) {
  if (points < ScalableContentMinPointSize || points > ScalableContentMaxPointSize) return false;
  const auto bit = uint64_t{1} << (points - ScalableContentMinPointSize);
  if (faces_[regular].registeredSizes & bit) return true;
  const int id = fontId(family, points);
  if (renderer_->getFontMap().count(id)) {
    LOG_ERR("BKF", "Publisher font ID collision");
    return false;
  }
  const EpdFont* styles[4] = {};
  for (size_t i = 0; i < faceCount_; ++i) {
    const auto& face = faces_[i];
    if (face.family == family && face.font) styles[face.style] = face.font->atSize(points);
  }
  if (!styles[0]) styles[0] = faces_[regular].font->atSize(points);
  renderer_->insertFont(id, EpdFontFamily(styles[0], styles[1], styles[2], styles[3]));
  faces_[regular].registeredSizes |= bit;
  faces_[regular].registered = true;
  return true;
}

bool BookFontSystem::registerFamily(const uint32_t family, const uint8_t points) {
  size_t regular = MaxFaces;
  for (size_t i = 0; i < faceCount_; ++i) {
    const auto& face = faces_[i];
    if (face.family != family) continue;
    if (face.font && (regular == MaxFaces || face.style == 0)) regular = i;
  }
  return regular != MaxFaces && registerPointSize(family, regular, points);
}

int BookFontSystem::resolve(const uint32_t family, const uint8_t points) {
  if (!renderer_ || !family) return 0;
  loadFamily(family);
  if (!registerFamily(family, points)) return 0;
  const int id = fontId(family, points);
  return renderer_->getFontMap().count(id) ? id : 0;
}

uint32_t BookFontSystem::loadedFaces() const {
  uint32_t mask = 0;
  for (size_t i = 0; i < faceCount_; ++i)
    if (faces_[i].font) mask |= 1U << i;
  return mask;
}

bool BookFontSystem::hasLoadedFamily(const uint32_t family) const {
  for (size_t i = 0; i < faceCount_; ++i)
    if (faces_[i].family == family && faces_[i].registered) return true;
  return false;
}

bool BookFontSystem::copyFamilyName(const uint32_t family, char* name, const size_t capacity) {
  if (!name || !capacity) return false;
  name[0] = '\0';
  for (size_t i = 0; i < faceCount_; ++i) {
    auto& face = faces_[i];
    if (face.family == family && face.registered && face.font) return face.font->copyFamilyName(name, capacity);
  }
  return false;
}

uint32_t BookFontSystem::faceSignature(const uint32_t mask) const {
  if (!mask) return 0;
  uint32_t hash = 2166136261U;
  for (size_t i = 0; i < faceCount_; ++i) {
    if (!(mask & (1U << i))) continue;
    const auto& face = faces_[i];
    for (const uint32_t value : {face.family, static_cast<uint32_t>(face.style),
                                 face.font ? face.font->fingerprint() : 0U, static_cast<uint32_t>(face.registered)})
      hash = (hash ^ value) * 16777619U;
  }
  return hash;
}

bool BookFontSystem::restoreFaces(const uint32_t mask, const uint32_t loaded, const uint32_t signature) {
  const uint32_t valid = faceCount_ == MaxFaces ? UINT32_MAX : (1U << faceCount_) - 1;
  if ((mask & ~valid) || (loaded & ~mask)) return false;
  std::array<bool, MaxFaces> registeredBefore{};
  for (size_t i = 0; i < faceCount_; ++i) registeredBefore[i] = faces_[i].registered;
  // Preserve successful admissions before retrying fallbacks; CSS order can differ from reading order.
  for (const uint32_t group : {loaded, mask & ~loaded})
    for (size_t i = 0; i < faceCount_; ++i)
      if (group & (1U << i)) loadFace(i);
  // A cached page stores family and point size per word. Restore the family
  // representative used by the saved signature, then let word rendering
  // register only the exact sizes it needs.
  for (size_t i = 0; i < faceCount_; ++i)
    if (mask & (1U << i)) {
      const auto family = faces_[i].family;
      size_t regular = MaxFaces;
      for (size_t candidate = 0; candidate < faceCount_; ++candidate) {
        const auto& face = faces_[candidate];
        if (face.family != family || !(loaded & (1U << candidate)) || !face.font) continue;
        if (regular == MaxFaces || face.style == 0) regular = candidate;
      }
      if (regular != MaxFaces) faces_[regular].registered = true;
    }
  if (faceSignature(mask) == signature) return true;
  // A changed font set must not leave a speculative representative behind;
  // full indexing will establish the representative for the repaired set.
  for (size_t i = 0; i < faceCount_; ++i) faces_[i].registered = registeredBefore[i];
  return false;
}
#endif
