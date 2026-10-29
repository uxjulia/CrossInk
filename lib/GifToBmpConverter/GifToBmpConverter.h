#pragma once

#include <HalStorage.h>

class Print;

class GifToBmpConverter {
  static bool gifFileToBmpStreamInternal(FsFile& gifFile, Print& bmpOut, int targetWidth, int targetHeight, bool oneBit,
                                         bool crop = true, bool originalThresholds = false,
                                         bool adaptiveContain = false);

 public:
  static bool gifFileToBmpStream(FsFile& gifFile, Print& bmpOut, bool crop = true, bool originalThresholds = false);
  static bool gifFileTo1BitBmpStreamWithSize(FsFile& gifFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                             bool adaptiveContain = false);
};
