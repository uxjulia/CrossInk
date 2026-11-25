#pragma once
#include <strings.h>

#include <cstring>

namespace fontfiles {
// Extension selects a candidate only; HalScalableFont validates the outline format.
inline bool isScalable(const char* name) {
  if (!name) return false;
  const size_t length = std::strlen(name);
  return length > 4 && (strcasecmp(name + length - 4, ".ttf") == 0 || strcasecmp(name + length - 4, ".otf") == 0);
}
}  // namespace fontfiles
