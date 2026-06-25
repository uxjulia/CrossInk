#pragma once

#include <vector>

#include "activities/ActivityResult.h"
#include "activities/reader/WordRef.h"

namespace ClipTextBuilder {

ClippingResult build(const std::vector<WordRef>& words, const std::vector<int>& wordOrder, int fromOrder, int toOrder,
                     int totalOrder, int startPageInSection, int sectionPageCount);

}  // namespace ClipTextBuilder
