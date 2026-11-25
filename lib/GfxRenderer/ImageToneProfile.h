#pragma once

#include <cstdint>

enum class ImageToneProfile : uint8_t { Legacy, Even, PiccoCover };

constexpr ImageToneProfile imageToneProfile(bool evenLevels) {
  return evenLevels ? ImageToneProfile::Even : ImageToneProfile::Legacy;
}

constexpr const char* coverToneSuffix(ImageToneProfile profile) {
  switch (profile) {
    case ImageToneProfile::Legacy:
      return "";
    case ImageToneProfile::Even:
      return "_absolute";
    case ImageToneProfile::PiccoCover:
      return "_picco_image";
  }
  return "";
}
