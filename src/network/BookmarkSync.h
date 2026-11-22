#pragma once

#include <string>

#include "StatsUploadClient.h"

namespace BookmarkSync {
// Call after the live EPUB reader has saved/unloaded BOOKMARKS. The operation
// owns its working copy and can leave pending changes for a subsequent sleep.
StatsUploadClient::Result run(const std::string& path, const std::string& document, bool (*cancel)(void*) = nullptr,
                              void* context = nullptr);
}  // namespace BookmarkSync
