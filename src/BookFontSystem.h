#pragma once

#if CROSSINK_SCALABLE_FONTS
#include <HalScalableFont.h>

#include <array>
#include <memory>
#include <string>

class Epub;
class GfxRenderer;

// Owns publisher faces for one reader session; families load when layout requests them.
class BookFontSystem {
 public:
  void ensure(Epub& epub, GfxRenderer& renderer, bool enabled);
  void open(Epub& epub, GfxRenderer& renderer);
  void close();
  int resolve(uint32_t family, uint8_t points);
  bool hasLoadedFamily(uint32_t family) const;
  bool copyFamilyName(uint32_t family, char* name, size_t capacity);
  uint32_t requestedFaces() const { return requestedFaces_; }
  uint32_t loadedFaces() const;
  uint32_t faceSignature(uint32_t mask) const;
  bool restoreFaces(uint32_t mask, uint32_t loaded, uint32_t signature);
  uint32_t signature() const { return signature_; }

 private:
  static constexpr size_t MaxFaces = 32;
  struct Face {
    uint32_t family = 0;
    uint8_t style = 0;
    bool attempted = false;
    bool registered = false;
    std::string href;
    std::unique_ptr<HalScalableFont> font;
  };
  std::array<Face, MaxFaces> faces_{};
  GfxRenderer* renderer_ = nullptr;
  size_t faceCount_ = 0;
  uint32_t signature_ = 0;
  Epub* book_ = nullptr;
  size_t remainingBytes_ = 4U * 1024U * 1024U;
  uint32_t requestedFaces_ = 0;
  void loadFace(size_t index);
  void registerFamily(uint32_t family);
  void loadFamily(uint32_t family);
  static int fontId(uint32_t family, uint8_t points);
};
extern BookFontSystem bookFontSystem;
#endif
