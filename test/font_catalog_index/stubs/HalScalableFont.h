#pragma once
#include <cstring>
#include <fstream>

#include "HalStorage.h"
class HalScalableFont {
 public:
  struct Info {
    char family[64] = {};
    uint8_t style = 0;
    uint16_t weight = 400;
  };
  static inline unsigned inspections = 0;
  static bool inspectFile(const char* path, Info& info, bool* unavailable = nullptr) {
    ++inspections;
    if (unavailable) *unavailable = false;
    std::ifstream in(testRoot + path);
    std::string family;
    unsigned style = 0;
    if (!(in >> family >> style) || family.size() >= sizeof(info.family) || style > 3) return false;
    std::strcpy(info.family, family.c_str());
    info.style = style;
    info.weight = style & 1 ? 700 : 400;
    uint16_t weight;
    if (in >> weight) info.weight = weight;
    return true;
  }
};
