#pragma once

#include <HalStorage.h>

#include "../GfxRenderer/ImageToneProfile.h"

class Print;
struct JpegThumbnailTarget {
  Print& output;
  int width;
  int height;
};

class JpegToBmpConverter {
  static bool jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool crop = true, bool adaptiveContain = false,
                                          ImageToneProfile toneProfile = ImageToneProfile::Legacy,
                                          const JpegThumbnailTarget* second = nullptr);

 public:
  static bool jpegFileToThumbnailPair(FsFile& jpegFile, const JpegThumbnailTarget& first,
                                      const JpegThumbnailTarget& second);
  static bool jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, bool crop = true,
                                  ImageToneProfile toneProfile = ImageToneProfile::Legacy);
  // Convert with custom target size (for thumbnails)
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  static bool jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                              bool adaptiveContain = false);
};
