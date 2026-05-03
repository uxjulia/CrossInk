#pragma once

#include <string>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

class GfxRenderer;
class MappedInputManager;

void renderBookStatsView(GfxRenderer& renderer, const MappedInputManager* mappedInput, const std::string& bookTitle,
                         const BookReadingStats& stats, const GlobalReadingStats& globalStats, bool showButtonHints);
