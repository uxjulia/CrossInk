#include "SyncState.h"

#include <Arduino.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>
#if !defined(SIMULATOR)
#include <esp_system.h>
#endif

#include "SyncFiles.h"
#include "SyncParked.h"

namespace {
// Version 2 adds the scope list after the version 1 fields.
constexpr uint8_t VERSION = 2;
constexpr uint8_t VERSION_WITHOUT_SCOPES = 1;
constexpr uint8_t FLAG_STATS_CAUGHT_UP = 1 << 0;
constexpr uint8_t FLAG_CLIPPINGS_CAUGHT_UP = 1 << 1;
constexpr uint8_t SCOPE_PREPARED = 1 << 0;
constexpr uint8_t SCOPE_STATS_CAUGHT_UP = 1 << 1;
constexpr uint8_t SCOPE_CLIPPINGS_CAUGHT_UP = 1 << 2;
constexpr size_t MAX_IDENTITY_LENGTH = 512;

uint32_t randomStamp() {
#if !defined(SIMULATOR)
  return esp_random() | 1u;
#else
  return (static_cast<uint32_t>(millis()) ^ 0x53594e43u) | 1u;
#endif
}

SyncState freshFor(const std::string& baseUrl, const std::string& username, const uint32_t nextStampId) {
  SyncState state;
  state.baseUrl = baseUrl;
  state.username = username;
  // A random start keeps stamps from a lost state file from matching new ones.
  state.nextStampId = nextStampId ? nextStampId : randomStamp();
  return state;
}

// Reads the fixed fields; `version` tells the caller whether scopes follow.
bool readHeader(SyncFiles::Reader& reader, SyncState& state, uint8_t& version) {
  state = {};
  uint8_t flags = 0;
  if (!reader.pod(version) || (version != VERSION && version != VERSION_WITHOUT_SCOPES) ||
      !reader.string16(state.baseUrl, MAX_IDENTITY_LENGTH) || !reader.string16(state.username, MAX_IDENTITY_LENGTH) ||
      !reader.pod(flags) || !reader.pod(state.statsCatchupId) || !reader.pod(state.clippingsCatchupId) ||
      !reader.pod(state.nextStampId)) {
    return false;
  }
  state.statsCaughtUp = flags & FLAG_STATS_CAUGHT_UP;
  state.clippingsCaughtUp = flags & FLAG_CLIPPINGS_CAUGHT_UP;
  if (!state.nextStampId) state.nextStampId = randomStamp();
  return true;
}

bool writeHeader(SyncFiles::Writer& writer, const SyncState& state) {
  const uint8_t flags =
      (state.statsCaughtUp ? FLAG_STATS_CAUGHT_UP : 0) | (state.clippingsCaughtUp ? FLAG_CLIPPINGS_CAUGHT_UP : 0);
  return writer.pod(VERSION) && writer.string16(state.baseUrl) && writer.string16(state.username) &&
         writer.pod(flags) && writer.pod(state.statsCatchupId) && writer.pod(state.clippingsCatchupId) &&
         writer.pod(state.nextStampId);
}

bool readScope(SyncFiles::Reader& reader, SyncScope& scope) {
  uint8_t flags = 0;
  if (!reader.string16(scope.path, SyncScope::MAX_PATH_LENGTH) || !reader.pod(flags) || !reader.pod(scope.cursor) ||
      !reader.pod(scope.lastSynced) || !reader.pod(scope.walkId) || !reader.pod(scope.statsCatchupId) ||
      !reader.pod(scope.clippingsCatchupId)) {
    return false;
  }
  scope.prepared = flags & SCOPE_PREPARED;
  scope.statsCaughtUp = flags & SCOPE_STATS_CAUGHT_UP;
  scope.clippingsCaughtUp = flags & SCOPE_CLIPPINGS_CAUGHT_UP;
  return true;
}

bool writeScope(SyncFiles::Writer& writer, const SyncScope& scope) {
  const uint8_t flags = (scope.prepared ? SCOPE_PREPARED : 0) | (scope.statsCaughtUp ? SCOPE_STATS_CAUGHT_UP : 0) |
                        (scope.clippingsCaughtUp ? SCOPE_CLIPPINGS_CAUGHT_UP : 0);
  return writer.string16(scope.path) && writer.pod(flags) && writer.pod(scope.cursor) && writer.pod(scope.lastSynced) &&
         writer.pod(scope.walkId) && writer.pod(scope.statsCatchupId) && writer.pod(scope.clippingsCatchupId);
}

// Edits applied while streaming the file into its replacement. Every field is
// optional: header replaces the fixed fields, `edit` may change or drop each
// stored scope (return false to drop), and `append` is written last.
struct Rewrite {
  const SyncState* header = nullptr;
  bool (*edit)(SyncScope& scope, void* context) = nullptr;
  void* context = nullptr;
  const SyncScope* append = nullptr;
};

bool rewriteState(const Rewrite& rewrite) {
  SyncFiles::Reader reader(SyncState::PATH);
  SyncState stored;
  uint8_t version = 0;
  const bool readable = reader.open() != SyncFiles::Reader::Source::None && readHeader(reader, stored, version);
  if (!readable && !rewrite.header) return false;
  const SyncState& header = rewrite.header ? *rewrite.header : stored;
  // Scopes belong to the account they were made for.
  const bool keepScopes =
      readable && version == VERSION && stored.baseUrl == header.baseUrl && stored.username == header.username;
  SyncFiles::Writer writer(SyncState::PATH);
  if (!writer.open() || !writeHeader(writer, header)) return false;
  if (keepScopes) {
    SyncScope scope;
    while (!reader.atEnd()) {
      if (!readScope(reader, scope)) {
        LOG_ERR("SYNC", "Sync scope unreadable; dropping the rest");
        break;
      }
      if (rewrite.append && scope.path == rewrite.append->path) continue;  // replaced below
      if (rewrite.edit && !rewrite.edit(scope, rewrite.context)) continue;
      if (!writeScope(writer, scope)) return false;
    }
  }
  if (rewrite.append && !writeScope(writer, *rewrite.append)) return false;
  reader.close();
  const bool ok = writer.commit();
  if (!ok) LOG_ERR("SYNC", "Cannot save sync state");
  return ok;
}

// Streams the current account's scopes; `visit` returns false to stop.
template <typename Visit>
bool forEachScope(Visit visit) {
  SyncFiles::Reader reader(SyncState::PATH);
  SyncState stored;
  uint8_t version = 0;
  if (reader.open() == SyncFiles::Reader::Source::None || !readHeader(reader, stored, version) || version != VERSION ||
      stored.baseUrl != KOREADER_STORE.getBaseUrl() || stored.username != KOREADER_STORE.getUsername())
    return false;
  SyncScope scope;
  while (!reader.atEnd()) {
    if (!readScope(reader, scope)) return false;
    if (!visit(scope)) return true;
  }
  return true;
}

bool clearScopeBits(SyncScope& scope, void* context) {
  const uint8_t which = *static_cast<const uint8_t*>(context);
  if (which & 1) {
    scope.statsCaughtUp = false;
    scope.statsCatchupId = 0;
  }
  if (which & 2) {
    scope.clippingsCaughtUp = false;
    scope.clippingsCatchupId = 0;
  }
  if (which & 4) {
    // The scope must walk again; its cursor is read anew before that walk.
    scope.prepared = false;
    scope.walkId = 0;
    scope.cursor = 0;
  }
  return true;
}
}  // namespace

