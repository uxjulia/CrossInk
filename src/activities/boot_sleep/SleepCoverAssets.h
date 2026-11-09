#pragma once

#include <ImageToneProfile.h>

#include <string>

class GfxRenderer;
class Txt;
class Xtc;

namespace SleepCoverAssets {

bool prepareXtc(const Xtc& xtc);
bool prepareTxt(const Txt& txt);
bool prepareFullCoverForPath(const std::string& bookPath, bool cropped, const GfxRenderer* renderer = nullptr,
                             ImageToneProfile toneProfile = ImageToneProfile::Legacy);
bool prepareMinimalCoverForPath(const std::string& bookPath, const GfxRenderer& renderer, bool statsSleep = false);
bool prepareDashboardCoverForPath(const std::string& bookPath, const GfxRenderer& renderer);

std::string reusableCoverPathFor(const std::string& bookPath);
std::string cachedCoverPathFor(const std::string& bookPath, bool cropped,
                               ImageToneProfile toneProfile = ImageToneProfile::Legacy, bool* sourceBitmap = nullptr);
std::string cachedMinimalCoverPathFor(const std::string& bookPath, const GfxRenderer& renderer,
                                      bool statsSleep = false);
std::string cachedDashboardCoverPathFor(const std::string& bookPath, const GfxRenderer& renderer);

}  // namespace SleepCoverAssets
