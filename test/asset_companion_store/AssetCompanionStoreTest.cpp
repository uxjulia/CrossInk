#include <AssetCompanionStore.h>
#include <DownloadFileSwap.h>
#include <HttpDownloader.h>
#include <mbedtls/sha256.h>

#include <cassert>
#include <functional>
#include <iostream>
using AssetDownloadService::Result;
std::map<std::string, std::string> responses;
std::function<void(const std::string&)> beforeDownload, afterDownload;
int downloads = 0;
bool cancel = false;
HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& dest,
                                                             ProgressCallback progress, bool*, const std::string&,
                                                             const std::string&, DownloadOptions options) {
  ++downloads;
  assert(options.transport == Transport::ESP_HTTP && options.bufferSize == 1024 && options.maxBytes == 2097152);
  assert(options.maxRedirects == 2 && options.checkFreeSpace && options.stageAsPart && options.requireBackupCleanup);
  assert(options.allowUrl(url, options.urlContext));
  if (beforeDownload) beforeDownload(url);
  if (options.shouldCancel && options.shouldCancel()) return ABORTED;
  if (!responses.count(url)) return HTTP_ERROR;
  const auto& bytes = responses[url];
  if (bytes.size() > options.maxBytes || (options.expectedBytes && bytes.size() != options.expectedBytes))
    return HTTP_ERROR;
  const auto partial = dest + ".part";
  files[partial] = bytes;
  if (progress) progress(bytes.size(), bytes.size());
  if (afterDownload) afterDownload(url);
  if (options.shouldCancel && options.shouldCancel()) {
    files.erase(partial);
    return ABORTED;
  }
  if (options.validateWithContext && !options.validateWithContext(partial, options.validationContext)) {
    files.erase(partial);
    return FILE_ERROR;
  }
  return DownloadFileSwap::publish(dest, true) ? OK : FILE_ERROR;
}
HttpDownloader::DownloadError HttpDownloader::streamUrl(const std::string& url, const DataCallback& data,
                                                        ProgressCallback progress, const std::string&,
                                                        const std::string&, DownloadOptions options,
                                                        HttpRequestDiagnostics*) {
  ++downloads;
  assert(options.transport == Transport::ESP_HTTP && options.bufferSize == 1024 && options.maxBytes == 2097152);
  assert(options.maxRedirects == 2 && options.allowUrl(url, options.urlContext));
  if (beforeDownload) beforeDownload(url);
  if (options.shouldCancel && options.shouldCancel()) return ABORTED;
  if (!responses.count(url)) return HTTP_ERROR;
  const auto& bytes = responses[url];
  if (bytes.size() > options.maxBytes) return HTTP_ERROR;
  for (size_t at = 0; at < bytes.size();) {
    const size_t count = std::min(size_t(1024), bytes.size() - at);
    if (!data(reinterpret_cast<const uint8_t*>(bytes.data()) + at, count)) return ABORTED;
    at += count;
    if (progress) progress(at, bytes.size());
  }
  if (afterDownload) afterDownload(url);
  return OK;
}
namespace AssetDownloadService {
bool allowUrl(std::string_view value, void* context) {
  const auto& e = *static_cast<Endpoints*>(context);
  return value.substr(0, std::strlen(e.base)) == e.base;
}
bool buildUrl(const Endpoints& e, const char* relative, char* out, size_t cap) {
  int n = std::snprintf(out, cap, "%s%s", e.base, relative);
  return n > 0 && size_t(n) < cap && allowUrl(out, const_cast<Endpoints*>(&e));
}
const char* resultName(Result) { return "failure"; }
}  // namespace AssetDownloadService
std::string digest(const std::string& value) {
  unsigned char bytes[32];
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), bytes);
  char out[65];
  for (size_t i = 0; i < 32; ++i) std::sprintf(out + 2 * i, "%02x", bytes[i]);
  return out;
}
const std::string rev(40, 'a'), prefix = "snapshots/" + rev + "/", base = "https://assets.invalid/";
AssetCatalog::Catalog catalog;
AssetDownloadService::Endpoints endpoints{base.c_str(), ""};
AssetDownloadService::Callbacks callbacks{nullptr, [](void*) { return cancel; }, nullptr, nullptr};
std::string root() { return "/.crosspoint/asset-sources/" + std::string(catalog.entries[0].sha256) + "/" + rev; }
void reset() {
  files.clear();
  directories.clear();
  readers.clear();
  responses.clear();
  failRemove.clear();
  failRename.clear();
  failRead = failWrite = failClose = failSync = failIteration = cancel = false;
  totalCapacity = 64 * 1024 * 1024;
  downloads = 0;
  beforeDownload = {};
  afterDownload = {};
  catalog = {};
  catalog.count = 1;
  std::strcpy(catalog.revision, rev.c_str());
  std::strcpy(catalog.entries[0].code, "de");
  std::strcpy(catalog.entries[0].sha256, digest("payload").c_str());
  catalog.entries[0].bytes = 7;
}
std::string reference(const std::string& path, const std::string& bytes, uint32_t advertised = 0,
                      const std::string& hash = "") {
  return "{\"path\":\"" + path + "\",\"bytes\":" + std::to_string(advertised ? advertised : bytes.size()) +
         ",\"sha256\":\"" + (hash.empty() ? digest(bytes) : hash) + "\"}";
}
std::string index(const std::string& refs) {
  return "{\"hyphenation\":{\"de\":{\"files\":[" + refs + "]}},\"translation\":{}}";
}
void indexes(const std::string& notices, const std::string& sources) {
  responses[base + prefix + "notices.json"] = index(notices);
  responses[base + prefix + "sources.json"] = index(sources);
}
void fixture() {
  const auto a = reference(prefix + "source/LICENSE", "license"),
             b = reference(prefix + "source/archive.zip", "archive");
  indexes(a, a + "," + b);
  responses[base + prefix + "source/LICENSE"] = "license";
  responses[base + prefix + "source/archive.zip"] = "archive";
}
Result run() {
  auto r = AssetCompanionStore::prepare(catalog, catalog.entries[0], endpoints, callbacks);
  for (const auto& v : readers) assert(v.second == 0);
  return r;
}
void prior() {
  Storage.ensureDirectoryExists(root().c_str());
  files[root() + "/complete.txt"] = "old generation";
  files["/.crossink/hyphenation/hyph-de.cphyph"] = "old payload";
}
void assertPrior() {
  assert(files.at(root() + "/complete.txt") == "old generation");
  assert(files.at("/.crossink/hyphenation/hyph-de.cphyph") == "old payload");
}
int main() {
  reset();
  fixture();
  assert(run() == Result::Ok);
  assert(downloads == 4);
  assert(files.at(root() + "/source/LICENSE") == "license");
  assert(files.at(root() + "/source/archive.zip") == "archive");
  assert(files.at(root() + "/notices.json") == responses.at(base + prefix + "notices.json"));
  assert(files.at(root() + "/complete.txt").find("payload=" + std::string(catalog.entries[0].sha256)) !=
         std::string::npos);
  // Fresh authenticated indexes are mandatory even when a local marker exists.
  const int first = downloads;
  assert(run() == Result::Ok && downloads == first + 4);
  reset();
  fixture();
  prior();
  responses.erase(base + prefix + "source/archive.zip");
  assert(run() == Result::Network);
  assertPrior();
  reset();
  fixture();
  prior();
  responses[base + prefix + "source/archive.zip"] = "changed";
  assert(run() == Result::Invalid);
  assertPrior();
  reset();
  fixture();
  prior();
  responses[base + prefix + "sources.json"] = "{\"hyphenation\":{}}";
  assert(run() == Result::Invalid);
  assertPrior();
  reset();
  fixture();
  prior();
  afterDownload = [](const auto& u) {
    if (u.find("archive.zip") != std::string::npos) cancel = true;
  };
  assert(run() == Result::Cancelled);
  assertPrior();
  cancel = false;
  afterDownload = {};
  assert(run() == Result::Ok);
  reset();
  fixture();
  prior();
  totalCapacity = 1024;
  assert(run() == Result::Io && downloads == 0);
  assertPrior();
  reset();
  fixture();
  prior();
  afterDownload = [](const auto& u) {
    if (u.find("sources.json") != std::string::npos) totalCapacity = Storage.usedBytes() + 32;
  };
  assert(run() == Result::Io && downloads == 2);
  assertPrior();
  reset();
  fixture();
  prior();
  failRename.insert(root() + ".part");
  assert(run() == Result::Io);
  assertPrior();
  failRename.clear();
  assert(run() == Result::Ok);
  // Power loss after old rename: retry restores the old complete generation first.
  reset();
  fixture();
  prior();
  assert(Storage.rename(root().c_str(), (root() + ".old").c_str()));
  Storage.ensureDirectoryExists((root() + ".part/source").c_str());
  files[root() + ".part/source/interrupted"] = "partial";
  responses.erase(base + prefix + "source/archive.zip");
  assert(run() == Result::Network);
  assertPrior();
  // Power loss after new directory rename: old cleanup is retried only once a fresh closure is ready.
  reset();
  fixture();
  prior();
  Storage.ensureDirectoryExists((root() + ".old/source").c_str());
  files[root() + ".old/source/previous"] = "prior";
  responses.erase(base + prefix + "source/archive.zip");
  assert(run() == Result::Network);
  assert(files.at(root() + ".old/source/previous") == "prior");
  assertPrior();
  reset();
  fixture();
  prior();
  failIteration = true;
  Storage.ensureDirectoryExists((root() + ".part").c_str());
  assert(run() == Result::Io);
  assertPrior();
  reset();
  fixture();
  prior();
  failWrite = true;
  assert(run() == Result::Io);
  assertPrior();
  reset();
  fixture();
  prior();
  failSync = true;
  assert(run() == Result::Io);
  assertPrior();
  reset();
  fixture();
  prior();
  auto a = reference(prefix + "source/LICENSE", "license");
  indexes(a, reference(prefix + "source/LICENSE", "different"));
  assert(run() == Result::Collision);
  assertPrior();
  reset();
  fixture();
  prior();
  indexes(a, reference(prefix + "source/license", "license"));
  assert(run() == Result::Collision);
  assertPrior();
  reset();
  fixture();
  prior();
  indexes(a, reference(prefix + "source/LICENSE/child", "license"));
  assert(run() == Result::Collision);
  assertPrior();
  for (const auto& bad :
       {prefix + "source/../bad", prefix + "source//bad", std::string("/absolute"), prefix + "source/bad\\\\path"}) {
    reset();
    fixture();
    prior();
    indexes(reference(bad, "bad"), a);
    assert(run() == Result::Invalid);
    assertPrior();
  }
  reset();
  fixture();
  prior();
  indexes(a, reference(prefix + "source/oversize", "a", 2097153));
  assert(run() == Result::Invalid);
  assertPrior();
  reset();
  fixture();
  prior();
  responses[base + prefix + "notices.json"] = std::string(2097153, ' ');
  assert(run() == Result::Network);
  assertPrior();
  reset();
  fixture();
  prior();
  std::string refs;
  for (int i = 0; i < 17; ++i) {
    if (i) refs += ",";
    refs += reference(prefix + "source/" + std::to_string(i), "x");
  }
  indexes(refs, a);
  assert(run() == Result::Invalid);
  assertPrior();
  reset();
  fixture();
  prior();
  refs.clear();
  for (int i = 0; i < 3; ++i) {
    if (i) refs += ",";
    refs += reference(prefix + "source/" + std::to_string(i), "x", 2097152);
  }
  indexes(refs, refs);
  assert(run() == Result::Invalid);
  assertPrior();
  reset();
  fixture();
  prior();
  indexes(reference(prefix + "source/Folder/a", "a"), reference(prefix + "source/folder/b", "b"));
  assert(run() == Result::Collision);
  assertPrior();
  reset();
  fixture();
  prior();
  afterDownload = [](const auto& u) {
    if (u.find("archive.zip") != std::string::npos) files[root() + ".part/notices.json"][0] = '!';
  };
  assert(run() == Result::Io);
  assertPrior();
  reset();
  fixture();
  prior();
  failRemove.insert(root() + ".old/complete.txt");
  assert(run() == Result::Io);
  assert(files.at(root() + "/complete.txt") != "old generation");
  assert(files.at("/.crossink/hyphenation/hyph-de.cphyph") == "old payload");
  failRemove.clear();
  assert(run() == Result::Ok);
  assert(!Storage.exists((root() + ".old").c_str()));
  reset();
  fixture();
  prior();
  cancel = true;
  assert(run() == Result::Cancelled);
  assertPrior();
  reset();
  fixture();
  prior();
  std::strcpy(catalog.revision, "../../aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  assert(run() == Result::Invalid && downloads == 0);
  reset();
  fixture();
  prior();
  afterDownload = [](const auto& u) {
    if (u.find("sources.json") != std::string::npos) files[root() + ".part/references.bin"].assign(312, 'x');
  };
  assert(run() != Result::Ok);
  assertPrior();
  reset();
  fixture();
  prior();
  afterDownload = [](const auto& u) {
    if (u.find("archive.zip") != std::string::npos) files[root() + ".part/references.bin"][55] = 'z';
  };
  assert(run() != Result::Ok);
  assertPrior();
  reset();
  fixture();
  prior();
  afterDownload = [](const auto& u) {
    if (u.find("sources.json") != std::string::npos) files[root() + ".part/references.bin"].replace(241, 65, 65, 'x');
  };
  assert(run() != Result::Ok);
  assertPrior();
  reset();
  fixture();
  prior();
  afterDownload = [](const auto& u) {
    if (u.find("notices.json") != std::string::npos) {
      auto& bytes = files[root() + ".part/notices.json"];
      bytes.replace(bytes.find("LICENSE"), 7, "license");
    }
  };
  assert(run() == Result::Invalid);
  assertPrior();
  // 32 raw references and exact 8MiB duplicate-counted boundary, dedup to 16.
  reset();
  fixture();
  refs.clear();
  for (int i = 0; i < 16; ++i) {
    if (i) refs += ",";
    std::string bytes(262144, char('a' + i));
    const auto p = prefix + "source/" + std::to_string(i);
    refs += reference(p, bytes);
    responses[base + p] = bytes;
  }
  indexes(refs, refs);
  assert(run() == Result::Ok && downloads == 18);
  // A multi-megabyte opaque companion is streamed without extraction.
  reset();
  fixture();
  std::string archive(2097152, 'x');
  auto r = reference(prefix + "source/archive.zip", archive);
  indexes(r, r);
  responses[base + prefix + "source/archive.zip"] = archive;
  assert(run() == Result::Ok && files.at(root() + "/source/archive.zip") == archive);
  std::cout << "AssetCompanionStore transaction groups passed\n";
}
