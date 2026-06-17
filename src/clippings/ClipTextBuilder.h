#pragma once

#include <vector>

#include "activities/ActivityResult.h"
#include "activities/reader/WordRef.h"

namespace ClipTextBuilder {

ClippingResult build(const std::vector<WordRef>& words, int from, int to, int total, int startPageInSection,
                     int sectionPageCount);

}  // namespace ClipTextBuilder
