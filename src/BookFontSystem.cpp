#include "BookFontSystem.h"

#if CROSSINK_SCALABLE_FONTS
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>

BookFontSystem bookFontSystem;

int BookFontSystem::fontId(const uint32_t family, const uint8_t points) {
  // A stable ID is useful for the renderer; page caches retain the CSS family instead.
  return static_cast<int>((family * 16777619U ^ points ^ 0x424b4600U) | 0x80000000U);
}

void BookFontSystem::close() {
  if (renderer_) {
    renderer_->setBookFontResolver(nullptr, nullptr);
    for (size_t i = 0; i < faceCount_; ++i) {
      if (!faces_[i].registered) continue;
      for (uint8_t points = ScalableContentMinPointSize; points <= ScalableContentMaxPointSize; ++points)
        renderer_->removeFont(fontId(faces_[i].family, points));
    }
  }
  for (auto& face : faces_) face = Face{};
  faceCount_ = 0;
  signature_ = 0;
  requestedFaces_ = 0;
  remainingBytes_ = 4U * 1024U * 1024U;
  renderer_ = nullptr;
  book_ = nullptr;
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
  size_t size = 0;
  if (!book_->getItemSize(face.href, &size) || size == 0 || size > HalScalableFont::MaxFileBytes ||
      size > remainingBytes_ || !HalScalableFont::prepareFamily(size, 1)) {
    LOG_ERR("BKF", "Skipping publisher face: %s", face.href.c_str());
    return;
  }
  const auto path = book_->getCachePath() + "/publisher-font-" + std::to_string(index);
  HalFile file;
  if (!Storage.openFileForWrite("BKF", path, file)) return;
  const bool extracted = book_->readFontContentsToStream(face.href, file, 1024) && file.size() == size;
  file.close();
  // The HAL owns the bytes; allocate one face once per session, never a duplicate extraction buffer.
  auto font = makeUniqueNoThrow<HalScalableFont>();
  if (!extracted || !font || !font->openFile(path.c_str(), size)) {
    LOG_ERR("BKF", "Cannot load publisher face: %s", face.href.c_str());
    Storage.remove(path.c_str());
    return;
  }
  remainingBytes_ -= size;
  face.font = std::move(font);
}

void BookFontSystem::loadFamily(const uint32_t family) {
  for (size_t i = 0; i < faceCount_; ++i)
    if (faces_[i].family == family) loadFace(i);
  registerFamily(family);
}

void BookFontSystem::registerFamily(const uint32_t family) {
  size_t regular = MaxFaces;
  for (size_t i = 0; i < faceCount_; ++i) {
    const auto& face = faces_[i];
    if (face.family != family) continue;
    if (face.registered) return;
    if (face.font && (regular == MaxFaces || face.style == 0)) regular = i;
  }
  if (regular == MaxFaces) return;
  for (uint8_t points = ScalableContentMinPointSize; points <= ScalableContentMaxPointSize; ++points) {
    if (renderer_->getFontMap().count(fontId(family, points))) {
      LOG_ERR("BKF", "Publisher font ID collision");
      return;
    }
  }
  for (uint8_t points = ScalableContentMinPointSize; points <= ScalableContentMaxPointSize; ++points) {
    const EpdFont* styles[4] = {};
    for (size_t i = 0; i < faceCount_; ++i) {
      const auto& face = faces_[i];
      if (face.family == family && face.font) styles[face.style] = face.font->atSize(points);
    }
    if (!styles[0]) styles[0] = faces_[regular].font->atSize(points);
    renderer_->insertFont(fontId(family, points), EpdFontFamily(styles[0], styles[1], styles[2], styles[3]));
  }
  faces_[regular].registered = true;
}

int BookFontSystem::resolve(const uint32_t family, const uint8_t points) {
  if (!renderer_ || !family) return 0;
  loadFamily(family);
  for (size_t i = 0; i < faceCount_; ++i) {
    if (faces_[i].family != family || !faces_[i].registered) continue;
    const int id = fontId(family, points);
    return renderer_->getFontMap().count(id) ? id : 0;
  }
  return 0;
}

uint32_t BookFontSystem::loadedFaces() const {
  uint32_t mask = 0;
  for (size_t i = 0; i < faceCount_; ++i)
    if (faces_[i].font) mask |= 1U << i;
  return mask;
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
  // Preserve successful admissions before retrying fallbacks; CSS order can differ from reading order.
  for (const uint32_t group : {loaded, mask & ~loaded})
    for (size_t i = 0; i < faceCount_; ++i)
      if (group & (1U << i)) loadFace(i);
  for (size_t i = 0; i < faceCount_; ++i)
    if (mask & (1U << i)) registerFamily(faces_[i].family);
  return faceSignature(mask) == signature;
}
#endif