bool SyncScope::covers(const std::string& bookPath) const {
  if (path == "/") return true;
  return bookPath.size() > path.size() && bookPath.compare(0, path.size(), path) == 0 && bookPath[path.size()] == '/';
}

bool SyncState::readStored(SyncState& state) {
  SyncFiles::Reader reader(PATH);
  uint8_t version = 0;
  if (reader.open() == SyncFiles::Reader::Source::None) return false;
  if (!readHeader(reader, state, version)) {
    LOG_ERR("SYNC", "Unreadable sync state; starting fresh");
    return false;
  }
  return true;
}

SyncState SyncState::load() {
  const std::string baseUrl = KOREADER_STORE.getBaseUrl();
  const std::string& username = KOREADER_STORE.getUsername();
  SyncState state;
  if (!readStored(state)) return freshFor(baseUrl, username, 0);
  if (state.baseUrl != baseUrl || state.username != username) {
    LOG_INF("SYNC", "Sync account changed; catch-ups run again");
    // Another account's remote positions must not hold back this one's pushes.
    SyncParked::clear();
    return freshFor(baseUrl, username, state.nextStampId);
  }
  return state;
}

bool SyncState::save() const {
  Rewrite rewrite;
  rewrite.header = this;
  return rewriteState(rewrite);
}

