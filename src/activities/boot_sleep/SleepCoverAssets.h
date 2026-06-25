#pragma once

#include <string>

class Epub;
class Txt;
class Xtc;

namespace SleepCoverAssets {

bool prepareEpub(const Epub& epub);
bool prepareXtc(const Xtc& xtc);
bool prepareTxt(const Txt& txt);
bool prepareFullCoverForPath(const std::string& bookPath, bool cropped);
bool prepareDashboardCoverForPath(const std::string& bookPath);

std::string reusableCoverPathFor(const std::string& bookPath);
std::string cachedCoverPathFor(const std::string& bookPath, bool cropped);
std::string cachedMinimalCoverPathFor(const std::string& bookPath);
std::string cachedDashboardCoverPathFor(const std::string& bookPath);

}  // namespace SleepCoverAssets
