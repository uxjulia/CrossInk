#pragma once

#include <HalStorage.h>

#include "../GfxRenderer/ImageToneProfile.h"

class Print;

class GifToBmpConverter {
  static bool gifFileToBmpStreamInternal(FsFile& gifFile, Print& bmpOut, int targetWidth, int targetHeight, bool oneBit,
                                         bool crop = true, ImageToneProfile toneProfile = ImageToneProfile::Legacy,
                                         bool adaptiveContain = false);

 public:
  static bool gifFileToBmpStream(FsFile& gifFile, Print& bmpOut, bool crop = true,
                                 ImageToneProfile toneProfile = ImageToneProfile::Legacy);
  static bool gifFileTo1BitBmpStreamWithSize(FsFile& gifFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                             bool adaptiveContain = false);
};
