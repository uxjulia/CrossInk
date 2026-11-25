#pragma once

#include <cstdint>
#include <string>

// Document id -> book path for books touched on this reader:
// /.crosspoint/sync_ids.bin. The progress pull uses it to turn a change-feed
// hash back into a local file without walking the card. It only holds books
// with saved progress, stats or clippings, never XTC books (no position) and
// never finished books, so it stays close to "books being read".
//
// Format: a header (version, match method, generation, CRC32) then entries of
// [16-byte MD5][u8 path length][path][CRC32 of the entry], appended as books
// are recorded. A torn append only loses that entry: the next record() finds
// the partial tail and rewrites before appending. Whole-file rewrites (rename,
// removal, compaction) use the SyncFiles .tmp/.bak swap, and a crash between
// its renames is recovered from the newest complete copy.
namespace SyncIds {
constexpr const char* PATH = "/.crosspoint/sync_ids.bin";
constexpr size_t MAX_PATH_LENGTH = 255;

// Identifies this incarnation of the file; changes when it is created anew
// (reset, match method change, loss). 0 when there is no valid file. A book's
// cache sync record keeps the generation it was recorded under, so it only
// scans the file again after one of those events.
uint32_t generation();
// Records `path` under `id` for the configured match method. An existing
// entry for the path with another id is replaced. Returns the generation the
// entry now belongs to (0 on failure).
uint32_t record(const std::string& path, const uint8_t (&id)[16]);
bool removePath(const std::string& path);
// Every book in `folder` and its subfolders (an in-app folder delete), in one rewrite.
bool removeUnder(const std::string& folder);
bool renamePath(const std::string& from, const std::string& to);
// Match method change: every id is different now.
void reset();

// Streams every valid entry once; `visit` returns false to stop. `stale`
// (optional) counts entries whose book no longer exists, which costs one
// exists check per entry.
using Visit = bool (*)(const uint8_t (&id)[16], const std::string& path, void* context);
bool forEach(Visit visit, void* context, uint32_t* entries = nullptr, uint32_t* stale = nullptr);
// Rewrites the file without entries for missing books, under a new generation.
bool compact();
}  // namespace SyncIds
