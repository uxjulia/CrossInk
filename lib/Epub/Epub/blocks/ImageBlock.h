#pragma once
#include <HalStorage.h>
#include <Memory.h>

#include <memory>
#include <string>

#include "Block.h"

class ImageBlock final : public Block {
 public:
  ImageBlock(std::string imagePath, std::string sourcePath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  void prepareCache() const;
  bool hasValidCache() const;
  bool needsDecode() const;
  void renderPlaceholder(GfxRenderer& renderer, int x, int y, bool foregroundBlack) const;
  static void clearSessionRenderFailures();
  static void releaseSessionPixelCache();

  // The section builder only reads image headers. The reader supplies this
  // allocation-free callback pair to seed a bundled pixel cache or extract the
  // original image on its first render.
  using ExtractFn = bool (*)(void* context, const char* sourcePath, const char* destinationPath);
  using SeedCacheFn = bool (*)(void* context, const char* sourcePath, int width, int height,
                               const char* destinationPath);
  using LoadFn = HeapByteBuffer (*)(void*, const char*, size_t&);
  static void setExtractor(void* context, ExtractFn extract, SeedCacheFn seedCache, LoadFn load = nullptr);

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y, const bool foregroundBlack, bool imageGrayscale = true);
  bool serialize(Print& file);
  static std::unique_ptr<ImageBlock> deserialize(FsFile& file);

 private:
  std::string imagePath;
  std::string sourcePath;
  int16_t width;
  int16_t height;

  static void* extractContext;
  static ExtractFn extractFn;
  static SeedCacheFn seedCacheFn;
  static LoadFn loadFn;
};
