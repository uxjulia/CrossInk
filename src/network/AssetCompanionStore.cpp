#include "AssetCompanionStore.h"

#include <Arduino.h>
#include <AssetCompanionIndex.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <cstring>

#include "HttpDownloader.h"

namespace AssetCompanionStore {
namespace {
using AssetCompanionIndex::Reference;
using AssetDownloadService::Result;
constexpr size_t PATH_BYTES = 384;
constexpr uint32_t FILE_LIMIT = 2097152;
constexpr uint32_t TOTAL_LIMIT = 8388608;
constexpr size_t REFERENCE_LIMIT = 32;
constexpr size_t WALK_LIMIT = 8192;
constexpr size_t METADATA_RESERVE = 1048576;
static constexpr const char* INDEX_NAMES[] = {"notices.json", "sources.json"};
struct Workspace {
  uint8_t buffer[1024] = {};
  char path[PATH_BYTES] = {};
  char other[PATH_BYTES] = {};
  char url[AssetDownloadService::MAX_URL] = {};
  char root[160] = {};
  char hashes[2][65] = {};
  char digest[65] = {};
  char spoolHash[65] = {};
  mbedtls_sha256_context spoolContext;
  mbedtls_sha256_context indexContext;
  Reference reference{};
  Reference compared{};
  uint32_t indexBytes[2] = {};
  size_t rawCount = 0;
  size_t count = 0;
  uint64_t rawBytes = 0;
  uint64_t uniqueBytes = 0;
  uint64_t progressDone = 0;
  uint64_t progressTotal = 0;
  const AssetCatalog::Catalog* catalog = nullptr;
  const AssetCatalog::Entry* entry = nullptr;
  const AssetDownloadService::Endpoints* endpoints = nullptr;
  AssetDownloadService::Callbacks callbacks;
  Result result = Result::Ok;
  Workspace() {
    mbedtls_sha256_init(&spoolContext);
    mbedtls_sha256_init(&indexContext);
  }
  ~Workspace() {
    mbedtls_sha256_free(&spoolContext);
    mbedtls_sha256_free(&indexContext);
  }
};
static_assert(sizeof(Workspace) <= 4096, "Companion scratch exceeds 4 KiB");
Result fail(Result result) {
  LOG_ERR("SOURCE", "Companion transaction: %s", AssetDownloadService::resultName(result));
  return result;
}
bool hex(const char* value, size_t length) {
  if (strnlen(value, length + 1) != length) return false;
  for (size_t i = 0; i < length; ++i)
    if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
  return true;
}
bool stopped(Workspace& w) {
  if (w.callbacks.cancelled && w.callbacks.cancelled(w.callbacks.context)) {
    w.result = Result::Cancelled;
    return true;
  }
  return false;
}
bool stagePath(Workspace& w, const char* suffix) {
  const int n = std::snprintf(w.path, sizeof(w.path), "%s.part/%s", w.root, suffix);
  return n > 0 && size_t(n) < sizeof(w.path);
}
bool capacity(uint64_t required) {
  const uint64_t total = Storage.totalBytes();
  if (!total) return true;  // Unknown capacity: every write still checks its result.
  const uint64_t used = Storage.usedBytes();
  return used <= total && required <= total - used;
}
bool mkdirParent(Workspace& w) {
  std::strcpy(w.other, w.path);
  char* slash = std::strrchr(w.other, '/');
  if (!slash) return false;
  *slash = 0;
  return Storage.ensureDirectoryExists(w.other);
}
// Reopen at each step, keeping only one path and two short-lived handles. The
// path length and operation count bound hostile/interrupted local directory trees.
bool removeTree(Workspace& w, const char* root) {
  if (!Storage.exists(root)) return true;
  const size_t base = std::strlen(root);
  if (base >= sizeof(w.path)) return false;
  std::memmove(w.path, root, base + 1);
  for (size_t steps = 0; steps < WALK_LIMIT; ++steps) {
    if (stopped(w)) return false;
    auto dir = Storage.open(w.path);
    if (!dir || !dir.isDirectory()) {
      dir.close();
      return false;
    }
    auto child = dir.openNextFile();
    if (!child) {
      const bool ok = !dir.iterationFailed() && !dir.allocationFailed();
      const bool childClosed = child.close();
      const bool dirClosed = dir.close();
      if (!ok || !childClosed || !dirClosed || !Storage.rmdir(w.path)) return false;
      if (std::strlen(w.path) == base) return true;
      *std::strrchr(w.path, '/') = 0;
      continue;
    }
    const size_t length = child.getName(w.other, sizeof(w.other));
    const bool directory = child.isDirectory();
    const bool childClosed = child.close();
    const bool dirClosed = dir.close();
    if (!length || length >= sizeof(w.other) - 1 || !childClosed || !dirClosed || std::strchr(w.other, '/') ||
        std::strchr(w.other, '\\') || !std::strcmp(w.other, ".") || !std::strcmp(w.other, ".."))
      return false;
    const size_t current = std::strlen(w.path);
    if (current + length + 2 > sizeof(w.path)) return false;
    w.path[current] = '/';
    std::memcpy(w.path + current + 1, w.other, length + 1);
    if (!directory) {
      if (!Storage.remove(w.path)) return false;
      w.path[current] = 0;
    }
    delay(1);
  }
  return false;
}
bool hashFile(Workspace& w, const char* path, uint32_t expected, char* output) {
  if (stopped(w)) return false;
  FsFile file;
  if (!Storage.openFileForRead("SOURCE", path, file)) return false;
  const uint64_t bytes = file.fileSize64();
  if (bytes > FILE_LIMIT || (expected && bytes != expected)) {
    file.close();
    w.result = Result::Invalid;
    return false;
  }
  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  bool ok = mbedtls_sha256_starts(&hash, 0) == 0;
  uint64_t offset = 0;
  while (ok && offset < bytes && !stopped(w)) {
    const size_t count = bytes - offset < sizeof(w.buffer) ? size_t(bytes - offset) : sizeof(w.buffer);
    ok = file.read(w.buffer, count) == int(count) && mbedtls_sha256_update(&hash, w.buffer, count) == 0;
    offset += count;
    delay(1);
  }
  uint8_t digest[32];
  ok = ok && offset == bytes && mbedtls_sha256_finish(&hash, digest) == 0;
  mbedtls_sha256_free(&hash);
  const bool closed = file.close();
  if (!ok || !closed || stopped(w)) return false;
  static constexpr char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < 32; ++i) {
    output[i * 2] = digits[digest[i] >> 4];
    output[i * 2 + 1] = digits[digest[i] & 15];
  }
  output[64] = 0;
  return true;
}
bool validateFile(const std::string& path, void* context) {
  auto& w = *static_cast<Workspace*>(context);
  if (!hashFile(w, path.c_str(), w.reference.bytes, w.digest)) {
    if (w.result == Result::Ok) w.result = Result::Io;
    return false;
  }
  if (std::strcmp(w.digest, w.reference.sha256)) {
    w.result = Result::Invalid;
    return false;
  }
  return true;
}
bool download(Workspace& w, const char* relative, uint32_t expected = 0, bool verify = false) {
  if (stopped(w)) return false;
  if (!AssetDownloadService::buildUrl(*w.endpoints, relative, w.url, sizeof(w.url))) {
    w.result = Result::Unavailable;
    return false;
  }
  if (!capacity(expected ? expected : FILE_LIMIT) || !mkdirParent(w)) {
    w.result = Result::Io;
    return false;
  }
  HttpDownloader::DownloadOptions options(false, false, [&] { return stopped(w); }, 1024);
  options.stageAsPart = true;
  options.requireBackupCleanup = true;
  options.checkFreeSpace = true;
  options.maxBytes = FILE_LIMIT;
  options.expectedBytes = expected;
  options.maxRedirects = 2;
  options.allowUrl = AssetDownloadService::allowUrl;
  options.urlContext = const_cast<AssetDownloadService::Endpoints*>(w.endpoints);
  options.validateWithContext = verify ? validateFile : nullptr;
  options.validationContext = &w;
  const auto result = HttpDownloader::downloadToFile(
      w.url, w.path,
      [&](size_t done, size_t total) {
        if (w.callbacks.progress)
          w.callbacks.progress(w.callbacks.context, size_t(w.progressDone + done),
                               size_t(w.progressTotal ? w.progressTotal : w.progressDone + total));
      },
      nullptr, "", "", options);
  if (result == HttpDownloader::OK) return !stopped(w);
  if (w.result == Result::Ok)
    w.result = result == HttpDownloader::ABORTED ? Result::Cancelled
               : result == HttpDownloader::FILE_ERROR || result == HttpDownloader::INSUFFICIENT_SPACE ? Result::Io
                                                                                                      : Result::Network;
  return false;
}
bool downloadIndex(Workspace& w, size_t index) {
  if (stopped(w) || !stagePath(w, INDEX_NAMES[index])) return false;
  std::snprintf(w.other, sizeof(w.other), "snapshots/%s/%s", w.catalog->revision, INDEX_NAMES[index]);
  if (!AssetDownloadService::buildUrl(*w.endpoints, w.other, w.url, sizeof(w.url))) {
    w.result = Result::Unavailable;
    return false;
  }
  if (!capacity(FILE_LIMIT) || !mkdirParent(w)) {
    w.result = Result::Io;
    return false;
  }
  auto file = Storage.open(w.path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file || mbedtls_sha256_starts(&w.indexContext, 0)) {
    file.close();
    w.result = Result::Io;
    return false;
  }
  HttpDownloader::DownloadOptions options(false, false, [&] { return stopped(w); }, 1024);
  options.maxBytes = FILE_LIMIT;
  options.maxRedirects = 2;
  options.allowUrl = AssetDownloadService::allowUrl;
  options.urlContext = const_cast<AssetDownloadService::Endpoints*>(w.endpoints);
  const auto result = HttpDownloader::streamUrl(
      w.url,
      [&](const uint8_t* bytes, size_t count) {
        if (stopped(w)) return false;
        if (count > FILE_LIMIT - w.indexBytes[index]) {
          w.result = Result::Invalid;
          return false;
        }
        if (file.write(bytes, count) != count || mbedtls_sha256_update(&w.indexContext, bytes, count)) {
          w.result = Result::Io;
          return false;
        }
        w.indexBytes[index] += uint32_t(count);
        return true;
      },
      [&](size_t done, size_t total) {
        if (w.callbacks.progress)
          // Unknown-length (chunked) indexes report received bytes only.
          w.callbacks.progress(w.callbacks.context, size_t(w.progressDone + done),
                               total ? size_t(w.progressDone + total) : 0);
      },
      "", "", options);
  const bool synced = file.sync();
  const bool closed = file.close();
  if (result != HttpDownloader::OK || !synced || !closed || stopped(w)) {
    if (w.result == Result::Ok) w.result = !synced || !closed ? Result::Io : Result::Network;
    return false;
  }
  uint8_t digest[32];
  if (mbedtls_sha256_finish(&w.indexContext, digest)) {
    w.result = Result::Io;
    return false;
  }
  for (size_t i = 0; i < sizeof(digest); ++i) std::snprintf(w.hashes[index] + 2 * i, 3, "%02x", digest[i]);
  return true;
}
char lower(char value) { return value >= 'A' && value <= 'Z' ? char(value + 'a' - 'A') : value; }
bool collision(const char* a, const char* b) {
  bool spellingDiffers = false;
  while (*a && *b && lower(*a) == lower(*b)) {
    spellingDiffers = spellingDiffers || *a != *b;
    if (*a == '/' && spellingDiffers) return true;
    ++a;
    ++b;
  }
  return (!*a && (!*b || *b == '/')) || (!*b && *a == '/');
}
bool validReference(const Reference& reference, const char* revision) {
  const size_t size = strnlen(reference.path, sizeof(reference.path));
  if (size < 59 || size == sizeof(reference.path) || !hex(reference.sha256, 64) || !reference.bytes ||
      reference.bytes > FILE_LIMIT || std::strncmp(reference.path, "snapshots/", 10) ||
      std::strncmp(reference.path + 10, revision, 40) || std::strncmp(reference.path + 50, "/source/", 8))
    return false;
  const char* segment = reference.path;
  for (const char* p = segment;; ++p) {
    if (*p == '/' || !*p) {
      if (p == segment || p[-1] == '.') return false;
      if (!*p) return true;
      segment = p + 1;
    } else if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' ||
                 *p == '_' || *p == '.'))
      return false;
  }
}
bool readReference(Workspace& w, size_t index, Reference& reference) {
  if (stopped(w) || !stagePath(w, "references.bin")) return false;
  auto file = Storage.open(w.path);
  const bool ok =
      file && file.seek(index * sizeof(Reference)) && file.read(&reference, sizeof(reference)) == sizeof(reference);
  const bool closed = file.close();
  if (ok && closed && !validReference(reference, w.catalog->revision)) {
    w.result = Result::Invalid;
    return false;
  }
  return ok && closed;
}
bool recordReference(const Reference& reference, void* context) {
  auto& w = *static_cast<Workspace*>(context);
  if (stopped(w)) return false;
  if (++w.rawCount > REFERENCE_LIMIT || reference.bytes > FILE_LIMIT || (w.rawBytes += reference.bytes) > TOTAL_LIMIT) {
    w.result = Result::Invalid;
    return false;
  }
  for (size_t i = 0; i < w.count; ++i) {
    if (!readReference(w, i, w.compared)) {
      if (w.result == Result::Ok) w.result = Result::Io;
      return false;
    }
    if (!std::strcmp(reference.path, w.compared.path)) {
      if (reference.bytes == w.compared.bytes && !std::strcmp(reference.sha256, w.compared.sha256)) return true;
      w.result = Result::Collision;
      return false;
    }
    if (collision(reference.path, w.compared.path)) {
      w.result = Result::Collision;
      return false;
    }
  }
  if (!stagePath(w, "references.bin")) return false;
  auto file = Storage.open(w.path, O_WRONLY | O_CREAT | O_APPEND);
  const bool ok = file && file.write(&reference, sizeof(reference)) == sizeof(reference) && file.sync();
  const bool closed = file.close();
  if (!ok || !closed) {
    w.result = Result::Io;
    return false;
  }
  if (mbedtls_sha256_update(&w.spoolContext, reinterpret_cast<const uint8_t*>(&reference), sizeof(reference))) {
    w.result = Result::Io;
    return false;
  }
  ++w.count;
  w.uniqueBytes += reference.bytes;
  return true;
}
bool parseIndex(Workspace& w, size_t index) {
  if (!stagePath(w, INDEX_NAMES[index])) return false;
  auto file = Storage.open(w.path);
  if (!file) return false;
  if (file.fileSize64() != w.indexBytes[index]) {
    file.close();
    w.result = Result::Invalid;
    return false;
  }
  AssetCompanionIndex::Options options{w.catalog->kind, w.entry->code, w.catalog->revision, recordReference, &w};
  // The bounded parser is temporary; it cannot share the chunk buffer because
  // reference callbacks use that buffer's sibling spool scratch synchronously.
  auto parser = makeUniqueNoThrow<AssetCompanionIndex::Parser>(options);
  if (!parser) {
    file.close();
    w.result = Result::Memory;
    return false;
  }
  bool ok = w.indexBytes[index] <= FILE_LIMIT;
  while (ok && parser->receivedBytes() < w.indexBytes[index] && !stopped(w)) {
    const size_t left = w.indexBytes[index] - parser->receivedBytes();
    const size_t count = left < sizeof(w.buffer) ? left : sizeof(w.buffer);
    ok = file.read(w.buffer, count) == int(count) && parser->feed(reinterpret_cast<char*>(w.buffer), count);
    delay(1);
  }
  ok = ok && !stopped(w) && parser->finish();
  const bool closed = file.close();
  if (!ok || !closed) {
    LOG_ERR("SOURCE", "Index rejected: %s", parser->error() ? parser->error() : "SD read interrupted");
    if (w.result == Result::Ok) w.result = ok ? Result::Io : Result::Invalid;
    return false;
  }
  if (!stagePath(w, INDEX_NAMES[index])) return false;
  if (!hashFile(w, w.path, w.indexBytes[index], w.digest)) return false;
  if (std::strcmp(w.digest, w.hashes[index])) {
    w.result = Result::Invalid;
    return false;
  }
  return true;
}
bool verifySpool(Workspace& w) {
  return stagePath(w, "references.bin") && hashFile(w, w.path, uint32_t(w.count * sizeof(Reference)), w.digest) &&
         std::strcmp(w.digest, w.spoolHash) == 0;
}
bool verifyAll(Workspace& w) {
  if (!verifySpool(w)) return false;
  for (size_t i = 0; i < 2; ++i) {
    if (!stagePath(w, INDEX_NAMES[i]) || !hashFile(w, w.path, w.indexBytes[i], w.digest) ||
        std::strcmp(w.digest, w.hashes[i]))
      return false;
  }
  for (size_t i = 0; i < w.count; ++i) {
    if (!readReference(w, i, w.reference)) return false;
    const char* suffix = w.reference.path + 10 + std::strlen(w.catalog->revision) + 1;
    if (!stagePath(w, suffix) || !hashFile(w, w.path, w.reference.bytes, w.digest) ||
        std::strcmp(w.digest, w.reference.sha256))
      return false;
  }
  return true;
}
bool complete(Workspace& w) {
  if (!verifySpool(w)) return false;
  const int size = std::snprintf(
      reinterpret_cast<char*>(w.buffer), sizeof(w.buffer),
      "CrossInk recipient companions "
      "1\nrevision=%s\nkind=%s\ncode=%s\npayload=%s\nbytes=%lu\nnotices=%lu:%s\nsources=%lu:%s\nreferences=%u:%s\n",
      w.catalog->revision, w.catalog->kind == AssetCatalog::Kind::Hyphenation ? "hyphenation" : "translation",
      w.entry->code, w.entry->sha256, static_cast<unsigned long>(w.entry->bytes),
      static_cast<unsigned long>(w.indexBytes[0]), w.hashes[0], static_cast<unsigned long>(w.indexBytes[1]),
      w.hashes[1], unsigned(w.count), w.digest);
  if (size <= 0 || size_t(size) >= sizeof(w.buffer) || stopped(w) || !stagePath(w, "complete.txt")) return false;
  auto file = Storage.open(w.path, O_WRONLY | O_CREAT | O_TRUNC);
  bool ok = file && file.write(w.buffer, size_t(size)) == size_t(size) && file.sync();
  const bool closed = file.close();
  if (!ok || !closed) return false;
  // Compare the exact marker read back without allocating a second buffer.
  file = Storage.open(w.path);
  ok = file && file.fileSize64() == uint64_t(size);
  for (int i = 0; ok && i < size; ++i) {
    uint8_t byte = 0;
    ok = !stopped(w) && file.read(&byte, 1) == 1 && byte == w.buffer[i];
  }
  const bool readClosed = file.close();
  return ok && readClosed;
}
}  // namespace
Result prepare(const AssetCatalog::Catalog& catalog, const AssetCatalog::Entry& entry,
               const AssetDownloadService::Endpoints& endpoints, const AssetDownloadService::Callbacks& callbacks) {
  // One bounded cold-operation workspace, not task-stack storage. Never contains
  // an archive/index body or a reference array; all are streamed/spooled to SD.
  auto work = makeUniqueNoThrow<Workspace>();
  if (!work) return fail(Result::Memory);
  auto& w = *work;
  w.catalog = &catalog;
  w.entry = &entry;
  w.endpoints = &endpoints;
  w.callbacks = callbacks;
  if (stopped(w)) return fail(Result::Cancelled);
  if (mbedtls_sha256_starts(&w.spoolContext, 0)) return fail(Result::Io);
  const int length =
      std::snprintf(w.root, sizeof(w.root), "/.crosspoint/asset-sources/%s/%s", entry.sha256, catalog.revision);
  if (length <= 0 || size_t(length) + 5 >= sizeof(w.root) || !hex(entry.sha256, 64) || !hex(catalog.revision, 40))
    return fail(Result::Invalid);
  // Restore a previous committed generation before discarding interrupted work.
  std::snprintf(w.other, sizeof(w.other), "%s.old", w.root);
  if (Storage.exists(w.other) && !Storage.exists(w.root) && !Storage.rename(w.other, w.root)) return fail(Result::Io);
  std::snprintf(w.other, sizeof(w.other), "%s.part", w.root);
  if (!removeTree(w, w.other)) return fail(w.result == Result::Ok ? Result::Io : w.result);
  std::snprintf(w.other, sizeof(w.other), "%s.part", w.root);
  if (!Storage.ensureDirectoryExists(w.other)) return fail(Result::Io);
  // Unknown-length indexes reserve their full cap before starting either request.
  if (!capacity(2 * uint64_t(FILE_LIMIT) + entry.bytes + METADATA_RESERVE)) return fail(Result::Io);
  for (size_t i = 0; i < 2; ++i) {
    if (!downloadIndex(w, i) || !parseIndex(w, i)) return fail(w.result == Result::Ok ? Result::Io : w.result);
    w.progressDone += w.indexBytes[i];
  }
  uint8_t spoolDigest[32];
  if (mbedtls_sha256_finish(&w.spoolContext, spoolDigest)) return fail(Result::Io);
  for (size_t i = 0; i < sizeof(spoolDigest); ++i) std::snprintf(w.spoolHash + 2 * i, 3, "%02x", spoolDigest[i]);
  if (!w.count || !verifySpool(w) || !capacity(w.uniqueBytes + entry.bytes + METADATA_RESERVE))
    return fail(w.result == Result::Ok ? Result::Io : w.result);
  w.progressTotal = w.progressDone + w.uniqueBytes;
  for (size_t i = 0; i < w.count; ++i) {
    if (!readReference(w, i, w.reference)) return fail(w.result == Result::Ok ? Result::Io : w.result);
    const char* suffix = w.reference.path + 10 + std::strlen(catalog.revision) + 1;
    if (!stagePath(w, suffix) || !download(w, w.reference.path, w.reference.bytes, true))
      return fail(w.result == Result::Ok ? Result::Io : w.result);
    w.progressDone += w.reference.bytes;
  }
  if (callbacks.verifying) callbacks.verifying(callbacks.context);
  if (!verifyAll(w) || !complete(w) || stopped(w)) return fail(w.result == Result::Ok ? Result::Io : w.result);
  // Never trust an editable local marker as authentication: all preceding bytes
  // came from this fresh HTTPS closure. Old directories remain until it is ready.
  std::snprintf(w.other, sizeof(w.other), "%s.old", w.root);
  if (!removeTree(w, w.other)) return fail(w.result == Result::Ok ? Result::Io : w.result);
  std::snprintf(w.other, sizeof(w.other), "%s.old", w.root);
  const bool replacing = Storage.exists(w.root);
  if (replacing && !Storage.rename(w.root, w.other)) return fail(Result::Io);
  std::snprintf(w.path, sizeof(w.path), "%s.part", w.root);
  if (!Storage.rename(w.path, w.root)) {
    if (replacing && !Storage.rename(w.other, w.root))
      LOG_ERR("SOURCE", "Previous companion generation retained in .old");
    return fail(Result::Io);
  }
  if (!removeTree(w, w.other)) return fail(w.result == Result::Ok ? Result::Io : w.result);
  return Result::Ok;
}
}  // namespace AssetCompanionStore
