#pragma once

#include <ImageToneProfile.h>

class HalFile;
class Print;

class BmpToBmpConverter {
 public:
  // Scales a BMP to the requested bounds the way book covers are converted: crop or fit, then one Atkinson
  // pass into a 2-bit BMP. 1-bit sources stay 1-bit so B/W images keep the B/W refresh path.
  static bool bmpFileToBmpStream(HalFile& bmpFile, Print& bmpOut, int targetWidth, int targetHeight, bool crop,
                                 ImageToneProfile toneProfile);
};
