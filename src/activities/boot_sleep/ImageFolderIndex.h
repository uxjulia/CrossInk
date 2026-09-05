#pragma once

#include <cstdint>
#include <string>

namespace ImageFolderIndex {

struct Selection {
  std::string path;
  bool isPng = false;
  uint16_t index = 0;
};

// Finds the configured root-level boot-screen folder regardless of ASCII case.
// The hidden folder takes precedence when both names exist.
bool resolveBootScreenDirectory(std::string& directory);

// Select one image from the cached directory index. The cache is rebuilt when
// it is missing, stale, or when header validation is requested after a failed
// render. A cache failure returns false so callers can retain their directory
// scan fallback and never make sleep/boot depend on a writable cache.
//
// recentIndices/recentCapacity/recentPos/recentFill describe the caller's own
// circular buffer of recently-shown record indices (recentPos is the next
// write slot, recentFill is the number of valid entries), so this module has
// no dependency on any particular caller's history storage. recentWindow caps
// how far back to look when avoiding repeats.
bool select(const std::string& directory, bool includePng, bool validateBmpHeaders, const uint16_t* recentIndices,
            uint8_t recentCapacity, uint8_t recentPos, uint8_t recentFill, uint8_t recentWindow, Selection& selection);

// Drop all indexes. This only removes files owned by this module.
void invalidate();

// Invalidate only when a successful file mutation can affect a folder this
// module indexes (sleep-image or boot-screen folders). Paths are expected to
// be normalized absolute SD paths, as provided by the storage/file-transfer
// callers.
void invalidateForPath(const char* path);

}  // namespace ImageFolderIndex
