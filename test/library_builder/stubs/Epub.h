#pragma once

#include <map>
#include <string>
#include <vector>

#include "HalStorage.h"

struct FakeMetadata {
  std::string title = "Title";
  std::string author = "Author";
  std::string series;
  std::string seriesIndex;
  std::string genre;
  bool success = true;
};

inline std::map<std::string, FakeMetadata> bookMetadata;
inline std::map<std::string, FakeMetadata> cachedBookMetadata;
inline std::vector<bool> metadataCacheUse;

class Epub {
  std::string path;

 public:
  Epub(const std::string& path, const char*) : path(path) {}

  static std::string resolveCachePathForFilePath(const std::string& path, const char*) {
    return "/.crosspoint/cache_" + path.substr(1);
  }
  std::string getCachePath() const { return resolveCachePathForFilePath(path, "/.crosspoint"); }
  bool clearCache() const {
    const std::string cachePath = getCachePath();
    return !Storage.exists(cachePath.c_str()) || Storage.removeDir(cachePath.c_str());
  }

  bool loadMetadata(std::string& title, std::string& author, const bool allowCachedMetadata = true,
                    std::string* series = nullptr, std::string* genre = nullptr, std::string* seriesIndex = nullptr) {
    ++fake::parses;
    metadataCacheUse.push_back(allowCachedMetadata);
    const auto cached = cachedBookMetadata.find(path);
    const auto& metadata =
        allowCachedMetadata && !series && !genre && !seriesIndex && cached != cachedBookMetadata.end()
            ? cached->second
            : bookMetadata[path];
    if (!metadata.success) return false;
    title = metadata.title;
    author = metadata.author;
    if (series) *series = metadata.series;
    if (genre) *genre = metadata.genre;
    if (seriesIndex) *seriesIndex = metadata.seriesIndex;
    return true;
  }
};