uint32_t SyncState::newStampId() {
  const uint32_t id = nextStampId ? nextStampId : randomStamp();
  nextStampId = id + 1 ? id + 1 : 1;  // 0 means "none"
  return id;
}

void SyncState::resetAfterPendingLoss() {
  // Whatever account the file belongs to: the pending list is shared, so a
  // loss while signed out must still be redone after signing back in.
  SyncState state;
  if (!readStored(state)) return;
  bool scopesPrepared = false;
  {
    SyncFiles::Reader reader(PATH);
    SyncState header;
    uint8_t version = 0;
    if (reader.open() != SyncFiles::Reader::Source::None && readHeader(reader, header, version) && version == VERSION) {
      SyncScope scope;
      while (!scopesPrepared && !reader.atEnd() && readScope(reader, scope))
        scopesPrepared = scope.prepared || scope.walkId;
    }
  }
  if (!state.anythingCompleted() && !state.statsCatchupId && !state.clippingsCatchupId && !scopesPrepared) return;
  LOG_ERR("SYNC", "Pending list lost; catch-ups and first walks will run again");
  state.statsCaughtUp = state.clippingsCaughtUp = false;
  state.statsCatchupId = state.clippingsCatchupId = 0;
  uint8_t which = 1 | 2 | 4;
  Rewrite rewrite;
  rewrite.header = &state;
  rewrite.edit = clearScopeBits;
  rewrite.context = &which;
  rewriteState(rewrite);
}

void SyncState::invalidateCatchup(const bool stats) {
  // Whatever account the file belongs to: with the username cleared ("signed
  // out") the current identity matches nothing, yet re-entering it later must
  // still find the catch-up undone.
  SyncState state;
  if (!readStored(state)) return;
  bool recorded =
      stats ? state.statsCaughtUp || state.statsCatchupId : state.clippingsCaughtUp || state.clippingsCatchupId;
  {
    SyncFiles::Reader reader(PATH);
    SyncState header;
    uint8_t version = 0;
    if (!recorded && reader.open() != SyncFiles::Reader::Source::None && readHeader(reader, header, version) &&
        version == VERSION) {
      SyncScope scope;
      while (!recorded && !reader.atEnd() && readScope(reader, scope))
        recorded =
            stats ? scope.statsCaughtUp || scope.statsCatchupId : scope.clippingsCaughtUp || scope.clippingsCatchupId;
    }
  }
  if (!recorded) return;
  LOG_INF("SYNC", "%s change not queued; catch-up will run again", stats ? "Stats" : "Clippings");
  if (stats) {
    state.statsCaughtUp = false;
    state.statsCatchupId = 0;
  } else {
    state.clippingsCaughtUp = false;
    state.clippingsCatchupId = 0;
  }
  uint8_t which = stats ? 1 : 2;
  Rewrite rewrite;
  rewrite.header = &state;
  rewrite.edit = clearScopeBits;
  rewrite.context = &which;
  rewriteState(rewrite);
}

