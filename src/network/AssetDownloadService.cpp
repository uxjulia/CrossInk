#include "AssetDownloadService.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LanguageCache.h>
#include <Logging.h>
#include <Memory.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>

#include "AssetCompanionStore.h"
#include "DownloadFileSwap.h"
#include "HttpDownloader.h"
#include "HyphenationPackStore.h"

namespace AssetDownloadService {
namespace {
constexpr size_t MAX_PAYLOAD = 262144;
constexpr size_t MAX_DIRECTORY_ENTRIES = 256;
constexpr size_t MAX_SOURCES = 64;
std::atomic_flag transferring = ATOMIC_FLAG_INIT;
struct TransferGuard {
  ~TransferGuard() { transferring.clear(); }
};
// One cold-operation allocation keeps scratch and metadata off the small task
// stack. Reused for discovery, hashing and validation; never stores a payload.
struct Workspace {
  uint8_t buffer[1024];
  char filename[MAX_PATH];
  char candidate[MAX_PATH];
  char canonical[MAX_PATH];
  char relative[MAX_PATH];
  char url[MAX_URL];
  Source source;
  Source original;
  language_cache::Metadata metadata;
  std::optional<language_cache::Inspector> inspector;
  const AssetCatalog::Catalog* catalog = nullptr;
  const AssetCatalog::Entry* entry = nullptr;
  Callbacks callbacks;
  Result validationResult = Result::Invalid;
  uint32_t lastPoll = 0;
  size_t discoverableSources = 0;
};
static_assert(sizeof(Workspace) <= 3072, "Asset operation scratch budget exceeded");
Result failure(Result result) {
  LOG_ERR("ASSET", "%s", resultName(result));
  return result;
}
bool cancelled(const Callbacks& callbacks) { return callbacks.cancelled && callbacks.cancelled(callbacks.context); }
bool continueWork(void* context) {
  auto& work = *static_cast<Workspace*>(context);
  const uint32_t now = millis();
  if (now - work.lastPoll < 20) return true;
  work.lastPoll = now;
  delay(1);
  return !cancelled(work.callbacks);
}
char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 'a' - 'A') : c; }
bool equalFold(const char* a, const char* b) {
  while (*a && *b && lower(*a) == lower(*b)) {
    ++a;
    ++b;
  }
  return *a == *b;
}
bool endsFold(const char* value, const char* suffix) {
  const size_t a = std::strlen(value), b = std::strlen(suffix);
  return a >= b && equalFold(value + a - b, suffix);
}
bool safeName(const char* value) {
  return *value && !std::strchr(value, '/') && !std::strchr(value, '\\') && !std::strstr(value, "..");
}
bool canonicalPath(AssetCatalog::Kind kind, const char* code, char* path, size_t size) {
  if (!language_cache::validCode(code)) return false;
  const int n = kind == AssetCatalog::Kind::Translation
                    ? std::snprintf(path, size, "%s/%s.yaml", language_cache::DIRECTORY, code)
                    : std::snprintf(path, size, "%s/hyph-%s.cphyph", HyphenationPackStore::DIRECTORY, code);
  return n > 0 && size_t(n) < size;
}
int readInput(void* context, void* bytes, size_t count) { return static_cast<FsFile*>(context)->read(bytes, count); }
Result hashFile(const char* path, char* hex, Workspace& work, size_t expected = 0) {
  FsFile file;
  if (!Storage.openFileForRead("ASSET", path, file)) return failure(Result::Io);
  const auto size = file.fileSize64();
  if (size > MAX_PAYLOAD || (expected && size != expected)) {
    LOG_ERR("ASSET", "SD size check failed: path=%s bytes=%llu expected=%u limit=%u", path,
            static_cast<unsigned long long>(size), unsigned(expected), unsigned(MAX_PAYLOAD));
    file.close();
    return failure(Result::Invalid);
  }
  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  bool ok = mbedtls_sha256_starts(&hash, 0) == 0;
  size_t offset = 0;
  while (ok && offset < size && !cancelled(work.callbacks)) {
    const size_t count = size - offset < sizeof(work.buffer) ? size - offset : sizeof(work.buffer);
    ok = file.read(work.buffer, count) == int(count) && mbedtls_sha256_update(&hash, work.buffer, count) == 0;
    offset += count;
    delay(1);
  }
  uint8_t digest[32];
  ok = ok && offset == size && mbedtls_sha256_finish(&hash, digest) == 0;
  mbedtls_sha256_free(&hash);
  const bool closed = file.close();
  if (cancelled(work.callbacks)) return failure(Result::Cancelled);
  if (!ok || !closed) return failure(Result::Io);
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(digest); ++i) {
    hex[2 * i] = HEX_DIGITS[digest[i] >> 4];
    hex[2 * i + 1] = HEX_DIGITS[digest[i] & 15];
  }
  hex[64] = 0;
  return Result::Ok;
}
Result scan(AssetCatalog::Kind kind, const AssetCatalog::Entry& entry, Source& source, Workspace& work) {
  source = {};
  work.discoverableSources = 0;
  if (!canonicalPath(kind, entry.code, work.canonical, sizeof(work.canonical))) {
    LOG_ERR("ASSET", "SD check failed: invalid destination code=%s", entry.code);
    return failure(Result::Invalid);
  }
  std::strcpy(source.path, work.canonical);
  const char* directoryPath =
      kind == AssetCatalog::Kind::Translation ? language_cache::DIRECTORY : HyphenationPackStore::DIRECTORY;
  LOG_DBG("ASSET", "SD check start: code=%s dir=%s target=%s", entry.code, directoryPath, work.canonical);
  if (!Storage.exists(directoryPath)) {
    LOG_DBG("ASSET", "SD check complete: code=%s directory missing; new download", entry.code);
    return Result::Ok;
  }
  auto directory = Storage.open(directoryPath);
  if (!directory || !directory.isDirectory()) {
    LOG_ERR("ASSET", "SD check failed: cannot open directory=%s code=%s", directoryPath, entry.code);
    directory.close();
    return failure(Result::Io);
  }
  if (kind == AssetCatalog::Kind::Translation && !work.inspector) work.inspector.emplace();
  if (work.inspector && !work.inspector->available()) {
    LOG_ERR("ASSET", "SD check failed: metadata workspace allocation code=%s", entry.code);
    directory.close();
    return failure(Result::Memory);
  }
  Result result = Result::Ok;
  size_t found = 0, sources = 0, count = 0;
  while (result == Result::Ok) {
    if (cancelled(work.callbacks)) {
      result = Result::Cancelled;
      break;
    }
    auto file = directory.openNextFile();
    if (!file) {
      if (directory.allocationFailed())
        result = Result::Memory;
      else if (directory.iterationFailed())
        result = Result::Io;
      if (result != Result::Ok)
        LOG_ERR("ASSET", "SD check failed: directory iteration dir=%s code=%s result=%s", directoryPath, entry.code,
                resultName(result));
      file.close();
      break;
    }
    if (++count > MAX_DIRECTORY_ENTRIES) {
      LOG_ERR("ASSET", "SD check failed: directory entry limit dir=%s count=%u limit=%u", directoryPath,
              unsigned(count), unsigned(MAX_DIRECTORY_ENTRIES));
      file.close();
      result = Result::Invalid;
      break;
    }
    work.filename[0] = '\0';
    const size_t length = file.getName(work.filename, sizeof(work.filename));
    work.filename[sizeof(work.filename) - 1] = '\0';
    const bool metadataFile = language_cache::isAppleDoubleFile(work.filename);
    if (!length || length >= sizeof(work.filename) - 1 || (!metadataFile && !safeName(work.filename))) {
      LOG_ERR("ASSET", "SD filename rejected: name=%s", work.filename);
      LOG_ERR("ASSET", "SD filename reason=%s length=%u dir=%s",
              !length                               ? "unreadable filename"
              : length >= sizeof(work.filename) - 1 ? "filename too long"
                                                    : "unsafe filename",
              unsigned(length), directoryPath);
      file.close();
      result = Result::Invalid;
      break;
    }
    if (metadataFile) {
      LOG_DBG("ASSET", "Skipping macOS metadata: path=%s/%s", directoryPath, work.filename);
      file.close();
      continue;
    }
    LOG_DBG("ASSET", "SD entry: path=%s/%s bytes=%llu position=%llu directory=%u", directoryPath, work.filename,
            file.isDirectory() ? 0ULL : static_cast<unsigned long long>(file.fileSize64()),
            file.isDirectory() ? 0ULL : static_cast<unsigned long long>(file.position()), unsigned(file.isDirectory()));
    // Match I18n::discover exactly for the publication source-count preflight:
    // directories, long names, sidecars and uppercase extensions are excluded.
    if (kind == AssetCatalog::Kind::Translation && !file.isDirectory() && length >= 6 && length < 127 &&
        std::strcmp(work.filename + length - 5, ".yaml") == 0)
      ++work.discoverableSources;
    // .old is visible only to transaction recovery, never an installable source.
    // Include an orphan .old under its logical destination so manual filenames
    // remain recoverable after power loss between the two renames.
    const bool backup = endsFold(work.filename, ".old");
    if (backup) work.filename[length - 4] = 0;
    const char* extension = kind == AssetCatalog::Kind::Translation ? ".yaml" : ".cphyph";
    if (!endsFold(work.filename, extension)) {
      file.close();
      continue;
    }
    const int n = std::snprintf(work.candidate, sizeof(work.candidate), "%s/%s", directoryPath, work.filename);
    if (n <= 0 || size_t(n) + 4 >= sizeof(work.candidate)) {
      LOG_ERR("ASSET", "SD check failed: path too long dir=%s file=%s", directoryPath, work.filename);
      file.close();
      result = Result::Invalid;
      break;
    }
    if (backup && Storage.exists(work.candidate)) {
      file.close();
      continue;
    }
    ++sources;
    if ((kind == AssetCatalog::Kind::Translation && work.discoverableSources > I18n::Catalog::MAX_SOURCE_FILES) ||
        (kind == AssetCatalog::Kind::Hyphenation && sources > MAX_SOURCES)) {
      LOG_ERR("ASSET", "SD check failed: source count limit dir=%s count=%u", directoryPath,
              unsigned(kind == AssetCatalog::Kind::Translation ? work.discoverableSources : sources));
      file.close();
      result = Result::Invalid;
      break;
    }
    const bool canonical = equalFold(work.candidate, work.canonical);
    bool sameCode = false;
    if (file.isDirectory()) {
      if (canonical) {
        LOG_ERR("ASSET", "SD check failed: destination is a directory path=%s", work.candidate);
        result = Result::Collision;
      }
    } else if (kind == AssetCatalog::Kind::Translation) {
      language_cache::InspectionDiagnostic diagnostic;
      const auto bytes = file.fileSize64();
      const auto inspected = bytes <= MAX_PAYLOAD
                                 ? work.inspector->inspect({&file, readInput}, work.metadata, &diagnostic)
                                 : language_cache::Result::TooLarge;
      if (inspected != language_cache::Result::Ok) {
        LOG_ERR("ASSET", "SD metadata rejected: path=%s%s", work.candidate, backup ? ".old" : "");
        LOG_ERR("ASSET", "SD metadata details: selected=%s bytes=%llu canonical=%u result=%s", entry.code,
                static_cast<unsigned long long>(bytes), unsigned(canonical), language_cache::resultName(inspected));
        LOG_ERR("ASSET", "SD metadata reason=%s line=%u offset=%u",
                bytes > MAX_PAYLOAD ? "source exceeds byte limit"
                : diagnostic.reason ? diagnostic.reason
                                    : "unknown",
                unsigned(diagnostic.line), unsigned(diagnostic.offset));
      } else
        LOG_DBG("ASSET", "SD metadata accepted: path=%s%s code=%s selected=%s", work.candidate, backup ? ".old" : "",
                work.metadata.code, entry.code);
      // Unknown identity cannot safely coexist with a new same-code source.
      if (inspected == language_cache::Result::Io)
        result = Result::Io;
      else if (inspected == language_cache::Result::Memory)
        result = Result::Memory;
      else if (inspected != language_cache::Result::Ok && !canonical)
        result = Result::Invalid;
      sameCode = inspected == language_cache::Result::Ok && equalFold(work.metadata.code, entry.code);
      if ((sameCode && std::strcmp(work.metadata.code, entry.code)) ||
          (canonical && inspected == language_cache::Result::Ok && !sameCode)) {
        LOG_ERR("ASSET", "SD check failed: language identity collision path=%s file_code=%s selected=%s",
                work.candidate, work.metadata.code, entry.code);
        result = Result::Collision;
      }
    } else {
      // CPHY identity bytes are read without installing or allocating a trie.
      char header[7];
      if (file.read(header, sizeof(header)) == sizeof(header) && !std::memcmp(header, "CPHY", 4)) {
        sameCode = std::strlen(entry.code) == 2 && lower(header[5]) == lower(entry.code[0]) &&
                   lower(header[6]) == lower(entry.code[1]);
      }
    }
    if (!file.close()) {
      LOG_ERR("ASSET", "SD check failed: cannot close path=%s%s", work.candidate, backup ? ".old" : "");
      result = Result::Io;
    }
    if (result != Result::Ok) break;
    if (!canonical && !sameCode) continue;
    if ((canonical && std::strcmp(work.candidate, work.canonical)) ||
        (kind == AssetCatalog::Kind::Hyphenation && !canonical) || ++found > 1) {
      LOG_ERR("ASSET", "SD check failed: source collision path=%s target=%s matches=%u", work.candidate, work.canonical,
              unsigned(found));
      result = Result::Collision;
      break;
    }
    std::strcpy(source.path, work.candidate);
  }
  if (!directory.close()) {
    LOG_ERR("ASSET", "SD check failed: cannot close directory=%s", directoryPath);
    result = Result::Io;
  }
  if (result != Result::Ok) {
    if (result == Result::Collision) source.status = Status::Collision;
    return failure(result);
  }
  if (!found) {
    LOG_DBG("ASSET", "SD check complete: code=%s entries=%u destination missing; new download", entry.code,
            unsigned(count));
    return Result::Ok;
  }
  const char* path = source.path;
  // Derive this from the selected logical path, including manual filenames.
  // Discovery remains read-only even when the published bytes are current.
  std::snprintf(work.candidate, sizeof(work.candidate), "%s.old", path);
  source.recoveryPending = Storage.exists(work.candidate);
  const bool recoveryNeeded = !Storage.exists(path);
  if (recoveryNeeded) {
    path = work.candidate;
  }
  result = hashFile(path, source.sha256, work);
  if (result != Result::Ok) return result;
  // A recovery sidecar is not an installable source. Keep Download enabled so
  // its transaction recovery can restore the logical path before retrying.
  source.status = recoveryNeeded                                                   ? Status::Missing
                  : !*entry.sha256                                                 ? Status::FileOnSd
                  : !std::strcmp(entry.sha256, source.sha256)                      ? Status::Current
                  : *entry.previous && !std::strcmp(entry.previous, source.sha256) ? Status::Previous
                                                                                   : Status::Different;
  return Result::Ok;
}
Result sourceCountCapacity(const Workspace& work) {
  if (work.catalog->kind != AssetCatalog::Kind::Translation) return Result::Ok;
  const char* filename = std::strrchr(work.source.path, '/');
  if (!filename) return failure(Result::Invalid);
  ++filename;
  const size_t length = std::strlen(filename);
  // Do not publish into a manual filename that the installer cannot discover.
  if (length < 6 || length >= 127 || std::strcmp(filename + length - 5, ".yaml")) return failure(Result::Invalid);
  const size_t projected = work.discoverableSources + (Storage.exists(work.source.path) ? 0 : 1);
  return projected <= I18n::Catalog::MAX_SOURCE_FILES ? Result::Ok : failure(Result::Invalid);
}
Result translationCapacity(const std::string& partial, Workspace& work) {
  if (work.catalog->kind != AssetCatalog::Kind::Translation) return Result::Ok;
  FsFile file;
  if (!Storage.openFileForRead("ASSET", partial.c_str(), file)) return failure(Result::Io);
  // Full validation already succeeded; read the actual payload's display name,
  // not the catalog label, for the installer's text-arena projection.
  if (!work.inspector) work.inspector.emplace();
  const auto inspected = work.inspector->inspect({&file, readInput}, work.metadata);
  const bool closed = file.close();
  if (!closed || inspected == language_cache::Result::Io) return failure(Result::Io);
  if (inspected != language_cache::Result::Ok)
    return failure(inspected == language_cache::Result::Memory ? Result::Memory : Result::Invalid);
  // Reuse the installer's exact built-in/cached/duplicate accounting. This
  // temporary <3KiB catalog cannot live on the task stack. Release our metadata
  // parser first so discovery's Inspector replaces, rather than doubles, it.
  work.inspector.reset();
  auto projected = makeUniqueNoThrow<I18n::Catalog>();
  if (!projected) return failure(Result::Memory);
  const I18n::SourceProjection projection{work.source.path, &work.metadata};
  const auto result = I18N.discover(*projected, &work, continueWork, &projection);
  if (cancelled(work.callbacks)) return failure(Result::Cancelled);
  if (result == language_cache::Result::Ok) return Result::Ok;
  return failure(result == language_cache::Result::Memory      ? Result::Memory
                 : result == language_cache::Result::Io        ? Result::Io
                 : result == language_cache::Result::Cancelled ? Result::Cancelled
                                                               : Result::Invalid);
}
bool validate(const std::string& path, void* context) {
  auto& work = *static_cast<Workspace*>(context);
  if (work.callbacks.verifying) work.callbacks.verifying(work.callbacks.context);
  work.validationResult = hashFile(path.c_str(), work.source.sha256, work, work.entry->bytes);
  if (work.validationResult != Result::Ok) return false;
  if (std::strcmp(work.source.sha256, work.entry->sha256)) {
    work.validationResult = failure(Result::Invalid);
    return false;
  }
  // hashFile closes before either full-format reader reopens this same file.
  const bool valid = work.catalog->kind == AssetCatalog::Kind::Translation
                         ? I18N.validateSource(path.c_str(), work.entry->code, work.catalog->source,
                                               work.catalog->keyset, &work, continueWork) == language_cache::Result::Ok
                         : HyphenationPackStore::validateSource(path.c_str(), work.entry->code, &work, continueWork) ==
                               hyphenation_pack::Result::Ok;
  work.validationResult = valid ? Result::Ok : cancelled(work.callbacks) ? Result::Cancelled : Result::Invalid;
  if (!valid) {
    failure(work.validationResult);
    return false;
  }
  work.validationResult = translationCapacity(path, work);
  if (work.validationResult != Result::Ok) return false;
  // Recheck identity and bytes immediately before publication. A concurrent SD
  // writer must not silently turn an approved replacement into data loss.
  work.validationResult = scan(work.catalog->kind, *work.entry, work.source, work);
  if (work.validationResult != Result::Ok) return false;
  if (std::strcmp(work.source.path, work.original.path) || std::strcmp(work.source.sha256, work.original.sha256)) {
    work.validationResult = failure(Result::Collision);
    return false;
  }
  work.validationResult = sourceCountCapacity(work);
  if (work.validationResult != Result::Ok) return false;
  return true;
}
bool cleanHttps(std::string_view value) {
  if (value.size() < 10 || value.size() >= MAX_URL || value.substr(0, 8) != "https://") return false;
  const size_t slash = value.find('/', 8);
  if (slash == std::string_view::npos || slash == 8) return false;
  if (std::any_of(value.begin(), value.end(), [](char c) {
        return c <= 32 || static_cast<unsigned char>(c) >= 127 || c == '@' || c == '%' || c == '?' || c == '#' ||
               c == '\\';
      }))
    return false;
  if (value.substr(8, slash - 8).find(':') != std::string_view::npos) return false;
  const auto path = value.substr(slash);
  return path.find("//") == std::string_view::npos && path.find("/../") == std::string_view::npos &&
         path.find("/./") == std::string_view::npos && path.substr(path.size() > 3 ? path.size() - 3 : 0) != "/.." &&
         path.substr(path.size() > 2 ? path.size() - 2 : 0) != "/.";
}
bool matches(std::string_view value, const char* prefix) {
  if (!prefix) return false;
  const std::string_view allowed(prefix);
  return cleanHttps(allowed) && allowed.back() == '/' && value.size() > allowed.size() &&
         value.substr(0, allowed.size()) == allowed;
}
}  // namespace
// The downloader callback contract requires a mutable opaque context.
// cppcheck-suppress constParameterCallback
bool allowUrl(std::string_view url, void* context) {
  if (!context || !cleanHttps(url)) return false;
  const auto& endpoints = *static_cast<const Endpoints*>(context);
  return matches(url, endpoints.base) || matches(url, endpoints.redirectBase);
}
bool buildUrl(const Endpoints& endpoints, const char* relative, char* out, size_t capacity) {
  if (!relative || !endpoints.base || !out || !capacity) return false;
  const int n = std::snprintf(out, capacity, "%s%s", endpoints.base, relative);
  return n > 0 && size_t(n) < capacity && allowUrl(out, const_cast<Endpoints*>(&endpoints));
}
Result discover(AssetCatalog::Kind kind, const AssetCatalog::Entry& entry, Source& source, const Callbacks& callbacks) {
  auto work = makeUniqueNoThrow<Workspace>();
  if (!work) return failure(Result::Memory);
  work->callbacks = callbacks;
  return scan(kind, entry, source, *work);
}
Result download(const AssetCatalog::Catalog& catalog, const AssetCatalog::Entry& entry, const Endpoints& endpoints,
                bool overwriteApproved, const Callbacks& callbacks) {
  if (transferring.test_and_set()) return failure(Result::Unavailable);
  TransferGuard guard;
  auto work = makeUniqueNoThrow<Workspace>();
  if (!work) return failure(Result::Memory);
  work->callbacks = callbacks;
  work->catalog = &catalog;
  work->entry = &entry;
  if (!entry.bytes || entry.bytes > MAX_PAYLOAD ||
      !AssetCatalog::immutableAssetPath(catalog, entry, work->relative, sizeof(work->relative)) ||
      !buildUrl(endpoints, work->relative, work->url, sizeof(work->url)))
    return failure(Result::Unavailable);
  auto result = scan(catalog.kind, entry, work->source, *work);
  if (result != Result::Ok) return result;
  if (!DownloadFileSwap::recover(work->source.path)) return failure(Result::Io);
  result = scan(catalog.kind, entry, work->source, *work);
  if (result != Result::Ok) return result;
  result = sourceCountCapacity(*work);
  if (result != Result::Ok) return result;
  if (work->source.status == Status::Different && !overwriteApproved) return Result::OverwriteRequired;
  work->original = work->source;
  const char* directory =
      catalog.kind == AssetCatalog::Kind::Translation ? language_cache::DIRECTORY : HyphenationPackStore::DIRECTORY;
  if (!Storage.ensureDirectoryExists(directory)) return failure(Result::Io);
  // Recipient sources/notices must be completely verified on SD before the
  // installable source can be published. Release the discovery parser before
  // the companion workspace is allocated; no whole archive is held in memory.
  work->inspector.reset();
  result = AssetCompanionStore::prepare(catalog, entry, endpoints, callbacks);
  if (result != Result::Ok) return failure(result);
  if (callbacks.payloadStarted) callbacks.payloadStarted(callbacks.context);
  HttpDownloader::DownloadOptions options(false, false, [&] { return cancelled(callbacks); }, 1024);
  options.stageAsPart = true;
  options.requireBackupCleanup = true;
  options.checkFreeSpace = true;
  options.maxBytes = MAX_PAYLOAD;
  options.expectedBytes = entry.bytes;
  options.maxRedirects = 2;
  options.allowUrl = allowUrl;
  options.urlContext = const_cast<Endpoints*>(&endpoints);
  options.validateWithContext = validate;
  options.validationContext = work.get();
  work->validationResult = Result::Ok;
  const auto downloaded = HttpDownloader::downloadToFile(
      work->url, work->source.path,
      [&](size_t done, size_t total) {
        if (callbacks.progress) callbacks.progress(callbacks.context, done, total);
      },
      nullptr, "", "", options);
  if (work->validationResult != Result::Ok) return work->validationResult;
  if (downloaded == HttpDownloader::OK) return Result::Ok;
  return failure(downloaded == HttpDownloader::ABORTED      ? Result::Cancelled
                 : downloaded == HttpDownloader::FILE_ERROR ? Result::Io
                                                            : Result::Network);
}
const char* resultName(Result result) {
  switch (result) {
    case Result::Ok:
      return "ok";
    case Result::Io:
      return "SD error";
    case Result::Memory:
      return "out of memory";
    case Result::Cancelled:
      return "cancelled";
    case Result::Invalid:
      return "invalid source";
    case Result::Collision:
      return "source collision";
    case Result::OverwriteRequired:
      return "overwrite approval required";
    case Result::Unavailable:
      return "official endpoint unavailable";
    case Result::Network:
      return "download failed";
  }
  return "unknown error";
}
}  // namespace AssetDownloadService
