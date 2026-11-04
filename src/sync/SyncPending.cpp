#include "SyncPending.h"

#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "SyncFiles.h"
#include "SyncState.h"

namespace SyncPending {
namespace {
constexpr uint8_t VERSION = 1;
constexpr size_t MAX_PATH_LENGTH = 1024;
constexpr const char* SNAPSHOT_PATH = "/.crosspoint/sync_pending.run";
constexpr uint8_t ALL_BITS = PROGRESS_BIT | STATS_BIT | CLIPPINGS_BIT;

// Which bits are worth recording right now.
uint8_t allowedBits(const uint8_t bits) {
  if (!KOREADER_STORE.hasCredentials()) return 0;
  uint8_t allowed = bits & PROGRESS_BIT;
  if (KOREADER_STORE.getSyncStats() && SETTINGS.shouldTrackReadingStats()) allowed |= bits & STATS_BIT;
  if (KOREADER_STORE.getSyncClippings()) allowed |= bits & CLIPPINGS_BIT;
  return allowed;
}

// One pass over the list: `edit` may change or zero (drop) each entry's mask.
// `append` adds an entry if no existing one had that path. Reading and
// writing are separate files (main vs .tmp), and the reader is closed before
// the swap, so SdFat never sees two handles on one file.
struct Edit {
  const std::string* path = nullptr;  // entry to change; nullptr = all (unless `paths`)
  const std::vector<std::string>* paths = nullptr;
  uint8_t setBits = 0;
  uint8_t clearBits = 0;
  const std::string* renameTo = nullptr;
  bool remove = false;
  bool appendIfMissing = false;
};

// Bumped whenever bits may have gone away, so SessionMarkers know to mark again.
uint32_t clearGeneration = 0;

bool matches(const Edit& edit, const std::string& entryPath) {
  if (edit.paths) {
    for (const auto& candidate : *edit.paths)
      if (candidate == entryPath) return true;
    return false;
  }
  return !edit.path || entryPath == *edit.path;
}

// Applies `edit` to every entry after the version byte. With a writer, keeps
// the entries that still have bits; without one, only reports what would change.
bool applyEdit(SyncFiles::Reader& reader, const Edit& edit, SyncFiles::Writer* writer, bool& changed, bool& found) {
  std::string entryPath;
  entryPath.reserve(128);
  while (!reader.atEnd()) {
    uint8_t mask = 0;
    if (!reader.pod(mask) || !reader.string16(entryPath, MAX_PATH_LENGTH)) {
      LOG_ERR("SYNC", "Pending list entry unreadable");
      return false;
    }
    if (matches(edit, entryPath)) {
      found = true;
      const uint8_t before = mask;
      mask = static_cast<uint8_t>((mask | edit.setBits) & ~edit.clearBits);
      if (edit.remove) mask = 0;
      if (edit.renameTo && edit.path) entryPath = *edit.renameTo;
      changed |= mask != before || edit.renameTo || edit.remove;
    }
    if (writer && (mask & ALL_BITS) && !(writer->pod(mask) && writer->string16(entryPath))) return false;
  }
  return true;
}

bool rewrite(const Edit& edit, const char* target = PATH) {
  SyncFiles::Reader reader(PATH);
  bool missing = false;
  auto source = reader.open(&missing);
  if (source == SyncFiles::Reader::Source::None) {
    // Never created, or lost along with its backup. A reader that has
    // completed sync work must redo it, since unpushed changes were lost.
    if (!missing) LOG_ERR("SYNC", "Pending list and backup unreadable");
    SyncState::resetAfterPendingLoss();
  } else {
    uint8_t version = 0;
    if (!reader.pod(version) || version != VERSION) {
      // Written by newer firmware: its entries can't be read, so treat them as lost.
      LOG_ERR("SYNC", "Unknown pending list version %u; starting empty", version);
      SyncState::resetAfterPendingLoss();
      reader.close();
      source = SyncFiles::Reader::Source::None;
    }
  }
  const bool readable = source != SyncFiles::Reader::Source::None;
  bool changed = source != SyncFiles::Reader::Source::Main;  // recreate a missing or restored file
  bool found = false;
  const bool append = edit.appendIfMissing && edit.path && edit.setBits;
  if (!changed && target == PATH) {
    // Most clears are no-ops (the book was never pending): find out by reading
    // before writing anything.
    if (!applyEdit(reader, edit, nullptr, changed, found)) return false;
    if (!changed && !(append && !found)) return true;
    if (!reader.seek(1)) return false;
    changed = false;
    found = false;
  }
  SyncFiles::Writer writer(target);
  if (!writer.open() || !writer.pod(VERSION)) return false;
  if (readable && !applyEdit(reader, edit, &writer, changed, found)) return false;
  if (append && !found && !(writer.pod(edit.setBits) && writer.string16(*edit.path))) return false;
  reader.close();
  if (!writer.commit()) return false;
  if (target == PATH && (edit.clearBits || edit.remove || edit.renameTo)) ++clearGeneration;
  return true;
}
}  // namespace

bool ensureExists() {
  Edit none;
  return rewrite(none);
}

bool mark(const std::string& path, const uint8_t bits) {
  const uint8_t allowed = allowedBits(bits);
  // A stats or clippings change that can't be queued right now (toggle off,
  // signed out, server not accepting them) would otherwise never be sent once
  // that data is back on: its catch-up has to run again.
  const uint8_t dropped = static_cast<uint8_t>(bits & ~allowed);
  if (dropped & STATS_BIT) SyncState::invalidateCatchup(true);
  if (dropped & CLIPPINGS_BIT) SyncState::invalidateCatchup(false);
  if (!allowed || path.empty() || path.size() > MAX_PATH_LENGTH) return true;
  Edit edit;
  edit.path = &path;
  edit.setBits = allowed;
  edit.appendIfMissing = true;
  const bool ok = rewrite(edit);
  if (!ok) LOG_ERR("SYNC", "Cannot mark pending: %s", path.c_str());
  return ok;
}

bool clear(const std::string& path, const uint8_t bits) {
  Edit edit;
  edit.path = &path;
  edit.clearBits = bits;
  return rewrite(edit);
}

bool clearPaths(const std::vector<std::string>& paths, const uint8_t bits) {
  if (paths.empty()) return true;
  Edit edit;
  edit.paths = &paths;
  edit.clearBits = bits;
  return rewrite(edit);
}

bool clearEverywhere(const uint8_t bits) {
  Edit edit;
  edit.clearBits = bits;
  return rewrite(edit);
}

bool renamePath(const std::string& from, const std::string& to) {
  if (from == to) return true;
  Edit edit;
  edit.path = &from;
  edit.renameTo = &to;
  return rewrite(edit);
}

bool removePath(const std::string& path) {
  Edit edit;
  edit.path = &path;
  edit.remove = true;
  return rewrite(edit);
}

Cursor::~Cursor() { end(); }

uint32_t count(bool (*keep)(const std::string& path, void* context), void* context) {
  SyncFiles::Reader reader(PATH);
  uint8_t version = 0;
  if (reader.open() == SyncFiles::Reader::Source::None || !reader.pod(version) || version != VERSION) return 0;
  uint32_t total = 0;
  std::string entryPath;
  entryPath.reserve(128);
  while (!reader.atEnd()) {
    uint8_t mask = 0;
    if (!reader.pod(mask) || !reader.string16(entryPath, MAX_PATH_LENGTH)) break;
    if (!keep || keep(entryPath, context)) ++total;
  }
  return total;
}

bool Cursor::begin() {
  end();
  // Copy the current list; clear() and mark() keep working on the live one.
  Edit copy;
  if (!rewrite(copy, SNAPSHOT_PATH)) return false;
  offset = 1;  // past the version byte
  active = true;
  return true;
}

Cursor::Step Cursor::next(std::string& path, uint8_t& mask) {
  if (!active) return Step::Done;
  // begin() just wrote and checked this snapshot; skip re-checking it per entry.
  SyncFiles::Reader reader(SNAPSHOT_PATH);
  if (reader.openUnchecked() == SyncFiles::Reader::Source::None || !reader.seek(offset)) {
    LOG_ERR("SYNC", "Pending snapshot unreadable");
    return Step::Error;
  }
  if (reader.atEnd()) return Step::Done;
  if (!reader.pod(mask) || !reader.string16(path, MAX_PATH_LENGTH)) return Step::Error;
  offset = reader.offset();
  return Step::Entry;
}

void Cursor::end() {
  if (!active) return;
  active = false;
  SyncFiles::removeAll(SNAPSHOT_PATH);
}

void SessionMarker::mark(const std::string& bookPath, const uint8_t bits) {
  // A sync may have cleared this book's bits since; mark again after any clear.
  if (bookPath != path || generation != clearGeneration) {
    path = bookPath;
    marked = 0;
    generation = clearGeneration;
  }
  const uint8_t fresh = static_cast<uint8_t>(bits & ~marked);
  if (!fresh || path.empty()) return;
  // Remember even when nothing was recordable (no account, toggle off), so a
  // reading session does not retry on every save.
  if (SyncPending::mark(path, fresh)) marked |= fresh;
}
}  // namespace SyncPending
