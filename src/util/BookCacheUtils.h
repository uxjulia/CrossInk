#pragma once

#include <string>

// Clears the reading cache for a book file if its extension is recognised
// (EPUB, XTC, or TXT). Does nothing for other file types.
void clearBookCache(const std::string& path);

// Clears derived reading cache files while preserving user-owned state such as
// progress and per-book stats. Returns false if the cache clear or state
// preservation fails. Set sourceChanged after a file replacement to invalidate
// the cached sync document id while keeping its sync history.
bool clearBookCachePreservingUserState(const std::string& path, bool sourceChanged = false);

// Clears a known book cache directory while preserving dictionary lookup
// history, per-book stats and sync history.
bool clearBookCacheDirectoryPreservingStats(const std::string& cachePath);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);
