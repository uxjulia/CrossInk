#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Books whose local state changed since their last successful upload:
// /.crosspoint/sync_pending.bin. Each entry is a dirty mask plus the book's
// absolute path. Its size follows books changed since they were last synced,
// not the library: data types toggled off under What to Sync are never marked,
// and nothing is marked before a sync account exists. Every change rewrites the
// small file through the crash-safe .tmp/.bak pattern (SyncFiles).
namespace SyncPending {
constexpr const char* PATH = "/.crosspoint/sync_pending.bin";

enum Bit : uint8_t {
  PROGRESS_BIT = 1 << 0,
  STATS_BIT = 1 << 1,
  CLIPPINGS_BIT = 1 << 2,
};

// Creates the (empty) list if needed. Called when a bulk sync starts, before
// any catch-up can be recorded as done, so a later missing file always means
// the list was lost (and the catch-ups re-run), never "not created yet".
bool ensureExists();
// ORs `bits` into the book's entry (adding it if needed). Bits whose What to
// Sync toggle is off, and every bit without a sync account, are dropped first.
bool mark(const std::string& path, uint8_t bits);
// Clears `bits` after a successful upload; the entry goes when none remain.
bool clear(const std::string& path, uint8_t bits);
// Clears a USB sync acknowledgement without creating a previously missing queue.
bool clearExisting(const std::string& path, uint8_t bits);
// clear() for several books in one rewrite (a batched stats upload).
bool clearPaths(const std::vector<std::string>& paths, uint8_t bits);
// Clears `bits` on every entry (a What to Sync toggle was turned off).
bool clearEverywhere(uint8_t bits);
// In-app rename or move: the not-yet-uploaded entry follows the book.
bool renamePath(const std::string& from, const std::string& to);
// In-app delete.
bool removePath(const std::string& path);

// Entries whose path `keep` accepts (all when null); 0 when the list is
// missing or unreadable. Read-only, for sizing a progress bar.
uint32_t count(bool (*keep)(const std::string& path, void* context) = nullptr, void* context = nullptr);

// Walks a snapshot of the list taken by begin(), so the live list can change
// (bits cleared as uploads succeed) while the walk continues. No file handle is
// held between next() calls.
class Cursor {
 public:
  enum class Step : uint8_t { Entry, Done, Error };
  ~Cursor();
  bool begin();
  Step next(std::string& path, uint8_t& mask);
  void end();

 private:
  uint32_t offset = 0;
  bool active = false;
};

// Per-book "already marked" flags, so a reader writes each bit at most once
// however often it saves progress or stats. Any clear (a sync uploading the
// book) re-arms every marker, so a change after that sync is queued again.
class SessionMarker {
 public:
  // A different book than last time starts a new session.
  void mark(const std::string& bookPath, uint8_t bits);

 private:
  std::string path;
  uint8_t marked = 0;
  uint32_t generation = 0;
};
}  // namespace SyncPending