void SyncState::restartCatchup(const bool stats) {
  SyncState state = load();
  if (stats) {
    state.statsCaughtUp = false;
    state.statsCatchupId = 0;
  } else {
    state.clippingsCaughtUp = false;
    state.clippingsCatchupId = 0;
  }
  uint8_t which = stats ? 1 : 2;
  Rewrite rewrite;
  rewrite.header = &state;
  rewrite.edit = clearScopeBits;
  rewrite.context = &which;
  rewriteState(rewrite);
}

void SyncState::resetForNewDocumentIds() {
  SyncParked::clear();  // keyed by the old ids
  SyncState state = load();
  state.statsCaughtUp = state.clippingsCaughtUp = false;
  state.statsCatchupId = state.clippingsCatchupId = 0;
  uint8_t which = 1 | 2 | 4;
  Rewrite rewrite;
  rewrite.header = &state;
  rewrite.edit = clearScopeBits;
  rewrite.context = &which;
  rewriteState(rewrite);
}

bool SyncState::findScope(const std::string& path, SyncScope& out) {
  bool found = false;
  forEachScope([&](const SyncScope& scope) {
    if (scope.path != path) return true;
    out = scope;
    found = true;
    return false;
  });
  return found;
}

bool SyncState::findPreparedCovering(const std::string& path, SyncScope& out) {
  bool found = false;
  forEachScope([&](const SyncScope& scope) {
    const bool candidate = scope.prepared && (scope.path == path || scope.covers(path));
    // The nearest (longest) prepared ancestor wins.
    if (candidate && (!found || scope.path.size() > out.path.size())) {
      out = scope;
      found = true;
    }
    return true;
  });
  return found;
}

bool SyncState::putScope(const SyncScope& scope) {
  if (scope.path.empty() || scope.path.size() > SyncScope::MAX_PATH_LENGTH) return false;
  // Count the other scopes and find the least recently synced one, in case
  // adding this one goes over the cap.
  size_t others = 0;
  std::string oldest;
  uint32_t oldestSynced = UINT32_MAX;
  forEachScope([&](const SyncScope& stored) {
    if (stored.path == scope.path) return true;
    ++others;
    // "/" is never dropped: every folder falls back to its cursor.
    if (stored.path != "/" && stored.lastSynced < oldestSynced) {
      oldestSynced = stored.lastSynced;
      oldest = stored.path;
    }
    return true;
  });
  struct Evict {
    const std::string* path;
  } evict{others >= MAX_SCOPES && !oldest.empty() ? &oldest : nullptr};
  if (evict.path) LOG_INF("SYNC", "Dropping least recently synced scope: %s", oldest.c_str());
  // Stamp it as the most recently synced, from the saved counter so the order
  // survives restarts.
  SyncState state = load();
  SyncScope updated = scope;
  updated.lastSynced = state.newStampId();
  Rewrite rewrite;
  rewrite.header = &state;
  rewrite.append = &updated;
  rewrite.edit = [](SyncScope& stored, void* context) {
    const auto* drop = static_cast<const Evict*>(context);
    return !(drop->path && stored.path == *drop->path);
  };
  rewrite.context = &evict;
  return rewriteState(rewrite);
}

bool SyncState::markScopesCaughtUp(const bool stats) {
  SyncState state = load();
  Rewrite rewrite;
  rewrite.header = &state;
  bool which = stats;
  rewrite.edit = [](SyncScope& scope, void* context) {
    if (!scope.prepared) return true;
    if (*static_cast<const bool*>(context)) {
      scope.statsCaughtUp = true;
      scope.statsCatchupId = 0;
    } else {
      scope.clippingsCaughtUp = true;
      scope.clippingsCatchupId = 0;
    }
    return true;
  };
  rewrite.context = &which;
  return rewriteState(rewrite);
}
