#pragma once
#include "EpdFontFamily.h"

class ScalableFontAccess {};
class HalScalableFont {
 public:
  class IdlePrewarmScope {};
  enum class BitmapPreparation { Ready, Skipped, Failed };
  inline static bool refused = false;
  static BitmapPreparation prepareIdleBitmap(const EpdFontData* data, const EpdGlyph* glyph) {
    refused = false;
    if (data->bitmapHandler(data->glyphMissCtx, glyph)) return BitmapPreparation::Ready;
    return refused ? BitmapPreparation::Skipped : BitmapPreparation::Failed;
  }
};
