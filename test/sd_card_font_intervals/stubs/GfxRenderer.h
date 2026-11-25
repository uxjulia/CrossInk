#pragma once
#include <EpdFontFamily.h>

#include <map>
class SdCardFont;
class GfxRenderer {
  std::map<int, EpdFontFamily> fonts_;
  std::map<int, SdCardFont*> sd_;

 public:
  const auto& getFontMap() const { return fonts_; }
  const auto& getSdCardFonts() const { return sd_; }
  void insertFont(int id, EpdFontFamily font) { fonts_.emplace(id, font); }
  void registerSdCardFont(int id, SdCardFont* font) { sd_[id] = font; }
  void removeFont(int id) {
    fonts_.erase(id);
    sd_.erase(id);
  }
  void clearSdCardFonts() { sd_.clear(); }
};
