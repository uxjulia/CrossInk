#pragma once
#include <SupportInfo.h>

namespace SupportInfoExport {
bool lastOpenedEpubAvailable();
SupportInfo::Result save(bool includeLastOpenedEpub);
}  // namespace SupportInfoExport
