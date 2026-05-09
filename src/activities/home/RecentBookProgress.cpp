#include "RecentBookProgress.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Serialization.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include "RecentBooksStore.h"

namespace {
constexpr uint32_t TXT_CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t TXT_CACHE_VERSION = 2;

float clampProgressPercent(const float progress) { return std::clamp(progress, 0.0f, 100.0f); }

float loadEpubProgressPercent(const RecentBook& book) {
  Epub epub(book.path, "/.crosspoint");
  if (!epub.load(false, true)) {
    return -1.0f;
  }

  FsFile file;
  if (!Storage.openFileForRead("RBPR", epub.getCachePath() + "/progress.bin", file)) {
    return -1.0f;
  }

  uint8_t data[6];
  const int bytesRead = file.read(data, sizeof(data));
  file.close();
  if (bytesRead != 6) {
    return -1.0f;
  }

  const int spineIndex = data[0] | (data[1] << 8);
  const int currentPage = data[2] | (data[3] << 8);
  const int pageCount = data[4] | (data[5] << 8);
  if (pageCount <= 0) {
    return 0.0f;
  }

  const float chapterProgress = static_cast<float>(currentPage + 1) / static_cast<float>(pageCount);
  return clampProgressPercent(epub.calculateProgress(spineIndex, chapterProgress) * 100.0f);
}

float loadXtcProgressPercent(const RecentBook& book) {
  Xtc xtc(book.path, "/.crosspoint");
  if (!xtc.load()) {
    return -1.0f;
  }

  FsFile file;
  if (!Storage.openFileForRead("RBPR", xtc.getCachePath() + "/progress.bin", file)) {
    return -1.0f;
  }

  uint8_t data[4];
  const int bytesRead = file.read(data, sizeof(data));
  file.close();
  if (bytesRead != 4) {
    return -1.0f;
  }

  const uint32_t currentPage = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                               (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return clampProgressPercent(static_cast<float>(xtc.calculateProgress(currentPage)));
}

float loadTxtProgressPercent(const RecentBook& book) {
  Txt txt(book.path, "/.crosspoint");
  if (!txt.load()) {
    return -1.0f;
  }

  FsFile progressFile;
  if (!Storage.openFileForRead("RBPR", txt.getCachePath() + "/progress.bin", progressFile)) {
    return -1.0f;
  }

  uint8_t progressData[4];
  const int progressBytes = progressFile.read(progressData, sizeof(progressData));
  progressFile.close();
  if (progressBytes != 4) {
    return -1.0f;
  }

  const uint32_t currentPage = static_cast<uint32_t>(progressData[0]) | (static_cast<uint32_t>(progressData[1]) << 8) |
                               (static_cast<uint32_t>(progressData[2]) << 16) |
                               (static_cast<uint32_t>(progressData[3]) << 24);

  FsFile indexFile;
  if (!Storage.openFileForRead("RBPR", txt.getCachePath() + "/index.bin", indexFile)) {
    return -1.0f;
  }

  uint32_t magic = 0;
  uint8_t version = 0;
  uint32_t fileSize = 0;
  int32_t cachedWidth = 0;
  int32_t cachedLines = 0;
  int32_t fontId = 0;
  int32_t margin = 0;
  uint8_t alignment = 0;
  uint32_t totalPages = 0;
  const bool readOk =
      serialization::tryReadPod(indexFile, magic) && serialization::tryReadPod(indexFile, version) &&
      serialization::tryReadPod(indexFile, fileSize) && serialization::tryReadPod(indexFile, cachedWidth) &&
      serialization::tryReadPod(indexFile, cachedLines) && serialization::tryReadPod(indexFile, fontId) &&
      serialization::tryReadPod(indexFile, margin) && serialization::tryReadPod(indexFile, alignment) &&
      serialization::tryReadPod(indexFile, totalPages);
  indexFile.close();
  if (!readOk) {
    return -1.0f;
  }
  (void)cachedWidth;
  (void)cachedLines;
  (void)fontId;
  (void)margin;
  (void)alignment;

  if (magic != TXT_CACHE_MAGIC || version != TXT_CACHE_VERSION || fileSize != txt.getFileSize() || totalPages == 0) {
    return -1.0f;
  }

  return clampProgressPercent((static_cast<float>(currentPage + 1) / static_cast<float>(totalPages)) * 100.0f);
}
}  // namespace

float RecentBookProgress::loadPercent(const RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    return loadEpubProgressPercent(book);
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    return loadXtcProgressPercent(book);
  }
  if (FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path)) {
    return loadTxtProgressPercent(book);
  }
  return -1.0f;
}

bool RecentBookProgress::hasPercent(const float progress) { return progress >= 0.0f; }

std::string RecentBookProgress::formatPercent(const float progress) {
  if (!hasPercent(progress)) {
    return "";
  }
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "%.0f%%", clampProgressPercent(progress));
  return buffer;
}
