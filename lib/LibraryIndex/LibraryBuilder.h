#pragma once

// Builds the CLX1 index by walking the SD card once.
//
// The walk derives filename fallbacks and can read title, author, series/order, and subject metadata from
// EPUBs. Fresh metadata is reused from the previous index.
//
// Shape of the build, and why:
//
//   * ONE walk. Records go straight into a staging file in discovery order, so
//     nothing proportional to the library stays resident. The previous index is
//     matched through a sorted file with a small in-RAM fence, and every order
//     is sorted in bounded runs (LibrarySort.h) whose buffer is the only sizable
//     allocation after the walk: 32 KiB on the C3, the whole sort in PSRAM.
//   * Duplicate directory entries are dropped. A damaged FAT can hand the same
//     file out twice, and without this the shelf shows phantom books that
//     cannot be opened.
//   * Unreadable entries are skipped, never fatal.
//   * Install is write-then-rename, so an interrupted build leaves the previous
//     index untouched rather than a half-written one.

#include <cstdint>
#include <string>

#include "LibraryFormat.h"

namespace library {

// Directory levels below the scan root that are walked. The measured corpus is
// two deep (genre/author/book); the cap exists because a corrupted FAT can
// contain a directory that contains itself, and an uncapped walk would never
// return.
inline constexpr int LIBRARY_MAX_DEPTH = 5;

// Duplicate identities remembered while one directory is enumerated. The
// fixed, fallible allocation is 8 KiB at this cap; unlike std::vector it cannot
// grow into abort() when a damaged or unusually flat directory is scanned.
inline constexpr uint16_t LIBRARY_MAX_DEDUP_KEYS = 1024;

// Why a build did not install a new index. Error covers I/O, allocation, and
// card faults; the other values let the UI say something more useful.
enum class BuildFailure : uint8_t {
  None,
  Error,
  Cancelled,
  TooManyBooks,
};

struct BuildStats {
  BuildFailure failure = BuildFailure::None;
  uint16_t books = 0;
  uint16_t folders = 0;
  uint16_t duplicatesDropped = 0;
  uint16_t unreadableSkipped = 0;
  uint32_t walkMs = 0;
  // Reconciliation against the previous index. Their sum over a rebuild with no
  // card changes should be: unchanged == books, everything else zero.
  uint16_t unchanged = 0;  // same full path: keeps its place in "Recently added"
  uint16_t added = 0;      // matched nothing, not even by size
  uint16_t renamed = 0;    // matched a leftover entry by size alone
  uint16_t removed = 0;    // previous entry no book claimed
  uint16_t enriched = 0;   // took its title or author from the book rather than the filename
  uint16_t parsed = 0;     // EPUB metadata reads performed by this build
  uint16_t metadataReused = 0;
  uint16_t metadataCached = 0;  // EPUB metadata restored from library.meta instead of parsed
  bool indexReplaced = false;
  bool ranksDegraded = false;
  // Duplicate detection was incomplete: either its key buffer could not be
  // allocated (dedupAllocFailed, worth retrying) or one folder chain holds more
  // books than the tracker remembers (same result on every scan).
  bool dedupDegraded = false;
  bool dedupAllocFailed = false;
  bool arrivalDegraded = false;
};

enum class BuildPhase : uint8_t {
  Scanning,    // walking folders and reading book metadata; `books` counts what was found
  Organizing,  // sorting and writing the finished index
};

struct BuildProgress {
  BuildPhase phase = BuildPhase::Scanning;
  uint16_t books = 0;
};

// Optional hooks for a foreground build. Plain function pointers keep the
// library free of std::function; `context` is borrowed and passed back as-is.
struct BuildCallbacks {
  void* context = nullptr;
  // Polled between bounded units of build work. Returning true stops the build
  // and leaves the previous index installed.
  bool (*cancelRequested)(void* context) = nullptr;
  // Throttled to one call per LIBRARY_PROGRESS_INTERVAL_MS, plus one at each
  // phase change, because every call usually repaints an e-ink panel.
  void (*progress)(void* context, const BuildProgress& progress) = nullptr;
};

inline constexpr uint32_t LIBRARY_CANCEL_POLL_MS = 50;
inline constexpr uint32_t LIBRARY_PROGRESS_INTERVAL_MS = 3000;

// Books one build can index. Sorting spills bounded runs to the card, so RAM
// does not limit this on any device; the format ceiling does.
uint16_t libraryBookLimit();

// Walk `rootPath`, write `/.crosspoint/library.idx`, and report what happened.
// The previous index, including its monotonic "recently added" counter, is read
// internally so callers cannot accidentally split one rebuild state across two
// file opens.
// `readMetadata` takes title, author, series/order, and subject from each EPUB. It
// stops the normal EPUB parser at the end of <metadata>, before the manifest,
// without building the reader's spine, TOC, CSS, or section caches. Unchanged
// books reuse these values from the prior Library index.
// `callbacks` may be null.
bool buildLibraryIndex(const char* rootPath, BuildStats& stats, bool readMetadata = false,
                       const BuildCallbacks* callbacks = nullptr);

// Starts dirty on cold boots to reconcile external card edits. File-changing
// activities must invalidate before returning to Library. A successful scan
// clears only changes known when it started; failures and allocation-starved
// builds remain retryable. Results a rescan would repeat, such as skipped
// unreadable files or a capped duplicate tracker, do not re-dirty the index.
void invalidateLibraryIndex();
bool libraryIndexNeedsRefresh();
// Changes when known book paths change or Library reconciles the card.
uint32_t libraryPathRevision();
void resetLibraryPathRevision();
// Boot-only: called after validating a clean scan retained across deep sleep.
void restoreLibraryIndexAfterSleep();

// Live index path, shared by the builder and activity.
const char* libraryIndexPath();

}  // namespace library
