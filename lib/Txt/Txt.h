#pragma once

#include <HalStorage.h>

#include <memory>
#include <string>

#include "../GfxRenderer/ImageToneProfile.h"

class Txt {
  std::string filepath;
  std::string cacheBasePath;
  std::string cachePath;
  std::string piccoSourceCoverPath() const { return cachePath + "/cover_picco_source.bmp"; }
  bool loaded = false;
  size_t fileSize = 0;

 public:
  explicit Txt(std::string path, std::string cacheBasePath);

  bool load();
  [[nodiscard]] const std::string& getPath() const { return filepath; }
  [[nodiscard]] const std::string& getCachePath() const { return cachePath; }
  [[nodiscard]] std::string getTitle() const;
  [[nodiscard]] size_t getFileSize() const { return fileSize; }

  void setupCacheDir() const;
  bool clearCache() const;

  // Cover image support - looks for cover.bmp/jpg/jpeg/png in same folder as txt file
  [[nodiscard]] std::string getCoverBmpPath(ImageToneProfile toneProfile = ImageToneProfile::Legacy,
                                            bool* sourceBitmap = nullptr) const;
  [[nodiscard]] bool generateCoverBmp(ImageToneProfile toneProfile = ImageToneProfile::Legacy) const;
  [[nodiscard]] std::string findCoverImage() const;

  // Read content from file
  [[nodiscard]] bool readContent(uint8_t* buffer, size_t offset, size_t length) const;
};
