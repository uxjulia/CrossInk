#pragma once

#include "AssetDownloadService.h"

// Verified recipient sources/notices on SD; never activates a language or pack.
namespace AssetCompanionStore {
AssetDownloadService::Result prepare(const AssetCatalog::Metadata& catalog, const AssetCatalog::Entry& entry,
                                     const AssetDownloadService::Endpoints& endpoints,
                                     const AssetDownloadService::Callbacks& callbacks = {});
}  // namespace AssetCompanionStore
